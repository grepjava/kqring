/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

#include <sys/sysent.h>

MALLOC_DEFINE(M_KQRING, "kqring", "FreeBSD kqring I/O ring state");

struct kqring_proc_list kqring_proc_hash[KQRING_PROC_HASH];
struct kqring_ctx_list_head kqring_ctx_list =
    LIST_HEAD_INITIALIZER(kqring_ctx_list);
struct mtx kqring_proc_mtx;
struct rmlock kqring_rings_rm;
uint32_t kqring_live_workers;
uint32_t kqring_live_zc;
bool kqring_unloading;
static eventhandler_tag kqring_exit_tag;
static eventhandler_tag kqring_exec_tag;
static eventhandler_tag kqring_fork_tag;

static struct cdev *kqring_cdev;

/* Forward declarations */
static d_open_t		kqring_open;
static d_ioctl_t	kqring_ioctl;
static d_mmap_single_t	kqring_mmap_single;
static d_poll_t		kqring_poll;
static d_kqfilter_t	kqring_kqfilter;

static struct cdevsw kqring_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	kqring_open,
	.d_ioctl =	kqring_ioctl,
	.d_mmap_single = kqring_mmap_single,
	.d_poll =	kqring_poll,
	.d_kqfilter =	kqring_kqfilter,
	.d_name =	"kqring",
};

/*
 * poll(2) on a ring: POLLIN while kqring_ring_readable(), POLLOUT while
 * the SQ has room, as io_uring_poll().
 */
static int
kqring_poll(struct cdev *dev __unused, int events, struct thread *td)
{
	struct kqring_ctx *ctx;
	int revents = 0;

	if (devfs_get_cdevpriv((void **)&ctx) != 0)
		return (events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM));
	mtx_lock(&ctx->mtx);
	if (ctx->shm == NULL || ctx->closing) {
		mtx_unlock(&ctx->mtx);
		return (POLLHUP);
	}
	if (kqring_ring_readable(ctx))
		revents |= events & (POLLIN | POLLRDNORM);
	if (atomic_load_acq_32(&ctx->shm->sq_tail) -
	    atomic_load_acq_32(&ctx->shm->sq_head) < ctx->sq_entries)
		revents |= events & (POLLOUT | POLLWRNORM);
	if (revents == 0 && (events & (POLLIN | POLLRDNORM)) != 0) {
		ctx->pollers = true;
		selrecord(td, &ctx->rsel);
	}
	mtx_unlock(&ctx->mtx);
	return (revents);
}

static void
kqring_kq_detach(struct knote *kn)
{
	struct kqring_ctx *ctx = kn->kn_hook;

	knlist_remove(&ctx->rsel.si_note, kn, 0);
}

/* Called with ctx->mtx (the knote list's lock) held. */
static int
kqring_kq_read(struct knote *kn, long hint __unused)
{
	struct kqring_ctx *ctx = kn->kn_hook;

	if (ctx->closing) {
		kn->kn_flags |= EV_EOF;
		return (1);
	}
	return (kqring_ring_readable(ctx));
}

static const struct filterops kqring_read_filtops = {
	.f_isfd = 1,
	.f_detach = kqring_kq_detach,
	.f_event = kqring_kq_read,
};

static int
kqring_kqfilter(struct cdev *dev __unused, struct knote *kn)
{
	struct kqring_ctx *ctx;

	if (devfs_get_cdevpriv((void **)&ctx) != 0)
		return (EBADF);
	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);
	kn->kn_fop = &kqring_read_filtops;
	kn->kn_hook = ctx;
	mtx_lock(&ctx->mtx);
	ctx->pollers = true;
	mtx_unlock(&ctx->mtx);
	knlist_add(&ctx->rsel.si_note, kn, 0);
	return (0);
}

/* Whether fp is a /dev/kqring descriptor. */
bool
kqring_fp_is_ring(const struct file *fp)
{
	return (fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
	    fp->f_vnode->v_type == VCHR && fp->f_vnode->v_rdev == kqring_cdev);
}

/* The ring context of a /dev/kqring file. */
int
kqring_fp_ctx(struct file *fp, struct kqring_ctx **ctxp)
{
	struct thread *td = curthread;
	struct file *saved;
	int error;

	if (!kqring_fp_is_ring(fp))
		return (EBADF);
	/* devfs_get_cdevpriv() looks at the file being operated on. */
	saved = td->td_fpop;
	td->td_fpop = fp;
	error = devfs_get_cdevpriv((void **)ctxp);
	td->td_fpop = saved;
	return (error != 0 ? EBADF : 0);
}

/* ============================================================================
 * Device File Operations (/dev/kqring)
 * ============================================================================
 */

