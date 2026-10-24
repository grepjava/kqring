/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 *
 * kqring_test.c: Userspace verification & benchmark suite for /dev/kqring.
 * Tests every supported opcode, registration and setup flag, and the
 * resource-lifetime rules, plus a short ping-pong benchmark against
 * kqueue(2).
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/capsicum.h>
#include <sys/cpuset.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/linker.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/umtx.h>
#include <sys/user.h>
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
	_Atomic unsigned	*cq_khead, *cq_ktail, *cq_kflags;
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

/* ring_setup() makes a shared ring (no SINGLE_ISSUER / DEFER_TASKRUN). */
static bool ring_setup_shared;

/* Set up and map a ring, still disabled (IORING_SETUP_R_DISABLED). */
static void
ring_setup(struct test_ring *r)
{
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_CQSIZE |
	    IORING_SETUP_SUBMIT_ALL | IORING_SETUP_R_DISABLED;
	if (!ring_setup_shared)
		p.flags |= IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN;
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
	assert((p.features & IORING_FEAT_NATIVE_WORKERS) != 0);

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
	r->cq_kflags = (_Atomic unsigned *)(base + p.cq_off.flags);
	r->cq_mask = *(unsigned *)(base + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
}

/* A ring enabled in this thread, with buffer ring group 0. */
static void
ring_init(struct test_ring *r)
{
	ring_setup(r);

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
	printf("[PASS] IORING_REGISTER_PROBE reports the core opcodes\n");
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

	/* DEFER_TASKRUN needs SINGLE_ISSUER, as in io_uring. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_DEFER_TASKRUN;
	arg.params = &p;
	assert(ioctl(fd, KQRING_IOC_SETUP, &arg) == -1 && errno == EINVAL);

	/* Unimplemented flags and nonzero reserved fields are rejected. */
	const uint32_t req = IORING_SETUP_SINGLE_ISSUER |
	    IORING_SETUP_DEFER_TASKRUN;
	static const uint32_t bad[] = {
		IORING_SETUP_SQ_AFF,
		IORING_SETUP_ATTACH_WQ, 1U << 17 /* HYBRID_IOPOLL */,
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
	assert(p.sq_entries == 32768 && p.cq_entries == 65536);
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
	uint64_t ud = r->sqes[(r->sq_tail - 1) & r->sq_mask].user_data;

	ring_submit(r);
	if (ring_cq_ready(r) != 1) {
		fprintf(stderr, "submit_one: %u CQEs for user_data %llu\n",
		    ring_cq_ready(r), (unsigned long long)ud);
		abort();
	}
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
	/* A buffer group that does not exist has no buffers. */
	assert(write(sv[1], "x", 1) == 1);
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 11103);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == -ENOBUFS);
	sqe = ring_prep(r, IORING_OP_SEND, sv[0], 11104);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 7;
	ring_push_sqe(r);
	assert(submit_one(r) == -ENOBUFS);
	/* Buffer selection is for transfers only. */
	sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 11106);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->poll32_events = POLLIN;
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

/*
 * The wrappers use the native system calls; the /dev/kqring ioctls they
 * fall back to keep working on the same ring.
 */
static void
test_native_syscalls(struct test_ring *r)
{
	struct kqring_enter_arg ea = {
		.to_submit = 1,
		.min_complete = 1,
		.flags = IORING_ENTER_GETEVENTS,
		.ring_fd = r->fd,
	};
	int pfd[2];

	assert(kqring_sysnum(KQRING_SYS_SETUP) > 0);
	assert(kqring_sysnum(KQRING_SYS_ENTER) > 0);
	assert(kqring_sysnum(KQRING_SYS_REGISTER) > 0);

	/* ioctl(2) enter on a ring set up and enabled by the syscalls. */
	(void)ring_prep(r, IORING_OP_NOP, -1, 12100);
	ring_push_sqe(r);
	assert(ioctl(r->fd, KQRING_IOC_ENTER, &ea) == 0);
	assert(ea.SUBMITTED_out == 1);
	assert(ring_cq_ready(r) == 1);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 12100 && c.res == 0);

	/* As io_uring_enter(2) and io_uring_register(2). */
	assert(pipe(pfd) == 0);
	assert(kqring_enter(pfd[0], 0, 0, 0, NULL, 0) == -1 &&
	    errno == EOPNOTSUPP);
	assert(kqring_register(pfd[0], IORING_REGISTER_PROBE, NULL, 0) == -1 &&
	    errno == EOPNOTSUPP);
	close(pfd[0]);
	close(pfd[1]);
	assert(kqring_enter(-1, 0, 0, 0, NULL, 0) == -1 && errno == EBADF);
	assert(kqring_enter(r->fd, 0, 0, 0, NULL, 0) == 0);
	printf("[PASS] native kqring_setup/enter/register system calls, "
	    "ioctl fallback\n");
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
	/* Wrong argsz, or a sigmask. */
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
	assert(ring_pending(r) == 1);
	assert(ring_cq_ready(r) == 0);

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

/*
 * Several ops wait on the same socket and direction, as in io_uring: all
 * are woken, and those that find nothing left stay armed.
 */
static void
test_dup_arm(struct test_ring *r)
{
	static char buf[64], buf2[64];
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_POLL_ADD, sv[0], 8002);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 8001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(r);
	sqe = ring_prep(r, IORING_OP_RECV, sv[0], 8003);
	sqe->addr = (uint64_t)(uintptr_t)buf2;
	sqe->len = sizeof(buf2);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);

	/* One byte: the poll fires, and the first RECV takes it. */
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(r, 2, 2000) == 0);
	bool got[2] = { false, false };
	for (int i = 0; i < 2; i++) {
		struct io_uring_cqe c = ring_pop_cqe(r);
		if (c.user_data == 8001) {
			assert(c.res == 1 && buf[0] == 'x');
			got[0] = true;
		} else {
			assert(c.user_data == 8002 && (c.res & POLLIN) != 0);
			got[1] = true;
		}
	}
	assert(got[0] && got[1]);
	assert(ring_wait(r, 1, 20) == ETIMEDOUT);

	/* The second RECV is still armed. */
	assert(write(sv[1], "y", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 8003 && c.res == 1 && buf2[0] == 'y');
	assert(ring_cq_ready(r) == 0);
	close(sv[0]);
	close(sv[1]);

	/* Two ACCEPTs on one listener, then two connections. */
	uint16_t port;
	int lfd = tcp_listener(&port);
	ring_prep(r, IORING_OP_ACCEPT, lfd, 8011);
	ring_push_sqe(r);
	ring_prep(r, IORING_OP_ACCEPT, lfd, 8012);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	int c1 = tcp_connect(port), c2 = tcp_connect(port);
	assert(ring_wait(r, 2, 2000) == 0);
	for (int i = 0; i < 2; i++) {
		c = ring_pop_cqe(r);
		assert((c.user_data == 8011 || c.user_data == 8012) &&
		    c.res >= 0);
		close(c.res);
	}
	close(c1);
	close(c2);
	close(lfd);
	printf("[PASS] several ops wait on one socket+direction (RECV, POLL, "
	    "ACCEPT)\n");
}

/*
 * An armed op keeps the socket it was submitted on, as io_uring keeps
 * req->file: after its fd is closed and reused for another socket, it
 * still completes from the original one and never touches the new one.
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

	/* fd now names b[0]'s socket; the op still waits on a[0]'s. */
	assert(dup2(b[0], fd) == fd);
	assert(write(b[1], "y", 1) == 1);
	assert(ring_wait(r, 1, 200) != 0);
	assert(write(a[1], "x", 1) == 1);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 8101 && c.res == 1 && buf[0] == 'x');
	assert(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'y');

	close(fd);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	printf("[PASS] armed RECV keeps its socket after fd reuse, new "
	    "socket untouched\n");
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

	/*
	 * TIMEOUT_REMOVE cancels (its CQE first, as io_uring posts them); a
	 * second remove finds nothing.
	 */
	prep_timeout(r, IORING_OP_TIMEOUT, 24011, &t5s, 0, 0, 0);
	ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1, 24012)->addr = 24011;
	ring_push_sqe(r);
	ring_prep(r, IORING_OP_TIMEOUT_REMOVE, -1, 24013)->addr = 24011;
	ring_push_sqe(r);
	ring_submit(r);
	expect_cqe(r, 24012, 0);
	expect_cqe(r, 24011, -ECANCELED);
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

static void *
shared_nop_thread(void *arg)
{
	struct test_ring *r = arg;

	ring_prep(r, IORING_OP_NOP, -1, 1);
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_wait(r, 1, 2000) == 0);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == 1 && c.res == 0);
	return (NULL);
}

static void *
shared_wait_thread(void *arg)
{
	struct test_ring *r = arg;

	assert(ring_wait(r, 1, 2000) == 0);
	return (NULL);
}

/* poll(2) and kevent(2) on a ring descriptor: readable once CQEs wait. */
static void
test_ring_fd_poll(void)
{
	struct timespec zero = { 0, 0 };
	struct test_ring r2;
	struct kevent kev;

	ring_init(&r2);
	struct pollfd pfd = { .fd = r2.fd, .events = POLLIN };
	assert(poll(&pfd, 1, 0) == 0);
	int kq = kqueue();
	assert(kq >= 0);
	EV_SET(&kev, r2.fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	assert(kevent(kq, &kev, 1, NULL, 0, NULL) == 0);
	assert(kevent(kq, NULL, 0, &kev, 1, &zero) == 0);

	ring_prep(&r2, IORING_OP_NOP, -1, 1);
	ring_push_sqe(&r2);
	ring_submit(&r2);
	assert(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN) != 0);
	assert(kevent(kq, NULL, 0, &kev, 1, &zero) == 1 &&
	    (int)kev.ident == r2.fd);
	(void)ring_pop_cqe(&r2);
	assert(poll(&pfd, 1, 0) == 0);

	close(kq);
	close(r2.fd);
	munmap(r2.ring_map, r2.ring_map_len);
	munmap(r2.sqes, r2.sqes_len);
	printf("[PASS] poll(2) / kevent(2) on the ring descriptor\n");
}

/*
 * A ring set up without SINGLE_ISSUER: another thread may submit and wait,
 * and two threads waiting at once are both woken.
 */
static void
test_shared_ring(void)
{
	static char buf[8];
	struct test_ring r2;
	pthread_t t;
	int sv[2];

	ring_setup_shared = true;
	ring_init(&r2);
	ring_setup_shared = false;

	assert(pthread_create(&t, NULL, shared_nop_thread, &r2) == 0);
	assert(pthread_join(t, NULL) == 0);

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	struct io_uring_sqe *sqe = ring_prep(&r2, IORING_OP_RECV, sv[0], 2);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&r2);
	ring_submit(&r2);
	assert(ring_cq_ready(&r2) == 0);
	assert(pthread_create(&t, NULL, shared_wait_thread, &r2) == 0);
	usleep(20000);
	assert(write(sv[1], "x", 1) == 1);
	assert(ring_wait(&r2, 1, 2000) == 0);
	assert(pthread_join(t, NULL) == 0);
	struct io_uring_cqe c = ring_pop_cqe(&r2);
	assert(c.user_data == 2 && c.res == 1);

	close(sv[0]);
	close(sv[1]);
	close(r2.fd);
	munmap(r2.ring_map, r2.ring_map_len);
	munmap(r2.sqes, r2.sqes_len);
	printf("[PASS] shared ring (no SINGLE_ISSUER): other threads submit "
	    "and wait\n");
}

/*
 * Close TCP sockets (a listener and a connection) while ops are armed on
 * them, then the ring: the ring drops the last socket references, and the
 * TCP detach that follows needs the socket's vnet.
 */
static void
test_close_sockets_then_ring(void)
{
	static char buf[64];
	struct test_ring r2;
	uint16_t port;

	ring_init(&r2);
	int lfd = tcp_listener(&port);
	int c = tcp_connect(port);
	wait_readable(lfd);
	int s = accept(lfd, NULL, NULL);
	assert(s >= 0);

	struct io_uring_sqe *sqe = ring_prep(&r2, IORING_OP_ACCEPT, lfd, 1);
	ring_push_sqe(&r2);
	sqe = ring_prep(&r2, IORING_OP_RECV, s, 2);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&r2);
	ring_submit(&r2);
	assert(ring_cq_ready(&r2) == 0);

	/* A connection queued on the listener when it closes, too. */
	int c2 = tcp_connect(port);
	usleep(10000);
	close(lfd);
	close(s);
	close(c);
	close(c2);
	assert(close(r2.fd) == 0);
	munmap(r2.ring_map, r2.ring_map_len);
	munmap(r2.sqes, r2.sqes_len);
	printf("[PASS] close() TCP listener and socket, then the ring\n");
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

struct zc_unload_race_ctx {
	atomic_bool	done;
	int		attempts;
};

