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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/umtx.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
	_Atomic unsigned	*sq_khead, *sq_ktail, *sq_kflags, *sq_kdropped;
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
	r->sq_kdropped = (_Atomic unsigned *)(base + p.sq_off.dropped);
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
	assert(probe->last_op == IORING_OP_LAST - 1);
	assert(probe->ops_len == IORING_OP_LAST);

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

	/* Unimplemented flags and nonzero reserved fields are rejected. */
	const uint32_t req = IORING_SETUP_SINGLE_ISSUER |
	    IORING_SETUP_DEFER_TASKRUN;
	static const uint32_t bad[] = {
		IORING_SETUP_SQPOLL, IORING_SETUP_IOPOLL,
		IORING_SETUP_SQE128, IORING_SETUP_CQE32,
		IORING_SETUP_ATTACH_WQ, 1U << 16 /* NO_SQARRAY */,
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		p.flags = req | bad[i];
		assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 &&
		    errno == EINVAL);
	}
	p.flags = req;
	p.resv[1] = 1;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EINVAL);
	p.resv[1] = 0;

	/* Too many entries: EINVAL, unless CLAMP is set. */
	arg.entries = 1U << 20;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EINVAL);
	int cfd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	assert(cfd >= 0);
	p.flags = req | IORING_SETUP_CLAMP;
	assert(ioctl(cfd, KQRING_IOC_SETUP, &arg) == 0);
	assert(p.sq_entries == 4096 && p.cq_entries == 8192);
	close(cfd);
	arg.entries = 8;
	memset(&p, 0, sizeof(p));

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
	printf("[PASS] SETUP: flags/resv validated, CLAMP, retry after "
	    "failed copyout\n");
}

/* Submit one prepared SQE and return its CQE's res. */
static int32_t
submit_one(struct test_ring *r)
{
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	return (ring_pop_cqe(r).res);
}

static void
test_sqe_validation(struct test_ring *r)
{
	static char buf[64];
	static const uint8_t bad_flags[] = {
		IOSQE_FIXED_FILE, 1U << 7,
	};
	struct io_uring_sqe *sqe;
	int sv[2];

	for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++) {
		sqe = ring_prep(r, IORING_OP_NOP, -1, 11000 + i);
		sqe->flags = bad_flags[i];
		ring_push_sqe(r);
		assert(submit_one(r) == -EINVAL);
	}
	/* IOSQE_ASYNC is a hint. */
	sqe = ring_prep(r, IORING_OP_NOP, -1, 11100);
	sqe->flags = IOSQE_ASYNC;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	/* Multishot RECV needs IOSQE_BUFFER_SELECT. */
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 11101);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->ioprio = IORING_RECV_MULTISHOT;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	/* Unknown ioprio bits. */
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 11102);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	/* Only buffer group 0 exists. */
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 11103);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == -ENOBUFS);
	/* Buffer selection is for receives only. */
	sqe = ring_prep(r, IORING_OP_SEND, sv[0], 11104);
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	close(sv[0]);
	close(sv[1]);

	/* An out-of-range SQ array index is consumed and counted dropped. */
	unsigned slot = r->sq_tail & r->sq_mask;
	unsigned dropped = atomic_load(r->sq_kdropped);
	r->sq_array[slot] = 0xffff;
	(void)ring_prep(r, IORING_OP_NOP, -1, 11105);
	ring_push_sqe(r);
	assert(kqring_enter(r->fd, 1, 0, 0, NULL, 0) == 0);
	assert(ring_pending(r) == 0);
	assert(ring_cq_ready(r) == 0);
	assert(atomic_load(r->sq_kdropped) == dropped + 1);
	r->sq_array[slot] = slot;

	printf("[PASS] SQE flags/ioprio/buf_group validated, bad index "
	    "-> sq_dropped\n");
}

static void
test_enter_validation(struct test_ring *r)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct io_uring_getevents_arg ga = {
		.ts = (uint64_t)(uintptr_t)&ts,
	};
	static const unsigned bad_flags[] = {
		IORING_ENTER_SQ_WAKEUP, IORING_ENTER_SQ_WAIT,
		IORING_ENTER_REGISTERED_RING, 1U << 9,
	};

	/* A rejected enter must not consume the pending SQE. */
	(void)ring_prep(r, IORING_OP_NOP, -1, 12000);
	ring_push_sqe(r);
	for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++)
		assert(kqring_enter(r->fd, 1, 0, bad_flags[i], NULL, 0) == -1 &&
		    errno == EINVAL);
	/* Without EXT_ARG, arg would be a sigmask. */
	assert(kqring_enter(r->fd, 1, 1, IORING_ENTER_GETEVENTS, &ga,
	    sizeof(ga)) == -1 && errno == EINVAL);
	/* Wrong argsz, a sigmask, or nonzero pad (min_wait_usec). */
	assert(kqring_enter(r->fd, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga) - 1) == -1 && errno == EINVAL);
	sigset_t set;
	sigemptyset(&set);
	ga.sigmask = (uint64_t)(uintptr_t)&set;
	ga.sigmask_sz = sizeof(set);
	assert(kqring_enter(r->fd, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == -1 && errno == EINVAL);
	ga.sigmask = 0;
	ga.sigmask_sz = 0;
	ga.pad = 1;
	assert(kqring_enter(r->fd, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == -1 && errno == EINVAL);
	assert(ring_pending(r) == 1);
	assert(ring_cq_ready(r) == 0);

	ga.pad = 0;
	assert(kqring_enter(r->fd, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == 1);
	assert(ring_pop_cqe(r).user_data == 12000);
	printf("[PASS] ENTER flags/EXT_ARG validated before consuming SQEs\n");
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
	/* Only the flags asked for, as accept4(2). */
	assert((fcntl(a1, F_GETFL) & O_NONBLOCK) == 0);
	assert(write(c1, "ping", 4) == 4);
	wait_readable(a1);
	assert(read(a1, buf, sizeof(buf)) == 4);
	assert(memcmp(buf, "ping", 4) == 0);

	/* 2. Connection already queued: ACCEPT completes inline. */
	int c2 = tcp_connect(port);
	wait_readable(lfd);
	sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 5002);
	sqe->accept_flags = SOCK_NONBLOCK;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = ring_pop_cqe(r);
	assert(c.user_data == 5002 && c.res >= 0);
	int a2 = c.res;
	assert((fcntl(a2, F_GETFD) & FD_CLOEXEC) == 0);
	assert((fcntl(a2, F_GETFL) & O_NONBLOCK) != 0);

	close(a1);
	close(a2);
	close(c1);
	close(c2);
	close(lfd);
	printf("[PASS] ACCEPT: armed on listener + inline, accept4 flags\n");

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

/* Pop one CQE and check it. */
static struct io_uring_cqe
expect_cqe(struct test_ring *r, uint64_t ud, int32_t res)
{
	assert(ring_cq_ready(r) > 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	if (c.user_data != ud || c.res != res) {
		fprintf(stderr, "expected cqe ud=%llu res=%d, got ud=%llu "
		    "res=%d flags=%#x\n", (unsigned long long)ud, res,
		    (unsigned long long)c.user_data, c.res, c.flags);
		abort();
	}
	return (c);
}

static void
prep_recv(struct test_ring *r, int fd, uint64_t ud, void *buf, unsigned len,
    uint8_t flags)
{
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, fd, ud);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = len;
	sqe->flags = flags;
	ring_push_sqe(r);
}

static void
prep_timeout(struct test_ring *r, uint8_t opcode, uint64_t ud,
    struct __kernel_timespec *ts, uint64_t off, uint32_t tflags,
    uint8_t flags)
{
	struct io_uring_sqe *sqe = ring_prep(r, opcode, -1, ud);
	sqe->addr = (uint64_t)(uintptr_t)ts;
	sqe->len = 1;
	sqe->off = off;
	sqe->timeout_flags = tflags;
	sqe->flags = flags;
	ring_push_sqe(r);
}

static void
prep_nop(struct test_ring *r, uint64_t ud, uint8_t flags)
{
	ring_prep(r, IORING_OP_NOP, -1, ud)->flags = flags;
	ring_push_sqe(r);
}

static void
test_links(struct test_ring *r)
{
	static char buf[64];
	int sv[2];

	/* A chain of NOPs completes in order. */
	prep_nop(r, 20001, IOSQE_IO_LINK);
	prep_nop(r, 20002, IOSQE_IO_LINK);
	prep_nop(r, 20003, 0);
	ring_submit(r);
	expect_cqe(r, 20001, 0);
	expect_cqe(r, 20002, 0);
	expect_cqe(r, 20003, 0);

	/* A failure cancels the rest of the chain... */
	ring_prep(r, IORING_OP_READ, -1, 20011)->flags = IOSQE_IO_LINK;
	ring_push_sqe(r);
	prep_nop(r, 20012, IOSQE_IO_LINK);
	prep_nop(r, 20013, 0);
	prep_nop(r, 20014, 0);		/* not part of the chain */
	ring_submit(r);
	expect_cqe(r, 20011, -EBADF);
	expect_cqe(r, 20012, -ECANCELED);
	expect_cqe(r, 20013, -ECANCELED);
	expect_cqe(r, 20014, 0);

	/* ...unless it is a hard link. */
	ring_prep(r, IORING_OP_READ, -1, 20021)->flags = IOSQE_IO_HARDLINK;
	ring_push_sqe(r);
	prep_nop(r, 20022, 0);
	ring_submit(r);
	expect_cqe(r, 20021, -EBADF);
	expect_cqe(r, 20022, 0);

	/* A bad SQE fails its whole chain at submission. */
	prep_nop(r, 20031, IOSQE_IO_LINK);
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_POLL_ADD, 0, 20032);
	sqe->addr = 1;			/* must be 0 */
	sqe->flags = IOSQE_IO_LINK;
	ring_push_sqe(r);
	prep_nop(r, 20033, 0);
	ring_submit(r);
	expect_cqe(r, 20031, -ECANCELED);
	expect_cqe(r, 20032, -EINVAL);
	expect_cqe(r, 20033, -ECANCELED);

	/* A link flag on the last SQE of a batch just ends the chain. */
	prep_nop(r, 20041, IOSQE_IO_LINK);
	ring_submit(r);
	expect_cqe(r, 20041, 0);

	/* A successor waits for an armed (async) predecessor. */
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	prep_recv(r, sv[0], 20051, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_nop(r, 20052, 0);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(write(sv[1], "abc", 3) == 3);
	assert(ring_wait(r, 2, 2000) == 0);
	expect_cqe(r, 20051, 3);
	expect_cqe(r, 20052, 0);

	/* An armed predecessor that is cancelled breaks the chain. */
	prep_recv(r, sv[0], 20061, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_nop(r, 20062, 0);
	ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 20063)->addr = 20061;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 20061, -ECANCELED);
	expect_cqe(r, 20062, -ECANCELED);
	expect_cqe(r, 20063, 0);

	/* A long chain is issued iteratively (no kernel stack growth). */
	for (unsigned i = 0; i < SQ_ENTRIES; i++)
		prep_nop(r, 21000 + i, i + 1 < SQ_ENTRIES ? IOSQE_IO_LINK : 0);
	ring_submit(r);
	for (unsigned i = 0; i < SQ_ENTRIES; i++)
		expect_cqe(r, 21000 + i, 0);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] IO_LINK / IO_HARDLINK chains (inline, async, failed, "
	    "cancelled, %u long)\n", SQ_ENTRIES);
}

