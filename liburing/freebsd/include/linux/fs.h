/* SPDX-License-Identifier: MIT */
/*
 * liburing's io_uring.h includes <linux/fs.h>; compat.h has the types it
 * needs.  Programs also use it for the block-device size ioctl.
 */
#ifndef LIBURING_FREEBSD_LINUX_FS_H
#define LIBURING_FREEBSD_LINUX_FS_H

#include <sys/ioctl.h>
#include <sys/disk.h>

/* A disk's size in bytes (a 64-bit value), as DIOCGMEDIASIZE. */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64	DIOCGMEDIASIZE
#endif

#endif
