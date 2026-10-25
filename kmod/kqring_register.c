/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

/*
 * Put fp (a reference the table takes over) in the process's registered-
 * ring table, at *slotp or, for IORING_FILE_INDEX_ALLOC, in the first free
 * slot, which *slotp is set to.
 */
int
kqring_ring_fds_insert(struct thread *td, struct file *fp, uint32_t *slotp)
{
	struct kqring_proc_rings *pr, *alloc_pr;
	uint32_t slot = *slotp;
	int error = 0;

	alloc_pr = malloc(sizeof(*alloc_pr), M_KQRING, M_WAITOK | M_ZERO);
	alloc_pr->proc = td->td_proc;
	mtx_lock(&kqring_proc_mtx);
	pr = kqring_proc_find_locked(td->td_proc);
	if (pr == NULL) {
		pr = alloc_pr;
		alloc_pr = NULL;
		rm_wlock(&kqring_rings_rm);
		LIST_INSERT_HEAD(kqring_proc_bucket(td->td_proc), pr, link);
		rm_wunlock(&kqring_rings_rm);
	}
	if (slot == IORING_FILE_INDEX_ALLOC) {
		for (slot = 0; slot < KQRING_MAX_REG_RINGS; slot++) {
			if (pr->rings[slot] == NULL)
				break;
		}
		if (slot == KQRING_MAX_REG_RINGS)
			error = ENFILE;
	} else if (pr->rings[slot] != NULL) {
		error = EBUSY;
	}
	if (error == 0) {
		rm_wlock(&kqring_rings_rm);
		pr->rings[slot] = fp;
		rm_wunlock(&kqring_rings_rm);
		pr->nrings++;
		*slotp = slot;
	}
	mtx_unlock(&kqring_proc_mtx);
	free(alloc_pr, M_KQRING);
	return (error);
}

static int
kqring_ring_fds_register(struct thread *td,
    const struct kqring_register_arg *ra, int *resp)
{
	struct io_uring_rsrc_update up;
	struct kqring_proc_rings *pr;
	struct kqring_ctx *rctx;
	struct file *fp;
	cap_rights_t rights;
	uint32_t i, slot;
	int error = 0;

	if (ra->arg == NULL || ra->nr_args == 0)
		return (EINVAL);
	if (ra->nr_args > KQRING_MAX_REG_RINGS)
		return (EMFILE);

	for (i = 0; i < ra->nr_args; i++) {
		uint8_t *uptr = (uint8_t *)ra->arg + i * sizeof(up);

		error = copyin(uptr, &up, sizeof(up));
		if (error != 0)
			break;
		if (up.resv != 0) {
			error = EINVAL;
			break;
		}
		if (up.offset != IORING_FILE_INDEX_ALLOC &&
		    up.offset >= KQRING_MAX_REG_RINGS) {
			error = EINVAL;
			break;
		}
		error = fget(td, (int)up.data,
		    cap_rights_init_one(&rights, CAP_IOCTL), &fp);
		if (error != 0)
			break;
		error = kqring_fp_ctx(fp, &rctx);
		if (error == 0 && (!rctx->setup_done || rctx->closing))
			error = EBADF;
		if (error != 0) {
			fdrop(fp, td);
			break;
		}
		slot = up.offset;
		error = kqring_ring_fds_insert(td, fp, &slot);
		if (error != 0) {
			fdrop(fp, td);
			break;
		}
		if (up.offset == IORING_FILE_INDEX_ALLOC) {
			up.offset = slot;
			error = copyout(&up.offset, uptr, sizeof(up.offset));
			if (error != 0) {
				mtx_lock(&kqring_proc_mtx);
				pr = kqring_proc_find_locked(td->td_proc);
				if (pr != NULL && pr->rings[slot] == fp) {
					rm_wlock(&kqring_rings_rm);
					pr->rings[slot] = NULL;
					rm_wunlock(&kqring_rings_rm);
					pr->nrings--;
				} else {
					fp = NULL;
				}
				mtx_unlock(&kqring_proc_mtx);
				if (fp != NULL)
					fdrop(fp, td);
				break;
			}
		}
	}
	if (i == 0)
		return (error);
	*resp = (int)i;
	return (0);
}

