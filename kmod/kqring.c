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
 *         buffer from PBUF_RING, calling soreceive()/solisten_dequeue() with
 *         UIO_USERSPACE, and posting CQEs to the shared CQ ring.
 *       * On EOF (bytes == 0), any selected PBUF_RING buffer is NOT consumed,
 *         so cqe->res == 0 with no leaked buffer ID.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/callout.h>
#include <sys/cpuset.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/sleepqueue.h>
#include <sys/smp.h>
#include <sys/unistd.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/filio.h>
#include <sys/capsicum.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/fcntl.h>
#include <sys/extattr.h>
#include <sys/limits.h>
#include <sys/mbuf.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/selinfo.h>
#include <sys/pipe.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/resourcevar.h>
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
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>

#include <sys/kqring.h>

MALLOC_DEFINE(M_KQRING, "kqring", "FreeBSD kqring I/O ring state");
/* posix_fadvise(2) state hung off struct file (freed by the kernel). */
MALLOC_DECLARE(M_FADVISE);

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
/* Registered resources, with io_uring's limits. */
#define KQRING_MAX_FILES	(1U << 20)	/* also RLIMIT_NOFILE */
#define KQRING_MAX_BUFS		(1U << 14)
#define KQRING_MAX_BUF_LEN	(1ULL << 30)
#define KQRING_MAX_PERSONALITIES 1024U
/* Buffers handed over with PROVIDE_BUFFERS and not yet used, per group. */
#define KQRING_MAX_GROUP_BUFS	65536U
/* ... and per ring, which io_uring leaves to memory accounting. */
#define KQRING_MAX_PROVIDED	(1U << 18)
/* Wired memory of IOU_PBUF_RING_MMAP rings, per ring. */
#define KQRING_MAX_RING_MEM	(16U * 1024 * 1024)
/* Provided buffers one bundle (IORING_RECVSEND_BUNDLE) transfer may use. */
#define KQRING_MAX_BUNDLE	32
#define KQRING_BGROUP_HASH	64
/* In-process worker pools: IO_WQ_BOUND (0) and IO_WQ_UNBOUND (1). */
#define KQRING_WQ_TYPES		2
#define KQRING_WQ_MAX_WORKERS	256U
#define KQRING_WQ_IDLE_SBT	(5 * SBT_1S)

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
	TAILQ_ENTRY(kqring_op)	wq_link;	/* wq_ops[wq_type] */
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
	/* In-process worker thread state (under ctx->mtx). */
	struct thread		*wq_td;		/* worker executing this op */
	struct file		*wq_fp;		/* direct OPENAT(2) file */
	struct filecaps		wq_caps;	/* and its Capsicum rights */
	uint8_t			wq_type;	/* IO_WQ_BOUND / IO_WQ_UNBOUND */
	bool			wq_queued;	/* on wq_ops[wq_type] */
	bool			wq_running;	/* worker executing this op */
	bool			wq_blocking;	/* worker in unbounded sleep */
	bool			wq_canceled;	/* cancelled while running */
	bool			wq_done;	/* worker finished */
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
	/*
	 * Held from issue until the request is freed, so that replacing or
	 * unregistering a slot never pulls a file or buffer out from under
	 * a request using it: the IOSQE_FIXED_FILE slot, the registered
	 * buffer (READ_FIXED, ...), and sqe->personality's credentials.
	 */
	struct kqring_rsrc	*ffile;
	struct kqring_rsrc	*fbuf;
	struct ucred		*cred;
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

/*
 * A MSG_RING CQE queued for the target ring's owner to post.  For
 * IORING_MSG_SEND_FD it carries a file (referenced) that the owner first
 * installs in its fixed table, at file_index.
 */
struct kqring_rcqe {
	STAILQ_ENTRY(kqring_rcqe) link;
	struct io_uring_cqe	cqe;
	struct file		*fp;
	struct filecaps		caps;
	uint32_t		file_index;
	bool			skip_cqe;
};

/*
 * A registered file or buffer: one slot of the fixed file table
 * (IORING_REGISTER_FILES, direct descriptors) or of the registered buffer
 * table.  The table holds one reference and each request using the slot
 * another, so replacing or unregistering a slot does not affect requests
 * already using it; once the last reference is gone the slot's tag CQE is
 * posted (IORING_FEAT_RSRC_TAGS), as in io_uring.  Owner thread only.
 *
 * Registered buffers are not pinned: kqring keeps the range, checks that
 * READ_FIXED and friends stay inside it, and copies to and from user
 * memory as for any other request.
 */
struct kqring_rsrc {
	uint32_t		refs;
	uint64_t		tag;
	struct file		*fp;		/* file slot */
	struct filecaps		caps;		/* its Capsicum rights */
	uint64_t		addr;		/* buffer slot */
	uint64_t		len;
};

/* A buffer handed over with PROVIDE_BUFFERS. */
struct kqring_kbuf {
	STAILQ_ENTRY(kqring_kbuf) link;
	uint64_t		addr;
	uint32_t		len;
	uint16_t		bid;
};

/*
 * A provided buffer group: a buffer ring in user memory
 * (IORING_REGISTER_PBUF_RING) or in kernel memory userspace maps
 * (IOU_PBUF_RING_MMAP), or a list of buffers given with PROVIDE_BUFFERS.
 * Only the owner thread uses and changes groups; the hash table is also
 * changed under ctx->mtx, because mmap(2) of a ring looks it up from any
 * thread.
 */
struct kqring_bgroup {
	LIST_ENTRY(kqring_bgroup) link;
	uint16_t		bgid;
	bool			is_ring;
	uint16_t		ring_flags;	/* IOU_PBUF_RING_* */
	uint64_t		ring_uaddr;	/* user memory ring */
	struct io_uring_buf_ring *ring_kva;	/* IOU_PBUF_RING_MMAP ring */
	vm_object_t		ring_obj;
	vm_offset_t		ring_kva_base;
	size_t			ring_alloc_len;
	uint32_t		ring_entries;
	uint32_t		ring_mask;
	uint16_t		head;		/* kernel consumer index */
	STAILQ_HEAD(, kqring_kbuf) bufs;	/* PROVIDE_BUFFERS */
	uint32_t		nbufs;
};

LIST_HEAD(kqring_bgroup_list, kqring_bgroup);

/* Credentials registered with IORING_REGISTER_PERSONALITY. */
struct kqring_cred {
	LIST_ENTRY(kqring_cred)	link;
	uint16_t		id;
	struct ucred		*cred;
};

/*
 * A buffer (or, for a bundle, buffers) picked from a provided buffer group
 * for one transfer, consumed by kqring_buf_commit() once bytes moved.  It
 * keeps its own copy of what was picked: the transfer's uiomove() advances
 * the iovecs it was given.
 */
struct kqring_sel {
	struct kqring_bgroup	*bg;
	u_int			nbufs;
	uint16_t		bid;		/* first buffer */
	uint64_t		addr0;		/* its address */
	uint32_t		len0;		/* and full length */
	uint32_t		lens[KQRING_MAX_BUNDLE]; /* lengths used */
};

#define	KQ_DIR_RCV	0x1
#define	KQ_DIR_SND	0x2

/* A CQE that did not fit in the CQ ring, waiting for userspace to reap. */
struct kqring_ocqe {
	STAILQ_ENTRY(kqring_ocqe) link;
	struct io_uring_cqe	cqe;
};

/* IORING_REGISTER_RESTRICTIONS, in force once the ring is enabled. */
struct kqring_restrictions {
	uint64_t	register_op[4];		/* bitmaps over 0..255 */
	uint64_t	sqe_op[4];
	uint8_t		sqe_flags_allowed;
	uint8_t		sqe_flags_required;
	bool		registered;
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

	/* Provided buffer groups by bgid; see struct kqring_bgroup. */
	struct sx		bg_sx;
	struct kqring_bgroup_list bgroups[KQRING_BGROUP_HASH];
	uint32_t		nprovided;	/* PROVIDE_BUFFERS buffers held */
	size_t			ring_mem;	/* IOU_PBUF_RING_MMAP rings */

	/*
	 * In-process worker thread pool (kthread_add into curproc; under mtx).
	 * Workers run blocking and IOSQE_ASYNC requests with the owner's
	 * vmspace and descriptor table, then queue completions on
	 * taskwork_ops so only the owner touches CQEs and resource tables.
	 */
	struct {
		struct kqring_ctx	*ctx;
		int			type;
	} wq_arg[KQRING_WQ_TYPES];
	struct kqring_op_list	wq_ops[KQRING_WQ_TYPES];
	uint32_t		wq_max[KQRING_WQ_TYPES];
	uint32_t		wq_total[KQRING_WQ_TYPES];
	uint32_t		wq_busy[KQRING_WQ_TYPES];
	uint32_t		wq_pending[KQRING_WQ_TYPES];
	uint32_t		wq_inflight;	/* ops queued or running */
	struct cpuset		*wq_base_cpuset;
	cpuset_t		wq_cpumask;
	bool			wq_aff_set;
	uint32_t		wq_aff_gen;

	/*
	 * Registered files and buffers (owner thread only).  A NULL slot is
	 * empty.  IORING_FILE_INDEX_ALLOC picks slots in
	 * [file_alloc_off, file_alloc_end), starting at file_alloc_hint.
	 */
	struct kqring_rsrc	**files;
	uint32_t		nfiles;
	uint32_t		file_alloc_off;
	uint32_t		file_alloc_end;
	uint32_t		file_alloc_hint;
	struct kqring_rsrc	**bufs;
	uint32_t		nbufs;

	/* IORING_REGISTER_PERSONALITY (owner thread only) */
	LIST_HEAD(, kqring_cred) creds;
	uint32_t		ncreds;
	uint16_t		cred_next;	/* next id to try */

	/*
	 * IORING_REGISTER_EVENTFD.  Changed under mtx, and signalled under
	 * mtx from upcalls; the owner reads it without.  evfd_cqes counts
	 * CQEs posted since the last signal (only asynchronous ones with
	 * IORING_REGISTER_EVENTFD_ASYNC); submitting is set while SQEs are
	 * being issued inline.
	 */
	struct eventfd		*evfd;
	bool			evfd_async;
	bool			submitting;
	uint32_t		evfd_cqes;

