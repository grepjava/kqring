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
 *   kqring_xbench [reps]	(default 5)
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
	struct utsname u;
	double v[64];

	CHECK(reps > 0 && reps <= 64);
	CHECK(uname(&u) == 0);
	printf("# %s %s %s\n", u.sysname, u.release, u.machine);
	ring_init(&R, 64);
	CHECK(pipe(pfd) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sfd) == 0);
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		(void)run(cases[c].op, 200000000ULL);
		for (int i = 0; i < reps; i++)
			v[i] = run(cases[c].op, 1000000000ULL);
		qsort(v, (size_t)reps, sizeof(v[0]), cmpd);
		printf("%-18s %8.1f ns  (%.1f-%.1f)  %s\n", cases[c].name,
		    v[reps / 2], v[0], v[reps - 1], cases[c].what);
		fflush(stdout);
	}
	return (0);
}
