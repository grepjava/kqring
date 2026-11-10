/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * kqring_fuzz: random input for kqring.ko, to find what a malformed
 * request does to the kernel.  Each thread, over and over, sets up a ring
 * with random parameters, and submits random SQEs (random opcodes, flags,
 * descriptors, lengths, offsets and pointers, and links, timeouts and
 * cancellations between them), enters with random flags and arguments,
 * makes random io_uring_register() calls, and now and then closes
 * descriptors under its requests, replaces them with dup2(2), forks a
 * child that pokes at the ring, or closes the ring with requests in
 * flight.
 *
 * Most random requests fail their checks early, so about 60% of rings
 * are set up from known-good templates instead, with a fixed-file table,
 * registered buffers, a buffer ring and a personality, and on them most
 * requests are well formed for their opcode (reads and writes on real
 * descriptors, provided and fixed buffers, multishot receives and polls,
 * timeouts, cancellations and poll removals aimed at recent requests,
 * MSG_RING, futexes, splice; its file is O_DIRECT half the time), bursts of
 * reads of one pipe or FIFO, runs of 64-255 reads of the file, long enough
 * that the ring's reads look cached and run in the enter itself, all
 * chained with links and link timeouts, with
 * data written to the other ends of the pipes and sockets they wait on.
 * Enters wait with signal masks (never blocking the fuzzer's own signals),
 * absolute deadlines and registered waits, in the caller's or the module's
 * memory region, registered before the ring is enabled.
 * Half of those rings (and a few others) opt in to kqring's extensions,
 * sometimes with bad arguments, and get event-driven chains: triggers on
 * those pipes and sockets with random modes and 1-3 templates from the
 * same well-formed requests, or a RECV passing its provided buffer to a
 * SEND, cancelled like any other request.
 *
 * It fails on:
 *  - a kernel panic (run it with debug.debugger_on_panic=0; the dump
 *    names the bug);
 *  - a system call that does not return for 30 s, signals included, or a
 *    teardown (closing everything, reaping children) that takes 120 s;
 *  - a CQ ring that holds more entries than it has room for;
 *  - SIGBUS or SIGSEGV in the fuzzer, which says where and dies;
 *  - its log descriptors no longer being its log, which would hide the rest;
 *  - writes to a file out of order: each thread appends bursts of numbered
 *    records to a file of its own (not on IOPOLL rings, not behind a link,
 *    not on rings whose SQ array it scrambles),
 *    and once every ring is gone, a ring's records must be whole and in the
 *    order it submitted them (cancelled ones may be missing);
 *  - kqring memory, user-wired pages or sockets nothing refers to still
 *    held once everything is closed.
 *
 * The kernel is never given a pointer into the fuzzer's own memory: user
 * pointers point into a scratch arena, into a read-only table of names
 * and socket addresses, at an unmapped page, at NULL or into the kernel,
 * so what the kernel writes can't corrupt the fuzzer.  It runs in a
 * scratch directory of its own, and path names are relative names from
 * the table; sockets are AF_UNIX only, so nothing leaves the host.
 *
 * With -j 1 and -i, a seed replays the same requests (timing aside).
 *
 * usage: kqring_fuzz [-t seconds] [-j threads] [-s seed] [-i iterations]
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <sys/kqring.h>

#include "kqring_leak.h"

#define MAX_THREADS	32
#define ARENA_SIZE	(1U << 20)
#define NAMES_SIZE	4096
#define NPOOL		12
#define NDYN		64		/* descriptors a ring made, to close */
#define NOPS		256		/* opcode and addr per user_data slot */
#define OP_ECHO		0xfd		/* in ops[]: a step sending a passed buffer */
#define OP_PIPERD	0xfc		/* in ops[]: a read of a pipe burst */
#define OP_ORDW		0xfb		/* in ops[]: an ordered write, addr seq */
#define HANG_NS		(30ULL * 1000000000ULL)
#define KICK_NS		(2ULL * 1000000000ULL)
#define REG_RINGS	16

static int		nthreads = 4, run_secs = 60;
static uint64_t		seed;
static long		max_iters;		/* per thread; 0: until -t */
static _Atomic bool	stop;
static char		scratch[64];

static _Atomic uint64_t	st_rings, st_setup_fail, st_sqes, st_enters,
			st_regs, st_cqes, st_chaos, st_forks, st_kicks;
/* Triggers submitted; their per-event and final CQEs; finals not -EINVAL. */
static _Atomic uint64_t	st_trig, st_trig_fire, st_trig_end, st_trig_live;
static _Atomic uint64_t	st_kev;
static _Atomic uint64_t	st_kqb;
/* BUF_FROM_PREV pairs submitted; sends of a buffer that was passed. */
static _Atomic uint64_t	st_echo, st_echo_sent;
/* Scratch files opened with and without O_DIRECT. */
static _Atomic uint64_t	st_direct, st_buffered;
/* New rings closed under us before they could be mapped. */
static _Atomic uint64_t	st_ring_gone;
static _Atomic uint64_t	st_dyn_reserved;	/* refused, see dyn_add() */
static struct stat	out_st, err_st;		/* OUT_FD, ERR_FD at start */
/* Reads in pipe bursts submitted; those that got data. */
static _Atomic uint64_t	st_piperd, st_piperd_data;
/* Reads in file runs submitted. */
static _Atomic uint64_t	st_fileread;
/* Pipe slots opened as FIFOs. */
static _Atomic uint64_t	st_fifo;
static _Atomic int	cq_bad, nfinished;

struct fz {
	int		id;
	pthread_t	thr;
	uint64_t	rng;
	_Atomic uint64_t in_call;	/* ns, 0 when not in one */
	const char *_Atomic call;
	char		*arena;		/* ARENA_SIZE, guard pages around */
	char		*hole;		/* a PROT_NONE page */
	int		pool[NPOOL];
	int		kq;		/* what KQRING_EXT_KQUEUE attaches */
	int		dyn[NDYN];
	int		ndyn;
	struct { uint8_t op; uint64_t addr; } ops[NOPS];
	/* the current ring */
	int		ring;
	struct io_uring_params p;
	char		*sqmap, *cqmap;
	size_t		sqmap_len, cqmap_len;
	char		*sqes;
	size_t		sqes_map_len;	/* if mapped, not NO_MMAP memory */
	unsigned	sqe_size;
	_Atomic unsigned *sq_khead, *sq_ktail, *cq_khead, *cq_ktail;
	unsigned	*sq_array;
	unsigned	sq_tail, sq_mask, cq_mask;
	char		*user_mem[2];	/* NO_MMAP rings */
	char		*rw_page;	/* 2 pages for a caller's region */
	struct io_uring_reg_wait *rw;	/* the ring's wait region, or NULL */
	size_t		rw_size;
	void		*rw_kmap;	/* a module region's mapping */
	size_t		rw_kmap_len;
	uint64_t	ud;
	/*
	 * Structured mode: a ring set up from a known-good template, and
	 * what was registered on it, so requests can be well formed.
	 */
	bool		valid;
	unsigned	nfiles;		/* fixed-file slots */
	unsigned	nbufs;		/* registered buffers, 4 KiB each */
	bool		pbuf;		/* buffer ring, group PBUF_BGID */
	char		*pbuf_page;
	uint16_t	personality;
	uint64_t	recent[16];	/* user_data of recent requests */
	unsigned	nrecent;
	uint8_t		ext_op;		/* escape opcode once opted in, or 0 */
	unsigned	tmpl_left;	/* templates still due after a trigger */
	bool		tmpl_echo;	/* they are a buffer-passing pair */
	int		echo_fd;	/* on this socket */
	unsigned	pipe_left;	/* reads of a burst still due */
	unsigned	file_left;	/* reads of a file run still due */
	int		ord_fd;		/* O_APPEND file of numbered records */
	char		*ord_buf;	/* the records, each written once */
	struct ord_meta	*ord_meta;	/* how each was submitted */
	uint32_t	ord_seq;	/* next record */
	uint32_t	ord_gen;	/* the current ring's number */
	unsigned	ord_left;	/* writes of a burst still due */
	bool		sq_chaos;	/* this ring's SQ array gets random slots */
	uint32_t	ord_cqes;	/* CQEs of them reaped */
	unsigned	batch_room;	/* do_submit()'s room, for ord reports */
	int		pipe_fd;	/* on this pipe */
};

#define PBUF_BGID	7
#define PBUF_N		8
#define REGBUF_OFF	(ARENA_SIZE / 2)	/* registered buffers */
#define PBUF_OFF	(ARENA_SIZE / 2 + 64 * 1024)	/* ring buffers */

static struct fz	fzs[MAX_THREADS];
static _Atomic int	ring_fds[MAX_THREADS];	/* MSG_RING targets */
static char		*names;			/* read-only name table */
static dev_t		kqring_rdev;		/* of /dev/kqring */

/* Offsets into names[]. */
#define NAME_PATHS	0	/* 16 paths, 32 bytes each */
#define NAME_ADDRS	1024	/* 4 struct sockaddr_un, 128 bytes each */
#define NAME_XATTR	1600	/* "user.k" */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/*
 * Descriptors 0-2 are /dev/null while the fuzzer runs, since arena words
 * are often small integers and end up as descriptors (PIPE results, file
 * tables): the output goes to these instead, which are out of reach.
 */
#define OUT_FD		900
#define MAP_FD		800		/* + thread: a ring while it's mapped */
#define ORD_FD		700		/* + thread: its ordered-writes file */
#define ERR_FD		901

static void
stdio_hide(void)
{
	int null;

	if (fcntl(STDOUT_FILENO, F_DUPFD, OUT_FD) != OUT_FD ||
	    fcntl(STDERR_FILENO, F_DUPFD, ERR_FD) != ERR_FD ||
	    (null = open("/dev/null", O_RDWR)) < 0) {
		perror("kqring_fuzz: stdio");
		exit(1);
	}
	fflush(stdout);
	for (int fd = 0; fd <= STDERR_FILENO; fd++)
		(void)dup2(null, fd);
	if (null > STDERR_FILENO)
		close(null);
}

static void
stdio_restore(void)
{
	(void)dup2(OUT_FD, STDOUT_FILENO);
	(void)dup2(ERR_FD, STDERR_FILENO);
}

static void __dead2 __printflike(1, 2)
die(const char *fmt, ...)
{
	va_list ap;

	stdio_restore();
	va_start(ap, fmt);
	fprintf(stderr, "kqring_fuzz: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, " (seed %ju)\n", (uintmax_t)seed);
	va_end(ap);
	exit(1);
}

static uint64_t
rnd(struct fz *f)
{
	uint64_t x = f->rng;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	f->rng = x;
	return (x * 0x2545f4914f6cdd1dULL);
}

static uint64_t
rn(struct fz *f, uint64_t n)		/* [0, n) */
{
	return (n == 0 ? 0 : rnd(f) % n);
}

static bool
chance(struct fz *f, unsigned pct)
{
	return (rn(f, 100) < pct);
}

/* ------------------------------------------------------------------------
 * Values: pointers that are never the fuzzer's own, descriptors, sizes.
 * ------------------------------------------------------------------------
 */

static uint64_t
gen_ptr(struct fz *f)
{
	switch (rn(f, 10)) {
	case 0:
		return (0);
	case 1:
		return ((uint64_t)(uintptr_t)f->hole + rn(f, PAGE_SIZE));
	case 2:
		return (0xfffff80000000000ULL + (rnd(f) & 0xffffffffffULL));
	case 3:		/* just before the arena's end: runs into the guard */
		return ((uint64_t)(uintptr_t)f->arena + ARENA_SIZE -
		    rn(f, 256));
	case 4:
		return ((uint64_t)(uintptr_t)names + rn(f, NAMES_SIZE));
	default:
		return ((uint64_t)(uintptr_t)f->arena +
		    (chance(f, 50) ? rn(f, ARENA_SIZE / 8) * 8 :
		    rn(f, ARENA_SIZE)));
	}
}

static uint64_t
name_path(struct fz *f)
{
	return ((uint64_t)(uintptr_t)names + NAME_PATHS + rn(f, 16) * 32);
}

static int
gen_fd(struct fz *f)
{
	switch (rn(f, 12)) {
	case 0:
		return (-1);
	case 1:
		return ((int)(1024 + rn(f, 1 << 20)));	/* not open */
	case 2:
		return ((int)rnd(f));
	case 3:
		return (f->ndyn > 0 ? f->dyn[rn(f, f->ndyn)] : f->ring);
	case 4:
		return (atomic_load(&ring_fds[rn(f, nthreads)]));
	case 5:
		return (f->ring);
	default:
		return (f->pool[rn(f, NPOOL)]);
	}
}

static uint32_t
gen_len(struct fz *f)
{
	switch (rn(f, 10)) {
	case 0:
		return (0);
	case 1:
		return (UINT32_MAX);
	case 2:
		return ((uint32_t)rnd(f));
	case 3: case 4: case 5:
		return ((uint32_t)rn(f, 65));
	default:
		return ((uint32_t)rn(f, ARENA_SIZE / 4));
	}
}

static uint64_t
gen_off(struct fz *f)
{
	switch (rn(f, 6)) {
	case 0:
		return ((uint64_t)-1);
	case 1:
		return (rnd(f));
	default:
		return (rn(f, 1 << 20));
	}
}

/* Fill the arena: random words, small numbers and pointers into it. */
static void
arena_fill(struct fz *f)
{
	uint64_t *w = (uint64_t *)(void *)f->arena;

	for (size_t i = 0; i < ARENA_SIZE / 8; i++) {
		switch (rn(f, 10)) {
		case 0: case 1: case 2:
			w[i] = rnd(f);
			break;
		case 3: case 4:
			w[i] = rn(f, 64);
			break;
		case 5: case 6:
			w[i] = (uint64_t)(uintptr_t)f->arena +
			    rn(f, ARENA_SIZE / 8) * 8;
			break;
		case 7:
			w[i] = (uint64_t)(uintptr_t)names + rn(f, NAMES_SIZE);
			break;
		default:
			w[i] = 0;
		}
	}
}

static void
names_init(void)
{
	names = mmap(NULL, NAMES_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (names == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	for (int i = 0; i < 16; i++)
		snprintf(names + NAME_PATHS + i * 32, 32, "%s%d",
		    i < 12 ? "f" : "d", i);
	for (int i = 0; i < 4; i++) {
		struct sockaddr_un *sun = (void *)(names + NAME_ADDRS +
		    i * 128);

		sun->sun_family = AF_UNIX;
		sun->sun_len = sizeof(*sun);
		snprintf(sun->sun_path, sizeof(sun->sun_path), "s%d", i);
	}
	strlcpy(names + NAME_XATTR, "user.k", 16);
	if (mprotect(names, NAMES_SIZE, PROT_READ) != 0)
		die("mprotect: %s", strerror(errno));
}

/* ------------------------------------------------------------------------
 * Descriptors the requests act on.
 * ------------------------------------------------------------------------
 */

static int
pool_open(struct fz *f, int i)
{
	char path[32];
	int sv[2], fd = -1;

	/* Slots 0-3 hold one end of a pipe, FIFO or socketpair, 6-9 the other. */
	if (i < 4 && f->pool[i + 6] >= 0) {
		close(f->pool[i + 6]);
		f->pool[i + 6] = -1;
	}
	switch (i) {
	case 0: case 1:
		if (chance(f, 50)) {
			snprintf(path, sizeof(path), "q%d.%d", f->id, i);
			(void)mkfifo(path, 0600);
			fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd >= 0) {
				f->pool[i + 6] = open(path, O_WRONLY |
				    O_NONBLOCK | O_CLOEXEC);
				if (i == 0)
					(void)fcntl(fd, F_SETFL, 0);
				atomic_fetch_add(&st_fifo, 1);
			}
		} else if (pipe2(sv, O_CLOEXEC |
		    (i == 1 ? O_NONBLOCK : 0)) == 0) {
			fd = sv[0];
			f->pool[i + 6] = sv[1];
		}
		break;
	case 2: case 3:
		if (socketpair(AF_UNIX, i == 2 ? SOCK_STREAM : SOCK_DGRAM, 0,
		    sv) == 0) {
			fd = sv[0];
			f->pool[i + 6] = sv[1];
		}
		break;
	}
	/* The fuzzer writes to the other end itself, and must not block. */
	if (i < 4 && f->pool[i + 6] >= 0)
		(void)fcntl(f->pool[i + 6], F_SETFL, O_NONBLOCK);
	switch (i) {
	case 0: case 1: case 2: case 3:
		break;
	case 4:
		snprintf(path, sizeof(path), "t%d", f->id);
		/* O_DIRECT: its writes leave the issuer, as reads do. */
		fd = open(path, O_RDWR | O_CREAT |
		    (chance(f, 50) ? O_DIRECT : 0), 0600);
		atomic_fetch_add(fd >= 0 && (fcntl(fd, F_GETFL) & O_DIRECT) != 0 ?
		    &st_direct : &st_buffered, 1);
		break;
	case 5:
		fd = open(".", O_RDONLY | O_DIRECTORY);
		break;
	case 10:
		fd = eventfd(0, EFD_NONBLOCK);
		break;
	case 11:
		fd = kqueue();
		break;
	default:
		return (f->pool[i]);	/* the other ends, set above */
	}
	return (fd);
}

static void
pool_init(struct fz *f)
{
	for (int i = 0; i < NPOOL; i++)
		f->pool[i] = -1;
	for (int i = 0; i < NPOOL; i++)
		if (i < 6 || i >= 10)
			f->pool[i] = pool_open(f, i);
	f->kq = kqueue();
}

static void
pool_close(struct fz *f)
{
	for (int i = 0; i < NPOOL; i++)
		if (f->pool[i] >= 0) {
			close(f->pool[i]);
			f->pool[i] = -1;
		}
	if (f->kq >= 0) {
		close(f->kq);
		f->kq = -1;
	}
}

static void
dyn_add(struct fz *f, int fd, unsigned op, int64_t res, uint64_t ud)
{
	/* stdio is the log; PIPE fds are read back from the arena. */
	if (fd <= STDERR_FILENO)
		return;
	/*
	 * The fuzzer's own numbers (OUT_FD, ERR_FD, MAP_FD and ORD_FD +
	 * thread) are never taken as a request's: a CQE taken for another
	 * request's, whose user_data slot was reused, would have the fuzzer
	 * close its log, or write into the records of ordered writes.
	 * Others above them are real once the table has grown so far.
	 */
	if (fd == OUT_FD || fd == ERR_FD ||
	    (fd >= MAP_FD && fd < MAP_FD + MAX_THREADS) ||
	    (fd >= ORD_FD && fd < ORD_FD + MAX_THREADS)) {
		if (atomic_fetch_add(&st_dyn_reserved, 1) < 20)
			dprintf(ERR_FD, "kqring_fuzz: thread %d: fd %d from "
			    "op %u res %jd ud %ju not taken as a request's\n",
			    f->id, fd, op, (intmax_t)res, (uintmax_t)ud);
		return;
	}
	if (f->ndyn == NDYN) {
		close(f->dyn[0]);
		memmove(&f->dyn[0], &f->dyn[1], (NDYN - 1) * sizeof(int));
		f->ndyn--;
	}
	f->dyn[f->ndyn++] = fd;
}

static void
dyn_close(struct fz *f)
{
	while (f->ndyn > 0)
		close(f->dyn[--f->ndyn]);
}

/* ------------------------------------------------------------------------
 * The ring.
 * ------------------------------------------------------------------------
 */

#define CALL(f, what, expr) ({						\
	atomic_store(&(f)->call, (what));				\
	atomic_store(&(f)->in_call, now_ns());				\
	__typeof(expr) _r = (expr);					\
	atomic_store(&(f)->in_call, 0);					\
	_r;								\
})

static void
ring_unmap(struct fz *f)
{
	if (f->sqmap != NULL)
		munmap(f->sqmap, f->sqmap_len);
	if (f->cqmap != NULL && f->cqmap != f->sqmap)
		munmap(f->cqmap, f->cqmap_len);
	f->sqmap = f->cqmap = NULL;
	if (f->sqes_map_len != 0)
		munmap(f->sqes, f->sqes_map_len);
	f->sqes_map_len = 0;
	f->sqes = NULL;
	f->sq_khead = f->sq_ktail = f->cq_khead = f->cq_ktail = NULL;
	f->sq_array = NULL;
}

static void
ring_close(struct fz *f)
{
	if (f->ring < 0)
		return;
	ring_unmap(f);
	atomic_store(&ring_fds[f->id], -1);
	(void)CALL(f, "close(ring)", close(f->ring));
	f->ring = -1;
	if (f->rw_kmap != NULL) {
		munmap(f->rw_kmap, f->rw_kmap_len);
		f->rw_kmap = NULL;
	}
	f->rw = NULL;
	for (int i = 0; i < 2; i++)
		if (f->user_mem[i] != NULL) {
			munmap(f->user_mem[i], (2 << 20) + PAGE_SIZE);
			f->user_mem[i] = NULL;
		}
}

/*
 * A descriptor of the new ring to map, out of reach of the numbers requests
 * use, or -1 if it has gone.  Other threads' requests close ring descriptors
 * on purpose, and a file that took the number would map beyond its end, so
 * that the first touch of the SQEs raised SIGBUS here.
 */
static int
ring_map_fd(struct fz *f)
{
	struct stat st;
	int fd = fcntl(f->ring, F_DUPFD_CLOEXEC, MAP_FD + f->id);

	if (fd < 0)
		return (-1);
	if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
	    st.st_rdev != kqring_rdev) {
		close(fd);
		return (-1);
	}
	return (fd);
}

