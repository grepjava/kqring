/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring: A memory-mapped Submission/Completion Ring and Provided Buffer Ring
 * (PBUF_RING) driver for FreeBSD, implementing the SINGLE_ISSUER |
 * DEFER_TASKRUN subset of the Linux io_uring ABI.
 *
 * Security & Design Invariants:
 *   1. SINGLE_ISSUER identifies the userspace issuer by kernel thread ID
 *      (lwpid_t) and process. Without SQPOLL, that thread submits, drains
 *      taskwork, posts CQEs and changes resource tables. With SQPOLL, an
 *      in-process kernel thread also does this work; sq_sx serializes it with
 *      enter and registration.
 *   2. In-process worker kthreads share the process address space and file
 *      descriptor table. Requests retain resource and credential references.
 *      Workers execute I/O and queue results; the serialized issuer posts
 *      CQEs and releases request resources. Cancellation of running I/O is
 *      deferred until worker release; ring close detaches running workers and
 *      defers final context/pool deallocation until the last worker returns
 *      (module unload drains any remaining workers). Process exit and
 *      execve(2) close and disarm open rings in the calling process and
 *      release their fixed tables.
 *   3. Fixed slots retain file references and Capsicum rights. Rings are
 *      rejected from fixed tables, and fixed tables are released on process
 *      exit/exec so an AF_UNIX SCM_RIGHTS message carrying a ring fd on a
 *      fixed-table socket cannot leak a reference cycle; registered-ring
 *      descriptors separately retain ring file references until unregister,
 *      exec or process exit. Ordinary descriptors are resolved with rights
 *      checks when used.
 *   4. Ordinary user I/O buffers are accessed through copyin/copyout or
 *      uiomove in process context. Registered buffers hold their backing
 *      pages for zero-copy SEND_ZC (IORING_RECVSEND_FIXED_BUF), charged against
 *      RLIMIT_MEMLOCK and vm.max_user_wired until released by the slot and all
 *      in-flight mbufs. Kernel-allocated ring VM objects are wired; workers
 *      reserve a provided group's head across blocking I/O (ENOBUFS / EBUSY).
 */

#ifndef _SYS_KQRING_H_
#define _SYS_KQRING_H_

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/ioccom.h>
#else
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#endif

/*
 * CQE results are negated FreeBSD errno values.  FreeBSD has no ETIME,
 * which io_uring timeouts complete with; it is reported as ETIMEDOUT.
 */
#ifndef ETIME
#define ETIME	ETIMEDOUT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Linux-compatible fixed-width types for source compatibility with code
 * written against <linux/io_uring.h>.
 */
#ifndef __u8
typedef uint8_t   __u8;
typedef uint16_t  __u16;
typedef uint32_t  __u32;
typedef uint64_t  __u64;
typedef int32_t   __s32;
typedef int64_t   __s64;
#endif

#ifndef __kernel_timespec
struct __kernel_timespec {
	int64_t	tv_sec;
	long long tv_nsec;
};
#endif

/*
 * Submission Queue Entry (SQE) - 64 bytes, matching Linux io_uring ABI.
 */
struct io_uring_sqe {
	__u8	opcode;		/* IORING_OP_* */
	__u8	flags;		/* IOSQE_* flags */
	__u16	ioprio;		/* IORING_RECV_MULTISHOT | IORING_RECVSEND_POLL_FIRST */
	__s32	fd;		/* target file descriptor */
	union {
		__u64	off;
		__u64	addr2;
		struct {
			__u32	cmd_op;
			__u32	__pad1;
		};
	};
	union {
		__u64	addr;	/* buffer pointer or msghdr/futex addr */
		__u64	splice_off_in;
		struct {
			__u32	level;		/* URING_CMD socket options */
			__u32	optname;
		};
	};
	__u32	len;		/* buffer length or flags */
	union {
		__u32	rw_flags;
		__u32	fsync_flags;
		__u16	poll_events;
		__u32	poll32_events;
		__u32	sync_range_flags;
		__u32	msg_flags;
		__u32	timeout_flags;
		__u32	accept_flags;
		__u32	cancel_flags;
		__u32	open_flags;
		__u32	statx_flags;
		__u32	fadvise_advice;
		__u32	splice_flags;
		__u32	rename_flags;
		__u32	unlink_flags;
		__u32	hardlink_flags;
		__u32	xattr_flags;
		__u32	msg_ring_flags;
		__u32	uring_cmd_flags;
		__u32	waitid_flags;
		__u32	futex_flags;
		__u32	install_fd_flags;
		__u32	nop_flags;
		__u32	pipe_flags;
	};
	__u64	user_data;	/* echoed back in CQE */
	union {
		__u16	buf_index;
		__u16	buf_group;
	} __attribute__((packed));
	__u16	personality;
	union {
		__s32	splice_fd_in;
		__u32	file_index;
		__u32	optlen;
		struct {
			__u16	addr_len;
			__u16	__pad3[1];
		};
	};
	union {
		struct {
			__u64	addr3;
			__u64	__pad2[1];
		};
		__u64	optval;
		__u8	cmd[0];
	};
};

