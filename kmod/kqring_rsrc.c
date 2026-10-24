/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

/* ============================================================================
 * Provided Buffer Groups (PBUF_RING, PROVIDE_BUFFERS)
 * ============================================================================
 *
 * Buffers are picked by kqring_buf_select() right before a transfer and
 * consumed by kqring_buf_commit() only once bytes moved, under bg_sx.
 * Blocking workers reserve the group head and release bg_sx during I/O.
 * A transfer that hits EOF or fails without moving bytes leaves the group
 * unchanged. Removal/unregister of a reserved group fails EBUSY; otherwise
 * requests look their group up again on every attempt.
 */


struct kqring_bgroup *
kqring_bgroup_find(struct kqring_ctx *ctx, uint16_t bgid)
{
	struct kqring_bgroup *bg;

	LIST_FOREACH(bg, kqring_bgroup_bucket(ctx, bgid), link) {
		if (bg->bgid == bgid)
			return (bg);
	}
	return (NULL);
}

/* A ring's tail, which userspace publishes after filling entries. */
static int
kqring_bring_tail(const struct kqring_bgroup *bg, uint16_t *tail)
{
	int32_t v;

	if (bg->ring_kva != NULL) {
		*tail = atomic_load_acq_16(&bg->ring_kva->tail);
		return (0);
	}
	/* fuword16() is cheaper than copyin() for 2 bytes; -1 means fault. */
	v = fuword16((const void *)(uintptr_t)(bg->ring_uaddr +
	    offsetof(struct io_uring_buf_ring, tail)));
	if (v == -1)
		return (EFAULT);
	*tail = (uint16_t)v;
	return (0);
}

static int
kqring_bring_get(const struct kqring_bgroup *bg, uint16_t idx,
    struct io_uring_buf *b)
{
	idx &= bg->ring_mask;
	if (bg->ring_kva != NULL) {
		/* Userspace may be writing it: a torn entry only faults. */
		*b = ((volatile struct io_uring_buf *)bg->ring_kva)[idx];
		return (0);
	}
	return (copyin((const void *)(uintptr_t)(bg->ring_uaddr +
	    idx * sizeof(*b)), b, sizeof(*b)));
}

/* IOU_PBUF_RING_INC: store what is left of entry idx (addr, len). */
static int
kqring_bring_put(const struct kqring_bgroup *bg, uint16_t idx,
    const struct io_uring_buf *b)
{
	volatile struct io_uring_buf *e;

	idx &= bg->ring_mask;
	if (bg->ring_kva != NULL) {
		e = &((volatile struct io_uring_buf *)bg->ring_kva)[idx];
		e->addr = b->addr;
		e->len = b->len;
		return (0);
	}
	CTASSERT(offsetof(struct io_uring_buf, len) == sizeof(uint64_t));
	return (copyout(b, (void *)(uintptr_t)(bg->ring_uaddr +
	    idx * sizeof(*b)), offsetof(struct io_uring_buf, bid)));
}

/*
 * Pick buffers from group bgid for a transfer of at most `max` bytes (0:
 * no limit): one buffer, or for a bundle up to `nmax` consecutive ones,
 * described in iov[] (the last one trimmed to `max`).  ENOBUFS if the
 * group has none.  Nothing is consumed yet.
 */
int
kqring_buf_select(struct kqring_ctx *ctx, uint16_t bgid, size_t max,
    struct iovec *iov, u_int nmax, struct kqring_sel *sel)
{
	struct kqring_bgroup *bg;
	struct kqring_kbuf *kb;
	struct io_uring_buf b;
	uint16_t tail;
	size_t total = 0;
	u_int n = 0;
	int error;

	bg = kqring_bgroup_find(ctx, bgid);
	if (bg == NULL || bg->busy)
		return (ENOBUFS);
	sel->bg = bg;
	nmax = MIN(nmax, KQRING_MAX_BUNDLE);
	if (max == 0 || max > (size_t)INT32_MAX)
		max = INT32_MAX;
	if (bg->is_ring) {
		error = kqring_bring_tail(bg, &tail);
		if (error != 0)
			return (error);
		/* A buffer consumed in parts is used one at a time. */
		if ((bg->ring_flags & IOU_PBUF_RING_INC) != 0)
			nmax = 1;
		for (uint16_t h = bg->head; h != tail && n < nmax; h++) {
			error = kqring_bring_get(bg, h, &b);
			if (error != 0)
				return (error);
			if (b.len > (uint32_t)INT32_MAX)
				return (EINVAL);
			if (n == 0) {
				sel->bid = b.bid;
				sel->addr0 = b.addr;
				sel->len0 = b.len;
			}
			iov[n].iov_base = (void *)(uintptr_t)b.addr;
			iov[n].iov_len = b.len;
			total += iov[n++].iov_len;
			if (max != 0 && total >= max)
				break;
		}
	} else {
		STAILQ_FOREACH(kb, &bg->bufs, link) {
			if (n == nmax)
				break;
			if (kb->len > (uint32_t)INT32_MAX)
				return (EINVAL);
			if (n == 0) {
				sel->bid = kb->bid;
				sel->addr0 = kb->addr;
				sel->len0 = kb->len;
			}
			iov[n].iov_base = (void *)(uintptr_t)kb->addr;
			iov[n].iov_len = kb->len;
			total += iov[n++].iov_len;
			if (max != 0 && total >= max)
				break;
		}
	}
	if (n == 0)
		return (ENOBUFS);
	if (max != 0 && total > max)
		iov[n - 1].iov_len -= total - max;
	for (u_int i = 0; i < n; i++)
		sel->lens[i] = (uint32_t)iov[i].iov_len;
	sel->nbufs = n;
	return (0);
}