static void
test_cqe_skip(struct test_ring *r)
{
	prep_nop(r, 22001, IOSQE_CQE_SKIP_SUCCESS);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	/* Errors are always posted. */
	ring_prep(r, IORING_OP_READ, -1, 22002)->flags =
	    IOSQE_CQE_SKIP_SUCCESS;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 22002, -EBADF);

	prep_nop(r, 22003, IOSQE_CQE_SKIP_SUCCESS | IOSQE_IO_LINK);
	prep_nop(r, 22004, 0);
	ring_submit(r);
	expect_cqe(r, 22004, 0);
	assert(ring_cq_ready(r) == 0);
	printf("[PASS] IOSQE_CQE_SKIP_SUCCESS\n");
}

static void
test_drain(struct test_ring *r)
{
	static char buf[64];
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	/* The drained NOP, and everything after it, waits for the RECV. */
	prep_recv(r, sv[0], 23001, buf, sizeof(buf), 0);
	prep_nop(r, 23002, IOSQE_IO_DRAIN);
	prep_nop(r, 23003, 0);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 3, 2000) == 0);
	expect_cqe(r, 23001, 1);
	expect_cqe(r, 23002, 0);
	expect_cqe(r, 23003, 0);

	/* With nothing in flight, a drained request runs at once. */
	prep_nop(r, 23011, IOSQE_IO_DRAIN);
	ring_submit(r);
	expect_cqe(r, 23011, 0);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] IOSQE_IO_DRAIN orders against in-flight requests\n");
}

static void
test_timeouts(struct test_ring *r)
{
	struct __kernel_timespec t20 = { .tv_nsec = 20000000 };
	struct __kernel_timespec t10 = { .tv_nsec = 10000000 };
	struct __kernel_timespec t5s = { .tv_sec = 5 };
	struct io_uring_sqe *sqe;
	uint64_t t0;

	/* Relative timeout. */
	t0 = now_ns();
	prep_timeout(r, IORING_OP_TIMEOUT, 24001, &t20, 0, 0, 0);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 24001, -ETIME);
	uint64_t ms = (now_ns() - t0) / 1000000ULL;
	assert(ms >= 15 && ms < 1000);

	/* Absolute CLOCK_MONOTONIC timeout. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct __kernel_timespec abs = {
		.tv_sec = now.tv_sec,
		.tv_nsec = now.tv_nsec + 20000000LL,
	};
	if (abs.tv_nsec >= 1000000000LL) {
		abs.tv_sec++;
		abs.tv_nsec -= 1000000000LL;
	}
	t0 = now_ns();
	prep_timeout(r, IORING_OP_TIMEOUT, 24002, &abs, 0,
	    IORING_TIMEOUT_ABS, 0);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 24002, -ETIME);
	assert((now_ns() - t0) / 1000000ULL >= 10);

	/* Completion count: done (res 0) after 2 other CQEs. */
	prep_timeout(r, IORING_OP_TIMEOUT, 24003, &t5s, 2, 0, 0);
	prep_nop(r, 24004, 0);
	prep_nop(r, 24005, 0);
	ring_submit(r);
	expect_cqe(r, 24004, 0);
	expect_cqe(r, 24005, 0);
	expect_cqe(r, 24003, 0);

	/* TIMEOUT_REMOVE cancels; a second remove finds nothing. */
	prep_timeout(r, IORING_OP_TIMEOUT, 24011, &t5s, 0, 0, 0);
	ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1, 24012)->addr = 24011;
	ring_push_sqe(r);
	ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1, 24013)->addr = 24011;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 24011, -ECANCELED);
	expect_cqe(r, 24012, 0);
	expect_cqe(r, 24013, -ENOENT);

	/* TIMEOUT_UPDATE re-arms a 5 s timeout to 10 ms. */
	t0 = now_ns();
	prep_timeout(r, IORING_OP_TIMEOUT, 24021, &t5s, 0, 0, 0);
	sqe = ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1, 24022);
	sqe->addr = 24021;
	sqe->addr2 = (uint64_t)(uintptr_t)&t10;
	sqe->timeout_flags = IORING_TIMEOUT_UPDATE;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 24022, 0);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 24021, -ETIME);
	assert((now_ns() - t0) / 1000000ULL < 1000);

	/* Multishot: 3 expirations, the last without F_MORE. */
	prep_timeout(r, IORING_OP_TIMEOUT, 24031, &t10, 3,
	    IORING_TIMEOUT_MULTISHOT, 0);
	for (int i = 0; i < 3; i++) {
		assert(ring_wait(r, 1, 2000) == 0);
		struct io_uring_cqe c = expect_cqe(r, 24031, -ETIME);
		assert(((c.flags & IORING_CQE_F_MORE) != 0) == (i < 2));
	}

	/* ETIME_SUCCESS does not break a link. */
	prep_timeout(r, IORING_OP_TIMEOUT, 24041, &t10, 0,
	    IORING_TIMEOUT_ETIME_SUCCESS, IOSQE_IO_LINK);
	prep_nop(r, 24042, 0);
	assert(ring_wait(r, 2, 2000) == 0);
	expect_cqe(r, 24041, -ETIME);
	expect_cqe(r, 24042, 0);

	/* Validation: len must be 1, both clocks is invalid. */
	prep_timeout(r, IORING_OP_TIMEOUT, 24051, &t10, 0, 0, 0);
	r->sqes[(r->sq_tail - 1) & r->sq_mask].len = 2;
	prep_timeout(r, IORING_OP_TIMEOUT, 24052, &t10, 0,
	    IORING_TIMEOUT_BOOTTIME | IORING_TIMEOUT_REALTIME, 0);
	prep_timeout(r, IORING_OP_TIMEOUT, 24053, NULL, 0, 0, 0);
	ring_submit(r);
	expect_cqe(r, 24051, -EINVAL);
	expect_cqe(r, 24052, -EINVAL);
	expect_cqe(r, 24053, -EFAULT);

	printf("[PASS] TIMEOUT (relative, ABS, count, remove, update, "
	    "multishot, ETIME_SUCCESS)\n");
}