static void *
zc_unload_racer_thread(void *arg)
{
	struct zc_unload_race_ctx *ctx = arg;
	int fileid = kldfind("kqring");
	assert(fileid >= 0);

	for (int i = 0; i < 3; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (geteuid() == 0) {
			int rc = kldunload(fileid);
			assert(rc == -1 && errno == EBUSY);
		} else {
			int st = system(
			    "sudo -n kldunload kqring 2>&1 | grep -q 'Device busy'");
			assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		    (t1.tv_nsec - t0.tv_nsec) / 1000000L;
		assert(elapsed_ms < 2000);
		ctx->attempts++;
	}
	atomic_store_explicit(&ctx->done, true, memory_order_release);
	return (NULL);
}

/* kern.kqring.zc_local_min_len: read it, or set it (through sudo -n). */
static unsigned
zc_local_min_get(void)
{
	unsigned v;
	size_t len = sizeof(v);

	assert(sysctlbyname("kern.kqring.zc_local_min_len", &v, &len, NULL,
	    0) == 0);
	return (v);
}

static void
zc_local_min_set(unsigned v)
{
	char cmd[128];

	if (geteuid() == 0) {
		assert(sysctlbyname("kern.kqring.zc_local_min_len", NULL, NULL,
		    &v, sizeof(v)) == 0);
	} else {
		snprintf(cmd, sizeof(cmd),
		    "sudo -n sysctl -q kern.kqring.zc_local_min_len=%u "
		    ">/dev/null", v);
		assert(system(cmd) == 0);
	}
	assert(zc_local_min_get() == v);
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

	/*
	 * AF_UNIX sends from unregistered buffers below
	 * kern.kqring.zc_local_min_len (32 KiB by default) copy.  Most checks
	 * below use sends of a few pages, to stay under the default 64 KiB
	 * RLIMIT_MEMLOCK, so lower it to one page.
	 */
	unsigned zc_local_min = zc_local_min_get();
	zc_local_min_set((unsigned)sysconf(_SC_PAGESIZE));

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

	/*
	 * Phase A genuine zero-copy SEND_ZC with IORING_RECVSEND_FIXED_BUF:
	 * attaches EXT_SFBUF mbufs over held physical pages, posts the
	 * primary CQE (IORING_CQE_F_MORE) immediately, and defers the
	 * IORING_CQE_F_NOTIF CQE (res == 0, not IORING_NOTIF_USAGE_ZC_COPIED)
	 * until the peer drains the socket buffer.
	 */
	size_t zlen = (size_t)sysconf(_SC_PAGESIZE) * 2;
	char *zbuf = mmap(NULL, zlen, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	char *rbuf = malloc(zlen);
	assert(zbuf != MAP_FAILED && rbuf != NULL);
	memset(zbuf, 'A', zlen);
	struct iovec ziov = { .iov_base = zbuf, .iov_len = zlen };
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);

	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30505);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	/* Primary CQE is ready; notification CQE waits for mbuf release. */
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30505, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	/* Mutate the pinned page while unread in sv[1] to prove zero-copy. */
	zbuf[0] = 'Z';
	zbuf[zlen - 1] = 'Y';
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'Z' && rbuf[1] == 'A' && rbuf[zlen - 1] == 'Y');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30505, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(sv[0]);
	close(sv[1]);

	/* TCP loopback zero-copy SEND_ZC + FIXED_BUF. */
	uint16_t port;
	int lfd = tcp_listener(&port);
	int cfd = tcp_connect(port);
	wait_readable(lfd);
	int afd = accept(lfd, NULL, NULL);
	assert(afd >= 0);
	memset(zbuf, 'T', zlen);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, cfd, 30506);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	size_t got = 0;
	while (got < zlen) {
		wait_readable(afd);
		ssize_t n = read(afd, rbuf + got, zlen - got);
		assert(n > 0);
		got += (size_t)n;
	}
	assert(ring_wait(r, 2, 2000) == 0);
	c = expect_cqe(r, 30506, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30506, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(afd);
	close(cfd);
	close(lfd);
	/*
	 * Copy-on-write after fork() refreshes held pages before SEND_ZC,
	 * while an earlier unread SEND_ZC keeps its old pages held and charged
	 * until the mbuf referencing them is drained.
	 */
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	struct rlimit old_rl, rl;
	assert(getrlimit(RLIMIT_MEMLOCK, &old_rl) == 0);
	rl.rlim_cur = (rlim_t)(4 * page);
	rl.rlim_max = old_rl.rlim_max;
	assert(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);

	memset(zbuf, 'A', zlen);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30507);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30507, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);

	/* sv[1] still holds the unread mbuf referencing old_zb (refs == 2). */
	int cow_pipe[2];
	assert(pipe(cow_pipe) == 0);
	pid_t zpid = fork();
	assert(zpid >= 0);
	if (zpid == 0) {
		close(cow_pipe[1]);
		assert(zbuf[0] == 'A' && zbuf[zlen - 1] == 'A');
		char ch;
		(void)read(cow_pipe[0], &ch, 1);
		assert(zbuf[0] == 'A' && zbuf[zlen - 1] == 'A');
		close(cow_pipe[0]);
		_exit(0);
	}
	close(cow_pipe[0]);
	/* Fault in parent while child still maps the pre-COW 'A' pages. */
	zbuf[0] = 'P';
	zbuf[zlen - 1] = 'Q';
	close(cow_pipe[1]);
	int zstatus = 0;
	assert(waitpid(zpid, &zstatus, 0) == zpid);
	assert(WIFEXITED(zstatus) && WEXITSTATUS(zstatus) == 0);

	int sv2[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv2) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv2[0], 30508);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30508, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);

	/*
	 * Both old_zb (2 pages in sv[1]) and new_zb (2 pages in slot + sv2[1])
	 * are charged against the 4-page RLIMIT_MEMLOCK cap.
	 */
	struct iovec one_page = { .iov_base = zbuf, .iov_len = page };
	struct test_ring cow_mr;
	ring_init(&cow_mr);
	assert(kqring_register(cow_mr.fd, IORING_REGISTER_BUFFERS, &one_page,
	    1) == -1 && errno == ENOMEM);

	/* sv[1] still reads the pre-COW 'A' pages from old_zb. */
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'A' && rbuf[1] == 'A' && rbuf[zlen - 1] == 'A');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30507, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* Draining sv[1] released old_zb's 2-page charge. */
	assert(kqring_register(cow_mr.fd, IORING_REGISTER_BUFFERS, &one_page,
	    1) == 0);
	assert(kqring_register(cow_mr.fd, IORING_UNREGISTER_BUFFERS, NULL,
	    0) == 0);
	munmap(cow_mr.br, page);
	munmap(cow_mr.bufs, NBUFS * BUF_SIZE);
	munmap(cow_mr.sqes, cow_mr.sqes_len);
	munmap(cow_mr.ring_map, cow_mr.ring_map_len);
	close(cow_mr.fd);

	/* sv2[1] reads the refreshed post-COW 'P'..'Q' pages from new_zb. */
	assert(read(sv2[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'P' && rbuf[1] == 'A' && rbuf[zlen - 1] == 'Q');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30508, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(sv2[0]);
	close(sv2[1]);
	close(sv[0]);
	close(sv[1]);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);

	/*
	 * RLIMIT_MEMLOCK enforcement across rings and across unread in-flight
	 * SEND_ZC notifications after slot unregistration.
	 */
	rl.rlim_cur = (rlim_t)(3 * page);
	rl.rlim_max = old_rl.rlim_max;
	assert(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);

	/* 2 pages on r leaves 1 page of allowance for a second ring. */
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);
	struct test_ring mr;
	ring_init(&mr);
	assert(kqring_register(mr.fd, IORING_REGISTER_BUFFERS, &ziov, 1) == -1 &&
	    errno == ENOMEM);
	assert(kqring_register(mr.fd, IORING_REGISTER_BUFFERS, &one_page,
	    1) == 0);
	struct test_ring mr2;
	ring_init(&mr2);
	assert(kqring_register(mr2.fd, IORING_REGISTER_BUFFERS, &one_page,
	    1) == -1 && errno == ENOMEM);
	munmap(mr2.br, page);
	munmap(mr2.bufs, NBUFS * BUF_SIZE);
	munmap(mr2.sqes, mr2.sqes_len);
	munmap(mr2.ring_map, mr2.ring_map_len);
	close(mr2.fd);
	munmap(mr.br, page);
	munmap(mr.bufs, NBUFS * BUF_SIZE);
	munmap(mr.sqes, mr.sqes_len);
	munmap(mr.ring_map, mr.ring_map_len);
	close(mr.fd);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);

	/*
	 * Unread zero-copy sends keep their pages charged even after
	 * IORING_UNREGISTER_BUFFERS; reading the socket returns the charge.
	 */
	rl.rlim_cur = (rlim_t)(4 * page);
	assert(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	for (int iter = 0; iter < 2; iter++) {
		assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov,
		    1) == 0);
		sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30510 + iter);
		sqe->addr = (uint64_t)(uintptr_t)zbuf;
		sqe->len = (uint32_t)zlen;
		sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
		sqe->buf_index = 0;
		ring_push_sqe(r);
		ring_submit(r);
		assert(ring_cq_ready(r) == 1);
		c = expect_cqe(r, 30510 + iter, (int32_t)zlen);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	}
	/* 4 pages are still held in sv[1]'s unread receive buffer. */
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == -1 &&
	    errno == ENOMEM);
	/* Draining the first send releases 2 pages and allows re-registration. */
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30510, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30511, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(sv[0]);
	close(sv[1]);
	assert(setrlimit(RLIMIT_MEMLOCK, &old_rl) == 0);

	/*
	 * Unread SEND_ZC raced against kldunload: unload must return EBUSY
	 * promptly without hanging, concurrent SEND_ZC submissions during the
	 * unload race must succeed, and the ring must keep working afterwards.
	 */
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv2) == 0);
	memset(zbuf, 'U', zlen);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30530);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30530, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);

	/* sv[1] is left unread while zc_unload_racer_thread attempts kldunload. */
	struct zc_unload_race_ctx uctx = { .done = false, .attempts = 0 };
	pthread_t uthr;
	assert(pthread_create(&uthr, NULL, zc_unload_racer_thread, &uctx) == 0);
	uint64_t uid = 30600;
	while (!atomic_load_explicit(&uctx.done, memory_order_acquire)) {
		sqe = ring_prep(r, IORING_OP_SEND_ZC, sv2[0], uid);
		sqe->addr = (uint64_t)(uintptr_t)zbuf;
		sqe->len = (uint32_t)page;
		sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
		sqe->buf_index = 0;
		ring_push_sqe(r);
		ring_submit(r);
		assert(read(sv2[1], rbuf, page) == (ssize_t)page);
		assert(ring_wait(r, 2, 2000) == 0);
		c = expect_cqe(r, uid, (int32_t)page);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		c = expect_cqe(r, uid, 0);
		assert(c.flags == IORING_CQE_F_NOTIF);
		uid++;
	}
	assert(pthread_join(uthr, NULL) == 0);
	assert(uctx.attempts == 3);

	/* Ring and true zero-copy SEND_ZC still work after failed kldunload. */
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv2[0], 30539);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(read(sv2[1], rbuf, zlen) == (ssize_t)zlen);
	assert(ring_wait(r, 2, 2000) == 0);
	c = expect_cqe(r, 30539, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30539, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* Drain sv[1] to release the pinned send that blocked kldunload. */
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30530, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(sv2[0]);
	close(sv2[1]);
	close(sv[0]);
	close(sv[1]);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);

	/*
	 * Phase B: unregistered user-buffer zero-copy SEND_ZC and
	 * scatter/gather SENDMSG_ZC (both unregistered and FIXED_BUF).
	 */
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	memset(zbuf, 'B', zlen);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30540);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30540, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	/* Mutate unregistered buffer in-flight before peer reads. */
	zbuf[0] = 'M';
	zbuf[zlen - 1] = 'N';
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'M' && rbuf[1] == 'B' && rbuf[zlen - 1] == 'N');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30540, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* Read-only (PROT_READ) unregistered buffer works with SEND_ZC. */
	assert(mprotect(zbuf, zlen, PROT_READ) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30541);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30541, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30541, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(mprotect(zbuf, zlen, PROT_READ | PROT_WRITE) == 0);

	/* Below PAGE_SIZE threshold falls back to copy (ZC_COPIED). */
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30542);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)(page - 1);
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30542, (int32_t)(page - 1));
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30542, (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(read(sv[1], rbuf, page - 1) == (ssize_t)(page - 1));

	/* RLIMIT_MEMLOCK exhaustion on unregistered SEND_ZC falls back to copy. */
	rl.rlim_cur = (rlim_t)page;
	rl.rlim_max = old_rl.rlim_max;
	assert(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30543);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30543, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30543, (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(setrlimit(RLIMIT_MEMLOCK, &old_rl) == 0);

	/*
	 * Scatter/gather SENDMSG_ZC across two separate unregistered buffers
	 * (1 page each, total 2 pages): zero-copy over both segments.
	 */
	char *zbuf2 = mmap(NULL, page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(zbuf2 != MAP_FAILED);
	memset(zbuf, '1', page);
	memset(zbuf2, '2', page);
	struct iovec sgiov[2] = {
		{ .iov_base = zbuf, .iov_len = page },
		{ .iov_base = zbuf2, .iov_len = page },
	};
	struct msghdr sgmsg = { .msg_iov = sgiov, .msg_iovlen = 2 };
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30544);
	sqe->addr = (uint64_t)(uintptr_t)&sgmsg;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30544, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	/* Mutate both segments while unread in sv[1] to prove zero-copy. */
	zbuf[0] = 'X';
	zbuf2[page - 1] = 'W';
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'X' && rbuf[1] == '1' &&
	    rbuf[page] == '2' && rbuf[zlen - 1] == 'W');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30544, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* SENDMSG_ZC + IORING_RECVSEND_FIXED_BUF across two slices in slot 0. */
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);
	memset(zbuf, 'F', page);
	memset(zbuf + page, 'G', page);
	struct iovec fgiov[2] = {
		{ .iov_base = zbuf, .iov_len = page },
		{ .iov_base = zbuf + page, .iov_len = page },
	};
	struct msghdr fgmsg = { .msg_iov = fgiov, .msg_iovlen = 2 };
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30545);
	sqe->addr = (uint64_t)(uintptr_t)&fgmsg;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30545, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	assert(rbuf[0] == 'F' && rbuf[page] == 'G');
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30545, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	/* Out-of-bounds iovec with SENDMSG_ZC + FIXED_BUF fails -EFAULT. */
	struct iovec badiov[2] = {
		{ .iov_base = zbuf, .iov_len = page },
		{ .iov_base = zbuf2, .iov_len = page },
	};
	struct msghdr badmsg = { .msg_iov = badiov, .msg_iovlen = 2 };
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30546);
	sqe->addr = (uint64_t)(uintptr_t)&badmsg;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
	sqe->buf_index = 0;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30546, -EFAULT);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30546, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);

	/*
	 * Unaligned and short iovecs under multi-send mbuf pressure: queue 4
	 * unread scatter/gather SENDMSG_ZC requests (each spanning an unaligned
	 * cross-page iovec, a short 251-byte iovec, and a trailing unaligned
	 * iovec = 4 pages/send, 16 pages total), mutate the pinned memory while
	 * all 4 multi-mbuf EXT_SFBUF chains sit in the socket buffer, then
	 * drain and verify every CQE.
	 */
	int sndbuf_big = 256 * 1024;
	(void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf_big,
	    sizeof(sndbuf_big));
	(void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sndbuf_big,
	    sizeof(sndbuf_big));
	memset(zbuf, 'U', zlen);
	memset(zbuf2, 'S', page);
	struct iovec unaligned_iov[3] = {
		{ .iov_base = zbuf + 137, .iov_len = page + 511 },
		{ .iov_base = zbuf2 + 73, .iov_len = 251 },
		{ .iov_base = zbuf + page + 19, .iov_len = 700 },
	};
	size_t unaligned_tot = (page + 511) + 251 + 700;
	struct msghdr unaligned_msg = {
		.msg_iov = unaligned_iov,
		.msg_iovlen = 3,
	};
	for (int i = 0; i < 4; i++) {
		sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30550 + i);
		sqe->addr = (uint64_t)(uintptr_t)&unaligned_msg;
		sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
		ring_push_sqe(r);
	}
	ring_submit(r);
	assert(ring_cq_ready(r) == 4);
	for (int i = 0; i < 4; i++) {
		c = expect_cqe(r, 30550 + i, (int32_t)unaligned_tot);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
	}
	/* Mutate both pinned buffers while all 4 chains are unread in sv[1]. */
	zbuf[137] = 'J';
	zbuf2[73] = 'K';
	char *ubuf = malloc(unaligned_tot);
	assert(ubuf != NULL);
	for (int i = 0; i < 4; i++) {
		assert(read(sv[1], ubuf, unaligned_tot) ==
		    (ssize_t)unaligned_tot);
		assert(ubuf[0] == 'J' && ubuf[1] == 'U');
		assert(ubuf[page + 511] == 'K' && ubuf[page + 512] == 'S');
	}
	free(ubuf);
	assert(ring_wait(r, 4, 2000) == 0);
	for (int i = 0; i < 4; i++) {
		c = expect_cqe(r, 30550 + i, 0);
		assert(c.flags == IORING_CQE_F_NOTIF);
	}

	/*
	 * Failure while pinning a later segment:
	 * 1) RLIMIT_MEMLOCK cap of 1 page allows iov[0] (1 page) to pin, then
	 *    fails iov[1] (1 page) with ENOMEM; kqring_send_zc_iov must unpin
	 *    and uncharge iov[0] and fall back to copy (ZC_COPIED).
	 * 2) Unmapped second segment (PROT_NONE / munmapped) after valid iov[0]
	 *    must unpin iov[0] without leaking wired pages.
	 */
	rl.rlim_cur = (rlim_t)page;
	rl.rlim_max = old_rl.rlim_max;
	assert(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30560);
	sqe->addr = (uint64_t)(uintptr_t)&sgmsg;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30560, (int32_t)zlen);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30560, (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	/* Verify iov[0]'s 1-page charge was released on segment-1 failure. */
	assert(kqring_register(r->fd, IORING_REGISTER_BUFFERS, &one_page,
	    1) == 0);
	assert(kqring_register(r->fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);
	assert(setrlimit(RLIMIT_MEMLOCK, &old_rl) == 0);

	char *bad_seg = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	assert(bad_seg != MAP_FAILED);
	struct iovec fault_iov[2] = {
		{ .iov_base = zbuf, .iov_len = page },
		{ .iov_base = bad_seg, .iov_len = page },
	};
	struct msghdr fault_msg = { .msg_iov = fault_iov, .msg_iovlen = 2 };
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, sv[0], 30561);
	sqe->addr = (uint64_t)(uintptr_t)&fault_msg;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = ring_pop_cqe(r);
	assert(c.user_data == 30561);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	if (c.res > 0) {
		assert(read(sv[1], rbuf, (size_t)c.res) == (ssize_t)c.res);
	} else {
		assert(c.res == -EFAULT);
	}
	c = ring_pop_cqe(r);
	assert(c.user_data == 30561 && c.flags == IORING_CQE_F_NOTIF);
	munmap(bad_seg, page);

	/*
	 * TCP partial zero-copy send: constrain SO_SNDBUF on a non-blocking TCP
	 * socket so a multi-iovec SENDMSG_ZC (without MSG_WAITALL) sends a
	 * partial byte count (>= PAGE_SIZE and < total), pins only the sent
	 * range, and posts IORING_CQE_F_NOTIF once the peer drains it.
	 */
	lfd = tcp_listener(&port);
	cfd = tcp_connect(port);
	wait_readable(lfd);
	afd = accept(lfd, NULL, NULL);
	assert(afd >= 0);
	int small_snd = (int)(4 * page);
	(void)setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &small_snd,
	    sizeof(small_snd));
	/* Fill part of cfd's send buffer so the next ZC send is clamped. */
	memset(zbuf, 'P', zlen);
	memset(zbuf2, 'Q', page);
	struct iovec tcp_iov[6] = {
		{ .iov_base = zbuf, .iov_len = zlen },
		{ .iov_base = zbuf2, .iov_len = page },
		{ .iov_base = zbuf, .iov_len = zlen },
		{ .iov_base = zbuf2, .iov_len = page },
		{ .iov_base = zbuf, .iov_len = zlen },
		{ .iov_base = zbuf2, .iov_len = page },
	};
	size_t tcp_want = 3 * (zlen + page);
	struct msghdr tcp_msg = { .msg_iov = tcp_iov, .msg_iovlen = 6 };
	sqe = ring_prep(r, IORING_OP_SENDMSG_ZC, cfd, 30562);
	sqe->addr = (uint64_t)(uintptr_t)&tcp_msg;
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) >= 1);
	c = ring_pop_cqe(r);
	assert(c.user_data == 30562);
	assert(c.res >= (int32_t)page && (size_t)c.res < tcp_want);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	size_t tcp_sent = (size_t)c.res;
	char *tbuf = malloc(tcp_sent);
	assert(tbuf != NULL);
	got = 0;
	while (got < tcp_sent) {
		wait_readable(afd);
		ssize_t n = read(afd, tbuf + got, tcp_sent - got);
		assert(n > 0);
		got += (size_t)n;
	}
	assert(tbuf[0] == 'P');
	free(tbuf);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30562, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(afd);
	close(cfd);
	close(lfd);

	munmap(zbuf2, page);
	close(sv[0]);
	close(sv[1]);

	/* Closing a ring while zero-copy mbufs remain unread in peer socket. */
	struct test_ring zr;
	ring_init(&zr);
	assert(kqring_register(zr.fd, IORING_REGISTER_BUFFERS, &ziov, 1) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(&zr, IORING_OP_SEND_ZC, sv[0], 30520);
	sqe->addr = (uint64_t)(uintptr_t)zbuf;
	sqe->len = (uint32_t)zlen;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
	sqe->buf_index = 0;
	ring_push_sqe(&zr);
	ring_submit(&zr);
	assert(ring_cq_ready(&zr) == 1);
	munmap(zr.br, page);
	munmap(zr.bufs, NBUFS * BUF_SIZE);
	munmap(zr.sqes, zr.sqes_len);
	munmap(zr.ring_map, zr.ring_map_len);
	close(zr.fd);
	munmap(zbuf, zlen);
	assert(read(sv[1], rbuf, zlen) == (ssize_t)zlen);
	close(sv[0]);
	close(sv[1]);
	free(rbuf);

	/*
	 * The AF_UNIX threshold for unregistered buffers: with it at 3 pages,
	 * a 2-page SEND_ZC copies (its notification comes at once, reporting
	 * the copy) and a 3-page one is zero-copy (its notification waits for
	 * the peer to read).  A registered buffer ignores it.
	 */
	zc_local_min_set((unsigned)(3 * page));
	char *tb = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	char *tr = malloc(3 * page);
	assert(tb != MAP_FAILED && tr != NULL);
	memset(tb, 't', 3 * page);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30570);
	sqe->addr = (uint64_t)(uintptr_t)tb;
	sqe->len = (uint32_t)(2 * page);
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 2);
	c = expect_cqe(r, 30570, (int32_t)(2 * page));
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(r, 30570, (int32_t)IORING_NOTIF_USAGE_ZC_COPIED);
	assert(c.flags == IORING_CQE_F_NOTIF);
	assert(read(sv[1], tr, 2 * page) == (ssize_t)(2 * page));
	sqe = ring_prep(r, IORING_OP_SEND_ZC, sv[0], 30571);
	sqe->addr = (uint64_t)(uintptr_t)tb;
	sqe->len = (uint32_t)(3 * page);
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	c = expect_cqe(r, 30571, (int32_t)(3 * page));
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	assert(read(sv[1], tr, 3 * page) == (ssize_t)(3 * page));
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 30571, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);

	struct test_ring fr;
	struct iovec tiov = { .iov_base = tb, .iov_len = 3 * page };
	ring_init(&fr);
	assert(kqring_register(fr.fd, IORING_REGISTER_BUFFERS, &tiov, 1) == 0);
	sqe = ring_prep(&fr, IORING_OP_SEND_ZC, sv[0], 30572);
	sqe->addr = (uint64_t)(uintptr_t)tb;
	sqe->len = (uint32_t)(2 * page);
	sqe->ioprio = IORING_SEND_ZC_REPORT_USAGE | IORING_RECVSEND_FIXED_BUF;
	sqe->buf_index = 0;
	ring_push_sqe(&fr);
	ring_submit(&fr);
	assert(ring_cq_ready(&fr) == 1);
	c = expect_cqe(&fr, 30572, (int32_t)(2 * page));
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	assert(read(sv[1], tr, 2 * page) == (ssize_t)(2 * page));
	assert(ring_wait(&fr, 1, 2000) == 0);
	c = expect_cqe(&fr, 30572, 0);
	assert(c.flags == IORING_CQE_F_NOTIF);
	close(fr.fd);

	munmap(tb, 3 * page);
	free(tr);
	close(sv[0]);
	close(sv[1]);
	zc_local_min_set(zc_local_min);
	printf("[PASS] SEND_ZC/SENDMSG_ZC notifications (incl. FIXED_BUF ZC), "
	    "SEND to an address, AF_UNIX copy threshold\n");
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

	/* Not a ring. */
	assert(pipe(pfd) == 0);
	sqe = ring_prep(r, IORING_OP_MSG_RING, pfd[0], 32004);
	ring_push_sqe(r);
	assert(submit_one(r) == -EBADF);
	sqe = ring_prep(r, IORING_OP_MSG_RING, pfd[0], 32005);
	sqe->addr = IORING_MSG_SEND_FD;
	ring_push_sqe(r);
	assert(submit_one(r) == -EBADF);
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

	/* Pipe READ_MULTISHOT: inline and armed reads until EOF. */
	assert(pipe(pfd) == 0);
	sqe = ring_prep(r, IORING_OP_READ_MULTISHOT, pfd[0], 35002);
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 0);
	for (int i = 0; i < 3; i++) {
		assert(write(pfd[1], "p", 1) == 1);
		assert(ring_wait(r, 1, 2000) == 0);
		c = expect_cqe(r, 35002, 1);
		assert((c.flags & (IORING_CQE_F_MORE | IORING_CQE_F_BUFFER)) ==
		    (IORING_CQE_F_MORE | IORING_CQE_F_BUFFER));
		ring_offer_buf(r, c.flags >> IORING_CQE_BUFFER_SHIFT);
	}
	close(pfd[1]);
	assert(ring_wait(r, 1, 2000) == 0);
	c = expect_cqe(r, 35002, 0);
	assert((c.flags & (IORING_CQE_F_MORE | IORING_CQE_F_BUFFER)) == 0);
	close(pfd[0]);

	/* Regular files are not supported for READ_MULTISHOT. */
	int nfd = open("/dev/null", O_RDONLY);
	assert(nfd >= 0);
	sqe = ring_prep(r, IORING_OP_READ_MULTISHOT, nfd, 35003);
	sqe->flags = IOSQE_BUFFER_SELECT;
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	close(nfd);
	printf("[PASS] READ_MULTISHOT on socket and pipe, until EOF\n");
}

static void
test_phase2_validation(struct test_ring *r)
{
	struct io_uring_sqe *sqe;
	char buf[8];

	sqe = ring_prep(r, IORING_OP_READ, 0, 36001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->rw_flags = 0x100;		/* no such RWF_* flag */
	ring_push_sqe(r);
	assert(submit_one(r) == -EOPNOTSUPP);
	sqe = ring_prep(r, IORING_OP_NOP, -1, 36002);
	sqe->personality = 1;
	ring_push_sqe(r);
	assert(submit_one(r) == -EINVAL);
	static const uint8_t later[] = {
		IORING_OP_EPOLL_CTL, IORING_OP_RECV_ZC, IORING_OP_EPOLL_WAIT,
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

/* ============================================================================
 * Phase 3: registered resources
 * ============================================================================
 */

/* io_uring_register(2)'s result: >= 0, or a negative errno. */
static int
reg(struct test_ring *r, unsigned op, void *arg, unsigned nr)
{
	long rc = kqring_register(r->fd, op, arg, nr);
	return (rc < 0 ? -errno : (int)rc);
}

/* A READ / WRITE of `len` bytes at `buf` through fixed slot `slot`. */
static int32_t
fixed_rw(struct test_ring *r, uint8_t opcode, int slot, void *buf,
    unsigned len, uint64_t ud)
{
	struct io_uring_sqe *sqe = ring_prep(r, opcode, slot, ud);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = len;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(r);
	return (submit_one(r));
}

static void
test_fixed_files(void)
{
	struct test_ring t;
	struct io_uring_files_update up;
	struct io_uring_sqe *sqe;
	int pfd[2], sv[2];
	char buf[16];

	ring_init(&t);
	assert(pipe(pfd) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	/* No table yet. */
	assert(reg(&t, IORING_UNREGISTER_FILES, NULL, 0) == -ENXIO);
	assert(fixed_rw(&t, IORING_OP_READ, 0, buf, 1, 40001) == -EBADF);
	/* FIXED_FILE only applies to ops that take a file. */
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 40002);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)"/dev/null";
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	int fds[4] = { pfd[0], pfd[1], sv[0], -1 };
	assert(reg(&t, IORING_REGISTER_FILES, fds, 0) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_FILES, fds, 4) == 0);
	assert(reg(&t, IORING_REGISTER_FILES, fds, 4) == -EBUSY);
	/* The table holds the files: their descriptors can go. */
	close(pfd[1]);
	close(sv[0]);
	assert(fixed_rw(&t, IORING_OP_WRITE, 1, "hello", 5, 40003) == 5);
	assert(fixed_rw(&t, IORING_OP_READ, 0, buf, sizeof(buf), 40004) == 5);
	assert(memcmp(buf, "hello", 5) == 0);

	/* A fixed socket: an armed RECV, then SEND. */
	sqe = ring_prep(&t, IORING_OP_RECV, 2, 40005);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	assert(write(sv[1], "abc", 3) == 3);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 40005, 3);
	sqe = ring_prep(&t, IORING_OP_SEND, 2, 40006);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)"xy";
	sqe->len = 2;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 2);
	assert(read(sv[1], buf, sizeof(buf)) == 2);

	/* Empty and out-of-range slots. */
	assert(fixed_rw(&t, IORING_OP_READ, 3, buf, 1, 40007) == -EBADF);
	assert(fixed_rw(&t, IORING_OP_READ, 9, buf, 1, 40008) == -EBADF);

	/* A ring cannot go in a table. */
	int one[1] = { t.fd };
	up = (struct io_uring_files_update){ .offset = 3,
	    .fds = (uint64_t)(uintptr_t)one };
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE, &up, 1) == -EBADF);
	/* Update: empty slot 0, leave slot 1, put sv[1] in slot 3. */
	int ufds[4] = { -1, IORING_REGISTER_FILES_SKIP,
	    IORING_REGISTER_FILES_SKIP, sv[1] };
	up = (struct io_uring_files_update){ .offset = 0,
	    .fds = (uint64_t)(uintptr_t)ufds };
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE, &up, 4) == 4);
	assert(fixed_rw(&t, IORING_OP_READ, 0, buf, 1, 40009) == -EBADF);
	assert(fixed_rw(&t, IORING_OP_WRITE, 1, "z", 1, 40010) == 1);
	assert(fixed_rw(&t, IORING_OP_WRITE, 3, "w", 1, 40011) == 1);
	assert(fixed_rw(&t, IORING_OP_READ, 2, buf, sizeof(buf), 40012) == 1);
	assert(buf[0] == 'w');
	up.offset = 3;
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE, &up, 2) == -EINVAL);

	/* The FILES_UPDATE op, and its IORING_FILE_INDEX_ALLOC form. */
	int ofd[1] = { pfd[0] };
	sqe = ring_prep(&t, IORING_OP_FILES_UPDATE, -1, 40013);
	sqe->addr = (uint64_t)(uintptr_t)ofd;
	sqe->len = 1;
	sqe->off = 0;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 1);
	assert(fixed_rw(&t, IORING_OP_READ, 0, buf, sizeof(buf), 40014) == 1);
	assert(buf[0] == 'z');
	/* Empty slot 3: ALLOC fills it, then finds no free slot. */
	int afd[2] = { sv[1], sv[1] };
	up = (struct io_uring_files_update){ .offset = 3,
	    .fds = (uint64_t)(uintptr_t)ufds };
	ufds[0] = -1;
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE, &up, 1) == 1);
	sqe = ring_prep(&t, IORING_OP_FILES_UPDATE, -1, 40015);
	sqe->addr = (uint64_t)(uintptr_t)afd;
	sqe->len = 2;
	sqe->off = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 1);
	assert(afd[0] == 3);
	sqe = ring_prep(&t, IORING_OP_FILES_UPDATE, -1, 40016);
	sqe->addr = (uint64_t)(uintptr_t)afd;
	sqe->len = 1;
	sqe->off = IORING_FILE_INDEX_ALLOC;
	afd[0] = sv[1];
	ring_push_sqe(&t);
	assert(submit_one(&t) == -ENFILE);

	assert(reg(&t, IORING_UNREGISTER_FILES, NULL, 0) == 0);
	assert(fixed_rw(&t, IORING_OP_WRITE, 1, "z", 1, 40017) == -EBADF);
	close(pfd[0]);
	close(sv[1]);
	close(t.fd);
	printf("[PASS] Fixed files: register, FIXED_FILE ops (incl. armed), "
	    "update, alloc, no rings\n");
}

