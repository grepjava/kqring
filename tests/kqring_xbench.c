/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The ring's own costs, measured the same way on Linux io_uring and on
 * kqring, so the two can be compared on one machine.  Each case is also
 * run as the plain system call it replaces, which gives each OS's own
 * baseline: a ring's cost is best read against its own kernel's.
 *
 *   FreeBSD: make -C tests kqring_xbench
 *   Linux:   cc -O2 -pthread -o kqring_xbench tests/kqring_xbench.c
 *
 *   kqring_xbench [reps [case]]	(default 5 reps, every case)
 *
 * Naming a case runs only that one, which gives a profiler a steady loop.
 * The fan-in cases use XBENCH_FANIN_N connections (default 256).
 * XBENCH_DEFER=1 sets the rings up with SINGLE_ISSUER | DEFER_TASKRUN.
 * XBENCH_FANIN_OWNBUF=1 makes fanin_kevent read each connection into a
 * buffer of its own, as the ring's cases receive, rather than into one.
 *
 * Each case runs for about a second after a warm-up, reps times; the
 * output is one line per case with the median and the range, in
 * nanoseconds per operation.  Nothing is written to disk.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif
#include <sys/utsname.h>

#include <errno.h>
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
#define ring_sys_register(fd, op, arg, n) \
	((int)syscall(__NR_io_uring_register, (fd), (op), (arg), (n)))
#else
#include <sys/kqring.h>
#define ring_sys_setup(n, p)	((int)kqring_setup((n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)kqring_enter((fd), (s), (w), (f), (a), (z)))
#define ring_sys_register(fd, op, arg, n) \
	((int)kqring_register((fd), (op), (arg), (n)))
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
	_Atomic unsigned	*sq_head, *sq_tail, *cq_head, *cq_tail;
	struct io_uring_cqe	*cqes;
	unsigned		sq_mask, cq_mask;
	unsigned		tail;
};

