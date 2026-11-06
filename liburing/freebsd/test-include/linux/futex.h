/* SPDX-License-Identifier: MIT */
/* Linux's futex constants. The kqring opcodes take them as they are. */
#ifndef LIBURING_FREEBSD_TEST_LINUX_FUTEX_H
#define LIBURING_FREEBSD_TEST_LINUX_FUTEX_H

#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_PRIVATE_FLAG	128
#define FUTEX_BITSET_MATCH_ANY	0xffffffff

#endif
