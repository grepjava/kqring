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
	int			so_dir;		/* SO_RCV or SO_SND */
	bool			armed_upcall;
	bool			listening;	/* armed via solisten_upcall_set */
	bool			in_taskwork;
	bool			cancelled;
	bool			polled;		/* no upcall; see npolled */
	struct io_uring_sqe	sqe;
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
	struct kqring_op	*futex_op;
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

static void
kqring_post_cqe(struct kqring_ctx *ctx, uint64_t user_data, int32_t res,
    uint32_t flags)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t tail, head;
	struct io_uring_cqe *cqe;

	head = atomic_load_acq_32(&shm->cq_head);
	tail = shm->cq_tail;

	if (tail - head >= ctx->cq_entries) {
		atomic_add_32(&shm->cq_overflow, 1);
		return;
	}

	cqe = &ctx->cqes[tail & ctx->cq_mask];
	cqe->user_data = user_data;
	cqe->res = res;
	cqe->flags = flags;

	atomic_store_rel_32(&shm->cq_tail, tail + 1);
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
			if (op->armed_upcall && SOLISTENING(so))
				solisten_upcall_set(so, NULL, NULL);
			op->armed_upcall = false;
			SOCK_UNLOCK(so);
		} else {
			SOCK_BUF_LOCK(so, op->so_dir);
			if (op->armed_upcall) {
				soupcall_clear(so, op->so_dir);
				op->armed_upcall = false;
			}
			SOCK_BUF_UNLOCK(so, op->so_dir);
		}
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
 */
static int
kqring_exec_recv(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    bool *keep_multishot_out, bool *more_out)
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

	/*
	 * If buffer selection is requested, check whether bytes or EOF/error
	 * are actually waiting before popping a buffer, and only commit the
	 * buffer when > 0 bytes are read.
	 */
	if (buf_select) {
		if (SOLISTENING(so)) {
			fdrop(fp, td);
			kqring_post_cqe(ctx, sqe->user_data, -ENOTCONN, 0);
			return (0);
		}
		SOCK_RECVBUF_LOCK(so);
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
		*more_out = sbavail(&so->so_rcv) > 0 ||
		    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0;
	}

	fdrop(fp, td);
	kqring_post_cqe(ctx, sqe->user_data, (int32_t)transferred, cqe_flags);
	return (0);
}

