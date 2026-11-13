/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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
 * A minimal ring (SINGLE_ISSUER | DEFER_TASKRUN, which kqring requires).
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
	unsigned		nbufs;		/* a power of two */
	size_t			buf_size, br_len;
	uint16_t		br_tail;
};

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/*
 * A ring with provided-buffer group 0 of nbufs buffers of size bytes.  The
 * group hands its buffers out in turn, so a group much larger than what is
 * in flight sends every receive to memory that is out of cache.
 */
static void
ring_init_bufs(struct ring *r, unsigned nbufs, size_t size)
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

	assert(nbufs != 0 && (nbufs & (nbufs - 1)) == 0);
	r->nbufs = nbufs;
	r->buf_size = size;
	r->br_len = ((nbufs * sizeof(struct io_uring_buf)) + page - 1) &
	    ~(size_t)(page - 1);
	r->br = mmap(NULL, r->br_len, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	r->bufs = mmap(NULL, (size_t)nbufs * size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(r->br != MAP_FAILED && r->bufs != MAP_FAILED);
	struct io_uring_buf_reg reg = {
		.ring_addr = (uint64_t)(uintptr_t)r->br,
		.ring_entries = nbufs,
		.bgid = 0,
	};
	assert(kqring_register(r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0);
	struct io_uring_buf *b = (struct io_uring_buf *)r->br;
	for (unsigned i = 0; i < nbufs; i++) {
		b[i].addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)i * size);
		b[i].len = (uint32_t)size;
		b[i].bid = (uint16_t)i;
	}
	r->br_tail = (uint16_t)nbufs;
	atomic_store_explicit((_Atomic uint16_t *)&r->br->tail, r->br_tail,
	    memory_order_release);
}

/* 4,096 buffers of 4 KiB, for groups that don't size their own. */
static void
ring_init(struct ring *r)
{
	ring_init_bufs(r, NBUFS, BUF_SIZE);
}

static void
ring_fini(struct ring *r)
{
	close(r->fd);
	munmap(r->br, r->br_len);
	munmap(r->bufs, (size_t)r->nbufs * r->buf_size);
}

static void
ring_offer_buf(struct ring *r, uint16_t bid)
{
	struct io_uring_buf *b =
	    &((struct io_uring_buf *)r->br)[r->br_tail & (r->nbufs - 1)];

	b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * r->buf_size);
	b->len = (uint32_t)r->buf_size;
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
#define PP_NBUFS	16		/* provided buffers: one in flight */

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

	/* Few buffers, so that they stay in cache as kqueue's one does. */
	ring_init_bufs(&r, PP_NBUFS, BUF_SIZE);
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

	/*
	 * kqring: one multishot RECV per socket; one enter reaps all N.  The
	 * group has a buffer of the message's size for each of the N in
	 * flight, the least a ring can receive them into.
	 */
	ring_init_bufs(&r, (unsigned)n, FANIN_LEN);
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
	ring_init_bufs(&r, (unsigned)n, FANIN_LEN);
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

	/* The same, busy-waiting up to 50 us before sleeping. */
	struct io_uring_napi napi = {
		.busy_poll_to = 50,
		.op_param = IO_URING_NAPI_TRACKING_DYNAMIC,
	};
	assert(kqring_register(r.fd, IORING_REGISTER_NAPI, &napi, 1) == 0);
	t0 = now_ns();
	for (int i = 0; i < ACCEPT_ITERS; i++) {
		c = accept_client(&sin);
		ring_enter(&r, 1);
		struct io_uring_cqe cq = ring_pop(&r);
		assert(cq.res >= 0 && (cq.flags & IORING_CQE_F_MORE) != 0);
		close(cq.res);
		close(c);
	}
	row("accept", "kqring_multishot_busypoll", now_ns() - t0,
	    ACCEPT_ITERS);
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
 *
 * On ZFS a cached read runs at one of two speeds.  Until the transaction
 * group that holds the writes syncs (vfs.zfs.txg.timeout, 5 s), the dirty
 * records stay in memory uncompressed.  After it they live in the dbuf
 * cache, which keeps at most 1/2^vfs.zfs.dbuf.cache_shift (1/32) of the
 * ARC target: if the file doesn't fit, every read evicts a 128 KiB record
 * and rebuilds another from the ARC (decompressing or copying all 128 KiB),
 * at about 18 us a read on a Ryzen 3 PRO 4350G against 1.3 us.
 * So on ZFS the group waits for the sync, to measure the state that lasts,
 * and prints the dbuf cache target, which says which speed was measured.
 * ------------------------------------------------------------------------
 */

