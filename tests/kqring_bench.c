/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring_bench: kqring compared with kqueue(2) (and plain syscalls where
 * kqueue has nothing to offer) on the same workloads.
 *
 * Every result is the mean wall-clock time per operation, in nanoseconds,
 * printed as one line per measurement:
 *
 *	ROW <group> <variant> <ns/op>
 *
 * so that several runs can be aggregated (see BENCHMARK.MD).  An "op" is
 * one message delivered, one connection accepted, or one request completed,
 * as each group says.
 *
 * usage: kqring_bench [group ...]	(default: all groups)
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/kqring.h>

#define SQ_ENTRIES	1024
#define CQ_ENTRIES	8192
#define NBUFS		4096		/* provided buffers, group 0 */
#define BUF_SIZE	4096
#define MAX_FANIN	1024

/* ------------------------------------------------------------------------
 * A minimal ring (SINGLE_ISSUER | DEFER_TASKRUN, as Vega uses it).
 * ------------------------------------------------------------------------
 */

struct ring {
	int			fd;
	struct io_uring_sqe	*sqes;
	_Atomic unsigned	*sq_khead, *sq_ktail;
	_Atomic unsigned	*cq_khead, *cq_ktail;
	unsigned		sq_mask, cq_mask, sq_tail;
	struct io_uring_cqe	*cqes;
	struct io_uring_buf_ring *br;
	char			*bufs;
	uint16_t		br_tail;
};

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void
ring_init(struct ring *r)
{
	struct io_uring_params p;
	long page = sysconf(_SC_PAGESIZE);

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL;
	p.cq_entries = CQ_ENTRIES;
	memset(r, 0, sizeof(*r));
	r->fd = (int)kqring_setup(SQ_ENTRIES, &p);
	if (r->fd < 0) {
		perror("kqring_setup (is kqring.ko loaded?)");
		exit(1);
	}

	size_t ring_len = p.cq_off.cqes +
	    p.cq_entries * sizeof(struct io_uring_cqe);
	char *base = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    r->fd, IORING_OFF_SQ_RING);
	assert(base != MAP_FAILED);
	r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
	    PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQES);
	assert(r->sqes != MAP_FAILED);
	r->sq_khead = (_Atomic unsigned *)(base + p.sq_off.head);
	r->sq_ktail = (_Atomic unsigned *)(base + p.sq_off.tail);
	r->sq_mask = *(unsigned *)(base + p.sq_off.ring_mask);
	unsigned *sq_array = (unsigned *)(base + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		sq_array[i] = i;
	r->cq_khead = (_Atomic unsigned *)(base + p.cq_off.head);
	r->cq_ktail = (_Atomic unsigned *)(base + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(base + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);

	size_t br_len = ((NBUFS * sizeof(struct io_uring_buf)) + page - 1) &
	    ~(size_t)(page - 1);
	r->br = mmap(NULL, br_len, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	r->bufs = mmap(NULL, (size_t)NBUFS * BUF_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(r->br != MAP_FAILED && r->bufs != MAP_FAILED);
	struct io_uring_buf_reg reg = {
		.ring_addr = (uint64_t)(uintptr_t)r->br,
		.ring_entries = NBUFS,
		.bgid = 0,
	};
	assert(kqring_register(r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0);
	struct io_uring_buf *b = (struct io_uring_buf *)r->br;
	for (unsigned i = 0; i < NBUFS; i++) {
		b[i].addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)i * BUF_SIZE);
		b[i].len = BUF_SIZE;
		b[i].bid = (uint16_t)i;
	}
	r->br_tail = NBUFS;
	atomic_store_explicit((_Atomic uint16_t *)&r->br->tail, r->br_tail,
	    memory_order_release);
}

static void
ring_fini(struct ring *r)
{
	close(r->fd);
}

static void
ring_offer_buf(struct ring *r, uint16_t bid)
{
	struct io_uring_buf *b =
	    &((struct io_uring_buf *)r->br)[r->br_tail & (NBUFS - 1)];

	b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * BUF_SIZE);
	b->len = BUF_SIZE;
	b->bid = bid;
	r->br_tail++;
	atomic_store_explicit((_Atomic uint16_t *)&r->br->tail, r->br_tail,
	    memory_order_release);
}

static struct io_uring_sqe *
ring_prep(struct ring *r, uint8_t opcode, int fd, uint64_t user_data)
{
	struct io_uring_sqe *sqe = &r->sqes[r->sq_tail & r->sq_mask];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = opcode;
	sqe->fd = fd;
	sqe->user_data = user_data;
	r->sq_tail++;
	return (sqe);
}

/* Publish prepared SQEs, submit them, and wait for `min` CQEs. */
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

static unsigned
ring_ready(struct ring *r)
{
	return (atomic_load_explicit(r->cq_ktail, memory_order_acquire) -
	    atomic_load_explicit(r->cq_khead, memory_order_relaxed));
}

static struct io_uring_cqe
ring_pop(struct ring *r)
{
	unsigned h = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	struct io_uring_cqe c;

	assert(h != atomic_load_explicit(r->cq_ktail, memory_order_acquire));
	c = r->cqes[h & r->cq_mask];
	atomic_store_explicit(r->cq_khead, h + 1, memory_order_release);
	return (c);
}

/* Reap one CQE, check it, and give its provided buffer back. */
static void
ring_reap_buf(struct ring *r, int32_t want)
{
	struct io_uring_cqe c = ring_pop(r);

	assert(c.res == want);
	assert((c.flags & IORING_CQE_F_BUFFER) != 0);
	ring_offer_buf(r, (uint16_t)(c.flags >> IORING_CQE_BUFFER_SHIFT));
}

static void
ring_drain(struct ring *r)
{
	while (ring_ready(r) > 0) {
		struct io_uring_cqe c = ring_pop(r);

		if ((c.flags & IORING_CQE_F_BUFFER) != 0)
			ring_offer_buf(r,
			    (uint16_t)(c.flags >> IORING_CQE_BUFFER_SHIFT));
	}
}

static void
row(const char *group, const char *variant, uint64_t total_ns, uint64_t ops)
{
	printf("ROW %s %s %.1f\n", group, variant,
	    (double)total_ns / (double)ops);
	fflush(stdout);
}

static char payload[BUF_SIZE];

static void
xwrite(int fd, size_t len)
{
	ssize_t n = write(fd, payload, len);

	assert(n == (ssize_t)len);
}

/* ------------------------------------------------------------------------
 * floor: the cost of one empty kernel round trip.
 * ------------------------------------------------------------------------
 */

#define FLOOR_ITERS	500000
#define NOP_BATCH	32

static void
bench_floor(void)
{
	struct ring r;
	struct timespec zero = { 0, 0 };
	uint64_t t0;
	int kq = kqueue();

	assert(kq >= 0);
	t0 = now_ns();
	for (int i = 0; i < FLOOR_ITERS; i++)
		assert(kevent(kq, NULL, 0, NULL, 0, &zero) == 0);
	row("floor", "kqueue_empty_kevent", now_ns() - t0, FLOOR_ITERS);
	close(kq);

	ring_init(&r);
	t0 = now_ns();
	for (int i = 0; i < FLOOR_ITERS; i++) {
		ring_prep(&r, IORING_OP_NOP, -1, 0);
		ring_enter(&r, 1);
		(void)ring_pop(&r);
	}
	row("floor", "kqring_nop", now_ns() - t0, FLOOR_ITERS);

	t0 = now_ns();
	for (int i = 0; i < FLOOR_ITERS / NOP_BATCH; i++) {
		for (int j = 0; j < NOP_BATCH; j++)
			ring_prep(&r, IORING_OP_NOP, -1, 0);
		ring_enter(&r, NOP_BATCH);
		for (int j = 0; j < NOP_BATCH; j++)
			(void)ring_pop(&r);
	}
	row("floor", "kqring_nop_batch32", now_ns() - t0,
	    (FLOOR_ITERS / NOP_BATCH) * NOP_BATCH);

	/* IOSQE_ASYNC: the NOP makes a round trip through a worker thread. */
	t0 = now_ns();
	for (int i = 0; i < FLOOR_ITERS / 10; i++) {
		ring_prep(&r, IORING_OP_NOP, -1, 0)->flags = IOSQE_ASYNC;
		ring_enter(&r, 1);
		(void)ring_pop(&r);
	}
	row("floor", "kqring_nop_async", now_ns() - t0, FLOOR_ITERS / 10);
	ring_fini(&r);
}

/* ------------------------------------------------------------------------
 * pingpong: one message written, then received, on one connection.
 * ------------------------------------------------------------------------
 */

#define PP_ITERS	200000

static void
pp_kqueue(const char *group, int rx, int tx, size_t len)
{
	struct kevent kev, out;
	static char buf[BUF_SIZE];
	int kq = kqueue();
	uint64_t t0;

	assert(kq >= 0);
	EV_SET(&kev, rx, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);
	t0 = now_ns();
	for (int i = 0; i < PP_ITERS; i++) {
		xwrite(tx, len);
		assert(kevent(kq, NULL, 0, &out, 1, NULL) == 1);
		assert(recv(rx, buf, sizeof(buf), 0) == (ssize_t)len);
	}
	row(group, "kqueue", now_ns() - t0, PP_ITERS);
	close(kq);
}

static void
pp_plain(const char *group, int rx, int tx, size_t len)
{
	static char buf[BUF_SIZE];
	static bool warm;
	uint64_t t0 = now_ns();

	for (int i = 0; i < PP_ITERS; i++) {
		xwrite(tx, len);
		assert(recv(rx, buf, sizeof(buf), 0) == (ssize_t)len);
	}
	/* The first call only warms caches. */
	if (warm)
		row(group, "plain_recv", now_ns() - t0, PP_ITERS);
	warm = !warm;
}

/* Data is there before submission: the RECV completes inline. */
static void
pp_kqring_direct(const char *group, struct ring *r, int rx, int tx,
    size_t len)
{
	uint64_t t0 = now_ns();

	for (int i = 0; i < PP_ITERS; i++) {
		struct io_uring_sqe *sqe;

		xwrite(tx, len);
		sqe = ring_prep(r, IORING_OP_RECV, rx, 1);
		sqe->flags = IOSQE_BUFFER_SELECT;
		ring_enter(r, 1);
		ring_reap_buf(r, (int32_t)len);
	}
	row(group, "kqring_recv", now_ns() - t0, PP_ITERS);
}

/*
 * As pp_kqring_direct(), into one fixed buffer like the kqueue loop uses,
 * so that provided-buffer rotation (16 MiB of buffers) is not measured.
 */
static void
pp_kqring_direct_fixed(const char *group, struct ring *r, int rx, int tx,
    size_t len)
{
	static char buf[BUF_SIZE];
	uint64_t t0 = now_ns();

	for (int i = 0; i < PP_ITERS; i++) {
		struct io_uring_sqe *sqe;

		xwrite(tx, len);
		sqe = ring_prep(r, IORING_OP_RECV, rx, 1);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		ring_enter(r, 1);
		assert(ring_pop(r).res == (int32_t)len);
	}
	row(group, "kqring_recv_onebuf", now_ns() - t0, PP_ITERS);
}

/* RECV submitted first and armed; the write's upcall completes it. */
static void
pp_kqring_armed(const char *group, struct ring *r, int rx, int tx,
    size_t len)
{
	uint64_t t0 = now_ns();

	for (int i = 0; i < PP_ITERS; i++) {
		struct io_uring_sqe *sqe;

		sqe = ring_prep(r, IORING_OP_RECV, rx, 1);
		sqe->flags = IOSQE_BUFFER_SELECT;
		ring_enter(r, 0);
		xwrite(tx, len);
		ring_enter(r, 1);
		ring_reap_buf(r, (int32_t)len);
	}
	row(group, "kqring_recv_armed", now_ns() - t0, PP_ITERS);
}

/* One multishot RECV, armed once, delivers every message. */
static void
pp_kqring_multishot(const char *group, struct ring *r, int rx, int tx,
    size_t len)
{
	struct io_uring_sqe *sqe;
	uint64_t t0;

	sqe = ring_prep(r, IORING_OP_RECV, rx, 2);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->ioprio = IORING_RECV_MULTISHOT;
	ring_enter(r, 0);
	t0 = now_ns();
	for (int i = 0; i < PP_ITERS; i++) {
		xwrite(tx, len);
		ring_enter(r, 1);
		ring_reap_buf(r, (int32_t)len);
	}
	row(group, "kqring_multishot", now_ns() - t0, PP_ITERS);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 3);
	sqe->addr = 2;
	ring_enter(r, 2);
	ring_drain(r);
}