/*
 * Consume the selected buffers that n > 0 bytes went into, and return the
 * CQE flags reporting them (the first buffer's ID).  With
 * IOU_PBUF_RING_INC, a buffer only partly used stays at the head with
 * what is left of it, and IORING_CQE_F_BUF_MORE says so.
 */
uint32_t
kqring_buf_commit(struct kqring_ctx *ctx, const struct kqring_sel *sel,
    size_t n)
{
	struct kqring_bgroup *bg = sel->bg;
	struct kqring_kbuf *kb;
	uint32_t cflags;
	size_t left = n;
	u_int used;

	KASSERT(n > 0, ("kqring: committing an empty transfer"));
	cflags = IORING_CQE_F_BUFFER |
	    ((uint32_t)sel->bid << IORING_CQE_BUFFER_SHIFT);
	for (used = 0; used < sel->nbufs && left > 0; used++)
		left -= MIN(left, sel->lens[used]);
	if (!bg->is_ring) {
		while (used-- > 0) {
			kb = STAILQ_FIRST(&bg->bufs);
			STAILQ_REMOVE_HEAD(&bg->bufs, link);
			bg->nbufs--;
			ctx->nprovided--;
			free(kb, M_KQRING);
		}
		return (cflags);
	}
	if ((bg->ring_flags & IOU_PBUF_RING_INC) != 0 && n < sel->len0) {
		struct io_uring_buf b = {
			.addr = sel->addr0 + n,
			.len = sel->len0 - (uint32_t)n,
		};

		/* If the ring cannot be updated, use the buffer up. */
		if (kqring_bring_put(bg, bg->head, &b) == 0)
			return (cflags | IORING_CQE_F_BUF_MORE);
	}
	bg->head += used;
	return (cflags);
}


/*
 * Drop a reference to a registered file or buffer; the last one posts its
 * tag CQE (not while the ring is being torn down) and releases the file.
 * Owner thread (or kqring_dtor()), ctx->mtx not held: fdrop() may close.
 */
void
kqring_rsrc_put(struct kqring_ctx *ctx, struct kqring_rsrc *r)
{
	if (--r->refs != 0)
		return;
	if (r->tag != 0 && !ctx->closing)
		kqring_post_cqe(ctx, r->tag, 0, 0);
	if (r->fp != NULL) {
		filecaps_free(&r->caps);
		fdrop(r->fp, curthread);
	}
	if (r->zbuf != NULL) {
		kqring_zc_buf_rele(r->zbuf);
		r->zbuf = NULL;
	}
	free(r, M_KQRING);
}

/* Give every Capsicum right, as a descriptor kqring itself created has. */
void
kqring_caps_all(struct filecaps *caps)
{
	filecaps_init(caps);
	CAP_ALL(&caps->fc_rights);
	caps->fc_fcntls = CAP_FCNTL_ALL;
}

/*
 * A new fixed-file slot holding `fp` (whose reference passes to it) with
 * the rights in `caps` (moved into it).
 */
static struct kqring_rsrc *
kqring_rsrc_file(struct file *fp, struct filecaps *caps, uint64_t tag)
{
	struct kqring_rsrc *r;

	r = malloc(sizeof(*r), M_KQRING, M_WAITOK | M_ZERO);
	r->refs = 1;
	r->tag = tag;
	r->fp = fp;
	filecaps_move(caps, &r->caps);
	return (r);
}

/* ============================================================================
 * Fixed Files (IORING_REGISTER_FILES, direct descriptors)
 * ============================================================================
 */



/* A free slot for IORING_FILE_INDEX_ALLOC, searched from the hint. */
static int
kqring_fixed_alloc(struct kqring_ctx *ctx, uint32_t *slotp)
{
	uint32_t n = ctx->file_alloc_end - ctx->file_alloc_off;
	uint32_t s = ctx->file_alloc_hint;

	for (uint32_t i = 0; i < n; i++, s++) {
		if (s < ctx->file_alloc_off || s >= ctx->file_alloc_end)
			s = ctx->file_alloc_off;
		if (ctx->files[s] == NULL) {
			ctx->file_alloc_hint = s + 1;
			*slotp = s;
			return (0);
		}
	}
	return (ENFILE);
}

/*
 * Put `fp` in the fixed table, passing on its reference and `caps`: in
 * slot file_index - 1, replacing what was there, or for
 * IORING_FILE_INDEX_ALLOC in a free slot.  Returns the slot for ALLOC,
 * else 0, or a negative errno (and drops fp).
 */
int32_t
kqring_fixed_install(struct kqring_ctx *ctx, struct file *fp,
    struct filecaps *caps, uint32_t file_index)
{
	struct kqring_rsrc *old;
	uint32_t slot;
	int32_t res = 0;
	int error;

