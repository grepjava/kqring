/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring.c: FreeBSD Kernel Module implementing the SINGLE_ISSUER |
 * DEFER_TASKRUN io_uring subset + Provided Buffer Rings (PBUF_RING).
 *
 * Architectural Summary:
 *   - Exposes /dev/kqring with per-descriptor state via devfs_set_cdevpriv(9).
 *   - Maps SQ/CQ rings and SQE arrays via .d_mmap_single backed by wired
 *     anonymous VM objects (vm_pager_allocate + pmap_qenter). Each mapping
 *     retains a VM object reference across close(2).
 *   - SINGLE_ISSUER restricts userspace submission and registration.
 *     Normally the issuer drains taskwork, changes tables and posts CQEs.
 *     SQPOLL adds an in-process submission kthread; sq_sx serializes it with
 *     enter and registration. Worker kthreads share the process address
 *     space and descriptor table, execute I/O with held credentials, and
 *     queue results for the serialized issuer to complete.
 *   - Socket readiness callbacks never access user I/O buffers or provided
 *     buffer groups. They queue taskwork, update shared notification flags
 *     and wake the issuer. Workers reserve a provided group's head under
 *     bg_sx, release the lock for blocking I/O, then commit bytes transferred.
 *     Other selections of that group fail ENOBUFS while it is reserved;
 *     removal/unregister fails EBUSY. Other groups remain usable.
 *   - Cancellation requests an interrupt and defers completion/resource
 *     release until a running worker returns. Ring close detaches running
 *     workers and defers final context/pool deallocation until the last
 *     worker returns; module unload drains any remaining workers.
 *   - On EOF (bytes == 0), a selected PBUF_RING buffer is not consumed.
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
#include <sys/rmlock.h>
#include <sys/sx.h>
#include <sys/condvar.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/priv.h>
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
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/extattr.h>
#include <sys/limits.h>
#include <sys/mbuf.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/selinfo.h>
#include <sys/sf_buf.h>
#include <sys/pipe.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <netinet/in.h>
#include <net/vnet.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/refcount.h>
#include <sys/resourcevar.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/umtxvar.h>
#include <sys/vmmeter.h>
#include <sys/sysctl.h>
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
#include <vm/vm_pageout.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>

#include <sys/kqring.h>


#ifndef _KQRING_VAR_H_
#define _KQRING_VAR_H_

MALLOC_DECLARE(M_KQRING);
/* posix_fadvise(2) state hung off struct file (freed by the kernel). */
MALLOC_DECLARE(M_FADVISE);
MALLOC_DECLARE(M_SELFD);

#define KQRING_MAX_SQ_ENTRIES	32768U	/* IORING_MAX_ENTRIES */
#define KQRING_MAX_CQ_ENTRIES	65536U	/* IORING_MAX_CQ_ENTRIES */
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
/* Live requests (armed, ready, deferred, or linked) per ring. */
#define KQRING_MAX_REQS		32768U
/* Registered resources, with io_uring's limits. */
#define KQRING_MAX_FILES	(1U << 20)	/* also RLIMIT_NOFILE */
#define KQRING_MAX_BUFS		(1U << 14)
#define KQRING_MAX_BUF_LEN	(1ULL << 30)
/* Zero-copy registered buffer limits (Phase A SEND_ZC + FIXED_BUF). */
#define KQRING_ZC_MIN_LEN	PAGE_SIZE
/*
 * On AF_LOCAL a copy is one memcpy into the peer's receive buffer, cheaper
 * than pinning an unregistered buffer for each send plus a deferred
 * notification until sends are about this large (see BENCHMARK.MD), so
 * smaller SEND_ZC / SENDMSG_ZC from unregistered buffers copy there.  A
 * registered buffer's pages are held already: it goes zero-copy from one
 * page.  The default for sysctl kern.kqring.zc_local_min_len.
 */
#define KQRING_ZC_LOCAL_MIN_LEN	(32U * 1024)
#define KQRING_MAX_ZC_BUF_LEN	(16U * 1024 * 1024)
/*
 * Disk-device I/O sent straight to the driver (kqring_file_bio()): at most
 * this many iovecs per request, and this much user memory held per ring
 * by requests in flight; beyond either, the request runs on a worker.
 */
#define KQRING_BIO_MAX_IOV	16
/* The longest busy-wait IORING_REGISTER_NAPI sets (microseconds). */
#define KQRING_MAX_BUSY_POLL_US	100000
#define KQRING_BIO_MAX_HELD	(64UL * 1024 * 1024)
#define KQRING_MAX_ZC_PAGES	16384U
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
/* Buckets of armed non-socket polls, by descriptor. */
#define KQRING_KP_HASH		256
/* Buckets of armed POLL_ADDs, by user_data (POLL_REMOVE's lookup). */
#define KQRING_POLL_HASH	256
/* In-process worker pools: IO_WQ_BOUND (0) and IO_WQ_UNBOUND (1). */
#define KQRING_WQ_TYPES		2
#define KQRING_WQ_MAX_WORKERS	256U
#define KQRING_WQ_IDLE_SBT	(5 * SBT_1S)