static void
kqring_ctx_free_final(struct kqring_ctx *ctx)
{
	struct kqring_op *op;
	struct kqring_ocqe *ocqe;
	struct kqring_rcqe *rc;
	struct kqring_cred *kc;
	struct kqring_wq *wq;

	mtx_lock(&ctx->mtx);
	if (ctx->wq_abort_pending) {
		callout_stop(&ctx->wq_abort_callout);
		ctx->wq_abort_pending = false;
	}
	while ((op = TAILQ_FIRST(&ctx->retired_ops)) != NULL) {
		KASSERT(!op->wq_running,
		    ("kqring: freeing ctx with running retired worker"));
		TAILQ_REMOVE(&ctx->retired_ops, op, active_link);
		op->wq_retired = false;
		kqring_op_disarm_locked(op);
		mtx_unlock(&ctx->mtx);
		kqring_req_destroy(ctx, op);
		mtx_lock(&ctx->mtx);
	}
	wq = ctx->wq;
	ctx->wq = NULL;
	mtx_unlock(&ctx->mtx);
	callout_drain(&ctx->wq_abort_callout);
	kqring_wq_rele(wq);
	while ((op = TAILQ_FIRST(&ctx->ready_q)) != NULL) {
		TAILQ_REMOVE(&ctx->ready_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	while ((op = TAILQ_FIRST(&ctx->defer_q)) != NULL) {
		TAILQ_REMOVE(&ctx->defer_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}

	while ((op = TAILQ_FIRST(&ctx->free_ops)) != NULL) {
		TAILQ_REMOVE(&ctx->free_ops, op, active_link);
		free(op, M_KQRING);
	}

	/* Every request (and its references) is gone: drop the tables. */
	kqring_kpoll_close(ctx);
	kqring_files_free(ctx);
	kqring_bufs_free(ctx);
	for (int i = 0; i < KQRING_BGROUP_HASH; i++) {
		struct kqring_bgroup *bg;

		while ((bg = LIST_FIRST(&ctx->bgroups[i])) != NULL) {
			LIST_REMOVE(bg, link);
			kqring_bgroup_free(ctx, bg);
		}
	}
	while ((kc = LIST_FIRST(&ctx->creds)) != NULL) {
		LIST_REMOVE(kc, link);
		crfree(kc->cred);
		free(kc, M_KQRING);
	}
	if (ctx->evfd != NULL)
		eventfd_put(ctx->evfd);
	while ((ocqe = STAILQ_FIRST(&ctx->ocq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->ocq, link);
		free(ocqe, M_KQRING);
	}
	/* closing (set under mtx) stops MSG_RING senders from adding more. */
	while ((rc = STAILQ_FIRST(&ctx->rcq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->rcq, link);
		kqring_rcqe_free(rc);
	}

	/* Pollers of a closed ring: poll(2) saw POLLHUP, kevent(2) EV_EOF. */
	seldrain(&ctx->rsel);
	knlist_clear(&ctx->rsel.si_note, 0);
	knlist_destroy(&ctx->rsel.si_note);
	kqring_free_shm(ctx->ring_obj, ctx->ring_kva, ctx->ring_alloc_len);
	kqring_free_shm(ctx->sqes_obj, ctx->sqes_kva, ctx->sqes_alloc_len);
	sx_destroy(&ctx->sq_sx);
	sx_destroy(&ctx->bg_sx);
	mtx_destroy(&ctx->mtx);
	free(ctx, M_KQRING);
}

void
kqring_ctx_rele(struct kqring_ctx *ctx)
{
	if (refcount_release(&ctx->refs))
		kqring_ctx_free_final(ctx);
}

void
kqring_ctx_cancel_all(struct kqring_ctx *ctx)
{
	struct kqring_op *op;
	struct kqring_rcqe *rc;
	struct kqring_wq *wq;

	/*
	 * Signal closing and wait for the SQPOLL kthread to exit before
	 * taking sq_sx (since kqring_sqpoll_thread() acquires sq_sx in its
	 * main loop).  If called from the SQPOLL kthread itself (e.g. via
	 * fdrop() of an SCM_RIGHTS socket holding the last reference to
	 * this ring), defer teardown to the end of kqring_sqpoll_thread().
	 */
	mtx_lock(&ctx->mtx);
	ctx->closing = true;
	if (ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
	if (ctx->pollers) {
		selwakeuppri(&ctx->rsel, PZERO);
		KNOTE_LOCKED(&ctx->rsel.si_note, 0);
	}
	if (ctx->sqpoll_waiting)
		wakeup(&ctx->sqpoll_wait_chan);
	if (curthread == ctx->sqpoll_td) {
		mtx_unlock(&ctx->mtx);
		return;
	}
	while (ctx->sqpoll_running)
		msleep(&ctx->sqpoll_running, &ctx->mtx, 0, "kqsqdt", 0);
	kqring_kpoll_stop(ctx);
	if (ctx->cancelling) {
		if (ctx->cancel_td != curthread) {
			while (!ctx->cancelled)
				msleep(&ctx->cancelled, &ctx->mtx, 0,
				    "kqctxd", 0);
		}
		mtx_unlock(&ctx->mtx);
		return;
	}
	ctx->cancelling = true;
	ctx->cancel_td = curthread;
	mtx_unlock(&ctx->mtx);

	sx_xlock(&ctx->sq_sx);
	mtx_lock(&ctx->mtx);
	/* Disk requests own their user pages until their bios finish. */
	while (ctx->nbio > 0)
		msleep(&ctx->nbio, &ctx->mtx, 0, "kqbio", 0);
	while ((op = TAILQ_FIRST(&ctx->active_ops)) != NULL) {
		bool retired;

		kqring_op_unarm_locked(ctx, op);
		retired = op->wq_retired;
		mtx_unlock(&ctx->mtx);
		if (!retired)
			kqring_req_destroy(ctx, op);
		mtx_lock(&ctx->mtx);
	}
	for (;;) {
		TAILQ_FOREACH(op, &ctx->retired_ops, active_link) {
			if (!op->wq_running)
				break;
		}
		if (op == NULL)
			break;
		TAILQ_REMOVE(&ctx->retired_ops, op, active_link);
		op->wq_retired = false;
		if (TAILQ_EMPTY(&ctx->retired_ops) && ctx->wq_abort_pending) {
			callout_stop(&ctx->wq_abort_callout);
			ctx->wq_abort_pending = false;
		}
		kqring_op_disarm_locked(op);
		mtx_unlock(&ctx->mtx);
		kqring_req_destroy(ctx, op);
		mtx_lock(&ctx->mtx);
	}
	wq = ctx->wq;
	mtx_unlock(&ctx->mtx);
	kqring_wq_close(wq);
	/* No poll is armed and the poller is gone: close the kqueue. */
	kqring_kpoll_close(ctx);

	while ((op = TAILQ_FIRST(&ctx->ready_q)) != NULL) {
		TAILQ_REMOVE(&ctx->ready_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	while ((op = TAILQ_FIRST(&ctx->defer_q)) != NULL) {
		TAILQ_REMOVE(&ctx->defer_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	mtx_lock(&kqring_zc_mtx);
	mtx_lock(&ctx->mtx);
	while (!TAILQ_EMPTY(&ctx->zc_pending)) {
		struct kqring_zc_notif *zc = TAILQ_FIRST(&ctx->zc_pending);

		TAILQ_REMOVE(&ctx->zc_pending, zc, link);
		zc->on_ctx = false;
		zc->ctx = NULL;
	}
	while ((rc = STAILQ_FIRST(&ctx->rcq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->rcq, link);
		ctx->nrcq--;
		mtx_unlock(&ctx->mtx);
		mtx_unlock(&kqring_zc_mtx);
		kqring_rcqe_free(rc);
		mtx_lock(&kqring_zc_mtx);
		mtx_lock(&ctx->mtx);
	}
	mtx_unlock(&ctx->mtx);
	mtx_unlock(&kqring_zc_mtx);
	/*
	 * Drop fixed-file and registered-buffer tables on process exit/exec
	 * as well as ring close, breaking any AF_UNIX SCM_RIGHTS cycle where
	 * a ring's own fd is in flight on a socket held in its fixed table.
	 */
	kqring_files_free(ctx);
	kqring_bufs_free(ctx);
	sx_xunlock(&ctx->sq_sx);

	mtx_lock(&ctx->mtx);
	ctx->cancelled = true;
	ctx->cancel_td = NULL;
	wakeup(&ctx->cancelled);
	mtx_unlock(&ctx->mtx);
}

static void
kqring_dtor(void *data)
{
	struct kqring_ctx *ctx = data;

	mtx_lock(&kqring_proc_mtx);
	if (ctx->in_ctx_list) {
		LIST_REMOVE(ctx, proc_link);
		ctx->in_ctx_list = false;
	}
	mtx_unlock(&kqring_proc_mtx);

	kqring_ctx_cancel_all(ctx);
	kqring_ctx_rele(ctx);
}

static int
kqring_open(struct cdev *dev __unused, int oflags __unused, int devtype __unused,
    struct thread *td)
{
	struct kqring_ctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_KQRING, M_WAITOK | M_ZERO);
	refcount_init(&ctx->refs, 1);
	mtx_init(&ctx->mtx, "kqring", NULL, MTX_DEF);
	knlist_init_mtx(&ctx->rsel.si_note, &ctx->mtx);
	callout_init_mtx(&ctx->wq_abort_callout, &ctx->mtx, 0);
	sx_init(&ctx->bg_sx, "kqring bg");
	sx_init(&ctx->sq_sx, "kqring sq");
	ctx->wq = kqring_wq_alloc(td);
	TAILQ_INIT(&ctx->retired_ops);
	TAILQ_INIT(&ctx->active_ops);
	TAILQ_INIT(&ctx->taskwork_ops);
	TAILQ_INIT(&ctx->free_ops);
	TAILQ_INIT(&ctx->ready_q);
	TAILQ_INIT(&ctx->defer_q);
	TAILQ_INIT(&ctx->zc_pending);
	for (int i = 0; i < KQRING_KP_HASH; i++)
		TAILQ_INIT(&ctx->kp_ops[i]);
	for (int i = 0; i < KQRING_POLL_HASH; i++)
		LIST_INIT(&ctx->poll_hash[i]);
	STAILQ_INIT(&ctx->ocq);
	STAILQ_INIT(&ctx->rcq);
	for (int i = 0; i < KQRING_BGROUP_HASH; i++)
		LIST_INIT(&ctx->bgroups[i]);
	LIST_INIT(&ctx->creds);
	ctx->clockid = CLOCK_MONOTONIC;
	ctx->napi_track = IO_URING_NAPI_TRACKING_INACTIVE;

	error = devfs_set_cdevpriv(ctx, kqring_dtor);
	if (error != 0) {
		kqring_wq_put(ctx->wq);
		callout_drain(&ctx->wq_abort_callout);
		knlist_destroy(&ctx->rsel.si_note);
		sx_destroy(&ctx->sq_sx);
		sx_destroy(&ctx->bg_sx);
		mtx_destroy(&ctx->mtx);
		free(ctx, M_KQRING);
		return (error);
	}
	mtx_lock(&kqring_proc_mtx);
	ctx->in_ctx_list = true;
	LIST_INSERT_HEAD(&kqring_ctx_list, ctx, proc_link);
	mtx_unlock(&kqring_proc_mtx);
	return (0);
}

static uint32_t
roundup_pow2(uint32_t v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return (v + 1);
}

static int
kqring_setup_rings(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	struct io_uring_params p;
	struct kqring_wq *attach_wq = NULL;
	bool wq_swapped = false;
	uint32_t sq_entries, cq_entries;
	size_t ring_bytes, sqes_bytes, array_off, cqes_off;
	size_t cqe_shift, sqe_shift;
	int error;

	error = copyin(uarg->params, &p, sizeof(p));
	if (error != 0)
		return (error);

	/*
	 * Reject every flag kqring does not implement, as io_uring_setup(2)
	 * does, so that callers probing for a feature see EINVAL rather than a
	 * silently different ring.  Completions always run as DEFER_TASKRUN
	 * describes, in an enter (liburing enters when IORING_SQ_TASKRUN is
	 * set).  Without SINGLE_ISSUER, any thread of the process may submit
	 * and wait (ctx->shared); DEFER_TASKRUN needs SINGLE_ISSUER, as in
	 * io_uring.  COOP_TASKRUN and TASKRUN_FLAG describe what kqring
	 * always does; without SUBMIT_ALL, submission stops at an SQE that
	 * fails its checks (kqring_submit_sqes()).
	 */
	if ((p.flags & ~KQRING_SETUP_FLAGS) != 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_DEFER_TASKRUN) != 0 &&
	    (p.flags & IORING_SETUP_SINGLE_ISSUER) == 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_SQ_AFF) != 0 &&
	    (p.flags & IORING_SETUP_SQPOLL) == 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_SQ_AFF) != 0) {
		if (p.sq_thread_cpu >= CPU_SETSIZE ||
		    CPU_ABSENT((int)p.sq_thread_cpu) ||
		    (td->td_cpuset != NULL &&
		    !CPU_ISSET((int)p.sq_thread_cpu, &td->td_cpuset->cs_mask)))
			return (EINVAL);
	}
	for (size_t i = 0; i < nitems(p.resv); i++)
		if (p.resv[i] != 0)
			return (EINVAL);

	sq_entries = uarg->entries;
	if (sq_entries == 0)
		return (EINVAL);
	if (sq_entries > KQRING_MAX_SQ_ENTRIES) {
		if ((p.flags & IORING_SETUP_CLAMP) == 0)
			return (EINVAL);
		sq_entries = KQRING_MAX_SQ_ENTRIES;
	}
	sq_entries = roundup_pow2(sq_entries);
	if (p.flags & IORING_SETUP_CQSIZE) {
		if (p.cq_entries == 0)
			return (EINVAL);
		if (p.cq_entries > KQRING_MAX_CQ_ENTRIES) {
			if ((p.flags & IORING_SETUP_CLAMP) == 0)
				return (EINVAL);
			p.cq_entries = KQRING_MAX_CQ_ENTRIES;
		}
		if (p.cq_entries < sq_entries)
			return (EINVAL);
		cq_entries = roundup_pow2(p.cq_entries);
	} else {
		cq_entries = sq_entries * 2;
	}

	if ((p.flags & IORING_SETUP_ATTACH_WQ) != 0) {
		struct kqring_ctx *wctx;
		struct file *wfp;
		cap_rights_t rights;

		if (fget(td, (int)p.wq_fd,
		    cap_rights_init_one(&rights, CAP_IOCTL), &wfp) != 0)
			return (EINVAL);
		if (kqring_fp_ctx(wfp, &wctx) != 0 || wctx == ctx) {
			fdrop(wfp, td);
			return (EINVAL);
		}
		mtx_lock(&wctx->mtx);
		if (!wctx->setup_done || wctx->closing || wctx->wq == NULL ||
		    wctx->wq->proc != td->td_proc) {
			mtx_unlock(&wctx->mtx);
			fdrop(wfp, td);
			return (EINVAL);
		}
		attach_wq = wctx->wq;
		mtx_lock(&attach_wq->mtx);
		if (attach_wq->closing) {
			mtx_unlock(&attach_wq->mtx);
			mtx_unlock(&wctx->mtx);
			fdrop(wfp, td);
			return (EINVAL);
		}
		attach_wq->refs++;
		attach_wq->ring_refs++;
		mtx_unlock(&attach_wq->mtx);
		mtx_unlock(&wctx->mtx);
		fdrop(wfp, td);
	}

	cqe_shift = (p.flags & IORING_SETUP_CQE32) != 0 ? 1 : 0;
	sqe_shift = (p.flags & IORING_SETUP_SQE128) != 0 ? 1 : 0;
	array_off = sizeof(struct kqring_shm_header);
	cqes_off = array_off + sq_entries * sizeof(uint32_t);
	ring_bytes = cqes_off +
	    ((size_t)cq_entries * sizeof(struct io_uring_cqe) << cqe_shift);
	sqes_bytes = (size_t)sq_entries * sizeof(struct io_uring_sqe) <<
	    sqe_shift;

	error = kqring_alloc_shm(ring_bytes, &ctx->ring_obj, &ctx->ring_kva,
	    &ctx->ring_alloc_len);
	if (error != 0) {
		kqring_wq_put(attach_wq);
		return (error);
	}

	error = kqring_alloc_shm(sqes_bytes, &ctx->sqes_obj, &ctx->sqes_kva,
	    &ctx->sqes_alloc_len);
	if (error != 0) {
		kqring_free_shm(ctx->ring_obj, ctx->ring_kva,
		    ctx->ring_alloc_len);
		ctx->ring_obj = NULL;
		kqring_wq_put(attach_wq);
		return (error);
	}

	ctx->shm = (struct kqring_shm_header *)ctx->ring_kva;
	ctx->sq_array = (uint32_t *)(ctx->ring_kva + array_off);
	ctx->cqes = (struct io_uring_cqe *)(ctx->ring_kva + cqes_off);
	ctx->sqes = (struct io_uring_sqe *)ctx->sqes_kva;

	ctx->sq_entries = sq_entries;
	ctx->cq_entries = cq_entries;
	ctx->sq_mask = sq_entries - 1;
	ctx->cq_mask = cq_entries - 1;
	ctx->flags = p.flags;
	ctx->shared = (p.flags & IORING_SETUP_SINGLE_ISSUER) == 0;
	ctx->taskrun_only = (p.flags & (IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_SQPOLL)) == 0;
	if ((p.flags & IORING_SETUP_SQPOLL) != 0) {
		uint32_t idle_ms = p.sq_thread_idle != 0 ? p.sq_thread_idle :
		    1000;

		if (idle_ms > 3600000U)
			idle_ms = 3600000U;
		ctx->sqpoll_idle_sbt = (sbintime_t)idle_ms * SBT_1MS;
		ctx->sqpoll_cpu = p.sq_thread_cpu;
	}
	ctx->enabled = (p.flags & IORING_SETUP_R_DISABLED) == 0;
	if (ctx->enabled)
		kqring_set_owner(ctx, td);

	ctx->shm->sq_ring_mask = ctx->sq_mask;
	ctx->shm->sq_ring_entries = sq_entries;
	ctx->shm->cq_ring_mask = ctx->cq_mask;
	ctx->shm->cq_ring_entries = cq_entries;

	p.sq_entries = sq_entries;
	p.cq_entries = cq_entries;
	p.features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP |
	    IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_RW_CUR_POS |
	    IORING_FEAT_EXT_ARG | IORING_FEAT_FAST_POLL |
	    IORING_FEAT_POLL_32BITS | IORING_FEAT_SQPOLL_NONFIXED |
	    IORING_FEAT_CQE_SKIP | IORING_FEAT_LINKED_FILE |
	    IORING_FEAT_NATIVE_WORKERS | IORING_FEAT_CUR_PERSONALITY |
	    IORING_FEAT_RSRC_TAGS | IORING_FEAT_REG_REG_RING;

	p.sq_off.head = offsetof(struct kqring_shm_header, sq_head);
	p.sq_off.tail = offsetof(struct kqring_shm_header, sq_tail);
	p.sq_off.ring_mask = offsetof(struct kqring_shm_header, sq_ring_mask);
	p.sq_off.ring_entries = offsetof(struct kqring_shm_header,
	    sq_ring_entries);
	p.sq_off.flags = offsetof(struct kqring_shm_header, sq_flags);
	p.sq_off.dropped = offsetof(struct kqring_shm_header, sq_dropped);
	p.sq_off.array = (uint32_t)array_off;

	p.cq_off.head = offsetof(struct kqring_shm_header, cq_head);
	p.cq_off.tail = offsetof(struct kqring_shm_header, cq_tail);
	p.cq_off.ring_mask = offsetof(struct kqring_shm_header, cq_ring_mask);
	p.cq_off.ring_entries = offsetof(struct kqring_shm_header,
	    cq_ring_entries);
	p.cq_off.overflow = offsetof(struct kqring_shm_header, cq_overflow);
	p.cq_off.cqes = (uint32_t)cqes_off;
	p.cq_off.flags = offsetof(struct kqring_shm_header, cq_flags);

	error = copyout(&p, uarg->params, sizeof(p));
	/*
	 * ATTACH_WQ: install the shared pool only once nothing else can fail
	 * but SQPOLL start; attach_wq then holds the ring's own pool, which
	 * the error path swaps back (only if it was swapped) and either path
	 * releases.
	 */
	if (error == 0 && attach_wq != NULL) {
		struct kqring_wq *old_wq;

		mtx_lock(&ctx->mtx);
		old_wq = ctx->wq;
		ctx->wq = attach_wq;
		mtx_unlock(&ctx->mtx);
		attach_wq = old_wq;
		wq_swapped = true;
	}
	if (error == 0 && ctx->enabled && kqring_has_kthread(ctx))
		error = kqring_sqpoll_start(ctx, td);
	if (error != 0) {
		if (wq_swapped) {
			struct kqring_wq *cur_wq;

			mtx_lock(&ctx->mtx);
			cur_wq = ctx->wq;
			ctx->wq = attach_wq;
			mtx_unlock(&ctx->mtx);
			attach_wq = cur_wq;
		}
		/* Undo, so that a retried setup does not leak the rings. */
		kqring_free_shm(ctx->ring_obj, ctx->ring_kva,
		    ctx->ring_alloc_len);
		kqring_free_shm(ctx->sqes_obj, ctx->sqes_kva,
		    ctx->sqes_alloc_len);
		ctx->ring_obj = ctx->sqes_obj = NULL;
		ctx->shm = NULL;
		ctx->sq_array = NULL;
		ctx->cqes = NULL;
		ctx->sqes = NULL;
		ctx->enabled = false;
		kqring_clear_owner(ctx);
		kqring_wq_put(attach_wq);
		return (error);
	}
	kqring_wq_put(attach_wq);
	return (0);
}

static int
kqring_ioc_setup(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	int error;

	/* Serialize against a concurrent setup or teardown on the descriptor. */
	sx_xlock(&ctx->sq_sx);
	mtx_lock(&ctx->mtx);
	if (ctx->closing) {
		mtx_unlock(&ctx->mtx);
		sx_xunlock(&ctx->sq_sx);
		return (EBADF);
	}
	if (ctx->setup_done || ctx->setup_busy) {
		mtx_unlock(&ctx->mtx);
		sx_xunlock(&ctx->sq_sx);
		return (EINVAL);
	}
	ctx->setup_busy = true;
	mtx_unlock(&ctx->mtx);

	error = kqring_setup_rings(ctx, uarg, td);

	mtx_lock(&ctx->mtx);
	ctx->setup_busy = false;
	if (error == 0)
		ctx->setup_done = true;
	mtx_unlock(&ctx->mtx);
	sx_xunlock(&ctx->sq_sx);
	return (error);
}

/*
 * Queue polled ops (armed SO_SND on AF_UNIX streams, which get no upcall)
 * whose socket has become writable.  Called with ctx->mtx held, so the
 * socket buffer is read without its lock: a stale value only causes a
 * spurious EAGAIN attempt or a delay until the next 50 us tick.
 */
void
kqring_poll_fallback_locked(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	mtx_assert(&ctx->mtx, MA_OWNED);
	TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
		struct socket *so = op->so;

		/* A listen(2) since arming reused so_snd; see kqring_sb_disarm(). */
		if (!op->polled || op->in_taskwork || op->cancelled ||
		    so == NULL || SOLISTENING(so))
			continue;
		if (sbspace(&so->so_snd) >= MAX(so->so_snd.sb_lowat, 1) ||
		    (so->so_snd.sb_state & SBS_CANTSENDMORE) != 0 ||
		    so->so_error != 0)
			kqring_taskwork_add_locked(ctx, op);
	}
}


/* Complete what became ready without an upcall, and issue successors. */
void
kqring_run_checks(struct kqring_ctx *ctx)
{
	/* Each is a no-op without its kind of request: skip the calls. */
	if (ctx->nfutex != 0)
		kqring_check_futexes(ctx);
	if (ctx->nwaitid != 0)
		kqring_check_waitids(ctx);
	if (ctx->nseqto != 0)
		kqring_check_timeouts(ctx);
	if (!TAILQ_EMPTY(&ctx->ready_q) || !TAILQ_EMPTY(&ctx->defer_q))
		kqring_flush_ready(ctx);
}

/*
 * Whether enter takes sq_sx.  It serializes the issuer with the SQPOLL
 * kthread, the threads sharing a ring without SINGLE_ISSUER, and teardown
 * in other threads.  Without SQPOLL, teardown
 * can only run concurrently when the process whose worker pool the ring
 * uses (IORING_SETUP_ATTACH_WQ, or a ring set up elsewhere) exits or
 * execs: close(2) waits for the enter's file reference, and the issuer's
 * own process exits or execs only once the issuer has left the kernel.
 */
static inline bool
kqring_enter_locks(const struct kqring_ctx *ctx, const struct thread *td)
{
	return (kqring_has_kthread(ctx) || ctx->shared ||
	    (ctx->wq != NULL && ctx->wq->proc != td->td_proc));
}

static inline void
kqring_enter_lock(struct kqring_ctx *ctx, bool lock)
{
	if (lock)
		sx_xlock(&ctx->sq_sx);
}

static inline void
kqring_enter_unlock(struct kqring_ctx *ctx, bool lock)
{
	if (lock)
		sx_xunlock(&ctx->sq_sx);
}

static int kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, bool lock);

static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t submitted = 0;
	sbintime_t sbt = 0;
	bool has_timeout = false;
	bool sqpoll = (ctx->flags & IORING_SETUP_SQPOLL) != 0;
	bool iopoll = (ctx->flags & IORING_SETUP_IOPOLL) != 0;
	bool lock = kqring_enter_locks(ctx, td);
	bool wait;
	int error = 0;

	if (!ctx->setup_done || !ctx->enabled || ctx->closing)
		return (EBADF);
	/*
	 * SINGLE_ISSUER: only the owner submits.  With SQPOLL the kthread is
	 * the issuer, so any thread or process may wake it and wait, as in
	 * io_uring.
	 */
	if (!sqpoll && !kqring_is_owner(ctx, td))
		return (EEXIST);

	/* SQ_WAKEUP/SQ_WAIT need SQPOLL. */
	if ((ea->flags & ~KQRING_ENTER_FLAGS) != 0)
		return (EINVAL);
	if (!sqpoll && (ea->flags & (IORING_ENTER_SQ_WAKEUP |
	    IORING_ENTER_SQ_WAIT)) != 0)
		return (EINVAL);
	/* Without EXT_ARG, io_uring_enter(2)'s arg is a sigmask. */
	if ((ea->flags & IORING_ENTER_EXT_ARG) == 0 && ea->arg != NULL)
		return (EINVAL);

	wait = (ea->flags & IORING_ENTER_GETEVENTS) != 0 &&
	    (ea->min_complete != 0 || iopoll);

	/*
	 * 1. Validate the optional IORING_ENTER_EXT_ARG before consuming
	 * any SQE, so that a bad argument fails the call without losing
	 * submissions.
	 */
	if ((ea->flags & IORING_ENTER_EXT_ARG) != 0 && ea->arg != NULL) {
		struct io_uring_getevents_arg ext;
		if (ea->argsz != sizeof(ext))
			return (EINVAL);
		error = copyin(ea->arg, &ext, sizeof(ext));
		if (error != 0)
			return (error);
		/*
		 * Signal masks are not implemented, and pad is min_wait_usec
		 * on newer Linux: reject both rather than ignore them.
		 */
		if (ext.sigmask != 0 || ext.pad != 0)
			return (EINVAL);
		if (wait && ext.ts != 0) {
			struct timespec ts;
			error = kqring_copyin_ts(ext.ts, &ts);
			if (error != 0)
				return (error);
			sbt = kqring_ts_to_sbt(&ts);
			if (sbt == 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0))
				sbt = 1;
			has_timeout = true;
		}
	}

	if (sqpoll) {
		/* The kthread submits; report to_submit, as io_uring does. */
		ea->SUBMITTED_out = ea->to_submit;
		if ((ea->flags & IORING_ENTER_SQ_WAKEUP) != 0) {
			mtx_lock(&ctx->mtx);
			atomic_clear_32(&shm->sq_flags, IORING_SQ_NEED_WAKEUP);
			if (ctx->sleeping)
				wakeup(&ctx->wakeup_chan);
			mtx_unlock(&ctx->mtx);
		}
		if ((ea->flags & IORING_ENTER_SQ_WAIT) != 0) {
			mtx_lock(&ctx->mtx);
			while ((atomic_load_acq_32(&shm->sq_tail) -
			    atomic_load_acq_32(&shm->sq_head)) >=
			    ctx->sq_entries && !ctx->closing) {
				if (ctx->sleeping)
					wakeup(&ctx->wakeup_chan);
				ctx->sqpoll_waiting = true;
				error = msleep_sbt(&ctx->sqpoll_wait_chan,
				    &ctx->mtx, PCATCH, "kqsqwt", SBT_1MS, 0,
				    0);
				ctx->sqpoll_waiting = false;
				if (error == EINTR || error == ERESTART) {
					mtx_unlock(&ctx->mtx);
					return (EINTR);
				}
			}
			mtx_unlock(&ctx->mtx);
		}
		if ((ea->flags & IORING_ENTER_GETEVENTS) != 0 &&
		    (atomic_load_32(&shm->sq_flags) &
		    IORING_SQ_CQ_OVERFLOW) != 0) {
			sx_xlock(&ctx->sq_sx);
			kqring_flush_overflow(ctx);
			sx_xunlock(&ctx->sq_sx);
		}
		if (!wait || ea->min_complete == 0)
			return (0);
		return (kqring_sqpoll_wait_cqes(ctx, ea->min_complete,
		    has_timeout, sbt));
	}

	/*
	 * 2. Drain and submit queued SQEs.
	 */
	kqring_enter_lock(ctx, lock);
	if (ctx->closing) {
		kqring_enter_unlock(ctx, lock);
		return (EBADF);
	}
	kqring_submit_sqes(ctx, ea->to_submit, &submitted);
	ea->SUBMITTED_out = submitted;

	/* 3. Run pending DEFER_TASKRUN completions, futex and timeout checks */
	kqring_run_taskwork(ctx);
	kqring_run_checks(ctx);
	kqring_flush_overflow(ctx);
	kqring_enter_unlock(ctx, lock);

	if (!wait) {
		kqring_evfd_flush(ctx);
		return (0);
	}

	/*
	 * 4. Wait.  As with io_uring_enter(2), once SQEs have been consumed
	 * the call reports success (the submitted count) even if the wait
	 * then times out or is interrupted: an ioctl error would make
	 * sys_ioctl() skip copying SUBMITTED_out back to userspace.
	 */
	error = kqring_wait_cqes(ctx, ea->min_complete, has_timeout, sbt,
	    lock);
	kqring_evfd_flush(ctx);
	if (error != 0 && submitted > 0)
		error = 0;
	return (error);
}

/*
 * Wait until the CQ holds min_complete entries (or overflowed), or until the
 * timeout expires or a signal arrives.
 */
static int
kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, bool lock)
{
	struct kqring_shm_header *shm = ctx->shm;
	bool iopoll = (ctx->flags & IORING_SETUP_IOPOLL) != 0;
	bool may_poll = true;
	sbintime_t spin = ctx->busy_poll_sbt;
	uint32_t timeouts = atomic_load_acq_32(&ctx->cq_timeouts);
	int error;

	for (;;) {
		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		/*
		 * A full CQ with overflow pending must be reaped first.  A
		 * TIMEOUT completing also ends the wait, as in io_uring.
		 */
		if (cq_count >= min_complete || !STAILQ_EMPTY(&ctx->ocq) ||
		    atomic_load_acq_32(&ctx->cq_timeouts) != timeouts)
			return (0);

		/* Timed out with CQEs (if fewer): success, as io_uring. */
		if (has_timeout && sbt == 0)
			return (cq_count > 0 ? 0 : ETIMEDOUT);

		/*
		 * IORING_REGISTER_NAPI: busy-wait (once per wait) for work
		 * that arrives within busy_poll_to, rather than sleep and be
		 * woken.  The queues are peeked at without ctx->mtx; the
		 * locked checks below still decide whether to sleep.
		 */
		if (spin != 0) {
			sbintime_t start = getsbinuptime(), now = start;

			if (has_timeout && spin > sbt)
				spin = sbt;
			while (now - start < spin) {
				if (atomic_load_acq_32(&shm->cq_tail) -
				    atomic_load_acq_32(&shm->cq_head) >=
				    min_complete ||
				    !TAILQ_EMPTY(&ctx->taskwork_ops) ||
				    !STAILQ_EMPTY(&ctx->rcq))
					break;
				cpu_spinwait();
				now = getsbinuptime();
			}
			spin = 0;
			if (has_timeout)
				sbt = MAX(sbt - (now - start), 0);
			if (!TAILQ_EMPTY(&ctx->taskwork_ops) ||
			    !STAILQ_EMPTY(&ctx->rcq))
				goto drain;
			continue;
		}

		/* Tell an eventfd watcher about CQEs before sleeping. */
		kqring_evfd_flush(ctx);
		mtx_lock(&ctx->mtx);
		/* Poll at most once per sleep so EAGAIN can't spin us. */
		if (ctx->npolled > 0 && may_poll) {
			kqring_poll_fallback_locked(ctx);
			may_poll = false;
		}
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if (!TAILQ_EMPTY(&ctx->taskwork_ops) ||
		    !STAILQ_EMPTY(&ctx->rcq)) {
			mtx_unlock(&ctx->mtx);
			goto drain;
		}
		/*
		 * A ring's kthread may have posted CQEs since the check above;
		 * it wakes waiters only under mtx, so look again here.
		 */
		if (atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head) >= min_complete ||
		    !STAILQ_EMPTY(&ctx->ocq) ||
		    atomic_load_acq_32(&ctx->cq_timeouts) != timeouts) {
			mtx_unlock(&ctx->mtx);
			return (0);
		}
		if (iopoll && ctx->nreqs == ctx->ndeferred) {
			mtx_unlock(&ctx->mtx);
			return (0);
		}
		/*
		 * If a futex or polled op is armed, wake periodically (50 us)
		 * to re-check it in addition to socket upcalls; a WAITID is
		 * re-polled every millisecond.
		 */
		sbintime_t sleep_sbt = sbt, period = 0;
		if (ctx->nfutex > 0 || ctx->npolled > 0 ||
		    !TAILQ_EMPTY(&ctx->retired_ops))
			period = 50 * SBT_1US;
		else if (ctx->nwaitid > 0)
			period = SBT_1MS;
		if (period != 0 && (!has_timeout || sleep_sbt > period))
			sleep_sbt = period;

		/* Timeout is sbintime_t, not ticks: use the _sbt variant. */
		ctx->sleeping++;
		/* As precise as the io_uring_enter(2) timeout (an hrtimer). */
		error = msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
		    "kqwait", sleep_sbt, 0, C_PREL(10));
		ctx->sleeping--;
		may_poll = true;
		mtx_unlock(&ctx->mtx);

		if (error == EINTR || error == ERESTART)
			return (EINTR);
		if (error == EWOULDBLOCK && has_timeout) {
			if (sbt <= sleep_sbt) {
				kqring_enter_lock(ctx, lock);
				if (!ctx->closing) {
					kqring_run_taskwork(ctx);
					kqring_run_checks(ctx);
					kqring_flush_overflow(ctx);
				}
				kqring_enter_unlock(ctx, lock);
				cq_count = atomic_load_acq_32(&shm->cq_tail) -
				    atomic_load_acq_32(&shm->cq_head);
				if (cq_count > 0 || !STAILQ_EMPTY(&ctx->ocq))
					return (0);
				return (ETIMEDOUT);
			}
			sbt -= sleep_sbt;
		}
drain:
		kqring_enter_lock(ctx, lock);
		if (ctx->closing) {
			kqring_enter_unlock(ctx, lock);
			return (EBADF);
		}
		kqring_run_taskwork(ctx);
		kqring_run_checks(ctx);
		kqring_flush_overflow(ctx);
		kqring_enter_unlock(ctx, lock);
	}
}


