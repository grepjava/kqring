/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

/* ============================================================================
 * Shared Memory Allocation (Wired VM Objects + Kernel Virtual Address Mapping)
 * ============================================================================
 */

int
kqring_alloc_shm(size_t bytes, vm_object_t *obj_out, vm_offset_t *kva_out,
    size_t *alloc_len_out)
{
	vm_object_t obj;
	vm_offset_t kva;
	vm_page_t m;
	size_t size, npages, i;

	size = round_page(bytes);
	npages = atop(size);

	obj = vm_pager_allocate(OBJT_DEFAULT, NULL, size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, curthread->td_ucred);
	if (obj == NULL)
		return (ENOMEM);

	kva = kva_alloc(size);
	if (kva == 0) {
		vm_object_deallocate(obj);
		return (ENOMEM);
	}

	VM_OBJECT_WLOCK(obj);
	for (i = 0; i < npages; i++) {
		m = vm_page_grab(obj, i, VM_ALLOC_NOBUSY | VM_ALLOC_ZERO |
		    VM_ALLOC_WIRED | VM_ALLOC_NORMAL);
		if (m == NULL) {
			/* Unwire and release pages grabbed so far */
			for (size_t j = 0; j < i; j++) {
				vm_page_t prev = vm_page_lookup(obj, j);
				if (prev != NULL)
					vm_page_unwire_noq(prev);
			}
			VM_OBJECT_WUNLOCK(obj);
			pmap_qremove(kva, i);
			kva_free(kva, size);
			vm_object_deallocate(obj);
			return (ENOMEM);
		}
		vm_page_valid(m);
		pmap_qenter(kva + ptoa(i), &m, 1);
	}
	VM_OBJECT_WUNLOCK(obj);

	memset((void *)kva, 0, size);
	*obj_out = obj;
	*kva_out = kva;
	*alloc_len_out = size;
	return (0);
}

void
kqring_free_shm(vm_object_t obj, vm_offset_t kva, size_t alloc_len)
{
	size_t npages, i;
	vm_page_t m;

	if (obj == NULL)
		return;

	npages = atop(alloc_len);
	pmap_qremove(kva, npages);
	kva_free(kva, alloc_len);

	VM_OBJECT_WLOCK(obj);
	for (i = 0; i < npages; i++) {
		m = vm_page_lookup(obj, i);
		if (m != NULL)
			vm_page_unwire(m, PQ_INACTIVE);
	}
	VM_OBJECT_WUNLOCK(obj);
	vm_object_deallocate(obj);
}

/* ============================================================================
 * CQE Posting & Provided Buffer Ring (PBUF_RING) Selection
 * ============================================================================
 */

/* Copy one CQE into the CQ ring; false if the ring is full. */
static bool
kqring_cq_push(struct kqring_ctx *ctx, const struct io_uring_cqe *src,
    const uint64_t *extra)
{
	struct kqring_shm_header *shm = ctx->shm;
	struct io_uring_cqe *dst;
	uint32_t tail, head;
	size_t shift = (ctx->flags & IORING_SETUP_CQE32) != 0 ? 1 : 0;

	head = atomic_load_acq_32(&shm->cq_head);
	tail = shm->cq_tail;
	if (tail - head >= ctx->cq_entries)
		return (false);

	dst = &ctx->cqes[(tail & ctx->cq_mask) << shift];
	*dst = *src;
	if (shift != 0) {
		if (extra != NULL) {
			dst->big_cqe[0] = extra[0];
			dst->big_cqe[1] = extra[1];
		} else {
			dst->big_cqe[0] = 0;
			dst->big_cqe[1] = 0;
		}
	}
	atomic_store_rel_32(&shm->cq_tail, tail + 1);
	return (true);
}

/* Move overflowed CQEs into the CQ ring as userspace frees up space. */
void
kqring_flush_overflow(struct kqring_ctx *ctx)
{
	struct kqring_ocqe *ocqe;

	if (STAILQ_EMPTY(&ctx->ocq))
		return;
	while ((ocqe = STAILQ_FIRST(&ctx->ocq)) != NULL) {
		if (!kqring_cq_push(ctx, &ocqe->cqe, ocqe->extra))
			return;
		STAILQ_REMOVE_HEAD(&ctx->ocq, link);
		ctx->nocq--;
		free(ocqe, M_KQRING);
	}
	atomic_clear_32(&ctx->shm->sq_flags, IORING_SQ_CQ_OVERFLOW);
}

/*
 * Post a CQE.  When the CQ ring is full it is queued in the kernel (in
 * order, behind earlier overflow) and IORING_SQ_CQ_OVERFLOW is set; only
 * beyond KQRING_MAX_OVERFLOW queued CQEs is one dropped and counted in
 * cq_overflow.  Never called with ctx->mtx held.  TIMEOUT completions use
 * this directly; everything else goes through kqring_post_cqe(), which also
 * advances the sequence that completion-count TIMEOUTs wait on.
 */
void
kqring_post_cqe_extra(struct kqring_ctx *ctx, uint64_t user_data,
    int32_t res, uint32_t flags, const uint64_t *extra)
{
	struct io_uring_cqe cqe = {
		.user_data = user_data,
		.res = res,
		.flags = flags,
	};
	struct kqring_ocqe *ocqe;

	/* The eventfd is signalled once per enter; see kqring_evfd_flush(). */
	if (!ctx->evfd_async || !ctx->submitting)
		ctx->evfd_cqes++;
	if (STAILQ_EMPTY(&ctx->ocq) && kqring_cq_push(ctx, &cqe, extra))
		return;

	if (ctx->nocq >= KQRING_MAX_OVERFLOW) {
		atomic_add_32(&ctx->shm->cq_overflow, 1);
		return;
	}
	ocqe = malloc(sizeof(*ocqe), M_KQRING, M_WAITOK | M_ZERO);
	ocqe->cqe = cqe;
	if (extra != NULL) {
		ocqe->extra[0] = extra[0];
		ocqe->extra[1] = extra[1];
	}
	STAILQ_INSERT_TAIL(&ctx->ocq, ocqe, link);
	ctx->nocq++;
	atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_CQ_OVERFLOW);
}

void
kqring_op_disarm_locked(struct kqring_op *op)
{
	struct kqring_ctx *ctx = op->ctx;
	struct kqring_wq *wq = ctx->wq;
	struct socket *so = op->so;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->polled) {
		op->polled = false;
		ctx->npolled--;
	}
	if (op->kp_on)
		kqring_kpoll_disarm_locked(ctx, op);
	if (wq != NULL) {
		mtx_lock(&wq->mtx);
		op->wq_canceled = true;
		if (op->wq_queued) {
			TAILQ_REMOVE(&wq->ops[op->wq_type], op, wq_link);
			op->wq_queued = false;
			wq->pending[op->wq_type]--;
		}
		if (op->wq_running) {
			if (op->in_taskwork) {
				TAILQ_REMOVE(&ctx->taskwork_ops, op,
				    taskwork_link);
				op->in_taskwork = false;
			}
			wakeup(&op->in_taskwork);
			kqring_wq_abort_td(op->wq_td);
			if (!op->wq_retired) {
				op->wq_retired = true;
				TAILQ_INSERT_TAIL(&ctx->retired_ops, op,
				    active_link);
			}
			if (!ctx->wq_abort_pending) {
				ctx->wq_abort_pending = true;
				callout_reset_sbt(&ctx->wq_abort_callout,
				    50 * SBT_1US, 0, kqring_wq_abort_cb,
				    ctx, 0);
			}
			mtx_unlock(&wq->mtx);
			/* The worker still owns op and may be using its socket. */
			return;
		}
		mtx_unlock(&wq->mtx);
	}
	op->wq_wait_ready = false;
	kqring_timer_stop_locked(op);
	kqring_futex_unhash_locked(op);
	if (so != NULL) {
		op->so = NULL;
		/*
		 * Drop ctx->mtx before acquiring the socket lock to respect
		 * lock ordering (socket lock -> ctx->mtx in kqring_soupcall).
		 */
		mtx_unlock(&ctx->mtx);
		if (op->listening) {
			kqring_listen_disarm(op, so);
		} else {
			if ((op->dirs & KQ_DIR_RCV) != 0)
				kqring_sb_disarm(op, so, SO_RCV);
			if ((op->dirs & KQ_DIR_SND) != 0)
				kqring_sb_disarm(op, so, SO_SND);
		}
		op->dirs = 0;
		kqring_sorele(so);
		mtx_lock(&ctx->mtx);
	}
}

/*
 * Take an armed request off active_ops and release everything it holds
 * (upcalls, socket reference, timer, queued taskwork).  May drop and
 * retake ctx->mtx.
 */
void
kqring_op_unarm_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	KASSERT(op->armed, ("kqring: unarming an op that is not armed"));
	op->cancelled = true;
	op->armed = false;
	TAILQ_REMOVE(&ctx->active_ops, op, active_link);
	if (op->sqe.opcode == IORING_OP_POLL_ADD)
		LIST_REMOVE(op, poll_hlink);
	if (op->nfwait != 0)
		ctx->nfutex--;
	if (op->sqe.opcode == IORING_OP_WAITID && ctx->nwaitid > 0)
		ctx->nwaitid--;
	if (op->seq_timeout) {
		op->seq_timeout = false;
		ctx->nseqto--;
	}
	kqring_op_disarm_locked(op);
}


/*
 * Take the fixed file and registered buffer a request names.  As in
 * io_uring this happens at issue, so that a linked request sees slots its
 * predecessors filled, and READ_FIXED and friends must stay inside their
 * registered buffer.
 */
static int
kqring_req_resources(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct iovec *iov = op->args;
	struct kqring_rsrc *r;
	uint32_t idx = sqe->buf_index;
	int error;

	if ((sqe->flags & IOSQE_FIXED_FILE) != 0 && op->ffile == NULL) {
		error = kqring_fixed_get(ctx, (uint32_t)sqe->fd, &op->ffile);
		if (error != 0)
			return (error);
	}
	if ((sqe->opcode == IORING_OP_SPLICE || sqe->opcode == IORING_OP_TEE) &&
	    (sqe->splice_flags & SPLICE_F_FD_IN_FIXED) != 0 &&
	    op->ffile_in == NULL) {
		error = kqring_fixed_get(ctx, (uint32_t)sqe->splice_fd_in,
		    &op->ffile_in);
		if (error != 0)
			return (error);
	}
	switch (sqe->opcode) {
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		break;
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG_ZC:
		if ((sqe->ioprio & IORING_RECVSEND_FIXED_BUF) != 0)
			break;
		return (0);
	default:
		return (0);
	}
	if (op->fbuf == NULL) {
		if (ctx->bufs == NULL || idx >= ctx->nbufs ||
		    (r = ctx->bufs[idx]) == NULL)
			return (EFAULT);
		r->refs++;
		op->fbuf = r;
	}
	if (sqe->opcode == IORING_OP_SENDMSG_ZC) {
		const struct kqring_msg_args *ma = op->args;

		for (int i = 0; i < ma->msg.msg_iovlen; i++) {
			if (!kqring_buf_within(op->fbuf,
			    (uintptr_t)ma->msg.msg_iov[i].iov_base,
			    ma->msg.msg_iov[i].iov_len))
				return (EFAULT);
		}
		return (0);
	}
	if (kqring_op_readv(sqe->opcode) || kqring_op_writev(sqe->opcode)) {
		for (uint32_t i = 0; i < sqe->len; i++) {
			if (!kqring_buf_within(op->fbuf,
			    (uintptr_t)iov[i].iov_base, iov[i].iov_len))
				return (EFAULT);
		}
		return (0);
	}
	return (kqring_buf_within(op->fbuf, sqe->addr, sqe->len) ? 0 :
	    EFAULT);
}