	if (ctx->files == NULL) {
		/* No slot to allocate from: ENFILE, as io_uring. */
		res = file_index == IORING_FILE_INDEX_ALLOC ? -ENFILE : -ENXIO;
		goto fail;
	}
	/*
	 * A ring held in a ring's table (its own, or in a cycle) would never
	 * be closed, as io_uring found; nothing is gained by allowing it.
	 */
	if (kqring_fp_is_ring(fp)) {
		res = -EBADF;
		goto fail;
	}
	if (file_index == IORING_FILE_INDEX_ALLOC) {
		error = kqring_fixed_alloc(ctx, &slot);
		if (error != 0) {
			res = -error;
			goto fail;
		}
		res = (int32_t)slot;
	} else {
		slot = file_index - 1;
		if (slot >= ctx->nfiles) {
			res = -EINVAL;
			goto fail;
		}
	}
	old = ctx->files[slot];
	ctx->files[slot] = kqring_rsrc_file(fp, caps, 0);
	if (old != NULL)
		kqring_rsrc_put(ctx, old);
	return (res);
fail:
	filecaps_free(caps);
	fdrop(fp, curthread);
	return (res);
}

/*
 * Set fixed slot `slot` to descriptor fd's file, with its Capsicum rights,
 * or empty it (fd -1).  A tag only goes with a file.
 */
static int
kqring_files_set(struct kqring_ctx *ctx, uint32_t slot, int fd,
    uint64_t tag)
{
	struct thread *td = curthread;
	struct kqring_rsrc *old, *r = NULL;
	struct filecaps caps;
	struct file *fp;

	if (fd == -1) {
		if (tag != 0)
			return (EINVAL);
	} else {
		if (fget_cap(td, fd, &cap_no_rights, NULL, &fp, &caps) != 0)
			return (EBADF);
		if (kqring_fp_is_ring(fp)) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return (EBADF);
		}
		r = kqring_rsrc_file(fp, &caps, tag);
	}
	old = ctx->files[slot];
	ctx->files[slot] = r;
	if (old != NULL)
		kqring_rsrc_put(ctx, old);
	return (0);
}

/*
 * FILES_UPDATE (op and register): slots [off, off + nr) from the user's
 * fds (IORING_REGISTER_FILES_SKIP leaves a slot alone) and optional tags.
 * Returns how many were processed, or a negative errno if none was.
 */
int32_t
kqring_files_update(struct kqring_ctx *ctx, uint32_t off, uint64_t ufds,
    uint64_t utags, uint32_t nr)
{
	uint64_t tag = 0;
	uint32_t i;
	int32_t fd;
	int error = 0;

	if (ctx->files == NULL)
		return (-ENXIO);
	if (off > ctx->nfiles || nr > ctx->nfiles - off)
		return (-EINVAL);
	for (i = 0; i < nr; i++) {
		error = copyin((const void *)(uintptr_t)(ufds + i * sizeof(fd)),
		    &fd, sizeof(fd));
		if (error == 0 && utags != 0)
			error = copyin((const void *)(uintptr_t)(utags +
			    i * sizeof(tag)), &tag, sizeof(tag));
		if (error != 0)
			break;
		if (fd == IORING_REGISTER_FILES_SKIP) {
			if (tag != 0) {
				error = EINVAL;
				break;
			}
			continue;
		}
		error = kqring_files_set(ctx, off + i, fd, tag);
		if (error != 0)
			break;
	}
	return (i != 0 ? (int32_t)i : -error);
}

/*
 * FILES_UPDATE with offset IORING_FILE_INDEX_ALLOC: each fd goes to a free
 * slot, whose index is written back over it.
 */
int32_t
kqring_files_update_alloc(struct kqring_ctx *ctx, uint64_t ufds,
    uint32_t nr)
{
	struct thread *td = curthread;
	struct filecaps caps;
	struct file *fp;
	uint32_t i;
	int32_t fd, slot = 0;
	int error = 0;

	if (ctx->files == NULL)
		return (-ENXIO);
	for (i = 0; i < nr; i++) {
		void *ufd = (void *)(uintptr_t)(ufds + i * sizeof(fd));

		error = copyin(ufd, &fd, sizeof(fd));
		if (error != 0)
			break;
		error = fget_cap(td, fd, &cap_no_rights, NULL, &fp, &caps);
		if (error != 0)
			break;
		slot = kqring_fixed_install(ctx, fp, &caps,
		    IORING_FILE_INDEX_ALLOC);
		if (slot < 0) {
			error = -slot;
			break;
		}
		error = copyout(&slot, ufd, sizeof(slot));
		if (error != 0) {
			(void)kqring_fixed_remove(ctx, (uint32_t)slot);
			break;
		}
	}
	return (i != 0 ? (int32_t)i : -error);
}

/*
 * A buffer for IORING_REGISTER_BUFFERS: up to KQRING_MAX_ZC_BUF_LEN of
 * mapped, writable user memory (held and charged to RLIMIT_MEMLOCK and
 * vm.max_user_wired in kqring_bufs_set), or the empty slot {NULL, 0}.
 */
