/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring_test.c: Userspace verification & benchmark suite for /dev/kqring.
 * Tests the exact SINGLE_ISSUER | DEFER_TASKRUN + PBUF_RING patterns used by
 * Vega's cvega.c, plus a head-to-head ping-pong benchmark against kqueue(2).
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
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

#define SQ_ENTRIES	128
#define CQ_ENTRIES	512
#define NBUFS		64
#define BUF_SIZE	4096

struct test_ring {
	int			fd;
	void			*ring_map;
	size_t			ring_map_len;
	struct io_uring_sqe	*sqes;
	size_t			sqes_len;
	_Atomic unsigned	*sq_khead, *sq_ktail, *sq_kflags;
	_Atomic unsigned	*cq_khead, *cq_ktail;
	unsigned		*sq_array;
	unsigned		sq_mask, cq_mask;
	unsigned		sq_tail;
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
ring_init(struct test_ring *r)
{
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_CQSIZE |
	    IORING_SETUP_SUBMIT_ALL | IORING_SETUP_R_DISABLED;
	p.cq_entries = CQ_ENTRIES;

	memset(r, 0, sizeof(*r));
	r->fd = (int)kqring_setup(SQ_ENTRIES, &p);
	if (r->fd < 0) {
		perror("kqring_setup (/dev/kqring loaded?)");
		exit(1);
	}
	assert((p.features & IORING_FEAT_SINGLE_MMAP) != 0);
	assert((p.features & IORING_FEAT_NODROP) != 0);
	assert((p.features & IORING_FEAT_EXT_ARG) != 0);

	r->ring_map_len = p.cq_off.cqes +
	    p.cq_entries * sizeof(struct io_uring_cqe);
	r->ring_map = mmap(NULL, r->ring_map_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, r->fd, IORING_OFF_SQ_RING);
	assert(r->ring_map != MAP_FAILED);

	r->sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
	r->sqes = mmap(NULL, r->sqes_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, r->fd, IORING_OFF_SQES);
	assert(r->sqes != MAP_FAILED);

	char *base = (char *)r->ring_map;
	r->sq_khead = (_Atomic unsigned *)(base + p.sq_off.head);
	r->sq_ktail = (_Atomic unsigned *)(base + p.sq_off.tail);
	r->sq_kflags = (_Atomic unsigned *)(base + p.sq_off.flags);
	r->sq_mask = *(unsigned *)(base + p.sq_off.ring_mask);
	r->sq_array = (unsigned *)(base + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		r->sq_array[i] = i;

	r->cq_khead = (_Atomic unsigned *)(base + p.cq_off.head);
	r->cq_ktail = (_Atomic unsigned *)(base + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(base + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);

	/* Enable ring on owning thread */
	assert(kqring_register(r->fd, IORING_REGISTER_ENABLE_RINGS, NULL, 0) == 0);

	/* Register Provided Buffer Ring (PBUF_RING) */
	long page = sysconf(_SC_PAGESIZE);
	size_t br_len = ((NBUFS * sizeof(struct io_uring_buf)) + page - 1) &
	    ~(page - 1);
	r->br = mmap(NULL, br_len, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(r->br != MAP_FAILED);
	r->bufs = mmap(NULL, NBUFS * BUF_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(r->bufs != MAP_FAILED);

	struct io_uring_buf_reg reg = {
		.ring_addr = (uint64_t)(uintptr_t)r->br,
		.ring_entries = NBUFS,
		.bgid = 0,
	};
	assert(kqring_register(r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0);

	struct io_uring_buf *br_bufs = (struct io_uring_buf *)r->br;
	for (unsigned i = 0; i < NBUFS; i++) {
		br_bufs[i].addr = (uint64_t)(uintptr_t)(r->bufs + i * BUF_SIZE);
		br_bufs[i].len = BUF_SIZE;
		br_bufs[i].bid = (uint16_t)i;
	}
	r->br_tail = NBUFS;
	atomic_store_explicit((_Atomic uint16_t *)&r->br->tail, r->br_tail,
	    memory_order_release);
}

static void
ring_offer_buf(struct test_ring *r, uint16_t bid)
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
ring_get_sqe(struct test_ring *r)
{
	struct io_uring_sqe *sqe = &r->sqes[r->sq_tail & r->sq_mask];
	memset(sqe, 0, sizeof(*sqe));
	return (sqe);
}

static void
ring_push_sqe(struct test_ring *r)
{
	r->sq_tail++;
	atomic_store_explicit(r->sq_ktail, r->sq_tail, memory_order_release);
}

#define BENCH_ITERS	200000
static const char bench_msg[64] =
    "ping-pong-64-byte-payload-for-kqring-vs-kqueue-benchmark-test!";

static int failures;

static struct io_uring_sqe *
ring_prep(struct test_ring *r, uint8_t opcode, int fd, uint64_t user_data)
{
	struct io_uring_sqe *sqe = ring_get_sqe(r);
	sqe->opcode = opcode;
	sqe->fd = fd;
	sqe->user_data = user_data;
	return (sqe);
}

static unsigned
ring_pending(struct test_ring *r)
{
	return (r->sq_tail -
	    atomic_load_explicit(r->sq_khead, memory_order_acquire));
}

static unsigned
ring_cq_ready(struct test_ring *r)
{
	return (atomic_load_explicit(r->cq_ktail, memory_order_acquire) -
	    atomic_load_explicit(r->cq_khead, memory_order_relaxed));
}

/* Consume one CQE; returns it by value. Caller must know one is ready. */
static struct io_uring_cqe
ring_pop_cqe(struct test_ring *r)
{
	unsigned h = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	assert(h != atomic_load_explicit(r->cq_ktail, memory_order_acquire));
	struct io_uring_cqe c = r->cqes[h & r->cq_mask];
	atomic_store_explicit(r->cq_khead, h + 1, memory_order_release);
	return (c);
}

static void
ring_drain_cqes(struct test_ring *r)
{
	unsigned h = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	unsigned t = atomic_load_explicit(r->cq_ktail, memory_order_acquire);
	for (; h != t; h++) {
		struct io_uring_cqe *c = &r->cqes[h & r->cq_mask];
		if (c->flags & IORING_CQE_F_BUFFER)
			ring_offer_buf(r, c->flags >> IORING_CQE_BUFFER_SHIFT);
	}
	atomic_store_explicit(r->cq_khead, h, memory_order_release);
}

/* Submit all pending SQEs without waiting. */
static void
ring_submit(struct test_ring *r)
{
	unsigned n = ring_pending(r);
	assert(kqring_enter(r->fd, n, 0, 0, NULL, 0) == (long)n);
}

/* Submit pending SQEs and wait for min CQEs. Returns 0 or an errno. */
static int
ring_wait(struct test_ring *r, unsigned min, int timeout_ms)
{
	struct __kernel_timespec ts = {
		.tv_sec = timeout_ms / 1000,
		.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL,
	};
	struct io_uring_getevents_arg ga = {
		.ts = (uint64_t)(uintptr_t)&ts,
	};
	if (kqring_enter(r->fd, ring_pending(r), min,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) < 0)
		return (errno);
	return (0);
}

static int
tcp_listener(uint16_t *port)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	int one = 1;
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	assert(s >= 0);
	assert(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one,
	    sizeof(one)) == 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(s, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(listen(s, 16) == 0);
	assert(getsockname(s, (struct sockaddr *)&sin, &len) == 0);
	*port = ntohs(sin.sin_port);
	return (s);
}

static int
tcp_connect(uint16_t port)
{
	struct sockaddr_in sin;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	assert(s >= 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	return (s);
}

static void
wait_readable(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	assert(poll(&pfd, 1, 2000) == 1);
}

/* Write until the socket's send path is full (EAGAIN), then let it settle. */
static void
fill_socket(int fd)
{
	static char blob[16384];
	for (int round = 0; round < 2; round++) {
		while (write(fd, blob, sizeof(blob)) > 0)
			;
		assert(errno == EAGAIN);
		usleep(50000);
	}
	while (write(fd, blob, 1) > 0)
		;
	assert(errno == EAGAIN);
}

static void
drain_socket(int fd)
{
	static char blob[65536];
	while (read(fd, blob, sizeof(blob)) > 0)
		;
}

static void
test_probe(struct test_ring *r)
{
	size_t len = sizeof(struct io_uring_probe) +
	    256 * sizeof(struct io_uring_probe_op);
	struct io_uring_probe *probe = calloc(1, len);
	assert(probe != NULL);
	assert(kqring_register(r->fd, IORING_REGISTER_PROBE, probe, 256) == 0);

	static const int needed[] = {
		IORING_OP_RECV, IORING_OP_SEND, IORING_OP_ACCEPT,
		IORING_OP_POLL_ADD, IORING_OP_ASYNC_CANCEL,
		IORING_OP_FUTEX_WAIT
	};
	for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
		int op = needed[i];
		assert(op <= probe->last_op);
		assert((probe->ops[op].flags & IO_URING_OP_SUPPORTED) != 0);
	}
	free(probe);
	printf("[PASS] IORING_REGISTER_PROBE verified all Vega opcodes\n");
}

static void
test_multishot_and_eof(struct test_ring *r)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	/* Arm multishot recv with IOSQE_BUFFER_SELECT on sv[0] */
	struct io_uring_sqe *sqe = ring_get_sqe(r);
	sqe->opcode = IORING_OP_RECV;
	sqe->fd = sv[0];
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;
	sqe->ioprio = IORING_RECV_MULTISHOT | IORING_RECVSEND_POLL_FIRST;
	sqe->user_data = 1001;
	ring_push_sqe(r);

	unsigned pending = r->sq_tail -
	    atomic_load_explicit(r->sq_khead, memory_order_acquire);
	assert(kqring_enter(r->fd, pending, 0, 0, NULL, 0) == (long)pending);

	/* Send two messages and then close sv[1] to trigger EOF */
	assert(write(sv[1], "hello ", 6) == 6);
	assert(write(sv[1], "freebsd", 7) == 7);
	close(sv[1]);

	assert(kqring_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) >= 0);

	char combined[64] = {0};
	size_t off = 0;
	bool saw_eof = false;

	unsigned head = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	unsigned tail = atomic_load_explicit(r->cq_ktail, memory_order_acquire);
	while (head != tail) {
		struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
		assert(cqe->user_data == 1001);
		if (cqe->res > 0) {
			assert((cqe->flags & IORING_CQE_F_BUFFER) != 0);
			uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
			memcpy(combined + off, r->bufs + bid * BUF_SIZE,
			    (size_t)cqe->res);
			off += (size_t)cqe->res;
			ring_offer_buf(r, bid);
		} else if (cqe->res == 0) {
			/* EOF must NOT consume/leak a PBUF_RING buffer */
			assert((cqe->flags & IORING_CQE_F_BUFFER) == 0);
			assert((cqe->flags & IORING_CQE_F_MORE) == 0);
			saw_eof = true;
		}
		head++;
	}
	atomic_store_explicit(r->cq_khead, head, memory_order_release);

	assert(strcmp(combined, "hello freebsd") == 0);
	assert(saw_eof);
	close(sv[0]);
	printf("[PASS] IORING_RECV_MULTISHOT + PBUF_RING + zero-leak EOF\n");
}

static void
test_timeout(struct test_ring *r)
{
	uint64_t t0 = now_ns();
	int err = ring_wait(r, 1, 30);
	uint64_t ms = (now_ns() - t0) / 1000000ULL;
	assert(err == ETIMEDOUT);
	assert(ms >= 25 && ms < 500);
	assert(ring_cq_ready(r) == 0);
	printf("[PASS] EXT_ARG 30 ms timeout -> ETIMEDOUT after %llu ms\n",
	    (unsigned long long)ms);
}

static void *
enter_from_other_thread(void *arg)
{
	struct test_ring *r = arg;
	long rc = kqring_enter(r->fd, 0, 0, 0, NULL, 0);
	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

static void *
unregister_from_other_thread(void *arg)
{
	struct test_ring *r = arg;
	long rc = kqring_register(r->fd, IORING_UNREGISTER_PBUF_RING, NULL, 0);
	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

static void
test_single_issuer(struct test_ring *r)
{
	pthread_t t;
	void *res;
	assert(pthread_create(&t, NULL, enter_from_other_thread, r) == 0);
	assert(pthread_join(t, &res) == 0);
	assert((intptr_t)res == EEXIST);
	assert(pthread_create(&t, NULL, unregister_from_other_thread, r) == 0);
	assert(pthread_join(t, &res) == 0);
	assert((intptr_t)res == EEXIST);
	printf("[PASS] SINGLE_ISSUER: enter/unregister from another thread "
	    "-> EEXIST\n");
}

static void
test_setup_errors(void)
{
	struct kqring_setup_arg arg = { .entries = 8 };
	struct io_uring_params p;
	int fd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	assert(fd >= 0);

	/* SINGLE_ISSUER | DEFER_TASKRUN are required. */
	memset(&p, 0, sizeof(p));
	arg.params = &p;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EINVAL);

	/* copyout of the params fails: the rings must be torn down... */
	long page = sysconf(_SC_PAGESIZE);
	struct io_uring_params *rop = mmap(NULL, (size_t)page,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(rop != MAP_FAILED);
	rop->flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	assert(mprotect(rop, (size_t)page, PROT_READ) == 0);
	arg.params = rop;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EFAULT);

	/* ...so that setup can be retried on the same descriptor. */
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	arg.params = &p;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == 0);
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EINVAL);

	munmap(rop, (size_t)page);
	close(fd);
	printf("[PASS] SETUP: flags enforced, retry after failed copyout\n");
}

static void
test_poll_add(struct test_ring *r)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0],
	    2001);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 20) == ETIMEDOUT);

	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 2001);
	assert((c.res & POLLIN) != 0);
	assert(ring_cq_ready(r) == 0);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] POLL_ADD armed, completes with POLLIN on data\n");
}