_Static_assert(sizeof(struct io_uring_sqe) == 64, "io_uring_sqe must be 64 bytes");

/*
 * Completion Queue Entry (CQE) - 16 bytes, matching Linux io_uring ABI.
 */
struct io_uring_cqe {
	__u64	user_data;
	__s32	res;
	__u32	flags;
	__u64	big_cqe[0];
};

_Static_assert(sizeof(struct io_uring_cqe) == 16, "io_uring_cqe must be 16 bytes");

/*
 * SQE flags (sqe->flags)
 */
#define IOSQE_FIXED_FILE	(1U << 0)
#define IOSQE_IO_DRAIN		(1U << 1)
#define IOSQE_IO_LINK		(1U << 2)
#define IOSQE_IO_HARDLINK	(1U << 3)
#define IOSQE_ASYNC		(1U << 4)
#define IOSQE_BUFFER_SELECT	(1U << 5)	/* select buffer from PBUF_RING */
#define IOSQE_CQE_SKIP_SUCCESS	(1U << 6)

/*
 * io_uring_setup() flags (params->flags)
 */
#define IORING_SETUP_IOPOLL		(1U << 0)
#define IORING_SETUP_SQPOLL		(1U << 1)	/* in-process submission thread */
#define IORING_SETUP_SQ_AFF		(1U << 2)
#define IORING_SETUP_CQSIZE		(1U << 3)
#define IORING_SETUP_CLAMP		(1U << 4)
#define IORING_SETUP_ATTACH_WQ		(1U << 5)
#define IORING_SETUP_R_DISABLED		(1U << 6)
#define IORING_SETUP_SUBMIT_ALL		(1U << 7)
#define IORING_SETUP_COOP_TASKRUN	(1U << 8)
#define IORING_SETUP_TASKRUN_FLAG	(1U << 9)
#define IORING_SETUP_SQE128		(1U << 10)
#define IORING_SETUP_CQE32		(1U << 11)
#define IORING_SETUP_SINGLE_ISSUER	(1U << 12)	/* required */
#define IORING_SETUP_DEFER_TASKRUN	(1U << 13)	/* required */

/*
 * Opcodes (sqe->opcode)
 */
enum io_uring_op {
	IORING_OP_NOP = 0,
	IORING_OP_READV = 1,
	IORING_OP_WRITEV = 2,
	IORING_OP_FSYNC = 3,
	IORING_OP_READ_FIXED = 4,
	IORING_OP_WRITE_FIXED = 5,
	IORING_OP_POLL_ADD = 6,
	IORING_OP_POLL_REMOVE = 7,
	IORING_OP_SYNC_FILE_RANGE = 8,
	IORING_OP_SENDMSG = 9,
	IORING_OP_RECVMSG = 10,
	IORING_OP_TIMEOUT = 11,
	IORING_OP_TIMEOUT_REMOVE = 12,
	IORING_OP_ACCEPT = 13,
	IORING_OP_ASYNC_CANCEL = 14,
	IORING_OP_LINK_TIMEOUT = 15,
	IORING_OP_CONNECT = 16,
	IORING_OP_FALLOCATE = 17,
	IORING_OP_OPENAT = 18,
	IORING_OP_CLOSE = 19,
	IORING_OP_FILES_UPDATE = 20,
	IORING_OP_STATX = 21,
	IORING_OP_READ = 22,
	IORING_OP_WRITE = 23,
	IORING_OP_FADVISE = 24,
	IORING_OP_MADVISE = 25,
	IORING_OP_SEND = 26,
	IORING_OP_RECV = 27,
	IORING_OP_OPENAT2 = 28,
	IORING_OP_EPOLL_CTL = 29,
	IORING_OP_SPLICE = 30,
	IORING_OP_PROVIDE_BUFFERS = 31,
	IORING_OP_REMOVE_BUFFERS = 32,
	IORING_OP_TEE = 33,
	IORING_OP_SHUTDOWN = 34,
	IORING_OP_RENAMEAT = 35,
	IORING_OP_UNLINKAT = 36,
	IORING_OP_MKDIRAT = 37,
	IORING_OP_SYMLINKAT = 38,
	IORING_OP_LINKAT = 39,
	IORING_OP_MSG_RING = 40,
	IORING_OP_FSETXATTR = 41,
	IORING_OP_SETXATTR = 42,
	IORING_OP_FGETXATTR = 43,
	IORING_OP_GETXATTR = 44,
	IORING_OP_SOCKET = 45,
	IORING_OP_URING_CMD = 46,
	IORING_OP_SEND_ZC = 47,
	IORING_OP_SENDMSG_ZC = 48,
	IORING_OP_READ_MULTISHOT = 49,
	IORING_OP_WAITID = 50,
	IORING_OP_FUTEX_WAIT = 51,
	IORING_OP_FUTEX_WAKE = 52,
	IORING_OP_FUTEX_WAITV = 53,
	IORING_OP_FIXED_FD_INSTALL = 54,
	IORING_OP_FTRUNCATE = 55,
	IORING_OP_BIND = 56,
	IORING_OP_LISTEN = 57,
	IORING_OP_RECV_ZC = 58,
	IORING_OP_EPOLL_WAIT = 59,
	IORING_OP_READV_FIXED = 60,
	IORING_OP_WRITEV_FIXED = 61,
	IORING_OP_PIPE = 62,
	IORING_OP_LAST
};