	struct kqring_restrictions rst;
	bool			restricted;	/* rst in force */
	uint32_t		clockid;	/* IORING_REGISTER_CLOCK */
	bool			reg_busy;	/* register on a disabled ring */

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
static bool kqring_fp_is_ring(const struct file *fp);
static int  kqring_fixed_remove(struct kqring_ctx *ctx, uint32_t slot);
static void kqring_files_free(struct kqring_ctx *ctx);
static void kqring_bufs_free(struct kqring_ctx *ctx);
static void kqring_bgroup_free(struct kqring_ctx *ctx,
    struct kqring_bgroup *bg);
static void kqring_evfd_flush(struct kqring_ctx *ctx);
static bool kqring_wq_enqueue(struct kqring_ctx *ctx, struct kqring_op *op,
    int type);

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

	/* The eventfd is signalled once per enter; see kqring_evfd_flush(). */
	if (!ctx->evfd_async || !ctx->submitting)
		ctx->evfd_cqes++;
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

static inline bool
kqring_op_readv(uint8_t opcode)
{
	return (opcode == IORING_OP_READV || opcode == IORING_OP_READV_FIXED);
}

static inline bool
kqring_op_writev(uint8_t opcode)
{
	return (opcode == IORING_OP_WRITEV || opcode == IORING_OP_WRITEV_FIXED);
}

/* ============================================================================
 * Provided Buffer Groups (PBUF_RING, PROVIDE_BUFFERS)
 * ============================================================================
 *
 * Buffers are picked by kqring_buf_select() right before a transfer and
 * consumed by kqring_buf_commit() only once bytes moved, in the owner
 * thread.  A transfer that hits EOF (0 bytes) or fails leaves the group as
 * it was, so no buffer ID leaks.  Requests look their group up again on
 * every attempt, so unregistering a group under an armed request just makes
 * it fail with ENOBUFS.
 */

static inline struct kqring_bgroup_list *
kqring_bgroup_bucket(struct kqring_ctx *ctx, uint16_t bgid)
{
	return (&ctx->bgroups[bgid % KQRING_BGROUP_HASH]);
}

static struct kqring_bgroup *
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
static int
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
	if (bg == NULL)
		return (ENOBUFS);
	sel->bg = bg;
	nmax = MIN(nmax, KQRING_MAX_BUNDLE);
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
static uint32_t
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
/*
 * Signal the registered eventfd, unless userspace set
 * IORING_CQ_EVENTFD_DISABLED.  ctx->mtx keeps the eventfd registered.
 */
static void
kqring_evfd_signal_locked(struct kqring_ctx *ctx)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (ctx->evfd != NULL && ctx->shm != NULL &&
	    (atomic_load_32(&ctx->shm->cq_flags) &
	    IORING_CQ_EVENTFD_DISABLED) == 0)
		eventfd_signal(ctx->evfd);
}

static void
kqring_taskwork_add_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->cancelled || ctx->closing || op->in_taskwork)
		return;
	op->in_taskwork = true;
	/*
	 * As io_uring does for DEFER_TASKRUN, tell an eventfd watcher that
	 * the ring has work to run (once, until the owner runs it), unless
	 * EVENTFD_ASYNC is set and this is the submitting enter itself.
	 */
	if (TAILQ_EMPTY(&ctx->taskwork_ops) &&
	    (!ctx->evfd_async || !ctx->submitting))
		kqring_evfd_signal_locked(ctx);
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
kqring_wq_abort_td(struct thread *wtd)
{
	if (wtd == NULL)
		return;
	thread_lock(wtd);
	if (TD_CAN_ABORT(wtd))
		sleepq_abort(wtd, EINTR);	/* unlocks wtd */
	else
		thread_unlock(wtd);
}

static void
kqring_op_disarm_locked(struct kqring_op *op)
{
	struct kqring_ctx *ctx = op->ctx;
	struct socket *so = op->so;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->polled) {
		op->polled = false;
		ctx->npolled--;
	}
	if (op->wq_queued) {
		TAILQ_REMOVE(&ctx->wq_ops[op->wq_type], op, wq_link);
		op->wq_queued = false;
		ctx->wq_pending[op->wq_type]--;
		ctx->wq_inflight--;
	}
	while (op->wq_running) {
		op->wq_canceled = true;
		kqring_wq_abort_td(op->wq_td);
		msleep_sbt(&op->wq_running, &ctx->mtx, 0, "kqwqcn",
		    SBT_1MS, 0, 0);
	}
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
		mtx_lock(&ctx->mtx);
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
	if (op->sqe.opcode == IORING_OP_WAITID && ctx->nwaitid > 0)
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

/*
 * Drop a reference to a registered file or buffer; the last one posts its
 * tag CQE (not while the ring is being torn down) and releases the file.
 * Owner thread (or kqring_dtor()), ctx->mtx not held: fdrop() may close.
 */
static void
kqring_rsrc_put(struct kqring_ctx *ctx, struct kqring_rsrc *r)
{
	KASSERT(r->refs > 0, ("kqring: rsrc refcount underflow"));
	if (--r->refs != 0)
		return;
	if (r->tag != 0 && !ctx->closing)
		kqring_post_cqe(ctx, r->tag, 0, 0);
	if (r->fp != NULL) {
		filecaps_free(&r->caps);
		fdrop(r->fp, curthread);
	}
	free(r, M_KQRING);
}

/* Give every Capsicum right, as a descriptor kqring itself created has. */
static void
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

/* Take a reference to fixed slot `slot` for a request. */
static int
kqring_fixed_get(struct kqring_ctx *ctx, uint32_t slot,
    struct kqring_rsrc **rp)
{
	struct kqring_rsrc *r;

	if (ctx->files == NULL || slot >= ctx->nfiles ||
	    (r = ctx->files[slot]) == NULL)
		return (EBADF);
	r->refs++;
	*rp = r;
	return (0);
}

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
static int32_t
kqring_fixed_install(struct kqring_ctx *ctx, struct file *fp,
    struct filecaps *caps, uint32_t file_index)
{
	struct kqring_rsrc *old;
	uint32_t slot;
	int32_t res = 0;
	int error;