static struct kqring_op *
kqring_op_alloc(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	op = TAILQ_FIRST(&ctx->free_ops);
	if (op != NULL) {
		TAILQ_REMOVE(&ctx->free_ops, op, active_link);
		ctx->nfree--;
		bzero(op, offsetof(struct kqring_op, callout));
	} else {
		op = malloc(sizeof(*op), M_KQRING, M_WAITOK | M_ZERO);
	}
	op->ctx = ctx;
	ctx->nreqs++;
	return (op);
}

/* Never called with ctx->mtx held (callout_drain() may sleep). */
static void
kqring_op_free(struct kqring_op *op)
{
	struct kqring_ctx *ctx = op->ctx;

	KASSERT(!op->armed && !op->in_taskwork && !op->timer_pending &&
	    !op->wq_queued && !op->wq_running && !op->wq_retired,
	    ("kqring: freeing a live op"));
	ctx->nreqs--;
	/*
	 * The callout was stopped under ctx->mtx, so its function will not
	 * run; drain anyway so that softclock is done with the structure
	 * before it is reused or freed.
	 */
	if (op->timer_init)
		callout_drain(&op->callout);
	if (op->wq_fp != NULL) {
		filecaps_free(&op->wq_caps);
		fdrop(op->wq_fp, curthread);
		op->wq_fp = NULL;
	}
	if (op->sock_fp != NULL) {
		fdrop(op->sock_fp, curthread);
		op->sock_fp = NULL;
	}
	if (op->bio_fp != NULL) {
		fdrop(op->bio_fp, curthread);
		op->bio_fp = NULL;
	}
	if (op->args != NULL) {
		struct kqring_fwait *fw = op->args;

		for (uint32_t i = 0; i < op->nfwait; i++) {
			KASSERT(!fw[i].hashed, ("kqring: freeing hashed futex"));
			if (fw[i].keyed)
				umtx_key_release(&fw[i].key);
		}
		ctx->args_bytes -= op->args_len;
		free(op->args, M_KQRING);
		op->args = NULL;
	}
	if (op->ffile != NULL) {
		kqring_rsrc_put(ctx, op->ffile);
		op->ffile = NULL;
	}
	if (op->ffile_in != NULL) {
		kqring_rsrc_put(ctx, op->ffile_in);
		op->ffile_in = NULL;
	}
	if (op->fbuf != NULL) {
		kqring_rsrc_put(ctx, op->fbuf);
		op->fbuf = NULL;
	}
	if (op->zc_notif != NULL) {
		kqring_zc_abort(op->zc_notif);
		op->zc_notif = NULL;
	}
	if (op->cred != NULL) {
		crfree(op->cred);
		op->cred = NULL;
	}
	if (!ctx->closing && ctx->nfree < KQRING_OP_CACHE_MAX) {
		TAILQ_INSERT_HEAD(&ctx->free_ops, op, active_link);
		ctx->nfree++;
		return;
	}
	free(op, M_KQRING);
}

/*
 * Ops that complete in one synchronous call; the result goes to op->res
 * and op->cflags.
 */
void
kqring_exec_sync(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa = op->args;
	struct thread *td = curthread;
	int32_t res;

	switch (sqe->opcode) {
	case IORING_OP_NOP:
		res = (sqe->nop_flags & IORING_NOP_INJECT_RESULT) != 0 ?
		    (int32_t)sqe->len : 0;
		break;
	case IORING_OP_READ:
	case IORING_OP_READV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_READV_FIXED:
		kqring_exec_file_rw(ctx, op, false);
		return;
	case IORING_OP_WRITE:
	case IORING_OP_WRITEV:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITEV_FIXED:
		kqring_exec_file_rw(ctx, op, true);
		return;
	case IORING_OP_FSYNC:
		res = kqring_exec_fsync(op,
		    (sqe->fsync_flags & IORING_FSYNC_DATASYNC) == 0);
		break;
	case IORING_OP_SYNC_FILE_RANGE:
		/* No range sync on FreeBSD: fdatasync(2) the whole file. */
		res = sqe->sync_range_flags == 0 ? 0 :
		    kqring_exec_fsync(op, false);
		break;
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
		res = kqring_exec_openat(ctx, op);
		break;
	case IORING_OP_CLOSE:
		res = kqring_exec_close(ctx, sqe);
		break;
	case IORING_OP_FILES_UPDATE:
		/* The offset is 32 bits (liburing sign-extends it into off). */
		res = (uint32_t)sqe->off == IORING_FILE_INDEX_ALLOC ?
		    kqring_files_update_alloc(ctx, sqe->addr, sqe->len) :
		    kqring_files_update(ctx, (uint32_t)sqe->off, sqe->addr, 0,
		    sqe->len);
		break;
	case IORING_OP_FIXED_FD_INSTALL:
		res = kqring_exec_fixed_fd_install(op);
		break;
	case IORING_OP_PROVIDE_BUFFERS:
		res = kqring_provide_buffers(ctx, sqe);
		break;
	case IORING_OP_REMOVE_BUFFERS:
		res = kqring_remove_buffers(ctx, sqe);
		break;
	case IORING_OP_PIPE:
		res = kqring_exec_pipe(ctx, sqe);
		break;
	case IORING_OP_STATX:
		res = kqring_exec_statx(op);
		break;
	case IORING_OP_FALLOCATE:
		res = kqring_exec_fallocate(op);
		break;
	case IORING_OP_FTRUNCATE:
		res = kqring_exec_ftruncate(op);
		break;
	case IORING_OP_FADVISE:
		res = kqring_exec_fadvise(op);
		break;
	case IORING_OP_MADVISE:
		res = -kern_madvise(td, (uintptr_t)sqe->addr,
		    sqe->off != 0 ? (size_t)sqe->off : (size_t)sqe->len,
		    (int)sqe->fadvise_advice);
		break;
	case IORING_OP_RENAMEAT:
		res = -kern_renameat(td, sqe->fd, pa->path, (int)sqe->len,
		    pa->path2, UIO_SYSSPACE, sqe->rename_flags);
		break;
	case IORING_OP_UNLINKAT:
		if ((sqe->unlink_flags & AT_REMOVEDIR) != 0)
			res = -kern_frmdirat(td, sqe->fd, pa->path, FD_NONE,
			    UIO_SYSSPACE, (int)sqe->unlink_flags & ~AT_REMOVEDIR);
		else
			res = -kern_funlinkat(td, sqe->fd, pa->path, FD_NONE,
			    UIO_SYSSPACE, (int)sqe->unlink_flags, 0);
		break;
	case IORING_OP_MKDIRAT:
		res = -kern_mkdirat(td, sqe->fd, pa->path, UIO_SYSSPACE,
		    (int)sqe->len);
		break;
	case IORING_OP_SYMLINKAT:
		res = -kern_symlinkat(td, pa->path, sqe->fd, pa->path2,
		    UIO_SYSSPACE);
		break;
	case IORING_OP_LINKAT:
		res = -kern_linkat(td, sqe->fd, (int)sqe->len, pa->path,
		    pa->path2, UIO_SYSSPACE, (int)sqe->hardlink_flags);
		break;
	case IORING_OP_FSETXATTR:
	case IORING_OP_SETXATTR:
		res = kqring_exec_setxattr(op);
		break;
	case IORING_OP_FGETXATTR:
	case IORING_OP_GETXATTR:
		res = kqring_exec_getxattr(op);
		break;
	case IORING_OP_SOCKET:
		res = kqring_exec_socket(ctx, op);
		break;
	case IORING_OP_BIND:
		res = kqring_exec_bind(op);
		break;
	case IORING_OP_LISTEN:
		res = kqring_exec_listen(op);
		break;
	case IORING_OP_SHUTDOWN:
		res = kqring_exec_shutdown(op);
		break;
	case IORING_OP_URING_CMD:
		res = kqring_exec_uring_cmd(op);
		break;
	case IORING_OP_SPLICE:
		res = kqring_exec_splice(ctx, op);
		break;
	case IORING_OP_TEE:
		res = kqring_exec_tee(ctx, op);
		break;
	default:
		res = -EOPNOTSUPP;
		break;
	}
	kqring_done(op, res, 0);
}

/*
 * One attempt at a socket op.  Intermediate multishot results are posted
 * here (receives keep going while more data is buffered).  Returns EAGAIN
 * if the op should be (or stay) armed, or 0 once it is done, with its final
 * result in op->res / op->cflags.
 */
static int
kqring_op_attempt(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect)
{
	bool more;
	int rc;

	for (;;) {
		more = false;
		switch (op->sqe.opcode) {
		case IORING_OP_RECV:
		case IORING_OP_READ:
		case IORING_OP_READV:
		case IORING_OP_READ_FIXED:
		case IORING_OP_READV_FIXED:
			rc = kqring_exec_recv(ctx, op, expect, &more);
			break;
		case IORING_OP_READ_MULTISHOT:
			rc = kqring_exec_read_multishot(ctx, op, expect, &more);
			break;
		case IORING_OP_RECVMSG:
			rc = kqring_exec_recvmsg(ctx, op, expect, &more);
			break;
		case IORING_OP_SEND:
		case IORING_OP_SEND_ZC:
		case IORING_OP_WRITE:
		case IORING_OP_WRITEV:
		case IORING_OP_WRITE_FIXED:
		case IORING_OP_WRITEV_FIXED:
			rc = kqring_exec_send(ctx, op, expect);
			/* A bundle with buffers left sends again at once. */
			more = true;
			break;
		case IORING_OP_SENDMSG:
		case IORING_OP_SENDMSG_ZC:
			rc = kqring_exec_sendmsg(ctx, op, expect);
			break;
		case IORING_OP_CONNECT:
			rc = kqring_exec_connect(op, expect);
			break;
		case IORING_OP_ACCEPT:
			rc = kqring_exec_accept(ctx, op, expect, &more);
			break;
		case IORING_OP_POLL_ADD:
			rc = kqring_exec_poll(op, expect);
			break;
		default:
			return (kqring_done(op, -EOPNOTSUPP, 0));
		}
		if (rc != 0 || (op->cflags & IORING_CQE_F_MORE) == 0)
			return (rc);
		kqring_post_cqe(ctx, op->sqe.user_data, op->res, op->cflags);
		if (!more)
			return (EAGAIN);
	}
}