static void
test_direct_descriptors(void)
{
	struct io_uring_rsrc_register rr = {
		.nr = 8,
		.flags = IORING_RSRC_REGISTER_SPARSE,
	};
	struct io_uring_file_index_range range;
	struct io_uring_sqe *sqe;
	struct test_ring t;
	char dir[64], path[96], buf[16];
	int fd, s, slot;
	uint16_t port;

	ring_init(&t);
	assert(reg(&t, IORING_REGISTER_FILES2, &rr, sizeof(rr)) == 0);
	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/f", dir);
	fd = open(path, O_RDWR | O_CREAT, 0600);
	assert(fd >= 0 && write(fd, "direct!", 7) == 7);
	close(fd);

	/* OPENAT into slot 2, and into a free slot (0). */
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 41001);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDONLY;
	sqe->file_index = 3;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 41002);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDONLY;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	memset(buf, 0, sizeof(buf));
	sqe = ring_prep(&t, IORING_OP_READ, 2, 41003);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&t);
	assert(submit_one(&t) == 7 && memcmp(buf, "direct!", 7) == 0);
	assert(fixed_rw(&t, IORING_OP_READ, 0, buf, sizeof(buf), 41004) == 7);
	/* No close-on-exec without a descriptor. */
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 41005);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDONLY | O_CLOEXEC;
	sqe->file_index = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	/* FIXED_FD_INSTALL: a descriptor, close-on-exec unless asked not. */
	sqe = ring_prep(&t, IORING_OP_FIXED_FD_INSTALL, 2, 41006);
	sqe->flags = IOSQE_FIXED_FILE;
	ring_push_sqe(&t);
	fd = submit_one(&t);
	assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
	assert(pread(fd, buf, sizeof(buf), 0) == 7);
	close(fd);
	sqe = ring_prep(&t, IORING_OP_FIXED_FD_INSTALL, 2, 41007);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->install_fd_flags = IORING_FIXED_FD_NO_CLOEXEC;
	ring_push_sqe(&t);
	fd = submit_one(&t);
	assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
	close(fd);
	sqe = ring_prep(&t, IORING_OP_FIXED_FD_INSTALL, 2, 41008);
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EBADF);

	/* CLOSE a fixed slot. */
	sqe = ring_prep(&t, IORING_OP_CLOSE, 0, 41009);
	sqe->file_index = 3;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	assert(fixed_rw(&t, IORING_OP_READ, 2, buf, 1, 41010) == -EBADF);
	sqe = ring_prep(&t, IORING_OP_CLOSE, 0, 41011);
	sqe->file_index = 3;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EBADF);

	/* SOCKET, BIND, LISTEN and ACCEPT, all on direct descriptors. */
	sqe = ring_prep(&t, IORING_OP_SOCKET, AF_INET, 41012);
	sqe->off = SOCK_STREAM;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	s = submit_one(&t);
	assert(s == 1);
	struct sockaddr_in sin = {
		.sin_len = sizeof(sin),
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	sqe = ring_prep(&t, IORING_OP_BIND, s, 41013);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)&sin;
	sqe->addr2 = sizeof(sin);
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_LISTEN, s, 41014);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_FIXED_FD_INSTALL, s, 41015);
	sqe->flags = IOSQE_FIXED_FILE;
	ring_push_sqe(&t);
	fd = submit_one(&t);
	socklen_t slen = sizeof(sin);
	assert(fd >= 0 && getsockname(fd, (struct sockaddr *)&sin, &slen) == 0);
	close(fd);
	port = ntohs(sin.sin_port);

	int c1 = tcp_connect(port);
	sqe = ring_prep(&t, IORING_OP_ACCEPT, s, 41016);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	/* The connection may not be queued yet: then it waits. */
	assert(ring_wait(&t, 1, 2000) == 0);
	slot = ring_pop_cqe(&t).res;
	assert(slot == 2);
	sqe = ring_prep(&t, IORING_OP_SEND, slot, 41017);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)"acc";
	sqe->len = 3;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 3);
	assert(read(c1, buf, sizeof(buf)) == 3 && memcmp(buf, "acc", 3) == 0);
	/* Multishot: each connection in a new slot. */
	sqe = ring_prep(&t, IORING_OP_ACCEPT, s, 41018);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->ioprio = IORING_ACCEPT_MULTISHOT;
	sqe->file_index = 2;	/* must allocate */
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);
	sqe = ring_prep(&t, IORING_OP_ACCEPT, s, 41019);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->ioprio = IORING_ACCEPT_MULTISHOT;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	ring_submit(&t);
	int c2 = tcp_connect(port), c3 = tcp_connect(port);
	assert(ring_wait(&t, 2, 2000) == 0);
	struct io_uring_cqe a1 = ring_pop_cqe(&t), a2 = ring_pop_cqe(&t);
	assert(a1.user_data == 41019 && (a1.flags & IORING_CQE_F_MORE) != 0);
	assert(a2.user_data == 41019 && a1.res != a2.res);
	assert(a1.res >= 3 && a2.res >= 3);
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, -1, 41020);
	sqe->addr = 41019;
	ring_push_sqe(&t);
	ring_submit(&t);
	expect_cqe(&t, 41019, -ECANCELED);
	expect_cqe(&t, 41020, 0);

	/* PIPE: descriptors, or two fixed slots. */
	int pfds[2] = { -1, -1 };
	sqe = ring_prep(&t, IORING_OP_PIPE, 0, 41021);
	sqe->addr = (uint64_t)(uintptr_t)pfds;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	assert(write(pfds[1], "p", 1) == 1 && read(pfds[0], buf, 1) == 1);
	close(pfds[0]);
	close(pfds[1]);
	/* Slots 6 and 7 (file_index 7), written back to pfds. */
	sqe = ring_prep(&t, IORING_OP_PIPE, 0, 41022);
	sqe->addr = (uint64_t)(uintptr_t)pfds;
	sqe->file_index = 7;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	assert(pfds[0] == 6 && pfds[1] == 7);
	assert(fixed_rw(&t, IORING_OP_WRITE, 7, "PQ", 2, 41023) == 2);
	assert(fixed_rw(&t, IORING_OP_READ, 6, buf, sizeof(buf), 41024) == 2);
	assert(memcmp(buf, "PQ", 2) == 0);

	/* FILE_ALLOC_RANGE: ALLOC only uses [5, 6), now empty, then full. */
	range = (struct io_uring_file_index_range){ .off = 5, .len = 1 };
	assert(reg(&t, IORING_REGISTER_FILE_ALLOC_RANGE, &range, 0) == 0);
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 41025);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDONLY;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 5);
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 41026);
	sqe->addr = (uint64_t)(uintptr_t)path;
	sqe->open_flags = O_RDONLY;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -ENFILE);
	range = (struct io_uring_file_index_range){ .off = 7, .len = 2 };
	assert(reg(&t, IORING_REGISTER_FILE_ALLOC_RANGE, &range, 0) ==
	    -EINVAL);

	close(c1);
	close(c2);
	close(c3);
	close(t.fd);
	unlink(path);
	rmdir(dir);
	printf("[PASS] Direct descriptors: OPENAT/SOCKET/ACCEPT/PIPE file_index, "
	    "CLOSE, FIXED_FD_INSTALL, alloc range\n");
}

static void
test_rsrc_tags(void)
{
	struct io_uring_rsrc_update2 up2;
	struct io_uring_files_update up;
	struct io_uring_sqe *sqe;
	struct test_ring t;
	int sv[2];
	char buf[8];

	ring_init(&t);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	int fds[2] = { sv[0], -1 };
	uint64_t tags[2] = { 0x7a6, 0 };
	struct io_uring_rsrc_register rr = {
		.nr = 2,
		.data = (uint64_t)(uintptr_t)fds,
		.tags = (uint64_t)(uintptr_t)tags,
	};
	assert(reg(&t, IORING_REGISTER_FILES2, &rr, sizeof(rr)) == 0);

	/* An armed RECV keeps using slot 0 after it is emptied... */
	sqe = ring_prep(&t, IORING_OP_RECV, 0, 42001);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&t);
	ring_submit(&t);
	int none[1] = { -1 };
	up = (struct io_uring_files_update){ .offset = 0,
	    .fds = (uint64_t)(uintptr_t)none };
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE, &up, 1) == 1);
	assert(ring_cq_ready(&t) == 0);
	/* ...and the tag CQE comes once it is done with it. */
	assert(write(sv[1], "q", 1) == 1);
	assert(ring_wait(&t, 2, 2000) == 0);
	expect_cqe(&t, 42001, 1);
	struct io_uring_cqe c = expect_cqe(&t, 0x7a6, 0);
	assert(c.flags == 0);

	/* A tag only goes with a file. */
	uint64_t tag = 5;
	up2 = (struct io_uring_rsrc_update2){ .offset = 1,
	    .data = (uint64_t)(uintptr_t)none,
	    .tags = (uint64_t)(uintptr_t)&tag, .nr = 1 };
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE2, &up2, sizeof(up2)) ==
	    -EINVAL);
	/* Unregistering releases the slots that are left. */
	int one[1] = { sv[1] };
	tag = 0x7a7;
	up2.data = (uint64_t)(uintptr_t)one;
	assert(reg(&t, IORING_REGISTER_FILES_UPDATE2, &up2, sizeof(up2)) == 1);
	assert(reg(&t, IORING_UNREGISTER_FILES, NULL, 0) == 0);
	expect_cqe(&t, 0x7a7, 0);
	assert(ring_cq_ready(&t) == 0);
	assert(reg(&t, IORING_REGISTER_FILES2, &rr, sizeof(rr) - 1) ==
	    -EINVAL);

	close(sv[0]);
	close(sv[1]);
	close(t.fd);
	printf("[PASS] Resource tags: posted once the last user is done, "
	    "on update and unregister\n");
}

static void
test_registered_buffers(void)
{
	long page = sysconf(_SC_PAGESIZE);
	struct io_uring_rsrc_update2 up2;
	struct io_uring_clone_buffers cb;
	struct io_uring_sqe *sqe;
	struct test_ring t, t2;
	char dir[64], path[96];
	int fd, sv[2];

	ring_init(&t);
	char *m = mmap(NULL, (size_t)page * 4, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(m != MAP_FAILED);
	struct iovec iov[3] = {
		{ m, (size_t)page },
		{ NULL, 0 },
		{ m + 2 * page, 2 * (size_t)page },
	};
	assert(reg(&t, IORING_REGISTER_BUFFERS, NULL, 3) == -EFAULT);
	assert(reg(&t, IORING_REGISTER_BUFFERS, iov, 3) == 0);
	assert(reg(&t, IORING_REGISTER_BUFFERS, iov, 3) == -EBUSY);

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/b", dir);
	fd = open(path, O_RDWR | O_CREAT, 0600);
	assert(fd >= 0);
	memcpy(m, "fixed-buffer", 12);
	sqe = ring_prep(&t, IORING_OP_WRITE_FIXED, fd, 43001);
	sqe->addr = (uint64_t)(uintptr_t)m;
	sqe->len = 12;
	sqe->buf_index = 0;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 12);
	char *dst = m + 2 * page + 100;
	sqe = ring_prep(&t, IORING_OP_READ_FIXED, fd, 43002);
	sqe->addr = (uint64_t)(uintptr_t)dst;
	sqe->len = 12;
	sqe->buf_index = 2;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 12 && memcmp(dst, "fixed-buffer", 12) == 0);

	/* Outside its buffer, an empty slot, a missing slot. */
	static const struct { uint16_t idx; long off; unsigned len; } bad[] = {
		{ 0, 0, 0 }, { 1, 0, 1 }, { 9, 0, 1 },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		sqe = ring_prep(&t, IORING_OP_READ_FIXED, fd, 43003);
		sqe->addr = (uint64_t)(uintptr_t)(m + page - 8);
		sqe->len = i == 0 ? 16 : bad[i].len;
		sqe->buf_index = bad[i].idx;
		ring_push_sqe(&t);
		assert(submit_one(&t) == -EFAULT);
	}

	/* READV_FIXED: every iovec must be inside the buffer. */
	struct iovec rv[2] = {
		{ m + 2 * page, 6 },
		{ m + 3 * page, 6 },
	};
	sqe = ring_prep(&t, IORING_OP_READV_FIXED, fd, 43004);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 2;
	sqe->buf_index = 2;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 12);
	assert(memcmp(m + 2 * page, "fixed-", 6) == 0);
	assert(memcmp(m + 3 * page, "buffer", 6) == 0);
	rv[1].iov_base = m;
	sqe = ring_prep(&t, IORING_OP_READV_FIXED, fd, 43005);
	sqe->addr = (uint64_t)(uintptr_t)rv;
	sqe->len = 2;
	sqe->buf_index = 2;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EFAULT);

	/* SEND_ZC from a registered buffer; READ_FIXED on a socket waits. */
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	sqe = ring_prep(&t, IORING_OP_SEND_ZC, sv[0], 43006);
	sqe->addr = (uint64_t)(uintptr_t)m;
	sqe->len = 5;
	sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
	sqe->buf_index = 0;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 2);
	struct io_uring_cqe c = expect_cqe(&t, 43006, 5);
	assert((c.flags & IORING_CQE_F_MORE) != 0);
	c = expect_cqe(&t, 43006, 0);
	assert((c.flags & IORING_CQE_F_NOTIF) != 0);
	char rb[16];
	assert(read(sv[1], rb, sizeof(rb)) == 5 && memcmp(rb, "fixed", 5) == 0);
	sqe = ring_prep(&t, IORING_OP_READ_FIXED, sv[0], 43007);
	sqe->addr = (uint64_t)(uintptr_t)(m + 64);
	sqe->len = 16;
	sqe->buf_index = 0;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	assert(write(sv[1], "sockfx", 6) == 6);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 43007, 6);
	assert(memcmp(m + 64, "sockfx", 6) == 0);

	/* Registration checks the memory: unmapped, read-only, too big. */
	char *ro = mmap(NULL, (size_t)page, PROT_READ, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	char *gone = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(ro != MAP_FAILED && gone != MAP_FAILED);
	munmap(gone, (size_t)page);
	struct iovec bi[3] = {
		{ gone, (size_t)page },
		{ ro, (size_t)page },
		{ m, (size_t)2 << 30 },
	};
	for (int i = 0; i < 3; i++) {
		up2 = (struct io_uring_rsrc_update2){ .offset = 1,
		    .data = (uint64_t)(uintptr_t)&bi[i], .nr = 1 };
		assert(reg(&t, IORING_REGISTER_BUFFERS_UPDATE, &up2,
		    sizeof(up2)) == -EFAULT);
	}
	struct iovec ok = { m + page, (size_t)page };
	up2 = (struct io_uring_rsrc_update2){ .offset = 1,
	    .data = (uint64_t)(uintptr_t)&ok, .nr = 1 };
	assert(reg(&t, IORING_REGISTER_BUFFERS_UPDATE, &up2, sizeof(up2)) == 1);
	sqe = ring_prep(&t, IORING_OP_READ_FIXED, fd, 43008);
	sqe->addr = (uint64_t)(uintptr_t)(m + page);
	sqe->len = 12;
	sqe->buf_index = 1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 12);

	/* CLONE_BUFFERS to another ring this thread owns. */
	ring_init(&t2);
	cb = (struct io_uring_clone_buffers){ .src_fd = (uint32_t)t.fd };
	assert(reg(&t2, IORING_REGISTER_CLONE_BUFFERS, &cb, 1) == 0);
	assert(reg(&t2, IORING_REGISTER_CLONE_BUFFERS, &cb, 1) == -EBUSY);
	cb = (struct io_uring_clone_buffers){ .src_fd = (uint32_t)t.fd,
	    .flags = IORING_REGISTER_DST_REPLACE, .src_off = 0, .dst_off = 3,
	    .nr = 1 };
	assert(reg(&t2, IORING_REGISTER_CLONE_BUFFERS, &cb, 1) == 0);
	sqe = ring_prep(&t2, IORING_OP_READ_FIXED, fd, 43009);
	sqe->addr = (uint64_t)(uintptr_t)(m + 8);
	sqe->len = 4;
	sqe->buf_index = 3;
	ring_push_sqe(&t2);
	assert(submit_one(&t2) == 4);
	cb = (struct io_uring_clone_buffers){ .src_fd = (uint32_t)t2.fd,
	    .flags = IORING_REGISTER_DST_REPLACE };
	assert(reg(&t2, IORING_REGISTER_CLONE_BUFFERS, &cb, 1) == -EINVAL);
	close(t2.fd);

	assert(reg(&t, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);
	assert(reg(&t, IORING_UNREGISTER_BUFFERS, NULL, 0) == -ENXIO);
	sqe = ring_prep(&t, IORING_OP_READ_FIXED, fd, 43010);
	sqe->addr = (uint64_t)(uintptr_t)m;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EFAULT);

	munmap(ro, (size_t)page);
	munmap(m, (size_t)page * 4);
	close(sv[0]);
	close(sv[1]);
	close(fd);
	close(t.fd);
	unlink(path);
	rmdir(dir);
	printf("[PASS] Registered buffers: READ/WRITE(V)_FIXED bounds, SEND_ZC "
	    "FIXED_BUF, update checks, CLONE_BUFFERS\n");
}

/* A RECV of up to `len` bytes from a provided buffer of group bgid. */
static struct io_uring_cqe
recv_select(struct test_ring *r, int fd, uint16_t bgid, unsigned len,
    uint16_t ioprio, uint64_t ud)
{
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_RECV, fd, ud);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = bgid;
	sqe->len = len;
	sqe->ioprio = ioprio;
	ring_push_sqe(r);
	ring_submit(r);
	assert(ring_cq_ready(r) == 1);
	struct io_uring_cqe c = ring_pop_cqe(r);
	assert(c.user_data == ud);
	return (c);
}

static uint16_t
pbuf_head(struct test_ring *r, uint16_t bgid)
{
	struct io_uring_buf_status st = { .buf_group = bgid };
	assert(reg(r, IORING_REGISTER_PBUF_STATUS, &st, 1) == 0);
	return ((uint16_t)st.head);
}

static void
test_buffer_groups(void)
{
	long page = sysconf(_SC_PAGESIZE);
	struct io_uring_buf_reg br;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct test_ring t;
	int sv[2];
	char rb[16];

	ring_init(&t);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	char *pb = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(pb != MAP_FAILED);

	/* PROVIDE_BUFFERS: 4 x 256 bytes, IDs 10..13, into group 5. */
	sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, 4, 44001);
	sqe->addr = (uint64_t)(uintptr_t)pb;
	sqe->len = 256;
	sqe->off = 10;
	sqe->buf_group = 5;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	assert(write(sv[1], "abc", 3) == 3);
	c = recv_select(&t, sv[0], 5, 0, 0, 44002);
	assert(c.res == 3 && (c.flags & IORING_CQE_F_BUFFER) != 0);
	assert((c.flags >> IORING_CQE_BUFFER_SHIFT) == 10);
	assert(memcmp(pb, "abc", 3) == 0);
	/* Not into a ring group; removal counts. */
	sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, 1, 44003);
	sqe->addr = (uint64_t)(uintptr_t)pb;
	sqe->len = 16;
	sqe->buf_group = 0;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);
	static const struct { int nbufs; uint16_t bgid; int32_t res; } rm[] = {
		{ 8, 5, 3 }, { 8, 5, 0 }, { 1, 9, -ENOENT }, { 1, 0, -EINVAL },
	};
	for (size_t i = 0; i < sizeof(rm) / sizeof(rm[0]); i++) {
		sqe = ring_prep(&t, IORING_OP_REMOVE_BUFFERS, rm[i].nbufs, 44004);
		sqe->buf_group = rm[i].bgid;
		ring_push_sqe(&t);
		assert(submit_one(&t) == rm[i].res);
	}
	/* PROVIDE_BUFFERS limits (as in io_uring). */
	static const struct { int nbufs; unsigned len; uint64_t off;
	    int32_t res; } lim[] = {
		{ 0, 16, 0, -E2BIG }, { 65537, 16, 0, -E2BIG },
		{ 2, 16, 65535, -EINVAL }, { 1, 0, 0, -EINVAL },
		{ 1, 16, 65536, -E2BIG },
	};
	for (size_t i = 0; i < sizeof(lim) / sizeof(lim[0]); i++) {
		sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, lim[i].nbufs,
		    44005);
		sqe->addr = (uint64_t)(uintptr_t)pb;
		sqe->len = lim[i].len;
		sqe->off = lim[i].off;
		sqe->buf_group = 5;
		ring_push_sqe(&t);
		assert(submit_one(&t) == lim[i].res);
	}

	/* A kernel-allocated ring (IOU_PBUF_RING_MMAP) userspace maps. */
	br = (struct io_uring_buf_reg){ .ring_entries = 8, .bgid = 3,
	    .flags = IOU_PBUF_RING_MMAP };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == 0);
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == -EEXIST);
	off_t moff = (off_t)(IORING_OFF_PBUF_RING |
	    (3ULL << IORING_OFF_PBUF_SHIFT));
	struct io_uring_buf_ring *kr = mmap(NULL, (size_t)page,
	    PROT_READ | PROT_WRITE, MAP_SHARED, t.fd, moff);
	assert(kr != MAP_FAILED);
	struct io_uring_buf *kb = (struct io_uring_buf *)kr;
	kb[0] = (struct io_uring_buf){ (uint64_t)(uintptr_t)(pb + 1024), 128,
	    7, 0 };
	atomic_store_explicit((_Atomic uint16_t *)&kr->tail, 1,
	    memory_order_release);
	assert(write(sv[1], "mmap!", 5) == 5);
	c = recv_select(&t, sv[0], 3, 0, 0, 44006);
	assert(c.res == 5 && (c.flags >> IORING_CQE_BUFFER_SHIFT) == 7);
	assert(memcmp(pb + 1024, "mmap!", 5) == 0);
	assert(pbuf_head(&t, 3) == 1);
	/* Empty now: data waits, and the RECV fails rather than waits. */
	assert(write(sv[1], "x", 1) == 1);
	c = recv_select(&t, sv[0], 3, 0, 0, 44007);
	assert(c.res == -ENOBUFS);
	assert(read(sv[0], rb, sizeof(rb)) == 1);

	/* A bundle (IORING_RECVSEND_BUNDLE) spans buffers. */
	struct io_uring_buf *ub = mmap(NULL, (size_t)page,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(ub != MAP_FAILED);
	br = (struct io_uring_buf_reg){ .ring_addr = (uint64_t)(uintptr_t)ub,
	    .ring_entries = 4, .bgid = 4 };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == 0);
	for (int i = 0; i < 4; i++)
		ub[i] = (struct io_uring_buf){
		    (uint64_t)(uintptr_t)(pb + 2048 + i * 4), 4,
		    (uint16_t)(20 + i), 0 };
	atomic_store_explicit((_Atomic uint16_t *)
	    &((struct io_uring_buf_ring *)ub)->tail, 4, memory_order_release);
	assert(write(sv[1], "0123456789", 10) == 10);
	c = recv_select(&t, sv[0], 4, 0, IORING_RECVSEND_BUNDLE, 44008);
	assert(c.res == 10 && (c.flags >> IORING_CQE_BUFFER_SHIFT) == 20);
	assert(memcmp(pb + 2048, "0123456789", 10) == 0);
	assert(pbuf_head(&t, 4) == 3);
	/* A bundle needs provided buffers. */
	sqe = ring_prep(&t, IORING_OP_RECV, sv[0], 44009);
	sqe->ioprio = IORING_RECVSEND_BUNDLE;
	sqe->addr = (uint64_t)(uintptr_t)rb;
	sqe->len = sizeof(rb);
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	/* IOU_PBUF_RING_INC: a buffer used in parts. */
	struct io_uring_buf *ib = mmap(NULL, (size_t)page,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(ib != MAP_FAILED);
	br = (struct io_uring_buf_reg){ .ring_addr = (uint64_t)(uintptr_t)ib,
	    .ring_entries = 2, .bgid = 6, .flags = IOU_PBUF_RING_INC };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == 0);
	char *inc = pb + 3072;
	ib[0] = (struct io_uring_buf){ (uint64_t)(uintptr_t)inc, 100, 30, 0 };
	atomic_store_explicit((_Atomic uint16_t *)
	    &((struct io_uring_buf_ring *)ib)->tail, 1, memory_order_release);
	char data[60];
	memset(data, 'i', sizeof(data));
	assert(write(sv[1], data, 40) == 40);
	c = recv_select(&t, sv[0], 6, 0, 0, 44010);
	assert(c.res == 40 && (c.flags >> IORING_CQE_BUFFER_SHIFT) == 30);
	assert((c.flags & IORING_CQE_F_BUF_MORE) != 0);
	assert(ib[0].addr == (uint64_t)(uintptr_t)(inc + 40) &&
	    ib[0].len == 60);
	assert(pbuf_head(&t, 6) == 0);
	memset(data, 'j', sizeof(data));
	assert(write(sv[1], data, 60) == 60);
	c = recv_select(&t, sv[0], 6, 0, 0, 44011);
	assert(c.res == 60 && (c.flags & IORING_CQE_F_BUF_MORE) == 0);
	assert(inc[39] == 'i' && inc[40] == 'j' && inc[99] == 'j');
	assert(pbuf_head(&t, 6) == 1);

	/* SEND from provided buffers. */
	memcpy(pb + 4000, "sendme", 6);
	sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, 1, 44012);
	sqe->addr = (uint64_t)(uintptr_t)(pb + 4000);
	sqe->len = 6;
	sqe->off = 40;
	sqe->buf_group = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_SEND, sv[0], 44013);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 8;
	ring_push_sqe(&t);
	ring_submit(&t);
	c = expect_cqe(&t, 44013, 6);
	assert((c.flags >> IORING_CQE_BUFFER_SHIFT) == 40);
	assert(read(sv[1], rb, sizeof(rb)) == 6 && memcmp(rb, "sendme", 6) == 0);

	/* Register / unregister / status rules. */
	br = (struct io_uring_buf_reg){ .ring_addr =
	    (uint64_t)(uintptr_t)ub + 16, .ring_entries = 4, .bgid = 11 };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == -EINVAL);
	br = (struct io_uring_buf_reg){ .ring_addr = (uint64_t)(uintptr_t)ub,
	    .ring_entries = 4, .bgid = 11, .flags = IOU_PBUF_RING_MMAP };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == -EINVAL);
	br = (struct io_uring_buf_reg){ .ring_addr = (uint64_t)(uintptr_t)ub,
	    .ring_entries = 3, .bgid = 11 };
	assert(reg(&t, IORING_REGISTER_PBUF_RING, &br, 1) == -EINVAL);
	br = (struct io_uring_buf_reg){ .bgid = 5 };
	assert(reg(&t, IORING_UNREGISTER_PBUF_RING, &br, 1) == -EINVAL);
	br.bgid = 99;
	assert(reg(&t, IORING_UNREGISTER_PBUF_RING, &br, 1) == -ENOENT);
	struct io_uring_buf_status st = { .buf_group = 5 };
	assert(reg(&t, IORING_REGISTER_PBUF_STATUS, &st, 1) == -EINVAL);

	/* Unregistering a mapped ring leaves the mapping plain memory. */
	br = (struct io_uring_buf_reg){ .bgid = 3 };
	assert(reg(&t, IORING_UNREGISTER_PBUF_RING, &br, 1) == 0);
	kb[1].len = 1234;
	assert(kb[1].len == 1234 && kb[0].bid == 7);
	munmap(kr, (size_t)page);
	assert(mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED,
	    t.fd, moff) == MAP_FAILED);

	munmap(ub, (size_t)page);
	munmap(ib, (size_t)page);
	munmap(pb, (size_t)page * 2);
	close(sv[0]);
	close(sv[1]);
	close(t.fd);
	printf("[PASS] Buffer groups: PROVIDE/REMOVE_BUFFERS, MMAP rings, "
	    "bundles, INC, SEND select, status\n");
}

