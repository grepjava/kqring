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
#include <sys/prctl.h>
#include <sys/syscall.h>
#define ring_sys_setup(n, p)	((int)syscall(__NR_io_uring_setup, (n), (p)))
#define ring_sys_enter(fd, s, w, f, a, z) \
	((int)syscall(__NR_io_uring_enter, (fd), (s), (w), (f), (a), (z)))
#define ring_sys_register(fd, op, a, n) \
	((int)syscall(__NR_io_uring_register, (fd), (op), (a), (n)))
#else
#include <sys/eventfd.h>
#include <sys/kqring.h>
#include <sys/procctl.h>
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
		{ ENXIO, "ENXIO" }, { EPERM, "EPERM" },
		{ EMSGSIZE, "EMSGSIZE" }, { ELOOP, "ELOOP" },
		{ EXDEV, "EXDEV" }, { ENOTDIR, "ENOTDIR" },
		{ EISDIR, "EISDIR" }, { ENAMETOOLONG, "ENAMETOOLONG" },
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
		{ "hybrid", 1U << 17 },		/* IORING_SETUP_HYBRID_IOPOLL */
		{ "hybrid+iopoll", (1U << 17) | IORING_SETUP_IOPOLL },
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

/*
 * A multishot RECV that takes its group's last buffer: does it end at
 * once, or only when data comes with no buffer for it?
 */
static void
c_recv_mshot_drained(void)
{
	static char bufs[3][64];
	struct ring r;
	struct io_uring_sqe *s;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	ring_init(&r, 16, NULL);
	s = prep(&r, IORING_OP_PROVIDE_BUFFERS, 2, 1);
	s->addr = (uint64_t)(uintptr_t)bufs;
	s->len = 64;
	s->buf_group = 8;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "provide");

	s = prep(&r, IORING_OP_RECV, sv[0], 2);
	s->flags = IOSQE_BUFFER_SELECT;
	s->buf_group = 8;
	s->ioprio = IORING_RECV_MULTISHOT;
	(void)submit(&r);
	CHECK(write(sv[1], "a", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "first");
	CHECK(write(sv[1], "b", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "last_buffer");
	await(&r, 1, 100);
	dump(&r, "idle");

	/* A buffer provided now, and then data. */
	s = prep(&r, IORING_OP_PROVIDE_BUFFERS, 1, 3);
	s->addr = (uint64_t)(uintptr_t)bufs[2];
	s->len = 64;
	s->buf_group = 8;
	s->off = 2;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "provide_more");
	CHECK(write(sv[1], "c", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "after_provide");

	/* Data with no buffer for it. */
	CHECK(write(sv[1], "d", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "no_buffer");

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

/*
 * A registered buffer across fork(2). The child reads a file into the
 * parent's buffer, by its own request; then the parent writes to the
 * buffer's memory and writes the buffer to the file. Each side reports
 * what its own memory holds.
 */
static void
c_buffer_fork(void)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	struct io_uring_sqe *s;
	struct ring r;
	struct iovec iov;
	char out[8], view[16];
	int fd = scratch_file("filedata", 8), p[2], st;
	pid_t pid;
	char *m;

	CHECK(ftruncate(fd, (off_t)page) == 0);
	CHECK(pipe(p) == 0);
	ring_init(&r, 8, NULL);
	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	CHECK(m != MAP_FAILED);
	memcpy(m, "parent0!", 8);
	iov.iov_base = m;
	iov.iov_len = page;
	CHECK(ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1) == 0);
	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		char rep[64];
		struct io_uring_cqe c;
		unsigned h;

		s = prep(&r, IORING_OP_READ_FIXED, fd, 1);
		s->addr = (uint64_t)(uintptr_t)m;
		s->len = 8;
		s->buf_index = 0;
		(void)submit(&r);
		await(&r, 1, 2000);
		h = atomic_load(r.cq_head);
		if (h == atomic_load(r.cq_tail)) {
			snprintf(rep, sizeof(rep), "none %.8s", m);
		} else {
			c = r.cqes[h & r.cq_mask];
			atomic_store(r.cq_head, h + 1);
			if (c.res < 0)
				snprintf(rep, sizeof(rep), "-%s %.8s",
				    errname(-c.res), m);
			else
				snprintf(rep, sizeof(rep), "%d %.8s", c.res, m);
		}
		(void)write(p[1], rep, sizeof(rep));
		_exit(0);
	}
	{
		char rep[64];

		CHECK(read(p[0], rep, sizeof(rep)) == sizeof(rep));
		CHECK(waitpid(pid, &st, 0) == pid);
		printf("%s/child_read %s\n", current, rep);
	}
	r.tail = atomic_load(r.sq_tail);
	memcpy(view, m, 8);
	printf("%s/parent_sees %.8s\n", current, view);

	memcpy(m, "parentB!", 8);
	s = prep(&r, IORING_OP_WRITE_FIXED, fd, 2);
	s->addr = (uint64_t)(uintptr_t)m;
	s->len = 8;
	s->off = 100;
	s->buf_index = 0;
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "parent_write");
	CHECK(pread(fd, out, 8, 100) == 8);
	printf("%s/written %.8s\n", current, out);

	munmap(m, page);
	close(p[0]);
	close(p[1]);
	close(fd);
	ring_free(&r);
}