/*
 * `plain` adds the no-wait floor, write() then recv(), which needs the data
 * to be there as soon as write() returns: true of AF_UNIX, not of TCP.
 */
static void
pp_run(const char *group, int rx, int tx, size_t len, bool plain)
{
	struct ring r;

	ring_init(&r);
	if (plain) {
		pp_plain(group, rx, tx, len);	/* warm-up, discarded */
		pp_plain(group, rx, tx, len);
	}
	pp_kqueue(group, rx, tx, len);
	pp_kqring_direct(group, &r, rx, tx, len);
	pp_kqring_direct_fixed(group, &r, rx, tx, len);
	pp_kqring_armed(group, &r, rx, tx, len);
	pp_kqring_multishot(group, &r, rx, tx, len);
	ring_fini(&r);
}

static void
tcp_pair(int *a, int *b)
{
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	int one = 1, l, c;

	l = socket(AF_INET, SOCK_STREAM, 0);
	assert(l >= 0);
	assert(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(l, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(listen(l, 1) == 0);
	assert(getsockname(l, (struct sockaddr *)&sin, &slen) == 0);
	c = socket(AF_INET, SOCK_STREAM, 0);
	assert(c >= 0);
	assert(connect(c, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	*a = accept(l, NULL, NULL);
	assert(*a >= 0);
	close(l);
	assert(setsockopt(*a, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
	assert(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
	assert(fcntl(*a, F_SETFL, O_NONBLOCK) == 0);
	*b = c;
}

static void
bench_pingpong(void)
{
	static const size_t sizes[] = { 64, 1024, 4096 };
	char group[64];
	int sv[2];

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0,
		    sv) == 0);
		snprintf(group, sizeof(group), "pingpong_unix_%zu", sizes[i]);
		pp_run(group, sv[0], sv[1], sizes[i], true);
		close(sv[0]);
		close(sv[1]);
	}
	tcp_pair(&sv[0], &sv[1]);
	pp_run("pingpong_tcp_64", sv[0], sv[1], 64, false);
	close(sv[0]);
	close(sv[1]);
}

/* ------------------------------------------------------------------------
 * fanin: N connections each get one message; collect all N.  Per op = per
 * message.  This is the shape of a busy server: many ready sockets per
 * wakeup.
 * ------------------------------------------------------------------------
 */

#define FANIN_MSGS	524288		/* messages per variant */
#define FANIN_LEN	64

static void
bench_fanin_n(int n)
{
	static struct kevent evs[MAX_FANIN];
	static char buf[BUF_SIZE];
	int rx[MAX_FANIN], tx[MAX_FANIN], sv[2];
	int rounds = FANIN_MSGS / n;
	char group[64];
	struct ring r;
	uint64_t t0;

	snprintf(group, sizeof(group), "fanin_%d", n);
	for (int i = 0; i < n; i++) {
		assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0,
		    sv) == 0);
		rx[i] = sv[0];
		tx[i] = sv[1];
	}

	/* kqueue: one kevent() returns all N ready sockets, then N recv(). */
	int kq = kqueue();
	assert(kq >= 0);
	for (int i = 0; i < n; i++) {
		EV_SET(&evs[i], rx[i], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0,
		    NULL);
	}
	assert(kevent(kq, evs, n, NULL, 0, NULL) == 0);
	t0 = now_ns();
	for (int k = 0; k < rounds; k++) {
		int got = 0;

		for (int i = 0; i < n; i++)
			xwrite(tx[i], FANIN_LEN);
		while (got < n) {
			int m = kevent(kq, NULL, 0, evs, n, NULL);

			assert(m > 0);
			for (int j = 0; j < m; j++)
				assert(recv((int)evs[j].ident, buf,
				    sizeof(buf), 0) == FANIN_LEN);
			got += m;
		}
	}
	row(group, "kqueue", now_ns() - t0, (uint64_t)rounds * n);
	close(kq);

	/* kqring: one multishot RECV per socket; one enter reaps all N. */
	ring_init(&r);
	for (int i = 0; i < n; i++) {
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_RECV, rx[i],
		    (uint64_t)i);

		sqe->flags = IOSQE_BUFFER_SELECT;
		sqe->ioprio = IORING_RECV_MULTISHOT;
	}
	ring_enter(&r, 0);
	t0 = now_ns();
	for (int k = 0; k < rounds; k++) {
		for (int i = 0; i < n; i++)
			xwrite(tx[i], FANIN_LEN);
		ring_enter(&r, (unsigned)n);
		for (int i = 0; i < n; i++)
			ring_reap_buf(&r, FANIN_LEN);
	}
	row(group, "kqring_multishot", now_ns() - t0, (uint64_t)rounds * n);
	ring_fini(&r);

	/* kqring: N single-shot RECVs submitted and completed in one enter. */
	ring_init(&r);
	t0 = now_ns();
	for (int k = 0; k < rounds; k++) {
		for (int i = 0; i < n; i++)
			xwrite(tx[i], FANIN_LEN);
		for (int i = 0; i < n; i++) {
			struct io_uring_sqe *sqe = ring_prep(&r,
			    IORING_OP_RECV, rx[i], (uint64_t)i);

			sqe->flags = IOSQE_BUFFER_SELECT;
		}
		ring_enter(&r, (unsigned)n);
		for (int i = 0; i < n; i++)
			ring_reap_buf(&r, FANIN_LEN);
	}
	row(group, "kqring_recv_batch", now_ns() - t0, (uint64_t)rounds * n);
	ring_fini(&r);

	for (int i = 0; i < n; i++) {
		close(rx[i]);
		close(tx[i]);
	}
}

static void
bench_fanin(void)
{
	bench_fanin_n(16);
	bench_fanin_n(256);
	bench_fanin_n(1024);
}

/* ------------------------------------------------------------------------
 * accept: TCP loopback connection setup; per op = per connection.  The
 * client resets on close (SO_LINGER 0) so no TIME_WAIT builds up.
 * ------------------------------------------------------------------------
 */

#define ACCEPT_ITERS	20000

static int
accept_listener(struct sockaddr_in *sin)
{
	socklen_t slen = sizeof(*sin);
	int one = 1;
	int l = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	assert(l >= 0);
	assert(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(l, (struct sockaddr *)sin, sizeof(*sin)) == 0);
	assert(listen(l, 128) == 0);
	assert(getsockname(l, (struct sockaddr *)sin, &slen) == 0);
	return (l);
}

static int
accept_client(const struct sockaddr_in *sin)
{
	struct linger lg = { .l_onoff = 1, .l_linger = 0 };
	int c = socket(AF_INET, SOCK_STREAM, 0);

	assert(c >= 0);
	assert(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
	assert(connect(c, (const struct sockaddr *)sin, sizeof(*sin)) == 0);
	return (c);
}

static void
bench_accept(void)
{
	struct sockaddr_in sin;
	struct kevent kev, out;
	struct ring r;
	uint64_t t0;
	int l, c, a;

	/*
	 * Floor: a blocking accept().  On loopback the handshake may finish
	 * just after connect() returns, so a non-blocking one can miss it.
	 */
	l = accept_listener(&sin);
	assert(fcntl(l, F_SETFL, 0) == 0);
	t0 = now_ns();
	for (int i = 0; i < ACCEPT_ITERS; i++) {
		c = accept_client(&sin);
		a = accept(l, NULL, NULL);
		assert(a >= 0);
		close(a);
		close(c);
	}
	row("accept", "plain_accept", now_ns() - t0, ACCEPT_ITERS);
	assert(fcntl(l, F_SETFL, O_NONBLOCK) == 0);

	int kq = kqueue();
	assert(kq >= 0);
	EV_SET(&kev, l, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);
	t0 = now_ns();
	for (int i = 0; i < ACCEPT_ITERS; i++) {
		c = accept_client(&sin);
		assert(kevent(kq, NULL, 0, &out, 1, NULL) == 1);
		a = accept(l, NULL, NULL);
		assert(a >= 0);
		close(a);
		close(c);
	}
	row("accept", "kqueue", now_ns() - t0, ACCEPT_ITERS);
	close(kq);

	ring_init(&r);
	struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_ACCEPT, l, 7);
	sqe->ioprio = IORING_ACCEPT_MULTISHOT;
	ring_enter(&r, 0);
	t0 = now_ns();
	for (int i = 0; i < ACCEPT_ITERS; i++) {
		c = accept_client(&sin);
		ring_enter(&r, 1);
		struct io_uring_cqe cq = ring_pop(&r);
		assert(cq.res >= 0 && (cq.flags & IORING_CQE_F_MORE) != 0);
		close(cq.res);
		close(c);
	}
	row("accept", "kqring_multishot", now_ns() - t0, ACCEPT_ITERS);
	ring_fini(&r);
	close(l);
}

/* ------------------------------------------------------------------------
 * pipe: a message through a pipe.  kqring runs blocking-pipe reads on a
 * worker thread, so this measures that hand-off.
 * ------------------------------------------------------------------------
 */

#define PIPE_ITERS	100000
#define PIPE_LEN	64

static void
bench_pipe(void)
{
	static char buf[BUF_SIZE];
	struct kevent kev, out;
	struct ring r;
	uint64_t t0;
	int p[2];

	assert(pipe2(p, O_NONBLOCK) == 0);
	t0 = now_ns();
	for (int i = 0; i < PIPE_ITERS; i++) {
		xwrite(p[1], PIPE_LEN);
		assert(read(p[0], buf, sizeof(buf)) == PIPE_LEN);
	}
	row("pipe", "plain_read", now_ns() - t0, PIPE_ITERS);

	int kq = kqueue();
	assert(kq >= 0);
	EV_SET(&kev, p[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);
	t0 = now_ns();
	for (int i = 0; i < PIPE_ITERS; i++) {
		xwrite(p[1], PIPE_LEN);
		assert(kevent(kq, NULL, 0, &out, 1, NULL) == 1);
		assert(read(p[0], buf, sizeof(buf)) == PIPE_LEN);
	}
	row("pipe", "kqueue", now_ns() - t0, PIPE_ITERS);
	close(kq);
	close(p[0]);
	close(p[1]);

	/* A blocking pipe, as a READ that has to wait would use. */
	assert(pipe(p) == 0);
	ring_init(&r);
	t0 = now_ns();
	for (int i = 0; i < PIPE_ITERS; i++) {
		xwrite(p[1], PIPE_LEN);
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_READ, p[0], 1);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->off = (uint64_t)-1;
		ring_enter(&r, 1);
		assert(ring_pop(&r).res == PIPE_LEN);
	}
	row("pipe", "kqring_read", now_ns() - t0, PIPE_ITERS);
	close(p[0]);
	close(p[1]);

	/* A non-blocking pipe never waits, so the READ runs inline. */
	assert(pipe2(p, O_NONBLOCK) == 0);
	t0 = now_ns();
	for (int i = 0; i < PIPE_ITERS; i++) {
		xwrite(p[1], PIPE_LEN);
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_READ, p[0], 1);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->off = (uint64_t)-1;
		ring_enter(&r, 1);
		assert(ring_pop(&r).res == PIPE_LEN);
	}
	row("pipe", "kqring_read_nonblock", now_ns() - t0, PIPE_ITERS);
	ring_fini(&r);
	close(p[0]);
	close(p[1]);
}