static void
ring_open(struct fz *f)
{
	static const uint32_t good = IORING_SETUP_SQPOLL |
	    IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP |
	    IORING_SETUP_R_DISABLED | IORING_SETUP_SUBMIT_ALL |
	    IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
	    IORING_SETUP_SQE128 | IORING_SETUP_CQE32 |
	    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
	    IORING_SETUP_NO_MMAP | IORING_SETUP_NO_SQARRAY |
	    IORING_SETUP_IOPOLL | IORING_SETUP_ATTACH_WQ |
	    IORING_SETUP_CQE_MIXED | IORING_SETUP_SQE_MIXED |
	    IORING_SETUP_SQ_REWIND;
	struct io_uring_params *p = &f->p;
	unsigned entries;
	char *base;
	long r;

	/* Setups io_uring programs use, plus a few optional flags. */
	static const uint32_t templates[] = {
		IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
		IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
		    IORING_SETUP_TASKRUN_FLAG,
		0,
		IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG,
		IORING_SETUP_SQPOLL,
		IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
		    IORING_SETUP_IOPOLL,
		IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED,
	};
	static const uint32_t extras = IORING_SETUP_CQSIZE |
	    IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SQE128 |
	    IORING_SETUP_CQE32 | IORING_SETUP_NO_SQARRAY |
	    IORING_SETUP_CQE_MIXED | IORING_SETUP_SQE_MIXED;

	memset(p, 0, sizeof(*p));
	f->valid = chance(f, 60);
	if (f->valid) {
		p->flags = templates[rn(f, nitems(templates))] |
		    ((uint32_t)rnd(f) & extras);
		if ((p->flags & IORING_SETUP_CQE32) != 0)
			p->flags &= ~(uint32_t)IORING_SETUP_CQE_MIXED;
		if ((p->flags & IORING_SETUP_SQE128) != 0)
			p->flags &= ~(uint32_t)IORING_SETUP_SQE_MIXED;
		if ((p->flags & IORING_SETUP_SQPOLL) == 0 && chance(f, 10))
			p->flags |= IORING_SETUP_SQ_REWIND |
			    IORING_SETUP_NO_SQARRAY;
		entries = 1U << (1 + rn(f, 8));
		p->cq_entries = entries * (1U << rn(f, 3));
	} else {
		p->flags = (uint32_t)rnd(f) & good;
		if (chance(f, 5))
			p->flags = (uint32_t)rnd(f) &
			    ~(uint32_t)IORING_SETUP_REGISTERED_FD_ONLY;
		if (chance(f, 50))
			p->flags &= ~(uint32_t)(IORING_SETUP_SQPOLL |
			    IORING_SETUP_IOPOLL);
		entries = chance(f, 90) ? 1U << rn(f, 9) : (uint32_t)rnd(f);
		p->cq_entries = chance(f, 80) ? entries * (1U << rn(f, 3)) :
		    (uint32_t)rnd(f);
	}
	p->sq_thread_idle = (uint32_t)rn(f, 50);
	p->sq_thread_cpu = (uint32_t)rn(f, 16);
	p->wq_fd = (uint32_t)gen_fd(f);
	if ((p->flags & IORING_SETUP_NO_MMAP) != 0) {
		/*
		 * With a guard page after each: a ring needing more than we
		 * gave must fail, not reach into the next mapping.
		 */
		for (int i = 0; i < 2; i++) {
			f->user_mem[i] = mmap(NULL, (2 << 20) + PAGE_SIZE,
			    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1,
			    0);
			if (f->user_mem[i] == MAP_FAILED)
				f->user_mem[i] = NULL;
			else if (mprotect(f->user_mem[i] + (2 << 20),
			    PAGE_SIZE, PROT_NONE) != 0)
				die("mprotect: %s", strerror(errno));
		}
		p->sq_off.user_addr = chance(f, 90) ?
		    (uint64_t)(uintptr_t)f->user_mem[0] : gen_ptr(f);
		p->cq_off.user_addr = chance(f, 90) ?
		    (uint64_t)(uintptr_t)f->user_mem[1] : gen_ptr(f);
	}
	r = CALL(f, "kqring_setup", kqring_setup(entries, p));
	if (r < 0) {
		atomic_fetch_add(&st_setup_fail, 1);
		for (int i = 0; i < 2; i++)
			if (f->user_mem[i] != NULL) {
				munmap(f->user_mem[i], (2 << 20) + PAGE_SIZE);
				f->user_mem[i] = NULL;
			}
		return;
	}
	f->ring = (int)r;
	atomic_store(&ring_fds[f->id], f->ring);
	atomic_fetch_add(&st_rings, 1);

	f->sqe_size = (p->flags & IORING_SETUP_SQE128) != 0 ? 128 : 64;
	size_t cqe = (p->flags & IORING_SETUP_CQE32) != 0 ? 32 : 16;
	if ((p->flags & IORING_SETUP_NO_MMAP) != 0) {
		/* Ours, and big enough for what we touch, or left alone. */
		if (f->user_mem[0] == NULL || f->user_mem[1] == NULL ||
		    p->sq_off.user_addr != (uint64_t)(uintptr_t)f->user_mem[0] ||
		    p->cq_off.user_addr != (uint64_t)(uintptr_t)f->user_mem[1] ||
		    (size_t)p->sq_entries * f->sqe_size > (2 << 20) ||
		    (size_t)p->cq_off.cqes + (size_t)p->cq_entries * cqe >
		    (2 << 20) || (size_t)p->sq_off.array +
		    (size_t)p->sq_entries * sizeof(unsigned) > (2 << 20))
			return;
		base = f->user_mem[1];
		f->sqes = f->user_mem[0];
	} else {
		f->sqmap_len = MAX((size_t)p->sq_off.array +
		    p->sq_entries * sizeof(unsigned),
		    (size_t)p->cq_off.cqes + p->cq_entries * cqe);
		int mfd = ring_map_fd(f);

		if (mfd < 0) {
			atomic_fetch_add(&st_ring_gone, 1);
			return;
		}
		base = mmap(NULL, f->sqmap_len, PROT_READ | PROT_WRITE,
		    MAP_SHARED, mfd, IORING_OFF_SQ_RING);
		if (base == MAP_FAILED) {
			close(mfd);
			return;
		}
		/*
		 * Another thread's new ring can take the number between the
		 * setup and the dup: its rings aren't the ones p describes,
		 * and its CQ would look overrun here.
		 */
		if (*(unsigned *)(void *)(base + p->sq_off.ring_entries) !=
		    p->sq_entries ||
		    *(unsigned *)(void *)(base + p->cq_off.ring_entries) !=
		    p->cq_entries) {
			munmap(base, f->sqmap_len);
			close(mfd);
			atomic_fetch_add(&st_ring_gone, 1);
			return;
		}
		f->sqmap = f->cqmap = base;
		f->sqes = mmap(NULL, (size_t)p->sq_entries * f->sqe_size,
		    PROT_READ | PROT_WRITE, MAP_SHARED, mfd, IORING_OFF_SQES);
		close(mfd);
		if (f->sqes == MAP_FAILED) {
			f->sqes = NULL;
			return;
		}
		f->sqes_map_len = (size_t)p->sq_entries * f->sqe_size;
	}
	f->sq_khead = (void *)(base + p->sq_off.head);
	f->sq_ktail = (void *)(base + p->sq_off.tail);
	f->sq_mask = *(unsigned *)(void *)(base + p->sq_off.ring_mask);
	f->sq_array = (p->flags & IORING_SETUP_NO_SQARRAY) != 0 ? NULL :
	    (void *)(base + p->sq_off.array);
	f->cq_khead = (void *)(base + p->cq_off.head);
	f->cq_ktail = (void *)(base + p->cq_off.tail);
	f->cq_mask = *(unsigned *)(void *)(base + p->cq_off.ring_mask);
	f->sq_tail = atomic_load(f->sq_ktail);

	/* kqring's extensions: opt in (sometimes badly, sometimes twice). */
	f->ext_op = 0;
	f->tmpl_left = 0;
	f->tmpl_echo = false;
	f->ord_left = 0;
	f->ord_gen++;
	/*
	 * Half the rings get a random SQ array entry now and then, which runs
	 * whatever SQE is in that slot, again or early: no ordered writes on
	 * them, whose order that would rightly break.
	 */
	f->sq_chaos = chance(f, 50);
	if (chance(f, f->valid ? 50 : 10)) {
		struct kqring_ext_setup es = { .version = KQRING_EXT_VERSION };

		if (chance(f, 5))
			es.version = (uint32_t)rnd(f);
		if (chance(f, 15))
			es.ext_opcode = (uint8_t)rnd(f);
		if (chance(f, 3))
			es.resv[0] = rnd(f);
		if (CALL(f, "EXT_SETUP", ioctl(f->ring, KQRING_IOC_EXT_SETUP,
		    &es)) == 0)
			f->ext_op = es.ext_opcode;
		if (chance(f, 5))
			(void)ioctl(f->ring, KQRING_IOC_EXT_SETUP, &es);
	}
}

