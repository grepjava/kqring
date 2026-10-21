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
#include <sys/capsicum.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

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
 * An in-flight asynchronous operation armed on a socket or futex.
 * Holds soref(so) (not fhold(fp)) while armed on a socket so that user-space
 * close(fd) still triggers soclose() + TCP FIN immediately and fires our
 * soupcall with SBS_CANTRCVMORE.
 */
struct kqring_op {
	TAILQ_ENTRY(kqring_op)	active_link;
	TAILQ_ENTRY(kqring_op)	taskwork_link;
	struct kqring_ctx	*ctx;
	struct socket		*so;		/* soref'd while armed */
	int			dirs;		/* KQ_DIR_* upcalls installed */
	bool			listening;	/* armed via solisten_upcall_set */
	bool			in_taskwork;
	bool			cancelled;
	bool			polled;		/* no upcall; see npolled */
	struct io_uring_sqe	sqe;
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
	TAILQ_HEAD(, kqring_op)	active_ops;
	TAILQ_HEAD(, kqring_op)	taskwork_ops;
	int			nfutex;		/* armed FUTEX_WAIT ops */
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
	TAILQ_HEAD(, kqring_op)	free_ops;
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
 * cq_overflow.  Never called with ctx->mtx held.
 */
static void
kqring_post_cqe(struct kqring_ctx *ctx, uint64_t user_data, int32_t res,
    uint32_t flags)
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
static int
kqring_soupcall(struct socket *so __unused, void *arg, int waitflag __unused)
{
	struct kqring_op *op = arg;
	struct kqring_ctx *ctx = op->ctx;

	mtx_lock(&ctx->mtx);
	if (!op->cancelled && !ctx->closing && !op->in_taskwork) {
		op->in_taskwork = true;
		TAILQ_INSERT_TAIL(&ctx->taskwork_ops, op, taskwork_link);
		if (ctx->shm != NULL) {
			atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
		}
		/*
		 * ctx->sleeping is only changed under ctx->mtx, and the owner
		 * re-checks taskwork_ops under ctx->mtx before sleeping, so
		 * skipping wakeup() (sleepq lookup) here cannot lose a wakeup.
		 */
		if (ctx->sleeping)
			wakeup(&ctx->wakeup_chan);
	}
	mtx_unlock(&ctx->mtx);
	return (SU_OK);
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

static void
kqring_op_disarm_locked(struct kqring_op *op)
{
	struct socket *so = op->so;

	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (op->polled) {
		op->polled = false;
		op->ctx->npolled--;
	}
	if (op->in_taskwork) {
		TAILQ_REMOVE(&op->ctx->taskwork_ops, op, taskwork_link);
		op->in_taskwork = false;
	}
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
	return (op);
}

static void
kqring_op_free(struct kqring_op *op)
{
	struct kqring_ctx *ctx = op->ctx;

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

/*
 * Try a non-blocking socket receive (direct buffer or IOSQE_BUFFER_SELECT).
 * Returns:
 *   0: completed (CQE posted), and if *keep_multishot_out is true, keep armed.
 *   EAGAIN: socket would block; caller should arm (or keep armed) upcall.
 * *more_out is an unlocked hint that another receive would not block
 * (bytes still buffered or EOF), used to end multishot drain loops without
 * a final EAGAIN soreceive(); a stale value is harmless because the upcall
 * stays armed.
 *
 * For an armed op (taskwork), `expect` is the socket the op was armed on.
 * All exec functions re-resolve sqe->fd (for Capsicum rights), and if the
 * descriptor was closed or reused for another file in the meantime they
 * complete with -EBADF rather than operating on the new file.
 */
static int
kqring_exec_recv(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    struct socket *expect, bool *keep_multishot_out, bool *more_out)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct uio auio;
	struct iovec aiov;
	struct io_uring_buf pbuf;
	cap_rights_t rights;
	void *uaddr;
	size_t ulen;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool multishot = (sqe->ioprio & IORING_RECV_MULTISHOT) != 0;
	int error, flags;

	*keep_multishot_out = false;
	*more_out = false;

	error = fget_read(td, sqe->fd, cap_rights_init_one(&rights, CAP_RECV),
	    &fp);
	if (error != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -ENOTSOCK, 0);
		return (0);
	}
	so = fp->f_data;
	if (expect != NULL && so != expect) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EBADF, 0);
		return (0);
	}

	/*
	 * If buffer selection is requested, check whether bytes or EOF/error
	 * are actually waiting before popping a buffer, and only commit the
	 * buffer when > 0 bytes are read.
	 */
	if (buf_select) {
		SOCK_RECVBUF_LOCK(so);
		/* Checked under the sockbuf lock; see kqring_sb_disarm(). */
		if (SOLISTENING(so)) {
			SOCK_RECVBUF_UNLOCK(so);
			fdrop(fp, td);
			kqring_post_cqe(ctx, sqe->user_data, -ENOTCONN, 0);
			return (0);
		}
		bool readable = (sbavail(&so->so_rcv) > 0) ||
		    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0 ||
		    so->so_error != 0;
		SOCK_RECVBUF_UNLOCK(so);
		if (!readable) {
			fdrop(fp, td);
			return (EAGAIN);
		}
		error = kqring_pbuf_peek(ctx, &pbuf);
		if (error != 0) {
			fdrop(fp, td);
			kqring_post_cqe(ctx, sqe->user_data, -error, 0);
			return (0);
		}
		uaddr = (void *)(uintptr_t)pbuf.addr;
		ulen = pbuf.len;
	} else {
		uaddr = (void *)(uintptr_t)sqe->addr;
		ulen = sqe->len;
	}

	aiov.iov_base = uaddr;
	aiov.iov_len = ulen;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_resid = ulen;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = td;

	flags = MSG_DONTWAIT | MSG_NOSIGNAL;
	error = soreceive(so, NULL, &auio, NULL, NULL, &flags);
	size_t transferred = ulen - auio.uio_resid;

	if (error == EWOULDBLOCK && transferred == 0) {
		fdrop(fp, td);
		return (EAGAIN);
	}

	if (error != 0 && transferred == 0) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}

	/*
	 * Commit the PBUF_RING buffer ONLY when transferred > 0!
	 * On EOF (transferred == 0), we leave the buffer in PBUF_RING and
	 * post res = 0 without IORING_CQE_F_BUFFER.
	 */
	if (buf_select && transferred > 0) {
		kqring_pbuf_commit(ctx);
		cqe_flags |= IORING_CQE_F_BUFFER |
		    ((uint32_t)pbuf.bid << IORING_CQE_BUFFER_SHIFT);
	}

	if (multishot && transferred > 0) {
		cqe_flags |= IORING_CQE_F_MORE;
		*keep_multishot_out = true;
		*more_out = !SOLISTENING(so) && (sbavail(&so->so_rcv) > 0 ||
		    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0);
	}

	fdrop(fp, td);
	kqring_post_cqe(ctx, sqe->user_data, (int32_t)transferred, cqe_flags);
	return (0);
}