static void
ring_init(struct ring *r, unsigned entries)
{
	struct io_uring_params p;
	size_t sq_len, cq_len, len;
	unsigned *array;
	char *b;

	memset(&p, 0, sizeof(p));
	memset(r, 0, sizeof(*r));
	/* XBENCH_DEFER=1: completions run in the enter, on this thread. */
	if (getenv("XBENCH_DEFER") != NULL &&
	    strcmp(getenv("XBENCH_DEFER"), "1") == 0)
		p.flags = IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN;
	/* XBENCH_COOP=1: no DEFER_TASKRUN, but COOP_TASKRUN. */
	else if (getenv("XBENCH_COOP") != NULL &&
	    strcmp(getenv("XBENCH_COOP"), "1") == 0)
		p.flags = IORING_SETUP_COOP_TASKRUN;
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
	r->sq_head = (_Atomic unsigned *)(b + p.sq_off.head);
	r->sq_tail = (_Atomic unsigned *)(b + p.sq_off.tail);
	r->sq_mask = *(unsigned *)(b + p.sq_off.ring_mask);
	r->cq_head = (_Atomic unsigned *)(b + p.cq_off.head);
	r->cq_tail = (_Atomic unsigned *)(b + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(b + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(b + p.cq_off.cqes);
	array = (unsigned *)(b + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		array[i] = i;
}

static struct io_uring_sqe *
prep(struct ring *r, uint8_t op, int fd)
{
	struct io_uring_sqe *s = &r->sqes[r->tail & r->sq_mask];

	memset(s, 0, sizeof(*s));
	s->opcode = op;
	s->fd = fd;
	r->tail++;
	return (s);
}

/* Submit what is queued and wait for n CQEs, then take them. */
static void
submit_wait(struct ring *r, unsigned n)
{
	unsigned head, tail;

	atomic_store_explicit(r->sq_tail, r->tail, memory_order_release);
	CHECK(ring_sys_enter(r->fd, n, n, IORING_ENTER_GETEVENTS, NULL, 0) >=
	    0);
	head = atomic_load_explicit(r->cq_head, memory_order_relaxed);
	tail = atomic_load_explicit(r->cq_tail, memory_order_acquire);
	CHECK(tail - head == n);
	for (unsigned i = head; i != tail; i++)
		CHECK(r->cqes[i & r->cq_mask].res >= 0);
	atomic_store_explicit(r->cq_head, tail, memory_order_release);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static struct ring R;
static int pfd[2], sfd[2];
static char buf[64];

/* One operation each; returns how many operations it did. */
static unsigned
op_getppid(void)
{
	(void)getppid();
	return (1);
}

static unsigned
op_nop1(void)
{
	prep(&R, IORING_OP_NOP, -1);
	submit_wait(&R, 1);
	return (1);
}

static unsigned
op_nop32(void)
{
	for (int i = 0; i < 32; i++)
		prep(&R, IORING_OP_NOP, -1);
	submit_wait(&R, 32);
	return (32);
}

static unsigned
op_pipe_read_syscall(void)
{
	CHECK(write(pfd[1], buf, sizeof(buf)) == sizeof(buf));
	CHECK(read(pfd[0], buf, sizeof(buf)) == sizeof(buf));
	return (1);
}

static unsigned
op_pipe_read_ring(void)
{
	struct io_uring_sqe *s;

	CHECK(write(pfd[1], buf, sizeof(buf)) == sizeof(buf));
	s = prep(&R, IORING_OP_READ, pfd[0]);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->off = (uint64_t)-1;
	submit_wait(&R, 1);
	return (1);
}

static unsigned
op_unix_recv_syscall(void)
{
	CHECK(write(sfd[1], buf, sizeof(buf)) == sizeof(buf));
	CHECK(recv(sfd[0], buf, sizeof(buf), 0) == sizeof(buf));
	return (1);
}

static unsigned
op_unix_recv_ring(void)
{
	struct io_uring_sqe *s;

	CHECK(write(sfd[1], buf, sizeof(buf)) == sizeof(buf));
	s = prep(&R, IORING_OP_RECV, sfd[0]);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	submit_wait(&R, 1);
	return (1);
}

/*
 * Fan-in: N connections each get one 64-byte message, and all N are
 * collected, as a busy server does.  Per operation = per message; each
 * includes its write(2).
 */
#define	FANIN_MAX	4096
#define	FANIN_BGID	7
static struct ring F;
static int fan_n, fan_rx[FANIN_MAX], fan_tx[FANIN_MAX], fan_poll = -1;
static char fan_buf[FANIN_MAX][64];
static struct io_uring_buf_ring *fan_br;
static unsigned fan_br_mask;
static uint16_t fan_br_tail;
static int fan_armed;

static void
fanin_setup(void)
{
	const char *e = getenv("XBENCH_FANIN_N");
	int sv[2];

	if (fan_n != 0)
		return;
	fan_n = e != NULL ? atoi(e) : 256;
	CHECK(fan_n > 0 && fan_n <= FANIN_MAX);
	for (int i = 0; i < fan_n; i++) {
		CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0,
		    sv) == 0);
		fan_rx[i] = sv[0];
		fan_tx[i] = sv[1];
	}
	ring_init(&F, (unsigned)fan_n);
}

static void
fanin_write(void)
{
	for (int i = 0; i < fan_n; i++)
		CHECK(write(fan_tx[i], buf, sizeof(buf)) == sizeof(buf));
}

/*
 * Take the n CQEs, each a whole message.  With recycle, give each one's
 * buffer back to the ring, and note a multishot RECV that stopped.
 */
static void
fanin_reap(struct ring *r, unsigned n, int recycle)
{
	unsigned head = atomic_load_explicit(r->cq_head, memory_order_relaxed);
	unsigned tail = atomic_load_explicit(r->cq_tail, memory_order_acquire);

	if (tail - head != n) {
		fprintf(stderr, "fan-in: %u CQEs, expected %u\n", tail - head,
		    n);
		for (unsigned i = head; i != tail && i - head < 40; i++)
			fprintf(stderr, "  user_data %llu res %d flags %#x\n",
			    (unsigned long long)r->cqes[i & r->cq_mask].user_data,
			    r->cqes[i & r->cq_mask].res,
			    r->cqes[i & r->cq_mask].flags);
		exit(2);
	}
	for (unsigned i = head; i != tail; i++) {
		struct io_uring_cqe *c = &r->cqes[i & r->cq_mask];

		CHECK(c->res == (int)sizeof(buf));
		if (recycle) {
			unsigned bid = c->flags >> IORING_CQE_BUFFER_SHIFT;
			struct io_uring_buf *b =
			    &fan_br->bufs[fan_br_tail & fan_br_mask];

			b->addr = (uint64_t)(uintptr_t)fan_buf[bid];
			b->len = sizeof(buf);
			b->bid = (uint16_t)bid;
			fan_br_tail++;
			if ((c->flags & IORING_CQE_F_MORE) == 0)
				fan_armed = 0;
		}
	}
	if (recycle)
		atomic_store_explicit((_Atomic uint16_t *)&fan_br->tail,
		    fan_br_tail, memory_order_release);
	atomic_store_explicit(r->cq_head, tail, memory_order_release);
}

/* The system calls: N write(2), one kevent(2) or epoll_wait(2), N recv(2). */
static unsigned
op_fanin_kevent(void)
{
	static int ownbuf = -1;
	int got = 0;

	if (ownbuf < 0)
		ownbuf = getenv("XBENCH_FANIN_OWNBUF") != NULL &&
		    strcmp(getenv("XBENCH_FANIN_OWNBUF"), "1") == 0;
	if (fan_poll < 0) {
		fanin_setup();
#if defined(__linux__)
		fan_poll = epoll_create1(0);
		CHECK(fan_poll >= 0);
		for (int i = 0; i < fan_n; i++) {
			struct epoll_event ev = { .events = EPOLLIN | EPOLLET,
			    .data.u64 = (uint64_t)(uint32_t)fan_rx[i] |
			    (uint64_t)i << 32 };

			CHECK(epoll_ctl(fan_poll, EPOLL_CTL_ADD, fan_rx[i],
			    &ev) == 0);
		}
#else
		struct kevent kev;

		fan_poll = kqueue();
		CHECK(fan_poll >= 0);
		for (int i = 0; i < fan_n; i++) {
			EV_SET(&kev, fan_rx[i], EVFILT_READ, EV_ADD | EV_CLEAR,
			    0, 0, (void *)(intptr_t)i);
			CHECK(kevent(fan_poll, &kev, 1, NULL, 0, NULL) == 0);
		}
#endif
	}
	fanin_write();
	while (got < fan_n) {
#if defined(__linux__)
		static struct epoll_event evs[FANIN_MAX];
		int m = epoll_wait(fan_poll, evs, fan_n, -1);

		CHECK(m > 0);
		for (int j = 0; j < m; j++)
			CHECK(recv((int)(uint32_t)evs[j].data.u64, ownbuf ?
			    fan_buf[evs[j].data.u64 >> 32] : buf, sizeof(buf),
			    0) == sizeof(buf));
#else
		static struct kevent evs[FANIN_MAX];
		int m = kevent(fan_poll, NULL, 0, evs, fan_n, NULL);

		CHECK(m > 0);
		for (int j = 0; j < m; j++)
			CHECK(recv((int)evs[j].ident, ownbuf ?
			    fan_buf[(intptr_t)evs[j].udata] : buf, sizeof(buf),
			    0) == sizeof(buf));
#endif
		got += m;
	}
	return ((unsigned)fan_n);
}

/* N single-shot RECVs, each into its own buffer, in one enter. */
static unsigned
op_fanin_ring_batch(void)
{
	fanin_setup();
	fanin_write();
	for (int i = 0; i < fan_n; i++) {
		struct io_uring_sqe *s = prep(&F, IORING_OP_RECV, fan_rx[i]);

		s->addr = (uint64_t)(uintptr_t)fan_buf[i];
		s->len = sizeof(buf);
	}
	atomic_store_explicit(F.sq_tail, F.tail, memory_order_release);
	CHECK(ring_sys_enter(F.fd, (unsigned)fan_n, (unsigned)fan_n,
	    IORING_ENTER_GETEVENTS, NULL, 0) == fan_n);
	fanin_reap(&F, (unsigned)fan_n, 0);
	return ((unsigned)fan_n);
}

/*
 * N multishot RECVs into a provided buffer ring, armed once; each round,
 * one enter waits for the N CQEs.
 */
static unsigned
op_fanin_ring_multishot(void)
{
	if (fan_br == NULL) {
		struct io_uring_buf_reg reg;
		unsigned entries = 1;

		fanin_setup();
		while (entries < (unsigned)fan_n * 2)
			entries <<= 1;
		fan_br = mmap(NULL, entries * sizeof(struct io_uring_buf),
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		CHECK(fan_br != MAP_FAILED);
		fan_br_mask = entries - 1;
		for (int i = 0; i < fan_n; i++) {
			fan_br->bufs[i].addr = (uint64_t)(uintptr_t)fan_buf[i];
			fan_br->bufs[i].len = sizeof(buf);
			fan_br->bufs[i].bid = (uint16_t)i;
		}
		fan_br_tail = (uint16_t)fan_n;
		atomic_store_explicit((_Atomic uint16_t *)&fan_br->tail,
		    fan_br_tail, memory_order_release);
		memset(&reg, 0, sizeof(reg));
		reg.ring_addr = (uint64_t)(uintptr_t)fan_br;
		reg.ring_entries = entries;
		reg.bgid = FANIN_BGID;
		CHECK(ring_sys_register(F.fd, IORING_REGISTER_PBUF_RING, &reg,
		    1) == 0);
	}
	if (!fan_armed) {
		for (int i = 0; i < fan_n; i++) {
			struct io_uring_sqe *s = prep(&F, IORING_OP_RECV,
			    fan_rx[i]);

			s->user_data = (uint64_t)i;
			s->flags = IOSQE_BUFFER_SELECT;
			s->buf_group = FANIN_BGID;
			s->ioprio = IORING_RECV_MULTISHOT;
		}
		atomic_store_explicit(F.sq_tail, F.tail, memory_order_release);
		CHECK(ring_sys_enter(F.fd, (unsigned)fan_n, 0, 0, NULL, 0) ==
		    fan_n);
		fan_armed = 1;
	}
	fanin_write();
	CHECK(ring_sys_enter(F.fd, 0, (unsigned)fan_n, IORING_ENTER_GETEVENTS,
	    NULL, 0) >= 0);
	fanin_reap(&F, (unsigned)fan_n, 1);
	return ((unsigned)fan_n);
}

/*
 * Ping-pong on one AF_UNIX connection, the way a server waits for a
 * message: readiness first, then the read.  Per operation = one 64-byte
 * message written and received.
 */
static int pp_poll = -1;

static unsigned
op_pp_kevent(void)
{
	if (pp_poll < 0) {
#if defined(__linux__)
		struct epoll_event ev = { .events = EPOLLIN | EPOLLET,
		    .data.fd = sfd[0] };

		pp_poll = epoll_create1(0);
		CHECK(pp_poll >= 0);
		CHECK(epoll_ctl(pp_poll, EPOLL_CTL_ADD, sfd[0], &ev) == 0);
#else
		struct kevent kev;

		pp_poll = kqueue();
		CHECK(pp_poll >= 0);
		EV_SET(&kev, sfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
		CHECK(kevent(pp_poll, &kev, 1, NULL, 0, NULL) == 0);
#endif
	}
	CHECK(write(sfd[1], buf, sizeof(buf)) == sizeof(buf));
#if defined(__linux__)
	struct epoll_event ev;

	CHECK(epoll_wait(pp_poll, &ev, 1, -1) == 1);
#else
	struct kevent ev;

	CHECK(kevent(pp_poll, NULL, 0, &ev, 1, NULL) == 1);
#endif
	CHECK(recv(sfd[0], buf, sizeof(buf), 0) == sizeof(buf));
	return (1);
}

/*
 * A RECV submitted before the data, which arms the socket's upcall; the
 * write completes it, and a second enter waits for the CQE.
 */
static unsigned
op_pp_ring_armed(void)
{
	struct io_uring_sqe *s;

	s = prep(&R, IORING_OP_RECV, sfd[0]);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	atomic_store_explicit(R.sq_tail, R.tail, memory_order_release);
	CHECK(ring_sys_enter(R.fd, 1, 0, 0, NULL, 0) == 1);
	CHECK(write(sfd[1], buf, sizeof(buf)) == sizeof(buf));
	CHECK(ring_sys_enter(R.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) >=
	    0);
	fanin_reap(&R, 1, 0);
	return (1);
}

/*
 * Ping-pong with a peer process, which echoes each 64 bytes back: the
 * waiter is asleep when the reply comes, as a server's usually is.
 */
static int xfd[2] = { -1, -1 }, xp_poll = -1;

static void
xp_peer(void)
{
	pid_t pid;

	if (xfd[0] >= 0)
		return;
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, xfd) == 0);
	CHECK((pid = fork()) >= 0);
	if (pid == 0) {
		char b[64];
		ssize_t n;

		close(xfd[0]);
		while ((n = read(xfd[1], b, sizeof(b))) > 0)
			if (write(xfd[1], b, (size_t)n) != n)
				_exit(1);
		_exit(0);
	}
	close(xfd[1]);
}

static unsigned
op_xp_kevent(void)
{
	xp_peer();
	if (xp_poll < 0) {
#if defined(__linux__)
		struct epoll_event ev = { .events = EPOLLIN | EPOLLET,
		    .data.fd = xfd[0] };

		xp_poll = epoll_create1(0);
		CHECK(xp_poll >= 0);
		CHECK(epoll_ctl(xp_poll, EPOLL_CTL_ADD, xfd[0], &ev) == 0);
#else
		struct kevent kev;

		xp_poll = kqueue();
		CHECK(xp_poll >= 0);
		EV_SET(&kev, xfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
		CHECK(kevent(xp_poll, &kev, 1, NULL, 0, NULL) == 0);
#endif
	}
	CHECK(write(xfd[0], buf, sizeof(buf)) == sizeof(buf));
#if defined(__linux__)
	struct epoll_event ev;

	CHECK(epoll_wait(xp_poll, &ev, 1, -1) == 1);
#else
	struct kevent ev;

	CHECK(kevent(xp_poll, NULL, 0, &ev, 1, NULL) == 1);
#endif
	CHECK(recv(xfd[0], buf, sizeof(buf), MSG_WAITALL) == sizeof(buf));
	return (1);
}

static unsigned
op_xp_ring_armed(void)
{
	struct io_uring_sqe *s;

	xp_peer();
	s = prep(&R, IORING_OP_RECV, xfd[0]);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->msg_flags = MSG_WAITALL;
	atomic_store_explicit(R.sq_tail, R.tail, memory_order_release);
	CHECK(ring_sys_enter(R.fd, 1, 0, 0, NULL, 0) == 1);
	CHECK(write(xfd[0], buf, sizeof(buf)) == sizeof(buf));
	CHECK(ring_sys_enter(R.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) >=
	    0);
	fanin_reap(&R, 1, 0);
	return (1);
}

static const struct bcase {
	const char	*name;
	unsigned	(*op)(void);
	const char	*what;
} cases[] = {
	{ "getppid", op_getppid, "getppid(2): the system call floor" },
	{ "nop1", op_nop1, "1 NOP per enter, waited for" },
	{ "nop32", op_nop32, "32 NOPs per enter, waited for, per NOP" },
	{ "pipe_read_syscall", op_pipe_read_syscall,
	    "write(2) 64 B then read(2) it from a pipe" },
	{ "pipe_read_ring", op_pipe_read_ring,
	    "write(2) 64 B then READ it from a pipe" },
	{ "unix_recv_syscall", op_unix_recv_syscall,
	    "write(2) 64 B then recv(2) it, AF_UNIX stream" },
	{ "unix_recv_ring", op_unix_recv_ring,
	    "write(2) 64 B then RECV it, AF_UNIX stream" },
	{ "pp_kevent", op_pp_kevent,
	    "write(2) 64 B, kevent/epoll_wait, recv(2), AF_UNIX stream" },
	{ "pp_ring_armed", op_pp_ring_armed,
	    "RECV armed first, write(2) 64 B, enter to wait, AF_UNIX stream" },
	{ "xp_kevent", op_xp_kevent,
	    "peer process echoes 64 B: write(2), kevent/epoll_wait, recv(2)" },
	{ "xp_ring_armed", op_xp_ring_armed,
	    "peer process echoes 64 B: RECV armed, write(2), enter to wait" },
	{ "fanin_kevent", op_fanin_kevent,
	    "fan-in: N write(2), kevent/epoll_wait, N recv(2), per message" },
	{ "fanin_ring_batch", op_fanin_ring_batch,
	    "fan-in: N write(2), N RECVs in one enter, per message" },
	{ "fanin_ring_multishot", op_fanin_ring_multishot,
	    "fan-in: N write(2), one enter for N multishot CQEs, per message" },
};

static double
run(unsigned (*op)(void), uint64_t ns)
{
	uint64_t t0, t, n = 0;

	t0 = now_ns();
	do {
		for (int i = 0; i < 256; i++)
			n += op();
		t = now_ns();
	} while (t - t0 < ns);
	return ((double)(t - t0) / (double)n);
}

static int
cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x < y ? -1 : x > y);
}

int
main(int argc, char **argv)
{
	int reps = argc > 1 ? atoi(argv[1]) : 5;
	const char *only = argc > 2 ? argv[2] : NULL;
	int found = 0;
	struct utsname u;
	double v[64];

	CHECK(reps > 0 && reps <= 64);
	CHECK(uname(&u) == 0);
	printf("# %s %s %s\n", u.sysname, u.release, u.machine);
	ring_init(&R, 64);
	CHECK(pipe(pfd) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sfd) == 0);
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		if (only != NULL && strcmp(only, cases[c].name) != 0)
			continue;
		found = 1;
		(void)run(cases[c].op, 200000000ULL);
		for (int i = 0; i < reps; i++)
			v[i] = run(cases[c].op, 1000000000ULL);
		qsort(v, (size_t)reps, sizeof(v[0]), cmpd);
		printf("%-18s %8.1f ns  (%.1f-%.1f)  %s\n", cases[c].name,
		    v[reps / 2], v[0], v[reps - 1], cases[c].what);
		fflush(stdout);
	}
	if (!found) {
		fprintf(stderr, "no case %s\n", only);
		return (1);
	}
	return (0);
}