static int	pool_fd(struct fz *f, int kind);
static void	maybe_fixed(struct fz *f, struct io_uring_sqe *s);
static void	sanitize(struct fz *f, struct io_uring_sqe *s);

/*
 * A trigger SQE (KQRING_EXT_TRIGGER) in s, whose `k` templates are the
 * next SQEs; its struct in the arena, mostly well formed.
 */
static void gen_kevent(struct fz *f, struct kevent *k);

static void
sqe_trigger(struct fz *f, struct io_uring_sqe *s, unsigned k)
{
	struct kqring_trigger *t = (void *)(f->arena +
	    rn(f, REGBUF_OFF / 64) * 64);
	static const uint32_t evs[] = { POLLIN, POLLOUT, POLLIN | POLLOUT,
	    POLLIN | POLLRDHUP, POLLPRI };
	static const uint8_t steps[] = { KQRING_STEP_LEN_FROM_PREV,
	    KQRING_STEP_STOP_ON_ZERO,
	    KQRING_STEP_LEN_FROM_PREV | KQRING_STEP_STOP_ON_ZERO,
	    KQRING_STEP_BUF_FROM_PREV,
	    KQRING_STEP_BUF_FROM_PREV | KQRING_STEP_STOP_ON_ZERO };

	memset(t, 0, sizeof(*t));
	t->nsteps = chance(f, 95) ? (uint8_t)k : (uint8_t)rn(f, 10);
	t->flags = (uint32_t)rn(f, 16);	/* CONCURRENT .. CANCEL_RUNNING */
	/* KQFUZZ_NO_CANCEL_RUNNING: leave it out (narrowing down a crash). */
	if (getenv("KQFUZZ_NO_CANCEL_RUNNING") != NULL)
		t->flags &= ~KQRING_TRIG_CANCEL_RUNNING;
	if ((t->flags & KQRING_TRIG_CONCURRENT) != 0)
		t->max_inflight = (uint16_t)(1 + rn(f, 8));
	if (chance(f, 3))
		t->max_inflight = (uint16_t)rnd(f);
	if (chance(f, 3))
		t->flags = (uint32_t)rnd(f);
	/* Valid step flags often (refused on steps that have no len). */
	for (unsigned i = 1; i < k && i < KQRING_TRIG_MAX_STEPS; i++) {
		if (chance(f, 40))
			t->step_flags[i] = steps[rn(f, nitems(steps))];
	}
	if (chance(f, 2))
		t->step_flags[rn(f, 8)] = (uint8_t)rnd(f);
	if (chance(f, 2))
		t->source = (uint8_t)rnd(f);
	if (f->tmpl_echo) {
		t->nsteps = 2;
		memset(t->step_flags, 0, sizeof(t->step_flags));
		t->step_flags[1] = KQRING_STEP_BUF_FROM_PREV |
		    (chance(f, 50) ? KQRING_STEP_STOP_ON_ZERO : 0);
		atomic_fetch_add(&st_echo, 1);
	}
	/* Sometimes a kqueue event instead of readiness. */
	if (chance(f, 25)) {
		struct kevent *kv = (void *)(f->arena +
		    rn(f, REGBUF_OFF / 64) * 64);

		gen_kevent(f, kv);
		t->source = KQRING_TRIG_SRC_KEVENT;
		t->kev = (uint64_t)(uintptr_t)kv;
	}

	memset(s, 0, f->sqe_size);
	s->opcode = f->ext_op;
	s->ioprio = chance(f, 97) ? KQRING_EXT_TRIGGER : (uint16_t)rnd(f);
	s->fd = pool_fd(f, chance(f, 50) ? 1 : chance(f, 80) ? 2 : 3);
	if (f->tmpl_echo)
		s->fd = f->echo_fd = pool_fd(f, 2);
	maybe_fixed(f, s);
	s->poll32_events = f->tmpl_echo ? POLLIN : evs[rn(f, nitems(evs))];
	s->addr = chance(f, 97) ? (uint64_t)(uintptr_t)t : gen_ptr(f);
	s->len = chance(f, 97) ? sizeof(*t) : gen_len(f);
	s->flags |= chance(f, 90) ? IOSQE_IO_LINK : IOSQE_IO_HARDLINK;
	if (t->source == KQRING_TRIG_SRC_KEVENT && chance(f, 95)) {
		s->fd = -1;
		s->poll32_events = 0;
		s->flags &= ~IOSQE_FIXED_FILE;
	}
	s->user_data = f->ud;
	sanitize(f, s);
	atomic_fetch_add(&st_trig, 1);
	f->recent[f->nrecent++ % nitems(f->recent)] = f->ud;
	f->ops[f->ud % NOPS].op = s->opcode;
	f->ops[f->ud % NOPS].addr = s->addr;
	f->ud++;
}

/*
 * The templates of a trigger passing a provided buffer: a RECV or READ
 * that selects one, then the SEND or WRITE that takes it, in place of the
 * request sqe_fill() put in s.  Random templates hardly ever make this.
 */
static void
sqe_echo(struct fz *f, struct io_uring_sqe *s, bool first)
{
	uint64_t ud = s->user_data;

	memset(s, 0, f->sqe_size);
	s->user_data = ud;
	s->fd = f->echo_fd;
	if (first) {
		s->opcode = chance(f, 70) ? IORING_OP_RECV : IORING_OP_READ;
		s->flags = IOSQE_BUFFER_SELECT;
		s->buf_group = PBUF_BGID;
		f->ops[ud % NOPS].op = s->opcode;
	} else {
		s->opcode = chance(f, 70) ? IORING_OP_SEND : IORING_OP_WRITE;
		if (s->opcode == IORING_OP_WRITE)
			s->off = (uint64_t)-1;
		else if (chance(f, 50))
			s->msg_flags = MSG_WAITALL;
		f->ops[ud % NOPS].op = OP_ECHO;
	}
	f->ops[ud % NOPS].addr = 0;
}

/*
 * A kqueue bridge (KQRING_EXT_KQUEUE) of f's kqueue, with a few events
 * registered on it as other code would: user events, and pool descriptors
 * level-triggered or not.  Their udata is this request's, which the CQE
 * checks know.
 */
static void
sqe_kqueue(struct fz *f, struct io_uring_sqe *s)
{
	void *ud = (void *)(uintptr_t)f->ud;
	struct kevent k;

	for (unsigned n = (unsigned)rn(f, 4); n > 0 && f->kq >= 0; n--) {
		if (chance(f, 50))
			EV_SET(&k, rn(f, 64), EVFILT_USER, EV_ADD |
			    (chance(f, 70) ? EV_CLEAR : EV_ONESHOT), 0, 0, ud);
		else
			EV_SET(&k, pool_fd(f, 3), chance(f, 70) ? EVFILT_READ :
			    EVFILT_WRITE, EV_ADD | (chance(f, 50) ? EV_CLEAR :
			    0), 0, 0, ud);
		(void)kevent(f->kq, &k, 1, NULL, 0, NULL);
		/* NOTE_TRIGGER does nothing along with EV_ADD. */
		if (k.filter == EVFILT_USER && chance(f, 70)) {
			k.flags = 0;
			k.fflags = NOTE_TRIGGER;
			(void)kevent(f->kq, &k, 1, NULL, 0, NULL);
		}
	}
	memset(s, 0, f->sqe_size);
	s->opcode = f->ext_op;
	s->ioprio = KQRING_EXT_KQUEUE;
	s->fd = chance(f, 90) ? f->kq : pool_fd(f, 3);
	s->addr = chance(f, 97) ? 0 : gen_ptr(f);
	s->user_data = f->ud;
	atomic_fetch_add(&st_kqb, 1);
	f->recent[f->nrecent++ % nitems(f->recent)] = f->ud;
	/* Its CQEs aren't a trigger's. */
	f->ops[f->ud % NOPS].op = IORING_OP_NOP;
	f->ops[f->ud % NOPS].addr = s->addr;
	f->ud++;
}

/*
 * A kevent request (KQRING_EXT_KEVENT): a file's vnode events, a signal
 * that is ignored by default, or this process, its parent or a random
 * pid (the fuzzer forks, so NOTE_FORK on itself fires).  Mostly valid.
 */
static void
gen_kevent(struct fz *f, struct kevent *k)
{
	static const int sigs[] = { SIGURG, SIGWINCH, SIGINFO, SIGCHLD };

	memset(k, 0, sizeof(*k));
	switch (rn(f, 4)) {
	case 3:
		/* KQFUZZ_NO_KEVCOUNT: leave byte counts out. */
		if (getenv("KQFUZZ_NO_KEVCOUNT") != NULL) {
			EV_SET(k, SIGURG, EVFILT_SIGNAL, 0, 0, 0, NULL);
			break;
		}
		EV_SET(k, pool_fd(f, 3), chance(f, 60) ? EVFILT_READ :
		    EVFILT_WRITE, 0, 0, 0, NULL);
		break;
	case 0:
		EV_SET(k, pool_fd(f, chance(f, 70) ? 0 : 3), EVFILT_VNODE, 0,
		    1U + (uint32_t)rn(f, 0x7ff), 0, NULL);
		break;
	case 1:
		EV_SET(k, sigs[rn(f, nitems(sigs))], EVFILT_SIGNAL, 0, 0, 0,
		    NULL);
		break;
	default:
		EV_SET(k, chance(f, 40) ? getpid() : chance(f, 50) ?
		    getppid() : (pid_t)rn(f, 100000), EVFILT_PROC, 0,
		    (chance(f, 50) ? NOTE_EXIT : 0) | (chance(f, 50) ?
		    NOTE_FORK : 0) | NOTE_EXEC, 0, NULL);
		break;
	}
	if (chance(f, 5))
		k->filter = (short)rnd(f);
	if (chance(f, 3))
		k->flags = (unsigned short)rnd(f);
	if (chance(f, 3))
		k->fflags = (unsigned)rnd(f);
}

static void
sqe_kevent(struct fz *f, struct io_uring_sqe *s)
{
	struct kevent *k = (void *)(f->arena + rn(f, REGBUF_OFF / 64) * 64);

	gen_kevent(f, k);
	memset(s, 0, f->sqe_size);
	s->opcode = f->ext_op;
	s->ioprio = KQRING_EXT_KEVENT;
	s->fd = chance(f, 97) ? -1 : pool_fd(f, 3);
	s->addr = chance(f, 97) ? (uint64_t)(uintptr_t)k : gen_ptr(f);
	s->len = chance(f, 97) ? sizeof(*k) : gen_len(f);
	s->rw_flags = chance(f, 50) ? KQRING_KEV_MULTISHOT :
	    chance(f, 95) ? 0 : (uint32_t)rnd(f);
	s->user_data = f->ud;
	atomic_fetch_add(&st_kev, 1);
	f->recent[f->nrecent++ % nitems(f->recent)] = f->ud;
	/* Its CQEs aren't a trigger's. */
	f->ops[f->ud % NOPS].op = IORING_OP_NOP;
	f->ops[f->ud % NOPS].addr = s->addr;
	f->ud++;
}

/*
 * Keep a request's pointers away from anything but the arena and names,
 * its paths relative, and its sockets local; and CLOSE to descriptors of
 * this thread's requests, never the fuzzer's own.
 */
