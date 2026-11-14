/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/smr.h>
#include <geom/geom.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <fs/tmpfs/tmpfs_vnops.h>
#include <fs/tmpfs/tmpfs.h>

VFS_SMR_DECLARE;

static int	kqring_file_rw(struct kqring_ctx *ctx, struct kqring_op *op,
	    bool is_write, bool nowait, bool local, bool *punt);

/*
 * A FIFO's file is a pipe's for reading: pipeops, a struct pipe in f_data,
 * and PIPE_EOF as read(2) sees it.  Not for writing, where a FIFO is its own
 * peer (PIPE_PEER()) and has no pipe_peer to look at.
 */
static bool
kqring_pipe_reads(const struct file *fp)
{
	return (fp->f_type == DTYPE_PIPE || fp->f_type == DTYPE_FIFO);
}

static bool
kqring_pipe_readable(const struct file *fp)
{
	struct pipe *rpipe;
	bool ok;

	/* O_NONBLOCK too: io_uring waits for a non-blocking pipe's data. */
	if (!kqring_pipe_reads(fp))
		return (true);
	rpipe = fp->f_data;
	PIPE_LOCK(rpipe);
	ok = rpipe->pipe_buffer.cnt > 0 || rpipe->pipe_pages.cnt > 0 ||
	    (rpipe->pipe_state & PIPE_EOF) != 0;
	PIPE_UNLOCK(rpipe);
	return (ok);
}

bool
kqring_pipe_will_block(const struct file *fp, bool is_write, size_t want)
{
	struct pipe *p, *wpipe;
	bool block;

	if (!(is_write ? fp->f_type == DTYPE_PIPE : kqring_pipe_reads(fp)) ||
	    (fp->f_flag & FNONBLOCK) != 0 ||
	    (fp->f_flag & (is_write ? FWRITE : FREAD)) == 0)
		return (false);
	p = fp->f_data;
	PIPE_LOCK(p);
	if (!is_write) {
		block = p->pipe_buffer.cnt == 0 && p->pipe_pages.cnt == 0 &&
		    (p->pipe_state & PIPE_EOF) == 0;
	} else {
		wpipe = p->pipe_peer;
		if (wpipe == NULL || (wpipe->pipe_state & PIPE_EOF) != 0)
			block = false;
		else
			block = want >= PIPE_MINDIRECT ||
			    (wpipe->pipe_state & PIPE_DIRECTW) != 0 ||
			    want > wpipe->pipe_buffer.size -
			    wpipe->pipe_buffer.cnt;
	}
	PIPE_UNLOCK(p);
	return (block);
}

static int
kqring_pipe_wait_read(struct kqring_ctx *ctx, struct file *fp,
    struct kqring_op *op)
{
	struct pipe *rpipe = fp->f_data;
	int error = 0;

	PIPE_LOCK(rpipe);
	while (rpipe->pipe_buffer.cnt == 0 && rpipe->pipe_pages.cnt == 0 &&
	    (rpipe->pipe_state & PIPE_EOF) == 0) {
		if (op->wq_canceled || op->cancelled || ctx->closing) {
			error = ECANCELED;
			break;
		}
		/* Off a worker there is no one to tell: sleep, never spin. */
		if (!op->wq_blocking && op->wq_running) {
			PIPE_UNLOCK(rpipe);
			kqring_wq_mark_blocking(ctx, op);
			PIPE_LOCK(rpipe);
			continue;
		}
		rpipe->pipe_state |= PIPE_WANTR | PIPE_WANT;
		error = msleep(rpipe, PIPE_MTX(rpipe), PRIBIO | PCATCH,
		    "piperd", 0);
		if (error != 0) {
			if ((error == EINTR || error == ERESTART) &&
			    (op->wq_canceled || op->cancelled || ctx->closing))
				error = ECANCELED;
			break;
		}
	}
	PIPE_UNLOCK(rpipe);
	return (error);
}

/* op's file for a read or write, checked as kern_preadv() checks it. */
static int
kqring_rw_fget(struct kqring_op *op, bool is_write, struct file **fpp,
    int *flagsp)
{
	off_t offset = (off_t)op->sqe.off;
	cap_rights_t rights;
	struct file *fp;
	int error, flags = 0;

	/* off == -1 means "use the file position", as in io_uring. */
	if (offset != (off_t)-1)
		flags = FOF_OFFSET;
again:
	if (is_write && flags == 0)
		cap_rights_init_one(&rights, CAP_WRITE);
	else if (is_write)
		cap_rights_init_one(&rights, CAP_PWRITE);
	else if (flags == 0)
		cap_rights_init_one(&rights, CAP_READ);
	else
		cap_rights_init_one(&rights, CAP_PREAD);
	error = kqring_op_fget(op, &rights, is_write ? FWRITE : FREAD, &fp);
	if (error == ENOTCAPABLE && flags != 0) {
		/* A stream with CAP_READ only: the offset is ignored. */
		flags = 0;
		goto again;
	}
	if (error != 0)
		return (error);
	if (flags == 0 && offset != (off_t)-1 &&
	    (fp->f_ops->fo_flags & DFLAG_SEEKABLE) != 0) {
		/* Retried without CAP_SEEK, but the file is seekable. */
		fdrop(fp, curthread);
		return (ENOTCAPABLE);
	}
	if (flags != 0) {
		/*
		 * io_uring ignores the offset on a stream (a pipe, FIFO or
		 * socket: FMODE_STREAM), rather than failing as pread(2) does.
		 */
		if ((fp->f_ops->fo_flags & DFLAG_SEEKABLE) == 0)
			flags = 0;
		else if (offset < 0 && (fp->f_vnode == NULL ||
		    fp->f_vnode->v_type != VCHR))
			error = EINVAL;
		if (error != 0) {
			fdrop(fp, curthread);
			return (error);
		}
	}
	*fpp = fp;
	*flagsp = flags;
	return (0);
}

/* ============================================================================
 * Disk devices: block I/O without a worker
 * ============================================================================
 *
 * A positioned READ / WRITE (plain, vectored or _FIXED) on a disk device
 * (/dev/ada0, nda0, md0, ... or a GEOM partition) is sent to the driver
 * as bios over the user's held pages, as aio(4) does (aio_qbio()), and
 * completes from the bio callback: no worker thread, and any number in
 * flight.  Like io_uring's block requests, bios in flight cannot be
 * cancelled (ASYNC_CANCEL finds them -EALREADY), and ring teardown waits
 * for them.  Anything the driver path cannot take (unaligned lengths,
 * transfers over maxphys or the device's limit, drivers without unmapped
 * I/O) runs on a worker as before.
 */

static void
kqring_bio_pages_free(struct bio *bp)
{
	/* bio_caller2: the pages were held for this bio, not registered. */
	if (bp->bio_caller2 != NULL) {
		vm_page_unhold_pages(bp->bio_ma, bp->bio_ma_n);
		free(bp->bio_ma, M_KQRING);
	}
	g_destroy_bio(bp);
}

/*
 * A bio finished (from the driver or GEOM, in any thread).  The last one
 * queues the request as taskwork; the owner posts its CQE.
 */
static void
kqring_bio_done(struct bio *bp)
{
	struct kqring_op *op = bp->bio_caller1;
	struct kqring_ctx *ctx = op->ctx;
	int error;

	if ((bp->bio_flags & BIO_ERROR) != 0)
		atomic_store_int(&op->bio_errno, bp->bio_error);
	atomic_add_long(&op->bio_bytes, (u_long)(bp->bio_bcount -
	    bp->bio_resid));
	kqring_bio_pages_free(bp);
	if (!refcount_release(&op->bio_refs))
		return;

	error = atomic_load_int(&op->bio_errno);
	mtx_lock(&ctx->mtx);
	op->res = error != 0 ? kqring_errno(error) :
	    kqring_xfer_res(atomic_load_long(&op->bio_bytes));
	op->short_io = atomic_load_long(&op->bio_bytes) < op->bio_len;
	op->cflags = 0;
	op->bio_done = true;
	ctx->bio_held -= op->bio_len;
	(void)kqring_taskwork_add_lazy_locked(ctx, op);
	/* After this, op and ctx may be freed by a teardown waiting here. */
	if (--ctx->nbio == 0 && ctx->closing)
		wakeup(&ctx->nbio);
	mtx_unlock(&ctx->mtx);
}

/*
 * Send op to fp's disk driver if it can go there.  Returns true if it was
 * sent (op is armed and completes from kqring_bio_done()), false if it
 * must run some other way; op is unchanged then.
 */
static bool
kqring_file_bio(struct kqring_ctx *ctx, struct kqring_op *op,
    struct file *fp, bool is_write)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct bio *bios[KQRING_BIO_MAX_IOV];
	struct iovec aiov, *iov = &aiov;
	struct vnode *vp = fp->f_vnode;
	struct cdevsw *csw;
	struct cdev *dev;
	off_t offset = (off_t)sqe->off;
	size_t total = 0;
	u_int iovcnt = 1, bsize, i, n = 0;
	vm_prot_t prot;
	int ref;

	if (fp->f_type != DTYPE_VNODE || vp == NULL || vp->v_type != VCHR ||
	    offset < 0 || (sqe->flags & IOSQE_BUFFER_SELECT) != 0)
		return (false);
	bsize = vp->v_bufobj.bo_bsize;
	if (bsize == 0)
		return (false);
	if (kqring_op_readv(sqe->opcode) || kqring_op_writev(sqe->opcode)) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (void *)(uintptr_t)sqe->addr;
		aiov.iov_len = sqe->len;
	}
	if (iovcnt == 0 || iovcnt > KQRING_BIO_MAX_IOV)
		return (false);
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0 || iov[i].iov_len % bsize != 0 ||
		    iov[i].iov_len > maxphys)
			return (false);
		total += iov[i].iov_len;
	}
	if (total > INT32_MAX || offset > OFF_MAX - (off_t)total)
		return (false);

	csw = devvn_refthread(vp, &dev, &ref);
	if (csw == NULL)
		return (false);
	if ((csw->d_flags & D_DISK) == 0 || total > (size_t)dev->si_iosize_max ||
	    (dev->si_flags & SI_UNMAPPED) == 0 || !unmapped_buf_allowed) {
		dev_relthread(dev, ref);
		return (false);
	}
	mtx_lock(&ctx->mtx);
	if (ctx->bio_held + total > KQRING_BIO_MAX_HELD) {
		mtx_unlock(&ctx->mtx);
		dev_relthread(dev, ref);
		return (false);
	}
	ctx->bio_held += total;
	mtx_unlock(&ctx->mtx);

	/* A read from the disk writes the user's pages. */
	prot = is_write ? VM_PROT_READ : VM_PROT_READ | VM_PROT_WRITE;
	for (n = 0; n < iovcnt; n++) {
		struct bio *bp;
		vm_offset_t va = (vm_offset_t)iov[n].iov_base;
		size_t len = iov[n].iov_len;
		int maxpages = (int)atop(round_page(len)) + 1;
		vm_page_t *pages;
		int npages;

		if (op->kbuf) {
			/*
			 * A registered buffer: its pages are held already
			 * (op->fzb), and va is in their kernel mapping.
			 */
			pages = &op->fzb->pages[(va - op->fzb->kva) >>
			    PAGE_SHIFT];
			npages = (int)atop(round_page((va & PAGE_MASK) + len));
		} else {
			pages = malloc(sizeof(*pages) * maxpages, M_KQRING,
			    M_WAITOK);
			npages = vm_fault_quick_hold_pages(
			    &curproc->p_vmspace->vm_map, va, len, prot, pages,
			    maxpages);
			if (npages < 0) {
				free(pages, M_KQRING);
				break;
			}
		}
		bp = g_alloc_bio();
		bp->bio_cmd = is_write ? BIO_WRITE : BIO_READ;
		bp->bio_offset = offset;
		bp->bio_length = len;
		bp->bio_bcount = len;
		bp->bio_dev = dev;
		bp->bio_done = kqring_bio_done;
		bp->bio_caller1 = op;
		bp->bio_caller2 = op->kbuf ? NULL : pages;
		bp->bio_ma = pages;
		bp->bio_ma_n = npages;
		bp->bio_ma_offset = va & PAGE_MASK;
		bp->bio_data = unmapped_buf;
		bp->bio_flags |= BIO_UNMAPPED;
		bios[n] = bp;
		offset += len;
	}
	if (n < iovcnt) {
		/* A buffer could not be held: the worker reports EFAULT. */
		while (n > 0)
			kqring_bio_pages_free(bios[--n]);
		mtx_lock(&ctx->mtx);
		ctx->bio_held -= total;
		mtx_unlock(&ctx->mtx);
		dev_relthread(dev, ref);
		return (false);
	}

	if (!fhold(fp)) {
		while (n > 0)
			kqring_bio_pages_free(bios[--n]);
		mtx_lock(&ctx->mtx);
		ctx->bio_held -= total;
		mtx_unlock(&ctx->mtx);
		dev_relthread(dev, ref);
		return (false);
	}
	op->bio_fp = fp;
	op->bio = true;
	op->bio_done = false;
	op->bio_errno = 0;
	op->bio_bytes = 0;
	op->bio_len = total;
	refcount_init(&op->bio_refs, iovcnt);
	mtx_lock(&ctx->mtx);
	ctx->nbio++;
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	for (i = 0; i < iovcnt; i++)
		csw->d_strategy(bios[i]);
	dev_relthread(dev, ref);
	return (true);
}

