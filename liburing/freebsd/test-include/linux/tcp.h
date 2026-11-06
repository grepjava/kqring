/* SPDX-License-Identifier: MIT */
/*
 * For test/socket-getsetsock-cmd.c, stand-ins for two options FreeBSD lacks.
 * The test sets TCP_USER_TIMEOUT and reads it back, which any integer TCP
 * option does.  It compares SO_PEERNAME's value and length with getsockopt(2),
 * which any read-only option does.
 */
#ifndef LIBURING_FREEBSD_TEST_LINUX_TCP_H
#define LIBURING_FREEBSD_TEST_LINUX_TCP_H

#include <netinet/tcp.h>

#include <sys/socket.h>

#define TCP_USER_TIMEOUT	TCP_KEEPIDLE
#define SO_PEERNAME		SO_TYPE

#endif