static int
kqring_exec_send(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
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
kqring_exec_accept(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
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
	int revents = 0;

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

	SOCK_RECVBUF_LOCK(so);
	bool rd_eof = (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0;
	if (sbavail(&so->so_rcv) >= MAX(so->so_rcv.sb_lowat, 1) || rd_eof ||
	    so->so_error != 0)
		revents |= events & (POLLIN | POLLRDNORM);
	SOCK_RECVBUF_UNLOCK(so);

	SOCK_SENDBUF_LOCK(so);
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
kqring_exec_poll(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
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

static int
kqring_arm_socket_op(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    int so_dir)
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
		return (0);
	}
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		kqring_post_cqe(ctx, sqe->user_data, -ENOTSOCK, 0);
		return (0);
	}
	so = fp->f_data;
	soref(so);
	fdrop(fp, td);

	op = kqring_op_alloc(ctx);
	op->so = so;
	op->so_dir = so_dir;
	op->sqe = *sqe;

	/*
	 * Install the upcall, then check whether the socket became ready
	 * before it was installed.  Listening sockets have no so_rcv/so_snd
	 * (that storage is reused for the listen queues), so they use the
	 * dedicated listen upcall under the listen lock.
	 */
	bool ready_now = false;
	SOCK_LOCK(so);
	if (SOLISTENING(so)) {
		op->listening = true;
		solisten_upcall_set(so, kqring_soupcall, op);
		op->armed_upcall = true;
		ready_now = !TAILQ_EMPTY(&so->sol_comp) || so->so_error != 0;
		SOCK_UNLOCK(so);
	} else {
		SOCK_UNLOCK(so);
		SOCK_BUF_LOCK(so, so_dir);
		soupcall_set(so, so_dir, kqring_soupcall, op);
		op->polled = so_dir == SO_SND &&
		    so->so_proto->pr_domain->dom_family == AF_LOCAL &&
		    so->so_type != SOCK_DGRAM;
		op->armed_upcall = true;
		if (so_dir == SO_RCV) {
			ready_now = sbavail(&so->so_rcv) > 0 ||
			    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0 ||
			    so->so_error != 0;
		} else {
			ready_now = sbspace(&so->so_snd) > 0 ||
			    (so->so_snd.sb_state & SBS_CANTSENDMORE) != 0 ||
			    so->so_error != 0;
		}
		SOCK_BUF_UNLOCK(so, so_dir);
	}

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
	return (0);
}

static void
kqring_submit_sqe(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	struct thread *td = curthread;
	bool keep_multishot = false;
	int rc;

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
			rc = kqring_exec_recv(ctx, sqe, &keep_multishot, &more);
		} while (multishot && rc == 0 && keep_multishot && more);
		if (rc == EAGAIN || (multishot && rc == 0 && keep_multishot))
			kqring_arm_socket_op(ctx, sqe, SO_RCV);
		break;
	}

	case IORING_OP_SEND:
		rc = kqring_exec_send(ctx, sqe);
		if (rc == EAGAIN)
			kqring_arm_socket_op(ctx, sqe, SO_SND);
		break;

	case IORING_OP_ACCEPT:
		rc = kqring_exec_accept(ctx, sqe);
		if (rc == EAGAIN)
			kqring_arm_socket_op(ctx, sqe, SO_RCV);
		break;

	case IORING_OP_POLL_ADD: {
		short events = (short)(sqe->poll32_events ? sqe->poll32_events :
		    sqe->poll_events);
		rc = kqring_exec_poll(ctx, sqe);
		if (rc == EAGAIN) {
			int dir = (events & (POLLOUT | POLLWRNORM)) ?
			    SO_SND : SO_RCV;
			kqring_arm_socket_op(ctx, sqe, dir);
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
		struct kqring_op *op = kqring_op_alloc(ctx);
		op->sqe = *sqe;
		mtx_lock(&ctx->mtx);
		if (ctx->futex_op != NULL) {
			struct kqring_op *old = ctx->futex_op;
			TAILQ_REMOVE(&ctx->active_ops, old, active_link);
			kqring_op_free(old);
		}
		ctx->futex_op = op;
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
				if (ctx->futex_op == found)
					ctx->futex_op = NULL;
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
				rc = kqring_exec_recv(ctx, &sqe,
				    &keep_multishot, &more);
			} while (multishot && rc == 0 && keep_multishot &&
			    more);
			break;
		}
		case IORING_OP_SEND:
			rc = kqring_exec_send(ctx, &sqe);
			break;
		case IORING_OP_ACCEPT:
			rc = kqring_exec_accept(ctx, &sqe);
			break;
		case IORING_OP_POLL_ADD:
			rc = kqring_exec_poll(ctx, &sqe);
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

	mtx_lock(&ctx->mtx);
	ctx->closing = true;
	ctx->futex_op = NULL;
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
kqring_ioc_setup(struct kqring_ctx *ctx, struct kqring_setup_arg *uarg,
    struct thread *td)
{
	struct io_uring_params p;
	uint32_t sq_entries, cq_entries;
	size_t ring_bytes, sqes_bytes, array_off, cqes_off;
	int error;

	if (ctx->setup_done)
		return (EINVAL);
	if (uarg->entries == 0 || uarg->entries > KQRING_MAX_SQ_ENTRIES)
		return (EINVAL);

	error = copyin(uarg->params, &p, sizeof(p));
	if (error != 0)
		return (error);

