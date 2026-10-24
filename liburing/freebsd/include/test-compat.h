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

#endif