/*
 * sqe->file_index for ops that can create a direct descriptor (OPENAT,
 * OPENAT2, ACCEPT, SOCKET, PIPE): 0 installs a normal descriptor, n puts
 * the file in fixed slot n - 1, and IORING_FILE_INDEX_ALLOC in a free slot
 * (returned as the result).
 */
#define IORING_FILE_INDEX_ALLOC		(~0U)

/* FIXED_FD_INSTALL flags (sqe->install_fd_flags) */
#define IORING_FIXED_FD_NO_CLOEXEC	(1U << 0)

/*
 * sqe->rw_flags for READ/WRITE(V) (Linux's preadv2(2) RWF_* values):
 * HIPRI is accepted and ignored, NOWAIT fails with -EAGAIN rather than
 * waiting or going to a worker, DSYNC/SYNC sync the data (and metadata)
 * after a write.  Others fail with -EOPNOTSUPP.
 */
#define KQRING_RWF_HIPRI		0x00000001
#define KQRING_RWF_DSYNC		0x00000002
#define KQRING_RWF_SYNC			0x00000004
#define KQRING_RWF_NOWAIT		0x00000008
#define KQRING_RWF_SUPPORTED		(KQRING_RWF_HIPRI | KQRING_RWF_DSYNC | \
	KQRING_RWF_SYNC | KQRING_RWF_NOWAIT)

/* sqe->nop_flags: complete with (int32_t)sqe->len instead of 0. */
#define IORING_NOP_INJECT_RESULT	(1U << 0)

/*
 * sqe->ioprio flags for SEND/RECV
 */
#define IORING_RECVSEND_POLL_FIRST	(1U << 0)
#define IORING_RECV_MULTISHOT		(1U << 1)
#define IORING_RECVSEND_FIXED_BUF	(1U << 2)
#define IORING_SEND_ZC_REPORT_USAGE	(1U << 3)
#define IORING_RECVSEND_BUNDLE		(1U << 4)

/*
 * SEND_ZC / SENDMSG_ZC notification CQE result bit: the data was copied.
 * Reported when IORING_SEND_ZC_REPORT_USAGE is set and the request used the
 * copy path rather than zero-copy EXT_SFBUF page attachment.
 */
#define IORING_NOTIF_USAGE_ZC_COPIED	(1U << 31)

/*
 * Multishot RECVMSG: each selected buffer starts with this header, then
 * msg_namelen bytes of name, msg_controllen bytes of control data, and the
 * payload (sizes taken from the msghdr given at submission).
 */
struct io_uring_recvmsg_out {
	__u32	namelen;
	__u32	controllen;
	__u32	payloadlen;
	__u32	flags;
};

/*
 * MSG_RING: sqe->addr is the command, sqe->msg_ring_flags the flags.
 */
#define IORING_MSG_DATA			0
#define IORING_MSG_SEND_FD		1
#define IORING_MSG_RING_CQE_SKIP	(1U << 0)
#define IORING_MSG_RING_FLAGS_PASS	(1U << 1)

/*
 * URING_CMD on sockets (sqe->cmd_op)
 */
#define SOCKET_URING_OP_SIOCINQ		0
#define SOCKET_URING_OP_SIOCOUTQ	1
#define SOCKET_URING_OP_GETSOCKOPT	2
#define SOCKET_URING_OP_SETSOCKOPT	3

/*
 * Linux constants FreeBSD lacks, for opcodes that take them.  Everything
 * else (O_*, AT_*, MSG_*, POSIX_FADV_*, MADV_*, idtype_t, W*) is passed
 * with its native FreeBSD value.
 */
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE		0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE		0x02
#endif
#ifndef SYNC_FILE_RANGE_WAIT_BEFORE
#define SYNC_FILE_RANGE_WAIT_BEFORE	0x1
#define SYNC_FILE_RANGE_WRITE		0x2
#define SYNC_FILE_RANGE_WAIT_AFTER	0x4
#endif
#ifndef XATTR_CREATE
#define XATTR_CREATE			0x1
#define XATTR_REPLACE			0x2
#endif
#ifndef SPLICE_F_MOVE
#define SPLICE_F_MOVE			0x01U
#define SPLICE_F_NONBLOCK		0x02U
#define SPLICE_F_MORE			0x04U
#define SPLICE_F_GIFT			0x08U
#endif
#ifndef SPLICE_F_FD_IN_FIXED
#define SPLICE_F_FD_IN_FIXED		(1U << 31)
#endif
#define KQRING_XATTR_NAME_MAX		255
#define KQRING_XATTR_SIZE_MAX		65536

/*
 * OPENAT2 (sqe->addr2 points to it, sqe->len is its size).  how.flags are
 * FreeBSD O_* flags.
 */