/*
 * kqring_file_bio() for a request that does not first try the inline
 * paths (IORING_SETUP_IOPOLL rings).
 */
bool
kqring_file_bio_try(struct kqring_ctx *ctx, struct kqring_op *op,
    bool is_write)
{
	struct file *fp;
	bool sent;
	int flags;

	if (kqring_rw_fget(op, is_write, &fp, &flags) != 0)
		return (false);
	sent = flags != 0 && kqring_file_bio(ctx, op, fp, is_write);
	fdrop(fp, curthread);
	return (sent);
}

static bool
kqring_vp_is_tmpfs(const struct vnode *vp)
{
	return (vp->v_op == &tmpfs_vnodeop_entries ||
	    vp->v_op == &tmpfs_vnodeop_nonc_entries);
}

/*
 * A tmpfs file's resident pages, copied to uio without sleeping.  tmpfs's
 * own VOP_READ_PGCACHE does this only on a mount with -o pgread (off by
 * default), and even then pages a swapped-out page back in, which sleeps.
 * This is vn_read_from_obj() for tmpfs: the node's swap object is the
 * vnode's, but its size is the node's (tn_size), not the vnode pager's.
 * The node is read under SMR, as tmpfs_read_pgcache() does; vm_objects are
 * type-stable, so holding the object (vm_object_pip_add()) after leaving
 * SMR is safe, and OBJ_DEAD is checked once it is held.  Pages are taken
 * one at a time, never created or waited for, and copied with
 * uiomove_fromphys(); the caller holds the range read-locked, so the file
 * can't be truncated under the copy, and has page faults disabled.
 *
 * Returns 0 when uio was filled or the file ended (a short read at end of
 * file), or EJUSTRETURN when a page wasn't resident and valid, with uio
 * advanced over what was copied: the caller redoes the whole read on a
 * worker.  Other errors are the copy's (EFAULT).
 */
static int
kqring_tmpfs_read(struct vnode *vp, struct uio *uio)
{
	struct tmpfs_node *node;
	vm_object_t obj;
	vm_page_t m;
	off_t size;
	size_t len;
	int error;
	u_int off;

	vfs_smr_enter();
	node = VP_TO_TMPFS_NODE_SMR(vp);
	if (node == NULL || VN_IS_DOOMED(vp) || node->tn_type != VREG ||
	    (obj = node->tn_reg.tn_aobj) == NULL) {
		vfs_smr_exit();
		return (EJUSTRETURN);
	}
	size = node->tn_size;
	tmpfs_set_accessed(node->tn_reg.tn_tmp, node);
	vfs_smr_exit();

	vm_object_pip_add(obj, 1);
	if ((obj->flags & OBJ_DEAD) != 0) {
		vm_object_pip_wakeup(obj);
		return (EJUSTRETURN);
	}
	error = 0;
	while (uio->uio_resid > 0 && uio->uio_offset < size) {
		off = uio->uio_offset & PAGE_MASK;
		len = MIN(PAGE_SIZE - off,
		    MIN((size_t)uio->uio_resid, (size_t)(size - uio->uio_offset)));
		m = vm_page_grab_unlocked(obj, OFF_TO_IDX(uio->uio_offset),
		    VM_ALLOC_NOCREAT | VM_ALLOC_SBUSY | VM_ALLOC_IGN_SBUSY |
		    VM_ALLOC_NOWAIT);
		if (m == NULL) {
			error = EJUSTRETURN;
			break;
		}
		/*
		 * A busied page keeps vgonel() from terminating the object
		 * past it, so the vnode is checked again after.
		 */
		if (!vm_page_all_valid(m) || VN_IS_DOOMED(vp)) {
			vm_page_sunbusy(m);
			error = EJUSTRETURN;
			break;
		}
		error = uiomove_fromphys(&m, off, len, uio);
		vm_page_activate(m);
		vm_page_sunbusy(m);
		if (error != 0)
			break;
	}
	vm_object_pip_wakeup(obj);
	return (error);
}

/*
 * READ / WRITE on a descriptor that is not a socket, on the issuer when
 * that cannot block it, as io_uring tries non-blocking I/O before punting
 * to io-wq:
 *
 *  - A pipe with data to read (or room to write), or a FIFO with data to
 *    read, blocking or not, read or written as if O_NONBLOCK
 *    (kqring_pipe_rw_nowait()).  If another
 *    reader drained it since the check, the read goes to a worker; a
 *    write that filled it leaves the rest to a worker.
 *  - A positioned read of a regular file that the page cache satisfies.
 *    VOP_READ_PGCACHE grabs resident pages without sleeping and gives up
 *    with EJUSTRETURN otherwise; vn_read() tries it first the same way.
 *    tmpfs files are read by kqring_tmpfs_read() instead, which needs no
 *    mount option and never pages in from swap.  Its EJUSTRETURN always
 *    sends the read to a worker: it returns 0 at end of file, and a tmpfs
 *    object has no vnode pager size to tell the two apart by.
 *    A destination that is not mapped yet, or a contended range, also
 *    goes to the worker.
 *
 * A positioned read or write of a disk device goes to its driver instead
 * (kqring_file_bio()).
 *
 * Returns KQRING_RW_DONE with op->res set if the request completed, or
 * KQRING_RW_BIO if it was sent to a disk driver, or KQRING_RW_SOCKET
 * if the descriptor needs nonblocking socket dispatch.  Otherwise
 * (KQRING_RW_PUNT) op is unchanged and goes to a worker; a read that
 * copied only part of a range (a page was not resident) is redone there in
 * full, so the result is the same as if it had never been tried.
 */
