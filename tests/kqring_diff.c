/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Differential conformance: the same cases on Linux io_uring and on kqring.
 * Build it on each, run it on each, and compare the two outputs with
 * tests/diff_compare.py, which fails on any difference that
 * tests/diff_expected.txt doesn't list.
 *
 * Each case prints the CQEs it got, in order, with errors by name and
 * descriptors masked, so that the output depends only on behaviour.
 *
 *   FreeBSD: make -C tests kqring_diff
 *   Linux:   cc -O2 -pthread -o kqring_diff tests/kqring_diff.c
 *
 * The test directory is the current directory, so run it on the filesystem
 * being compared. It needs no privileges beyond RLIMIT_MEMLOCK for a page.
 */

#define _GNU_SOURCE
#if defined(__FreeBSD__)
#define __BSD_VISIBLE 1
#endif

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#define ring_sys_setup(n, p)	((int)syscall(__NR_io_uring_setup, (n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)syscall(__NR_io_uring_enter, (fd), (s), (w), (f), (a), (z)))
#define ring_sys_register(fd, op, a, n) \
	((int)syscall(__NR_io_uring_register, (fd), (op), (a), (n)))
#else
#include <sys/eventfd.h>
#include <sys/kqring.h>
#define ring_sys_setup(n, p)	((int)kqring_setup((n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)kqring_enter((fd), (s), (w), (f), (a), (z)))
#define ring_sys_register(fd, op, a, n) \
	((int)kqring_register((fd), (op), (a), (n)))
#endif

#define	DIE(msg)	do {						\
	fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, (msg),	\
	    strerror(errno));						\
	exit(2);							\
} while (0)
#define	CHECK(x)	do { if (!(x)) DIE(#x); } while (0)

struct ring {
	int			fd;
	void			*map;
	size_t			map_len;
	struct io_uring_sqe	*sqes;
	size_t			sqes_len;
	_Atomic unsigned	*sq_head, *sq_tail, *cq_head, *cq_tail;
	_Atomic unsigned	*sq_flags;
	unsigned		*sq_array;
	struct io_uring_cqe	*cqes;
	unsigned		sq_mask, cq_mask, sq_entries, cq_entries;
	unsigned		tail;
};

static const char *current;
static const char *self;
static uint64_t res_fd_mask;	/* user_data bits whose res is a descriptor. */
static uint64_t res_poll_mask;	/* user_data bits whose res is poll events. */

/* Linux's ETIME is its own errno; kqring defines it as ETIMEDOUT. */
static const char *
errname(int e)
{
	static const struct { int e; const char *n; } t[] = {
		{ ETIME, "ETIME" }, { ECANCELED, "ECANCELED" },
		{ ENOENT, "ENOENT" }, { EBADF, "EBADF" }, { EINVAL, "EINVAL" },
		{ EFAULT, "EFAULT" }, { EEXIST, "EEXIST" },
		{ ENOBUFS, "ENOBUFS" }, { EALREADY, "EALREADY" },
		{ EAGAIN, "EAGAIN" }, { EBUSY, "EBUSY" },
		{ EOPNOTSUPP, "EOPNOTSUPP" }, { EPIPE, "EPIPE" },
		{ EINTR, "EINTR" }, { ENOMEM, "ENOMEM" }, { EACCES, "EACCES" },
		{ ENOTSOCK, "ENOTSOCK" }, { EOVERFLOW, "EOVERFLOW" },
		{ ENXIO, "ENXIO" },
#if defined(__linux__)
		{ EBADFD, "EBADFD" }, { ETIMEDOUT, "ETIMEDOUT" },
#endif
	};
	static char buf[16];

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i].e == e)
			return (t[i].n);
	snprintf(buf, sizeof(buf), "errno%d", e);
	return (buf);
}