#define FILE_SIZE	(64U << 20)
#define FILE_BLK	4096
#define FILE_ITERS	200000
#define FILE_BATCH	32

static uint64_t
sysctl_u64(const char *name)
{
	union { uint64_t u64; uint32_t u32; } v = { 0 };
	size_t len = sizeof(v);

	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return (0);
	return (len == sizeof(v.u32) ? v.u32 : v.u64);
}

static bool
file_on_zfs(int fd)
{
	struct statfs sf;

	assert(fstatfs(fd, &sf) == 0);
	return (strcmp(sf.f_fstypename, "zfs") == 0);
}

static void
file_dbuf_note(const char *when)
{
	printf("# file_4k %s: ZFS dbuf cache target %ju MiB, ARC target %ju "
	    "MiB, file %u MiB\n", when, (uintmax_t)(sysctl_u64(
	    "kstat.zfs.misc.dbufstats.cache_target_bytes") >> 20),
	    (uintmax_t)(sysctl_u64("kstat.zfs.misc.arcstats.c") >> 20),
	    FILE_SIZE >> 20);
}

static void
bench_file(void)
{
	char path[PATH_MAX];
	const char *dir = getenv("KQRING_BENCH_FILE_DIR");
	static char buf[FILE_BATCH][FILE_BLK];
	uint32_t seed = 12345;
	struct ring r;
	uint64_t t0;
	off_t off;
	bool zfs;
	int fd, n;

	n = snprintf(path, sizeof(path), "%s/kqring_bench.XXXXXX",
	    dir != NULL ? dir : "/tmp");
	assert(n > 0 && (size_t)n < sizeof(path));
	fd = mkstemp(path);
	assert(fd >= 0);
	unlink(path);
	memset(payload, 'f', sizeof(payload));
	for (off = 0; off < FILE_SIZE; off += FILE_BLK)
		assert(pwrite(fd, payload, FILE_BLK, off) == FILE_BLK);
	zfs = file_on_zfs(fd);
	if (zfs) {
		/* Past the sync of the writes' transaction group. */
		u_int wait = (u_int)sysctl_u64("vfs.zfs.txg.timeout") + 2;

		assert(fsync(fd) == 0);
		sleep(wait < 3 ? 3 : wait);
	}
	for (off = 0; off < FILE_SIZE; off += FILE_BLK)	/* warm the cache */
		assert(pread(fd, buf[0], FILE_BLK, off) == FILE_BLK);
	if (zfs)
		file_dbuf_note("start");

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

	/* A registered file skips the descriptor-table lookups. */
	assert(kqring_register(r.fd, IORING_REGISTER_FILES, &fd, 1) == 0);
	t0 = now_ns();
	for (int i = 0; i < FILE_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_prep(&r, IORING_OP_READ, 0, 1);
		sqe->flags = IOSQE_FIXED_FILE;
		sqe->addr = (uint64_t)(uintptr_t)buf[0];
		sqe->len = FILE_BLK;
		sqe->off = (uint64_t)NEXT_OFF();
		ring_enter(&r, 1);
		assert(ring_pop(&r).res == FILE_BLK);
	}
	row("file_4k", "kqring_read_fixed", now_ns() - t0, FILE_ITERS);

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
	if (zfs)
		file_dbuf_note("end");
	close(fd);
}

/* ------------------------------------------------------------------------
 * send: bulk messages on one connection; per op = per message.  Each
 * message is sent and then read back in full with recv(MSG_WAITALL).
 * kqueue has nothing to add while the send buffer has room, so the
 * baseline is write(2).  The zero-copy variants rotate through ZC_SLOTS
 * buffers and only reuse one once its IORING_CQE_F_NOTIF has arrived, as
 * an application must.  Registering ZC_SLOTS x 256 KiB needs RLIMIT_MEMLOCK
 * above the 64 KiB default: run under ulimit -l 8192.  On AF_UNIX, kqring
 * copies SEND_ZC from an unregistered buffer below
 * kern.kqring.zc_local_min_len, because there the copy is cheaper; those
 * rows measure that.
 * ------------------------------------------------------------------------
 */