int
kqring_file_rw_inline(struct kqring_ctx *ctx, struct kqring_op *op,
    bool is_write)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	struct vnode *vp;
	vm_object_t obj;
	struct uio auio;
	struct iovec aiov, tiov[16], *hiov = NULL, *iov = &aiov;
	u_int iovcnt = 1;
	ssize_t want;
	size_t done;
	void *rl;
	int error, flags, save;
	bool tmpfs;

	/* A bad descriptor fails here, as the worker would fail it. */
	if ((error = kqring_rw_fget(op, is_write, &fp, &flags)) != 0) {
		kqring_done(op, -error, 0);
		return (KQRING_RW_DONE);
	}
	if (fp->f_type == DTYPE_SOCKET) {
		fdrop(fp, td);
		return (KQRING_RW_SOCKET);
	}
	/*
	 * A pipe that is ready completes here, even blocking (the read or
	 * write then doesn't wait).  One that isn't waits on a worker, even
	 * O_NONBLOCK, as io_uring polls rather than fail with EAGAIN; with
	 * RWF_NOWAIT the issuer fails it instead.
	 */
	if (is_write ? fp->f_type == DTYPE_PIPE &&
	    !kqring_pipe_will_block(fp, true, 1) :
	    kqring_pipe_reads(fp) && kqring_pipe_readable(fp)) {
		bool punt;

		fdrop(fp, td);
		kqring_file_rw(ctx, op, is_write, true, false, &punt);
		return (punt ? KQRING_RW_PUNT : KQRING_RW_DONE);
	}
	vp = fp->f_vnode;
	if (flags != 0 && vp != NULL && vp->v_type == VCHR) {
		bool sent = kqring_file_bio(ctx, op, fp, is_write);

		fdrop(fp, td);
		return (sent ? KQRING_RW_BIO : KQRING_RW_PUNT);
	}
	if (is_write || flags == 0 || (sqe->flags & IOSQE_BUFFER_SELECT) != 0 ||
	    fp->f_type != DTYPE_VNODE || vp->v_type != VREG ||
	    (fp->f_flag & O_DIRECT) != 0 || mac_vnode_check_read_enabled() ||
	    (vn_irflag_read(vp) & VIRF_DOOMED) != 0) {
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	tmpfs = kqring_vp_is_tmpfs(vp);
	if (!tmpfs && (vn_irflag_read(vp) & VIRF_PGREAD) == 0) {
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	if (kqring_op_readv(sqe->opcode)) {
		iovcnt = sqe->len;
		if (iovcnt <= nitems(tiov)) {
			iov = tiov;
		} else {
			hiov = malloc(iovcnt * sizeof(*hiov), M_KQRING,
			    M_WAITOK);
			iov = hiov;
		}
		memcpy(iov, op->args, iovcnt * sizeof(*iov));
	} else {
		aiov.iov_base = (void *)(uintptr_t)sqe->addr;
		aiov.iov_len = sqe->len;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_READ, op->kbuf);
	auio.uio_offset = (off_t)sqe->off;
	want = auio.uio_resid;
	if (want == 0 || auio.uio_offset > OFF_MAX - want) {
		free(hiov, M_KQRING);
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	/*
	 * As vn_io_fault() does for read(2): hold the range against writers
	 * (but never wait for it), and copy out with page faults disabled.  A
	 * fault on the destination while file pages are busied could need
	 * those very pages (a buffer mapped from the same file); a copy that
	 * would fault fails with EFAULT and goes to the worker instead, whose
	 * fo_read() faults the buffer in safely.
	 */
	rl = vn_rangelock_tryrlock(vp, auio.uio_offset, auio.uio_offset + want);
	if (rl == NULL) {
		free(hiov, M_KQRING);
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	save = vm_fault_disable_pagefaults();
	error = tmpfs ? kqring_tmpfs_read(vp, &auio) :
	    VOP_READ_PGCACHE(vp, &auio, 0, fp->f_cred);
	vm_fault_enable_pagefaults(save);
	vn_rangelock_unlock(vp, rl);
	free(hiov, M_KQRING);
	done = (size_t)(want - auio.uio_resid);
	if (error == EFAULT) {
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	if (error == EJUSTRETURN) {
		/*
		 * Nothing, or only part of the range, came from the page
		 * cache (vn_read() would go on with VOP_READ()).  Part is the
		 * whole answer at end of file, as for read(2).
		 */
		obj = atomic_load_ptr(&vp->v_object);
		if (tmpfs || done == 0 || obj == NULL || auio.uio_offset <
		    (off_t)atomic_load_64(&obj->un_pager.vnp.vnp_size)) {
			fdrop(fp, td);
			return (KQRING_RW_PUNT);
		}
		error = 0;
	}
	if (error == 0)
		fp->f_nextoff[UIO_READ] = auio.uio_offset;
	fdrop(fp, td);
	op->short_io = done < (size_t)want;
	if (error != 0 && done == 0)
		kqring_done(op, kqring_errno(error), 0);
	else
		kqring_done(op, kqring_xfer_res(done), 0);
	return (KQRING_RW_DONE);
}

/*
 * READ / WRITE / READV / WRITEV and their _FIXED forms on anything but a
 * socket (sockets go through kqring_exec_recv() / kqring_exec_send() and
 * can be armed): fo_read() / fo_write() on the file, with the checks
 * kern_preadv() and dofileread() make.  READ / READV may take a provided
 * buffer, only consumed if bytes were read.  A SIGPIPE is never raised.
 */
int
kqring_exec_file_rw(struct kqring_ctx *ctx, struct kqring_op *op,
    bool is_write)
{
	bool punt;

	return (kqring_file_rw(ctx, op, is_write, false, false, &punt));
}

/*
 * As kqring_exec_file_rw(), for a read run by the thread that submitted it
 * (kqring_wq_try_inline()): only of a regular file on a local filesystem.
 * Anything else sets *punt, with op unchanged, for a worker to read.
 */
int
kqring_exec_file_read_local(struct kqring_ctx *ctx, struct kqring_op *op,
    bool *punt)
{
	return (kqring_file_rw(ctx, op, false, false, true, punt));
}

/*
 * fo_read() / fo_write() on a pipe as if it were O_NONBLOCK, without
 * changing the descriptor's flags for anyone else.  pipe_read() and
 * pipe_write() look at the file only for f_data and f_flag, so a file of
 * our own sharing the pipe does it.
 */
static int
kqring_pipe_rw_nowait(struct file *fp, struct uio *uio, bool is_write,
    int flags, struct thread *td)
{
	struct file nb;

	memset(&nb, 0, sizeof(nb));
	nb.f_data = fp->f_data;
	nb.f_ops = &pipeops;
	nb.f_cred = fp->f_cred;
	nb.f_type = fp->f_type;
	nb.f_flag = fp->f_flag | FNONBLOCK;
	refcount_init(&nb.f_count, 1);
	return (is_write ? fo_write(&nb, uio, td->td_ucred, flags, td) :
	    fo_read(&nb, uio, td->td_ucred, flags, td));
}

/*
 * As kqring_exec_file_rw(); with nowait (the issuer, on a pipe), a pipe is
 * never waited for: *punt is set, with op unchanged, if it would have to,
 * or with op->done_io set to what a write got in before the pipe filled.
 * With local, *punt is set, with op unchanged, unless the file is a regular
 * file on a local filesystem (kqring_exec_file_read_local()).
 */
/*
 * Whether fp is a regular file whose filesystem copies to and from user
 * memory while it holds the file's pages: tmpfs busies the page it copies
 * (uiomove_object()), and a filesystem on the buffer cache locks the
 * buffer.  If the user memory is mapped from that very page, the copy's
 * fault waits for the page or buffer, which is ours, and the thread hangs
 * for good: read(2) does on tmpfs (FreeBSD bug 300734).  vn_io_fault()
 * prevents it by holding the user's pages first, but only on a filesystem
 * that sets MNTK_NO_IOPF (UFS, ZFS); tmpfs doesn't.
 */
static bool
kqring_vn_iopf(const struct file *fp)
{
	struct vnode *vp = fp->f_vnode;
	struct mount *mp;

	if (fp->f_type != DTYPE_VNODE || vp == NULL || vp->v_type != VREG)
		return (false);
	mp = atomic_load_ptr(&vp->v_mount);
	return (mp != NULL && (mp->mnt_kern_flag & MNTK_NO_IOPF) == 0);
}

#define	KQRING_HELD_RW_PAGES	256	/* 1 MiB of user memory at a time */

/*
 * fo_read() / fo_write() of uio's user memory, held first: up to
 * KQRING_HELD_RW_PAGES of its pages at a time are held (faulted in, and
 * for a read made writable and marked dirty), and the filesystem copies
 * through their direct-map addresses, which can't fault.  uio advances
 * over what was transferred, as uiomove() would advance it.  A buffer that
 * can't be held fails with EFAULT, unless part was transferred already;
 * one held page outside the direct map (device memory) leaves the rest to
 * a plain fo_read() / fo_write().
 */
static int
kqring_vn_rw_held(struct file *fp, struct uio *uio, bool is_write, int flags,
    struct thread *td)
{
	vm_map_t map = &td->td_proc->p_vmspace->vm_map;
	vm_prot_t prot = is_write ? VM_PROT_READ :
	    VM_PROT_READ | VM_PROT_WRITE;
	vm_page_t *pages;
	struct iovec *kiov;
	struct uio kuio;
	ssize_t moved = 0;
	int error = 0;

	if (!PMAP_HAS_DMAP)
		return (is_write ? fo_write(fp, uio, td->td_ucred, flags, td) :
		    fo_read(fp, uio, td->td_ucred, flags, td));
	pages = malloc(KQRING_HELD_RW_PAGES * sizeof(*pages), M_KQRING,
	    M_WAITOK);
	kiov = malloc(KQRING_HELD_RW_PAGES * sizeof(*kiov), M_KQRING,
	    M_WAITOK);
	while (uio->uio_resid > 0) {
		ssize_t chunk = 0, done;
		int npages = 0, nkiov = 0, i;
		bool dmap = true;

		/* Hold the next pieces of the user's iovecs, page by page. */
		for (i = 0; i < uio->uio_iovcnt &&
		    npages < KQRING_HELD_RW_PAGES; i++) {
			vm_offset_t va =
			    (vm_offset_t)uio->uio_iov[i].iov_base;
			size_t len = uio->uio_iov[i].iov_len, n, want;
			int held, p;

			if (len == 0)
				continue;
			/* Whole pages of it that still fit. */
			n = want = MIN(len, ptoa(KQRING_HELD_RW_PAGES -
			    npages) - (va & PAGE_MASK));
			held = vm_fault_quick_hold_pages(map, va, n, prot,
			    &pages[npages], KQRING_HELD_RW_PAGES - npages);
			if (held < 0)
				break;
			for (p = 0; p < held; p++) {
				vm_paddr_t pa = VM_PAGE_TO_PHYS(pages[npages + p]);
				size_t off = p == 0 ? va & PAGE_MASK : 0;
				size_t plen = MIN(PAGE_SIZE - off, n);

				if (!PHYS_IN_DMAP(pa))
					dmap = false;
				kiov[nkiov].iov_base =
				    (void *)(PHYS_TO_DMAP(pa) + off);
				kiov[nkiov].iov_len = plen;
				nkiov++;
				n -= plen;
				chunk += plen;
			}
			npages += held;
			/* The iovec goes on past what fits: stop with it. */
			if (want < len)
				break;
		}
		if (npages == 0 || !dmap) {
			vm_page_unhold_pages(pages, npages);
			if (npages == 0 && moved == 0)
				error = EFAULT;
			else if (!dmap)
				error = is_write ? fo_write(fp, uio,
				    td->td_ucred, flags, td) : fo_read(fp, uio,
				    td->td_ucred, flags, td);
			break;
		}
		kuio.uio_iov = kiov;
		kuio.uio_iovcnt = nkiov;
		kuio.uio_offset = uio->uio_offset;
		kuio.uio_resid = chunk;
		kuio.uio_segflg = UIO_SYSSPACE;
		kuio.uio_rw = uio->uio_rw;
		kuio.uio_td = td;
		error = is_write ? fo_write(fp, &kuio, td->td_ucred, flags, td) :
		    fo_read(fp, &kuio, td->td_ucred, flags, td);
		vm_page_unhold_pages(pages, npages);
		done = chunk - kuio.uio_resid;
		uio->uio_offset = kuio.uio_offset;
		/* Advance the user's iovecs over done, as uiomove() does. */
		moved += done;
		uio->uio_resid -= done;
		while (done > 0) {
			size_t step = MIN((size_t)done, uio->uio_iov->iov_len);

			uio->uio_iov->iov_base =
			    (char *)uio->uio_iov->iov_base + step;
			uio->uio_iov->iov_len -= step;
			done -= step;
			if (uio->uio_iov->iov_len == 0 && uio->uio_iovcnt > 1) {
				uio->uio_iov++;
				uio->uio_iovcnt--;
			}
		}
		/* An error, end of file, or a short write. */
		if (error != 0 || kuio.uio_resid > 0)
			break;
	}
	free(kiov, M_KQRING);
	free(pages, M_KQRING);
	return (error);
}

static int
kqring_file_rw(struct kqring_ctx *ctx, struct kqring_op *op, bool is_write,
    bool nowait, bool local, bool *punt)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct kqring_sel sel;
	struct file *fp;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	off_t offset = (off_t)sqe->off;
	u_int iovcnt = 1;
	uint32_t cflags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool ordered;
	int error, flags;

	*punt = false;
	error = kqring_rw_fget(op, is_write, &fp, &flags);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	/*
	 * O_NONBLOCK too: another reader can drain the pipe after the issuer
	 * saw it ready, and the issuer must then punt rather than wait.
	 */
	nowait = nowait && fp->f_ops == &pipeops;
	if (local) {
		struct vnode *vp = fp->f_type == DTYPE_VNODE ?
		    fp->f_vnode : NULL;
		struct mount *mp = vp != NULL && vp->v_type == VREG ?
		    atomic_load_ptr(&vp->v_mount) : NULL;

		if (mp == NULL || (mp->mnt_flag & MNT_LOCAL) == 0)
			goto punt;
	}

	if (buf_select) {
		struct kqring_bgroup *bg;
again:
		if (!is_write && !kqring_pipe_readable(fp)) {
			if (nowait)
				goto punt;
			error = kqring_pipe_wait_read(ctx, fp, op);
			if (error != 0) {
				fdrop(fp, td);
				return (kqring_done(op, kqring_errno(error),
				    0));
			}
		}
		sx_xlock(&ctx->bg_sx);
		error = kqring_buf_select(ctx, sqe->buf_group,
		    kqring_buf_cap(op), &aiov, 1, &sel);
		/*
		 * On a worker, another read holding the group's head buffer
		 * (for as long as its fo_read() takes) is waited for rather
		 * than failed with ENOBUFS, as io_uring readers share a group.
		 * The issuer leaves such a read to a worker.
		 */
		if (error == ENOBUFS && nowait &&
		    (bg = kqring_bgroup_find(ctx, sqe->buf_group)) != NULL &&
		    bg->busy) {
			sx_xunlock(&ctx->bg_sx);
			goto punt;
		}
		if (error == ENOBUFS && op->wq_running && !op->wq_canceled &&
		    !op->cancelled && !ctx->closing &&
		    (bg = kqring_bgroup_find(ctx, sqe->buf_group)) != NULL &&
		    bg->busy) {
			sx_xunlock(&ctx->bg_sx);
			if (kqring_wq_suspend_point(op)) {
				fdrop(fp, td);
				return (kqring_done(op, -ECANCELED, 0));
			}
			/* Until kqring_bg_release(), or a tick at most. */
			sx_xlock(&ctx->bg_sx);
			if ((bg = kqring_bgroup_find(ctx, sqe->buf_group)) != NULL &&
			    bg->busy)
				(void)sx_sleep(&bg->busy, &ctx->bg_sx, 0,
				    "kqbgbz", 1);
			sx_xunlock(&ctx->bg_sx);
			goto again;
		}
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
		sel.bg->busy = true;
		sx_xunlock(&ctx->bg_sx);
	} else if (kqring_op_readv(sqe->opcode) ||
	    kqring_op_writev(sqe->opcode)) {
		/*
		 * What the issuer wrote before the pipe filled (below) has
		 * already advanced op->args in place through uiomove().
		 */
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, is_write ? UIO_WRITE : UIO_READ,
	    op->kbuf);
	ssize_t want = auio.uio_resid;
	auio.uio_offset = offset;

	/* Zero-length reads finish here, as in dofileread(). */
	if (want == 0 && !is_write) {
		if (buf_select) {
			sx_xlock(&ctx->bg_sx);
			kqring_bg_release(sel.bg);
			sx_xunlock(&ctx->bg_sx);
		}
		fdrop(fp, td);
		return (kqring_done(op, 0, 0));
	}
retry:
	if (!buf_select && !is_write && !kqring_pipe_readable(fp)) {
		if (nowait)
			goto punt;
		error = kqring_pipe_wait_read(ctx, fp, op);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, kqring_errno(error), 0));
		}
	}
	/*
	 * Reads of a pipe or FIFO post their CQEs in the order they got the
	 * data, as in io_uring, where the read and the CQE are one step.  A
	 * worker keeps pipe_sx until its op is on its taskwork
	 * (kqring_wq_thread()), so it must not wait for the pipe with it.
	 * The issuer posts its own CQE before it runs any taskwork, so it
	 * only has to leave a read to a worker while older ones are still to
	 * post, unless RWF_NOWAIT would turn that into a failure.
	 */
	ordered = !is_write && kqring_pipe_reads(fp) &&
	    (sqe->rw_flags & KQRING_RWF_NOWAIT) == 0 &&
	    (nowait || op->wq_running);
	if (ordered && nowait) {
		if (!sx_try_xlock(&ctx->pipe_sx)) {
			error = EAGAIN;
			goto read_busy;
		}
		if (!TAILQ_EMPTY(&op->tctx->taskwork)) {
			sx_xunlock(&ctx->pipe_sx);
			error = EAGAIN;
			goto read_busy;
		}
	} else if (ordered) {
		/* Through taskwork: the issuer mustn't post it at once. */
		kqring_wq_mark_blocking(ctx, op);
		sx_xlock(&ctx->pipe_sx);
	}
	if (nowait || ordered) {
		error = kqring_pipe_rw_nowait(fp, &auio, is_write, flags, td);
		if (nowait && ordered)
			sx_xunlock(&ctx->pipe_sx);
	} else {
		if (kqring_pipe_will_block(fp, is_write, (size_t)want))
			kqring_wq_mark_blocking(ctx, op);
		if (auio.uio_segflg == UIO_USERSPACE && kqring_vn_iopf(fp))
			error = kqring_vn_rw_held(fp, &auio, is_write, flags,
			    td);
		else
			error = is_write ? fo_write(fp, &auio, td->td_ucred,
			    flags, td) : fo_read(fp, &auio, td->td_ucred,
			    flags, td);
	}
read_busy:
	if (!nowait) {
		/*
		 * Another reader drained the pipe between the wake-up and the
		 * read: wait for it again.
		 */
		if (!is_write && error == EAGAIN && kqring_pipe_reads(fp) &&
		    auio.uio_resid == want) {
			if (ordered)
				sx_xunlock(&ctx->pipe_sx);
			if (!buf_select)
				goto retry;
			sx_xlock(&ctx->bg_sx);
			kqring_bg_release(sel.bg);
			sx_xunlock(&ctx->bg_sx);
			goto again;
		}
	}
	bool vnode = fp->f_type == DTYPE_VNODE;
	fdrop(fp, td);

	size_t done = (size_t)(want - auio.uio_resid);
	/*
	 * Someone else drained (or filled) the pipe since the issuer looked.
	 * A read that got nothing goes to a worker unchanged; a write keeps
	 * what it got in, and a worker writes the rest, as the blocking
	 * write(2) would.
	 */
	if (nowait && (error == EAGAIN || (error == 0 && is_write &&
	    auio.uio_resid > 0))) {
		if (is_write) {
			op->done_io += done;
			*punt = true;
			return (0);
		}
		if (done == 0) {
			if (buf_select) {
				sx_xlock(&ctx->bg_sx);
				kqring_bg_release(sel.bg);
				sx_xunlock(&ctx->bg_sx);
			}
			*punt = true;
			return (0);
		}
		error = 0;
	}
	if (is_write) {
		done += op->done_io;
		want += op->done_io;
		op->done_io = 0;
	}
	op->short_io = done < (size_t)want;
	/* RWF_DSYNC / RWF_SYNC: as an O_DSYNC / O_SYNC write. */
	if (is_write && error == 0 && vnode &&
	    (sqe->rw_flags & (KQRING_RWF_DSYNC | KQRING_RWF_SYNC)) != 0) {
		int32_t sres = kqring_exec_fsync(op,
		    (sqe->rw_flags & KQRING_RWF_SYNC) != 0);

		if (sres < 0)
			return (kqring_done(op, sres, 0));
	}
	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		kqring_bg_release(sel.bg);
		if (done > 0)
			cflags = kqring_buf_commit_op(ctx, op, &sel, done);
		else
			cflags = kqring_buf_eof(&sel);
		sx_xunlock(&ctx->bg_sx);
	}
	if (error != 0 && done == 0) {
		if ((error == EINTR || error == ERESTART) &&
		    (op->wq_canceled || op->cancelled))
			return (kqring_done(op, -ECANCELED, 0));
		return (kqring_done(op, kqring_errno(error), 0));
	}
	return (kqring_done(op, kqring_xfer_res(done), cflags));