static struct kqring_proc_rings *
kqring_proc_find(const struct proc *p)
{
	struct kqring_proc_rings *pr;

	LIST_FOREACH(pr, kqring_proc_bucket(p), link) {
		if (pr->proc == p)
			return (pr);
	}
	return (NULL);
}

struct kqring_proc_rings *
kqring_proc_find_locked(const struct proc *p)
{
	mtx_assert(&kqring_proc_mtx, MA_OWNED);
	return (kqring_proc_find(p));
}

int
kqring_reg_ring_fget(struct thread *td, uint32_t slot, bool is_enter,
    struct file **fpp)
{
	struct kqring_proc_rings *pr;
	struct file *fp;

	struct rm_priotracker tracker;

	if (slot >= KQRING_MAX_REG_RINGS)
		return (EINVAL);
	rm_rlock(&kqring_rings_rm, &tracker);
	pr = kqring_proc_find(td->td_proc);
	if (pr == NULL) {
		rm_runlock(&kqring_rings_rm, &tracker);
		return (is_enter ? EINVAL : EBADF);
	}
	fp = pr->rings[slot];
	if (fp == NULL || !fhold(fp)) {
		rm_runlock(&kqring_rings_rm, &tracker);
		return (EBADF);
	}
	rm_runlock(&kqring_rings_rm, &tracker);
	*fpp = fp;
	return (0);
}