/* ============================================================================
 * Submission: SQE Validation and Argument Copy-in
 * ============================================================================
 */

/* Allocate op->args (zeroed), charged to the ring's argument budget. */
static void *
kqring_args_alloc(struct kqring_op *op, size_t len, int *errorp)
{
	struct kqring_ctx *ctx = op->ctx;

	KASSERT(op->args == NULL, ("kqring: op args allocated twice"));
	if (len > KQRING_MAX_ARG_BYTES - ctx->args_bytes) {
		*errorp = ENOMEM;
		return (NULL);
	}
	op->args = malloc(len, M_KQRING, M_WAITOK | M_ZERO);
	op->args_len = len;
	ctx->args_bytes += len;
	*errorp = 0;
	return (op->args);
}

/* Copy in an iovec array, checking that its total length fits ssize_t. */
static int
kqring_copyin_iov(uint64_t uaddr, u_int iovcnt, struct iovec *iov)
{
	size_t total = 0;
	int error;

	error = copyin((const void *)(uintptr_t)uaddr, iov,
	    iovcnt * sizeof(*iov));
	if (error != 0)
		return (error);
	for (u_int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len > IOSIZE_MAX - total)
			return (EINVAL);
		total += iov[i].iov_len;
	}
	return (0);
}

/* READV / WRITEV: sqe->addr is an iovec array of sqe->len entries. */
static int
kqring_prep_iov(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct iovec *iov;
	int error;

	if (sqe->len > UIO_MAXIOV)
		return (EINVAL);
	if (sqe->len == 0)
		return (0);
	iov = kqring_args_alloc(op, sqe->len * sizeof(*iov), &error);
	if (iov == NULL)
		return (error);
	return (kqring_copyin_iov(sqe->addr, sqe->len, iov));
}

/* A socket address, as getsockaddr() copies one in. */
static int
kqring_prep_sockaddr(struct kqring_op *op, uint64_t uaddr, uint64_t len)
{
	struct sockaddr *sa;
	int error;

	if (len > SOCK_MAXADDRLEN)
		return (ENAMETOOLONG);
	if (len < offsetof(struct sockaddr, sa_data[0]))
		return (EINVAL);
	sa = kqring_args_alloc(op, len, &error);
	if (sa == NULL)
		return (error);
	error = copyin((const void *)(uintptr_t)uaddr, sa, len);
	if (error != 0)
		return (error);
	sa->sa_len = (uint8_t)len;
	return (0);
}

/*
 * SENDMSG(_ZC) / RECVMSG: copy the msghdr and its iovec, and for a send
 * the name and control data too.  A receive's name and control buffers
 * are outputs and stay user pointers.
 */
static int
kqring_prep_msg(struct kqring_op *op, bool is_send)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma;
	struct msghdr msg;
	size_t ctllen = 0, iovsz;
	int error;

	error = copyin((const void *)(uintptr_t)sqe->addr, &msg, sizeof(msg));
	if (error != 0)
		return (error);
	if ((u_int)msg.msg_iovlen > UIO_MAXIOV)
		return (EMSGSIZE);
	if (is_send && msg.msg_name != NULL && msg.msg_namelen != 0 &&
	    (msg.msg_namelen > SOCK_MAXADDRLEN ||
	    msg.msg_namelen < offsetof(struct sockaddr, sa_data[0])))
		return (msg.msg_namelen > SOCK_MAXADDRLEN ? ENAMETOOLONG :
		    EINVAL);
	if (is_send && msg.msg_control != NULL && msg.msg_controllen != 0) {
		/* As sendit() / sockargs(). */
		if (msg.msg_controllen < sizeof(struct cmsghdr) ||
		    msg.msg_controllen > MCLBYTES)
			return (EINVAL);
		ctllen = msg.msg_controllen;
	}
	iovsz = (size_t)msg.msg_iovlen * sizeof(struct iovec);
	ma = kqring_args_alloc(op, sizeof(*ma) + iovsz + ctllen, &error);
	if (ma == NULL)
		return (error);
	if (msg.msg_iovlen != 0) {
		error = kqring_copyin_iov((uint64_t)(uintptr_t)msg.msg_iov,
		    msg.msg_iovlen, ma->iov);
		if (error != 0)
			return (error);
	}
	ma->msg = msg;
	ma->msg.msg_iov = ma->iov;
	if (!is_send)
		return (0);

	if (msg.msg_name != NULL && msg.msg_namelen != 0) {
		error = copyin(msg.msg_name, ma->name, msg.msg_namelen);
		if (error != 0)
			return (error);
		((struct sockaddr *)ma->name)->sa_len =
		    (uint8_t)msg.msg_namelen;
		ma->msg.msg_name = ma->name;
	} else {
		ma->msg.msg_name = NULL;
		ma->msg.msg_namelen = 0;
	}
	if (ctllen != 0) {
		ma->msg.msg_control = (char *)ma->iov + iovsz;
		error = copyin(msg.msg_control, ma->msg.msg_control, ctllen);
		if (error != 0)
			return (error);
	} else {
		ma->msg.msg_control = NULL;
		ma->msg.msg_controllen = 0;
	}
	return (0);
}

static int
kqring_copyin_path(uint64_t uaddr, char *buf)
{
	return (copyinstr((const void *)(uintptr_t)uaddr, buf, MAXPATHLEN,
	    NULL));
}

/* One path (sqe->addr), or two (sqe->addr2 too). */
static int
kqring_prep_paths(struct kqring_op *op, bool two)
{
	struct kqring_path_args *pa;
	int error;

	pa = kqring_args_alloc(op, two ? sizeof(*pa) :
	    offsetof(struct kqring_path_args, path2), &error);
	if (pa == NULL)
		return (error);
	error = kqring_copyin_path(op->sqe.addr, pa->path);
	if (error == 0 && two)
		error = kqring_copyin_path(op->sqe.addr2, pa->path2);
	return (error);
}

/* OPENAT2: struct open_how at sqe->addr2, sqe->len bytes. */
static int
kqring_prep_openat2(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_path_args *pa;
	struct open_how how;
	uint8_t extra[64];
	int error;

	if (sqe->len < sizeof(how))
		return (EINVAL);
	if (sqe->len > PAGE_SIZE)
		return (E2BIG);
	error = copyin((const void *)(uintptr_t)sqe->addr2, &how, sizeof(how));
	if (error != 0)
		return (error);
	/* A larger (newer) struct is fine if the fields we lack are zero. */
	for (size_t off = sizeof(how); off < sqe->len; off += sizeof(extra)) {
		size_t n = MIN(sizeof(extra), sqe->len - off);

		error = copyin((const char *)(uintptr_t)sqe->addr2 + off,
		    extra, n);
		if (error != 0)
			return (error);
		for (size_t i = 0; i < n; i++)
			if (extra[i] != 0)
				return (E2BIG);
	}
	if ((how.flags & ~(uint64_t)FUSERALLOWED) != 0 ||
	    (how.mode & ~(uint64_t)07777) != 0 ||
	    (how.mode != 0 && (how.flags & O_CREAT) == 0))
		return (EINVAL);
	/*
	 * FreeBSD has no magic links, and RESOLVE_CACHED only asks to fail
	 * rather than block, so both are honored by a normal lookup.
	 */
	if ((how.resolve & ~(uint64_t)(RESOLVE_BENEATH |
	    RESOLVE_NO_MAGICLINKS | RESOLVE_CACHED)) != 0)
		return (EINVAL);
	if ((how.resolve & RESOLVE_BENEATH) != 0)
		how.flags |= O_RESOLVE_BENEATH;

	error = kqring_prep_paths(op, false);
	if (error != 0)
		return (error);
	pa = op->args;
	pa->how = how;
	return (0);
}

/*
 * xattr ops: Linux "namespace.name" names.  "user." maps to the user
 * extattr namespace, "system." and "trusted." (both privileged) to the
 * system one; others (such as "security.") have no FreeBSD counterpart.
 */
static int
kqring_prep_xattr(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	static const struct {
		const char	*prefix;
		int		ns;
	} nsmap[] = {
		{ "user.", EXTATTR_NAMESPACE_USER },
		{ "system.", EXTATTR_NAMESPACE_SYSTEM },
		{ "trusted.", EXTATTR_NAMESPACE_SYSTEM },
	};
	bool set = sqe->opcode == IORING_OP_SETXATTR ||
	    sqe->opcode == IORING_OP_FSETXATTR;
	bool path = sqe->opcode == IORING_OP_SETXATTR ||
	    sqe->opcode == IORING_OP_GETXATTR;
	char name[KQRING_XATTR_NAME_MAX + 1];
	struct kqring_xattr_args *xa;
	size_t i, plen;
	int error;

	if ((sqe->xattr_flags & ~(uint32_t)(set ? XATTR_CREATE |
	    XATTR_REPLACE : 0)) != 0 ||
	    sqe->xattr_flags == (XATTR_CREATE | XATTR_REPLACE))
		return (EINVAL);
	if (set && sqe->len > KQRING_XATTR_SIZE_MAX)
		return (E2BIG);
	error = copyinstr((const void *)(uintptr_t)sqe->addr, name,
	    sizeof(name), NULL);
	if (error == ENAMETOOLONG)
		return (ERANGE);
	if (error != 0)
		return (error);
	for (i = 0; i < nitems(nsmap); i++) {
		plen = strlen(nsmap[i].prefix);
		if (strncmp(name, nsmap[i].prefix, plen) == 0)
			break;
	}
	if (i == nitems(nsmap))
		return (EOPNOTSUPP);
	if (name[plen] == '\0')
		return (EINVAL);

	xa = kqring_args_alloc(op, sizeof(*xa) + (set ? sqe->len : 0),
	    &error);
	if (xa == NULL)
		return (error);
	xa->ns = nsmap[i].ns;
	strlcpy(xa->name, name + plen, sizeof(xa->name));
	if (path) {
		error = kqring_copyin_path(sqe->addr3, xa->path);
		if (error != 0)
			return (error);
	}
	if (set && sqe->len != 0)
		error = copyin((const void *)(uintptr_t)sqe->addr2, xa->value,
		    sqe->len);
	return (error);
}

/* futex2 flags: 32-bit words only, as in io_uring. */
static bool
kqring_futex_flags_ok(uint32_t flags)
{
	return ((flags & ~(uint32_t)(FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)) ==
	    0 && (flags & FUTEX2_SIZE_MASK) == FUTEX2_SIZE_U32);
}