static void
test_cancel(struct test_ring *r)
{
	static char buf[64];
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, sv[0], 3001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 3002);
	sqe->addr = 3001;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	struct io_uring_cqe c1 = ring_pop_cqe(r);
	struct io_uring_cqe c2 = ring_pop_cqe(r);
	assert(c1.user_data == 3001 && c1.res == -ECANCELED);
	assert(c2.user_data == 3002 && c2.res == 0);

	/* Cancelling it again finds nothing. */
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 3003);
	sqe->addr = 3001;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c1 = ring_pop_cqe(r);
	assert(c1.user_data == 3003 && c1.res == -ENOENT);

	/* The cancelled op must be fully disarmed: data produces no CQE. */
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 1, 30) == ETIMEDOUT);
	assert(ring_cq_ready(r) == 0);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] ASYNC_CANCEL: -ECANCELED, -ENOENT, upcall disarmed\n");
}

static _Atomic uint32_t futex_word;

static void *
futex_waker(void *arg)
{
	(void)arg;
	usleep(20000);
	atomic_store(&futex_word, 1);
	return (NULL);
}

static void
test_futex_wait(struct test_ring *r)
{
	atomic_store(&futex_word, 0);

	/* Value mismatch completes immediately with -EAGAIN. */
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_FUTEX_WAIT,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 4000);
	sqe->addr = (uint64_t)(uintptr_t)&futex_word;
	sqe->off = 1;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 4000 && c.res == -EAGAIN);

	/* Matching value arms; completes once another thread changes it. */
	sqe = ring_prep(r, IORING_OP_FUTEX_WAIT,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 4001);
	sqe->addr = (uint64_t)(uintptr_t)&futex_word;
	sqe->off = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 30) == ETIMEDOUT);

	pthread_t t;
	assert(pthread_create(&t, NULL, futex_waker, NULL) == 0);
	assert(ring_wait(r, 1, 2000) == 0);
	c = ring_pop_cqe(r);
	assert(c.user_data == 4001 && c.res == 0);
	assert(pthread_join(t, NULL) == 0);
	printf("[PASS] FUTEX_WAIT: -EAGAIN on mismatch, wakes on change\n");

	/*
	 * Several waits may be armed at once, and a non-waiting enter
	 * completes the ones whose word changed.
	 */
	static _Atomic uint32_t w1, w2;
	atomic_store(&w1, 0);
	atomic_store(&w2, 0);
	sqe = ring_prep(r, IORING_OP_FUTEX_WAIT,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 4101);
	sqe->addr = (uint64_t)(uintptr_t)&w1;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_FUTEX_WAIT,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 4102);
	sqe->addr = (uint64_t)(uintptr_t)&w2;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	atomic_store(&w2, 1);
	assert(kqring_enter(r->fd, 0, 0, 0, NULL, 0) == 0);
	assert(ring_cq_ready(r) == 1);
	c = ring_pop_cqe(r);
	assert(c.user_data == 4102 && c.res == 0);

	atomic_store(&w1, 1);
	assert(kqring_enter(r->fd, 0, 0, 0, NULL, 0) == 0);
	assert(ring_cq_ready(r) == 1);
	c = ring_pop_cqe(r);
	assert(c.user_data == 4101 && c.res == 0);
	printf("[PASS] FUTEX_WAIT: two armed at once, seen by non-waiting "
	    "enter\n");
}