static void
test_link_timeout(struct test_ring *r)
{
	struct __kernel_timespec t20 = { .tv_nsec = 20000000 };
	struct __kernel_timespec t5s = { .tv_sec = 5 };
	static char buf[64];
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	/* Fires: the guarded RECV is cancelled, and so is its successor. */
	prep_recv(r, sv[0], 25001, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_timeout(r, IORING_OP_LINK_TIMEOUT, 25002, &t20, 0, 0,
	    IOSQE_IO_LINK);
	prep_nop(r, 25003, 0);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 3, 2000) == 0);
	expect_cqe(r, 25002, -ETIME);
	expect_cqe(r, 25001, -ECANCELED);
	expect_cqe(r, 25003, -ECANCELED);

	/* The RECV wins: the timeout is cancelled, the chain goes on. */
	prep_recv(r, sv[0], 25011, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_timeout(r, IORING_OP_LINK_TIMEOUT, 25012, &t5s, 0, 0,
	    IOSQE_IO_LINK);
	prep_nop(r, 25013, 0);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(write(sv[1], "hi", 2) == 2);
	assert(ring_wait(r, 3, 2000) == 0);
	expect_cqe(r, 25011, 2);
	expect_cqe(r, 25012, -ECANCELED);
	expect_cqe(r, 25013, 0);

	/* Guarding a request that completes inline. */
	prep_nop(r, 25021, IOSQE_IO_LINK);
	prep_timeout(r, IORING_OP_LINK_TIMEOUT, 25022, &t5s, 0, 0, 0);
	ring_submit(r);
	expect_cqe(r, 25021, 0);
	expect_cqe(r, 25022, -ECANCELED);

	/* LINK_TIMEOUT_UPDATE shortens an armed link timeout. */
	prep_recv(r, sv[0], 25031, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_timeout(r, IORING_OP_LINK_TIMEOUT, 25032, &t5s, 0, 0, 0);
	ring_submit(r);
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1,
	    25033);
	sqe->addr = 25032;
	sqe->addr2 = (uint64_t)(uintptr_t)&t20;
	sqe->timeout_flags = IORING_LINK_TIMEOUT_UPDATE;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 25033, 0);
	assert(ring_wait(r, 2, 2000) == 0);
	expect_cqe(r, 25032, -ETIME);
	expect_cqe(r, 25031, -ECANCELED);

	/* Must follow a linked request. */
	prep_timeout(r, IORING_OP_LINK_TIMEOUT, 25041, &t20, 0, 0, 0);
	ring_submit(r);
	expect_cqe(r, 25041, -EINVAL);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] LINK_TIMEOUT (fires, cancelled, inline, update)\n");
}

static void
test_poll_remove(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	char c;
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	/* Remove an armed poll. */
	sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 26001);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	ring_prep(r, IORING_OP_POLL_REMOVE, -1, 26002)->addr = 26001;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 26001, -ECANCELED);
	expect_cqe(r, 26002, 0);
	ring_prep(r, IORING_OP_POLL_REMOVE, -1, 26003)->addr = 26001;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 26003, -ENOENT);

	/* Multishot poll: one CQE per readiness edge, with F_MORE. */
	sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 26011);
	sqe->poll32_events = POLLIN;
	sqe->len = IORING_POLL_ADD_MULTI;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	for (int i = 0; i < 3; i++) {
		assert(write(sv[1], "x", 1) == 1);
		assert(ring_wait(r, 1, 2000) == 0);
		struct io_uring_cqe cq = expect_cqe(r, 26011, POLLIN);
		assert((cq.flags & IORING_CQE_F_MORE) != 0);
		assert(read(sv[0], &c, 1) == 1);
	}
	ring_prep(r, IORING_OP_POLL_REMOVE, -1, 26012)->addr = 26011;
	ring_push_sqe(r);
	ring_submit(r);
	struct io_uring_cqe last = expect_cqe(r, 26011, -ECANCELED);
	assert((last.flags & IORING_CQE_F_MORE) == 0);
	expect_cqe(r, 26012, 0);

	/* Update user_data of an armed poll. */
	sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 26021);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_POLL_REMOVE, -1, 26022);
	sqe->addr = 26021;
	sqe->off = 26023;
	sqe->len = IORING_POLL_UPDATE_USER_DATA;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 26022, 0);
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 26023, POLLIN);
	assert(read(sv[0], &c, 1) == 1);

	close(sv[0]);
	close(sv[1]);
	printf("[PASS] POLL_REMOVE (remove, multishot, update user_data)\n");
}

static void
test_cancel_flags(struct test_ring *r)
{
	static char buf[64];
	struct io_uring_sqe *sqe;
	int a[2], b[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, a) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, b) == 0);

	/* CANCEL_ALL by user_data. */
	prep_recv(r, a[0], 27001, buf, sizeof(buf), 0);
	prep_recv(r, b[0], 27001, buf, sizeof(buf), 0);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 27002);
	sqe->addr = 27001;
	sqe->cancel_flags = IORING_ASYNC_CANCEL_ALL;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 27001, -ECANCELED);
	expect_cqe(r, 27001, -ECANCELED);
	expect_cqe(r, 27002, 2);

	/* CANCEL_FD. */
	prep_recv(r, a[0], 27011, buf, sizeof(buf), 0);
	prep_recv(r, b[0], 27012, buf, sizeof(buf), 0);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, b[0], 27013);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 27012, -ECANCELED);
	expect_cqe(r, 27013, 0);

	/* CANCEL_OP | CANCEL_ALL: every armed RECV (27011 on a[0]). */
	sqe = ring_prep(r, IORING_OP_POLL_ADD, b[0], 27021);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 27022);
	sqe->len = IORING_OP_RECV;
	sqe->cancel_flags = IORING_ASYNC_CANCEL_OP | IORING_ASYNC_CANCEL_ALL;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 27011, -ECANCELED);
	expect_cqe(r, 27022, 1);

	/* CANCEL_ANY takes the rest (the poll); then nothing is left. */
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 27031);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 27032);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 27033);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD_FIXED;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 27021, -ECANCELED);
	expect_cqe(r, 27031, 1);
	expect_cqe(r, 27032, -ENOENT);
	expect_cqe(r, 27033, -EINVAL);

	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	printf("[PASS] ASYNC_CANCEL ALL / FD / OP / ANY\n");
}

/*
 * Close a ring holding timers, link timeouts, link chains and drained
 * requests; the callouts then expire against a destroyed ring.
 */
static void
test_close_with_timers(void)
{
	struct __kernel_timespec t10 = { .tv_nsec = 10000000 };
	static char buf[64];
	struct test_ring r2;
	int sv[2];

	ring_init(&r2);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	prep_timeout(&r2, IORING_OP_TIMEOUT, 1, &t10, 0, 0, 0);
	prep_timeout(&r2, IORING_OP_TIMEOUT, 2, &t10, 0,
	    IORING_TIMEOUT_MULTISHOT, 0);
	prep_recv(&r2, sv[0], 3, buf, sizeof(buf), IOSQE_IO_LINK);
	prep_timeout(&r2, IORING_OP_LINK_TIMEOUT, 4, &t10, 0, 0,
	    IOSQE_IO_LINK);
	prep_nop(&r2, 5, 0);
	prep_nop(&r2, 6, IOSQE_IO_DRAIN | IOSQE_IO_LINK);
	prep_nop(&r2, 7, 0);
	ring_submit(&r2);
	assert(ring_cq_ready(&r2) == 0);
	assert(close(r2.fd) == 0);
	usleep(50000);
	assert(write(sv[1], "x", 1) == 1);
	close(sv[0]);
	close(sv[1]);
	printf("[PASS] close() with timers, link timeouts, chains, drains\n");
}

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
/* ------------------------------------------------------------------------
 * Phase 2: the remaining inline opcodes.
 * ------------------------------------------------------------------------
 */

static void
make_tmpdir(char *dir, size_t len)
{
	snprintf(dir, len, "/tmp/kqring.XXXXXX");
	assert(mkdtemp(dir) != NULL);
}

static struct sockaddr_in
udp_bound(int *fdp)
{
	struct sockaddr_in sin = {
		.sin_len = sizeof(sin),
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t slen = sizeof(sin);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	assert(fd >= 0);
	assert(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sin, &slen) == 0);
	*fdp = fd;
	return (sin);
}