/* io_uring_params flags accepted by KQRING_IOC_SETUP. */
#define	KQRING_SETUP_FLAGS						\
	(IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL |			\
	IORING_SETUP_SQ_AFF | IORING_SETUP_CQSIZE |			\
	IORING_SETUP_CLAMP | IORING_SETUP_ATTACH_WQ |			\
	IORING_SETUP_R_DISABLED | IORING_SETUP_SUBMIT_ALL |		\
	IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |		\
	IORING_SETUP_SQE128 | IORING_SETUP_CQE32 |			\
	IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN)

/* KQRING_IOC_ENTER flags implemented. */
#define	KQRING_ENTER_FLAGS						\
	(IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP |		\
	IORING_ENTER_SQ_WAIT | IORING_ENTER_EXT_ARG |			\
	IORING_ENTER_REGISTERED_RING)

#define	KQRING_MAX_REG_RINGS	16U
#define	KQRING_PROC_HASH	64

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
	/* The waiter lists op is on: so_rcv (or the listener), so_snd. */
	struct kqring_sowatch	*sw[2];
	TAILQ_ENTRY(kqring_op)	sw_link[2];
	/*
	 * POLL_ADD on a non-socket file, as knotes on the ring's kqueue
	 * (kqring_kpoll.c): in ctx->kp_ops[] by fd while kp_on, with the events
	 * the poller saw since the last delivery (under ctx->mtx).
	 */
	TAILQ_ENTRY(kqring_op)	kp_link;
	/* An armed POLL_ADD: in ctx->poll_hash[] by user_data. */
	LIST_ENTRY(kqring_op)	poll_hlink;
	uint64_t		kp_id;		/* knotes' udata */
	int			kp_fd;		/* their ident */
	uint8_t			kp_filters;	/* KQRING_KP_READ / _WRITE */
	const void		*kp_data;	/* its file's f_data: only compared */
	bool			kp_on;
	int			kp_revents;
	u_int			kp_nev;
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
	bool			wq_retired;	/* awaiting worker release */
	bool			wq_complete_pending;
	int32_t			wq_cancel_res;
	uint32_t		wq_cancel_flags;
	bool			wq_done;	/* worker finished */
	bool			wq_wait_ready;	/* worker waits for readiness */
	const struct file	*armed_fp;	/* non-socket armed file id */
	int			prep_err;	/* SQE rejected at submission */
	uint32_t		nchain;		/* chain head: requests in chain */
	int32_t			res;		/* result of the last attempt */
	uint32_t		cflags;		/* CQE flags of the last attempt */
	struct kqring_op	*link;		/* next request in the chain */
	struct kqring_op	*ltimeout;	/* LINK_TIMEOUT guarding this */
	struct kqring_op	*ltarget;	/* LINK_TIMEOUT: request guarded */
	/* TIMEOUT / LINK_TIMEOUT state (and callout below) */
	struct timespec		ts;
	/* Completion-count TIMEOUT: done once cq_seq - seq_start >= count. */
	uint32_t		seq_start;
	uint32_t		seq_count;
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
	struct kqring_rsrc	*ffile_in;
	struct kqring_rsrc	*fbuf;
	struct kqring_zc_notif	*zc_notif;
	struct ucred		*cred;
	struct file		*sock_fp;	/* socket held while retried */
	/*
	 * Block I/O on a disk device, sent straight to its driver
	 * (kqring_file_bio()): bios in flight, and their summed results
	 * (updated by the completions, which may run concurrently).
	 */
	struct file		*bio_fp;	/* the device, held until free */
	bool			bio;		/* sent as bios; not cancelable */
	bool			bio_done;	/* all bios done; res is set */
	u_int			bio_refs;	/* bios not yet completed */
	int			bio_errno;
	u_long			bio_bytes;
	size_t			bio_len;	/* bytes held in user pages */
	/*
	 * Not cleared when the op is reused (kqring_op_alloc()): the
	 * callout is initialized once timer_init is set, and the SQE copied
	 * in at submission (sqe_extra only on IORING_SETUP_SQE128 rings).
	 */
	struct callout		callout;	/* uses ctx->mtx */
	struct io_uring_sqe	sqe;
	uint8_t			sqe_extra[64];
};

TAILQ_HEAD(kqring_op_list, kqring_op);
LIST_HEAD(kqring_poll_head, kqring_op);

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

LIST_HEAD(kqring_fwait_list, kqring_fwait);
extern struct kqring_fwait_list kqring_futex_hash[KQRING_FUTEX_HASH];
extern struct mtx kqring_futex_mtx;

struct kqring_proc_rings {
	LIST_ENTRY(kqring_proc_rings) link;
	struct proc		*proc;
	struct file		*rings[KQRING_MAX_REG_RINGS];
	uint32_t		nrings;
};


LIST_HEAD(kqring_proc_list, kqring_proc_rings);
extern struct kqring_proc_list kqring_proc_hash[KQRING_PROC_HASH];
extern LIST_HEAD(kqring_ctx_list_head, kqring_ctx) kqring_ctx_list;
extern struct mtx kqring_proc_mtx;
/*
 * The registered-ring tables (kqring_proc_hash and each table's rings[]):
 * changed with kqring_proc_mtx and this write-locked, looked up on every
 * IORING_ENTER_REGISTERED_RING enter with it read-locked only.
 */