static void
kqring_proc_clear(struct proc *p)
{
	struct kqring_proc_rings *pr;

	mtx_lock(&kqring_proc_mtx);
	pr = kqring_proc_find_locked(p);
	if (pr != NULL) {
		rm_wlock(&kqring_rings_rm);
		LIST_REMOVE(pr, link);
		rm_wunlock(&kqring_rings_rm);
	}
	mtx_unlock(&kqring_proc_mtx);
	if (pr == NULL)
		return;
	for (uint32_t i = 0; i < KQRING_MAX_REG_RINGS; i++) {
		if (pr->rings[i] != NULL)
			fdrop(pr->rings[i], curthread);
	}
	free(pr, M_KQRING);
}

static void
kqring_proc_rings_teardown(struct proc *p)
{
	struct kqring_ctx *ctx;

	kqring_proc_clear(p);
	for (;;) {
		mtx_lock(&kqring_proc_mtx);
		LIST_FOREACH(ctx, &kqring_ctx_list, proc_link) {
			if ((ctx->owner_proc == p ||
			    (ctx->wq != NULL && ctx->wq->proc == p)) &&
			    !ctx->closing) {
				ctx->in_ctx_list = false;
				LIST_REMOVE(ctx, proc_link);
				refcount_acquire(&ctx->refs);
				break;
			}
		}
		mtx_unlock(&kqring_proc_mtx);
		if (ctx == NULL)
			break;
		mtx_lock(&ctx->mtx);
		if (ctx->owner_proc == p)
			kqring_clear_owner(ctx);
		mtx_unlock(&ctx->mtx);
		kqring_ctx_cancel_all(ctx);
		kqring_ctx_rele(ctx);
	}
}

