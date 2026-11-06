/* SPDX-License-Identifier: MIT */
/*
 * For tests that use a pthread mutex as a signal between threads: locked by
 * one thread, unlocked by another, then locked again by the first.  glibc
 * lets that pass.  FreeBSD's libthr refuses the unlock, so the first thread
 * runs on without waiting.  A semaphore does what these tests expect.
 */
#ifndef LIBURING_FREEBSD_TEST_MUTEX_SIGNAL_H
#define LIBURING_FREEBSD_TEST_MUTEX_SIGNAL_H

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

typedef struct {
	sem_t	sem;
} kq_signal_t;

static inline int
kq_signal_lock(kq_signal_t *m)
{
	while (sem_wait(&m->sem) != 0 && errno == EINTR)
		;
	return (0);
}

#define pthread_mutex_t			kq_signal_t
#define pthread_mutex_init(m, attr)	sem_init(&(m)->sem, 0, 1)
#define pthread_mutex_lock(m)		kq_signal_lock(m)
#define pthread_mutex_unlock(m)		sem_post(&(m)->sem)

#endif
