/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Enough of Linux's <linux/nvme_ioctl.h> for fio's io_uring_cmd engine to
 * compile.  kqring has no URING_CMD for NVMe: that engine fails at run time.
 */
#ifndef FIO_FREEBSD_LINUX_NVME_IOCTL_H
#define FIO_FREEBSD_LINUX_NVME_IOCTL_H
#include <sys/ioccom.h>
#include <linux/types.h>

struct nvme_passthru_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32	result;
};

#define NVME_IOCTL_ID		_IO('N', 0x40)
#define NVME_IOCTL_ADMIN_CMD	_IOWR('N', 0x41, struct nvme_passthru_cmd)
#define NVME_IOCTL_IO_CMD	_IOWR('N', 0x43, struct nvme_passthru_cmd)
#endif
