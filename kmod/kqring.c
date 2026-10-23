/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring.c: FreeBSD Kernel Module implementing the CVE-free SINGLE_ISSUER |
 * DEFER_TASKRUN io_uring subset + Provided Buffer Rings (PBUF_RING).
 *
 * Architectural Summary:
 *   - Exposes /dev/kqring with per-descriptor state via devfs_set_cdevpriv(9).
 *   - Maps SQ/CQ rings and SQE arrays via .d_mmap_single backed by wired
 *     anonymous VM objects (vm_pager_allocate + pmap_qenter), preventing any
 *     use-after-free across munmap(2) or close(2).
 *   - Enforces SINGLE_ISSUER + DEFER_TASKRUN:
 *       * Socket readiness callbacks (soupcall_set) never touch user memory
 *         or PBUF_RINGs from interrupt/netisr context. They only move the
 *         kqring_op onto the ring's taskwork list, set IORING_SQ_TASKRUN in
 *         the shared SQ flags word, and wake the sleeping ring owner.
 *       * When the owning thread enters KQRING_IOC_ENTER, it drains the
 *         taskwork list in its own process context (curthread): selecting a
 *         buffer from PBUF_RING, calling soreceive()/kern_accept4() with
 *         UIO_USERSPACE, and posting CQEs to the shared CQ ring.
 *       * On EOF (bytes == 0), any selected PBUF_RING buffer is NOT consumed,
 *         so cqe->res == 0 with no leaked buffer ID.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/callout.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/unistd.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/filio.h>
#include <sys/capsicum.h>
#include <sys/fcntl.h>
#include <sys/extattr.h>
#include <sys/mbuf.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/umtxvar.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/atomic.h>

#ifdef KQRING_MAC
#include <security/mac/mac_framework.h>
#endif

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_param.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <sys/kqring.h>

MALLOC_DEFINE(M_KQRING, "kqring", "FreeBSD kqring I/O ring state");

#define KQRING_MAX_SQ_ENTRIES	4096U
#define KQRING_MAX_CQ_ENTRIES	16384U
#define KQRING_MAX_PBUF_ENTRIES	32768U
/* CQEs held in the kernel while the CQ ring is full (IORING_FEAT_NODROP). */
#define KQRING_MAX_OVERFLOW	16384U
/* MSG_RING CQEs queued for a ring's owner and not yet posted. */
#define KQRING_MAX_REMOTE	16384U
/*
 * Kernel memory a ring may hold in SQE arguments copied at submission
 * (paths, iovecs, msghdrs, xattr values, futex vectors) for requests that
 * have not completed; past it submission fails with -ENOMEM.
 */
#define KQRING_MAX_ARG_BYTES	(32U * 1024 * 1024)

/* io_uring_params flags accepted by KQRING_IOC_SETUP. */
#define	KQRING_SETUP_FLAGS						\
	(IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP |			\
	IORING_SETUP_R_DISABLED | IORING_SETUP_SUBMIT_ALL |		\
	IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |		\
	IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN)

/* KQRING_IOC_ENTER flags implemented. */
#define	KQRING_ENTER_FLAGS	(IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG)

/*
 * Layout of the single-mmap SQ+CQ shared memory region (ctx->ring_kva).
 */
struct kqring_shm_header {
	uint32_t	sq_head;
	uint32_t	sq_tail;
	uint32_t	sq_ring_mask;
	uint32_t	sq_ring_entries;
	uint32_t	sq_flags;
	uint32_t	sq_dropped;
	uint32_t	cq_head;
	uint32_t	cq_tail;
	uint32_t	cq_ring_mask;
	uint32_t	cq_ring_entries;
	uint32_t	cq_overflow;
	uint32_t	cq_flags;
	uint32_t	reserved[4];
	/* Followed by:
	 *   uint32_t sq_array[sq_entries]
	 *   struct io_uring_cqe cqes[cq_entries]
	 */
};

_Static_assert(sizeof(struct kqring_shm_header) == 64, "shm header is 64 bytes");

/*
 * A request: one submitted SQE, from submission until its final CQE.
 *
 * A request is in exactly one of these states, all owned by the ring owner
 * thread:
 *   - linked:   waiting in its predecessor's ->link chain (IOSQE_IO_LINK);
 *   - deferred: a chain head on ctx->defer_q behind an IOSQE_IO_DRAIN;
 *   - ready:    on ctx->ready_q, to be issued by kqring_flush_ready();
 *   - armed:    on ctx->active_ops, waiting for a socket upcall, a futex
 *               change or a timer (->armed);
 *   - a LINK_TIMEOUT guarding an armed request (its ->ltimeout);
 * or transiently executing in kqring_req_issue().  Upcalls and callouts
 * never complete requests themselves: they only queue them as taskwork.
 *
 * Holds soref(so) (not fhold(fp)) while armed on a socket so that user-space
 * close(fd) still triggers soclose() + TCP FIN immediately and fires our
 * soupcall with SBS_CANTRCVMORE.
 */
struct kqring_op {
	TAILQ_ENTRY(kqring_op)	active_link;	/* active_ops or free_ops */
	TAILQ_ENTRY(kqring_op)	taskwork_link;
	TAILQ_ENTRY(kqring_op)	q_link;		/* ready_q or defer_q */
	struct kqring_ctx	*ctx;
	struct socket		*so;		/* soref'd while armed */
	int			dirs;		/* KQ_DIR_* upcalls installed */
	bool			listening;	/* armed via solisten_upcall_set */
	bool			in_taskwork;
	bool			cancelled;	/* no longer armed; under mtx */
	bool			polled;		/* no upcall; see npolled */
	bool			armed;		/* on active_ops */
	bool			drain;		/* chain has IOSQE_IO_DRAIN */
	bool			timer_init;	/* callout initialized */
	bool			timer_pending;	/* callout scheduled; under mtx */
	int			prep_err;	/* SQE rejected at submission */
	uint32_t		nchain;		/* chain head: requests in chain */
	int32_t			res;		/* result of the last attempt */
	uint32_t		cflags;		/* CQE flags of the last attempt */
	struct kqring_op	*link;		/* next request in the chain */
	struct kqring_op	*ltimeout;	/* LINK_TIMEOUT guarding this */
	struct kqring_op	*ltarget;	/* LINK_TIMEOUT: request guarded */
	/* TIMEOUT / LINK_TIMEOUT state */
	struct callout		callout;	/* uses ctx->mtx */
	struct timespec		ts;
	uint32_t		target_seq;	/* completion-count TIMEOUT */
	uint32_t		repeats;	/* multishot: left, 0 = forever */
	bool			seq_timeout;	/* counted in ctx->nseqto */
	/*
	 * SQE arguments copied in at submission (IORING_FEAT_SUBMIT_STABLE):
	 * a request that runs later (linked, drained, armed) never reads
	 * user memory the SQE pointed to, except for output buffers.
	 */
	void			*args;
	size_t			args_len;
	size_t			done_io;	/* MSG_WAITALL: bytes so far */
	bool			sock_rw;	/* READ/WRITE(V) on a socket */
	bool			connecting;	/* CONNECT: soconnect() issued */
	bool			futex_woken;	/* FUTEX_WAKE hit; res in ->res */
	uint32_t		nfwait;		/* FUTEX_WAIT(V) nodes in args */
	struct io_uring_sqe	sqe;
};

TAILQ_HEAD(kqring_op_list, kqring_op);

/*
 * An armed FUTEX_WAIT, or one word of an armed FUTEX_WAITV, hashed by its
 * umtx key in kqring_futex_hash so that FUTEX_WAKE from any ring (or
 * process, for shared futexes) finds it.  Lock order: kqring_futex_mtx,
 * then ctx->mtx.
 */
struct kqring_fwait {
	LIST_ENTRY(kqring_fwait) link;
	struct kqring_op	*op;
	struct umtx_key		key;
	uint64_t		uaddr;
	uint64_t		val;
	uint32_t		mask;
	uint32_t		flags;		/* FUTEX2_* */
	uint32_t		idx;		/* FUTEX_WAITV index */
	bool			keyed;		/* key holds a reference */
	bool			hashed;		/* on kqring_futex_hash */
};

#define	KQRING_FUTEX_HASH	256
static LIST_HEAD(kqring_fwait_list, kqring_fwait)
    kqring_futex_hash[KQRING_FUTEX_HASH];
static struct mtx kqring_futex_mtx;

/* A MSG_RING CQE queued for the target ring's owner to post. */
struct kqring_rcqe {
	STAILQ_ENTRY(kqring_rcqe) link;
	struct io_uring_cqe	cqe;
};

#define	KQ_DIR_RCV	0x1
#define	KQ_DIR_SND	0x2

/* A CQE that did not fit in the CQ ring, waiting for userspace to reap. */
struct kqring_ocqe {
	STAILQ_ENTRY(kqring_ocqe) link;
	struct io_uring_cqe	cqe;
};

/*
 * Provided Buffer Ring (group 0) registered via IORING_REGISTER_PBUF_RING.
 */
struct kqring_pbuf_group {
	bool		registered;
	uint64_t	user_ring_addr;
	uint32_t	ring_entries;
	uint32_t	ring_mask;
	uint16_t	head;		/* kernel consumer index */
};

/*
 * Per-open /dev/kqring context, attached to the file descriptor via
 * devfs_set_cdevpriv(9).
 */
struct kqring_ctx {
	struct mtx		mtx;
	bool			setup_busy;	/* KQRING_IOC_SETUP running */
	bool			setup_done;
	bool			enabled;
	bool			closing;
	struct thread		*owner_td;	/* SINGLE_ISSUER owner */

	uint32_t		flags;
	uint32_t		sq_entries;
	uint32_t		cq_entries;
	uint32_t		sq_mask;
	uint32_t		cq_mask;

	/* Shared memory regions (wired VM objects + KVA mappings) */
	vm_object_t		ring_obj;
	vm_offset_t		ring_kva;
	size_t			ring_alloc_len;
	struct kqring_shm_header *shm;
	uint32_t		*sq_array;
	struct io_uring_cqe	*cqes;

	vm_object_t		sqes_obj;
	vm_offset_t		sqes_kva;
	size_t			sqes_alloc_len;
	struct io_uring_sqe	*sqes;

	/* Provided buffer group 0 */
	struct kqring_pbuf_group pbuf;

	/* Armed operations and deferred taskwork queue */
	struct kqring_op_list	active_ops;
	struct kqring_op_list	taskwork_ops;
	int			nfutex;		/* armed FUTEX_WAIT(V) ops */
	int			nseqto;		/* armed completion-count TIMEOUTs */
	int			nwaitid;	/* armed WAITID ops */
	size_t			args_bytes;	/* sum of live ops' args_len */
	/*
	 * MSG_RING CQEs from other rings (or this one), posted by the owner
	 * in kqring_run_taskwork().  Under mtx.
	 */
	STAILQ_HEAD(, kqring_rcqe) rcq;
	uint32_t		nrcq;
	/*
	 * Requests to issue (link successors) and IOSQE_IO_DRAIN-deferred
	 * chains.  Owner thread only.  nreqs counts every live request,
	 * ndeferred those on defer_q, so nreqs - ndeferred are in flight.
	 */
	struct kqring_op_list	ready_q;
	struct kqring_op_list	defer_q;
	uint32_t		nreqs;
	uint32_t		ndeferred;
	bool			drain_barrier;	/* a drained chain is in flight */
	uint32_t		cq_seq;		/* CQEs posted (not by TIMEOUTs) */
	/*
	 * CQ overflow list.  Only the owner thread (enter) and kqring_dtor()
	 * touch it, so no lock is needed.
	 */
	STAILQ_HEAD(, kqring_ocqe) ocq;
	uint32_t		nocq;
	/*
	 * Armed SO_SND ops on AF_UNIX stream sockets, whose writer wakeup
	 * never calls the so_snd upcall; the wait loop polls them.
	 */
	int			npolled;
	int			wakeup_chan;
	bool			sleeping;	/* owner in msleep; under mtx */
	/*
	 * Recycled ops (linked via active_link).  Only touched by the owner
	 * thread (enter) and by kqring_dtor(), so no lock is needed.
	 */
	struct kqring_op_list	free_ops;
	int			nfree;
};

#define	KQRING_OP_CACHE_MAX	32

static struct cdev *kqring_cdev;

/* Forward declarations */
static d_open_t		kqring_open;
static d_ioctl_t	kqring_ioctl;
static d_mmap_single_t	kqring_mmap_single;

static struct cdevsw kqring_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	kqring_open,
	.d_ioctl =	kqring_ioctl,
	.d_mmap_single = kqring_mmap_single,
	.d_name =	"kqring",
};

static void kqring_op_disarm_locked(struct kqring_op *op);
static void kqring_op_free(struct kqring_op *op);
static int  kqring_soupcall(struct socket *so, void *arg, int waitflag);
static void kqring_req_complete(struct kqring_ctx *ctx, struct kqring_op *op,
    int32_t res, uint32_t cflags);
static void kqring_req_issue(struct kqring_ctx *ctx, struct kqring_op *op);

/* ============================================================================
 * Shared Memory Allocation (Wired VM Objects + Kernel Virtual Address Mapping)
 * ============================================================================
 */

static int
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

static void
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
kqring_cq_push(struct kqring_ctx *ctx, const struct io_uring_cqe *src)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t tail, head;

	head = atomic_load_acq_32(&shm->cq_head);
	tail = shm->cq_tail;
	if (tail - head >= ctx->cq_entries)
		return (false);

	ctx->cqes[tail & ctx->cq_mask] = *src;
	atomic_store_rel_32(&shm->cq_tail, tail + 1);
	return (true);
}