extern struct rmlock kqring_rings_rm;
extern uint32_t kqring_live_workers;
extern uint32_t kqring_live_zc;
extern bool kqring_unloading;

/*
 * A MSG_RING CQE queued for the target ring's owner to post.  For
 * IORING_MSG_SEND_FD it carries a file (referenced) that the owner first
 * installs in its fixed table, at file_index.
 */
struct kqring_rcqe {
	STAILQ_ENTRY(kqring_rcqe) link;
	struct file		*fp;
	struct filecaps		caps;
	uint32_t		file_index;
	bool			skip_cqe;
	bool			has_extra;
	struct io_uring_cqe	cqe;
	uint64_t		extra[2];
};

/*
 * A registered file or buffer: one slot of the fixed file table
 * (IORING_REGISTER_FILES, direct descriptors) or of the registered buffer
 * table.  The table holds one reference and each request using the slot
 * another, so replacing or unregistering a slot does not affect requests
 * already using it; once the last reference is gone the slot's tag CQE is
 * posted (IORING_FEAT_RSRC_TAGS), as in io_uring.  Owner thread only.
 *
 * Registered buffers hold their backing pages in struct kqring_zc_buf for
 * zero-copy SEND_ZC (IORING_RECVSEND_FIXED_BUF), charged against the
 * registering user's RLIMIT_MEMLOCK and vm.max_user_wired until both the
 * slot and any in-flight EXT_SFBUF mbufs release them.  READ_FIXED and
 * friends check that requests stay inside [addr, addr + len) and copy via
 * uiomove().
 */
struct kqring_pin_acct {
	LIST_ENTRY(kqring_pin_acct) link;
	struct uidinfo		*uip;
	rlim_t			bytes;
	uint32_t		refs;
};

/*
 * Pinned physical pages backing a registered buffer or an in-flight
 * unregistered zero-copy send (up to KQRING_MAX_ZC_BUF_LEN) for
 * SEND_ZC / SENDMSG_ZC, refcounted across the buffer table slot (if any)
 * and in-flight EXT_SFBUF mbufs.
 */
struct kqring_zc_buf {
	volatile u_int		refs;
	int			npages;
	vm_offset_t		page_base;
	struct kqring_pin_acct	*acct;
	vm_page_t		pages[];
};

/*
 * Per-send completion tracker for zero-copy EXT_SFBUF mbuf chains.  Each
 * attached page mbuf holds a reference; when the last reference drops, the
 * preallocated IORING_CQE_F_NOTIF CQE is queued on the ring.
 */
struct kqring_zc_notif {
	TAILQ_ENTRY(kqring_zc_notif) link;
	struct kqring_ctx	*ctx;
	struct kqring_zc_buf	*zbuf;
	struct kqring_zc_buf	**zbufs;
	u_int			nzbufs;
	struct kqring_rcqe	*rcqe;
	volatile u_int		refs;
	bool			on_ctx;
};

extern struct mtx kqring_zc_mtx;
SYSCTL_DECL(_kern_kqring);
extern u_int kqring_zc_local_min_len;

struct kqring_rsrc {
	uint32_t		refs;
	uint64_t		tag;
	struct file		*fp;		/* file slot */
	struct filecaps		caps;		/* its Capsicum rights */
	uint64_t		addr;		/* buffer slot */
	uint64_t		len;
	struct kqring_zc_buf	*zbuf;		/* held pages for SEND_ZC */
	uint32_t		cow_gen;	/* ctx->cow_gen at hold */
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
 * Issuer and workers serialize group access with bg_sx. The hash table
 * is also changed under ctx->mtx, because mmap(2) of a ring looks it up from any
 * thread.
 */
struct kqring_bgroup {
	LIST_ENTRY(kqring_bgroup) link;
	uint16_t		bgid;
	bool			is_ring;
	bool			busy;		/* worker reserves head, bg_sx */
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
	uint64_t		extra[2];
};

/*
 * In-process worker thread pool (kthread_add into curproc), refcounted
 * so rings in the same process can share it via IORING_SETUP_ATTACH_WQ.
 * Lock order: ctx->mtx -> wq->mtx.
 */
struct kqring_wq {
	struct mtx		mtx;
	uint32_t		refs;		/* rings + spawned workers */
	uint32_t		ring_refs;	/* open rings sharing pool */
	bool			closing;
	struct proc		*proc;
	struct {
		struct kqring_wq *wq;
		int		type;
	} arg[KQRING_WQ_TYPES];
	struct kqring_op_list	ops[KQRING_WQ_TYPES];
	uint32_t		max[KQRING_WQ_TYPES];
	uint32_t		total[KQRING_WQ_TYPES];
	uint32_t		busy[KQRING_WQ_TYPES];
	uint32_t		pending[KQRING_WQ_TYPES];
	struct cpuset		*base_cpuset;
	cpuset_t		cpumask;
	bool			aff_set;
	uint32_t		aff_gen;
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
	LIST_ENTRY(kqring_ctx)	proc_link;	/* kqring_ctx_list; proc_mtx */
	struct mtx		mtx;
	volatile u_int		refs;		/* open fd + running workers */
	bool			setup_busy;	/* KQRING_IOC_SETUP running */
	bool			setup_done;
	bool			enabled;
	bool			closing;
	bool			cancelling;
	bool			cancelled;
	bool			in_ctx_list;
	struct thread		*cancel_td;
	struct proc		*owner_proc;	/* SINGLE_ISSUER process */
	lwpid_t			owner_tid;	/* SINGLE_ISSUER thread ID */
	bool			shared;		/* no SINGLE_ISSUER: any thread */

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

