/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * fio's io_uring engine on FreeBSD, through kqring.ko.  Included by
 * engines/io_uring.c after its other headers.
 *
 * kqring registers kqring_setup(2), kqring_enter(2) and kqring_register(2)
 * in free system-call slots when it loads; their numbers are in sysctl
 * kern.kqring.sys_{setup,enter,register}.  They take io_uring_setup(2),
 * io_uring_enter(2) and io_uring_register(2)'s arguments, so the engine's
 * calls only need the number looked up.
 */
#ifndef FIO_FREEBSD_KQRING_SYSCALL_H
#define FIO_FREEBSD_KQRING_SYSCALL_H

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE	0
#endif

#undef FIO_ARCH_HAS_SYSCALL
#undef __NR_io_uring_setup
#undef __NR_io_uring_enter
#undef __NR_io_uring_register
#define __NR_io_uring_setup	0
#define __NR_io_uring_enter	1
#define __NR_io_uring_register	2

static long
kqring_fio_syscall(int which, ...)
{
	static const char *const names[] = {
		"kern.kqring.sys_setup",
		"kern.kqring.sys_enter",
		"kern.kqring.sys_register",
	};
	static int nums[3];
	int n = __atomic_load_n(&nums[which], __ATOMIC_RELAXED);
	long a[6];
	va_list ap;

	if (n == 0) {
		size_t len = sizeof(n);

		if (sysctlbyname(names[which], &n, &len, NULL, 0) != 0 ||
		    n <= 0) {
			errno = ENOSYS;
			return (-1);
		}
		__atomic_store_n(&nums[which], n, __ATOMIC_RELAXED);
	}
	/* Unused trailing arguments are read as garbage and ignored. */
	va_start(ap, which);
	for (int i = 0; i < 6; i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	return ((syscall)(n, a[0], a[1], a[2], a[3], a[4], a[5]));
}

#define syscall		kqring_fio_syscall

#endif
