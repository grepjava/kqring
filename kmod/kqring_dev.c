/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

#include <sys/imgact.h>
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
	kqring_mem_uncharge(m->uip, m->charged, m->ring_pages != NULL ||
	    m->sqes_pages != NULL);
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
	size_t charge = round_page(ring_bytes) + round_page(sqes_bytes);
	bool nommap = (p->flags & IORING_SETUP_NO_MMAP) != 0;
	struct uidinfo *uip;
	int error;

	memset(m, 0, sizeof(*m));
	error = kqring_mem_charge(td->td_ucred, charge, nommap, &uip);
	if (error != 0)
		return (error);
	if (nommap) {
		error = kqring_pin_shm(td, p->cq_off.user_addr, ring_bytes,
		    true, &m->ring_kva, &m->ring_len, &m->ring_pages);
		if (error == 0)
			error = kqring_pin_shm(td, p->sq_off.user_addr,
			    sqes_bytes, true, &m->sqes_kva, &m->sqes_len,
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
	if (error != 0) {
		kqring_rmem_free(m);
		kqring_mem_uncharge(uip, charge, nommap);
		return (error);
	}
	m->uip = uip;
	m->charged = charge;
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

/* Rings' sizes and offsets, as io_uring_setup(2) returns them in p. */
static void
kqring_ring_offsets(uint32_t flags, uint32_t sq_entries, uint32_t cq_entries,
    const struct kqring_layout *l, struct io_uring_params *p)
{
	p->sq_entries = sq_entries;
	p->cq_entries = cq_entries;
	p->sq_off.head = offsetof(struct kqring_shm_header, sq_head);
	p->sq_off.tail = offsetof(struct kqring_shm_header, sq_tail);
	p->sq_off.ring_mask = offsetof(struct kqring_shm_header, sq_ring_mask);
	p->sq_off.ring_entries = offsetof(struct kqring_shm_header,
	    sq_ring_entries);
	p->sq_off.flags = offsetof(struct kqring_shm_header, sq_flags);
	p->sq_off.dropped = offsetof(struct kqring_shm_header, sq_dropped);
	p->sq_off.array = (flags & IORING_SETUP_NO_SQARRAY) == 0 ?
	    (uint32_t)l->array_off : 0;

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
 * until they are unmapped.  The new offsets are copied out before the
 * swap, so that a failure leaves the ring as it was.
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
	    IORING_SETUP_SQE128 | IORING_SETUP_CQE32 | IORING_SETUP_NO_MMAP |
	    IORING_SETUP_SQE_MIXED | IORING_SETUP_CQE_MIXED |
	    IORING_SETUP_SQ_REWIND);
	error = kqring_ring_entries(p.sq_entries, &p, &sq_entries,
	    &cq_entries);
	if (error != 0)
		return (error);
	kqring_ring_layout(p.flags, sq_entries, cq_entries, &l);
	error = kqring_rmem_alloc(td, &p, l.ring_bytes, l.sqes_bytes, &m);
	if (error != 0)
		return (error);

	kqring_ring_offsets(p.flags, sq_entries, cq_entries, &l, &p);
	error = copyout(&p, ra->arg, sizeof(p));
	if (error != 0) {
		kqring_rmem_free(&m);
		return (error);
	}

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
	/* SQ_REWIND keeps its SQEs from slot 0, the head and tail at 0. */
	if ((ctx->flags & IORING_SETUP_SQ_REWIND) != 0)
		memcpy(nsqes, ctx->sqes,
		    (size_t)MIN(ctx->sq_entries, sq_entries) * sqe_size);
	for (uint32_t i = head; i != tail; i++) {
		uint32_t src = ctx->sq_array != NULL ?
		    ctx->sq_array[i & ctx->sq_mask] : i & ctx->sq_mask;
		uint32_t dst = i & (sq_entries - 1);

		if (narray != NULL)
			narray[dst] = src < ctx->sq_entries ? dst : sq_entries;
		if (src >= ctx->sq_entries)
			continue;
		memcpy(nsqes + dst * sqe_size,
		    (char *)ctx->sqes + src * sqe_size, sqe_size);
		/* SQE_MIXED: a 128-byte SQE's second half goes with it. */
		if ((ctx->flags & IORING_SETUP_SQE_MIXED) != 0 &&
		    i + 1 != tail && src != ctx->sq_mask &&
		    (ctx->sqes[src].opcode == IORING_OP_NOP128 ||
		    ctx->sqes[src].opcode == IORING_OP_URING_CMD128)) {
			i++;
			dst = i & (sq_entries - 1);
			if (narray != NULL)
				narray[dst] = dst;
			memcpy(nsqes + dst * sqe_size,
			    (char *)ctx->sqes + (src + 1) * sqe_size, sqe_size);
		}
	}
	if ((ctx->flags & IORING_SETUP_CQE_MIXED) != 0) {
		/*
		 * Repack: a 32-byte CQE mustn't wrap in the new CQ either, so
		 * the old SKIP entries go and new ones go where needed.
		 */
		uint32_t nt = chead, nmask = cq_entries - 1;

		for (uint32_t i = chead; i != ctail;) {
			const struct io_uring_cqe *c =
			    &ctx->cqes[i & ctx->cq_mask];
			uint32_t n = (c->flags & IORING_CQE_F_32) != 0 ? 2 : 1;

			if ((c->flags & IORING_CQE_F_SKIP) != 0) {
				i++;
				continue;
			}
			/* Only a head moved into a CQE's second half gets here. */
			if (ctail - i < n)
				break;
			if (n == 2 && (nt & nmask) == nmask) {
				struct io_uring_cqe *sk = (struct io_uring_cqe *)
				    (ncqes + (nt & nmask) * cqe_size);

				sk->user_data = 0;
				sk->res = 0;
				sk->flags = IORING_CQE_F_SKIP;
				nt++;
			}
			if (nt - chead + n > cq_entries) {
				mtx_unlock(&ctx->mtx);
				kqring_rmem_free(&m);
				return (EOVERFLOW);
			}
			memcpy(ncqes + (nt & nmask) * cqe_size, c,
			    n * cqe_size);
			nt += n;
			i += n;
		}
		ctail = nt;
	} else {
		for (uint32_t i = chead; i != ctail; i++)
			memcpy(ncqes + (i & (cq_entries - 1)) * cqe_size,
			    (char *)ctx->cqes + (i & ctx->cq_mask) * cqe_size,
			    cqe_size);
	}
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
	/*
	 * Pollers' kicks (kqring_kp_kick()) may still use the old header:
	 * wait them out, and keep the IORING_SQ_TASKRUN they set there.
	 */
	kqring_kp_shm_barrier(ctx);
	if ((atomic_load_32(&shm->sq_flags) & IORING_SQ_TASKRUN) != 0)
		atomic_set_32(&nshm->sq_flags, IORING_SQ_TASKRUN);
	mtx_unlock(&ctx->mtx);
	kqring_rmem_free(&old);
	return (0);
}

static void
kqring_ctx_free_final(struct kqring_ctx *ctx)
{
	struct kqring_op *op;
	struct kqring_ocqe *ocqe;
	struct kqring_rcqe *rc;
	struct kqring_cred *kc;
	struct kqring_tctx *t;

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
	mtx_unlock(&ctx->mtx);
	callout_drain(&ctx->wq_abort_callout);
	while ((op = TAILQ_FIRST(&ctx->ready_q)) != NULL) {
		TAILQ_REMOVE(&ctx->ready_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	while ((op = TAILQ_FIRST(&ctx->defer_q)) != NULL) {
		TAILQ_REMOVE(&ctx->defer_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	kqring_trig_teardown(ctx);

	while ((op = TAILQ_FIRST(&ctx->free_ops)) != NULL) {
		TAILQ_REMOVE(&ctx->free_ops, op, active_link);
		free(op, M_KQRING);
	}
	free(ctx->sw_cache[0], M_KQRING);
	free(ctx->sw_cache[1], M_KQRING);

	/* Every request (and its references) is gone: drop the tables. */
	kqring_kpoll_close_all(ctx);
	kqring_files_free(ctx);
	kqring_bufs_free(ctx);
	kqring_region_free(ctx);
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
	sx_destroy(&ctx->pipe_sx);
	mtx_destroy(&ctx->mtx);
	/* Every request is gone, and every kthread and exit task. */
	while ((t = LIST_FIRST(&ctx->tctxs)) != NULL) {
		LIST_REMOVE(t, link);
		t->linked = false;
		kqring_tctx_rele(t);
	}
	free(ctx, M_KQRING);
}

void
kqring_ctx_rele(struct kqring_ctx *ctx)
{
	if (refcount_release(&ctx->refs))
		kqring_ctx_free_final(ctx);
}

/*
 * Ring teardown and process stops.
 *
 * Teardown waits for other threads of the ring's process: its kthreads
 * (SQPOLL or completions, the poller) to exit, and a thread that holds
 * sq_sx.  Those park themselves for SIGSTOP and ptrace stops.  A process
 * only counts as stopped once none of its threads is in an uninterruptible
 * sleep, and a thread that enters an interruptible sleep while a stop is
 * pending is not suspended there: the sleep fails, for the system call to
 * unwind and stop at the user boundary.  close(2) cannot unwind, so a
 * teardown that would wait while its process stops hands the rest of the
 * work to kqring_teardown_tq, outside the process (as io_uring hands ring
 * exit to a workqueue), and returns; the task finishes it once the
 * kthreads have gone.  During exit the waits go on: the kthreads exit then.
 */
static struct taskqueue *kqring_teardown_tq;

static void kqring_ctx_cancel_locked(struct kqring_ctx *ctx);

static bool
kqring_teardown_may_defer(void)
{
	struct proc *p = curproc;

	return ((p->p_flag & (P_SYSTEM | P_SINGLE_EXIT)) == 0 &&
	    (p->p_flag2 & P2_WEXIT) == 0);
}

/*
 * Whether curthread is in execve(2) with its process's other threads parked.
 * thread_single(SINGLE_BOUNDARY) holds them until the new image is in place,
 * after the process_exec handler, and only then does post_execve() make them
 * exit.  A ring's kthreads can't get out until that thread moves on, and
 * nothing interrupts it, so it mustn't wait for them.
 */
static bool
kqring_teardown_in_exec(void)
{
	struct proc *p = curproc;

	return ((p->p_flag & (P_STOPPED_SINGLE | P_SINGLE_EXIT)) ==
	    P_STOPPED_SINGLE && p->p_singlethread == curthread);
}

/*
 * Sleep in teardown until woken or a second passes; callers loop on their
 * condition.  EINTR: defer the teardown (see above).
 */
int
kqring_teardown_sleep(void *chan, struct mtx *mtx, const char *wmesg)
{
	int error;

	if ((curproc->p_flag & P_SYSTEM) != 0) {
		(void)msleep(chan, mtx, 0, wmesg, hz);
		return (0);
	}
	if (kqring_teardown_in_exec())
		return (EINTR);
	error = msleep(chan, mtx, PCATCH, wmesg, hz);
	if (error != EINTR && error != ERESTART)
		return (0);
	if (kqring_teardown_may_defer())
		return (EINTR);
	(void)msleep(chan, mtx, 0, wmesg, 1);
	return (0);
}

/* sq_sx for teardown, as kqring_teardown_sleep(). */
static int
kqring_teardown_sq_lock(struct kqring_ctx *ctx)
{
	for (;;) {
		if ((curproc->p_flag & P_SYSTEM) != 0) {
			sx_xlock(&ctx->sq_sx);
			return (0);
		}
		if (kqring_teardown_in_exec())
			return (sx_try_xlock(&ctx->sq_sx) ? 0 : EINTR);
		if (sx_xlock_sig(&ctx->sq_sx) == 0)
			return (0);
		if (kqring_teardown_may_defer())
			return (EINTR);
		pause("kqsqsx", 1);
	}
}

/*
 * The first part of teardown: mark the ring closing, wait for its kthreads
 * and take sq_sx.  With nowait (kqring_teardown_tq) it does not wait but
 * returns EWOULDBLOCK.  Returns with sq_sx and ctx->mtx held and the ring
 * marked cancelling by curthread (0), or EINTR (defer), or EALREADY:
 * another thread is cancelling it (waited for, unless nowait or that is
 * curthread).
 */
static int
kqring_ctx_cancel_begin(struct kqring_ctx *ctx, bool nowait)
{
	struct kqring_tctx *t;
	int error;

	/*
	 * Signal closing and wait for the SQPOLL or completion kthreads to
	 * exit before taking sq_sx (since kqring_sqpoll_thread() acquires
	 * sq_sx in its main loop).  If called from one of them (e.g. via
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
	LIST_FOREACH(t, &ctx->tctxs, link) {
		if (curthread == t->task_td) {
			mtx_unlock(&ctx->mtx);
			return (EALREADY);
		}
	}
	for (;;) {
		LIST_FOREACH(t, &ctx->tctxs, link) {
			if (t->task_running)
				break;
		}
		if (t == NULL)
			break;
		error = nowait ? EWOULDBLOCK :
		    kqring_teardown_sleep(&t->task_running, &ctx->mtx,
		    "kqsqdt");
		if (error != 0)
			goto fail;
	}
	if ((error = kqring_kpoll_stop(ctx, NULL, nowait)) != 0)
		goto fail;
	while (!ctx->cancelling) {
		mtx_unlock(&ctx->mtx);
		if (nowait)
			error = sx_try_xlock(&ctx->sq_sx) ? 0 : EWOULDBLOCK;
		else
			error = kqring_teardown_sq_lock(ctx);
		mtx_lock(&ctx->mtx);
		if (error != 0)
			goto fail;
		if (!ctx->cancelling) {
			ctx->cancelling = true;
			ctx->cancel_td = curthread;
			return (0);
		}
		sx_xunlock(&ctx->sq_sx);
	}
	if (ctx->cancel_td != curthread && !nowait) {
		while (!ctx->cancelled)
			if (kqring_teardown_sleep(&ctx->cancelled, &ctx->mtx,
			    "kqctxd") != 0)
				break;	/* The canceller finishes alone. */
	}
	error = EALREADY;
fail:
	mtx_unlock(&ctx->mtx);
	return (error);
}

/* Teardown handed off by kqring_ctx_cancel_all(): finish it, unwaiting. */
static void
kqring_ctx_cancel_task(void *arg, int pending __unused)
{
	struct kqring_ctx *ctx = arg;
	int error;

	error = kqring_ctx_cancel_begin(ctx, true);
	if (error == EWOULDBLOCK) {
		/* A kthread is still parked for the stop: look again soon. */
		pause("kqtdwt", hz / 50 + 1);
		taskqueue_enqueue(kqring_teardown_tq, &ctx->cancel_task);
		return;
	}
	if (error == 0)
		kqring_ctx_cancel_locked(ctx);
	kqring_ctx_rele(ctx);
	kqring_worker_rele();
}

/*
 * Whether processes other than the one that set it up may use ctx, each
 * with its own context (docs/concurrent-submitters.md): a shared ring
 * without SQPOLL, whose thread is the one issuer, and without kqring's
 * extensions, which keep a ring to one process.
 */
static bool
kqring_ctx_multiproc(const struct kqring_ctx *ctx)
{
	return (ctx->shared && (ctx->flags & IORING_SETUP_SQPOLL) == 0 &&
	    ctx->setup_done && !ctx->ext_on);
}

void
kqring_ctx_cancel_all(struct kqring_ctx *ctx)
{
	bool defer;

	switch (kqring_ctx_cancel_begin(ctx, false)) {
	case 0:
		kqring_ctx_cancel_locked(ctx);
		return;
	case EINTR:
		mtx_lock(&ctx->mtx);
		defer = !ctx->cancel_deferred;
		ctx->cancel_deferred = true;
		mtx_unlock(&ctx->mtx);
		if (defer) {
			refcount_acquire(&ctx->refs);
			kqring_worker_hold();
			taskqueue_enqueue(kqring_teardown_tq, &ctx->cancel_task);
		}
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------------------
 * Process contexts (docs/concurrent-submitters.md)
 * ------------------------------------------------------------------------
 */

static uint64_t kqring_tctx_ids;
static void kqring_tctx_exit_task(void *arg, int pending);

/* A context for td's process, with wq or a new pool in it; not linked. */
struct kqring_tctx *
kqring_tctx_new(struct kqring_ctx *ctx, struct thread *td, struct kqring_wq *wq)
{
	struct kqring_tctx *t;

	t = malloc(sizeof(*t), M_KQRING, M_WAITOK | M_ZERO);
	t->ctx = ctx;
	t->proc = td->td_proc;
	t->wq = wq != NULL ? wq : kqring_wq_alloc(td);
	TAILQ_INIT(&t->taskwork);
	sx_init(&t->kp_sx, "kqring kpoll");
	TIMEOUT_TASK_INIT(kqring_teardown_tq, &t->exit_task, 0,
	    kqring_tctx_exit_task, t);
	refcount_init(&t->refs, 1);
	t->id = atomic_fetchadd_64(&kqring_tctx_ids, 1) + 1;
	return (t);
}

/* Put t on ctx's list: ctx->mtx, and sq_sx unless nobody else has ctx. */
void
kqring_tctx_link_locked(struct kqring_ctx *ctx, struct kqring_tctx *t)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	LIST_INSERT_HEAD(&ctx->tctxs, t, link);
	t->linked = true;
}

static void
kqring_tctx_unlink(struct kqring_ctx *ctx, struct kqring_tctx *t)
{
	mtx_lock(&ctx->mtx);
	if (t->linked) {
		LIST_REMOVE(t, link);
		t->linked = false;
	}
	mtx_unlock(&ctx->mtx);
}

/*
 * A ring nothing has used yet (being set up, or still disabled) gets its
 * first context in td's process, if another process (one that inherited
 * the descriptor) opened it: td sets it up or enables it, and its
 * requests run in td's process.  Its only context, idle and with no
 * kthread, is replaced.
 */
void
kqring_tctx_rebind(struct kqring_ctx *ctx, struct thread *td)
{
	struct kqring_tctx *ot = LIST_FIRST(&ctx->tctxs), *nt;

	if (ot == NULL || ot->proc == td->td_proc)
		return;
	KASSERT(LIST_NEXT(ot, link) == NULL && ot->nops == 0 &&
	    !ot->task_running && !ot->kp_running && ot->kp_fp == NULL,
	    ("kqring: rebinding a used ring"));
	nt = kqring_tctx_new(ctx, td, NULL);
	mtx_lock(&ctx->mtx);
	LIST_REMOVE(ot, link);
	ot->linked = false;
	kqring_tctx_link_locked(ctx, nt);
	mtx_unlock(&ctx->mtx);
	kqring_tctx_rele(ot);
}

/* Close t's pool, once: its workers exit as they finish. */
static void
kqring_tctx_close_wq(struct kqring_tctx *t)
{
	if (t->wq_closed)
		return;
	t->wq_closed = true;
	kqring_wq_close(t->wq);
}

void
kqring_tctx_rele(struct kqring_tctx *t)
{
	if (!refcount_release(&t->refs))
		return;
	KASSERT(!t->linked && TAILQ_EMPTY(&t->taskwork) && t->nops == 0 &&
	    !t->task_running && !t->kp_running && t->kp_fp == NULL,
	    ("kqring: freeing a live process context"));
	sx_destroy(&t->kp_sx);
	if (t->wq_closed)
		kqring_wq_rele(t->wq);
	else
		kqring_wq_put(t->wq);
	free(t, M_KQRING);
}

/*
 * td's process's context on ctx, made if it has none: it submits or
 * registers.  Only a ring other processes may use (kqring_ctx_multiproc())
 * takes a new one; otherwise EEXIST, as for the SINGLE_ISSUER rule.  Not
 * with sq_sx held.
 */
int
kqring_tctx_attach(struct kqring_ctx *ctx, struct thread *td,
    struct kqring_tctx **tp)
{
	struct kqring_tctx *t, *n;
	int error = 0;

	mtx_lock(&ctx->mtx);
	t = kqring_tctx_cur(ctx);
	mtx_unlock(&ctx->mtx);
	if (t != NULL) {
		*tp = t;
		return (0);
	}
	if (!kqring_ctx_multiproc(ctx))
		return (EEXIST);
	n = kqring_tctx_new(ctx, td, NULL);
	if (sx_xlock_sig(&ctx->sq_sx) != 0) {
		kqring_tctx_rele(n);
		return (EINTR);
	}
	mtx_lock(&ctx->mtx);
	if (ctx->closing || ctx->cancelling || ctx->ext_on)
		error = ctx->ext_on ? EEXIST : EBADF;
	else if ((t = kqring_tctx_cur(ctx)) == NULL) {
		kqring_tctx_link_locked(ctx, n);
		t = n;
		n = NULL;
	}
	mtx_unlock(&ctx->mtx);
	if (error == 0 && n == NULL && ctx->enabled && kqring_has_kthread(ctx)) {
		error = kqring_sqpoll_start(ctx, t, td);
		if (error != 0) {
			kqring_tctx_unlink(ctx, t);
			n = t;
		}
	}
	sx_xunlock(&ctx->sq_sx);
	if (n != NULL)
		kqring_tctx_rele(n);
	*tp = t;
	return (error);
}

/*
 * One of t's requests taken off its taskwork as its process went: finish
 * what a worker or the disk finished, cancel the rest.  Nothing here may
 * touch the process's memory, which is gone or going.
 */
static void
kqring_tctx_exit_taskwork(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (op->tw_issue || op->wq_retired || op->wq_done || op->bio_done ||
	    op->futex_woken || op->sqe.opcode == IORING_OP_TIMEOUT ||
	    op->sqe.opcode == IORING_OP_LINK_TIMEOUT) {
		kqring_taskwork_run_one(ctx, op);
		return;
	}
	mtx_lock(&ctx->mtx);
	if (op->armed)
		kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, -ECANCELED, 0);
}

/*
 * A TIMEOUT on its own, which goes on after its process (below).  One
 * that heads a link chain is cancelled with the chain, as in io_uring.
 */
static bool
kqring_op_standalone_timeout(const struct kqring_op *op)
{
	return (op->sqe.opcode == IORING_OP_TIMEOUT && op->armed &&
	    op->link == NULL);
}

/*
 * Cancel t's requests and wait for them: each completes with a CQE, with
 * its result if a worker or the disk had finished it, or -ECANCELED, as
 * io_uring cancels an exiting task's requests.  A standalone TIMEOUT
 * stays armed and fires, as in io_uring: it needs nothing of the process.
 * With sq_sx, which is dropped while waiting for workers and the disk.
 * False if the ring started closing meanwhile: the close destroys what
 * is left.
 */
static bool
kqring_tctx_cancel(struct kqring_ctx *ctx, struct kqring_tctx *t)
{
	struct kqring_op *op;
	u_int ntimeouts;

	sx_assert(&ctx->sq_sx, SA_XLOCKED);
	for (;;) {
		mtx_lock(&ctx->mtx);
		if (ctx->closing) {
			mtx_unlock(&ctx->mtx);
			return (false);
		}
		if ((op = TAILQ_FIRST(&t->taskwork)) != NULL) {
			kqring_tw_remove_locked(ctx, op);
			op->in_taskwork = false;
			mtx_unlock(&ctx->mtx);
			kqring_tctx_exit_taskwork(ctx, op);
			continue;
		}
		TAILQ_FOREACH(op, &ctx->ready_q, q_link) {
			if (op->tctx == t)
				break;
		}
		if (op != NULL) {
			TAILQ_REMOVE(&ctx->ready_q, op, q_link);
			mtx_unlock(&ctx->mtx);
			kqring_req_complete(ctx, op, -ECANCELED, 0);
			continue;
		}
		TAILQ_FOREACH(op, &ctx->defer_q, q_link) {
			if (op->tctx == t)
				break;
		}
		if (op != NULL) {
			TAILQ_REMOVE(&ctx->defer_q, op, q_link);
			ctx->ndeferred -= op->nchain;
			mtx_unlock(&ctx->mtx);
			kqring_req_complete(ctx, op, -ECANCELED, 0);
			continue;
		}
		ntimeouts = 0;
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (op->tctx != t || op->cancelled)
				continue;
			if (kqring_op_standalone_timeout(op)) {
				ntimeouts++;
				continue;
			}
			/* Disk or file I/O in flight: wait for it below. */
			if (!kqring_op_cancel_busy_locked(ctx, op))
				break;
		}
		if (op != NULL) {
			kqring_op_unarm_locked(ctx, op);
			mtx_unlock(&ctx->mtx);
			kqring_req_complete(ctx, op, -ECANCELED, 0);
			continue;
		}
		if (t->nops == ntimeouts) {
			mtx_unlock(&ctx->mtx);
			break;
		}
		/* Workers and bios still running: their taskwork wakes us. */
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		sx_xunlock(&ctx->sq_sx);
		ctx->sleeping++;
		(void)msleep(&ctx->wakeup_chan, &ctx->mtx, 0, "kqtxit",
		    hz / 100 + 1);
		ctx->sleeping--;
		mtx_unlock(&ctx->mtx);
		sx_xlock(&ctx->sq_sx);
	}
	/* CQEs posted for an exited process: tell the others. */
	kqring_flush_ready(ctx);
	kqring_evfd_flush(ctx);
	return (true);
}

/*
 * The rest of a context's end, on kqring_teardown_tq once its process has
 * exited or exec'd (kqring_tctx_exit_locked()): after its kthreads, which
 * park during exec and leave only after it, are gone.  Standalone
 * TIMEOUTs it leaves armed fire into its taskwork, which later runs post
 * (every 100 ms); after the last, t leaves the ring.
 */
static void
kqring_tctx_exit_task(void *arg, int pending __unused)
{
	struct kqring_tctx *t = arg;
	struct kqring_ctx *ctx = t->ctx;
	struct kqring_op *op;
	bool done = false;

	if (!t->exit_started) {
		mtx_lock(&ctx->mtx);
		while (t->task_running) {
			wakeup(&ctx->wakeup_chan);
			(void)msleep(&t->task_running, &ctx->mtx, 0, "kqtxkt",
			    hz / 100 + 1);
		}
		mtx_unlock(&ctx->mtx);
	}
	sx_xlock(&ctx->sq_sx);
	if (!t->linked || ctx->closing) {
		/* The close destroys its requests; the ring's end frees t. */
		done = true;
	} else if (!t->exit_started) {
		t->exit_started = true;
		if (!kqring_tctx_cancel(ctx, t)) {
			done = true;
		} else {
			/* Its polls are gone: so is its poller. */
			mtx_lock(&ctx->mtx);
			while (kqring_kpoll_stop(ctx, t, true) != 0)
				(void)msleep(&t->kp_running, &ctx->mtx, 0,
				    "kqtxkp", hz / 100 + 1);
			mtx_unlock(&ctx->mtx);
			kqring_kpoll_close(t);
			kqring_tctx_close_wq(t);
		}
	} else {
		/* Expired TIMEOUTs it left. */
		mtx_lock(&ctx->mtx);
		while ((op = TAILQ_FIRST(&t->taskwork)) != NULL) {
			kqring_tw_remove_locked(ctx, op);
			op->in_taskwork = false;
			mtx_unlock(&ctx->mtx);
			kqring_tctx_exit_taskwork(ctx, op);
			mtx_lock(&ctx->mtx);
		}
		mtx_unlock(&ctx->mtx);
		kqring_evfd_flush(ctx);
	}
	if (!done && t->nops != 0) {
		/* TIMEOUTs still armed: look again later, keeping the refs. */
		sx_xunlock(&ctx->sq_sx);
		taskqueue_enqueue_timeout(kqring_teardown_tq, &t->exit_task,
		    hz / 10 + 1);
		return;
	}
	if (!done) {
		kqring_tctx_unlink(ctx, t);
		mtx_lock(&ctx->mtx);
		if (ctx->sleeping)
			wakeup(&ctx->wakeup_chan);
		mtx_unlock(&ctx->mtx);
	}
	sx_xunlock(&ctx->sq_sx);
	if (!done)
		kqring_tctx_rele(t);	/* the list's */
	kqring_tctx_rele(t);		/* kqring_tctx_exit_locked()'s */
	kqring_ctx_rele(ctx);
	kqring_worker_rele();
}

/*
 * t's process exits or execs: it is dead from here, so its threads find
 * no context on ctx (a new image that keeps the ring gets a new one), its
 * kthreads exit, and requests that become ready for it are cancelled.
 * The rest runs on kqring_teardown_tq: during exec its kthreads are parked
 * until this handler returns.  Takes refs on t and ctx for the task.
 * With kqring_proc_mtx and ctx->mtx.
 */
static void
kqring_tctx_exit_locked(struct kqring_ctx *ctx, struct kqring_tctx *t)
{
	mtx_assert(&kqring_proc_mtx, MA_OWNED);
	mtx_assert(&ctx->mtx, MA_OWNED);
	t->dead = true;
	if (ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
	refcount_acquire(&t->refs);
	refcount_acquire(&ctx->refs);
	/* kqring_worker_hold(), whose lock the caller holds: unload waits. */
	kqring_live_workers++;
	taskqueue_enqueue_timeout(kqring_teardown_tq, &t->exit_task, 0);
}

/*
 * The rest of teardown, entered from kqring_ctx_cancel_begin() with sq_sx
 * and ctx->mtx held; releases both.
 */
static void
kqring_ctx_cancel_locked(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *next;
	struct kqring_rcqe *rc;
	struct kqring_tctx *t;

	mtx_assert(&ctx->mtx, MA_OWNED);
	sx_assert(&ctx->sq_sx, SA_XLOCKED);
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
	/*
	 * Requests waiting to be issued in their own process
	 * (kqring_issue_elsewhere()), on no other list.
	 */
	LIST_FOREACH(t, &ctx->tctxs, link) {
		TAILQ_FOREACH_SAFE(op, &t->taskwork, taskwork_link, next) {
			if (!op->tw_issue)
				continue;
			kqring_tw_remove_locked(ctx, op);
			op->in_taskwork = false;
			op->tw_issue = false;
			mtx_unlock(&ctx->mtx);
			kqring_req_destroy(ctx, op);
			mtx_lock(&ctx->mtx);
			/* The list may have changed: start over. */
			next = TAILQ_FIRST(&t->taskwork);
		}
	}
	mtx_unlock(&ctx->mtx);
	LIST_FOREACH(t, &ctx->tctxs, link)
		kqring_tctx_close_wq(t);
	/* No poll is armed and the pollers are gone: close their kqueues. */
	kqring_kpoll_close_all(ctx);

	while ((op = TAILQ_FIRST(&ctx->ready_q)) != NULL) {
		TAILQ_REMOVE(&ctx->ready_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	while ((op = TAILQ_FIRST(&ctx->defer_q)) != NULL) {
		TAILQ_REMOVE(&ctx->defer_q, op, q_link);
		kqring_req_destroy(ctx, op);
	}
	/* Their polls and runs are gone: drop the triggers themselves. */
	kqring_trig_teardown(ctx);
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
	 * Drop fixed-file and registered-buffer tables with the ring.  Ring
	 * files can be neither passed over a socket nor held in a fixed
	 * table, so no reference cycle keeps a ring open.
	 */
	kqring_files_free(ctx);
	kqring_bufs_free(ctx);
	sx_xunlock(&ctx->sq_sx);

	mtx_lock(&ctx->mtx);
	ctx->cancel_td = NULL;
	ctx->cancelled = true;
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
	struct kqring_tctx *t;

	if (fp == NULL)
		return (ENXIO);
	ctx = malloc(sizeof(*ctx), M_KQRING, M_WAITOK | M_ZERO);
	refcount_init(&ctx->refs, 1);
	mtx_init(&ctx->mtx, "kqring", NULL, MTX_DEF);
	knlist_init_mtx(&ctx->rsel.si_note, &ctx->mtx);
	callout_init_mtx(&ctx->wq_abort_callout, &ctx->mtx, 0);
	TASK_INIT(&ctx->cancel_task, 0, kqring_ctx_cancel_task, ctx);
	sx_init(&ctx->bg_sx, "kqring bg");
	sx_init(&ctx->pipe_sx, "kqring pipe");
	sx_init_flags(&ctx->sq_sx, "kqring sq", SX_DUPOK);
	/* The opener's context; others come with other processes. */
	LIST_INIT(&ctx->tctxs);
	t = kqring_tctx_new(ctx, td, NULL);
	mtx_lock(&ctx->mtx);
	kqring_tctx_link_locked(ctx, t);
	mtx_unlock(&ctx->mtx);
	TAILQ_INIT(&ctx->retired_ops);
	TAILQ_INIT(&ctx->active_ops);
	TAILQ_INIT(&ctx->free_ops);
	TAILQ_INIT(&ctx->ready_q);
	TAILQ_INIT(&ctx->defer_q);
	LIST_INIT(&ctx->triggers);
	TAILQ_INIT(&ctx->trig_work);
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
		LIST_REMOVE(t, link);
		t->linked = false;
		kqring_tctx_rele(t);
		callout_drain(&ctx->wq_abort_callout);
		knlist_destroy(&ctx->rsel.si_note);
		sx_destroy(&ctx->sq_sx);
		sx_destroy(&ctx->bg_sx);
		sx_destroy(&ctx->pipe_sx);
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
	/* As io_uring: these say how an owner is told, which SQPOLL doesn't. */
	if ((p.flags & IORING_SETUP_SQPOLL) != 0 &&
	    (p.flags & (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
	    IORING_SETUP_DEFER_TASKRUN)) != 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_TASKRUN_FLAG) != 0 &&
	    (p.flags & (IORING_SETUP_COOP_TASKRUN |
	    IORING_SETUP_DEFER_TASKRUN)) == 0)
		return (EINVAL);
	/* One CQE size or both, one SQE size or both. */
	if ((p.flags & IORING_SETUP_CQE32) != 0 &&
	    (p.flags & IORING_SETUP_CQE_MIXED) != 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_SQE128) != 0 &&
	    (p.flags & IORING_SETUP_SQE_MIXED) != 0)
		return (EINVAL);
	/* SQ_REWIND reads SQEs from slot 0, not through the head and array. */
	if ((p.flags & IORING_SETUP_SQ_REWIND) != 0 &&
	    ((p.flags & IORING_SETUP_NO_SQARRAY) == 0 ||
	    (p.flags & IORING_SETUP_SQPOLL) != 0))
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
	kqring_tctx_rebind(ctx, td);

	if ((p.flags & IORING_SETUP_ATTACH_WQ) != 0) {
		struct kqring_ctx *wctx;
		struct kqring_tctx *wt;
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
		wt = kqring_tctx_cur(wctx);
		if (!wctx->setup_done || wctx->closing || wt == NULL ||
		    wt->wq == NULL || wt->wq->proc != td->td_proc) {
			mtx_unlock(&wctx->mtx);
			fdrop(wfp, td);
			return (EINVAL);
		}
		attach_wq = wt->wq;
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

	p.features = KQRING_FEATURES;

	kqring_ring_offsets(p.flags, sq_entries, cq_entries, &l, &p);

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
		old_wq = LIST_FIRST(&ctx->tctxs)->wq;
		LIST_FIRST(&ctx->tctxs)->wq = attach_wq;
		mtx_unlock(&ctx->mtx);
		attach_wq = old_wq;
		wq_swapped = true;
	}
	if (error == 0 && ctx->enabled && kqring_has_kthread(ctx))
		error = kqring_sqpoll_start(ctx, LIST_FIRST(&ctx->tctxs), td);
	if (error != 0) {
		if (wq_swapped) {
			struct kqring_wq *cur_wq;

			mtx_lock(&ctx->mtx);
			cur_wq = LIST_FIRST(&ctx->tctxs)->wq;
			LIST_FIRST(&ctx->tctxs)->wq = attach_wq;
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
	if (sx_xlock_sig(&ctx->sq_sx) != 0)
		return (EINTR);
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
	/* A process's own work, as kqring_run_taskwork(). */
	if (__predict_false(kqring_tctx_cur(ctx) == NULL))
		return;
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
	    (LIST_FIRST(&ctx->tctxs)->wq != NULL &&
	    LIST_FIRST(&ctx->tctxs)->wq->proc != td->td_proc));
}

/*
 * Interruptible (EINTR or ERESTART): a thread stopped while it holds sq_sx
 * (in the worker hand-off wait, say) must not leave others in an
 * uninterruptible wait for it, or the process never counts as stopped.
 */
static inline int
kqring_enter_lock(struct kqring_ctx *ctx, bool lock)
{
	return (lock ? sx_xlock_sig(&ctx->sq_sx) : 0);
}

static inline void
kqring_enter_unlock(struct kqring_ctx *ctx, bool lock)
{
	if (lock)
		sx_xunlock(&ctx->sq_sx);
}

static int kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, bool lock, bool foreign);

/*
 * The wait of an enter, with io_uring's min_wait_usec: wait up to min_sbt
 * for min_complete CQEs, then return if there are any, or else wait for
 * just one until the timeout (which counts from the start; one no later
 * than min_sbt ends the wait there).
 */
static int
kqring_enter_wait(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, sbintime_t min_sbt, bool lock,
    bool foreign)
{
	bool sqpoll = (ctx->flags & IORING_SETUP_SQPOLL) != 0;
	sbintime_t deadline;
	int error;

	if (min_sbt == 0 || min_complete <= 1)
		return (sqpoll ? kqring_sqpoll_wait_cqes(ctx, min_complete,
		    has_timeout, sbt) : kqring_wait_cqes(ctx, min_complete,
		    has_timeout, sbt, lock, foreign));
	deadline = has_timeout ? kqring_deadline(sbt) : 0;
	error = sqpoll ? kqring_sqpoll_wait_cqes(ctx, min_complete, true,
	    min_sbt) : kqring_wait_cqes(ctx, min_complete, true, min_sbt, lock,
	    foreign);
	if (error != ETIMEDOUT)
		return (error);
	if (has_timeout && sbt <= min_sbt)
		return (ETIMEDOUT);
	sbt = has_timeout ? kqring_until(deadline) : 0;
	if (has_timeout && sbt == 0)
		return (ETIMEDOUT);
	return (sqpoll ? kqring_sqpoll_wait_cqes(ctx, 1, has_timeout, sbt) :
	    kqring_wait_cqes(ctx, 1, has_timeout, sbt, lock, foreign));
}

/*
 * The wait with io_uring_enter(2)'s signal mask, as io_cqring_wait() and
 * ppoll(2) do it: if the CQ doesn't already hold min_complete CQEs, the
 * thread waits with usig as its mask (whose size is checked only then, as
 * in Linux).  A wait a signal ends leaves it for ast() to restore after the
 * handler has run; any other ends here.  IOPOLL rings ignore the mask, as
 * io_iopoll_check() does.
 */
static int
kqring_enter_wait_sig(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, sbintime_t min_sbt, bool lock,
    const void *usig, size_t sigsz, bool abs, bool foreign)
{
	struct kqring_shm_header *shm = ctx->shm;
	struct thread *td = curthread;
	sigset_t set;
	int error;

	/*
	 * IORING_ENTER_ABS_TIMER: sbt is a time of the ring's clock.  Both
	 * clocks IORING_REGISTER_CLOCK takes are the uptime here (FreeBSD
	 * defines CLOCK_BOOTTIME as CLOCK_MONOTONIC), so the wait is what is
	 * left until then.
	 */
	if (has_timeout && abs)
		sbt = kqring_until(sbt);
	if (usig == NULL || (ctx->flags & IORING_SETUP_IOPOLL) != 0 ||
	    atomic_load_acq_32(&shm->cq_tail) -
	    atomic_load_acq_32(&shm->cq_head) >= min_complete ||
	    !KQRING_STQ_EMPTY_HINT(&ctx->ocq)) {
		error = kqring_enter_wait(ctx, min_complete, has_timeout, sbt,
		    min_sbt, lock, foreign);
	} else {
		if (sigsz != sizeof(set))
			return (EINVAL);
		if ((error = copyin(usig, &set, sizeof(set))) != 0)
			return (error);
		error = kern_sigprocmask(td, SIG_SETMASK, &set,
		    &td->td_oldsigmask, 0);
		if (error != 0)
			return (error);
		td->td_pflags |= TDP_OLDMASK;
		error = kqring_enter_wait(ctx, min_complete, has_timeout, sbt,
		    min_sbt, lock, foreign);
		if (error == EINTR) {
			ast_sched(td, TDA_SIGSUSPEND);
		} else {
			td->td_pflags &= ~TDP_OLDMASK;
			kern_sigprocmask(td, SIG_SETMASK, &td->td_oldsigmask,
			    NULL, 0);
		}
	}
	/* A wait a signal ends with CQEs posted succeeds, as in io_uring. */
	if (error == EINTR && atomic_load_acq_32(&shm->cq_tail) !=
	    atomic_load_acq_32(&shm->cq_head))
		error = 0;
	return (error);
}

static int kqring_ioc_enter_body(struct kqring_ctx *ctx,
    struct kqring_enter_arg *ea, struct thread *td);

/*
 * An enter's timeout as sbintime_t: a duration, at least 1 if nonzero, or
 * with IORING_ENTER_ABS_TIMER a time (kqring_enter_wait_sig()).
 */
static sbintime_t
kqring_wait_ts_sbt(const struct timespec *ts, uint32_t flags)
{
	sbintime_t sbt = kqring_ts_to_sbt(ts);

	if (sbt == 0 && (ts->tv_sec > 0 || ts->tv_nsec > 0) &&
	    (flags & IORING_ENTER_ABS_TIMER) == 0)
		sbt = 1;
	return (sbt);
}

/*
 * An enter, then, if it woke pollers of the ring, their completions on
 * the process's other rings (kqring_kpoll_run()).
 */
static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	int error;

	kqring_wq_enter_count(1);
	error = kqring_ioc_enter_body(ctx, ea, td);
	kqring_wq_enter_count(-1);
	if (__predict_false(ctx->woke_pollers)) {
		ctx->woke_pollers = false;
		kqring_kpoll_run(td);
	}
	return (error);
}

static int
kqring_ioc_enter_body(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t submitted = 0;
	sbintime_t sbt = 0, min_sbt = 0;
	bool has_timeout = false;
	bool sqpoll = (ctx->flags & IORING_SETUP_SQPOLL) != 0;
	bool iopoll = (ctx->flags & IORING_SETUP_IOPOLL) != 0;
	bool lock = kqring_enter_locks(ctx, td);
	bool wait, foreign = false, owner;
	const void *usig = NULL;
	size_t sigsz = 0;
	int error = 0;

	if (!ctx->setup_done || !ctx->enabled || ctx->closing)
		return (EBADF);
	/* SQ_WAKEUP/SQ_WAIT need SQPOLL. */
	if ((ea->flags & ~KQRING_ENTER_FLAGS) != 0)
		return (EINVAL);
	if (!sqpoll && (ea->flags & (IORING_ENTER_SQ_WAKEUP |
	    IORING_ENTER_SQ_WAIT)) != 0)
		return (EINVAL);
	/* Without EXT_ARG, io_uring_enter(2)'s arg is a sigmask. */
	if ((ea->flags & IORING_ENTER_EXT_ARG) == 0) {
		usig = ea->arg;
		sigsz = ea->argsz;
	}

	wait = (ea->flags & IORING_ENTER_GETEVENTS) != 0 &&
	    (ea->min_complete != 0 || iopoll);
	/*
	 * On a SINGLE_ISSUER ring only the owner thread submits; another
	 * thread gets EEXIST.  On a shared ring each process that submits
	 * gets a context of its own, and its requests run in it
	 * (docs/concurrent-submitters.md); one that only waits needs none
	 * (foreign), and runs nothing of the others': their kthreads post
	 * their completions.  Not on an IOPOLL ring, whose waits do the
	 * polling.  With SQPOLL the kthread is the issuer, so any thread or
	 * process may wake it and wait, as in io_uring.
	 */
	if (sqpoll) {
		owner = true;
	} else if (ctx->shared) {
		mtx_lock(&ctx->mtx);
		owner = kqring_tctx_cur(ctx) != NULL;
		mtx_unlock(&ctx->mtx);
	} else {
		owner = kqring_is_owner(ctx, td);
	}
	if (!owner) {
		if (!ctx->shared) {
			/*
			 * SINGLE_ISSUER: another thread sees the CQ as it
			 * stands, and a wait it can't satisfy yet fails.
			 */
			if (ea->to_submit > 0 ||
			    ((ea->flags & IORING_ENTER_GETEVENTS) != 0 &&
			    (ctx->flags & IORING_SETUP_DEFER_TASKRUN) != 0) ||
			    (wait && (iopoll ||
			    atomic_load_acq_32(&shm->cq_tail) -
			    atomic_load_acq_32(&shm->cq_head) <
			    ea->min_complete)))
				return (EEXIST);
			ea->SUBMITTED_out = 0;
			return (0);
		}
		if (ea->to_submit > 0) {
			struct kqring_tctx *t;

			if ((error = kqring_tctx_attach(ctx, td, &t)) != 0)
				return (error);
		} else if (wait && iopoll) {
			return (EEXIST);
		} else {
			foreign = true;
		}
	}
	/* Waiting for events is when kevent(2) would look at a kqueue. */
	if (!foreign && (ea->flags & IORING_ENTER_GETEVENTS) != 0)
		kqring_kq_rescan(ctx);

	/*
	 * 1. Validate the optional IORING_ENTER_EXT_ARG before consuming
	 * any SQE, so that a bad argument fails the call without losing
	 * submissions.
	 */
	if ((ea->flags & (IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG)) ==
	    (IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG)) {
		/* A registered wait: arg is its offset in the region. */
		struct io_uring_reg_wait w;

		if (iopoll || ea->argsz != sizeof(w))
			return (EINVAL);
		error = kqring_reg_wait_get(ctx, (uintptr_t)ea->arg, &w);
		if (error != 0)
			return (error);
		if ((w.flags & ~(uint32_t)IORING_REG_WAIT_TS) != 0)
			return (EINVAL);
		usig = (const void *)(uintptr_t)w.sigmask;
		sigsz = w.sigmask_sz;
		min_sbt = (sbintime_t)w.min_wait_usec * SBT_1US;
		if (wait && (w.flags & IORING_REG_WAIT_TS) != 0) {
			struct timespec ts;

			if (w.ts.tv_sec < 0 || w.ts.tv_nsec < 0 ||
			    w.ts.tv_nsec >= 1000000000LL)
				return (EINVAL);
			ts.tv_sec = w.ts.tv_sec;
			ts.tv_nsec = (long)w.ts.tv_nsec;
			sbt = kqring_wait_ts_sbt(&ts, ea->flags);
			has_timeout = true;
		}
	} else if ((ea->flags & IORING_ENTER_EXT_ARG) != 0 && ea->arg != NULL) {
		struct io_uring_getevents_arg ext;
		if (ea->argsz != sizeof(ext))
			return (EINVAL);
		error = copyin(ea->arg, &ext, sizeof(ext));
		if (error != 0)
			return (error);
		usig = (const void *)(uintptr_t)ext.sigmask;
		sigsz = ext.sigmask_sz;
		min_sbt = (sbintime_t)ext.min_wait_usec * SBT_1US;
		if (wait && ext.ts != 0) {
			struct timespec ts;
			error = kqring_copyin_ts(ext.ts, &ts);
			if (error != 0)
				return (error);
			sbt = kqring_wait_ts_sbt(&ts, ea->flags);
			has_timeout = true;
		}
	}

	if (foreign) {
		ea->SUBMITTED_out = 0;
		if (!wait)
			return (0);
		return (kqring_enter_wait_sig(ctx, ea->min_complete,
		    has_timeout, sbt, min_sbt, false, usig, sigsz,
		    (ea->flags & IORING_ENTER_ABS_TIMER) != 0, true));
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
			if (kqring_enter_lock(ctx, true) != 0)
				return (EINTR);
			kqring_flush_overflow(ctx);
			sx_xunlock(&ctx->sq_sx);
		}
		if (!wait || ea->min_complete == 0)
			return (0);
		return (kqring_enter_wait_sig(ctx, ea->min_complete,
		    has_timeout, sbt, min_sbt, false, usig, sigsz,
		    (ea->flags & IORING_ENTER_ABS_TIMER) != 0, false));
	}

	/*
	 * Events a knote kicked the ring with become taskwork for step 3, so
	 * that an enter made on seeing IORING_SQ_TASKRUN finds what it was
	 * set for, whether or not it goes on to wait.
	 */
	if ((ea->flags & IORING_ENTER_GETEVENTS) != 0)
		kqring_kpoll_kicked(ctx);

	/*
	 * 2. Drain and submit queued SQEs.  Nothing is done yet: a stop
	 * restarts the call (ERESTART).
	 */
	if ((error = kqring_enter_lock(ctx, lock)) != 0)
		return (error);
	if (ctx->closing) {
		kqring_enter_unlock(ctx, lock);
		return (EBADF);
	}
	if (ea->to_submit > 0) {
		/* Cached file reads run here: kqring_wq_try_inline(). */
		ctx->submit_inline = kqring_wq_inline_ok(ctx);
		kqring_submit_sqes(ctx, ea->to_submit, &submitted);
		ctx->submit_inline = false;
		ea->SUBMITTED_out = submitted;
	} else {
		kqring_flush_overflow(ctx);
		ea->SUBMITTED_out = 0;
	}

	/* 3. Run pending DEFER_TASKRUN completions, futex and timeout checks */
	kqring_run_taskwork(ctx);
	kqring_run_checks(ctx);
	kqring_flush_overflow(ctx);
	/*
	 * NODROP: SQEs left unsubmitted because the CQ overflows fail the
	 * enter with EBUSY, as io_uring_enter(2) did; a dropped CQE fails
	 * the next enter that reaps with EOVERFLOW (Linux's EBADR), once.
	 */
	if (submitted == 0 && ea->to_submit > 0 &&
	    ctx->nocq >= KQRING_OCQ_BUSY &&
	    atomic_load_acq_32(&shm->sq_tail) != shm->sq_head) {
		kqring_enter_unlock(ctx, lock);
		return (EBUSY);
	}
	if (submitted == 0 && ctx->cq_dropped &&
	    (ea->flags & IORING_ENTER_GETEVENTS) != 0) {
		ctx->cq_dropped = false;
		kqring_enter_unlock(ctx, lock);
		return (EOVERFLOW);
	}
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
	error = kqring_enter_wait_sig(ctx, ea->min_complete, has_timeout, sbt,
	    min_sbt, lock, usig, sigsz,
	    (ea->flags & IORING_ENTER_ABS_TIMER) != 0, false);
	kqring_evfd_flush(ctx);
	if (error != 0 && submitted > 0)
		error = 0;
	return (error);
}

/*
 * Wait until the CQ holds min_complete entries (or overflowed), or until the
 * timeout expires or a signal arrives.  A foreign wait, from a process
 * other than the owner, only sleeps: the owner's work (taskwork, polls,
 * timeouts) is left to its kqtask thread.
 */
static int
kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt, bool lock, bool foreign)
{
	struct kqring_shm_header *shm = ctx->shm;
	/*
	 * Our taskwork, looked up once the CQ can't satisfy the wait (a
	 * foreign waiter, with no context, never looks).  A ring with one
	 * context needs no lock for it.
	 */
	struct kqring_tctx *mine = NULL;
	struct kqring_op_list *tw = NULL;
	bool tw_known = foreign;
	bool iopoll = (ctx->flags & IORING_SETUP_IOPOLL) != 0;
	bool may_poll = !foreign, deadline_set = false;
	sbintime_t spin = foreign ? 0 : ctx->busy_poll_sbt, deadline = 0;
	uint32_t timeouts = atomic_load_acq_32(&ctx->cq_timeouts);
	int error;

	for (;;) {
		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);

		/*
		 * A full CQ with overflow pending must be reaped first.  A
		 * TIMEOUT completing also ends the wait, as in io_uring.
		 */
		if (cq_count >= min_complete || !KQRING_STQ_EMPTY_HINT(&ctx->ocq) ||
		    atomic_load_acq_32(&ctx->cq_timeouts) != timeouts)
			return (0);

		if (!tw_known) {
			mine = ctx->shared ? kqring_tctx_cur_unlocked(ctx) :
			    kqring_tctx_cur(ctx);
			tw = mine != NULL ? &mine->taskwork : NULL;
			if (tw == NULL)
				spin = 0;
			tw_known = true;
		}

		/* The timeout counts from the first wait, however often woken. */
		if (has_timeout) {
			if (!deadline_set) {
				deadline = kqring_deadline(sbt);
				deadline_set = true;
			} else {
				sbt = kqring_until(deadline);
			}
		}

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
				    !TAILQ_EMPTY(tw) ||
				    !KQRING_STQ_EMPTY_HINT(&ctx->rcq))
					break;
				cpu_spinwait();
				now = getsbinuptime();
			}
			spin = 0;
			if (!TAILQ_EMPTY(tw) ||
			    !KQRING_STQ_EMPTY_HINT(&ctx->rcq))
				goto drain;
			continue;
		}

		/* Tell an eventfd watcher about CQEs before sleeping. */
		if (!foreign)
			kqring_evfd_flush(ctx);
		mtx_lock(&ctx->mtx);
		if (foreign && ctx->closing) {
			mtx_unlock(&ctx->mtx);
			return (EBADF);
		}
		/* Poll at most once per sleep so EAGAIN can't spin us. */
		if (ctx->npolled > 0 && may_poll) {
			kqring_poll_fallback_locked(ctx);
			may_poll = false;
		}
		if (!foreign && !TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if (!foreign && ((tw != NULL && !TAILQ_EMPTY(tw)) ||
		    !KQRING_STQ_EMPTY_HINT(&ctx->rcq) ||
		    (mine != NULL &&
		    atomic_load_acq_int(&mine->kp_kick) != 0))) {
			mtx_unlock(&ctx->mtx);
			goto drain;
		}
		/*
		 * A ring's kthread may have posted CQEs since the check above;
		 * it wakes waiters only under mtx, so look again here.
		 */
		cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= min_complete || !KQRING_STQ_EMPTY_HINT(&ctx->ocq) ||
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

		/*
		 * Timeout is sbintime_t, not ticks: use the _sbt variant.  The
		 * CQEs still wanted let plain completions batch their wake-ups
		 * (kqring_taskwork_add_lazy_locked()); our taskwork is empty.
		 */
		ctx->sleeping++;
		ctx->cq_sleepers++;
		ctx->cq_wait_nr = min_complete - cq_count;
		ctx->tw_lazy = 0;
		/* As precise as the io_uring_enter(2) timeout (an hrtimer). */
		error = msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
		    "kqwait", sleep_sbt, 0, C_PREL(10));
		/* A waiter still asleep may have needed less: wake it each time. */
		if (--ctx->cq_sleepers != 0)
			ctx->cq_wait_nr = 1;
		ctx->sleeping--;
		may_poll = !foreign;
		mtx_unlock(&ctx->mtx);

		if (error == EINTR || error == ERESTART)
			return (EINTR);
		if (error == EWOULDBLOCK && has_timeout) {
			if (kqring_until(deadline) == 0) {
				if (foreign) {
					/* Nothing of the owner's to run. */
				} else if (kqring_enter_lock(ctx, lock) != 0) {
					return (EINTR);
				} else {
					if (!ctx->closing) {
						kqring_run_taskwork(ctx);
						kqring_run_checks(ctx);
						kqring_flush_overflow(ctx);
					}
					kqring_enter_unlock(ctx, lock);
				}
				cq_count = atomic_load_acq_32(&shm->cq_tail) -
				    atomic_load_acq_32(&shm->cq_head);
				if (cq_count > 0 || !KQRING_STQ_EMPTY_HINT(&ctx->ocq))
					return (0);
				return (ETIMEDOUT);
			}
		}
		if (foreign)
			continue;