/* A second op on the same socket and direction gets -EBUSY. */
static void
test_dup_arm(struct test_ring *r)
{
	static char buf[64];
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, sv[0], 8001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 8002);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 8002 && c.res == -EBUSY);

	/* The first op is still armed and completes normally. */
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	c = ring_pop_cqe(r);
	assert(c.user_data == 8001 && c.res == 1);
	assert(ring_cq_ready(r) == 0);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] second op on same socket+direction -> -EBUSY\n");
}

/*
 * An armed op whose fd is closed and reused for another socket must not
 * operate on the new socket.
 */
static void
test_fd_reuse(struct test_ring *r)
{
	static char buf[64];
	int a[2], b[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, a) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, b) == 0);
	int fd = a[0];

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, fd, 8101);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	/* close(fd) fires the upcall; fd then names b[0]'s socket. */
	assert(dup2(b[0], fd) == fd);
	assert(write(b[1], "y", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 8101 && c.res == -EBADF);
	assert(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'y');

	close(fd);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	printf("[PASS] armed RECV after fd reuse -> -EBADF, new socket "
	    "untouched\n");
}

/* Timeout or EINTR after submitting still reports the submitted count. */
static void
test_enter_returns_submitted(struct test_ring *r)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0],
	    8201);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	struct __kernel_timespec ts = { .tv_nsec = 20000000 };
	struct io_uring_getevents_arg ga = {
		.ts = (uint64_t)(uintptr_t)&ts,
	};
	assert(kqring_enter(r->fd, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == 1);
	assert(ring_pending(r) == 0);
	assert(ring_cq_ready(r) == 0);

	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 8202);
	sqe->addr = 8201;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	ring_drain_cqes(r);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] enter with timeout after submit returns submitted "
	    "count\n");
}