static void
sanitize(struct fz *f, struct io_uring_sqe *s)
{
	switch (s->opcode) {
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
	case IORING_OP_STATX:
	case IORING_OP_UNLINKAT:
	case IORING_OP_MKDIRAT:
		s->addr = name_path(f);
		s->fd = chance(f, 50) ? AT_FDCWD : f->pool[5];
		break;
	case IORING_OP_RENAMEAT:
	case IORING_OP_LINKAT:
	case IORING_OP_SYMLINKAT:
		s->addr = name_path(f);
		s->addr2 = name_path(f);
		s->fd = chance(f, 50) ? AT_FDCWD : f->pool[5];
		if (s->opcode != IORING_OP_SYMLINKAT)
			s->len = (uint32_t)(chance(f, 50) ? AT_FDCWD :
			    f->pool[5]);
		break;
	case IORING_OP_SETXATTR:
	case IORING_OP_GETXATTR:
		s->addr3 = name_path(f);
		/* FALLTHROUGH */
	case IORING_OP_FSETXATTR:
	case IORING_OP_FGETXATTR:
		if (chance(f, 70))
			s->addr = (uint64_t)(uintptr_t)names + NAME_XATTR;
		break;
	case IORING_OP_BIND:
	case IORING_OP_CONNECT:
		if (chance(f, 80)) {
			s->addr = (uint64_t)(uintptr_t)names + NAME_ADDRS +
			    rn(f, 4) * 128;
			s->addr2 = sizeof(struct sockaddr_un);
		}
		break;
	case IORING_OP_SOCKET:
		s->fd = AF_UNIX;
		break;
	case IORING_OP_CLOSE:
		s->fd = f->ndyn > 0 && chance(f, 80) ? f->dyn[rn(f, f->ndyn)] :
		    (int)(1024 + rn(f, 1 << 20));
		break;
	case IORING_OP_FALLOCATE:
	case IORING_OP_FTRUNCATE:
		s->off = rn(f, 1 << 24);	/* no disk filling */
		break;
	case IORING_OP_MADVISE: {
		/*
		 * madvise(2) acts on the address space itself, guard pages or
		 * not: keep it inside the arena, or MADV_FREE would empty the
		 * fuzzer's own heap.
		 */
		uint64_t o = rn(f, ARENA_SIZE);

		s->addr = (uint64_t)(uintptr_t)f->arena + o;
		s->len = (uint32_t)rn(f, ARENA_SIZE - o + 1);
		break;
	}
	}
	if (s->opcode == IORING_OP_WRITE || s->opcode == IORING_OP_WRITEV ||
	    s->opcode == IORING_OP_WRITE_FIXED || s->opcode == IORING_OP_SPLICE)
		s->off = rn(f, 1 << 24);
}

/*
 * Structured mode: register what well-formed requests use (a fixed-file
 * table of the pool, buffers, a buffer ring, a personality), each of
 * which may fail (RLIMIT_MEMLOCK, a disabled ring) and is then not used.
 */
/*
 * A memory region for registered waits on a ring not yet enabled: the
 * thread's pages or the module's, mapped; now and then a bad one.
 */
static void
wait_region(struct fz *f)
{
	struct io_uring_region_desc rd;
	struct io_uring_mem_region_reg mr;
	bool user = chance(f, 50);

	memset(&rd, 0, sizeof(rd));
	memset(&mr, 0, sizeof(mr));
	rd.size = (1 + rn(f, 2)) * PAGE_SIZE;
	if (user) {
		rd.flags = IORING_MEM_REGION_TYPE_USER;
		rd.user_addr = (uint64_t)(uintptr_t)f->rw_page;
	}
	mr.region_uptr = (uint64_t)(uintptr_t)&rd;
	mr.flags = IORING_MEM_REGION_REG_WAIT_ARG;
	if (chance(f, 10)) {
		switch (rn(f, 5)) {
		case 0: rd.user_addr = gen_ptr(f); break;
		case 1: rd.size = rnd(f); break;
		case 2: rd.flags = (uint32_t)rnd(f); break;
		case 3: mr.flags = rnd(f); break;
		default: mr.region_uptr = gen_ptr(f); break;
		}
	}
	if (CALL(f, "mem_region", kqring_register(f->ring,
	    IORING_REGISTER_MEM_REGION, &mr, 1)) != 0 ||
	    mr.flags != IORING_MEM_REGION_REG_WAIT_ARG)
		return;
	if (user) {
		f->rw = (void *)f->rw_page;
	} else {
		void *m = mmap(NULL, rd.size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, f->ring, (off_t)rd.mmap_offset);

		if (m == MAP_FAILED)
			return;
		f->rw_kmap = m;
		f->rw_kmap_len = rd.size;
		f->rw = m;
	}
	f->rw_size = rd.size;
}

static void
ring_resources(struct fz *f)
{
	int fds[16];
	struct iovec iov[4];
	struct io_uring_buf_reg reg;
	struct io_uring_buf *b = (void *)f->pbuf_page;

	f->nfiles = f->nbufs = 0;
	f->pbuf = false;
	f->personality = 0;
	f->nrecent = 0;
	if ((f->p.flags & IORING_SETUP_R_DISABLED) != 0 && chance(f, 50))
		wait_region(f);
	if ((f->p.flags & IORING_SETUP_R_DISABLED) != 0 && chance(f, 90))
		(void)CALL(f, "enable", kqring_register(f->ring,
		    IORING_REGISTER_ENABLE_RINGS, NULL, 0));
	for (int i = 0; i < 16; i++)
		fds[i] = i < NPOOL ? f->pool[i] : -1;
	if (CALL(f, "files", kqring_register(f->ring, IORING_REGISTER_FILES,
	    fds, 16)) == 0)
		f->nfiles = 16;
	for (int i = 0; i < 4; i++) {
		iov[i].iov_base = f->arena + REGBUF_OFF + i * 8192;
		iov[i].iov_len = 4096;
	}
	if (CALL(f, "buffers", kqring_register(f->ring,
	    IORING_REGISTER_BUFFERS, iov, 4)) == 0)
		f->nbufs = 4;
	/* The pool's eventfd as the CQ's, which polls on it then watch. */
	if (chance(f, 50) && f->pool[10] >= 0)
		(void)CALL(f, "eventfd", kqring_register(f->ring,
		    IORING_REGISTER_EVENTFD, &f->pool[10], 1));
	memset(f->pbuf_page, 0, PAGE_SIZE);
	memset(&reg, 0, sizeof(reg));
	reg.ring_addr = (uint64_t)(uintptr_t)f->pbuf_page;
	reg.ring_entries = PBUF_N;
	reg.bgid = PBUF_BGID;
	if (chance(f, 50)) {
		reg.flags = IOU_PBUF_RING_INC;
		if (chance(f, 50))
			reg.min_left = (uint32_t)rn(f, 512);
	}
	if (CALL(f, "pbuf", kqring_register(f->ring, IORING_REGISTER_PBUF_RING,
	    &reg, 1)) == 0) {
		for (int i = 0; i < PBUF_N; i++) {
			b[i].addr = (uint64_t)(uintptr_t)f->arena + PBUF_OFF +
			    i * 4096;
			b[i].len = 4096;
			b[i].bid = (uint16_t)i;
		}
		atomic_store_explicit((_Atomic uint16_t *)(void *)
		    &((struct io_uring_buf_ring *)(void *)f->pbuf_page)->tail,
		    PBUF_N, memory_order_release);
		f->pbuf = true;
	}
	long id = CALL(f, "personality", kqring_register(f->ring,
	    IORING_REGISTER_PERSONALITY, NULL, 0));
	if (id > 0)
		f->personality = (uint16_t)id;
}

/* Give the buffer ring its buffers back (all of them, again). */
static void
pbuf_refill(struct fz *f)
{
	struct io_uring_buf_ring *br = (void *)f->pbuf_page;
	uint16_t tail;

	if (!f->pbuf)
		return;
	tail = atomic_load_explicit((_Atomic uint16_t *)(void *)&br->tail,
	    memory_order_relaxed);
	for (int i = 0; i < PBUF_N; i++) {
		struct io_uring_buf *b = &br->bufs[(tail + i) & (PBUF_N - 1)];

		b->addr = (uint64_t)(uintptr_t)f->arena + PBUF_OFF + i * 4096;
		b->len = 4096;
		b->bid = (uint16_t)i;
	}
	atomic_store_explicit((_Atomic uint16_t *)(void *)&br->tail,
	    (uint16_t)(tail + PBUF_N), memory_order_release);
}

static uint64_t
recent_ud(struct fz *f)
{
	return (f->nrecent == 0 ? rnd(f) :
	    f->recent[rn(f, MIN(f->nrecent, nitems(f->recent)))]);
}

static struct __kernel_timespec *
short_ts(struct fz *f)
{
	struct __kernel_timespec *ts = (void *)(f->arena +
	    rn(f, REGBUF_OFF / 16) * 16);

	ts->tv_sec = 0;
	ts->tv_nsec = (long long)rn(f, 3000000);
	return (ts);
}

/* An arena buffer of n bytes that stays inside the arena. */
static uint64_t
arena_buf(struct fz *f, uint32_t n)
{
	return ((uint64_t)(uintptr_t)f->arena +
	    rn(f, (ARENA_SIZE - n) / 8) * 8);
}

/* A pool descriptor of a kind: 0 file, 1 pipe read, 2 socket, 3 any. */
static int
pool_fd(struct fz *f, int kind)
{
	static const int files[] = { 4 }, pipes[] = { 0, 1 },
	    socks[] = { 2, 3, 8, 9 };

	switch (kind) {
	case 0:
		return (f->pool[files[0]]);
	case 1:
		return (f->pool[pipes[rn(f, 2)]]);
	case 2:
		return (f->pool[socks[rn(f, 4)]]);
	default:
		return (f->pool[rn(f, NPOOL)]);
	}
}

/*
 * Ordered writes: records of 16 bytes, appended by bursts of WRITEs on
 * workers to the thread's file, never in a chain (a linked write runs after
 * the ones submitted after it, rightly).  Each record has its own place in
 * ord_buf, so a write still running after its ring closed reads what it was
 * given.  ord_check() reads the files back.
 */
#define ORD_MAX		65536
#define ORD_MAGIC	0x4f524445U	/* "ORDE" */

struct ord_rec {
	uint32_t	magic;
	uint32_t	id;
	uint32_t	gen;
	uint32_t	seq;
};

/* For a report of records out of order. */
struct ord_meta {
	uint64_t	ud;
	uint32_t	sq_tail;	/* where in the SQ */
	uint32_t	ring_flags;
	uint8_t		flags;		/* the SQE's */
	uint8_t		before;		/* the SQE's before it in the SQ */
	uint8_t		first;		/* first of its do_submit() */
	uint32_t	sq_head;	/* the kernel's, at prep */
	uint32_t	sq_entries;	/* f->p's */
	uint32_t	room;		/* do_submit()'s */
	uint32_t	cqe_at;		/* its CQE was the thread's nth, or 0 */
	int32_t		res;
};

static _Atomic uint64_t	st_ord;

static bool
sqe_ord_write(struct fz *f, struct io_uring_sqe *s, struct io_uring_sqe *prev)
{
	uint64_t ud = s->user_data;
	struct ord_rec *r;

	if (f->ord_fd < 0 || f->ord_seq >= ORD_MAX)
		return (false);
	/*
	 * The SQE before this one goes in with it if the kernel hasn't taken
	 * it yet (an enter may submit fewer than were queued), and a link on
	 * it then puts this write behind its chain, as on Linux.  Within this
	 * batch that's prev, cleared below; before it, the ring's last slot.
	 * An SQPOLL thread may be reading it, so then don't write at all.
	 */
	if (prev == NULL && atomic_load(f->sq_khead) != f->sq_tail) {
		struct io_uring_sqe *b = (struct io_uring_sqe *)(void *)
		    (f->sqes + (size_t)((f->sq_tail - 1) & f->sq_mask) *
		    f->sqe_size);

		if ((b->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) != 0) {
			if ((f->p.flags & IORING_SETUP_SQPOLL) != 0)
				return (false);
			b->flags &= ~(IOSQE_IO_LINK | IOSQE_IO_HARDLINK);
		}
	}
	r = (struct ord_rec *)(void *)f->ord_buf + f->ord_seq;
	{
		struct ord_meta *m = &f->ord_meta[f->ord_seq];
		struct io_uring_sqe *b = (struct io_uring_sqe *)(void *)
		    (f->sqes + (size_t)((f->sq_tail - 1) & f->sq_mask) *
		    f->sqe_size);

		m->ud = ud;
		m->sq_tail = f->sq_tail;
		m->sq_head = atomic_load(f->sq_khead);
		m->sq_entries = f->p.sq_entries;
		m->room = f->batch_room;
		m->cqe_at = 0;
		m->ring_flags = f->p.flags;
		m->before = b->flags;
		m->first = prev == NULL;
	}
	r->magic = ORD_MAGIC;
	r->id = (uint32_t)f->id;
	r->gen = f->ord_gen;
	r->seq = f->ord_seq++;
	if (prev != NULL)
		prev->flags &= ~(IOSQE_IO_LINK | IOSQE_IO_HARDLINK);
	memset(s, 0, f->sqe_size);
	s->user_data = ud;
	s->opcode = IORING_OP_WRITE;
	s->fd = f->ord_fd;
	/* At an offset too: O_APPEND appends it all the same. */
	s->off = chance(f, 30) ? rn(f, 1 << 20) : (uint64_t)-1;
	s->addr = (uint64_t)(uintptr_t)r;
	s->len = sizeof(*r);
	if (chance(f, 30))
		s->flags = IOSQE_ASYNC;
	f->ord_meta[r->seq].flags = s->flags;
	f->ops[ud % NOPS].op = OP_ORDW;
	f->ops[ud % NOPS].addr = r->seq;
	atomic_fetch_add(&st_ord, 1);
	return (true);
}

/*
 * One of a burst of reads of a pipe, in place of the request sqe_fill() put
 * in s: they wait on workers together and take turns at the data, which
 * random requests hardly ever do.
 */
