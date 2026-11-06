/* SPDX-License-Identifier: MIT */
/*
 * Included (with -include) into liburing's test programs only, not the
 * library: Linux behaviour some tests depend on.
 *
 * mmap(2) with an address hint: Linux maps at the hint when the range is
 * free, and the syzbot reproducers (test/<hex>.c) then use fixed addresses
 * such as 0x20000000.  FreeBSD may place the mapping elsewhere (that range
 * is reserved for brk(2)), so ask for the hint with MAP_FIXED | MAP_EXCL,
 * which never replaces an existing mapping, before falling back.
 */
#ifndef LIBURING_FREEBSD_TEST_COMPAT_H
#define LIBURING_FREEBSD_TEST_COMPAT_H

#include <sys/types.h>
#include <sys/mman.h>
#include <stddef.h>

static inline void *
__kqring_test_mmap(void *addr, size_t len, int prot, int flags, int fd,
    off_t off)
{
	if (addr != NULL && (flags & MAP_FIXED) == 0) {
		void *p = mmap(addr, len, prot, flags | MAP_FIXED | MAP_EXCL,
		    fd, off);

		if (p != MAP_FAILED)
			return (p);
	}
	return (mmap(addr, len, prot, flags, fd, off));
}
#define mmap(a, l, p, f, d, o)	__kqring_test_mmap(a, l, p, f, d, o)

/*
 * sendmsg(2) of a ring descriptor with SCM_RIGHTS: Linux (6.7+) refuses
 * it with EINVAL, which tests take as "rings cannot be passed" and skip.
 * FreeBSD refuses any descriptor that cannot be passed with EOPNOTSUPP.
 */
#include <sys/socket.h>
#include <errno.h>

static inline ssize_t
__kqring_test_sendmsg(int s, const struct msghdr *msg, int flags)
{
	ssize_t n = sendmsg(s, msg, flags);

	if (n < 0 && errno == EOPNOTSUPP && msg->msg_controllen != 0)
		errno = EINVAL;
	return (n);
}
#define sendmsg(s, m, f)	__kqring_test_sendmsg(s, m, f)

/*
 * Errors FreeBSD doesn't have, as kqring reports them: EBADF for a ring
 * that isn't enabled, and EOVERFLOW for a dropped completion.
 */
#define EBADFD		EBADF
#define EBADR		EOVERFLOW

/*
 * Files are always large here, sends are never corked, mappings always
 * reserve nothing up front, and waitpid(2) always sees every child.
 */
#define O_LARGEFILE	0
#define MSG_MORE	0
#define MAP_NORESERVE	0
#define __WALL		0

/* Linux's name for IOV_MAX, which kqring's limit is. */
#define UIO_MAXIOV	1024

/*
 * Huge pages by request: there are none here, as on a Linux machine with
 * nr_hugepages at 0, so such a mapping fails and the test takes its own
 * way out.  FreeBSD promotes pages to superpages by itself.
 */
#define MAP_HUGETLB	0x00800000
#define MAP_HUGE_SHIFT	26
#define MAP_HUGE_2MB	(21 << MAP_HUGE_SHIFT)

/* Linux's struct statx, which kqring fills. */
#include <stdint.h>

struct statx_timestamp {
	int64_t		tv_sec;
	uint32_t	tv_nsec;
	int32_t		__reserved;
};

struct statx {
	uint32_t	stx_mask;
	uint32_t	stx_blksize;
	uint64_t	stx_attributes;
	uint32_t	stx_nlink;
	uint32_t	stx_uid;
	uint32_t	stx_gid;
	uint16_t	stx_mode;
	uint16_t	__spare0[1];
	uint64_t	stx_ino;
	uint64_t	stx_size;
	uint64_t	stx_blocks;
	uint64_t	stx_attributes_mask;
	struct statx_timestamp	stx_atime;
	struct statx_timestamp	stx_btime;
	struct statx_timestamp	stx_ctime;
	struct statx_timestamp	stx_mtime;
	uint32_t	stx_rdev_major;
	uint32_t	stx_rdev_minor;
	uint32_t	stx_dev_major;
	uint32_t	stx_dev_minor;
	uint64_t	__spare2[14];
};

#define STATX_BASIC_STATS	0x000007ffU
#define STATX_BTIME		0x00000800U
#define STATX_ALL		0x00000fffU

#define STATX_ATTR_IMMUTABLE	0x00000010
#define STATX_ATTR_APPEND	0x00000020
#define STATX_ATTR_NODUMP	0x00000040