	/*
	 * Reject IORING_SETUP_SQPOLL: kqring never spawns background kernel
	 * threads. Require SINGLE_ISSUER | DEFER_TASKRUN.
	 */
	if (p.flags & IORING_SETUP_SQPOLL)
		return (EINVAL);

	sq_entries = roundup_pow2(uarg->entries);
	if (p.flags & IORING_SETUP_CQSIZE) {
		if (p.cq_entries < sq_entries ||
		    p.cq_entries > KQRING_MAX_CQ_ENTRIES)
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
	if (error != 0)
		return (error);

	ctx->setup_done = true;
	return (0);
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

		if (!op->polled || op->in_taskwork || op->cancelled ||
		    so == NULL)
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

static int
kqring_ioc_enter(struct kqring_ctx *ctx, struct kqring_enter_arg *ea,
    struct thread *td)
{
	struct kqring_shm_header *shm = ctx->shm;
	uint32_t head, tail, submitted = 0;
	sbintime_t sbt = 0;
	bool has_timeout = false;
	bool may_poll = true;
	int error = 0;

	if (!ctx->setup_done || !ctx->enabled)
		return (EBADF);
	if (ctx->owner_td != td)
		return (EEXIST); /* SINGLE_ISSUER violation */

	/* 1. Drain and submit queued SQEs */
	head = shm->sq_head;
	tail = atomic_load_acq_32(&shm->sq_tail);
	while (head != tail && submitted < ea->to_submit) {
		uint32_t sqe_idx = ctx->sq_array[head & ctx->sq_mask];
		head++;
		submitted++;
		if (sqe_idx < ctx->sq_entries) {
			struct io_uring_sqe sqe = ctx->sqes[sqe_idx];
			kqring_submit_sqe(ctx, &sqe);
		}
	}
	atomic_store_rel_32(&shm->sq_head, head);
	ea->SUBMITTED_out = submitted;

	/* 2. Run any pending DEFER_TASKRUN socket completions */
	kqring_run_taskwork(ctx);

	if ((ea->flags & IORING_ENTER_GETEVENTS) == 0 || ea->min_complete == 0)
		return (0);

	/* 3. Parse optional timeout from IORING_ENTER_EXT_ARG */
	if ((ea->flags & IORING_ENTER_EXT_ARG) != 0 && ea->arg != NULL) {
		struct io_uring_getevents_arg ext;
		if (ea->argsz < sizeof(ext))
			return (EINVAL);
		error = copyin(ea->arg, &ext, sizeof(ext));
		if (error != 0)
			return (error);
		if (ext.ts != 0) {
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

	/* 4. Wait loop until CQ has min_complete entries, futex changes, or timeout */
	for (;;) {
		kqring_run_taskwork(ctx);

		/* Check if armed FUTEX_WAIT word changed in user space */
		if (ctx->futex_op != NULL) {
			uint32_t uval = 0;
			struct kqring_op *fop = ctx->futex_op;
			if (copyin((const void *)(uintptr_t)fop->sqe.addr,
			    &uval, sizeof(uval)) == 0 &&
			    uval != (uint32_t)fop->sqe.off) {
				mtx_lock(&ctx->mtx);
				if (ctx->futex_op == fop) {
					ctx->futex_op = NULL;
					TAILQ_REMOVE(&ctx->active_ops, fop,
					    active_link);
					mtx_unlock(&ctx->mtx);
					kqring_post_cqe(ctx,
					    fop->sqe.user_data, 0, 0);
					kqring_op_free(fop);
				} else {
					mtx_unlock(&ctx->mtx);
				}
			}
		}

		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= ea->min_complete)
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
		if ((ctx->futex_op != NULL || ctx->npolled > 0) &&
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
		uint32_t max_ops = ra->nr_args > 256 ? 256 : ra->nr_args;
		hdr.last_op = IORING_OP_FUTEX_WAIT;
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