static void
sqe_pipe_read(struct fz *f, struct io_uring_sqe *s)
{
	uint64_t ud = s->user_data;

	memset(s, 0, f->sqe_size);
	s->user_data = ud;
	s->opcode = IORING_OP_READ;
	s->fd = f->pipe_fd;
	s->off = (uint64_t)-1;
	s->len = 1 + (uint32_t)rn(f, 64);
	if (f->pbuf && chance(f, 50)) {
		s->flags = IOSQE_BUFFER_SELECT;
		s->buf_group = PBUF_BGID;
	} else {
		s->addr = arena_buf(f, s->len);
	}
	f->ops[ud % NOPS].op = OP_PIPERD;
	f->ops[ud % NOPS].addr = 0;
	atomic_fetch_add(&st_piperd, 1);
}

/*
 * One of a run of reads of the thread's scratch file, in place of the
 * request sqe_fill() put in s.  Enough of them in a row, with nothing that
 * sleeps on the workers between, make the ring's reads look cached, and
 * then, with the CPUs taken, they run in the enter itself
 * (kqring_wq_try_inline()) while the rest of the fuzzer cancels, closes
 * and forks around them.
 */
static void
sqe_file_read(struct fz *f, struct io_uring_sqe *s)
{
	uint64_t ud = s->user_data;

	memset(s, 0, f->sqe_size);
	s->user_data = ud;
	s->opcode = IORING_OP_READ;
	s->fd = pool_fd(f, 0);
	s->off = rn(f, 64 * 1024);
	s->len = 1 + (uint32_t)rn(f, 4096);
	if (f->pbuf && chance(f, 25)) {
		s->flags = IOSQE_BUFFER_SELECT;
		s->buf_group = PBUF_BGID;
		s->len = MIN(s->len, 64);
	} else {
		s->addr = arena_buf(f, s->len);
	}
	maybe_fixed(f, s);
	f->ops[ud % NOPS].op = IORING_OP_READ;
	f->ops[ud % NOPS].addr = s->addr;
	atomic_fetch_add(&st_fileread, 1);
}

/* Use the fixed-file slot of pool slot i instead, if there is one. */
static void
maybe_fixed(struct fz *f, struct io_uring_sqe *s)
{
	for (int i = 0; i < NPOOL; i++)
		if (f->nfiles != 0 && f->pool[i] == s->fd && chance(f, 30)) {
			s->fd = i;
			s->flags |= IOSQE_FIXED_FILE;
			return;
		}
}

/*
 * A well-formed request of a random kind, on real descriptors, buffers
 * and earlier requests.
 */
static void
sqe_struct(struct fz *f, struct io_uring_sqe *s)
{
	uint32_t n = 1 + (uint32_t)rn(f, 4096);
	struct iovec *iov;
	struct msghdr *mh;

	memset(s, 0, f->sqe_size);
	switch (rn(f, 24)) {
	case 0:
		s->opcode = IORING_OP_NOP;
		break;
	case 1: case 2:
		s->opcode = chance(f, 50) ? IORING_OP_READ : IORING_OP_WRITE;
		s->fd = pool_fd(f, (int)rn(f, 4));
		s->addr = arena_buf(f, n);
		s->len = n;
		s->off = chance(f, 30) ? (uint64_t)-1 : rn(f, 65536);
		if (s->opcode == IORING_OP_READ && f->pbuf && chance(f, 30)) {
			s->flags |= IOSQE_BUFFER_SELECT;
			s->buf_group = PBUF_BGID;
			s->addr = 0;
		}
		maybe_fixed(f, s);
		break;
	case 3:
		if (f->nbufs == 0)
			goto nop;
		s->opcode = chance(f, 50) ? IORING_OP_READ_FIXED :
		    IORING_OP_WRITE_FIXED;
		s->buf_index = (uint16_t)rn(f, f->nbufs);
		s->fd = pool_fd(f, (int)rn(f, 2));
		s->addr = (uint64_t)(uintptr_t)f->arena + REGBUF_OFF +
		    s->buf_index * 8192 + rn(f, 2048);
		s->len = (uint32_t)rn(f, 2049);
		s->off = rn(f, 65536);
		maybe_fixed(f, s);
		break;
	case 4: {
		int cnt = 1 + (int)rn(f, 4);

		s->opcode = chance(f, 50) ? IORING_OP_READV : IORING_OP_WRITEV;
		iov = (void *)(uintptr_t)arena_buf(f, 64);
		for (int i = 0; i < cnt; i++) {
			iov[i].iov_len = (size_t)rn(f, 2048);
			iov[i].iov_base = (void *)(uintptr_t)arena_buf(f,
			    (uint32_t)iov[i].iov_len);
		}
		s->fd = pool_fd(f, (int)rn(f, 4));
		s->addr = (uint64_t)(uintptr_t)iov;
		s->len = (uint32_t)cnt;
		s->off = rn(f, 65536);
		break;
	}
	case 5: case 6:
		s->opcode = chance(f, 50) ? IORING_OP_RECV : IORING_OP_SEND;
		s->fd = pool_fd(f, 2);
		s->addr = arena_buf(f, n);
		s->len = n;
		s->msg_flags = chance(f, 70) ? 0 : (uint32_t)rn(f, 0x200);
		if (s->opcode == IORING_OP_RECV && f->pbuf && chance(f, 40)) {
			s->flags |= IOSQE_BUFFER_SELECT;
			s->buf_group = PBUF_BGID;
			s->addr = 0;
			s->len = 0;
			if (chance(f, 50)) {
				s->ioprio = IORING_RECV_MULTISHOT;
				/* A cap per run, and on the total. */
				if (chance(f, 40))
					s->len = 1U << rn(f, 14);
				if (chance(f, 30))
					s->optlen = (uint32_t)rn(f, 16384);
			}
			if (chance(f, 30))
				s->ioprio |= IORING_RECVSEND_BUNDLE;
		}
		if (chance(f, 20))
			s->ioprio |= IORING_RECVSEND_POLL_FIRST;
		maybe_fixed(f, s);
		break;
	case 7:
		s->opcode = chance(f, 50) ? IORING_OP_RECVMSG :
		    IORING_OP_SENDMSG;
		mh = (void *)(uintptr_t)arena_buf(f, 128);
		iov = (void *)(uintptr_t)arena_buf(f, 32);
		memset(mh, 0, sizeof(*mh));
		iov[0].iov_len = n;
		iov[0].iov_base = (void *)(uintptr_t)arena_buf(f, n);
		mh->msg_iov = iov;
		mh->msg_iovlen = 1;
		if (chance(f, 30)) {
			mh->msg_control = (void *)(uintptr_t)arena_buf(f, 256);
			mh->msg_controllen = (socklen_t)rn(f, 256);
		}
		s->fd = pool_fd(f, 2);
		s->addr = (uint64_t)(uintptr_t)mh;
		break;
	case 8:
		s->opcode = IORING_OP_POLL_ADD;
		s->fd = chance(f, 10) ? f->pool[10] : pool_fd(f, 3);
		s->poll32_events = chance(f, 60) ? POLLIN : chance(f, 50) ?
		    POLLOUT : (uint32_t)rn(f, 0x200);
		if (chance(f, 40))
			s->len = IORING_POLL_ADD_MULTI;
		if (chance(f, 3))
			s->len |= IORING_POLL_ADD_LEVEL;	/* EINVAL */
		maybe_fixed(f, s);
		break;
	case 9:
		s->opcode = IORING_OP_POLL_REMOVE;
		s->addr = recent_ud(f);
		break;
	case 10:
		s->opcode = IORING_OP_TIMEOUT;
		s->addr = (uint64_t)(uintptr_t)short_ts(f);
		s->len = 1;
		s->off = rn(f, 4);
		s->timeout_flags = (uint32_t)rn(f, 0x40);
		break;
	case 11:
		s->opcode = IORING_OP_TIMEOUT_REMOVE;
		s->addr = recent_ud(f);
		if (chance(f, 50)) {
			s->timeout_flags = IORING_TIMEOUT_UPDATE;
			s->addr2 = (uint64_t)(uintptr_t)short_ts(f);
		}
		break;
	case 12:
		s->opcode = IORING_OP_ASYNC_CANCEL;
		s->addr = recent_ud(f);
		s->cancel_flags = (uint32_t)rn(f, 8) & (IORING_ASYNC_CANCEL_ALL |
		    IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ANY);
		s->fd = pool_fd(f, 3);
		break;
	case 13:
		s->opcode = chance(f, 50) ? IORING_OP_FSYNC :
		    IORING_OP_FTRUNCATE;
		s->fd = pool_fd(f, 0);
		s->off = rn(f, 1 << 20);
		maybe_fixed(f, s);
		break;
	case 14:
		s->opcode = IORING_OP_SPLICE;
		s->splice_fd_in = pool_fd(f, 1);
		s->fd = chance(f, 50) ? f->pool[6 + rn(f, 2)] : pool_fd(f, 0);
		s->len = n;
		s->splice_off_in = (uint64_t)-1;
		s->off = chance(f, 50) ? (uint64_t)-1 : rn(f, 65536);
		break;
	case 15:
		s->opcode = IORING_OP_MSG_RING;
		s->fd = chance(f, 70) ? atomic_load(&ring_fds[rn(f, nthreads)]) :
		    f->ring;
		s->addr = chance(f, 80) ? IORING_MSG_DATA : IORING_MSG_SEND_FD;
		s->len = (uint32_t)rnd(f);
		s->off = rnd(f);
		s->addr3 = rn(f, 16);
		s->file_index = (uint32_t)rn(f, 17);
		break;
	case 16: {
		/* Mostly a few shared words, for wakes to find their waits. */
		static uint32_t words[4];
		uint32_t *w = (void *)(uintptr_t)arena_buf(f, 4);

		w = (uint32_t *)((uintptr_t)w & ~(uintptr_t)3);
		if (chance(f, 80))
			w = &words[rn(f, nitems(words))];
		s->opcode = chance(f, 50) ? IORING_OP_FUTEX_WAIT :
		    IORING_OP_FUTEX_WAKE;
		s->addr = (uint64_t)(uintptr_t)w;
		s->addr2 = s->opcode == IORING_OP_FUTEX_WAIT ? *w : rn(f, 4);
		s->addr3 = chance(f, 90) ? UINT32_MAX : ~0ULL;
		s->fd = FUTEX2_SIZE_U32;
		break;
	}
	case 17:
		if (f->nfiles == 0)
			goto nop;
		s->opcode = IORING_OP_FILES_UPDATE;
		{
			int *fds = (void *)(uintptr_t)arena_buf(f, 16);

			for (int i = 0; i < 4; i++)
				fds[i] = chance(f, 70) ? pool_fd(f, 3) : -1;
			s->addr = (uint64_t)(uintptr_t)fds;
		}
		s->len = 1 + (uint32_t)rn(f, 4);
		s->off = rn(f, 16);
		break;
	case 18:
		if (!f->pbuf)
			goto nop;
		s->opcode = IORING_OP_READ_MULTISHOT;
		s->fd = pool_fd(f, 1);
		s->flags = IOSQE_BUFFER_SELECT;
		s->buf_group = PBUF_BGID;
		break;
	case 19:
		s->opcode = IORING_OP_SEND_ZC;
		s->fd = pool_fd(f, 2);
		s->addr = arena_buf(f, n);
		s->len = n;
		if (f->nbufs != 0 && chance(f, 50)) {
			s->ioprio = IORING_RECVSEND_FIXED_BUF;
			s->buf_index = (uint16_t)rn(f, f->nbufs);
			s->addr = (uint64_t)(uintptr_t)f->arena + REGBUF_OFF +
			    s->buf_index * 8192;
			s->len = (uint32_t)rn(f, 4097);
		}
		break;
	case 20:
		s->opcode = IORING_OP_SHUTDOWN;
		s->fd = pool_fd(f, 2);
		s->len = (uint32_t)rn(f, 3);
		break;
	case 21:
		if (f->nfiles == 0)
			goto nop;
		s->opcode = IORING_OP_FIXED_FD_INSTALL;
		s->fd = (int)rn(f, 16);
		s->flags = IOSQE_FIXED_FILE;
		break;
	case 22:
		s->opcode = chance(f, 50) ? IORING_OP_OPENAT : IORING_OP_STATX;
		s->open_flags = O_RDWR | (chance(f, 50) ? O_CREAT : 0);
		s->len = 0600;
		s->addr2 = arena_buf(f, 512);	/* STATX buffer */
		break;			/* paths: sanitize() */
	default:
	nop:
		s->opcode = IORING_OP_NOP;
	}
	if (f->personality != 0 && chance(f, 10))
		s->personality = f->personality;
	if (chance(f, 10))
		s->flags |= IOSQE_ASYNC;
	if (chance(f, 5))
		s->flags |= IOSQE_CQE_SKIP_SUCCESS;
	if (chance(f, 3))
		s->flags |= IOSQE_IO_DRAIN;
	s->user_data = f->ud;
	f->recent[f->nrecent++ % nitems(f->recent)] = f->ud;
	sanitize(f, s);
	f->ops[f->ud % NOPS].op = s->opcode;
	f->ops[f->ud % NOPS].addr = s->addr;
	f->ud++;
}