drain:
		/* Events a knote woke us for (kqring_kp_filt_event()). */
		kqring_kpoll_kicked(ctx);
		if (kqring_enter_lock(ctx, lock) != 0)
			return (EINTR);
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

/*
 * Whether p's threads run ctx's work: its owner (a ring with one process),
 * or a process whose pool its context uses (IORING_SETUP_ATTACH_WQ).
 * Under ctx->mtx.
 */
static bool
kqring_ctx_runs_in(struct kqring_ctx *ctx, const struct proc *p)
{
	struct kqring_tctx *t;

	if (ctx->owner_proc == p)
		return (true);
	LIST_FOREACH(t, &ctx->tctxs, link) {
		if (t->wq != NULL && t->wq->proc == p)
			return (true);
	}
	return (false);
}

/*
 * A set-ID exec closes the rings the process holds: the new image, with
 * other credentials, must not inherit personalities, registered files or
 * restrictions made by the old one (docs/concurrent-submitters.md).
 */
static void
kqring_proc_setid_close(struct proc *p)
{
	struct filedesc *fdp = p->p_fd;
	struct kqring_ctx *ctx;
	struct file *fp;
	int fd, last;

	if (fdp == NULL)
		return;
	for (;;) {
		ctx = NULL;
		FILEDESC_SLOCK(fdp);
		for (fd = 0, last = fdlastfile(fdp); fd <= last; fd++) {
			if ((fp = fdp->fd_ofiles[fd].fde_file) == NULL)
				continue;
			if (kqring_fp_is_ring(fp) &&
			    !atomic_load_bool(&((struct kqring_ctx *)
			    fp->f_data)->closing)) {
				ctx = fp->f_data;
				refcount_acquire(&ctx->refs);
				break;
			}
		}
		FILEDESC_SUNLOCK(fdp);
		if (ctx == NULL)
			break;
		/* Marks it closing first, so the loop moves on. */
		kqring_ctx_cancel_all(ctx);
		kqring_ctx_rele(ctx);
	}
}