/* Move overflowed CQEs into the CQ ring as userspace frees up space. */
static void
kqring_flush_overflow(struct kqring_ctx *ctx)
{
	struct kqring_ocqe *ocqe;

	if (STAILQ_EMPTY(&ctx->ocq))
		return;
	while ((ocqe = STAILQ_FIRST(&ctx->ocq)) != NULL) {
		if (!kqring_cq_push(ctx, &ocqe->cqe))
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
static void
kqring_post_cqe_uncounted(struct kqring_ctx *ctx, uint64_t user_data,
    int32_t res, uint32_t flags)
{
	struct io_uring_cqe cqe = {
		.user_data = user_data,
		.res = res,
		.flags = flags,
	};
	struct kqring_ocqe *ocqe;

	if (STAILQ_EMPTY(&ctx->ocq) && kqring_cq_push(ctx, &cqe))
		return;

	if (ctx->nocq >= KQRING_MAX_OVERFLOW) {
		atomic_add_32(&ctx->shm->cq_overflow, 1);
		return;
	}
	ocqe = malloc(sizeof(*ocqe), M_KQRING, M_WAITOK);
	ocqe->cqe = cqe;
	STAILQ_INSERT_TAIL(&ctx->ocq, ocqe, link);
	ctx->nocq++;
	atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_CQ_OVERFLOW);
}

static void
kqring_post_cqe(struct kqring_ctx *ctx, uint64_t user_data, int32_t res,
    uint32_t flags)
{
	ctx->cq_seq++;
	kqring_post_cqe_uncounted(ctx, user_data, res, flags);
}

/*
 * Peek at the next available buffer in group 0's PBUF_RING without advancing
 * head yet. Only called in curthread context during KQRING_IOC_ENTER.
 * If the receive succeeds with > 0 bytes, kqring_pbuf_commit() consumes it.
 * If the receive hits EOF (0 bytes) or fails (< 0), head is NOT advanced,
 * preventing the EOF buffer-leak bug!
 */
static int
kqring_pbuf_peek(struct kqring_ctx *ctx, struct io_uring_buf *buf_out)
{
	struct kqring_pbuf_group *pb = &ctx->pbuf;
	uint16_t user_tail;
	uintptr_t tail_uaddr, entry_uaddr;
	uint32_t idx;
	int32_t tailv;
	int error;

	if (!pb->registered)
		return (ENOBUFS);

	/* tail lives at byte offset 14 of struct io_uring_buf_ring */
	tail_uaddr = (uintptr_t)pb->user_ring_addr +
	    offsetof(struct io_uring_buf_ring, tail);
	/* fuword16() is cheaper than copyin() for 2 bytes; -1 means fault. */
	tailv = fuword16((const void *)tail_uaddr);
	if (tailv == -1)
		return (EFAULT);
	user_tail = (uint16_t)tailv;

	if (pb->head == user_tail)
		return (ENOBUFS);

	idx = pb->head & pb->ring_mask;
	entry_uaddr = (uintptr_t)pb->user_ring_addr +
	    idx * sizeof(struct io_uring_buf);
	error = copyin((const void *)entry_uaddr, buf_out, sizeof(*buf_out));
	if (error != 0)
		return (error);

	return (0);
}

static inline void
kqring_pbuf_commit(struct kqring_ctx *ctx)
{
	ctx->pbuf.head++;
}

/* ============================================================================
 * Socket Upcall (DEFER_TASKRUN notification)
 * ============================================================================
 *
 * Runs with the socket buffer lock (or, for listening sockets, the listen
 * lock) held when data arrives, space opens, a connection is queued, or a
 * peer closes.
 * Never touches user memory: simply moves `op` onto ctx->taskwork_ops, sets
 * IORING_SQ_TASKRUN in shm->sq_flags, and wakes the owner if sleeping.
 */
static void
kqring_taskwork_add_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->cancelled || ctx->closing || op->in_taskwork)
		return;
	op->in_taskwork = true;
	TAILQ_INSERT_TAIL(&ctx->taskwork_ops, op, taskwork_link);
	if (ctx->shm != NULL)
		atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
	/*
	 * ctx->sleeping is only changed under ctx->mtx, and the owner
	 * re-checks taskwork_ops under ctx->mtx before sleeping, so
	 * skipping wakeup() (sleepq lookup) here cannot lose a wakeup.
	 */
	if (ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
}

static int
kqring_soupcall(struct socket *so __unused, void *arg, int waitflag __unused)
{
	struct kqring_op *op = arg;
	struct kqring_ctx *ctx = op->ctx;

	mtx_lock(&ctx->mtx);
	kqring_taskwork_add_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	return (SU_OK);
}

/*
 * TIMEOUT / LINK_TIMEOUT expiry.  The callout runs with ctx->mtx held
 * (callout_init_mtx), so callout_stop() under ctx->mtx guarantees it will
 * not run afterwards; like an upcall it only queues taskwork.
 */
static void
kqring_timer_cb(void *arg)
{
	struct kqring_op *op = arg;

	mtx_assert(&op->ctx->mtx, MA_OWNED);
	op->timer_pending = false;
	kqring_taskwork_add_locked(op->ctx, op);
}

/* Stop op's timer and unqueue an expiry not yet run. */
static void
kqring_timer_stop_locked(struct kqring_op *op)
{
	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (op->timer_pending) {
		callout_stop(&op->callout);
		op->timer_pending = false;
	}
	if (op->in_taskwork) {
		TAILQ_REMOVE(&op->ctx->taskwork_ops, op, taskwork_link);
		op->in_taskwork = false;
	}
}

static inline struct sockbuf *
kqring_sb(struct socket *so, int which)
{
	return (which == SO_RCV ? &so->so_rcv : &so->so_snd);
}

/*
 * Remove op's upcall from one socket buffer.  listen(2) may have turned the
 * socket into a listener since it was armed: solisten_proto() then reused
 * the sockbuf storage for the listen queues (wiping our upcall), and it did
 * so holding both sockbuf mutexes, so SOLISTENING() is stable here.
 */
static void
kqring_sb_disarm(struct kqring_op *op, struct socket *so, int which)
{
	SOCK_BUF_LOCK(so, which);
	if (!SOLISTENING(so) && kqring_sb(so, which)->sb_upcallarg == op)
		soupcall_clear(so, which);
	SOCK_BUF_UNLOCK(so, which);
}

/*
 * Take op's FUTEX_WAIT(V) words off kqring_futex_hash.  Only the owner
 * thread hashes and unhashes, so ->hashed is stable without the lock.
 */
static void
kqring_futex_unhash_locked(struct kqring_op *op)
{
	struct kqring_fwait *fw = op->args;
	uint32_t i;

	for (i = 0; i < op->nfwait && !fw[i].hashed; i++)
		;
	if (i == op->nfwait)
		return;
	/* Lock order: kqring_futex_mtx before ctx->mtx. */
	mtx_unlock(&op->ctx->mtx);
	mtx_lock(&kqring_futex_mtx);
	for (; i < op->nfwait; i++) {
		if (fw[i].hashed) {
			LIST_REMOVE(&fw[i], link);
			fw[i].hashed = false;
		}
	}
	mtx_unlock(&kqring_futex_mtx);
	mtx_lock(&op->ctx->mtx);
}

static void
kqring_op_disarm_locked(struct kqring_op *op)
{
	struct socket *so = op->so;

	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (op->polled) {
		op->polled = false;
		op->ctx->npolled--;
	}
	kqring_timer_stop_locked(op);
	kqring_futex_unhash_locked(op);
	if (so != NULL) {
		op->so = NULL;
		/*
		 * Drop ctx->mtx before acquiring the socket lock to respect
		 * lock ordering (socket lock -> ctx->mtx in kqring_soupcall).
		 */
		mtx_unlock(&op->ctx->mtx);
		if (op->listening) {
			SOCK_LOCK(so);
			if (so->sol_upcallarg == op)
				solisten_upcall_set(so, NULL, NULL);
			SOCK_UNLOCK(so);
		} else {
			if ((op->dirs & KQ_DIR_RCV) != 0)
				kqring_sb_disarm(op, so, SO_RCV);
			if ((op->dirs & KQ_DIR_SND) != 0)
				kqring_sb_disarm(op, so, SO_SND);
		}
		op->dirs = 0;
		sorele(so);
		mtx_lock(&op->ctx->mtx);
	}
}

/*
 * Take an armed request off active_ops and release everything it holds
 * (upcalls, socket reference, timer, queued taskwork).  May drop and
 * retake ctx->mtx.
 */
static void
kqring_op_unarm_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	KASSERT(op->armed, ("kqring: unarming an op that is not armed"));
	op->cancelled = true;
	op->armed = false;
	TAILQ_REMOVE(&ctx->active_ops, op, active_link);
	if (op->nfwait != 0)
		ctx->nfutex--;
	if (op->sqe.opcode == IORING_OP_WAITID)
		ctx->nwaitid--;
	if (op->seq_timeout) {
		op->seq_timeout = false;
		ctx->nseqto--;
	}
	kqring_op_disarm_locked(op);
}

static void
kqring_op_arm_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	op->armed = true;
	TAILQ_INSERT_TAIL(&ctx->active_ops, op, active_link);
}