/* CQEs beyond the CQ ring are held, in order, until there is room. */
static void
test_cq_overflow(struct test_ring *r)
{
	unsigned cq_entries = r->cq_mask + 1;
	unsigned total = cq_entries + SQ_ENTRIES;

	for (unsigned i = 0; i < total; i++) {
		(void)ring_prep(r, IORING_OP_NOP, -1, 10000 + i);
		ring_push_sqe(r);
		if (ring_pending(r) == SQ_ENTRIES)
			ring_submit(r);
	}
	if (ring_pending(r) != 0)
		ring_submit(r);
	assert(ring_cq_ready(r) == cq_entries);
	assert((atomic_load(r->sq_kflags) & IORING_SQ_CQ_OVERFLOW) != 0);

	unsigned next = 10000;
	for (unsigned i = 0; i < cq_entries; i++)
		assert(ring_pop_cqe(r).user_data == next++);

	/* Any enter flushes the held CQEs into the freed space. */
	assert(kqring_enter(r->fd, 0, 0, 0, NULL, 0) == 0);
	assert(ring_cq_ready(r) == total - cq_entries);
	assert((atomic_load(r->sq_kflags) & IORING_SQ_CQ_OVERFLOW) == 0);
	while (ring_cq_ready(r) > 0)
		assert(ring_pop_cqe(r).user_data == next++);
	assert(next == 10000 + total);
	printf("[PASS] CQ overflow: %u CQEs held in kernel, delivered in "
	    "order\n", total - cq_entries);
}