	/* In-process worker thread pool (shared with IORING_SETUP_ATTACH_WQ). */
	struct kqring_wq	*wq;

	/* IORING_SETUP_SQPOLL kernel thread and submission lock. */
	struct sx		sq_sx;
	struct thread		*sqpoll_td;
	sbintime_t		sqpoll_idle_sbt;
	uint32_t		sqpoll_cpu;
	bool			sqpoll_running;
	bool			sqpoll_waiting;
	int			sqpoll_wait_chan;
	/*
	 * No DEFER_TASKRUN (and no SQPOLL): the same kthread runs only
	 * taskwork, so that completions appear without an enter, as for an
	 * io_uring ring whose task_work runs on its own.
	 */
	bool			taskrun_only;

	/*
	 * Non-socket POLL_ADD (kqring_kpoll.c): a kqueue with no descriptor,
	 * and a kthread blocked in kevent(2) on it.
	 */
	struct file		*kp_fp;
	struct thread		*kp_td;
	bool			kp_running;
	uint64_t		kp_next_id;
	TAILQ_HEAD(, kqring_op)	kp_ops[KQRING_KP_HASH];

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
	volatile u_int		cow_gen;	/* bumped on fork() */
	TAILQ_HEAD(, kqring_zc_notif) zc_pending;

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
	/*
	 * poll(2) / select(2) / kevent(2) on the ring descriptor: readable
	 * while CQEs or taskwork wait (kqring_ring_readable()), woken with
	 * the eventfd.  The knote list's lock is mtx.
	 */
	struct selinfo		rsel;
	bool			pollers;	/* someone polls or kevents */
	/* TIMEOUTs completed; any one ends kqring_wait_cqes(), as Linux. */
	volatile uint32_t	cq_timeouts;

	struct kqring_restrictions rst;
	bool			restricted;	/* rst in force */
	uint32_t		clockid;	/* IORING_REGISTER_CLOCK */
	/* IORING_REGISTER_NAPI: busy-wait before sleeping (0: never) */
	sbintime_t		busy_poll_sbt;
	uint32_t		busy_poll_us;
	uint8_t			napi_prefer;
	uint8_t			napi_track;
	bool			reg_busy;	/* register on a disabled ring */