static int
kqring_futex_word_ok(uint64_t uaddr, uint64_t val, uint32_t flags)
{
	if (!kqring_futex_flags_ok(flags) || (uaddr & 3) != 0 ||
	    val > UINT32_MAX)
		return (EINVAL);
	return (0);
}

/*
 * FUTEX_WAIT: fd = futex2 flags, addr = word, addr2 = expected value,
 * addr3 = wake mask (0 is taken as FUTEX_BITSET_MATCH_ANY).
 * FUTEX_WAITV: addr = struct futex_waitv[len].
 */
static int
kqring_prep_futex_wait(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct futex_waitv *wv = NULL;
	struct kqring_fwait *fw;
	uint32_t n;
	int error;

	if (sqe->futex_flags != 0 || sqe->buf_index != 0 ||
	    sqe->file_index != 0)
		return (EINVAL);
	if (sqe->opcode == IORING_OP_FUTEX_WAIT) {
		if (sqe->len != 0 || sqe->addr3 > UINT32_MAX)
			return (EINVAL);
		error = kqring_futex_word_ok(sqe->addr, sqe->addr2,
		    (uint32_t)sqe->fd);
		if (error != 0)
			return (error);
		n = 1;
	} else {
		if (sqe->fd != 0 || sqe->addr2 != 0 || sqe->addr3 != 0 ||
		    sqe->len == 0 || sqe->len > FUTEX_WAITV_MAX)
			return (EINVAL);
		n = sqe->len;
		wv = malloc(n * sizeof(*wv), M_KQRING, M_WAITOK);
		error = copyin((const void *)(uintptr_t)sqe->addr, wv,
		    n * sizeof(*wv));
		for (uint32_t i = 0; error == 0 && i < n; i++) {
			if (wv[i].__reserved != 0)
				error = EINVAL;
			else
				error = kqring_futex_word_ok(wv[i].uaddr,
				    wv[i].val, wv[i].flags);
		}
		if (error != 0) {
			free(wv, M_KQRING);
			return (error);
		}
	}

	fw = kqring_args_alloc(op, n * sizeof(*fw), &error);
	if (fw == NULL) {
		free(wv, M_KQRING);
		return (error);
	}
	op->nfwait = n;
	for (uint32_t i = 0; i < n; i++) {
		fw[i].op = op;
		fw[i].idx = i;
		if (wv == NULL) {
			fw[i].uaddr = sqe->addr;
			fw[i].val = sqe->addr2;
			fw[i].flags = (uint32_t)sqe->fd;
			fw[i].mask = sqe->addr3 != 0 ? (uint32_t)sqe->addr3 :
			    FUTEX_BITSET_MATCH_ANY;
		} else {
			fw[i].uaddr = wv[i].uaddr;
			fw[i].val = wv[i].val;
			fw[i].flags = wv[i].flags;
			fw[i].mask = FUTEX_BITSET_MATCH_ANY;
		}
	}
	free(wv, M_KQRING);
	return (0);
}

#define	KQRING_SQE_FLAGS						\
	(IOSQE_FIXED_FILE | IOSQE_IO_DRAIN | IOSQE_IO_LINK |		\
	IOSQE_IO_HARDLINK | IOSQE_ASYNC | IOSQE_BUFFER_SELECT |		\
	IOSQE_CQE_SKIP_SUCCESS)

/* Opcodes working on sqe->fd, which IOSQE_FIXED_FILE makes a fixed slot. */
static bool
kqring_op_takes_file(uint8_t opcode)
{
	switch (opcode) {
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_FSYNC:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_POLL_ADD:
	case IORING_OP_SYNC_FILE_RANGE:
	case IORING_OP_SENDMSG:
	case IORING_OP_RECVMSG:
	case IORING_OP_ACCEPT:
	case IORING_OP_CONNECT:
	case IORING_OP_FALLOCATE:
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_FADVISE:
	case IORING_OP_SEND:
	case IORING_OP_RECV:
	case IORING_OP_SHUTDOWN:
	case IORING_OP_MSG_RING:
	case IORING_OP_FSETXATTR:
	case IORING_OP_FGETXATTR:
	case IORING_OP_URING_CMD:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG_ZC:
	case IORING_OP_READ_MULTISHOT:
	case IORING_OP_FIXED_FD_INSTALL:
	case IORING_OP_FTRUNCATE:
	case IORING_OP_BIND:
	case IORING_OP_LISTEN:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
	case IORING_OP_SPLICE:
	case IORING_OP_TEE:
		return (true);
	default:
		return (false);
	}
}


/* IORING_REGISTER_RESTRICTIONS: whether the SQE may be submitted. */
static bool
kqring_restriction_ok(const struct kqring_ctx *ctx,
    const struct io_uring_sqe *sqe)
{
	const struct kqring_restrictions *r = &ctx->rst;

	return (kqring_bit_isset(r->sqe_op, sqe->opcode) &&
	    (sqe->flags & r->sqe_flags_required) == r->sqe_flags_required &&
	    (sqe->flags & ~(r->sqe_flags_allowed |
	    r->sqe_flags_required)) == 0);
}

/* msg_flags a receive / send may pass (FreeBSD MSG_* values). */
#define	KQRING_RECV_MSG_FLAGS						\
	(MSG_OOB | MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_DONTWAIT |	\
	MSG_CMSG_CLOEXEC | MSG_CMSG_CLOFORK)
#define	KQRING_SEND_MSG_FLAGS						\
	(MSG_OOB | MSG_DONTROUTE | MSG_EOR | MSG_WAITALL | MSG_DONTWAIT | \
	MSG_EOF | MSG_NOSIGNAL | MSG_MORETOCOME)

/* Opcodes permitted on an IORING_SETUP_IOPOLL ring. */
static bool
kqring_iopoll_op_ok(uint8_t opcode)
{
	switch (opcode) {
	case IORING_OP_NOP:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_FSYNC:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_SYNC_FILE_RANGE:
	case IORING_OP_ASYNC_CANCEL:
	case IORING_OP_CLOSE:
	case IORING_OP_FILES_UPDATE:
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_PROVIDE_BUFFERS:
	case IORING_OP_REMOVE_BUFFERS:
	case IORING_OP_MSG_RING:
	case IORING_OP_URING_CMD:
	case IORING_OP_FIXED_FD_INSTALL:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		return (true);
	default:
		return (false);
	}
}

/*
 * Validate an SQE and copy in the arguments it points to (paths, iovecs,
 * msghdrs, socket addresses, timespecs, xattr values, futex vectors), as
 * io_uring's prep does, so a linked, drained or armed request does not read
 * user memory later.  Reject flags, ioprio bits and fields kqring does not
 * implement instead of silently ignoring them.  IOSQE_ASYNC is only a
 * hint.  `prev` is the request this one is linked behind, if any.
 */