static int
kqring_exec_send(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    struct socket *expect)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct uio auio;
	struct iovec aiov;
	cap_rights_t rights;
	int error;

	error = fget_write(td, sqe->fd, cap_rights_init_one(&rights, CAP_SEND),
	    &fp);
	if (error != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -ENOTSOCK, 0);
		return (0);
	}
	so = fp->f_data;
	if (expect != NULL && so != expect) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EBADF, 0);
		return (0);
	}

	aiov.iov_base = (void *)(uintptr_t)sqe->addr;
	aiov.iov_len = sqe->len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_resid = sqe->len;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = td;

	error = sosend(so, NULL, &auio, NULL, NULL, MSG_DONTWAIT | MSG_NOSIGNAL,
	    td);
	size_t sent = sqe->len - auio.uio_resid;
	fdrop(fp, td);

	if (error == EWOULDBLOCK && sent == 0)
		return (EAGAIN);
	if (error != 0 && sent == 0)
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
	else
		kqring_post_cqe(ctx, sqe->user_data, (int32_t)sent, 0);
	return (0);
}

static int
kqring_exec_accept(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    struct socket *expect)
{
	struct thread *td = curthread;
	struct file *headfp = NULL, *nfp = NULL;
	struct socket *headso;
	struct sockaddr_storage ss = { .ss_len = sizeof(ss) };
	cap_rights_t rights;
	int error, nfd = -1, aflags;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_ACCEPT),
	    &headfp);
	if (error != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}
	if (headfp->f_type != DTYPE_SOCKET) {
		fdrop(headfp, td);
		kqring_post_cqe(ctx, sqe->user_data, -ENOTSOCK, 0);
		return (0);
	}
	headso = headfp->f_data;
	if (expect != NULL && headso != expect) {
		fdrop(headfp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EBADF, 0);
		return (0);
	}
	if ((headso->so_options & SO_ACCEPTCONN) == 0) {
		fdrop(headfp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EINVAL, 0);
		return (0);
	}
	/* Listening sockets have no so_rcv; only sol_comp/so_error apply. */
	if (TAILQ_EMPTY(&headso->sol_comp) && headso->so_error == 0) {
		fdrop(headfp, td);
		return (EAGAIN);
	}
	fdrop(headfp, td);

	aflags = SOCK_NONBLOCK;
	if (sqe->accept_flags & SOCK_CLOEXEC)
		aflags |= SOCK_CLOEXEC;

	error = kern_accept4(td, sqe->fd, (struct sockaddr *)&ss, aflags,
	    &nfp);
	if (error != 0) {
		if (error == EWOULDBLOCK)
			return (EAGAIN);
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}
	nfd = td->td_retval[0];
	if (nfp != NULL)
		fdrop(nfp, td);
	kqring_post_cqe(ctx, sqe->user_data, nfd, 0);
	return (0);
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
kqring_exec_poll(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    struct socket *expect)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	cap_rights_t rights;
	int error, revents;
	int events = sqe->poll32_events ? (int)sqe->poll32_events :
	    sqe->poll_events;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_POLL_EVENT),
	    &fp);
	if (error != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return (0);
	}
	if (fp->f_type != DTYPE_SOCKET) {
		/* Only sockets can be armed (see kqring_arm_socket_op). */
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EOPNOTSUPP, 0);
		return (0);
	}
	if (expect != NULL && fp->f_data != expect) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -EBADF, 0);
		return (0);
	}
	revents = kqring_so_revents(fp->f_data, events);
	fdrop(fp, td);

	if (revents == 0)
		return (EAGAIN);

	kqring_post_cqe(ctx, sqe->user_data, revents, 0);
	return (0);
}