static void
kqring_proc_exit(void *arg __unused, struct proc *p)
{
	kqring_proc_rings_teardown(p);
}

static void
kqring_proc_exec(void *arg __unused, struct proc *p,
    struct image_params *imgp __unused)
{
	kqring_proc_rings_teardown(p);
}

static void
kqring_proc_fork(void *arg __unused, struct proc *p1,
    struct proc *p2 __unused, int flags __unused)
{
	struct kqring_ctx *ctx;

	mtx_lock(&kqring_proc_mtx);
	LIST_FOREACH(ctx, &kqring_ctx_list, proc_link) {
		if ((ctx->owner_proc == p1 ||
		    (ctx->wq != NULL && ctx->wq->proc == p1)) &&
		    !ctx->closing)
			atomic_add_int(&ctx->cow_gen, 1);
	}
	mtx_unlock(&kqring_proc_mtx);
}


static int
kqring_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	struct kqring_ctx *ctx;
	struct file *rfp = NULL;
	int error, res = 0;

	error = devfs_get_cdevpriv((void **)&ctx);
	if (error != 0)
		return (error);

	switch (cmd) {
	case KQRING_IOC_SETUP:
		error = kqring_ioc_setup(ctx, (struct kqring_setup_arg *)data,
		    td);
		break;
	case KQRING_IOC_ENTER: {
		struct kqring_enter_arg *ea = (struct kqring_enter_arg *)data;

		if ((ea->flags & IORING_ENTER_REGISTERED_RING) != 0) {
			error = kqring_reg_ring_fget(td, (uint32_t)ea->ring_fd,
			    true, &rfp);
			if (error == 0)
				error = kqring_fp_ctx(rfp, &ctx);
			if (error != 0)
				break;
		}
		error = kqring_ioc_enter(ctx, ea, td);
		break;
	}
	case KQRING_IOC_REGISTER: {
		struct kqring_register_arg *ra =
		    (struct kqring_register_arg *)data;

		if ((ra->opcode & IORING_REGISTER_USE_REGISTERED_RING) != 0) {
			ra->opcode &= ~IORING_REGISTER_USE_REGISTERED_RING;
			error = kqring_reg_ring_fget(td, (uint32_t)ra->ring_fd,
			    false, &rfp);
			if (error == 0)
				error = kqring_fp_ctx(rfp, &ctx);
			if (error != 0)
				break;
		}
		error = kqring_ioc_register(ctx, ra, td, &res);
		break;
	}
	default:
		return (ENOTTY);
	}
	if (rfp != NULL)
		fdrop(rfp, td);
	/*
	 * kern_*() calls made for SQEs (openat, accept, socket...) leave
	 * their result in td_retval[0], which is ioctl(2)'s return value:
	 * only a register result may be returned.
	 */
	td->td_retval[0] = error == 0 ? res : 0;
	return (error);
}