/*
 * Process exit or exec (docs/concurrent-submitters.md).  On a ring other
 * processes may use, the process's context ends: its requests complete
 * with CQEs and the ring stays open for whoever holds it.  A ring with one
 * process (SINGLE_ISSUER, SQPOLL, kqring's extensions) closes when that
 * process exits or execs, as do rings whose workers run in it.
 */
static void
kqring_proc_rings_teardown(struct proc *p, bool setid)
{
	struct kqring_ctx *ctx;
	struct kqring_tctx *t;
	bool close;

	kqring_proc_clear(p);
	if (setid)
		kqring_proc_setid_close(p);
	for (;;) {
		close = false;
		mtx_lock(&kqring_proc_mtx);
		LIST_FOREACH(ctx, &kqring_ctx_list, proc_link) {
			if (ctx->closing)
				continue;
			mtx_lock(&ctx->mtx);
			if (kqring_ctx_multiproc(ctx)) {
				t = kqring_tctx_find(ctx, p);
				if (t != NULL)
					kqring_tctx_exit_locked(ctx, t);
				mtx_unlock(&ctx->mtx);
				continue;
			}
			close = kqring_ctx_runs_in(ctx, p);
			if (close && ctx->owner_proc == p)
				kqring_clear_owner(ctx);
			mtx_unlock(&ctx->mtx);
			if (close) {
				ctx->in_ctx_list = false;
				LIST_REMOVE(ctx, proc_link);
				refcount_acquire(&ctx->refs);
				break;
			}
		}
		mtx_unlock(&kqring_proc_mtx);
		if (ctx == NULL)
			break;
		kqring_ctx_cancel_all(ctx);
		kqring_ctx_rele(ctx);
	}
}