static struct kqring_op *
kqring_op_alloc(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	op = TAILQ_FIRST(&ctx->free_ops);
	if (op != NULL) {
		TAILQ_REMOVE(&ctx->free_ops, op, active_link);
		ctx->nfree--;
		bzero(op, sizeof(*op));
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

	KASSERT(!op->armed && !op->in_taskwork && !op->timer_pending,
	    ("kqring: freeing a live op"));
	ctx->nreqs--;
	/*
	 * The callout was stopped under ctx->mtx, so its function will not
	 * run; drain anyway so that softclock is done with the structure
	 * before it is reused or freed.
	 */
	if (op->timer_init)
		callout_drain(&op->callout);
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
	if (!ctx->closing && ctx->nfree < KQRING_OP_CACHE_MAX) {
		TAILQ_INSERT_HEAD(&ctx->free_ops, op, active_link);
		ctx->nfree++;
		return;
	}
	free(op, M_KQRING);
}

/* ============================================================================
 * Synchronous Execution of Individual Opcodes (in curthread context)
 * ============================================================================
 */

/* Record op's result; the exec functions return 0 when op is done. */
static inline int
kqring_done(struct kqring_op *op, int32_t res, uint32_t cflags)
{
	op->res = res;
	op->cflags = cflags;
	return (0);
}

/*
 * Argument layouts in op->args, copied in by kqring_req_prep():
 *   READV / WRITEV:		struct iovec[sqe->len]
 *   CONNECT / BIND:		struct sockaddr (sa_len = its length)
 *   SEND / SEND_ZC:		struct sockaddr, if a destination was given
 *   SENDMSG(_ZC) / RECVMSG:	struct kqring_msg_args
 *   path ops / OPENAT2:	struct kqring_path_args
 *   xattr ops:			struct kqring_xattr_args
 *   FUTEX_WAIT(V):		struct kqring_fwait[op->nfwait]
 */
struct kqring_msg_args {
	struct msghdr	msg;		/* msg_iov -> iov[] */
	uint8_t		name[SOCK_MAXADDRLEN];	/* SENDMSG: msg_name */
	struct iovec	iov[];		/* then SENDMSG control data */
};

/*
 * RECVMSG with a provided buffer: the msghdr has at most one iovec, whose
 * length (0: none) caps the buffer used, as in io_uring.
 */
static inline uint32_t
kqring_msg_buf_cap(const struct kqring_msg_args *ma)
{
	if (ma->msg.msg_iovlen == 0)
		return (0);
	return ((uint32_t)MIN(ma->iov[0].iov_len, UINT32_MAX));
}

/*
 * Look up the socket an op works on.  For an armed op (taskwork),
 * `expect` is the socket it was armed on.  Every attempt re-resolves
 * sqe->fd (for Capsicum rights), and if the descriptor was closed or
 * reused for another file in the meantime the op fails with EBADF rather
 * than operating on the new file.
 */
static int
kqring_getsock(const struct kqring_op *op, struct socket *expect,
    cap_rights_t *rights, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	error = fget(td, op->sqe.fd, rights, &fp);
	if (error != 0)
		return (error);
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		return (ENOTSOCK);
	}
	if (expect != NULL && fp->f_data != expect) {
		fdrop(fp, td);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

static inline bool
kqring_so_stream(const struct socket *so)
{
	return (so->so_type == SOCK_STREAM || so->so_type == SOCK_SEQPACKET);
}

/*
 * Whether a receive would not block: bytes buffered, EOF or a pending
 * error.  ENOTCONN for a listening socket.  Checked before popping a
 * provided buffer, which is only consumed when bytes are received.
 */
static int
kqring_so_readable(struct socket *so, bool *readable)
{
	SOCK_RECVBUF_LOCK(so);
	/* Checked under the sockbuf lock; see kqring_sb_disarm(). */
	if (SOLISTENING(so)) {
		SOCK_RECVBUF_UNLOCK(so);
		return (ENOTCONN);
	}
	*readable = sbavail(&so->so_rcv) > 0 ||
	    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0 ||
	    so->so_error != 0;
	SOCK_RECVBUF_UNLOCK(so);
	return (0);
}

/* Unlocked hint: more is buffered (or EOF) after a receive. */
static inline bool
kqring_so_rcv_pending(struct socket *so, bool eof)
{
	return (!SOLISTENING(so) && (sbavail(&so->so_rcv) > 0 ||
	    (eof && (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0)));
}

/*
 * Pick a provided buffer for a receive of at most `len` bytes (0: the
 * buffer's size).  Returns EAGAIN, without consuming a buffer, if nothing
 * is there to receive.
 */
static int
kqring_recv_pick_buf(struct kqring_ctx *ctx, struct socket *so, uint32_t len,
    struct io_uring_buf *pbuf)
{
	bool readable;
	int error;

	error = kqring_so_readable(so, &readable);
	if (error != 0)
		return (error);
	if (!readable)
		return (EAGAIN);
	error = kqring_pbuf_peek(ctx, pbuf);
	if (error != 0)
		return (error);
	if (len != 0 && len < pbuf->len)
		pbuf->len = len;
	return (0);
}

static inline uint32_t
kqring_pbuf_cflags(const struct io_uring_buf *pbuf)
{
	return (IORING_CQE_F_BUFFER |
	    ((uint32_t)pbuf->bid << IORING_CQE_BUFFER_SHIFT));
}

/* The result of a transfer, which io_uring reports as an int. */
static inline int32_t
kqring_xfer_res(size_t n)
{
	return ((int32_t)MIN(n, (size_t)INT32_MAX));
}

/*
 * End one attempt that transferred `n` bytes with `error`.  An error after
 * earlier MSG_WAITALL progress reports the progress, as in io_uring.
 * Returns EAGAIN to wait (nothing transferred yet), else 0 with the
 * result in op->res.
 */
static int
kqring_xfer_end(struct kqring_op *op, int error, size_t n)
{
	if (n == 0 && error == EWOULDBLOCK)
		return (EAGAIN);
	if (n == 0 && error != 0) {
		if (op->done_io != 0)
			return (kqring_done(op, kqring_xfer_res(op->done_io), 0));
		return (kqring_done(op, -error, 0));
	}
	return (-1);
}

/*
 * MSG_WAITALL on a stream socket: keep a short transfer's progress and
 * wait for the rest (EAGAIN), as io_uring retries.  Returns the total so
 * far once done.
 */
static int
kqring_waitall(struct kqring_op *op, size_t n, size_t want, size_t *total)
{
	op->done_io += n;
	if (n > 0 && op->done_io < want)
		return (EAGAIN);
	*total = op->done_io;
	return (0);
}

static size_t
kqring_iov_len(const struct iovec *iov, u_int iovcnt)
{
	size_t len = 0;

	/* kqring_req_prep() checked that the sum fits in a ssize_t. */
	for (u_int i = 0; i < iovcnt; i++)
		len += iov[i].iov_len;
	return (len);
}

static void
kqring_uio_init(struct uio *auio, struct iovec *iov, u_int iovcnt,
    enum uio_rw rw)
{
	auio->uio_iov = iov;
	auio->uio_iovcnt = iovcnt;
	auio->uio_offset = 0;
	auio->uio_resid = kqring_iov_len(iov, iovcnt);
	auio->uio_segflg = UIO_USERSPACE;
	auio->uio_rw = rw;
	auio->uio_td = curthread;
}

/*
 * Socket receive for RECV, READ_MULTISHOT, and READ / READV on a socket:
 * into the SQE's buffer, its iovec, or a provided buffer
 * (IOSQE_BUFFER_SELECT).  Returns:
 *   0: the attempt finished, with its result in op->res / op->cflags;
 *      IORING_CQE_F_MORE in op->cflags means a multishot receive goes on.
 *   EAGAIN: socket would block; caller should arm (or keep armed) upcall.
 * *more_out is an unlocked hint that another receive would not block
 * (bytes still buffered or EOF), used to end multishot drain loops without
 * a final EAGAIN soreceive(); a stale value is harmless because the upcall
 * stays armed.
 */
static int
kqring_exec_recv(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	struct io_uring_buf pbuf;
	cap_rights_t rights;
	u_int iovcnt = 1;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool multishot = sqe->opcode == IORING_OP_READ_MULTISHOT ||
	    (sqe->opcode == IORING_OP_RECV &&
	    (sqe->ioprio & IORING_RECV_MULTISHOT) != 0);
	bool waitall;
	size_t n;
	int error, flags, rc;

	*more_out = false;

	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_RECV), &fp);
	if (error != 0) {
		/* READ_MULTISHOT needs readiness, which only sockets have yet. */
		if (error == ENOTSOCK && sqe->opcode == IORING_OP_READ_MULTISHOT)
			error = EOPNOTSUPP;
		return (kqring_done(op, -error, 0));
	}
	so = fp->f_data;

	if (buf_select) {
		error = kqring_recv_pick_buf(ctx, so, sqe->len, &pbuf);
		if (error != 0) {
			fdrop(fp, td);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		aiov.iov_base = (void *)(uintptr_t)pbuf.addr;
		aiov.iov_len = pbuf.len;
	} else if (sqe->opcode == IORING_OP_READV) {
		/* uiomove() advances the kernel iovec copy in place. */
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_READ);
	ssize_t want = auio.uio_resid;

	flags = MSG_DONTWAIT;
	if (sqe->opcode == IORING_OP_RECV)
		flags |= (int)sqe->msg_flags;
	waitall = (flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	error = soreceive(so, NULL, &auio, NULL, NULL, &flags);
	/* MSG_TRUNC on a datagram: uio_resid underflows to the real size. */
	n = (size_t)(want - auio.uio_resid);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		fdrop(fp, td);
		return (rc);
	}

	/*
	 * Commit the PBUF_RING buffer ONLY when n > 0!  On EOF (n == 0) the
	 * buffer stays in the PBUF_RING and res = 0 has no
	 * IORING_CQE_F_BUFFER.
	 */
	if (buf_select && n > 0) {
		kqring_pbuf_commit(ctx);
		cqe_flags |= kqring_pbuf_cflags(&pbuf);
	}
	if (multishot && n > 0) {
		cqe_flags |= IORING_CQE_F_MORE;
		*more_out = kqring_so_rcv_pending(so, true);
	} else if (kqring_so_rcv_pending(so, false)) {
		cqe_flags |= IORING_CQE_F_SOCK_NONEMPTY;
	}
	if (waitall && kqring_waitall(op, n, sqe->len, &n) == EAGAIN) {
		fdrop(fp, td);
		return (EAGAIN);
	}
	fdrop(fp, td);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/* SEND, SEND_ZC, and WRITE / WRITEV on a socket. */
static int
kqring_exec_send(struct kqring_op *op, struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct sockaddr *to = NULL;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	cap_rights_t rights;
	u_int iovcnt = 1;
	bool waitall = false;
	size_t n;
	int error, flags = MSG_DONTWAIT | MSG_NOSIGNAL, rc;

	if (sqe->opcode == IORING_OP_SEND || sqe->opcode == IORING_OP_SEND_ZC) {
		flags |= (int)sqe->msg_flags;
		to = op->args;
	}
	/* As sendto(2): no addressed sends in capability mode. */
	if (to != NULL && IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	if (to != NULL)
		cap_rights_init(&rights, CAP_SEND, CAP_CONNECT);
	else
		cap_rights_init_one(&rights, CAP_SEND);
	error = kqring_getsock(op, expect, &rights, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;

	if (sqe->opcode == IORING_OP_WRITEV) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_WRITE);
	ssize_t want = auio.uio_resid;

	waitall = (flags & MSG_WAITALL) != 0 && kqring_so_stream(so);
	flags &= ~MSG_WAITALL;
	error = sosend(so, to, &auio, NULL, NULL, flags, td);
	n = (size_t)(want - auio.uio_resid);
	fdrop(fp, td);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0)
		return (rc);
	if (waitall && kqring_waitall(op, n, sqe->len, &n) == EAGAIN)
		return (EAGAIN);
	return (kqring_done(op, kqring_xfer_res(n), 0));
}

/*
 * Copy a received message's control data (SCM_RIGHTS already installed
 * as descriptors) to `ubuf`, as recvmsg(2) does; what does not fit is
 * disposed of (closing such descriptors) and flagged MSG_CTRUNC.
 */
static int
kqring_copyout_control(struct mbuf *control, void *ubuf, size_t space,
    size_t *lenp, int *flagsp)
{
	struct mbuf *m;
	size_t len = 0;
	int error = 0;

	for (m = control; m != NULL && space - len >= (size_t)m->m_len;
	    m = m->m_next) {
		error = copyout(mtod(m, void *), (char *)ubuf + len, m->m_len);
		if (error != 0)
			break;
		len += m->m_len;
	}
	if (m != NULL) {
		if (error == 0)
			*flagsp |= MSG_CTRUNC;
		m_dispose_extcontrolm(m);
	}
	m_freem(control);
	*lenp = len;
	return (error);
}

/*
 * Multishot RECVMSG: one message per provided buffer, laid out as
 * struct io_uring_recvmsg_out, then the name and control areas sized by
 * the submitted msghdr, then the payload.
 */
static int
kqring_exec_recvmsg_multi(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *so, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct io_uring_recvmsg_out out;
	struct io_uring_buf pbuf;
	struct msghdr m;
	struct iovec aiov;
	struct mbuf *control = NULL;
	uint8_t name[SOCK_MAXADDRLEN];
	size_t namesz = ma->msg.msg_namelen;
	size_t ctlsz = ma->msg.msg_controllen;
	size_t hdr = sizeof(out) + namesz + ctlsz, clen, n;
	char *ubuf;
	int error;

	error = kqring_recv_pick_buf(ctx, so, kqring_msg_buf_cap(ma), &pbuf);
	if (error != 0) {
		if (error == EAGAIN)
			return (EAGAIN);
		return (kqring_done(op, -error, 0));
	}
	if (pbuf.len < hdr)
		return (kqring_done(op, -EFAULT, 0));
	ubuf = (char *)(uintptr_t)pbuf.addr;

	/* Ask for the whole name, to report its real length. */
	bzero(&m, sizeof(m));
	m.msg_name = name;
	m.msg_namelen = sizeof(name);
	aiov.iov_base = ubuf + hdr;
	aiov.iov_len = pbuf.len - hdr;
	m.msg_iov = &aiov;
	m.msg_iovlen = 1;
	m.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	error = kern_recvit(td, sqe->fd, &m, UIO_SYSSPACE,
	    ctlsz != 0 ? &control : NULL);
	if (error == EWOULDBLOCK)
		return (EAGAIN);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	n = td->td_retval[0];
	if (n == 0 && kqring_so_stream(so)) {
		/* EOF: ends the multishot, buffer not consumed. */
		m_freem(control);
		return (kqring_done(op, 0, 0));
	}

	bzero(&out, sizeof(out));
	out.namelen = m.msg_namelen;
	if (MIN(out.namelen, namesz) != 0)
		error = copyout(name, ubuf + sizeof(out),
		    MIN(out.namelen, namesz));
	clen = 0;
	if (control != NULL) {
		if (error == 0)
			error = kqring_copyout_control(control,
			    ubuf + sizeof(out) + namesz, ctlsz, &clen,
			    &m.msg_flags);
		else
			m_freem(control);
	}
	out.controllen = (uint32_t)clen;
	out.payloadlen = (uint32_t)MIN(n, UINT32_MAX);
	out.flags = (uint32_t)(m.msg_flags & ~MSG_DONTWAIT);
	if (error == 0)
		error = copyout(&out, ubuf, sizeof(out));
	if (error != 0)
		return (kqring_done(op, -error, 0));

	kqring_pbuf_commit(ctx);
	*more_out = kqring_so_rcv_pending(so, true);
	return (kqring_done(op, kqring_xfer_res(hdr + MIN(n, aiov.iov_len)),
	    kqring_pbuf_cflags(&pbuf) | IORING_CQE_F_MORE));
}

/* RECVMSG: kern_recvit() on the kernel copy of the msghdr's iovec. */
static int
kqring_exec_recvmsg(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct msghdr *umsg = (struct msghdr *)(uintptr_t)sqe->addr;
	struct io_uring_buf pbuf;
	struct file *fp;
	struct socket *so;
	struct msghdr msg;
	struct iovec aiov;
	cap_rights_t rights;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool waitall;
	size_t n;
	int error, rc;

	*more_out = false;
	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_RECV), &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;

	if ((sqe->ioprio & IORING_RECV_MULTISHOT) != 0) {
		rc = kqring_exec_recvmsg_multi(ctx, op, so, more_out);
		fdrop(fp, td);
		return (rc);
	}

	msg = ma->msg;
	if (buf_select) {
		error = kqring_recv_pick_buf(ctx, so, kqring_msg_buf_cap(ma),
		    &pbuf);
		if (error != 0) {
			fdrop(fp, td);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		aiov.iov_base = (void *)(uintptr_t)pbuf.addr;
		aiov.iov_len = pbuf.len;
		msg.msg_iov = &aiov;
		msg.msg_iovlen = 1;
	}
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	msg.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	waitall = (msg.msg_flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	error = kern_recvit(td, sqe->fd, &msg, UIO_USERSPACE, NULL);
	n = error == 0 ? (size_t)td->td_retval[0] : 0;
	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		fdrop(fp, td);
		return (rc);
	}

	/* As recvmsg(2), report the name, control and flags lengths. */
	int oflags = msg.msg_flags & ~(MSG_DONTWAIT & ~(int)sqe->msg_flags);
	error = copyout(&msg.msg_namelen, &umsg->msg_namelen,
	    sizeof(msg.msg_namelen));
	if (error == 0)
		error = copyout(&msg.msg_controllen, &umsg->msg_controllen,
		    sizeof(msg.msg_controllen));
	if (error == 0)
		error = copyout(&oflags, &umsg->msg_flags, sizeof(oflags));
	if (buf_select && n > 0) {
		kqring_pbuf_commit(ctx);
		cqe_flags |= kqring_pbuf_cflags(&pbuf);
	}
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, cqe_flags));
	}
	if (kqring_so_rcv_pending(so, false))
		cqe_flags |= IORING_CQE_F_SOCK_NONEMPTY;
	if (waitall && kqring_waitall(op, n, want + op->done_io, &n) ==
	    EAGAIN) {
		fdrop(fp, td);
		return (EAGAIN);
	}
	fdrop(fp, td);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/* SENDMSG / SENDMSG_ZC: kern_sendit() on the kernel copy of the msghdr. */
static int
kqring_exec_sendmsg(struct kqring_op *op, struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct mbuf *control = NULL;
	struct file *fp;
	struct socket *so;
	struct msghdr msg;
	cap_rights_t rights;
	bool waitall;
	size_t n;
	int error, flags, rc;

	/* As sendmsg(2): no addressed sends in capability mode. */
	if (ma->msg.msg_name != NULL && IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_SEND), &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;
	waitall = (sqe->msg_flags & MSG_WAITALL) != 0 && kqring_so_stream(so);

	msg = ma->msg;
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	if (msg.msg_controllen != 0) {
		/* kern_sendit() consumes the mbuf: rebuild it each attempt. */
		control = m_get2(msg.msg_controllen, M_WAITOK, MT_CONTROL, 0);
		control->m_len = msg.msg_controllen;
		bcopy(msg.msg_control, mtod(control, void *),
		    msg.msg_controllen);
	}
	flags = ((int)sqe->msg_flags & ~MSG_WAITALL) | MSG_DONTWAIT |
	    MSG_NOSIGNAL;
	error = kern_sendit(td, sqe->fd, &msg, flags, control, UIO_USERSPACE);
	n = error == 0 ? (size_t)td->td_retval[0] : 0;
	fdrop(fp, td);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0)
		return (rc);
	/* The control data went with the first part, as in io_uring. */
	if (n > 0)
		ma->msg.msg_controllen = 0;
	if (waitall && kqring_waitall(op, n, want + op->done_io, &n) ==
	    EAGAIN)
		return (EAGAIN);
	return (kqring_done(op, kqring_xfer_res(n), 0));
}