static int
kqring_req_prep(struct kqring_op *op, const struct kqring_op *prev)
{
	struct kqring_ctx *ctx = op->ctx;
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_msg_args *ma;
	struct kqring_path_args *pa;
	struct kqring_cred *kc;
	uint32_t ioprio_ok;
	bool direct = sqe->file_index != 0;
	bool multishot = false, buf_select_ok = false, bundle_ok = false;
	int error;

	/* An opcode past the ABI fails here (and ends a submission). */
	if (sqe->opcode >= IORING_OP_LAST)
		return (EINVAL);
	if ((sqe->flags & ~KQRING_SQE_FLAGS) != 0)
		return (EINVAL);
	if (ctx->restricted && !kqring_restriction_ok(ctx, sqe))
		return (EACCES);
	if ((ctx->flags & IORING_SETUP_IOPOLL) != 0 &&
	    !kqring_iopoll_op_ok(sqe->opcode))
		return (EINVAL);
	if ((sqe->flags & IOSQE_FIXED_FILE) != 0 &&
	    !kqring_op_takes_file(sqe->opcode))
		return (EINVAL);
	if (sqe->personality != 0) {
		LIST_FOREACH(kc, &ctx->creds, link) {
			if (kc->id == sqe->personality)
				break;
		}
		if (kc == NULL)
			return (EINVAL);
		/*
		 * Credentials registered before cap_enter(2) must not take
		 * the process back out of capability mode.
		 */
		if (IN_CAPABILITY_MODE(curthread) &&
		    (kc->cred->cr_flags & CRED_FLAG_CAPMODE) == 0)
			return (ECAPMODE);
		op->cred = crhold(kc->cred);
	}

	/*
	 * A READ/WRITE's ioprio is an I/O priority, checked as Linux's
	 * ioprio_check_cap() does (class 13 bits up, level below), then
	 * ignored: the realtime class needs privilege, and class or level
	 * out of range is EINVAL.
	 */
	if (kqring_op_readv(sqe->opcode) || kqring_op_writev(sqe->opcode) ||
	    sqe->opcode == IORING_OP_READ || sqe->opcode == IORING_OP_WRITE ||
	    sqe->opcode == IORING_OP_READ_FIXED ||
	    sqe->opcode == IORING_OP_WRITE_FIXED) {
		u_int pclass = (uint16_t)sqe->ioprio >> 13;
		u_int plevel = (uint16_t)sqe->ioprio & 0x1fff;

		if (pclass > 3 || ((pclass == 1 || pclass == 2) && plevel >= 8))
			return (EINVAL);
		if (pclass == 1 && priv_check(curthread, PRIV_SCHED_RTPRIO) != 0)
			return (EPERM);
	}

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
		if ((sqe->rw_flags & ~KQRING_RWF_SUPPORTED) != 0)
			return (EOPNOTSUPP);
		buf_select_ok = sqe->opcode == IORING_OP_READ;
		break;
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		if ((sqe->rw_flags & ~KQRING_RWF_SUPPORTED) != 0)
			return (EOPNOTSUPP);
		/* A provided buffer replaces READV's (single) iovec. */
		if (sqe->opcode == IORING_OP_READV &&
		    (sqe->flags & IOSQE_BUFFER_SELECT) != 0) {
			if (sqe->len != 1)
				return (EINVAL);
			buf_select_ok = true;
		}
		error = kqring_prep_iov(op);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_READ_MULTISHOT:
		if (sqe->rw_flags != 0 ||
		    (sqe->flags & IOSQE_BUFFER_SELECT) == 0)
			return (EINVAL);
		buf_select_ok = multishot = true;
		break;
	case IORING_OP_RECV:
	case IORING_OP_RECVMSG:
		ioprio_ok = IORING_RECVSEND_POLL_FIRST | IORING_RECV_MULTISHOT;
		if (sqe->opcode == IORING_OP_RECV)
			ioprio_ok |= IORING_RECVSEND_BUNDLE;
		if ((sqe->ioprio & ~ioprio_ok) != 0 ||
		    (sqe->msg_flags & ~(uint32_t)KQRING_RECV_MSG_FLAGS) != 0)
			return (EINVAL);
		multishot = (sqe->ioprio & IORING_RECV_MULTISHOT) != 0;
		/* Each multishot completion needs its own buffer. */
		if (multishot && ((sqe->flags & IOSQE_BUFFER_SELECT) == 0 ||
		    (sqe->msg_flags & MSG_WAITALL) != 0))
			return (EINVAL);
		buf_select_ok = bundle_ok = true;
		if (sqe->opcode == IORING_OP_RECV)
			break;
		error = kqring_prep_msg(op, false);
		if (error != 0)
			return (error);
		ma = op->args;
		/* A provided buffer replaces the (single) iovec. */
		if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0 &&
		    ma->msg.msg_iovlen > 1)
			return (EINVAL);
		if (multishot && (ma->msg.msg_namelen > 65536 ||
		    ma->msg.msg_controllen > 65536))
			return (EINVAL);
		break;
	case IORING_OP_SEND:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG:
	case IORING_OP_SENDMSG_ZC:
		ioprio_ok = IORING_RECVSEND_POLL_FIRST;
		if (sqe->opcode == IORING_OP_SEND_ZC ||
		    sqe->opcode == IORING_OP_SENDMSG_ZC)
			ioprio_ok |= IORING_SEND_ZC_REPORT_USAGE;
		/* A registered buffer (buf_index) may hold SEND(_MSG)_ZC data. */
		if (sqe->opcode == IORING_OP_SEND_ZC ||
		    sqe->opcode == IORING_OP_SENDMSG_ZC)
			ioprio_ok |= IORING_RECVSEND_FIXED_BUF;
		/* SEND may take its data from provided buffers. */
		if (sqe->opcode == IORING_OP_SEND) {
			ioprio_ok |= IORING_RECVSEND_BUNDLE;
			buf_select_ok = bundle_ok = true;
		}
		if ((sqe->ioprio & ~ioprio_ok) != 0 ||
		    (sqe->msg_flags & ~(uint32_t)KQRING_SEND_MSG_FLAGS) != 0)
			return (EINVAL);
		if (sqe->opcode == IORING_OP_SENDMSG ||
		    sqe->opcode == IORING_OP_SENDMSG_ZC) {
			error = kqring_prep_msg(op, true);
			if (error != 0)
				return (error);
		} else if (sqe->addr2 != 0 || sqe->addr_len != 0) {
			/* sendto(2): destination at addr2, addr_len bytes. */
			error = kqring_prep_sockaddr(op, sqe->addr2,
			    sqe->addr_len);
			if (error != 0)
				return (error);
		}
		break;
	case IORING_OP_ACCEPT:
		if ((sqe->ioprio & ~(IORING_ACCEPT_MULTISHOT |
		    IORING_ACCEPT_DONTWAIT | IORING_ACCEPT_POLL_FIRST)) != 0 ||
		    (sqe->accept_flags & ~(uint32_t)(SOCK_CLOEXEC |
		    SOCK_NONBLOCK | SOCK_CLOFORK)) != 0 || sqe->len != 0)
			return (EINVAL);
		multishot = (sqe->ioprio & IORING_ACCEPT_MULTISHOT) != 0;
		/*
		 * As in io_uring: no close-on-exec without a descriptor, and
		 * a multishot accept needs a new slot each time.
		 */
		if (direct && ((sqe->accept_flags & SOCK_CLOEXEC) != 0 ||
		    (multishot && sqe->file_index != IORING_FILE_INDEX_ALLOC)))
			return (EINVAL);
		break;
	case IORING_OP_CONNECT:
	case IORING_OP_BIND:
		if (sqe->len != 0 || sqe->rw_flags != 0 || sqe->buf_index != 0 ||
		    sqe->splice_fd_in != 0)
			return (EINVAL);
		error = kqring_prep_sockaddr(op, sqe->addr, sqe->addr2);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_LISTEN:
	case IORING_OP_SHUTDOWN:
		if (sqe->addr != 0 || sqe->addr2 != 0 || sqe->rw_flags != 0 ||
		    sqe->buf_index != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		break;
	case IORING_OP_SOCKET:
		if (sqe->rw_flags != 0 || sqe->addr != 0 ||
		    (direct && (sqe->off & SOCK_CLOEXEC) != 0))
			return (EINVAL);
		break;
	case IORING_OP_NOP:
		if ((sqe->nop_flags & ~IORING_NOP_INJECT_RESULT) != 0)
			return (EINVAL);
		break;
	case IORING_OP_PIPE:
		if (sqe->fd != 0 || sqe->off != 0 || sqe->addr3 != 0 ||
		    sqe->len != 0 || sqe->buf_index != 0 ||
		    (sqe->pipe_flags & ~(uint32_t)(O_CLOEXEC | O_CLOFORK |
		    O_NONBLOCK)) != 0 ||
		    (direct && (sqe->pipe_flags & O_CLOEXEC) != 0))
			return (EINVAL);
		break;
	case IORING_OP_POLL_ADD:
		if (sqe->buf_index != 0 || sqe->off != 0 || sqe->addr != 0)
			return (EINVAL);
		if ((sqe->len & ~(IORING_POLL_ADD_MULTI |
		    IORING_POLL_ADD_LEVEL)) != 0)
			return (EINVAL);
		multishot = (sqe->len & IORING_POLL_ADD_MULTI) != 0;
		break;
	case IORING_OP_POLL_REMOVE:
		if (sqe->buf_index != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		if ((sqe->len & ~(IORING_POLL_UPDATE_EVENTS |
		    IORING_POLL_UPDATE_USER_DATA | IORING_POLL_ADD_MULTI |
		    IORING_POLL_ADD_LEVEL)) != 0 ||
		    ((sqe->len & (IORING_POLL_UPDATE_EVENTS |
		    IORING_POLL_UPDATE_USER_DATA)) == 0 && sqe->len != 0))
			return (EINVAL);
		break;
	case IORING_OP_TIMEOUT:
	case IORING_OP_LINK_TIMEOUT:
		/* No ioprio, as for every io_uring op that doesn't take one. */
		if (sqe->ioprio != 0)
			return (EINVAL);
		error = kqring_timeout_prep(op);
		if (error != 0)
			return (error);
		multishot = (sqe->timeout_flags & IORING_TIMEOUT_MULTISHOT) != 0;
		break;
	case IORING_OP_TIMEOUT_REMOVE:
		error = kqring_timeout_remove_prep(op);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_ASYNC_CANCEL:
		if ((sqe->cancel_flags & ~(IORING_ASYNC_CANCEL_ALL |
		    IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ANY |
		    IORING_ASYNC_CANCEL_FD_FIXED | IORING_ASYNC_CANCEL_USERDATA |
		    IORING_ASYNC_CANCEL_OP)) != 0)
			return (EINVAL);
		/* FD_FIXED qualifies CANCEL_FD, which ANY would override. */
		if ((sqe->cancel_flags & IORING_ASYNC_CANCEL_FD) != 0 ?
		    (sqe->cancel_flags & IORING_ASYNC_CANCEL_ANY) != 0 :
		    (sqe->cancel_flags & IORING_ASYNC_CANCEL_FD_FIXED) != 0)
			return (EINVAL);
		break;
	case IORING_OP_FILES_UPDATE:
		if (sqe->rw_flags != 0 || sqe->splice_fd_in != 0 ||
		    sqe->len == 0)
			return (EINVAL);
		break;
	case IORING_OP_FIXED_FD_INSTALL:
		if (sqe->off != 0 || sqe->addr != 0 || sqe->len != 0 ||
		    sqe->buf_index != 0 || sqe->splice_fd_in != 0 ||
		    sqe->addr3 != 0 ||
		    (sqe->install_fd_flags & ~IORING_FIXED_FD_NO_CLOEXEC) != 0)
			return (EINVAL);
		/* It installs a fixed file, so sqe->fd must be one. */
		if ((sqe->flags & IOSQE_FIXED_FILE) == 0)
			return (EBADF);
		/* Not with borrowed credentials, as io_uring (EPERM). */
		if (sqe->personality != 0)
			return (EPERM);
		break;
	case IORING_OP_PROVIDE_BUFFERS:
		/* sqe->fd buffers, IDs from sqe->off, as in io_uring. */
		if (sqe->rw_flags != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		if (sqe->fd <= 0 || sqe->fd > (int)KQRING_MAX_GROUP_BUFS)
			return (E2BIG);
		if (sqe->len == 0 || sqe->len > IOSIZE_MAX)
			return (EINVAL);
		if (sqe->off > UINT16_MAX)
			return (E2BIG);
		if (sqe->off + (uint32_t)sqe->fd > KQRING_MAX_GROUP_BUFS)
			return (EINVAL);
		/* The buffers' span must not wrap, and be user memory. */
		if (sqe->addr + (uint64_t)sqe->len * (uint32_t)sqe->fd <
		    sqe->addr)
			return (EOVERFLOW);
		if (sqe->addr + (uint64_t)sqe->len * (uint32_t)sqe->fd >
		    VM_MAXUSER_ADDRESS)
			return (EFAULT);
		break;
	case IORING_OP_REMOVE_BUFFERS:
		if (sqe->rw_flags != 0 || sqe->addr != 0 || sqe->len != 0 ||
		    sqe->off != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		if (sqe->fd <= 0 || sqe->fd > (int)KQRING_MAX_GROUP_BUFS)
			return (EINVAL);
		break;
	case IORING_OP_FSYNC:
		if ((sqe->fsync_flags & ~IORING_FSYNC_DATASYNC) != 0 ||
		    sqe->addr != 0 || sqe->buf_index != 0 ||
		    sqe->splice_fd_in != 0)
			return (EINVAL);
		break;
	case IORING_OP_SYNC_FILE_RANGE:
		if ((sqe->sync_range_flags & ~(SYNC_FILE_RANGE_WAIT_BEFORE |
		    SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER)) != 0 ||
		    sqe->addr != 0 || sqe->buf_index != 0 ||
		    sqe->splice_fd_in != 0)
			return (EINVAL);
		break;
	case IORING_OP_FALLOCATE:
	case IORING_OP_FADVISE:
	case IORING_OP_MADVISE:
		if ((sqe->opcode == IORING_OP_FALLOCATE && sqe->rw_flags != 0) ||
		    sqe->buf_index != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		break;
	case IORING_OP_FTRUNCATE:
		if (sqe->rw_flags != 0 || sqe->addr != 0 || sqe->len != 0 ||
		    sqe->buf_index != 0 || sqe->splice_fd_in != 0 ||
		    sqe->addr3 != 0)
			return (EINVAL);
		break;
	case IORING_OP_OPENAT:
		if (sqe->buf_index != 0 ||
		    (direct && (sqe->open_flags & O_CLOEXEC) != 0))
			return (EINVAL);
		error = kqring_prep_paths(op, false);
		if (error != 0)
			return (error);
		pa = op->args;
		pa->how.flags = sqe->open_flags;
		pa->how.mode = sqe->len;
		break;
	case IORING_OP_OPENAT2:
		if (sqe->buf_index != 0)
			return (EINVAL);
		error = kqring_prep_openat2(op);
		if (error != 0)
			return (error);
		pa = op->args;
		/* No close-on-exec without a descriptor, as in io_uring. */
		if (direct && (pa->how.flags & O_CLOEXEC) != 0)
			return (EINVAL);
		break;
	case IORING_OP_CLOSE:
		/* sqe->fd, or with file_index that fixed slot. */
		if (sqe->off != 0 || sqe->addr != 0 || sqe->len != 0 ||
		    sqe->rw_flags != 0 || sqe->buf_index != 0 ||
		    (direct && sqe->fd != 0))
			return (EINVAL);
		break;
	case IORING_OP_STATX:
		if (sqe->buf_index != 0 || sqe->splice_fd_in != 0 ||
		    (sqe->statx_flags & ~(uint32_t)(AT_SYMLINK_NOFOLLOW |
		    AT_EMPTY_PATH | AT_RESOLVE_BENEATH)) != 0 ||
		    (sqe->len & STATX__RESERVED) != 0)
			return (EINVAL);
		error = kqring_prep_paths(op, false);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_RENAMEAT:
	case IORING_OP_LINKAT:
	case IORING_OP_SYMLINKAT:
		if (sqe->buf_index != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		if ((sqe->opcode == IORING_OP_RENAMEAT &&
		    (sqe->rename_flags & ~(uint32_t)AT_RENAME_NOREPLACE) != 0) ||
		    (sqe->opcode == IORING_OP_LINKAT &&
		    (sqe->hardlink_flags & ~(uint32_t)(AT_SYMLINK_FOLLOW |
		    AT_RESOLVE_BENEATH | AT_EMPTY_PATH)) != 0) ||
		    (sqe->opcode == IORING_OP_SYMLINKAT &&
		    (sqe->len != 0 || sqe->rw_flags != 0)))
			return (EINVAL);
		error = kqring_prep_paths(op, true);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_UNLINKAT:
	case IORING_OP_MKDIRAT:
		if (sqe->off != 0 || sqe->buf_index != 0 ||
		    sqe->splice_fd_in != 0 ||
		    (sqe->opcode == IORING_OP_UNLINKAT && (sqe->len != 0 ||
		    (sqe->unlink_flags & ~(uint32_t)(AT_REMOVEDIR |
		    AT_RESOLVE_BENEATH)) != 0)) ||
		    (sqe->opcode == IORING_OP_MKDIRAT && sqe->rw_flags != 0))
			return (EINVAL);
		error = kqring_prep_paths(op, false);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_FGETXATTR:
	case IORING_OP_FSETXATTR:
	case IORING_OP_GETXATTR:
	case IORING_OP_SETXATTR:
		error = kqring_prep_xattr(op);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_FUTEX_WAIT:
	case IORING_OP_FUTEX_WAITV:
		error = kqring_prep_futex_wait(op);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_FUTEX_WAKE:
		if (sqe->len != 0 || sqe->futex_flags != 0 ||
		    sqe->buf_index != 0 || sqe->file_index != 0 ||
		    sqe->addr3 > UINT32_MAX ||
		    !kqring_futex_flags_ok((uint32_t)sqe->fd) ||
		    (sqe->addr & 3) != 0)
			return (EINVAL);
		break;
	case IORING_OP_MSG_RING:
		if (sqe->buf_index != 0 ||
		    (sqe->msg_ring_flags & ~(IORING_MSG_RING_CQE_SKIP |
		    IORING_MSG_RING_FLAGS_PASS)) != 0)
			return (EINVAL);
		if (sqe->addr == IORING_MSG_SEND_FD) {
			/* addr3: our fixed slot, file_index: the target's. */
			if (sqe->len != 0 || sqe->addr3 > UINT32_MAX ||
			    (sqe->msg_ring_flags &
			    IORING_MSG_RING_FLAGS_PASS) != 0)
				return (EINVAL);
			break;
		}
		if (sqe->addr != IORING_MSG_DATA || sqe->addr3 != 0 ||
		    (sqe->msg_ring_flags & IORING_MSG_RING_CQE_SKIP) != 0 ||
		    ((sqe->msg_ring_flags & IORING_MSG_RING_FLAGS_PASS) == 0 &&
		    sqe->file_index != 0))
			return (EINVAL);
		break;
	case IORING_OP_WAITID:
		if (sqe->addr != 0 || sqe->buf_index != 0 || sqe->addr3 != 0 ||
		    sqe->waitid_flags != 0 ||
		    (sqe->file_index & ~(uint32_t)(WNOHANG | WUNTRACED |
		    WCONTINUED | WNOWAIT | WEXITED | WTRAPPED)) != 0)
			return (EINVAL);
		break;
	case IORING_OP_URING_CMD:
		/* IORING_URING_CMD_FIXED needs registered buffers. */
		if (sqe->__pad1 != 0 || sqe->uring_cmd_flags != 0)
			return (EINVAL);
		break;
	case IORING_OP_SPLICE:
		if (sqe->buf_index != 0 ||
		    (sqe->splice_flags & ~(SPLICE_F_MOVE | SPLICE_F_NONBLOCK |
		    SPLICE_F_MORE | SPLICE_F_GIFT | SPLICE_F_FD_IN_FIXED)) != 0)
			return (EINVAL);
		break;
	case IORING_OP_TEE:
		if (sqe->off != 0 || sqe->splice_off_in != 0 ||
		    sqe->buf_index != 0 ||
		    (sqe->splice_flags & ~(SPLICE_F_MOVE | SPLICE_F_NONBLOCK |
		    SPLICE_F_MORE | SPLICE_F_GIFT | SPLICE_F_FD_IN_FIXED)) != 0)
			return (EINVAL);
		break;
	}
	if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0 && !buf_select_ok)
		return (EINVAL);
	/* A bundle is several provided buffers. */
	if ((sqe->ioprio & IORING_RECVSEND_BUNDLE) != 0 && bundle_ok &&
	    (sqe->flags & IOSQE_BUFFER_SELECT) == 0)
		return (EINVAL);
	/* A LINK_TIMEOUT guards the request it is linked behind. */
	if (sqe->opcode == IORING_OP_LINK_TIMEOUT && (prev == NULL ||
	    prev->sqe.opcode == IORING_OP_LINK_TIMEOUT))
		return (EINVAL);
	/* A multishot request never completes, so nothing can follow it. */
	if (multishot && (sqe->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK |
	    IOSQE_CQE_SKIP_SUCCESS)) != 0)
		return (EINVAL);
	return (0);
}

/* ============================================================================
 * Request Lifecycle: Links, Drain, Issue and Completion
 * ============================================================================
 */

/*
 * Complete every request of a chain that will not run: with its own
 * submission error if it had one, else -ECANCELED.  With skip_cqes (the
 * request that failed had IOSQE_CQE_SKIP_SUCCESS) their CQEs are not
 * posted, as io_uring's REQ_F_SKIP_LINK_CQES.
 */
static void
kqring_chain_fail(struct kqring_ctx *ctx, struct kqring_op *op,
    bool skip_cqes)
{
	struct kqring_op *next;

	for (; op != NULL; op = next) {
		next = op->link;
		op->link = NULL;
		if (!skip_cqes)
			kqring_post_cqe(ctx, op->sqe.user_data,
			    op->prep_err != 0 ? -op->prep_err : -ECANCELED, 0);
		kqring_op_free(op);
	}
}

/*
 * kqring_req_complete() for a retired op (cancelled while a worker ran
 * it): if the worker is still running, record the result for its return
 * and leave the op retired; otherwise take it off retired_ops.
 */
static void
kqring_req_complete_retired_locked(struct kqring_ctx *ctx,
    struct kqring_op *op, int32_t *resp, uint32_t *cflagsp)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (!op->wq_retired)
		return;
	if (op->ltimeout != NULL)
		kqring_timer_stop_locked(op->ltimeout);
	if (op->wq_running) {
		op->wq_complete_pending = true;
		op->wq_cancel_res = *resp;
		op->wq_cancel_flags = *cflagsp;
		return;
	}
	/* Report a consumed provided buffer even if cancellation raced I/O. */
	if (*resp == -ECANCELED && op->wq_done && op->res > 0 &&
	    (op->cflags & IORING_CQE_F_BUFFER) != 0) {
		*resp = op->res;
		*cflagsp = op->cflags;
	}
	TAILQ_REMOVE(&ctx->retired_ops, op, active_link);
	op->wq_retired = false;
	if (TAILQ_EMPTY(&ctx->retired_ops) && ctx->wq_abort_pending) {
		callout_stop(&ctx->wq_abort_callout);
		ctx->wq_abort_pending = false;
	}
	kqring_op_disarm_locked(op);
}

/*
 * Final completion of a request that is no longer armed: post its CQE
 * (unless IOSQE_CQE_SKIP_SUCCESS), cancel its LINK_TIMEOUT, and queue the
 * next request of its chain, or fail the rest of the chain if this one
 * failed without IOSQE_IO_HARDLINK.  Successors are queued on ready_q and
 * issued by kqring_flush_ready(), never recursively, so the kernel stack
 * does not grow with the chain length.  Owner thread, ctx->mtx not held.
 */
void
kqring_req_complete(struct kqring_ctx *ctx, struct kqring_op *op, int32_t res,
    uint32_t cflags)
{
	struct kqring_op *next, *lt;
	bool failed, zc;

	/*
	 * Cancellation never waits for a running worker or frees its state.
	 * Only the issuer retires an op (kqring_op_disarm_locked()), and this
	 * runs on the issuer, so the common case is told without ctx->mtx.
	 */
	if (__predict_false(op->wq_retired)) {
		mtx_lock(&ctx->mtx);
		kqring_req_complete_retired_locked(ctx, op, &res, &cflags);
		if (op->wq_retired) {
			/* Still running: the worker's return completes it. */
			mtx_unlock(&ctx->mtx);
			return;
		}
		mtx_unlock(&ctx->mtx);
	}
	KASSERT(!op->armed, ("kqring: completing an armed op"));
	failed = res < 0 && !(res == -ETIME &&
	    (op->sqe.opcode == IORING_OP_TIMEOUT ||
	    op->sqe.opcode == IORING_OP_LINK_TIMEOUT) &&
	    (op->sqe.timeout_flags & IORING_TIMEOUT_ETIME_SUCCESS) != 0);

	/*
	 * SEND_ZC / SENDMSG_ZC: IORING_CQE_F_MORE announces the notification
	 * CQE that the buffer may be reused (deferred until EXT_SFBUF mbufs
	 * are freed on the zero-copy path, or posted immediately when copied).
	 */
	zc = op->sqe.opcode == IORING_OP_SEND_ZC ||
	    op->sqe.opcode == IORING_OP_SENDMSG_ZC;
	if (zc)
		cflags |= IORING_CQE_F_MORE;

	/*
	 * IOSQE_CQE_SKIP_SUCCESS skips a success, and a LINK_TIMEOUT that
	 * fired (-ETIME), which io_uring does not count as failing.
	 */
	if ((op->sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0 || (res < 0 &&
	    !(op->sqe.opcode == IORING_OP_LINK_TIMEOUT && res == -ETIME))) {
		if (op->sqe.opcode == IORING_OP_TIMEOUT)
			kqring_post_cqe_uncounted(ctx, op->sqe.user_data, res,
			    cflags);
		else
			kqring_post_cqe(ctx, op->sqe.user_data, res, cflags);
	}
	if (op->zc_notif != NULL) {
		struct kqring_zc_notif *zcn = op->zc_notif;

		op->zc_notif = NULL;
		if (res < 0)
			kqring_zc_abort(zcn);
		else
			kqring_zc_rele(zcn, true);
	} else if (zc) {
		kqring_post_cqe(ctx, op->sqe.user_data,
		    (op->sqe.ioprio & IORING_SEND_ZC_REPORT_USAGE) != 0 ?
		    (int32_t)IORING_NOTIF_USAGE_ZC_COPIED : 0,
		    IORING_CQE_F_NOTIF);
	}

	lt = op->ltimeout;
	if (lt != NULL) {
		op->ltimeout = NULL;
		mtx_lock(&ctx->mtx);
		kqring_timer_stop_locked(lt);
		mtx_unlock(&ctx->mtx);
		/*
		 * Disarmed, not failed: IOSQE_CQE_SKIP_SUCCESS still skips its
		 * CQE, as in io_uring.
		 */
		if ((lt->sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0)
			kqring_post_cqe(ctx, lt->sqe.user_data, -ECANCELED, 0);
		kqring_op_free(lt);
	}

	next = op->link;
	op->link = NULL;
	/* A LINK_TIMEOUT never armed (op finished inline) is moot. */
	if (next != NULL && next->sqe.opcode == IORING_OP_LINK_TIMEOUT) {
		lt = next;
		next = lt->link;
		lt->link = NULL;
		if ((lt->sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0)
			kqring_post_cqe(ctx, lt->sqe.user_data, -ECANCELED, 0);
		kqring_op_free(lt);
	}
	if (next != NULL) {
		if (failed && (op->sqe.flags & IOSQE_IO_HARDLINK) == 0)
			kqring_chain_fail(ctx, next,
			    (op->sqe.flags & IOSQE_CQE_SKIP_SUCCESS) != 0);
		else
			TAILQ_INSERT_TAIL(&ctx->ready_q, next, q_link);
	}
	kqring_op_free(op);
}

/* Whether op's file is a socket (lookup errors are left to the op). */
bool
kqring_op_is_socket(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct file *fp;
	bool sock;

	if (op->ffile != NULL)
		return (op->ffile->fp->f_type == DTYPE_SOCKET);
	if (fget(td, op->sqe.fd, &cap_no_rights, &fp) != 0)
		return (false);
	sock = fp->f_type == DTYPE_SOCKET;
	fdrop(fp, td);
	return (sock);
}


static void kqring_req_issue_cred(struct kqring_ctx *ctx,
    struct kqring_op *op);

/*
 * Issue a request: run it inline, or arm it to finish later from
 * taskwork.  Every path ends in kqring_req_complete() or an armed op.
 */
void
kqring_req_issue(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct ucred *saved;
	int error;

	saved = kqring_cred_enter(op);
	error = kqring_req_resources(ctx, op);
	if (error != 0)
		kqring_req_complete(ctx, op, -error, 0);
	else
		kqring_req_issue_cred(ctx, op);
	kqring_cred_exit(saved);
}

static int
kqring_op_iopoll_check(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct file *fp;
	bool ok;
	int error;

	if (op->ffile != NULL) {
		fp = op->ffile->fp;
		ok = fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
		    (fp->f_ops->fo_flags & DFLAG_SEEKABLE) != 0;
		return (ok ? 0 : EOPNOTSUPP);
	}
	error = fget(td, op->sqe.fd, &cap_no_rights, &fp);
	if (error != 0)
		return (error);
	ok = fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
	    (fp->f_ops->fo_flags & DFLAG_SEEKABLE) != 0;
	fdrop(fp, td);
	return (ok ? 0 : EOPNOTSUPP);
}

static void
kqring_req_issue_cred(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	bool armed = false;

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		if ((ctx->flags & IORING_SETUP_IOPOLL) != 0) {
			int error = kqring_op_iopoll_check(op);

			if (error != 0) {
				kqring_req_complete(ctx, op, -error, 0);
				return;
			}
			if ((sqe->flags & IOSQE_ASYNC) != 0) {
				armed = kqring_wq_enqueue(ctx, op,
				    kqring_op_wq_type(op));
				break;
			}
			if (kqring_file_bio_try(ctx, op,
			    kqring_op_is_write(sqe->opcode))) {
				armed = true;
				break;
			}
			kqring_exec_file_rw(ctx, op,
			    kqring_op_is_write(sqe->opcode));
			kqring_req_complete(ctx, op, op->res, op->cflags);
			return;
		}
		/*
		 * A socket would block the ring owner in read(2); treat it like
		 * RECV / SEND instead, so it can be armed.  Other descriptors
		 * (files, pipes, devices) run on the in-process worker pool,
		 * unless they can complete here without blocking.
		 */
		if (!kqring_op_is_socket(op)) {
			int rw = KQRING_RW_PUNT;

			if ((sqe->flags & IOSQE_ASYNC) == 0)
				rw = kqring_file_rw_inline(ctx, op,
				    kqring_op_is_write(sqe->opcode));
			if (rw == KQRING_RW_DONE) {
				kqring_req_complete(ctx, op, op->res,
				    op->cflags);
				return;
			}
			if (rw == KQRING_RW_BIO) {
				armed = true;
			} else if ((sqe->rw_flags & KQRING_RWF_NOWAIT) != 0) {
				/* RWF_NOWAIT: no worker, which might block. */
				kqring_req_complete(ctx, op, -EAGAIN, 0);
				return;
			} else {
				armed = kqring_wq_enqueue(ctx, op,
				    kqring_op_wq_type(op));
			}
			break;
		}
		op->sock_rw = true;
		/* FALLTHROUGH */
	case IORING_OP_RECV:
	case IORING_OP_RECVMSG:
	case IORING_OP_READ_MULTISHOT:
	case IORING_OP_SEND:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG:
	case IORING_OP_SENDMSG_ZC:
	case IORING_OP_CONNECT:
	case IORING_OP_ACCEPT:
	case IORING_OP_POLL_ADD:
		/*
		 * Try inline first (unless IOSQE_ASYNC asks to defer), even
		 * with POLL_FIRST (a hint) or multishot: data that arrived
		 * before submission completes in this same enter, without an
		 * upcall + taskwork round trip.  A multishot poll is armed
		 * directly, so that readiness that already exists is reported
		 * once (through ready_now).
		 */
		if ((sqe->flags & IOSQE_ASYNC) == 0 &&
		    (sqe->opcode != IORING_OP_POLL_ADD ||
		    (sqe->len & IORING_POLL_ADD_MULTI) == 0)) {
			if (kqring_op_attempt(ctx, op, NULL) == 0) {
				kqring_req_complete(ctx, op, op->res,
				    op->cflags);
				return;
			}
		}
		/*
		 * IORING_ACCEPT_DONTWAIT, RWF_NOWAIT on a READ/WRITE of a
		 * socket, or MSG_DONTWAIT on a send or receive: fail rather
		 * than wait, as io_uring does.
		 */
		if ((sqe->opcode == IORING_OP_ACCEPT &&
		    (sqe->ioprio & IORING_ACCEPT_DONTWAIT) != 0) ||
		    (op->sock_rw && (sqe->rw_flags & KQRING_RWF_NOWAIT) != 0) ||
		    (!op->sock_rw && kqring_op_msg_flags_ok(sqe->opcode) &&
		    (sqe->msg_flags & MSG_DONTWAIT) != 0)) {
			kqring_req_complete(ctx, op, -EAGAIN, 0);
			return;
		}
		if ((sqe->opcode == IORING_OP_POLL_ADD ||
		    sqe->opcode == IORING_OP_READ_MULTISHOT) &&
		    !kqring_op_is_socket(op))
			armed = kqring_arm_nonsock_op(ctx, op);
		else
			armed = kqring_arm_socket_op(ctx, op,
			    kqring_op_dirs(sqe));
		break;
	case IORING_OP_TIMEOUT:
		kqring_timeout_arm(ctx, op);
		armed = true;
		break;
	case IORING_OP_LINK_TIMEOUT:
		/* Consumed by the request it follows; never issued alone. */
		kqring_req_complete(ctx, op, -EINVAL, 0);
		return;
	case IORING_OP_FUTEX_WAIT:
	case IORING_OP_FUTEX_WAITV:
		armed = kqring_futex_arm(ctx, op);
		break;
	case IORING_OP_FUTEX_WAKE:
		kqring_req_complete(ctx, op, kqring_futex_wake(sqe), 0);
		return;
	case IORING_OP_WAITID:
		armed = kqring_waitid_arm(ctx, op);
		break;
	case IORING_OP_MSG_RING:
		kqring_req_complete(ctx, op, kqring_msg_ring(ctx, op), 0);
		return;
	case IORING_OP_ASYNC_CANCEL:
		kqring_req_complete(ctx, op, kqring_cancel(ctx, sqe), 0);
		return;
	case IORING_OP_TIMEOUT_REMOVE: {
		struct kqring_op *gone = NULL;
		int32_t res = kqring_timeout_remove(ctx, op, &gone);

		kqring_req_complete(ctx, op, res, 0);
		if (gone != NULL)
			kqring_req_complete(ctx, gone, -ECANCELED, 0);
		return;
	}
	case IORING_OP_POLL_REMOVE:
		kqring_req_complete(ctx, op, kqring_poll_remove(ctx, sqe), 0);
		return;
	default:
		if (kqring_op_use_wq(sqe)) {
			armed = kqring_wq_enqueue(ctx, op,
			    kqring_op_wq_type(op));
			break;
		}
		kqring_exec_sync(ctx, op);
		kqring_req_complete(ctx, op, op->res, op->cflags);
		return;
	}
	if (armed)
		kqring_ltimeout_arm(ctx, op);
}

/*
 * Issue queued link successors, then deferred chains whose IOSQE_IO_DRAIN
 * barrier has cleared: a drained chain starts once nothing else is in
 * flight, and nothing submitted after it starts before it completes.
 */
void
kqring_flush_ready(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	for (;;) {
		if ((op = TAILQ_FIRST(&ctx->ready_q)) != NULL) {
			TAILQ_REMOVE(&ctx->ready_q, op, q_link);
			kqring_req_issue(ctx, op);
			continue;
		}
		if ((op = TAILQ_FIRST(&ctx->defer_q)) == NULL)
			return;
		if (ctx->nreqs == ctx->ndeferred)
			ctx->drain_barrier = false;
		if ((op->drain || ctx->drain_barrier) &&
		    ctx->nreqs != ctx->ndeferred)
			return;
		TAILQ_REMOVE(&ctx->defer_q, op, q_link);
		ctx->ndeferred -= op->nchain;
		if (op->drain)
			ctx->drain_barrier = true;
		TAILQ_INSERT_TAIL(&ctx->ready_q, op, q_link);
	}
}

/* Submit a complete chain (a lone request is a chain of one). */
static void
kqring_chain_submit(struct kqring_ctx *ctx, struct kqring_op *head,
    bool failed)
{
	uint32_t others;

	if (failed) {
		/* As in io_uring, a bad SQE fails its whole chain. */
		kqring_chain_fail(ctx, head, false);
		return;
	}
	others = ctx->nreqs - ctx->ndeferred - head->nchain;
	if (others == 0)
		ctx->drain_barrier = false;
	if (!TAILQ_EMPTY(&ctx->defer_q) ||
	    ((head->drain || ctx->drain_barrier) && others != 0)) {
		ctx->ndeferred += head->nchain;
		TAILQ_INSERT_TAIL(&ctx->defer_q, head, q_link);
		return;
	}
	if (head->drain)
		ctx->drain_barrier = true;
	TAILQ_INSERT_TAIL(&ctx->ready_q, head, q_link);
	kqring_flush_ready(ctx);
}

/* Free a request that will never complete (ring teardown). */
void
kqring_req_destroy(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_op *next;

	for (; op != NULL; op = next) {
		next = op->link;
		op->link = NULL;
		if (op->ltimeout != NULL) {
			mtx_lock(&ctx->mtx);
			kqring_timer_stop_locked(op->ltimeout);
			mtx_unlock(&ctx->mtx);
			kqring_op_free(op->ltimeout);
			op->ltimeout = NULL;
		}
		kqring_op_free(op);
	}
}

/*
 * Run one queued taskwork item: a worker completion, a socket that became
 * ready, a woken futex, or an expired timer.
 */
static void
kqring_taskwork_run_one(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (op->wq_retired) {
		kqring_req_complete(ctx, op, op->wq_cancel_res,
		    op->wq_cancel_flags);
		return;
	}
	/* A worker, or the last of a disk request's bios, finished. */
	if (op->wq_done || op->bio_done) {
		int32_t res = op->res;

		if (op->wq_fp != NULL) {
			struct file *fp = op->wq_fp;

			op->wq_fp = NULL;
			res = kqring_fixed_install(ctx, fp, &op->wq_caps,
			    op->sqe.file_index);
		}
		mtx_lock(&ctx->mtx);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, res, op->cflags);
		return;
	}
	/* A non-socket poll's knotes fired (kqring_kpoll.c). */
	if (op->kp_on) {
		kqring_kpoll_deliver(ctx, op);
		return;
	}
	switch (op->sqe.opcode) {
	case IORING_OP_TIMEOUT:
	case IORING_OP_LINK_TIMEOUT:
		kqring_timeout_fire(ctx, op);
		return;
	case IORING_OP_FUTEX_WAIT:
	case IORING_OP_FUTEX_WAITV:
		/* Queued by FUTEX_WAKE, which set ->res; nothing else queues. */
		if (!op->futex_woken)
			return;
		mtx_lock(&ctx->mtx);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, op->res, 0);
		return;
	}
	/*
	 * Only this thread disarms ops, so op->so stays put.  EAGAIN (a
	 * spurious wakeup, or a multishot op that drained its burst) keeps
	 * the op armed.
	 */
	struct ucred *saved = kqring_cred_enter(op);

	if (kqring_op_attempt(ctx, op, op->so) == EAGAIN) {
		if (op->wq_wait_ready)
			kqring_wq_rearm_ready(ctx, op);
		kqring_cred_exit(saved);
		return;
	}
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, op->res, op->cflags);
	kqring_cred_exit(saved);
}