struct open_how {
	__u64	flags;
	__u64	mode;
	__u64	resolve;
};

#ifndef RESOLVE_NO_XDEV
#define RESOLVE_NO_XDEV			0x01
#define RESOLVE_NO_MAGICLINKS		0x02
#define RESOLVE_NO_SYMLINKS		0x04
#define RESOLVE_BENEATH			0x08
#define RESOLVE_IN_ROOT			0x10
#define RESOLVE_CACHED			0x20
#endif

/*
 * STATX: Linux struct statx, filled from FreeBSD stat(2).  sqe->statx_flags
 * are FreeBSD AT_* flags.
 */
struct statx_timestamp {
	__s64	tv_sec;
	__u32	tv_nsec;
	__s32	__reserved;
};

struct statx {
	__u32	stx_mask;
	__u32	stx_blksize;
	__u64	stx_attributes;
	__u32	stx_nlink;
	__u32	stx_uid;
	__u32	stx_gid;
	__u16	stx_mode;
	__u16	__spare0[1];
	__u64	stx_ino;
	__u64	stx_size;
	__u64	stx_blocks;
	__u64	stx_attributes_mask;
	struct statx_timestamp	stx_atime;
	struct statx_timestamp	stx_btime;
	struct statx_timestamp	stx_ctime;
	struct statx_timestamp	stx_mtime;
	__u32	stx_rdev_major;
	__u32	stx_rdev_minor;
	__u32	stx_dev_major;
	__u32	stx_dev_minor;
	__u64	stx_mnt_id;
	__u32	stx_dio_mem_align;
	__u32	stx_dio_offset_align;
	__u64	__spare3[12];
};

_Static_assert(sizeof(struct statx) == 256, "struct statx must be 256 bytes");

#define STATX_TYPE			0x00000001U
#define STATX_MODE			0x00000002U
#define STATX_NLINK			0x00000004U
#define STATX_UID			0x00000008U
#define STATX_GID			0x00000010U
#define STATX_ATIME			0x00000020U
#define STATX_MTIME			0x00000040U
#define STATX_CTIME			0x00000080U
#define STATX_INO			0x00000100U
#define STATX_SIZE			0x00000200U
#define STATX_BLOCKS			0x00000400U
#define STATX_BASIC_STATS		0x000007ffU
#define STATX_BTIME			0x00000800U
#define STATX__RESERVED			0x80000000U

#define STATX_ATTR_COMPRESSED		0x00000004
#define STATX_ATTR_IMMUTABLE		0x00000010
#define STATX_ATTR_APPEND		0x00000020
#define STATX_ATTR_NODUMP		0x00000040

/*
 * ACCEPT flags (sqe->ioprio)
 */
#define IORING_ACCEPT_MULTISHOT		(1U << 0)
#define IORING_ACCEPT_DONTWAIT		(1U << 1)
#define IORING_ACCEPT_POLL_FIRST	(1U << 2)

/*
 * sqe->fsync_flags
 */
#define IORING_FSYNC_DATASYNC		(1U << 0)

/*
 * sqe->timeout_flags
 */
#define IORING_TIMEOUT_ABS		(1U << 0)
#define IORING_TIMEOUT_UPDATE		(1U << 1)
#define IORING_TIMEOUT_BOOTTIME		(1U << 2)
#define IORING_TIMEOUT_REALTIME		(1U << 3)
#define IORING_LINK_TIMEOUT_UPDATE	(1U << 4)
#define IORING_TIMEOUT_ETIME_SUCCESS	(1U << 5)
#define IORING_TIMEOUT_MULTISHOT	(1U << 6)
#define IORING_TIMEOUT_CLOCK_MASK					\
	(IORING_TIMEOUT_BOOTTIME | IORING_TIMEOUT_REALTIME)
#define IORING_TIMEOUT_UPDATE_MASK					\
	(IORING_TIMEOUT_UPDATE | IORING_LINK_TIMEOUT_UPDATE)

/*
 * POLL_ADD / POLL_REMOVE flags, stored in sqe->len
 */
#define IORING_POLL_ADD_MULTI		(1U << 0)
#define IORING_POLL_UPDATE_EVENTS	(1U << 1)
#define IORING_POLL_UPDATE_USER_DATA	(1U << 2)
#define IORING_POLL_ADD_LEVEL		(1U << 3)

/*
 * ASYNC_CANCEL flags (sqe->cancel_flags)
 */
#define IORING_ASYNC_CANCEL_ALL		(1U << 0)
#define IORING_ASYNC_CANCEL_FD		(1U << 1)
#define IORING_ASYNC_CANCEL_ANY		(1U << 2)
#define IORING_ASYNC_CANCEL_FD_FIXED	(1U << 3)
#define IORING_ASYNC_CANCEL_USERDATA	(1U << 4)
#define IORING_ASYNC_CANCEL_OP		(1U << 5)

/*
 * CQE flags (cqe->flags)
 */