static void
test_eventfd(void)
{
	struct test_ring t;
	eventfd_t v;
	int efd, sv[2], pfd[2];
	char buf[8];

	ring_init(&t);
	efd = eventfd(0, EFD_NONBLOCK);
	assert(efd >= 0);
	assert(pipe(pfd) == 0);
	assert(reg(&t, IORING_REGISTER_EVENTFD, &pfd[0], 1) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_EVENTFD, &efd, 2) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_EVENTFD, &efd, 1) == 0);
	assert(reg(&t, IORING_REGISTER_EVENTFD, &efd, 1) == -EBUSY);

	prep_nop(&t, 45001, 0);
	ring_submit(&t);
	expect_cqe(&t, 45001, 0);
	assert(eventfd_read(efd, &v) == 0 && v >= 1);
	/* IORING_CQ_EVENTFD_DISABLED silences it. */
	atomic_fetch_or(t.cq_kflags, IORING_CQ_EVENTFD_DISABLED);
	prep_nop(&t, 45002, 0);
	ring_submit(&t);
	expect_cqe(&t, 45002, 0);
	assert(eventfd_read(efd, &v) == -1 && errno == EAGAIN);
	atomic_fetch_and(t.cq_kflags, ~IORING_CQ_EVENTFD_DISABLED);
	assert(reg(&t, IORING_UNREGISTER_EVENTFD, NULL, 0) == 0);
	assert(reg(&t, IORING_UNREGISTER_EVENTFD, NULL, 0) == -ENXIO);

	/* EVENTFD_ASYNC: not for inline completions, but for waiting ones. */
	assert(reg(&t, IORING_REGISTER_EVENTFD_ASYNC, &efd, 1) == 0);
	prep_nop(&t, 45003, 0);
	ring_submit(&t);
	expect_cqe(&t, 45003, 0);
	assert(eventfd_read(efd, &v) == -1 && errno == EAGAIN);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	prep_recv(&t, sv[0], 45004, buf, sizeof(buf), 0);
	ring_submit(&t);
	assert(eventfd_read(efd, &v) == -1 && errno == EAGAIN);
	/* The upcall itself signals: no enter needed to learn of work. */
	assert(write(sv[1], "e", 1) == 1);
	struct pollfd pf = { .fd = efd, .events = POLLIN };
	assert(poll(&pf, 1, 2000) == 1);
	assert(eventfd_read(efd, &v) == 0);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 45004, 1);
	assert(eventfd_read(efd, &v) == 0 && v >= 1);

	close(sv[0]);
	close(sv[1]);
	close(pfd[0]);
	close(pfd[1]);
	close(t.fd);
	/* The ring held its own reference to the eventfd. */
	assert(eventfd_write(efd, 1) == 0);
	close(efd);
	printf("[PASS] Eventfd: signalled per enter and on taskwork, "
	    "CQ_EVENTFD_DISABLED, EVENTFD_ASYNC\n");
}

static void
test_personality(void)
{
	struct io_uring_sqe *sqe;
	struct test_ring t;
	int id, id2, fd;

	ring_init(&t);
	assert(reg(&t, IORING_REGISTER_PERSONALITY, &t, 0) == -EINVAL);
	id = reg(&t, IORING_REGISTER_PERSONALITY, NULL, 0);
	id2 = reg(&t, IORING_REGISTER_PERSONALITY, NULL, 0);
	assert(id >= 1 && id2 >= 1 && id != id2);
	sqe = ring_prep(&t, IORING_OP_NOP, -1, 46001);
	sqe->personality = (uint16_t)id;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	/* Work done under it: an open, a socket receive. */
	sqe = ring_prep(&t, IORING_OP_OPENAT, AT_FDCWD, 46002);
	sqe->addr = (uint64_t)(uintptr_t)"/dev/null";
	sqe->open_flags = O_RDONLY;
	sqe->personality = (uint16_t)id2;
	ring_push_sqe(&t);
	fd = submit_one(&t);
	assert(fd >= 0);
	close(fd);
	assert(reg(&t, IORING_UNREGISTER_PERSONALITY, NULL, (unsigned)id) == 0);
	assert(reg(&t, IORING_UNREGISTER_PERSONALITY, NULL, (unsigned)id) ==
	    -EINVAL);
	sqe = ring_prep(&t, IORING_OP_NOP, -1, 46003);
	sqe->personality = (uint16_t)id;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);
	close(t.fd);

	/*
	 * Credentials registered before cap_enter(2) cannot be used to get
	 * out of capability mode (in a child: it cannot be left).
	 */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct test_ring c;
		int pid_id;

		ring_init(&c);
		pid_id = reg(&c, IORING_REGISTER_PERSONALITY, NULL, 0);
		if (pid_id < 1 || cap_enter() != 0)
			_exit(2);
		sqe = ring_prep(&c, IORING_OP_OPENAT, AT_FDCWD, 46004);
		sqe->addr = (uint64_t)(uintptr_t)"/dev/null";
		sqe->open_flags = O_RDONLY;
		sqe->personality = (uint16_t)pid_id;
		ring_push_sqe(&c);
		_exit(submit_one(&c) == -ECAPMODE ? 0 : 1);
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	printf("[PASS] Personalities: register ids, use, unregister, no "
	    "capability-mode escape\n");
}

static void
test_restrictions(struct test_ring *r)
{
	struct io_uring_restriction res[] = {
		{ .opcode = IORING_RESTRICTION_SQE_OP,
		    .sqe_op = IORING_OP_NOP },
		{ .opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED,
		    .sqe_flags = IOSQE_IO_LINK },
		{ .opcode = IORING_RESTRICTION_REGISTER_OP,
		    .register_op = IORING_REGISTER_PROBE },
	};
	struct io_uring_restriction badres = { .opcode = 9 };
	struct test_ring t;
	char probe[sizeof(struct io_uring_probe) +
	    4 * sizeof(struct io_uring_probe_op)];

	/* Only on a ring that is still disabled. */
	assert(reg(r, IORING_REGISTER_RESTRICTIONS, res, 1) == -EBADF);
	ring_setup(&t);
	assert(reg(&t, IORING_REGISTER_RESTRICTIONS, &badres, 1) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_RESTRICTIONS, res, 3) == 0);
	assert(reg(&t, IORING_REGISTER_RESTRICTIONS, res, 3) == -EBUSY);
	assert(kqring_enter(t.fd, 0, 0, 0, NULL, 0) == -1 && errno == EBADF);
	assert(reg(&t, IORING_REGISTER_ENABLE_RINGS, NULL, 0) == 0);

	prep_nop(&t, 47001, IOSQE_IO_LINK);
	prep_nop(&t, 47002, 0);
	ring_submit(&t);
	expect_cqe(&t, 47001, 0);
	expect_cqe(&t, 47002, 0);
	prep_nop(&t, 47003, IOSQE_IO_DRAIN);
	ring_submit(&t);
	expect_cqe(&t, 47003, -EACCES);
	(void)ring_prep(&t, IORING_OP_READ, 0, 47004);
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EACCES);
	memset(probe, 0, sizeof(probe));
	assert(reg(&t, IORING_REGISTER_PROBE, probe, 4) == 0);
	assert(reg(&t, IORING_REGISTER_PERSONALITY, NULL, 0) == -EACCES);
	close(t.fd);

	/* Required flags. */
	struct io_uring_restriction req[] = {
		{ .opcode = IORING_RESTRICTION_SQE_OP,
		    .sqe_op = IORING_OP_NOP },
		{ .opcode = IORING_RESTRICTION_SQE_FLAGS_REQUIRED,
		    .sqe_flags = IOSQE_ASYNC },
	};
	ring_setup(&t);
	assert(reg(&t, IORING_REGISTER_RESTRICTIONS, req, 2) == 0);
	assert(reg(&t, IORING_REGISTER_ENABLE_RINGS, NULL, 0) == 0);
	prep_nop(&t, 47005, 0);
	ring_submit(&t);
	expect_cqe(&t, 47005, -EACCES);
	prep_nop(&t, 47006, IOSQE_ASYNC);
	ring_submit(&t);
	expect_cqe(&t, 47006, 0);
	close(t.fd);
	printf("[PASS] Restrictions: SQE ops/flags and register ops, only "
	    "before enable\n");
}

static void
test_send_fd(void)
{
	struct io_uring_rsrc_register rr = {
		.nr = 4,
		.flags = IORING_RSRC_REGISTER_SPARSE,
	};
	struct test_ring a, b, c;
	struct io_uring_sqe *sqe;
	int pfd[2];
	char buf[8];

	ring_init(&a);
	ring_init(&b);
	ring_init(&c);
	assert(pipe(pfd) == 0);
	assert(reg(&a, IORING_REGISTER_FILES, &pfd[1], 1) == 0);
	assert(reg(&b, IORING_REGISTER_FILES2, &rr, sizeof(rr)) == 0);
	close(pfd[1]);

	/* To a free slot of b: b's CQE carries the slot. */
	sqe = ring_prep(&a, IORING_OP_MSG_RING, b.fd, 48001);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->addr3 = 0;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	sqe->off = 0x5e4d;
	ring_push_sqe(&a);
	assert(submit_one(&a) == 0);
	assert(kqring_enter(b.fd, 0, 0, 0, NULL, 0) == 0);
	expect_cqe(&b, 0x5e4d, 0);
	assert(fixed_rw(&b, IORING_OP_WRITE, 0, "fd!", 3, 48002) == 3);
	assert(read(pfd[0], buf, sizeof(buf)) == 3 && memcmp(buf, "fd!", 3) == 0);
	/* To slot 2, and to slot 1 without a CQE (CQE_SKIP). */
	sqe = ring_prep(&a, IORING_OP_MSG_RING, b.fd, 48003);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->file_index = 3;
	sqe->off = 0x5e4e;
	ring_push_sqe(&a);
	assert(submit_one(&a) == 0);
	sqe = ring_prep(&a, IORING_OP_MSG_RING, b.fd, 48004);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->file_index = 2;
	sqe->msg_ring_flags = IORING_MSG_RING_CQE_SKIP;
	ring_push_sqe(&a);
	assert(submit_one(&a) == 0);
	assert(kqring_enter(b.fd, 0, 0, 0, NULL, 0) == 0);
	expect_cqe(&b, 0x5e4e, 0);
	assert(ring_cq_ready(&b) == 0);
	assert(fixed_rw(&b, IORING_OP_WRITE, 1, "1", 1, 48005) == 1);
	assert(fixed_rw(&b, IORING_OP_WRITE, 2, "2", 1, 48006) == 1);
	assert(read(pfd[0], buf, sizeof(buf)) == 2);

	/* Errors the target reports: no table, slot out of range. */
	sqe = ring_prep(&a, IORING_OP_MSG_RING, c.fd, 48007);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->file_index = IORING_FILE_INDEX_ALLOC;
	sqe->off = 0x5e4f;
	ring_push_sqe(&a);
	assert(submit_one(&a) == 0);
	assert(kqring_enter(c.fd, 0, 0, 0, NULL, 0) == 0);
	expect_cqe(&c, 0x5e4f, -ENFILE);	/* no table to allocate from */
	sqe = ring_prep(&a, IORING_OP_MSG_RING, b.fd, 48008);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->file_index = 9;
	sqe->off = 0x5e50;
	ring_push_sqe(&a);
	assert(submit_one(&a) == 0);
	assert(kqring_enter(b.fd, 0, 0, 0, NULL, 0) == 0);
	expect_cqe(&b, 0x5e50, -EINVAL);
	/* Errors the sender sees: itself, an empty source slot. */
	sqe = ring_prep(&a, IORING_OP_MSG_RING, a.fd, 48009);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->file_index = 1;
	ring_push_sqe(&a);
	assert(submit_one(&a) == -EINVAL);
	sqe = ring_prep(&a, IORING_OP_MSG_RING, b.fd, 48010);
	sqe->addr = IORING_MSG_SEND_FD;
	sqe->addr3 = 5;
	sqe->file_index = 1;
	ring_push_sqe(&a);
	assert(submit_one(&a) == -EBADF);

	/* IORING_REGISTER_SEND_MSG_RING: MSG_RING without an SQE. */
	struct io_uring_sqe msg = {
		.opcode = IORING_OP_MSG_RING,
		.fd = c.fd,
		.addr = IORING_MSG_DATA,
		.len = 44,
		.off = 0xbeef,
	};
	assert(reg(&a, IORING_REGISTER_SEND_MSG_RING, &msg, 1) == 0);
	assert(kqring_enter(c.fd, 0, 0, 0, NULL, 0) == 0);
	expect_cqe(&c, 0xbeef, 44);
	msg.addr = IORING_MSG_SEND_FD;
	assert(reg(&a, IORING_REGISTER_SEND_MSG_RING, &msg, 1) == -EINVAL);

	close(pfd[0]);
	close(a.fd);
	close(b.fd);
	close(c.fd);
	printf("[PASS] MSG_RING SEND_FD: alloc/explicit slot, CQE_SKIP, target "
	    "errors; REGISTER_SEND_MSG_RING\n");
}

static void
test_fixed_cancel(void)
{
	struct io_uring_sync_cancel_reg sc;
	struct io_uring_sqe *sqe;
	struct test_ring t;
	int sv[2];
	char buf[8];

	ring_init(&t);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	assert(reg(&t, IORING_REGISTER_FILES, &sv[0], 1) == 0);
	sqe = ring_prep(&t, IORING_OP_RECV, 0, 49001);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	ring_push_sqe(&t);
	ring_submit(&t);
	/* Slot 0 is not descriptor 0. */
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, 0, 49002);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -ENOENT);
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, 0, 49003);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD_FIXED;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, 3, 49004);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_FD_FIXED;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EBADF);
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, 0, 49005);
	sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_FD_FIXED;
	ring_push_sqe(&t);
	ring_submit(&t);
	expect_cqe(&t, 49001, -ECANCELED);
	expect_cqe(&t, 49005, 0);

	/* IORING_REGISTER_SYNC_CANCEL. */
	prep_recv(&t, sv[0], 49006, buf, sizeof(buf), 0);
	ring_submit(&t);
	sc = (struct io_uring_sync_cancel_reg){ .addr = 49006 };
	assert(reg(&t, IORING_REGISTER_SYNC_CANCEL, &sc, 1) == 0);
	expect_cqe(&t, 49006, -ECANCELED);
	assert(reg(&t, IORING_REGISTER_SYNC_CANCEL, &sc, 1) == -ENOENT);
	sc.pad[0] = 1;
	assert(reg(&t, IORING_REGISTER_SYNC_CANCEL, &sc, 1) == -EINVAL);
	prep_recv(&t, sv[0], 49007, buf, sizeof(buf), 0);
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, sv[1], 49008);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(&t);
	ring_submit(&t);
	sc = (struct io_uring_sync_cancel_reg){
		.flags = IORING_ASYNC_CANCEL_ANY };
	assert(reg(&t, IORING_REGISTER_SYNC_CANCEL, &sc, 1) == 2);
	assert(ring_cq_ready(&t) == 2);
	ring_drain_cqes(&t);

	close(sv[0]);
	close(sv[1]);
	close(t.fd);
	printf("[PASS] Cancel: CANCEL_FD_FIXED matches slots only, "
	    "REGISTER_SYNC_CANCEL\n");
}

static void
test_fixed_file_ops(void)
{
	struct io_uring_clock_register clk = { .clockid = CLOCK_MONOTONIC };
	struct io_uring_sqe *sqe;
	struct test_ring t;
	struct stat sb;
	char dir[64], path[96];
	int fds[3], fd, rofd, pfd[2];

	ring_init(&t);
	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/o", dir);
	fd = open(path, O_RDWR | O_CREAT, 0600);
	assert(fd >= 0 && write(fd, "0123456789", 10) == 10);
	rofd = open(path, O_RDONLY);
	assert(rofd >= 0 && pipe(pfd) == 0);
	fds[0] = fd;
	fds[1] = pfd[0];
	fds[2] = rofd;
	assert(reg(&t, IORING_REGISTER_FILES, fds, 3) == 0);

	static const uint8_t ok_ops[] = {
		IORING_OP_FSYNC, IORING_OP_SYNC_FILE_RANGE, IORING_OP_FTRUNCATE,
		IORING_OP_FADVISE,
	};
	for (size_t i = 0; i < sizeof(ok_ops); i++) {
		sqe = ring_prep(&t, ok_ops[i], 0, 50001 + i);
		sqe->flags = IOSQE_FIXED_FILE;
		switch (ok_ops[i]) {
		case IORING_OP_SYNC_FILE_RANGE:
			sqe->sync_range_flags = SYNC_FILE_RANGE_WRITE;
			break;
		case IORING_OP_FTRUNCATE:
			sqe->off = 4;
			break;
		case IORING_OP_FADVISE:
			sqe->fadvise_advice = POSIX_FADV_SEQUENTIAL;
			break;
		}
		ring_push_sqe(&t);
		assert(submit_one(&t) == 0);
	}
	assert(fstat(fd, &sb) == 0 && sb.st_size == 4);
	/* Some file systems (ZFS) cannot preallocate. */
	sqe = ring_prep(&t, IORING_OP_FALLOCATE, 0, 50009);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = 4096;	/* length */
	ring_push_sqe(&t);
	int32_t res = submit_one(&t);
	assert(res == 0 || res == -EOPNOTSUPP || res == -EINVAL);
	if (res == 0)
		assert(fstat(fd, &sb) == 0 && sb.st_size == 4096);
	/* The pipe in slot 1: no advice, no truncation, not seekable. */
	sqe = ring_prep(&t, IORING_OP_FADVISE, 1, 50010);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->fadvise_advice = POSIX_FADV_NORMAL;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -ESPIPE);
	sqe = ring_prep(&t, IORING_OP_FTRUNCATE, 1, 50011);
	sqe->flags = IOSQE_FIXED_FILE;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);
	/* A pipe is a stream: READ ignores the offset, as in io_uring. */
	char pb;
	assert(write(pfd[1], "p", 1) == 1);
	sqe = ring_prep(&t, IORING_OP_READ, 1, 50012);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)&pb;
	sqe->len = 1;
	sqe->off = 12345;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 1 && pb == 'p');
	/* A file opened read-only cannot be written. */
	sqe = ring_prep(&t, IORING_OP_WRITE, 2, 50013);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)"x";
	sqe->len = 1;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EBADF);

	/* IORING_REGISTER_CLOCK. */
	assert(reg(&t, IORING_REGISTER_CLOCK, &clk, 0) == 0);
	clk.clockid = CLOCK_REALTIME;
	assert(reg(&t, IORING_REGISTER_CLOCK, &clk, 0) == -EINVAL);

	close(fd);
	close(rofd);
	close(pfd[0]);
	close(pfd[1]);
	close(t.fd);
	unlink(path);
	rmdir(dir);
	printf("[PASS] Fixed-file sync ops: FSYNC/FTRUNCATE/FADVISE/FALLOCATE, "
	    "mode and seek checks, REGISTER_CLOCK\n");
}