#define SEND_BYTES	(2ULL << 30)	/* bytes per variant */
#define SEND_MAX	(256U << 10)
#define SEND_SOCKBUF	(1U << 20)
#define ZC_SLOTS	8

static char *send_src;			/* ZC_SLOTS x SEND_MAX */
static char send_dst[SEND_MAX];
static bool send_may_copy;		/* AF_UNIX below the threshold */
static int send_zc_copied;		/* other copies: short remainders */

static void
send_recv_all(int rx, size_t len)
{
	if (rx < 0)		/* the stream group: another thread receives */
		return;
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

/*
 * A send on a stream socket may be short when the receiver is draining it
 * concurrently (the stream group): without MSG_WAITALL, io_uring reports
 * what was sent, and the program sends the rest, as here.
 */
static void
send_kqring(const char *group, struct ring *r, int rx, int tx, size_t len,
    int iters)
{
	uint64_t t0 = now_ns();

	for (int i = 0; i < iters; i++) {
		for (size_t off = 0; off < len;) {
			struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_SEND,
			    tx, 1);
			int32_t res;

			sqe->addr = (uint64_t)(uintptr_t)(send_src + off);
			sqe->len = (uint32_t)(len - off);
			ring_enter(r, 1);
			res = ring_pop(r).res;
			assert(res > 0 && (size_t)res <= len - off);
			off += (size_t)res;
		}
		send_recv_all(rx, len);
	}
	row(group, "kqring_send", now_ns() - t0, (uint64_t)iters);
}

/*
 * Reap what is ready.  A send's first CQE carries IORING_CQE_F_MORE and the
 * bytes sent; its notification (user_data = slot) is one fewer that the
 * slot waits for before reuse.  Returns whether a send's CQE came, in *res.
 */
static bool
send_zc_reap(struct ring *r, int *busy, int32_t *res)
{
	bool got = false;

	while (ring_ready(r) > 0) {
		struct io_uring_cqe c = ring_pop(r);

		if ((c.flags & IORING_CQE_F_NOTIF) != 0) {
			/*
			 * Zero-copy, unless kqring copies by design here or
			 * the rest of a short send was below a page.
			 */
			assert(c.res == 0 ||
			    c.res == (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
			if (c.res != 0 && !send_may_copy)
				send_zc_copied++;
			assert(busy[c.user_data] > 0);
			busy[c.user_data]--;
		} else {
			assert(c.res > 0);
			assert((c.flags & IORING_CQE_F_MORE) != 0);
			*res = c.res;
			got = true;
		}
	}
	return (got);
}

/*
 * With waitall, each SEND_ZC has MSG_WAITALL, and kqring sends the whole
 * message across as many attempts as the socket needs: one SQE and one CQE
 * per message.
 */
static void
send_kqring_zc(const char *group, struct ring *r, int rx, int tx, size_t len,
    int iters, bool fixed, bool waitall)
{
	const char *name = waitall ? "kqring_send_zc_fixed_waitall" :
	    fixed ? "kqring_send_zc_fixed" : "kqring_send_zc";
	int busy[ZC_SLOTS] = { 0 }, sends = 0;
	int32_t res = 0;
	uint64_t t0 = now_ns();

	send_zc_copied = 0;

	for (int i = 0; i < iters; i++) {
		int slot = i % ZC_SLOTS;
		char *base = send_src + (size_t)slot * SEND_MAX;

		while (busy[slot] > 0) {
			ring_enter(r, 1);
			(void)send_zc_reap(r, busy, &res);
		}
		for (size_t off = 0; off < len;) {
			struct io_uring_sqe *sqe = ring_prep(r,
			    IORING_OP_SEND_ZC, tx, (uint64_t)slot);

			sqe->addr = (uint64_t)(uintptr_t)(base + off);
			sqe->len = (uint32_t)(len - off);
			sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
			if (fixed) {
				sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
				sqe->buf_index = (uint16_t)slot;
			}
			if (waitall)
				sqe->msg_flags = MSG_WAITALL;
			busy[slot]++;
			sends++;
			ring_enter(r, 0);
			/* The send usually completes inline: reap first. */
			while (!send_zc_reap(r, busy, &res))
				ring_enter(r, 1);
			assert((size_t)res <= len - off);
			assert(!waitall || (size_t)res == len - off);
			off += (size_t)res;
		}
		send_recv_all(rx, len);
		(void)send_zc_reap(r, busy, &res);
	}
	for (int s = 0; s < ZC_SLOTS; s++) {
		while (busy[s] > 0) {
			ring_enter(r, 1);
			(void)send_zc_reap(r, busy, &res);
		}
	}
	row(group, name, now_ns() - t0, (uint64_t)iters);
	if (sends > iters || send_zc_copied > 0)
		printf("# %s %s: %d sends for %d messages, %d copied\n", group,
		    name, sends, iters, send_zc_copied);
}

static void
send_run(const char *transport, int rx, int tx)
{
	static const size_t sizes[] = { 16U << 10, 32U << 10, 64U << 10,
	    SEND_MAX };
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
		send_may_copy = strcmp(transport, "unix") == 0;
		send_plain(group, rx, tx, sizes[i], iters);
		send_kqring(group, &r, rx, tx, sizes[i], iters);
		send_kqring_zc(group, &r, rx, tx, sizes[i], iters, true,
		    false);
		send_kqring_zc(group, &r, rx, tx, sizes[i], iters, false,
		    false);
	}
	ring_fini(&r);
}

static void
bench_send_common(bool unix_only)
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
	if (!unix_only) {
		tcp_pair(&sv[0], &sv[1]);
		send_run("tcp", sv[0], sv[1]);
		close(sv[0]);
		close(sv[1]);
	}
	munmap(send_src, (size_t)ZC_SLOTS * SEND_MAX);
}