#define IORING_CQE_F_BUFFER		(1U << 0)
#define IORING_CQE_F_MORE		(1U << 1)
#define IORING_CQE_F_SOCK_NONEMPTY	(1U << 2)
#define IORING_CQE_F_NOTIF		(1U << 3)
#define IORING_CQE_F_BUF_MORE		(1U << 4)	/* IOU_PBUF_RING_INC */
#define IORING_CQE_F_32			(1U << 5)	/* 32-byte CQE */

#define IORING_CQE_BUFFER_SHIFT		16

/*
 * CQ ring flags (*cq_flags), set by userspace
 */
#define IORING_CQ_EVENTFD_DISABLED	(1U << 0)

/*
 * Magic mmap offsets for ring fd
 */
#define IORING_OFF_SQ_RING		0ULL
#define IORING_OFF_CQ_RING		0x8000000ULL
#define IORING_OFF_SQES			0x10000000ULL
#define IORING_OFF_PBUF_RING		0x80000000ULL
#define IORING_OFF_PBUF_SHIFT		16	/* bgid << shift, for IOU_PBUF_RING_MMAP */
#define IORING_OFF_MMAP_MASK		0xf8000000ULL

/*
 * SQ ring kflags (*sq_kflags)
 */
#define IORING_SQ_NEED_WAKEUP		(1U << 0)
#define IORING_SQ_CQ_OVERFLOW		(1U << 1)
#define IORING_SQ_TASKRUN		(1U << 2)

/*
 * io_uring_enter() flags
 */
#define IORING_ENTER_GETEVENTS		(1U << 0)
#define IORING_ENTER_SQ_WAKEUP		(1U << 1)
#define IORING_ENTER_SQ_WAIT		(1U << 2)
#define IORING_ENTER_EXT_ARG		(1U << 3)
#define IORING_ENTER_REGISTERED_RING	(1U << 4)

/*
 * Features reported in params->features
 */
#define IORING_FEAT_SINGLE_MMAP		(1U << 0)
#define IORING_FEAT_NODROP		(1U << 1)
#define IORING_FEAT_SUBMIT_STABLE	(1U << 2)
#define IORING_FEAT_RW_CUR_POS		(1U << 3)
#define IORING_FEAT_CUR_PERSONALITY	(1U << 4)
#define IORING_FEAT_FAST_POLL		(1U << 5)
#define IORING_FEAT_POLL_32BITS		(1U << 6)
#define IORING_FEAT_SQPOLL_NONFIXED	(1U << 7)
#define IORING_FEAT_EXT_ARG		(1U << 8)
#define IORING_FEAT_NATIVE_WORKERS	(1U << 9)
#define IORING_FEAT_RSRC_TAGS		(1U << 10)
#define IORING_FEAT_CQE_SKIP		(1U << 11)
#define IORING_FEAT_LINKED_FILE		(1U << 12)
#define IORING_FEAT_REG_REG_RING	(1U << 13)
#define IORING_FEAT_RECVSEND_BUNDLE	(1U << 14)
#define IORING_FEAT_MIN_TIMEOUT		(1U << 15)

/*
 * io_uring_register() opcodes
 */
enum {
	IORING_REGISTER_BUFFERS			= 0,
	IORING_UNREGISTER_BUFFERS		= 1,
	IORING_REGISTER_FILES			= 2,
	IORING_UNREGISTER_FILES			= 3,
	IORING_REGISTER_EVENTFD			= 4,
	IORING_UNREGISTER_EVENTFD		= 5,
	IORING_REGISTER_FILES_UPDATE		= 6,
	IORING_REGISTER_EVENTFD_ASYNC		= 7,
	IORING_REGISTER_PROBE			= 8,
	IORING_REGISTER_PERSONALITY		= 9,
	IORING_UNREGISTER_PERSONALITY		= 10,
	IORING_REGISTER_RESTRICTIONS		= 11,
	IORING_REGISTER_ENABLE_RINGS		= 12,
	IORING_REGISTER_FILES2			= 13,
	IORING_REGISTER_FILES_UPDATE2		= 14,
	IORING_REGISTER_BUFFERS2		= 15,
	IORING_REGISTER_BUFFERS_UPDATE		= 16,
	IORING_REGISTER_IOWQ_AFF		= 17,
	IORING_UNREGISTER_IOWQ_AFF		= 18,
	IORING_REGISTER_IOWQ_MAX_WORKERS	= 19,
	IORING_REGISTER_RING_FDS		= 20,
	IORING_UNREGISTER_RING_FDS		= 21,
	IORING_REGISTER_PBUF_RING		= 22,
	IORING_UNREGISTER_PBUF_RING		= 23,
	IORING_REGISTER_SYNC_CANCEL		= 24,
	IORING_REGISTER_FILE_ALLOC_RANGE	= 25,
	IORING_REGISTER_PBUF_STATUS		= 26,
	IORING_REGISTER_NAPI			= 27,
	IORING_UNREGISTER_NAPI			= 28,
	IORING_REGISTER_CLOCK			= 29,
	IORING_REGISTER_CLONE_BUFFERS		= 30,
	IORING_REGISTER_SEND_MSG_RING		= 31,
	IORING_REGISTER_LAST
};

