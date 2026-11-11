/* SPDX-License-Identifier: MIT */
/*
 * liburing's io_uring.h includes <linux/fs.h>; compat.h has the types it
 * needs.  Programs also use it for the block-device size ioctl.
 *
 * Not <sys/disk.h>: it includes <sys/param.h>, whose CACHE_LINE_SIZE, MIN,
 * MAX and the like would then come with every <liburing.h> (RocksDB, for
 * one, defines its own CACHE_LINE_SIZE).  Its two ioctls are copied here.
 */
#ifndef LIBURING_FREEBSD_LINUX_FS_H
#define LIBURING_FREEBSD_LINUX_FS_H

#include <sys/types.h>
#include <sys/ioctl.h>

/* A disk's sector size (u_int) and size in bytes (off_t), as <sys/disk.h>. */
#ifndef BLKSSZGET
#define BLKSSZGET	_IOR('d', 128, u_int)	/* DIOCGSECTORSIZE */
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64	_IOR('d', 129, off_t)	/* DIOCGMEDIASIZE */
#endif

#endif