/*
 * CONNECT: start a non-blocking connect, then (armed on so_snd, which
 * soisconnected() and soisdisconnected() wake) wait for it to finish.
 * The checks mirror kern_connectat(), which would sleep.
 */
static int
kqring_exec_connect(struct kqring_op *op, struct socket *expect)
{
	struct thread *td = curthread;
	struct sockaddr *sa = op->args;
	struct file *fp;
	struct socket *so;
	cap_rights_t rights;
	int error;

	if (IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_CONNECT), &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;

	if (!op->connecting) {
		if ((so->so_state & SS_ISCONNECTING) != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -EALREADY, 0));
		}
#ifdef KQRING_MAC
		error = mac_socket_check_connect(td->td_ucred, so, sa);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
#endif
		error = soconnect(so, sa, td);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
		op->connecting = true;
	}

	SOCK_LOCK(so);
	if ((so->so_state & SS_ISCONNECTING) != 0 && so->so_error == 0) {
		SOCK_UNLOCK(so);
		fdrop(fp, td);
		return (EAGAIN);
	}
	error = so->so_error;
	so->so_error = 0;
	SOCK_UNLOCK(so);
	fdrop(fp, td);
	return (kqring_done(op, -error, 0));
}

static int
kqring_exec_accept(struct kqring_op *op, struct socket *expect,
    bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *headfp = NULL, *nfp = NULL;
	struct socket *headso;
	struct sockaddr_storage ss = { .ss_len = sizeof(ss) };
	cap_rights_t rights;
	int error, nfd = -1, aflags;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_ACCEPT),
	    &headfp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (headfp->f_type != DTYPE_SOCKET) {
		fdrop(headfp, td);
		return (kqring_done(op, -ENOTSOCK, 0));
	}
	headso = headfp->f_data;
	if (expect != NULL && headso != expect) {
		fdrop(headfp, td);
		return (kqring_done(op, -EBADF, 0));
	}
	if ((headso->so_options & SO_ACCEPTCONN) == 0) {
		fdrop(headfp, td);
		return (kqring_done(op, -EINVAL, 0));
	}
	/* Listening sockets have no so_rcv; only sol_comp/so_error apply. */
	if (TAILQ_EMPTY(&headso->sol_comp) && headso->so_error == 0) {
		fdrop(headfp, td);
		return (EAGAIN);
	}

	/* The new descriptor gets only the flags asked for, as accept4(2). */
	aflags = (int)sqe->accept_flags;
	error = kern_accept4(td, sqe->fd, (struct sockaddr *)&ss, aflags,
	    &nfp);
	if (error != 0) {
		fdrop(headfp, td);
		if (error == EWOULDBLOCK)
			return (EAGAIN);
		return (kqring_done(op, -error, 0));
	}
	nfd = td->td_retval[0];
	if (nfp != NULL)
		fdrop(nfp, td);

	/* As accept(2): the peer address to addr, its length to *addr2. */
	if (sqe->addr != 0) {
		socklen_t len;

		error = copyin((const void *)(uintptr_t)sqe->addr2, &len,
		    sizeof(len));
		if (error == 0) {
			len = MIN(len, ss.ss_len);
			error = copyout(&ss, (void *)(uintptr_t)sqe->addr, len);
		}
		if (error == 0) {
			len = ss.ss_len;
			error = copyout(&len, (void *)(uintptr_t)sqe->addr2,
			    sizeof(len));
		}
		if (error != 0) {
			fdrop(headfp, td);
			(void)kern_close(td, nfd);
			return (kqring_done(op, -error, 0));
		}
	}

	if ((sqe->ioprio & IORING_ACCEPT_MULTISHOT) != 0) {
		*more_out = !TAILQ_EMPTY(&headso->sol_comp);
		fdrop(headfp, td);
		return (kqring_done(op, nfd, IORING_CQE_F_MORE));
	}
	fdrop(headfp, td);
	return (kqring_done(op, nfd, 0));
}

/*
 * Socket readiness without fo_poll(): when nothing is ready, sopoll() calls
 * selrecord(td, ...), which requires the select/poll per-thread state
 * (td->td_sel) that only exists inside select(2)/poll(2).
 */
static int
kqring_so_revents(struct socket *so, int events)
{
	int revents;

again:
	revents = 0;
	SOCK_LOCK(so);
	if (SOLISTENING(so)) {
		if (!TAILQ_EMPTY(&so->sol_comp) || so->so_error != 0)
			revents |= events & (POLLIN | POLLRDNORM);
		if (so->so_error != 0)
			revents |= POLLERR;
		SOCK_UNLOCK(so);
		return (revents);
	}
	SOCK_UNLOCK(so);

	/* A racing listen(2) reuses the sockbufs; see kqring_sb_disarm(). */
	SOCK_RECVBUF_LOCK(so);
	if (SOLISTENING(so)) {
		SOCK_RECVBUF_UNLOCK(so);
		goto again;
	}
	bool rd_eof = (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0;
	if (sbavail(&so->so_rcv) >= MAX(so->so_rcv.sb_lowat, 1) || rd_eof ||
	    so->so_error != 0)
		revents |= events & (POLLIN | POLLRDNORM);
	SOCK_RECVBUF_UNLOCK(so);

	SOCK_SENDBUF_LOCK(so);
	if (SOLISTENING(so)) {
		SOCK_SENDBUF_UNLOCK(so);
		goto again;
	}
	bool wr_eof = (so->so_snd.sb_state & SBS_CANTSENDMORE) != 0;
	if ((sbspace(&so->so_snd) >= so->so_snd.sb_lowat &&
	    (so->so_state & SS_ISCONNECTED) != 0) || wr_eof ||
	    so->so_error != 0)
		revents |= events & (POLLOUT | POLLWRNORM);
	SOCK_SENDBUF_UNLOCK(so);

	if (so->so_error != 0)
		revents |= POLLERR;
	if (rd_eof && wr_eof)
		revents |= POLLHUP;
	return (revents);
}

static int
kqring_exec_poll(struct kqring_op *op, struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	cap_rights_t rights;
	int error, revents;
	int events = sqe->poll32_events ? (int)sqe->poll32_events :
	    sqe->poll_events;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_POLL_EVENT),
	    &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (fp->f_type != DTYPE_SOCKET) {
		/* Only sockets can be armed (see kqring_arm_socket_op). */
		fdrop(fp, td);
		return (kqring_done(op, -EOPNOTSUPP, 0));
	}
	if (expect != NULL && fp->f_data != expect) {
		fdrop(fp, td);
		return (kqring_done(op, -EBADF, 0));
	}
	revents = kqring_so_revents(fp->f_data, events);
	fdrop(fp, td);

	if (revents == 0)
		return (EAGAIN);
	return (kqring_done(op, revents,
	    (sqe->len & IORING_POLL_ADD_MULTI) != 0 ? IORING_CQE_F_MORE : 0));
}

/*
 * READ / WRITE / READV / WRITEV on anything but a socket (sockets go
 * through kqring_exec_recv() / kqring_exec_send() and can be armed).
 * READ may take a provided buffer, only consumed if bytes were read.
 */
static int
kqring_exec_file_rw(struct kqring_ctx *ctx, struct kqring_op *op,
    bool is_write)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct io_uring_buf pbuf;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	off_t offset = (off_t)sqe->off;
	u_int iovcnt = 1;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	int error;

	if (buf_select) {
		error = kqring_pbuf_peek(ctx, &pbuf);
		if (error != 0)
			return (kqring_done(op, -error, 0));
		if (sqe->len != 0 && sqe->len < pbuf.len)
			pbuf.len = sqe->len;
		aiov.iov_base = (void *)(uintptr_t)pbuf.addr;
		aiov.iov_len = pbuf.len;
	} else if (sqe->opcode == IORING_OP_READV ||
	    sqe->opcode == IORING_OP_WRITEV) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (void *)(uintptr_t)sqe->addr;
		aiov.iov_len = sqe->len;
	}
	kqring_uio_init(&auio, iov, iovcnt, is_write ? UIO_WRITE : UIO_READ);
	ssize_t want = auio.uio_resid;
	auio.uio_offset = (offset == (off_t)-1) ? 0 : offset;

	/* off == -1 means "use the file position", as in io_uring. */
	if (offset == (off_t)-1)
		error = is_write ? kern_writev(td, sqe->fd, &auio) :
		    kern_readv(td, sqe->fd, &auio);
	else
		error = is_write ? kern_pwritev(td, sqe->fd, &auio, offset) :
		    kern_preadv(td, sqe->fd, &auio, offset);

	size_t done = (size_t)(want - auio.uio_resid);
	if (error != 0 && done == 0)
		return (kqring_done(op, -error, 0));
	if (buf_select && done > 0) {
		kqring_pbuf_commit(ctx);
		return (kqring_done(op, kqring_xfer_res(done),
		    kqring_pbuf_cflags(&pbuf)));
	}
	return (kqring_done(op, kqring_xfer_res(done), 0));
}

/* Path operations; OPENAT2 also uses `how`. */
struct kqring_path_args {
	struct open_how	how;
	char		path[MAXPATHLEN];
	char		path2[MAXPATHLEN];	/* RENAMEAT, LINKAT, SYMLINKAT */
};

/* xattr operations; Linux "user." / "system." names map to extattr. */
struct kqring_xattr_args {
	int		ns;			/* EXTATTR_NAMESPACE_* */
	char		name[KQRING_XATTR_NAME_MAX + 1];	/* no prefix */
	char		path[MAXPATHLEN];	/* GETXATTR / SETXATTR */
	char		value[];		/* SETXATTR / FSETXATTR */
};

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

static int32_t
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

static int32_t
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
		error = getvnode_path(td, sqe->fd,
		    cap_rights_init_one(&rights, CAP_EXTATTR_SET), NULL, &fp);
		if (error != 0)
			return (-error);
		error = kqring_setxattr_vp(fp->f_vnode, xa, sqe->len,
		    sqe->xattr_flags, td);
		fdrop(fp, td);
		return (-error);
	}
	NDINIT(&nd, LOOKUP, FOLLOW | AUDITVNODE1, UIO_SYSSPACE, xa->path);
	error = namei(&nd);
	if (error != 0)
		return (-error);
	NDFREE_PNBUF(&nd);
	error = kqring_setxattr_vp(nd.ni_vp, xa, sqe->len, sqe->xattr_flags,
	    td);
	vrele(nd.ni_vp);
	return (-error);
}

/*
 * GETXATTR / FGETXATTR into the user buffer.  extattr_get(2) truncates
 * silently, so ask for the size first and fail with ERANGE like Linux
 * when the buffer is too small (len 0 asks for the size).
 */
static int32_t
kqring_exec_getxattr(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_xattr_args *xa = op->args;
	struct thread *td = curthread;
	void *data = (void *)(uintptr_t)sqe->addr2;
	bool fd_op = sqe->opcode == IORING_OP_FGETXATTR;
	int error;

	for (int pass = 0; pass < 2; pass++) {
		if (fd_op)
			error = kern_extattr_get_fd(td, sqe->fd, xa->ns,
			    xa->name, pass == 0 ? NULL : data,
			    pass == 0 ? 0 : sqe->len);
		else
			error = kern_extattr_get_path(td, xa->path, xa->ns,
			    xa->name, pass == 0 ? NULL : data,
			    pass == 0 ? 0 : sqe->len, FOLLOW, UIO_SYSSPACE);
		if (error != 0)
			return (-error);
		if (pass == 0 && sqe->len == 0)
			break;
		if (pass == 0 && (size_t)td->td_retval[0] > sqe->len)
			return (-ERANGE);
	}
	return (kqring_xfer_res((size_t)td->td_retval[0]));
}

static int32_t
kqring_exec_fallocate(const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct spacectl_range sr = {
		.r_offset = (off_t)sqe->off,
		.r_len = (off_t)sqe->addr,
	};

	switch (sqe->len) {
	case 0:
		return (-kern_posix_fallocate(td, sqe->fd, sr.r_offset,
		    sr.r_len));
	case FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE:
		return (-kern_fspacectl(td, sqe->fd, SPACECTL_DEALLOC, &sr, 0,
		    &sr));
	default:
		/* FreeBSD cannot allocate without extending (KEEP_SIZE). */
		return (-EOPNOTSUPP);
	}
}

/* Whether fp is a /dev/kqring descriptor. */
static bool
kqring_fp_is_ring(const struct file *fp)
{
	return (fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
	    fp->f_vnode->v_type == VCHR && fp->f_vnode->v_rdev == kqring_cdev);
}

/* CLOSE: like close(2), but a ring cannot close itself or another ring. */
static int32_t
kqring_exec_close(const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct file *fp;
	bool ring;
	int error;

	error = fget(td, sqe->fd, &cap_no_rights, &fp);
	if (error != 0)
		return (-error);
	ring = kqring_fp_is_ring(fp);
	fdrop(fp, td);
	if (ring)
		return (-EBADF);
	return (-kern_close(td, sqe->fd));
}

/* URING_CMD on a socket: SIOCINQ, SIOCOUTQ, getsockopt, setsockopt. */
static int32_t
kqring_exec_uring_cmd(const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct file *fp;
	socklen_t optlen;
	int error, n;
	bool sock;

	error = fget(td, sqe->fd, &cap_no_rights, &fp);
	if (error != 0)
		return (-error);
	sock = fp->f_type == DTYPE_SOCKET;
	fdrop(fp, td);
	/* No other file type implements commands. */
	if (!sock)
		return (-EOPNOTSUPP);

	switch (sqe->cmd_op) {
	case SOCKET_URING_OP_SIOCINQ:
	case SOCKET_URING_OP_SIOCOUTQ:
		error = kern_ioctl(td, sqe->fd,
		    sqe->cmd_op == SOCKET_URING_OP_SIOCINQ ? FIONREAD :
		    FIONWRITE, (caddr_t)&n);
		return (error != 0 ? -error : n);
	case SOCKET_URING_OP_GETSOCKOPT:
		/* As in io_uring, only SOL_SOCKET options. */
		if (sqe->level != SOL_SOCKET)
			return (-EOPNOTSUPP);
		optlen = sqe->optlen;
		error = kern_getsockopt(td, sqe->fd, (int)sqe->level,
		    (int)sqe->optname, (void *)(uintptr_t)sqe->optval,
		    UIO_USERSPACE, &optlen);
		return (error != 0 ? -error : (int32_t)optlen);
	case SOCKET_URING_OP_SETSOCKOPT:
		return (-kern_setsockopt(td, sqe->fd, (int)sqe->level,
		    (int)sqe->optname, (const void *)(uintptr_t)sqe->optval,
		    UIO_USERSPACE, sqe->optlen));
	default:
		return (-EOPNOTSUPP);
	}
}