/* ------------------------------------------------------------------------
 * file: 4 KiB random reads from a 64 MiB file in the page cache.  kqueue
 * cannot wait on regular files, so the comparison is pread(2).
 * ------------------------------------------------------------------------
 */

#define FILE_SIZE	(64U << 20)
#define FILE_BLK	4096
#define FILE_ITERS	200000
#define FILE_BATCH	32

static void
bench_file(void)
{
	char path[] = "/tmp/kqring_bench.XXXXXX";
	static char buf[FILE_BATCH][FILE_BLK];
	uint32_t seed = 12345;
	struct ring r;
	uint64_t t0;
	off_t off;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	unlink(path);
	memset(payload, 'f', sizeof(payload));
	for (off = 0; off < FILE_SIZE; off += FILE_BLK)
		assert(pwrite(fd, payload, FILE_BLK, off) == FILE_BLK);
	for (off = 0; off < FILE_SIZE; off += FILE_BLK)	/* warm the cache */
		assert(pread(fd, buf[0], FILE_BLK, off) == FILE_BLK);

#define NEXT_OFF()							\
	(seed = seed * 1103515245U + 12345U,				\
	(off_t)((seed >> 8) % (FILE_SIZE / FILE_BLK)) * FILE_BLK)

	t0 = now_ns();
	for (int i = 0; i < FILE_ITERS; i++)
		assert(pread(fd, buf[0], FILE_BLK, NEXT_OFF()) == FILE_BLK);
	row("file_4k", "pread", now_ns() - t0, FILE_ITERS);

	ring_init(&r);
	t0 = now_ns();
	for (int i = 0; i < FILE_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_READ, fd, 1);
		sqe->addr = (uint64_t)(uintptr_t)buf[0];
		sqe->len = FILE_BLK;
		sqe->off = (uint64_t)NEXT_OFF();
		ring_enter(&r, 1);
		assert(ring_pop(&r).res == FILE_BLK);
	}
	row("file_4k", "kqring_read", now_ns() - t0, FILE_ITERS);

	t0 = now_ns();
	for (int i = 0; i < FILE_ITERS / FILE_BATCH; i++) {
		for (int j = 0; j < FILE_BATCH; j++) {
			struct io_uring_sqe *sqe = ring_prep(&r,
			    IORING_OP_READ, fd, 1);
			sqe->addr = (uint64_t)(uintptr_t)buf[j];
			sqe->len = FILE_BLK;
			sqe->off = (uint64_t)NEXT_OFF();
		}
		ring_enter(&r, FILE_BATCH);
		for (int j = 0; j < FILE_BATCH; j++)
			assert(ring_pop(&r).res == FILE_BLK);
	}
	row("file_4k", "kqring_read_batch32", now_ns() - t0,
	    (FILE_ITERS / FILE_BATCH) * FILE_BATCH);
	ring_fini(&r);