static void
ring_init(struct ring *r, unsigned entries, struct io_uring_params *pp)
{
	struct io_uring_params p;
	size_t sq_len, cq_len;
	char *b;

	if (pp != NULL)
		p = *pp;
	else
		memset(&p, 0, sizeof(p));
	memset(r, 0, sizeof(*r));
	r->fd = ring_sys_setup(entries, &p);
	CHECK(r->fd >= 0);
	sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	cq_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	r->map_len = sq_len > cq_len ? sq_len : cq_len;
	r->map = mmap(NULL, r->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    r->fd, IORING_OFF_SQ_RING);
	CHECK(r->map != MAP_FAILED);
	r->sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
	r->sqes = mmap(NULL, r->sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    r->fd, IORING_OFF_SQES);
	CHECK(r->sqes != MAP_FAILED);
	b = r->map;
	r->sq_head = (_Atomic unsigned *)(b + p.sq_off.head);
	r->sq_tail = (_Atomic unsigned *)(b + p.sq_off.tail);
	r->sq_flags = (_Atomic unsigned *)(b + p.sq_off.flags);
	r->sq_array = (unsigned *)(b + p.sq_off.array);
	r->sq_mask = *(unsigned *)(b + p.sq_off.ring_mask);
	r->cq_head = (_Atomic unsigned *)(b + p.cq_off.head);
	r->cq_tail = (_Atomic unsigned *)(b + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(b + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(b + p.cq_off.cqes);
	r->sq_entries = p.sq_entries;
	r->cq_entries = p.cq_entries;
	for (unsigned i = 0; i < p.sq_entries; i++)
		r->sq_array[i] = i;
}

static void
ring_free(struct ring *r)
{
	munmap(r->sqes, r->sqes_len);
	munmap(r->map, r->map_len);
	close(r->fd);
}

static struct io_uring_sqe *
prep(struct ring *r, uint8_t op, int fd, uint64_t ud)
{
	struct io_uring_sqe *s = &r->sqes[r->tail & r->sq_mask];

	memset(s, 0, sizeof(*s));
	s->opcode = op;
	s->fd = fd;
	s->user_data = ud;
	r->tail++;
	atomic_store_explicit(r->sq_tail, r->tail, memory_order_release);
	return (s);
}

static unsigned
pending(struct ring *r)
{
	return (r->tail - atomic_load_explicit(r->sq_head,
	    memory_order_acquire));
}

static int
submit(struct ring *r)
{
	return (ring_sys_enter(r->fd, pending(r), 0, 0, NULL, 0));
}

static unsigned
ready(struct ring *r)
{
	return (atomic_load_explicit(r->cq_tail, memory_order_acquire) -
	    atomic_load_explicit(r->cq_head, memory_order_relaxed));
}

static void
pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Wait for n CQEs, at most ms, then let any that belong with them arrive. */
static void
await(struct ring *r, unsigned n, int ms)
{
	for (int waited = 0; ready(r) < n && waited < ms; waited += 2) {
		(void)ring_sys_enter(r->fd, 0, 0, IORING_ENTER_GETEVENTS, NULL,
		    0);
		if (ready(r) < n)
			pause_ms(2);
	}
	pause_ms(20);
	(void)ring_sys_enter(r->fd, 0, 0, IORING_ENTER_GETEVENTS, NULL, 0);
}

static void
flags_str(unsigned f, char *b, size_t n)
{
	snprintf(b, n, "%s%s%s", (f & IORING_CQE_F_BUFFER) != 0 ? "B" : "",
	    (f & IORING_CQE_F_MORE) != 0 ? "M" : "",
	    (f & IORING_CQE_F_NOTIF) != 0 ? "N" : "");
	if (b[0] == 0)
		snprintf(b, n, "-");
}

static void
print_res(uint64_t ud, int res, char *b, size_t n)
{
	if (res < 0)
		snprintf(b, n, "-%s", errname(-res));
	else if (ud < 64 && (res_fd_mask & (1ULL << ud)) != 0)
		snprintf(b, n, "fd");
	else if (ud < 64 && (res_poll_mask & (1ULL << ud)) != 0)
		snprintf(b, n, "%s%s%s", (res & POLLIN) != 0 ? "IN" : "",
		    (res & POLLOUT) != 0 ? "OUT" : "",
		    (res & POLLHUP) != 0 ? "HUP" : "");
	else
		snprintf(b, n, "%d", res);
}

/* Print and consume every CQE that is ready, in order. */
static void
dump(struct ring *r, const char *step)
{
	unsigned h = atomic_load_explicit(r->cq_head, memory_order_relaxed);
	unsigned t = atomic_load_explicit(r->cq_tail, memory_order_acquire);
	char res[32], fl[8];

	if (h == t)
		printf("%s/%s none\n", current, step);
	for (; h != t; h++) {
		struct io_uring_cqe *c = &r->cqes[h & r->cq_mask];

		print_res(c->user_data, c->res, res, sizeof(res));
		flags_str(c->flags, fl, sizeof(fl));
		printf("%s/%s ud=%llu res=%s flags=%s\n", current, step,
		    (unsigned long long)c->user_data, res, fl);
	}
	atomic_store_explicit(r->cq_head, h, memory_order_release);
}

static int
cqe_cmp(const void *a, const void *b)
{
	uint64_t x = ((const struct io_uring_cqe *)a)->user_data;
	uint64_t y = ((const struct io_uring_cqe *)b)->user_data;

	return (x < y ? -1 : x > y);
}

/* dump(), for CQEs whose order neither system promises. */
static void
dump_sorted(struct ring *r, const char *step)
{
	struct io_uring_cqe cq[16];
	unsigned h = atomic_load_explicit(r->cq_head, memory_order_relaxed);
	unsigned t = atomic_load_explicit(r->cq_tail, memory_order_acquire);
	unsigned n = 0;
	char res[32], fl[8];

	CHECK(t - h <= 16);
	for (; h != t; h++)
		cq[n++] = r->cqes[h & r->cq_mask];
	atomic_store_explicit(r->cq_head, h, memory_order_release);
	qsort(cq, n, sizeof(cq[0]), cqe_cmp);
	if (n == 0)
		printf("%s/%s none\n", current, step);
	for (unsigned i = 0; i < n; i++) {
		print_res(cq[i].user_data, cq[i].res, res, sizeof(res));
		flags_str(cq[i].flags, fl, sizeof(fl));
		printf("%s/%s ud=%llu res=%s flags=%s\n", current, step,
		    (unsigned long long)cq[i].user_data, res, fl);
	}
}

static void
say(const char *step, const char *fmt, int v)
{
	printf("%s/%s ", current, step);
	printf(fmt, v);
	printf("\n");
}

/* A system call's result: its value, or the errno's name. */
static void
say_rc(const char *step, int rc)
{
	if (rc < 0)
		printf("%s/%s -%s\n", current, step, errname(errno));
	else
		printf("%s/%s %d\n", current, step, rc);
}

static void
set_timeout(struct io_uring_sqe *s, struct __kernel_timespec *ts, long ms)
{
	ts->tv_sec = ms / 1000;
	ts->tv_nsec = (ms % 1000) * 1000000L;
	s->addr = (uint64_t)(uintptr_t)ts;
	s->len = 1;
}

static int
scratch_file(const char *data, size_t len)
{
	char path[] = "kqdiff.XXXXXX";
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	unlink(path);
	CHECK(write(fd, data, len) == (ssize_t)len);
	return (fd);
}

/* Cases. */

static void
c_nop(void)
{
	struct ring r;

	ring_init(&r, 8, NULL);
	prep(&r, IORING_OP_NOP, -1, 1);
	say_rc("enter", submit(&r));
	await(&r, 1, 2000);
	dump(&r, "cqe");
	ring_free(&r);
}

/* Which flags setup takes alone and in pairs. */
static void
c_setup_flags(void)
{
	static const struct {
		const char *name;
		uint32_t flag;
	} fl[] = {
		{ "IOPOLL", IORING_SETUP_IOPOLL },
		{ "SQPOLL", IORING_SETUP_SQPOLL },
		{ "SQ_AFF", IORING_SETUP_SQ_AFF },
		{ "CQSIZE", IORING_SETUP_CQSIZE },
		{ "CLAMP", IORING_SETUP_CLAMP },
		{ "R_DISABLED", IORING_SETUP_R_DISABLED },
		{ "SUBMIT_ALL", IORING_SETUP_SUBMIT_ALL },
		{ "COOP_TASKRUN", IORING_SETUP_COOP_TASKRUN },
		{ "TASKRUN_FLAG", IORING_SETUP_TASKRUN_FLAG },
		{ "SQE128", IORING_SETUP_SQE128 },
		{ "CQE32", IORING_SETUP_CQE32 },
		{ "SINGLE_ISSUER", IORING_SETUP_SINGLE_ISSUER },
		{ "DEFER_TASKRUN", IORING_SETUP_DEFER_TASKRUN },
		{ "NO_SQARRAY", IORING_SETUP_NO_SQARRAY },
	};
	static const struct {
		const char *name;
		uint32_t flags;
	} more[] = {
		{ "defer+sqpoll", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SQPOLL },
		{ "defer+coop", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN },
		{ "defer+flag", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG },
		{ "defer+iopoll", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_IOPOLL },
	};
	size_t n = sizeof(fl) / sizeof(fl[0]);
	char step[64];

	for (size_t i = 0; i < n; i++) {
		for (size_t j = i; j < n; j++) {
			struct io_uring_params p;
			int fd;

			memset(&p, 0, sizeof(p));
			p.flags = fl[i].flag | fl[j].flag;
			p.cq_entries = 16;
			fd = ring_sys_setup(4, &p);
			snprintf(step, sizeof(step), "%s+%s", fl[i].name,
			    fl[j].name);
			say_rc(step, fd < 0 ? -1 : 0);
			if (fd >= 0)
				close(fd);
		}
	}
	for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
		struct io_uring_params p;
		int fd;

		memset(&p, 0, sizeof(p));
		p.flags = more[i].flags;
		fd = ring_sys_setup(4, &p);
		say_rc(more[i].name, fd < 0 ? -1 : 0);
		if (fd >= 0)
			close(fd);
	}
}

/* What Linux 7.3 has past IORING_OP_PIPE, which kqring refuses. */
static void
c_newer(void)
{
	struct io_uring_params p;
	struct ring r;
	struct io_uring_sqe *s;
	uint64_t reg[8];
	void *m;
	int fd, sv[2];

	memset(&p, 0, sizeof(p));
	p.flags = 1U << 19;	/* IORING_SETUP_SQE_MIXED */
	fd = ring_sys_setup(4, &p);
	say_rc("sqe_mixed", fd < 0 ? -1 : 0);
	if (fd >= 0)
		close(fd);
	memset(&p, 0, sizeof(p));
	p.flags = (1U << 20) | IORING_SETUP_NO_SQARRAY;	/* SQ_REWIND */
	fd = ring_sys_setup(4, &p);
	say_rc("sq_rewind", fd < 0 ? -1 : 0);
	if (fd >= 0)
		close(fd);

	ring_init(&r, 8, NULL);
	prep(&r, 63, -1, 1);	/* IORING_OP_NOP128 */
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "nop128");
	prep(&r, 64, -1, 2);	/* IORING_OP_URING_CMD128 */
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "uring_cmd128");

	s = prep(&r, IORING_OP_TIMEOUT, -1, 3);
	s->addr = 1000000;
	s->len = 1;
	s->timeout_flags = 1U << 7;	/* IORING_TIMEOUT_IMMEDIATE_ARG */
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "timeout_immediate");

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	s = prep(&r, IORING_OP_URING_CMD, sv[0], 4);
	s->cmd_op = 5;		/* SOCKET_URING_OP_GETSOCKNAME */
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "getsockname");
	close(sv[0]);
	close(sv[1]);

	say_rc("register_zcrx_ctrl", ring_sys_register(r.fd, 36, NULL, 0));
	say_rc("register_bpf_filter", ring_sys_register(r.fd, 37, NULL, 0));
	say_rc("task_restrictions", ring_sys_register(-1,
	    IORING_REGISTER_RESTRICTIONS, NULL, 0));

	/* struct io_uring_buf_reg's min_left, once reserved. */
	m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	CHECK(m != MAP_FAILED);
	memset(reg, 0, sizeof(reg));
	reg[0] = (uint64_t)(uintptr_t)m;
	((uint32_t *)reg)[2] = 8;
	((uint16_t *)reg)[6] = 1;
	((uint32_t *)reg)[4] = 2;
	say_rc("pbuf_min_left", ring_sys_register(r.fd,
	    IORING_REGISTER_PBUF_RING, reg, 1));
	munmap(m, 4096);
	ring_free(&r);
}