/* Post the CQEs other rings sent with MSG_RING.  Drops ctx->mtx. */
static void
kqring_post_remote_locked(struct kqring_ctx *ctx)
{
	STAILQ_HEAD(, kqring_rcqe) q = STAILQ_HEAD_INITIALIZER(q);
	struct kqring_rcqe *rc;

	mtx_assert(&ctx->mtx, MA_OWNED);
	STAILQ_CONCAT(&q, &ctx->rcq);
	ctx->nrcq = 0;
	mtx_unlock(&ctx->mtx);
	while ((rc = STAILQ_FIRST(&q)) != NULL) {
		STAILQ_REMOVE_HEAD(&q, link);
		if (rc->fp != NULL) {
			/* IORING_MSG_SEND_FD: the table takes fp and caps. */
			rc->cqe.res = kqring_fixed_install(ctx, rc->fp,
			    &rc->caps, rc->file_index);
			rc->fp = NULL;
		}
		if (!rc->skip_cqe) {
			ctx->cq_seq++;
			kqring_post_cqe_extra(ctx, rc->cqe.user_data,
			    rc->cqe.res, rc->cqe.flags,
			    rc->has_extra ? rc->extra : NULL);
		}
		kqring_rcqe_free(rc);
	}
	mtx_lock(&ctx->mtx);
}

