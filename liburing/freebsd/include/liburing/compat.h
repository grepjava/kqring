/* SPDX-License-Identifier: MIT */
/*
 * liburing/compat.h for FreeBSD (configure writes it on Linux): the Linux
 * types and constants liburing's headers and sources use that FreeBSD
 * lacks.  Flags passed through SQEs (O_*, MSG_*, ...) keep their FreeBSD
 * values, as kqring expects.
 */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/ioccom.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>

/*
 * glibc's byte-order names, which liburing.h tests (poll masks are
 * half-word swapped on big-endian).  Undefined, both sides of
 * `__BYTE_ORDER == __BIG_ENDIAN` are 0 and every poll mask is swapped.
 */
#ifndef __BYTE_ORDER
#define __BYTE_ORDER	_BYTE_ORDER
#endif
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN	_LITTLE_ENDIAN
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN	_BIG_ENDIAN
#endif

typedef int __kernel_rwf_t;
typedef off_t loff_t;

struct __kernel_timespec {
	int64_t		tv_sec;
	long long	tv_nsec;
};
/* struct __kernel_timespec is defined above, not by <linux/time_types.h>. */
#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1

struct open_how {
	uint64_t	flags;
	uint64_t	mode;
	uint64_t	resolve;
};

#define FUTEX_32	2
#define FUTEX_WAITV_MAX	128

struct futex_waitv {
	uint64_t	val;
	uint64_t	uaddr;
	uint32_t	flags;
	uint32_t	__reserved;
};

#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD	_IO(0x12, 0)
#endif

#include <sys/socket.h>
#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len)	_ALIGN(len)
#endif

/* Only named by pointer in liburing.h's prototypes. */
struct statx;
struct epoll_event;

/* io_uring timeouts complete with -ETIME; kqring reports -ETIMEDOUT. */
#ifndef ETIME
#define ETIME		ETIMEDOUT
#endif

/* The sigset size io_uring_enter(2) takes; kqring rejects signal masks. */
#ifndef _NSIG
#define _NSIG		(_SIG_MAXSIG + 1)
#endif

/* Prefault the rings, as MAP_POPULATE does. */
#ifndef MAP_POPULATE
#define MAP_POPULATE	MAP_PREFAULT_READ
#endif
/* liburing's huge-page ring allocation: superpage-aligned on FreeBSD. */
#ifndef MAP_HUGETLB
#define MAP_HUGETLB	MAP_ALIGNED_SUPER
#endif
/* Done with minherit(2) in arch/freebsd/syscall.h. */
#ifndef MADV_DONTFORK
#define MADV_DONTFORK	0x7f00
#define MADV_DOFORK	0x7f01
#endif

#endif