static void
c_bad_sqe(void)
{
	struct ring r;
	struct io_uring_sqe *s;

	ring_init(&r, 8, NULL);
	prep(&r, 250, -1, 1);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "opcode");
	s = prep(&r, IORING_OP_NOP, -1, 2);
	s->flags = 0x80;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "flags");
	s = prep(&r, IORING_OP_READ, 9999, 3);
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "badfd");
	s = prep(&r, IORING_OP_READ, 0, 4);
	s->flags = IOSQE_FIXED_FILE;
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "no_fixed_table");
	ring_free(&r);
}

static void
c_batch_stop(void)
{
	struct ring r;

	ring_init(&r, 8, NULL);
	prep(&r, IORING_OP_NOP, -1, 1);
	prep(&r, 250, -1, 2);
	prep(&r, IORING_OP_NOP, -1, 3);
	say_rc("enter", ring_sys_enter(r.fd, 3, 0, 0, NULL, 0));
	say("left", "%d", (int)pending(&r));
	await(&r, 2, 2000);
	dump(&r, "cqe");
	ring_free(&r);
}

static void
c_links(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	char buf[16];
	int fd = scratch_file("abcd", 4);
	int p[2];

	ring_init(&r, 16, NULL);

	s = prep(&r, IORING_OP_NOP, -1, 1);
	s->flags = IOSQE_IO_LINK;
	s = prep(&r, IORING_OP_NOP, -1, 2);
	s->flags = IOSQE_IO_LINK;
	prep(&r, IORING_OP_NOP, -1, 3);
	(void)submit(&r);
	await(&r, 3, 2000);
	dump(&r, "ok");

	s = prep(&r, IORING_OP_READ, 9999, 4);
	s->flags = IOSQE_IO_LINK;
	s->len = 1;
	s = prep(&r, IORING_OP_NOP, -1, 5);
	s->flags = IOSQE_IO_LINK;
	prep(&r, IORING_OP_NOP, -1, 6);
	(void)submit(&r);
	await(&r, 3, 2000);
	dump(&r, "failed");

	s = prep(&r, IORING_OP_READ, 9999, 7);
	s->flags = IOSQE_IO_HARDLINK;
	s->len = 1;
	prep(&r, IORING_OP_NOP, -1, 8);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "hardlink");

	/* A short read breaks a link. */
	s = prep(&r, IORING_OP_READ, fd, 9);
	s->flags = IOSQE_IO_LINK;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	prep(&r, IORING_OP_NOP, -1, 10);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "short_read");

	/* The same from a pipe, and from a socket, where only MSG_WAITALL asks
	 * for the whole length. */
	CHECK(pipe(p) == 0);
	CHECK(write(p[1], "abc", 3) == 3);
	s = prep(&r, IORING_OP_READ, p[0], 20);
	s->flags = IOSQE_IO_LINK;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	prep(&r, IORING_OP_NOP, -1, 21);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "short_read_pipe");
	close(p[0]);
	close(p[1]);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0);
	CHECK(write(p[1], "abc", 3) == 3);
	s = prep(&r, IORING_OP_RECV, p[0], 22);
	s->flags = IOSQE_IO_LINK;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	prep(&r, IORING_OP_NOP, -1, 23);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "short_recv");

	/* MSG_WAITALL that the peer's end of file leaves short. */
	CHECK(write(p[1], "abc", 3) == 3);
	shutdown(p[1], SHUT_WR);
	s = prep(&r, IORING_OP_RECV, p[0], 24);
	s->flags = IOSQE_IO_LINK;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->msg_flags = MSG_WAITALL;
	prep(&r, IORING_OP_NOP, -1, 25);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "short_waitall");
	close(p[0]);
	close(p[1]);

	/* A failed request's CQE isn't skipped, nor are the ones after it. */
	s = prep(&r, IORING_OP_READ, fd, 26);
	s->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	prep(&r, IORING_OP_NOP, -1, 27);
	(void)submit(&r);
	await(&r, 2, 200);
	dump(&r, "short_skip");

	s = prep(&r, IORING_OP_READ, fd, 28);
	s->flags = IOSQE_IO_HARDLINK;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	prep(&r, IORING_OP_NOP, -1, 29);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "short_hardlink");

	{
		int q[2];

		CHECK(pipe(p) == 0);
		CHECK(pipe(q) == 0);
		CHECK(write(p[1], "abc", 3) == 3);
		s = prep(&r, IORING_OP_SPLICE, q[1], 30);
		s->flags = IOSQE_IO_LINK;
		s->splice_fd_in = p[0];
		s->splice_off_in = (uint64_t)-1;
		s->off = (uint64_t)-1;
		s->len = sizeof(buf);
		prep(&r, IORING_OP_NOP, -1, 31);
		(void)submit(&r);
		await(&r, 2, 2000);
		dump(&r, "short_splice");
		close(p[0]);
		close(p[1]);
		close(q[0]);
		close(q[1]);
	}

	/* A chain the batch leaves unfinished ends with the batch. */
	s = prep(&r, IORING_OP_NOP, -1, 11);
	s->flags = IOSQE_IO_LINK;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "open_chain");

	s = prep(&r, IORING_OP_NOP, -1, 12);
	s->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
	prep(&r, IORING_OP_NOP, -1, 13);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "skip");

	close(fd);
	ring_free(&r);
}