/* What kinds of memory may be registered as a buffer. */
static void
c_buffer_backing(void)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	struct ring r;
	struct iovec iov;
	int fd = scratch_file("", 0), mfd, rc;
	char *m;

	CHECK(ftruncate(fd, (off_t)page) == 0);
	ring_init(&r, 8, NULL);
	iov.iov_len = page;

	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	rc = ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
	say_rc("file_shared", rc);
	if (rc == 0)
		CHECK(ring_sys_register(r.fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	munmap(m, page);

	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	rc = ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
	say_rc("file_private", rc);
	if (rc == 0)
		CHECK(ring_sys_register(r.fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	munmap(m, page);

	mfd = memfd_create("kqdiff", 0);
	CHECK(mfd >= 0);
	CHECK(ftruncate(mfd, (off_t)page) == 0);
	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	rc = ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
	say_rc("memfd_shared", rc);
	if (rc == 0)
		CHECK(ring_sys_register(r.fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	munmap(m, page);
	close(mfd);

	m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON,
	    -1, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	rc = ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
	say_rc("anon_shared", rc);
	if (rc == 0)
		CHECK(ring_sys_register(r.fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	munmap(m, page);

	m = mmap(NULL, page, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	rc = ring_sys_register(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
	say_rc("anon_readonly", rc);
	if (rc == 0)
		CHECK(ring_sys_register(r.fd, IORING_UNREGISTER_BUFFERS, NULL,
		    0) == 0);
	munmap(m, page);

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

	/* The child's SQE moved the shared tail past this copy's. */
	r.tail = atomic_load(r.sq_tail);
	prep(&r, IORING_OP_NOP, -1, 2);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "parent_after");
	close(p[0]);
	close(p[1]);
	ring_free(&r);
}

/*
 * Several processes with requests in flight on one ring. The child polls a
 * pipe of its own at the number the parent polls another pipe at, then
 * exits with a standalone TIMEOUT in flight, which still fires; the parent's
 * poll still fires too. With armed, the child also leaves a read of an empty
 * pipe in flight, and the parent sees when and how it ends.
 */
static void
fork_inflight(bool armed)
{
	struct io_uring_sqe *s;
	struct ring r;
	int pa[2], pb[2], pc[2], up[2], down[2], rc, st;
	char c = 0;
	pid_t pid;

	CHECK(pipe(pa) == 0);
	CHECK(pipe(pb) == 0);
	CHECK(pipe(pc) == 0);
	CHECK(pipe(up) == 0);
	CHECK(pipe(down) == 0);
	res_poll_mask = 1ULL << 1 | 1ULL << 2;
	ring_init(&r, 8, NULL);
	s = prep(&r, IORING_OP_POLL_ADD, pa[0], 1);
	s->poll32_events = POLLIN;
	say_rc("parent_submit", submit(&r));
	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		static char cbuf[8];
		static struct __kernel_timespec ts = { 0, 300000000 };

		CHECK(dup2(pb[0], pa[0]) == pa[0]);
		s = prep(&r, IORING_OP_POLL_ADD, pa[0], 2);
		s->poll32_events = POLLIN;
		if (armed) {
			s = prep(&r, IORING_OP_READ, pc[0], 3);
			s->addr = (uint64_t)(uintptr_t)cbuf;
			s->len = sizeof(cbuf);
		}
		s = prep(&r, IORING_OP_TIMEOUT, -1, 4);
		s->addr = (uint64_t)(uintptr_t)&ts;
		s->len = 1;
		rc = submit(&r);
		if (rc < 0)
			rc = -errno;
		(void)write(up[1], &rc, sizeof(rc));
		/* Wait to be told to exit; a CQE's task work may interrupt. */
		while (read(down[0], &c, 1) != 1 && errno == EINTR)
			;
		_exit(0);
	}
	CHECK(read(up[0], &rc, sizeof(rc)) == sizeof(rc));
	if (rc < 0)
		errno = -rc;
	say_rc("child_submit", rc < 0 ? -1 : rc);
	r.tail = atomic_load(r.sq_tail);
	CHECK(write(pb[1], "b", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "child_poll");
	CHECK(write(down[1], "x", 1) == 1);
	CHECK(waitpid(pid, &st, 0) == pid);
	await(&r, armed ? 2 : 1, 2000);
	dump_sorted(&r, "child_exit");
	if (armed) {
		CHECK(write(pc[1], "c", 1) == 1);
		await(&r, 1, 500);
		dump(&r, "child_read_late");
	}
	CHECK(write(pa[1], "a", 1) == 1);
	await(&r, 1, 2000);
	dump(&r, "parent_poll");
	prep(&r, IORING_OP_NOP, -1, 5);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "parent_after");
	res_poll_mask = 0;
	for (int i = 0; i < 2; i++) {
		close(pa[i]);
		close(pb[i]);
		close(pc[i]);
		close(up[i]);
		close(down[i]);
	}
	ring_free(&r);
}

static void
c_fork_inflight(void)
{
	fork_inflight(false);
}

static void
c_exit_armed(void)
{
	fork_inflight(true);
}

/*
 * The ring after its owner's execve(2), with its descriptor kept open and a
 * poll armed: whether the new image can enter it, and what the poll ends
 * with. The new image maps the CQ from the offsets passed to it.
 */
static void
c_exec(void)
{
	struct io_uring_sqe *s;
	struct ring r;
	char arg[64], pfd[16];
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
		snprintf(arg, sizeof(arg), "%d,%zu,%td,%td,%td,%u", r.fd,
		    r.map_len, (char *)r.cq_head - (char *)r.map,
		    (char *)r.cq_tail - (char *)r.map,
		    (char *)r.cqes - (char *)r.map, r.cq_mask);
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
	struct ring r;
	long head, tail, cqes;
	int fd, rc;

	current = "exec";
	memset(&r, 0, sizeof(r));
	if (sscanf(arg, "%d,%zu,%ld,%ld,%ld,%u", &fd, &r.map_len, &head,
	    &tail, &cqes, &r.cq_mask) != 6)
		return (2);
	rc = ring_sys_enter(fd, 0, 0, 0, NULL, 0);
	say_rc("enter_after", rc);
	if (rc >= 0) {
		r.fd = fd;
		r.map = mmap(NULL, r.map_len, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, IORING_OFF_SQ_RING);
		CHECK(r.map != MAP_FAILED);
		r.cq_head = (_Atomic unsigned *)((char *)r.map + head);
		r.cq_tail = (_Atomic unsigned *)((char *)r.map + tail);
		r.cqes = (struct io_uring_cqe *)((char *)r.map + cqes);
		await(&r, 1, 2000);
		dump(&r, "cq_after");
	}
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

/* IORING_ENTER_ABS_TIMER on the default clock, and NO_IOWAIT. */
static void
c_abs_timer(void)
{
	struct __kernel_timespec ts;
	struct io_uring_getevents_arg ga = { .ts = (uint64_t)(uintptr_t)&ts };
	struct io_uring_params p;
	struct timespec now, t1;
	struct ring r;
	long ms;

	memset(&p, 0, sizeof(p));
	ring_init(&r, 8, &p);
	say("feat_no_iowait", "%d", (p.features & IORING_FEAT_NO_IOWAIT) != 0);

	clock_gettime(CLOCK_MONOTONIC, &now);
	ts.tv_sec = now.tv_sec;
	ts.tv_nsec = now.tv_nsec + 100000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	say_rc("abs_100ms", ring_sys_enter(r.fd, 0, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG | IORING_ENTER_ABS_TIMER, &ga, sizeof(ga)));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (t1.tv_sec - now.tv_sec) * 1000 +
	    (t1.tv_nsec - now.tv_nsec) / 1000000;
	say("abs_100ms_waited", "%d", ms >= 90 && ms < 1000);

	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	say_rc("abs_past", ring_sys_enter(r.fd, 0, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG | IORING_ENTER_ABS_TIMER, &ga, sizeof(ga)));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (t1.tv_sec - now.tv_sec) * 1000 +
	    (t1.tv_nsec - now.tv_nsec) / 1000000;
	say("abs_past_at_once", "%d", ms < 50);

	prep(&r, IORING_OP_NOP, -1, 1);
	say_rc("no_iowait", ring_sys_enter(r.fd, 1, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_NO_IOWAIT, NULL, 0));
	dump(&r, "cqe");
	ring_free(&r);
}

/*
 * A bad wait argument with SQEs to submit: kqring refuses the enter before
 * it submits; Linux submits, then drops the wait's error for the count.
 */
static void
c_ext_arg_order(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct io_uring_getevents_arg ga = { .ts = (uint64_t)(uintptr_t)&ts };
	struct ring r;

	ring_init(&r, 8, NULL);
	prep(&r, IORING_OP_NOP, -1, 1);
	say_rc("bad_argsz", ring_sys_enter(r.fd, 1, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG, &ga, sizeof(ga) - 1));
	say("submitted", "%d", pending(&r) == 0);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "cqe");
	ring_free(&r);
}

/* The kernel's sigset_t: 8 bytes on Linux (_NSIG / 8), 16 on FreeBSD. */
#if defined(__linux__)
#define	KSIGSZ	8
#else
#define	KSIGSZ	sizeof(sigset_t)
#endif

static _Atomic int sig_hits;

static void
sig_count(int sig)
{
	(void)sig;
	atomic_fetch_add(&sig_hits, 1);
}

static void *
sig_kill_later(void *arg)
{
	pause_ms(50);
	pthread_kill(*(pthread_t *)arg, SIGUSR1);
	return (NULL);
}

static int
sig_blocked(void)
{
	sigset_t cur;

	CHECK(sigprocmask(SIG_SETMASK, NULL, &cur) == 0);
	return (sigismember(&cur, SIGUSR1) == 1);
}

/* io_uring_enter(2)'s signal mask, plain and through EXT_ARG. */
static void
c_sigmask(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 300000000 };
	struct io_uring_getevents_arg ga = { .ts = (uint64_t)(uintptr_t)&ts };
	struct sigaction sa, osa;
	sigset_t none, usr1, all, old;
	pthread_t me = pthread_self(), killer;
	struct timespec t0, t1;
	struct ring r;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_count;
	CHECK(sigaction(SIGUSR1, &sa, &osa) == 0);
	sigemptyset(&none);
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	sigfillset(&all);
	CHECK(sigprocmask(SIG_BLOCK, &usr1, &old) == 0);
	ring_init(&r, 8, NULL);

	raise(SIGUSR1);
	say_rc("plain_unblocks", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS, &none, KSIGSZ));
	say("plain_hits", "%d", atomic_load(&sig_hits));
	say("plain_blocked_after", "%d", sig_blocked());

	raise(SIGUSR1);
	ga.sigmask = (uint64_t)(uintptr_t)&none;
	ga.sigmask_sz = KSIGSZ;
	say_rc("ext_unblocks", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga, sizeof(ga)));
	say("ext_hits", "%d", atomic_load(&sig_hits));

	prep(&r, IORING_OP_NOP, -1, 1);
	(void)submit(&r);
	raise(SIGUSR1);
	say_rc("with_cqe", ring_sys_enter(r.fd, 0, 2, IORING_ENTER_GETEVENTS,
	    &none, KSIGSZ));
	say("with_cqe_hits", "%d", atomic_load(&sig_hits));
	dump(&r, "cqe");

	prep(&r, IORING_OP_NOP, -1, 2);
	say_rc("bad_size_ready", ring_sys_enter(r.fd, 1, 1,
	    IORING_ENTER_GETEVENTS, &none, 3));
	dump(&r, "cqe");
	say_rc("bad_size_wait", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS, &none, 3));
	ga.sigmask_sz = KSIGSZ + 1;
	say_rc("ext_bad_size", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga, sizeof(ga)));
	ga.sigmask = 8;
	ga.sigmask_sz = KSIGSZ;
	say_rc("ext_fault", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga, sizeof(ga)));
	say("bad_hits", "%d", atomic_load(&sig_hits));

	CHECK(sigprocmask(SIG_UNBLOCK, &usr1, NULL) == 0);
	CHECK(pthread_create(&killer, NULL, sig_kill_later, &me) == 0);
	ga.sigmask = (uint64_t)(uintptr_t)&usr1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	say_rc("blocks", ring_sys_enter(r.fd, 0, 1,
	    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga, sizeof(ga)));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(pthread_join(killer, NULL) == 0);
	say("blocks_full_wait", "%d", (t1.tv_sec - t0.tv_sec) * 1000 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 250);
	say("blocks_hits", "%d", atomic_load(&sig_hits));
	say("blocks_unblocked_after", "%d", !sig_blocked());

	prep(&r, IORING_OP_NOP, -1, 3);
	say_rc("all_ready", ring_sys_enter(r.fd, 1, 1, IORING_ENTER_GETEVENTS,
	    &all, KSIGSZ));
	dump(&r, "cqe");
	say("all_unblocked_after", "%d", !sig_blocked());

	CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
	CHECK(sigaction(SIGUSR1, &osa, NULL) == 0);
	ring_free(&r);
}

static int
dregion(int fd, void *addr, uint64_t size, uint32_t rflags, uint64_t flags,
    uint64_t *moff)
{
	struct io_uring_region_desc rd;
	struct io_uring_mem_region_reg mr;
	int rc;

	memset(&rd, 0, sizeof(rd));
	memset(&mr, 0, sizeof(mr));
	rd.user_addr = (uint64_t)(uintptr_t)addr;
	rd.size = size;
	rd.flags = rflags;
	mr.region_uptr = (uint64_t)(uintptr_t)&rd;
	mr.flags = flags;
	rc = ring_sys_register(fd, IORING_REGISTER_MEM_REGION, &mr, 1);
	if (rc == 0 && moff != NULL)
		*moff = rd.mmap_offset;
	return (rc);
}

static int
dwait(int fd, unsigned submit, uint64_t off, long *ms)
{
	struct timespec t0, t1;
	int rc;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = ring_sys_enter(fd, submit, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG,
	    (void *)(uintptr_t)off, sizeof(struct io_uring_reg_wait));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (ms != NULL)
		*ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		    (t1.tv_nsec - t0.tv_nsec) / 1000000;
	return (rc);
}

static void
dwait_ts(struct io_uring_reg_wait *w, long ms)
{
	memset(w, 0, sizeof(*w));
	w->ts.tv_sec = ms / 1000;
	w->ts.tv_nsec = (ms % 1000) * 1000000L;
	w->flags = IORING_REG_WAIT_TS;
}

/* IORING_REGISTER_MEM_REGION and registered waits. */
static void
c_reg_wait(void)
{
	long page = sysconf(_SC_PAGESIZE), ms;
	size_t size = 2 * (size_t)page;
	size_t nw = size / sizeof(struct io_uring_reg_wait);
	struct io_uring_params p;
	struct io_uring_reg_wait *w, *kw;
	struct ring r, k;
	uint64_t moff = 1;
	char *user, *ro;
	pid_t pid;
	int st;

	user = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ro = mmap(NULL, (size_t)page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
	    -1, 0);
	CHECK(user != MAP_FAILED && ro != MAP_FAILED);
	w = (struct io_uring_reg_wait *)(void *)user;
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_R_DISABLED;
	ring_init(&r, 8, &p);

	say_rc("no_args", ring_sys_register(r.fd, IORING_REGISTER_MEM_REGION,
	    NULL, 1));
	say_rc("kernel_with_addr", dregion(r.fd, user, size, 0,
	    IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("user_no_addr", dregion(r.fd, NULL, size,
	    IORING_MEM_REGION_TYPE_USER, IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("size_0", dregion(r.fd, user, 0, IORING_MEM_REGION_TYPE_USER,
	    IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("misaligned", dregion(r.fd, user + 16, size,
	    IORING_MEM_REGION_TYPE_USER, IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("read_only", dregion(r.fd, ro, (uint64_t)page,
	    IORING_MEM_REGION_TYPE_USER, IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("bad_flags", dregion(r.fd, user, size,
	    IORING_MEM_REGION_TYPE_USER, 2, NULL));
	say_rc("user", dregion(r.fd, user, size, IORING_MEM_REGION_TYPE_USER,
	    IORING_MEM_REGION_REG_WAIT_ARG, &moff));
	say("user_mmap_offset", "%d", (int)moff);
	say_rc("again", dregion(r.fd, user, size, IORING_MEM_REGION_TYPE_USER,
	    0, NULL));
	say_rc("enable", ring_sys_register(r.fd, IORING_REGISTER_ENABLE_RINGS,
	    NULL, 0));

	dwait_ts(&w[0], 100);
	say_rc("wait_first", dwait(r.fd, 0, 0, &ms));
	say("wait_first_100ms", "%d", ms >= 90 && ms < 1000);
	dwait_ts(&w[nw - 1], 1);
	say_rc("wait_last", dwait(r.fd, 0, (nw - 1) * sizeof(*w), NULL));
	say_rc("wait_past_end", dwait(r.fd, 0, nw * sizeof(*w), NULL));
	say_rc("wait_straddles", dwait(r.fd, 0, size - sizeof(long), NULL));
	say_rc("wait_wraps", dwait(r.fd, 0, 0 - sizeof(long), NULL));
	say_rc("wait_unaligned", dwait(r.fd, 0, 4, NULL));
	say_rc("wait_argsz", ring_sys_enter(r.fd, 0, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG, NULL,
	    sizeof(*w) - 1));
	dwait_ts(&w[1], 1);
	w[1].flags = 2;
	say_rc("wait_bad_flags", dwait(r.fd, 0, sizeof(*w), NULL));
	dwait_ts(&w[1], 1000);
	w[1].sigmask = 100;
	w[1].sigmask_sz = 1;
	say_rc("wait_sigmask_size", dwait(r.fd, 0, sizeof(*w), NULL));
	w[1].sigmask_sz = KSIGSZ;
	say_rc("wait_sigmask_fault", dwait(r.fd, 0, sizeof(*w), NULL));
	memset(&w[2], 0, sizeof(*w));
	prep(&r, IORING_OP_NOP, -1, 1);
	say_rc("wait_nop", dwait(r.fd, 1, 2 * sizeof(*w), NULL));
	dump(&r, "cqe");

	dwait_ts(&w[3], 10000);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0)
		_exit(0);
	CHECK(waitpid(pid, &st, 0) == pid);
	dwait_ts(&w[3], 30);
	say_rc("after_fork", dwait(r.fd, 0, 3 * sizeof(*w), &ms));
	say("after_fork_reads_new", "%d", ms < 5000);

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_R_DISABLED;
	ring_init(&k, 8, &p);
	say_rc("kernel", dregion(k.fd, NULL, (uint64_t)page, 0,
	    IORING_MEM_REGION_REG_WAIT_ARG, &moff));
	say("kernel_mmap_offset", "%#x", (int)moff);
	kw = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, k.fd,
	    (off_t)moff);
	say("kernel_mapped", "%d", kw != MAP_FAILED);
	say_rc("kernel_enable", ring_sys_register(k.fd,
	    IORING_REGISTER_ENABLE_RINGS, NULL, 0));
	if (kw != MAP_FAILED) {
		dwait_ts(&kw[1], 20);
		say_rc("kernel_wait", dwait(k.fd, 0, sizeof(*kw), NULL));
		munmap(kw, (size_t)page);
	}
	ring_free(&k);

	ring_init(&k, 8, NULL);
	say_rc("enabled_wait_arg", dregion(k.fd, NULL, (uint64_t)page, 0,
	    IORING_MEM_REGION_REG_WAIT_ARG, NULL));
	say_rc("enabled_plain", dregion(k.fd, NULL, (uint64_t)page, 0, 0,
	    NULL));
	say_rc("enabled_wait", dwait(k.fd, 0, 0, NULL));
	ring_free(&k);

	ring_free(&r);
	munmap(user, size);
	munmap(ro, (size_t)page);
}

/*
 * Restrictions and classic BPF filters (IORING_REGISTER_RESTRICTIONS,
 * IORING_REGISTER_BPF_FILTER).  The filter structs and BPF opcodes are
 * spelled out here, in Linux's layout, so that the case doesn't depend on
 * which headers the system has.
 */
struct t_insn {
	uint16_t	code;
	uint8_t		jt, jf;
	uint32_t	k;
};
#define	T_LD		0x00
#define	T_LDX		0x01
#define	T_ST		0x02
#define	T_ALU		0x04
#define	T_JMP		0x05
#define	T_RET		0x06
#define	T_MISC		0x07
#define	T_B		0x10
#define	T_ABS		0x20
#define	T_MEM		0x60
#define	T_LEN		0x80
#define	T_DIV		0x30
#define	T_MOD		0x90
#define	T_JEQ		0x10
#define	T_X		0x08
#define	T_A		0x10
#define	T_TXA		0x80
#define	T_STMT(c, k)	{ (uint16_t)(c), 0, 0, (k) }
#define	T_JUMP(c, k, t, f) { (uint16_t)(c), (t), (f), (k) }
#define	T_N(a)		((uint32_t)(sizeof(a) / sizeof((a)[0])))

struct t_bpf {
	uint16_t	cmd_type;
	uint16_t	cmd_flags;
	uint32_t	resv;
	uint32_t	opcode;
	uint32_t	flags;
	uint32_t	filter_len;
	uint8_t		pdu_size;
	uint8_t		resv1[3];
	uint64_t	filter_ptr;
	uint64_t	resv2[5];
};

struct t_task_rst {
	uint16_t	flags;
	uint16_t	nr_res;
	uint32_t	resv[3];
	struct io_uring_restriction res[2];
};

static const struct t_insn t_allow[] = { T_STMT(T_RET, 1) };

/* no_new_privs, which registering for the process needs (or root). */
static void
set_nnp(void)
{
#if defined(__linux__)
	CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
#else
	int v = PROC_NO_NEW_PRIVS_ENABLE;

	CHECK(procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, &v) == 0);
#endif
}

/* IORING_REGISTER_BPF_FILTER on fd (-1: the process); *pdu in and out. */
static int
t_filter(int fd, uint32_t opcode, uint32_t flags, const struct t_insn *f,
    uint32_t len, uint8_t *pdu)
{
	struct t_bpf b;
	int rc;

	memset(&b, 0, sizeof(b));
	b.cmd_type = 1;		/* IO_URING_BPF_CMD_FILTER */
	b.opcode = opcode;
	b.flags = flags;
	b.filter_len = len;
	b.filter_ptr = (uint64_t)(uintptr_t)f;
	b.pdu_size = pdu != NULL ? *pdu : 0;
	rc = ring_sys_register(fd, 37, &b, 1);
	if (pdu != NULL)
		*pdu = b.pdu_size;
	return (rc);
}

/* Run fn in a child, so what it registers for itself goes with it. */
static void
in_child(void (*fn)(void))
{
	pid_t pid;
	int st;

	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		fn();
		fflush(stdout);
		_exit(0);
	}
	CHECK(waitpid(pid, &st, 0) == pid);
	say("child", "%d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

/*
 * Ring restrictions: opcode entries alone leave registrations free, and an
 * empty set restricts both.
 */
static void
c_restrictions_split(void)
{
	struct io_uring_params p;
	struct io_uring_restriction res[1];
	struct io_uring_sqe *s;
	struct ring r;
	int fds[2];

	CHECK(pipe(fds) == 0);
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_R_DISABLED;
	ring_init(&r, 8, &p);
	memset(res, 0, sizeof(res));
	res[0].opcode = IORING_RESTRICTION_SQE_OP;
	res[0].sqe_op = IORING_OP_NOP;
	say_rc("ops_only", ring_sys_register(r.fd,
	    IORING_REGISTER_RESTRICTIONS, res, 1));
	say_rc("again", ring_sys_register(r.fd,
	    IORING_REGISTER_RESTRICTIONS, res, 1));
	say_rc("enable", ring_sys_register(r.fd,
	    IORING_REGISTER_ENABLE_RINGS, NULL, 0));
	say_rc("register_files", ring_sys_register(r.fd,
	    IORING_REGISTER_FILES, fds, 2));
	prep(&r, IORING_OP_NOP, -1, 1);
	s = prep(&r, IORING_OP_READ, fds[0], 2);
	s->len = 0;
	say_rc("submit", submit(&r));
	await(&r, 2, 2000);
	dump_sorted(&r, "ops_only");
	ring_free(&r);

	ring_init(&r, 8, &p);
	say_rc("none", ring_sys_register(r.fd,
	    IORING_REGISTER_RESTRICTIONS, res, 0));
	say_rc("enable", ring_sys_register(r.fd,
	    IORING_REGISTER_ENABLE_RINGS, NULL, 0));
	say_rc("register_files_none", ring_sys_register(r.fd,
	    IORING_REGISTER_FILES, fds, 2));
	prep(&r, IORING_OP_NOP, -1, 3);
	(void)submit(&r);
	await(&r, 1, 2000);
	dump(&r, "none");
	ring_free(&r);
	close(fds[0]);
	close(fds[1]);
}

static void
task_restrict_child(void)
{
	struct t_task_rst tr;
	struct io_uring_params p;
	struct io_uring_sqe *s;
	struct ring r;
	uint64_t buf;
	struct iovec iov = { &buf, sizeof(buf) };
	int fds[2];

	CHECK(pipe(fds) == 0);
	memset(&tr, 0, sizeof(tr));
	tr.nr_res = 2;
	tr.res[0].opcode = IORING_RESTRICTION_SQE_OP;
	tr.res[0].sqe_op = IORING_OP_NOP;
	tr.res[1].opcode = IORING_RESTRICTION_REGISTER_OP;
	tr.res[1].register_op = IORING_REGISTER_FILES;
	if (geteuid() != 0)
		say_rc("without_nnp", ring_sys_register(-1,
		    IORING_REGISTER_RESTRICTIONS, &tr, 1));
	else
		printf("%s/without_nnp -EACCES\n", current);
	set_nnp();
	say_rc("nr_args_2", ring_sys_register(-1,
	    IORING_REGISTER_RESTRICTIONS, &tr, 2));
	say_rc("register", ring_sys_register(-1,
	    IORING_REGISTER_RESTRICTIONS, &tr, 1));
	say_rc("again", ring_sys_register(-1,
	    IORING_REGISTER_RESTRICTIONS, &tr, 1));

	ring_init(&r, 8, NULL);
	say_rc("register_files", ring_sys_register(r.fd,
	    IORING_REGISTER_FILES, fds, 2));
	say_rc("register_buffers", ring_sys_register(r.fd,
	    IORING_REGISTER_BUFFERS, &iov, 1));
	prep(&r, IORING_OP_NOP, -1, 1);
	s = prep(&r, IORING_OP_READ, fds[0], 2);
	s->len = 0;
	say_rc("submit", submit(&r));
	await(&r, 2, 2000);
	dump_sorted(&r, "inherited");
	ring_free(&r);

	/* A disabled ring: the process's register rules apply already. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_R_DISABLED;
	ring_init(&r, 8, &p);
	say_rc("ring_restrictions", ring_sys_register(r.fd,
	    IORING_REGISTER_RESTRICTIONS, tr.res, 1));
	say_rc("enable", ring_sys_register(r.fd,
	    IORING_REGISTER_ENABLE_RINGS, NULL, 0));
	ring_free(&r);

	/* A filter for the process too, after restrictions: allowed. */
	say_rc("filter", t_filter(-1, IORING_OP_NOP, 0, t_allow, 1, NULL));
	close(fds[0]);
	close(fds[1]);
}

static void
task_restrict_grandchild(void)
{
	struct ring r;

	struct io_uring_sqe *s;
	int fd = scratch_file("x", 1);

	ring_init(&r, 8, NULL);
	prep(&r, IORING_OP_NOP, -1, 1);
	s = prep(&r, IORING_OP_FSYNC, fd, 2);
	s->len = 0;
	(void)submit(&r);
	await(&r, 2, 2000);
	dump_sorted(&r, "forked");
	ring_free(&r);
	close(fd);
}

static void
task_restrict_forker(void)
{
	struct t_task_rst tr;

	memset(&tr, 0, sizeof(tr));
	tr.nr_res = 1;
	tr.res[0].opcode = IORING_RESTRICTION_SQE_OP;
	tr.res[0].sqe_op = IORING_OP_NOP;
	set_nnp();
	say_rc("register", ring_sys_register(-1,
	    IORING_REGISTER_RESTRICTIONS, &tr, 1));
	in_child(task_restrict_grandchild);
}

/* Restrictions for the process, then for the rings it sets up. */
static void
c_task_restrict(void)
{
	in_child(task_restrict_child);
	in_child(task_restrict_forker);
}

static void
bpf_filter_child(void)
{
	/* user_data's low word 7: deny. */
	static const struct t_insn deny7[] = {
		T_STMT(T_LD | T_ABS, 0),
		T_JUMP(T_JMP | T_JEQ, 7, 0, 1),
		T_STMT(T_RET, 0),
		T_STMT(T_RET, 1),
	};
	/* The context's length (40) through scratch memory and X. */
	static const struct t_insn len40[] = {
		T_STMT(T_LD | T_LEN, 0),
		T_STMT(T_ST, 0),
		T_STMT(T_LDX | T_MEM, 0),
		T_STMT(T_MISC | T_TXA, 0),
		T_JUMP(T_JMP | T_JEQ, 40, 0, 1),
		T_STMT(T_RET, 1),
		T_STMT(T_RET, 0),
	};
	/* A division by an X of 0: the program returns 0. */
	static const struct t_insn divx0[] = {
		T_STMT(T_LDX, 0),
		T_STMT(T_LD, 1),
		T_STMT(T_ALU | T_DIV | T_X, 0),
		T_STMT(T_RET, 1),
	};
	/* SOCKET: only family 2 (AF_INET on both systems). */
	static const struct t_insn inet[] = {
		T_STMT(T_LD | T_ABS, 16),
		T_JUMP(T_JMP | T_JEQ, 2, 0, 1),
		T_STMT(T_RET, 1),
		T_STMT(T_RET, 0),
	};
	static const struct t_insn mod[] = {
		T_STMT(T_LD, 5), T_STMT(T_ALU | T_MOD, 2), T_STMT(T_RET | T_A, 0),
	};
	static const struct t_insn byte[] = {
		T_STMT(T_LD | T_B | T_ABS, 0), T_STMT(T_RET | T_A, 0),
	};
	static const struct t_insn unaligned[] = {
		T_STMT(T_LD | T_ABS, 2), T_STMT(T_RET | T_A, 0),
	};
	static const struct t_insn past[] = {
		T_STMT(T_LD | T_ABS, 40), T_STMT(T_RET | T_A, 0),
	};
	static const struct t_insn jump_past[] = {
		T_STMT(T_JMP, 1), T_STMT(T_RET, 1),
	};
	static const struct t_insn no_ret[] = { T_STMT(T_LD, 1) };
	static const struct t_insn mem_unset[] = {
		T_STMT(T_LD | T_MEM, 0), T_STMT(T_RET | T_A, 0),
	};
	static const struct t_insn div0[] = {
		T_STMT(T_LD, 1), T_STMT(T_ALU | T_DIV, 0), T_STMT(T_RET | T_A, 0),
	};
	struct io_uring_sqe *s;
	struct t_bpf b;
	struct ring r, r2;
	uint8_t pdu;
	int fd = scratch_file("x", 1);

	memset(&b, 0, sizeof(b));
	if (geteuid() != 0)
		say_rc("task_without_nnp", t_filter(-1, IORING_OP_NOP, 0,
		    t_allow, 1, NULL));
	else
		printf("%s/task_without_nnp -EACCES\n", current);
	set_nnp();
	ring_init(&r, 8, NULL);
	say_rc("nr_args_0", ring_sys_register(r.fd, 37, &b, 0));
	say_rc("mod", t_filter(r.fd, IORING_OP_NOP, 0, mod, T_N(mod), NULL));
	say_rc("byte_load", t_filter(r.fd, IORING_OP_NOP, 0, byte, T_N(byte),
	    NULL));
	say_rc("unaligned", t_filter(r.fd, IORING_OP_NOP, 0, unaligned,
	    T_N(unaligned), NULL));
	say_rc("past_ctx", t_filter(r.fd, IORING_OP_NOP, 0, past, T_N(past),
	    NULL));
	say_rc("jump_past", t_filter(r.fd, IORING_OP_NOP, 0, jump_past,
	    T_N(jump_past), NULL));
	say_rc("no_ret", t_filter(r.fd, IORING_OP_NOP, 0, no_ret,
	    T_N(no_ret), NULL));
	say_rc("mem_unset", t_filter(r.fd, IORING_OP_NOP, 0, mem_unset,
	    T_N(mem_unset), NULL));
	say_rc("div_zero", t_filter(r.fd, IORING_OP_NOP, 0, div0, T_N(div0),
	    NULL));
	say_rc("len_0", t_filter(r.fd, IORING_OP_NOP, 0, t_allow, 0, NULL));
	say_rc("len_4097", t_filter(r.fd, IORING_OP_NOP, 0, t_allow, 4097,
	    NULL));
	say_rc("opcode_255", t_filter(r.fd, 255, 0, t_allow, 1, NULL));
	say_rc("flags_4", t_filter(r.fd, IORING_OP_NOP, 4, t_allow, 1, NULL));
	memset(&b, 0, sizeof(b));
	b.cmd_type = 2;
	b.opcode = IORING_OP_NOP;
	b.filter_len = 1;
	b.filter_ptr = (uint64_t)(uintptr_t)t_allow;
	say_rc("cmd_type_2", ring_sys_register(r.fd, 37, &b, 1));
	pdu = 24;
	say_rc("pdu_big", t_filter(r.fd, IORING_OP_SOCKET, 0, t_allow, 1,
	    &pdu));
	say("pdu_big_out", "%d", pdu);
	pdu = 8;
	say_rc("pdu_strict", t_filter(r.fd, IORING_OP_SOCKET, 2, t_allow, 1,
	    &pdu));
	say("pdu_strict_out", "%d", pdu);
	pdu = 0;
	say_rc("pdu_nop", t_filter(r.fd, IORING_OP_NOP, 0, t_allow, 1, &pdu));
	say("pdu_nop_out", "%d", pdu);

	say_rc("deny7", t_filter(r.fd, IORING_OP_NOP, 0, deny7, T_N(deny7),
	    NULL));
	say_rc("len40", t_filter(r.fd, IORING_OP_NOP, 0, len40, T_N(len40),
	    NULL));
	prep(&r, IORING_OP_NOP, -1, 7);
	prep(&r, IORING_OP_NOP, -1, 8);
	say_rc("submit", submit(&r));
	await(&r, 2, 2000);
	dump_sorted(&r, "nop_filters");

	ring_init(&r2, 8, NULL);
	say_rc("divx0", t_filter(r2.fd, IORING_OP_NOP, 0, divx0, T_N(divx0),
	    NULL));
	prep(&r2, IORING_OP_NOP, -1, 9);
	(void)submit(&r2);
	await(&r2, 1, 2000);
	dump(&r2, "divx0");
	ring_free(&r2);

	/* Deny the rest; a filter added later can't undo that. */
	ring_init(&r2, 8, NULL);
	say_rc("deny_rest", t_filter(r2.fd, IORING_OP_NOP, 1, t_allow, 1,
	    NULL));
	say_rc("allow_fsync", t_filter(r2.fd, IORING_OP_FSYNC, 0, t_allow,
	    1, NULL));
	prep(&r2, IORING_OP_NOP, -1, 10);
	prep(&r2, IORING_OP_FSYNC, fd, 11);
	(void)submit(&r2);
	await(&r2, 2, 2000);
	dump_sorted(&r2, "deny_rest");
	ring_free(&r2);

	ring_init(&r2, 8, NULL);
	say_rc("inet_only", t_filter(r2.fd, IORING_OP_SOCKET, 0, inet,
	    T_N(inet), NULL));
	s = prep(&r2, IORING_OP_SOCKET, 2, 12);	/* AF_INET */
	s->off = SOCK_STREAM;
	s = prep(&r2, IORING_OP_SOCKET, 1, 13);	/* AF_UNIX */
	s->off = SOCK_STREAM;
	res_fd_mask = 1ULL << 12;
	(void)submit(&r2);
	await(&r2, 2, 2000);
	dump_sorted(&r2, "socket");
	res_fd_mask = 0;
	ring_free(&r2);

	/* For the process: rings set up afterwards only. */
	ring_free(&r);
	ring_init(&r2, 8, NULL);
	say_rc("task_deny7", t_filter(-1, IORING_OP_NOP, 0, deny7,
	    T_N(deny7), NULL));
	prep(&r2, IORING_OP_NOP, -1, 7);
	(void)submit(&r2);
	await(&r2, 1, 2000);
	dump(&r2, "ring_before");
	ring_free(&r2);
	ring_init(&r, 8, NULL);
	prep(&r, IORING_OP_NOP, -1, 7);
	prep(&r, IORING_OP_NOP, -1, 6);
	(void)submit(&r);
	await(&r, 2, 2000);
	dump_sorted(&r, "ring_after");
	ring_free(&r);
	close(fd);
}

/* Classic BPF filters on a ring and for the process. */
static void
c_bpf_filter(void)
{
	in_child(bpf_filter_child);
}

/* Linux 7.3's values, for older headers. */
#define	D_NOP_INJECT_RESULT	(1U << 0)
#define	D_NOP_FILE		(1U << 1)
#define	D_NOP_FIXED_FILE	(1U << 2)
#define	D_NOP_FIXED_BUFFER	(1U << 3)
#define	D_NOP_TW		(1U << 4)
#define	D_RESOLVE_NO_XDEV	0x01
#define	D_RESOLVE_NO_SYMLINKS	0x04
#define	D_RESOLVE_BENEATH	0x08
#define	D_RESOLVE_IN_ROOT	0x10
#define	D_RESOLVE_CACHED	0x20

struct d_open_how {
	uint64_t	flags;
	uint64_t	mode;
	uint64_t	resolve;
};

/* One request, submitted alone: its CQE. */
static void
one(struct ring *r, const char *step)
{
	(void)submit(r);
	await(r, 1, 2000);
	dump(r, step);
}

/*
 * NOP's flags: a descriptor or fixed slot that must be open, a registered
 * buffer that must exist, and a completion from task work, which on a
 * DEFER_TASKRUN ring waits for a wait.
 */
static void
c_nop_flags(void)
{
	static const struct {
		const char	*name;
		uint32_t	nop_flags;
		uint8_t		sqe_flags;
		int		fd;		/* -2: the pipe */
		uint16_t	buf_index;
		uint32_t	len;
	} t[] = {
		{ "file", D_NOP_FILE, 0, -2, 0, 0 },
		{ "file_inject", D_NOP_FILE | D_NOP_INJECT_RESULT, 0, -2, 0, 7 },
		{ "file_bad", D_NOP_FILE, 0, 1000, 0, 0 },
		{ "file_bad_inject", D_NOP_FILE | D_NOP_INJECT_RESULT, 0, 1000,
		    0, 7 },
		{ "fixed", D_NOP_FILE | D_NOP_FIXED_FILE, 0, 0, 0, 0 },
		{ "fixed_empty", D_NOP_FILE | D_NOP_FIXED_FILE, 0, 1, 0, 0 },
		{ "fixed_past", D_NOP_FILE | D_NOP_FIXED_FILE, 0, 99, 0, 0 },
		{ "sqe_fixed", D_NOP_FILE, IOSQE_FIXED_FILE, 0, 0, 0 },
		{ "sqe_fixed_empty", D_NOP_FILE, IOSQE_FIXED_FILE, 1, 0, 0 },
		{ "sqe_fixed_alone", 0, IOSQE_FIXED_FILE, 1, 0, 0 },
		{ "fixed_alone", D_NOP_FIXED_FILE, 0, 5, 0, 0 },
		{ "buffer", D_NOP_FIXED_BUFFER, 0, -1, 0, 0 },
		{ "buffer_past", D_NOP_FIXED_BUFFER, 0, -1, 3, 0 },
		{ "buffer_inject", D_NOP_FIXED_BUFFER | D_NOP_INJECT_RESULT,
		    0, -1, 3, 9 },
		{ "file_bad_buffer_past", D_NOP_FILE | D_NOP_FIXED_BUFFER, 0,
		    1000, 3, 0 },
		{ "tw", D_NOP_TW, 0, -1, 0, 0 },
		{ "tw_inject", D_NOP_TW | D_NOP_INJECT_RESULT, 0, -1, 0, 3 },
		{ "tw_file_bad", D_NOP_TW | D_NOP_FILE, 0, 1000, 0, 0 },
		{ "unknown", 1U << 6, 0, -1, 0, 0 },
	};
	struct io_uring_params p;
	struct io_uring_sqe *s;
	struct iovec iov;
	struct ring r;
	int pfd[2], files[2];
	void *m;

	ring_init(&r, 8, NULL);
	CHECK(pipe(pfd) == 0);
	files[0] = pfd[0];
	files[1] = -1;
	say_rc("register_files", ring_sys_register(r.fd, IORING_REGISTER_FILES,
	    files, 2));
	m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	CHECK(m != MAP_FAILED);
	iov.iov_base = m;
	iov.iov_len = 4096;
	say_rc("register_buffers", ring_sys_register(r.fd,
	    IORING_REGISTER_BUFFERS, &iov, 1));
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		s = prep(&r, IORING_OP_NOP, t[i].fd == -2 ? pfd[0] : t[i].fd,
		    i + 1);
		s->nop_flags = t[i].nop_flags;
		s->flags = t[i].sqe_flags;
		s->buf_index = t[i].buf_index;
		s->len = t[i].len;
		one(&r, t[i].name);
	}
	ring_free(&r);

	/* DEFER_TASKRUN: the TW one's CQE waits for a wait. */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	ring_init(&r, 8, &p);
	prep(&r, IORING_OP_NOP, -1, 30);
	s = prep(&r, IORING_OP_NOP, -1, 31);
	s->nop_flags = D_NOP_TW;
	say_rc("defer_submit", submit(&r));
	say("defer_ready", "%d", (int)ready(&r));
	dump(&r, "defer_before_wait");
	{
		struct __kernel_timespec ts = { .tv_sec = 1 };
		struct io_uring_getevents_arg ga;

		memset(&ga, 0, sizeof(ga));
		ga.ts = (uint64_t)(uintptr_t)&ts;
		say_rc("defer_wait", ring_sys_enter(r.fd, 0, 1,
		    IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &ga,
		    sizeof(ga)));
	}
	dump(&r, "defer_after_wait");
	ring_free(&r);
	close(pfd[0]);
	close(pfd[1]);
	munmap(m, 4096);
}

/* OPENAT2 of path from dirfd with resolve: "fd", or the error. */
static void
openat2_one(struct ring *r, int dirfd, const char *path, int flags,
    uint64_t resolve, const char *step)
{
	struct d_open_how how = {
		.flags = (uint64_t)flags,
		.mode = (flags & O_CREAT) != 0 ? 0644 : 0,
		.resolve = resolve,
	};
	struct io_uring_sqe *s;
	struct io_uring_cqe *c;
	unsigned h;

	s = prep(r, IORING_OP_OPENAT2, dirfd, 1);
	s->addr = (uint64_t)(uintptr_t)path;
	s->len = sizeof(how);
	s->off = (uint64_t)(uintptr_t)&how;
	(void)submit(r);
	await(r, 1, 2000);
	if (ready(r) != 1) {
		printf("%s/%s cqes=%u\n", current, step, ready(r));
		dump(r, step);
		return;
	}
	h = atomic_load_explicit(r->cq_head, memory_order_relaxed);
	c = &r->cqes[h & r->cq_mask];
	if (c->res >= 0) {
		printf("%s/%s fd\n", current, step);
		close(c->res);
	} else {
		printf("%s/%s -%s\n", current, step, errname(-c->res));
	}
	atomic_store_explicit(r->cq_head, h + 1, memory_order_release);
}

/*
 * OPENAT2's resolve flags over a tree with every kind of symlink, from a
 * directory taken as the root, and across mounts from "/".
 */
static void
c_openat2_resolve(void)
{
	static const struct {
		const char	*name;
		uint64_t	resolve;
	} rs[] = {
		{ "none", 0 },
		{ "nosym", D_RESOLVE_NO_SYMLINKS },
		{ "noxdev", D_RESOLVE_NO_XDEV },
		{ "beneath", D_RESOLVE_BENEATH },
		{ "inroot", D_RESOLVE_IN_ROOT },
		{ "inroot_nosym", D_RESOLVE_IN_ROOT | D_RESOLVE_NO_SYMLINKS },
		{ "beneath_nosym", D_RESOLVE_BENEATH | D_RESOLVE_NO_SYMLINKS },
		{ "beneath_inroot", D_RESOLVE_BENEATH | D_RESOLVE_IN_ROOT },
	};
	static const char *paths[] = {
		"a/f", "a/rel", "a/abs", "a/dir/a/f", "a/up", "a/up/a/f",
		"a/../a/f", "../x", "/a/f", "a/f/", "a/", ".", "..", "a/rel/",
		"a/none", "a/dangling", "a/loop", "a/f/x",
	};
	char root[] = "kqdiff.XXXXXX", step[96];
	struct ring r;
	int dfd, devfd;

	CHECK(mkdtemp(root) != NULL);
	dfd = open(root, O_RDONLY | O_DIRECTORY);
	CHECK(dfd >= 0);
	CHECK(mkdirat(dfd, "a", 0755) == 0);
	CHECK(close(openat(dfd, "a/f", O_CREAT | O_WRONLY, 0644)) == 0);
	CHECK(symlinkat("f", dfd, "a/rel") == 0);
	CHECK(symlinkat("/a/f", dfd, "a/abs") == 0);
	CHECK(symlinkat("..", dfd, "a/dir") == 0);
	CHECK(symlinkat("../..", dfd, "a/up") == 0);
	CHECK(symlinkat("nothing", dfd, "a/dangling") == 0);
	CHECK(symlinkat("loop", dfd, "a/loop") == 0);

	ring_init(&r, 8, NULL);
	for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
		for (size_t j = 0; j < sizeof(paths) / sizeof(paths[0]); j++) {
			/* Whether "/" is another mount depends on the host. */
			if (rs[i].resolve == D_RESOLVE_NO_XDEV &&
			    strcmp(paths[j], "a/abs") == 0)
				continue;
			snprintf(step, sizeof(step), "%s:%s", rs[i].name,
			    paths[j]);
			openat2_one(&r, dfd, paths[j], O_RDONLY, rs[i].resolve,
			    step);
		}
	}
	/* Trailing symlinks not followed, and creating through symlinks. */
	openat2_one(&r, dfd, "a/dir/a/rel", O_RDONLY | O_NOFOLLOW | O_PATH,
	    D_RESOLVE_NO_SYMLINKS, "nosym_path_through");
	openat2_one(&r, dfd, "a/up/new1", O_CREAT | O_WRONLY,
	    D_RESOLVE_IN_ROOT, "inroot_create_up");
	say("inroot_create_up_where", "%d", faccessat(dfd, "new1", F_OK,
	    0) == 0);
	openat2_one(&r, dfd, "a/dangling", O_CREAT | O_WRONLY,
	    D_RESOLVE_IN_ROOT, "inroot_create_dangling");
	say("inroot_create_dangling_where", "%d", faccessat(dfd, "a/nothing",
	    F_OK, 0) == 0);
	openat2_one(&r, dfd, "a/dangling", O_CREAT | O_WRONLY,
	    D_RESOLVE_NO_SYMLINKS, "nosym_create_dangling");
	openat2_one(&r, dfd, "a/rel", O_CREAT | O_EXCL | O_WRONLY,
	    D_RESOLVE_IN_ROOT, "inroot_create_excl_link");
	openat2_one(&r, dfd, "a/new2", O_CREAT | O_WRONLY, D_RESOLVE_CACHED,
	    "cached_create");
	openat2_one(&r, dfd, "a/f", O_RDONLY | O_TRUNC, D_RESOLVE_CACHED,
	    "cached_trunc");
	openat2_one(&r, dfd, "a/f", O_RDONLY, D_RESOLVE_CACHED, "cached");
	openat2_one(&r, dfd, "a/f", O_RDONLY | O_DIRECTORY, D_RESOLVE_IN_ROOT,
	    "inroot_directory");
	openat2_one(&r, dfd, "a", O_RDONLY | O_DIRECTORY, D_RESOLVE_IN_ROOT,
	    "inroot_dir");
	openat2_one(&r, dfd, "", O_RDONLY, D_RESOLVE_IN_ROOT, "inroot_empty");
	openat2_one(&r, AT_FDCWD, root, O_RDONLY, D_RESOLVE_IN_ROOT,
	    "inroot_cwd");
	/* From "/": /dev is a mount of its own on both systems. */
	openat2_one(&r, AT_FDCWD, "/dev/null", O_RDONLY, D_RESOLVE_NO_XDEV,
	    "noxdev_dev");
	openat2_one(&r, AT_FDCWD, "/dev/null", O_RDONLY, 0, "dev");
	openat2_one(&r, AT_FDCWD, "/", O_RDONLY, D_RESOLVE_NO_XDEV,
	    "noxdev_root");
	devfd = open("/dev", O_RDONLY | O_DIRECTORY);
	CHECK(devfd >= 0);
	openat2_one(&r, devfd, "..", O_RDONLY, D_RESOLVE_NO_XDEV,
	    "noxdev_dotdot_out");
	close(devfd);
	ring_free(&r);

	(void)unlinkat(dfd, "a/nothing", 0);
	(void)unlinkat(dfd, "new1", 0);
	(void)unlinkat(dfd, "a/new2", 0);
	for (size_t j = 0; j < 6; j++) {
		static const char *ln[] = { "a/rel", "a/abs", "a/dir", "a/up",
		    "a/dangling", "a/loop" };

		CHECK(unlinkat(dfd, ln[j], 0) == 0);
	}
	CHECK(unlinkat(dfd, "a/f", 0) == 0);
	CHECK(unlinkat(dfd, "a", AT_REMOVEDIR) == 0);
	close(dfd);
	CHECK(rmdir(root) == 0);
}

static void *
other_thread_wait(void *arg)
{
	struct ring *r = arg;
	struct __kernel_timespec ts = { .tv_sec = 2 };
	struct io_uring_getevents_arg ga;
	int rc;

	memset(&ga, 0, sizeof(ga));
	ga.ts = (uint64_t)(uintptr_t)&ts;
	rc = ring_sys_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS |
	    IORING_ENTER_EXT_ARG, &ga, sizeof(ga));
	return ((void *)(intptr_t)(rc < 0 ? errno : 0));
}

/*
 * SINGLE_ISSUER: another thread's wait waits, for the owner's CQE or a
 * read's, unless the ring is DEFER_TASKRUN.
 */
static void
c_single_issuer_wait(void)
{
	static const struct {
		const char	*name;
		uint32_t	flags;
	} rings[] = {
		{ "plain", IORING_SETUP_SINGLE_ISSUER },
		{ "coop", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_COOP_TASKRUN },
		{ "defer", IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN },
	};
	struct io_uring_params p;
	struct io_uring_sqe *s;
	struct ring r;
	pthread_t th;
	void *res;
	char step[64], c;
	int pfd[2];

	for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
		memset(&p, 0, sizeof(p));
		p.flags = rings[i].flags;
		ring_init(&r, 8, &p);

		CHECK(pthread_create(&th, NULL, other_thread_wait, &r) == 0);
		pause_ms(50);
		prep(&r, IORING_OP_NOP, -1, 1);
		(void)submit(&r);
		CHECK(pthread_join(th, &res) == 0);
		errno = (int)(intptr_t)res;
		snprintf(step, sizeof(step), "%s_nop", rings[i].name);
		say_rc(step, errno != 0 ? -1 : 0);
		await(&r, 1, 2000);
		dump(&r, step);

		CHECK(pipe(pfd) == 0);
		s = prep(&r, IORING_OP_READ, pfd[0], 2);
		s->addr = (uint64_t)(uintptr_t)&c;
		s->len = 1;
		s->off = (uint64_t)-1;
		(void)submit(&r);
		CHECK(pthread_create(&th, NULL, other_thread_wait, &r) == 0);
		pause_ms(50);
		CHECK(write(pfd[1], "w", 1) == 1);
		CHECK(pthread_join(th, &res) == 0);
		errno = (int)(intptr_t)res;
		snprintf(step, sizeof(step), "%s_read", rings[i].name);
		say_rc(step, errno != 0 ? -1 : 0);
		await(&r, 1, 2000);
		dump(&r, step);
		close(pfd[0]);
		close(pfd[1]);
		ring_free(&r);
	}
}

/*
 * IOSQE_FIXED_FILE on opcodes that take no file: ignored, but a path
 * opcode fails with EBADF.
 */
static void
c_fixed_flag(void)
{
	struct __kernel_timespec ts = { 0, 1000000 };
	struct io_uring_sqe *s;
	struct ring r;
	int files[1], pfd[2];

	ring_init(&r, 8, NULL);
	files[0] = open("/dev/null", O_RDONLY);
	CHECK(files[0] >= 0);
	CHECK(ring_sys_register(r.fd, IORING_REGISTER_FILES, files, 1) == 0);
	s = prep(&r, IORING_OP_ASYNC_CANCEL, 0, 1);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = 12345;
	one(&r, "cancel");
	s = prep(&r, IORING_OP_TIMEOUT, 0, 2);
	s->flags = IOSQE_FIXED_FILE;
	set_timeout(s, &ts, 1);
	one(&r, "timeout");
	res_fd_mask = 1ULL << 3;
	s = prep(&r, IORING_OP_SOCKET, 0, 3);
	s->flags = IOSQE_FIXED_FILE;
	s->fd = AF_UNIX;
	s->off = SOCK_STREAM;
	one(&r, "socket");
	res_fd_mask = 0;
	s = prep(&r, IORING_OP_PIPE, 0, 4);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)pfd;
	one(&r, "pipe");
	close(pfd[0]);
	close(pfd[1]);
	s = prep(&r, IORING_OP_OPENAT, AT_FDCWD, 5);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)"/dev/null";
	one(&r, "openat");
	s = prep(&r, IORING_OP_CLOSE, 0, 6);
	s->flags = IOSQE_FIXED_FILE;
	one(&r, "close");
	s = prep(&r, IORING_OP_MKDIRAT, AT_FDCWD, 7);
	s->flags = IOSQE_FIXED_FILE;
	s->addr = (uint64_t)(uintptr_t)"kqdiff.fixeddir";
	one(&r, "mkdirat");
	(void)rmdir("kqdiff.fixeddir");
	ring_free(&r);
	close(files[0]);
}

/*
 * MSG_RING to the sender's own ring: posted before the request's CQE, but
 * on a DEFER_TASKRUN ring only once a wait runs its task work.
 */
static void
c_msg_ring_self(void)
{
	struct io_uring_params p;
	struct io_uring_sqe *s;
	struct ring r;

	for (int defer = 0; defer < 2; defer++) {
		const char *pfx = defer ? "defer" : "plain";
		char step[32];

		memset(&p, 0, sizeof(p));
		if (defer)
			p.flags = IORING_SETUP_SINGLE_ISSUER |
			    IORING_SETUP_DEFER_TASKRUN;
		ring_init(&r, 8, &p);
		s = prep(&r, IORING_OP_MSG_RING, r.fd, 1);
		s->addr = IORING_MSG_DATA;
		s->len = 9;
		s->off = 2;
		(void)submit(&r);
		snprintf(step, sizeof(step), "%s_ready", pfx);
		say(step, "%d", (int)ready(&r));
		snprintf(step, sizeof(step), "%s_submit", pfx);
		dump(&r, step);
		await(&r, 1, 1000);
		snprintf(step, sizeof(step), "%s_wait", pfx);
		dump(&r, step);
		ring_free(&r);
	}
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
	{ "recv_mshot_drained", c_recv_mshot_drained },
	{ "buffer_lifetime", c_buffer_lifetime },
	{ "buffer_fork", c_buffer_fork },
	{ "buffer_backing", c_buffer_backing },
	{ "cq_overflow", c_cq_overflow },
	{ "fixed_files", c_fixed_files },
	{ "sockets", c_sockets },
	{ "fork", c_fork },
	{ "fork_inflight", c_fork_inflight },
	{ "exit_armed", c_exit_armed },
	{ "exec", c_exec },
	{ "threads", c_threads },
	{ "eventfd", c_eventfd },
	{ "sigmask", c_sigmask },
	{ "abs_timer", c_abs_timer },
	{ "reg_wait", c_reg_wait },
	{ "ext_arg_order", c_ext_arg_order },
	{ "restrictions_split", c_restrictions_split },
	{ "task_restrict", c_task_restrict },
	{ "bpf_filter", c_bpf_filter },
	{ "nop_flags", c_nop_flags },
	{ "openat2_resolve", c_openat2_resolve },
	{ "single_issuer_wait", c_single_issuer_wait },
	{ "fixed_flag", c_fixed_flag },
	{ "msg_ring_self", c_msg_ring_self },
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