static int
kqring_exec_file_rw(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    bool is_write)
{
	struct thread *td = curthread;
	struct uio auio;
	struct iovec aiov;
	off_t offset = (off_t)sqe->off;
	int error;

	aiov.iov_base = (void *)(uintptr_t)sqe->addr;
	aiov.iov_len = sqe->len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_resid = sqe->len;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = is_write ? UIO_WRITE : UIO_READ;
	auio.uio_td = td;

	auio.uio_offset = (offset == (off_t)-1) ? 0 : offset;

	/* off == -1 means "use the file position", as in io_uring. */
	if (offset == (off_t)-1)
		error = is_write ? kern_writev(td, sqe->fd, &auio) :
		    kern_readv(td, sqe->fd, &auio);
	else
		error = is_write ? kern_pwritev(td, sqe->fd, &auio, offset) :
		    kern_preadv(td, sqe->fd, &auio, offset);

	size_t done = sqe->len - auio.uio_resid;
	if (error != 0 && done == 0)
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
	else
		kqring_post_cqe(ctx, sqe->user_data, (int32_t)done, 0);
	return (0);
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

/*
 * Arm sqe on its socket for the KQ_DIR_* directions in `dirs` (ignored for
 * listening sockets).  Failures complete the SQE with an error CQE.
 */
static void
kqring_arm_socket_op(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    int dirs)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct kqring_op *op;
	cap_rights_t rights;
	int error;

	error = fget(td, sqe->fd, cap_rights_init_one(&rights, CAP_EVENT), &fp);
	if (error != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return;
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -ENOTSOCK, 0);
		return;
	}
	so = fp->f_data;
	soref(so);
	fdrop(fp, td);

	op = kqring_op_alloc(ctx);
	op->so = so;
	op->sqe = *sqe;

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
		kqring_op_free(op);
		kqring_post_cqe(ctx, sqe->user_data, -error, 0);
		return;
	}
	op->polled = (op->dirs & KQ_DIR_SND) != 0 &&
	    so->so_proto->pr_domain->dom_family == AF_LOCAL &&
	    so->so_type != SOCK_DGRAM;

	mtx_lock(&ctx->mtx);
	TAILQ_INSERT_TAIL(&ctx->active_ops, op, active_link);
	if (op->polled)
		ctx->npolled++;
	if (ready_now && !op->in_taskwork) {
		op->in_taskwork = true;
		TAILQ_INSERT_TAIL(&ctx->taskwork_ops, op, taskwork_link);
		atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
	}
	mtx_unlock(&ctx->mtx);
}

