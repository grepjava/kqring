/* SPDX-License-Identifier: MIT */
/* The two queue-size ioctls under Linux's names. */
#ifndef LIBURING_FREEBSD_TEST_LINUX_SOCKIOS_H
#define LIBURING_FREEBSD_TEST_LINUX_SOCKIOS_H

#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/sockio.h>

#define SIOCINQ		FIONREAD
#define SIOCOUTQ	FIONWRITE

#endif