static void
test_vectored_io(struct test_ring *r)
{
	char dir[64], path[128], a[5], b[6];
	struct iovec wv[2] = {
		{ .iov_base = "hello ", .iov_len = 6 },
		{ .iov_base = "world", .iov_len = 5 },
	};
	struct iovec rv[2] = {
		{ .iov_base = a, .iov_len = sizeof(a) },
		{ .iov_base = b, .iov_len = sizeof(b) },
	};
	struct io_uring_sqe *sqe;
	int fd, sv[2];

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/v", dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);

	sqe = ring_prep(r, IORING_OP_WRITEV, fd, 30001);
	sqe->addr = (uint64_t)(uintptr_t)wv;
	sqe->len = 2;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	sqe = ring_prep(r, IORING_OP_READV, fd, 30002);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 2;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	assert(memcmp(a, "hello", 5) == 0 && memcmp(b, " world", 6) == 0);
	/* off -1: the file position (still 0 after the positioned I/O). */
	memset(a, 0, sizeof(a));
	sqe = ring_prep(r, IORING_OP_READV, fd, 30003);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 1;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(r);
	assert(submit_one(r) == 5);
	assert(lseek(fd, 0, SEEK_CUR) == 5);
	/* Too many iovecs. */
	sqe = ring_prep(r, IORING_OP_READV, fd, 30004);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 1025;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	close(fd);
	unlink(path);
	rmdir(dir);

	/* On a blocking socket, READV / READ are armed, not blocking. */
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_READV, sv[0], 30005);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 2;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(write(sv[1], "abcdefghijk", 11) == 11);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30005, 11);
	assert(memcmp(a, "abcde", 5) == 0 && memcmp(b, "fghijk", 6) == 0);

	sqe = ring_prep(r, IORING_OP_READ, sv[0], 30006);
	sqe->addr = (uint64_t)(uintptr_t)a;
	sqe->len = sizeof(a);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(write(sv[1], "xy", 2) == 2);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30006, 2);

	sqe = ring_prep(r, IORING_OP_WRITEV, sv[0], 30007);
	sqe->addr = (uint64_t)(uintptr_t)wv;
	sqe->len = 2;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	char got[16];
	assert(read(sv[1], got, sizeof(got)) == 11);
	assert(memcmp(got, "hello world", 11) == 0);
	close(sv[0]);
	close(sv[1]);
	printf("[PASS] READV/WRITEV on files and sockets; socket READ is "
	    "armed\n");
}

/*
 * IORING_FEAT_SUBMIT_STABLE: arguments are copied at submission, so a
 * request that runs later does not see buffers reused after submit.
 */
static void
test_submit_stable(struct test_ring *r)
{
	static char buf[8];
	char dir[64], path[128], saved[128], a[4];
	struct iovec iov = { .iov_base = a, .iov_len = sizeof(a) };
	struct io_uring_sqe *sqe;
	struct stat st;
	int sv[2];

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/stable", dir);
	strlcpy(saved, path, sizeof(saved));
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	prep_recv(r, sv[0], 30101, buf, sizeof(buf), IOSQE_IO_LINK);
	sqe = ring_prep(r, IORING_OP_MKDIRAT, AT_FDCWD, 30102);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->len = 0700;
	sqe->flags = IOSQE_IO_LINK;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_WRITEV, sv[0], 30103);
	sqe->addr = (uint64_t)(uintptr_t)&iov;
	sqe->len = 1;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	/* Reuse the path and iovec memory before the chain runs. */
	memset(path, 'X', sizeof(path) - 1);
	memcpy(a, "abcd", 4);
	iov.iov_len = 1;
	assert(write(sv[1], "go", 2) == 2);
	assert(ring_wait(r, 3, 2000) == 0);
	expect_cqe(r, 30101, 2);
	expect_cqe(r, 30102, 0);
	expect_cqe(r, 30103, 4);
	assert(stat(saved, &st) == 0 && S_ISDIR(st.st_mode));
	assert(read(sv[1], buf, sizeof(buf)) == 4);
	assert(memcmp(buf, "abcd", 4) == 0);
	rmdir(saved);
	rmdir(dir);
	close(sv[0]);
	close(sv[1]);
	printf("[PASS] SUBMIT_STABLE: paths and iovecs copied at submission\n");
}

static void
test_msg_ops(struct test_ring *r)
{
	char data[16];
	_Alignas(struct cmsghdr) char ctl[CMSG_SPACE(sizeof(int))];
	_Alignas(struct cmsghdr) char rctl[CMSG_SPACE(sizeof(int))];
	struct iovec siov[2] = {
		{ .iov_base = "hello ", .iov_len = 6 },
		{ .iov_base = "world", .iov_len = 5 },
	};
	struct iovec riov = { .iov_base = data, .iov_len = sizeof(data) };
	struct msghdr smsg, rmsg;
	struct cmsghdr *cm;
	struct io_uring_sqe *sqe;
	int sv[2], pfd[2];

	/* SENDMSG with SCM_RIGHTS, RECVMSG receiving the descriptor. */
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	assert(pipe(pfd) == 0);
	memset(&smsg, 0, sizeof(smsg));
	smsg.msg_iov = siov;
	smsg.msg_iovlen = 2;
	smsg.msg_control = ctl;
	smsg.msg_controllen = sizeof(ctl);
	cm = CMSG_FIRSTHDR(&smsg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &pfd[0], sizeof(int));
	sqe = ring_prep(r, IORING_OP_SENDMSG, sv[1], 30201);
	sqe->addr = (uint64_t)(uintptr_t)&smsg;
	sqe->len = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);

	memset(&rmsg, 0, sizeof(rmsg));
	rmsg.msg_iov = &riov;
	rmsg.msg_iovlen = 1;
	rmsg.msg_control = rctl;
	rmsg.msg_controllen = sizeof(rctl);
	sqe = ring_prep(r, IORING_OP_RECVMSG, sv[0], 30202);
	sqe->addr = (uint64_t)(uintptr_t)&rmsg;
	sqe->len = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	assert(memcmp(data, "hello world", 11) == 0);
	assert(rmsg.msg_controllen >= CMSG_LEN(sizeof(int)));
	cm = CMSG_FIRSTHDR(&rmsg);
	assert(cm != NULL && cm->cmsg_type == SCM_RIGHTS);
	int rfd;
	memcpy(&rfd, CMSG_DATA(cm), sizeof(int));
	assert(rfd >= 0 && rfd != pfd[0]);
	assert(write(pfd[1], "p", 1) == 1);
	assert(read(rfd, data, 1) == 1 && data[0] == 'p');
	close(rfd);

	/* RECVMSG is armed until data arrives. */
	rmsg.msg_control = NULL;
	rmsg.msg_controllen = 0;
	sqe = ring_prep(r, IORING_OP_RECVMSG, sv[0], 30203);
	sqe->addr = (uint64_t)(uintptr_t)&rmsg;
	sqe->len = 1;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	smsg.msg_control = NULL;
	smsg.msg_controllen = 0;
	sqe = ring_prep(r, IORING_OP_SENDMSG, sv[1], 30204);
	sqe->addr = (uint64_t)(uintptr_t)&smsg;
	sqe->len = 1;
	ring_push_sqe(r);
	assert(ring_wait(r, 2, 2000) == 0);
	expect_cqe(r, 30204, 11);
	expect_cqe(r, 30203, 11);

	/* RECVMSG into a provided buffer. */
	assert(write(sv[1], "bufsel", 6) == 6);
	rmsg.msg_iov = NULL;
	rmsg.msg_iovlen = 0;
	sqe = ring_prep(r, IORING_OP_RECVMSG, sv[0], 30205);
	sqe->addr = (uint64_t)(uintptr_t)&rmsg;
	sqe->len = 1;
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	ring_submit(r);
	struct io_uring_cqe c = expect_cqe(r, 30205, 6);
	assert((c.flags & IORING_CQE_F_BUFFER) != 0);
	uint16_t bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
	assert(memcmp(r->bufs + (size_t)bid * BUF_SIZE, "bufsel", 6) == 0);
	ring_offer_buf(r, bid);
	close(sv[0]);
	close(sv[1]);
	close(pfd[0]);
	close(pfd[1]);

	/* UDP: SENDMSG to an address, RECVMSG reporting the sender. */
	int ua, ub;
	struct sockaddr_in aaddr = udp_bound(&ua), baddr = udp_bound(&ub);
	struct sockaddr_in from;
	smsg.msg_name = &aaddr;
	smsg.msg_namelen = sizeof(aaddr);
	smsg.msg_iov = siov;
	smsg.msg_iovlen = 2;
	sqe = ring_prep(r, IORING_OP_SENDMSG, ub, 30206);
	sqe->addr = (uint64_t)(uintptr_t)&smsg;
	sqe->len = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	memset(&from, 0, sizeof(from));
	rmsg.msg_name = &from;
	rmsg.msg_namelen = sizeof(from);
	rmsg.msg_iov = &riov;
	rmsg.msg_iovlen = 1;
	wait_readable(ua);
	sqe = ring_prep(r, IORING_OP_RECVMSG, ua, 30207);
	sqe->addr = (uint64_t)(uintptr_t)&rmsg;
	sqe->len = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == 11);
	assert(rmsg.msg_namelen == sizeof(from));
	assert(from.sin_port == baddr.sin_port);
	close(ua);
	close(ub);
	printf("[PASS] SENDMSG/RECVMSG: iovecs, SCM_RIGHTS, armed, buffer "
	    "select, names\n");
}