/* ============================================================================
 * Native System Calls
 * ============================================================================
 *
 * kqring_setup(2), kqring_enter(2) and kqring_register(2) take the
 * arguments of io_uring_setup(2), io_uring_enter(2) and
 * io_uring_register(2) in registers.  They skip the devfs ioctl path
 * (cdev thread accounting, the argument copyin/copyout), which costs about
 * as much as a NOP enter itself.  They are registered in free sysent slots
 * at load; kern.kqring.sys_{setup,enter,register} give their numbers, or
 * -1 where only the ioctl interface exists.
 */

/* As sys/sysproto.h, which #undefs its own. */
#define	PAD_(t)	(sizeof(syscallarg_t) <= sizeof(t) ? \
		0 : sizeof(syscallarg_t) - sizeof(t))
#if BYTE_ORDER == LITTLE_ENDIAN
#define	PADL_(t)	0
#define	PADR_(t)	PAD_(t)
#else
#define	PADL_(t)	PAD_(t)
#define	PADR_(t)	0
#endif

struct kqring_setup_args {
	char entries_l_[PADL_(u_int)]; u_int entries; char entries_r_[PADR_(u_int)];
	char params_l_[PADL_(struct io_uring_params *)]; struct io_uring_params * params; char params_r_[PADR_(struct io_uring_params *)];
};
struct kqring_enter_args {
	char fd_l_[PADL_(int)]; int fd; char fd_r_[PADR_(int)];
	char to_submit_l_[PADL_(u_int)]; u_int to_submit; char to_submit_r_[PADR_(u_int)];
	char min_complete_l_[PADL_(u_int)]; u_int min_complete; char min_complete_r_[PADR_(u_int)];
	char flags_l_[PADL_(u_int)]; u_int flags; char flags_r_[PADR_(u_int)];
	char arg_l_[PADL_(void *)]; void * arg; char arg_r_[PADR_(void *)];
	char argsz_l_[PADL_(size_t)]; size_t argsz; char argsz_r_[PADR_(size_t)];
};
struct kqring_register_args {
	char fd_l_[PADL_(int)]; int fd; char fd_r_[PADR_(int)];
	char opcode_l_[PADL_(u_int)]; u_int opcode; char opcode_r_[PADR_(u_int)];
	char arg_l_[PADL_(void *)]; void * arg; char arg_r_[PADR_(void *)];
	char nr_args_l_[PADL_(u_int)]; u_int nr_args; char nr_args_r_[PADR_(u_int)];
};
#undef PAD_
#undef PADL_
#undef PADR_

