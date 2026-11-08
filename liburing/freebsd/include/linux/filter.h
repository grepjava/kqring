/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * <linux/filter.h> for liburing on FreeBSD: the classic BPF program types
 * that liburing.h names for IORING_REGISTER_BPF_FILTER, which kqring
 * refuses.  The layout is Linux's, the same as FreeBSD's struct bpf_insn.
 */
#ifndef _KQRING_LINUX_FILTER_H_
#define _KQRING_LINUX_FILTER_H_

#include <linux/types.h>
#include <net/bpf.h>	/* BPF_STMT(), BPF_RET, ... with Linux's values */

struct sock_filter {
	__u16	code;
	__u8	jt;
	__u8	jf;
	__u32	k;
};

struct sock_fprog {
	unsigned short		len;
	struct sock_filter	*filter;
};

#endif /* !_KQRING_LINUX_FILTER_H_ */