	if (ctx->files == NULL) {
		res = -ENXIO;
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
static int32_t
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
static int32_t
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
 * A buffer for IORING_REGISTER_BUFFERS: up to 1 GiB of mapped, writable
 * user memory (as io_uring's pinning requires), or the empty slot
 * {NULL, 0}.  It is not pinned: see struct kqring_rsrc.
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
	struct kqring_rsrc *old, *r = NULL;
	int error;

	error = kqring_buf_validate(curthread, iov);
	if (error != 0)
		return (error);
	if (iov->iov_base == NULL) {
		if (tag != 0)
			return (EINVAL);
	} else {
		r = malloc(sizeof(*r), M_KQRING, M_WAITOK | M_ZERO);
		r->refs = 1;
		r->tag = tag;
		r->addr = (uintptr_t)iov->iov_base;
		r->len = iov->iov_len;
	}
	old = ctx->bufs[slot];
	ctx->bufs[slot] = r;
	if (old != NULL)
		kqring_rsrc_put(ctx, old);
	return (0);
}

/* Whether [addr, addr + len) lies inside registered buffer r. */
static inline bool
kqring_buf_within(const struct kqring_rsrc *r, uint64_t addr, uint64_t len)
{
	return (addr >= r->addr && len <= r->len &&
	    addr - r->addr <= r->len - len);
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
	switch (sqe->opcode) {
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		break;
	case IORING_OP_SEND_ZC:
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

/*
 * Run a request's own work with the credentials sqe->personality names
 * (IORING_REGISTER_PERSONALITY).  Only this thread's td_ucred changes,
 * and only until kqring_cred_exit(); the swap holds its own reference,
 * as the request may be freed in between.
 */
static struct ucred *
kqring_cred_enter(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct ucred *saved;

	if (op->cred == NULL)
		return (NULL);
	saved = td->td_ucred;
	td->td_ucred = crhold(op->cred);
	return (saved);
}

static void
kqring_cred_exit(struct ucred *saved)
{
	struct thread *td = curthread;
	struct ucred *cr;

	if (saved == NULL)
		return;
	cr = td->td_ucred;
	td->td_ucred = saved;
	crfree(cr);
}

/* Empty fixed slot `slot` (CLOSE with file_index, FILES_UPDATE). */
static int
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

	KASSERT(!op->armed && !op->in_taskwork && !op->timer_pending &&
	    !op->wq_queued && !op->wq_running,
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
	if (op->fbuf != NULL) {
		kqring_rsrc_put(ctx, op->fbuf);
		op->fbuf = NULL;
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
 * The file an op works on: its fixed file (IOSQE_FIXED_FILE), held since
 * issue and checked against the Capsicum rights it was registered with,
 * or sqe->fd looked up now.  `fflag` (FREAD, FWRITE or 0) is the open mode
 * needed, as fget_read() / fget_write() check it.
 */
static int
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
static int
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

	error = kqring_op_fget(op, rights, 0, &fp);
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
 * Pick provided buffers (from sqe->buf_group) for a receive of at most
 * `len` bytes (0: the buffers' size): one, or up to `nmax` for a bundle.
 * Returns EAGAIN, without consuming a buffer, if nothing is there to
 * receive.
 */
static int
kqring_recv_pick_buf(struct kqring_ctx *ctx, const struct kqring_op *op,
    struct socket *so, uint32_t len, struct iovec *iov, u_int nmax,
    struct kqring_sel *sel)
{
	bool readable;
	int error;

	error = kqring_so_readable(so, &readable);
	if (error != 0)
		return (error);
	if (!readable)
		return (EAGAIN);
	return (kqring_buf_select(ctx, op->sqe.buf_group, len, iov, nmax,
	    sel));
}

/*
 * READV with IOSQE_BUFFER_SELECT: its single iovec only caps the
 * provided buffer's length, as in io_uring.
 */
static inline uint32_t
kqring_buf_cap(const struct kqring_op *op)
{
	const struct iovec *iov = op->args;

	if (kqring_op_readv(op->sqe.opcode))
		return ((uint32_t)MIN(iov[0].iov_len, UINT32_MAX));
	return (op->sqe.len);
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
	struct iovec biov[KQRING_MAX_BUNDLE];
	struct kqring_sel sel;
	cap_rights_t rights;
	u_int iovcnt = 1;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool multishot = sqe->opcode == IORING_OP_READ_MULTISHOT ||
	    (sqe->opcode == IORING_OP_RECV &&
	    (sqe->ioprio & IORING_RECV_MULTISHOT) != 0);
	bool bundle = sqe->opcode == IORING_OP_RECV &&
	    (sqe->ioprio & IORING_RECVSEND_BUNDLE) != 0;
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
#ifdef KQRING_MAC
	error = mac_socket_check_receive(td->td_ucred, so);
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
#endif

	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_recv_pick_buf(ctx, op, so, kqring_buf_cap(op),
		    biov, bundle ? KQRING_MAX_BUNDLE : 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		iov = biov;
		iovcnt = sel.nbufs;
	} else if (kqring_op_readv(sqe->opcode)) {
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
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		return (rc);
	}

	/*
	 * Commit the provided buffer ONLY when n > 0!  On EOF (n == 0) the
	 * buffer stays in its group and res = 0 has no IORING_CQE_F_BUFFER.
	 */
	if (buf_select) {
		if (n > 0)
			cqe_flags |= kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
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

/*
 * SEND, SEND_ZC, and WRITE / WRITEV on a socket.  SEND may send from
 * provided buffers (IOSQE_BUFFER_SELECT, several with
 * IORING_RECVSEND_BUNDLE), consuming those the bytes sent came from.
 */
static int
kqring_exec_send(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct sockaddr *to = NULL;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	struct iovec biov[KQRING_MAX_BUNDLE];
	struct kqring_sel sel;
	cap_rights_t rights;
	u_int iovcnt = 1;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
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
#ifdef KQRING_MAC
	if (to != NULL)
		error = mac_socket_check_connect(td->td_ucred, so, to);
	if (error == 0)
		error = mac_socket_check_send(td->td_ucred, so);
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
#endif

	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_buf_select(ctx, sqe->buf_group, sqe->len, biov,
		    (sqe->ioprio & IORING_RECVSEND_BUNDLE) != 0 ?
		    KQRING_MAX_BUNDLE : 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
		iov = biov;
		iovcnt = sel.nbufs;
	} else if (kqring_op_writev(sqe->opcode)) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_WRITE);
	ssize_t want = auio.uio_resid;

	waitall = (flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	flags &= ~MSG_WAITALL;
	error = sosend(so, to, &auio, NULL, NULL, flags, td);
	n = (size_t)(want - auio.uio_resid);
	fdrop(fp, td);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		return (rc);
	}
	if (buf_select) {
		cqe_flags = kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
	}
	if (waitall && kqring_waitall(op, n, sqe->len, &n) == EAGAIN)
		return (EAGAIN);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
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
 * kern_recvit() on a socket rather than a descriptor (fixed files have
 * none).  mp->msg_iov is a kernel copy of user iovecs; the name goes to
 * `fromseg` memory, and without controlp the control data to user memory.
 * The byte count goes to *np.
 */
static int
kqring_recvit(struct thread *td, struct socket *so, struct msghdr *mp,
    enum uio_seg fromseg, struct mbuf **controlp, size_t *np)
{
	struct uio auio;
	struct mbuf *control = NULL, *m;
	struct sockaddr *fromsa = NULL;
	caddr_t ctlbuf;
	ssize_t len;
	int error;

	if (controlp != NULL)
		*controlp = NULL;
#ifdef KQRING_MAC
	error = mac_socket_check_receive(td->td_ucred, so);
	if (error != 0)
		return (error);
#endif
	kqring_uio_init(&auio, mp->msg_iov, mp->msg_iovlen, UIO_READ);
	len = auio.uio_resid;
	error = soreceive(so, &fromsa, &auio, NULL,
	    (mp->msg_control != NULL || controlp != NULL) ? &control : NULL,
	    &mp->msg_flags);
	if (error != 0 && auio.uio_resid != len && (error == ERESTART ||
	    error == EINTR || error == EWOULDBLOCK))
		error = 0;
	if (error != 0)
		goto out;
	*np = (size_t)(len - auio.uio_resid);
	if (mp->msg_name != NULL) {
		len = mp->msg_namelen;
		if (len <= 0 || fromsa == NULL) {
			len = 0;
		} else {
			len = MIN(len, fromsa->sa_len);
			if (fromseg == UIO_USERSPACE) {
				error = copyout(fromsa, mp->msg_name,
				    (unsigned)len);
				if (error != 0)
					goto out;
			} else {
				bcopy(fromsa, mp->msg_name, len);
			}
		}
		mp->msg_namelen = len;
	}
	if (mp->msg_control != NULL && controlp == NULL) {
		ctlbuf = mp->msg_control;
		len = mp->msg_controllen;
		mp->msg_controllen = 0;
		for (m = control; m != NULL && len >= m->m_len;
		    m = m->m_next) {
			error = copyout(mtod(m, caddr_t), ctlbuf, m->m_len);
			if (error != 0)
				goto out;
			ctlbuf += m->m_len;
			len -= m->m_len;
			mp->msg_controllen += m->m_len;
		}
		if (m != NULL) {
			mp->msg_flags |= MSG_CTRUNC;
			m_dispose_extcontrolm(m);
		}
	}
out:
	free(fromsa, M_SONAME);
	if (error == 0 && controlp != NULL) {
		*controlp = control;
	} else if (control != NULL) {
		if (error != 0)
			m_dispose_extcontrolm(control);
		m_freem(control);
	}
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
	struct kqring_sel sel;
	struct msghdr m;
	struct iovec aiov, biov;
	struct mbuf *control = NULL;
	uint8_t name[SOCK_MAXADDRLEN];
	size_t namesz = ma->msg.msg_namelen;
	size_t ctlsz = ma->msg.msg_controllen;
	size_t hdr = sizeof(out) + namesz + ctlsz, clen, n;
	char *ubuf;
	int error;

	error = kqring_recv_pick_buf(ctx, op, so, kqring_msg_buf_cap(ma),
	    &biov, 1, &sel);
	if (error != 0) {
		if (error == EAGAIN)
			return (EAGAIN);
		return (kqring_done(op, -error, 0));
	}
	if (biov.iov_len < hdr)
		return (kqring_done(op, -EFAULT, 0));
	ubuf = biov.iov_base;

	/* Ask for the whole name, to report its real length. */
	bzero(&m, sizeof(m));
	m.msg_name = name;
	m.msg_namelen = sizeof(name);
	aiov.iov_base = ubuf + hdr;
	aiov.iov_len = biov.iov_len - hdr;
	m.msg_iov = &aiov;
	m.msg_iovlen = 1;
	m.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	error = kqring_recvit(td, so, &m, UIO_SYSSPACE,
	    ctlsz != 0 ? &control : NULL, &n);
	if (error == EWOULDBLOCK)
		return (EAGAIN);
	if (error != 0)
		return (kqring_done(op, -error, 0));
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

	n = hdr + MIN(n, aiov.iov_len);
	*more_out = kqring_so_rcv_pending(so, true);
	return (kqring_done(op, kqring_xfer_res(n),
	    kqring_buf_commit(ctx, &sel, n) | IORING_CQE_F_MORE));
}

/* RECVMSG: kqring_recvit() on the kernel copy of the msghdr's iovec. */
static int
kqring_exec_recvmsg(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct msghdr *umsg = (struct msghdr *)(uintptr_t)sqe->addr;
	struct kqring_sel sel;
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
		sx_xlock(&ctx->bg_sx);
		rc = kqring_exec_recvmsg_multi(ctx, op, so, more_out);
		sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		return (rc);
	}

	msg = ma->msg;
	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_recv_pick_buf(ctx, op, so,
		    kqring_msg_buf_cap(ma), &aiov, 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		msg.msg_iov = &aiov;
		msg.msg_iovlen = 1;
	}
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	msg.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	waitall = (msg.msg_flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	n = 0;
	error = kqring_recvit(td, so, &msg, UIO_USERSPACE, NULL, &n);
	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
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
	if (buf_select) {
		if (n > 0)
			cqe_flags |= kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
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

/*
 * kern_sendit() on a socket rather than a descriptor (see kqring_recvit()).
 * Consumes `control`.  The byte count goes to *np.
 */
static int
kqring_sendit(struct thread *td, struct socket *so, struct msghdr *mp,
    int flags, struct mbuf *control, size_t *np)
{
	struct uio auio;
	ssize_t len;
	int error;

	*np = 0;
#ifdef KQRING_MAC
	error = 0;
	if (mp->msg_name != NULL)
		error = mac_socket_check_connect(td->td_ucred, so,
		    mp->msg_name);
	if (error == 0)
		error = mac_socket_check_send(td->td_ucred, so);
	if (error != 0) {
		m_freem(control);
		return (error);
	}
#endif
	kqring_uio_init(&auio, mp->msg_iov, mp->msg_iovlen, UIO_WRITE);
	len = auio.uio_resid;
	error = sousrsend(so, mp->msg_name, &auio, control, flags, NULL);
	if (error == 0)
		*np = (size_t)(len - auio.uio_resid);
	return (error);
}

/* SENDMSG / SENDMSG_ZC: kqring_sendit() on the kernel copy of the msghdr. */
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
	if (ma->msg.msg_name != NULL)
		cap_rights_init(&rights, CAP_SEND, CAP_CONNECT);
	else
		cap_rights_init_one(&rights, CAP_SEND);
	error = kqring_getsock(op, expect, &rights, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;
	waitall = (sqe->msg_flags & MSG_WAITALL) != 0 && kqring_so_stream(so);

	msg = ma->msg;
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	if (msg.msg_controllen != 0) {
		/* kqring_sendit() consumes the mbuf: rebuild it each attempt. */
		control = m_get2(msg.msg_controllen, M_WAITOK, MT_CONTROL, 0);
		control->m_len = msg.msg_controllen;
		bcopy(msg.msg_control, mtod(control, void *),
		    msg.msg_controllen);
	}
	flags = ((int)sqe->msg_flags & ~MSG_WAITALL) | MSG_DONTWAIT |
	    MSG_NOSIGNAL;
	error = kqring_sendit(td, so, &msg, flags, control, &n);
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

/*
 * ACCEPT: kern_accept4() on the listening socket's file, so that it works
 * on a fixed file, and can create a direct descriptor (sqe->file_index).
 * The new file inherits the listener's Capsicum rights, as accept(2)'s
 * descriptor does, and gets only the flags asked for, as accept4(2).
 */
static int
kqring_exec_accept(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *headfp = NULL, *nfp = NULL;
	struct filecaps hcaps, ncaps;
	struct socket *head, *so;
	struct sockaddr_storage ss = { .ss_len = sizeof(ss) };
	cap_rights_t rights;
	bool direct = sqe->file_index != 0;
	u_int fflag;
	int error, nfd = -1, tmp;
	int aflags = (int)sqe->accept_flags;
	int32_t res;

	error = kqring_op_fget_caps(op, cap_rights_init_one(&rights,
	    CAP_ACCEPT), &headfp, &hcaps);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (headfp->f_type != DTYPE_SOCKET) {
		error = ENOTSOCK;
		goto fail;
	}
	head = headfp->f_data;
	if (expect != NULL && head != expect) {
		error = EBADF;
		goto fail;
	}
	if (!SOLISTENING(head)) {
		error = EINVAL;
		goto fail;
	}
	/* Listening sockets have no so_rcv; only sol_comp/so_error apply. */
	if (TAILQ_EMPTY(&head->sol_comp) && head->so_error == 0) {
		filecaps_free(&hcaps);
		fdrop(headfp, td);
		return (EAGAIN);
	}
#ifdef KQRING_MAC
	error = mac_socket_check_accept(td->td_ucred, head);
	if (error != 0)
		goto fail;
#endif

	if (direct) {
		error = falloc_noinstall(td, &nfp);
	} else {
		(void)filecaps_copy(&hcaps, &ncaps, false);
		error = falloc_caps(td, &nfp, &nfd,
		    ((aflags & SOCK_CLOEXEC) != 0 ? O_CLOEXEC : 0) |
		    ((aflags & SOCK_CLOFORK) != 0 ? O_CLOFORK : 0), &ncaps);
		if (error != 0)
			filecaps_free(&ncaps);
	}
	if (error != 0) {
		nfp = NULL;
		goto fail;
	}
	SOCK_LOCK(head);
	if (!SOLISTENING(head)) {
		SOCK_UNLOCK(head);
		error = EINVAL;
		goto fail;
	}
	/*
	 * Re-check under the listen lock: another accepter may have taken
	 * the connection, and solisten_dequeue() would then sleep (for a
	 * blocking listener) in the ring owner.
	 */
	if (TAILQ_EMPTY(&head->sol_comp) && head->so_error == 0) {
		SOCK_UNLOCK(head);
		error = EWOULDBLOCK;
		goto fail;
	}
	error = solisten_dequeue(head, &so, aflags);	/* unlocks head */
	if (error != 0)
		goto fail;
	KNOTE_UNLOCKED(&head->so_rdsel.si_note, 0);

	fflag = atomic_load_int(&headfp->f_flag) & ~(FNONBLOCK | FASYNC);
	if ((aflags & SOCK_NONBLOCK) != 0)
		fflag |= FNONBLOCK;
	finit(nfp, fflag, DTYPE_SOCKET, so, &socketops);
	/* Sync socket nonblocking/async state with file flags. */
	tmp = fflag & FNONBLOCK;
	(void)fo_ioctl(nfp, FIONBIO, &tmp, td->td_ucred, td);
	tmp = fflag & FASYNC;
	(void)fo_ioctl(nfp, FIOASYNC, &tmp, td->td_ucred, td);
	error = soaccept(so, (struct sockaddr *)&ss);
	if (error != 0)
		goto fail;

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
		if (error != 0)
			goto fail;
	}

	if (direct) {
		/* The fixed table takes nfp's reference and hcaps. */
		res = kqring_fixed_install(ctx, nfp, &hcaps,
		    sqe->file_index);
		nfp = NULL;
	} else {
		filecaps_free(&hcaps);
		fdrop(nfp, td);		/* falloc_caps()'s extra reference */
		res = nfd;
	}
	if (res >= 0 && (sqe->ioprio & IORING_ACCEPT_MULTISHOT) != 0) {
		*more_out = !TAILQ_EMPTY(&head->sol_comp);
		fdrop(headfp, td);
		return (kqring_done(op, res, IORING_CQE_F_MORE));
	}
	fdrop(headfp, td);
	return (kqring_done(op, res, 0));

fail:
	if (nfp != NULL) {
		/* Closes the accepted socket too, if finit() ran. */
		if (!direct)
			fdclose(td, nfp, nfd);
		fdrop(nfp, td);
	}
	filecaps_free(&hcaps);
	fdrop(headfp, td);
	if (error == EWOULDBLOCK)
		return (EAGAIN);
	return (kqring_done(op, -error, 0));
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

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_POLL_EVENT),
	    0, &fp);
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
#ifdef KQRING_MAC
	error = mac_socket_check_poll(td->td_ucred, fp->f_data);
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
#endif
	revents = kqring_so_revents(fp->f_data, events);
	fdrop(fp, td);

	if (revents == 0)
		return (EAGAIN);
	return (kqring_done(op, revents,
	    (sqe->len & IORING_POLL_ADD_MULTI) != 0 ? IORING_CQE_F_MORE : 0));
}

/*
 * A CQE result for an errno.  An interrupted sleep reports EINTR:
 * ERESTART and EJUSTRETURN are negative, so negating them would look like
 * a successful byte count.
 */
static inline int32_t
kqring_errno(int error)
{
	if (error == ERESTART || error == EJUSTRETURN)
		return (-EINTR);
	return (-error);
}

static bool
kqring_pipe_readable(const struct file *fp)
{
	struct pipe *rpipe;
	bool ok;

	if (fp->f_type != DTYPE_PIPE || (fp->f_flag & FNONBLOCK) != 0)
		return (true);
	rpipe = fp->f_data;
	PIPE_LOCK(rpipe);
	ok = rpipe->pipe_buffer.cnt > 0 || rpipe->pipe_pages.cnt > 0 ||
	    (rpipe->pipe_state & PIPE_EOF) != 0;
	PIPE_UNLOCK(rpipe);
	return (ok);
}

static bool
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

static void
kqring_wq_mark_blocking(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (!op->wq_running || op->wq_blocking)
		return;
	mtx_lock(&ctx->mtx);
	op->wq_blocking = true;
	if (ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
	mtx_unlock(&ctx->mtx);
}

static int
kqring_pipe_wait_read(struct kqring_ctx *ctx, struct file *fp,
    struct kqring_op *op)
{
	struct pipe *rpipe = fp->f_data;
	int error = 0;

	kqring_wq_mark_blocking(ctx, op);
	PIPE_LOCK(rpipe);
	while (rpipe->pipe_buffer.cnt == 0 && rpipe->pipe_pages.cnt == 0 &&
	    (rpipe->pipe_state & PIPE_EOF) == 0 &&
	    (fp->f_flag & FNONBLOCK) == 0) {
		if (op->wq_canceled || op->cancelled || ctx->closing) {
			error = ECANCELED;
			break;
		}
		rpipe->pipe_state |= PIPE_WANT;
		error = msleep(rpipe, PIPE_MTX(rpipe), PRIBIO | PCATCH,
		    "piperd", 0);
		if (error != 0)
			break;
	}
	PIPE_UNLOCK(rpipe);
	return (error);
}

/*
 * READ / WRITE / READV / WRITEV and their _FIXED forms on anything but a
 * socket (sockets go through kqring_exec_recv() / kqring_exec_send() and
 * can be armed): fo_read() / fo_write() on the file, with the checks
 * kern_preadv() and dofileread() make.  READ / READV may take a provided
 * buffer, only consumed if bytes were read.  A SIGPIPE is never raised.
 */
static int
kqring_exec_file_rw(struct kqring_ctx *ctx, struct kqring_op *op,
    bool is_write)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct kqring_sel sel;
	struct file *fp;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	cap_rights_t rights;
	off_t offset = (off_t)sqe->off;
	u_int iovcnt = 1;
	uint32_t cflags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	int error, flags = 0;

	/* off == -1 means "use the file position", as in io_uring. */
	if (offset != (off_t)-1)
		flags = FOF_OFFSET;
	if (is_write && flags == 0)
		cap_rights_init_one(&rights, CAP_WRITE);
	else if (is_write)
		cap_rights_init_one(&rights, CAP_PWRITE);
	else if (flags == 0)
		cap_rights_init_one(&rights, CAP_READ);
	else
		cap_rights_init_one(&rights, CAP_PREAD);
	error = kqring_op_fget(op, &rights, is_write ? FWRITE : FREAD, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (flags != 0) {
		if ((fp->f_ops->fo_flags & DFLAG_SEEKABLE) == 0)
			error = ESPIPE;
		else if (offset < 0 && (fp->f_vnode == NULL ||
		    fp->f_vnode->v_type != VCHR))
			error = EINVAL;
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
	}

	if (buf_select) {
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
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
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
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		return (kqring_done(op, 0, 0));
	}
	if (kqring_pipe_will_block(fp, is_write, (size_t)want))
		kqring_wq_mark_blocking(ctx, op);
	error = is_write ? fo_write(fp, &auio, td->td_ucred, flags, td) :
	    fo_read(fp, &auio, td->td_ucred, flags, td);
	fdrop(fp, td);

	size_t done = (size_t)(want - auio.uio_resid);
	if (buf_select) {
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
static int32_t
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
static int32_t
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
static int32_t
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

static int32_t
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
static int32_t
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

/* Whether fp is a /dev/kqring descriptor. */
static bool
kqring_fp_is_ring(const struct file *fp)
{
	return (fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
	    fp->f_vnode->v_type == VCHR && fp->f_vnode->v_rdev == kqring_cdev);
}

/*
 * CLOSE: like close(2), or with file_index, empty that fixed slot.  A ring
 * cannot close itself or another ring.
 */
static int32_t
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
 * URING_CMD on a socket: SIOCINQ, SIOCOUTQ, getsockopt, setsockopt, with
 * the Capsicum checks ioctl(2) and kern_getsockopt() make.
 */
static int32_t
kqring_exec_uring_cmd(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct filecaps caps;
	struct sockopt sopt;
	struct file *fp;
	cap_rights_t rights;
	u_long cmd;
	int error, n;

	switch (sqe->cmd_op) {
	case SOCKET_URING_OP_SIOCINQ:
	case SOCKET_URING_OP_SIOCOUTQ:
		cap_rights_init_one(&rights, CAP_IOCTL);
		break;
	case SOCKET_URING_OP_GETSOCKOPT:
		/* As in io_uring, only SOL_SOCKET options. */
		if (sqe->level != SOL_SOCKET)
			return (-EOPNOTSUPP);
		cap_rights_init_one(&rights, CAP_GETSOCKOPT);
		break;
	case SOCKET_URING_OP_SETSOCKOPT:
		cap_rights_init_one(&rights, CAP_SETSOCKOPT);
		break;
	default:
		return (-EOPNOTSUPP);
	}
	error = kqring_op_fget_caps(op, &rights, &fp, &caps);
	if (error != 0)
		return (-error);
	/* No other file type implements commands. */
	if (fp->f_type != DTYPE_SOCKET) {
		error = EOPNOTSUPP;
		goto out;
	}

	switch (sqe->cmd_op) {
	case SOCKET_URING_OP_SIOCINQ:
	case SOCKET_URING_OP_SIOCOUTQ:
		cmd = sqe->cmd_op == SOCKET_URING_OP_SIOCINQ ? FIONREAD :
		    FIONWRITE;
		if (caps.fc_nioctls != -1) {
			int i;

			for (i = 0; i < caps.fc_nioctls; i++)
				if (caps.fc_ioctls[i] == cmd)
					break;
			if (i == caps.fc_nioctls) {
				error = ENOTCAPABLE;
				goto out;
			}
		}
		error = fo_ioctl(fp, cmd, (caddr_t)&n, td->td_ucred, td);
		if (error == 0) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return (n);
		}
		break;
	default:
		bzero(&sopt, sizeof(sopt));
		sopt.sopt_dir = sqe->cmd_op == SOCKET_URING_OP_GETSOCKOPT ?
		    SOPT_GET : SOPT_SET;
		sopt.sopt_level = (int)sqe->level;
		sopt.sopt_name = (int)sqe->optname;
		sopt.sopt_val = (void *)(uintptr_t)sqe->optval;
		sopt.sopt_valsize = sqe->optlen;
		sopt.sopt_td = td;
		sopt.sopt_rights = &caps.fc_rights;
		/* The checks of kern_getsockopt() / kern_setsockopt(). */
		if (sopt.sopt_dir == SOPT_GET) {
			if (sopt.sopt_val == NULL)
				sopt.sopt_valsize = 0;
			else if ((int)sqe->optlen < 0)
				error = EINVAL;
			if (error == 0)
				error = sogetopt(fp->f_data, &sopt);
		} else {
			if (sopt.sopt_val == NULL && sqe->optlen != 0)
				error = EFAULT;
			else if ((int)sqe->optlen < 0)
				error = EINVAL;
			if (error == 0)
				error = sosetopt(fp->f_data, &sopt);
		}
		if (error == 0 && sopt.sopt_dir == SOPT_GET) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return ((int32_t)MIN(sopt.sopt_valsize, INT32_MAX));
		}
		break;
	}
out:
	filecaps_free(&caps);
	fdrop(fp, td);
	return (-error);
}

/* The socket of a synchronous op (BIND, LISTEN, SHUTDOWN). */
static int
kqring_op_sock(const struct kqring_op *op, cap_rights_t *rights,
    struct file **fpp)
{
	return (kqring_getsock(op, NULL, rights, fpp));
}

static int32_t
kqring_exec_bind(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct sockaddr *sa = op->args;
	struct file *fp;
	cap_rights_t rights;
	int error;

	/* As bind(2): only with a directory descriptor in capability mode. */
	if (IN_CAPABILITY_MODE(td))
		return (-ECAPMODE);
	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_BIND), &fp);
	if (error != 0)
		return (-error);
#ifdef KQRING_MAC
	error = mac_socket_check_bind(td->td_ucred, fp->f_data, sa);
	if (error == 0)
#endif
		error = sobind(fp->f_data, sa, td);
	fdrop(fp, td);
	return (-error);
}

static int32_t
kqring_exec_listen(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct file *fp;
	cap_rights_t rights;
	int error;

	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_LISTEN),
	    &fp);
	if (error != 0)
		return (-error);
#ifdef KQRING_MAC
	error = mac_socket_check_listen(td->td_ucred, fp->f_data);
	if (error == 0)
#endif
		error = solisten(fp->f_data, (int)op->sqe.len, td);
	fdrop(fp, td);
	return (-error);
}

static int32_t
kqring_exec_shutdown(const struct kqring_op *op)
{
	struct file *fp;
	cap_rights_t rights;
	int how = (int)op->sqe.len, error;

	if (how < SHUT_RD || how > SHUT_RDWR)
		return (-EINVAL);
	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_SHUTDOWN),
	    &fp);
	if (error != 0)
		return (-error);
	error = soshutdown(fp->f_data, how);
	fdrop(fp, curthread);
	return (-error);
}

/*
 * SOCKET: socket(2), or with file_index kern_socket() without a
 * descriptor, into the fixed table.
 */
static int32_t
kqring_exec_socket(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct filecaps caps;
	struct socket *so;
	struct file *fp;
	int domain = sqe->fd, type = (int)sqe->off, proto = (int)sqe->len;
	int error, fflag = 0;

	if (sqe->file_index == 0) {
		error = kern_socket(td, domain, type, proto);
		return (error == 0 ? (int32_t)td->td_retval[0] : -error);
	}
	/* Close-on-exec/fork mean nothing without a descriptor. */
	type &= ~SOCK_CLOFORK;
	if ((type & SOCK_NONBLOCK) != 0) {
		type &= ~SOCK_NONBLOCK;
		fflag |= FNONBLOCK;
	}
#ifdef KQRING_MAC
	error = mac_socket_check_create(td->td_ucred, domain, type, proto);
	if (error != 0)
		return (-error);
#endif
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (-error);
	error = socreate(domain, &so, type, proto, td->td_ucred, td);
	if (error != 0) {
		fdrop(fp, td);
		return (-error);
	}
	finit(fp, FREAD | FWRITE | fflag, DTYPE_SOCKET, so, &socketops);
	if ((fflag & FNONBLOCK) != 0)
		(void)fo_ioctl(fp, FIONBIO, &fflag, td->td_ucred, td);
	kqring_caps_all(&caps);
	if (op->wq_running) {
		op->wq_fp = fp;
		filecaps_move(&caps, &op->wq_caps);
		return (0);
	}
	return (kqring_fixed_install(ctx, fp, &caps, sqe->file_index));
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
static int32_t
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
static int32_t
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

/* FIXED_FD_INSTALL: a descriptor for a fixed file, with its rights. */
static int32_t
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
static int32_t
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
static int32_t
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

	switch (sqe->opcode) {
	case IORING_OP_NOP:
		res = 0;
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
		res = sqe->off == IORING_FILE_INDEX_ALLOC ?
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
		case IORING_OP_WRITE_FIXED:
		case IORING_OP_WRITEV_FIXED:
			rc = kqring_exec_send(ctx, op, expect);
			break;
		case IORING_OP_SENDMSG:
		case IORING_OP_SENDMSG_ZC:
			rc = kqring_exec_sendmsg(op, expect);
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
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITEV_FIXED:
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

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_EVENT), 0,
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
 * WAITID: wait6(2) on the op's child selector.  With `nohang`, poll once
 * with WNOHANG and return EAGAIN if a blocking wait is still needed;
 * without `nohang`, block in kern_wait6() on an unbound worker thread.
 */
static int
kqring_exec_waitid(struct kqring_op *op, bool nohang)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	siginfo_t si;
	int options = (int)sqe->file_index;
	int status, error;

	if (nohang)
		options |= WNOHANG;
	bzero(&si, sizeof(si));
	td->td_retval[0] = 0;
	error = kern_wait6(td, (idtype_t)sqe->len, (id_t)sqe->fd, &status,
	    options, NULL, &si);
	if (error != 0) {
		if ((error == EINTR || error == ERESTART) &&
		    (op->wq_canceled || op->cancelled))
			return (kqring_done(op, -ECANCELED, 0));
		return (kqring_done(op, kqring_errno(error), 0));
	}
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
	if ((op->sqe.flags & IOSQE_ASYNC) == 0 &&
	    kqring_exec_waitid(op, true) == 0) {
		kqring_req_complete(ctx, op, op->res, 0);
		return (false);
	}
	return (kqring_wq_enqueue(ctx, op, IO_WQ_UNBOUND));
}

/* The ring context of a /dev/kqring file. */
static int
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

/* Free a queued MSG_RING CQE, with the file it may carry. */
static void
kqring_rcqe_free(struct kqring_rcqe *rc)
{
	if (rc->fp != NULL) {
		filecaps_free(&rc->caps);
		fdrop(rc->fp, curthread);
	}
	free(rc, M_KQRING);
}

/*
 * Queue rc on ring tctx for its owner to post (and for SEND_FD, first
 * install its file), and wake the owner.
 */
static int
kqring_rcqe_queue(struct kqring_ctx *tctx, struct kqring_rcqe *rc)
{
	int error = 0;

	mtx_lock(&tctx->mtx);
	if (!tctx->setup_done || !tctx->enabled || tctx->closing) {
		error = EBADF;
	} else if (tctx->nrcq >= KQRING_MAX_REMOTE) {
		error = EOVERFLOW;
	} else {
		if (STAILQ_EMPTY(&tctx->rcq))
			kqring_evfd_signal_locked(tctx);
		STAILQ_INSERT_TAIL(&tctx->rcq, rc, link);
		tctx->nrcq++;
		atomic_set_32(&tctx->shm->sq_flags, IORING_SQ_TASKRUN);
		if (tctx->sleeping)
			wakeup(&tctx->wakeup_chan);
	}
	mtx_unlock(&tctx->mtx);
	return (error);
}

/*
 * MSG_RING: post a CQE to the ring sqe->fd (IORING_MSG_DATA, possibly this
 * ring), or pass it our fixed file sqe->addr3 for its fixed slot
 * sqe->file_index (IORING_MSG_SEND_FD).  Both are only queued on the
 * target, whose owner posts the CQE, and installs the file, from its own
 * taskwork: no thread ever touches another ring's CQ or file table.  So a
 * SEND_FD that the target cannot install (no table, bad slot) reports the
 * error in the target's CQE, where io_uring reports it to the sender.
 * Holding the target's file keeps its context alive.
 */
static int32_t
kqring_msg_ring(struct kqring_ctx *ctx, const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct kqring_ctx *tctx;
	struct kqring_rcqe *rc;
	struct kqring_rsrc *src;
	struct file *fp;
	cap_rights_t rights;
	int error;

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_IOCTL), 0,
	    &fp);
	if (error != 0)
		return (-error);
	error = kqring_fp_ctx(fp, &tctx);
	if (error != 0) {
		fdrop(fp, td);
		return (-error);
	}

	rc = malloc(sizeof(*rc), M_KQRING, M_WAITOK | M_ZERO);
	rc->cqe.user_data = sqe->off;
	if (sqe->addr == IORING_MSG_SEND_FD) {
		if (tctx == ctx) {
			error = EINVAL;
		} else if (ctx->files == NULL || sqe->addr3 >= ctx->nfiles ||
		    (src = ctx->files[sqe->addr3]) == NULL) {
			error = EBADF;
		} else if (!fhold(src->fp)) {
			error = EBADF;
		} else {
			rc->fp = src->fp;
			(void)filecaps_copy(&src->caps, &rc->caps, false);
			rc->file_index = sqe->file_index;
			rc->skip_cqe = (sqe->msg_ring_flags &
			    IORING_MSG_RING_CQE_SKIP) != 0;
		}
	} else {
		rc->cqe.res = (int32_t)sqe->len;
		rc->cqe.flags = (sqe->msg_ring_flags &
		    IORING_MSG_RING_FLAGS_PASS) != 0 ? sqe->file_index : 0;
	}
	if (error == 0)
		error = kqring_rcqe_queue(tctx, rc);
	if (error != 0)
		kqring_rcqe_free(rc);
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

/*
 * Whether armed request op matches a cancellation.  CANCEL_FD matches
 * requests on descriptor sqe->fd, or with CANCEL_FD_FIXED those on the
 * file in fixed slot sqe->fd (`cfp`); a fixed request's sqe->fd is a slot
 * number, never compared with a descriptor.
 */
static bool
kqring_cancel_match(const struct kqring_op *op, const struct io_uring_sqe *sqe,
    const struct file *cfp)
{
	uint32_t cflags = sqe->cancel_flags;

	if (op->cancelled)
		return (false);
	if ((cflags & IORING_ASYNC_CANCEL_ANY) != 0)
		return (true);
	if ((cflags & IORING_ASYNC_CANCEL_FD) != 0) {
		if (cfp != NULL ? op->ffile == NULL || op->ffile->fp != cfp :
		    op->ffile != NULL || op->sqe.fd != sqe->fd)
			return (false);
	}
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
	struct file *cfp = NULL;
	int32_t n = 0;

	if ((sqe->cancel_flags & IORING_ASYNC_CANCEL_FD_FIXED) != 0) {
		if (ctx->files == NULL || (uint32_t)sqe->fd >= ctx->nfiles ||
		    ctx->files[sqe->fd] == NULL)
			return (-EBADF);
		/* Only compared: the slot's reference is not needed. */
		cfp = ctx->files[sqe->fd]->fp;
	}
	for (;;) {
		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (kqring_cancel_match(op, sqe, cfp))
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
		return (true);
	default:
		return (false);
	}
}

static inline bool
kqring_bit_isset(const uint64_t *map, u_int bit)
{
	return (((map[bit / 64] >> (bit % 64)) & 1) != 0);
}

static inline void
kqring_bit_set(uint64_t *map, u_int bit)
{
	map[bit / 64] |= (uint64_t)1 << (bit % 64);
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

	if ((sqe->flags & ~KQRING_SQE_FLAGS) != 0)
		return (EINVAL);
	if (ctx->restricted && !kqring_restriction_ok(ctx, sqe))
		return (EACCES);
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

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
		if (sqe->rw_flags != 0)
			return (EINVAL);
		buf_select_ok = sqe->opcode == IORING_OP_READ;
		break;
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		if (sqe->rw_flags != 0)
			return (EINVAL);
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
		/* A registered buffer (buf_index) holds SEND_ZC's data. */
		if (sqe->opcode == IORING_OP_SEND_ZC)
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
		break;
	case IORING_OP_PROVIDE_BUFFERS:
		/* sqe->fd buffers, IDs from sqe->off, as in io_uring. */
		if (sqe->rw_flags != 0 || sqe->splice_fd_in != 0)
			return (EINVAL);
		if (sqe->fd <= 0 || sqe->fd > (int)KQRING_MAX_GROUP_BUFS)
			return (E2BIG);
		if (sqe->len == 0)
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

/* Whether op's file is a socket (lookup errors are left to the op). */
static bool
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

/* ============================================================================
 * In-Process Worker Thread Pool (IO_WQ_BOUND / IO_WQ_UNBOUND)
 * ============================================================================
 */

/*
 * Handle process-wide stop/single-threading requests (fork, execve,
 * exit1, SIGSTOP) on an in-process worker kthread.  Called with
 * ctx->mtx held; returns true if the process is exiting or exec'ing
 * and the caller must kthread_exit().
 */
static bool
kqring_wq_check_suspend(struct kqring_ctx *ctx)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (!P_SHOULDSTOP(p) && (p->p_flag2 & P2_WEXIT) == 0 &&
	    !td_ast_pending(td, TDA_SUSPEND))
		return (false);
	mtx_unlock(&ctx->mtx);
	PROC_LOCK(p);
	while (thread_suspend_check_needed() ||
	    (p->p_flag2 & P2_WEXIT) != 0) {
		if ((p->p_flag & P_SINGLE_EXIT) != 0 ||
		    (p->p_flag2 & P2_WEXIT) != 0) {
			PROC_UNLOCK(p);
			mtx_lock(&ctx->mtx);
			return (true);
		}
		PROC_SLOCK(p);
		thread_stopped(p);
		if (P_SHOULDSTOP(p) == P_STOPPED_SINGLE &&
		    p->p_numthreads == p->p_suspcount + 1) {
			thread_lock(p->p_singlethread);
			if (TD_IS_SUSPENDED(p->p_singlethread)) {
				TD_CLR_SUSPENDED(p->p_singlethread);
				p->p_singlethread->td_flags &= ~TDF_ALLPROCSUSP;
				p->p_suspcount--;
				setrunnable(p->p_singlethread, 0);
			} else {
				thread_unlock(p->p_singlethread);
			}
		}
		PROC_UNLOCK(p);
		thread_lock(td);
		thread_suspend_one(td);
		p->p_boundary_count++;
		td->td_flags |= TDF_BOUNDARY;
		PROC_SUNLOCK(p);
		mi_switch(SW_INVOL | SWT_SUSPEND);
		PROC_LOCK(p);
	}
	PROC_UNLOCK(p);
	mtx_lock(&ctx->mtx);
	return (false);
}

static void
kqring_wq_apply_affinity(struct kqring_ctx *ctx, uint32_t *genp)
{
	cpuset_t mask;
	uint32_t gen;

	mtx_lock(&ctx->mtx);
	gen = ctx->wq_aff_gen;
	if (gen == *genp && gen != 0) {
		mtx_unlock(&ctx->mtx);
		return;
	}
	if (ctx->wq_aff_set)
		CPU_COPY(&ctx->wq_cpumask, &mask);
	else if (ctx->wq_base_cpuset != NULL)
		CPU_COPY(&ctx->wq_base_cpuset->cs_mask, &mask);
	else
		CPU_COPY(&all_cpus, &mask);
	*genp = gen;
	mtx_unlock(&ctx->mtx);
	(void)cpuset_setthread(curthread->td_tid, &mask);
}

static void
kqring_wq_exec_op(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct ucred *saved;

	saved = kqring_cred_enter(op);
	if (op->sqe.opcode == IORING_OP_WAITID)
		(void)kqring_exec_waitid(op, false);
	else
		kqring_exec_sync(ctx, op);
	kqring_cred_exit(saved);
}

static void
kqring_wq_thread(void *arg)
{
	struct {
		struct kqring_ctx	*ctx;
		int			type;
	} *wa = arg;
	struct kqring_ctx *ctx = wa->ctx;
	int type = wa->type;
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct kqring_op *op;
	uint32_t aff_gen = UINT32_MAX;
	int error;

	/*
	 * Mask all maskable signals on this worker kthread so process-wide
	 * user signals (SIGINT, SIGALRM, SIGUSR1, ...) are delivered to
	 * user threads rather than interrupting background ring I/O.
	 */
	PROC_LOCK(p);
	SIGFILLSET(td->td_sigmask);
	SIG_CANTMASK(td->td_sigmask);
	PROC_UNLOCK(p);

	kqring_wq_apply_affinity(ctx, &aff_gen);

	mtx_lock(&ctx->mtx);
	for (;;) {
		if (ctx->closing || kqring_wq_check_suspend(ctx))
			break;
		if (ctx->wq_total[type] > ctx->wq_max[type])
			break;
		if (aff_gen != ctx->wq_aff_gen) {
			mtx_unlock(&ctx->mtx);
			kqring_wq_apply_affinity(ctx, &aff_gen);
			mtx_lock(&ctx->mtx);
			continue;
		}
		op = TAILQ_FIRST(&ctx->wq_ops[type]);
		if (op != NULL) {
			TAILQ_REMOVE(&ctx->wq_ops[type], op, wq_link);
			op->wq_queued = false;
			ctx->wq_pending[type]--;
			if (op->cancelled || op->wq_canceled) {
				ctx->wq_inflight--;
				continue;
			}
			op->wq_running = true;
			op->wq_td = td;
			ctx->wq_busy[type]++;
			if (ctx->wq_total[type] <
			    ctx->wq_pending[type] + ctx->wq_busy[type] &&
			    ctx->wq_total[type] < ctx->wq_max[type]) {
				ctx->wq_total[type]++;
				mtx_unlock(&ctx->mtx);
				if (kthread_add(kqring_wq_thread,
				    &ctx->wq_arg[type], p, NULL, 0, 0,
				    "kqwq%c", type == IO_WQ_BOUND ? 'b' : 'u')
				    != 0) {
					mtx_lock(&ctx->mtx);
					ctx->wq_total[type]--;
					mtx_unlock(&ctx->mtx);
				}
			} else {
				mtx_unlock(&ctx->mtx);
			}

			kqring_wq_exec_op(ctx, op);

			mtx_lock(&ctx->mtx);
			op->wq_running = false;
			op->wq_td = NULL;
			op->wq_done = true;
			ctx->wq_busy[type]--;
			ctx->wq_inflight--;
			wakeup(&op->wq_running);
			if (!op->cancelled && !ctx->closing && op->wq_blocking)
				kqring_taskwork_add_locked(ctx, op);
			if (ctx->sleeping)
				wakeup(&ctx->wakeup_chan);
			continue;
		}

		error = msleep_sbt(&ctx->wq_ops[type], &ctx->mtx, PCATCH,
		    "kqwqid", KQRING_WQ_IDLE_SBT, 0, 0);
		if (error == EWOULDBLOCK &&
		    TAILQ_EMPTY(&ctx->wq_ops[type]))
			break;
	}
	ctx->wq_total[type]--;
	if (ctx->wq_total[type] == 0)
		wakeup(&ctx->wq_total[type]);
	mtx_unlock(&ctx->mtx);
	kthread_exit();
}

static bool
kqring_op_will_block(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	size_t want = sqe->len;
	bool is_write = false, block = false;

	switch (sqe->opcode) {
	case IORING_OP_WRITE:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITEV:
	case IORING_OP_WRITEV_FIXED:
		is_write = true;
		/* FALLTHROUGH */
	case IORING_OP_READ:
	case IORING_OP_READ_FIXED:
	case IORING_OP_READV:
	case IORING_OP_READV_FIXED:
		if ((off_t)sqe->off != (off_t)-1)
			return (false);
		if (kqring_op_readv(sqe->opcode) ||
		    kqring_op_writev(sqe->opcode)) {
			const struct iovec *iov = op->args;

			want = 0;
			for (u_int i = 0; i < sqe->len; i++)
				want += iov[i].iov_len;
		}
		if (want == 0 && !is_write &&
		    (sqe->flags & IOSQE_BUFFER_SELECT) == 0)
			return (false);
		if (op->ffile != NULL)
			return (kqring_pipe_will_block(op->ffile->fp, is_write,
			    want));
		if (fget(td, sqe->fd, &cap_no_rights, &fp) != 0)
			return (false);
		block = kqring_pipe_will_block(fp, is_write, want);
		fdrop(fp, td);
		return (block);
	case IORING_OP_WAITID:
		return ((sqe->file_index & WNOHANG) == 0);
	default:
		return (false);
	}
}

static int
kqring_op_wq_type(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	bool vreg;

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		if (op->ffile != NULL) {
			fp = op->ffile->fp;
			vreg = fp->f_type == DTYPE_VNODE &&
			    fp->f_vnode != NULL && fp->f_vnode->v_type == VREG;
			return (vreg ? IO_WQ_BOUND : IO_WQ_UNBOUND);
		}
		if (fget(td, sqe->fd, &cap_no_rights, &fp) != 0)
			return (IO_WQ_BOUND);
		vreg = fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
		    fp->f_vnode->v_type == VREG;
		fdrop(fp, td);
		return (vreg ? IO_WQ_BOUND : IO_WQ_UNBOUND);
	case IORING_OP_WAITID:
		return (IO_WQ_UNBOUND);
	default:
		return (IO_WQ_BOUND);
	}
}

static bool
kqring_wq_can_wait_op_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->cancelled || op->wq_done || op->wq_blocking || ctx->closing)
		return (false);
	if (op->wq_running) {
		if (op->wq_td != NULL) {
			bool abortable;

			thread_lock(op->wq_td);
			abortable = TD_CAN_ABORT(op->wq_td);
			thread_unlock(op->wq_td);
			if (abortable) {
				op->wq_blocking = true;
				return (false);
			}
		}
		return (true);
	}
	if (op->wq_queued &&
	    ctx->wq_pending[op->wq_type] + ctx->wq_busy[op->wq_type] <=
	    ctx->wq_total[op->wq_type])
		return (true);
	op->wq_blocking = true;
	return (false);
}