static void
sqe_fill(struct fz *f, struct io_uring_sqe *s)
{
	if (f->valid && chance(f, 60)) {
		sqe_struct(f, s);
		return;
	}
	memset(s, 0, f->sqe_size);
	if (chance(f, 85))
		s->opcode = (uint8_t)rn(f, IORING_OP_LAST);
	else
		s->opcode = (uint8_t)rnd(f);
	s->flags = (uint8_t)(chance(f, 60) ? 0 : chance(f, 90) ?
	    rn(f, 0x80) : rnd(f));
	s->ioprio = (uint16_t)(chance(f, 70) ? 0 : rnd(f));
	s->fd = gen_fd(f);
	s->off = chance(f, 20) ? gen_ptr(f) : gen_off(f);
	s->addr = gen_ptr(f);
	s->len = gen_len(f);
	s->rw_flags = (uint32_t)(chance(f, 50) ? 0 : chance(f, 50) ?
	    rn(f, 64) : rnd(f));
	s->user_data = f->ud;
	s->buf_index = (uint16_t)(chance(f, 80) ? rn(f, 16) : rnd(f));
	s->personality = (uint16_t)(chance(f, 85) ? 0 : rn(f, 8));
	s->file_index = (uint32_t)(chance(f, 70) ? rn(f, 32) : rnd(f));
	s->addr3 = gen_ptr(f);
	if (s->opcode == IORING_OP_TIMEOUT ||
	    s->opcode == IORING_OP_LINK_TIMEOUT) {
		/* Mostly short: a long one only waits for ring close. */
		struct __kernel_timespec *ts = (void *)(f->arena +
		    rn(f, ARENA_SIZE / 16) * 16);

		if (chance(f, 80)) {
			ts->tv_sec = 0;
			ts->tv_nsec = (long long)rn(f, 2000000);
			s->addr = (uint64_t)(uintptr_t)ts;
			s->len = 1;
		}
	}
	if (f->sqe_size == 128 && chance(f, 50))
		for (int i = 64; i < 128; i += 8)
			*(uint64_t *)(void *)((char *)s + i) = gen_ptr(f);
	sanitize(f, s);
	f->ops[f->ud % NOPS].op = s->opcode;
	f->ops[f->ud % NOPS].addr = s->addr;
	f->ud++;
}

/* Take every CQE, check the ring's indices, and keep made descriptors. */
static void
reap(struct fz *f)
{
	unsigned head, tail;

	if (f->cq_ktail == NULL)
		return;
	head = atomic_load_explicit(f->cq_khead, memory_order_relaxed);
	tail = atomic_load_explicit(f->cq_ktail, memory_order_acquire);
	if (tail - head > f->p.cq_entries &&
	    (f->p.flags & IORING_SETUP_NO_MMAP) == 0) {
		dprintf(ERR_FD, "kqring_fuzz: thread %d: CQ holds %u of %u "
		    "entries\n", f->id, tail - head, f->p.cq_entries);
		atomic_store(&cq_bad, 1);
		head = tail;
	}
	size_t cqe = (f->p.flags & IORING_SETUP_CQE32) != 0 ? 32 : 16;
	char *cqes = (char *)f->cq_ktail - f->p.cq_off.tail + f->p.cq_off.cqes;

	for (; head != tail; head++) {
		struct io_uring_cqe *c = (void *)(cqes +
		    (size_t)(head & f->cq_mask) * cqe);
		uint8_t op;

		/*
		 * CQE_MIXED: a SKIP pads the last slot; a 32-byte CQE takes
		 * two, never the last and the first, and is never cut off by
		 * the tail.
		 */
		if ((f->p.flags & IORING_SETUP_CQE_MIXED) != 0) {
			if ((c->flags & IORING_CQE_F_SKIP) != 0) {
				if ((head & f->cq_mask) != f->cq_mask ||
				    (c->flags & IORING_CQE_F_32) != 0) {
					dprintf(ERR_FD, "kqring_fuzz: thread "
					    "%d: SKIP CQE at %u (flags %#x)\n",
					    f->id, head, c->flags);
					atomic_store(&cq_bad, 1);
				}
				continue;
			}
			if ((c->flags & IORING_CQE_F_32) != 0) {
				if ((head & f->cq_mask) == f->cq_mask ||
				    head + 1 == tail) {
					dprintf(ERR_FD, "kqring_fuzz: thread "
					    "%d: 32-byte CQE at %u of %u, tail "
					    "%u\n", f->id, head & f->cq_mask,
					    f->p.cq_entries, tail);
					atomic_store(&cq_bad, 1);
				} else
					head++;
			}
		}
		op = f->ops[c->user_data % NOPS].op;

		atomic_fetch_add(&st_cqes, 1);
		if (op == OP_ECHO && c->res > 0)
			atomic_fetch_add(&st_echo_sent, 1);
		if (op == OP_PIPERD && c->res > 0)
			atomic_fetch_add(&st_piperd_data, 1);
		if (op == OP_ORDW) {
			uint64_t q = f->ops[c->user_data % NOPS].addr;

			if (q < ORD_MAX && f->ord_meta[q].ud == c->user_data) {
				f->ord_meta[q].cqe_at = ++f->ord_cqes;
				f->ord_meta[q].res = c->res;
			}
		}
		if (f->ext_op != 0 && op == f->ext_op) {
			if ((c->flags & IORING_CQE_F_MORE) != 0)
				atomic_fetch_add(&st_trig_fire, 1);
			else {
				atomic_fetch_add(&st_trig_end, 1);
				if (c->res != -EINVAL)
					atomic_fetch_add(&st_trig_live, 1);
			}
			continue;
		}
		if (c->res < 0)
			continue;
		switch (op) {
		case IORING_OP_OPENAT:
		case IORING_OP_OPENAT2:
		case IORING_OP_SOCKET:
		case IORING_OP_ACCEPT:
		case IORING_OP_FIXED_FD_INSTALL:
			if (c->res > 2)
				dyn_add(f, c->res, op, c->res, c->user_data);
			break;
		case IORING_OP_PIPE: {
			uint64_t a = f->ops[c->user_data % NOPS].addr;

			if (a >= (uint64_t)(uintptr_t)f->arena && a + 8 <=
			    (uint64_t)(uintptr_t)f->arena + ARENA_SIZE) {
				dyn_add(f, ((int *)(uintptr_t)a)[0], op, c->res,
				    c->user_data);
				dyn_add(f, ((int *)(uintptr_t)a)[1], op, c->res,
				    c->user_data);
			}
			break;
		}
		}
	}
	atomic_store_explicit(f->cq_khead, head, memory_order_release);
}

static void
do_submit(struct fz *f)
{
	unsigned n, room, submit;

	if (f->sqes != NULL && f->sq_ktail != NULL) {
		/* SQ_REWIND: every enter reads from slot 0. */
		if ((f->p.flags & IORING_SETUP_SQ_REWIND) != 0)
			f->sq_tail = 0;
		room = f->p.sq_entries -
		    (f->sq_tail - atomic_load(f->sq_khead));
		/* One draw: MIN() would take a second, past the room. */
		n = 1 + (unsigned)rn(f, MIN(f->p.sq_entries, 16));
		n = MIN(room, n);
		f->batch_room = room;
		if (f->p.sq_entries == 0 || room > f->p.sq_entries)
			n = 0;
		struct io_uring_sqe *prev = NULL;

		for (unsigned i = 0; i < n; i++) {
			unsigned idx = f->sq_tail & f->sq_mask;
			struct io_uring_sqe *s = (void *)(f->sqes +
			    (size_t)idx * f->sqe_size);

			if (f->sq_array != NULL)
				f->sq_array[idx] = !f->sq_chaos ||
				    chance(f, 98) ? idx :
				    (unsigned)rnd(f);
			/* An event-driven chain: a trigger, then templates. */
			if (f->ext_op != 0 && f->tmpl_left == 0 &&
			    i + 1 < n && chance(f, f->valid ? 8 : 2)) {
				unsigned k = 1 + (unsigned)rn(f,
				    MIN(3, n - i - 1));

				f->tmpl_echo = f->pbuf && i + 2 < n &&
				    chance(f, 30);
				if (f->tmpl_echo)
					k = 2;
				sqe_trigger(f, s, k);
				f->tmpl_left = k;
				prev = s;
				f->sq_tail++;
				continue;
			}
			if (f->ext_op != 0 && f->tmpl_left == 0 &&
			    chance(f, f->valid ? 4 : 1)) {
				sqe_kevent(f, s);
				prev = s;
				f->sq_tail++;
				continue;
			}
			if (f->ext_op != 0 && f->tmpl_left == 0 &&
			    chance(f, f->valid ? 2 : 1)) {
				sqe_kqueue(f, s);
				prev = s;
				f->sq_tail++;
				continue;
			}
			sqe_fill(f, s);
			if (f->valid && f->tmpl_left == 0 &&
			    f->pipe_left == 0 && chance(f, 3)) {
				f->pipe_left = 2 + (unsigned)rn(f, 5);
				f->pipe_fd = pool_fd(f, 1);
			}
			if (f->valid && f->tmpl_left == 0 &&
			    f->pipe_left == 0 && f->file_left == 0 &&
			    f->ord_left == 0 && f->pool[4] >= 0 &&
			    (f->p.flags & IORING_SETUP_IOPOLL) == 0 &&
			    rn(f, 1000) < 15)
				f->file_left = 64 + (unsigned)rn(f, 192);
			if (f->pipe_left > 0 && f->tmpl_left == 0) {
				sqe_pipe_read(f, s);
				f->pipe_left--;
			} else if (f->file_left > 0 && f->tmpl_left == 0) {
				/* Most of the run; a few random ones between. */
				if (chance(f, 95))
					sqe_file_read(f, s);
				f->file_left--;
			} else if (f->valid && f->tmpl_left == 0 &&
			    !f->sq_chaos &&
			    (f->p.flags & (IORING_SETUP_IOPOLL |
			    IORING_SETUP_SQE_MIXED |
			    IORING_SETUP_SQ_REWIND)) == 0) {
				if (f->ord_left == 0 && rn(f, 1000) < 3)
					f->ord_left = 2 + (unsigned)rn(f, 7);
				if (f->ord_left > 0) {
					f->ord_left--;
					(void)sqe_ord_write(f, s, prev);
				}
			}
			/*
			 * Chains: link to the next request, and sometimes
			 * make that a LINK_TIMEOUT of this one.
			 */
			if (f->valid && prev != NULL &&
			    (prev->flags & (IOSQE_IO_LINK |
			    IOSQE_IO_HARDLINK)) != 0 && chance(f, 25)) {
				uint64_t ud = s->user_data;

				memset(s, 0, f->sqe_size);
				s->opcode = IORING_OP_LINK_TIMEOUT;
				s->addr = (uint64_t)(uintptr_t)short_ts(f);
				s->len = 1;
				s->user_data = ud;
				f->ops[ud % NOPS].op = s->opcode;
			}
			if (f->valid && i + 1 < n && chance(f, 25))
				s->flags |= chance(f, 80) ? IOSQE_IO_LINK :
				    IOSQE_IO_HARDLINK;
			if (f->tmpl_left > 0) {
				/*
				 * A template runs again on every event: never
				 * a CLOSE, whose descriptor number may be
				 * reused, or a request that makes one, whose
				 * CQEs come back as the trigger's and would
				 * fill the table (a socket that stays readable
				 * made 100,000 in minutes).  Linked up to the
				 * last, mostly.
				 */
				switch (s->opcode) {
				case IORING_OP_CLOSE:
				case IORING_OP_OPENAT:
				case IORING_OP_OPENAT2:
				case IORING_OP_SOCKET:
				case IORING_OP_ACCEPT:
				case IORING_OP_FIXED_FD_INSTALL:
				case IORING_OP_PIPE:
					s->opcode = IORING_OP_NOP;
					break;
				}
				if (f->tmpl_echo)
					sqe_echo(f, s, f->tmpl_left == 2);
				if (--f->tmpl_left > 0)
					s->flags |= IOSQE_IO_LINK;
				else {
					f->tmpl_echo = false;
					if (chance(f, 95))
						s->flags &= ~(IOSQE_IO_LINK |
						    IOSQE_IO_HARDLINK);
				}
			}
			prev = s;
			f->sq_tail++;
			/*
			 * SQE_MIXED: a 128-byte SQE's last 64 bytes are the
			 * next slot.  A NOP there, for when the kernel reads
			 * it as an SQE (the first half was in the last slot).
			 */
			if ((f->p.flags & IORING_SETUP_SQE_MIXED) != 0 &&
			    (s->opcode == IORING_OP_NOP128 ||
			    s->opcode == IORING_OP_URING_CMD128) && i + 1 < n) {
				unsigned idx2 = f->sq_tail & f->sq_mask;
				struct io_uring_sqe *s2 = (void *)(f->sqes +
				    (size_t)idx2 * f->sqe_size);

				if (f->sq_array != NULL)
					f->sq_array[idx2] = idx2;
				memset(s2, 0, sizeof(*s2));
				s2->opcode = IORING_OP_NOP;
				s2->user_data = f->ud;
				f->ops[f->ud % NOPS].op = IORING_OP_NOP;
				f->ud++;
				f->sq_tail++;
				i++;
			}
		}
		/* Something for reads, receives and polls to find. */
		if (f->valid && chance(f, 40)) {
			struct pollfd pw = { .fd = f->pool[6 + rn(f, 4)],
			    .events = POLLOUT };

			/*
			 * dup2() chaos can put a blocking file behind the
			 * number: write only where there's room, and as a CALL
			 * so the watchdog's kick ends it if the ring fills it.
			 */
			if (pw.fd >= 0 && poll(&pw, 1, 0) == 1 &&
			    (pw.revents & POLLOUT) != 0)
				(void)CALL(f, "write(pool)", write(pw.fd,
				    f->arena + rn(f, 4096),
				    1 + (size_t)rn(f, 512)));
		}
		if (chance(f, 10))
			pbuf_refill(f);
		atomic_store_explicit(f->sq_ktail, f->sq_tail,
		    memory_order_release);
		atomic_fetch_add(&st_sqes, n);
		submit = chance(f, 90) ? n : (unsigned)rn(f, 2 * n + 2);
	} else
		submit = (unsigned)rn(f, 8);

	unsigned flags = 0, min = 0;
	struct __kernel_timespec ts = { 0, (long long)rn(f, 2000000) };
	struct io_uring_getevents_arg ga = { .ts = (uint64_t)(uintptr_t)&ts };
	void *arg = NULL;
	size_t argsz = 0;

	if (chance(f, 60)) {
		flags |= IORING_ENTER_GETEVENTS;
		min = (unsigned)rn(f, 4);
	}
	if (chance(f, 10))
		flags |= IORING_ENTER_SQ_WAKEUP;
	if (chance(f, 5))
		flags |= IORING_ENTER_SQ_WAIT;
	if (min > 0 || chance(f, 20)) {
		/* A wait is bounded (a 2 s signal is the backstop). */
		flags |= IORING_ENTER_EXT_ARG;
		ga.min_wait_usec = chance(f, 80) ? 0 : (uint32_t)rn(f, 500);
		arg = &ga;
		argsz = sizeof(ga);
		if (chance(f, 5)) {	/* a bad argument instead */
			arg = (void *)(uintptr_t)gen_ptr(f);
			argsz = (size_t)rn(f, 64);
			min = (unsigned)rn(f, 2);
		}
	}
	/*
	 * A signal mask: the thread's, with more blocked, never the
	 * watchdog's SIGUSR1 or the faults it reports.
	 */
	sigset_t mask;
	if (chance(f, 10)) {
		uint64_t bits = rnd(f);

		pthread_sigmask(SIG_SETMASK, NULL, &mask);
		for (int s = 1; s < 64; s++)
			if ((bits & (1ULL << s)) != 0)
				sigaddset(&mask, s);
		sigdelset(&mask, SIGUSR1);
		sigdelset(&mask, SIGBUS);
		sigdelset(&mask, SIGSEGV);
		if ((flags & IORING_ENTER_EXT_ARG) != 0) {
			ga.sigmask = (uint64_t)(uintptr_t)&mask;
			ga.sigmask_sz = chance(f, 90) ? sizeof(mask) :
			    (uint32_t)rn(f, 32);
		} else if (arg == NULL) {
			arg = &mask;
			argsz = chance(f, 90) ? sizeof(mask) : (size_t)rn(f, 32);
		}
	}
	/* A deadline: soon, or long past (the duration taken as a time). */
	if (chance(f, 5)) {
		flags |= IORING_ENTER_ABS_TIMER;
		if (chance(f, 50)) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			ts.tv_sec = now.tv_sec;
			ts.tv_nsec = now.tv_nsec;
		}
	}
	/* A registered wait, by its offset in the region. */
	if ((flags & IORING_ENTER_EXT_ARG) != 0 && f->rw != NULL &&
	    chance(f, 40)) {
		size_t nw = f->rw_size / sizeof(*f->rw);
		uint64_t off = chance(f, 90) ? rn(f, nw) * sizeof(*f->rw) :
		    rnd(f) % (f->rw_size + 128);
		uint64_t at = (off % f->rw_size) & ~(uint64_t)7;
		struct io_uring_reg_wait *w;

		if (at > f->rw_size - sizeof(*w))
			at = f->rw_size - sizeof(*w);
		w = (void *)((char *)f->rw + at);

		memset(w, 0, sizeof(*w));
		w->ts.tv_nsec = (long long)rn(f, 2000000);
		w->flags = chance(f, 97) ? IORING_REG_WAIT_TS :
		    (uint32_t)rnd(f);
		w->min_wait_usec = chance(f, 80) ? 0 : (uint32_t)rn(f, 500);
		if (ga.sigmask != 0) {
			w->sigmask = ga.sigmask;
			w->sigmask_sz = ga.sigmask_sz;
		}
		if ((flags & IORING_ENTER_ABS_TIMER) != 0 && chance(f, 50)) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			w->ts.tv_sec = now.tv_sec;
			w->ts.tv_nsec = now.tv_nsec;
		}
		flags |= IORING_ENTER_EXT_ARG_REG;
		arg = (void *)(uintptr_t)off;
		argsz = chance(f, 95) ? sizeof(*w) : (size_t)rn(f, 128);
	}
	if (chance(f, 3))
		flags = (unsigned)rnd(f) & ~(unsigned)IORING_ENTER_REGISTERED_RING;
	(void)CALL(f, "kqring_enter", kqring_enter(f->ring, submit, min, flags,
	    arg, argsz));
	atomic_fetch_add(&st_enters, 1);
	reap(f);
}