static int
kqring_ring_fds_unregister(struct thread *td,
    const struct kqring_register_arg *ra, int *resp)
{
	struct io_uring_rsrc_update up;
	struct kqring_proc_rings *pr;
	struct file *fp;
	uint32_t i;
	int error = 0;

	if (ra->arg == NULL || ra->nr_args == 0 ||
	    ra->nr_args > KQRING_MAX_REG_RINGS)
		return (EINVAL);

	for (i = 0; i < ra->nr_args; i++) {
		error = copyin((const uint8_t *)ra->arg + i * sizeof(up),
		    &up, sizeof(up));
		if (error != 0)
			break;
		if (up.resv != 0 || up.offset >= KQRING_MAX_REG_RINGS) {
			error = EINVAL;
			break;
		}
		mtx_lock(&kqring_proc_mtx);
		pr = kqring_proc_find_locked(td->td_proc);
		fp = pr != NULL ? pr->rings[up.offset] : NULL;
		if (fp != NULL) {
			rm_wlock(&kqring_rings_rm);
			pr->rings[up.offset] = NULL;
			rm_wunlock(&kqring_rings_rm);
			pr->nrings--;
		}
		mtx_unlock(&kqring_proc_mtx);
		if (fp == NULL) {
			error = EBADF;
			break;
		}
		fdrop(fp, td);
	}
	if (i == 0)
		return (error);
	*resp = (int)i;
	return (0);
}


/* IORING_REGISTER_RESTRICTIONS, on a ring created IORING_SETUP_R_DISABLED. */
static int
kqring_restrictions_register(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct kqring_restrictions *r = &ctx->rst;
	struct io_uring_restriction res;
	int error = 0;

	if ((ctx->flags & IORING_SETUP_R_DISABLED) == 0 || ctx->enabled)
		return (EBADF);
	if (r->registered)
		return (EBUSY);
	if (ra->arg == NULL || ra->nr_args == 0 || ra->nr_args >
	    IORING_RESTRICTION_LAST + IORING_REGISTER_LAST + IORING_OP_LAST)
		return (EINVAL);
	for (uint32_t i = 0; i < ra->nr_args && error == 0; i++) {
		error = copyin((const char *)ra->arg + i * sizeof(res), &res,
		    sizeof(res));
		if (error != 0)
			break;
		switch (res.opcode) {
		case IORING_RESTRICTION_REGISTER_OP:
			if (res.register_op >= IORING_REGISTER_LAST)
				error = EINVAL;
			else
				kqring_bit_set(r->register_op, res.register_op);
			break;
		case IORING_RESTRICTION_SQE_OP:
			if (res.sqe_op >= IORING_OP_LAST)
				error = EINVAL;
			else
				kqring_bit_set(r->sqe_op, res.sqe_op);
			break;
		case IORING_RESTRICTION_SQE_FLAGS_ALLOWED:
			r->sqe_flags_allowed = res.sqe_flags;
			break;
		case IORING_RESTRICTION_SQE_FLAGS_REQUIRED:
			r->sqe_flags_required = res.sqe_flags;
			break;
		default:
			error = EINVAL;
			break;
		}
	}
	if (error != 0) {
		bzero(r, sizeof(*r));
		return (error);
	}
	r->registered = true;
	return (0);
}


/*
 * IORING_REGISTER_EVENTFD(_ASYNC): signal an eventfd as CQEs are posted
 * (with _ASYNC, only for requests that did not complete inline) and when
 * taskwork is queued for the owner.  kqring holds an eventfd reference,
 * and signals it under ctx->mtx, which unregistering also takes.
 */
static int
kqring_evfd_register(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra, bool async)
{
	struct thread *td = curthread;
	struct eventfd *efd;
	struct file *fp;
	cap_rights_t rights;
	int error, fd;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	if (ctx->evfd != NULL)
		return (EBUSY);
	error = copyin(ra->arg, &fd, sizeof(fd));
	if (error != 0)
		return (error);
	error = fget(td, fd, cap_rights_init_one(&rights, CAP_WRITE), &fp);
	if (error != 0)
		return (error);
	efd = eventfd_get(fp);
	fdrop(fp, td);
	if (efd == NULL)
		return (EINVAL);
	mtx_lock(&ctx->mtx);
	ctx->evfd = efd;
	ctx->evfd_async = async;
	ctx->evfd_cqes = 0;
	mtx_unlock(&ctx->mtx);
	return (0);
}