static void
c_drain(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	struct __kernel_timespec ts;

	ring_init(&r, 8, NULL);
	s = prep(&r, IORING_OP_TIMEOUT, -1, 1);
	set_timeout(s, &ts, 30);
	s = prep(&r, IORING_OP_NOP, -1, 2);
	s->flags = IOSQE_IO_DRAIN;
	prep(&r, IORING_OP_NOP, -1, 3);
	(void)submit(&r);
	await(&r, 3, 2000);
	dump(&r, "cqe");
	ring_free(&r);
}

static void
c_timeouts(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	struct __kernel_timespec ts, ts2;

	ring_init(&r, 8, NULL);

	s = prep(&r, IORING_OP_TIMEOUT, -1, 1);
	set_timeout(s, &ts, 10);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "expire");

	s = prep(&r, IORING_OP_TIMEOUT, -1, 2);
	set_timeout(s, &ts, 5000);
	s->off = 2;
	prep(&r, IORING_OP_NOP, -1, 3);
	prep(&r, IORING_OP_NOP, -1, 4);
	(void)submit(&r);
	await(&r, 3, 2000);
	dump(&r, "count");

	s = prep(&r, IORING_OP_TIMEOUT, -1, 5);
	set_timeout(s, &ts, 5000);
	(void)submit(&r);
	s = prep(&r, IORING_OP_TIMEOUT_REMOVE, -1, 6);
	s->addr = 5;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "remove");

	s = prep(&r, IORING_OP_TIMEOUT_REMOVE, -1, 7);
	s->addr = 12345;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "remove_missing");

	s = prep(&r, IORING_OP_TIMEOUT, -1, 8);
	set_timeout(s, &ts, 5000);
	(void)submit(&r);
	s = prep(&r, IORING_OP_TIMEOUT_REMOVE, -1, 9);
	s->addr = 8;
	s->timeout_flags = IORING_TIMEOUT_UPDATE;
	ts2.tv_sec = 0;
	ts2.tv_nsec = 10000000;
	s->off = (uint64_t)(uintptr_t)&ts2;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "update");

	ring_free(&r);
}

