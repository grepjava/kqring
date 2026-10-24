/* SPDX-License-Identifier: MIT */
/* The <linux/types.h> fixed-width types liburing's io_uring.h uses. */
#ifndef LIBURING_FREEBSD_LINUX_TYPES_H
#define LIBURING_FREEBSD_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t		__u8;
typedef uint16_t	__u16;
typedef uint32_t	__u32;
typedef uint64_t	__u64;
typedef int8_t		__s8;
typedef int16_t		__s16;
typedef int32_t		__s32;
typedef int64_t		__s64;
typedef uint16_t	__le16;
typedef uint32_t	__le32;
typedef uint64_t	__le64;
typedef uint16_t	__be16;
typedef uint32_t	__be32;
typedef uint64_t	__be64;
typedef uint64_t	__aligned_u64 __attribute__((aligned(8)));

#endif