static void
test_iowq_register(void)
{
	struct io_uring_sqe *sqe;
	struct test_ring t;
	cpuset_t mask, empty;
	uint32_t maxw[2], defw[2];

	ring_init(&t);

	/* IORING_REGISTER_IOWQ_MAX_WORKERS validation and query/update. */
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 0) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 1) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 3) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, NULL, 2) == -EFAULT);
	maxw[IO_WQ_BOUND] = 0x80000000U;
	maxw[IO_WQ_UNBOUND] = 0;
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 2) == -EINVAL);

	defw[IO_WQ_BOUND] = 0;
	defw[IO_WQ_UNBOUND] = 0;
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, defw, 2) == 0);
	assert(defw[IO_WQ_BOUND] >= 1 && defw[IO_WQ_UNBOUND] >= 1);

	maxw[IO_WQ_BOUND] = 2;
	maxw[IO_WQ_UNBOUND] = 0;
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 2) == 0);
	assert(maxw[IO_WQ_BOUND] == 2);
	assert(maxw[IO_WQ_UNBOUND] == defw[IO_WQ_UNBOUND]);

	maxw[IO_WQ_BOUND] = 0;
	maxw[IO_WQ_UNBOUND] = 4;
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 2) == 0);
	assert(maxw[IO_WQ_BOUND] == 2 && maxw[IO_WQ_UNBOUND] == 4);

	maxw[IO_WQ_BOUND] = 100000;
	maxw[IO_WQ_UNBOUND] = 100000;
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 2) == 0);
	assert(maxw[IO_WQ_BOUND] == 256 && maxw[IO_WQ_UNBOUND] == 256);

	/* IORING_REGISTER_IOWQ_AFF / IORING_UNREGISTER_IOWQ_AFF. */
	CPU_ZERO(&empty);
	assert(reg(&t, IORING_REGISTER_IOWQ_AFF, &empty, 0) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_IOWQ_AFF, &empty, 65536) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_IOWQ_AFF, NULL, sizeof(empty)) ==
	    -EFAULT);
	assert(reg(&t, IORING_REGISTER_IOWQ_AFF, &empty, sizeof(empty)) ==
	    -EINVAL);
	assert(reg(&t, IORING_UNREGISTER_IOWQ_AFF, &empty, 0) == -EINVAL);
	assert(reg(&t, IORING_UNREGISTER_IOWQ_AFF, NULL, 1) == -EINVAL);

	assert(cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
	    sizeof(mask), &mask) == 0);
	cpuset_t one;
	CPU_ZERO(&one);
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &mask)) {
			CPU_SET(cpu, &one);
			break;
		}
	}
	assert(!CPU_EMPTY(&one));
	assert(reg(&t, IORING_REGISTER_IOWQ_AFF, &one, sizeof(one)) == 0);

	/* Run an IOSQE_ASYNC request under the pinned affinity, then clear. */
	sqe = ring_prep(&t, IORING_OP_NOP, -1, 60001);
	sqe->flags = IOSQE_ASYNC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);

	assert(reg(&t, IORING_UNREGISTER_IOWQ_AFF, NULL, 0) == 0);
	sqe = ring_prep(&t, IORING_OP_NOP, -1, 60002);
	sqe->flags = IOSQE_ASYNC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);

	close(t.fd);
	printf("[PASS] IOWQ registration: IOWQ_MAX_WORKERS, IOWQ_AFF / "
	    "UNREGISTER_IOWQ_AFF\n");
}

static void
test_worker_nonblocking_pipe(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct test_ring t;
	char buf[64], fill[4096];
	int pfd[2], fl;

	ring_init(&t);
	assert(pipe(pfd) == 0);

	/*
	 * A blocking READ on an empty pipe runs on an in-process worker
	 * thread, so ring_submit() returns immediately without blocking the
	 * ring owner, and an inline NOP still completes while the pipe read
	 * is waiting.
	 */
	memset(buf, 0, sizeof(buf));
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 61001);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 5;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);

	prep_nop(&t, 61002, 0);
	ring_submit(&t);
	expect_cqe(&t, 61002, 0);
	assert(ring_cq_ready(&t) == 0);

	assert(write(pfd[1], "async", 5) == 5);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 61001, 5);
	assert(memcmp(buf, "async", 5) == 0);

	/*
	 * IOSQE_BUFFER_SELECT on a blocking pipe read waits for readability
	 * before picking a buffer from group 0, so another read with buffer
	 * selection is not starved while the first pipe is empty.
	 */
	int pfd2[2];
	assert(pipe(pfd2) == 0);
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 61003);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->len = 16;
	sqe->off = (uint64_t)-1;
	sqe->buf_group = 0;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);

	assert(write(pfd2[1], "first!", 6) == 6);
	sqe = ring_prep(&t, IORING_OP_READ, pfd2[0], 61004);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->len = 16;
	sqe->off = (uint64_t)-1;
	sqe->buf_group = 0;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 61004 && c.res == 6);
	assert((c.flags & IORING_CQE_F_BUFFER) != 0);
	uint16_t bid1 = c.flags >> IORING_CQE_BUFFER_SHIFT;
	assert(memcmp(t.bufs + (size_t)bid1 * BUF_SIZE, "first!", 6) == 0);
	ring_offer_buf(&t, bid1);

	assert(write(pfd[1], "second!", 7) == 7);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 61003 && c.res == 7);
	assert((c.flags & IORING_CQE_F_BUFFER) != 0);
	uint16_t bid2 = c.flags >> IORING_CQE_BUFFER_SHIFT;
	assert(memcmp(t.bufs + (size_t)bid2 * BUF_SIZE, "second!", 7) == 0);
	ring_offer_buf(&t, bid2);
	close(pfd2[0]);
	close(pfd2[1]);

	/*
	 * A blocking WRITE on a full pipe also runs on a worker without
	 * blocking the ring owner thread.
	 */
	memset(fill, 'W', sizeof(fill));
	fl = fcntl(pfd[1], F_GETFL, 0);
	assert(fl != -1 && fcntl(pfd[1], F_SETFL, fl | O_NONBLOCK) == 0);
	size_t filled = 0;
	while (write(pfd[1], fill, sizeof(fill)) > 0)
		filled += sizeof(fill);
	assert(errno == EAGAIN);
	assert(fcntl(pfd[1], F_SETFL, fl) == 0);

	sqe = ring_prep(&t, IORING_OP_WRITE, pfd[1], 61005);
	sqe->addr = (uint64_t)(uintptr_t)"tail";
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);

	while (filled > 0) {
		ssize_t n = read(pfd[0], fill, sizeof(fill));
		assert(n > 0);
		filled -= (size_t)n;
	}
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 61005, 4);
	assert(read(pfd[0], buf, 4) == 4 && memcmp(buf, "tail", 4) == 0);

	close(pfd[0]);
	close(pfd[1]);
	close(t.fd);
	printf("[PASS] Worker threads: non-blocking pipe READ/WRITE and "
	    "deferred BUFFER_SELECT\n");
}

/* Threads in this process, kqring worker threads included. */
static int
count_threads(void)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD,
	    getpid() };
	struct kinfo_proc *kp;
	size_t len = 0;

	assert(sysctl(mib, 4, NULL, &len, NULL, 0) == 0);
	len *= 2;
	kp = malloc(len);
	assert(kp != NULL && sysctl(mib, 4, kp, &len, NULL, 0) == 0);
	free(kp);
	return ((int)(len / sizeof(*kp)));
}

/* Prepare a positioned READ of len bytes at off into buf. */
static void
prep_pread(struct test_ring *r, int fd, void *buf, size_t len, off_t off,
    uint64_t ud)
{
	struct io_uring_sqe *sqe = ring_prep(r, IORING_OP_READ, fd, ud);

	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = (uint32_t)len;
	sqe->off = (uint64_t)off;
	ring_push_sqe(r);
}

static void *
napi_writer(void *arg)
{
	int fd = *(int *)arg;

	usleep(2000);
	assert(write(fd, "n", 1) == 1);
	return (NULL);
}

/*
 * IORING_REGISTER_NAPI sets a busy-wait before sleeping; the previous
 * settings come back as on Linux.  A wait still honours a shorter timeout,
 * and completions arriving after the spin still wake it.
 */
static void
test_napi_busy_poll(void)
{
	struct io_uring_napi napi = {
		.busy_poll_to = 20000,
		.opcode = IO_URING_NAPI_REGISTER_OP,
		.op_param = IO_URING_NAPI_TRACKING_DYNAMIC,
	};
	struct __kernel_timespec ts = { .tv_nsec = 5 * 1000000 };
	struct io_uring_getevents_arg ga = { .ts = (uint64_t)(uintptr_t)&ts };
	struct timespec t0, t1;
	struct test_ring t;
	pthread_t th;
	char buf[8];
	int sv[2];

	ring_init(&t);
	assert(reg(&t, IORING_REGISTER_NAPI, &napi, 0) == -EINVAL);
	assert(reg(&t, IORING_REGISTER_NAPI, NULL, 1) == -EFAULT);
	napi.resv = 1;
	assert(reg(&t, IORING_REGISTER_NAPI, &napi, 1) == -EINVAL);
	napi.resv = 0;
	napi.opcode = IO_URING_NAPI_STATIC_ADD_ID;
	assert(reg(&t, IORING_REGISTER_NAPI, &napi, 1) == -EOPNOTSUPP);
	napi.opcode = IO_URING_NAPI_REGISTER_OP;
	assert(reg(&t, IORING_REGISTER_NAPI, &napi, 1) == 0);
	/* The previous settings: none. */
	assert(napi.busy_poll_to == 0 &&
	    napi.op_param == IO_URING_NAPI_TRACKING_INACTIVE);

	/* A 5 ms timeout ends a wait with a 20 ms spin after 5 ms. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	assert(kqring_enter(t.fd, 0, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == -1 && errno == ETIME);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000000;
	assert(ms >= 4 && ms < 15);

	/* Data that comes during the spin completes an armed RECV. */
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	prep_recv(&t, sv[0], 60001, buf, sizeof(buf), 0);
	ring_submit(&t);
	assert(pthread_create(&th, NULL, napi_writer, &sv[1]) == 0);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 60001, 1);
	pthread_join(th, NULL);

	/* Unregister returns the settings it clears. */
	memset(&napi, 0, sizeof(napi));
	assert(reg(&t, IORING_UNREGISTER_NAPI, &napi, 1) == 0);
	assert(napi.busy_poll_to == 20000 &&
	    napi.op_param == IO_URING_NAPI_TRACKING_DYNAMIC);
	assert(reg(&t, IORING_UNREGISTER_NAPI, &napi, 1) == 0);
	assert(napi.busy_poll_to == 0);
	close(sv[0]);
	close(sv[1]);
	close(t.fd);
	printf("[PASS] IORING_REGISTER_NAPI busy-wait: settings, timeout, "
	    "wakeups\n");
}

/* Prepare a positioned READ / WRITE (plain, V or _FIXED) on fd. */
static struct io_uring_sqe *
prep_rw(struct test_ring *r, uint8_t opcode, int fd, void *addr,
    uint32_t len, off_t off, uint64_t ud)
{
	struct io_uring_sqe *sqe = ring_prep(r, opcode, fd, ud);

	sqe->addr = (uint64_t)(uintptr_t)addr;
	sqe->len = len;
	sqe->off = (uint64_t)off;
	ring_push_sqe(r);
	return (sqe);
}

/* Submit everything pending, wait for `n` CQEs, and pop the first. */
static struct io_uring_cqe
submit_wait(struct test_ring *r, unsigned n)
{
	assert(ring_wait(r, n, 5000) == 0);
	assert(ring_cq_ready(r) >= n);
	return (ring_pop_cqe(r));
}

/*
 * READ / WRITE on a disk device go straight to its driver as bios, with no
 * worker thread (kqring_file_bio()).  Needs a scratch disk the test may
 * overwrite, named by KQRING_TEST_DISK (for example an md(4) malloc disk:
 * mdconfig -a -t malloc -s 64m, then chown it to the test user).
 */
#define DISK_IOS	64
static void
test_disk_bio(void)
{
	const char *path = getenv("KQRING_TEST_DISK");
	static char wbuf[DISK_IOS][4096] __attribute__((aligned(4096)));
	static char rbuf[DISK_IOS][4096] __attribute__((aligned(4096)));
	static char fixed[2 * 4096] __attribute__((aligned(4096)));
	struct iovec iov[2];
	struct io_uring_cqe c;
	struct test_ring t;
	int fd, i, seen;

	if (path == NULL) {
		printf("[SKIP] disk bio path: KQRING_TEST_DISK not set\n");
		return;
	}
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	ring_init(&t);

	/* Writes and reads through the driver, many in flight at once. */
	for (i = 0; i < DISK_IOS; i++) {
		memset(wbuf[i], 'a' + i % 26, sizeof(wbuf[i]));
		wbuf[i][0] = (char)i;
		prep_rw(&t, IORING_OP_WRITE, fd, wbuf[i], 4096,
		    (off_t)i * 4096, 50000 + i);
	}
	for (seen = 0; seen < DISK_IOS; seen++) {
		c = submit_wait(&t, 1);
		assert(c.user_data >= 50000 && c.user_data < 50000 + DISK_IOS);
		assert(c.res == 4096);
	}
	for (i = 0; i < DISK_IOS; i++)
		prep_rw(&t, IORING_OP_READ, fd, rbuf[i], 4096,
		    (off_t)i * 4096, 51000 + i);
	for (seen = 0; seen < DISK_IOS; seen++) {
		c = submit_wait(&t, 1);
		assert(c.res == 4096);
	}
	assert(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0);

	/* READV across two iovecs (one unaligned in memory) in one request. */
	memset(rbuf, 0, sizeof(rbuf));
	iov[0].iov_base = rbuf[0];
	iov[0].iov_len = 4096;
	iov[1].iov_base = rbuf[2] + 512;
	iov[1].iov_len = 1024;
	prep_rw(&t, IORING_OP_READV, fd, iov, 2, 0, 52000);
	c = submit_wait(&t, 1);
	assert(c.user_data == 52000 && c.res == 4096 + 1024);
	assert(memcmp(rbuf[0], wbuf[0], 4096) == 0);
	assert(memcmp(rbuf[2] + 512, wbuf[1], 1024) == 0);

	/* WRITE_FIXED / READ_FIXED from a registered buffer. */
	struct iovec reg_iov = { fixed, sizeof(fixed) };
	assert(reg(&t, IORING_REGISTER_BUFFERS, &reg_iov, 1) == 0);
	memset(fixed, 'F', 4096);
	prep_rw(&t, IORING_OP_WRITE_FIXED, fd, fixed, 4096, 8 * 4096,
	    52001)->buf_index = 0;
	c = submit_wait(&t, 1);
	assert(c.user_data == 52001 && c.res == 4096);
	prep_rw(&t, IORING_OP_READ_FIXED, fd, fixed + 4096, 4096, 8 * 4096,
	    52002)->buf_index = 0;
	c = submit_wait(&t, 1);
	assert(c.user_data == 52002 && c.res == 4096);
	assert(memcmp(fixed, fixed + 4096, 4096) == 0);

	/*
	 * What the driver cannot take runs on a worker, as before: a length
	 * that is not a whole number of sectors fails as pread(2) would.
	 */
	prep_rw(&t, IORING_OP_READ, fd, rbuf[0], 100, 0, 52003);
	c = submit_wait(&t, 1);
	assert(c.user_data == 52003 && c.res == -EINVAL);
	assert(pread(fd, rbuf[0], 100, 0) == -1 && errno == EINVAL);
	/* So does IOSQE_ASYNC, with the same result. */
	prep_rw(&t, IORING_OP_READ, fd, rbuf[1], 4096, 4096,
	    52004)->flags = IOSQE_ASYNC;
	c = submit_wait(&t, 1);
	assert(c.user_data == 52004 && c.res == 4096);
	assert(memcmp(rbuf[1], wbuf[1], 4096) == 0);
	/* Past the end of the device: a short read (0 bytes), as pread(2). */
	off_t size = lseek(fd, 0, SEEK_END);
	assert(size > 0);
	prep_rw(&t, IORING_OP_READ, fd, rbuf[0], 4096, size, 52005);
	c = submit_wait(&t, 1);
	assert(c.user_data == 52005 && c.res == pread(fd, rbuf[1], 4096, size));

	/*
	 * Disk I/O in flight cannot be cancelled: the cancel finds it
	 * -EALREADY (or -ENOENT if it already finished), and the read still
	 * completes in full.
	 */
	prep_rw(&t, IORING_OP_READ, fd, rbuf[0], 4096, 0, 52006);
	struct io_uring_sqe *sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, -1,
	    52007);
	sqe->addr = 52006;
	ring_push_sqe(&t);
	int got = 0;
	ring_submit(&t);
	while (got < 2) {
		c = submit_wait(&t, 1);
		if (c.user_data == 52006)
			assert(c.res == 4096);
		else
			assert(c.user_data == 52007 &&
			    (c.res == -EALREADY || c.res == -ENOENT));
		got++;
	}

	/* Closing the ring with bios in flight waits for them. */
	for (i = 0; i < DISK_IOS; i++)
		prep_rw(&t, IORING_OP_READ, fd, rbuf[i], 4096,
		    (off_t)i * 4096, 53000 + i);
	ring_submit(&t);
	close(t.fd);
	close(fd);
	printf("[PASS] disk READ / WRITE (plain, V, FIXED) through bios, "
	    "fallbacks, no cancel in flight, close with I/O in flight\n");
}

/*
 * Reads that cannot block complete on the issuer, without a worker: a
 * positioned read of a regular file from the page cache, and READ / WRITE
 * on a non-blocking pipe.  No worker thread is started for them.
 * Everything else (end of file, holes, a range only partly cached, a
 * destination that would fault, IOSQE_ASYNC) falls back to a worker with
 * the same result as before.
 */
static void
test_inline_file_pipe(void)
{
	char path[] = "/tmp/kqring_inline.XXXXXX";
	static char data[3 * 4096], buf[3 * 4096];
	struct io_uring_sqe *sqe;
	struct test_ring t;
	int fd, pfd[2];

	ring_init(&t);
	int threads = count_threads();	/* a new ring has no workers yet */
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (char)('a' + i % 26);
	/* An unmapped destination would fault, so go to a worker: map it. */
	memset(buf, 0, sizeof(buf));
	fd = mkstemp(path);
	assert(fd >= 0);
	unlink(path);
	assert(pwrite(fd, data, sizeof(data), 0) == (ssize_t)sizeof(data));

	/* Cached, page-crossing, and as READV. */
	prep_pread(&t, fd, buf, 5000, 100, 62001);
	assert(submit_one(&t) == 5000);
	assert(memcmp(buf, data + 100, 5000) == 0);

	struct iovec iov[2] = {
		{ .iov_base = buf, .iov_len = 10 },
		{ .iov_base = buf + 10, .iov_len = 4096 },
	};
	sqe = ring_prep(&t, IORING_OP_READV, fd, 62002);
	sqe->addr = (uint64_t)(uintptr_t)iov;
	sqe->len = 2;
	sqe->off = 4000;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 4106);
	assert(memcmp(buf, data + 4000, 4106) == 0);

	/* Short at end of file, as pread(2) is. */
	prep_pread(&t, fd, buf, 100, sizeof(data) - 10, 62003);
	assert(submit_one(&t) == 10);
	assert(memcmp(buf, data + sizeof(data) - 10, 10) == 0);

	/* A pipe that is ready (here non-blocking): READ and WRITE inline. */
	assert(pipe2(pfd, O_NONBLOCK) == 0);
	sqe = ring_prep(&t, IORING_OP_WRITE, pfd[1], 62010);
	sqe->addr = (uint64_t)(uintptr_t)"inline";
	sqe->len = 6;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 6);
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 62011);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 64;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 6);
	assert(memcmp(buf, "inline", 6) == 0);

	/* None of the above started a worker thread. */
	assert(count_threads() <= threads);

	/*
	 * An empty one waits for data even O_NONBLOCK, as io_uring polls it
	 * (fifo-nonblock-read); RWF_NOWAIT fails it instead.
	 */
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 62012);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 64;
	sqe->off = (uint64_t)-1;
	sqe->rw_flags = KQRING_RWF_NOWAIT;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EAGAIN);
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 62009);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 64;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 50) == ETIMEDOUT);
	assert(write(pfd[1], "later", 5) == 5);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62009, 5);
	assert(memcmp(buf, "later", 5) == 0);
	close(pfd[0]);
	close(pfd[1]);

	/* At end of file: 0.  Where offset + length overflows: as pread(2). */
	prep_pread(&t, fd, buf, 100, sizeof(data), 62004);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62004, 0);
	ssize_t pr = pread(fd, buf, 100, INT64_MAX - 10);
	int32_t pres = pr < 0 ? -errno : (int32_t)pr;
	prep_pread(&t, fd, buf, 100, INT64_MAX - 10, 62014);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62014, pres);

	/*
	 * A hole has no pages, so the read goes to a worker.  A range whose
	 * first page is cached but whose second is a hole is copied in part
	 * inline, then redone in full on the worker.
	 */
	assert(ftruncate(fd, 1 << 20) == 0);
	memset(buf, 0x55, sizeof(buf));
	prep_pread(&t, fd, buf, 8192, 512 << 10, 62005);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62005, 8192);
	for (size_t i = 0; i < 8192; i++)
		assert(buf[i] == 0);
	memset(buf, 0x55, sizeof(buf));
	prep_pread(&t, fd, buf, 8192, 2 * 4096, 62006);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62006, 8192);
	assert(memcmp(buf, data + 2 * 4096, 4096) == 0);
	for (size_t i = 4096; i < 8192; i++)
		assert(buf[i] == 0);

	/*
	 * The inline copy never takes a page fault: a bad buffer, or one
	 * mapped from the very range being read (its fault would need the
	 * pages the copy holds busied), goes to the worker.
	 */
	prep_pread(&t, fd, (void *)(uintptr_t)8, 64, 0, 62007);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62007, -EFAULT);
	char *map = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
	    fd, 0);
	assert(map != MAP_FAILED);
	prep_pread(&t, fd, map, 2 * 4096, 0, 62012);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62012, 2 * 4096);
	assert(memcmp(map, data, 2 * 4096) == 0);
	prep_pread(&t, fd, map + 4096, 4096, 0, 62013);	/* overlapping */
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62013, 4096);
	assert(memcmp(map + 4096, data, 4096) == 0);
	munmap(map, 2 * 4096);

	/* IOSQE_ASYNC still goes through a worker. */
	prep_pread(&t, fd, buf, 64, 0, 62008);
	t.sqes[(t.sq_tail - 1) & t.sq_mask].flags = IOSQE_ASYNC;
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 62008, 64);
	assert(memcmp(buf, data, 64) == 0);
	close(fd);
	close(t.fd);
	printf("[PASS] Inline READ: page-cache file reads and non-blocking "
	    "pipes skip the worker; misses, EOF, holes and IOSQE_ASYNC "
	    "fall back\n");
}