static void
c_link_timeout(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	struct __kernel_timespec ts;
	char buf[8];
	int p[2];

	CHECK(pipe(p) == 0);
	ring_init(&r, 8, NULL);

	s = prep(&r, IORING_OP_READ, p[0], 1);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->flags = IOSQE_IO_LINK;
	s = prep(&r, IORING_OP_LINK_TIMEOUT, -1, 2);
	set_timeout(s, &ts, 20);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "fires");

	CHECK(write(p[1], "x", 1) == 1);
	s = prep(&r, IORING_OP_READ, p[0], 3);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->flags = IOSQE_IO_LINK;
	s = prep(&r, IORING_OP_LINK_TIMEOUT, -1, 4);
	set_timeout(s, &ts, 5000);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "beaten");

	close(p[0]);
	close(p[1]);
	ring_free(&r);
}

static void
c_cancel(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	char buf[8];
	int p[2];

	CHECK(pipe(p) == 0);
	ring_init(&r, 16, NULL);
	res_poll_mask = 1ULL << 1;

	s = prep(&r, IORING_OP_POLL_ADD, p[0], 1);
	s->poll32_events = POLLIN;
	(void)submit(&r);
	s = prep(&r, IORING_OP_ASYNC_CANCEL, -1, 2);
	s->addr = 1;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump_sorted(&r, "poll");

	s = prep(&r, IORING_OP_ASYNC_CANCEL, -1, 3);
	s->addr = 12345;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "missing");

	/* A read blocked on a pipe, which on kqring is on a worker. */
	s = prep(&r, IORING_OP_READ, p[0], 4);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	(void)submit(&r);
	pause_ms(20);
	s = prep(&r, IORING_OP_ASYNC_CANCEL, -1, 5);
	s->addr = 4;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump(&r, "blocked_read");

	for (uint64_t i = 6; i < 9; i++) {
		s = prep(&r, IORING_OP_POLL_ADD, p[0], i);
		s->poll32_events = POLLIN;
		res_poll_mask |= 1ULL << i;
	}
	(void)submit(&r);
	s = prep(&r, IORING_OP_ASYNC_CANCEL, -1, 9);
	s->cancel_flags = IORING_ASYNC_CANCEL_ANY;
	(void)submit(&r);
	await(&r, 4, 2000);
	dump_sorted(&r, "any_one");
	s = prep(&r, IORING_OP_ASYNC_CANCEL, -1, 10);
	s->cancel_flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "any_all");

	res_poll_mask = 0;
	close(p[0]);
	close(p[1]);
	ring_free(&r);
}

static void
c_poll(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	int p[2];

	CHECK(pipe(p) == 0);
	ring_init(&r, 16, NULL);
	res_poll_mask = (1ULL << 1) | (1ULL << 2) | (1ULL << 4) | (1ULL << 5);

	s = prep(&r, IORING_OP_POLL_ADD, p[1], 1);
	s->poll32_events = POLLOUT;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "ready");

	s = prep(&r, IORING_OP_POLL_ADD, p[0], 2);
	s->poll32_events = POLLIN;
	s->len = IORING_POLL_ADD_MULTI;
	(void)submit(&r);
	await(&r, 0, 0);
	dump(&r, "armed");
	for (int i = 0; i < 3; i++) {
		CHECK(write(p[1], "x", 1) == 1);
		await(&r, 1, 2000);
		dump(&r, "multi");
	}
	s = prep(&r, IORING_OP_POLL_REMOVE, -1, 3);
	s->addr = 2;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump_sorted(&r, "removed");

	/* Three bytes are still in the pipe. How often does this fire? */
	s = prep(&r, IORING_OP_POLL_ADD, p[0], 4);
	s->poll32_events = POLLIN;
	s->len = IORING_POLL_ADD_LEVEL;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "level");
	s = prep(&r, IORING_OP_POLL_ADD, p[0], 5);
	s->poll32_events = POLLIN;
	s->len = IORING_POLL_ADD_LEVEL | IORING_POLL_ADD_MULTI;
	(void)submit(&r);
	await(&r, 1, 2000);
	pause_ms(50);
	(void)ring_sys_enter(r.fd, 0, 0, IORING_ENTER_GETEVENTS, NULL, 0);
	say("level_multi_many", "%d", ready(&r) > 1);
	atomic_store_explicit(r.cq_head, atomic_load(r.cq_tail),
	    memory_order_release);
	s = prep(&r, IORING_OP_POLL_REMOVE, -1, 6);
	s->addr = 5;
	(void)submit(&r);
	pause_ms(30);
	atomic_store_explicit(r.cq_head, atomic_load(r.cq_tail),
	    memory_order_release);

	res_poll_mask = 0;
	close(p[0]);
	close(p[1]);
	ring_free(&r);
}

/* The peer's close ends a poll, with what poll(2) would say. */
static void
c_poll_hup(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	struct pollfd pf;
	int p[2];

	CHECK(pipe(p) == 0);
	ring_init(&r, 16, NULL);
	res_poll_mask = 1ULL << 7;
	s = prep(&r, IORING_OP_POLL_ADD, p[0], 7);
	s->poll32_events = POLLIN;
	(void)submit(&r);
	close(p[1]);
	await(&r, 1, 2000);
	dump(&r, "hup");
	res_poll_mask = 0;
	pf.fd = p[0];
	pf.events = POLLIN;
	pf.revents = 0;
	CHECK(poll(&pf, 1, 0) == 1);
	say("native_in", "%d", (pf.revents & POLLIN) != 0);
	say("native_hup", "%d", (pf.revents & POLLHUP) != 0);
	close(p[0]);
	ring_free(&r);
}

