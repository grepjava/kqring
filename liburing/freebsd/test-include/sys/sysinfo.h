/* SPDX-License-Identifier: MIT */
/* glibc's CPU counts. */
#ifndef LIBURING_FREEBSD_TEST_SYS_SYSINFO_H
#define LIBURING_FREEBSD_TEST_SYS_SYSINFO_H

#include <unistd.h>

static inline int
get_nprocs(void)
{
	return ((int)sysconf(_SC_NPROCESSORS_ONLN));
}

static inline int
get_nprocs_conf(void)
{
	return ((int)sysconf(_SC_NPROCESSORS_CONF));
}

#endif