static void
kqring_proc_exit(void *arg __unused, struct proc *p)
{
	kqring_proc_rings_teardown(p, false);
}

static void
kqring_proc_exec(void *arg __unused, struct proc *p,
    struct image_params *imgp)
{
	kqring_proc_rings_teardown(p, imgp->credential_setid);
}

/*
 * p1 forked p2, in p1, after copying its address space and before p2
 * runs.  Pinned user memory p1 shares with its rings (provided buffer
 * rings, the registered wait region) is held again on next use
 * (ctx->cow_gen).  Registered buffers p1 registered are held again now
 * (kqring_bufs_fork()), unless p2 shares the address space (vfork(2)).
 */
static void
kqring_proc_fork(void *arg __unused, struct proc *p1,
    struct proc *p2, int flags __unused)
{
	struct kqring_fork_ring {
		struct kqring_ctx	*ctx;
		uint64_t		tid;
	} *v = NULL;
	struct kqring_ctx *ctx;
	struct kqring_tctx *t;
	bool rehold = curproc == p1 && p2->p_vmspace != p1->p_vmspace;
	u_int max = 0, n = 0;

	for (int pass = 0; pass < 2; pass++) {
		mtx_lock(&kqring_proc_mtx);
		LIST_FOREACH(ctx, &kqring_ctx_list, proc_link) {
			if (ctx->closing)
				continue;
			mtx_lock(&ctx->mtx);
			t = kqring_tctx_find(ctx, p1);
			if (pass == 0 && (t != NULL ||
			    kqring_ctx_runs_in(ctx, p1)))
				atomic_add_int(&ctx->cow_gen, 1);
			if (rehold && t != NULL && ctx->bufs != NULL) {
				if (pass == 0) {
					max++;
				} else if (n < max) {
					refcount_acquire(&ctx->refs);
					v[n].ctx = ctx;
					v[n++].tid = t->id;
				}
			}
			mtx_unlock(&ctx->mtx);
		}
		mtx_unlock(&kqring_proc_mtx);
		if (max == 0)
			return;
		if (pass == 0)
			v = malloc(max * sizeof(*v), M_KQRING, M_WAITOK);
	}
	for (u_int i = 0; i < n; i++) {
		kqring_bufs_fork(v[i].ctx, v[i].tid, curthread);
		kqring_ctx_rele(v[i].ctx);
	}
	free(v, M_KQRING);
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