static void
test_recvmsg_multishot(struct test_ring *r)
{
	static const char *msgs[] = { "one", "two", "three" };
	struct msghdr tmpl;
	struct io_uring_sqe *sqe;
	int ua, ub;
	struct sockaddr_in aaddr = udp_bound(&ua), baddr = udp_bound(&ub);

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.msg_namelen = sizeof(struct sockaddr_in);
	sqe = ring_prep(r, IORING_OP_RECVMSG, ua, 30301);
	sqe->addr = (uint64_t)(uintptr_t)&tmpl;
	sqe->len = 1;
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->ioprio = IORING_RECV_MULTISHOT;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	/* The template may be reused right away. */
	tmpl.msg_namelen = 0;

	for (int i = 0; i < 3; i++)
		assert(sendto(ub, msgs[i], strlen(msgs[i]), 0,
		    (struct sockaddr *)&aaddr, sizeof(aaddr)) ==
		    (ssize_t)strlen(msgs[i]));
	for (int i = 0; i < 3; i++) {
		if (ring_cq_ready(r) == 0)
			assert(ring_wait(r, 1, 2000) == 0);
		struct io_uring_cqe c = ring_pop_cqe(r);
		size_t plen = strlen(msgs[i]);
		assert(c.user_data == 30301);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		assert((c.flags & IORING_CQE_F_BUFFER) != 0);
		uint16_t bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
		char *buf = r->bufs + (size_t)bid * BUF_SIZE;
		struct io_uring_recvmsg_out out;
		struct sockaddr_in name;
		memcpy(&out, buf, sizeof(out));
		memcpy(&name, buf + sizeof(out), sizeof(name));
		assert(out.namelen == sizeof(struct sockaddr_in));
		assert(out.controllen == 0);
		assert(out.payloadlen == plen);
		assert(name.sin_port == baddr.sin_port);
		assert(memcmp(buf + sizeof(out) + sizeof(name), msgs[i],
		    plen) == 0);
		assert(c.res == (int32_t)(sizeof(out) + sizeof(name) + plen));
		ring_offer_buf(r, bid);
	}
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 30302);
	sqe->addr = 30301;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 30301 && c.res == -ECANCELED &&
	    (c.flags & IORING_CQE_F_MORE) == 0);
	expect_cqe(r, 30302, 0);
	close(ua);
	close(ub);
	printf("[PASS] multishot RECVMSG: io_uring_recvmsg_out, names, "
	    "payloads\n");
}

static void
test_waitall(struct test_ring *r)
{
	static char buf[16];
	struct io_uring_sqe *sqe;
	int sv[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 30401);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 10;
	sqe->msg_flags = MSG_WAITALL;
	ring_push_sqe(r);
	ring_submit(r);
	assert(write(sv[1], "0123", 4) == 4);
	assert(ring_wait(r, 1, 30) == ETIMEDOUT);
	assert(write(sv[1], "456789", 6) == 6);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30401, 10);
	assert(memcmp(buf, "0123456789", 10) == 0);

	/* EOF ends it with what arrived. */
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 30402);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 10;
	sqe->msg_flags = MSG_WAITALL;
	ring_push_sqe(r);
	ring_submit(r);
	assert(write(sv[1], "abc", 3) == 3);
	assert(ring_wait(r, 1, 30) == ETIMEDOUT);
	close(sv[1]);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30402, 3);
	close(sv[0]);

	/* Multishot with MSG_WAITALL is rejected. */
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 30403);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->ioprio = IORING_RECV_MULTISHOT;
	sqe->msg_flags = MSG_WAITALL;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	printf("[PASS] MSG_WAITALL: RECV waits for the full length or EOF\n");
}

static void
test_send_zc(struct test_ring *r)
{
	struct iovec iov = { .iov_base = "zcmsg", .iov_len = 5 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	char buf[16];
	int sv[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30501);
	sqe->addr = (uint64_t)(uintptr_t)"zcdata";
	sqe->len = 6;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30501, 6);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30501, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30502);
	sqe->addr = (uint64_t)(uintptr_t)&msg;
	sqe->len = 1;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30502, 5);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30502, (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* Skipping the result still delivers the notification. */
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30503);
	sqe->addr = (uint64_t)(uintptr_t)"s";
	sqe->len = 1;
	sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30503, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(read(sv[1], buf, sizeof(buf)) == 12);
	assert(memcmp(buf, "zcdatazcmsgs", 12) == 0);
	close(sv[0]);
	close(sv[1]);

	/* SEND with a destination address (sendto). */
	int ua, ub;
	struct sockaddr_in aaddr = udp_bound(&ua);
	(void)udp_bound(&ub);
	sqe = ring_prep(r, IORING_OP_SEND, ub, 30504);
	sqe->addr = (uint64_t)(uintptr_t)"dgram";
	sqe->len = 5;
	sqe->addr2 = (uint64_t)(uintptr_t)&aaddr;
	sqe->addr_len = sizeof(aaddr);
	ring_push_sqe(r);
	assert(submit_one(r) == 5);
	wait_readable(ua);
	assert(recv(ua, buf, sizeof(buf), 0) == 5);
	close(ua);
	close(ub);
	printf("[PASS] SEND_ZC/SENDMSG_ZC notifications, SEND to an address\n");
}

static void
test_socket_ops(struct test_ring *r)
{
	struct sockaddr_in sin = {
		.sin_len = sizeof(sin),
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	}, peer, local;
	socklen_t slen = sizeof(sin), plen;
	struct io_uring_sqe *sqe;
	char c1;

	sqe = ring_prep(r, IORING_OP_SOCKET, AF_INET, 30601);
	sqe->off = SOCK_STREAM;
	ring_push_sqe(r);
	int lfd = submit_one(r);
	assert(lfd >= 0);
	sqe = ring_prep(r, IORING_OP_BIND, lfd, 30602);
	sqe->addr = (uint64_t)(uintptr_t)&sin;
	sqe->addr2 = sizeof(sin);
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	sqe = ring_prep(r, IORING_OP_LISTEN, lfd, 30603);
	sqe->len = 16;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(getsockname(lfd, (struct sockaddr *)&sin, &slen) == 0);

	sqe = ring_prep(r, IORING_OP_SOCKET, AF_INET, 30604);
	sqe->off = SOCK_STREAM;
	ring_push_sqe(r);
	int cfd = submit_one(r);
	assert(cfd >= 0);
	sqe = ring_prep(r, IORING_OP_CONNECT, cfd, 30605);
	sqe->addr = (uint64_t)(uintptr_t)&sin;
	sqe->addr2 = sizeof(sin);
	ring_push_sqe(r);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30605, 0);

	plen = sizeof(peer);
	sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 30606);
	sqe->addr = (uint64_t)(uintptr_t)&peer;
	sqe->addr2 = (uint64_t)(uintptr_t)&plen;
	ring_push_sqe(r);
	assert(ring_wait(r, 1, 2000) == 0);
	int afd = ring_pop_cqe(r).res;
	assert(afd >= 0);
	slen = sizeof(local);
	assert(getsockname(cfd, (struct sockaddr *)&local, &slen) == 0);
	assert(plen == sizeof(peer) && peer.sin_port == local.sin_port);

	sqe = ring_prep(r, IORING_OP_SHUTDOWN, cfd, 30607);
	sqe->len = SHUT_WR;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(read(afd, &c1, 1) == 0);

	sqe = ring_prep(r, IORING_OP_CLOSE, afd, 30608);
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(fcntl(afd, F_GETFD) == -1 && errno == EBADF);
	/* A ring cannot close itself. */
	sqe = ring_prep(r, IORING_OP_CLOSE, r->fd, 30609);
	ring_push_sqe(r);
	assert(submit_one(r) == -EBADF);
	close(cfd);

	/* A refused connection. */
	close(lfd);
	sqe = ring_prep(r, IORING_OP_SOCKET, AF_INET, 30610);
	sqe->off = SOCK_STREAM;
	ring_push_sqe(r);
	cfd = submit_one(r);
	sqe = ring_prep(r, IORING_OP_CONNECT, cfd, 30611);
	sqe->addr = (uint64_t)(uintptr_t)&sin;
	sqe->addr2 = sizeof(sin);
	ring_push_sqe(r);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 30611, -ECONNREFUSED);
	close(cfd);
	printf("[PASS] SOCKET/BIND/LISTEN/CONNECT/ACCEPT addr/SHUTDOWN/CLOSE\n");
}

static void
test_accept_multishot(struct test_ring *r)
{
	uint16_t port;
	int lfd = tcp_listener(&port), cfd[3];
	struct io_uring_sqe *sqe;

	sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 30701);
	sqe->ioprio = IORING_ACCEPT_MULTISHOT;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	for (int i = 0; i < 3; i++)
		cfd[i] = tcp_connect(port);
	for (int i = 0; i < 3; i++) {
		if (ring_cq_ready(r) == 0)
			assert(ring_wait(r, 1, 2000) == 0);
		struct io_uring_cqe c = ring_pop_cqe(r);
		assert(c.user_data == 30701 && c.res >= 0);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		close(c.res);
	}
	sqe = ring_prep(r, IORING_OP_ASYNC_CANCEL, -1, 30702);
	sqe->addr = 30701;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	struct io_uring_cqe c = expect_cqe(r, 30701, -ECANCELED);
	assert((c.flags & IORING_CQE_F_MORE) == 0);
	expect_cqe(r, 30702, 0);

	/* DONTWAIT fails instead of arming. */
	sqe = ring_prep(r, IORING_OP_ACCEPT, lfd, 30703);
	sqe->ioprio = IORING_ACCEPT_DONTWAIT;
	ring_push_sqe(r);
	assert(submit_one(r) == -EAGAIN);
	for (int i = 0; i < 3; i++)
		close(cfd[i]);
	close(lfd);
	printf("[PASS] multishot ACCEPT, ACCEPT_DONTWAIT\n");
}