/*
 * Ops that complete in one synchronous call; the result goes to op->res
 * and op->cflags.
 */
static void
kqring_exec_sync(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa = op->args;
	struct thread *td = curthread;
	int32_t res;
	int error;

	switch (sqe->opcode) {
	case IORING_OP_NOP:
		res = 0;
		break;
	case IORING_OP_READ:
	case IORING_OP_READV:
		kqring_exec_file_rw(ctx, op, false);
		return;
	case IORING_OP_WRITE:
	case IORING_OP_WRITEV:
		kqring_exec_file_rw(ctx, op, true);
		return;
	case IORING_OP_FSYNC:
		res = -kern_fsync(td, sqe->fd,
		    (sqe->fsync_flags & IORING_FSYNC_DATASYNC) == 0);
		break;
	case IORING_OP_SYNC_FILE_RANGE:
		/* No range sync on FreeBSD: fdatasync(2) the whole file. */
		res = sqe->sync_range_flags == 0 ? 0 :
		    -kern_fsync(td, sqe->fd, false);
		break;
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
		error = kern_openat(td, sqe->fd, pa->path, UIO_SYSSPACE,
		    (int)pa->how.flags, (int)pa->how.mode);
		res = error == 0 ? (int32_t)td->td_retval[0] : -error;
		break;
	case IORING_OP_CLOSE:
		res = kqring_exec_close(sqe);
		break;
	case IORING_OP_STATX:
		res = kqring_exec_statx(op);
		break;
	case IORING_OP_FALLOCATE:
		res = kqring_exec_fallocate(sqe);
		break;
	case IORING_OP_FTRUNCATE:
		res = -kern_ftruncate(td, sqe->fd, (off_t)sqe->off);
		break;
	case IORING_OP_FADVISE:
		/* A 64-bit length in addr, else the 32-bit one in len. */
		res = -kern_posix_fadvise(td, sqe->fd, (off_t)sqe->off,
		    sqe->addr != 0 ? (off_t)sqe->addr : (off_t)sqe->len,
		    (int)sqe->fadvise_advice);
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
		error = kern_socket(td, sqe->fd, (int)sqe->off, (int)sqe->len);
		res = error == 0 ? (int32_t)td->td_retval[0] : -error;
		break;
	case IORING_OP_BIND:
		res = -kern_bindat(td, AT_FDCWD, sqe->fd, op->args);
		break;
	case IORING_OP_LISTEN:
		res = -kern_listen(td, sqe->fd, (int)sqe->len);
		break;
	case IORING_OP_SHUTDOWN:
		res = -kern_shutdown(td, sqe->fd, (int)sqe->len);
		break;
	case IORING_OP_URING_CMD:
		res = kqring_exec_uring_cmd(sqe);
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
		case IORING_OP_READ_MULTISHOT:
			rc = kqring_exec_recv(ctx, op, expect, &more);
			break;
		case IORING_OP_RECVMSG:
			rc = kqring_exec_recvmsg(ctx, op, expect, &more);
			break;
		case IORING_OP_SEND:
		case IORING_OP_SEND_ZC:
		case IORING_OP_WRITE:
		case IORING_OP_WRITEV:
			rc = kqring_exec_send(op, expect);
			break;
		case IORING_OP_SENDMSG:
		case IORING_OP_SENDMSG_ZC:
			rc = kqring_exec_sendmsg(op, expect);
			break;
		case IORING_OP_CONNECT:
			rc = kqring_exec_connect(op, expect);
			break;
		case IORING_OP_ACCEPT:
			rc = kqring_exec_accept(op, expect, &more);
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
 * Arming Asynchronous Socket & Futex Operations
 * ============================================================================
 */

/*
 * Install op's upcall on one socket buffer and OR its readiness into *ready.
 * A sockbuf has a single upcall slot that soupcall_set() silently
 * overwrites, so fail with EBUSY if it is taken (another op armed on the
 * same socket and direction, on this ring or another).  ERESTART means the
 * socket became a listener; see kqring_sb_disarm().
 */
static int
kqring_sb_arm(struct kqring_op *op, struct socket *so, int which, bool *ready)
{
	struct sockbuf *sb;

	SOCK_BUF_LOCK(so, which);
	if (SOLISTENING(so)) {
		SOCK_BUF_UNLOCK(so, which);
		return (ERESTART);
	}
	sb = kqring_sb(so, which);
	if (sb->sb_upcall != NULL) {
		SOCK_BUF_UNLOCK(so, which);
		return (EBUSY);
	}
	soupcall_set(so, which, kqring_soupcall, op);
	if (which == SO_RCV) {
		op->dirs |= KQ_DIR_RCV;
		if (sbavail(sb) > 0 || (sb->sb_state & SBS_CANTRCVMORE) != 0 ||
		    so->so_error != 0)
			*ready = true;
	} else {
		op->dirs |= KQ_DIR_SND;
		if (sbspace(sb) > 0 || (sb->sb_state & SBS_CANTSENDMORE) != 0 ||
		    so->so_error != 0)
			*ready = true;
	}
	SOCK_BUF_UNLOCK(so, which);
	return (0);
}

/* Socket buffer directions an op waits on. */
static int
kqring_op_dirs(const struct io_uring_sqe *sqe)
{
	uint32_t events;
	int dirs = 0;

	switch (sqe->opcode) {
	case IORING_OP_SEND:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG:
	case IORING_OP_SENDMSG_ZC:
	case IORING_OP_WRITE:
	case IORING_OP_WRITEV:
	case IORING_OP_CONNECT:
		/* soisconnected() / soisdisconnected() also wake so_snd. */
		return (KQ_DIR_SND);
	case IORING_OP_POLL_ADD:
		/*
		 * Watch every direction asked for; anything but POLLOUT
		 * (POLLIN, POLLHUP, ...) shows up on so_rcv.
		 */
		events = sqe->poll32_events ? sqe->poll32_events :
		    sqe->poll_events;
		if ((events & (POLLOUT | POLLWRNORM)) != 0)
			dirs |= KQ_DIR_SND;
		if ((events & ~(uint32_t)(POLLOUT | POLLWRNORM)) != 0 ||
		    dirs == 0)
			dirs |= KQ_DIR_RCV;
		return (dirs);
	default:
		return (KQ_DIR_RCV);
	}
}

/*
 * Arm op on its socket for the KQ_DIR_* directions in `dirs` (ignored for
 * listening sockets).  Returns true if armed; on failure op is completed
 * with an error CQE.
 */
static bool
kqring_arm_socket_op(struct kqring_ctx *ctx, struct kqring_op *op, int dirs)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	cap_rights_t rights;
	int error;

	error = fget(td, op->sqe.fd, cap_rights_init_one(&rights, CAP_EVENT),
	    &fp);
	if (error != 0) {
		kqring_req_complete(ctx, op, -error, 0);
		return (false);
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		kqring_req_complete(ctx, op, -ENOTSOCK, 0);
		return (false);
	}
	so = fp->f_data;
	soref(so);
	fdrop(fp, td);

	/* A re-armed op (POLL_REMOVE update) was cancelled when unarmed. */
	op->cancelled = false;
	op->listening = false;
	op->so = so;

	/*
	 * Install the upcall(s), then check whether the socket became ready
	 * before they were installed.  Listening sockets have no so_rcv/so_snd
	 * (that storage is reused for the listen queues), so they use the
	 * dedicated listen upcall under the listen lock.
	 */
	bool ready_now;
retry:
	ready_now = false;
	error = 0;
	SOCK_LOCK(so);
	if (SOLISTENING(so)) {
		if (so->sol_upcall != NULL) {
			error = EBUSY;
		} else {
			op->listening = true;
			solisten_upcall_set(so, kqring_soupcall, op);
			ready_now = !TAILQ_EMPTY(&so->sol_comp) ||
			    so->so_error != 0;
		}
		SOCK_UNLOCK(so);
	} else {
		SOCK_UNLOCK(so);
		if ((dirs & KQ_DIR_RCV) != 0)
			error = kqring_sb_arm(op, so, SO_RCV, &ready_now);
		if (error == 0 && (dirs & KQ_DIR_SND) != 0)
			error = kqring_sb_arm(op, so, SO_SND, &ready_now);
		if (error == ERESTART) {
			/* solisten_proto() wiped any upcall we installed. */
			op->dirs = 0;
			goto retry;
		}
	}

	if (error != 0) {
		/*
		 * An upcall installed before the failure may already have
		 * queued op as taskwork; cancelling under ctx->mtx stops
		 * that, and disarm unqueues it and drops the socket.
		 */
		mtx_lock(&ctx->mtx);
		op->cancelled = true;
		kqring_op_disarm_locked(op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, -error, 0);
		return (false);
	}
	op->polled = (op->dirs & KQ_DIR_SND) != 0 &&
	    so->so_proto->pr_domain->dom_family == AF_LOCAL &&
	    so->so_type != SOCK_DGRAM;

	mtx_lock(&ctx->mtx);
	kqring_op_arm_locked(ctx, op);
	if (op->polled)
		ctx->npolled++;
	if (ready_now)
		kqring_taskwork_add_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	return (true);
}

/*
 * Futexes.  A FUTEX_WAIT(V) is armed with its words hashed by umtx key
 * (the same key _umtx_op(2) uses: vmspace + address for FUTEX2_PRIVATE,
 * else the backing VM object + offset for shared mappings), so FUTEX_WAKE
 * from any ring, in any process sharing the memory, finds it.  The waker
 * only marks the request and queues it as taskwork for its ring's owner.
 * Armed words are also re-read on every enter (kqring_check_futexes()), so
 * a word changed without a FUTEX_WAKE still ends the wait, a spurious
 * wakeup futex users must already handle.
 */
static int
kqring_futex_key(uint64_t uaddr, uint32_t flags, struct umtx_key *key)
{
	return (umtx_key_get((void *)(uintptr_t)uaddr, TYPE_SIMPLE_WAIT,
	    (flags & FUTEX2_PRIVATE) != 0 ? THREAD_SHARE : AUTO_SHARE, key));
}

static inline struct kqring_fwait_list *
kqring_futex_bucket(const struct umtx_key *key)
{
	return (&kqring_futex_hash[(u_int)key->hash % KQRING_FUTEX_HASH]);
}

/* Arm FUTEX_WAIT / FUTEX_WAITV; returns false if completed instead. */
static bool
kqring_futex_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_fwait *fw = op->args;
	uint32_t i, uval = 0;
	int32_t res;
	int error = 0;

	for (i = 0; i < op->nfwait; i++) {
		error = kqring_futex_key(fw[i].uaddr, fw[i].flags, &fw[i].key);
		if (error != 0) {
			kqring_req_complete(ctx, op, -error, 0);
			return (false);
		}
		fw[i].keyed = true;
	}

	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	op->futex_woken = false;
	ctx->nfutex++;
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);

	/* Hash before reading the words, so a racing FUTEX_WAKE is seen. */
	mtx_lock(&kqring_futex_mtx);
	for (i = 0; i < op->nfwait; i++) {
		LIST_INSERT_HEAD(kqring_futex_bucket(&fw[i].key), &fw[i], link);
		fw[i].hashed = true;
	}
	mtx_unlock(&kqring_futex_mtx);

	for (i = 0; i < op->nfwait; i++) {
		error = copyin((const void *)(uintptr_t)fw[i].uaddr, &uval,
		    sizeof(uval));
		if (error != 0 || uval != (uint32_t)fw[i].val)
			break;
	}
	if (i == op->nfwait)
		return (true);

	mtx_lock(&ctx->mtx);
	/* A FUTEX_WAKE that already counted this waiter wins. */
	res = op->futex_woken ? op->res : (error != 0 ? -error : -EAGAIN);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, res, 0);
	return (false);
}

/*
 * FUTEX_WAKE: wake up to sqe->addr2 waiters whose mask intersects
 * sqe->addr3, returning how many ring waiters were woken.  Threads
 * sleeping in _umtx_op(UMTX_OP_WAIT_UINT[_PRIVATE]) on the word are woken
 * too, within the same budget.
 */
static int32_t
kqring_futex_wake(const struct io_uring_sqe *sqe)
{
	struct umtx_key key;
	struct kqring_fwait *fw;
	uint64_t nr = MIN(sqe->addr2, (uint64_t)INT32_MAX);
	uint32_t mask = sqe->addr3 != 0 ? (uint32_t)sqe->addr3 :
	    FUTEX_BITSET_MATCH_ANY;
	int32_t n = 0;
	int error;

	error = kqring_futex_key(sqe->addr, (uint32_t)sqe->fd, &key);
	if (error != 0)
		return (-error);
	mtx_lock(&kqring_futex_mtx);
	LIST_FOREACH(fw, kqring_futex_bucket(&key), link) {
		struct kqring_op *op = fw->op;

		if ((uint64_t)n >= nr)
			break;
		if (!umtx_key_match(&fw->key, &key) || (fw->mask & mask) == 0)
			continue;
		mtx_lock(&op->ctx->mtx);
		if (!op->cancelled && !op->futex_woken) {
			op->futex_woken = true;
			op->res = (int32_t)fw->idx;
			kqring_taskwork_add_locked(op->ctx, op);
			n++;
		}
		mtx_unlock(&op->ctx->mtx);
	}
	mtx_unlock(&kqring_futex_mtx);
	umtx_key_release(&key);

	if ((uint64_t)n < nr)
		(void)kern_umtx_wake(curthread, (void *)(uintptr_t)sqe->addr,
		    (int)(nr - (uint64_t)n), (sqe->fd & FUTEX2_PRIVATE) != 0);
	return (n);
}

/*
 * WAITID: one wait6(2) poll with WNOHANG.  EAGAIN keeps the request armed
 * (re-polled by kqring_check_waitids(), as no wakeup reaches us when a
 * child changes state) unless the caller itself asked for WNOHANG.
 */
