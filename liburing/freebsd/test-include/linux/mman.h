/* SPDX-License-Identifier: MIT */
/* MAP_HUGETLB and friends are in test-compat.h. */
#ifndef LIBURING_FREEBSD_TEST_LINUX_MMAN_H
#define LIBURING_FREEBSD_TEST_LINUX_MMAN_H

/* A hint the test doesn't depend on; FreeBSD has no such advice. */
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE	MADV_NORMAL
#endif

#endif
