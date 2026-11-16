/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * How long a completion waits when the thread that completed it then
 * blocks.  A RECV is armed on one end of an AF_UNIX stream socket pair; a
 * second thread of the process writes 1 MiB to the other end.  Its first
 * bytes complete the RECV, from inside that write(2), which then blocks:
 * the socket's buffers hold far less.  On a ring without DEFER_TASKRUN the
 * completion runs as task work of the writing thread (Linux) or at its
 * return to user space (kqring), so how soon the CQE appears depends on
 * what interrupts or replaces that wait.  The main thread watches for the
 * CQE, then reads the socket dry so that the write can finish, and records
 * what write(2) returned.
 *
 * Built and run the same way on Linux io_uring and on kqring:
 *
 *   FreeBSD: make -C tests kqring_twlat
 *   Linux:   cc -O2 -pthread -o kqring_twlat tests/kqring_twlat.c
 *
 *   kqring_twlat [self-]spin|poll|enter [rounds [bytes]]
 *
 * spin watches the CQ ring from user space, as a program polling its CQ
 * does; poll waits in poll(2) on the ring's descriptor, as an event loop
 * does; enter waits in io_uring_enter(2).  With self-, the writing thread
 * submits the RECV itself, just before its write, so that the request's
 * submitter is the thread that then blocks (io_uring gives the
 * completion to the submitting task).  XBENCH_DEFER=1 and XBENCH_COOP=1
 * set the ring up with SINGLE_ISSUER | DEFER_TASKRUN or COOP_TASKRUN (a
 * DEFER_TASKRUN ring only completes in an enter, so only "enter" applies).
 * A CQE not seen within 5 seconds fails the run.
 *
 * Output: the CQE's delay after the write began, in microseconds (median,
 * 90th and 99th percentile, maximum), and how many writes returned in
 * full, short, or failed.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#define ring_sys_setup(n, p)	((int)syscall(__NR_io_uring_setup, (n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)syscall(__NR_io_uring_enter, (fd), (s), (w), (f), (a), (z)))
#else
#include <sys/kqring.h>
#define ring_sys_setup(n, p)	((int)kqring_setup((n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)kqring_enter((fd), (s), (w), (f), (a), (z)))
#endif

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__,	\
		    #x, strerror(errno));				\
		exit(2);						\
	}								\
} while (0)

struct ring {
	int			fd;
	struct io_uring_sqe	*sqes;
	_Atomic unsigned	*sq_tail, *cq_head, *cq_tail;
	struct io_uring_cqe	*cqes;
	unsigned		sq_mask, cq_mask;
	unsigned		tail;
};