static void
test_accept(struct test_ring *r)
{
	uint16_t port;
	char buf[8];
	int lfd = tcp_listener(&port);

	/* 1. Nothing pending: ACCEPT arms on the listening socket. */
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 5001);
	sqe->accept_flags = SOCK_CLOEXEC;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 20) == ETIMEDOUT);

	int c1 = tcp_connect(port);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 5001 && c.res >= 0);
	int a1 = c.res;
	assert((fcntl(a1, F_GETFD) & FD_CLOEXEC) != 0);
	assert((fcntl(a1, F_GETFL) & O_NONBLOCK) != 0);
	assert(write(c1, "ping", 4) == 4);
	wait_readable(a1);
	assert(read(a1, buf, sizeof(buf)) == 4);
	assert(memcmp(buf, "ping", 4) == 0);

	/* 2. Connection already queued: ACCEPT completes inline. */
	int c2 = tcp_connect(port);
	wait_readable(lfd);
	sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 5002);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = ring_pop_cqe(r);
	assert(c.user_data == 5002 && c.res >= 0);
	int a2 = c.res;
	assert((fcntl(a2, F_GETFD) & FD_CLOEXEC) == 0);

	close(a1);
	close(a2);
	close(c1);
	close(c2);
	close(lfd);
	printf("[PASS] ACCEPT: armed on listener + inline, SOCK_CLOEXEC\n");

	/*
	 * listen(2) on a socket with an op armed on its so_rcv reuses that
	 * storage for the listen queues; disarming must not write into it.
	 */
	struct sockaddr_in sin = {
		.sin_len = sizeof(sin),
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t slen = sizeof(sin);
	lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	assert(lfd >= 0);
	assert(bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(getsockname(lfd, (struct sockaddr *)&sin, &slen) == 0);
	sqe = ring_prep(r, IORING_OP_POLL_ADD, lfd, 5101);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(listen(lfd, 16) == 0);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 5102);
	sqe->addr = 5101;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	ring_drain_cqes(r);

	/* The listen queue still works. */
	c1 = tcp_connect(ntohs(sin.sin_port));
	wait_readable(lfd);
	a1 = accept(lfd, NULL, NULL);
	assert(a1 >= 0);
	close(a1);
	close(c1);
	close(lfd);
	printf("[PASS] op armed before listen(2), cancelled after\n");
}

/* Arm a SEND on a full socket, drain the peer, expect completion. */
static int
send_when_writable(struct test_ring *r, int wfd, int rfd, uint64_t ud)
{
	fill_socket(wfd);
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_SEND, wfd, ud);
	sqe->addr = (uint64_t)(uintptr_t)bench_msg;
	sqe->len = sizeof(bench_msg);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 20) == ETIMEDOUT);

	for (int i = 0; i < 100; i++) {
		drain_socket(rfd);
		if (ring_wait(r, 1, 10) == 0)
			break;
	}
	if (ring_cq_ready(r) == 0)
		return (ETIMEDOUT);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == ud);
	assert(c.res == (int)sizeof(bench_msg));
	return (0);
}