		/* No ring: <sys/kqring.h> sends these to /dev/kqring. */
		if (ra->ring_fd == -1 && !ctx->setup_done &&
		    (ra->opcode & IORING_REGISTER_USE_REGISTERED_RING) == 0) {
			error = kqring_register_blind(ra);
			break;
		}
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
	case KQRING_IOC_EXT_SETUP:
		error = kqring_ext_setup(ctx, (struct kqring_ext_setup *)data);
		break;
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

	if (uap->fd == -1 &&
	    (uap->opcode & IORING_REGISTER_USE_REGISTERED_RING) == 0) {
		td->td_retval[0] = 0;
		return (kqring_register_blind(&ra));
	}
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
		/*
		 * Enter counts itself (kqring_sys_busy), so it is registered
		 * SY_THR_STATIC: the slot goes from absent to static in one
		 * store, and the kernel never counts a call to it.  (A
		 * counted call can't be made static later: a thread may have
		 * read it as counted and not yet counted itself.)
		 */
		kqring_sysnum[i] = NO_SYSCALL;
		if (kern_syscall_register(sysent, &kqring_sysnum[i],
		    &kqring_sysent[i], &kqring_old_sysent[i],
		    i == KQRING_SYS_ENTER ? SY_THR_STATIC : 0) != 0) {
			printf("kqring: no free system call slot\n");
			kqring_sysnum[i] = NO_SYSCALL;
		}
	}
}

