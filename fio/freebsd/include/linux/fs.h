/* SPDX-License-Identifier: BSD-2-Clause */
/* What fio's os/linux/io_uring.h and io_uring engine take from Linux. */
#ifndef FIO_FREEBSD_LINUX_FS_H
#define FIO_FREEBSD_LINUX_FS_H
typedef int	__kernel_rwf_t;
#define RWF_NOWAIT	0x00000008
#define RWF_ATOMIC	0x00000040
#define RWF_DONTCACHE	0x00000080
#endif