/*
 * Run deferred taskwork in curthread context (DEFER_TASKRUN), post
 * MSG_RING CQEs, and issue the link successors it unblocks.
 */
void
kqring_run_taskwork(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	/* Successors issued below may queue more taskwork; bound the loop. */
	for (int pass = 0; pass < 64; pass++) {
		/*
		 * Unlocked fast path: a racing upcall that is missed here is
		 * seen by the wait loop, which re-checks taskwork_ops under
		 * ctx->mtx before sleeping.
		 */
		if (TAILQ_EMPTY(&ctx->taskwork_ops) && (ctx->shm == NULL ||
		    (atomic_load_32(&ctx->shm->sq_flags) &
		    IORING_SQ_TASKRUN) == 0))
			break;

		mtx_lock(&ctx->mtx);
		for (;;) {
			if ((op = TAILQ_FIRST(&ctx->taskwork_ops)) != NULL) {
				TAILQ_REMOVE(&ctx->taskwork_ops, op,
				    taskwork_link);
				op->in_taskwork = false;
				if (ctx->closing || (op->cancelled &&
				    !op->wq_retired))
					continue;
				mtx_unlock(&ctx->mtx);
				kqring_taskwork_run_one(ctx, op);
				mtx_lock(&ctx->mtx);
				continue;
			}
			if (STAILQ_EMPTY(&ctx->rcq))
				break;
			kqring_post_remote_locked(ctx);
		}
		if (ctx->shm != NULL)
			atomic_clear_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
		mtx_unlock(&ctx->mtx);
		kqring_flush_ready(ctx);
	}
}