/*
 * After deregistration a thread that took one of these calls' sysent
 * entries before can still be about to enter kqring's code, or just
 * leaving it, a few instructions outside kqring_sys_busy.  Such a thread's
 * current system call (td_sa.code) is that call until it next returns to
 * user space, through ast(), which clears td_ast.  So mark each thread
 * whose current call is one of ours with an AST (TDA_AST, which only makes
 * ast() run) and wait until each has cleared it or is gone: then none can
 * be in the module.  A thread that doesn't return to user space within
 * 10 s (its process is stopped) fails the unload with EBUSY.
 */
static int
kqring_sys_drain_threads(const int *nums)
{
	struct kqring_drain { pid_t pid; lwpid_t tid; } *w = NULL;
	struct proc *p;
	struct thread *td;
	int n, max = 0, error = 0;

again:
	n = 0;
	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		PROC_LOCK(p);
		FOREACH_THREAD_IN_PROC(p, td) {
			bool ours = false;

			for (int i = 0; i < KQRING_NSYS; i++)
				ours |= nums[i] != NO_SYSCALL &&
				    td->td_sa.code == (u_int)nums[i];
			if (!ours || td == curthread)
				continue;
			if (n < max) {
				thread_lock(td);
				ast_sched_locked(td, TDA_AST);
				thread_unlock(td);
				w[n].pid = p->p_pid;
				w[n].tid = td->td_tid;
			}
			n++;
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);
	if (n > max) {
		free(w, M_KQRING);
		max = n + 64;
		w = malloc(max * sizeof(*w), M_KQRING, M_WAITOK);
		goto again;
	}
	for (int ticks_left = 10 * hz;; ticks_left--) {
		int left = 0;

		for (int i = 0; i < n; i++) {
			bool pending;

			if (w[i].tid == 0)
				continue;
			td = tdfind(w[i].tid, w[i].pid);
			if (td == NULL) {
				w[i].tid = 0;
				continue;
			}
			thread_lock(td);
			pending = (td->td_ast & TDAI(TDA_AST)) != 0;
			thread_unlock(td);
			PROC_UNLOCK(td->td_proc);
			if (pending)
				left++;
			else
				w[i].tid = 0;
		}
		if (left == 0)
			break;
		if (ticks_left == 0) {
			error = EBUSY;
			break;
		}
		pause("kqsysdr", 1);
	}
	free(w, M_KQRING);
	return (error);
}

