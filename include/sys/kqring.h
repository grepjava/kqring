/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring: A memory-mapped Submission/Completion Ring and Provided Buffer Ring
 * (PBUF_RING) driver for FreeBSD, implementing the safe SINGLE_ISSUER |
 * DEFER_TASKRUN subset of the Linux io_uring ABI.
 *
 * Security & Design Invariants:
 *   1. SINGLE_ISSUER + DEFER_TASKRUN only: All actual I/O data transfers
 *      (kern_recvit, kern_sendit, kern_accept4, uiomove) execute synchronously
 *      in the owning thread's context inside KQRING_IOC_ENTER.
 *   2. Zero background kernel threads (no io-wq): The kernel never borrows
 *      curproc->p_fd, vmspace, or ucred asynchronously.
 *   3. Zero persistent struct file* references across syscalls: File
 *      descriptors are looked up with Capsicum rights (fget_read / fget_write)
 *      only for the duration of the synchronous transfer inside KQRING_IOC_ENTER.
 *   4. Zero kernel page-pinning (vm_page_wire) of user I/O buffers: Provided
 *      buffer rings (PBUF_RING) and user buffers are read/written via standard
 *      copyin/uiomove in curthread context.
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
 * written against <linux/io_uring.h> (such as Vega's cvega.c).
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
#define IORING_SETUP_SQPOLL		(1U << 1)	/* rejected: no kernel threads */
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
	IORING_OP_LAST
};

/*
 * sqe->ioprio flags for SEND/RECV
 */
#define IORING_RECVSEND_POLL_FIRST	(1U << 0)
#define IORING_RECV_MULTISHOT		(1U << 1)
#define IORING_RECVSEND_FIXED_BUF	(1U << 2)

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

#define IORING_CQE_BUFFER_SHIFT		16

/*
 * Magic mmap offsets for ring fd
 */
#define IORING_OFF_SQ_RING		0ULL
#define IORING_OFF_CQ_RING		0x8000000ULL
#define IORING_OFF_SQES			0x10000000ULL
#define IORING_OFF_PBUF_RING		0x80000000ULL

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
	IORING_REGISTER_PBUF_RING		= 22,
	IORING_UNREGISTER_PBUF_RING		= 23,
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
	__u32	pad;
	__u64	ts;
};

/*
 * Futex flags for IORING_OP_FUTEX_WAIT compatibility
 */
#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32		0x02
#endif
#ifndef FUTEX2_PRIVATE
#define FUTEX2_PRIVATE		128
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY	0xffffffffU
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
};

struct kqring_register_arg {
	__u32	opcode;
	__u32	nr_args;
	void	*arg;
};

#define KQRING_IOC_MAGIC	'U'
#define KQRING_IOC_SETUP	_IOWR(KQRING_IOC_MAGIC, 1, struct kqring_setup_arg)
#define KQRING_IOC_ENTER	_IOWR(KQRING_IOC_MAGIC, 2, struct kqring_enter_arg)
#define KQRING_IOC_REGISTER	_IOWR(KQRING_IOC_MAGIC, 3, struct kqring_register_arg)

#ifndef _KERNEL
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * Drop-in userspace wrappers matching cvega.c's uring_setup / uring_enter /
 * uring_register signatures.
 */
static inline long
kqring_setup(unsigned entries, struct io_uring_params *p)
{
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
	struct kqring_enter_arg ea = {
		.to_submit = submit,
		.min_complete = wait,
		.flags = flags,
		.SUBMITTED_out = 0,
		.arg = arg,
		.argsz = argsz,
	};
	if (ioctl(fd, KQRING_IOC_ENTER, &ea) < 0)
		return (-1);
	return ((long)ea.SUBMITTED_out);
}

static inline long
kqring_register(int fd, unsigned op, void *arg, unsigned n)
{
	struct kqring_register_arg ra = {
		.opcode = op,
		.nr_args = n,
		.arg = arg,
	};
	return (ioctl(fd, KQRING_IOC_REGISTER, &ra));
}
#endif /* !_KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* !_SYS_KQRING_H_ */