/*
 * syscall(2) with Linux's numbers, for the few tests that use them:
 *
 * - statx, the reference test/statx.c compares the opcode's result with.
 *   Without it the test passes without comparing anything.  This fills the
 *   structure from fstatat(2), by the rules of kqring_stat_to_statx().
 * - io_uring_setup and io_uring_register, in test/regbuf-merge.c, which
 *   would otherwise make whatever call has that number on FreeBSD.
 * - futex in a syzbot reproducer, through _umtx_op(2).
 */
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/umtx.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#define __NR_statx		(-1001)
#define __NR_io_uring_setup	(-1002)
#define __NR_io_uring_register	(-1003)
#define SYS_futex		(-1004)

struct io_uring_params;
int io_uring_setup(unsigned int entries, struct io_uring_params *p);
int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg,
    unsigned int nr_args);

static inline int
__kqring_test_statx(int dfd, const char *path, int flags, struct statx *stx)
{
	struct stat sb;

	if (fstatat(dfd, path, &sb, flags) != 0)
		return (-1);
	memset(stx, 0, sizeof(*stx));
	stx->stx_mask = STATX_BASIC_STATS;
	stx->stx_blksize = (uint32_t)sb.st_blksize;
	stx->stx_nlink = (uint32_t)sb.st_nlink;
	stx->stx_uid = sb.st_uid;
	stx->stx_gid = sb.st_gid;
	stx->stx_mode = (uint16_t)sb.st_mode;
	stx->stx_ino = sb.st_ino;
	stx->stx_size = (uint64_t)sb.st_size;
	stx->stx_blocks = (uint64_t)sb.st_blocks;
	stx->stx_atime.tv_sec = sb.st_atim.tv_sec;
	stx->stx_atime.tv_nsec = (uint32_t)sb.st_atim.tv_nsec;
	stx->stx_mtime.tv_sec = sb.st_mtim.tv_sec;
	stx->stx_mtime.tv_nsec = (uint32_t)sb.st_mtim.tv_nsec;
	stx->stx_ctime.tv_sec = sb.st_ctim.tv_sec;
	stx->stx_ctime.tv_nsec = (uint32_t)sb.st_ctim.tv_nsec;
	if (sb.st_birthtim.tv_sec != (time_t)-1) {
		stx->stx_mask |= STATX_BTIME;
		stx->stx_btime.tv_sec = sb.st_birthtim.tv_sec;
		stx->stx_btime.tv_nsec = (uint32_t)sb.st_birthtim.tv_nsec;
	}
	stx->stx_rdev_major = (uint32_t)major(sb.st_rdev);
	stx->stx_rdev_minor = (uint32_t)minor(sb.st_rdev);
	stx->stx_dev_major = (uint32_t)major(sb.st_dev);
	stx->stx_dev_minor = (uint32_t)minor(sb.st_dev);
	stx->stx_attributes_mask = STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND |
	    STATX_ATTR_NODUMP;
	if ((sb.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) != 0)
		stx->stx_attributes |= STATX_ATTR_IMMUTABLE;
	if ((sb.st_flags & (UF_APPEND | SF_APPEND)) != 0)
		stx->stx_attributes |= STATX_ATTR_APPEND;
	if ((sb.st_flags & UF_NODUMP) != 0)
		stx->stx_attributes |= STATX_ATTR_NODUMP;
	return (0);
}

static inline long
__kqring_test_syscall(long n, ...)
{
	long a[6];
	va_list ap;

	va_start(ap, n);
	for (int i = 0; i < 6; i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	switch (n) {
	case __NR_statx:
		return (__kqring_test_statx((int)a[0], (const char *)a[1],
		    (int)a[2], (struct statx *)a[4]));
	case __NR_io_uring_setup:
		return (io_uring_setup((unsigned int)a[0],
		    (struct io_uring_params *)a[1]));
	case __NR_io_uring_register:
		return (io_uring_register((unsigned int)a[0],
		    (unsigned int)a[1], (const void *)a[2],
		    (unsigned int)a[3]));
	case SYS_futex:
		/* FUTEX_WAKE is 1 and FUTEX_WAIT 0, private or not. */
		if ((a[1] & 1) != 0)
			return (_umtx_op((void *)a[0], UMTX_OP_WAKE_PRIVATE,
			    INT_MAX, NULL, NULL));
		return (_umtx_op((void *)a[0], UMTX_OP_WAIT_UINT_PRIVATE,
		    (u_long)a[2], a[3] != 0 ?
		    (void *)sizeof(struct timespec) : NULL, (void *)a[3]));
	}
	return (syscall((int)n, a[0], a[1], a[2], a[3], a[4], a[5]));
}
#define syscall(...)	__kqring_test_syscall(__VA_ARGS__)

/* The System V form, which takes no arguments, as glibc has it. */
#define setpgrp()	setpgid(0, 0)

#endif