static void
test_path_ops(struct test_ring *r)
{
	char dir[64], lbuf[16];
	struct open_how how = {
		.flags = O_RDWR | O_CREAT,
		.mode = 0644,
		.resolve = RESOLVE_BENEATH,
	};
	struct statx stx;
	struct stat st;
	struct io_uring_sqe *sqe;
	int dfd, fd, res;

	make_tmpdir(dir, sizeof(dir));
	dfd = open(dir, O_DIRECTORY);
	assert(dfd >= 0);

	sqe = ring_prep(r, IORING_OP_MKDIRAT, dfd, 30801);
	sqe->addr = (uint64_t)(uintptr_t)"sub";
	sqe->len = 0755;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);

	sqe = ring_prep(r, IORING_OP_OPENAT2, dfd, 30802);
	sqe->addr = (uint64_t)(uintptr_t)"sub/f";
	sqe->addr2 = (uint64_t)(uintptr_t)&how;
	sqe->len = sizeof(how);
	ring_push_sqe(r);
	fd = submit_one(r);
	assert(fd >= 0);
	assert(write(fd, "0123456789", 10) == 10);

	/* RESOLVE_BENEATH stops an escape from dfd. */
	how.flags = O_RDONLY;
	how.mode = 0;
	sqe = ring_prep(r, IORING_OP_OPENAT2, dfd, 30803);
	sqe->addr = (uint64_t)(uintptr_t)"../";
	sqe->addr2 = (uint64_t)(uintptr_t)&how;
	sqe->len = sizeof(how);
	ring_push_sqe(r);
	assert(submit_one(r) == -ENOTCAPABLE);
	/* A mode without O_CREAT, and unknown trailing struct bytes. */
	how.mode = 0600;
	sqe = ring_prep(r, IORING_OP_OPENAT2, dfd, 30804);
	sqe->addr = (uint64_t)(uintptr_t)"sub/f";
	sqe->addr2 = (uint64_t)(uintptr_t)&how;
	sqe->len = sizeof(how);
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	struct { struct open_how h; uint64_t extra; } big = {
		.h = { .flags = O_RDONLY }, .extra = 1,
	};
	sqe = ring_prep(r, IORING_OP_OPENAT2, dfd, 30805);
	sqe->addr = (uint64_t)(uintptr_t)"sub/f";
	sqe->addr2 = (uint64_t)(uintptr_t)&big;
	sqe->len = sizeof(big);
	ring_push_sqe(r);
	assert(submit_one(r) == -E2BIG);

	memset(&stx, 0, sizeof(stx));
	sqe = ring_prep(r, IORING_OP_STATX, dfd, 30806);
	sqe->addr = (uint64_t)(uintptr_t)"sub/f";
	sqe->len = STATX_BASIC_STATS;
	sqe->addr2 = (uint64_t)(uintptr_t)&stx;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert((stx.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS);
	assert(stx.stx_size == 10 && S_ISREG(stx.stx_mode));
	assert((stx.stx_mode & 0777) == 0644);
	assert(fstat(fd, &st) == 0 && stx.stx_ino == st.st_ino);
	/* AT_EMPTY_PATH: the descriptor itself. */
	sqe = ring_prep(r, IORING_OP_STATX, fd, 30807);
	sqe->addr = (uint64_t)(uintptr_t)"";
	sqe->statx_flags = AT_EMPTY_PATH;
	sqe->addr2 = (uint64_t)(uintptr_t)&stx;
	ring_push_sqe(r);
	assert(submit_one(r) == 0 && stx.stx_size == 10);

	sqe = ring_prep(r, IORING_OP_FTRUNCATE, fd, 30808);
	sqe->off = 4;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(fstat(fd, &st) == 0 && st.st_size == 4);
	sqe = ring_prep(r, IORING_OP_FALLOCATE, fd, 30809);
	sqe->off = 0;
	sqe->addr = 8192;
	ring_push_sqe(r);
	res = submit_one(r);
	assert(res == 0 || res == -EOPNOTSUPP || res == -EINVAL);
	if (res == 0)
		assert(fstat(fd, &st) == 0 && st.st_size == 8192);
	sqe = ring_prep(r, IORING_OP_FADVISE, fd, 30810);
	sqe->fadvise_advice = POSIX_FADV_SEQUENTIAL;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	sqe = ring_prep(r, IORING_OP_SYNC_FILE_RANGE, fd, 30811);
	sqe->sync_range_flags = SYNC_FILE_RANGE_WRITE;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(page != MAP_FAILED);
	sqe = ring_prep(r, IORING_OP_MADVISE, -1, 30812);
	sqe->addr = (uint64_t)(uintptr_t)page;
	sqe->len = 4096;
	sqe->fadvise_advice = MADV_WILLNEED;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	munmap(page, 4096);
	close(fd);

	sqe = ring_prep(r, IORING_OP_RENAMEAT, dfd, 30813);
	sqe->addr = (uint64_t)(uintptr_t)"sub/f";
	sqe->len = (uint32_t)dfd;
	sqe->addr2 = (uint64_t)(uintptr_t)"sub/g";
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	sqe = ring_prep(r, IORING_OP_LINKAT, dfd, 30814);
	sqe->addr = (uint64_t)(uintptr_t)"sub/g";
	sqe->len = (uint32_t)dfd;
	sqe->addr2 = (uint64_t)(uintptr_t)"sub/h";
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(fstatat(dfd, "sub/g", &st, 0) == 0 && st.st_nlink == 2);
	sqe = ring_prep(r, IORING_OP_SYMLINKAT, dfd, 30815);
	sqe->addr = (uint64_t)(uintptr_t)"g";
	sqe->addr2 = (uint64_t)(uintptr_t)"sub/s";
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	memset(lbuf, 0, sizeof(lbuf));
	assert(readlinkat(dfd, "sub/s", lbuf, sizeof(lbuf)) == 1);
	assert(lbuf[0] == 'g');
	sqe = ring_prep(r, IORING_OP_RENAMEAT, dfd, 30816);
	sqe->addr = (uint64_t)(uintptr_t)"sub/g";
	sqe->len = (uint32_t)dfd;
	sqe->addr2 = (uint64_t)(uintptr_t)"sub/h";
	sqe->rename_flags = AT_RENAME_NOREPLACE;
	ring_push_sqe(r);
	assert(submit_one(r) == -EEXIST);

	static const char *gone[] = { "sub/s", "sub/h", "sub/g" };
	for (int i = 0; i < 3; i++) {
		sqe = ring_prep(r, IORING_OP_UNLINKAT, dfd, 30817);
		sqe->addr = (uint64_t)(uintptr_t)gone[i];
		ring_push_sqe(r);
		assert(submit_one(r) == 0);
	}
	sqe = ring_prep(r, IORING_OP_UNLINKAT, dfd, 30818);
	sqe->addr = (uint64_t)(uintptr_t)"sub";
	sqe->unlink_flags = AT_REMOVEDIR;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	close(dfd);
	assert(rmdir(dir) == 0);
	printf("[PASS] MKDIRAT/OPENAT2/STATX/FTRUNCATE/FALLOCATE/FADVISE/"
	    "MADVISE/RENAMEAT/LINKAT/SYMLINKAT/UNLINKAT\n");
}

static int32_t
xattr_op(struct test_ring *r, uint8_t opcode, int fd, const char *name,
    void *value, uint32_t len, const char *path, uint32_t xflags)
{
	struct io_uring_sqe *sqe = ring_prep(r, opcode, fd, 30900);
	sqe->addr = (uint64_t)(uintptr_t)name;
	sqe->addr2 = (uint64_t)(uintptr_t)value;
	sqe->len = len;
	sqe->addr3 = (uint64_t)(uintptr_t)path;
	sqe->xattr_flags = xflags;
	ring_push_sqe(r);
	return (submit_one(r));
}