punt:
	fdrop(fp, td);
	*punt = true;
	return (0);
}


/*
 * READ_MULTISHOT on a socket or a pipe: selects one provided buffer per
 * read, commits it only when bytes > 0, and terminates with a 0-byte CQE
 * (without consuming a buffer) on EOF.
 */
int
kqring_exec_read_multishot(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct kqring_sel sel;
	struct file *fp = NULL;
	struct pipe *rpipe;
	struct uio auio;
	struct iovec aiov;
	cap_rights_t rights;
	uint32_t cqe_flags;
	ssize_t want;
	size_t n;
	bool readable;
	int error;

	*more_out = false;
	if (expect != NULL || kqring_op_is_socket(op))
		return (kqring_exec_recv(ctx, op, expect, more_out));

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_READ),
	    FREAD, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (fp->f_type != DTYPE_PIPE) {
		/* A file that can't be polled is Linux's EBADFD. */
		error = fp->f_type == DTYPE_VNODE ? EBADF : EOPNOTSUPP;
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
	rpipe = fp->f_data;
	PIPE_LOCK(rpipe);
	readable = rpipe->pipe_buffer.cnt > 0 || rpipe->pipe_pages.cnt > 0 ||
	    (rpipe->pipe_state & PIPE_EOF) != 0;
	PIPE_UNLOCK(rpipe);
	if (!readable) {
		fdrop(fp, td);
		return (EAGAIN);
	}

	sx_xlock(&ctx->bg_sx);
	error = kqring_buf_select(ctx, sqe->buf_group, 0, &aiov, 1, &sel);
	if (error != 0) {
		sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
	sel.bg->busy = true;
	sx_xunlock(&ctx->bg_sx);
	kqring_uio_init(&auio, &aiov, 1, UIO_READ, false);
	want = auio.uio_resid;
	/*
	 * Never blocking: another reader can drain the pipe since it was
	 * seen readable, and this runs as taskwork (in an enter, or on the
	 * SQPOLL thread), where a wait would hold up the whole ring.
	 */
	error = kqring_pipe_rw_nowait(fp, &auio, false, 0, td);
	n = (size_t)(want - auio.uio_resid);
	sx_xlock(&ctx->bg_sx);
	kqring_bg_release(sel.bg);
	if (n == 0) {
		sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		if (error == EWOULDBLOCK || error == EAGAIN)
			return (EAGAIN);
		if (error != 0)
			return (kqring_done(op, kqring_errno(error), 0));
		return (kqring_done(op, 0, 0));
	}
	cqe_flags = kqring_buf_commit_op(ctx, op, &sel, n) | IORING_CQE_F_MORE;
	sx_xunlock(&ctx->bg_sx);
	PIPE_LOCK(rpipe);
	*more_out = rpipe->pipe_buffer.cnt > 0 || rpipe->pipe_pages.cnt > 0 ||
	    (rpipe->pipe_state & PIPE_EOF) != 0;
	PIPE_UNLOCK(rpipe);
	fdrop(fp, td);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/*
 * SPLICE: move up to sqe->len bytes between sqe->splice_fd_in (at
 * sqe->splice_off_in) and sqe->fd (at sqe->off); at least one endpoint
 * must be a pipe.
 */
int32_t
kqring_exec_splice(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp_in = NULL, *fp_out = NULL;
	cap_rights_t rights;
	off_t off_in = (off_t)sqe->splice_off_in;
	off_t off_out = (off_t)sqe->off;
	bool nonblock = (sqe->splice_flags & SPLICE_F_NONBLOCK) != 0;
	size_t want = sqe->len, cap = sqe->len, bufsz, nread, nwritten = 0;
	void *kbuf = NULL;
	struct uio auio;
	struct iovec aiov;
	int error, in_flags = 0, out_flags = 0;

	if (off_in != (off_t)-1)
		cap_rights_init_one(&rights, CAP_PREAD);
	else
		cap_rights_init_one(&rights, CAP_READ);
	error = kqring_op_fget_in(op, &rights, FREAD, &fp_in);
	if (error != 0)
		return (-error);
	if (off_out != (off_t)-1)
		cap_rights_init_one(&rights, CAP_PWRITE);
	else
		cap_rights_init_one(&rights, CAP_WRITE);
	error = kqring_op_fget(op, &rights, FWRITE, &fp_out);
	if (error != 0) {
		fdrop(fp_in, td);
		return (-error);
	}
	if (fp_in->f_type != DTYPE_PIPE && fp_out->f_type != DTYPE_PIPE) {
		error = EINVAL;
		goto out;
	}
	if (fp_in->f_type == DTYPE_PIPE && fp_out->f_type == DTYPE_PIPE) {
		struct pipe *pin = fp_in->f_data, *pout = fp_out->f_data;

		if (pin == pout || pin->pipe_peer == pout) {
			error = EINVAL;
			goto out;
		}
	}
	if (fp_in->f_type == DTYPE_PIPE || fp_in->f_type == DTYPE_SOCKET) {
		if (off_in != (off_t)-1) {
			error = ESPIPE;
			goto out;
		}
	} else if (off_in != (off_t)-1) {
		if ((fp_in->f_ops->fo_flags & DFLAG_SEEKABLE) == 0) {
			error = ESPIPE;
			goto out;
		}
		if (off_in < 0 || off_in > OFF_MAX - (off_t)want) {
			error = EINVAL;
			goto out;
		}
		in_flags = FOF_OFFSET;
	}
	if (fp_out->f_type == DTYPE_PIPE || fp_out->f_type == DTYPE_SOCKET) {
		if (off_out != (off_t)-1) {
			error = ESPIPE;
			goto out;
		}
	} else if (off_out != (off_t)-1) {
		if ((fp_out->f_ops->fo_flags & DFLAG_SEEKABLE) == 0) {
			error = ESPIPE;
			goto out;
		}
		if (off_out < 0 || off_out > OFF_MAX - (off_t)want) {
			error = EINVAL;
			goto out;
		}
		out_flags = FOF_OFFSET;
	}
	if (want == 0)
		goto out;

	/*
	 * Check output pipe health (and non-blocking space) before reading
	 * from fp_in so we never consume input bytes that cannot be written.
	 */
	if (fp_out->f_type == DTYPE_PIPE) {
		struct pipe *pout = fp_out->f_data, *wpipe;

		PIPE_LOCK(pout);
		wpipe = pout->pipe_peer;
		if (wpipe == NULL || wpipe->pipe_present != PIPE_ACTIVE ||
		    (wpipe->pipe_state & PIPE_EOF) != 0) {
			PIPE_UNLOCK(pout);
			error = EPIPE;
			goto out;
		}
		if (nonblock || (fp_out->f_flag & FNONBLOCK) != 0) {
			size_t space = (wpipe->pipe_state & PIPE_DIRECTW) != 0 ?
			    0 : (wpipe->pipe_buffer.size -
			    wpipe->pipe_buffer.cnt);

			if (space == 0) {
				PIPE_UNLOCK(pout);
				error = EAGAIN;
				goto out;
			}
			cap = MIN(cap, space);
		}
		PIPE_UNLOCK(pout);
	}

	if (fp_in->f_type == DTYPE_PIPE) {
		struct pipe *rpipe = fp_in->f_data;

		if (nonblock || (fp_in->f_flag & FNONBLOCK) != 0) {
			size_t avail;

			PIPE_LOCK(rpipe);
			avail = rpipe->pipe_buffer.cnt + rpipe->pipe_pages.cnt;
			if (avail == 0 && (rpipe->pipe_state & PIPE_EOF) == 0) {
				PIPE_UNLOCK(rpipe);
				error = EAGAIN;
				goto out;
			}
			PIPE_UNLOCK(rpipe);
			if (avail > 0)
				cap = MIN(cap, avail);
		} else {
			size_t avail;

			error = kqring_pipe_wait_read(ctx, fp_in, op);
			if (error != 0)
				goto out;
			PIPE_LOCK(rpipe);
			avail = rpipe->pipe_buffer.cnt + rpipe->pipe_pages.cnt;
			PIPE_UNLOCK(rpipe);
			if (avail > 0)
				cap = MIN(cap, avail);
		}
	}

	bufsz = MIN(cap, (size_t)BIG_PIPE_SIZE);
	kbuf = malloc(MAX(bufsz, 1), M_KQRING, M_WAITOK);
	aiov.iov_base = kbuf;
	aiov.iov_len = bufsz;
	bzero(&auio, sizeof(auio));
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = off_in;
	auio.uio_resid = bufsz;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = td;

	if (fp_in->f_type == DTYPE_SOCKET) {
		int sflags = (nonblock || (fp_in->f_flag & FNONBLOCK) != 0) ?
		    MSG_DONTWAIT : 0;

		if (sflags == 0 && !kqring_so_rcv_pending(fp_in->f_data, true))
			kqring_wq_mark_blocking(ctx, op);
		error = soreceive(fp_in->f_data, NULL, &auio, NULL, NULL,
		    &sflags);
	} else {
		error = fo_read(fp_in, &auio, td->td_ucred, in_flags, td);
	}
	nread = bufsz - (size_t)auio.uio_resid;
	if (nread == 0)
		goto out;
	error = 0;

	while (nwritten < nread) {
		size_t rem = nread - nwritten;

		aiov.iov_base = (char *)kbuf + nwritten;
		aiov.iov_len = rem;
		bzero(&auio, sizeof(auio));
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = off_out != (off_t)-1 ?
		    off_out + (off_t)nwritten : (off_t)-1;
		auio.uio_resid = rem;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_rw = UIO_WRITE;
		auio.uio_td = td;

		if (fp_out->f_type == DTYPE_SOCKET) {
			struct socket *so_out = fp_out->f_data;
			int sflags = MSG_NOSIGNAL |
			    ((nonblock || (fp_out->f_flag & FNONBLOCK) != 0) ?
			    MSG_DONTWAIT : 0);

			if ((sflags & MSG_DONTWAIT) == 0 &&
			    !SOLISTENING(so_out) &&
			    sbspace(&so_out->so_snd) < (long)rem)
				kqring_wq_mark_blocking(ctx, op);
			error = sosend(so_out, NULL, &auio, NULL, NULL,
			    sflags, td);
			/* A refused datagram used up the uio, unsent. */
			if (error != 0 &&
			    (so_out->so_proto->pr_flags & PR_ATOMIC) != 0)
				auio.uio_resid = (ssize_t)rem;
		} else {
			if (kqring_pipe_will_block(fp_out, true, rem))
				kqring_wq_mark_blocking(ctx, op);
			error = fo_write(fp_out, &auio, td->td_ucred,
			    out_flags, td);
		}
		nwritten += rem - (size_t)auio.uio_resid;
		if (error != 0)
			break;
	}
out:
	free(kbuf, M_KQRING);
	fdrop(fp_out, td);
	fdrop(fp_in, td);
	if (nwritten > 0)
		return (kqring_xfer_res(nwritten));
	if ((error == EINTR || error == ERESTART) &&
	    (op->wq_canceled || op->cancelled))
		return (-ECANCELED);
	return (kqring_errno(error));
}

/*
 * TEE: copy up to sqe->len bytes from pipe sqe->splice_fd_in to pipe
 * sqe->fd without consuming the input pipe's data.
 */
int32_t
kqring_exec_tee(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp_in = NULL, *fp_out = NULL;
	struct pipe *rpipe, *pout, *wpipe;
	cap_rights_t rights;
	bool nonblock = (sqe->splice_flags & SPLICE_F_NONBLOCK) != 0;
	size_t want = sqe->len, cap = sqe->len, n = 0, first;
	void *kbuf = NULL;
	struct uio auio;
	struct iovec aiov;
	int error;

	error = kqring_op_fget_in(op, cap_rights_init_one(&rights, CAP_READ),
	    FREAD, &fp_in);
	if (error != 0)
		return (-error);
	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_WRITE),
	    FWRITE, &fp_out);
	if (error != 0) {
		fdrop(fp_in, td);
		return (-error);
	}
	if (fp_in->f_type != DTYPE_PIPE || fp_out->f_type != DTYPE_PIPE) {
		error = EINVAL;
		goto out;
	}
	rpipe = fp_in->f_data;
	pout = fp_out->f_data;
	if (rpipe == pout || rpipe->pipe_peer == pout) {
		error = EINVAL;
		goto out;
	}
	if (want == 0)
		goto out;

	PIPE_LOCK(pout);
	wpipe = pout->pipe_peer;
	if (wpipe == NULL || wpipe->pipe_present != PIPE_ACTIVE ||
	    (wpipe->pipe_state & PIPE_EOF) != 0) {
		PIPE_UNLOCK(pout);
		error = EPIPE;
		goto out;
	}
	if (nonblock || (fp_out->f_flag & FNONBLOCK) != 0) {
		size_t space = (wpipe->pipe_state & PIPE_DIRECTW) != 0 ? 0 :
		    (wpipe->pipe_buffer.size - wpipe->pipe_buffer.cnt);

		if (space == 0) {
			PIPE_UNLOCK(pout);
			error = EAGAIN;
			goto out;
		}
		cap = MIN(cap, space);
	}
	PIPE_UNLOCK(pout);

	if (!nonblock && (fp_in->f_flag & FNONBLOCK) == 0) {
		error = kqring_pipe_wait_read(ctx, fp_in, op);
		if (error != 0)
			goto out;
	}

	cap = MIN(cap, (size_t)BIG_PIPE_SIZE);
	kbuf = malloc(cap, M_KQRING, M_WAITOK);
	PIPE_LOCK(rpipe);
	if (rpipe->pipe_buffer.cnt == 0) {
		error = (rpipe->pipe_state & PIPE_EOF) != 0 ? 0 : EAGAIN;
		PIPE_UNLOCK(rpipe);
		goto out;
	}
	n = MIN(cap, (size_t)rpipe->pipe_buffer.cnt);
	first = MIN(n, (size_t)(rpipe->pipe_buffer.size -
	    rpipe->pipe_buffer.out));
	memcpy(kbuf, rpipe->pipe_buffer.buffer + rpipe->pipe_buffer.out, first);
	if (n > first)
		memcpy((char *)kbuf + first, rpipe->pipe_buffer.buffer,
		    n - first);
	PIPE_UNLOCK(rpipe);

	aiov.iov_base = kbuf;
	aiov.iov_len = n;
	bzero(&auio, sizeof(auio));
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = (off_t)-1;
	auio.uio_resid = n;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = td;
	if (kqring_pipe_will_block(fp_out, true, n))
		kqring_wq_mark_blocking(ctx, op);
	error = fo_write(fp_out, &auio, td->td_ucred, 0, td);
	n -= (size_t)auio.uio_resid;
out:
	free(kbuf, M_KQRING);
	fdrop(fp_out, td);
	fdrop(fp_in, td);
	if (n > 0)
		return (kqring_xfer_res(n));
	if ((error == EINTR || error == ERESTART) &&
	    (op->wq_canceled || op->cancelled))
		return (-ECANCELED);
	return (kqring_errno(error));
}


