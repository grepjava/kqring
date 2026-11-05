/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Stands in for Linux's <linux/bsg.h> so that fio's io_uring_cmd engine
 * compiles.  kqring has no URING_CMD for SCSI: that fails at run time.
 */
#ifndef FIO_FREEBSD_LINUX_BSG_H
#define FIO_FREEBSD_LINUX_BSG_H
#include <linux/types.h>
#endif