/*
 * The ring a system call names: a /dev/kqring descriptor, or with
 * `registered` a slot of the IORING_REGISTER_RING_FDS table.
 */
static int
kqring_sys_ring(struct thread *td, int fd, bool registered, bool is_enter,
    struct file **fpp, struct kqring_ctx **ctxp)
{
	struct file *fp;
	cap_rights_t rights;
	int error;

	if (registered)
		error = kqring_reg_ring_fget(td, (uint32_t)fd, is_enter, &fp);
	else
		error = fget(td, fd, cap_rights_init_one(&rights, CAP_IOCTL),
		    &fp);
	if (error != 0)
		return (error);
	/* As io_uring_enter(2) on a descriptor that is not a ring. */
	error = kqring_fp_is_ring(fp) ? kqring_fp_ctx(fp, ctxp) : EOPNOTSUPP;
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}
	*fpp = fp;
	return (0);
}

static int
kqring_sys_setup(struct thread *td, struct kqring_setup_args *uap)
{
	struct kqring_setup_arg sa = {
		.entries = uap->entries,
		.params = uap->params,
	};
	struct kqring_ctx *ctx;
	struct file *fp;
	int error, fd;

	error = kern_openat(td, AT_FDCWD, "/dev/kqring", UIO_SYSSPACE,
	    O_RDWR | O_CLOEXEC, 0);
	if (error != 0)
		return (error);
	fd = (int)td->td_retval[0];
	error = kqring_sys_ring(td, fd, false, false, &fp, &ctx);
	if (error == 0) {
		error = kqring_ioc_setup(ctx, &sa, td);
		fdrop(fp, td);
	}
	if (error != 0) {
		(void)kern_close(td, fd);
		return (error);
	}
	td->td_retval[0] = fd;
	return (0);
}

static int
kqring_sys_enter(struct thread *td, struct kqring_enter_args *uap)
{
	struct kqring_enter_arg ea = {
		.to_submit = uap->to_submit,
		.min_complete = uap->min_complete,
		.flags = uap->flags,
		.arg = uap->arg,
		.argsz = uap->argsz,
		.ring_fd = uap->fd,
	};
	struct kqring_ctx *ctx;
	struct file *fp;
	int error;

	error = kqring_sys_ring(td, uap->fd,
	    (uap->flags & IORING_ENTER_REGISTERED_RING) != 0, true, &fp, &ctx);
	if (error != 0)
		return (error);
	error = kqring_ioc_enter(ctx, &ea, td);
	fdrop(fp, td);
	/* SQE ops' kern_*() calls leave their results in td_retval[0]. */
	td->td_retval[0] = error == 0 ? ea.SUBMITTED_out : 0;
	return (error);
}

static int
kqring_sys_register(struct thread *td, struct kqring_register_args *uap)
{
	struct kqring_register_arg ra = {
		.opcode = uap->opcode & ~IORING_REGISTER_USE_REGISTERED_RING,
		.nr_args = uap->nr_args,
		.arg = uap->arg,
		.ring_fd = uap->fd,
	};
	struct kqring_ctx *ctx;
	struct file *fp;
	int error, res = 0;

	error = kqring_sys_ring(td, uap->fd,
	    (uap->opcode & IORING_REGISTER_USE_REGISTERED_RING) != 0, false,
	    &fp, &ctx);
	if (error != 0)
		return (error);
	error = kqring_ioc_register(ctx, &ra, td, &res);
	fdrop(fp, td);
	td->td_retval[0] = error == 0 ? res : 0;
	return (error);
}

#define	KQRING_NSYS	3
#define	KQRING_AS(name)	(sizeof(struct name) / sizeof(syscallarg_t))

static struct sysent kqring_sysent[KQRING_NSYS] = {
	{
		.sy_narg = KQRING_AS(kqring_setup_args),
		.sy_call = (sy_call_t *)kqring_sys_setup,
		.sy_auevent = AUE_NULL,
	},
	{
		.sy_narg = KQRING_AS(kqring_enter_args),
		.sy_call = (sy_call_t *)kqring_sys_enter,
		.sy_auevent = AUE_NULL,
		.sy_flags = SYF_CAPENABLED,
	},
	{
		.sy_narg = KQRING_AS(kqring_register_args),
		.sy_call = (sy_call_t *)kqring_sys_register,
		.sy_auevent = AUE_NULL,
		.sy_flags = SYF_CAPENABLED,
	},
};
static struct sysent kqring_old_sysent[KQRING_NSYS];
static int kqring_sysnum[KQRING_NSYS] = { NO_SYSCALL, NO_SYSCALL, NO_SYSCALL };

SYSCTL_INT(_kern_kqring, OID_AUTO, sys_setup, CTLFLAG_RD,
    &kqring_sysnum[0], 0, "kqring_setup(2) system call number");
SYSCTL_INT(_kern_kqring, OID_AUTO, sys_enter, CTLFLAG_RD,
    &kqring_sysnum[1], 0, "kqring_enter(2) system call number");
SYSCTL_INT(_kern_kqring, OID_AUTO, sys_register, CTLFLAG_RD,
    &kqring_sysnum[2], 0, "kqring_register(2) system call number");

