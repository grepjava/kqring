/* SPDX-License-Identifier: MIT */
/* Enough of epoll(7) over kqueue(2) for the tests: level-triggered read and
 * write interest, each its own kevent.  An "epoll" descriptor is a kqueue, so
 * polling one through a ring polls a kqueue. */
#ifndef LIBURING_FREEBSD_TEST_SYS_EPOLL_H
#define LIBURING_FREEBSD_TEST_SYS_EPOLL_H

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>

#define EPOLLIN		0x001u
#define EPOLLOUT	0x004u
#define EPOLLERR	0x008u
#define EPOLLHUP	0x010u
#define EPOLLONESHOT	(1u << 30)
#define EPOLLET		(1u << 31)

#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3

typedef union epoll_data {
	void		*ptr;
	int		fd;
	uint32_t	u32;
	uint64_t	u64;
} epoll_data_t;

struct epoll_event {
	uint32_t	events;
	epoll_data_t	data;
} __packed;

static inline int
epoll_create1(int flags)
{
	return (kqueue());
}

static inline int
epoll_create(int size)
{
	return (kqueue());
}

static inline int
epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
	struct kevent kev[2];
	u_short flags = op == EPOLL_CTL_DEL ? EV_DELETE : EV_ADD;
	int n = 0;

	if (op != EPOLL_CTL_DEL) {
		if ((ev->events & EPOLLET) != 0)
			flags |= EV_CLEAR;
		if ((ev->events & EPOLLONESHOT) != 0)
			flags |= EV_ONESHOT;
	}
	if (op == EPOLL_CTL_DEL || (ev->events & EPOLLIN) != 0) {
		EV_SET(&kev[n], fd, EVFILT_READ, flags, 0, 0,
		    ev != NULL ? (void *)(uintptr_t)ev->data.u64 : NULL);
		n++;
	}
	if (op != EPOLL_CTL_DEL && (ev->events & EPOLLOUT) != 0) {
		EV_SET(&kev[n], fd, EVFILT_WRITE, flags, 0, 0,
		    (void *)(uintptr_t)ev->data.u64);
		n++;
	}
	return (kevent(epfd, kev, n, NULL, 0, NULL) < 0 ? -1 : 0);
}

static inline int
epoll_wait(int epfd, struct epoll_event *evs, int max, int timeout)
{
	struct kevent kev[16];
	struct timespec ts = {
		.tv_sec = timeout / 1000,
		.tv_nsec = (long)(timeout % 1000) * 1000000
	};
	int n;

	if (max > 16)
		max = 16;
	n = kevent(epfd, NULL, 0, kev, max, timeout < 0 ? NULL : &ts);
	for (int i = 0; i < n; i++) {
		evs[i].events = kev[i].filter == EVFILT_READ ? EPOLLIN : EPOLLOUT;
		if ((kev[i].flags & EV_EOF) != 0)
			evs[i].events |= EPOLLHUP;
		evs[i].data.u64 = (uint64_t)(uintptr_t)kev[i].udata;
	}
	return (n);
}

#endif