#undef NEXT_OFF
	close(fd);
}

/* ------------------------------------------------------------------------
 * send: bulk messages on one connection; per op = per message.  Each
 * message is sent and then read back in full with recv(MSG_WAITALL).
 * kqueue has nothing to add while the send buffer has room, so the
 * baseline is write(2).  The zero-copy variants rotate through ZC_SLOTS
 * buffers and only reuse one once its IORING_CQE_F_NOTIF has arrived, as
 * an application must.  Registering ZC_SLOTS x 256 KiB needs RLIMIT_MEMLOCK
 * above the 64 KiB default: run under ulimit -l 8192.
 * ------------------------------------------------------------------------
 */

#define SEND_BYTES	(2ULL << 30)	/* bytes per variant */
#define SEND_MAX	(256U << 10)
#define SEND_SOCKBUF	(1U << 20)
#define ZC_SLOTS	8

static char *send_src;			/* ZC_SLOTS x SEND_MAX */
static char send_dst[SEND_MAX];

static void
send_recv_all(int rx, size_t len)
{
	assert(recv(rx, send_dst, len, MSG_WAITALL) == (ssize_t)len);
}

static void
send_plain(const char *group, int rx, int tx, size_t len, int iters)
{
	uint64_t t0 = now_ns();

	for (int i = 0; i < iters; i++) {
		assert(write(tx, send_src, len) == (ssize_t)len);
		send_recv_all(rx, len);
	}
	row(group, "plain_write", now_ns() - t0, (uint64_t)iters);
}