static void
c_provided_buffers(void)
{
	static char bufs[2][64];
	struct ring r;
	struct io_uring_sqe *s;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	ring_init(&r, 16, NULL);

	s = prep(&r, IORING_OP_PROVIDE_BUFFERS, 2, 1);
	s->addr = (uint64_t)(uintptr_t)bufs;
	s->len = 64;
	s->off = 0;
	s->buf_group = 7;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "provide");

	CHECK(write(sv[1], "one", 3) == 3);
	s = prep(&r, IORING_OP_RECV, sv[0], 2);
	s->flags = IOSQE_BUFFER_SELECT;
	s->buf_group = 7;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "first");

	/* End of file: is a buffer spent? The next recvs tell. */
	shutdown(sv[1], SHUT_WR);
	s = prep(&r, IORING_OP_RECV, sv[0], 3);
	s->flags = IOSQE_BUFFER_SELECT;
	s->buf_group = 7;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "eof");
	s = prep(&r, IORING_OP_REMOVE_BUFFERS, 8, 4);
	s->buf_group = 7;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "left");

	s = prep(&r, IORING_OP_RECV, sv[0], 5);
	s->flags = IOSQE_BUFFER_SELECT;
	s->buf_group = 7;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "empty");

	close(sv[0]);
	close(sv[1]);
	ring_free(&r);
}

/* Fixed buffers when the mapping behind them goes or changes. */
static void
c_buffer_lifetime(void)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	struct ring r;
	struct io_uring_sqe *s;
	struct iovec iov;
	char out[8];
	int fd = scratch_file("", 0);
	char *m;

	CHECK(ftruncate(fd, (off_t)page) == 0);
	ring_init(&r, 8, NULL);

	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	CHECK(m != MAP_FAILED);
	memcpy(m, "before", 6);
	iov.iov_base = m;
	iov.iov_len = page;
	CHECK(ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1) == 0);

	/* Replace the page: which one does a fixed write see? */
	CHECK(mmap(m, page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == m);
	memcpy(m, "after!", 6);
	s = prep(&r, IORING_OP_WRITE_FIXED, fd, 1);
	s->addr = (uint64_t)(uintptr_t)m;
	s->len = 6;
	s->buf_index = 0;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "remapped");
	CHECK(pread(fd, out, 6, 0) == 6);
	printf("%s/remapped_data %.6s\n", current, out);

	CHECK(munmap(m, page) == 0);
	s = prep(&r, IORING_OP_WRITE_FIXED, fd, 2);
	s->addr = (uint64_t)(uintptr_t)m;
	s->len = 6;
	s->buf_index = 0;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "unmapped");

	s = prep(&r, IORING_OP_WRITE_FIXED, fd, 3);
	s->addr = (uint64_t)(uintptr_t)m;
	s->len = (unsigned)page + 1;
	s->buf_index = 0;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "too_long");

	close(fd);
	ring_free(&r);
}

static void
c_cq_overflow(void)
{
	struct io_uring_params p;
	struct ring r;
	unsigned seen = 0, order_ok = 1;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = 8;
	ring_init(&r, 8, &p);
	for (uint64_t i = 0; i < 24; i++) {
		prep(&r, IORING_OP_NOP, -1, i);
		if ((i & 7) == 7)
			say_rc("enter", submit(&r));
		pause_ms(2);
	}
	say("cq_ready", "%d", (int)ready(&r));
	say("sq_flag", "%d", (atomic_load(r.sq_flags) & IORING_SQ_CQ_OVERFLOW) != 0);
	for (int tries = 0; seen < 24 && tries < 100; tries++) {
		unsigned h = atomic_load(r.cq_head);
		unsigned t = atomic_load(r.cq_tail);

		for (; h != t; h++, seen++)
			if (r.cqes[h & r.cq_mask].user_data != seen)
				order_ok = 0;
		atomic_store(r.cq_head, h);
		(void)ring_sys_enter(r.fd, 0, 0, IORING_ENTER_GETEVENTS, NULL,
		    0);
	}
	say("reaped", "%d", (int)seen);
	say("in_order", "%d", (int)order_ok);
	say("sq_flag_after", "%d",
	    (atomic_load(r.sq_flags) & IORING_SQ_CQ_OVERFLOW) != 0);
	ring_free(&r);
}