/*
 * Waits for the system calls in progress, as destroy_dev() for ioctls;
 * EBUSY (all deregistered) if some thread may not have left them.
 */
static int
kqring_sys_deregister_all(void)
{
	int nums[KQRING_NSYS];
	u_int cpu, n;

	/*
	 * Count enter calls in the sysent entry again, so that
	 * kern_syscall_deregister() drains those that start from now on.
	 */
	if (kqring_sysnum[KQRING_SYS_ENTER] != NO_SYSCALL)
		atomic_store_rel_32(
		    &sysent[kqring_sysnum[KQRING_SYS_ENTER]].sy_thrcnt, 0);
	for (int i = 0; i < KQRING_NSYS; i++) {
		nums[i] = kqring_sysnum[i];
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
	 * And for any thread between the sysent dispatch and its count, or
	 * between the count and its return.
	 */
	return (kqring_sys_drain_threads(nums));
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
	if ((*offset & IORING_OFF_MMAP_MASK) == KQRING_OFF_PARAM_REGION) {
		/* A module-allocated region; the caller's isn't mapped. */
		error = EINVAL;
		mtx_lock(&ctx->mtx);
		if (ctx->region_obj != NULL && size <= ctx->region_len) {
			vm_object_reference(ctx->region_obj);
			*object = ctx->region_obj;
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
			TAILQ_INIT(&kqring_futex_hash[i]);
		for (int i = 0; i < KQRING_PROC_HASH; i++)
			LIST_INIT(&kqring_proc_hash[i]);
		kqring_teardown_tq = taskqueue_create("kqring teardown",
		    M_WAITOK, taskqueue_thread_enqueue, &kqring_teardown_tq);
		taskqueue_start_threads(&kqring_teardown_tq, 1, PWAIT,
		    "kqteardown");
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
			taskqueue_free(kqring_teardown_tq);
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
		if (kqring_sys_deregister_all() != 0) {
			kqring_sys_register_all();
			mtx_lock(&kqring_proc_mtx);
			kqring_unloading = false;
			mtx_unlock(&kqring_proc_mtx);
			return (EBUSY);
		}
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
		/* Deferred teardowns held workers: none is left to run. */
		taskqueue_free(kqring_teardown_tq);
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