static void
send_kqring(const char *group, struct ring *r, int rx, int tx, size_t len,
    int iters)
{
	uint64_t t0 = now_ns();

	for (int i = 0; i < iters; i++) {
		struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_SEND, tx, 1);

		sqe->addr = (uint64_t)(uintptr_t)send_src;
		sqe->len = (uint32_t)len;
		ring_enter(r, 1);
		assert(ring_pop(r).res == (int32_t)len);
		send_recv_all(rx, len);
	}
	row(group, "kqring_send", now_ns() - t0, (uint64_t)iters);
}

/*
 * Reap what is ready.  A send's first CQE carries IORING_CQE_F_MORE; its
 * notification (user_data = slot) frees the slot for reuse.
 */
static void
send_zc_reap(struct ring *r, bool *busy, int *sent, size_t len)
{
	while (ring_ready(r) > 0) {
		struct io_uring_cqe c = ring_pop(r);

		if ((c.flags & IORING_CQE_F_NOTIF) != 0) {
			/* Never the copy fallback: we are measuring zero-copy. */
			assert(c.res == 0);
			busy[c.user_data] = false;
		} else {
			assert(c.res == (int32_t)len);
			assert((c.flags & IORING_CQE_F_MORE) != 0);
			(*sent)++;
		}
	}
}