static void
c_fixed_files(void)
{
	struct io_uring_rsrc_register rr;
	struct io_uring_rsrc_update2 up;
	struct ring r;
	struct io_uring_sqe *s;
	uint64_t tags[2] = { 0, 0 };
	char buf[8];
	int fds[2] = { -1, -1 };
	int a = scratch_file("aaaa", 4), b = scratch_file("bbbb", 4), nfd;

	ring_init(&r, 16, NULL);

	memset(&rr, 0, sizeof(rr));
	rr.nr = 2;
	rr.flags = IORING_RSRC_REGISTER_SPARSE;
	say_rc("register", ring_sys_register(r.fd, IORING_REGISTER_FILES2, &rr,
	    sizeof(rr)));

	s = prep(&r, IORING_OP_READ, 0, 1);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "empty_slot");

	s = prep(&r, IORING_OP_READ, 5, 2);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "bad_slot");

	fds[0] = a;
	tags[0] = 100;
	memset(&up, 0, sizeof(up));
	up.offset = 0;
	up.data = (uint64_t)(uintptr_t)fds;
	up.tags = (uint64_t)(uintptr_t)tags;
	up.nr = 1;
	say_rc("update", ring_sys_register(r.fd, IORING_REGISTER_FILES_UPDATE2,
	    &up, sizeof(up)));

	s = prep(&r, IORING_OP_READ, 0, 3);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 4;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "read");
	printf("%s/data %.4s\n", current, buf);

	/* Replacing the slot posts the old file's tag. */
	fds[0] = b;
	tags[0] = 0;
	say_rc("replace", ring_sys_register(r.fd, IORING_REGISTER_FILES_UPDATE2,
	    &up, sizeof(up)));
	await(&r, 1, 2000);
	dump(&r, "tag");
	s = prep(&r, IORING_OP_READ, 0, 4);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 4;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "read2");
	printf("%s/data2 %.4s\n", current, buf);

	/* A ring can't go in its own table. */
	fds[0] = r.fd;
	say_rc("self", ring_sys_register(r.fd, IORING_REGISTER_FILES_UPDATE2,
	    &up, sizeof(up)));

	/* What the refused update left in the slot. */
	s = prep(&r, IORING_OP_READ, 0, 20);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 4;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "after_self");

	fds[0] = b;
	say_rc("refill", ring_sys_register(r.fd, IORING_REGISTER_FILES_UPDATE2,
	    &up, sizeof(up)));

	/* Direct descriptors. */
	s = prep(&r, IORING_OP_FIXED_FD_INSTALL, 0, 5);
	s->flags = IOSQE_FIXED_FILE;
	res_fd_mask = 1ULL << 5;
	(void)submit(&r);
	await(&r, 1, 2000);
	nfd = r.cqes[atomic_load(r.cq_head) & r.cq_mask].res;
	dump(&r, "install");
	res_fd_mask = 0;
	if (nfd >= 0) {
		printf("%s/cloexec %d\n", current,
		    (fcntl(nfd, F_GETFD) & FD_CLOEXEC) != 0);
		close(nfd);
	}
	s = prep(&r, IORING_OP_CLOSE, 0, 6);
	s->file_index = 1;
	s->fd = 0;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "close_slot");
	s = prep(&r, IORING_OP_READ, 0, 7);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "closed_slot");

	say_rc("unregister", ring_sys_register(r.fd, IORING_UNREGISTER_FILES,
	    NULL, 0));
	say_rc("unregister_again", ring_sys_register(r.fd,
	    IORING_UNREGISTER_FILES, NULL, 0));
	close(a);
	close(b);
	ring_free(&r);
}

static void
c_sockets(void)
{
	struct ring r;
	struct io_uring_sqe *s;
	static char buf[64];
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	ring_init(&r, 16, NULL);

	s = prep(&r, IORING_OP_RECV, sv[0], 1);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	(void)submit(&r);
	await(&r, 0, 0);
	dump(&r, "armed");
	CHECK(write(sv[1], "hello", 5) == 5);
	await(&r, 1, 2000);
	dump(&r, "woken");

	s = prep(&r, IORING_OP_RECV, sv[0], 2);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	s->msg_flags = MSG_DONTWAIT;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "dontwait");

	close(sv[1]);
	s = prep(&r, IORING_OP_RECV, sv[0], 3);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = sizeof(buf);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "eof");

	s = prep(&r, IORING_OP_SEND, sv[0], 4);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 1;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "closed_peer");

	s = prep(&r, IORING_OP_RECV, 0, 5);
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = 1;
	s->fd = scratch_file("x", 1);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "not_a_socket");

	close(sv[0]);
	ring_free(&r);
}

/* A forked child with its parent's ring. */
static void
c_fork(void)
{
	struct ring r;
	int st, res[2] = { 0, 0 }, p[2];
	pid_t pid;

	CHECK(pipe(p) == 0);
	ring_init(&r, 8, NULL);
	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		prep(&r, IORING_OP_NOP, -1, 1);
		res[0] = ring_sys_enter(r.fd, 1, 0, 0, NULL, 0);
		res[1] = res[0] < 0 ? errno : 0;
		(void)write(p[1], res, sizeof(res));
		_exit(0);
	}
	CHECK(read(p[0], res, sizeof(res)) == sizeof(res));
	CHECK(waitpid(pid, &st, 0) == pid);
	errno = res[1];
	say_rc("child_enter", res[0]);
	await(&r, 0, 0);
	dump(&r, "parent_cq");

	prep(&r, IORING_OP_NOP, -1, 2);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "parent_after");
	close(p[0]);
	close(p[1]);
	ring_free(&r);
}

/*
 * The ring after its owner's execve(2), with its descriptor kept open and a
 * poll armed. The process that made the ring has to be the one to exec, as
 * kqring refuses any other first.
 */
