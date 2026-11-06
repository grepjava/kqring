/* SPDX-License-Identifier: MIT */
/* The one prctl(2) the syzbot reproducers make, through procctl(2). */
#ifndef LIBURING_FREEBSD_TEST_SYS_PRCTL_H
#define LIBURING_FREEBSD_TEST_SYS_PRCTL_H

#include <sys/types.h>
#include <sys/procctl.h>
#include <errno.h>
#include <stdarg.h>

#define PR_SET_PDEATHSIG	1

static inline int
prctl(int option, ...)
{
	va_list ap;
	int sig;

	if (option != PR_SET_PDEATHSIG) {
		errno = EINVAL;
		return (-1);
	}
	va_start(ap, option);
	sig = (int)va_arg(ap, unsigned long);
	va_end(ap);
	return (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig));
}

#endif