static void
send_kqring_zc(const char *group, struct ring *r, int rx, int tx, size_t len,
    int iters, bool fixed)
{
	bool busy[ZC_SLOTS] = { false };
	int sent = 0;
	uint64_t t0 = now_ns();

	for (int i = 0; i < iters; i++) {
		int slot = i % ZC_SLOTS;
		struct io_uring_sqe *sqe;

		while (busy[slot]) {
			ring_enter(r, 1);
			send_zc_reap(r, busy, &sent, len);
		}
		sqe = ring_prep(r, IORING_OP_SEND_ZC, tx, (uint64_t)slot);
		sqe->addr = (uint64_t)(uintptr_t)(send_src +
		    (size_t)slot * SEND_MAX);
		sqe->len = (uint32_t)len;
		sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
		if (fixed) {
			sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
			sqe->buf_index = (uint16_t)slot;
		}
		busy[slot] = true;
		ring_enter(r, 0);
		while (sent <= i) {
			ring_enter(r, 1);
			send_zc_reap(r, busy, &sent, len);
		}
		send_recv_all(rx, len);
		send_zc_reap(r, busy, &sent, len);
	}
	for (int s = 0; s < ZC_SLOTS; s++) {
		while (busy[s]) {
			ring_enter(r, 1);
			send_zc_reap(r, busy, &sent, len);
		}
	}
	row(group, fixed ? "kqring_send_zc_fixed" : "kqring_send_zc",
	    now_ns() - t0, (uint64_t)iters);
}