static void
c_exec(void)
{
	struct io_uring_sqe *s;
	struct ring r;
	char arg[16], pfd[16];
	int st, p[2];
	pid_t pid;

	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		CHECK(pipe(p) == 0);
		ring_init(&r, 8, NULL);
		CHECK(fcntl(r.fd, F_SETFD, 0) == 0);
		s = prep(&r, IORING_OP_POLL_ADD, p[0], 1);
		s->poll32_events = POLLIN;
		CHECK(submit(&r) == 1);
		snprintf(arg, sizeof(arg), "%d", r.fd);
		snprintf(pfd, sizeof(pfd), "%d", p[1]);
		execl(self, self, "--exec-child", arg, pfd, (char *)NULL);
		_exit(99);
	}
	CHECK(waitpid(pid, &st, 0) == pid);
	say("status", "%d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static int
exec_child(const char *arg, const char *pfd)
{
	int fd = atoi(arg), rc;

	current = "exec";
	rc = ring_sys_enter(fd, 0, 0, 0, NULL, 0);
	say_rc("enter_after", rc);
	(void)pfd;
	fflush(stdout);
	return (0);
}

struct mt {
	struct ring	*r;
	pthread_mutex_t	*mx;
	uint64_t	base;
	int		enter_errno;
};

static void *
mt_submit(void *arg)
{
	struct mt *m = arg;

	for (uint64_t i = 0; i < 200; i++) {
		pthread_mutex_lock(m->mx);
		while (m->r->tail - atomic_load(m->r->sq_head) >=
		    m->r->sq_entries)
			(void)submit(m->r);
		prep(m->r, IORING_OP_NOP, -1, m->base + i);
		if (submit(m->r) < 0)
			m->enter_errno = errno;
		pthread_mutex_unlock(m->mx);
	}
	return (NULL);
}

static void *
other_thread_enter(void *arg)
{
	struct ring *r = arg;
	int rc = ring_sys_enter(r->fd, 0, 0, 0, NULL, 0);

	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

static void *
other_thread_submit(void *arg)
{
	struct ring *r = arg;
	int rc;

	prep(r, IORING_OP_NOP, -1, 1);
	rc = ring_sys_enter(r->fd, 1, 0, 0, NULL, 0);
	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

static void *
other_thread_getevents(void *arg)
{
	struct ring *r = arg;
	int rc = ring_sys_enter(r->fd, 0, 0, IORING_ENTER_GETEVENTS, NULL, 0);

	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

/* Several submitters on one ring, then what SINGLE_ISSUER refuses. */
static void
c_threads(void)
{
	pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
	struct io_uring_params p;
	pthread_t th[4];
	struct mt m[4];
	struct ring r;
	void *res;
	uint64_t sum = 0;
	unsigned n = 0;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = 2048;
	ring_init(&r, 64, &p);
	for (int i = 0; i < 4; i++) {
		m[i] = (struct mt){ &r, &mx, (uint64_t)i * 1000, 0 };
		CHECK(pthread_create(&th[i], NULL, mt_submit, &m[i]) == 0);
	}
	for (int i = 0; i < 4; i++) {
		CHECK(pthread_join(th[i], NULL) == 0);
		say("enter_errors", "%d", m[i].enter_errno);
	}
	await(&r, 800, 5000);
	for (; ready(&r) > 0; n++) {
		unsigned h = atomic_load(r.cq_head);

		sum += r.cqes[h & r.cq_mask].user_data;
		atomic_store(r.cq_head, h + 1);
	}
	say("cqes", "%d", (int)n);
	say("sum", "%d", (int)sum);
	ring_free(&r);

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER;
	ring_init(&r, 8, &p);
	CHECK(pthread_create(&th[0], NULL, other_thread_enter, &r) == 0);
	CHECK(pthread_join(th[0], &res) == 0);
	errno = (int)(intptr_t)res;
	say_rc("single_issuer", errno != 0 ? -1 : 0);
	CHECK(pthread_create(&th[0], NULL, other_thread_submit, &r) == 0);
	CHECK(pthread_join(th[0], &res) == 0);
	errno = (int)(intptr_t)res;
	say_rc("single_issuer_submit", errno != 0 ? -1 : 0);
	CHECK(pthread_create(&th[0], NULL, other_thread_getevents, &r) == 0);
	CHECK(pthread_join(th[0], &res) == 0);
	errno = (int)(intptr_t)res;
	say_rc("single_issuer_getevents", errno != 0 ? -1 : 0);
	ring_free(&r);

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	ring_init(&r, 8, &p);
	CHECK(pthread_create(&th[0], NULL, other_thread_enter, &r) == 0);
	CHECK(pthread_join(th[0], &res) == 0);
	errno = (int)(intptr_t)res;
	say_rc("defer_enter", errno != 0 ? -1 : 0);
	CHECK(pthread_create(&th[0], NULL, other_thread_getevents, &r) == 0);
	CHECK(pthread_join(th[0], &res) == 0);
	errno = (int)(intptr_t)res;
	say_rc("defer_getevents", errno != 0 ? -1 : 0);
	ring_free(&r);
}

static void
c_eventfd(void)
{
	struct ring r;
	uint64_t v = 0;
	int e = eventfd(0, EFD_NONBLOCK);

	CHECK(e >= 0);
	ring_init(&r, 8, NULL);
	say_rc("register", ring_sys_register(r.fd, IORING_REGISTER_EVENTFD, &e,
	    1));
	say_rc("again", ring_sys_register(r.fd, IORING_REGISTER_EVENTFD, &e,
	    1));
	prep(&r, IORING_OP_NOP, -1, 1);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "cqe");
	say("signalled", "%d", read(e, &v, sizeof(v)) == sizeof(v) && v > 0);
	say_rc("unregister", ring_sys_register(r.fd, IORING_UNREGISTER_EVENTFD,
	    NULL, 0));
	close(e);
	ring_free(&r);
}

static const struct {
	const char	*name;
	void		(*fn)(void);
} cases[] = {
	{ "nop", c_nop },
	{ "setup_flags", c_setup_flags },
	{ "newer", c_newer },
	{ "bad_sqe", c_bad_sqe },
	{ "batch_stop", c_batch_stop },
	{ "links", c_links },
	{ "drain", c_drain },
	{ "timeouts", c_timeouts },
	{ "link_timeout", c_link_timeout },
	{ "cancel", c_cancel },
	{ "poll", c_poll },
	{ "poll_hup", c_poll_hup },
	{ "provided_buffers", c_provided_buffers },
	{ "buffer_lifetime", c_buffer_lifetime },
	{ "cq_overflow", c_cq_overflow },
	{ "fixed_files", c_fixed_files },
	{ "sockets", c_sockets },
	{ "fork", c_fork },
	{ "exec", c_exec },
	{ "threads", c_threads },
	{ "eventfd", c_eventfd },
};

static void
header(void)
{
	struct utsname u;
	struct rlimit rl;

	CHECK(uname(&u) == 0);
	CHECK(getrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	printf("# system %s %s %s\n", u.sysname, u.release, u.machine);
	printf("# memlock %llu\n", (unsigned long long)rl.rlim_cur);
#if defined(__VERSION__)
	printf("# compiler %s\n", __VERSION__);
#endif
}

int
main(int argc, char **argv)
{
	self = argv[0];
	if (argc == 4 && strcmp(argv[1], "--exec-child") == 0)
		return (exec_child(argv[2], argv[3]));
	signal(SIGPIPE, SIG_DFL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	header();
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (argc > 1 && strcmp(argv[1], cases[i].name) != 0)
			continue;
		current = cases[i].name;
		cases[i].fn();
	}
	printf("# end\n");
	return (0);
}