#define IORING_REGISTER_USE_REGISTERED_RING	(1U << 31)

/*
 * IORING_REGISTER_FILES / _FILES_UPDATE: an fd of -1 is an empty slot, and
 * in an update IORING_REGISTER_FILES_SKIP leaves the slot as it is.
 */
#define IORING_REGISTER_FILES_SKIP	(-2)

/* IORING_REGISTER_FILES_UPDATE (nr_args = number of fds) */
struct io_uring_files_update {
	__u32	offset;
	__u32	resv;
	__u64	fds;		/* __s32 * */
};

/* IORING_REGISTER_RING_FDS / IORING_UNREGISTER_RING_FDS */
struct io_uring_rsrc_update {
	__u32	offset;
	__u32	resv;
	__u64	data;
};

/* IORING_REGISTER_FILES2 / _BUFFERS2 (nr_args = sizeof) */
#define IORING_RSRC_REGISTER_SPARSE	(1U << 0)

struct io_uring_rsrc_register {
	__u32	nr;
	__u32	flags;
	__u64	resv2;
	__u64	data;		/* __s32 fds[nr] or struct iovec[nr] */
	__u64	tags;		/* __u64 tags[nr], or 0 */
};

/* IORING_REGISTER_FILES_UPDATE2 / _BUFFERS_UPDATE (nr_args = sizeof) */
struct io_uring_rsrc_update2 {
	__u32	offset;
	__u32	resv;
	__u64	data;
	__u64	tags;
	__u32	nr;
	__u32	resv2;
};

/* IORING_REGISTER_FILE_ALLOC_RANGE: slots IORING_FILE_INDEX_ALLOC uses */
struct io_uring_file_index_range {
	__u32	off;
	__u32	len;
	__u64	resv;
};

/* IORING_REGISTER_RESTRICTIONS (on an IORING_SETUP_R_DISABLED ring) */
enum {
	IORING_RESTRICTION_REGISTER_OP		= 0,
	IORING_RESTRICTION_SQE_OP		= 1,
	IORING_RESTRICTION_SQE_FLAGS_ALLOWED	= 2,
	IORING_RESTRICTION_SQE_FLAGS_REQUIRED	= 3,
	IORING_RESTRICTION_LAST
};

struct io_uring_restriction {
	__u16	opcode;
	union {
		__u8	register_op;
		__u8	sqe_op;
		__u8	sqe_flags;
	};
	__u8	resv;
	__u32	resv2[3];
};

/* IORING_REGISTER_SYNC_CANCEL */
struct io_uring_sync_cancel_reg {
	__u64	addr;
	__s32	fd;
	__u32	flags;
	struct __kernel_timespec timeout;
	__u8	opcode;
	__u8	pad[7];
	__u64	pad2[3];
};

/* IORING_REGISTER_CLOCK */
struct io_uring_clock_register {
	__u32	clockid;
	__u32	__resv[3];
};

/*
 * IORING_REGISTER_NAPI / IORING_UNREGISTER_NAPI.  kqring busy-waits up to
 * busy_poll_to microseconds for completions before sleeping in a wait;
 * FreeBSD has no NAPI ids, so the static id operations are not supported.
 */
enum io_uring_napi_op {
	IO_URING_NAPI_REGISTER_OP	= 0,
	IO_URING_NAPI_STATIC_ADD_ID	= 1,
	IO_URING_NAPI_STATIC_DEL_ID	= 2,
};

enum io_uring_napi_tracking_strategy {
	IO_URING_NAPI_TRACKING_DYNAMIC	= 0,
	IO_URING_NAPI_TRACKING_STATIC	= 1,
	IO_URING_NAPI_TRACKING_INACTIVE	= 255,
};

struct io_uring_napi {
	__u32	busy_poll_to;
	__u8	prefer_busy_poll;
	__u8	opcode;
	__u8	pad[2];
	__u32	op_param;
	__u32	resv;
};

/* IORING_REGISTER_IOWQ_MAX_WORKERS (uint32_t max[2]: bound, unbound) */
enum io_wq_type {
	IO_WQ_BOUND,
	IO_WQ_UNBOUND,
};

/* IORING_REGISTER_CLONE_BUFFERS */
#define IORING_REGISTER_SRC_REGISTERED	(1U << 0)
#define IORING_REGISTER_DST_REPLACE	(1U << 1)

struct io_uring_clone_buffers {
	__u32	src_fd;
	__u32	flags;
	__u32	src_off;
	__u32	dst_off;
	__u32	nr;
	__u32	pad[3];
};

/*
 * Ring offset descriptions returned by setup
 */
struct io_sqring_offsets {
	__u32 head;
	__u32 tail;
	__u32 ring_mask;
	__u32 ring_entries;
	__u32 flags;
	__u32 dropped;
	__u32 array;
	__u32 resv1;
	__u64 user_addr;
};