/* Fill a Linux struct statx from FreeBSD stat(2) data. */
static void
kqring_stat_to_statx(const struct stat *sb, struct statx *stx)
{
	bzero(stx, sizeof(*stx));
	stx->stx_mask = STATX_BASIC_STATS;
	stx->stx_blksize = (uint32_t)sb->st_blksize;
	stx->stx_nlink = (uint32_t)MIN(sb->st_nlink, UINT32_MAX);
	stx->stx_uid = sb->st_uid;
	stx->stx_gid = sb->st_gid;
	stx->stx_mode = (uint16_t)sb->st_mode;
	stx->stx_ino = sb->st_ino;
	stx->stx_size = (uint64_t)sb->st_size;
	stx->stx_blocks = (uint64_t)sb->st_blocks;
	stx->stx_atime.tv_sec = sb->st_atim.tv_sec;
	stx->stx_atime.tv_nsec = (uint32_t)sb->st_atim.tv_nsec;
	stx->stx_mtime.tv_sec = sb->st_mtim.tv_sec;
	stx->stx_mtime.tv_nsec = (uint32_t)sb->st_mtim.tv_nsec;
	stx->stx_ctime.tv_sec = sb->st_ctim.tv_sec;
	stx->stx_ctime.tv_nsec = (uint32_t)sb->st_ctim.tv_nsec;
	/* File systems without a birth time leave it at VNOVAL. */
	if (sb->st_birthtim.tv_sec != (time_t)VNOVAL) {
		stx->stx_mask |= STATX_BTIME;
		stx->stx_btime.tv_sec = sb->st_birthtim.tv_sec;
		stx->stx_btime.tv_nsec = (uint32_t)sb->st_birthtim.tv_nsec;
	}
	stx->stx_rdev_major = (uint32_t)major(sb->st_rdev);
	stx->stx_rdev_minor = (uint32_t)minor(sb->st_rdev);
	stx->stx_dev_major = (uint32_t)major(sb->st_dev);
	stx->stx_dev_minor = (uint32_t)minor(sb->st_dev);
	stx->stx_attributes_mask = STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND |
	    STATX_ATTR_NODUMP;
	if ((sb->st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) != 0)
		stx->stx_attributes |= STATX_ATTR_IMMUTABLE;
	if ((sb->st_flags & (UF_APPEND | SF_APPEND)) != 0)
		stx->stx_attributes |= STATX_ATTR_APPEND;
	if ((sb->st_flags & UF_NODUMP) != 0)
		stx->stx_attributes |= STATX_ATTR_NODUMP;
}