static int
kqring_exec_waitid(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	siginfo_t si;
	int status, error;

	bzero(&si, sizeof(si));
	td->td_retval[0] = 0;
	error = kern_wait6(td, (idtype_t)sqe->len, (id_t)sqe->fd, &status,
	    (int)sqe->file_index | WNOHANG, NULL, &si);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (td->td_retval[0] == 0) {
		if ((sqe->file_index & WNOHANG) == 0)
			return (EAGAIN);
		/*
		 * proc_to_reap() fills siginfo for a live child too; as
		 * libc's waitid(), report zeroes when nothing changed.
		 */
		bzero(&si, sizeof(si));
	}
	if (sqe->addr2 != 0) {
		error = copyout(&si, (void *)(uintptr_t)sqe->addr2,
		    sizeof(si));
		if (error != 0)
			return (kqring_done(op, -error, 0));
	}
	return (kqring_done(op, 0, 0));
}

static bool
kqring_waitid_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (kqring_exec_waitid(op) == 0) {
		kqring_req_complete(ctx, op, op->res, 0);
		return (false);
	}
	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	ctx->nwaitid++;
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	return (true);
}

/*
 * MSG_RING (IORING_MSG_DATA): post a CQE to the ring sqe->fd, possibly
 * this one.  The CQE is only queued on the target, whose owner posts it
 * from its own taskwork, so no thread ever touches another ring's CQ.
 * Holding the target's file keeps its context alive.
 */
static int32_t
kqring_msg_ring(const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	struct file *fp, *saved;
	struct kqring_ctx *tctx;
	struct kqring_rcqe *rc;
	cap_rights_t rights;
	int error;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_IOCTL),
	    &fp);
	if (error != 0)
		return (-error);
	if (!kqring_fp_is_ring(fp)) {
		fdrop(fp, td);
		return (-EBADF);
	}
	/* devfs_get_cdevpriv() looks at the file being operated on. */
	saved = td->td_fpop;
	td->td_fpop = fp;
	error = devfs_get_cdevpriv((void **)&tctx);
	td->td_fpop = saved;
	if (error != 0) {
		fdrop(fp, td);
		return (-EBADF);
	}

	rc = malloc(sizeof(*rc), M_KQRING, M_WAITOK);
	rc->cqe.user_data = sqe->off;
	rc->cqe.res = (int32_t)sqe->len;
	rc->cqe.flags = (sqe->msg_ring_flags & IORING_MSG_RING_FLAGS_PASS) !=
	    0 ? sqe->file_index : 0;

	mtx_lock(&tctx->mtx);
	if (!tctx->setup_done || !tctx->enabled || tctx->closing) {
		error = EBADF;
	} else if (tctx->nrcq >= KQRING_MAX_REMOTE) {
		error = EOVERFLOW;
	} else {
		STAILQ_INSERT_TAIL(&tctx->rcq, rc, link);
		tctx->nrcq++;
		atomic_set_32(&tctx->shm->sq_flags, IORING_SQ_TASKRUN);
		if (tctx->sleeping)
			wakeup(&tctx->wakeup_chan);
		rc = NULL;
	}
	mtx_unlock(&tctx->mtx);
	free(rc, M_KQRING);
	fdrop(fp, td);
	return (-error);
}

/* ============================================================================
 * Timeouts (TIMEOUT, LINK_TIMEOUT, TIMEOUT_REMOVE)
 * ============================================================================
 */

static int
kqring_copyin_ts(uint64_t uaddr, struct timespec *ts)
{
	struct __kernel_timespec kts;
	int error;

	error = copyin((const void *)(uintptr_t)uaddr, &kts, sizeof(kts));
	if (error != 0)
		return (error);
	if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000LL)
		return (EINVAL);
	ts->tv_sec = kts.tv_sec;
	ts->tv_nsec = (long)kts.tv_nsec;
	return (0);
}

static sbintime_t
kqring_ts_to_sbt(const struct timespec *ts)
{
	struct timespec t = *ts;

	/* sbintime_t holds 31 bits of seconds. */
	if (t.tv_sec > INT32_MAX)
		t.tv_sec = INT32_MAX;
	return (tstosbt(t));
}

/* Time left until op's timeout, from its timespec and timeout_flags. */
static sbintime_t
kqring_timeout_rel(const struct kqring_op *op)
{
	struct timespec now, rel;
	uint32_t tflags = op->sqe.timeout_flags;

	if ((tflags & IORING_TIMEOUT_ABS) == 0)
		return (kqring_ts_to_sbt(&op->ts));
	if ((tflags & IORING_TIMEOUT_REALTIME) != 0)
		nanotime(&now);
	else
		nanouptime(&now);
	if (timespeccmp(&op->ts, &now, <=))
		return (0);
	timespecsub(&op->ts, &now, &rel);
	return (kqring_ts_to_sbt(&rel));
}

static void
kqring_timer_start_locked(struct kqring_op *op)
{
	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (!op->timer_init) {
		callout_init_mtx(&op->callout, &op->ctx->mtx, 0);
		op->timer_init = true;
	}
	callout_reset_sbt(&op->callout, kqring_timeout_rel(op), 0,
	    kqring_timer_cb, op, 0);
	op->timer_pending = true;
}

static int
kqring_timeout_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	uint32_t tflags = sqe->timeout_flags;

	if (sqe->buf_index != 0 || sqe->len != 1 || sqe->splice_fd_in != 0)
		return (EINVAL);
	if ((tflags & ~(IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK |
	    IORING_TIMEOUT_ETIME_SUCCESS | IORING_TIMEOUT_MULTISHOT)) != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_CLOCK_MASK) == IORING_TIMEOUT_CLOCK_MASK)
		return (EINVAL);
	/* A multishot timeout repeats a relative interval. */
	if ((tflags & IORING_TIMEOUT_MULTISHOT) != 0 &&
	    (tflags & IORING_TIMEOUT_ABS) != 0)
		return (EINVAL);
	if (sqe->opcode == IORING_OP_LINK_TIMEOUT &&
	    (sqe->off != 0 || (tflags & IORING_TIMEOUT_MULTISHOT) != 0))
		return (EINVAL);
	return (kqring_copyin_ts(sqe->addr, &op->ts));
}

static int
kqring_timeout_remove_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	uint32_t tflags = sqe->timeout_flags;

	if (sqe->buf_index != 0 || sqe->len != 0 || sqe->splice_fd_in != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) == 0)
		return (tflags != 0 ? EINVAL : 0);
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) == IORING_TIMEOUT_UPDATE_MASK)
		return (EINVAL);
	if ((tflags & ~(IORING_TIMEOUT_UPDATE_MASK | IORING_TIMEOUT_ABS |
	    IORING_TIMEOUT_CLOCK_MASK)) != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_CLOCK_MASK) == IORING_TIMEOUT_CLOCK_MASK)
		return (EINVAL);
	return (kqring_copyin_ts(sqe->addr2, &op->ts));
}

/*
 * Arm a TIMEOUT.  With off != 0 (and no MULTISHOT) it also completes, with
 * res 0, once `off` more CQEs have been posted; for MULTISHOT, off is the
 * number of expirations (0: forever).
 */
static void
kqring_timeout_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	if ((op->sqe.timeout_flags & IORING_TIMEOUT_MULTISHOT) != 0) {
		op->repeats = (uint32_t)op->sqe.off;
	} else if (op->sqe.off != 0) {
		op->target_seq = ctx->cq_seq + (uint32_t)op->sqe.off;
		op->seq_timeout = true;
		ctx->nseqto++;
	}
	kqring_op_arm_locked(ctx, op);
	kqring_timer_start_locked(op);
	mtx_unlock(&ctx->mtx);
}

/*
 * Start the LINK_TIMEOUT that follows op in its chain, now that op is
 * armed.  It moves out of the chain into op->ltimeout; op's successors are
 * those that followed it.
 */
static void
kqring_ltimeout_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_op *lt = op->link;

	if (lt == NULL || lt->sqe.opcode != IORING_OP_LINK_TIMEOUT)
		return;
	op->link = lt->link;
	lt->link = NULL;
	op->ltimeout = lt;
	lt->ltarget = op;
	mtx_lock(&ctx->mtx);
	kqring_timer_start_locked(lt);
	mtx_unlock(&ctx->mtx);
}

/* A timer expired (run as taskwork). */
static void
kqring_timeout_fire(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (op->sqe.opcode == IORING_OP_LINK_TIMEOUT) {
		struct kqring_op *target = op->ltarget;

		/*
		 * Detach first (completing target would cancel us), then post
		 * -ETIME before target's -ECANCELED and its chain's, as
		 * io_uring does.
		 */
		mtx_lock(&ctx->mtx);
		target->ltimeout = NULL;
		op->ltarget = NULL;
		kqring_op_unarm_locked(ctx, target);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, -ETIME, 0);
		kqring_req_complete(ctx, target, -ECANCELED, 0);
		return;
	}
	if ((op->sqe.timeout_flags & IORING_TIMEOUT_MULTISHOT) != 0 &&
	    (op->repeats == 0 || --op->repeats > 0)) {
		kqring_post_cqe_uncounted(ctx, op->sqe.user_data, -ETIME,
		    IORING_CQE_F_MORE);
		mtx_lock(&ctx->mtx);
		kqring_timer_start_locked(op);
		mtx_unlock(&ctx->mtx);
		return;
	}
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, -ETIME, 0);
}

/* TIMEOUT_REMOVE: cancel, or (UPDATE flags) re-arm, a pending timeout. */
static int32_t
kqring_timeout_remove(struct kqring_ctx *ctx, const struct kqring_op *rop)
{
	const struct io_uring_sqe *sqe = &rop->sqe;
	uint32_t tflags = sqe->timeout_flags;
	struct kqring_op *op, *found = NULL;

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
		if ((tflags & IORING_LINK_TIMEOUT_UPDATE) != 0) {
			if (op->ltimeout != NULL &&
			    op->ltimeout->sqe.user_data == sqe->addr) {
				found = op->ltimeout;
				break;
			}
		} else if (op->sqe.opcode == IORING_OP_TIMEOUT &&
		    !op->cancelled && op->sqe.user_data == sqe->addr) {
			found = op;
			break;
		}
	}
	if (found == NULL) {
		mtx_unlock(&ctx->mtx);
		return (-ENOENT);
	}
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) != 0) {
		/* Already expired: its completion is queued. */
		if (found->in_taskwork) {
			mtx_unlock(&ctx->mtx);
			return (-EALREADY);
		}
		found->ts = rop->ts;
		found->sqe.timeout_flags = (found->sqe.timeout_flags &
		    ~(IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK)) |
		    (tflags & (IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK));
		kqring_timer_start_locked(found);
		mtx_unlock(&ctx->mtx);
		return (0);
	}
	kqring_op_unarm_locked(ctx, found);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, found, -ECANCELED, 0);
	return (0);
}

/*
 * Complete TIMEOUTs whose completion count has been reached.  Only the
 * owner thread completes CQEs, so this runs after each batch of them.
 */
static void
kqring_check_timeouts(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	while (ctx->nseqto > 0) {
		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (op->seq_timeout &&
			    (int32_t)(ctx->cq_seq - op->target_seq) >= 0)
				break;
		}
		if (op != NULL)
			kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		if (op == NULL)
			break;
		kqring_req_complete(ctx, op, 0, 0);
	}
}

/* ============================================================================
 * Cancellation (ASYNC_CANCEL, POLL_REMOVE)
 * ============================================================================
 */

static bool
kqring_cancel_match(const struct kqring_op *op, const struct io_uring_sqe *sqe)
{
	uint32_t cflags = sqe->cancel_flags;

	if (op->cancelled)
		return (false);
	if ((cflags & IORING_ASYNC_CANCEL_ANY) != 0)
		return (true);
	if ((cflags & IORING_ASYNC_CANCEL_FD) != 0 && op->sqe.fd != sqe->fd)
		return (false);
	if ((cflags & IORING_ASYNC_CANCEL_OP) != 0 &&
	    op->sqe.opcode != sqe->len)
		return (false);
	/* user_data is matched unless only FD and/or OP were asked for. */
	if (((cflags & (IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_OP)) == 0 ||
	    (cflags & IORING_ASYNC_CANCEL_USERDATA) != 0) &&
	    op->sqe.user_data != sqe->addr)
		return (false);
	return (true);
}

/*
 * Cancel the first matching armed request, or with CANCEL_ALL / CANCEL_ANY
 * every one, returning how many.  Cancelled requests complete -ECANCELED
 * (breaking their link chains); requests not yet issued (linked, deferred)
 * are not found, as in io_uring.
 */
static int32_t
kqring_cancel(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	bool all = (sqe->cancel_flags &
	    (IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY)) != 0;
	struct kqring_op *op;
	int32_t n = 0;

	for (;;) {
		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (kqring_cancel_match(op, sqe))
				break;
		}
		if (op != NULL)
			kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		if (op == NULL)
			break;
		kqring_req_complete(ctx, op, -ECANCELED, 0);
		n++;
		if (!all)
			break;
	}
	if (n == 0)
		return (-ENOENT);
	return (all ? n : 0);
}

/* POLL_REMOVE: cancel an armed POLL_ADD, or update its events/user_data. */
static int32_t
kqring_poll_remove(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	uint32_t pflags = sqe->len;
	struct kqring_op *op;

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
		if (op->sqe.opcode == IORING_OP_POLL_ADD && !op->cancelled &&
		    op->sqe.user_data == sqe->addr)
			break;
	}
	if (op != NULL)
		kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	if (op == NULL)
		return (-ENOENT);

	if ((pflags & (IORING_POLL_UPDATE_EVENTS |
	    IORING_POLL_UPDATE_USER_DATA)) == 0) {
		kqring_req_complete(ctx, op, -ECANCELED, 0);
		return (0);
	}
	/* Update: re-issue the same request with the new parameters. */
	if ((pflags & IORING_POLL_UPDATE_EVENTS) != 0) {
		op->sqe.poll32_events = sqe->poll32_events;
		op->sqe.len = pflags & IORING_POLL_ADD_MULTI;
	}
	if ((pflags & IORING_POLL_UPDATE_USER_DATA) != 0)
		op->sqe.user_data = sqe->off;
	kqring_req_issue(ctx, op);
	return (0);
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
	(IOSQE_IO_DRAIN | IOSQE_IO_LINK | IOSQE_IO_HARDLINK |		\
	IOSQE_ASYNC | IOSQE_BUFFER_SELECT | IOSQE_CQE_SKIP_SUCCESS)

