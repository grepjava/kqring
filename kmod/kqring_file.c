/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

#include <sys/bio.h>
#include <sys/buf.h>
#include <geom/geom.h>

static bool
kqring_pipe_readable(const struct file *fp)
{
	struct pipe *rpipe;
	bool ok;

	/* O_NONBLOCK too: io_uring waits for a non-blocking pipe's data. */
	if (fp->f_type != DTYPE_PIPE)
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

	if (fp->f_type != DTYPE_PIPE || (fp->f_flag & FNONBLOCK) != 0 ||
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
		if (!op->wq_blocking) {
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
	vm_page_unhold_pages(bp->bio_ma, bp->bio_ma_n);
	free(bp->bio_ma, M_KQRING);
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
	op->cflags = 0;
	op->bio_done = true;
	ctx->bio_held -= op->bio_len;
	kqring_taskwork_add_locked(ctx, op);
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

		pages = malloc(sizeof(*pages) * maxpages, M_KQRING, M_WAITOK);
		npages = vm_fault_quick_hold_pages(
		    &curproc->p_vmspace->vm_map, va, len, prot, pages,
		    maxpages);
		if (npages < 0) {
			free(pages, M_KQRING);
			break;
		}
		bp = g_alloc_bio();
		bp->bio_cmd = is_write ? BIO_WRITE : BIO_READ;
		bp->bio_offset = offset;
		bp->bio_length = len;
		bp->bio_bcount = len;
		bp->bio_dev = dev;
		bp->bio_done = kqring_bio_done;
		bp->bio_caller1 = op;
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

/*
 * READ / WRITE on a descriptor that is not a socket, on the issuer when
 * that cannot block it, as io_uring tries non-blocking I/O before punting
 * to io-wq:
 *
 *  - A pipe with data to read (or room to write), blocking or not.  A
 *    blocking pipe has no per-call non-blocking read, so if another
 *    reader drains it between the check and the read, the issuer waits
 *    in pipe_read() as read(2) would; an empty one goes to a worker.
 *  - A positioned read of a regular file that the page cache satisfies.
 *    VOP_READ_PGCACHE grabs resident pages without sleeping and gives up
 *    with EJUSTRETURN otherwise; vn_read() tries it first the same way.
 *    A destination that is not mapped yet, or a contended range, also
 *    goes to the worker.
 *
 * A positioned read or write of a disk device goes to its driver instead
 * (kqring_file_bio()).
 *
 * Returns KQRING_RW_DONE with op->res set if the request completed, or
 * KQRING_RW_BIO if it was sent to a disk driver.  Otherwise
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
	struct iovec aiov, *iov = &aiov;
	u_int iovcnt = 1;
	ssize_t want;
	size_t done;
	void *rl;
	int error, flags, save;

	if (kqring_rw_fget(op, is_write, &fp, &flags) != 0)
		return (KQRING_RW_PUNT);	/* the worker reports the error */
	/*
	 * A pipe that is ready completes here, even blocking (the read or
	 * write then doesn't wait).  One that isn't waits on a worker, even
	 * O_NONBLOCK, as io_uring polls rather than fail with EAGAIN; with
	 * RWF_NOWAIT the issuer fails it instead.
	 */
	if (fp->f_type == DTYPE_PIPE && (is_write ?
	    !kqring_pipe_will_block(fp, true, 1) : kqring_pipe_readable(fp))) {
		fdrop(fp, td);
		kqring_exec_file_rw(ctx, op, is_write);
		return (KQRING_RW_DONE);
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
	    (vn_irflag_read(vp) & (VIRF_DOOMED | VIRF_PGREAD)) != VIRF_PGREAD) {
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	if (kqring_op_readv(sqe->opcode)) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (void *)(uintptr_t)sqe->addr;
		aiov.iov_len = sqe->len;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_READ);
	auio.uio_offset = (off_t)sqe->off;
	want = auio.uio_resid;
	if (want == 0 || auio.uio_offset > OFF_MAX - want) {
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
		fdrop(fp, td);
		return (KQRING_RW_PUNT);
	}
	save = vm_fault_disable_pagefaults();
	error = VOP_READ_PGCACHE(vp, &auio, 0, fp->f_cred);
	vm_fault_enable_pagefaults(save);
	vn_rangelock_unlock(vp, rl);
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
		if (done == 0 || obj == NULL || auio.uio_offset <
		    (off_t)atomic_load_64(&obj->un_pager.vnp.vnp_size)) {
			fdrop(fp, td);
			return (KQRING_RW_PUNT);
		}
		error = 0;
	}
	if (error == 0)
		fp->f_nextoff[UIO_READ] = auio.uio_offset;
	fdrop(fp, td);
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
	int error, flags;

	error = kqring_rw_fget(op, is_write, &fp, &flags);
	if (error != 0)
		return (kqring_done(op, -error, 0));

	if (buf_select) {
		struct kqring_bgroup *bg;
again:
		if (!is_write && !kqring_pipe_readable(fp)) {
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
		 */
		if (error == ENOBUFS && op->wq_running && !op->wq_canceled &&
		    !op->cancelled && !ctx->closing &&
		    (bg = kqring_bgroup_find(ctx, sqe->buf_group)) != NULL &&
		    bg->busy) {
			sx_xunlock(&ctx->bg_sx);
			if (kqring_wq_suspend_point(ctx)) {
				fdrop(fp, td);
				return (kqring_done(op, -ECANCELED, 0));
			}
			pause("kqbgbz", 1);
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
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (void *)(uintptr_t)sqe->addr;
		aiov.iov_len = sqe->len;
	}
	kqring_uio_init(&auio, iov, iovcnt, is_write ? UIO_WRITE : UIO_READ);
	ssize_t want = auio.uio_resid;
	auio.uio_offset = offset;

	/* Zero-length reads finish here, as in dofileread(). */
	if (want == 0 && !is_write) {
		if (buf_select) {
			sx_xlock(&ctx->bg_sx);
			sel.bg->busy = false;
			sx_xunlock(&ctx->bg_sx);
		}
		fdrop(fp, td);
		return (kqring_done(op, 0, 0));
	}
	if (!buf_select && !is_write && !kqring_pipe_readable(fp)) {
		error = kqring_pipe_wait_read(ctx, fp, op);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, kqring_errno(error), 0));
		}
	}
	if (kqring_pipe_will_block(fp, is_write, (size_t)want))
		kqring_wq_mark_blocking(ctx, op);
	error = is_write ? fo_write(fp, &auio, td->td_ucred, flags, td) :
	    fo_read(fp, &auio, td->td_ucred, flags, td);
	bool vnode = fp->f_type == DTYPE_VNODE;
	fdrop(fp, td);

	size_t done = (size_t)(want - auio.uio_resid);
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
		sel.bg->busy = false;
		if (done > 0)
			cflags = kqring_buf_commit(ctx, &sel, done);
		sx_xunlock(&ctx->bg_sx);
	}
	if (error != 0 && done == 0) {
		if ((error == EINTR || error == ERESTART) &&
		    (op->wq_canceled || op->cancelled))
			return (kqring_done(op, -ECANCELED, 0));
		return (kqring_done(op, kqring_errno(error), 0));
	}
	return (kqring_done(op, kqring_xfer_res(done), cflags));
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
		fdrop(fp, td);
		return (kqring_done(op, -EOPNOTSUPP, 0));
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
	kqring_uio_init(&auio, &aiov, 1, UIO_READ);
	want = auio.uio_resid;
	error = fo_read(fp, &auio, td->td_ucred, 0, td);
	n = (size_t)(want - auio.uio_resid);
	sx_xlock(&ctx->bg_sx);
	sel.bg->busy = false;
	if (n == 0) {
		sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		if (error == EWOULDBLOCK || error == EAGAIN)
			return (EAGAIN);
		if (error != 0)
			return (kqring_done(op, kqring_errno(error), 0));
		return (kqring_done(op, 0, 0));
	}
	cqe_flags = kqring_buf_commit(ctx, &sel, n) | IORING_CQE_F_MORE;
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
	if (!punch && (uint64_t)(off + len) >
	    (uint64_t)lim_cur_proc(td->td_proc, RLIMIT_FSIZE))
		return (-EFBIG);
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

/* OPENAT / OPENAT2: openat(2), or with file_index into the fixed table. */
int32_t
kqring_exec_openat(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa = op->args;
	struct thread *td = curthread;
	struct filecaps caps;
	struct file *fp;
	int error;

	if (sqe->file_index == 0) {
		error = kern_openat(td, sqe->fd, pa->path, UIO_SYSSPACE,
		    (int)pa->how.flags, (int)pa->how.mode);
		return (error == 0 ? (int32_t)td->td_retval[0] :
		    kqring_errno(error));
	}
	kqring_open_caps(td, sqe->fd, &caps);
	error = kern_openatfp(td, sqe->fd, pa->path, UIO_SYSSPACE,
	    (int)pa->how.flags, (int)pa->how.mode, &fp);
	if (error != 0) {
		filecaps_free(&caps);
		return (kqring_errno(error));
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
