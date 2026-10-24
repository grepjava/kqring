/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_FREEBSD_LINUX_SWAB_H
#define LIBURING_FREEBSD_LINUX_SWAB_H

#include <linux/types.h>

#define __swab16(x)	__builtin_bswap16(x)
#define __swab32(x)	__builtin_bswap32(x)
#define __swab64(x)	__builtin_bswap64(x)
/* Swap the 16-bit halves of a 32-bit word. */
#define __swahw32(x)	((__u32)(((__u32)(x) << 16) | ((__u32)(x) >> 16)))

#endif