/* Without free slots, the ioctl interface still works: not an error. */
static void
kqring_sys_register_all(void)
{
	for (int i = 0; i < KQRING_NSYS; i++) {
		kqring_sysnum[i] = NO_SYSCALL;
		if (kern_syscall_register(sysent, &kqring_sysnum[i],
		    &kqring_sysent[i], &kqring_old_sysent[i], 0) != 0) {
			printf("kqring: no free system call slot\n");
			kqring_sysnum[i] = NO_SYSCALL;
		}
	}
}

/* Waits for the system calls in progress, as destroy_dev() for ioctls. */
static void
kqring_sys_deregister_all(void)
{
	for (int i = 0; i < KQRING_NSYS; i++) {
		if (kqring_sysnum[i] != NO_SYSCALL)
			(void)kern_syscall_deregister(sysent, kqring_sysnum[i],
			    &kqring_old_sysent[i]);
		kqring_sysnum[i] = NO_SYSCALL;
	}
}

static int
kqring_mmap_single(struct cdev *cdev __unused, vm_ooffset_t *offset,
    vm_size_t size, vm_object_t *object, int nprot)
{
	struct kqring_ctx *ctx;
	int error;

	if ((nprot & ~(VM_PROT_READ | VM_PROT_WRITE)) != 0)
		return (EINVAL);

	error = devfs_get_cdevpriv((void **)&ctx);
	if (error != 0 || !ctx->setup_done)
		return (EINVAL);

	if (*offset == IORING_OFF_SQ_RING || *offset == IORING_OFF_CQ_RING) {
		if (size > ctx->ring_alloc_len)
			return (EINVAL);
		vm_object_reference(ctx->ring_obj);
		*object = ctx->ring_obj;
		*offset = 0;
		return (0);
	}
	if (*offset == IORING_OFF_SQES) {
		if (size > ctx->sqes_alloc_len)
			return (EINVAL);
		vm_object_reference(ctx->sqes_obj);
		*object = ctx->sqes_obj;
		*offset = 0;
		return (0);
	}
	if ((*offset & IORING_OFF_MMAP_MASK) == IORING_OFF_PBUF_RING) {
		/* An IOU_PBUF_RING_MMAP ring, decoded as io_uring does. */
		uint16_t bgid = (uint16_t)((*offset & ~IORING_OFF_MMAP_MASK) >>
		    IORING_OFF_PBUF_SHIFT);
		struct kqring_bgroup *bg;

		error = EINVAL;
		mtx_lock(&ctx->mtx);
		bg = kqring_bgroup_find(ctx, bgid);
		if (bg != NULL && bg->ring_obj != NULL &&
		    size <= bg->ring_alloc_len) {
			/* The mapping's own reference outlives unregister. */
			vm_object_reference(bg->ring_obj);
			*object = bg->ring_obj;
			*offset = 0;
			error = 0;
		}
		mtx_unlock(&ctx->mtx);
		return (error);
	}
	return (EINVAL);
}

/* ============================================================================
 * Module Lifecycle
 * ============================================================================
 */

static int
kqring_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		mtx_init(&kqring_futex_mtx, "kqring futex", NULL, MTX_DEF);
		mtx_init(&kqring_proc_mtx, "kqring proc", NULL, MTX_DEF);
		rm_init(&kqring_rings_rm, "kqring rings");
		mtx_init(&kqring_zc_mtx, "kqring zc", NULL, MTX_DEF);
		for (int i = 0; i < KQRING_FUTEX_HASH; i++)
			LIST_INIT(&kqring_futex_hash[i]);
		for (int i = 0; i < KQRING_PROC_HASH; i++)
			LIST_INIT(&kqring_proc_hash[i]);
		kqring_exit_tag = EVENTHANDLER_REGISTER(process_exit,
		    kqring_proc_exit, NULL, EVENTHANDLER_PRI_ANY);
		kqring_exec_tag = EVENTHANDLER_REGISTER(process_exec,
		    kqring_proc_exec, NULL, EVENTHANDLER_PRI_ANY);
		kqring_fork_tag = EVENTHANDLER_REGISTER(process_fork,
		    kqring_proc_fork, NULL, EVENTHANDLER_PRI_ANY);
		kqring_cdev = make_dev(&kqring_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0666, "kqring");
		if (kqring_cdev == NULL) {
			EVENTHANDLER_DEREGISTER(process_fork, kqring_fork_tag);
			EVENTHANDLER_DEREGISTER(process_exec, kqring_exec_tag);
			EVENTHANDLER_DEREGISTER(process_exit, kqring_exit_tag);
			mtx_destroy(&kqring_zc_mtx);
			rm_destroy(&kqring_rings_rm);
			mtx_destroy(&kqring_proc_mtx);
			mtx_destroy(&kqring_futex_mtx);
			return (ENOMEM);
		}
		kqring_sys_register_all();
		printf("kqring: loaded (/dev/kqring, SINGLE_ISSUER | DEFER_TASKRUN)\n");
		return (0);

	case MOD_UNLOAD:
		/*
		 * Block new zero-copy admissions and wait briefly for any
		 * in-flight EXT_SFBUF mbufs before destroying /dev/kqring; if
		 * unread mbufs remain in a socket buffer, re-open admissions
		 * and refuse unload with EBUSY rather than hanging after
		 * /dev/kqring is gone.
		 */
		mtx_lock(&kqring_proc_mtx);
		kqring_unloading = true;
		while (kqring_live_zc > 0) {
			int error = msleep(&kqring_live_zc, &kqring_proc_mtx,
			    PCATCH, "kqzcun", MAX(hz / 10, 1));
			if (error != 0 && kqring_live_zc > 0) {
				kqring_unloading = false;
				mtx_unlock(&kqring_proc_mtx);
				return (EBUSY);
			}
		}
		mtx_unlock(&kqring_proc_mtx);
		/*
		 * The system calls go first, waiting for those in progress:
		 * after destroy_dev() their rings would be gone.
		 */
		kqring_sys_deregister_all();
		/*
		 * destroy_dev() runs the destructor of every ring still open,
		 * so no futex is hashed after it, and waits for ioctls in
		 * progress, so none is using the registered-ring table.
		 * EVENTHANDLER_DEREGISTER() waits for running exit/exec hooks.
		 * Only then is the table torn down.
		 */
		if (kqring_cdev != NULL)
			destroy_dev(kqring_cdev);
		EVENTHANDLER_DEREGISTER(process_fork, kqring_fork_tag);
		EVENTHANDLER_DEREGISTER(process_exec, kqring_exec_tag);
		EVENTHANDLER_DEREGISTER(process_exit, kqring_exit_tag);
		mtx_lock(&kqring_proc_mtx);
		while (kqring_live_workers > 0)
			msleep(&kqring_live_workers, &kqring_proc_mtx, 0,
			    "kqwqun", 0);
		mtx_unlock(&kqring_proc_mtx);
		for (int i = 0; i < KQRING_PROC_HASH; i++) {
			struct kqring_proc_rings *pr;

			for (;;) {
				mtx_lock(&kqring_proc_mtx);
				pr = LIST_FIRST(&kqring_proc_hash[i]);
				if (pr != NULL) {
					rm_wlock(&kqring_rings_rm);
					LIST_REMOVE(pr, link);
					rm_wunlock(&kqring_rings_rm);
				}
				mtx_unlock(&kqring_proc_mtx);
				if (pr == NULL)
					break;
				for (uint32_t s = 0; s < KQRING_MAX_REG_RINGS;
				    s++) {
					if (pr->rings[s] != NULL)
						fdrop(pr->rings[s], curthread);
				}
				free(pr, M_KQRING);
			}
		}
		mtx_destroy(&kqring_zc_mtx);
		rm_destroy(&kqring_rings_rm);
		mtx_destroy(&kqring_proc_mtx);
		mtx_destroy(&kqring_futex_mtx);
		printf("kqring: unloaded\n");
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(kqring, kqring_modevent, NULL);
MODULE_VERSION(kqring, 1);