	/* Armed operations and deferred taskwork queue */
	struct callout		wq_abort_callout;
	bool			wq_abort_pending;
	struct kqring_op_list	retired_ops;	/* canceled workers still live */
	struct kqring_op_list	active_ops;
	struct kqring_poll_head	poll_hash[KQRING_POLL_HASH];
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
	/*
	 * kqring_file_bio() requests in flight and the user memory they
	 * hold, under mtx.  Teardown waits for nbio to reach 0.
	 */
	uint32_t		nbio;
	size_t			bio_held;
	int			wakeup_chan;
	u_int			sleeping;	/* threads in msleep; under mtx */
	/*
	 * Recycled ops (linked via active_link).  Only touched by the owner
	 * thread (enter) and by kqring_dtor(), so no lock is needed.
	 */
	struct kqring_op_list	free_ops;
	int			nfree;
};

#define	KQRING_OP_CACHE_MAX	32


struct kqring_msg_args {
	struct msghdr	msg;		/* msg_iov -> iov[] */
	uint8_t		name[SOCK_MAXADDRLEN];	/* SENDMSG: msg_name */
	struct iovec	iov[];		/* then SENDMSG control data */
};

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

struct selfd {
	STAILQ_ENTRY(selfd)	sf_link;
	TAILQ_ENTRY(selfd)	sf_threads;
	struct selinfo		*sf_si;
	struct mtx		*sf_mtx;
	struct seltd		*sf_td;
	void			*sf_cookie;
};

struct seltd {
	STAILQ_HEAD(, selfd)	st_selq;
	struct selfd		*st_free1;
	struct selfd		*st_free2;
	struct mtx		st_mtx;
	struct cv		st_wait;
	int			st_flags;
};

#define	KQRING_SELTD_PENDING	0x0001
#define	KQRING_SELTD_RESCAN	0x0002


/* ============================================================================
 * Hot-Path Inline Helpers
 * ============================================================================
 */

static inline void
kqring_set_owner(struct kqring_ctx *ctx, struct thread *td)
{
	ctx->owner_proc = td->td_proc;
	ctx->owner_tid = td->td_tid;
}

static inline void
kqring_clear_owner(struct kqring_ctx *ctx)
{
	ctx->owner_proc = NULL;
	ctx->owner_tid = 0;
}

/*
 * Whether td may submit to, wait on and register with ctx: its owner
 * thread, or for a shared ring (no SINGLE_ISSUER) any thread of the
 * owner process.  Shared rings serialize those threads with sq_sx.
 */
static inline bool
kqring_is_owner(const struct kqring_ctx *ctx, const struct thread *td)
{
	return (ctx->owner_proc == td->td_proc &&
	    (ctx->shared || ctx->owner_tid == td->td_tid));
}

/* Ops whose sqe->msg_flags are send(2)/recv(2) flags. */
static inline bool
kqring_op_msg_flags_ok(uint8_t opcode)
{
	switch (opcode) {
	case IORING_OP_RECV:
	case IORING_OP_RECVMSG:
	case IORING_OP_SEND:
	case IORING_OP_SENDMSG:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG_ZC:
		return (true);
	default:
		return (false);
	}
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

/* READ / WRITE and their vectored and _FIXED forms: whether a write. */
static inline bool
kqring_op_is_write(uint8_t opcode)
{
	return (opcode == IORING_OP_WRITE || opcode == IORING_OP_WRITEV ||
	    opcode == IORING_OP_WRITE_FIXED || opcode == IORING_OP_WRITEV_FIXED);
}

static inline struct kqring_bgroup_list *
kqring_bgroup_bucket(struct kqring_ctx *ctx, uint16_t bgid)
{
	return (&ctx->bgroups[bgid % KQRING_BGROUP_HASH]);
}

/*
 * Signal the registered eventfd, unless userspace set
 * IORING_CQ_EVENTFD_DISABLED.  ctx->mtx keeps the eventfd registered.
 */
static inline void
kqring_evfd_signal_locked(struct kqring_ctx *ctx)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	if (ctx->evfd != NULL && ctx->shm != NULL &&
	    (atomic_load_32(&ctx->shm->cq_flags) &
	    IORING_CQ_EVENTFD_DISABLED) == 0)
		eventfd_signal(ctx->evfd);
	/* And whoever polls the ring descriptor itself. */
	if (ctx->pollers) {
		selwakeuppri(&ctx->rsel, PZERO);
		KNOTE_LOCKED(&ctx->rsel.si_note, 0);
	}
}

/* No room for another CQE in the CQ ring (it would overflow). */
static inline bool
kqring_cq_full(const struct kqring_ctx *ctx)
{
	const struct kqring_shm_header *shm = ctx->shm;

	return (!STAILQ_EMPTY(&ctx->ocq) || atomic_load_acq_32(&shm->cq_tail) -
	    atomic_load_acq_32(&shm->cq_head) >= ctx->cq_entries);
}

/*
 * The ring descriptor is readable: CQEs to reap, an overflow to flush, or
 * taskwork that an enter would run (as io_uring_poll() reports
 * DEFER_TASKRUN work).
 */
static inline bool
kqring_ring_readable(const struct kqring_ctx *ctx)
{
	const struct kqring_shm_header *shm = ctx->shm;

	if (shm == NULL)
		return (false);
	return (atomic_load_acq_32(&shm->cq_tail) !=
	    atomic_load_acq_32(&shm->cq_head) || !STAILQ_EMPTY(&ctx->ocq) ||
	    !TAILQ_EMPTY(&ctx->taskwork_ops) || !STAILQ_EMPTY(&ctx->rcq));
}


static inline void
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

static inline struct sockbuf *
kqring_sb(struct socket *so, int which)
{
	return (which == SO_RCV ? &so->so_rcv : &so->so_snd);
}

/*
 * Drop a soref().  Ours may be the last reference (the descriptor was
 * closed while the op was armed), and then sofree() runs the protocol's
 * detach, which needs the socket's vnet (TCP counts its states per vnet);
 * ring teardown and taskwork have none set.
 */
static inline void
kqring_sorele(struct socket *so)
{
	CURVNET_SET(so->so_vnet);
	sorele(so);
	CURVNET_RESTORE();
}

/*
 * The ops waiting on one socket buffer, or on a listening socket's queue,
 * installed as the upcall argument: a socket has one upcall slot per
 * buffer, but any number of requests (on any rings) may wait on it, as in
 * io_uring.  Protected by that buffer's lock (the listen lock for a
 * listener); freed by the last op to leave.
 */
struct kqring_sowatch {
	TAILQ_HEAD(, kqring_op)	ops;
	int			idx;		/* op->sw[] / sw_link[] slot */
};

#define	KQRING_SW_IDX(which)	((which) == SO_RCV ? 0 : 1)

static inline struct kqring_poll_head *
kqring_poll_bucket(struct kqring_ctx *ctx, uint64_t user_data)
{
	return (&ctx->poll_hash[(u_int)(user_data ^ (user_data >> 32)) %
	    KQRING_POLL_HASH]);
}

static inline void
kqring_op_arm_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	op->armed = true;
	TAILQ_INSERT_TAIL(&ctx->active_ops, op, active_link);
	if (op->sqe.opcode == IORING_OP_POLL_ADD)
		LIST_INSERT_HEAD(kqring_poll_bucket(ctx, op->sqe.user_data), op,
		    poll_hlink);
}

