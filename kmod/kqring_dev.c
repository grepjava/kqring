/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

#include <sys/mman.h>
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
/* Ring files not yet closed; unload waits for none (under kqring_proc_mtx). */
static u_int kqring_nfiles;

/*
 * Opening /dev/kqring makes a ring file with kqring's own file operations
 * (d_fdopen), not devfs's.  Like io_uring files since Linux 6.7, it cannot
 * be passed over a UNIX socket (no DFLAG_PASSABLE: SCM_RIGHTS fails with
 * EOPNOTSUPP), so no reference cycle through a socket in its own fixed
 * table can form.  A child still inherits it across fork (DFLAG_FORK).
 */
static d_fdopen_t	kqring_fdopen;
static fo_ioctl_t	kqring_ioctl;
static fo_poll_t	kqring_poll;
static fo_kqfilter_t	kqring_kqfilter;
static fo_close_t	kqring_close;
static fo_mmap_t	kqring_mmap;
static fo_fork_t	kqring_fork;

static struct cdevsw kqring_cdevsw = {
	.d_version =	D_VERSION,
	.d_fdopen =	kqring_fdopen,
	.d_name =	"kqring",
};

static const struct fileops kqring_fileops = {
	.fo_read = invfo_rdwr,
	.fo_write = invfo_rdwr,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = kqring_ioctl,
	.fo_poll = kqring_poll,
	.fo_kqfilter = kqring_kqfilter,
	.fo_stat = vn_statfile,
	.fo_close = kqring_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_fill_kinfo = vn_fill_kinfo,
	.fo_mmap = kqring_mmap,
	.fo_cmp = file_kcmp_generic,
	.fo_fork = kqring_fork,
	.fo_flags = DFLAG_FORK,
};

/*
 * poll(2) on a ring: POLLIN while kqring_ring_readable(), POLLOUT while
 * the SQ has room, as io_uring_poll().
 */