static bool
kqring_wq_enqueue(struct kqring_ctx *ctx, struct kqring_op *op, int type)
{
	struct thread *td = curthread;
	bool need_spawn = false;

	if (op->cred == NULL)
		op->cred = crhold(td->td_ucred);
	op->wq_type = (uint8_t)type;
	op->wq_blocking = kqring_op_will_block(op);

	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	op->wq_canceled = false;
	op->wq_done = false;
	op->wq_queued = true;
	kqring_op_arm_locked(ctx, op);
	TAILQ_INSERT_TAIL(&ctx->wq_ops[type], op, wq_link);
	ctx->wq_pending[type]++;
	ctx->wq_inflight++;
	if (ctx->wq_total[type] < ctx->wq_pending[type] + ctx->wq_busy[type] &&
	    ctx->wq_total[type] < ctx->wq_max[type]) {
		ctx->wq_total[type]++;
		need_spawn = true;
	}
	wakeup_one(&ctx->wq_ops[type]);
	if (need_spawn) {
		mtx_unlock(&ctx->mtx);
		if (kthread_add(kqring_wq_thread, &ctx->wq_arg[type],
		    td->td_proc, NULL, 0, 0, "kqwq%c",
		    type == IO_WQ_BOUND ? 'b' : 'u') != 0) {
			mtx_lock(&ctx->mtx);
			ctx->wq_total[type]--;
		} else {
			mtx_lock(&ctx->mtx);
		}
	}
	while (kqring_wq_can_wait_op_locked(ctx, op)) {
		int error;

		ctx->sleeping = true;
		error = msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
		    "kqwqsy", 100 * SBT_1US, 0, 0);
		ctx->sleeping = false;
		if ((error == EINTR || error == ERESTART) && !op->wq_done) {
			op->wq_blocking = true;
			break;
		}
	}
	if (op->wq_done && !op->cancelled && !ctx->closing) {
		int32_t res;

		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		res = op->res;
		if (op->wq_fp != NULL) {
			struct file *fp = op->wq_fp;

			op->wq_fp = NULL;
			res = kqring_fixed_install(ctx, fp, &op->wq_caps,
			    op->sqe.file_index);
		}
		kqring_req_complete(ctx, op, res, op->cflags);
		return (false);
	}
	mtx_unlock(&ctx->mtx);
	return (true);
}