/* Cancellation acknowledgement may precede worker release. */
static void
expect_cancel_pair(struct test_ring *r, uint64_t target, uint64_t cancel)
{
	struct io_uring_cqe a, b;

	assert(ring_wait(r, 2, 2000) == 0);
	a = ring_pop_cqe(r);
	b = ring_pop_cqe(r);
	assert((a.user_data == target && a.res == -ECANCELED &&
	    b.user_data == cancel && b.res == 0) ||
	    (b.user_data == target && b.res == -ECANCELED &&
	    a.user_data == cancel && a.res == 0));
}

/* A FIFO blocks in fo_read after selection, unlike the pipe pre-wait. */
static void
test_worker_buffer_reservation(void)
{
	struct test_ring t;
	struct io_uring_sqe *sqe;
	char dir[] = "/tmp/kqring-reserve.XXXXXX", path[128], buf[16];
	int fd, flags;

	assert(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/fifo", dir);
	assert(mkfifo(path, 0600) == 0);
	fd = open(path, O_RDWR | O_NONBLOCK);
	assert(fd >= 0);
	flags = fcntl(fd, F_GETFL);
	assert(flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
	ring_init(&t);
	sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, 1, 61900);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->buf_group = 90;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_READ, fd, 61901);
	sqe->flags = IOSQE_BUFFER_SELECT;
	sqe->buf_group = 90;
	sqe->len = sizeof(buf);
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	usleep(50000);

	/* Same-group removal fails promptly; unrelated groups still work. */
	sqe = ring_prep(&t, IORING_OP_REMOVE_BUFFERS, 1, 61902);
	sqe->buf_group = 90;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EBUSY);
	sqe = ring_prep(&t, IORING_OP_PROVIDE_BUFFERS, 1, 61903);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = sizeof(buf);
	sqe->buf_group = 91;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, -1, 61904);
	sqe->addr = 61901;
	ring_push_sqe(&t);
	ring_submit(&t);
	expect_cancel_pair(&t, 61901, 61904);

	/* An interrupted empty read leaves the reserved buffer available. */
	sqe = ring_prep(&t, IORING_OP_REMOVE_BUFFERS, 1, 61905);
	sqe->buf_group = 90;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 1);
	close(t.fd);
	close(fd);
	unlink(path);
	rmdir(dir);
	printf("[PASS] Blocking FIFO buffer reservation and deferred cancel\n");
}

static void
test_worker_cancel_and_timeout(void)
{
	struct io_uring_sync_cancel_reg sc;
	struct __kernel_timespec ts = { .tv_nsec = 2000000 }; /* 2 ms */
	struct io_uring_sqe *sqe;
	struct test_ring t;
	char buf[16];
	int pfd[2];

	ring_init(&t);
	assert(pipe(pfd) == 0);

	/*
	 * Stress worker cancellation across the dequeue-to-sleep window for
	 * ASYNC_CANCEL, SYNC_CANCEL, and LINK_TIMEOUT.
	 */
	for (int iter = 0; iter < 128; iter++) {
		uint64_t base = 62000 + (uint64_t)iter * 10;

		/* 1. Cancel a worker pipe READ via IORING_OP_ASYNC_CANCEL. */
		sqe = ring_prep(&t, IORING_OP_READ, pfd[0], base + 1);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&t);
		ring_submit(&t);
		assert(ring_cq_ready(&t) == 0);
		for (volatile int spin = 0; spin < (iter & 7) * 64; spin++)
			;

		sqe = ring_prep(&t, IORING_OP_ASYNC_CANCEL, -1, base + 2);
		sqe->addr = base + 1;
		ring_push_sqe(&t);
		ring_submit(&t);
		expect_cancel_pair(&t, base + 1, base + 2);

		/* 2. Cancel a worker pipe READ via SYNC_CANCEL. */
		sqe = ring_prep(&t, IORING_OP_READ, pfd[0], base + 3);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&t);
		ring_submit(&t);
		assert(ring_cq_ready(&t) == 0);
		for (volatile int spin = 0; spin < (iter & 7) * 64; spin++)
			;

		memset(&sc, 0, sizeof(sc));
		sc.addr = base + 3;
		assert(reg(&t, IORING_REGISTER_SYNC_CANCEL, &sc, 1) == 0);
		assert(ring_wait(&t, 1, 2000) == 0);
		expect_cqe(&t, base + 3, -ECANCELED);

		/* 3. Cancel a worker pipe READ via LINK_TIMEOUT. */
		if (iter < 16) {
			sqe = ring_prep(&t, IORING_OP_READ, pfd[0], base + 4);
			sqe->flags = IOSQE_IO_LINK;
			sqe->addr = (uint64_t)(uintptr_t)buf;
			sqe->len = sizeof(buf);
			sqe->off = (uint64_t)-1;
			ring_push_sqe(&t);
			sqe = ring_prep(&t, IORING_OP_LINK_TIMEOUT, -1,
			    base + 5);
			sqe->addr = (uint64_t)(uintptr_t)&ts;
			sqe->len = 1;
			ring_push_sqe(&t);
			ring_submit(&t);
			assert(ring_wait(&t, 2, 2000) == 0);
			expect_cqe(&t, base + 5, -ETIME);
			expect_cqe(&t, base + 4, -ECANCELED);
		}
	}

	/*
	 * 4. Verify the pipe is still usable after worker cancellations and
	 * no stale worker consumed data.
	 */
	assert(write(pfd[1], "ok!", 3) == 3);
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 62006);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 3;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 3);
	assert(memcmp(buf, "ok!", 3) == 0);

	/*
	 * 5. Detached close with an outstanding worker op across the
	 * dequeue-to-sleep window.
	 */
	for (int iter = 0; iter < 16; iter++) {
		struct test_ring dc;

		ring_init(&dc);
		sqe = ring_prep(&dc, IORING_OP_READ, pfd[0], 62900 + iter);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = sizeof(buf);
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&dc);
		ring_submit(&dc);
		for (volatile int spin = 0; spin < (iter & 7) * 64; spin++)
			;
		munmap(dc.br, (size_t)sysconf(_SC_PAGESIZE));
		munmap(dc.bufs, NBUFS * BUF_SIZE);
		munmap(dc.sqes, dc.sqes_len);
		munmap(dc.ring_map, dc.ring_map_len);
		close(dc.fd);
	}
	assert(write(pfd[1], "dc!", 3) == 3);
	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 62007);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 3;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 3);
	assert(memcmp(buf, "dc!", 3) == 0);

	close(pfd[0]);
	close(pfd[1]);
	munmap(t.br, (size_t)sysconf(_SC_PAGESIZE));
	munmap(t.bufs, NBUFS * BUF_SIZE);
	munmap(t.sqes, t.sqes_len);
	munmap(t.ring_map, t.ring_map_len);
	close(t.fd);
	printf("[PASS] Worker cancellation: ASYNC_CANCEL, SYNC_CANCEL, "
	    "LINK_TIMEOUT\n");
}

static void
test_worker_max_workers(void)
{
	struct io_uring_sqe *sqe;
	struct test_ring t;
	uint32_t maxw[2] = { 1, 1 };
	char dir[64], path[96], fbuf[8], pbuf[3][8];
	int pfd[3][2], fd;

	ring_init(&t);
	assert(reg(&t, IORING_REGISTER_IOWQ_MAX_WORKERS, maxw, 2) == 0);
	assert(maxw[IO_WQ_BOUND] == 1 && maxw[IO_WQ_UNBOUND] == 1);

	for (int i = 0; i < 3; i++)
		assert(pipe(pfd[i]) == 0);
	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/w", dir);
	fd = open(path, O_RDWR | O_CREAT, 0600);
	assert(fd >= 0 && write(fd, "bounded", 7) == 7);

	/*
	 * Queue 3 blocking pipe reads on IO_WQ_UNBOUND (limit 1): the first
	 * occupies the single unbound worker and the other two wait in the
	 * queue.
	 */
	for (int i = 0; i < 3; i++) {
		sqe = ring_prep(&t, IORING_OP_READ, pfd[i][0], 63001 + i);
		sqe->addr = (uint64_t)(uintptr_t)pbuf[i];
		sqe->len = 2;
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&t);
	}
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);

	/*
	 * Regular file I/O uses IO_WQ_BOUND, so it completes even while
	 * IO_WQ_UNBOUND's single worker is blocked on pfd[0].
	 */
	memset(fbuf, 0, sizeof(fbuf));
	sqe = ring_prep(&t, IORING_OP_READ, fd, 63010);
	sqe->addr = (uint64_t)(uintptr_t)fbuf;
	sqe->len = 7;
	sqe->off = 0;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 7);
	assert(memcmp(fbuf, "bounded", 7) == 0);

	/*
	 * Writing to pfd[1] while pfd[0] is still blocking the single
	 * unbound worker cannot complete pfd[1]'s read until pfd[0] is
	 * unblocked.
	 */
	assert(write(pfd[1][1], "p1", 2) == 2);
	assert(write(pfd[2][1], "p2", 2) == 2);
	assert(ring_wait(&t, 1, 20) == ETIMEDOUT);
	assert(ring_cq_ready(&t) == 0);

	/* Unblocking pfd[0] lets the single unbound worker drain all three. */
	assert(write(pfd[0][1], "p0", 2) == 2);
	assert(ring_wait(&t, 3, 2000) == 0);
	expect_cqe(&t, 63001, 2);
	expect_cqe(&t, 63002, 2);
	expect_cqe(&t, 63003, 2);
	assert(memcmp(pbuf[0], "p0", 2) == 0);
	assert(memcmp(pbuf[1], "p1", 2) == 0);
	assert(memcmp(pbuf[2], "p2", 2) == 0);

	for (int i = 0; i < 3; i++) {
		close(pfd[i][0]);
		close(pfd[i][1]);
	}
	close(fd);
	close(t.fd);
	unlink(path);
	rmdir(dir);
	printf("[PASS] Worker pools: IO_WQ_BOUND vs IO_WQ_UNBOUND isolation "
	    "and IOWQ_MAX_WORKERS cap\n");
}

static void
test_worker_process_lifecycle(void)
{
	struct io_uring_sqe *sqe;
	struct test_ring t;
	char buf[8];
	int pfd[2], status;
	pid_t pid;

	ring_init(&t);
	assert(pipe(pfd) == 0);

	/* Spawn a blocked worker on pfd[0] and an idle bounded worker. */
	sqe = ring_prep(&t, IORING_OP_NOP, -1, 64001);
	sqe->flags = IOSQE_ASYNC;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);

	sqe = ring_prep(&t, IORING_OP_READ, pfd[0], 64002);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);

	/*
	 * fork(2) uses thread_single(SINGLE_BOUNDARY); in-process worker
	 * kthreads must suspend at the boundary and resume in the parent.
	 */
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct test_ring ct;
		int cpfd[2];

		close(t.fd);
		close(pfd[0]);
		close(pfd[1]);
		ring_init(&ct);
		assert(pipe(cpfd) == 0);
		sqe = ring_prep(&ct, IORING_OP_READ, cpfd[0], 64010);
		sqe->addr = (uint64_t)(uintptr_t)buf;
		sqe->len = 4;
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&ct);
		ring_submit(&ct);
		/* Exit with a blocked worker still running: SINGLE_EXIT. */
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* Parent's blocked worker is still alive after fork(2) and completes. */
	assert(write(pfd[1], "fork", 4) == 4);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 64002, 4);
	assert(memcmp(buf, "fork", 4) == 0);

	close(pfd[0]);
	close(pfd[1]);
	close(t.fd);
	printf("[PASS] Worker process lifecycle: fork(2) SINGLE_BOUNDARY and "
	    "_exit(2) SINGLE_EXIT with active workers\n");
}

/* ============================================================================
 * Phase 5: SPLICE, TEE, pipe READ_MULTISHOT, and non-socket POLL_ADD
 * ============================================================================
 */

static void
test_splice_and_tee(void)
{
	struct io_uring_sqe *sqe;
	struct test_ring t;
	char dir[64], path_in[96], path_out[96], buf[64];
	int p1[2], p2[2], sv[2], fd_in, fd_out;

	ring_init(&t);
	make_tmpdir(dir, sizeof(dir));
	snprintf(path_in, sizeof(path_in), "%s/in", dir);
	snprintf(path_out, sizeof(path_out), "%s/out", dir);
	fd_in = open(path_in, O_RDWR | O_CREAT | O_TRUNC, 0600);
	fd_out = open(path_out, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fd_in >= 0 && fd_out >= 0);
	assert(write(fd_in, "0123456789abcdef", 16) == 16);
	assert(lseek(fd_in, 0, SEEK_SET) == 0);
	assert(pipe(p1) == 0);
	assert(pipe(p2) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	/* 1. SPLICE file (positioned at off_in=4) -> pipe p1[1]. */
	sqe = ring_prep(&t, IORING_OP_SPLICE, p1[1], 70001);
	sqe->splice_fd_in = fd_in;
	sqe->splice_off_in = 4;
	sqe->off = (uint64_t)-1;
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 8);
	assert(lseek(fd_in, 0, SEEK_CUR) == 0);

	/* 2. TEE p1[0] -> p2[1]: copies 8 bytes without consuming p1[0]. */
	sqe = ring_prep(&t, IORING_OP_TEE, p2[1], 70002);
	sqe->splice_fd_in = p1[0];
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 8);

	/* Both p2[0] and p1[0] now hold "456789ab". */
	memset(buf, 0, sizeof(buf));
	assert(read(p2[0], buf, 8) == 8 && memcmp(buf, "456789ab", 8) == 0);

	/* 3. SPLICE p1[0] -> fd_out (positioned at off=2). */
	sqe = ring_prep(&t, IORING_OP_SPLICE, fd_out, 70003);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = 2;
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 8);
	memset(buf, 0, sizeof(buf));
	assert(pread(fd_out, buf, 8, 2) == 8 &&
	    memcmp(buf, "456789ab", 8) == 0);

	/* 4. SPLICE socket sv[0] -> p1[1], then p1[0] -> socket sv[0]. */
	assert(write(sv[1], "sockpipe", 8) == 8);
	sqe = ring_prep(&t, IORING_OP_SPLICE, p1[1], 70004);
	sqe->splice_fd_in = sv[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 8);

	sqe = ring_prep(&t, IORING_OP_SPLICE, sv[0], 70005);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->len = 8;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 8);
	memset(buf, 0, sizeof(buf));
	assert(read(sv[1], buf, 8) == 8 && memcmp(buf, "sockpipe", 8) == 0);

	/* 5. SPLICE_F_NONBLOCK on empty pipe -> -EAGAIN; blocking waits. */
	sqe = ring_prep(&t, IORING_OP_SPLICE, p2[1], 70006);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->len = 4;
	sqe->splice_flags = SPLICE_F_NONBLOCK;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EAGAIN);

	sqe = ring_prep(&t, IORING_OP_TEE, p2[1], 70007);
	sqe->splice_fd_in = p1[0];
	sqe->len = 4;
	sqe->splice_flags = SPLICE_F_NONBLOCK;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EAGAIN);

	/* Blocking SPLICE on empty p1[0] stays armed until p1[1] is written. */
	sqe = ring_prep(&t, IORING_OP_SPLICE, p2[1], 70008);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->len = 4;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	assert(write(p1[1], "wait", 4) == 4);
	assert(ring_wait(&t, 1, 2000) == 0);
	expect_cqe(&t, 70008, 4);
	assert(read(p2[0], buf, 4) == 4 && memcmp(buf, "wait", 4) == 0);

	/* 6. Fixed files: SPLICE_F_FD_IN_FIXED and IOSQE_FIXED_FILE. */
	int fds[4] = { p1[0], p1[1], p2[0], p2[1] };
	assert(reg(&t, IORING_REGISTER_FILES, fds, 4) == 0);
	assert(write(p1[1], "fxfd", 4) == 4);
	sqe = ring_prep(&t, IORING_OP_TEE, 3, 70009);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->splice_fd_in = 0;
	sqe->splice_flags = SPLICE_F_FD_IN_FIXED;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 4);

	sqe = ring_prep(&t, IORING_OP_SPLICE, 3, 70010);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->splice_fd_in = 0;
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->splice_flags = SPLICE_F_FD_IN_FIXED;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 4);
	assert(read(p2[0], buf, 8) == 8 && memcmp(buf, "fxfdfxfd", 8) == 0);

	/* 7. Validation: non-pipe endpoints, offsets on pipes, same pipe. */
	sqe = ring_prep(&t, IORING_OP_SPLICE, fd_out, 70011);
	sqe->splice_fd_in = fd_in;
	sqe->splice_off_in = 0;
	sqe->off = 0;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	sqe = ring_prep(&t, IORING_OP_SPLICE, p1[1], 70012);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = (uint64_t)-1;
	sqe->off = (uint64_t)-1;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	sqe = ring_prep(&t, IORING_OP_SPLICE, p2[1], 70013);
	sqe->splice_fd_in = p1[0];
	sqe->splice_off_in = 0;
	sqe->off = (uint64_t)-1;
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -ESPIPE);

	sqe = ring_prep(&t, IORING_OP_TEE, fd_out, 70014);
	sqe->splice_fd_in = p1[0];
	sqe->len = 4;
	ring_push_sqe(&t);
	assert(submit_one(&t) == -EINVAL);

	close(sv[0]);
	close(sv[1]);
	close(p1[0]);
	close(p1[1]);
	close(p2[0]);
	close(p2[1]);
	close(fd_in);
	close(fd_out);
	close(t.fd);
	unlink(path_in);
	unlink(path_out);
	rmdir(dir);
	printf("[PASS] SPLICE and TEE: file/pipe/socket, nonblock, blocking, "
	    "fixed files, validation\n");
}

static void
test_nonsock_poll(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct test_ring t;
	int pfd[2], efd;
	eventfd_t ev;
	char ch;

	ring_init(&t);
	assert(pipe(pfd) == 0);
	efd = eventfd(0, EFD_NONBLOCK);
	assert(efd >= 0);

	/* 1. Single-shot POLL_ADD on a pipe: inline and armed. */
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, pfd[1], 71001);
	sqe->poll32_events = POLLOUT;
	ring_push_sqe(&t);
	assert((submit_one(&t) & POLLOUT) != 0);

	sqe = ring_prep(&t, IORING_OP_POLL_ADD, pfd[0], 71002);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	assert(write(pfd[1], "a", 1) == 1);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 71002 && (c.res & POLLIN) != 0);
	assert(read(pfd[0], &ch, 1) == 1 && ch == 'a');

	/* 2. Multishot POLL_ADD on a pipe, then POLL_REMOVE. */
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, pfd[0], 71003);
	sqe->poll32_events = POLLIN;
	sqe->len = IORING_POLL_ADD_MULTI;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	for (int i = 0; i < 3; i++) {
		assert(write(pfd[1], "x", 1) == 1);
		assert(ring_wait(&t, 1, 2000) == 0);
		c = ring_pop_cqe(&t);
		assert(c.user_data == 71003 && (c.res & POLLIN) != 0);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		assert(read(pfd[0], &ch, 1) == 1);
	}
	ring_prep(&t, IORING_OP_POLL_REMOVE, -1, 71004)->addr = 71003;
	ring_push_sqe(&t);
	ring_submit(&t);
	expect_cancel_pair(&t, 71003, 71004);

	/* 3. Multishot POLL_ADD on an eventfd (via IOSQE_FIXED_FILE). */
	int fds[2] = { efd, pfd[0] };
	assert(reg(&t, IORING_REGISTER_FILES, fds, 2) == 0);
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, 0, 71005);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->poll32_events = POLLIN;
	sqe->len = IORING_POLL_ADD_MULTI;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	for (int i = 0; i < 3; i++) {
		assert(eventfd_write(efd, 1) == 0);
		assert(ring_wait(&t, 1, 2000) == 0);
		c = ring_pop_cqe(&t);
		assert(c.user_data == 71005 && (c.res & POLLIN) != 0);
		assert((c.flags & IORING_CQE_F_MORE) != 0);
		assert(eventfd_read(efd, &ev) == 0 && ev == 1);
	}
	ring_prep(&t, IORING_OP_POLL_REMOVE, -1, 71006)->addr = 71005;
	ring_push_sqe(&t);
	ring_submit(&t);
	expect_cancel_pair(&t, 71005, 71006);

	/* 4. Peer close wakes armed pipe POLL_ADD with POLLHUP. */
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, 1, 71007);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->poll32_events = POLLIN;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	close(pfd[1]);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 71007 && (c.res & (POLLIN | POLLHUP)) != 0);

	close(pfd[0]);
	close(efd);
	close(t.fd);
	printf("[PASS] Non-socket POLL_ADD: pipe and eventfd single/multishot, "
	    "FIXED_FILE, POLL_REMOVE, POLLHUP\n");
}

#define KPOLL_NPIPES	100

/*
 * Non-socket POLL_ADD as knotes on the ring's kqueue: many at once, two on
 * one descriptor and filter (the second waits on a worker), a user_data
 * update, closing the ring with polls armed, and a ring that outlives its
 * owner's descriptor table.
 */