static int
kqring_buf_validate(struct thread *td, const struct iovec *iov)
{
	vm_map_t map = &td->td_proc->p_vmspace->vm_map;
	vm_offset_t start = (vm_offset_t)iov->iov_base;
	vm_offset_t end = start + iov->iov_len;
	boolean_t ok;

	if (iov->iov_base == NULL)
		return (iov->iov_len != 0 ? EFAULT : 0);
	if (iov->iov_len == 0 || iov->iov_len > KQRING_MAX_BUF_LEN)
		return (EFAULT);
	if (end < start)
		return (EOVERFLOW);
	if (end > vm_map_max(map))
		return (EFAULT);
	vm_map_lock_read(map);
	ok = vm_map_check_protection(map, trunc_page(start), round_page(end),
	    VM_PROT_READ | VM_PROT_WRITE);
	vm_map_unlock_read(map);
	return (ok ? 0 : EFAULT);
}

/* Set registered buffer slot `slot`; a tag only goes with a buffer. */
static int
kqring_bufs_set(struct kqring_ctx *ctx, uint32_t slot,
    const struct iovec *iov, uint64_t tag)
{
	struct thread *td = curthread;
	struct kqring_rsrc *old, *r = NULL;
	int error;

	error = kqring_buf_validate(td, iov);
	if (error != 0)
		return (error);
	old = ctx->bufs[slot];
	if (iov->iov_base == NULL) {
		if (tag != 0)
			return (EINVAL);
	} else {
		struct kqring_zc_buf *zb = NULL;
		size_t discount_pages = 0;

		if (old != NULL && old->refs == 1 && old->zbuf != NULL &&
		    atomic_load_int(&old->zbuf->refs) == 1 &&
		    old->zbuf->acct->uip == td->td_ucred->cr_ruidinfo)
			discount_pages = (size_t)old->zbuf->npages;
		error = kqring_zc_buf_alloc(td, (vm_offset_t)iov->iov_base,
		    iov->iov_len, VM_PROT_READ | VM_PROT_WRITE, discount_pages,
		    &zb);
		if (error != 0)
			return (error);
		r = malloc(sizeof(*r), M_KQRING, M_WAITOK | M_ZERO);
		r->refs = 1;
		r->tag = tag;
		r->addr = (uintptr_t)iov->iov_base;
		r->len = iov->iov_len;
		r->zbuf = zb;
		r->cow_gen = atomic_load_int(&ctx->cow_gen);
	}
	ctx->bufs[slot] = r;
	if (old != NULL)
		kqring_rsrc_put(ctx, old);
	return (0);
}

/* Whether [addr, addr + len) lies inside registered buffer r. */

/* Empty fixed slot `slot` (CLOSE with file_index, FILES_UPDATE). */
int
kqring_fixed_remove(struct kqring_ctx *ctx, uint32_t slot)
{
	struct kqring_rsrc *r;

	if (ctx->files == NULL)
		return (ENXIO);
	if (slot >= ctx->nfiles)
		return (EINVAL);
	if ((r = ctx->files[slot]) == NULL)
		return (EBADF);
	ctx->files[slot] = NULL;
	kqring_rsrc_put(ctx, r);
	return (0);
}


/*
 * The file an op works on: its fixed file (IOSQE_FIXED_FILE), held since
 * issue and checked against the Capsicum rights it was registered with,
 * or sqe->fd looked up now.  `fflag` (FREAD, FWRITE or 0) is the open mode
 * needed, as fget_read() / fget_write() check it.
 */