static void
test_xattr(struct test_ring *r)
{
	char dir[64], path[128], val[16];
	int fd, res;

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/x", dir);
	fd = open(path, O_RDWR | O_CREAT, 0600);
	assert(fd >= 0);

	res = xattr_op(r, IORING_OP_FSETXATTR, fd, "user.kq", "v1", 2, NULL,
	    0);
	if (res == -EOPNOTSUPP) {
		printf("[SKIP] xattr: file system has no extended attributes\n");
		goto out;
	}
	assert(res == 0);
	assert(xattr_op(r, IORING_OP_FGETXATTR, fd, "user.kq", NULL, 0, NULL,
	    0) == 2);
	assert(xattr_op(r, IORING_OP_FGETXATTR, fd, "user.kq", val, 1, NULL,
	    0) == -ERANGE);
	memset(val, 0, sizeof(val));
	assert(xattr_op(r, IORING_OP_FGETXATTR, fd, "user.kq", val,
	    sizeof(val), NULL, 0) == 2);
	assert(memcmp(val, "v1", 2) == 0);
	assert(xattr_op(r, IORING_OP_FSETXATTR, fd, "user.kq", "v2", 2, NULL,
	    XATTR_CREATE) == -EEXIST);
	assert(xattr_op(r, IORING_OP_FSETXATTR, fd, "user.none", "v", 1,
	    NULL, XATTR_REPLACE) == -ENOATTR);
	assert(xattr_op(r, IORING_OP_SETXATTR, -1, "user.p", "path!", 5, path,
	    0) == 0);
	memset(val, 0, sizeof(val));
	assert(xattr_op(r, IORING_OP_GETXATTR, -1, "user.p", val, sizeof(val),
	    path, 0) == 5);
	assert(memcmp(val, "path!", 5) == 0);
	assert(xattr_op(r, IORING_OP_FSETXATTR, fd, "security.x", "v", 1,
	    NULL, 0) == -EOPNOTSUPP);
	assert(xattr_op(r, IORING_OP_FSETXATTR, fd, "user.", "v", 1, NULL,
	    0) == -EINVAL);
	printf("[PASS] FSETXATTR/FGETXATTR/SETXATTR/GETXATTR via extattr\n");
out:
	close(fd);
	unlink(path);
	rmdir(dir);
}

static void
prep_futex(struct test_ring *r, uint8_t opcode, _Atomic uint32_t *word,
    uint64_t val, uint64_t mask, uint64_t ud)
{
	struct io_uring_sqe *sqe = ring_prep(r, opcode,
	    FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, ud);
	sqe->addr = (uint64_t)(uintptr_t)word;
	sqe->addr2 = val;
	sqe->addr3 = mask;
	ring_push_sqe(r);
}

static _Atomic uint32_t umtx_word;
static _Atomic int umtx_woken;

static void *
umtx_sleeper(void *arg)
{
	(void)arg;
	_umtx_op(&umtx_word, UMTX_OP_WAIT_UINT_PRIVATE, 0, NULL, NULL);
	atomic_store(&umtx_woken, 1);
	return (NULL);
}