struct io_cqring_offsets {
	__u32 head;
	__u32 tail;
	__u32 ring_mask;
	__u32 ring_entries;
	__u32 overflow;
	__u32 cqes;
	__u32 flags;
	__u32 resv1;
	__u64 user_addr;
};

struct io_uring_params {
	__u32 sq_entries;
	__u32 cq_entries;
	__u32 flags;
	__u32 sq_thread_cpu;
	__u32 sq_thread_idle;
	__u32 features;
	__u32 wq_fd;
	__u32 resv[3];
	struct io_sqring_offsets sq_off;
	struct io_cqring_offsets cq_off;
};

/*
 * Provided Buffer Ring (PBUF_RING) structures
 */
struct io_uring_buf {
	__u64	addr;
	__u32	len;
	__u16	bid;
	__u16	resv;
};

struct io_uring_buf_ring {
	union {
		struct {
			__u64	resv1;
			__u32	resv2;
			__u16	resv3;
			__u16	tail;
		};
		struct io_uring_buf bufs[0];
	};
};

_Static_assert(sizeof(struct io_uring_buf) == 16, "io_uring_buf must be 16 bytes");

struct io_uring_buf_reg {
	__u64	ring_addr;
	__u32	ring_entries;
	__u16	bgid;
	__u16	flags;
	__u64	resv[3];
};

/*
 * io_uring_buf_reg flags.  IOU_PBUF_RING_MMAP: the kernel allocates the
 * ring, which userspace maps at IORING_OFF_PBUF_RING |
 * (bgid << IORING_OFF_PBUF_SHIFT).  IOU_PBUF_RING_INC: a buffer may be
 * consumed in parts; the kernel advances its addr/len in the ring and sets
 * IORING_CQE_F_BUF_MORE while some of it is left.
 */
#define IOU_PBUF_RING_MMAP		(1U << 0)
#define IOU_PBUF_RING_INC		(1U << 1)

/* IORING_REGISTER_PBUF_STATUS */
struct io_uring_buf_status {
	__u32	buf_group;	/* in */
	__u32	head;		/* out */
	__u32	resv[8];
};

/*
 * Opcode probing structures (IORING_REGISTER_PROBE)
 */
#define IO_URING_OP_SUPPORTED	(1U << 0)

struct io_uring_probe_op {
	__u8	op;
	__u8	resv;
	__u16	flags;
	__u32	resv2;
};

struct io_uring_probe {
	__u8	last_op;
	__u8	ops_len;
	__u16	resv;
	__u32	resv2[3];
	struct io_uring_probe_op ops[0];
};

/*
 * Extended getevents argument for IORING_ENTER_EXT_ARG
 */
struct io_uring_getevents_arg {
	__u64	sigmask;
	__u32	sigmask_sz;
	__u32	min_wait_usec;
	__u64	ts;
};

/*
 * Futex flags for IORING_OP_FUTEX_WAIT compatibility
 */
#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U8		0x00
#define FUTEX2_SIZE_U16		0x01
#define FUTEX2_SIZE_U32		0x02
#define FUTEX2_SIZE_U64		0x03
#define FUTEX2_NUMA		0x04
#endif
#ifndef FUTEX2_SIZE_MASK
#define FUTEX2_SIZE_MASK	0x03
#endif
#ifndef FUTEX2_PRIVATE
#define FUTEX2_PRIVATE		128
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY	0xffffffffU
#endif
#ifndef FUTEX_WAITV_MAX
#define FUTEX_WAITV_MAX		128

/* FUTEX_WAITV vector entry (sqe->addr, sqe->len entries). */
struct futex_waitv {
	__u64	val;
	__u64	uaddr;
	__u32	flags;
	__u32	__reserved;
};
#endif

/*
 * ============================================================================
 * FreeBSD /dev/kqring ioctl interface
 * ============================================================================
 *
 * Opening "/dev/kqring" returns a file descriptor backed by cdevpriv.
 * Issuing KQRING_IOC_SETUP initializes the ring on that descriptor, after
 * which mmap(2) at IORING_OFF_SQ_RING and IORING_OFF_SQES maps the shared
 * ring and SQE array directly into user space.
 */
struct kqring_setup_arg {
	__u32	entries;
	__u32	pad;
	struct io_uring_params *params;
};

struct kqring_enter_arg {
	__u32	to_submit;
	__u32	min_complete;
	__u32	flags;
	__u32	 SUBMITTED_out;
	void	*arg;
	size_t	argsz;
	__s32	ring_fd;
	__u32	pad;
};

struct kqring_register_arg {
	__u32	opcode;
	__u32	nr_args;
	void	*arg;
	__s32	ring_fd;
	__u32	pad;
};

#define KQRING_IOC_MAGIC	'U'
#define KQRING_IOC_SETUP	_IOWR(KQRING_IOC_MAGIC, 1, struct kqring_setup_arg)
#define KQRING_IOC_ENTER	_IOWR(KQRING_IOC_MAGIC, 2, struct kqring_enter_arg)
#define KQRING_IOC_REGISTER	_IOWR(KQRING_IOC_MAGIC, 3, struct kqring_register_arg)