int32_t
kqring_exec_statx(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa = op->args;
	struct statx stx;
	struct stat sb;
	int error;

	error = kern_statat(curthread, (int)sqe->statx_flags, sqe->fd,
	    pa->path, UIO_SYSSPACE, &sb);
	if (error != 0)
		return (-error);
	kqring_stat_to_statx(&sb, &stx);
	return (-copyout(&stx, (void *)(uintptr_t)sqe->addr2, sizeof(stx)));
}

/*
 * Set an extended attribute from the kernel copy of its value, as
 * extattr_set_vp() does for user memory.  XATTR_CREATE / XATTR_REPLACE
 * are checked under the same exclusive vnode lock as the set.
 */
static int
kqring_setxattr_vp(struct vnode *vp, const struct kqring_xattr_args *xa,
    size_t vlen, uint32_t xflags, struct thread *td)
{
	struct mount *mp;
	struct uio auio;
	struct iovec aiov;
	size_t size;
	int error;

	error = vn_start_write(vp, &mp, V_WAIT | V_PCATCH);
	if (error != 0)
		return (error);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);

	if (xflags != 0) {
		error = VOP_GETEXTATTR(vp, xa->ns, xa->name, NULL, &size,
		    td->td_ucred, td);
		if (error == 0 && (xflags & XATTR_CREATE) != 0)
			error = EEXIST;
		else if (error == ENOATTR && (xflags & XATTR_CREATE) != 0)
			error = 0;
		if (error != 0)
			goto done;
	}
#ifdef KQRING_MAC
	error = mac_vnode_check_setextattr(td->td_ucred, vp, xa->ns,
	    xa->name);
	if (error != 0)
		goto done;
#endif
	aiov.iov_base = __DECONST(char *, xa->value);
	aiov.iov_len = vlen;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_resid = vlen;
	auio.uio_rw = UIO_WRITE;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_td = td;
	error = VOP_SETEXTATTR(vp, xa->ns, xa->name, &auio, td->td_ucred, td);
done:
	VOP_UNLOCK(vp);
	vn_finished_write(mp);
	return (error);
}

/*
 * The vnode-backed file an op works on, as getvnode() (or with `path`,
 * getvnode_path(), which also takes O_PATH descriptors) looks it up.
 */
static int
kqring_op_getvnode(const struct kqring_op *op, const cap_rights_t *rights,
    bool path, struct file **fpp)
{
	struct file *fp;
	int error;

	error = kqring_op_fget(op, rights, 0, &fp);
	if (error != 0)
		return (error);
	if (fp->f_vnode == NULL) {
		fdrop(fp, curthread);
		return (EINVAL);
	}
	if (!path && fp->f_ops == &path_fileops) {
		fdrop(fp, curthread);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

/*
 * extattr_get_vp() (static in vfs_extattr.c): the value into user memory
 * `data`, or with data NULL just its size; the length goes to *lenp.
 */
static int
kqring_getxattr_vp(struct vnode *vp, const struct kqring_xattr_args *xa,
    void *data, size_t nbytes, size_t *lenp, struct thread *td)
{
	struct uio auio;
	struct iovec aiov;
	size_t size;
	int error;

	vn_lock(vp, LK_SHARED | LK_RETRY);
#ifdef KQRING_MAC
	error = mac_vnode_check_getextattr(td->td_ucred, vp, xa->ns,
	    xa->name);
	if (error != 0)
		goto done;
#endif
	if (data == NULL) {
		error = VOP_GETEXTATTR(vp, xa->ns, xa->name, NULL, &size,
		    td->td_ucred, td);
		*lenp = size;
		goto done;
	}
	aiov.iov_base = data;
	aiov.iov_len = nbytes;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_resid = nbytes;
	auio.uio_rw = UIO_READ;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_td = td;
	error = VOP_GETEXTATTR(vp, xa->ns, xa->name, &auio, NULL,
	    td->td_ucred, td);
	*lenp = nbytes - auio.uio_resid;
done:
	VOP_UNLOCK(vp);
	return (error);
}

int32_t
kqring_exec_setxattr(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_xattr_args *xa = op->args;
	struct thread *td = curthread;
	struct nameidata nd;
	struct file *fp;
	cap_rights_t rights;
	int error;

	if (sqe->opcode == IORING_OP_FSETXATTR) {
		error = kqring_op_getvnode(op,
		    cap_rights_init_one(&rights, CAP_EXTATTR_SET), true, &fp);
		if (error != 0)
			return (-error);
		error = kqring_setxattr_vp(fp->f_vnode, xa, sqe->len,
		    sqe->xattr_flags, td);
		fdrop(fp, td);
		return (kqring_errno(error));
	}
	NDINIT(&nd, LOOKUP, FOLLOW | AUDITVNODE1, UIO_SYSSPACE, xa->path);
	error = namei(&nd);
	if (error != 0)
		return (-error);
	NDFREE_PNBUF(&nd);
	error = kqring_setxattr_vp(nd.ni_vp, xa, sqe->len, sqe->xattr_flags,
	    td);
	vrele(nd.ni_vp);
	return (kqring_errno(error));
}

/*
 * GETXATTR / FGETXATTR into the user buffer.  extattr_get(2) truncates
 * silently, so ask for the size first and fail with ERANGE like Linux
 * when the buffer is too small (len 0 asks for the size).
 */
int32_t
kqring_exec_getxattr(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_xattr_args *xa = op->args;
	struct thread *td = curthread;
	void *data = (void *)(uintptr_t)sqe->addr2;
	struct file *fp = NULL;
	cap_rights_t rights;
	size_t len = 0;
	int error;

	if (sqe->opcode == IORING_OP_FGETXATTR) {
		error = kqring_op_getvnode(op,
		    cap_rights_init_one(&rights, CAP_EXTATTR_GET), true, &fp);
		if (error != 0)
			return (-error);
	}
	for (int pass = 0; pass < 2; pass++) {
		if (fp != NULL) {
			error = kqring_getxattr_vp(fp->f_vnode, xa,
			    pass == 0 ? NULL : data, pass == 0 ? 0 : sqe->len,
			    &len, td);
		} else {
			error = kern_extattr_get_path(td, xa->path, xa->ns,
			    xa->name, pass == 0 ? NULL : data,
			    pass == 0 ? 0 : sqe->len, FOLLOW, UIO_SYSSPACE);
			len = (size_t)td->td_retval[0];
		}
		if (error != 0)
			break;
		if (pass == 0 && sqe->len == 0)
			break;
		if (pass == 0 && len > sqe->len) {
			error = ERANGE;
			break;
		}
	}
	if (fp != NULL)
		fdrop(fp, td);
	if (error != 0)
		return (-error);
	return (kqring_xfer_res(len));
}

/*
 * FALLOCATE: posix_fallocate(2), or with KEEP_SIZE | PUNCH_HOLE
 * fspacectl(2) SPACECTL_DEALLOC, with their checks.
 */
int32_t
kqring_exec_fallocate(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	cap_rights_t rights;
	off_t off = (off_t)sqe->off, len = (off_t)sqe->addr;
	bool punch;
	int error;

	punch = sqe->len == (FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE);
	/* FreeBSD cannot allocate without extending (KEEP_SIZE). */
	if (sqe->len != 0 && !punch)
		return (-EOPNOTSUPP);
	if (off < 0 || len <= 0)
		return (-EINVAL);
	if (off > OFF_MAX - len)
		return (punch ? -EINVAL : -EFBIG);
	/*
	 * RLIMIT_FSIZE, as io_uring's fallocate checks it (from a worker, so
	 * without the SIGXFSZ write(2) would raise).
	 */
	if (!punch) {
		rlim_t fsize;

		/* The process's limit now: a worker's td_limit can be stale. */
		PROC_LOCK(td->td_proc);
		fsize = lim_cur_proc(td->td_proc, RLIMIT_FSIZE);
		PROC_UNLOCK(td->td_proc);
		if ((uint64_t)(off + len) > (uint64_t)fsize)
			return (-EFBIG);
	}
	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_PWRITE),
	    FWRITE, &fp);
	if (error != 0)
		return (-error);
	if ((fp->f_ops->fo_flags & DFLAG_SEEKABLE) == 0)
		error = ESPIPE;
	else if (punch)
		error = fo_fspacectl(fp, SPACECTL_DEALLOC, &off, &len, 0,
		    td->td_ucred, td);
	else
		error = fo_fallocate(fp, off, len, td);
	fdrop(fp, td);
	return (kqring_errno(error));
}

/* FSYNC / SYNC_FILE_RANGE: kern_fsync() on the op's file. */
int32_t
kqring_exec_fsync(const struct kqring_op *op, bool fullsync)
{
	struct thread *td = curthread;
	struct file *fp;
	struct vnode *vp;
	struct mount *mp;
	cap_rights_t rights;
	int error;

	error = kqring_op_getvnode(op, cap_rights_init_one(&rights, CAP_FSYNC),
	    false, &fp);
	if (error != 0)
		return (-error);
	vp = fp->f_vnode;
retry:
	error = vn_start_write(vp, &mp, V_WAIT | V_PCATCH);
	if (error != 0)
		goto drop;
	vn_lock(vp, vn_lktype_write(mp, vp) | LK_RETRY);
	vnode_pager_clean_async(vp);
	error = fullsync ? VOP_FSYNC(vp, MNT_WAIT, td) : VOP_FDATASYNC(vp, td);
	VOP_UNLOCK(vp);
	vn_finished_write(mp);
	if (error == ERELOOKUP)
		goto retry;
drop:
	fdrop(fp, td);
	return (kqring_errno(error));
}

int32_t
kqring_exec_ftruncate(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct file *fp;
	cap_rights_t rights;
	off_t length = (off_t)op->sqe.off;
	int error;

	if (length < 0)
		return (-EINVAL);
	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_FTRUNCATE),
	    0, &fp);
	if (error != 0)
		return (-error);
	/* As ftruncate(2): EINVAL, not EBADF, if not open for writing. */
	if ((fp->f_flag & FWRITE) == 0)
		error = EINVAL;
	else
		error = fo_truncate(fp, length, td->td_ucred, td);
	fdrop(fp, td);
	return (kqring_errno(error));
}

/*
 * FADVISE: kern_posix_fadvise() on the op's file.  SEQUENTIAL, RANDOM and
 * NOREUSE (and NORMAL, which clears them) record a range on the file that
 * vn_read() / vn_write() consult; WILLNEED and DONTNEED go to VOP_ADVISE().
 */
