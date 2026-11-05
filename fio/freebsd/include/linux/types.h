/* SPDX-License-Identifier: BSD-2-Clause */
/* What fio's os/linux/io_uring.h takes from Linux's <linux/types.h>. */
#ifndef FIO_FREEBSD_LINUX_TYPES_H
#define FIO_FREEBSD_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t		__u8;
typedef uint16_t	__u16;
typedef uint32_t	__u32;
typedef uint64_t	__u64;
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