static void
do_register(struct fz *f)
{
	unsigned op = chance(f, 90) ? (unsigned)rn(f, IORING_REGISTER_LAST) :
	    (unsigned)rnd(f);
	void *arg = (void *)(uintptr_t)gen_ptr(f);
	unsigned nr = (unsigned)(chance(f, 80) ? rn(f, 17) : rnd(f));
	struct io_uring_params rp;
	long r;

	if (op == IORING_REGISTER_ENABLE_RINGS || op == IORING_REGISTER_PROBE ||
	    chance(f, 5))
		nr = op == IORING_REGISTER_PROBE ? (unsigned)rn(f, 300) : 0;
	/* A resize that can work, under whatever the ring has in flight. */
	if ((f->p.flags & IORING_SETUP_DEFER_TASKRUN) != 0 && chance(f, 10)) {
		memset(&rp, 0, sizeof(rp));
		rp.sq_entries = 1U << (1 + rn(f, 6));
		rp.cq_entries = rp.sq_entries * 2;
		rp.flags = IORING_SETUP_CQSIZE;
		op = IORING_REGISTER_RESIZE_RINGS;
		arg = &rp;
		nr = 1;
	}
	/* A synchronous cancel, mostly with a short timeout. */
	if (chance(f, 8)) {
		struct io_uring_sync_cancel_reg sc;
		static const uint8_t ops[] = { IORING_OP_READ, IORING_OP_RECV,
		    IORING_OP_POLL_ADD, IORING_OP_ACCEPT, IORING_OP_TIMEOUT };

		memset(&sc, 0, sizeof(sc));
		sc.flags = (uint32_t)rn(f, 8) & (IORING_ASYNC_CANCEL_ALL |
		    IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ANY);
		if (chance(f, 40)) {
			sc.flags |= IORING_ASYNC_CANCEL_OP;
			sc.opcode = ops[rn(f, sizeof(ops))];
		}
		sc.fd = f->pool[rn(f, NPOOL)];
		sc.addr = rn(f, 64);
		if (chance(f, 5)) {
			sc.timeout.tv_sec = -1;
			sc.timeout.tv_nsec = -1;
		} else
			sc.timeout.tv_nsec = (long long)rn(f, 1000000);
		op = IORING_REGISTER_SYNC_CANCEL;
		arg = &sc;
		nr = 1;
		(void)CALL(f, "sync_cancel", kqring_register(f->ring, op, arg,
		    nr));
		atomic_fetch_add(&st_regs, 1);
		return;
	}
	/* Without a ring: a message to this one, or anything at all. */
	if (chance(f, 3)) {
		struct io_uring_sqe ms;

		memset(&ms, 0, sizeof(ms));
		ms.opcode = IORING_OP_MSG_RING;
		ms.fd = f->ring;
		ms.addr = IORING_MSG_DATA;
		ms.off = rnd(f);
		ms.len = (uint32_t)rnd(f);
		if (chance(f, 70)) {
			op = IORING_REGISTER_SEND_MSG_RING;
			arg = &ms;
			nr = 1;
		}
		(void)CALL(f, "register(-1)", kqring_register(-1, op, arg, nr));
		atomic_fetch_add(&st_regs, 1);
		return;
	}
	r = CALL(f, "kqring_register", kqring_register(f->ring, op, arg, nr));
	atomic_fetch_add(&st_regs, 1);
	/* A resized ring is mapped anew; this one just closes. */
	if (r == 0 && op == IORING_REGISTER_RESIZE_RINGS)
		ring_close(f);
}