static inline bool
kqring_buf_within(const struct kqring_rsrc *r, uint64_t addr, uint64_t len)
{
	return (addr >= r->addr && len <= r->len &&
	    addr - r->addr <= r->len - len);
}

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

static inline bool
kqring_so_stream(const struct socket *so)
{
	return (so->so_type == SOCK_STREAM || so->so_type == SOCK_SEQPACKET);
}

/* Unlocked hint: more is buffered (or EOF) after a receive. */
static inline bool
kqring_so_rcv_pending(struct socket *so, bool eof)
{
	return (!SOLISTENING(so) && (sbavail(&so->so_rcv) > 0 ||
	    (eof && (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0)));
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

static inline struct kqring_fwait_list *
kqring_futex_bucket(const struct umtx_key *key)
{
	return (&kqring_futex_hash[(u_int)key->hash % KQRING_FUTEX_HASH]);
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

static inline struct kqring_proc_list *
kqring_proc_bucket(const struct proc *p)
{
	return (&kqring_proc_hash[((uintptr_t)p >> 4) % KQRING_PROC_HASH]);
}

/* ============================================================================
 * Cross-Module Prototypes
 * ============================================================================
 */

/* kqring_dev.c */
struct kqring_proc_rings *kqring_proc_find_locked(const struct proc *p);
bool	kqring_fp_is_ring(const struct file *fp);
int	kqring_fp_ctx(struct file *fp, struct kqring_ctx **ctxp);
void	kqring_ctx_rele(struct kqring_ctx *ctx);
void	kqring_ctx_cancel_all(struct kqring_ctx *ctx);
void	kqring_poll_fallback_locked(struct kqring_ctx *ctx);
void	kqring_run_checks(struct kqring_ctx *ctx);
int	kqring_reg_ring_fget(struct thread *td, uint32_t slot, bool is_enter,
	    struct file **fpp);

/* kqring_req.c */
int	kqring_alloc_shm(size_t bytes, vm_object_t *obj_out,
	    vm_offset_t *kva_out, size_t *alloc_len_out);
void	kqring_free_shm(vm_object_t obj, vm_offset_t kva, size_t alloc_len);
void	kqring_flush_overflow(struct kqring_ctx *ctx);
void	kqring_post_cqe_extra(struct kqring_ctx *ctx, uint64_t user_data,
	    int32_t res, uint32_t flags, const uint64_t *extra);

static inline void
kqring_post_cqe_uncounted(struct kqring_ctx *ctx, uint64_t user_data,
    int32_t res, uint32_t flags)
{
	kqring_post_cqe_extra(ctx, user_data, res, flags, NULL);
}

static inline void
kqring_post_cqe(struct kqring_ctx *ctx, uint64_t user_data, int32_t res,
    uint32_t flags)
{
	ctx->cq_seq++;
	kqring_post_cqe_uncounted(ctx, user_data, res, flags);
}

void	kqring_op_disarm_locked(struct kqring_op *op);
void	kqring_op_unarm_locked(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_exec_sync(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_req_complete(struct kqring_ctx *ctx, struct kqring_op *op,
	    int32_t res, uint32_t cflags);
bool	kqring_op_is_socket(const struct kqring_op *op);
void	kqring_req_issue(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_flush_ready(struct kqring_ctx *ctx);
void	kqring_req_destroy(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_run_taskwork(struct kqring_ctx *ctx);
void	kqring_submit_sqes(struct kqring_ctx *ctx, uint32_t max_submit,
	    uint32_t *submitted_out);

/* kqring_rsrc.c */
struct kqring_bgroup *kqring_bgroup_find(struct kqring_ctx *ctx, uint16_t bgid);
int	kqring_buf_select(struct kqring_ctx *ctx, uint16_t bgid, size_t max,
	    struct iovec *iov, u_int nmax, struct kqring_sel *sel);
bool	kqring_buf_none(struct kqring_ctx *ctx, uint16_t bgid);
uint32_t kqring_buf_commit(struct kqring_ctx *ctx,
	    const struct kqring_sel *sel, size_t done);
void	kqring_rsrc_put(struct kqring_ctx *ctx, struct kqring_rsrc *r);
void	kqring_caps_all(struct filecaps *caps);
int32_t	kqring_fixed_install(struct kqring_ctx *ctx, struct file *fp,
	    struct filecaps *caps, uint32_t file_index);
int32_t	kqring_files_update(struct kqring_ctx *ctx, uint32_t off,
	    uint64_t ufds, uint64_t utags, uint32_t nr);
int32_t	kqring_files_update_alloc(struct kqring_ctx *ctx, uint64_t ufds,
	    uint32_t nr);

/* Take a reference to fixed slot `slot` for a request. */
static inline int
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

/*
 * Run a request's own work with the credentials sqe->personality names
 * (IORING_REGISTER_PERSONALITY).  Only this thread's td_ucred changes,
 * and only until kqring_cred_exit(); the swap holds its own reference,
 * as the request may be freed in between.
 */
static inline struct ucred *
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

static inline void
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

int	kqring_fixed_remove(struct kqring_ctx *ctx, uint32_t slot);
int	kqring_op_fget(const struct kqring_op *op, const cap_rights_t *rights,
	    u_int fflag, struct file **fpp);
int	kqring_op_fget_in(const struct kqring_op *op,
	    const cap_rights_t *rights, u_int fflag, struct file **fpp);
int	kqring_op_fget_caps(const struct kqring_op *op,
	    const cap_rights_t *rights, struct file **fpp,
	    struct filecaps *caps);
int32_t	kqring_exec_fixed_fd_install(const struct kqring_op *op);
int32_t	kqring_provide_buffers(struct kqring_ctx *ctx,
	    const struct io_uring_sqe *sqe);
int32_t	kqring_remove_buffers(struct kqring_ctx *ctx,
	    const struct io_uring_sqe *sqe);
void	kqring_files_free(struct kqring_ctx *ctx);
void	kqring_bufs_free(struct kqring_ctx *ctx);
void	kqring_bgroup_free(struct kqring_ctx *ctx, struct kqring_bgroup *bg);
int	kqring_files_register(struct kqring_ctx *ctx, uint64_t ufds,
	    uint64_t utags, uint32_t nr, bool sparse);
int	kqring_bufs_register(struct kqring_ctx *ctx, uint64_t uiov,
	    uint64_t utags, uint32_t nr, bool sparse);
int32_t	kqring_bufs_update(struct kqring_ctx *ctx, uint32_t off, uint64_t uiov,
	    uint64_t utags, uint32_t nr);
int	kqring_bufs_clone(struct kqring_ctx *ctx,
	    const struct kqring_register_arg *ra);
int	kqring_pbuf_register(struct kqring_ctx *ctx,
	    const struct kqring_register_arg *ra);
int	kqring_pbuf_unregister(struct kqring_ctx *ctx,
	    const struct kqring_register_arg *ra);
int	kqring_pbuf_status(struct kqring_ctx *ctx,
	    const struct kqring_register_arg *ra);
int	kqring_personality_register(struct kqring_ctx *ctx, int *idp);
int	kqring_personality_unregister(struct kqring_ctx *ctx, uint32_t id);

/* kqring_register.c */
void	kqring_evfd_flush(struct kqring_ctx *ctx);
int	kqring_ioc_register(struct kqring_ctx *ctx,
	    struct kqring_register_arg *ra, struct thread *td, int *resp);

/* kqring_sock.c */
void	kqring_sb_disarm(struct kqring_op *op, struct socket *so, int which);
void	kqring_listen_disarm(struct kqring_op *op, struct socket *so);
void	kqring_uio_init(struct uio *auio, struct iovec *iov, u_int iovcnt,
	    enum uio_rw rw);
int	kqring_exec_recv(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect, bool *more_out);
int	kqring_exec_send(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect);
int	kqring_exec_recvmsg(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect, bool *more_out);
int	kqring_exec_sendmsg(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect);
int	kqring_exec_connect(struct kqring_op *op, struct socket *expect);
int	kqring_exec_accept(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect, bool *more_out);
void	kqring_wq_poll_wait(struct kqring_ctx *ctx, struct kqring_op *op);
bool	kqring_wq_suspend_pending(void);
bool	kqring_wq_suspend_point(struct kqring_ctx *ctx);
int	kqring_exec_poll(struct kqring_op *op, struct socket *expect);
int32_t	kqring_exec_uring_cmd(const struct kqring_op *op);
int32_t	kqring_exec_bind(const struct kqring_op *op);
int32_t	kqring_exec_listen(const struct kqring_op *op);
int32_t	kqring_exec_shutdown(const struct kqring_op *op);
int32_t	kqring_exec_socket(struct kqring_ctx *ctx, struct kqring_op *op);
int	kqring_op_dirs(const struct io_uring_sqe *sqe);
bool	kqring_arm_socket_op(struct kqring_ctx *ctx, struct kqring_op *op,
	    int dirs);

/* kqring_file.c */
bool	kqring_pipe_will_block(const struct file *fp, bool is_write,
	    size_t want);
int	kqring_exec_file_rw(struct kqring_ctx *ctx, struct kqring_op *op,
	    bool is_write);
/* kqring_file_rw_inline() results. */
#define	KQRING_RW_PUNT	0	/* not run: give it to a worker */
#define	KQRING_RW_DONE	1	/* completed; op->res is set */
#define	KQRING_RW_BIO	2	/* sent to the disk driver; op is armed */
int	kqring_file_rw_inline(struct kqring_ctx *ctx, struct kqring_op *op,
	    bool is_write);
bool	kqring_file_bio_try(struct kqring_ctx *ctx, struct kqring_op *op,
	    bool is_write);
int	kqring_exec_read_multishot(struct kqring_ctx *ctx, struct kqring_op *op,
	    struct socket *expect, bool *more_out);
int32_t	kqring_exec_splice(struct kqring_ctx *ctx, struct kqring_op *op);
int32_t	kqring_exec_tee(struct kqring_ctx *ctx, struct kqring_op *op);
int32_t	kqring_exec_statx(struct kqring_op *op);
int32_t	kqring_exec_setxattr(struct kqring_op *op);
int32_t	kqring_exec_getxattr(struct kqring_op *op);
int32_t	kqring_exec_fallocate(const struct kqring_op *op);
int32_t	kqring_exec_fsync(const struct kqring_op *op, bool fullsync);
int32_t	kqring_exec_ftruncate(const struct kqring_op *op);
int32_t	kqring_exec_fadvise(const struct kqring_op *op);
int32_t	kqring_exec_close(struct kqring_ctx *ctx,
	    const struct io_uring_sqe *sqe);
int32_t	kqring_exec_openat(struct kqring_ctx *ctx, struct kqring_op *op);
int32_t	kqring_exec_pipe(struct kqring_ctx *ctx,
	    const struct io_uring_sqe *sqe);

/* kqring_sync.c */
void	kqring_timer_stop_locked(struct kqring_op *op);
void	kqring_futex_unhash_locked(struct kqring_op *op);
bool	kqring_futex_arm(struct kqring_ctx *ctx, struct kqring_op *op);
int32_t	kqring_futex_wake(const struct io_uring_sqe *sqe);
int	kqring_exec_waitid(struct kqring_op *op, bool nohang);
bool	kqring_waitid_arm(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_rcqe_free(struct kqring_rcqe *rc);
int32_t	kqring_msg_ring(struct kqring_ctx *ctx, const struct kqring_op *op);
int	kqring_copyin_ts(uint64_t uaddr, struct timespec *ts);
sbintime_t kqring_ts_to_sbt(const struct timespec *ts);
int	kqring_timeout_prep(struct kqring_op *op);
int	kqring_timeout_remove_prep(struct kqring_op *op);
void	kqring_timeout_arm(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_ltimeout_arm(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_timeout_fire(struct kqring_ctx *ctx, struct kqring_op *op);
int32_t	kqring_timeout_remove(struct kqring_ctx *ctx,
	    const struct kqring_op *rop, struct kqring_op **gonep);
void	kqring_check_timeouts(struct kqring_ctx *ctx);
int32_t	kqring_cancel(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe);
int32_t	kqring_poll_remove(struct kqring_ctx *ctx,
	    const struct io_uring_sqe *sqe);
void	kqring_check_futexes(struct kqring_ctx *ctx);
void	kqring_check_waitids(struct kqring_ctx *ctx);

/* kqring_wq.c */
void	kqring_wq_abort_td(struct thread *wtd);
bool	kqring_wq_abort_retired_locked(struct kqring_ctx *ctx);
void	kqring_wq_abort_cb(void *arg);
void	kqring_wq_mark_blocking(struct kqring_ctx *ctx, struct kqring_op *op);
struct kqring_wq *kqring_wq_alloc(struct thread *td);
void	kqring_wq_rele(struct kqring_wq *wq);
void	kqring_wq_close(struct kqring_wq *wq);
void	kqring_wq_put(struct kqring_wq *wq);
bool	kqring_wq_enqueue(struct kqring_ctx *ctx, struct kqring_op *op,
	    int type);

static inline bool
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
	case IORING_OP_SPLICE:
	case IORING_OP_TEE:
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

int	kqring_op_wq_type(const struct kqring_op *op);
bool	kqring_arm_nonsock_op(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_wq_rearm_ready(struct kqring_ctx *ctx, struct kqring_op *op);
int	kqring_sqpoll_start(struct kqring_ctx *ctx, struct thread *td);
bool	kqring_kthread_check_suspend(struct kqring_ctx *ctx);
void	kqring_worker_hold(void);
void	kqring_worker_rele(void);

/* kqring_kpoll.c */
#define	KQRING_KP_READ	0x01
#define	KQRING_KP_WRITE	0x02
int	kqring_kpoll_arm(struct kqring_ctx *ctx, struct kqring_op *op,
	    const struct file *fp);
void	kqring_kpoll_disarm_locked(struct kqring_ctx *ctx,
	    struct kqring_op *op);
void	kqring_kpoll_deliver(struct kqring_ctx *ctx, struct kqring_op *op);
void	kqring_kpoll_stop(struct kqring_ctx *ctx);
void	kqring_kpoll_close(struct kqring_ctx *ctx);

/* Whether the ring has a kthread (SQPOLL, or completions only). */
static inline bool
kqring_has_kthread(const struct kqring_ctx *ctx)
{
	return ((ctx->flags & IORING_SETUP_SQPOLL) != 0 || ctx->taskrun_only);
}
int	kqring_sqpoll_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
	    bool has_timeout, sbintime_t sbt);

/* kqring_zc.c */
int	kqring_zc_buf_alloc(struct thread *td, vm_offset_t start, size_t len,
	    vm_prot_t prot, size_t discount_pages, struct kqring_zc_buf **zbp);
void	kqring_zc_buf_rele(struct kqring_zc_buf *zb);
void	kqring_zc_notif_free_bufs(struct kqring_zc_notif *zc);
int	kqring_zc_refresh_cow(struct kqring_ctx *ctx, struct kqring_rsrc *r,
	    struct thread *td);
void	kqring_zc_rele(struct kqring_zc_notif *zc, bool in_issuer);
void	kqring_zc_abort(struct kqring_zc_notif *zc);
void	kqring_zc_mext_free(struct mbuf *m);
bool	kqring_zc_hold(void);

#endif /* !_KQRING_VAR_H_ */