static bool
kqring_op_use_wq(const struct io_uring_sqe *sqe)
{
	switch (sqe->opcode) {
	case IORING_OP_FSYNC:
	case IORING_OP_SYNC_FILE_RANGE:
	case IORING_OP_FALLOCATE:
	case IORING_OP_FTRUNCATE:
	case IORING_OP_FADVISE:
	case IORING_OP_MADVISE:
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
	case IORING_OP_STATX:
	case IORING_OP_RENAMEAT:
	case IORING_OP_UNLINKAT:
	case IORING_OP_MKDIRAT:
	case IORING_OP_SYMLINKAT:
	case IORING_OP_LINKAT:
	case IORING_OP_FSETXATTR:
	case IORING_OP_SETXATTR:
	case IORING_OP_FGETXATTR:
	case IORING_OP_GETXATTR:
		return (true);
	case IORING_OP_FILES_UPDATE:
		return (false);
	case IORING_OP_CLOSE:
	case IORING_OP_PIPE:
		if (sqe->file_index != 0)
			return (false);
		return ((sqe->flags & IOSQE_ASYNC) != 0);
	default:
		return ((sqe->flags & IOSQE_ASYNC) != 0);
	}
}

static void kqring_req_issue_cred(struct kqring_ctx *ctx,
    struct kqring_op *op);