static void
test_send_armed(struct test_ring *r)
{
	/* TCP loopback with small buffers. */
	uint16_t port;
	int small = 8192;
	int lfd = tcp_listener(&port);
	int cfd = tcp_connect(port);
	wait_readable(lfd);
	int sfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
	assert(sfd >= 0);
	assert(setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &small,
	    sizeof(small)) == 0);
	assert(setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &small,
	    sizeof(small)) == 0);
	assert(fcntl(cfd, F_SETFL, O_NONBLOCK) == 0);

	assert(send_when_writable(r, cfd, sfd, 6001) == 0);

	/*
	 * POLLIN | POLLOUT on a full socket must wake for either direction
	 * (both upcalls armed), here for readability.
	 */
	fill_socket(cfd);
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_POLL_ADD, cfd, 6002);
	sqe->poll32_events = POLLIN | POLLOUT;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 20) == ETIMEDOUT);
	assert(write(sfd, "z", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 6002 && (c.res & POLLIN) != 0);
	printf("[PASS] POLL_ADD(POLLIN|POLLOUT) on full TCP socket wakes on "
	    "POLLIN\n");

	close(cfd);
	close(sfd);
	close(lfd);
	printf("[PASS] SEND armed on full TCP socket, completes on drain\n");

	/* AF_UNIX stream. */
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	if (send_when_writable(r, sv[0], sv[1], 6101) == 0) {
		printf("[PASS] SEND armed on full AF_UNIX socket, completes "
		    "on drain\n");
	} else {
		printf("[FAIL] SEND armed on full AF_UNIX socket never "
		    "completed (no so_snd upcall?)\n");
		failures++;
		struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL,
		    -1, 6102);
		sqe->addr = 6101;
		ring_push_sqe(r);
		ring_submit(r);
		ring_drain_cqes(r);
	}
	close(sv[0]);
	close(sv[1]);
}

static void
test_file_rw(struct test_ring *r)
{
	char path[] = "/tmp/kqring_test.XXXXXX";
	char buf[16] = {0};
	int tmp = mkstemp(path);
	assert(tmp >= 0);
	close(tmp);

	/* OPENAT */
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_OPENAT, AT_FDCWD,
	    7001);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDWR;
	ring_push_sqe(r);
	ring_submit(r);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 7001 && c.res >= 0);
	int fd = c.res;

	/* Positioned writes must not move the file offset. */
	sqe = ring_prep(r, IORING_OP_WRITE, fd, 7002);
	sqe->addr = (uint64_t)(uintptr_t)"hello world";
	sqe->len = 11;
	sqe->off = 0;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_WRITE, fd, 7003);
	sqe->addr = (uint64_t)(uintptr_t)"XY";
	sqe->len = 2;
	sqe->off = 6;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_FSYNC, fd, 7004);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 3);
	c = ring_pop_cqe(r);
	assert(c.user_data == 7002 && c.res == 11);
	c = ring_pop_cqe(r);
	assert(c.user_data == 7003 && c.res == 2);
	c = ring_pop_cqe(r);
	assert(c.user_data == 7004 && c.res == 0);
	assert(lseek(fd, 0, SEEK_CUR) == 0);

	/* Positioned read. */
	sqe = ring_prep(r, IORING_OP_READ, fd, 7005);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 5;
	sqe->off = 6;
	ring_push_sqe(r);
	ring_submit(r);
	c = ring_pop_cqe(r);
	assert(c.user_data == 7005 && c.res == 5);
	assert(memcmp(buf, "XYrld", 5) == 0);
	assert(lseek(fd, 0, SEEK_CUR) == 0);

	/* off == -1 uses and advances the file position. */
	sqe = ring_prep(r, IORING_OP_READ, fd, 7006);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 5;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(r);
	ring_submit(r);
	c = ring_pop_cqe(r);
	assert(c.user_data == 7006 && c.res == 5);
	assert(memcmp(buf, "hello", 5) == 0);
	assert(lseek(fd, 0, SEEK_CUR) == 5);

	close(fd);
	unlink(path);
	printf("[PASS] OPENAT + positioned WRITE/READ + FSYNC + off=-1\n");
}

static _Atomic uint32_t futex_word2;

/*
 * Close a ring while ops of every kind are armed, then poke all of them.
 * If teardown leaves any upcall installed, this panics the kernel (UAF).
 */