static void
send_run(const char *transport, int rx, int tx)
{
	static const size_t sizes[] = { 16U << 10, 64U << 10, SEND_MAX };
	int sz = SEND_SOCKBUF;
	char group[64];
	struct ring r;

	assert(fcntl(rx, F_SETFL, 0) == 0);
	assert(fcntl(tx, F_SETFL, 0) == 0);
	assert(setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0);
	assert(setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	ring_init(&r);
	struct iovec iov[ZC_SLOTS];
	for (int s = 0; s < ZC_SLOTS; s++) {
		iov[s].iov_base = send_src + (size_t)s * SEND_MAX;
		iov[s].iov_len = SEND_MAX;
	}
	if (kqring_register(r.fd, IORING_REGISTER_BUFFERS, iov,
	    ZC_SLOTS) != 0) {
		perror("IORING_REGISTER_BUFFERS (raise ulimit -l to 8192)");
		exit(1);
	}
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int iters = (int)(SEND_BYTES / sizes[i]);

		snprintf(group, sizeof(group), "send_%s_%zuk", transport,
		    sizes[i] >> 10);
		send_plain(group, rx, tx, sizes[i], iters);
		send_kqring(group, &r, rx, tx, sizes[i], iters);
		send_kqring_zc(group, &r, rx, tx, sizes[i], iters, true);
		send_kqring_zc(group, &r, rx, tx, sizes[i], iters, false);
	}
	ring_fini(&r);
}

