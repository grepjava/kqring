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

#endif