static int
kqring_evfd_unregister(struct kqring_ctx *ctx)
{
	struct eventfd *efd;

	mtx_lock(&ctx->mtx);
	efd = ctx->evfd;
	ctx->evfd = NULL;
	mtx_unlock(&ctx->mtx);
	if (efd == NULL)
		return (ENXIO);
	eventfd_put(efd);
	return (0);
}

/*
 * Signal the eventfd and pollers if CQEs were posted since the last
 * signal; whoever posts them (an enter, the ring's kthread) calls this
 * before it sleeps or returns.  On a ring that other threads may wait on
 * (shared, or with a kthread), wake those waiters too.
 */
void
kqring_evfd_flush(struct kqring_ctx *ctx)
{
	bool waiters = ctx->shared || kqring_has_kthread(ctx);

	if (ctx->evfd_cqes == 0 ||
	    (ctx->evfd == NULL && !ctx->pollers && !waiters))
		return;
	ctx->evfd_cqes = 0;
	mtx_lock(&ctx->mtx);
	kqring_evfd_signal_locked(ctx);
	if (waiters && ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
	mtx_unlock(&ctx->mtx);
}

/* Validate ASYNC_CANCEL flags, as kqring_req_prep() does. */
static int
kqring_cancel_flags_ok(uint32_t flags)
{
	if ((flags & ~(IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD |
	    IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_FD_FIXED |
	    IORING_ASYNC_CANCEL_USERDATA | IORING_ASYNC_CANCEL_OP)) != 0)
		return (EINVAL);
	if ((flags & IORING_ASYNC_CANCEL_FD) != 0 ?
	    (flags & IORING_ASYNC_CANCEL_ANY) != 0 :
	    (flags & IORING_ASYNC_CANCEL_FD_FIXED) != 0)
		return (EINVAL);
	return (0);
}

/*
 * IORING_REGISTER_SYNC_CANCEL: ASYNC_CANCEL without an SQE, which returns
 * once the requests it cancelled have completed (a request running on a
 * worker, or disk I/O, completes when that returns), or ETIME after
 * sc.timeout (both fields -1: no timeout), as in io_uring.
 */
static int
kqring_sync_cancel(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra, int *resp)
{
	struct io_uring_sync_cancel_reg sc;
	struct io_uring_sqe sqe;
	struct timespec ts;
	bool forever;
	int32_t res;
	int error;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	error = copyin(ra->arg, &sc, sizeof(sc));
	if (error != 0)
		return (error);
	for (size_t i = 0; i < nitems(sc.pad); i++)
		if (sc.pad[i] != 0)
			return (EINVAL);
	for (size_t i = 0; i < nitems(sc.pad2); i++)
		if (sc.pad2[i] != 0)
			return (EINVAL);
	error = kqring_cancel_flags_ok(sc.flags);
	if (error != 0)
		return (error);
	/* The whole request is checked before anything is cancelled. */
	ts.tv_sec = sc.timeout.tv_sec;
	ts.tv_nsec = sc.timeout.tv_nsec;
	forever = ts.tv_sec == -1 && ts.tv_nsec == -1;
	if (!forever &&
	    (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000))
		return (EINVAL);
	bzero(&sqe, sizeof(sqe));
	sqe.opcode = IORING_OP_ASYNC_CANCEL;
	sqe.cancel_flags = sc.flags;
	sqe.fd = sc.fd;
	sqe.addr = sc.addr;
	sqe.len = sc.opcode;
	res = kqring_cancel(ctx, &sqe);
	/* Issue the link successors that cancellation released. */
	kqring_flush_ready(ctx);
	if (res < 0 && res != -EALREADY)
		return (-res);
	error = kqring_cancel_wait(ctx, &sqe, !forever,
	    forever ? 0 : kqring_ts_to_sbt(&ts));
	kqring_flush_ready(ctx);
	if (error != 0)
		return (error);
	*resp = res > 0 ? res : 0;
	return (0);
}

/*
 * IORING_REGISTER_SEND_MSG_RING: MSG_RING (IORING_MSG_DATA only, as in
 * io_uring) without a submission, from any thread.
 */
static int
kqring_register_send_msg(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct kqring_op op;
	int32_t res;
	int error;

	if (ra->nr_args != 1 || ra->arg == NULL)
		return (EINVAL);
	bzero(&op, sizeof(op));
	error = copyin(ra->arg, &op.sqe, sizeof(op.sqe));
	if (error != 0)
		return (error);
	if (op.sqe.opcode != IORING_OP_MSG_RING || op.sqe.flags != 0 ||
	    op.sqe.addr != IORING_MSG_DATA || op.sqe.buf_index != 0 ||
	    op.sqe.addr3 != 0 || (op.sqe.msg_ring_flags &
	    ~IORING_MSG_RING_FLAGS_PASS) != 0 ||
	    ((op.sqe.msg_ring_flags & IORING_MSG_RING_FLAGS_PASS) == 0 &&
	    op.sqe.file_index != 0))
		return (EINVAL);
	op.ctx = ctx;
	res = kqring_msg_ring(ctx, &op);
	return (res < 0 ? -res : 0);
}

/*
 * IORING_REGISTER_FILE_ALLOC_RANGE: the slots IORING_FILE_INDEX_ALLOC
 * picks from.
 */
static int
kqring_file_alloc_range(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct io_uring_file_index_range rr;
	int error;

	if (ra->arg == NULL || ra->nr_args != 0)
		return (EINVAL);
	error = copyin(ra->arg, &rr, sizeof(rr));
	if (error != 0)
		return (error);
	if (rr.len > UINT32_MAX - rr.off)
		return (EOVERFLOW);
	if (rr.resv != 0 || rr.off + rr.len > ctx->nfiles)
		return (EINVAL);
	ctx->file_alloc_off = ctx->file_alloc_hint = rr.off;
	ctx->file_alloc_end = rr.off + rr.len;
	return (0);
}

/*
 * IORING_REGISTER_CLOCK: the clock of wait timeouts.  They are relative,
 * so either clock gives the same result except across a suspend.
 */
static int
kqring_register_clock(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct io_uring_clock_register cr;
	int error;

	if (ra->arg == NULL || ra->nr_args != 0)
		return (EINVAL);
	error = copyin(ra->arg, &cr, sizeof(cr));
	if (error != 0)
		return (error);
	if (cr.__resv[0] != 0 || cr.__resv[1] != 0 || cr.__resv[2] != 0)
		return (EINVAL);
	if (cr.clockid != CLOCK_MONOTONIC && cr.clockid != CLOCK_BOOTTIME)
		return (EINVAL);
	ctx->clockid = cr.clockid;
	return (0);
}

/*
 * IORING_REGISTER_NAPI / IORING_UNREGISTER_NAPI.  Linux busy-polls the
 * NAPI contexts of a ring's sockets while it waits; FreeBSD has no such
 * per-socket polling for a module to call, so kqring keeps the setting's
 * other half: a wait spins up to busy_poll_to microseconds for completions
 * (from the network stack, disk drivers or workers on other CPUs) before
 * it sleeps, saving the sleep and wake-up when they come quickly.  The
 * previous settings are copied back, as io_uring does.
 */
static int
kqring_register_napi(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra, bool reg)
{
	struct io_uring_napi napi, curr = {
		.busy_poll_to = ctx->busy_poll_us,
		.prefer_busy_poll = ctx->napi_prefer,
		.op_param = ctx->napi_track,
	};
	int error;

	if (ra->nr_args != 1)
		return (EINVAL);
	if (reg) {
		if (ra->arg == NULL)
			return (EFAULT);
		error = copyin(ra->arg, &napi, sizeof(napi));
		if (error != 0)
			return (error);
		if (napi.pad[0] != 0 || napi.pad[1] != 0 || napi.resv != 0)
			return (EINVAL);
		if (napi.opcode != IO_URING_NAPI_REGISTER_OP)
			return (EOPNOTSUPP);	/* NAPI ids */
		if (napi.op_param != IO_URING_NAPI_TRACKING_DYNAMIC &&
		    napi.op_param != IO_URING_NAPI_TRACKING_STATIC)
			return (EINVAL);
	}
	if (ra->arg != NULL) {
		error = copyout(&curr, ra->arg, sizeof(curr));
		if (error != 0)
			return (error);
	}
	if (reg) {
		ctx->busy_poll_us = MIN(napi.busy_poll_to,
		    KQRING_MAX_BUSY_POLL_US);
		ctx->busy_poll_sbt = ctx->busy_poll_us * SBT_1US;
		ctx->napi_prefer = napi.prefer_busy_poll;
		ctx->napi_track = (uint8_t)napi.op_param;
	} else {
		ctx->busy_poll_us = 0;
		ctx->busy_poll_sbt = 0;
		ctx->napi_prefer = 0;
		ctx->napi_track = IO_URING_NAPI_TRACKING_INACTIVE;
	}
	return (0);
}

/*
 * IORING_REGISTER_IOWQ_AFF / IORING_UNREGISTER_IOWQ_AFF: pin in-process
 * worker threads to a subset of the process's allowed CPUs, or restore
 * the process cpuset.
 */
static int
kqring_iowq_aff(struct kqring_ctx *ctx, const struct kqring_register_arg *ra,
    bool set)
{
	struct kqring_wq *wq = ctx->wq;
	cpuset_t mask;
	size_t len;
	int error;

	if (wq == NULL)
		return (EINVAL);
	if (!set) {
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		mtx_lock(&wq->mtx);
		wq->aff_set = false;
		CPU_ZERO(&wq->cpumask);
		wq->aff_gen++;
		wakeup(&wq->ops[IO_WQ_BOUND]);
		wakeup(&wq->ops[IO_WQ_UNBOUND]);
		mtx_unlock(&wq->mtx);
		return (0);
	}
	if (ra->nr_args == 0 || ra->nr_args > PAGE_SIZE)
		return (EINVAL);
	if (ra->arg == NULL)
		return (EFAULT);
	CPU_ZERO(&mask);
	len = MIN((size_t)ra->nr_args, sizeof(mask));
	error = copyin(ra->arg, &mask, len);
	if (error != 0)
		return (error);
	if (wq->base_cpuset != NULL)
		CPU_AND(&mask, &mask, &wq->base_cpuset->cs_mask);
	else
		CPU_AND(&mask, &mask, &all_cpus);
	if (CPU_EMPTY(&mask))
		return (EINVAL);
	mtx_lock(&wq->mtx);
	CPU_COPY(&mask, &wq->cpumask);
	wq->aff_set = true;
	wq->aff_gen++;
	wakeup(&wq->ops[IO_WQ_BOUND]);
	wakeup(&wq->ops[IO_WQ_UNBOUND]);
	mtx_unlock(&wq->mtx);
	return (0);
}

/*
 * IORING_REGISTER_IOWQ_MAX_WORKERS: set and/or query the maximum number of
 * IO_WQ_BOUND (arg[0]) and IO_WQ_UNBOUND (arg[1]) workers for this ring.
 * A zero entry leaves that pool's limit unchanged; both current limits are
 * written back.
 */
static int
kqring_iowq_max_workers(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra)
{
	struct kqring_wq *wq = ctx->wq;
	uint32_t vals[KQRING_WQ_TYPES];
	int error;

	if (wq == NULL || ra->nr_args != KQRING_WQ_TYPES)
		return (EINVAL);
	if (ra->arg == NULL)
		return (EFAULT);
	error = copyin(ra->arg, vals, sizeof(vals));
	if (error != 0)
		return (error);
	if (vals[IO_WQ_BOUND] > INT32_MAX || vals[IO_WQ_UNBOUND] > INT32_MAX)
		return (EINVAL);
	mtx_lock(&wq->mtx);
	for (int t = 0; t < KQRING_WQ_TYPES; t++) {
		if (vals[t] != 0) {
			wq->max[t] = MIN(vals[t], KQRING_WQ_MAX_WORKERS);
			if (wq->total[t] > wq->max[t])
				wakeup(&wq->ops[t]);
		}
		vals[t] = wq->max[t];
	}
	mtx_unlock(&wq->mtx);
	return (copyout(vals, ra->arg, sizeof(vals)));
}

/* IORING_REGISTER_PROBE: the opcodes implemented. */
static int
kqring_register_probe(const struct kqring_register_arg *ra)
{
	static const uint8_t supported_ops[] = {
		IORING_OP_NOP,
		IORING_OP_READV,
		IORING_OP_WRITEV,
		IORING_OP_FSYNC,
		IORING_OP_READ_FIXED,
		IORING_OP_WRITE_FIXED,
		IORING_OP_POLL_ADD,
		IORING_OP_POLL_REMOVE,
		IORING_OP_SYNC_FILE_RANGE,
		IORING_OP_SENDMSG,
		IORING_OP_RECVMSG,
		IORING_OP_TIMEOUT,
		IORING_OP_TIMEOUT_REMOVE,
		IORING_OP_ACCEPT,
		IORING_OP_ASYNC_CANCEL,
		IORING_OP_LINK_TIMEOUT,
		IORING_OP_CONNECT,
		IORING_OP_FALLOCATE,
		IORING_OP_OPENAT,
		IORING_OP_CLOSE,
		IORING_OP_FILES_UPDATE,
		IORING_OP_STATX,
		IORING_OP_READ,
		IORING_OP_WRITE,
		IORING_OP_FADVISE,
		IORING_OP_MADVISE,
		IORING_OP_SEND,
		IORING_OP_RECV,
		IORING_OP_OPENAT2,
		IORING_OP_PROVIDE_BUFFERS,
		IORING_OP_REMOVE_BUFFERS,
		IORING_OP_SHUTDOWN,
		IORING_OP_RENAMEAT,
		IORING_OP_UNLINKAT,
		IORING_OP_MKDIRAT,
		IORING_OP_SYMLINKAT,
		IORING_OP_LINKAT,
		IORING_OP_MSG_RING,
		IORING_OP_FSETXATTR,
		IORING_OP_SETXATTR,
		IORING_OP_FGETXATTR,
		IORING_OP_GETXATTR,
		IORING_OP_SOCKET,
		IORING_OP_URING_CMD,
		IORING_OP_SEND_ZC,
		IORING_OP_SENDMSG_ZC,
		IORING_OP_READ_MULTISHOT,
		IORING_OP_WAITID,
		IORING_OP_FUTEX_WAIT,
		IORING_OP_FUTEX_WAKE,
		IORING_OP_FUTEX_WAITV,
		IORING_OP_FIXED_FD_INSTALL,
		IORING_OP_FTRUNCATE,
		IORING_OP_BIND,
		IORING_OP_LISTEN,
		IORING_OP_READV_FIXED,
		IORING_OP_WRITEV_FIXED,
		IORING_OP_PIPE,
		IORING_OP_SPLICE,
		IORING_OP_TEE,
	};
	struct io_uring_probe hdr;
	int error;

	if (ra->arg == NULL)
		return (EINVAL);
	error = copyin(ra->arg, &hdr, sizeof(hdr));
	if (error != 0)
		return (error);

	/*
	 * As in io_uring: last_op is the last opcode this ABI knows and
	 * ops_len the number of entries filled in, at most IORING_OP_LAST
	 * (it is a uint8_t, so never 256).
	 */
	uint32_t max_ops = MIN(ra->nr_args, (uint32_t)IORING_OP_LAST);
	hdr.last_op = IORING_OP_LAST - 1;
	hdr.ops_len = (uint8_t)max_ops;
	error = copyout(&hdr, ra->arg, sizeof(hdr));
	if (error != 0)
		return (error);

	for (uint32_t i = 0; i < max_ops; i++) {
		struct io_uring_probe_op pop = {
			.op = (uint8_t)i,
			.resv = 0,
			.flags = 0,
			.resv2 = 0,
		};
		for (size_t j = 0; j < nitems(supported_ops); j++) {
			if (supported_ops[j] == i) {
				pop.flags = IO_URING_OP_SUPPORTED;
				break;
			}
		}
		uint8_t *dst = (uint8_t *)ra->arg + sizeof(hdr) +
		    i * sizeof(pop);
		error = copyout(&pop, dst, sizeof(pop));
		if (error != 0)
			return (error);
	}
	return (0);
}

/* Register opcodes that change the ring, run by kqring_ioc_register(). */
static int
kqring_register_op(struct kqring_ctx *ctx, const struct kqring_register_arg *ra,
    struct thread *td, int *resp)
{
	struct io_uring_rsrc_register rr;
	struct io_uring_rsrc_update2 up;
	struct io_uring_files_update fu;
	int32_t res;
	int error;

	switch (ra->opcode) {
	case IORING_UNREGISTER_FILES:
	case IORING_UNREGISTER_BUFFERS:
	case IORING_REGISTER_FILES_UPDATE:
	case IORING_REGISTER_FILES_UPDATE2:
	case IORING_REGISTER_BUFFERS_UPDATE:
	case IORING_REGISTER_CLONE_BUFFERS:
		/*
		 * Released slots post their tags' CQEs, which are never
		 * dropped: hold these back while many CQEs overflow, as
		 * submission is (KQRING_OCQ_BUSY).
		 */
		kqring_flush_overflow(ctx);
		if (ctx->nocq >= KQRING_OCQ_BUSY)
			return (EBUSY);
		break;
	}
	switch (ra->opcode) {
	case IORING_REGISTER_ENABLE_RINGS:
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		mtx_lock(&ctx->mtx);
		if (ctx->enabled) {
			mtx_unlock(&ctx->mtx);
			return (EINVAL);
		}
		kqring_set_owner(ctx, td);
		ctx->enabled = true;
		ctx->restricted = ctx->rst.registered;
		mtx_unlock(&ctx->mtx);
		if (kqring_has_kthread(ctx)) {
			error = kqring_sqpoll_start(ctx, td);
			if (error != 0) {
				mtx_lock(&ctx->mtx);
				ctx->enabled = false;
				kqring_clear_owner(ctx);
				mtx_unlock(&ctx->mtx);
				return (error);
			}
		}
		return (0);

	case IORING_REGISTER_RESTRICTIONS:
		return (kqring_restrictions_register(ctx, ra));

	case IORING_REGISTER_FILES:
		return (kqring_files_register(ctx, (uintptr_t)ra->arg, 0,
		    ra->nr_args, false));
	case IORING_REGISTER_BUFFERS:
		if (ra->arg == NULL)
			return (EFAULT);
		return (kqring_bufs_register(ctx, (uintptr_t)ra->arg, 0,
		    ra->nr_args, false));
	case IORING_REGISTER_FILES2:
	case IORING_REGISTER_BUFFERS2:
		if (ra->arg == NULL || ra->nr_args != sizeof(rr))
			return (EINVAL);
		error = copyin(ra->arg, &rr, sizeof(rr));
		if (error != 0)
			return (error);
		if (rr.nr == 0 || rr.resv2 != 0 ||
		    (rr.flags & ~IORING_RSRC_REGISTER_SPARSE) != 0)
			return (EINVAL);
		/* A sparse table starts empty: nothing to read. */
		if ((rr.flags & IORING_RSRC_REGISTER_SPARSE) != 0 &&
		    (rr.data != 0 || rr.tags != 0))
			return (EINVAL);
		if (ra->opcode == IORING_REGISTER_FILES2)
			return (kqring_files_register(ctx, rr.data, rr.tags,
			    rr.nr, (rr.flags & IORING_RSRC_REGISTER_SPARSE) !=
			    0));
		return (kqring_bufs_register(ctx, rr.data, rr.tags, rr.nr,
		    (rr.flags & IORING_RSRC_REGISTER_SPARSE) != 0));
	case IORING_UNREGISTER_FILES:
	case IORING_UNREGISTER_BUFFERS:
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		if (ra->opcode == IORING_UNREGISTER_FILES) {
			if (ctx->files == NULL)
				return (ENXIO);
			kqring_files_free(ctx);
		} else {
			if (ctx->bufs == NULL)
				return (ENXIO);
			kqring_bufs_free(ctx);
		}
		return (0);
	case IORING_REGISTER_FILES_UPDATE:
		if (ra->arg == NULL || ra->nr_args == 0)
			return (EINVAL);
		error = copyin(ra->arg, &fu, sizeof(fu));
		if (error != 0)
			return (error);
		if (fu.resv != 0)
			return (EINVAL);
		res = kqring_files_update(ctx, fu.offset, fu.fds, 0,
		    ra->nr_args);
		break;
	case IORING_REGISTER_FILES_UPDATE2:
	case IORING_REGISTER_BUFFERS_UPDATE:
		if (ra->arg == NULL || ra->nr_args != sizeof(up))
			return (EINVAL);
		error = copyin(ra->arg, &up, sizeof(up));
		if (error != 0)
			return (error);
		if (up.nr == 0 || up.resv != 0 || up.resv2 != 0)
			return (EINVAL);
		if (ra->opcode == IORING_REGISTER_FILES_UPDATE2)
			res = kqring_files_update(ctx, up.offset, up.data,
			    up.tags, up.nr);
		else
			res = kqring_bufs_update(ctx, up.offset, up.data,
			    up.tags, up.nr);
		break;
	case IORING_REGISTER_FILE_ALLOC_RANGE:
		return (kqring_file_alloc_range(ctx, ra));
	case IORING_REGISTER_CLONE_BUFFERS:
		return (kqring_bufs_clone(ctx, ra));

	case IORING_REGISTER_PBUF_RING:
		return (kqring_pbuf_register(ctx, ra));
	case IORING_UNREGISTER_PBUF_RING:
		return (kqring_pbuf_unregister(ctx, ra));
	case IORING_REGISTER_PBUF_STATUS:
		return (kqring_pbuf_status(ctx, ra));

	case IORING_REGISTER_EVENTFD:
	case IORING_REGISTER_EVENTFD_ASYNC:
		return (kqring_evfd_register(ctx, ra,
		    ra->opcode == IORING_REGISTER_EVENTFD_ASYNC));
	case IORING_UNREGISTER_EVENTFD:
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		return (kqring_evfd_unregister(ctx));

	case IORING_REGISTER_PERSONALITY:
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		return (kqring_personality_register(ctx, resp));
	case IORING_UNREGISTER_PERSONALITY:
		if (ra->arg != NULL)
			return (EINVAL);
		return (kqring_personality_unregister(ctx, ra->nr_args));

	case IORING_REGISTER_SYNC_CANCEL:
		return (kqring_sync_cancel(ctx, ra, resp));
	case IORING_REGISTER_CLOCK:
		return (kqring_register_clock(ctx, ra));

	case IORING_REGISTER_IOWQ_AFF:
		return (kqring_iowq_aff(ctx, ra, true));
	case IORING_UNREGISTER_IOWQ_AFF:
		return (kqring_iowq_aff(ctx, ra, false));
	case IORING_REGISTER_IOWQ_MAX_WORKERS:
		return (kqring_iowq_max_workers(ctx, ra));

	case IORING_REGISTER_RESIZE_RINGS:
		return (kqring_resize_rings(ctx, ra, td));
	case IORING_REGISTER_RING_FDS:
		return (kqring_ring_fds_register(td, ra, resp));
	case IORING_UNREGISTER_RING_FDS:
		return (kqring_ring_fds_unregister(td, ra, resp));

	case IORING_REGISTER_NAPI:
		return (kqring_register_napi(ctx, ra, true));
	case IORING_UNREGISTER_NAPI:
		return (kqring_register_napi(ctx, ra, false));

	default:
		return (EOPNOTSUPP);
	}
	if (res < 0)
		return (-res);
	*resp = res;
	return (0);
}

/*
 * KQRING_IOC_REGISTER.  Opcodes that change the ring run in its owner
 * thread, as SINGLE_ISSUER requires, which is what lets the owner use its
 * tables without locks; before IORING_REGISTER_ENABLE_RINGS (no owner yet)
 * any thread may, one at a time.  A positive result (personality id,
 * slots updated, requests cancelled) becomes ioctl(2)'s return value, as
 * io_uring_register(2)'s.
 */
int
kqring_ioc_register(struct kqring_ctx *ctx, struct kqring_register_arg *ra,
    struct thread *td, int *resp)
{
	bool busy = false;
	int error;

	if (!ctx->setup_done || ctx->closing)
		return (EBADF);
	if (ctx->restricted && (ra->opcode >= 256 ||
	    !kqring_bit_isset(ctx->rst.register_op, ra->opcode)))
		return (EACCES);

	switch (ra->opcode) {
	case IORING_REGISTER_PROBE:
		return (kqring_register_probe(ra));
	case IORING_REGISTER_SEND_MSG_RING:
		return (kqring_register_send_msg(ctx, ra));
	}

	mtx_lock(&ctx->mtx);
	if (ctx->closing) {
		mtx_unlock(&ctx->mtx);
		return (EBADF);
	}
	if (ctx->enabled) {
		if (!kqring_is_owner(ctx, td)) {
			mtx_unlock(&ctx->mtx);
			return (EEXIST);	/* SINGLE_ISSUER violation */
		}
	} else {
		if (ctx->reg_busy) {
			mtx_unlock(&ctx->mtx);
			return (EBUSY);
		}
		ctx->reg_busy = busy = true;
	}
	mtx_unlock(&ctx->mtx);

	sx_xlock(&ctx->sq_sx);
	if (ctx->closing) {
		sx_xunlock(&ctx->sq_sx);
		error = EBADF;
	} else {
		error = kqring_register_op(ctx, ra, td, resp);
		sx_xunlock(&ctx->sq_sx);
	}

	if (busy) {
		mtx_lock(&ctx->mtx);
		ctx->reg_busy = false;
		mtx_unlock(&ctx->mtx);
	}
	return (error);
}