static void
bench_send(void)
{
	int sv[2];

	send_src = mmap(NULL, (size_t)ZC_SLOTS * SEND_MAX,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(send_src != MAP_FAILED);
	memset(send_src, 's', (size_t)ZC_SLOTS * SEND_MAX);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	send_run("unix", sv[0], sv[1]);
	close(sv[0]);
	close(sv[1]);
	tcp_pair(&sv[0], &sv[1]);
	send_run("tcp", sv[0], sv[1]);
	close(sv[0]);
	close(sv[1]);
	munmap(send_src, (size_t)ZC_SLOTS * SEND_MAX);
}

/* ------------------------------------------------------------------------
 * timer: a 1 us one-shot timer, armed and fired.
 * ------------------------------------------------------------------------
 */

#define TIMER_ITERS	100000

static void
bench_timer(void)
{
	struct __kernel_timespec ts = { 0, 0 };
	struct kevent kev, out;
	struct ring r;
	uint64_t t0;
	int kq = kqueue();

	assert(kq >= 0);
	t0 = now_ns();
	for (int i = 0; i < TIMER_ITERS; i++) {
		EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS,
		    1, NULL);
		assert(kevent(kq, &kev, 1, &out, 1, NULL) == 1);
	}
	row("timer_1us", "kqueue", now_ns() - t0, TIMER_ITERS);
	close(kq);

	ts.tv_nsec = 1000;
	ring_init(&r);
	t0 = now_ns();
	for (int i = 0; i < TIMER_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_TIMEOUT, -1,
		    1);
		sqe->addr = (uint64_t)(uintptr_t)&ts;
		sqe->len = 1;
		ring_enter(&r, 1);
		assert(ring_pop(&r).res == -ETIME);
	}
	row("timer_1us", "kqring_timeout", now_ns() - t0, TIMER_ITERS);
	ring_fini(&r);
}

static const struct {
	const char	*name;
	void		(*fn)(void);
} groups[] = {
	{ "floor",	bench_floor },
	{ "pingpong",	bench_pingpong },
	{ "fanin",	bench_fanin },
	{ "accept",	bench_accept },
	{ "pipe",	bench_pipe },
	{ "file",	bench_file },
	{ "send",	bench_send },
	{ "timer",	bench_timer },
};

int
main(int argc, char **argv)
{
	memset(payload, 'x', sizeof(payload));
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		bool want = argc < 2;

		for (int j = 1; j < argc; j++)
			want |= strcmp(argv[j], groups[i].name) == 0;
		if (want)
			groups[i].fn();
	}
	return (0);
}