/* msg_flags a receive / send may pass (FreeBSD MSG_* values). */
#define	KQRING_RECV_MSG_FLAGS						\
	(MSG_OOB | MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_DONTWAIT |	\
	MSG_CMSG_CLOEXEC | MSG_CMSG_CLOFORK)
#define	KQRING_SEND_MSG_FLAGS						\
	(MSG_OOB | MSG_DONTROUTE | MSG_EOR | MSG_WAITALL | MSG_DONTWAIT | \
	MSG_EOF | MSG_NOSIGNAL | MSG_MORETOCOME)

/*
 * Validate an SQE and copy in the arguments it points to (paths, iovecs,
 * msghdrs, socket addresses, timespecs, xattr values, futex vectors), as
 * io_uring's prep does, so a linked, drained or armed request does not read
 * user memory later.  Reject flags, ioprio bits and fields kqring does not
 * implement instead of silently ignoring them: IOSQE_FIXED_FILE would turn
 * a fixed-file index into a raw fd.  IOSQE_ASYNC is only a hint.  `prev`
 * is the request this one is linked behind, if any.
 */
static int
kqring_req_prep(struct kqring_op *op, const struct kqring_op *prev)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_msg_args *ma;
	struct kqring_path_args *pa;
	uint32_t ioprio_ok;
	bool multishot = false, buf_select_ok = false;
	int error;

	if ((sqe->flags & ~KQRING_SQE_FLAGS) != 0)
		return (EINVAL);
	/* Registered personalities do not exist (yet). */
	if (sqe->personality != 0)
		return (EINVAL);

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
		if (sqe->rw_flags != 0)
			return (EINVAL);
		buf_select_ok = sqe->opcode == IORING_OP_READ;
		break;
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
		if (sqe->rw_flags != 0)
			return (EINVAL);
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
		if ((sqe->ioprio & ~(IORING_RECVSEND_POLL_FIRST |
		    IORING_RECV_MULTISHOT)) != 0 ||
		    (sqe->msg_flags & ~(uint32_t)KQRING_RECV_MSG_FLAGS) != 0)
			return (EINVAL);
		multishot = (sqe->ioprio & IORING_RECV_MULTISHOT) != 0;
		/* Each multishot completion needs its own buffer. */
		if (multishot && ((sqe->flags & IOSQE_BUFFER_SELECT) == 0 ||
		    (sqe->msg_flags & MSG_WAITALL) != 0))
			return (EINVAL);
		buf_select_ok = true;
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
		/* FIXED_BUF needs registered buffers. */
		ioprio_ok = IORING_RECVSEND_POLL_FIRST;
		if (sqe->opcode == IORING_OP_SEND_ZC ||
		    sqe->opcode == IORING_OP_SENDMSG_ZC)
			ioprio_ok |= IORING_SEND_ZC_REPORT_USAGE;
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
		    SOCK_NONBLOCK | SOCK_CLOFORK)) != 0 ||
		    sqe->len != 0 || sqe->file_index != 0)
			return (EINVAL);
		multishot = (sqe->ioprio & IORING_ACCEPT_MULTISHOT) != 0;
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
		/* file_index (direct descriptors) needs fixed files. */
		if (sqe->rw_flags != 0 || sqe->addr != 0 ||
		    sqe->file_index != 0)
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
		    IORING_POLL_UPDATE_USER_DATA | IORING_POLL_ADD_MULTI)) != 0 ||
		    sqe->len == IORING_POLL_ADD_MULTI)
			return (EINVAL);
		break;
	case IORING_OP_TIMEOUT:
	case IORING_OP_LINK_TIMEOUT:
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
		/* CANCEL_FD_FIXED needs fixed files. */
		if ((sqe->cancel_flags & ~(IORING_ASYNC_CANCEL_ALL |
		    IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ANY |
		    IORING_ASYNC_CANCEL_USERDATA | IORING_ASYNC_CANCEL_OP)) != 0)
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
		/* file_index (direct descriptors) needs fixed files. */
		if (sqe->buf_index != 0 || sqe->file_index != 0)
			return (EINVAL);
		error = kqring_prep_paths(op, false);
		if (error != 0)
			return (error);
		pa = op->args;
		pa->how.flags = sqe->open_flags;
		pa->how.mode = sqe->len;
		break;
	case IORING_OP_OPENAT2:
		if (sqe->buf_index != 0 || sqe->file_index != 0)
			return (EINVAL);
		error = kqring_prep_openat2(op);
		if (error != 0)
			return (error);
		break;
	case IORING_OP_CLOSE:
		if (sqe->off != 0 || sqe->addr != 0 || sqe->len != 0 ||
		    sqe->rw_flags != 0 || sqe->buf_index != 0 ||
		    sqe->file_index != 0)
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
		/* SEND_FD needs fixed files. */
		if (sqe->addr == IORING_MSG_SEND_FD)
			return (EOPNOTSUPP);
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
	}
	if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0 && !buf_select_ok)
		return (EINVAL);
	/* Only buffer group 0 exists. */
	if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0 && sqe->buf_group != 0)
		return (ENOBUFS);
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
 * submission error if it had one, else -ECANCELED.
 */
static void
kqring_chain_fail(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_op *next;

	for (; op != NULL; op = next) {
		next = op->link;
		op->link = NULL;
		kqring_post_cqe(ctx, op->sqe.user_data,
		    op->prep_err != 0 ? -op->prep_err : -ECANCELED, 0);
		kqring_op_free(op);
	}
}

/*
 * Final completion of a request that is no longer armed: post its CQE
 * (unless IOSQE_CQE_SKIP_SUCCESS), cancel its LINK_TIMEOUT, and queue the
 * next request of its chain, or fail the rest of the chain if this one
 * failed without IOSQE_IO_HARDLINK.  Successors are queued on ready_q and
 * issued by kqring_flush_ready(), never recursively, so the kernel stack
 * does not grow with the chain length.  Owner thread, ctx->mtx not held.
 */