/*
 * Reject SQE flags and ioprio bits kqring does not implement, as io_uring
 * does, instead of silently ignoring them: IOSQE_FIXED_FILE would turn a
 * fixed-file index into a raw fd, and links/drain/CQE skipping change
 * completion semantics.  IOSQE_ASYNC is only a hint and is accepted.
 */
static int
kqring_sqe_check(const struct io_uring_sqe *sqe)
{
	if ((sqe->flags & ~(IOSQE_BUFFER_SELECT | IOSQE_ASYNC)) != 0)
		return (EINVAL);

	switch (sqe->opcode) {
	case IORING_OP_RECV:
		if ((sqe->ioprio & ~(IORING_RECVSEND_POLL_FIRST |
		    IORING_RECV_MULTISHOT)) != 0)
			return (EINVAL);
		/* Each multishot completion needs its own buffer. */
		if ((sqe->ioprio & IORING_RECV_MULTISHOT) != 0 &&
		    (sqe->flags & IOSQE_BUFFER_SELECT) == 0)
			return (EINVAL);
		/* Only buffer group 0 exists. */
		if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0 &&
		    sqe->buf_group != 0)
			return (ENOBUFS);
		return (0);
	case IORING_OP_SEND:
		if ((sqe->ioprio & ~IORING_RECVSEND_POLL_FIRST) != 0)
			return (EINVAL);
		break;
	}
	if ((sqe->flags & IOSQE_BUFFER_SELECT) != 0)
		return (EINVAL);
	return (0);
}