static void
bench_send(void)
{
	bench_send_common(false);
}

static void
bench_send_unix(void)
{
	bench_send_common(true);
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

/* ------------------------------------------------------------------------
 * disk: 4 KiB random reads and writes on a disk device, which kqring sends
 * to the driver as bios.  IOSQE_ASYNC rows force the worker path instead.
 * Needs a scratch device the benchmark may overwrite, named by
 * KQRING_BENCH_DISK (for example an md(4) malloc disk); skipped otherwise.
 * ------------------------------------------------------------------------
 */

#define DISK_ITERS	100000
#define DISK_BATCH	32
#define DISK_BLK	4096

static void
disk_ring(const char *group, const char *variant, struct ring *r, int fd,
    uint8_t opcode, uint8_t sqe_flags, int batch, off_t blocks)
{
	static char buf[DISK_BATCH][DISK_BLK] __attribute__((aligned(4096)));
	uint32_t seed = 4321;
	uint64_t t0 = now_ns();

	for (int i = 0; i < DISK_ITERS / batch; i++) {
		for (int j = 0; j < batch; j++) {
			struct io_uring_sqe *sqe = ring_prep(r, opcode, fd, 1);

			seed = seed * 1103515245U + 12345U;
			sqe->flags = sqe_flags;
			sqe->addr = (uint64_t)(uintptr_t)buf[j];
			sqe->len = DISK_BLK;
			sqe->off = (uint64_t)((seed >> 8) % blocks) * DISK_BLK;
		}
		ring_enter(r, (unsigned)batch);
		for (int j = 0; j < batch; j++) {
			while (ring_ready(r) == 0)
				ring_enter(r, 1);
			assert(ring_pop(r).res == DISK_BLK);
		}
	}
	row(group, variant, now_ns() - t0, (DISK_ITERS / batch) * batch);
}

static void
bench_disk(void)
{
	static char buf[DISK_BLK] __attribute__((aligned(4096)));
	const char *path = getenv("KQRING_BENCH_DISK");
	uint32_t seed = 4321;
	struct ring r;
	off_t size, blocks;
	uint64_t t0;
	int fd;

	if (path == NULL) {
		fprintf(stderr, "disk: KQRING_BENCH_DISK not set, skipped\n");
		return;
	}
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	size = lseek(fd, 0, SEEK_END);
	assert(size >= DISK_BLK);
	blocks = MIN(size, (off_t)64 << 20) / DISK_BLK;

	t0 = now_ns();
	for (int i = 0; i < DISK_ITERS; i++) {
		seed = seed * 1103515245U + 12345U;
		assert(pread(fd, buf, DISK_BLK,
		    (off_t)((seed >> 8) % blocks) * DISK_BLK) == DISK_BLK);
	}
	row("disk_read_4k", "pread", now_ns() - t0, DISK_ITERS);
	ring_init(&r);
	disk_ring("disk_read_4k", "kqring_read", &r, fd, IORING_OP_READ, 0, 1,
	    blocks);
	disk_ring("disk_read_4k", "kqring_read_worker", &r, fd,
	    IORING_OP_READ, IOSQE_ASYNC, 1, blocks);
	disk_ring("disk_read_4k", "kqring_read_batch32", &r, fd,
	    IORING_OP_READ, 0, DISK_BATCH, blocks);
	disk_ring("disk_read_4k", "kqring_read_worker_batch32", &r, fd,
	    IORING_OP_READ, IOSQE_ASYNC, DISK_BATCH, blocks);

	seed = 4321;
	t0 = now_ns();
	for (int i = 0; i < DISK_ITERS; i++) {
		seed = seed * 1103515245U + 12345U;
		assert(pwrite(fd, buf, DISK_BLK,
		    (off_t)((seed >> 8) % blocks) * DISK_BLK) == DISK_BLK);
	}
	row("disk_write_4k", "pwrite", now_ns() - t0, DISK_ITERS);
	disk_ring("disk_write_4k", "kqring_write", &r, fd, IORING_OP_WRITE, 0,
	    1, blocks);
	disk_ring("disk_write_4k", "kqring_write_worker", &r, fd,
	    IORING_OP_WRITE, IOSQE_ASYNC, 1, blocks);
	disk_ring("disk_write_4k", "kqring_write_batch32", &r, fd,
	    IORING_OP_WRITE, 0, DISK_BATCH, blocks);
	ring_fini(&r);
	close(fd);
}

/* ------------------------------------------------------------------------
 * echo: a server thread echoes each message back to a client that sends
 * one and waits for it (one op = one round trip, as the client sees it).
 *
 *   kqueue            kevent(2), then read(2) and write(2)
 *   kqring_recv_send  io_uring style: each RECV's CQE makes user space
 *                     submit the SEND and the next RECV (one enter)
 *   kqring_trigger    an event-driven chain (KQRING_EXT_TRIGGER): RECV ->
 *                     SEND with LEN_FROM_PREV, both CQE_SKIP_SUCCESS; the
 *                     server sits in one enter for the whole run, and no
 *                     CQE or system call is needed per message
 *
 * Each variant also reports <variant>_server_cpu: the server thread's CPU
 * time per message, which the client's wakeup latency doesn't hide.
 * ------------------------------------------------------------------------
 */

#define ECHO_ITERS	100000

enum { ECHO_KQUEUE, ECHO_RECV_SEND, ECHO_TRIGGER };

struct echo_srv {
	int		fd;
	int		variant;
	_Atomic int	ready;
	uint64_t	cpu_ns;		/* the server thread's CPU time */
};

static uint64_t
thread_cpu_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void *
echo_server(void *arg)
{
	struct echo_srv *e = arg;
	static char buf[BUF_SIZE];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct ring r;
	uint64_t cpu0 = thread_cpu_ns();

	switch (e->variant) {
	case ECHO_KQUEUE: {
		struct kevent kev, out;
		int kq = kqueue();

		assert(kq >= 0);
		EV_SET(&kev, e->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
		assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);
		atomic_store(&e->ready, 1);
		for (;;) {
			ssize_t n;

			assert(kevent(kq, NULL, 0, &out, 1, NULL) == 1);
			n = read(e->fd, buf, sizeof(buf));
			if (n < 0 && errno == EAGAIN)
				continue;
			if (n <= 0)
				break;
			assert(write(e->fd, buf, (size_t)n) == n);
		}
		close(kq);
		break;
	}
	case ECHO_RECV_SEND:
		ring_init(&r);
		atomic_store(&e->ready, 1);
		sqe = ring_prep(&r, IORING_OP_RECV, e->fd, 1);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		for (;;) {
			ring_enter(&r, 1);
			c = ring_pop(&r);
			if (c.res <= 0)
				break;
			sqe = ring_prep(&r, IORING_OP_SEND, e->fd, 2);
			sqe->addr = (uint64_t)(uintptr_t)buf;
			sqe->len = (uint32_t)c.res;
			sqe->msg_flags = MSG_WAITALL;
			sqe->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
			sqe = ring_prep(&r, IORING_OP_RECV, e->fd, 1);
			sqe->addr = (uint64_t)(uintptr_t)buf;
			sqe->len = sizeof(buf);
		}
		ring_fini(&r);
		break;
	case ECHO_TRIGGER: {
		struct kqring_ext_setup es = { .version = KQRING_EXT_VERSION };
		struct kqring_trigger t = { .nsteps = 2,
		    .step_flags = { 0, KQRING_STEP_LEN_FROM_PREV |
		    KQRING_STEP_STOP_ON_ZERO } };
		bool done = false;

		ring_init(&r);
		assert(ioctl(r.fd, KQRING_IOC_EXT_SETUP, &es) == 0);
		sqe = ring_prep(&r, es.ext_opcode, e->fd, 1);
		sqe->ioprio = KQRING_EXT_TRIGGER;
		sqe->poll32_events = POLLIN;
		sqe->addr = (uint64_t)(uintptr_t)&t;
		sqe->len = sizeof(t);
		sqe->flags = IOSQE_IO_LINK;
		sqe = ring_prep(&r, IORING_OP_RECV, e->fd, 2);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
		sqe = ring_prep(&r, IORING_OP_SEND, e->fd, 3);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->msg_flags = MSG_WAITALL;
		sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
		atomic_store(&e->ready, 1);
		/* Only the trigger's final CQE, when the client hangs up. */
		while (!done) {
			ring_enter(&r, 1);
			while (ring_ready(&r) > 0) {
				c = ring_pop(&r);
				assert(c.user_data == 1);
				done = (c.flags & IORING_CQE_F_MORE) == 0;
			}
		}
		assert(c.res == 0);
		ring_fini(&r);
		break;
	}
	}
	e->cpu_ns = thread_cpu_ns() - cpu0;
	return (NULL);
}

static void
echo_run(const char *group, bool tcp, size_t len)
{
	static const char *const names[] = { "kqueue", "kqring_recv_send",
	    "kqring_trigger" };
	static const char *const cpu_names[] = { "kqueue_server_cpu",
	    "kqring_recv_send_server_cpu", "kqring_trigger_server_cpu" };
	static char back[BUF_SIZE];

	for (int v = ECHO_KQUEUE; v <= ECHO_TRIGGER; v++) {
		struct echo_srv e = { .variant = v };
		pthread_t th;
		uint64_t t0;
		int cl;

		if (tcp)
			tcp_pair(&e.fd, &cl);
		else {
			int sv[2];

			assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
			assert(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
			cl = sv[0];
			e.fd = sv[1];
		}
		assert(pthread_create(&th, NULL, echo_server, &e) == 0);
		while (atomic_load(&e.ready) == 0)
			usleep(100);
		for (int i = -1000; i < ECHO_ITERS; i++) {	/* warm-up */
			size_t got = 0;

			if (i == 0)
				t0 = now_ns();
			xwrite(cl, len);
			while (got < len) {
				ssize_t n = read(cl, back + got, len - got);

				assert(n > 0);
				got += (size_t)n;
			}
		}
		row(group, names[v], now_ns() - t0, ECHO_ITERS);
		close(cl);
		assert(pthread_join(th, NULL) == 0);
		/* Server CPU per message (setup and warm-up included). */
		row(group, cpu_names[v], e.cpu_ns, ECHO_ITERS + 1000);
		close(e.fd);
	}
}

static void
bench_echo(void)
{
	echo_run("echo_unix_64", false, 64);
	echo_run("echo_unix_4096", false, 4096);
	echo_run("echo_tcp_64", true, 64);
}

/* ------------------------------------------------------------------------
 * stream: the send group's variants with the receiver in its own thread,
 * draining the connection with recv(2) while the sender sends back to back.
 * In the send group one thread sends and then reads, so on TCP loopback a
 * small send mostly measures the hand-off to the netisr thread that
 * delivers it; here the two sides overlap, as in a real stream.  Per op =
 * per message, wall clock, from the first send to the last send's
 * completion (and, for SEND_ZC, every notification).  Also reported per
 * message: the CPU time of the sending thread (_sender_cpu) and of the
 * receiving thread (_receiver_cpu).  Copying avoided by SEND_ZC shows in
 * the sender's CPU even when the receiver's copy limits throughput.
 * ------------------------------------------------------------------------
 */

struct stream_rx {
	int		fd;
	uint64_t	bytes;		/* to receive */
	uint64_t	cpu_ns;
};

static void *
stream_receiver(void *arg)
{
	static char buf[SEND_MAX];
	struct stream_rx *x = arg;
	uint64_t cpu0 = thread_cpu_ns(), got = 0;

	while (got < x->bytes) {
		ssize_t n = recv(x->fd, buf, sizeof(buf), 0);

		assert(n > 0);
		got += (uint64_t)n;
	}
	assert(got == x->bytes);
	x->cpu_ns = thread_cpu_ns() - cpu0;
	return (NULL);
}

static void
stream_one(const char *group, const char *variant, struct ring *r, int rx,
    int tx, size_t len, int iters)
{
	struct stream_rx x = { .fd = rx, .bytes = (uint64_t)len * iters };
	char name[64];
	pthread_t th;
	uint64_t cpu0;

	assert(pthread_create(&th, NULL, stream_receiver, &x) == 0);
	cpu0 = thread_cpu_ns();
	if (strcmp(variant, "plain_write") == 0)
		send_plain(group, -1, tx, len, iters);
	else if (strcmp(variant, "kqring_send") == 0)
		send_kqring(group, r, -1, tx, len, iters);
	else
		send_kqring_zc(group, r, -1, tx, len, iters,
		    strcmp(variant, "kqring_send_zc") != 0,
		    strcmp(variant, "kqring_send_zc_fixed_waitall") == 0);
	snprintf(name, sizeof(name), "%s_sender_cpu", variant);
	row(group, name, thread_cpu_ns() - cpu0, (uint64_t)iters);
	assert(pthread_join(th, NULL) == 0);
	snprintf(name, sizeof(name), "%s_receiver_cpu", variant);
	row(group, name, x.cpu_ns, (uint64_t)iters);
}

static void
stream_run(const char *transport, int rx, int tx)
{
	static const size_t sizes[] = { 4U << 10, 16U << 10, 64U << 10,
	    SEND_MAX };
	static const char *const variants[] = { "plain_write", "kqring_send",
	    "kqring_send_zc_fixed", "kqring_send_zc",
	    "kqring_send_zc_fixed_waitall" };
	int sz = SEND_SOCKBUF;
	struct iovec iov[ZC_SLOTS];
	char group[64];
	struct ring r;

	assert(fcntl(rx, F_SETFL, 0) == 0);
	assert(fcntl(tx, F_SETFL, 0) == 0);
	assert(setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0);
	assert(setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	ring_init(&r);
	for (int s = 0; s < ZC_SLOTS; s++) {
		iov[s].iov_base = send_src + (size_t)s * SEND_MAX;
		iov[s].iov_len = SEND_MAX;
	}
	if (kqring_register(r.fd, IORING_REGISTER_BUFFERS, iov,
	    ZC_SLOTS) != 0) {
		perror("IORING_REGISTER_BUFFERS (raise ulimit -l to 8192)");
		exit(1);
	}
	send_may_copy = strcmp(transport, "unix") == 0;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(group, sizeof(group), "stream_%s_%zuk", transport,
		    sizes[i] >> 10);
		for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]);
		    v++)
			stream_one(group, variants[v], &r, rx, tx, sizes[i],
			    (int)(SEND_BYTES / sizes[i]));
	}
	ring_fini(&r);
}

static void
bench_stream(void)
{
	int sv[2];

	send_src = mmap(NULL, (size_t)ZC_SLOTS * SEND_MAX,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(send_src != MAP_FAILED);
	memset(send_src, 's', (size_t)ZC_SLOTS * SEND_MAX);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	stream_run("unix", sv[0], sv[1]);
	close(sv[0]);
	close(sv[1]);
	tcp_pair(&sv[0], &sv[1]);
	stream_run("tcp", sv[0], sv[1]);
	close(sv[0]);
	close(sv[1]);
	munmap(send_src, (size_t)ZC_SLOTS * SEND_MAX);
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
	{ "send-unix",	bench_send_unix },
	{ "timer",	bench_timer },
	{ "disk",	bench_disk },
	{ "echo",	bench_echo },
	{ "stream",	bench_stream },
};

int
main(int argc, char **argv)
{
	memset(payload, 'x', sizeof(payload));
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		bool want = argc < 2 && strcmp(groups[i].name, "send-unix") != 0;

		for (int j = 1; j < argc; j++)
			want |= strcmp(argv[j], groups[i].name) == 0;
		if (want)
			groups[i].fn();
	}
	return (0);
}