static void
do_chaos(struct fz *f)
{
	atomic_fetch_add(&st_chaos, 1);
	switch (rn(f, 5)) {
	case 0: {	/* close a descriptor under its requests, reopen it */
		int i = (int)rn(f, NPOOL);

		if (f->pool[i] >= 0)
			(void)CALL(f, "close(pool)", close(f->pool[i]));
		f->pool[i] = -1;
		if (i < 6 || i >= 10)
			f->pool[i] = pool_open(f, i);
		break;
	}
	case 1: {	/* another file behind the same number */
		int a = f->pool[rn(f, NPOOL)], b = f->pool[rn(f, NPOOL)];

		if (a >= 0 && b >= 0 && a != b)
			(void)CALL(f, "dup2", dup2(a, b));
		break;
	}
	case 2:		/* unmap and remap arena pages under requests */
		(void)CALL(f, "munmap", munmap(f->arena +
		    rn(f, ARENA_SIZE / PAGE_SIZE) * PAGE_SIZE, PAGE_SIZE));
		if (mmap(f->arena, ARENA_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
			die("mmap arena: %s", strerror(errno));
		arena_fill(f);
		break;
	case 3: {	/* a child pokes at the ring: takes it over, or EEXIST */
		pid_t pid = fork();

		if (pid == 0) {
			struct io_uring_params p;

			(void)kqring_enter(f->ring, 1, 0, 0, NULL, 0);
			/*
			 * Nothing to submit: a wait, which another process
			 * may make on an SQPOLL ring or one without
			 * SINGLE_ISSUER, for as long as it takes.
			 */
			alarm(1);
			(void)kqring_enter(f->ring, 0, (unsigned)rn(f, 2),
			    chance(f, 50) ? IORING_ENTER_GETEVENTS : 0, NULL, 0);
			alarm(0);
			(void)kqring_register(f->ring, IORING_REGISTER_PROBE,
			    f->arena, 0);
			/* Sometimes a ring of its own, and an exec on it. */
			if (chance(f, 30)) {
				memset(&p, 0, sizeof(p));
				p.flags = chance(f, 50) ? IORING_SETUP_SQPOLL : 0;
				if (kqring_setup(8, &p) >= 0)
					execl("/usr/bin/true", "true",
					    (char *)NULL);
			}
			_exit(0);
		}
		if (pid > 0)
			atomic_fetch_add(&st_forks, 1);
		break;
	}
	default:	/* the arena changes under requests in flight */
		arena_fill(f);
	}
}

static void *
fz_main(void *arg)
{
	struct fz *f = arg;
	long iters = 0;

	f->ring = -1;
	f->arena = mmap(NULL, ARENA_SIZE + 3 * PAGE_SIZE, PROT_NONE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (f->arena == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	f->hole = f->arena + ARENA_SIZE + 2 * PAGE_SIZE;
	f->arena += PAGE_SIZE;
	if (mprotect(f->arena, ARENA_SIZE, PROT_READ | PROT_WRITE) != 0)
		die("mprotect: %s", strerror(errno));
	arena_fill(f);
	pool_init(f);
	f->pbuf_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (f->pbuf_page == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	f->rw_page = mmap(NULL, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (f->rw_page == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	{
		char path[32];
		int fd;

		/*
		 * Out of reach, as the log is: at a small number, a CQE
		 * misread as an open's would put it among the descriptors
		 * that requests write to and close, and zeros from the arena
		 * would land among the records.
		 */
		snprintf(path, sizeof(path), "ord%d", f->id);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND |
		    O_CLOEXEC, 0600);
		if (fd < 0)
			die("open %s: %s", path, strerror(errno));
		f->ord_fd = fcntl(fd, F_DUPFD_CLOEXEC, ORD_FD + f->id);
		if (f->ord_fd != ORD_FD + f->id)
			die("ord%d at descriptor %d", f->id, f->ord_fd);
		close(fd);
		f->ord_buf = mmap(NULL, ORD_MAX * sizeof(struct ord_rec),
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		f->ord_meta = mmap(NULL, ORD_MAX * sizeof(struct ord_meta),
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (f->ord_buf == MAP_FAILED || f->ord_meta == MAP_FAILED)
			die("mmap: %s", strerror(errno));
	}

	while (!atomic_load(&stop) && (max_iters == 0 || iters < max_iters)) {
		int n;

		ring_open(f);
		iters++;
		if (f->ring < 0)
			continue;
		if (f->valid)
			ring_resources(f);
		n = 1 + (int)rn(f, 200);
		for (int i = 0; i < n && f->ring >= 0 && !atomic_load(&stop) &&
		    (max_iters == 0 || iters < max_iters); i++, iters++) {
			unsigned a = (unsigned)rn(f, 100);

			if (a < 75)
				do_submit(f);
			else if (a < 90)
				do_register(f);
			else if (a < 97)
				do_chaos(f);
			else
				break;		/* close with requests out */
			while (waitpid(-1, NULL, WNOHANG) > 0)
				;
		}
		reap(f);
		ring_close(f);
		dyn_close(f);
		if (f->pool[4] >= 0)
			(void)ftruncate(f->pool[4], 0);
	}
	pool_close(f);
	close(f->ord_fd);
	f->ord_fd = -1;
	atomic_fetch_add(&nfinished, 1);
	return (NULL);
}

static void
on_usr1(int sig __unused)
{
}

/*
 * The process's registered-ring table is made on first use and lives
 * until exit: make it before the baseline, so it isn't counted as held.
 */
static void
prime_ring_table(void)
{
	struct io_uring_params p;
	struct io_uring_rsrc_update u = { .offset = 0 };
	int fd;

	memset(&p, 0, sizeof(p));
	if ((fd = (int)kqring_setup(4, &p)) < 0)
		die("kqring_setup: %s (is kqring.ko loaded?)", strerror(errno));
	u.data = (uint64_t)fd;
	(void)kqring_register(fd, IORING_REGISTER_RING_FDS, &u, 1);
	(void)kqring_register(fd, IORING_UNREGISTER_RING_FDS, &u, 1);
	close(fd);
}

/* Drop rings left in the process's registered-ring table. */
static void
unregister_rings(void)
{
	struct io_uring_params p;
	struct io_uring_rsrc_update u[REG_RINGS];
	int fd;

	memset(&p, 0, sizeof(p));
	if ((fd = (int)kqring_setup(4, &p)) < 0)
		return;
	for (unsigned i = 0; i < REG_RINGS; i++)
		u[i] = (struct io_uring_rsrc_update){ .offset = i };
	(void)kqring_register(fd, IORING_UNREGISTER_RING_FDS, u, REG_RINGS);
	close(fd);
}

static const char *_Atomic teardown_at;	/* the step, NULL when done */

/* Teardown isn't watched by the loop in main(): fail it if it sticks. */
static void *
teardown_watch(void *arg __unused)
{
	uint64_t t0 = now_ns();
	char cmd[160];

	while (now_ns() - t0 < 4 * HANG_NS) {
		if (atomic_load(&teardown_at) == NULL)
			return (NULL);
		usleep(100000);
	}
	stdio_restore();
	fprintf(stderr, "kqring_fuzz: FAIL: teardown stuck in %s for %ju s "
	    "(seed %ju)\n", atomic_load(&teardown_at),
	    (uintmax_t)(4 * HANG_NS / 1000000000ULL), (uintmax_t)seed);
	snprintf(cmd, sizeof(cmd), "procstat -kk %d; for c in $(pgrep -P %d); "
	    "do procstat -kk $c; done", getpid(), getpid());
	(void)system(cmd);
	_exit(3);
}

/*
 * The fuzzer itself faulted: say where, and what each thread has mapped.
 * sudo leaves no core file, and a ring mapping that faults is a kernel bug.
 */
static void
on_fault(int sig, siginfo_t *si, void *ucv)
{
	const ucontext_t *uc = ucv;

	dprintf(ERR_FD, "kqring_fuzz: signal %d code %d at %p, rip %#lx\n",
	    sig, si->si_code, si->si_addr, (u_long)uc->uc_mcontext.mc_rip);
	for (int i = 0; i < nthreads; i++) {
		struct fz *f = &fzs[i];
		const char *call = atomic_load(&f->call);

		dprintf(ERR_FD, "  thread %d: ring %d flags %#x sq %u cq %u "
		    "sqe %u sqmap %p+%zu cqmap %p+%zu sqes %p arena %p "
		    "pbuf %p user_mem %p %p call %s\n", i, f->ring, f->p.flags,
		    f->p.sq_entries, f->p.cq_entries, f->sqe_size, f->sqmap,
		    f->sqmap_len, f->cqmap, f->cqmap_len, f->sqes, f->arena,
		    f->pbuf_page, f->user_mem[0], f->user_mem[1],
		    call != NULL ? call : "-");
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Read back each thread's ordered writes: whole records, and within each
 * ring's records the order it submitted them in.  0 if all are so.
 */
static int
ord_check(void)
{
	uint64_t total = 0, rings = 0;
	int bad = 0;

	for (int i = 0; i < nthreads; i++) {
		struct fz *f = &fzs[i];
		struct ord_rec r;
		char path[96];
		uint32_t *last;
		FILE *fp;
		size_t n;

		snprintf(path, sizeof(path), "%s/ord%d", scratch, i);
		if ((fp = fopen(path, "r")) == NULL) {
			fprintf(stderr, "kqring_fuzz: FAIL: %s: %s\n", path,
			    strerror(errno));
			return (1);
		}
		last = calloc((size_t)f->ord_gen + 1, sizeof(*last));
		if (last == NULL)
			die("calloc");
		struct ord_rec hist[16];
		unsigned nh = 0;

		while ((n = fread(&r, 1, sizeof(r), fp)) == sizeof(r)) {
			hist[nh++ % 16] = r;
			total++;
			if (r.magic != ORD_MAGIC || r.id != (uint32_t)i ||
			    r.gen == 0 || r.gen > f->ord_gen ||
			    r.seq >= f->ord_seq) {
				fprintf(stderr, "kqring_fuzz: FAIL: %s: record "
				    "%ju is not one written (magic %#x id %u "
				    "ring %u seq %u)\n", path,
				    (uintmax_t)total - 1, r.magic, r.id,
				    r.gen, r.seq);
				bad = 1;
				break;
			}
			if (last[r.gen] == 0)
				rings++;
			else if (r.seq + 1 <= last[r.gen]) {
				struct ord_rec x;
				unsigned dup = 0;

				fprintf(stderr, "kqring_fuzz: FAIL: %s: ring %u "
				    "wrote record %u after %u\n", path, r.gen,
				    r.seq, last[r.gen] - 1);
				fprintf(stderr, "  the file's last records:");
				for (unsigned h = nh > 16 ? nh - 16 : 0; h < nh;
				    h++)
					fprintf(stderr, " %u/%u", hist[h % 16].gen,
					    hist[h % 16].seq);
				fprintf(stderr, "\n");
				rewind(fp);
				while (fread(&x, 1, sizeof(x), fp) == sizeof(x))
					if (x.gen == r.gen && x.seq == r.seq)
						dup++;
				fprintf(stderr, "  record %u is in the file %u "
				    "times\n", r.seq, dup);
				for (uint32_t q = r.seq; q < last[r.gen] &&
				    q < r.seq + 8; q++) {
					struct ord_meta *m = &f->ord_meta[q];

					fprintf(stderr, "  record %u: ud %ju sq "
					    "%u (head %u) ring flags %#x flags "
					    "%#x, before it %#x, %s; CQE %u res "
					    "%d; entries %u room %u\n", q,
					    (uintmax_t)m->ud,
					    m->sq_tail, m->sq_head,
					    m->ring_flags, m->flags,
					    m->before, m->first ? "first of its "
					    "batch" : "in a batch", m->cqe_at,
					    m->res, m->sq_entries, m->room);
				}
				bad = 1;
				break;
			}
			last[r.gen] = r.seq + 1;
		}
		if (bad == 0 && n != 0) {
			fprintf(stderr, "kqring_fuzz: FAIL: %s: a torn record "
			    "at its end (%zu bytes)\n", path, n);
			bad = 1;
		}
		free(last);
		fclose(fp);
		if (bad)
			break;
	}
	printf("kqring_fuzz: ordered writes %ju submitted, %ju written by "
	    "%ju rings, %s\n", (uintmax_t)st_ord, (uintmax_t)total,
	    (uintmax_t)rings, bad ? "OUT OF ORDER" : "in order");
	return (bad);
}

int
main(int argc, char **argv)
{
	pthread_t tw;
	struct sigaction sa;
	long socks0, wired0, mem0, inline0;
	uint64_t t0, last = 0;
	char cmd[128];
	int ch, rc = 0;

	seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
	while ((ch = getopt(argc, argv, "i:j:s:t:")) != -1) {
		switch (ch) {
		case 'i':
			max_iters = atol(optarg);
			break;
		case 'j':
			nthreads = atoi(optarg);
			break;
		case 's':
			seed = strtoull(optarg, NULL, 0);
			break;
		case 't':
			run_secs = atoi(optarg);
			break;
		default:
			fprintf(stderr, "usage: kqring_fuzz [-t seconds] "
			    "[-j threads] [-s seed] [-i iterations]\n");
			return (2);
		}
	}
	if (nthreads < 1 || nthreads > MAX_THREADS)
		die("-j must be 1 to %d", MAX_THREADS);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;		/* no SA_RESTART: EINTR */
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = NULL;
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	strlcpy(scratch, "/tmp/kqring_fuzz.XXXXXX", sizeof(scratch));
	if (mkdtemp(scratch) == NULL || chdir(scratch) != 0)
		die("scratch directory: %s", strerror(errno));
	{
		struct stat st;

		if (stat("/dev/kqring", &st) != 0)
			die("/dev/kqring: %s", strerror(errno));
		kqring_rdev = st.st_rdev;
	}
	prime_ring_table();
	socks0 = open_sockets();
	wired0 = sysctl_num("vm.stats.vm.v_user_wire_count");
	mem0 = kqring_malloc_inuse();
	inline0 = sysctl_num("kern.kqring.wq_inline");
	names_init();
	printf("kqring_fuzz: seed %ju, %d threads, %d s, in %s\n",
	    (uintmax_t)seed, nthreads, run_secs, scratch);
	stdio_hide();
	if (fstat(OUT_FD, &out_st) != 0 || fstat(ERR_FD, &err_st) != 0)
		die("fstat: %s", strerror(errno));

	for (int i = 0; i < nthreads; i++) {
		fzs[i].id = i;
		fzs[i].rng = seed * 0x9e3779b97f4a7c15ULL + (uint64_t)i + 1;
		atomic_store(&ring_fds[i], -1);
	}
	for (int i = 0; i < nthreads; i++)
		if (pthread_create(&fzs[i].thr, NULL, fz_main, &fzs[i]) != 0)
			die("pthread_create");

	/* Watchdog: interrupt long calls, fail stuck ones. */
	t0 = now_ns();
	for (;;) {
		uint64_t now = now_ns();

		if (now - t0 > (uint64_t)run_secs * 1000000000ULL)
			atomic_store(&stop, true);
		for (int i = 0; i < nthreads; i++) {
			uint64_t since = atomic_load(&fzs[i].in_call);

			if (since == 0 || now < since)
				continue;
			if (now - since > HANG_NS) {
				stdio_restore();
				fprintf(stderr, "kqring_fuzz: FAIL: thread %d "
				    "stuck in %s for %ju s (seed %ju)\n", i,
				    atomic_load(&fzs[i].call),
				    (uintmax_t)((now - since) / 1000000000ULL),
				    (uintmax_t)seed);
				snprintf(cmd, sizeof(cmd), "procstat -kk %d",
				    getpid());
				(void)system(cmd);
				_exit(3);
			}
			if (now - since > KICK_NS) {
				pthread_kill(fzs[i].thr, SIGUSR1);
				atomic_fetch_add(&st_kicks, 1);
			}
		}
		if (atomic_load(&nfinished) == nthreads)
			break;
		if (now - last > 10000000000ULL) {
			struct stat o, e;

			/* The log must still be the log: fail, not go quiet. */
			if (fstat(OUT_FD, &o) != 0 || o.st_dev != out_st.st_dev ||
			    o.st_ino != out_st.st_ino || fstat(ERR_FD, &e) != 0 ||
			    e.st_dev != err_st.st_dev ||
			    e.st_ino != err_st.st_ino) {
				dprintf(ERR_FD, "kqring_fuzz: FAIL: descriptor "
				    "%d or %d is no longer the log\n", OUT_FD,
				    ERR_FD);
				dprintf(STDERR_FILENO, "kqring_fuzz: FAIL: log "
				    "descriptor replaced\n");
				_exit(4);
			}
			dprintf(OUT_FD, "[%4ju s] rings %ju (setup failed %ju) "
			    "sqes %ju "
			    "enters %ju registers %ju cqes %ju chaos %ju "
			    "forks %ju kicks %ju\n",
			    (uintmax_t)((now - t0) / 1000000000ULL),
			    (uintmax_t)st_rings, (uintmax_t)st_setup_fail,
			    (uintmax_t)st_sqes, (uintmax_t)st_enters,
			    (uintmax_t)st_regs, (uintmax_t)st_cqes,
			    (uintmax_t)st_chaos, (uintmax_t)st_forks,
			    (uintmax_t)st_kicks);
			last = now;
		}
		usleep(100000);
	}
	atomic_store(&teardown_at, "pthread_join");
	if (pthread_create(&tw, NULL, teardown_watch, NULL) != 0)
		die("pthread_create");
	for (int i = 0; i < nthreads; i++)
		pthread_join(fzs[i].thr, NULL);
	atomic_store(&teardown_at, "waitpid");
	while (waitpid(-1, NULL, 0) > 0)
		;
	atomic_store(&teardown_at, "unregister");
	unregister_rings();
	atomic_store(&teardown_at, "close");
	(void)close_range(3, OUT_FD - 1, 0);
	closefrom(ERR_FD + 1);
	stdio_restore();
	atomic_store(&teardown_at, NULL);
	pthread_join(tw, NULL);
	closefrom(3);
	printf("kqring_fuzz: rings %ju, sqes %ju, enters %ju, registers %ju, "
	    "cqes %ju\n", (uintmax_t)st_rings, (uintmax_t)st_sqes,
	    (uintmax_t)st_enters, (uintmax_t)st_regs, (uintmax_t)st_cqes);
	printf("kqring_fuzz: triggers %ju (ended %ju, %ju not -EINVAL), "
	    "firings reported %ju\n", (uintmax_t)st_trig,
	    (uintmax_t)st_trig_end, (uintmax_t)st_trig_live,
	    (uintmax_t)st_trig_fire);
	printf("kqring_fuzz: kevent requests %ju\n", (uintmax_t)st_kev);
	printf("kqring_fuzz: kqueue bridges %ju\n", (uintmax_t)st_kqb);
	printf("kqring_fuzz: buffer-passing pairs %ju, buffers sent %ju\n",
	    (uintmax_t)st_echo, (uintmax_t)st_echo_sent);
	printf("kqring_fuzz: scratch files O_DIRECT %ju, buffered %ju\n",
	    (uintmax_t)st_direct, (uintmax_t)st_buffered);
	printf("kqring_fuzz: new rings closed before mapping %ju\n",
	    (uintmax_t)st_ring_gone);
	printf("kqring_fuzz: CQE descriptors in the fuzzer's range, refused "
	    "%ju\n", (uintmax_t)st_dyn_reserved);
	printf("kqring_fuzz: pipe burst reads %ju, with data %ju, FIFOs %ju\n",
	    (uintmax_t)st_piperd, (uintmax_t)st_piperd_data,
	    (uintmax_t)st_fifo);
	printf("kqring_fuzz: file run reads %ju, reads run inline %ju\n",
	    (uintmax_t)st_fileread,
	    (uintmax_t)(sysctl_num("kern.kqring.wq_inline") - inline0));
	if (atomic_load(&cq_bad)) {
		fprintf(stderr, "kqring_fuzz: FAIL: a CQ ring overran\n");
		rc = 1;
	}
	if (check_leaks(socks0, wired0, mem0) != 0)
		rc = 1;
	/* Every ring is gone (check_leaks()), so every write has run. */
	if (ord_check() != 0)
		rc = 1;
	if (chdir("/tmp") == 0) {
		snprintf(cmd, sizeof(cmd), "rm -rf %s", scratch);
		(void)system(cmd);
	}
	printf(rc == 0 ? "[OK] kqring_fuzz\n" : "[FAIL] kqring_fuzz\n");
	return (rc);
}