static void
test_futex_wake(struct test_ring *r)
{
	static _Atomic uint32_t w, wv[3];
	struct test_ring r2;
	struct io_uring_sqe *sqe;

	/* FUTEX_WAKE ends a wait without the word changing. */
	atomic_store(&w, 0);
	prep_futex(r, IORING_OP_FUTEX_WAIT, &w, 0, 0, 31001);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	prep_futex(r, IORING_OP_FUTEX_WAKE, &w, 1, 0, 31002);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	expect_cqe(r, 31002, 1);
	expect_cqe(r, 31001, 0);

	/* Masks must intersect. */
	prep_futex(r, IORING_OP_FUTEX_WAIT, &w, 0, 0x1, 31003);
	ring_submit(r);
	prep_futex(r, IORING_OP_FUTEX_WAKE, &w, 1, 0x2, 31004);
	assert(submit_one(r) == 0);
	prep_futex(r, IORING_OP_FUTEX_WAKE, &w, 1, 0x3, 31005);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	expect_cqe(r, 31005, 1);
	expect_cqe(r, 31003, 0);

	/* Another ring's FUTEX_WAKE wakes this ring's waiter. */
	ring_init(&r2);
	prep_futex(r, IORING_OP_FUTEX_WAIT, &w, 0, 0, 31006);
	ring_submit(r);
	prep_futex(&r2, IORING_OP_FUTEX_WAKE, &w, 10, 0, 31007);
	assert(submit_one(&r2) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 31006, 0);

	/* FUTEX_WAITV completes with the index of the woken word. */
	struct futex_waitv vec[3];
	for (int i = 0; i < 3; i++) {
		atomic_store(&wv[i], 0);
		vec[i] = (struct futex_waitv){
			.val = 0,
			.uaddr = (uint64_t)(uintptr_t)&wv[i],
			.flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE,
		};
	}
	sqe = ring_prep(r, IORING_OP_FUTEX_WAITV, 0, 31008);
	sqe->addr = (uint64_t)(uintptr_t)vec;
	sqe->len = 3;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	prep_futex(&r2, IORING_OP_FUTEX_WAKE, &wv[1], 1, 0, 31009);
	assert(submit_one(&r2) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	expect_cqe(r, 31008, 1);
	/* A word changed before arming. */
	atomic_store(&wv[2], 5);
	sqe = ring_prep(r, IORING_OP_FUTEX_WAITV, 0, 31010);
	sqe->addr = (uint64_t)(uintptr_t)vec;
	sqe->len = 3;
	ring_push_sqe(r);
	assert(submit_one(r) == -EAGAIN);
	close(r2.fd);

	/* Threads in _umtx_op(UMTX_OP_WAIT_UINT_PRIVATE) are woken too. */
	pthread_t t;
	atomic_store(&umtx_word, 0);
	atomic_store(&umtx_woken, 0);
	assert(pthread_create(&t, NULL, umtx_sleeper, NULL) == 0);
	for (int i = 0; i < 200 && atomic_load(&umtx_woken) == 0; i++) {
		usleep(10000);
		prep_futex(r, IORING_OP_FUTEX_WAKE, &umtx_word, 1, 0, 31011);
		assert(submit_one(r) == 0);
	}
	assert(pthread_join(t, NULL) == 0);
	assert(atomic_load(&umtx_woken) == 1);

	/* Only 32-bit, aligned words. */
	sqe = ring_prep(r, IORING_OP_FUTEX_WAIT, FUTEX2_SIZE_U8, 31012);
	sqe->addr = (uint64_t)(uintptr_t)&w;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	prep_futex(r, IORING_OP_FUTEX_WAKE,
	    (_Atomic uint32_t *)((char *)&w + 1), 1, 0, 31013);
	assert(submit_one(r) == -EINVAL);
	printf("[PASS] FUTEX_WAKE (same/other ring, masks, umtx), "
	    "FUTEX_WAITV\n");
}

static _Atomic int msg_ring_fd = -1;
static _Atomic int msg_ring_got;

static void *
msg_ring_receiver(void *arg)
{
	struct test_ring r2;

	(void)arg;
	ring_init(&r2);
	atomic_store(&msg_ring_fd, r2.fd);
	/* Sleeps in the kernel until the other thread's MSG_RING. */
	assert(ring_wait(&r2, 1, 5000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(&r2);
	assert(c.user_data == 0xfeed && c.res == 77);
	atomic_store(&msg_ring_got, 1);
	close(r2.fd);
	return (NULL);
}

static void
test_msg_ring(struct test_ring *r)
{
	struct test_ring r2;
	struct io_uring_sqe *sqe;
	int pfd[2];

	ring_init(&r2);
	sqe = ring_prep(r, IORING_OP_MSG_RING, r2.fd, 32001);
	sqe->addr = IORING_MSG_DATA;
	sqe->len = 123;
	sqe->off = 0xabc;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(kqring_enter(r2.fd, 0, 0, 0, NULL, 0) == 0);
	struct io_uring_cqe c = expect_cqe(&r2, 0xabc, 123);
	assert(c.flags == 0);

	/* FLAGS_PASS sets the target CQE's flags. */
	sqe = ring_prep(r, IORING_OP_MSG_RING, r2.fd, 32002);
	sqe->addr = IORING_MSG_DATA;
	sqe->off = 0xabd;
	sqe->msg_ring_flags = IORING_MSG_RING_FLAGS_PASS;
	sqe->file_index = 0x5a0000;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(kqring_enter(r2.fd, 0, 0, 0, NULL, 0) == 0);
	c = expect_cqe(&r2, 0xabd, 0);
	assert(c.flags == 0x5a0000);
	close(r2.fd);

	/* To itself. */
	sqe = ring_prep(r, IORING_OP_MSG_RING, r->fd, 32003);
	sqe->addr = IORING_MSG_DATA;
	sqe->len = 9;
	sqe->off = 0xabe;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	expect_cqe(r, 32003, 0);
	expect_cqe(r, 0xabe, 9);

	/* Not a ring; SEND_FD needs fixed files. */
	assert(pipe(pfd) == 0);
	sqe = ring_prep(r, IORING_OP_MSG_RING, pfd[0], 32004);
	ring_push_sqe(r);
	assert(submit_one(r) == -EBADF);
	sqe = ring_prep(r, IORING_OP_MSG_RING, pfd[0], 32005);
	sqe->addr = IORING_MSG_SEND_FD;
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	close(pfd[0]);
	close(pfd[1]);

	/* Wakes a ring owner sleeping in another thread. */
	pthread_t t;
	atomic_store(&msg_ring_fd, -1);
	atomic_store(&msg_ring_got, 0);
	assert(pthread_create(&t, NULL, msg_ring_receiver, NULL) == 0);
	while (atomic_load(&msg_ring_fd) < 0)
		usleep(1000);
	usleep(20000);
	sqe = ring_prep(r, IORING_OP_MSG_RING, atomic_load(&msg_ring_fd),
	    32006);
	sqe->addr = IORING_MSG_DATA;
	sqe->len = 77;
	sqe->off = 0xfeed;
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	assert(pthread_join(t, NULL) == 0);
	assert(atomic_load(&msg_ring_got) == 1);
	printf("[PASS] MSG_RING: data, FLAGS_PASS, self, wakes sleeper\n");
}

static void
test_waitid(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	siginfo_t si;
	pid_t pid;

	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		usleep(30000);
		_exit(7);
	}
	memset(&si, 0, sizeof(si));
	sqe = ring_prep(r, IORING_OP_WAITID, (int)pid, 33001);
	sqe->len = P_PID;
	sqe->file_index = WEXITED;
	sqe->addr2 = (uint64_t)(uintptr_t)&si;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	assert(ring_wait(r, 1, 3000) == 0);
	expect_cqe(r, 33001, 0);
	assert(si.si_pid == pid && si.si_status == 7 &&
	    si.si_code == CLD_EXITED);

	/*
	 * WNOHANG: nothing to report is not an error, and reports zeroes
	 * (as waitid(2), though the kernel fills siginfo for a live child).
	 */
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		usleep(200000);
		_exit(0);
	}
	memset(&si, 0xff, sizeof(si));
	sqe = ring_prep(r, IORING_OP_WAITID, (int)pid, 33002);
	sqe->len = P_PID;
	sqe->file_index = WEXITED | WNOHANG;
	sqe->addr2 = (uint64_t)(uintptr_t)&si;
	ring_push_sqe(r);
	assert(submit_one(r) == 0 && si.si_pid == 0 && si.si_signo == 0);
	assert(waitpid(pid, NULL, 0) == pid);
	sqe = ring_prep(r, IORING_OP_WAITID, (int)pid, 33003);
	sqe->len = P_PID;
	sqe->file_index = WEXITED;
	ring_push_sqe(r);
	assert(submit_one(r) == -ECHILD);
	printf("[PASS] WAITID: armed until the child exits, WNOHANG, ECHILD\n");
}

static void
test_uring_cmd(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	int sv[2], v = 0, pfd[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	assert(write(sv[1], "12345", 5) == 5);
	sqe = ring_prep(r, IORING_OP_URING_CMD, sv[0], 34001);
	sqe->cmd_op = SOCKET_URING_OP_SIOCINQ;
	ring_push_sqe(r);
	assert(submit_one(r) == 5);
	sqe = ring_prep(r, IORING_OP_URING_CMD, sv[1], 34002);
	sqe->cmd_op = SOCKET_URING_OP_SIOCOUTQ;
	ring_push_sqe(r);
	assert(submit_one(r) >= 0);

	sqe = ring_prep(r, IORING_OP_URING_CMD, sv[0], 34003);
	sqe->cmd_op = SOCKET_URING_OP_GETSOCKOPT;
	sqe->level = SOL_SOCKET;
	sqe->optname = SO_TYPE;
	sqe->optval = (uint64_t)(uintptr_t)&v;
	sqe->optlen = sizeof(v);
	ring_push_sqe(r);
	assert(submit_one(r) == (int32_t)sizeof(v) && v == SOCK_STREAM);

	v = 32768;
	sqe = ring_prep(r, IORING_OP_URING_CMD, sv[0], 34004);
	sqe->cmd_op = SOCKET_URING_OP_SETSOCKOPT;
	sqe->level = SOL_SOCKET;
	sqe->optname = SO_SNDBUF;
	sqe->optval = (uint64_t)(uintptr_t)&v;
	sqe->optlen = sizeof(v);
	ring_push_sqe(r);
	assert(submit_one(r) == 0);
	socklen_t vlen = sizeof(v);
	v = 0;
	assert(getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &v, &vlen) == 0);
	assert(v >= 32768);

	sqe = ring_prep(r, IORING_OP_URING_CMD, sv[0], 34005);
	sqe->cmd_op = SOCKET_URING_OP_GETSOCKOPT;
	sqe->level = IPPROTO_TCP;
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	close(sv[0]);
	close(sv[1]);

	assert(pipe(pfd) == 0);
	sqe = ring_prep(r, IORING_OP_URING_CMD, pfd[0], 34006);
	sqe->cmd_op = SOCKET_URING_OP_SIOCINQ;
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	close(pfd[0]);
	close(pfd[1]);
	printf("[PASS] URING_CMD: SIOCINQ/SIOCOUTQ/GETSOCKOPT/SETSOCKOPT\n");
}

static void
test_read_multishot(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	int sv[2], pfd[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_READ_MULTISHOT, sv[0], 35001);
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	for (int i = 0; i < 3; i++) {
		assert(write(sv[1], "m", 1) == 1);
		assert(ring_wait(r, 1, 2000) == 0);
		struct io_uring_cqe c = expect_cqe(r, 35001, 1);
		assert((c.flags & (IORING_CQE_F_MORE | IORING_CQE_F_BUFFER)) ==
		    (IORING_CQE_F_MORE | IORING_CQE_F_BUFFER));
		ring_offer_buf(r, c.flags >> IORING_CQE_BUFFER_SHIFT);
	}
	/* EOF ends it. */
	close(sv[1]);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = expect_cqe(r, 35001, 0);
	assert((c.flags & IORING_CQE_F_MORE) == 0);
	close(sv[0]);

	/* Needs readiness, which only sockets have here. */
	assert(pipe(pfd) == 0);
	sqe = ring_prep(r, IORING_OP_READ_MULTISHOT, pfd[0], 35002);
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	close(pfd[0]);
	close(pfd[1]);
	printf("[PASS] READ_MULTISHOT on a socket, until EOF\n");
}

static void
test_phase2_validation(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	char buf[8];

	sqe = ring_prep(r, IORING_OP_READ, 0, 36001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->rw_flags = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	sqe = ring_prep(r, IORING_OP_NOP, -1, 36002);
	sqe->personality = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	static const uint8_t later[] = {
		IORING_OP_EPOLL_CTL, IORING_OP_SPLICE, IORING_OP_TEE,
		IORING_OP_READ_FIXED, IORING_OP_PROVIDE_BUFFERS,
	};
	for (size_t i = 0; i < sizeof(later); i++) {
		ring_prep(r, later[i], -1, 36003);
		ring_push_sqe(r);
		assert(submit_one(r) == -EOPNOTSUPP);
	}

	/* ioctl(2) itself returns 0, not an SQE's result (a new fd). */
	sqe = ring_prep(r, IORING_OP_OPENAT, AT_FDCWD, 36004);
	sqe->addr = (uint64_t)(uintptr_t)"/dev/null";
	sqe->open_flags = O_RDONLY;
	ring_push_sqe(r);
	struct kqring_enter_arg ea = { .to_submit = 1 };
	assert(ioctl(r->fd, KQRING_IOC_ENTER, &ea) == 0);
	assert(ea.SUBMITTED_out == 1);
	assert(ring_cq_ready(r) == 1);
	struct io_uring_cqe c = ring_pop_cqe(r);
	int nfd = c.res;
	assert(c.user_data == 36004 && nfd >= 0);
	close(nfd);
	printf("[PASS] Phase 2 validation: rw_flags, personality, unsupported "
	    "ops, ioctl return\n");
}

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
		test_sqe_validation(&r);
		test_enter_validation(&r);
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
		test_links(&r);
		test_cqe_skip(&r);
		test_drain(&r);
		test_timeouts(&r);
		test_link_timeout(&r);
		test_poll_remove(&r);
		test_cancel_flags(&r);
		test_close_with_timers();
		test_vectored_io(&r);
		test_submit_stable(&r);
		test_msg_ops(&r);
		test_recvmsg_multishot(&r);
		test_waitall(&r);
		test_send_zc(&r);
		test_socket_ops(&r);
		test_accept_multishot(&r);
		test_path_ops(&r);
		test_xattr(&r);
		test_futex_wake(&r);
		test_msg_ring(&r);
		test_waitid(&r);
		test_uring_cmd(&r);
		test_read_multishot(&r);
		test_phase2_validation(&r);
		printf("%s: %d failure(s)\n", failures ? "[FAIL]" : "[OK]",
		    failures);
	}
	if (run_bench)
		bench_ping_pong(&r);
	close(r.fd);
	return (failures ? 1 : 0);
}