#ifndef _KERNEL
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/sysctl.h>
#include <unistd.h>

static inline int
kqring_ctl_fd(void)
{
	static int ctl_fd = -1;
	int fd = __atomic_load_n(&ctl_fd, __ATOMIC_ACQUIRE);
	if (fd >= 0)
		return (fd);
	fd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	int expected = -1;
	if (!__atomic_compare_exchange_n(&ctl_fd, &expected, fd, false,
	    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
		close(fd);
		return (expected);
	}
	return (fd);
}

/*
 * The native system calls' numbers (kern.kqring.sys_*), looked up once;
 * -1 where the module could not register them.
 */
enum { KQRING_SYS_SETUP, KQRING_SYS_ENTER, KQRING_SYS_REGISTER };

static inline long
kqring_sysnum(int which)
{
	static const char *const names[] = {
		"kern.kqring.sys_setup",
		"kern.kqring.sys_enter",
		"kern.kqring.sys_register",
	};
	static int nums[] = { -2, -2, -2 };	/* -2: not looked up yet */
	int n = __atomic_load_n(&nums[which], __ATOMIC_RELAXED);

	if (n == -2) {
		size_t len = sizeof(n);
		int e = errno;

		if (sysctlbyname(names[which], &n, &len, NULL, 0) != 0 ||
		    n <= 0)
			n = -1;
		errno = e;
		__atomic_store_n(&nums[which], n, __ATOMIC_RELAXED);
	}
	return (n);
}

/*
 * syscall(2) passes the number as an argument and fetches a sixth one from
 * the stack; on amd64 the call is made directly instead.
 */
static inline long
kqring_syscall(long num, long a1, long a2, long a3, long a4, long a5,
    long a6)
{
#if defined(__amd64__)
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	unsigned char failed;

	__asm__ __volatile__("syscall; setc %1"
	    : "+a"(num), "=r"(failed), "+D"(a1), "+S"(a2), "+d"(a3),
	      "+r"(r10), "+r"(r8), "+r"(r9)
	    :
	    : "rcx", "r11", "memory", "cc");
	if (failed) {
		errno = (int)num;
		return (-1);
	}
	return (num);
#else
	return (syscall((int)num, a1, a2, a3, a4, a5, a6));
#endif
}

/*
 * Userspace wrappers with the arguments of Linux's io_uring_setup(2),
 * io_uring_enter(2) and io_uring_register(2): the native system calls,
 * or the /dev/kqring ioctls where those are not registered.
 */
static inline long
kqring_setup(unsigned entries, struct io_uring_params *p)
{
	long num = kqring_sysnum(KQRING_SYS_SETUP);

	if (num > 0)
		return (kqring_syscall(num, (long)entries, (long)p, 0, 0, 0,
		    0));

	int fd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	struct kqring_setup_arg arg = {
		.entries = entries,
		.pad = 0,
		.params = p,
	};
	if (ioctl(fd, KQRING_IOC_SETUP, &arg) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return (-1);
	}
	return (fd);
}

static inline long
kqring_enter(int fd, unsigned submit, unsigned wait, unsigned flags,
    void *arg, size_t argsz)
{
	long num = kqring_sysnum(KQRING_SYS_ENTER);

	if (num > 0)
		return (kqring_syscall(num, (long)fd, (long)submit,
		    (long)wait, (long)flags, (long)arg, (long)argsz));

	struct kqring_enter_arg ea = {
		.to_submit = submit,
		.min_complete = wait,
		.flags = flags,
		.SUBMITTED_out = 0,
		.arg = arg,
		.argsz = argsz,
		.ring_fd = fd,
		.pad = 0,
	};
	int ioctl_fd = fd;
	if ((flags & IORING_ENTER_REGISTERED_RING) != 0) {
		ioctl_fd = kqring_ctl_fd();
		if (ioctl_fd < 0)
			return (-1);
	}
	if (ioctl(ioctl_fd, KQRING_IOC_ENTER, &ea) < 0)
		return (-1);
	return ((long)ea.SUBMITTED_out);
}

static inline long
kqring_register(int fd, unsigned op, void *arg, unsigned n)
{
	long num = kqring_sysnum(KQRING_SYS_REGISTER);

	if (num > 0)
		return (kqring_syscall(num, (long)fd, (long)op, (long)arg,
		    (long)n, 0, 0));

	struct kqring_register_arg ra = {
		.opcode = op,
		.nr_args = n,
		.arg = arg,
		.ring_fd = fd,
		.pad = 0,
	};
	int ioctl_fd = fd;
	if ((op & ~IORING_REGISTER_USE_REGISTERED_RING) ==
	    IORING_REGISTER_RING_FDS)
		(void)kqring_ctl_fd();
	if ((op & IORING_REGISTER_USE_REGISTERED_RING) != 0) {
		ioctl_fd = kqring_ctl_fd();
		if (ioctl_fd < 0)
			return (-1);
	}
	return (ioctl(ioctl_fd, KQRING_IOC_REGISTER, &ra));
}
#endif /* !_KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* !_SYS_KQRING_H_ */