static void
kqring_submit_sqe(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	bool keep_multishot = false;
	int rc;

	rc = kqring_sqe_check(sqe);
	if (rc != 0) {
		kqring_post_cqe(ctx, sqe->user_data, -rc, 0);
		return;
	}

	switch (sqe->opcode) {
	case IORING_OP_NOP:
		kqring_post_cqe(ctx, sqe->user_data, 0, 0);
		break;

	case IORING_OP_RECV: {
		/*
		 * Always try inline first, even with POLL_FIRST (a hint) or
		 * multishot: data that arrived before submission completes in
		 * this same enter, without an upcall + taskwork round trip.
		 */
		bool multishot = (sqe->ioprio & IORING_RECV_MULTISHOT) != 0;
		bool more = false;

		do {
			rc = kqring_exec_recv(ctx, sqe, NULL, &keep_multishot,
			    &more);
		} while (multishot && rc == 0 && keep_multishot && more);
		if (rc == EAGAIN || (multishot && rc == 0 && keep_multishot))
			kqring_arm_socket_op(ctx, sqe, KQ_DIR_RCV);
		break;
	}

	case IORING_OP_SEND:
		rc = kqring_exec_send(ctx, sqe, NULL);
		if (rc == EAGAIN)
			kqring_arm_socket_op(ctx, sqe, KQ_DIR_SND);
		break;

	case IORING_OP_ACCEPT:
		rc = kqring_exec_accept(ctx, sqe, NULL);
		if (rc == EAGAIN)
			kqring_arm_socket_op(ctx, sqe, KQ_DIR_RCV);
		break;

	case IORING_OP_POLL_ADD: {
		uint32_t events = sqe->poll32_events ? sqe->poll32_events :
		    sqe->poll_events;
		rc = kqring_exec_poll(ctx, sqe, NULL);
		if (rc == EAGAIN) {
			/*
			 * Watch every direction asked for; anything but
			 * POLLOUT (POLLIN, POLLHUP, ...) shows up on so_rcv.
			 */
			int dirs = 0;
			if ((events & (POLLOUT | POLLWRNORM)) != 0)
				dirs |= KQ_DIR_SND;
			if ((events & ~(uint32_t)(POLLOUT | POLLWRNORM)) != 0 ||
			    dirs == 0)
				dirs |= KQ_DIR_RCV;
			kqring_arm_socket_op(ctx, sqe, dirs);
		}
		break;
	}

	case IORING_OP_READ:
		kqring_exec_file_rw(ctx, sqe, false);
		break;

	case IORING_OP_WRITE:
		kqring_exec_file_rw(ctx, sqe, true);
		break;

	case IORING_OP_FSYNC: {
		int err = kern_fsync(td, sqe->fd,
		    (sqe->fsync_flags & IORING_FSYNC_DATASYNC) == 0);
		kqring_post_cqe(ctx, sqe->user_data, -err, 0);
		break;
	}

	case IORING_OP_OPENAT: {
		int err = kern_openat(td, sqe->fd,
		    (const char *)(uintptr_t)sqe->addr, UIO_USERSPACE,
		    sqe->open_flags, sqe->len);
		int res = (err == 0) ? (int)td->td_retval[0] : -err;
		kqring_post_cqe(ctx, sqe->user_data, res, 0);
		break;
	}

	case IORING_OP_FUTEX_WAIT: {
		uint32_t uval = 0;
		int err = copyin((const void *)(uintptr_t)sqe->addr, &uval,
		    sizeof(uval));
		if (err != 0) {
			kqring_post_cqe(ctx, sqe->user_data, -err, 0);
			break;
		}
		if (uval != (uint32_t)sqe->off) {
			kqring_post_cqe(ctx, sqe->user_data, -EAGAIN, 0);
			break;
		}
		/* Re-checked by kqring_check_futexes() on every enter. */
		struct kqring_op *op = kqring_op_alloc(ctx);
		op->sqe = *sqe;
		mtx_lock(&ctx->mtx);
		ctx->nfutex++;
		TAILQ_INSERT_TAIL(&ctx->active_ops, op, active_link);
		mtx_unlock(&ctx->mtx);
		break;
	}

	case IORING_OP_ASYNC_CANCEL: {
		uint64_t target_ud = sqe->addr;
		struct kqring_op *op, *found = NULL;

		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (op->sqe.user_data == target_ud && !op->cancelled) {
				found = op;
				found->cancelled = true;
				TAILQ_REMOVE(&ctx->active_ops, found,
				    active_link);
				if (found->sqe.opcode == IORING_OP_FUTEX_WAIT)
					ctx->nfutex--;
				kqring_op_disarm_locked(found);
				break;
			}
		}
		mtx_unlock(&ctx->mtx);

		if (found != NULL) {
			kqring_post_cqe(ctx, target_ud, -ECANCELED, 0);
			kqring_post_cqe(ctx, sqe->user_data, 0, 0);
			kqring_op_free(found);
		} else {
			kqring_post_cqe(ctx, sqe->user_data, -ENOENT, 0);
		}
		break;
	}

	default:
		kqring_post_cqe(ctx, sqe->user_data, -EOPNOTSUPP, 0);
		break;
	}
}

/*
 * Run deferred taskwork in curthread context (DEFER_TASKRUN).
 */