static int
kqring_poll(struct file *fp, int events, struct ucred *active_cred __unused,
    struct thread *td)
{
	struct kqring_ctx *ctx = fp->f_data;
	int revents = 0;

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
kqring_kqfilter(struct file *fp, struct knote *kn)
{
	struct kqring_ctx *ctx = fp->f_data;

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

/* Whether fp is a ring file. */
bool
kqring_fp_is_ring(const struct file *fp)
{
	return (fp->f_ops == &kqring_fileops);
}

/* The ring context of a ring file. */
int
kqring_fp_ctx(struct file *fp, struct kqring_ctx **ctxp)
{
	if (!kqring_fp_is_ring(fp))
		return (EBADF);
	*ctxp = fp->f_data;
	return (0);
}

/* A forked child shares the parent's ring file, as on Linux. */
static int
kqring_fork(struct filedesc *fdp __unused, struct file *fp, struct file **fp1,
    struct proc *p1 __unused, struct thread *td __unused)
{
	/* Second pass, once every descriptor is in place: nothing to do. */
	if (fp == NULL)
		return (0);
	if (!fhold(fp))
		return (EBADF);
	*fp1 = fp;
	return (0);
}

/* ============================================================================
 * Device File Operations (/dev/kqring)
 * ============================================================================
 */

/* Free rings' memory, from kqring_rmem_alloc(). */
static void
kqring_rmem_free(struct kqring_rmem *m)
{
	if (m->ring_pages != NULL)
		kqring_unpin_shm(m->ring_kva, m->ring_len, m->ring_pages);
	else
		kqring_free_shm(m->ring_obj, m->ring_kva, m->ring_len);
	if (m->sqes_pages != NULL)
		kqring_unpin_shm(m->sqes_kva, m->sqes_len, m->sqes_pages);
	else
		kqring_free_shm(m->sqes_obj, m->sqes_kva, m->sqes_len);
	memset(m, 0, sizeof(*m));
}

/*
 * Memory for rings of ring_bytes and sqes_bytes: new wired objects or,
 * with IORING_SETUP_NO_MMAP, the caller's memory at cq_off.user_addr
 * (header, SQ array, CQEs) and sq_off.user_addr (SQEs), as io_uring
 * takes them.  Zeroed either way.
 */
static int
kqring_rmem_alloc(struct thread *td, const struct io_uring_params *p,
    size_t ring_bytes, size_t sqes_bytes, struct kqring_rmem *m)
{
	int error;

	memset(m, 0, sizeof(*m));
	if ((p->flags & IORING_SETUP_NO_MMAP) != 0) {
		error = kqring_pin_shm(td, p->cq_off.user_addr, ring_bytes,
		    &m->ring_kva, &m->ring_len, &m->ring_pages);
		if (error == 0)
			error = kqring_pin_shm(td, p->sq_off.user_addr,
			    sqes_bytes, &m->sqes_kva, &m->sqes_len,
			    &m->sqes_pages);
		if (error == 0)
			memset((void *)m->ring_kva, 0, ring_bytes);
	} else {
		error = kqring_alloc_shm(ring_bytes, &m->ring_obj,
		    &m->ring_kva, &m->ring_len);
		if (error == 0)
			error = kqring_alloc_shm(sqes_bytes, &m->sqes_obj,
			    &m->sqes_kva, &m->sqes_len);
	}
	if (error != 0)
		kqring_rmem_free(m);
	return (error);
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

/* SQ and CQ sizes for entries and p (CQSIZE, CLAMP), as io_uring sizes. */
static int
kqring_ring_entries(uint32_t entries, struct io_uring_params *p,
    uint32_t *sqp, uint32_t *cqp)
{
	uint32_t sq_entries = entries, cq_entries;

	if (sq_entries == 0)
		return (EINVAL);
	if (sq_entries > KQRING_MAX_SQ_ENTRIES) {
		if ((p->flags & IORING_SETUP_CLAMP) == 0)
			return (EINVAL);
		sq_entries = KQRING_MAX_SQ_ENTRIES;
	}
	sq_entries = roundup_pow2(sq_entries);
	if (p->flags & IORING_SETUP_CQSIZE) {
		if (p->cq_entries == 0)
			return (EINVAL);
		if (p->cq_entries > KQRING_MAX_CQ_ENTRIES) {
			if ((p->flags & IORING_SETUP_CLAMP) == 0)
				return (EINVAL);
			p->cq_entries = KQRING_MAX_CQ_ENTRIES;
		}
		if (p->cq_entries < sq_entries)
			return (EINVAL);
		cq_entries = roundup_pow2(p->cq_entries);
	} else {
		cq_entries = sq_entries * 2;
	}
	*sqp = sq_entries;
	*cqp = cq_entries;
	return (0);
}

/* Where the SQ array and CQEs go, and the two regions' sizes. */
struct kqring_layout {
	size_t	array_off;
	size_t	cqes_off;
	size_t	ring_bytes;
	size_t	sqes_bytes;
};

static void
kqring_ring_layout(uint32_t flags, uint32_t sq_entries, uint32_t cq_entries,
    struct kqring_layout *l)
{
	size_t cqe_shift = (flags & IORING_SETUP_CQE32) != 0 ? 1 : 0;
	size_t sqe_shift = (flags & IORING_SETUP_SQE128) != 0 ? 1 : 0;

	l->array_off = sizeof(struct kqring_shm_header);
	l->cqes_off = l->array_off;
	if ((flags & IORING_SETUP_NO_SQARRAY) == 0)
		l->cqes_off += sq_entries * sizeof(uint32_t);
	l->ring_bytes = l->cqes_off +
	    ((size_t)cq_entries * sizeof(struct io_uring_cqe) << cqe_shift);
	l->sqes_bytes = (size_t)sq_entries * sizeof(struct io_uring_sqe) <<
	    sqe_shift;
}

/* Make m, laid out as l, the ring's rings (under mtx once set up). */
static void
kqring_ring_install(struct kqring_ctx *ctx, const struct kqring_rmem *m,
    const struct kqring_layout *l, uint32_t sq_entries, uint32_t cq_entries)
{
	ctx->mem = *m;
	ctx->shm = (struct kqring_shm_header *)m->ring_kva;
	ctx->sq_array = (ctx->flags & IORING_SETUP_NO_SQARRAY) != 0 ? NULL :
	    (uint32_t *)(m->ring_kva + l->array_off);
	ctx->cqes = (struct io_uring_cqe *)(m->ring_kva + l->cqes_off);
	ctx->sqes = (struct io_uring_sqe *)m->sqes_kva;
	ctx->sq_entries = sq_entries;
	ctx->cq_entries = cq_entries;
	ctx->sq_mask = sq_entries - 1;
	ctx->cq_mask = cq_entries - 1;
	ctx->shm->sq_ring_mask = ctx->sq_mask;
	ctx->shm->sq_ring_entries = sq_entries;
	ctx->shm->cq_ring_mask = ctx->cq_mask;
	ctx->shm->cq_ring_entries = cq_entries;
}

/* The rings' offsets, as io_uring_setup(2) returns them in p. */
static void
kqring_ring_offsets(const struct kqring_ctx *ctx,
    const struct kqring_layout *l, struct io_uring_params *p)
{
	p->sq_entries = ctx->sq_entries;
	p->cq_entries = ctx->cq_entries;
	p->sq_off.head = offsetof(struct kqring_shm_header, sq_head);
	p->sq_off.tail = offsetof(struct kqring_shm_header, sq_tail);
	p->sq_off.ring_mask = offsetof(struct kqring_shm_header, sq_ring_mask);
	p->sq_off.ring_entries = offsetof(struct kqring_shm_header,
	    sq_ring_entries);
	p->sq_off.flags = offsetof(struct kqring_shm_header, sq_flags);
	p->sq_off.dropped = offsetof(struct kqring_shm_header, sq_dropped);
	p->sq_off.array = ctx->sq_array != NULL ? (uint32_t)l->array_off : 0;

	p->cq_off.head = offsetof(struct kqring_shm_header, cq_head);
	p->cq_off.tail = offsetof(struct kqring_shm_header, cq_tail);
	p->cq_off.ring_mask = offsetof(struct kqring_shm_header, cq_ring_mask);
	p->cq_off.ring_entries = offsetof(struct kqring_shm_header,
	    cq_ring_entries);
	p->cq_off.overflow = offsetof(struct kqring_shm_header, cq_overflow);
	p->cq_off.cqes = (uint32_t)l->cqes_off;
	p->cq_off.flags = offsetof(struct kqring_shm_header, cq_flags);
}

/*
 * IORING_REGISTER_RESIZE_RINGS: new rings of the sizes in p, with what is
 * queued in the old ones copied over (EOVERFLOW if it doesn't fit), and
 * their offsets returned in p, as io_uring does.  Only DEFER_TASKRUN
 * rings, where the issuer (the caller, holding sq_sx) is the only one to
 * post CQEs or read SQEs; everyone else looks at the rings under mtx,
 * which the swap holds.  Mappings of the old rings keep their memory
 * until they are unmapped.
 */
int
kqring_resize_rings(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra, struct thread *td)
{
	struct kqring_shm_header *shm, *nshm;
	struct io_uring_params p;
	struct kqring_layout l;
	struct kqring_rmem m, old;
	uint32_t sq_entries, cq_entries, head, tail, chead, ctail;
	uint32_t *narray;
	size_t sqe_size, cqe_size;
	char *nsqes, *ncqes;
	int error;

	sx_assert(&ctx->sq_sx, SA_XLOCKED);
	if (ra->arg == NULL || ra->nr_args != 1)
		return (EINVAL);
	if ((ctx->flags & IORING_SETUP_DEFER_TASKRUN) == 0 ||
	    (ctx->flags & IORING_SETUP_SQPOLL) != 0)
		return (EINVAL);
	error = copyin(ra->arg, &p, sizeof(p));
	if (error != 0)
		return (error);
	if ((p.flags & ~(IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP)) != 0)
		return (EINVAL);
	for (size_t i = 0; i < nitems(p.resv); i++)
		if (p.resv[i] != 0)
			return (EINVAL);
	/* What the rings were made with stays. */
	p.flags |= ctx->flags & (IORING_SETUP_NO_SQARRAY |
	    IORING_SETUP_SQE128 | IORING_SETUP_CQE32 | IORING_SETUP_NO_MMAP);
	error = kqring_ring_entries(p.sq_entries, &p, &sq_entries,
	    &cq_entries);
	if (error != 0)
		return (error);
	kqring_ring_layout(p.flags, sq_entries, cq_entries, &l);
	error = kqring_rmem_alloc(td, &p, l.ring_bytes, l.sqes_bytes, &m);
	if (error != 0)
		return (error);

	nshm = (struct kqring_shm_header *)m.ring_kva;
	narray = (ctx->flags & IORING_SETUP_NO_SQARRAY) != 0 ? NULL :
	    (uint32_t *)(m.ring_kva + l.array_off);
	nsqes = (char *)m.sqes_kva;
	ncqes = (char *)(m.ring_kva + l.cqes_off);
	sqe_size = sizeof(struct io_uring_sqe) <<
	    ((ctx->flags & IORING_SETUP_SQE128) != 0 ? 1 : 0);
	cqe_size = sizeof(struct io_uring_cqe) <<
	    ((ctx->flags & IORING_SETUP_CQE32) != 0 ? 1 : 0);

	mtx_lock(&ctx->mtx);
	shm = ctx->shm;
	head = shm->sq_head;
	tail = atomic_load_acq_32(&shm->sq_tail);
	chead = atomic_load_acq_32(&shm->cq_head);
	ctail = shm->cq_tail;
	if (tail - head > sq_entries || ctail - chead > cq_entries) {
		mtx_unlock(&ctx->mtx);
		kqring_rmem_free(&m);
		return (EOVERFLOW);
	}
	/*
	 * SQEs not yet consumed go to the same positions in the new SQ, the
	 * array (if any) pointing straight at them; one the old array pointed
	 * past the SQEs stays an invalid index, to be dropped as before.
	 */
	for (uint32_t i = head; i != tail; i++) {
		uint32_t src = ctx->sq_array != NULL ?
		    ctx->sq_array[i & ctx->sq_mask] : i & ctx->sq_mask;
		uint32_t dst = i & (sq_entries - 1);

		if (narray != NULL)
			narray[dst] = src < ctx->sq_entries ? dst : sq_entries;
		if (src < ctx->sq_entries)
			memcpy(nsqes + dst * sqe_size,
			    (char *)ctx->sqes + src * sqe_size, sqe_size);
	}
	for (uint32_t i = chead; i != ctail; i++)
		memcpy(ncqes + (i & (cq_entries - 1)) * cqe_size,
		    (char *)ctx->cqes + (i & ctx->cq_mask) * cqe_size,
		    cqe_size);
	nshm->sq_head = head;
	nshm->sq_tail = tail;
	nshm->cq_head = chead;
	nshm->cq_tail = ctail;
	nshm->sq_flags = atomic_load_32(&shm->sq_flags);
	nshm->sq_dropped = shm->sq_dropped;
	nshm->cq_flags = shm->cq_flags;
	nshm->cq_overflow = shm->cq_overflow;
	old = ctx->mem;
	kqring_ring_install(ctx, &m, &l, sq_entries, cq_entries);
	mtx_unlock(&ctx->mtx);
	kqring_rmem_free(&old);

	kqring_ring_offsets(ctx, &l, &p);
	return (copyout(&p, ra->arg, sizeof(p)));
}

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
	kqring_rmem_free(&ctx->mem);
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

/* Last close of a ring file. */
static int
kqring_close(struct file *fp, struct thread *td __unused)
{
	struct kqring_ctx *ctx = fp->f_data;

	mtx_lock(&kqring_proc_mtx);
	if (ctx->in_ctx_list) {
		LIST_REMOVE(ctx, proc_link);
		ctx->in_ctx_list = false;
	}
	mtx_unlock(&kqring_proc_mtx);

	kqring_ctx_cancel_all(ctx);
	kqring_ctx_rele(ctx);
	vrele(fp->f_vnode);
	mtx_lock(&kqring_proc_mtx);
	if (--kqring_nfiles == 0)
		wakeup(&kqring_nfiles);
	mtx_unlock(&kqring_proc_mtx);
	return (0);
}

/*
 * Open /dev/kqring: a new ring, as a file with kqring_fileops.  Returning
 * ENXIO with the file's operations replaced tells open(2) the file is
 * ready (as linuxkpi does); vn_open() then drops its vnode reference, so
 * the file takes its own, for fstat(2) and procstat(1).
 */
static int
kqring_fdopen(struct cdev *dev __unused, int fflags __unused,
    struct thread *td, struct file *fp)
{
	struct kqring_ctx *ctx;

	if (fp == NULL)
		return (ENXIO);
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

	mtx_lock(&kqring_proc_mtx);
	if (kqring_unloading) {
		mtx_unlock(&kqring_proc_mtx);
		kqring_wq_put(ctx->wq);
		callout_drain(&ctx->wq_abort_callout);
		knlist_destroy(&ctx->rsel.si_note);
		sx_destroy(&ctx->sq_sx);
		sx_destroy(&ctx->bg_sx);
		mtx_destroy(&ctx->mtx);
		free(ctx, M_KQRING);
		return (ENODEV);
	}
	kqring_nfiles++;
	ctx->in_ctx_list = true;
	LIST_INSERT_HEAD(&kqring_ctx_list, ctx, proc_link);
	mtx_unlock(&kqring_proc_mtx);
	vref(fp->f_vnode);
	finit(fp, fp->f_flag, DTYPE_DEV, ctx, &kqring_fileops);
	return (ENXIO);
}


static int
kqring_setup_rings(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	struct io_uring_params p;
	struct kqring_wq *attach_wq = NULL;
	struct kqring_layout l;
	struct kqring_rmem m;
	bool wq_swapped = false;
	uint32_t sq_entries, cq_entries;
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
	/* A ring with no descriptor has nothing to mmap through. */
	if ((p.flags & IORING_SETUP_REGISTERED_FD_ONLY) != 0 &&
	    (p.flags & IORING_SETUP_NO_MMAP) == 0)
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

	error = kqring_ring_entries(uarg->entries, &p, &sq_entries,
	    &cq_entries);
	if (error != 0)
		return (error);

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

	kqring_ring_layout(p.flags, sq_entries, cq_entries, &l);
	error = kqring_rmem_alloc(td, &p, l.ring_bytes, l.sqes_bytes, &m);
	if (error != 0) {
		kqring_wq_put(attach_wq);
		return (error);
	}
	ctx->flags = p.flags;
	kqring_ring_install(ctx, &m, &l, sq_entries, cq_entries);
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

	p.features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP |
	    IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_RW_CUR_POS |
	    IORING_FEAT_EXT_ARG | IORING_FEAT_FAST_POLL |
	    IORING_FEAT_POLL_32BITS | IORING_FEAT_SQPOLL_NONFIXED |
	    IORING_FEAT_CQE_SKIP | IORING_FEAT_LINKED_FILE |
	    IORING_FEAT_NATIVE_WORKERS | IORING_FEAT_CUR_PERSONALITY |
	    IORING_FEAT_RSRC_TAGS | IORING_FEAT_REG_REG_RING |
	    IORING_FEAT_RECVSEND_BUNDLE | IORING_FEAT_MIN_TIMEOUT;

	kqring_ring_offsets(ctx, &l, &p);

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
		kqring_rmem_free(&ctx->mem);
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

/*
 * The wait of an enter, with io_uring's min_wait_usec: wait up to min_sbt
 * for min_complete CQEs, then return if there are any, or else wait for
 * just one until the timeout (which counts from the start; one no later
 * than min_sbt ends the wait there).
 */
static int
kqring_enter_wait(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, sbintime_t min_sbt, bool lock)
{
	bool sqpoll = (ctx->flags & IORING_SETUP_SQPOLL) != 0;
	sbintime_t deadline;
	int error;

	if (min_sbt == 0 || min_complete <= 1)
		return (sqpoll ? kqring_sqpoll_wait_cqes(ctx, min_complete,
		    has_timeout, sbt) : kqring_wait_cqes(ctx, min_complete,
		    has_timeout, sbt, lock));
	deadline = has_timeout ? kqring_deadline(sbt) : 0;
	error = sqpoll ? kqring_sqpoll_wait_cqes(ctx, min_complete, true,
	    min_sbt) : kqring_wait_cqes(ctx, min_complete, true, min_sbt, lock);
	if (error != ETIMEDOUT)
		return (error);
	if (has_timeout && sbt <= min_sbt)
		return (ETIMEDOUT);
	sbt = has_timeout ? kqring_until(deadline) : 0;
	if (has_timeout && sbt == 0)
		return (ETIMEDOUT);
	return (sqpoll ? kqring_sqpoll_wait_cqes(ctx, 1, has_timeout, sbt) :
	    kqring_wait_cqes(ctx, 1, has_timeout, sbt, lock));
}

static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t submitted = 0;
	sbintime_t sbt = 0, min_sbt = 0;
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
		/* Signal masks are not implemented: reject, not ignore. */
		if (ext.sigmask != 0)
			return (EINVAL);
		min_sbt = (sbintime_t)ext.min_wait_usec * SBT_1US;
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
		return (kqring_enter_wait(ctx, ea->min_complete, has_timeout,
		    sbt, min_sbt, false));
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
	error = kqring_enter_wait(ctx, ea->min_complete, has_timeout, sbt,
	    min_sbt, lock);
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
	/* The timeout counts from here, however often the wait is woken. */
	sbintime_t deadline = has_timeout ? kqring_deadline(sbt) : 0;
	int error;

	for (;;) {
		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);

		if (has_timeout)
			sbt = kqring_until(deadline);
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
			if (kqring_until(deadline) == 0) {
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
kqring_ioctl(struct file *fp, u_long cmd, void *data,
    struct ucred *active_cred __unused, struct thread *td)
{
	struct kqring_ctx *ctx = fp->f_data;
	struct file *rfp = NULL;
	int error, res = 0;

	switch (cmd) {
	case FIONBIO:
		/* fcntl(F_SETFL, O_NONBLOCK): nothing on a ring blocks on it. */
		return (0);
	case FIOASYNC:
		return (*(int *)data != 0 ? EINVAL : 0);
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
		/*
		 * REGISTERED_FD_ONLY: the ring goes in the registered-ring
		 * table, with the reference fp holds, and its descriptor is
		 * closed; the slot is returned instead.
		 */
		if (error == 0 &&
		    (ctx->flags & IORING_SETUP_REGISTERED_FD_ONLY) != 0) {
			uint32_t slot = IORING_FILE_INDEX_ALLOC;

			error = kqring_ring_fds_insert(td, fp, &slot);
			if (error == 0) {
				(void)kern_close(td, fd);
				td->td_retval[0] = slot;
				return (0);
			}
		}
		fdrop(fp, td);
	}
	if (error != 0) {
		(void)kern_close(td, fd);
		return (error);
	}
	td->td_retval[0] = fd;
	return (0);
}

/*
 * kqring_enter(2) calls in progress, by the CPU each started on.  A
 * module's system calls are counted in their sysent entry (sy_thrcnt) so
 * that unload can wait for them, but that is one atomic word every CPU
 * updates twice per call, and enter is the call threads make in a loop:
 * it stopped enter scaling past one thread.  So the enter entry is marked
 * SY_THR_STATIC while loaded, and counted here instead, on a word of the
 * CPU's own.
 */
DPCPU_DEFINE_STATIC(u_int, kqring_sys_busy);

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
	u_int *busy = DPCPU_ID_PTR(PCPU_GET(cpuid), kqring_sys_busy);
	struct kqring_ctx *ctx;
	struct file *fp;
	int error;

	atomic_add_acq_int(busy, 1);
	error = kqring_sys_ring(td, uap->fd,
	    (uap->flags & IORING_ENTER_REGISTERED_RING) != 0, true, &fp, &ctx);
	if (error == 0) {
		error = kqring_ioc_enter(ctx, &ea, td);
		fdrop(fp, td);
		/* SQE ops' kern_*() calls leave their results in td_retval. */
		td->td_retval[0] = error == 0 ? ea.SUBMITTED_out : 0;
	}
	atomic_subtract_rel_int(busy, 1);
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

#define	KQRING_SYS_ENTER	1	/* kqring_sysent[] index */

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
	/*
	 * Enter counts itself (kqring_sys_busy): stop the kernel counting
	 * it, once no call it counted is in progress.  syscallenter()
	 * decides per call, so a call ends as it started.
	 */
	if (kqring_sysnum[KQRING_SYS_ENTER] != NO_SYSCALL) {
		while (!atomic_cmpset_32(
		    &sysent[kqring_sysnum[KQRING_SYS_ENTER]].sy_thrcnt, 0,
		    SY_THR_STATIC))
			cpu_spinwait();
	}
}

/* Waits for the system calls in progress, as destroy_dev() for ioctls. */
static void
kqring_sys_deregister_all(void)
{
	u_int cpu, n;

	/*
	 * Count enter calls in the sysent entry again, so that
	 * kern_syscall_deregister() drains those that start from now on.
	 */
	if (kqring_sysnum[KQRING_SYS_ENTER] != NO_SYSCALL)
		atomic_store_rel_32(
		    &sysent[kqring_sysnum[KQRING_SYS_ENTER]].sy_thrcnt, 0);
	for (int i = 0; i < KQRING_NSYS; i++) {
		if (kqring_sysnum[i] != NO_SYSCALL)
			(void)kern_syscall_deregister(sysent, kqring_sysnum[i],
			    &kqring_old_sysent[i]);
		kqring_sysnum[i] = NO_SYSCALL;
	}
	/*
	 * Then wait out those that started uncounted.  No new one starts,
	 * so each CPU's count only falls: one pass reading all zero means
	 * none is left.
	 */
	do {
		n = 0;
		CPU_FOREACH(cpu)
			n += atomic_load_acq_int(DPCPU_ID_PTR(cpu,
			    kqring_sys_busy));
		if (n != 0)
			pause("kqsysdr", 1);
	} while (n != 0);
	/*
	 * A call can be between the sysent dispatch and its count, or
	 * between the count and its return, for a few instructions: if it
	 * was preempted there, let it run on before the module goes.
	 */
	pause("kqsysun", hz);
}

/* The object (with a reference) that a mapping at *offset shows. */
static int
kqring_mmap_object(struct kqring_ctx *ctx, vm_ooffset_t *offset,
    vm_size_t size, vm_object_t *object)
{
	int error;

	if (!ctx->setup_done)
		return (EINVAL);

	if (*offset == IORING_OFF_SQ_RING || *offset == IORING_OFF_CQ_RING ||
	    *offset == IORING_OFF_SQES) {
		bool sqes = *offset == IORING_OFF_SQES;
		vm_object_t obj;

		/* Under mtx: IORING_REGISTER_RESIZE_RINGS swaps them. */
		mtx_lock(&ctx->mtx);
		obj = sqes ? ctx->mem.sqes_obj : ctx->mem.ring_obj;
		if (obj == NULL ||
		    size > (sqes ? ctx->mem.sqes_len : ctx->mem.ring_len)) {
			mtx_unlock(&ctx->mtx);
			return (EINVAL);
		}
		vm_object_reference(obj);
		mtx_unlock(&ctx->mtx);
		*object = obj;
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

/*
 * mmap(2) of the rings and SQE array at IORING_OFF_*: shared, readable
 * and writable as the descriptor allows, never executable.
 */
static int
kqring_mmap(struct file *fp, vm_map_t map, vm_offset_t *addr, vm_size_t size,
    vm_prot_t prot, vm_prot_t cap_maxprot, int flags, vm_ooffset_t foff,
    struct thread *td)
{
	struct kqring_ctx *ctx = fp->f_data;
	vm_object_t object;
	vm_prot_t maxprot = VM_PROT_NONE;
	int error;

	if ((prot & ~(VM_PROT_READ | VM_PROT_WRITE)) != 0 ||
	    (flags & MAP_SHARED) == 0)
		return (EINVAL);
	if ((fp->f_flag & FREAD) != 0)
		maxprot |= VM_PROT_READ;
	else if ((prot & VM_PROT_READ) != 0)
		return (EACCES);
	if ((fp->f_flag & FWRITE) != 0)
		maxprot |= VM_PROT_WRITE;
	else if ((prot & VM_PROT_WRITE) != 0)
		return (EACCES);
	maxprot &= cap_maxprot;

	error = kqring_mmap_object(ctx, &foff, size, &object);
	if (error != 0)
		return (error);
	error = vm_mmap_object(map, addr, size, prot, maxprot, flags, object,
	    foff, FALSE, td);
	if (error != 0)
		vm_object_deallocate(object);
	return (error);
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
		printf("kqring: loaded (/dev/kqring)\n");
		return (0);

	case MOD_UNLOAD:
		/*
		 * Block new zero-copy admissions and wait briefly for any
		 * in-flight EXT_SFBUF mbufs before destroying /dev/kqring; if
		 * unread mbufs remain in a socket buffer, re-open admissions
		 * and refuse unload with EBUSY rather than hanging after
		 * /dev/kqring is gone.
		 */
		/*
		 * Open ring files use this module's file operations, which
		 * cannot be revoked: unload only once they are all closed
		 * (no new one opens once kqring_unloading is set).
		 */
		mtx_lock(&kqring_proc_mtx);
		if (kqring_nfiles > 0) {
			mtx_unlock(&kqring_proc_mtx);
			return (EBUSY);
		}
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
		 * No ring is open, and destroy_dev() waits for opens in
		 * progress.  EVENTHANDLER_DEREGISTER() waits for running
		 * exit/exec hooks.  Only then is the registered-ring table
		 * torn down.
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