int32_t
kqring_exec_fadvise(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct fadvise_info *fa, *new = NULL;
	struct file *fp;
	struct vnode *vp;
	int advice = (int)sqe->fadvise_advice;
	/* A 64-bit length in addr, else the 32-bit one in len. */
	off_t offset = (off_t)sqe->off;
	off_t len = sqe->addr != 0 ? (off_t)sqe->addr : (off_t)sqe->len;
	off_t end;
	int error;

	if (offset < 0 || len < 0 || offset > OFF_MAX - len)
		return (-EINVAL);
	switch (advice) {
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_NOREUSE:
		new = malloc(sizeof(*fa), M_FADVISE, M_WAITOK);
		break;
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
		break;
	default:
		return (-EINVAL);
	}
	error = kqring_op_fget(op, &cap_no_rights, 0, &fp);
	if (error != 0)
		goto out;
	if ((fp->f_ops->fo_flags & DFLAG_SEEKABLE) == 0) {
		error = ESPIPE;
		goto drop;
	}
	if (fp->f_type != DTYPE_VNODE || (vp = fp->f_vnode)->v_type != VREG) {
		error = ENODEV;
		goto drop;
	}
	end = len == 0 ? OFF_MAX : offset + len - 1;
	switch (advice) {
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_NOREUSE:
		/* Merge with an overlapping or adjacent range of this advice. */
		mtx_pool_lock(mtxpool_sleep, fp);
		fa = fp->f_advice;
		if (fa != NULL && fa->fa_advice == advice &&
		    ((fa->fa_start <= end && fa->fa_end >= offset) ||
		    (end != OFF_MAX && fa->fa_start == end + 1) ||
		    (fa->fa_end != OFF_MAX && fa->fa_end + 1 == offset))) {
			if (offset < fa->fa_start)
				fa->fa_start = offset;
			if (end > fa->fa_end)
				fa->fa_end = end;
		} else {
			new->fa_advice = advice;
			new->fa_start = offset;
			new->fa_end = end;
			fp->f_advice = new;
			new = fa;
		}
		mtx_pool_unlock(mtxpool_sleep, fp);
		break;
	case POSIX_FADV_NORMAL:
		/* Trim or drop a recorded range this one overlaps. */
		mtx_pool_lock(mtxpool_sleep, fp);
		fa = fp->f_advice;
		if (fa != NULL) {
			if (offset <= fa->fa_start && end >= fa->fa_end) {
				new = fa;
				fp->f_advice = NULL;
			} else if (offset <= fa->fa_start &&
			    end >= fa->fa_start) {
				fa->fa_start = end + 1;
			} else if (offset <= fa->fa_end && end >= fa->fa_end) {
				fa->fa_end = offset - 1;
			} else if (offset >= fa->fa_start &&
			    end <= fa->fa_end) {
				new = fa;
				fp->f_advice = NULL;
			}
		}
		mtx_pool_unlock(mtxpool_sleep, fp);
		break;
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
		error = VOP_ADVISE(vp, offset, end, advice);
		break;
	}
drop:
	fdrop(fp, td);
out:
	free(new, M_FADVISE);
	return (-error);
}


/*
 * CLOSE: like close(2), or with file_index, empty that fixed slot.  A ring
 * cannot close itself or another ring.
 */
int32_t
kqring_exec_close(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct file *fp;
	bool ring;
	int error;

	if (sqe->file_index != 0)
		return (-kqring_fixed_remove(ctx, sqe->file_index - 1));
	error = fget(td, sqe->fd, &cap_no_rights, &fp);
	if (error != 0)
		return (-error);
	ring = kqring_fp_is_ring(fp);
	fdrop(fp, td);
	if (ring)
		return (-EBADF);
	return (-kern_close(td, sqe->fd));
}


/*
 * The Capsicum rights a file opened relative to dirfd gets, which a fixed
 * slot has to carry itself: as openat(2) does, the directory's rights if
 * they are limited (namei()'s NIRES_STRICTREL), else all of them.
 */
static void
kqring_open_caps(struct thread *td, int dirfd, struct filecaps *caps)
{
	struct file *dfp;
	cap_rights_t all;

	if (dirfd != AT_FDCWD &&
	    fget_cap(td, dirfd, &cap_no_rights, NULL, &dfp, caps) == 0) {
		fdrop(dfp, td);
		CAP_ALL(&all);
		if (!cap_rights_contains(&caps->fc_rights, &all))
			return;
		filecaps_free(caps);
	}
	kqring_caps_all(caps);
}

/*
 * OPENAT2's RESOLVE_IN_ROOT, RESOLVE_NO_XDEV and RESOLVE_NO_SYMLINKS, and
 * RESOLVE_BENEATH with any of them, as Linux's path walk (fs/namei.c)
 * defines them.  FreeBSD's namei() takes its root from the process, and
 * can't refuse a symlink or a mount point along the way, so the path is
 * walked here a component at a time: each is looked up with namei() from
 * the directory reached so far, without following it; a symlink is read,
 * and its target goes in front of what is left of the path; ".." goes back
 * to the directory the walk came from (a stack of them), and only past the
 * start to the real parent.  The last component is opened from its
 * directory with vn_open_cred(), as openat(2) would open it.
 *
 * A scoped walk (IN_ROOT, BENEATH) never leaves its root: ".." only goes
 * back the way the walk came, and an absolute symlink starts again at the
 * root.  Linux takes the real parent for "..", and fails with EAGAIN if a
 * rename meanwhile might have moved it above the root; here a directory
 * renamed meanwhile is left by the way the walk came.  In capability mode
 * a walk that isn't scoped is held beneath its start as namei() would
 * hold it, with ENOTCAPABLE.
 */
#define	KQRING_WALK_LINKS	40	/* Linux's MAXSYMLINKS */
#define	KQRING_WALK_DEPTH	(MAXPATHLEN / 2)

struct kqring_walk {
	struct thread	*td;
	uint64_t	resolve;
	bool		strict;		/* capability mode, unscoped */
	struct vnode	*root;		/* where "/" and ".." stop */
	struct vnode	*cur;
	struct vnode	**up;		/* the directories cur came through */
	int		depth;
};

static void
kqring_walk_free(struct kqring_walk *w)
{
	while (w->depth > 0)
		vrele(w->up[--w->depth]);
	if (w->cur != NULL)
		vrele(w->cur);
	if (w->root != NULL)
		vrele(w->root);
	free(w->up, M_TEMP);
}

/* Linux's may_lookup(): search permission on the directory. */
static int
kqring_walk_access(struct kqring_walk *w)
{
	int error;

	vn_lock(w->cur, LK_SHARED | LK_RETRY);
	error = VOP_ACCESS(w->cur, VEXEC, w->td->td_ucred, w->td);
	VOP_UNLOCK(w->cur);
	return (error);
}

/* One component from w->cur, not followed: *vpp referenced, unlocked. */
static int
kqring_walk_lookup(struct kqring_walk *w, const char *name,
    struct vnode **vpp)
{
	struct nameidata nd;
	int error;

	vrefact(w->cur);	/* namei() consumes it */
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW | NOCAPCHECK, UIO_SYSSPACE, name,
	    w->cur);
	error = namei(&nd);
	if (error != 0)
		return (error);
	NDFREE_PNBUF(&nd);
	*vpp = nd.ni_vp;
	return (0);
}

/* ".." from w->cur. */
static int
kqring_walk_dotdot(struct kqring_walk *w)
{
	struct vnode *vp;
	int error;

	if ((error = kqring_walk_access(w)) != 0)
		return (error);
	if (w->cur == w->root) {
		if ((w->resolve & RESOLVE_BENEATH) != 0)
			return (EXDEV);
		if (w->strict)
			return (ENOTCAPABLE);
		return (0);			/* IN_ROOT, or the process's */
	}
	if (w->depth > 0) {
		vp = w->up[--w->depth];
	} else {
		/* Past the start of a walk that isn't scoped. */
		if (w->strict)
			return (ENOTCAPABLE);
		if ((error = kqring_walk_lookup(w, "..", &vp)) != 0)
			return (error);
	}
	if ((w->resolve & RESOLVE_NO_XDEV) != 0 &&
	    vp->v_mount != w->cur->v_mount) {
		vrele(vp);
		return (EXDEV);
	}
	vrele(w->cur);
	w->cur = vp;
	return (0);
}

/* An absolute path or symlink: back to the root. */
static int
kqring_walk_jump_root(struct kqring_walk *w, bool start)
{
	if ((w->resolve & RESOLVE_BENEATH) != 0)
		return (EXDEV);
	if (w->strict)
		return (ENOTCAPABLE);
	/* As Linux, a path may start at "/" on another mount. */
	if (!start && (w->resolve & RESOLVE_NO_XDEV) != 0 &&
	    w->cur->v_mount != w->root->v_mount)
		return (EXDEV);
	while (w->depth > 0)
		vrele(w->up[--w->depth]);
	vref(w->root);
	vrele(w->cur);
	w->cur = w->root;
	return (0);
}

/* vp's target, NUL-terminated, into buf (MAXPATHLEN). */
static int
kqring_walk_readlink(struct kqring_walk *w, struct vnode *vp, char *buf,
    size_t *lenp)
{
	struct iovec aiov;
	struct uio auio;
	int error;

	if ((vp->v_mount->mnt_flag & MNT_NOSYMFOLLOW) != 0)
		return (EACCES);
	aiov.iov_base = buf;
	aiov.iov_len = MAXPATHLEN;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_rw = UIO_READ;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_td = w->td;
	auio.uio_resid = MAXPATHLEN;
	vn_lock(vp, LK_SHARED | LK_RETRY);
#ifdef MAC
	error = mac_vnode_check_readlink(w->td->td_ucred, vp);
	if (error == 0)
#endif
		error = VOP_READLINK(vp, &auio, w->td->td_ucred);
	VOP_UNLOCK(vp);
	if (error != 0)
		return (error);
	*lenp = MAXPATHLEN - auio.uio_resid;
	if (*lenp == MAXPATHLEN)
		return (ENAMETOOLONG);
	if (*lenp == 0)
		return (ENOENT);
	buf[*lenp] = '\0';
	return (0);
}

/*
 * openat(2) of name in dvp (referenced, kept), into an uninstalled file:
 * openatfp() from a directory vnode rather than a descriptor.  The walk
 * has followed every symlink it should, so name is opened without
 * following one (a symlink put there meanwhile fails it).
 */
static int
kqring_open_vp(struct thread *td, struct vnode *dvp, const char *name,
    int flags, int mode, struct file **fpp)
{
	struct nameidata nd;
	struct file *fp;
	struct vnode *vp;
	int cmode, error;

	/* openflags() */
	if ((flags & O_PATH) != 0)
		flags &= ~O_ACCMODE;
	else if ((flags & O_EXEC) != 0) {
		if ((flags & O_ACCMODE) != 0)
			return (EINVAL);
	} else if ((flags & O_ACCMODE) == O_ACCMODE)
		return (EINVAL);
	else
		flags = FFLAGS(flags);
	flags |= O_NOFOLLOW;

	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (error);
	fp->f_flag = flags & FMASK;
	cmode = ((mode & ~td->td_proc->p_pd->pd_cmask) & ALLPERMS) & ~S_ISTXT;
	vrefact(dvp);		/* namei() consumes it */
	NDINIT_ATVP(&nd, LOOKUP, NOCAPCHECK, UIO_SYSSPACE, name, dvp);
	error = vn_open_cred(&nd, &flags, cmode, VN_OPEN_NOCAPCHECK,
	    td->td_ucred, fp);
	if (error != 0) {
		/* devfs replaced the file's methods: it is open. */
		if (error == ENXIO && fp->f_ops != &badfileops) {
			*fpp = fp;
			return (0);
		}
		falloc_abort(td, fp);
		return (error);
	}
	NDFREE_PNBUF(&nd);
	vp = nd.ni_vp;
	/* finit_open() */
	fp->f_vnode = vp;
	if (fp->f_ops == &badfileops) {
		if ((flags & O_PATH) != 0)
			finit(fp, (flags & FMASK) | (fp->f_flag & FKQALLOWED),
			    DTYPE_VNODE, NULL, &path_fileops);
		else
			finit_vnode(fp, flags, NULL, &vnops);
	}
	VOP_UNLOCK(vp);
	if ((flags & O_TRUNC) != 0) {
		error = fo_truncate(fp, 0, td->td_ucred, td);
		if (error != 0) {
			falloc_abort(td, fp);
			return (error);
		}
	}
	*fpp = fp;
	return (0);
}