static const char *
ring_init(struct ring *r, unsigned entries)
{
	struct io_uring_params p;
	const char *kind = "default";
	size_t sq_len, cq_len, len;
	unsigned *array;
	char *b;

	memset(&p, 0, sizeof(p));
	memset(r, 0, sizeof(*r));
	if (getenv("XBENCH_DEFER") != NULL &&
	    strcmp(getenv("XBENCH_DEFER"), "1") == 0) {
		p.flags = IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN;
		kind = "defer";
	} else if (getenv("XBENCH_COOP") != NULL &&
	    strcmp(getenv("XBENCH_COOP"), "1") == 0) {
		p.flags = IORING_SETUP_COOP_TASKRUN;
		kind = "coop";
	}
	r->fd = ring_sys_setup(entries, &p);
	CHECK(r->fd >= 0);
	sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	cq_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	len = sq_len > cq_len ? sq_len : cq_len;
	b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd,
	    IORING_OFF_SQ_RING);
	CHECK(b != MAP_FAILED);
	r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
	    PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQES);
	CHECK(r->sqes != MAP_FAILED);
	r->sq_tail = (_Atomic unsigned *)(b + p.sq_off.tail);
	r->sq_mask = *(unsigned *)(b + p.sq_off.ring_mask);
	r->cq_head = (_Atomic unsigned *)(b + p.cq_off.head);
	r->cq_tail = (_Atomic unsigned *)(b + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(b + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(b + p.cq_off.cqes);
	array = (unsigned *)(b + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		array[i] = i;
	return (kind);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static struct ring r;
static int self;
static char rbuf[64];
static int sv[2], gop[2];
static size_t wlen = 1 << 20;
static char *wbuf;
static _Atomic uint64_t w_t0;
static _Atomic int w_done;
static ssize_t w_ret;
static int w_errno;

static int round_no;

/* Arm a RECV of rbuf on sv[1]. */
static void
submit_recv(void)
{
	struct io_uring_sqe *s = &r.sqes[r.tail & r.sq_mask];

	memset(s, 0, sizeof(*s));
	s->opcode = IORING_OP_RECV;
	s->fd = sv[1];
	s->addr = (uint64_t)(uintptr_t)rbuf;
	s->len = sizeof(rbuf);
	s->user_data = (uint64_t)round_no;
	r.tail++;
	atomic_store_explicit(r.sq_tail, r.tail, memory_order_release);
	CHECK(ring_sys_enter(r.fd, 1, 0, 0, NULL, 0) == 1);
}

/* The writer: one write(2) of wlen bytes per byte read from gop[0]. */
static void *
writer(void *arg)
{
	char c;

	(void)arg;
	while (read(gop[0], &c, 1) == 1) {
		ssize_t n;

		if (self)
			submit_recv();
		atomic_store_explicit(&w_t0, now_ns(), memory_order_release);
		n = write(sv[0], wbuf, wlen);
		w_errno = n < 0 ? errno : 0;
		w_ret = n;
		atomic_store_explicit(&w_done, 1, memory_order_release);
	}
	return (NULL);
}

static int
cmpu(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "spin";
	int rounds = argc > 2 ? atoi(argv[2]) : 200;
	unsigned long full = 0, shortw = 0, failed = 0;
	static char dbuf[65536];
	uint64_t *lat;
	struct utsname u;
	const char *kind;
	pthread_t th;

	if (argc > 3)
		wlen = (size_t)strtoul(argv[3], NULL, 0);
	CHECK(rounds > 0 && wlen > sizeof(rbuf));
	if (strncmp(mode, "self-", 5) == 0) {
		self = 1;
		mode += 5;
	}
	CHECK(strcmp(mode, "spin") == 0 || strcmp(mode, "poll") == 0 ||
	    strcmp(mode, "enter") == 0);
	CHECK((lat = calloc((size_t)rounds, sizeof(*lat))) != NULL);
	CHECK((wbuf = malloc(wlen)) != NULL);
	memset(wbuf, 'w', wlen);
	CHECK(uname(&u) == 0);
	kind = ring_init(&r, 8);
	if (strcmp(kind, "defer") == 0 && (self || strcmp(mode, "enter") != 0)) {
		fprintf(stderr, "a DEFER_TASKRUN ring completes only in an "
		    "enter: use enter\n");
		return (2);
	}
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
	CHECK(pipe(gop) == 0);
	CHECK(pthread_create(&th, NULL, writer, NULL) == 0);

	for (int i = 0; i < rounds; i++) {
		unsigned head, tail;
		uint64_t t0, tc, deadline;
		size_t got = 0;
		char c = 'g';

		round_no = i;
		if (!self)
			submit_recv();

		atomic_store_explicit(&w_done, 0, memory_order_relaxed);
		atomic_store_explicit(&w_t0, 0, memory_order_relaxed);
		CHECK(write(gop[1], &c, 1) == 1);

		/* Wait for the CQE, as the mode says. */
		deadline = now_ns() + 5000000000ULL;
		head = atomic_load_explicit(r.cq_head, memory_order_relaxed);
		for (;;) {
			tail = atomic_load_explicit(r.cq_tail,
			    memory_order_acquire);
			if (tail != head)
				break;
			if (now_ns() > deadline) {
				fprintf(stderr, "FAIL: round %d: no CQE in 5 s "
				    "(%s ring, %s)\n", i, kind, mode);
				return (1);
			}
			if (strcmp(mode, "poll") == 0) {
				struct pollfd pf = { .fd = r.fd,
				    .events = POLLIN };

				CHECK(poll(&pf, 1, 100) >= 0);
			} else if (strcmp(mode, "enter") == 0) {
				int e = ring_sys_enter(r.fd, 0, 1,
				    IORING_ENTER_GETEVENTS, NULL, 0);

				CHECK(e >= 0 || errno == EINTR);
			}
		}
		tc = now_ns();
		while ((t0 = atomic_load_explicit(&w_t0,
		    memory_order_acquire)) == 0)
			;
		lat[i] = tc > t0 ? tc - t0 : 0;
		CHECK(r.cqes[head & r.cq_mask].res == (int)sizeof(rbuf));
		atomic_store_explicit(r.cq_head, head + 1,
		    memory_order_release);

		/* Read the rest, so that the write finishes. */
		for (;;) {
			ssize_t n = read(sv[1], dbuf, sizeof(dbuf));

			if (n > 0) {
				got += (size_t)n;
				continue;
			}
			CHECK(n < 0 && errno == EAGAIN);
			if (atomic_load_explicit(&w_done,
			    memory_order_acquire) && (w_ret < 0 ||
			    got + sizeof(rbuf) >= (size_t)w_ret))
				break;
		}
		if (w_ret < 0)
			failed++;
		else if ((size_t)w_ret < wlen)
			shortw++;
		else
			full++;
		if (w_ret < 0 && i == 0)
			fprintf(stderr, "write: %s\n", strerror(w_errno));
	}
	close(gop[1]);
	CHECK(pthread_join(th, NULL) == 0);
	qsort(lat, (size_t)rounds, sizeof(*lat), cmpu);
	printf("# %s %s %s\n", u.sysname, u.release, u.machine);
	printf("twlat %s ring, %s%s, %d rounds, %zu-byte writes: CQE after "
	    "%.1f us (p90 %.1f, p99 %.1f, max %.1f); writes full %lu, "
	    "short %lu, failed %lu\n", kind, self ? "self-" : "", mode, rounds,
	    wlen,
	    lat[rounds / 2] / 1e3, lat[rounds * 9 / 10] / 1e3,
	    lat[rounds * 99 / 100] / 1e3, lat[rounds - 1] / 1e3, full, shortw,
	    failed);
	return (0);
}
