# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD kernel module Makefile for kqring.ko
# Build with: make
# Load with:  sudo kldload ./kqring.ko

KMOD=	kqring
SRCS=	kqring_dev.c kqring_req.c kqring_rsrc.c kqring_register.c \
	kqring_sock.c kqring_file.c kqring_sync.c kqring_wq.c \
	kqring_zc.c vnode_if.h

CFLAGS+= -I${.CURDIR}/../include -Wall -Wextra -Werror
# FreeBSD kernel headers (e.g. sys/ktrace.h) have unused parameters.
CFLAGS+= -Wno-unused-parameter
# MAC framework checks (connect, setxattr) as in the syscalls; GENERIC has
# options MAC.  Build with WITHOUT_MAC=1 for a kernel without it.
.if !defined(WITHOUT_MAC)
CFLAGS+= -DKQRING_MAC
.endif

.include <bsd.kmod.mk>