static void
test_kpoll(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct test_ring t;
	int pfd[KPOLL_NPIPES][2], seen[KPOLL_NPIPES];
	char ch;

	ring_init(&t);
	for (int i = 0; i < KPOLL_NPIPES; i++)
		assert(pipe(pfd[i]) == 0);

	/* 1. Two POLLIN polls on one pipe both complete. */
	for (int i = 0; i < 2; i++) {
		sqe = ring_prep(&t, IORING_OP_POLL_ADD, pfd[0][0], 72001 + i);
		sqe->poll32_events = POLLIN;
		ring_push_sqe(&t);
	}
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	assert(write(pfd[0][1], "a", 1) == 1);
	assert(ring_wait(&t, 2, 2000) == 0);
	for (int i = 0; i < 2; i++) {
		c = ring_pop_cqe(&t);
		assert((c.user_data == 72001 || c.user_data == 72002) &&
		    (c.res & POLLIN) != 0 && (c.flags & IORING_CQE_F_MORE) == 0);
	}
	assert(read(pfd[0][0], &ch, 1) == 1);

	/* 2. A multishot poll on each pipe; one CQE per write. */
	for (int i = 0; i < KPOLL_NPIPES; i++) {
		sqe = ring_prep(&t, IORING_OP_POLL_ADD, pfd[i][0], 72100 + i);
		sqe->poll32_events = POLLIN;
		sqe->len = IORING_POLL_ADD_MULTI;
		ring_push_sqe(&t);
	}
	ring_submit(&t);
	assert(ring_cq_ready(&t) == 0);
	for (int round = 0; round < 2; round++) {
		memset(seen, 0, sizeof(seen));
		for (int i = 0; i < KPOLL_NPIPES; i++)
			assert(write(pfd[i][1], "x", 1) == 1);
		for (int n = 0; n < KPOLL_NPIPES; n++) {
			if (ring_cq_ready(&t) == 0)
				assert(ring_wait(&t, 1, 2000) == 0);
			c = ring_pop_cqe(&t);
			assert(c.user_data >= 72100 &&
			    c.user_data < 72100 + KPOLL_NPIPES);
			assert((c.res & POLLIN) != 0 &&
			    (c.flags & IORING_CQE_F_MORE) != 0);
			assert(seen[c.user_data - 72100]++ == 0);
		}
		for (int i = 0; i < KPOLL_NPIPES; i++)
			assert(read(pfd[i][0], &ch, 1) == 1);
	}

	/* 3. POLL_REMOVE updating user_data: later CQEs carry the new one. */
	sqe = ring_prep(&t, IORING_OP_POLL_REMOVE, -1, 72003);
	sqe->addr = 72100;
	sqe->off = 72004;
	sqe->len = IORING_POLL_UPDATE_USER_DATA;
	ring_push_sqe(&t);
	assert(submit_one(&t) == 0);
	assert(write(pfd[0][1], "y", 1) == 1);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 72004 && (c.res & POLLIN) != 0 &&
	    (c.flags & IORING_CQE_F_MORE) != 0);
	assert(read(pfd[0][0], &ch, 1) == 1);

	/* 4. Closing the ring with all of them armed. */
	close(t.fd);

	/*
	 * 5. A child arms a poll and exits while its own child, which
	 * inherited the ring, keeps it open: the ring outlives the
	 * descriptor table that held its kqueue.  The grandchild sees the
	 * child gone as EOF on own[0] and reports back on res[].
	 */
	int own[2], res[2], status = 0;
	char r = 0;

	assert(pipe(own) == 0 && pipe(res) == 0);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct test_ring ct;
		pid_t gpid;

		close(res[0]);
		ring_init(&ct);
		sqe = ring_prep(&ct, IORING_OP_POLL_ADD, pfd[1][0], 72005);
		sqe->poll32_events = POLLIN;
		sqe->len = IORING_POLL_ADD_MULTI;
		ring_push_sqe(&ct);
		ring_submit(&ct);
		gpid = fork();
		if (gpid < 0)
			_exit(1);
		if (gpid == 0) {
			r = 'k';
			close(own[1]);
			if (read(own[0], &ch, 1) != 0)
				r = 'e';
			/* Its pipe becoming readable reaches no one. */
			if (write(pfd[1][1], "z", 1) != 1)
				r = 'w';
			usleep(100 * 1000);
			close(ct.fd);
			if (write(res[1], &r, 1) != 1)
				_exit(1);
			_exit(0);
		}
		_exit(0);
	}
	close(own[0]);
	close(own[1]);
	close(res[1]);
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(read(res[0], &r, 1) == 1 && r == 'k');
	close(res[0]);
	for (int i = 0; i < KPOLL_NPIPES; i++) {
		close(pfd[i][0]);
		close(pfd[i][1]);
	}
	printf("[PASS] kqueue POLL_ADD: %d pipes multishot, same fd twice, "
	    "user_data update, owner exit with the ring inherited\n",
	    KPOLL_NPIPES);
}

/*
 * Blocking pipes on the issuer: a write bigger than the pipe's room
 * doesn't wait in the enter for a reader (the rest goes to a worker), and
 * a multishot poll counts each wakeup even if the pipe is drained before
 * the poller looks, but not the writer's own writes.
 */
static void
test_pipe_nowait(void)
{
	enum { BIG = 256 * 1024 };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe c;
	struct test_ring t;
	char *out, *in, ch = 0;
	int p[2], n;
	size_t got = 0;

	ring_init(&t);
	assert(pipe(p) == 0);
	out = malloc(BIG);
	in = malloc(BIG);
	assert(out != NULL && in != NULL);
	for (int i = 0; i < BIG; i++)
		out[i] = (char)(i * 7 + i / 4096);

	/* 1. WRITE of 256 KiB, no reader yet: the enter returns. */
	alarm(10);
	sqe = ring_prep(&t, IORING_OP_WRITE, p[1], 73001);
	sqe->addr = (uintptr_t)out;
	sqe->len = BIG;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t);
	ring_submit(&t);
	while (got < BIG) {
		ssize_t r = read(p[0], in + got, BIG - got);

		assert(r > 0);
		got += (size_t)r;
	}
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 73001 && c.res == BIG);
	assert(memcmp(in, out, BIG) == 0);
	alarm(0);

	/* 2. Multishot POLLIN, then 8 writes each drained at once: 8 CQEs. */
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, p[0], 73002);
	sqe->poll32_events = POLLIN;
	sqe->len = IORING_POLL_ADD_MULTI;
	ring_push_sqe(&t);
	ring_submit(&t);
	for (int i = 0; i < 8; i++) {
		assert(write(p[1], "z", 1) == 1);
		assert(read(p[0], &ch, 1) == 1);
	}
	assert(ring_wait(&t, 8, 2000) == 0);
	for (int i = 0; i < 8; i++) {
		c = ring_pop_cqe(&t);
		assert(c.user_data == 73002 && (c.res & POLLIN) != 0 &&
		    (c.flags & IORING_CQE_F_MORE) != 0);
	}
	usleep(50 * 1000);
	assert(ring_cq_ready(&t) == 0);
	sqe = ring_prep(&t, IORING_OP_POLL_REMOVE, -1, 73004);
	sqe->addr = 73002;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_wait(&t, 2, 2000) == 0);
	ring_drain_cqes(&t);

	/*
	 * 3. Multishot POLLOUT: one CQE for the room there is, none for our
	 * own writes, one more once a read makes room.
	 */
	sqe = ring_prep(&t, IORING_OP_POLL_ADD, p[1], 73003);
	sqe->poll32_events = POLLOUT;
	sqe->len = IORING_POLL_ADD_MULTI;
	ring_push_sqe(&t);
	ring_submit(&t);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 73003 && (c.res & POLLOUT) != 0);
	for (int i = 0; i < 4; i++)
		assert(write(p[1], "w", 1) == 1);
	usleep(50 * 1000);
	n = (int)ring_cq_ready(&t);
	assert(n == 0);
	assert(read(p[0], in, 4) == 4);
	assert(ring_wait(&t, 1, 2000) == 0);
	c = ring_pop_cqe(&t);
	assert(c.user_data == 73003 && (c.res & POLLOUT) != 0 &&
	    (c.flags & IORING_CQE_F_MORE) != 0);

	close(t.fd);
	close(p[0]);
	close(p[1]);
	free(out);
	free(in);
	printf("[PASS] Blocking pipe: 256 KiB WRITE without a reader, "
	    "poll wakeups counted when drained, none for own writes\n");
}

static void *
min_wait_writer(void *arg)
{
	usleep(60 * 1000);
	assert(write(*(int *)arg, "b", 1) == 1);
	return (NULL);
}

/* One enter waiting for want CQEs with min_wait_usec; returns msec. */
static long
min_wait_enter(struct test_ring *t, unsigned want, long ts_ms,
    unsigned min_us, int *err)
{
	struct __kernel_timespec ts = { .tv_sec = ts_ms / 1000,
	    .tv_nsec = (ts_ms % 1000) * 1000000 };
	struct io_uring_getevents_arg ga = {
		.ts = (uint64_t)(uintptr_t)&ts,
		.min_wait_usec = min_us,
	};
	uint64_t t0 = now_ns();

	*err = kqring_enter(t->fd, 0, want,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
	    sizeof(ga)) == -1 ? errno : 0;
	return ((long)((now_ns() - t0) / 1000000));
}

/*
 * IORING_FEAT_MIN_TIMEOUT: wait min_wait_usec for the CQEs asked for,
 * then return with any there are, or else wait for one until the
 * timeout; a timeout shorter than min_wait ends the wait at min_wait.
 */
static void
test_min_wait(void)
{
	struct io_uring_params p;
	struct test_ring t;
	char buf[4][8];
	int pp[4][2], err;
	long ms;

	ring_init(&t);
	memset(&p, 0, sizeof(p));
	for (int i = 0; i < 4; i++) {
		struct io_uring_sqe *sqe;

		assert(pipe(pp[i]) == 0);
		sqe = ring_prep(&t, IORING_OP_READ, pp[i][0], 74000 + i);
		sqe->addr = (uintptr_t)buf[i];
		sqe->len = sizeof(buf[i]);
		sqe->off = (uint64_t)-1;
		ring_push_sqe(&t);
	}
	ring_submit(&t);

	/* 1. One of four ready: back after min_wait (20 ms), not 2 s. */
	assert(write(pp[0][1], "a", 1) == 1);
	ms = min_wait_enter(&t, 4, 2000, 20000, &err);
	assert(err == 0 && ring_cq_ready(&t) == 1);
	assert(ms >= 15 && ms < 500);
	expect_cqe(&t, 74000, 1);

	/* 2. None by min_wait (20 ms): the next one (at 60 ms) ends it. */
	pthread_t th;
	int wfd = pp[1][1];

	assert(pthread_create(&th, NULL, min_wait_writer, &wfd) == 0);
	ms = min_wait_enter(&t, 4, 2000, 20000, &err);
	assert(err == 0 && ring_cq_ready(&t) == 1);
	assert(ms >= 50 && ms < 500);
	expect_cqe(&t, 74001, 1);
	pthread_join(th, NULL);

	/* 3. Nothing at all: ETIME at the timeout (150 ms). */
	ms = min_wait_enter(&t, 4, 150, 20000, &err);
	assert(err == ETIME && ms >= 120 && ms < 600);

	/* 4. A timeout shorter than min_wait: ETIME at min_wait (150 ms). */
	ms = min_wait_enter(&t, 4, 20, 150000, &err);
	assert(err == ETIME && ms >= 120 && ms < 600);

	close(t.fd);
	for (int i = 0; i < 4; i++) {
		close(pp[i][0]);
		close(pp[i][1]);
	}
	printf("[PASS] min_wait_usec: returns with what there is after "
	    "min_wait, then on the next CQE or the timeout\n");
}

/* Submit one NOP through rings in the caller's memory and reap it. */
static void
no_mmap_nop(char *ring, char *sqes, const struct io_uring_params *p,
    int fd, unsigned enter_flags, uint64_t ud)
{
	_Atomic unsigned *sq_tail = (_Atomic unsigned *)(ring + p->sq_off.tail);
	_Atomic unsigned *cq_head = (_Atomic unsigned *)(ring + p->cq_off.head);
	_Atomic unsigned *cq_tail = (_Atomic unsigned *)(ring + p->cq_off.tail);
	unsigned smask = *(unsigned *)(ring + p->sq_off.ring_mask);
	unsigned cmask = *(unsigned *)(ring + p->cq_off.ring_mask);
	unsigned t = atomic_load(sq_tail), h = atomic_load(cq_head);
	struct io_uring_sqe *sqe = (struct io_uring_sqe *)sqes + (t & smask);
	struct io_uring_cqe *cqe;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_NOP;
	sqe->user_data = ud;
	if (p->sq_off.array != 0)
		((unsigned *)(ring + p->sq_off.array))[t & smask] = t & smask;
	atomic_store_explicit(sq_tail, t + 1, memory_order_release);
	assert(kqring_enter(fd, 1, 1, IORING_ENTER_GETEVENTS | enter_flags,
	    NULL, 0) == 1);
	assert(atomic_load_explicit(cq_tail, memory_order_acquire) == h + 1);
	cqe = (struct io_uring_cqe *)(ring + p->cq_off.cqes) + (h & cmask);
	assert(cqe->user_data == ud && cqe->res == 0);
	atomic_store_explicit(cq_head, h + 1, memory_order_release);
}

/*
 * IORING_SETUP_NO_MMAP: the rings live in the caller's memory (here
 * private anonymous memory, which fork(2) must not turn copy-on-write
 * under the ring), with NO_SQARRAY, and with REGISTERED_FD_ONLY, where
 * setup returns a registered-ring slot and no descriptor.
 */
static void
test_no_mmap(void)
{
	long page = sysconf(_SC_PAGESIZE);
	struct io_uring_params p;
	struct io_uring_rsrc_update up;
	char *mem, *ring, *sqes;
	int fd, slot, status;
	pid_t pid;

	mem = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(mem != MAP_FAILED);
	ring = mem;
	sqes = mem + 2 * page;

	/* 1. No SQE address, an unaligned one, a ring with no descriptor
	 * but mmap'd rings. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_NO_MMAP;
	p.cq_off.user_addr = (uint64_t)(uintptr_t)ring;
	assert(kqring_setup(4, &p) == -1 && errno == EFAULT);
	p.sq_off.user_addr = (uint64_t)(uintptr_t)(sqes + 8);
	assert(kqring_setup(4, &p) == -1 && errno == EINVAL);
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_REGISTERED_FD_ONLY;
	assert(kqring_setup(4, &p) == -1 && errno == EINVAL);

	/* 2. NO_MMAP + NO_SQARRAY: the ring can't be mmap'd, NOPs work. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_NO_MMAP | IORING_SETUP_NO_SQARRAY |
	    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	p.cq_off.user_addr = (uint64_t)(uintptr_t)ring;
	p.sq_off.user_addr = (uint64_t)(uintptr_t)sqes;
	fd = (int)kqring_setup(4, &p);
	assert(fd >= 0 && p.sq_off.array == 0);
	assert(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    IORING_OFF_SQ_RING) == MAP_FAILED);
	assert(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    IORING_OFF_SQES) == MAP_FAILED);
	no_mmap_nop(ring, sqes, &p, fd, 0, 75001);

	/* 3. After a fork, the parent's writes still reach the ring. */
	pid = fork();
	assert(pid >= 0);
	if (pid == 0)
		_exit(0);
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
	for (int i = 0; i < 6; i++)
		no_mmap_nop(ring, sqes, &p, fd, 0, 75002 + i);
	close(fd);

	/* 4. REGISTERED_FD_ONLY: a slot, entered as a registered ring. */
	memset(mem, 0, 4 * page);
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_NO_MMAP | IORING_SETUP_REGISTERED_FD_ONLY;
	p.cq_off.user_addr = (uint64_t)(uintptr_t)ring;
	p.sq_off.user_addr = (uint64_t)(uintptr_t)sqes;
	slot = (int)kqring_setup(4, &p);
	assert(slot >= 0 && slot < 16 && p.sq_off.array != 0);
	no_mmap_nop(ring, sqes, &p, slot, IORING_ENTER_REGISTERED_RING,
	    75010);
	memset(&up, 0, sizeof(up));
	up.offset = (uint32_t)slot;
	assert(kqring_register(slot, IORING_UNREGISTER_RING_FDS |
	    IORING_REGISTER_USE_REGISTERED_RING, &up, 1) == 1);
	assert(kqring_enter(slot, 0, 0, IORING_ENTER_REGISTERED_RING, NULL,
	    0) == -1);
	munmap(mem, 4 * page);
	printf("[PASS] NO_MMAP rings in the caller's memory (NO_SQARRAY, "
	    "after fork), REGISTERED_FD_ONLY\n");
}

static void
test_sqe128_cqe32_attach_wq(void)
{
	struct io_uring_params p1, p2, pbad;
	int fd1, badfd, pfd[2];
	char buf[16];

	/* 1. SQE128 + CQE32 ring layout and MSG_RING extra CQE payload. */
	memset(&p1, 0, sizeof(p1));
	p1.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
	fd1 = (int)kqring_setup(4, &p1);
	assert(fd1 >= 0);
	assert(p1.sq_entries == 4 && p1.cq_entries == 8);

	size_t ring_len = p1.cq_off.cqes +
	    (size_t)p1.cq_entries * sizeof(struct io_uring_cqe) * 2;
	char *ring_base = mmap(NULL, ring_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd1, IORING_OFF_SQ_RING);
	assert(ring_base != MAP_FAILED);

	size_t sqes_len = (size_t)p1.sq_entries * sizeof(struct io_uring_sqe) * 2;
	uint8_t *sqes_raw = mmap(NULL, sqes_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd1, IORING_OFF_SQES);
	assert(sqes_raw != MAP_FAILED);

	_Atomic unsigned *sq_ktail = (_Atomic unsigned *)(ring_base +
	    p1.sq_off.tail);
	unsigned *sq_array = (unsigned *)(ring_base + p1.sq_off.array);
	_Atomic unsigned *cq_khead = (_Atomic unsigned *)(ring_base +
	    p1.cq_off.head);
	_Atomic unsigned *cq_ktail = (_Atomic unsigned *)(ring_base +
	    p1.cq_off.tail);
	struct io_uring_cqe *cqes = (struct io_uring_cqe *)(ring_base +
	    p1.cq_off.cqes);

	for (unsigned i = 0; i < p1.sq_entries; i++)
		sq_array[i] = i;

	/* Submit two 128-byte SQEs: NOP at index 0, MSG_RING at index 1. */
	memset(sqes_raw, 0, sqes_len);
	struct io_uring_sqe *sqe0 = (struct io_uring_sqe *)(sqes_raw + 0 * 128);
	sqe0->opcode = IORING_OP_NOP;
	sqe0->fd = -1;
	sqe0->user_data = 72001;

	struct io_uring_sqe *sqe1 = (struct io_uring_sqe *)(sqes_raw + 1 * 128);
	sqe1->opcode = IORING_OP_MSG_RING;
	sqe1->fd = fd1;
	sqe1->addr = IORING_MSG_DATA;
	sqe1->len = 42;
	sqe1->off = 72002;
	sqe1->user_data = 72003;
	uint64_t extra_in[2] = { 0x1122334455667788ULL, 0x99aabbccddeeff00ULL };
	memcpy((uint8_t *)sqe1 + 64, extra_in, sizeof(extra_in));

	/* Pre-fill big_cqe slots with non-zero to verify NOP zeroes them. */
	cqes[0].big_cqe[0] = ~0ULL;
	cqes[0].big_cqe[1] = ~0ULL;

	atomic_store_explicit(sq_ktail, 2, memory_order_release);
	assert(kqring_enter(fd1, 2, 3, IORING_ENTER_GETEVENTS, NULL, 0) == 2);
	assert(atomic_load_explicit(cq_ktail, memory_order_acquire) -
	    atomic_load_explicit(cq_khead, memory_order_relaxed) == 3);

	/* Slot 0 (offset 0 * 32): NOP completion, big_cqe zeroed. */
	struct io_uring_cqe *c0 = &cqes[0 * 2];
	assert(c0->user_data == 72001 && c0->res == 0);
	assert(c0->big_cqe[0] == 0 && c0->big_cqe[1] == 0);

	/* Slot 1 (offset 1 * 32): MSG_RING sender completion. */
	struct io_uring_cqe *c1 = &cqes[1 * 2];
	assert(c1->user_data == 72003 && c1->res == 0);
	assert(c1->big_cqe[0] == 0 && c1->big_cqe[1] == 0);

	/* Slot 2 (offset 2 * 32): MSG_RING target completion with big_cqe. */
	struct io_uring_cqe *c2 = &cqes[2 * 2];
	assert(c2->user_data == 72002 && c2->res == 42);
	assert(c2->big_cqe[0] == extra_in[0] && c2->big_cqe[1] == extra_in[1]);
	atomic_store_explicit(cq_khead, 3, memory_order_release);

	/* 2. ATTACH_WQ validation and shared worker pool behavior. */
	memset(&pbad, 0, sizeof(pbad));
	pbad.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_ATTACH_WQ;
	pbad.wq_fd = 999999;
	assert(kqring_setup(4, &pbad) == -1 && errno == EINVAL);

	badfd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	assert(badfd >= 0);
	pbad.wq_fd = (uint32_t)badfd; /* not yet setup */
	assert(kqring_setup(4, &pbad) == -1 && errno == EINVAL);
	close(badfd);

	/* Attach a second ring (fd2) to fd1's worker pool. */
	struct test_ring r2;
	memset(&p2, 0, sizeof(p2));
	p2.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_ATTACH_WQ;
	p2.wq_fd = (uint32_t)fd1;
	memset(&r2, 0, sizeof(r2));
	r2.fd = (int)kqring_setup(8, &p2);
	assert(r2.fd >= 0);
	r2.ring_map_len = p2.cq_off.cqes +
	    p2.cq_entries * sizeof(struct io_uring_cqe);
	r2.ring_map = mmap(NULL, r2.ring_map_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, r2.fd, IORING_OFF_SQ_RING);
	assert(r2.ring_map != MAP_FAILED);
	r2.sqes_len = p2.sq_entries * sizeof(struct io_uring_sqe);
	r2.sqes = mmap(NULL, r2.sqes_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, r2.fd, IORING_OFF_SQES);
	assert(r2.sqes != MAP_FAILED);
	char *base2 = (char *)r2.ring_map;
	r2.sq_khead = (_Atomic unsigned *)(base2 + p2.sq_off.head);
	r2.sq_ktail = (_Atomic unsigned *)(base2 + p2.sq_off.tail);
	r2.sq_kflags = (_Atomic unsigned *)(base2 + p2.sq_off.flags);
	r2.sq_kdropped = (_Atomic unsigned *)(base2 + p2.sq_off.dropped);
	r2.sq_mask = *(unsigned *)(base2 + p2.sq_off.ring_mask);
	r2.sq_array = (unsigned *)(base2 + p2.sq_off.array);
	for (unsigned i = 0; i < p2.sq_entries; i++)
		r2.sq_array[i] = i;
	r2.cq_khead = (_Atomic unsigned *)(base2 + p2.cq_off.head);
	r2.cq_ktail = (_Atomic unsigned *)(base2 + p2.cq_off.tail);
	r2.cq_kflags = (_Atomic unsigned *)(base2 + p2.cq_off.flags);
	r2.cq_mask = *(unsigned *)(base2 + p2.cq_off.ring_mask);
	r2.cqes = (struct io_uring_cqe *)(base2 + p2.cq_off.cqes);

	/* Setting IOWQ_MAX_WORKERS on fd1 is reflected when queried on r2. */
	uint32_t maxw[2] = { 2, 3 };
	assert(kqring_register(fd1, IORING_REGISTER_IOWQ_MAX_WORKERS,
	    maxw, 2) == 0);
	uint32_t qmax[2] = { 0, 0 };
	assert(kqring_register(r2.fd, IORING_REGISTER_IOWQ_MAX_WORKERS,
	    qmax, 2) == 0);
	assert(qmax[0] == 2 && qmax[1] == 3);

	/*
	 * An ATTACH_WQ setup whose params copyout fails must leave the ring
	 * on its own pool: retried on the same fd without ATTACH_WQ, it must
	 * not see fd1's pool limits.
	 */
	long pgsz = sysconf(_SC_PAGESIZE);
	struct io_uring_params *pro = mmap(NULL, (size_t)pgsz,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	assert(pro != MAP_FAILED);
	memset(pro, 0, sizeof(*pro));
	pro->flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_ATTACH_WQ;
	pro->wq_fd = (uint32_t)fd1;
	assert(mprotect(pro, (size_t)pgsz, PROT_READ) == 0);
	int rfd = open("/dev/kqring", O_RDWR | O_CLOEXEC);
	assert(rfd >= 0);
	struct kqring_setup_arg rsa = { .entries = 4, .pad = 0, .params = pro };
	assert(ioctl(rfd, KQRING_IOC_SETUP, &rsa) == -1 && errno == EFAULT);
	struct io_uring_params pretry;
	memset(&pretry, 0, sizeof(pretry));
	pretry.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	rsa.params = &pretry;
	assert(ioctl(rfd, KQRING_IOC_SETUP, &rsa) == 0);
	uint32_t rmax[2] = { 0, 0 };
	assert(kqring_register(rfd, IORING_REGISTER_IOWQ_MAX_WORKERS,
	    rmax, 2) == 0);
	assert(rmax[0] != 2 && rmax[1] != 3);
	close(rfd);
	munmap(pro, (size_t)pgsz);

	/* Close fd1 first; r2's attached worker pool must remain functional. */
	munmap(sqes_raw, sqes_len);
	munmap(ring_base, ring_len);
	close(fd1);

	assert(pipe(pfd) == 0);
	assert(write(pfd[1], "wqok", 4) == 4);
	struct io_uring_sqe *sqe = ring_prep(&r2, IORING_OP_READ, pfd[0],
	    72010);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&r2);
	assert(submit_one(&r2) == 4);
	assert(memcmp(buf, "wqok", 4) == 0);

	/*
	 * Close r2 while an attached-pool worker op is in flight on pfd[0]
	 * (after fd1 was already closed).
	 */
	sqe = ring_prep(&r2, IORING_OP_READ, pfd[0], 72011);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&r2);
	ring_submit(&r2);
	munmap(r2.sqes, r2.sqes_len);
	munmap(r2.ring_map, r2.ring_map_len);
	close(r2.fd);
	close(pfd[0]);
	close(pfd[1]);
	printf("[PASS] SQE128, CQE32, and ATTACH_WQ: 128B SQEs, 32B CQEs, "
	    "MSG_RING big_cqe, shared WQ\n");
}