static void
test_close_with_pending(void)
{
	static char buf[64];
	struct test_ring r2;
	int a[2], b[2], m[2];
	uint16_t port;

	ring_init(&r2);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, a) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, b) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, m) == 0);
	int lfd = tcp_listener(&port);
	atomic_store(&futex_word2, 0);

	struct io_uring_sqe *sqe = ring_prep(&r2, IORING_OP_RECV, a[0], 1);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&r2);
	sqe = ring_prep(&r2, IORING_OP_POLL_ADD, b[0], 2);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(&r2);
	sqe = ring_prep(&r2, IORING_OP_ACCEPT, lfd, 3);
	ring_push_sqe(&r2);
	sqe = ring_prep(&r2, IORING_OP_FUTEX_WAIT,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 4);
	sqe->addr = (uint64_t)(uintptr_t)&futex_word2;
	sqe->off = 0;
	ring_push_sqe(&r2);
	sqe = ring_prep(&r2, IORING_OP_RECV, m[0], 5);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->ioprio = IORING_RECV_MULTISHOT;
	ring_push_sqe(&r2);
	ring_submit(&r2);
	assert(ring_cq_ready(&r2) == 0);

	/* Tear down the ring while everything is armed. */
	assert(close(r2.fd) == 0);

	/* Fire every former upcall. */
	assert(write(a[1], "x", 1) == 1);
	assert(write(b[1], "x", 1) == 1);
	assert(write(m[1], "x", 1) == 1);
	int c = tcp_connect(port);
	atomic_store(&futex_word2, 1);
	usleep(10000);

	munmap(r2.ring_map, r2.ring_map_len);
	munmap(r2.sqes, r2.sqes_len);
	close(a[0]); close(a[1]);
	close(b[0]); close(b[1]);
	close(m[0]); close(m[1]);
	close(c);
	close(lfd);
	printf("[PASS] ring close with RECV/POLL/ACCEPT/FUTEX/multishot armed\n");
}

/* NOP round trip: the floor cost of one KQRING_IOC_ENTER. */
static uint64_t
bench_nop(struct test_ring *r)
{
	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_get_sqe(r);
		sqe->opcode = IORING_OP_NOP;
		sqe->user_data = (uint64_t)i;
		ring_push_sqe(r);
		assert(kqring_enter(r->fd, ring_pending(r), 1,
		    IORING_ENTER_GETEVENTS, NULL, 0) >= 0);
		(void)ring_pop_cqe(r);
	}
	return ((now_ns() - t0) / BENCH_ITERS);
}

/*
 * Single-shot RECV + BUFFER_SELECT. With poll_first the op is always armed
 * (soupcall install/remove + malloc/free); without it the kernel tries the
 * receive inline at submit time.
 */
static uint64_t
bench_recv_single(struct test_ring *r, int sv[2], bool poll_first)
{
	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_get_sqe(r);
		sqe->opcode = IORING_OP_RECV;
		sqe->fd = sv[0];
		sqe->flags = IOSQE_BUFFER_SELECT;
		sqe->ioprio = poll_first ? IORING_RECVSEND_POLL_FIRST : 0;
		sqe->user_data = (uint64_t)i;
		ring_push_sqe(r);

		assert(write(sv[1], bench_msg, sizeof(bench_msg)) ==
		    sizeof(bench_msg));
		assert(kqring_enter(r->fd, ring_pending(r), 1,
		    IORING_ENTER_GETEVENTS, NULL, 0) >= 0);

		struct io_uring_cqe c = ring_pop_cqe(r);
		assert(c.res == sizeof(bench_msg));
		assert(c.flags & IORING_CQE_F_BUFFER);
		ring_offer_buf(r, c.flags >> IORING_CQE_BUFFER_SHIFT);
	}
	return ((now_ns() - t0) / BENCH_ITERS);
}

/*
 * Single-shot RECV submitted before the data exists, as in a real server:
 * enter(submit -> EAGAIN -> arm), write (upcall), enter(wait -> complete).
 */
static uint64_t
bench_recv_armed(struct test_ring *r, int sv[2])
{
	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, sv[0],
		    (uint64_t)i);
		sqe->flags = IOSQE_BUFFER_SELECT;
		ring_push_sqe(r);
		assert(kqring_enter(r->fd, ring_pending(r), 0, 0, NULL, 0) == 1);

		assert(write(sv[1], bench_msg, sizeof(bench_msg)) ==
		    sizeof(bench_msg));
		assert(kqring_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS,
		    NULL, 0) >= 0);

		struct io_uring_cqe c = ring_pop_cqe(r);
		assert(c.user_data == (uint64_t)i);
		assert(c.res == sizeof(bench_msg));
		assert(c.flags & IORING_CQE_F_BUFFER);
		ring_offer_buf(r, c.flags >> IORING_CQE_BUFFER_SHIFT);
	}
	return ((now_ns() - t0) / BENCH_ITERS);
}

