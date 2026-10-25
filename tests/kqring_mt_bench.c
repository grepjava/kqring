/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * kqring_mt_bench: multi-threaded scaling of kqring against kqueue(2) and
 * plain system calls.  Each thread has its own ring (or kqueue), as in the
 * thread-per-core model SINGLE_ISSUER rings are built for, and is pinned to
 * its own CPU.  Every workload runs with 1, 2, 4 ... up to the CPU count
 * threads for a fixed time; the result is the aggregate rate.
 *
 *	ROW <workload> <variant> <threads> <aggregate ops/s> <ns/op/thread>
 *
 * usage: kqring_mt_bench [seconds per run]	(default 1)
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/cpuset.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/kqring.h>

#define BATCH		32
#define FILE_SIZE	(64 << 20)
#define FILE_BLK	4096
#define MSG_LEN		64

struct ring {
	int			fd;
	struct io_uring_sqe	*sqes;
	_Atomic unsigned	*sq_khead, *sq_ktail, *cq_khead, *cq_ktail;
	unsigned		sq_mask, cq_mask, sq_tail;
	struct io_uring_cqe	*cqes;
};

static void
ring_init(struct ring *r)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	memset(r, 0, sizeof(*r));
	r->fd = (int)kqring_setup(256, &p);
	assert(r->fd >= 0);
	size_t len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	char *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd,
	    IORING_OFF_SQ_RING);
	r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
	    PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQES);
	assert(base != MAP_FAILED && r->sqes != MAP_FAILED);
	r->sq_khead = (_Atomic unsigned *)(base + p.sq_off.head);
	r->sq_ktail = (_Atomic unsigned *)(base + p.sq_off.tail);
	r->sq_mask = *(unsigned *)(base + p.sq_off.ring_mask);
	unsigned *arr = (unsigned *)(base + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		arr[i] = i;
	r->cq_khead = (_Atomic unsigned *)(base + p.cq_off.head);
	r->cq_ktail = (_Atomic unsigned *)(base + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(base + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
}

static struct io_uring_sqe *
ring_prep(struct ring *r, uint8_t opcode, int fd)
{
	struct io_uring_sqe *sqe = &r->sqes[r->sq_tail++ & r->sq_mask];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = opcode;
	sqe->fd = fd;
	return (sqe);
}

static void
ring_enter(struct ring *r, unsigned min)
{
	atomic_store_explicit(r->sq_ktail, r->sq_tail, memory_order_release);
	unsigned n = r->sq_tail -
	    atomic_load_explicit(r->sq_khead, memory_order_acquire);
	long rc = kqring_enter(r->fd, n, min,
	    min != 0 ? IORING_ENTER_GETEVENTS : 0, NULL, 0);
	assert(rc == (long)n);
}

static int32_t
ring_pop(struct ring *r)
{
	unsigned h = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	int32_t res;

	assert(h != atomic_load_explicit(r->cq_ktail, memory_order_acquire));
	res = r->cqes[h & r->cq_mask].res;
	atomic_store_explicit(r->cq_khead, h + 1, memory_order_release);
	return (res);
}

/* ------------------------------------------------------------------------
 * Workloads: one call does `ops` operations; per-thread state in *ws.
 * ------------------------------------------------------------------------
 */

struct wstate {
	struct ring	r;
	int		kq, sv[2], file;
	uint32_t	seed;
	char		buf[BATCH][FILE_BLK] __attribute__((aligned(4096)));
};

static int shared_file = -1;

static off_t
next_off(struct wstate *ws)
{
	ws->seed = ws->seed * 1103515245U + 12345U;
	return ((off_t)((ws->seed >> 8) % (FILE_SIZE / FILE_BLK)) * FILE_BLK);
}

static int
w_kevent(struct wstate *ws)
{
	struct timespec zero = { 0, 0 };

	assert(kevent(ws->kq, NULL, 0, NULL, 0, &zero) == 0);
	return (1);
}

static int
w_nop(struct wstate *ws)
{
	ring_prep(&ws->r, IORING_OP_NOP, -1);
	ring_enter(&ws->r, 1);
	(void)ring_pop(&ws->r);
	return (1);
}

static int
w_nop_batch(struct wstate *ws)
{
	for (int i = 0; i < BATCH; i++)
		ring_prep(&ws->r, IORING_OP_NOP, -1);
	ring_enter(&ws->r, BATCH);
	for (int i = 0; i < BATCH; i++)
		(void)ring_pop(&ws->r);
	return (BATCH);
}

static int
w_pp_kqueue(struct wstate *ws)
{
	struct kevent ev;

	assert(write(ws->sv[1], ws->buf[0], MSG_LEN) == MSG_LEN);
	assert(kevent(ws->kq, NULL, 0, &ev, 1, NULL) == 1);
	assert(recv(ws->sv[0], ws->buf[1], MSG_LEN, 0) == MSG_LEN);
	return (1);
}

static int
w_pp_kqring(struct wstate *ws)
{
	struct io_uring_sqe *sqe;

	assert(write(ws->sv[1], ws->buf[0], MSG_LEN) == MSG_LEN);
	sqe = ring_prep(&ws->r, IORING_OP_RECV, ws->sv[0]);
	sqe->addr = (uint64_t)(uintptr_t)ws->buf[1];
	sqe->len = MSG_LEN;
	ring_enter(&ws->r, 1);
	assert(ring_pop(&ws->r) == MSG_LEN);
	return (1);
}

static int
w_pread(struct wstate *ws)
{
	assert(pread(shared_file, ws->buf[0], FILE_BLK, next_off(ws)) ==
	    FILE_BLK);
	return (1);
}

static int
w_read(struct wstate *ws)
{
	struct io_uring_sqe *sqe = ring_prep(&ws->r, IORING_OP_READ,
	    shared_file);

	sqe->addr = (uint64_t)(uintptr_t)ws->buf[0];
	sqe->len = FILE_BLK;
	sqe->off = (uint64_t)next_off(ws);
	ring_enter(&ws->r, 1);
	assert(ring_pop(&ws->r) == FILE_BLK);
	return (1);
}

static int
w_read_batch(struct wstate *ws)
{
	for (int i = 0; i < BATCH; i++) {
		struct io_uring_sqe *sqe = ring_prep(&ws->r, IORING_OP_READ,
		    shared_file);

		sqe->addr = (uint64_t)(uintptr_t)ws->buf[i];
		sqe->len = FILE_BLK;
		sqe->off = (uint64_t)next_off(ws);
	}
	ring_enter(&ws->r, BATCH);
	for (int i = 0; i < BATCH; i++)
		assert(ring_pop(&ws->r) == FILE_BLK);
	return (BATCH);
}

static const struct workload {
	const char	*name, *variant;
	int		(*fn)(struct wstate *);
	bool		ring, kq, pair;
} workloads[] = {
	{ "floor", "kqueue_empty_kevent", w_kevent, false, true, false },
	{ "floor", "kqring_nop", w_nop, true, false, false },
	{ "floor", "kqring_nop_batch32", w_nop_batch, true, false, false },
	{ "pingpong_unix_64", "kqueue", w_pp_kqueue, false, true, true },
	{ "pingpong_unix_64", "kqring_recv_onebuf", w_pp_kqring, true, false,
	    true },
	{ "file_4k", "pread", w_pread, false, false, false },
	{ "file_4k", "kqring_read", w_read, true, false, false },
	{ "file_4k", "kqring_read_batch32", w_read_batch, true, false, false },
};

/* ------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------
 */

struct thr {
	pthread_t		td;
	int			cpu;
	const struct workload	*w;
	uint64_t		ops, ns;
};

static pthread_barrier_t start_barrier;
static _Atomic bool stop;
static double run_secs = 1.0;

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void *
thr_main(void *arg)
{
	struct thr *t = arg;
	const struct workload *w = t->w;
	struct wstate *ws;
	cpuset_t mask;
	uint64_t t0, ops = 0;

	CPU_SETOF(t->cpu, &mask);
	assert(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) ==
	    0);
	ws = aligned_alloc(4096, roundup(sizeof(*ws), 4096));
	assert(ws != NULL);
	memset(ws, 0, sizeof(*ws));
	ws->seed = 12345U + (uint32_t)t->cpu;
	if (w->ring)
		ring_init(&ws->r);
	if (w->kq) {
		ws->kq = kqueue();
		assert(ws->kq >= 0);
	}
	if (w->pair) {
		struct kevent kev;

		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ws->sv) == 0);
		if (w->kq) {
			EV_SET(&kev, ws->sv[0], EVFILT_READ, EV_ADD | EV_CLEAR,
			    0, 0, NULL);
			assert(kevent(ws->kq, &kev, 1, NULL, 0, NULL) == 0);
		}
	}
	/* The ring is created and owned by this (pinned) thread. */
	pthread_barrier_wait(&start_barrier);
	t0 = now_ns();
	while (!atomic_load_explicit(&stop, memory_order_relaxed))
		for (int i = 0; i < 64; i++)
			ops += (uint64_t)w->fn(ws);
	t->ns = now_ns() - t0;
	t->ops = ops;
	if (w->ring)
		close(ws->r.fd);
	if (w->kq)
		close(ws->kq);
	if (w->pair) {
		close(ws->sv[0]);
		close(ws->sv[1]);
	}
	free(ws);
	return (NULL);
}

