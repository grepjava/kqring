# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD kernel module Makefile for kqring.ko
# Build with: make
# Load with:  sudo kldload ./kqring.ko

KMOD=	kqring
SRCS=	kqring.c vnode_if.h

CFLAGS+= -I${.CURDIR}/../include -Wall -Wextra -Werror
# FreeBSD kernel headers (e.g. sys/ktrace.h) have unused parameters.
CFLAGS+= -Wno-unused-parameter

.include <bsd.kmod.mk>