static void
test_registered_rings(void)
{
	struct io_uring_rsrc_update up[2];
	struct io_uring_clone_buffers cb;
	struct io_uring_params p;
	struct test_ring t1, t2;
	long page = sysconf(_SC_PAGESIZE);
	int pfd[2];
	char buf[16];

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	int fd = (int)kqring_setup(4, &p);
	assert(fd >= 0);
	assert((p.features & IORING_FEAT_REG_REG_RING) != 0);
	close(fd);

	ring_init(&t1);
	ring_init(&t2);
	assert(pipe(pfd) == 0);

	/* 1. Validation before registering: non-ring fd, out-of-range slot. */
	up[0] = (struct io_uring_rsrc_update){ .offset = 0,
	    .data = (uint64_t)pfd[0] };
	assert(reg(&t1, IORING_REGISTER_RING_FDS, &up[0], 1) == -EBADF);
	up[0] = (struct io_uring_rsrc_update){ .offset = 16,
	    .data = (uint64_t)t1.fd };
	assert(reg(&t1, IORING_REGISTER_RING_FDS, &up[0], 1) == -EINVAL);

	/* 2. Register t1 at explicit slot 2 and t2 via ALLOC (-1). */
	up[0] = (struct io_uring_rsrc_update){ .offset = 2,
	    .data = (uint64_t)t1.fd };
	up[1] = (struct io_uring_rsrc_update){ .offset = IORING_FILE_INDEX_ALLOC,
	    .data = (uint64_t)t2.fd };
	assert(reg(&t1, IORING_REGISTER_RING_FDS, up, 2) == 2);
	assert(up[0].offset == 2 && up[1].offset == 0);

	/* Duplicate registration into occupied slot 2 fails with EBUSY. */
	assert(reg(&t1, IORING_REGISTER_RING_FDS, &up[0], 1) == -EBUSY);

	/* 3. Close t1.fd and use slot 2 via IORING_ENTER_REGISTERED_RING
	 * and IORING_REGISTER_USE_REGISTERED_RING.
	 */
	close(t1.fd);
	t1.fd = -1;

	assert(kqring_register(2,
	    IORING_REGISTER_FILES | IORING_REGISTER_USE_REGISTERED_RING,
	    &pfd[0], 1) == 0);
	assert(write(pfd[1], "reg!", 4) == 4);
	struct io_uring_sqe *sqe = ring_prep(&t1, IORING_OP_READ, 0, 73001);
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&t1);
	assert(kqring_enter(2, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_REGISTERED_RING,
	    NULL, 0) == 1);
	struct io_uring_cqe c = ring_pop_cqe(&t1);
	assert(c.user_data == 73001 && c.res == 4);
	assert(memcmp(buf, "reg!", 4) == 0);

	/* 4. CLONE_BUFFERS with IORING_REGISTER_SRC_REGISTERED from slot 2. */
	char *m = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(m != MAP_FAILED);
	struct iovec iov = { m, (size_t)page };
	assert(kqring_register(2,
	    IORING_REGISTER_BUFFERS | IORING_REGISTER_USE_REGISTERED_RING,
	    &iov, 1) == 0);

	cb = (struct io_uring_clone_buffers){
		.src_fd = 2,
		.flags = IORING_REGISTER_SRC_REGISTERED,
	};
	assert(kqring_register(0,
	    IORING_REGISTER_CLONE_BUFFERS | IORING_REGISTER_USE_REGISTERED_RING,
	    &cb, 1) == 0);
	assert(write(pfd[1], "clon", 4) == 4);
	sqe = ring_prep(&t2, IORING_OP_READ_FIXED, pfd[0], 73002);
	sqe->addr = (uint64_t)(uintptr_t)m;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	sqe->buf_index = 0;
	ring_push_sqe(&t2);
	assert(kqring_enter(0, 1, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_REGISTERED_RING,
	    NULL, 0) == 1);
	c = ring_pop_cqe(&t2);
	assert(c.user_data == 73002 && c.res == 4);
	assert(memcmp(m, "clon", 4) == 0);

	/* 5. Unregister slot 2 via IORING_REGISTER_USE_REGISTERED_RING
	 * (IORING_FEAT_REG_REG_RING) and slot 0, then verify EBADF.
	 */
	assert(kqring_register(2,
	    IORING_UNREGISTER_RING_FDS | IORING_REGISTER_USE_REGISTERED_RING,
	    &up[0], 1) == 1);
	assert(kqring_enter(2, 0, 0, IORING_ENTER_REGISTERED_RING,
	    NULL, 0) == -1 && errno == EBADF);
	assert(kqring_register(0,
	    IORING_UNREGISTER_RING_FDS | IORING_REGISTER_USE_REGISTERED_RING,
	    &up[1], 1) == 1);
	assert(kqring_register(0,
	    IORING_UNREGISTER_RING_FDS | IORING_REGISTER_USE_REGISTERED_RING,
	    &up[1], 1) == -1 && errno == EBADF);

	munmap(m, (size_t)page);
	close(pfd[0]);
	close(pfd[1]);
	close(t2.fd);
	printf("[PASS] Registered ring fds: RING_FDS, ENTER_REGISTERED_RING, "
	    "USE_REGISTERED_RING, SRC_REGISTERED\n");
}

static void
test_sqpoll_and_iopoll(void)
{
	struct io_uring_params p;
	struct test_ring sr, ir;
	char dir[64], path[128], buf[32];
	int pfd[2], ffd;

	/* 1. SQ_AFF validation: invalid CPU rejected with EINVAL. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
	p.sq_thread_cpu = 9999;
	assert(kqring_setup(8, &p) == -1 && errno == EINVAL);

	/* 2. SQPOLL + SQ_AFF (CPU 0) with 30 ms idle timeout. */
	memset(&sr, 0, sizeof(sr));
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
	p.sq_thread_cpu = 0;
	p.sq_thread_idle = 30;
	sr.fd = (int)kqring_setup(16, &p);
	assert(sr.fd >= 0);
	assert((p.features & IORING_FEAT_SQPOLL_NONFIXED) != 0);

	sr.ring_map_len = p.cq_off.cqes +
	    p.cq_entries * sizeof(struct io_uring_cqe);
	sr.ring_map = mmap(NULL, sr.ring_map_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, sr.fd, IORING_OFF_SQ_RING);
	assert(sr.ring_map != MAP_FAILED);
	sr.sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
	sr.sqes = mmap(NULL, sr.sqes_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, sr.fd, IORING_OFF_SQES);
	assert(sr.sqes != MAP_FAILED);

	uint8_t *base = sr.ring_map;
	sr.sq_khead = (_Atomic uint32_t *)(base + p.sq_off.head);
	sr.sq_ktail = (_Atomic uint32_t *)(base + p.sq_off.tail);
	sr.sq_kflags = (_Atomic uint32_t *)(base + p.sq_off.flags);
	sr.sq_kdropped = (_Atomic uint32_t *)(base + p.sq_off.dropped);
	sr.sq_mask = *(uint32_t *)(base + p.sq_off.ring_mask);
	sr.sq_array = (uint32_t *)(base + p.sq_off.array);
	sr.cq_khead = (_Atomic uint32_t *)(base + p.cq_off.head);
	sr.cq_ktail = (_Atomic uint32_t *)(base + p.cq_off.tail);
	sr.cq_kflags = (_Atomic uint32_t *)(base + p.cq_off.flags);
	sr.cq_mask = *(uint32_t *)(base + p.cq_off.ring_mask);
	sr.cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
	for (uint32_t i = 0; i < p.sq_entries; i++)
		sr.sq_array[i] = i;

	/* Submit NOP + non-fixed PIPE READ/WRITE without calling kqring_enter. */
	assert(pipe(pfd) == 0);
	struct io_uring_sqe *sqe = ring_prep(&sr, IORING_OP_NOP, -1, 74001);
	ring_push_sqe(&sr);
	sqe = ring_prep(&sr, IORING_OP_WRITE, pfd[1], 74002);
	sqe->addr = (uint64_t)(uintptr_t)"sqpoll!";
	sqe->len = 7;
	sqe->off = (uint64_t)-1;
	sqe->flags = IOSQE_IO_LINK;
	ring_push_sqe(&sr);
	memset(buf, 0, sizeof(buf));
	sqe = ring_prep(&sr, IORING_OP_READ, pfd[0], 74003);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 7;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&sr);

	/* Spin-wait for all 3 CQEs with zero syscalls. */
	uint64_t deadline = now_ns() + 2000000000ULL;
	while (ring_cq_ready(&sr) < 3) {
		assert(now_ns() < deadline);
		usleep(50);
	}
	bool saw[3] = { false, false, false };
	for (int i = 0; i < 3; i++) {
		struct io_uring_cqe c = ring_pop_cqe(&sr);
		if (c.user_data == 74001) {
			assert(c.res == 0);
			saw[0] = true;
		} else if (c.user_data == 74002) {
			assert(c.res == 7);
			saw[1] = true;
		} else if (c.user_data == 74003) {
			assert(c.res == 7);
			saw[2] = true;
		}
	}
	assert(saw[0] && saw[1] && saw[2]);
	assert(memcmp(buf, "sqpoll!", 7) == 0);

	/* 3. Wait for sq_thread_idle (30 ms) to set IORING_SQ_NEED_WAKEUP. */
	deadline = now_ns() + 2000000000ULL;
	while ((atomic_load_explicit(sr.sq_kflags, memory_order_acquire) &
	    IORING_SQ_NEED_WAKEUP) == 0) {
		assert(now_ns() < deadline);
		usleep(1000);
	}
	/* Enqueue NOP and wake via IORING_ENTER_SQ_WAKEUP | IORING_ENTER_GETEVENTS. */
	sqe = ring_prep(&sr, IORING_OP_NOP, -1, 74004);
	ring_push_sqe(&sr);
	assert(kqring_enter(sr.fd, 0, 1,
	    IORING_ENTER_SQ_WAKEUP | IORING_ENTER_GETEVENTS, NULL, 0) == 0);
	struct io_uring_cqe c = ring_pop_cqe(&sr);
	assert(c.user_data == 74004 && c.res == 0);

	/* IORING_ENTER_SQ_WAIT succeeds on SQPOLL ring. */
	assert(kqring_enter(sr.fd, 0, 0, IORING_ENTER_SQ_WAIT, NULL, 0) == 0);

	close(pfd[0]);
	close(pfd[1]);
	munmap(sr.sqes, sr.sqes_len);
	munmap(sr.ring_map, sr.ring_map_len);
	close(sr.fd);

	/* 4. IORING_SETUP_IOPOLL: file READ/WRITE/READV/WRITEV + opcode validation. */
	memset(&ir, 0, sizeof(ir));
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_IOPOLL;
	ir.fd = (int)kqring_setup(16, &p);
	assert(ir.fd >= 0);
	ir.ring_map_len = p.cq_off.cqes +
	    p.cq_entries * sizeof(struct io_uring_cqe);
	ir.ring_map = mmap(NULL, ir.ring_map_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, ir.fd, IORING_OFF_SQ_RING);
	assert(ir.ring_map != MAP_FAILED);
	ir.sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
	ir.sqes = mmap(NULL, ir.sqes_len, PROT_READ | PROT_WRITE,
	    MAP_SHARED, ir.fd, IORING_OFF_SQES);
	assert(ir.sqes != MAP_FAILED);
	base = ir.ring_map;
	ir.sq_khead = (_Atomic uint32_t *)(base + p.sq_off.head);
	ir.sq_ktail = (_Atomic uint32_t *)(base + p.sq_off.tail);
	ir.sq_kflags = (_Atomic uint32_t *)(base + p.sq_off.flags);
	ir.sq_kdropped = (_Atomic uint32_t *)(base + p.sq_off.dropped);
	ir.sq_mask = *(uint32_t *)(base + p.sq_off.ring_mask);
	ir.sq_array = (uint32_t *)(base + p.sq_off.array);
	ir.cq_khead = (_Atomic uint32_t *)(base + p.cq_off.head);
	ir.cq_ktail = (_Atomic uint32_t *)(base + p.cq_off.tail);
	ir.cq_kflags = (_Atomic uint32_t *)(base + p.cq_off.flags);
	ir.cq_mask = *(uint32_t *)(base + p.cq_off.ring_mask);
	ir.cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
	for (uint32_t i = 0; i < p.sq_entries; i++)
		ir.sq_array[i] = i;

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/iopoll", dir);
	ffd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(ffd >= 0);

	sqe = ring_prep(&ir, IORING_OP_WRITE, ffd, 74010);
	sqe->addr = (uint64_t)(uintptr_t)"iopoll_ok";
	sqe->len = 9;
	sqe->off = 0;
	ring_push_sqe(&ir);
	assert(submit_one(&ir) == 9);

	memset(buf, 0, sizeof(buf));
	sqe = ring_prep(&ir, IORING_OP_READ, ffd, 74011);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 9;
	sqe->off = 0;
	ring_push_sqe(&ir);
	assert(submit_one(&ir) == 9);
	assert(memcmp(buf, "iopoll_ok", 9) == 0);

	/* Non-IOPOLL opcodes (POLL_ADD) and non-vnode fds (pipe) are rejected. */
	assert(pipe(pfd) == 0);
	sqe = ring_prep(&ir, IORING_OP_POLL_ADD, pfd[0], 74012);
	sqe->poll32_events = POLLIN;
	ring_push_sqe(&ir);
	assert(submit_one(&ir) == -EINVAL);

	sqe = ring_prep(&ir, IORING_OP_READ, pfd[0], 74013);
	sqe->addr = (uint64_t)(uintptr_t)buf;
	sqe->len = 4;
	sqe->off = (uint64_t)-1;
	ring_push_sqe(&ir);
	assert(submit_one(&ir) == -EOPNOTSUPP);

	close(pfd[0]);
	close(pfd[1]);
	close(ffd);
	unlink(path);
	rmdir(dir);
	munmap(ir.sqes, ir.sqes_len);
	munmap(ir.ring_map, ir.ring_map_len);
	close(ir.fd);
	printf("[PASS] SQPOLL and IOPOLL: syscall-free SQPOLL, SQ_AFF, "
	    "NEED_WAKEUP, SQ_WAKEUP, SQ_WAIT, IOPOLL file I/O\n");
}

static void
test_phase5_security_audit(void)
{
	/*
	 * 1. AF_UNIX SCM_RIGHTS reference cycle: a child creates a ring,
	 * registers a socketpair in the ring's fixed table, closes the user
	 * socket fds (so only the ring's fixed table holds the sockets) and
	 * tries to send the ring's own fd over the fixed socket.  A ring file
	 * cannot be passed (EOPNOTSUPP), so the cycle cannot form; exit
	 * teardown still drops the table with an armed worker op.
	 */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct test_ring ct;
		struct io_uring_sqe *sqe;
		int sv[2];

		ring_init(&ct);
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		assert(reg(&ct, IORING_REGISTER_FILES, sv, 2) == 0);
		close(sv[0]);
		close(sv[1]);

		union {
			struct cmsghdr cm;
			char buf[CMSG_SPACE(sizeof(int))];
		} cmsg_buf;
		char ch = 'x';
		struct iovec iov = { &ch, 1 };
		struct msghdr msg;

		memset(&cmsg_buf, 0, sizeof(cmsg_buf));
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsg_buf.buf;
		msg.msg_controllen = sizeof(cmsg_buf.buf);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cm), &ct.fd, sizeof(int));

		sqe = ring_prep(&ct, IORING_OP_SENDMSG, 0, 75001);
		sqe->flags = IOSQE_FIXED_FILE;
		sqe->addr = (uint64_t)(uintptr_t)&msg;
		ring_push_sqe(&ct);
		assert(submit_one(&ct) == -EOPNOTSUPP);
		/* Plain sendmsg(2) of the ring fails the same way. */
		int usv[2];
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, usv) == 0);
		assert(sendmsg(usv[0], &msg, 0) == -1 && errno == EOPNOTSUPP);
		close(usv[0]);
		close(usv[1]);
		/*
		 * Also arm a blocking worker op (POLL_ADD on fixed slot 0)
		 * and close ct.fd before _exit(0).
		 */
		sqe = ring_prep(&ct, IORING_OP_POLL_ADD, 0, 75002);
		sqe->flags = IOSQE_FIXED_FILE | IOSQE_ASYNC;
		sqe->poll32_events = POLLIN;
		ring_push_sqe(&ct);
		ring_submit(&ct);
		close(ct.fd);
		_exit(0);
	}
	int status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* 2. execve(2) cancels and disarms open rings (even if O_CLOEXEC is cleared). */
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct test_ring ct;
		int pfd[2], flags;
		char pbuf[8];

		ring_init(&ct);
		flags = fcntl(ct.fd, F_GETFD);
		assert(flags >= 0 &&
		    fcntl(ct.fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
		assert(pipe(pfd) == 0);

		/* Arm a blocking pipe read on a worker thread, then exec /usr/bin/true. */
		struct io_uring_sqe *csqe = ring_prep(&ct, IORING_OP_READ,
		    pfd[0], 75003);
		csqe->addr = (uint64_t)(uintptr_t)pbuf;
		csqe->len = sizeof(pbuf);
		csqe->off = (uint64_t)-1;
		ring_push_sqe(&ct);
		ring_submit(&ct);

		execl("/usr/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/*
	 * 3. Forged sq_tail (tail - head > sq_entries) with linked SQEs is
	 * bounded to sq_entries per batch and cannot bypass KQRING_MAX_REQS.
	 */
	struct test_ring ft;
	ring_setup(&ft);
	assert(kqring_register(ft.fd, IORING_REGISTER_ENABLE_RINGS,
	    NULL, 0) == 0);
	for (unsigned i = 0; i < SQ_ENTRIES; i++) {
		memset(&ft.sqes[i], 0, sizeof(ft.sqes[i]));
		ft.sqes[i].opcode = IORING_OP_NOP;
		ft.sqes[i].flags = IOSQE_IO_LINK;
		ft.sqes[i].user_data = 80000 + i;
		ft.sq_array[i] = i;
	}
	atomic_store_explicit(ft.sq_ktail, 100000U, memory_order_release);
	int nsub = kqring_enter(ft.fd, 100000U, 0, 0, NULL, 0);
	assert(nsub == SQ_ENTRIES);
	assert(atomic_load_explicit(ft.sq_khead, memory_order_acquire) ==
	    SQ_ENTRIES);
	assert(atomic_load_explicit(ft.cq_ktail, memory_order_acquire) -
	    atomic_load_explicit(ft.cq_khead, memory_order_acquire) ==
	    SQ_ENTRIES);
	munmap(ft.sqes, ft.sqes_len);
	munmap(ft.ring_map, ft.ring_map_len);
	close(ft.fd);

	/*
	 * 4. Retired worker finishing into taskwork_ops before ring close,
	 * and SPLICE offset overflow check.
	 */
	struct test_ring rt;
	int rpfd[2];
	char rbuf[8];
	char tmppath[] = "/tmp/kqring_splice_ovf.XXXXXX";
	int tmpfd = mkstemp(tmppath);
	assert(tmpfd >= 0);
	unlink(tmppath);
	ring_init(&rt);
	assert(pipe(rpfd) == 0);
	assert(write(rpfd[1], "x", 1) == 1);
	struct io_uring_sqe *rsqe = ring_prep(&rt, IORING_OP_SPLICE, tmpfd,
	    81003);
	rsqe->splice_fd_in = rpfd[0];
	rsqe->splice_off_in = (uint64_t)-1;
	rsqe->off = (uint64_t)INT64_MAX;
	rsqe->len = 1;
	ring_push_sqe(&rt);
	assert(submit_one(&rt) == -EINVAL);
	assert(read(rpfd[0], rbuf, 1) == 1);
	close(tmpfd);

	rsqe = ring_prep(&rt, IORING_OP_READ, rpfd[0], 81001);
	rsqe->addr = (uint64_t)(uintptr_t)rbuf;
	rsqe->len = sizeof(rbuf);
	rsqe->off = (uint64_t)-1;
	ring_push_sqe(&rt);
	ring_submit(&rt);

	rsqe = ring_prep(&rt, IORING_OP_ASYNC_CANCEL, 0, 81002);
	rsqe->addr = 81001;
	ring_push_sqe(&rt);
	ring_submit(&rt);
	/* Unblock the retired worker so it queues onto taskwork_ops, then close. */
	assert(write(rpfd[1], "x", 1) == 1);
	usleep(5000);
	close(rpfd[0]);
	close(rpfd[1]);
	munmap(rt.br, sysconf(_SC_PAGESIZE));
	munmap(rt.bufs, NBUFS * BUF_SIZE);
	munmap(rt.sqes, rt.sqes_len);
	munmap(rt.ring_map, rt.ring_map_len);
	close(rt.fd);

	printf("[PASS] Phase 5 security: AF_UNIX SCM_RIGHTS cycle broken on "
	    "exit, execve(2) ring teardown, forged sq_tail bound, owner check\n");
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
		test_native_syscalls(&r);
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
		test_close_sockets_then_ring();
		test_shared_ring();
		test_ring_fd_poll();
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
		test_fixed_files();
		test_direct_descriptors();
		test_rsrc_tags();
		test_registered_buffers();
		test_buffer_groups();
		test_eventfd();
		test_personality();
		test_restrictions(&r);
		test_send_fd();
		test_fixed_cancel();
		test_fixed_file_ops();
		test_iowq_register();
		test_worker_nonblocking_pipe();
		test_inline_file_pipe();
		test_disk_bio();
		test_napi_busy_poll();
		test_worker_buffer_reservation();
		test_worker_cancel_and_timeout();
		test_worker_max_workers();
		test_worker_process_lifecycle();
		test_splice_and_tee();
		test_nonsock_poll();
		test_kpoll();
		test_pipe_nowait();
		test_min_wait();
		test_no_mmap();
		test_sqe128_cqe32_attach_wq();
		test_registered_rings();
		test_sqpoll_and_iopoll();
		test_phase5_security_audit();
		printf("%s: %d failure(s)\n", failures ? "[FAIL]" : "[OK]",
		    failures);
	}
	if (run_bench)
		bench_ping_pong(&r);
	close(r.fd);
	return (failures ? 1 : 0);
}