static void
run(const struct workload *w, int nthreads)
{
	struct thr *t = calloc((size_t)nthreads, sizeof(*t));
	double rate = 0, nsop = 0;

	assert(t != NULL);
	atomic_store(&stop, false);
	assert(pthread_barrier_init(&start_barrier, NULL,
	    (unsigned)nthreads + 1) == 0);
	for (int i = 0; i < nthreads; i++) {
		t[i].cpu = i;
		t[i].w = w;
		assert(pthread_create(&t[i].td, NULL, thr_main, &t[i]) == 0);
	}
	pthread_barrier_wait(&start_barrier);
	usleep((useconds_t)(run_secs * 1e6));
	atomic_store(&stop, true);
	for (int i = 0; i < nthreads; i++) {
		pthread_join(t[i].td, NULL);
		rate += (double)t[i].ops * 1e9 / (double)t[i].ns;
		nsop += (double)t[i].ns / (double)t[i].ops;
	}
	pthread_barrier_destroy(&start_barrier);
	printf("ROW %s %s %d %.0f %.1f\n", w->name, w->variant, nthreads, rate,
	    nsop / nthreads);
	fflush(stdout);
	free(t);
}

int
main(int argc, char **argv)
{
	static char blk[FILE_BLK];
	char path[] = "/tmp/kqring_mt_bench.XXXXXX";
	int ncpu;
	size_t len = sizeof(ncpu);

	if (argc > 1)
		run_secs = atof(argv[1]);
	assert(sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0);

	shared_file = mkstemp(path);
	assert(shared_file >= 0);
	unlink(path);
	memset(blk, 'm', sizeof(blk));
	for (off_t off = 0; off < FILE_SIZE; off += FILE_BLK)
		assert(pwrite(shared_file, blk, FILE_BLK, off) == FILE_BLK);
	for (off_t off = 0; off < FILE_SIZE; off += FILE_BLK)
		assert(pread(shared_file, blk, FILE_BLK, off) == FILE_BLK);

	for (size_t i = 0; i < nitems(workloads); i++)
		for (int n = 1; n <= ncpu; n *= 2)
			run(&workloads[i], n);
	close(shared_file);
	return (0);
}