void
kqring_submit_sqes(struct kqring_ctx *ctx, uint32_t max_submit,
    uint32_t *submitted_out)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t head, tail, avail, consumed = 0, submitted = 0;
	struct kqring_op *chain_head = NULL, *chain_tail = NULL;
	bool chain_failed = false, chain_drop = false;

	kqring_flush_overflow(ctx);
	head = shm->sq_head;
	tail = atomic_load_acq_32(&shm->sq_tail);
	avail = MIN(tail - head, ctx->sq_entries);
	max_submit = MIN(max_submit, avail);
	/* CQEs posted inline here are not "async" (EVENTFD_ASYNC). */
	ctx->submitting = true;
	while (consumed < max_submit) {
		uint32_t sqe_idx = ctx->sq_array[head & ctx->sq_mask];
		head++;
		consumed++;
		if (sqe_idx >= ctx->sq_entries) {
			/* As in io_uring: consumed, counted, not submitted. */
			atomic_add_32(&shm->sq_dropped, 1);
			continue;
		}
		if (chain_drop || ctx->nreqs >= KQRING_MAX_REQS) {
			struct io_uring_sqe sqe;
			int32_t err = chain_drop ? -ECANCELED : -EBUSY;

			if ((ctx->flags & IORING_SETUP_SQE128) != 0) {
				const uint8_t *raw =
				    (const uint8_t *)ctx->sqes +
				    (size_t)sqe_idx * 128;

				memcpy(&sqe, raw, sizeof(sqe));
			} else {
				sqe = ctx->sqes[sqe_idx];
			}
			if (chain_head != NULL) {
				kqring_chain_fail(ctx, chain_head, false);
				chain_head = chain_tail = NULL;
				chain_failed = false;
			}
			kqring_post_cqe(ctx, sqe.user_data, err, 0);
			if ((sqe.flags &
			    (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) != 0) {
				chain_drop = true;
			} else {
				chain_drop = false;
				atomic_store_rel_32(&shm->sq_head, head);
			}
			submitted++;
			continue;
		}
		struct kqring_op *op = kqring_op_alloc(ctx);
		if ((ctx->flags & IORING_SETUP_SQE128) != 0) {
			const uint8_t *raw = (const uint8_t *)ctx->sqes +
			    (size_t)sqe_idx * 128;

			memcpy(&op->sqe, raw, sizeof(op->sqe));
			memcpy(op->sqe_extra, raw + sizeof(op->sqe),
			    sizeof(op->sqe_extra));
		} else {
			op->sqe = ctx->sqes[sqe_idx];
		}
		op->prep_err = kqring_req_prep(op, chain_tail);
		if (chain_head == NULL) {
			chain_head = op;
			chain_failed = false;
		} else {
			chain_tail->link = op;
		}
		chain_tail = op;
		chain_head->nchain++;
		if (op->prep_err != 0)
			chain_failed = true;
		if ((op->sqe.flags & IOSQE_IO_DRAIN) != 0)
			chain_head->drain = true;
		if ((op->sqe.flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) == 0) {
			int err = op->prep_err;

			atomic_store_rel_32(&shm->sq_head, head);
			kqring_chain_submit(ctx, chain_head, chain_failed);
			chain_head = chain_tail = NULL;
			/*
			 * Without SUBMIT_ALL, an SQE that fails its checks ends
			 * the submission (counted, with its error CQE), as in
			 * io_uring; one inside a link only fails the link.
			 */
			if (err != 0 &&
			    (ctx->flags & IORING_SETUP_SUBMIT_ALL) == 0) {
				submitted++;
				break;
			}
		}
		submitted++;
	}
	atomic_store_rel_32(&shm->sq_head, head);
	if (chain_head != NULL)
		kqring_chain_submit(ctx, chain_head, chain_failed);
	ctx->submitting = false;
	*submitted_out = submitted;
}