/* Multishot RECV armed once; each round trip is write + enter(wait). */
static uint64_t
bench_recv_multishot(struct test_ring *r)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_get_sqe(r);
	sqe->opcode = IORING_OP_RECV;
	sqe->fd = sv[0];
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->ioprio = IORING_RECV_MULTISHOT;
	sqe->user_data = 0xBEEF;
	ring_push_sqe(r);
	assert(kqring_enter(r->fd, ring_pending(r), 0, 0, NULL, 0) == 1);

	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		assert(write(sv[1], bench_msg, sizeof(bench_msg)) ==
		    sizeof(bench_msg));
		assert(kqring_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS,
		    NULL, 0) >= 0);

		struct io_uring_cqe c = ring_pop_cqe(r);
		assert(c.user_data == 0xBEEF);
		assert(c.res == sizeof(bench_msg));
		assert(c.flags & IORING_CQE_F_MORE);
		ring_offer_buf(r, c.flags >> IORING_CQE_BUFFER_SHIFT);
	}
	uint64_t ns = (now_ns() - t0) / BENCH_ITERS;

	/* Terminate the multishot with EOF and drain its final CQE. */
	close(sv[1]);
	assert(kqring_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) >= 0);
	ring_drain_cqes(r);
	close(sv[0]);
	return (ns);
}

static uint64_t
bench_kqueue(int sv[2])
{
	char rxbuf[64];
	int kq = kqueue();
	assert(kq >= 0);
	struct kevent kev;
	EV_SET(&kev, sv[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);

	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		assert(write(sv[1], bench_msg, sizeof(bench_msg)) ==
		    sizeof(bench_msg));
		struct kevent out;
		assert(kevent(kq, NULL, 0, &out, 1, NULL) == 1);
		assert(recv(sv[0], rxbuf, sizeof(rxbuf), 0) ==
		    sizeof(bench_msg));
	}
	uint64_t ns = (now_ns() - t0) / BENCH_ITERS;
	close(kq);
	return (ns);
}

/* Lower bound: write + recv with no readiness mechanism at all. */
static uint64_t
bench_plain_recv(int sv[2])
{
	char rxbuf[64];
	uint64_t t0 = now_ns();
	for (int i = 0; i < BENCH_ITERS; i++) {
		assert(write(sv[1], bench_msg, sizeof(bench_msg)) ==
		    sizeof(bench_msg));
		assert(recv(sv[0], rxbuf, sizeof(rxbuf), 0) ==
		    sizeof(bench_msg));
	}
	return ((now_ns() - t0) / BENCH_ITERS);
}

static void
bench_ping_pong(struct test_ring *r)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	/* Warm up caches / branch predictors. */
	(void)bench_plain_recv(sv);

	uint64_t nop = bench_nop(r);
	uint64_t plain = bench_plain_recv(sv);
	uint64_t kq = bench_kqueue(sv);
	uint64_t direct = bench_recv_single(r, sv, false);
	uint64_t pollf = bench_recv_single(r, sv, true);
	uint64_t armed = bench_recv_armed(r, sv);
	close(sv[0]);
	close(sv[1]);
	uint64_t mshot = bench_recv_multishot(r);

	printf("[BENCH] %d iterations, 64-byte AF_UNIX stream ping-pong:\n",
	    BENCH_ITERS);
	printf("  kqring NOP enter        (1 syscall)            : %5llu ns/op\n",
	    (unsigned long long)nop);
	printf("  write+recv, no polling  (2 syscalls, floor)    : %5llu ns/op\n",
	    (unsigned long long)plain);
	printf("  kqueue                  (3: write+kevent+recv) : %5llu ns/op\n",
	    (unsigned long long)kq);
	printf("  kqring RECV direct      (2: write+enter)       : %5llu ns/op\n",
	    (unsigned long long)direct);
	printf("  kqring RECV POLL_FIRST  (2: write+enter)       : %5llu ns/op\n",
	    (unsigned long long)pollf);
	printf("  kqring RECV armed       (3: enter+write+enter) : %5llu ns/op\n",
	    (unsigned long long)armed);
	printf("  kqring RECV multishot   (2: write+enter)       : %5llu ns/op\n",
	    (unsigned long long)mshot);
}

/* Usage: kqring_test [tests|bench]  (default: both) */
int
main(int argc, char **argv)
{
	bool run_tests = argc < 2 || strcmp(argv[1], "bench") != 0;
	bool run_bench = argc < 2 || strcmp(argv[1], "tests") != 0;
	struct test_ring r;

	ring_init(&r);
	if (run_tests) {
		test_probe(&r);
		test_multishot_and_eof(&r);
		test_timeout(&r);
		test_single_issuer(&r);
		test_setup_errors();
		test_poll_add(&r);
		test_cancel(&r);
		test_futex_wait(&r);
		test_dup_arm(&r);
		test_fd_reuse(&r);
		test_enter_returns_submitted(&r);
		test_cq_overflow(&r);
		test_accept(&r);
		test_send_armed(&r);
		test_file_rw(&r);
		test_close_with_pending();
		printf("%s: %d failure(s)\n", failures ? "[FAIL]" : "[OK]",
		    failures);
	}
	if (run_bench)
		bench_ping_pong(&r);
	close(r.fd);
	return (failures ? 1 : 0);
}