static void
kqring_req_complete(struct kqring_ctx *ctx, struct kqring_op *op, int32_t res,
    uint32_t cflags)
{
	struct kqring_op *next, *lt;
	bool failed, zc;

	KASSERT(!op->armed, ("kqring: completing an armed op"));
	failed = res < 0 && !(res == -ETIME &&
	    (op->sqe.opcode == IORING_OP_TIMEOUT ||
	    op->sqe.opcode == IORING_OP_LINK_TIMEOUT) &&
	    (op->sqe.timeout_flags & IORING_TIMEOUT_ETIME_SUCCESS) != 0);

	/*
	 * SEND_ZC / SENDMSG_ZC: IORING_CQE_F_MORE announces the notification
	 * CQE that the buffer may be reused.  kqring copies the data, so
	 * that is right away.
	 */
	zc = op->sqe.opcode == IORING_OP_SEND_ZC ||
	    op->sqe.opcode == IORING_OP_SENDMSG_ZC;
	if (zc)
		cflags |= IORING_CQE_F_MORE;

	if (res < 0 || (op->sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0) {
		if (op->sqe.opcode == IORING_OP_TIMEOUT)
			kqring_post_cqe_uncounted(ctx, op->sqe.user_data, res,
			    cflags);
		else
			kqring_post_cqe(ctx, op->sqe.user_data, res, cflags);
	}
	if (zc)
		kqring_post_cqe(ctx, op->sqe.user_data,
		    (op->sqe.ioprio & IORING_SEND_ZC_REPORT_USAGE) != 0 ?
		    (int32_t)IORING_NOTIF_USAGE_ZC_COPIED : 0,
		    IORING_CQE_F_NOTIF);

	lt = op->ltimeout;
	if (lt != NULL) {
		op->ltimeout = NULL;
		mtx_lock(&ctx->mtx);
		kqring_timer_stop_locked(lt);
		mtx_unlock(&ctx->mtx);
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
		kqring_post_cqe(ctx, lt->sqe.user_data, -ECANCELED, 0);
		kqring_op_free(lt);
	}
	if (next != NULL) {
		if (failed && (op->sqe.flags & IOSQE_IO_HARDLINK) == 0)
			kqring_chain_fail(ctx, next);
		else
			TAILQ_INSERT_TAIL(&ctx->ready_q, next, q_link);
	}
	kqring_op_free(op);
}

/* Whether fd is a socket (lookup errors are left to the op itself). */
static bool
kqring_fd_is_socket(int fd)
{
	struct thread *td = curthread;
	struct file *fp;
	bool sock;

	if (fget(td, fd, &cap_no_rights, &fp) != 0)
		return (false);
	sock = fp->f_type == DTYPE_SOCKET;
	fdrop(fp, td);
	return (sock);
}

/*
 * Issue a request: run it inline, or arm it to finish later from
 * taskwork.  Every path ends in kqring_req_complete() or an armed op.
 */
static void
kqring_req_issue(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	bool armed = false;

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
		/*
		 * A socket would block the ring owner in read(2); treat it like
		 * RECV / SEND instead, so it can be armed.
		 */
		if (!kqring_fd_is_socket(sqe->fd)) {
			kqring_exec_sync(ctx, op);
			kqring_req_complete(ctx, op, op->res, op->cflags);
			return;
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
		 * Try inline first, even with POLL_FIRST (a hint) or
		 * multishot: data that arrived before submission completes in
		 * this same enter, without an upcall + taskwork round trip.
		 * A multishot poll is armed directly, so that readiness that
		 * already exists is reported once (through ready_now).
		 */
		if (sqe->opcode != IORING_OP_POLL_ADD ||
		    (sqe->len & IORING_POLL_ADD_MULTI) == 0) {
			if (kqring_op_attempt(ctx, op, NULL) == 0) {
				kqring_req_complete(ctx, op, op->res,
				    op->cflags);
				return;
			}
		}
		/* IORING_ACCEPT_DONTWAIT: fail rather than wait. */
		if (sqe->opcode == IORING_OP_ACCEPT &&
		    (sqe->ioprio & IORING_ACCEPT_DONTWAIT) != 0) {
			kqring_req_complete(ctx, op, -EAGAIN, 0);
			return;
		}
		armed = kqring_arm_socket_op(ctx, op, kqring_op_dirs(sqe));
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
		kqring_req_complete(ctx, op, kqring_msg_ring(sqe), 0);
		return;
	case IORING_OP_ASYNC_CANCEL:
		kqring_req_complete(ctx, op, kqring_cancel(ctx, sqe), 0);
		return;
	case IORING_OP_TIMEOUT_REMOVE:
		kqring_req_complete(ctx, op, kqring_timeout_remove(ctx, op), 0);
		return;
	case IORING_OP_POLL_REMOVE:
		kqring_req_complete(ctx, op, kqring_poll_remove(ctx, sqe), 0);
		return;
	default:
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
static void
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
		kqring_chain_fail(ctx, head);
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
static void
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
 * Run one queued taskwork item: a socket that became ready, or an expired
 * timer.
 */
static void
kqring_taskwork_run_one(struct kqring_ctx *ctx, struct kqring_op *op)
{
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
	if (kqring_op_attempt(ctx, op, op->so) == EAGAIN)
		return;
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, op->res, op->cflags);
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
		kqring_post_cqe(ctx, rc->cqe.user_data, rc->cqe.res,
		    rc->cqe.flags);
		free(rc, M_KQRING);
	}
	mtx_lock(&ctx->mtx);
}

/*
 * Run deferred taskwork in curthread context (DEFER_TASKRUN), post
 * MSG_RING CQEs, and issue the link successors it unblocks.
 */
static void
kqring_run_taskwork(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	/* Successors issued below may queue more taskwork; bound the loop. */
	for (int pass = 0; pass < 4; pass++) {
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
				if (op->cancelled || ctx->closing)
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

/* ============================================================================
 * Device File Operations (/dev/kqring)
 * ============================================================================
 */

static void
kqring_dtor(void *data)
{
	struct kqring_ctx *ctx = data;
	struct kqring_op *op;
	struct kqring_ocqe *ocqe;
	struct kqring_rcqe *rc;

	/* closing stops upcalls and callouts from queueing taskwork. */
	mtx_lock(&ctx->mtx);
	ctx->closing = true;
	while ((op = TAILQ_FIRST(&ctx->active_ops)) != NULL) {
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_destroy(ctx, op);
		mtx_lock(&ctx->mtx);
	}
	mtx_unlock(&ctx->mtx);
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
	while ((ocqe = STAILQ_FIRST(&ctx->ocq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->ocq, link);
		free(ocqe, M_KQRING);
	}
	/* closing (set under mtx) stops MSG_RING senders from adding more. */
	while ((rc = STAILQ_FIRST(&ctx->rcq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->rcq, link);
		free(rc, M_KQRING);
	}

	kqring_free_shm(ctx->ring_obj, ctx->ring_kva, ctx->ring_alloc_len);
	kqring_free_shm(ctx->sqes_obj, ctx->sqes_kva, ctx->sqes_alloc_len);
	mtx_destroy(&ctx->mtx);
	free(ctx, M_KQRING);
}

static int
kqring_open(struct cdev *dev __unused, int oflags __unused, int devtype __unused,
    struct thread *td __unused)
{
	struct kqring_ctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_KQRING, M_WAITOK | M_ZERO);
	mtx_init(&ctx->mtx, "kqring", NULL, MTX_DEF);
	TAILQ_INIT(&ctx->active_ops);
	TAILQ_INIT(&ctx->taskwork_ops);
	TAILQ_INIT(&ctx->free_ops);
	TAILQ_INIT(&ctx->ready_q);
	TAILQ_INIT(&ctx->defer_q);
	STAILQ_INIT(&ctx->ocq);
	STAILQ_INIT(&ctx->rcq);

	error = devfs_set_cdevpriv(ctx, kqring_dtor);
	if (error != 0) {
		mtx_destroy(&ctx->mtx);
		free(ctx, M_KQRING);
	}
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

static int
kqring_setup_rings(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	struct io_uring_params p;
	uint32_t sq_entries, cq_entries;
	size_t ring_bytes, sqes_bytes, array_off, cqes_off;
	int error;

	error = copyin(uarg->params, &p, sizeof(p));
	if (error != 0)
		return (error);

	/*
	 * Reject every flag kqring does not implement (SQPOLL: kqring never
	 * spawns background kernel threads; SQE128/CQE32/NO_SQARRAY would
	 * change the ring layout userspace uses), as io_uring_setup(2) does,
	 * so that callers probing for a feature see EINVAL rather than a
	 * silently different ring.  Require SINGLE_ISSUER | DEFER_TASKRUN,
	 * the only semantics kqring implements.  COOP_TASKRUN, TASKRUN_FLAG
	 * and SUBMIT_ALL describe what kqring always does.
	 */
	if ((p.flags & ~KQRING_SETUP_FLAGS) != 0)
		return (EINVAL);
	if ((p.flags & IORING_SETUP_SINGLE_ISSUER) == 0 ||
	    (p.flags & IORING_SETUP_DEFER_TASKRUN) == 0)
		return (EINVAL);
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

	array_off = sizeof(struct kqring_shm_header);
	cqes_off = array_off + sq_entries * sizeof(uint32_t);
	ring_bytes = cqes_off + cq_entries * sizeof(struct io_uring_cqe);
	sqes_bytes = sq_entries * sizeof(struct io_uring_sqe);

	error = kqring_alloc_shm(ring_bytes, &ctx->ring_obj, &ctx->ring_kva,
	    &ctx->ring_alloc_len);
	if (error != 0)
		return (error);

	error = kqring_alloc_shm(sqes_bytes, &ctx->sqes_obj, &ctx->sqes_kva,
	    &ctx->sqes_alloc_len);
	if (error != 0) {
		kqring_free_shm(ctx->ring_obj, ctx->ring_kva,
		    ctx->ring_alloc_len);
		ctx->ring_obj = NULL;
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
	ctx->enabled = (p.flags & IORING_SETUP_R_DISABLED) == 0;
	if (ctx->enabled)
		ctx->owner_td = td;

	ctx->shm->sq_ring_mask = ctx->sq_mask;
	ctx->shm->sq_ring_entries = sq_entries;
	ctx->shm->cq_ring_mask = ctx->cq_mask;
	ctx->shm->cq_ring_entries = cq_entries;

	p.sq_entries = sq_entries;
	p.cq_entries = cq_entries;
	p.features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP |
	    IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_RW_CUR_POS |
	    IORING_FEAT_EXT_ARG | IORING_FEAT_FAST_POLL |
	    IORING_FEAT_POLL_32BITS | IORING_FEAT_CQE_SKIP |
	    IORING_FEAT_LINKED_FILE;

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
	if (error != 0) {
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
		ctx->owner_td = NULL;
		return (error);
	}
	return (0);
}

static int
kqring_ioc_setup(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	int error;

	/* Serialize against a concurrent setup on the same descriptor. */
	mtx_lock(&ctx->mtx);
	if (ctx->setup_done || ctx->setup_busy) {
		mtx_unlock(&ctx->mtx);
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
	return (error);
}

/*
 * Queue polled ops (armed SO_SND on AF_UNIX streams, which get no upcall)
 * whose socket has become writable.  Called with ctx->mtx held, so the
 * socket buffer is read without its lock: a stale value only causes a
 * spurious EAGAIN attempt or a delay until the next 50 us tick.
 */
static void
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

/*
 * Complete armed FUTEX_WAIT(V) ops one of whose words no longer holds the
 * expected value (or can no longer be read), with that word's index.
 * Only the owner thread (this one) modifies active_ops, so it is walked
 * without ctx->mtx, which must not be held across copyin().
 */
static void
kqring_check_futexes(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *tmp;
	struct kqring_fwait *fw;
	uint32_t i, uval;
	int32_t res;
	int error = 0;

	if (ctx->nfutex == 0)
		return;
	TAILQ_FOREACH_SAFE(op, &ctx->active_ops, active_link, tmp) {
		if (op->nfwait == 0 || op->cancelled)
			continue;
		fw = op->args;
		for (i = 0; i < op->nfwait; i++) {
			error = copyin((const void *)(uintptr_t)fw[i].uaddr,
			    &uval, sizeof(uval));
			if (error != 0 || uval != (uint32_t)fw[i].val)
				break;
		}
		if (i == op->nfwait)
			continue;
		/*
		 * Completion only queues link successors (never touches
		 * active_ops), so tmp stays valid.
		 */
		mtx_lock(&ctx->mtx);
		res = op->futex_woken ? op->res :
		    (error != 0 ? -error : (int32_t)i);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, res, 0);
	}
}

/* Complete armed WAITIDs whose child changed state. */
static void
kqring_check_waitids(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *tmp;

	if (ctx->nwaitid == 0)
		return;
	TAILQ_FOREACH_SAFE(op, &ctx->active_ops, active_link, tmp) {
		if (op->sqe.opcode != IORING_OP_WAITID || op->cancelled ||
		    kqring_exec_waitid(op) == EAGAIN)
			continue;
		mtx_lock(&ctx->mtx);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, op->res, 0);
	}
}

/* Complete what became ready without an upcall, and issue successors. */
static void
kqring_run_checks(struct kqring_ctx *ctx)
{
	kqring_check_futexes(ctx);
	kqring_check_waitids(ctx);
	kqring_check_timeouts(ctx);
	kqring_flush_ready(ctx);
}

static int kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt);

static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t head, tail, consumed = 0, submitted = 0;
	struct kqring_op *chain_head = NULL, *chain_tail = NULL;
	bool chain_failed = false;
	sbintime_t sbt = 0;
	bool has_timeout = false;
	bool wait;
	int error = 0;

	if (!ctx->setup_done || !ctx->enabled)
		return (EBADF);
	if (ctx->owner_td != td)
		return (EEXIST); /* SINGLE_ISSUER violation */

	/* SQ_WAKEUP/SQ_WAIT need SQPOLL; REGISTERED_RING has no ioctl form. */
	if ((ea->flags & ~KQRING_ENTER_FLAGS) != 0)
		return (EINVAL);
	/* Without EXT_ARG, io_uring_enter(2)'s arg is a sigmask. */
	if ((ea->flags & IORING_ENTER_EXT_ARG) == 0 && ea->arg != NULL)
		return (EINVAL);

	wait = (ea->flags & IORING_ENTER_GETEVENTS) != 0 &&
	    ea->min_complete != 0;

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

	/*
	 * 2. Drain and submit queued SQEs.  SQEs carrying IOSQE_IO_LINK are
	 * gathered into a chain that is submitted once its last SQE (one
	 * without the flag, or the last of this batch, as in io_uring) has
	 * been read.  Each SQE is copied out of the shared array before it
	 * is validated, so userspace cannot change it afterwards.
	 */
	kqring_flush_overflow(ctx);
	head = shm->sq_head;
	tail = atomic_load_acq_32(&shm->sq_tail);
	while (head != tail && consumed < ea->to_submit) {
		uint32_t sqe_idx = ctx->sq_array[head & ctx->sq_mask];
		head++;
		consumed++;
		if (sqe_idx >= ctx->sq_entries) {
			/* As in io_uring: consumed, counted, not submitted. */
			atomic_add_32(&shm->sq_dropped, 1);
			continue;
		}
		struct kqring_op *op = kqring_op_alloc(ctx);
		op->sqe = ctx->sqes[sqe_idx];
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
			kqring_chain_submit(ctx, chain_head, chain_failed);
			chain_head = chain_tail = NULL;
		}
		submitted++;
	}
	if (chain_head != NULL)
		kqring_chain_submit(ctx, chain_head, chain_failed);
	atomic_store_rel_32(&shm->sq_head, head);
	ea->SUBMITTED_out = submitted;

	/* 3. Run pending DEFER_TASKRUN completions, futex and timeout checks */
	kqring_run_taskwork(ctx);
	kqring_run_checks(ctx);

	if (!wait)
		return (0);

	/*
	 * 4. Wait.  As with io_uring_enter(2), once SQEs have been consumed
	 * the call reports success (the submitted count) even if the wait
	 * then times out or is interrupted: an ioctl error would make
	 * sys_ioctl() skip copying SUBMITTED_out back to userspace.
	 */
	error = kqring_wait_cqes(ctx, ea->min_complete, has_timeout, sbt);
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
    bool has_timeout, sbintime_t sbt)
{
	struct kqring_shm_header *shm = ctx->shm;
	bool may_poll = true;
	int error;

	for (;;) {
		kqring_run_taskwork(ctx);
		kqring_run_checks(ctx);
		kqring_flush_overflow(ctx);

		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		/* A full CQ with overflow pending must be reaped first. */
		if (cq_count >= min_complete || !STAILQ_EMPTY(&ctx->ocq))
			return (0);

		if (has_timeout && sbt == 0)
			return (ETIMEDOUT);

		mtx_lock(&ctx->mtx);
		/* Poll at most once per sleep so EAGAIN can't spin us. */
		if (ctx->npolled > 0 && may_poll) {
			kqring_poll_fallback_locked(ctx);
			may_poll = false;
		}
		if (!TAILQ_EMPTY(&ctx->taskwork_ops) ||
		    !STAILQ_EMPTY(&ctx->rcq)) {
			mtx_unlock(&ctx->mtx);
			continue;
		}
		/*
		 * If a futex or polled op is armed, wake periodically (50 us)
		 * to re-check it in addition to socket upcalls; a WAITID is
		 * re-polled every millisecond.
		 */
		sbintime_t sleep_sbt = sbt, period = 0;
		if (ctx->nfutex > 0 || ctx->npolled > 0)
			period = 50 * SBT_1US;
		else if (ctx->nwaitid > 0)
			period = SBT_1MS;
		if (period != 0 && (!has_timeout || sleep_sbt > period))
			sleep_sbt = period;

		/* Timeout is sbintime_t, not ticks: use the _sbt variant. */
		ctx->sleeping = true;
		error = msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
		    "kqwait", sleep_sbt, 0, 0);
		ctx->sleeping = false;
		may_poll = true;
		mtx_unlock(&ctx->mtx);

		if (error == EINTR || error == ERESTART)
			return (EINTR);
		if (error == EWOULDBLOCK && has_timeout) {
			if (sbt <= sleep_sbt)
				return (ETIMEDOUT);
			sbt -= sleep_sbt;
		}
	}
}

static int
kqring_ioc_register(struct kqring_ctx *ctx, struct kqring_register_arg *ra,
    struct thread *td)
{
	int error;

	if (!ctx->setup_done)
		return (EBADF);

	switch (ra->opcode) {
	case IORING_REGISTER_ENABLE_RINGS:
		mtx_lock(&ctx->mtx);
		if (ctx->enabled) {
			mtx_unlock(&ctx->mtx);
			return (EINVAL);
		}
		ctx->owner_td = td;
		ctx->enabled = true;
		mtx_unlock(&ctx->mtx);
		return (0);

	case IORING_REGISTER_PROBE: {
		struct io_uring_probe hdr;
		if (ra->arg == NULL)
			return (EINVAL);
		error = copyin(ra->arg, &hdr, sizeof(hdr));
		if (error != 0)
			return (error);

		static const uint8_t supported_ops[] = {
			IORING_OP_NOP,
			IORING_OP_READV,
			IORING_OP_WRITEV,
			IORING_OP_FSYNC,
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
			IORING_OP_STATX,
			IORING_OP_READ,
			IORING_OP_WRITE,
			IORING_OP_FADVISE,
			IORING_OP_MADVISE,
			IORING_OP_SEND,
			IORING_OP_RECV,
			IORING_OP_OPENAT2,
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
			IORING_OP_FTRUNCATE,
			IORING_OP_BIND,
			IORING_OP_LISTEN,
		};
		/*
		 * As in io_uring: last_op is the last opcode this ABI knows
		 * and ops_len the number of entries filled in, at most
		 * IORING_OP_LAST (it is a uint8_t, so never 256).
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

	case IORING_REGISTER_PBUF_RING: {
		struct io_uring_buf_reg reg;
		if (ra->nr_args != 1 || ra->arg == NULL)
			return (EINVAL);
		if (ctx->owner_td != td)
			return (EEXIST);
		error = copyin(ra->arg, &reg, sizeof(reg));
		if (error != 0)
			return (error);
		if (reg.bgid != 0 || reg.ring_entries == 0 ||
		    reg.ring_entries > KQRING_MAX_PBUF_ENTRIES ||
		    (reg.ring_entries & (reg.ring_entries - 1)) != 0)
			return (EINVAL);

		ctx->pbuf.user_ring_addr = reg.ring_addr;
		ctx->pbuf.ring_entries = reg.ring_entries;
		ctx->pbuf.ring_mask = reg.ring_entries - 1;
		ctx->pbuf.head = 0;
		ctx->pbuf.registered = true;
		return (0);
	}

	case IORING_UNREGISTER_PBUF_RING:
		if (ctx->owner_td != td)
			return (EEXIST);
		ctx->pbuf.registered = false;
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

static int
kqring_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	struct kqring_ctx *ctx;
	int error;

	error = devfs_get_cdevpriv((void **)&ctx);
	if (error != 0)
		return (error);

	switch (cmd) {
	case KQRING_IOC_SETUP:
		error = kqring_ioc_setup(ctx, (struct kqring_setup_arg *)data,
		    td);
		break;
	case KQRING_IOC_ENTER:
		error = kqring_ioc_enter(ctx, (struct kqring_enter_arg *)data,
		    td);
		break;
	case KQRING_IOC_REGISTER:
		error = kqring_ioc_register(ctx,
		    (struct kqring_register_arg *)data, td);
		break;
	default:
		return (ENOTTY);
	}
	/*
	 * kern_*() calls made for SQEs (openat, accept, socket...) leave
	 * their result in td_retval[0], which would become ioctl(2)'s
	 * return value.
	 */
	td->td_retval[0] = 0;
	return (error);
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
		for (int i = 0; i < KQRING_FUTEX_HASH; i++)
			LIST_INIT(&kqring_futex_hash[i]);
		kqring_cdev = make_dev(&kqring_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0666, "kqring");
		if (kqring_cdev == NULL) {
			mtx_destroy(&kqring_futex_mtx);
			return (ENOMEM);
		}
		printf("kqring: loaded (/dev/kqring, SINGLE_ISSUER | DEFER_TASKRUN)\n");
		return (0);

	case MOD_UNLOAD:
		/*
		 * destroy_dev() runs the destructor of every ring still open,
		 * so no futex is hashed after it.
		 */
		if (kqring_cdev != NULL)
			destroy_dev(kqring_cdev);
		mtx_destroy(&kqring_futex_mtx);
		printf("kqring: unloaded\n");
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(kqring, kqring_modevent, NULL);
MODULE_VERSION(kqring, 1);