static void
kqring_run_taskwork(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	/*
	 * Unlocked fast path: a racing upcall that is missed here is seen by
	 * the wait loop, which re-checks taskwork_ops under ctx->mtx before
	 * sleeping.
	 */
	if (TAILQ_EMPTY(&ctx->taskwork_ops) && (ctx->shm == NULL ||
	    (atomic_load_32(&ctx->shm->sq_flags) & IORING_SQ_TASKRUN) == 0))
		return;

	mtx_lock(&ctx->mtx);
	while ((op = TAILQ_FIRST(&ctx->taskwork_ops)) != NULL) {
		TAILQ_REMOVE(&ctx->taskwork_ops, op, taskwork_link);
		op->in_taskwork = false;
		if (op->cancelled || ctx->closing)
			continue;

		struct io_uring_sqe sqe = op->sqe;
		/* Only this thread disarms ops, so op->so stays put. */
		struct socket *so = op->so;
		bool multishot = (sqe.opcode == IORING_OP_RECV) &&
		    (sqe.ioprio & IORING_RECV_MULTISHOT) != 0;
		mtx_unlock(&ctx->mtx);

		bool keep_multishot = false;
		int rc = 0;

		switch (sqe.opcode) {
		case IORING_OP_RECV: {
			/*
			 * For multishot, drain bursts of packets into separate
			 * PBUF_RING buffers in one pass until EAGAIN or ENOBUFS.
			 */
			bool more = false;
			do {
				keep_multishot = false;
				rc = kqring_exec_recv(ctx, &sqe, so,
				    &keep_multishot, &more);
			} while (multishot && rc == 0 && keep_multishot &&
			    more);
			break;
		}
		case IORING_OP_SEND:
			rc = kqring_exec_send(ctx, &sqe, so);
			break;
		case IORING_OP_ACCEPT:
			rc = kqring_exec_accept(ctx, &sqe, so);
			break;
		case IORING_OP_POLL_ADD:
			rc = kqring_exec_poll(ctx, &sqe, so);
			break;
		default:
			rc = 0;
			break;
		}

		mtx_lock(&ctx->mtx);
		/*
		 * Keep the operation armed if it returned EAGAIN (spurious wake
		 * or drained multishot burst that still has keep_multishot).
		 */
		if (rc == EAGAIN || keep_multishot)
			continue;

		if (!op->cancelled) {
			op->cancelled = true;
			TAILQ_REMOVE(&ctx->active_ops, op, active_link);
			kqring_op_disarm_locked(op);
			mtx_unlock(&ctx->mtx);
			kqring_op_free(op);
			mtx_lock(&ctx->mtx);
		}
	}
	if (TAILQ_EMPTY(&ctx->taskwork_ops) && ctx->shm != NULL)
		atomic_clear_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
	mtx_unlock(&ctx->mtx);
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

	mtx_lock(&ctx->mtx);
	ctx->closing = true;
	ctx->nfutex = 0;
	while ((op = TAILQ_FIRST(&ctx->active_ops)) != NULL) {
		op->cancelled = true;
		TAILQ_REMOVE(&ctx->active_ops, op, active_link);
		kqring_op_disarm_locked(op);
		mtx_unlock(&ctx->mtx);
		kqring_op_free(op);
		mtx_lock(&ctx->mtx);
	}
	mtx_unlock(&ctx->mtx);

	while ((op = TAILQ_FIRST(&ctx->free_ops)) != NULL) {
		TAILQ_REMOVE(&ctx->free_ops, op, active_link);
		free(op, M_KQRING);
	}
	while ((ocqe = STAILQ_FIRST(&ctx->ocq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->ocq, link);
		free(ocqe, M_KQRING);
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
	STAILQ_INIT(&ctx->ocq);

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
	    IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_EXT_ARG |
	    IORING_FEAT_FAST_POLL | IORING_FEAT_POLL_32BITS;

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
		    so->so_error != 0) {
			op->in_taskwork = true;
			TAILQ_INSERT_TAIL(&ctx->taskwork_ops, op,
			    taskwork_link);
		}
	}
}

/*
 * Complete armed FUTEX_WAIT ops whose word no longer holds the expected
 * value (or can no longer be read).  Only the owner thread (this one)
 * modifies active_ops, so it is walked without ctx->mtx, which must not be
 * held across copyin().
 */