static int
kqring_open_walk(struct thread *td, int dirfd, const char *path,
    const struct open_how *how, struct file **fpp)
{
	struct kqring_walk w = { .td = td, .resolve = how->resolve };
	struct vnode *start, *vp;
	struct pwd *pwd;
	char *buf, *link, *p, *comp;
	size_t len, rest;
	int flags = (int)how->flags, links = 0, error;
	bool scoped = (how->resolve & (RESOLVE_BENEATH | RESOLVE_IN_ROOT)) != 0;
	bool last, slash, follow;

	if (path[0] == '\0')
		return (ENOENT);
	/* Where the walk starts: dirfd, which must be a directory. */
	if (dirfd == AT_FDCWD) {
		if (IN_CAPABILITY_MODE(td))
			return (ECAPMODE);
		pwd = pwd_hold(td);
		start = pwd->pwd_cdir;
		vref(start);
		pwd_drop(pwd);
	} else {
		struct file *dfp;
		cap_rights_t rights;

		error = fget_cap(td, dirfd, cap_rights_init_one(&rights,
		    CAP_LOOKUP), NULL, &dfp, NULL);
		if (error != 0)
			return (error);
		start = dfp->f_vnode;
		if (dfp->f_type != DTYPE_VNODE || start == NULL ||
		    start->v_type != VDIR) {
			fdrop(dfp, td);
			return (ENOTDIR);
		}
		vref(start);
		fdrop(dfp, td);
	}
	w.strict = !scoped && IN_CAPABILITY_MODE(td);
	if (scoped || w.strict) {
		w.root = start;
		vref(start);
	} else {
		pwd = pwd_hold(td);
		w.root = pwd->pwd_rdir;
		vref(w.root);
		pwd_drop(pwd);
	}
	w.cur = start;
	w.up = malloc(KQRING_WALK_DEPTH * sizeof(*w.up), M_TEMP, M_WAITOK);
	buf = malloc(2 * MAXPATHLEN, M_TEMP, M_WAITOK);
	link = buf + MAXPATHLEN;
	strlcpy(buf, path, MAXPATHLEN);

	error = 0;
	p = buf;
	if (*p == '/' && (how->resolve & RESOLVE_IN_ROOT) == 0)
		error = kqring_walk_jump_root(&w, true);
	while (error == 0) {
		while (*p == '/')
			p++;
		if (*p == '\0') {
			/* The path ends at a directory: "/", or with "/". */
			error = kqring_walk_access(&w);
			if (error == 0)
				error = kqring_open_vp(td, w.cur, ".", flags,
				    (int)how->mode, fpp);
			break;
		}
		comp = p;
		while (*p != '/' && *p != '\0')
			p++;
		slash = *p == '/';
		if (slash)
			*p++ = '\0';
		while (*p == '/')
			p++;
		last = *p == '\0';
		if (strlen(comp) > NAME_MAX) {
			error = ENAMETOOLONG;
			break;
		}
		if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
			error = comp[1] == '.' ? kqring_walk_dotdot(&w) :
			    kqring_walk_access(&w);
			if (error == 0 && last)
				error = kqring_open_vp(td, w.cur, ".", flags,
				    (int)how->mode, fpp);
			if (last)
				break;
			continue;
		}
		error = kqring_walk_lookup(&w, comp, &vp);
		if (last && error == ENOENT && (flags & O_CREAT) != 0) {
			/* Created by the open, if the directory allows. */
			error = 0;
			vp = NULL;
		}
		if (error != 0)
			break;
		/* A trailing "/" follows, as on Linux. */
		follow = !last || slash || ((flags & O_NOFOLLOW) == 0 &&
		    (flags & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL));
		if (vp != NULL && vp->v_type == VLNK && follow) {
			if ((how->resolve & RESOLVE_NO_SYMLINKS) != 0)
				error = ELOOP;
			else if (++links > KQRING_WALK_LINKS)
				error = ELOOP;
			else
				error = kqring_walk_readlink(&w, vp, link,
				    &len);
			vrele(vp);
			if (error != 0)
				break;
			/* The target, then what was left (with its "/"). */
			rest = strlen(p);
			if (len + (slash ? 1 : 0) + rest >= MAXPATHLEN) {
				error = ENAMETOOLONG;
				break;
			}
			if (slash)
				link[len++] = '/';
			memcpy(link + len, p, rest + 1);
			memcpy(buf, link, len + rest + 1);
			p = buf;
			if (*p == '/')
				error = kqring_walk_jump_root(&w, false);
			continue;
		}
		if (vp != NULL && (how->resolve & RESOLVE_NO_XDEV) != 0 &&
		    vp->v_mount != w.cur->v_mount) {
			vrele(vp);
			error = EXDEV;
			break;
		}
		if (last) {
			if (vp != NULL)
				vrele(vp);
			/* Its name, with the "/" that requires a directory. */
			if (slash)
				comp[strlen(comp)] = '/';
			error = kqring_open_vp(td, w.cur, comp, flags,
			    (int)how->mode, fpp);
			break;
		}
		if (vp->v_type != VDIR) {
			vrele(vp);
			error = ENOTDIR;
			break;
		}
		if (w.depth == KQRING_WALK_DEPTH) {
			vrele(vp);
			error = ENAMETOOLONG;
			break;
		}
		w.up[w.depth++] = w.cur;
		w.cur = vp;
	}
	free(buf, M_TEMP);
	kqring_walk_free(&w);
	return (error);
}

/* OPENAT / OPENAT2: openat(2), or with file_index into the fixed table. */
int32_t
kqring_exec_openat(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa = op->args;
	struct thread *td = curthread;
	struct filecaps caps;
	struct file *fp;
	int error, fd;
	bool walk = (pa->how.resolve & (RESOLVE_IN_ROOT | RESOLVE_NO_XDEV |
	    RESOLVE_NO_SYMLINKS)) != 0;

	if (sqe->file_index == 0 && !walk) {
		error = kern_openat(td, sqe->fd, pa->path, UIO_SYSSPACE,
		    (int)pa->how.flags, (int)pa->how.mode);
		/* RESOLVE_BENEATH (O_RESOLVE_BENEATH): Linux's error. */
		if (error == ENOTCAPABLE &&
		    (pa->how.resolve & RESOLVE_BENEATH) != 0 &&
		    !IN_CAPABILITY_MODE(td))
			error = EXDEV;
		return (error == 0 ? (int32_t)td->td_retval[0] :
		    kqring_errno(error));
	}
	kqring_open_caps(td, sqe->fd, &caps);
	error = walk ? kqring_open_walk(td, sqe->fd, pa->path, &pa->how, &fp) :
	    kern_openatfp(td, sqe->fd, pa->path, UIO_SYSSPACE,
	    (int)pa->how.flags, (int)pa->how.mode, &fp);
	if (error == ENOTCAPABLE && !walk &&
	    (pa->how.resolve & RESOLVE_BENEATH) != 0 && !IN_CAPABILITY_MODE(td))
		error = EXDEV;
	if (error != 0) {
		filecaps_free(&caps);
		return (kqring_errno(error));
	}
	if (sqe->file_index == 0) {
		cap_rights_t all;

		/* The directory's rights if they are limited, as openat(2). */
		CAP_ALL(&all);
		if (cap_rights_contains(&caps.fc_rights, &all)) {
			filecaps_free(&caps);
			error = finstall_refed(td, fp, &fd, (int)pa->how.flags,
			    NULL);
		} else {
			/* finstall_refed() consumes the caps on success. */
			error = finstall_refed(td, fp, &fd, (int)pa->how.flags,
			    &caps);
			if (error != 0)
				filecaps_free(&caps);
		}
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_errno(error));
		}
		return (fd);
	}
	if (op->wq_running) {
		if (kqring_fp_is_ring(fp)) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return (-EBADF);
		}
		op->wq_fp = fp;
		filecaps_move(&caps, &op->wq_caps);
		return (0);
	}
	return (kqring_fixed_install(ctx, fp, &caps, sqe->file_index));
}

/*
 * PIPE: pipe2(2) into sqe->addr's int[2], or with file_index into two
 * fixed slots (file_index - 1 and the next, or two free ones), whose
 * indexes go to sqe->addr instead.  There is no fd-less pipe KPI, so a
 * direct pipe briefly has descriptors other threads could see.
 */
int32_t
kqring_exec_pipe(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct filecaps caps[2];
	struct file *fp[2];
	int fds[2], slots[2], error;
	int32_t res;

	/* Two slots from file_index - 1 (and not wrapping into ALLOC). */
	if (sqe->file_index != 0 && sqe->file_index != IORING_FILE_INDEX_ALLOC &&
	    sqe->file_index > IORING_FILE_INDEX_ALLOC - 2)
		return (-EINVAL);
	error = kern_pipe(td, fds, (int)sqe->pipe_flags, NULL, NULL);
	if (error != 0)
		return (-error);
	if (sqe->file_index == 0) {
		error = copyout(fds, (void *)(uintptr_t)sqe->addr, sizeof(fds));
		if (error != 0) {
			(void)kern_close(td, fds[0]);
			(void)kern_close(td, fds[1]);
		}
		return (-error);
	}
	/* Take both ends out of the descriptor table. */
	for (int i = 0; i < 2; i++) {
		error = fget_cap(td, fds[i], &cap_no_rights, NULL, &fp[i],
		    &caps[i]);
		if (error != 0) {
			/* Another thread closed it already. */
			if (i == 0) {
				(void)kern_close(td, fds[1]);
			} else {
				filecaps_free(&caps[0]);
				fdrop(fp[0], td);
			}
			return (-error);
		}
		fdclose(td, fp[i], fds[i]);
	}
	for (int i = 0; i < 2; i++) {
		res = kqring_fixed_install(ctx, fp[i], &caps[i],
		    sqe->file_index == IORING_FILE_INDEX_ALLOC ?
		    IORING_FILE_INDEX_ALLOC : sqe->file_index + i);
		if (res < 0) {
			if (i == 1)
				(void)kqring_fixed_remove(ctx, slots[0]);
			else {
				filecaps_free(&caps[1]);
				fdrop(fp[1], td);
			}
			return (res);
		}
		slots[i] = sqe->file_index == IORING_FILE_INDEX_ALLOC ? res :
		    (int)(sqe->file_index - 1 + i);
	}
	error = copyout(slots, (void *)(uintptr_t)sqe->addr, sizeof(slots));
	if (error != 0) {
		(void)kqring_fixed_remove(ctx, slots[0]);
		(void)kqring_fixed_remove(ctx, slots[1]);
	}
	return (-error);
}