int
kqring_op_fget(const struct kqring_op *op, const cap_rights_t *rights,
    u_int fflag, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (op->ffile != NULL) {
		error = cap_check(&op->ffile->caps.fc_rights, rights);
		if (error != 0)
			return (error);
		fp = op->ffile->fp;
		if (!fhold(fp))
			return (EBADF);
	} else {
		error = fget(td, op->sqe.fd, rights, &fp);
		if (error != 0)
			return (error);
		if (op->armed_fp != NULL && fp != op->armed_fp) {
			fdrop(fp, td);
			return (EBADF);
		}
	}
	if ((fp->f_flag & fflag) != fflag) {
		fdrop(fp, td);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

/* Input file for SPLICE / TEE (sqe->splice_fd_in or SPLICE_F_FD_IN_FIXED). */
int
kqring_op_fget_in(const struct kqring_op *op, const cap_rights_t *rights,
    u_int fflag, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (op->ffile_in != NULL) {
		error = cap_check(&op->ffile_in->caps.fc_rights, rights);
		if (error != 0)
			return (error);
		fp = op->ffile_in->fp;
		if (!fhold(fp))
			return (EBADF);
	} else {
		error = fget(td, op->sqe.splice_fd_in, rights, &fp);
		if (error != 0)
			return (error);
	}
	if ((fp->f_flag & fflag) != fflag) {
		fdrop(fp, td);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

/*
 * kqring_op_fget(), also returning a copy of the file's Capsicum state
 * (for the checks sogetopt() and ioctls make, and for accept to pass on);
 * the caller frees it with filecaps_free().
 */
int
kqring_op_fget_caps(const struct kqring_op *op, const cap_rights_t *rights,
    struct file **fpp, struct filecaps *caps)
{
	int error;

	if (op->ffile == NULL)
		return (fget_cap(curthread, op->sqe.fd, rights, NULL, fpp,
		    caps));
	error = kqring_op_fget(op, rights, 0, fpp);
	if (error == 0)
		(void)filecaps_copy(&op->ffile->caps, caps, false);
	return (error);
}

/* FIXED_FD_INSTALL: a descriptor for a fixed file, with its rights. */
int32_t
kqring_exec_fixed_fd_install(const struct kqring_op *op)
{
	struct filecaps caps;
	int error, fd;

	(void)filecaps_copy(&op->ffile->caps, &caps, false);
	error = finstall(curthread, op->ffile->fp, &fd,
	    (op->sqe.install_fd_flags & IORING_FIXED_FD_NO_CLOEXEC) != 0 ? 0 :
	    O_CLOEXEC, &caps);
	if (error != 0) {
		filecaps_free(&caps);
		return (-error);
	}
	return (fd);
}

/* A new, empty provided buffer group, not yet in the table. */
static struct kqring_bgroup *
kqring_bgroup_new(uint16_t bgid)
{
	struct kqring_bgroup *bg;

	bg = malloc(sizeof(*bg), M_KQRING, M_WAITOK | M_ZERO);
	bg->bgid = bgid;
	STAILQ_INIT(&bg->bufs);
	return (bg);
}

/* mmap(2) of IOU_PBUF_RING_MMAP rings looks groups up under mtx. */
static void
kqring_bgroup_insert(struct kqring_ctx *ctx, struct kqring_bgroup *bg)
{
	mtx_lock(&ctx->mtx);
	LIST_INSERT_HEAD(kqring_bgroup_bucket(ctx, bg->bgid), bg, link);
	mtx_unlock(&ctx->mtx);
}

/*
 * PROVIDE_BUFFERS: sqe->fd buffers of sqe->len bytes each from sqe->addr,
 * with IDs from sqe->off, into group sqe->buf_group (a buffer list,
 * created if need be).  Limited per group, as in io_uring, and per ring.
 */
int32_t
kqring_provide_buffers(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct kqring_bgroup *bg;
	struct kqring_kbuf *kb;
	uint32_t nbufs = (uint32_t)sqe->fd;

	sx_xlock(&ctx->bg_sx);
	bg = kqring_bgroup_find(ctx, sqe->buf_group);
	if (bg != NULL && bg->is_ring) {
		sx_xunlock(&ctx->bg_sx);
		return (-EINVAL);
	}
	if ((bg != NULL ? bg->nbufs : 0) + nbufs > KQRING_MAX_GROUP_BUFS ||
	    ctx->nprovided + nbufs > KQRING_MAX_PROVIDED) {
		sx_xunlock(&ctx->bg_sx);
		return (-ENOMEM);
	}
	if (bg == NULL) {
		bg = kqring_bgroup_new(sqe->buf_group);
		kqring_bgroup_insert(ctx, bg);
	}
	for (uint32_t i = 0; i < nbufs; i++) {
		kb = malloc(sizeof(*kb), M_KQRING, M_WAITOK);
		kb->addr = sqe->addr + (uint64_t)i * sqe->len;
		kb->len = sqe->len;
		kb->bid = (uint16_t)(sqe->off + i);
		STAILQ_INSERT_TAIL(&bg->bufs, kb, link);
	}
	bg->nbufs += nbufs;
	ctx->nprovided += nbufs;
	sx_xunlock(&ctx->bg_sx);
	return (0);
}

/* REMOVE_BUFFERS: up to sqe->fd buffers from a list group; how many. */
int32_t
kqring_remove_buffers(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct kqring_bgroup *bg;
	struct kqring_kbuf *kb;
	int32_t n = 0;

	sx_xlock(&ctx->bg_sx);
	bg = kqring_bgroup_find(ctx, sqe->buf_group);
	if (bg == NULL) {
		sx_xunlock(&ctx->bg_sx);
		return (-ENOENT);
	}
	if (bg->busy) {
		sx_xunlock(&ctx->bg_sx);
		return (-EBUSY);
	}
	/* Ring buffers belong to userspace. */
	if (bg->is_ring) {
		sx_xunlock(&ctx->bg_sx);
		return (-EINVAL);
	}
	while (n < sqe->fd && (kb = STAILQ_FIRST(&bg->bufs)) != NULL) {
		STAILQ_REMOVE_HEAD(&bg->bufs, link);
		free(kb, M_KQRING);
		n++;
	}
	bg->nbufs -= n;
	ctx->nprovided -= n;
	sx_xunlock(&ctx->bg_sx);
	return (n);
}


/* ============================================================================
 * Registration (IORING_REGISTER_*)
 * ============================================================================
 */

/* Release the fixed file table (UNREGISTER_FILES, teardown). */
void
kqring_files_free(struct kqring_ctx *ctx)
{
	struct kqring_rsrc **files = ctx->files;
	uint32_t n = ctx->nfiles;

	if (files == NULL)
		return;
	ctx->files = NULL;
	ctx->nfiles = 0;
	ctx->file_alloc_off = ctx->file_alloc_end = ctx->file_alloc_hint = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (files[i] != NULL)
			kqring_rsrc_put(ctx, files[i]);
	}
	free(files, M_KQRING);
}

/* Release the registered buffer table (UNREGISTER_BUFFERS, teardown). */
void
kqring_bufs_free(struct kqring_ctx *ctx)
{
	struct kqring_rsrc **bufs = ctx->bufs;
	uint32_t n = ctx->nbufs;

	if (bufs == NULL)
		return;
	ctx->bufs = NULL;
	ctx->nbufs = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (bufs[i] != NULL)
			kqring_rsrc_put(ctx, bufs[i]);
	}
	free(bufs, M_KQRING);
}

/* Free a provided buffer group already taken out of the table. */
void
kqring_bgroup_free(struct kqring_ctx *ctx, struct kqring_bgroup *bg)
{
	struct kqring_kbuf *kb;

	while ((kb = STAILQ_FIRST(&bg->bufs)) != NULL) {
		STAILQ_REMOVE_HEAD(&bg->bufs, link);
		free(kb, M_KQRING);
	}
	ctx->nprovided -= bg->nbufs;
	if (bg->ring_obj != NULL) {
		/* Mappings userspace still has keep their own reference. */
		ctx->ring_mem -= bg->ring_alloc_len;
		kqring_free_shm(bg->ring_obj, bg->ring_kva_base,
		    bg->ring_alloc_len);
	}
	free(bg, M_KQRING);
}

/*
 * IORING_REGISTER_FILES / FILES2: a table of nr slots, filled from the
 * user's fds (-1: empty) and optional tags, or all empty (sparse).
 */
int
kqring_files_register(struct kqring_ctx *ctx, uint64_t ufds, uint64_t utags,
    uint32_t nr, bool sparse)
{
	struct thread *td = curthread;
	uint64_t tag = 0;
	int32_t fd;
	int error = 0;

	if (ctx->files != NULL)
		return (EBUSY);
	if (nr == 0)
		return (EINVAL);
	if (nr > KQRING_MAX_FILES || (rlim_t)nr > lim_cur(td, RLIMIT_NOFILE))
		return (EMFILE);
	ctx->files = mallocarray(nr, sizeof(*ctx->files), M_KQRING,
	    M_WAITOK | M_ZERO);
	ctx->nfiles = nr;
	ctx->file_alloc_off = ctx->file_alloc_hint = 0;
	ctx->file_alloc_end = nr;
	for (uint32_t i = 0; i < nr && !sparse; i++) {
		error = copyin((const void *)(uintptr_t)(ufds + i * sizeof(fd)),
		    &fd, sizeof(fd));
		if (error == 0 && utags != 0)
			error = copyin((const void *)(uintptr_t)(utags +
			    i * sizeof(tag)), &tag, sizeof(tag));
		if (error == 0)
			error = kqring_files_set(ctx, i, fd, tag);
		if (error != 0)
			break;
	}
	if (error != 0)
		kqring_files_free(ctx);
	return (error);
}

/* IORING_REGISTER_BUFFERS / BUFFERS2: a table of nr registered buffers. */
int
kqring_bufs_register(struct kqring_ctx *ctx, uint64_t uiov, uint64_t utags,
    uint32_t nr, bool sparse)
{
	struct iovec iov = { NULL, 0 };
	uint64_t tag = 0;
	int error = 0;

	if (ctx->bufs != NULL)
		return (EBUSY);
	if (nr == 0 || nr > KQRING_MAX_BUFS)
		return (EINVAL);
	ctx->bufs = mallocarray(nr, sizeof(*ctx->bufs), M_KQRING,
	    M_WAITOK | M_ZERO);
	ctx->nbufs = nr;
	for (uint32_t i = 0; i < nr && !sparse; i++) {
		error = copyin((const void *)(uintptr_t)(uiov +
		    i * sizeof(iov)), &iov, sizeof(iov));
		if (error == 0 && utags != 0)
			error = copyin((const void *)(uintptr_t)(utags +
			    i * sizeof(tag)), &tag, sizeof(tag));
		if (error == 0)
			error = kqring_bufs_set(ctx, i, &iov, tag);
		if (error != 0)
			break;
	}
	if (error != 0)
		kqring_bufs_free(ctx);
	return (error);
}

/* BUFFERS_UPDATE: slots [off, off + nr); how many were done. */
int32_t
kqring_bufs_update(struct kqring_ctx *ctx, uint32_t off, uint64_t uiov,
    uint64_t utags, uint32_t nr)
{
	struct iovec iov;
	uint64_t tag = 0;
	uint32_t i;
	int error = 0;

	if (ctx->bufs == NULL)
		return (-ENXIO);
	if (off > ctx->nbufs || nr > ctx->nbufs - off)
		return (-EINVAL);
	for (i = 0; i < nr; i++) {
		error = copyin((const void *)(uintptr_t)(uiov +
		    i * sizeof(iov)), &iov, sizeof(iov));
		if (error == 0 && utags != 0)
			error = copyin((const void *)(uintptr_t)(utags +
			    i * sizeof(tag)), &tag, sizeof(tag));
		if (error == 0)
			error = kqring_bufs_set(ctx, off + i, &iov, tag);
		if (error != 0)
			break;
	}
	return (i != 0 ? (int32_t)i : -error);
}


/*
 * IORING_REGISTER_CLONE_BUFFERS: copy registered buffers [src_off,
 * src_off + nr) of another ring to [dst_off, ...) of this one.  The source
 * table is its owner's alone, so both rings must have the same owner (this
 * thread); each cloned slot shares the source's reference-counted
 * kqring_zc_buf.
 */
int
kqring_bufs_clone(struct kqring_ctx *ctx, const struct kqring_register_arg *ra)
{
	struct thread *td = curthread;
	struct io_uring_clone_buffers cb;
	struct kqring_rsrc **tab, *r, *s, *old;
	struct kqring_ctx *sctx;
	struct file *fp;
	cap_rights_t rights;
	uint32_t n, nr;
	int error;

	if (ra->arg == NULL || ra->nr_args != 1)
		return (EINVAL);
	error = copyin(ra->arg, &cb, sizeof(cb));
	if (error != 0)
		return (error);
	if ((cb.flags & ~(IORING_REGISTER_SRC_REGISTERED |
	    IORING_REGISTER_DST_REPLACE)) != 0 ||
	    cb.pad[0] != 0 || cb.pad[1] != 0 || cb.pad[2] != 0)
		return (EINVAL);
	if (ctx->bufs != NULL &&
	    (cb.flags & IORING_REGISTER_DST_REPLACE) == 0)
		return (EBUSY);
	if ((cb.flags & IORING_REGISTER_SRC_REGISTERED) != 0)
		error = kqring_reg_ring_fget(td, cb.src_fd, false, &fp);
	else
		error = fget(td, (int)cb.src_fd, cap_rights_init_one(&rights,
		    CAP_IOCTL), &fp);
	if (error != 0)
		return (error);
	error = kqring_fp_ctx(fp, &sctx);
	if (error == 0 && sctx == ctx)
		error = EINVAL;
	else if (error == 0 && !kqring_is_owner(sctx, td))
		error = EEXIST;
	else if (error == 0 && sctx->closing)
		error = EBADF;
	else if (error == 0 && sctx->bufs == NULL)
		error = ENXIO;
	if (error != 0)
		goto out;
	nr = cb.nr != 0 ? cb.nr : sctx->nbufs;
	if (cb.src_off > sctx->nbufs || nr > sctx->nbufs - cb.src_off ||
	    cb.dst_off > KQRING_MAX_BUFS || nr > KQRING_MAX_BUFS - cb.dst_off) {
		error = EINVAL;
		goto out;
	}
	n = MAX(ctx->nbufs, cb.dst_off + nr);
	tab = mallocarray(n, sizeof(*tab), M_KQRING, M_WAITOK | M_ZERO);
	if (ctx->bufs != NULL)
		memcpy(tab, ctx->bufs, ctx->nbufs * sizeof(*tab));
	free(ctx->bufs, M_KQRING);
	ctx->bufs = tab;
	ctx->nbufs = n;
	for (uint32_t i = 0; i < nr; i++) {
		s = sctx->bufs[cb.src_off + i];
		r = NULL;
		if (s != NULL) {
			r = malloc(sizeof(*r), M_KQRING, M_WAITOK | M_ZERO);
			r->refs = 1;
			r->addr = s->addr;
			r->len = s->len;
			r->cow_gen = s->cow_gen;
			if (s->zbuf != NULL) {
				refcount_acquire(&s->zbuf->refs);
				r->zbuf = s->zbuf;
			}
		}
		old = tab[cb.dst_off + i];
		tab[cb.dst_off + i] = r;
		if (old != NULL)
			kqring_rsrc_put(ctx, old);
	}
out:
	fdrop(fp, td);
	return (error);
}

/*
 * IORING_REGISTER_PBUF_RING: a buffer ring for group bgid, in user memory
 * (page aligned, as io_uring requires) or, with IOU_PBUF_RING_MMAP, in
 * wired kernel memory that userspace maps.  A mapped ring is a VM object
 * its mappings reference, so unregistering it while mapped leaves the
 * process with plain memory, never freed pages (Linux CVE-2024-0582).
 */
int
kqring_pbuf_register(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct io_uring_buf_reg reg;
	struct kqring_bgroup *bg, *old;
	size_t bytes;
	int error;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	error = copyin(ra->arg, &reg, sizeof(reg));
	if (error != 0)
		return (error);
	if (reg.resv[0] != 0 || reg.resv[1] != 0 || reg.resv[2] != 0 ||
	    (reg.flags & ~(IOU_PBUF_RING_MMAP | IOU_PBUF_RING_INC)) != 0)
		return (EINVAL);
	if (reg.ring_entries == 0 ||
	    reg.ring_entries > KQRING_MAX_PBUF_ENTRIES ||
	    !powerof2(reg.ring_entries))
		return (EINVAL);
	bytes = (size_t)reg.ring_entries * sizeof(struct io_uring_buf);
	if ((reg.flags & IOU_PBUF_RING_MMAP) != 0) {
		if (reg.ring_addr != 0)
			return (EINVAL);
		if (round_page(bytes) > KQRING_MAX_RING_MEM - ctx->ring_mem)
			return (ENOMEM);
	} else {
		if (reg.ring_addr == 0)
			return (EFAULT);
		if ((reg.ring_addr & PAGE_MASK) != 0)
			return (EINVAL);
		if (reg.ring_addr + bytes > VM_MAXUSER_ADDRESS)
			return (EFAULT);
		/*
		 * io_uring pins the ring's pages here, so an unmapped ring
		 * fails now rather than at the first buffer selection.
		 */
		for (uint64_t a = reg.ring_addr; a < reg.ring_addr + bytes;
		    a += PAGE_SIZE) {
			char b;

			if (copyin((const void *)(uintptr_t)a, &b, 1) != 0)
				return (EFAULT);
		}
	}
	/* An empty buffer list may become a ring, as in io_uring. */
	sx_xlock(&ctx->bg_sx);
	old = kqring_bgroup_find(ctx, reg.bgid);
	if (old != NULL && (old->is_ring || old->nbufs != 0 || old->busy)) {
		sx_xunlock(&ctx->bg_sx);
		return (EEXIST);
	}

	bg = kqring_bgroup_new(reg.bgid);
	bg->is_ring = true;
	bg->ring_flags = reg.flags;
	bg->ring_entries = reg.ring_entries;
	bg->ring_mask = reg.ring_entries - 1;
	if ((reg.flags & IOU_PBUF_RING_MMAP) != 0) {
		error = kqring_alloc_shm(bytes, &bg->ring_obj,
		    &bg->ring_kva_base, &bg->ring_alloc_len);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			free(bg, M_KQRING);
			return (error);
		}
		bg->ring_kva = (struct io_uring_buf_ring *)bg->ring_kva_base;
		ctx->ring_mem += bg->ring_alloc_len;
	} else {
		bg->ring_uaddr = reg.ring_addr;
	}
	mtx_lock(&ctx->mtx);
	if (old != NULL)
		LIST_REMOVE(old, link);
	LIST_INSERT_HEAD(kqring_bgroup_bucket(ctx, bg->bgid), bg, link);
	mtx_unlock(&ctx->mtx);
	if (old != NULL)
		kqring_bgroup_free(ctx, old);
	sx_xunlock(&ctx->bg_sx);
	return (0);
}

int
kqring_pbuf_unregister(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct io_uring_buf_reg reg;
	struct kqring_bgroup *bg;
	int error;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	error = copyin(ra->arg, &reg, sizeof(reg));
	if (error != 0)
		return (error);
	if (reg.resv[0] != 0 || reg.resv[1] != 0 || reg.resv[2] != 0 ||
	    reg.flags != 0)
		return (EINVAL);
	sx_xlock(&ctx->bg_sx);
	bg = kqring_bgroup_find(ctx, reg.bgid);
	if (bg == NULL) {
		sx_xunlock(&ctx->bg_sx);
		return (ENOENT);
	}
	if (!bg->is_ring) {
		sx_xunlock(&ctx->bg_sx);
		return (EINVAL);
	}
	if (bg->busy) {
		sx_xunlock(&ctx->bg_sx);
		return (EBUSY);
	}
	mtx_lock(&ctx->mtx);
	LIST_REMOVE(bg, link);
	mtx_unlock(&ctx->mtx);
	kqring_bgroup_free(ctx, bg);
	sx_xunlock(&ctx->bg_sx);
	return (0);
}

/* IORING_REGISTER_PBUF_STATUS: a ring's kernel head. */
int
kqring_pbuf_status(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct io_uring_buf_status st;
	struct kqring_bgroup *bg;
	int error;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	error = copyin(ra->arg, &st, sizeof(st));
	if (error != 0)
		return (error);
	for (size_t i = 0; i < nitems(st.resv); i++)
		if (st.resv[i] != 0)
			return (EINVAL);
	if (st.buf_group > UINT16_MAX)
		return (ENOENT);
	sx_xlock(&ctx->bg_sx);
	bg = kqring_bgroup_find(ctx, (uint16_t)st.buf_group);
	if (bg == NULL) {
		sx_xunlock(&ctx->bg_sx);
		return (ENOENT);
	}
	if (!bg->is_ring) {
		sx_xunlock(&ctx->bg_sx);
		return (EINVAL);
	}
	st.head = bg->head;
	sx_xunlock(&ctx->bg_sx);
	return (copyout(&st, ra->arg, sizeof(st)));
}


/*
 * IORING_REGISTER_PERSONALITY: register this thread's credentials, for
 * requests to name with sqe->personality.  Ids are 1..65535, cyclic.
 */
int
kqring_personality_register(struct kqring_ctx *ctx, int *idp)
{
	struct kqring_cred *kc, *k;
	uint16_t id;

	if (ctx->ncreds >= KQRING_MAX_PERSONALITIES)
		return (EBUSY);
	for (;;) {
		id = ++ctx->cred_next;
		if (id == 0)
			continue;
		LIST_FOREACH(k, &ctx->creds, link) {
			if (k->id == id)
				break;
		}
		if (k == NULL)
			break;
	}
	kc = malloc(sizeof(*kc), M_KQRING, M_WAITOK);
	kc->id = id;
	kc->cred = crhold(curthread->td_ucred);
	LIST_INSERT_HEAD(&ctx->creds, kc, link);
	ctx->ncreds++;
	*idp = id;
	return (0);
}

/* Requests already using the credentials hold their own reference. */
int
kqring_personality_unregister(struct kqring_ctx *ctx, uint32_t id)
{
	struct kqring_cred *kc;

	LIST_FOREACH(kc, &ctx->creds, link) {
		if (kc->id == id)
			break;
	}
	if (kc == NULL)
		return (EINVAL);
	LIST_REMOVE(kc, link);
	ctx->ncreds--;
	crfree(kc->cred);
	free(kc, M_KQRING);
	return (0);
}