static void
kqring_check_futexes(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *tmp;
	uint32_t uval;
	int error;

	if (ctx->nfutex == 0)
		return;
	TAILQ_FOREACH_SAFE(op, &ctx->active_ops, active_link, tmp) {
		if (op->sqe.opcode != IORING_OP_FUTEX_WAIT || op->cancelled)
			continue;
		error = copyin((const void *)(uintptr_t)op->sqe.addr, &uval,
		    sizeof(uval));
		if (error == 0 && uval == (uint32_t)op->sqe.off)
			continue;
		mtx_lock(&ctx->mtx);
		TAILQ_REMOVE(&ctx->active_ops, op, active_link);
		ctx->nfutex--;
		mtx_unlock(&ctx->mtx);
		kqring_post_cqe(ctx, op->sqe.user_data, -error, 0);
		kqring_op_free(op);
	}
}

static int kqring_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt);

static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t head, tail, consumed = 0, submitted = 0;
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
			struct __kernel_timespec kts;
			error = copyin((const void *)(uintptr_t)ext.ts, &kts,
			    sizeof(kts));
			if (error != 0)
				return (error);
			if (kts.tv_sec < 0 || kts.tv_nsec < 0 ||
			    kts.tv_nsec >= 1000000000LL)
				return (EINVAL);
			struct timespec ts = {
				.tv_sec = kts.tv_sec,
				.tv_nsec = (long)kts.tv_nsec,
			};
			sbt = tstosbt(ts);
			if (sbt == 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0))
				sbt = 1;
			has_timeout = true;
		}
	}

	/* 2. Drain and submit queued SQEs */
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
		struct io_uring_sqe sqe = ctx->sqes[sqe_idx];
		kqring_submit_sqe(ctx, &sqe);
		submitted++;
	}
	atomic_store_rel_32(&shm->sq_head, head);
	ea->SUBMITTED_out = submitted;

	/* 3. Run pending DEFER_TASKRUN socket completions and futex checks */
	kqring_run_taskwork(ctx);
	kqring_check_futexes(ctx);

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
		kqring_check_futexes(ctx);
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
		if (!TAILQ_EMPTY(&ctx->taskwork_ops)) {
			mtx_unlock(&ctx->mtx);
			continue;
		}
		/*
		 * If a futex or polled op is armed, wake periodically (50 us)
		 * to re-check it in addition to socket upcalls.
		 */
		sbintime_t sleep_sbt = sbt;
		if ((ctx->nfutex > 0 || ctx->npolled > 0) &&
		    (!has_timeout || sleep_sbt > 50 * SBT_1US))
			sleep_sbt = 50 * SBT_1US;

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
			IORING_OP_FSYNC,
			IORING_OP_POLL_ADD,
			IORING_OP_ACCEPT,
			IORING_OP_ASYNC_CANCEL,
			IORING_OP_OPENAT,
			IORING_OP_READ,
			IORING_OP_WRITE,
			IORING_OP_SEND,
			IORING_OP_RECV,
			IORING_OP_FUTEX_WAIT,
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
		return (kqring_ioc_setup(ctx, (struct kqring_setup_arg *)data,
		    td));
	case KQRING_IOC_ENTER:
		return (kqring_ioc_enter(ctx, (struct kqring_enter_arg *)data,
		    td));
	case KQRING_IOC_REGISTER:
		return (kqring_ioc_register(ctx,
		    (struct kqring_register_arg *)data, td));
	default:
		return (ENOTTY);
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
		kqring_cdev = make_dev(&kqring_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0666, "kqring");
		if (kqring_cdev == NULL)
			return (ENOMEM);
		printf("kqring: loaded (/dev/kqring, SINGLE_ISSUER | DEFER_TASKRUN)\n");
		return (0);

	case MOD_UNLOAD:
		if (kqring_cdev != NULL)
			destroy_dev(kqring_cdev);
		printf("kqring: unloaded\n");
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(kqring, kqring_modevent, NULL);
MODULE_VERSION(kqring, 1);