/*
 * Issue a request: run it inline, or arm it to finish later from
 * taskwork.  Every path ends in kqring_req_complete() or an armed op.
 */
static void
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
		/*
		 * A socket would block the ring owner in read(2); treat it like
		 * RECV / SEND instead, so it can be armed.  Other descriptors
		 * (files, pipes, devices) run on the in-process worker pool.
		 */
		if (!kqring_op_is_socket(op)) {
			armed = kqring_wq_enqueue(ctx, op,
			    kqring_op_wq_type(op));
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
		kqring_req_complete(ctx, op, kqring_msg_ring(ctx, op), 0);
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
 * Run one queued taskwork item: a worker completion, a socket that became
 * ready, a woken futex, or an expired timer.
 */
static void
kqring_taskwork_run_one(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (op->wq_done) {
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
		if (!rc->skip_cqe)
			kqring_post_cqe(ctx, rc->cqe.user_data, rc->cqe.res,
			    rc->cqe.flags);
		kqring_rcqe_free(rc);
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
	struct kqring_cred *kc;

	/* closing stops upcalls, workers and callouts from queueing taskwork. */
	mtx_lock(&ctx->mtx);
	ctx->closing = true;
	while ((op = TAILQ_FIRST(&ctx->active_ops)) != NULL) {
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_destroy(ctx, op);
		mtx_lock(&ctx->mtx);
	}
	for (int t = 0; t < KQRING_WQ_TYPES; t++) {
		wakeup(&ctx->wq_ops[t]);
		while (ctx->wq_total[t] > 0)
			msleep(&ctx->wq_total[t], &ctx->mtx, 0, "kqwqdr", 0);
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

	/* Every request (and its references) is gone: drop the tables. */
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

	kqring_free_shm(ctx->ring_obj, ctx->ring_kva, ctx->ring_alloc_len);
	kqring_free_shm(ctx->sqes_obj, ctx->sqes_kva, ctx->sqes_alloc_len);
	if (ctx->wq_base_cpuset != NULL)
		cpuset_rel(ctx->wq_base_cpuset);
	sx_destroy(&ctx->bg_sx);
	mtx_destroy(&ctx->mtx);
	free(ctx, M_KQRING);
}

static int
kqring_open(struct cdev *dev __unused, int oflags __unused, int devtype __unused,
    struct thread *td)
{
	struct kqring_ctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_KQRING, M_WAITOK | M_ZERO);
	mtx_init(&ctx->mtx, "kqring", NULL, MTX_DEF);
	sx_init(&ctx->bg_sx, "kqring bg");
	for (int t = 0; t < KQRING_WQ_TYPES; t++) {
		ctx->wq_arg[t].ctx = ctx;
		ctx->wq_arg[t].type = t;
		TAILQ_INIT(&ctx->wq_ops[t]);
	}
	ctx->wq_max[IO_WQ_BOUND] = MIN((uint32_t)mp_ncpus * 4,
	    KQRING_WQ_MAX_WORKERS);
	ctx->wq_max[IO_WQ_UNBOUND] = KQRING_WQ_MAX_WORKERS;
	ctx->wq_base_cpuset = cpuset_ref(td->td_cpuset);
	TAILQ_INIT(&ctx->active_ops);
	TAILQ_INIT(&ctx->taskwork_ops);
	TAILQ_INIT(&ctx->free_ops);
	TAILQ_INIT(&ctx->ready_q);
	TAILQ_INIT(&ctx->defer_q);
	STAILQ_INIT(&ctx->ocq);
	STAILQ_INIT(&ctx->rcq);
	for (int i = 0; i < KQRING_BGROUP_HASH; i++)
		LIST_INIT(&ctx->bgroups[i]);
	LIST_INIT(&ctx->creds);
	ctx->clockid = CLOCK_MONOTONIC;

	error = devfs_set_cdevpriv(ctx, kqring_dtor);
	if (error != 0) {
		if (ctx->wq_base_cpuset != NULL)
			cpuset_rel(ctx->wq_base_cpuset);
		sx_destroy(&ctx->bg_sx);
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
	    IORING_FEAT_LINKED_FILE | IORING_FEAT_NATIVE_WORKERS |
	    IORING_FEAT_CUR_PERSONALITY | IORING_FEAT_RSRC_TAGS;

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
		    op->wq_queued || op->wq_running || op->wq_done ||
		    kqring_exec_waitid(op, true) == EAGAIN)
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
	/* CQEs posted inline here are not "async" (EVENTFD_ASYNC). */
	ctx->submitting = true;
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
	ctx->submitting = false;
	atomic_store_rel_32(&shm->sq_head, head);
	ea->SUBMITTED_out = submitted;

	/* 3. Run pending DEFER_TASKRUN completions, futex and timeout checks */
	kqring_run_taskwork(ctx);
	kqring_run_checks(ctx);

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
	error = kqring_wait_cqes(ctx, ea->min_complete, has_timeout, sbt);
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

		/* Tell an eventfd watcher about CQEs before sleeping. */
		kqring_evfd_flush(ctx);
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

/* ============================================================================
 * Registration (IORING_REGISTER_*)
 * ============================================================================
 */

/* Release the fixed file table (UNREGISTER_FILES, teardown). */
static void
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
static void
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
static void
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
static int
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
static int
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
static int32_t
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
 * thread); registered buffers are plain ranges, so a copy shares nothing.
 */
static int
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
	/* There are no registered ring descriptors (SRC_REGISTERED). */
	if ((cb.flags & ~IORING_REGISTER_DST_REPLACE) != 0 ||
	    cb.pad[0] != 0 || cb.pad[1] != 0 || cb.pad[2] != 0)
		return (EINVAL);
	if (ctx->bufs != NULL &&
	    (cb.flags & IORING_REGISTER_DST_REPLACE) == 0)
		return (EBUSY);
	error = fget(td, (int)cb.src_fd, cap_rights_init_one(&rights,
	    CAP_IOCTL), &fp);
	if (error != 0)
		return (error);
	error = kqring_fp_ctx(fp, &sctx);
	if (error == 0 && sctx == ctx)
		error = EINVAL;
	else if (error == 0 && sctx->owner_td != td)
		error = EEXIST;
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
static int
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
	}
	/* An empty buffer list may become a ring, as in io_uring. */
	sx_xlock(&ctx->bg_sx);
	old = kqring_bgroup_find(ctx, reg.bgid);
	if (old != NULL && (old->is_ring || old->nbufs != 0)) {
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

static int
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
	mtx_lock(&ctx->mtx);
	LIST_REMOVE(bg, link);
	mtx_unlock(&ctx->mtx);
	kqring_bgroup_free(ctx, bg);
	sx_xunlock(&ctx->bg_sx);
	return (0);
}

/* IORING_REGISTER_PBUF_STATUS: a ring's kernel head. */
static int
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
 * IORING_REGISTER_PERSONALITY: register this thread's credentials, for
 * requests to name with sqe->personality.  Ids are 1..65535, cyclic.
 */
static int
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
static int
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
 * Signal the eventfd if CQEs were posted since the last signal; the owner
 * calls this at the end of each enter and before it sleeps.
 */
static void
kqring_evfd_flush(struct kqring_ctx *ctx)
{
	if (ctx->evfd_cqes == 0 || ctx->evfd == NULL)
		return;
	ctx->evfd_cqes = 0;
	mtx_lock(&ctx->mtx);
	kqring_evfd_signal_locked(ctx);
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
 * IORING_REGISTER_SYNC_CANCEL: ASYNC_CANCEL without an SQE.  kqring
 * cancels armed requests at once, so there is never anything to wait for
 * and the timeout does not matter.
 */
static int
kqring_sync_cancel(struct kqring_ctx *ctx,
    const struct kqring_register_arg *ra, int *resp)
{
	struct io_uring_sync_cancel_reg sc;
	struct io_uring_sqe sqe;
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
	bzero(&sqe, sizeof(sqe));
	sqe.opcode = IORING_OP_ASYNC_CANCEL;
	sqe.cancel_flags = sc.flags;
	sqe.fd = sc.fd;
	sqe.addr = sc.addr;
	sqe.len = sc.opcode;
	res = kqring_cancel(ctx, &sqe);
	/* Issue the link successors that cancellation released. */
	kqring_flush_ready(ctx);
	if (res < 0)
		return (-res);
	*resp = res;
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
 * IORING_REGISTER_IOWQ_AFF / IORING_UNREGISTER_IOWQ_AFF: pin in-process
 * worker threads to a subset of the process's allowed CPUs, or restore
 * the process cpuset.
 */
static int
kqring_iowq_aff(struct kqring_ctx *ctx, const struct kqring_register_arg *ra,
    bool set)
{
	cpuset_t mask;
	size_t len;
	int error;

	if (!set) {
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		mtx_lock(&ctx->mtx);
		ctx->wq_aff_set = false;
		CPU_ZERO(&ctx->wq_cpumask);
		ctx->wq_aff_gen++;
		wakeup(&ctx->wq_ops[IO_WQ_BOUND]);
		wakeup(&ctx->wq_ops[IO_WQ_UNBOUND]);
		mtx_unlock(&ctx->mtx);
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
	if (ctx->wq_base_cpuset != NULL)
		CPU_AND(&mask, &mask, &ctx->wq_base_cpuset->cs_mask);
	else
		CPU_AND(&mask, &mask, &all_cpus);
	if (CPU_EMPTY(&mask))
		return (EINVAL);
	mtx_lock(&ctx->mtx);
	CPU_COPY(&mask, &ctx->wq_cpumask);
	ctx->wq_aff_set = true;
	ctx->wq_aff_gen++;
	wakeup(&ctx->wq_ops[IO_WQ_BOUND]);
	wakeup(&ctx->wq_ops[IO_WQ_UNBOUND]);
	mtx_unlock(&ctx->mtx);
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
	uint32_t vals[KQRING_WQ_TYPES];
	int error;

	if (ra->nr_args != KQRING_WQ_TYPES)
		return (EINVAL);
	if (ra->arg == NULL)
		return (EFAULT);
	error = copyin(ra->arg, vals, sizeof(vals));
	if (error != 0)
		return (error);
	if (vals[IO_WQ_BOUND] > INT32_MAX || vals[IO_WQ_UNBOUND] > INT32_MAX)
		return (EINVAL);
	mtx_lock(&ctx->mtx);
	for (int t = 0; t < KQRING_WQ_TYPES; t++) {
		if (vals[t] != 0) {
			ctx->wq_max[t] = MIN(vals[t], KQRING_WQ_MAX_WORKERS);
			if (ctx->wq_total[t] > ctx->wq_max[t])
				wakeup(&ctx->wq_ops[t]);
		}
		vals[t] = ctx->wq_max[t];
	}
	mtx_unlock(&ctx->mtx);
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
	case IORING_REGISTER_ENABLE_RINGS:
		if (ra->arg != NULL || ra->nr_args != 0)
			return (EINVAL);
		mtx_lock(&ctx->mtx);
		if (ctx->enabled) {
			mtx_unlock(&ctx->mtx);
			return (EINVAL);
		}
		ctx->owner_td = td;
		ctx->enabled = true;
		ctx->restricted = ctx->rst.registered;
		mtx_unlock(&ctx->mtx);
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

	default:
		/*
		 * RING_FDS (no io_uring_enter(2) to take a registered ring),
		 * NAPI.
		 */
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
static int
kqring_ioc_register(struct kqring_ctx *ctx, struct kqring_register_arg *ra,
    struct thread *td, int *resp)
{
	bool busy = false;
	int error;

	if (!ctx->setup_done)
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
	if (ctx->enabled) {
		if (ctx->owner_td != td) {
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

	error = kqring_register_op(ctx, ra, td, resp);

	if (busy) {
		mtx_lock(&ctx->mtx);
		ctx->reg_busy = false;
		mtx_unlock(&ctx->mtx);
	}
	return (error);
}

static int
kqring_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	struct kqring_ctx *ctx;
	int error, res = 0;

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
		    (struct kqring_register_arg *)data, td, &res);
		break;
	default:
		return (ENOTTY);
	}
	/*
	 * kern_*() calls made for SQEs (openat, accept, socket...) leave
	 * their result in td_retval[0], which is ioctl(2)'s return value:
	 * only a register result may be returned.
	 */
	td->td_retval[0] = error == 0 ? res : 0;
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
