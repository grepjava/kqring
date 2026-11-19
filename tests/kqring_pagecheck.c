/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Does a process's memory stay its own under kqring and fork(2)?
 * kqring_fuzz found the page of its .bss holding its counters and the
 * words its futex requests wait on changed in place mid-run, a few times
 * in many runs: zeroed on older builds, and later back to what it held in
 * the run's first moments.  This narrows that down.  One page-aligned
 * block of .bss holds known values, the futex words, counters that only
 * go up and a value only the checker writes:
 *
 *  - rings submit FUTEX_WAIT (with the word's value, and with a stale
 *    one) and FUTEX_WAKE on those words, shared futexes as the fuzzer's
 *    are (no FUTEX2_PRIVATE); with -F, NOPs instead (no futex keys);
 *  - two of the rings have registered buffers (in an anonymous mapping,
 *    as the fuzzer's arena), which kqring holds again at every fork;
 *  - threads change the words and a counter on the same page;
 *  - a thread forks without waiting (up to MAXKIDS children at once, a
 *    reaper thread collects them), as the fuzzer does; each child writes
 *    its copy of the page, and some submit a FUTEX_WAKE or a fixed write
 *    and read on an inherited ring, wait on that ring for up to a second,
 *    probe it, and make a ring of their own (SQPOLL half the time) and
 *    exec /usr/bin/true.
 *
 * A checker reads the page every millisecond: the known values, that the
 * counters never went back, and that its own value is still the one it
 * wrote.
 *
 *   kqring_pagecheck [-F] [-t seconds]
 *
 * Built with -DPAGECHECK_LAST (kqring_pagecheck_last), the page is the
 * last of a 40-page .bss, as the fuzzer's is.
 *
 * Exits 0 if the page held for the whole run, 1 as soon as it changed
 * (with what it found; KQFUZZ_STOP_ON_RESET=1 stops the process there,
 * to be looked at), 2 on a setup error.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
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

#include <sys/kqring.h>

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__,	\
		    #x, strerror(errno));				\
		exit(2);						\
	}								\
} while (0)

#define	NMAGIC	470
#define	NWORDS	8
#define	NRINGS	3
#define	NBUFS	4
#define	MAXKIDS	16

/* The page under test. */
struct page {
	uint64_t		magic[NMAGIC];
	_Atomic uint32_t	words[NWORDS];
	_Atomic uint64_t	ctr;		/* the writer threads' */
	_Atomic uint64_t	n_forks;	/* the fork thread's */
	_Atomic uint64_t	n_reqs;		/* the ring threads' */
	_Atomic uint64_t	gen;		/* only the checker writes it */
};

_Static_assert(sizeof(struct page) <= 4096, "the page under test is one page");

#ifdef PAGECHECK_LAST
/*
 * The fuzzer's layout: the page under test is the last of a 40-page
 * anonymous mapping of .bss, the pages before it never touched.  Copy
 * relocations end .bss, so the block is a NOBITS large section, which
 * the linker places after it (checked at startup).
 */
struct bsslast {
	char		untouched[38 * 4096];
	struct page	pg;
};
__asm__(".section .lbss,\"awl\",@nobits\n"
    ".p2align 12\n"
    "bsslast:\n"
    ".zero (38 * 4096 + 4096)\n"
    ".previous\n");
extern struct bsslast bsslast __asm__("bsslast");
#define	pg	bsslast.pg
#else
static struct page pg __attribute__((aligned(4096)));
#endif

static _Atomic int stop, ready, kids;
static bool nofutex;
static const unsigned ring_flags[NRINGS] = { 0, IORING_SETUP_SQPOLL,
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN };
static _Atomic uint64_t n_checks;
static char *arena;
static int filefd = -1;

static uint64_t
magic_of(unsigned i)
{
	return (0x5a17c0de00000000ULL ^ ((uint64_t)i * 0x9e3779b97f4a7c15ULL));
}

struct ring {
	int			fd;
	unsigned		flags;
	bool			bufs;
	struct io_uring_sqe	*sqes;
	_Atomic unsigned	*sq_tail, *sq_flags, *cq_head, *cq_tail;
	struct io_uring_cqe	*cqes;
	unsigned		sq_mask, cq_mask, tail;
};

static struct ring rings[NRINGS];

static void
ring_init(struct ring *r, unsigned flags)
{
	struct io_uring_params p;
	struct iovec iov[NBUFS];
	size_t sq_len, cq_len, len;
	unsigned *array;
	char *b;

	memset(&p, 0, sizeof(p));
	memset(r, 0, sizeof(*r));
	p.flags = flags;
	r->flags = flags;
	r->fd = (int)kqring_setup(64, &p);
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
	r->sq_tail = (_Atomic unsigned *)(b + p.sq_off.tail);
	r->sq_flags = (_Atomic unsigned *)(b + p.sq_off.flags);
	r->sq_mask = *(unsigned *)(b + p.sq_off.ring_mask);
	r->cq_head = (_Atomic unsigned *)(b + p.cq_off.head);
	r->cq_tail = (_Atomic unsigned *)(b + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(b + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(b + p.cq_off.cqes);
	array = (unsigned *)(b + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		array[i] = i;
	r->tail = atomic_load(r->sq_tail);
	/* Registered buffers, held again at every fork(2). */
	if ((flags & IORING_SETUP_SINGLE_ISSUER) == 0) {
		for (unsigned i = 0; i < NBUFS; i++) {
			iov[i].iov_base = arena + i * 8192;
			iov[i].iov_len = 4096;
		}
		CHECK(kqring_register(r->fd, IORING_REGISTER_BUFFERS, iov,
		    NBUFS) == 0);
		r->bufs = true;
	}
}

static struct io_uring_sqe *
next_sqe(struct ring *r)
{
	struct io_uring_sqe *s = &r->sqes[r->tail & r->sq_mask];

	memset(s, 0, sizeof(*s));
	r->tail++;
	return (s);
}

static void
prep_futex(struct ring *r, uint8_t op, unsigned w, uint64_t val)
{
	struct io_uring_sqe *s = next_sqe(r);

	if (nofutex) {
		s->opcode = IORING_OP_NOP;
		return;
	}
	s->opcode = op;
	s->addr = (uint64_t)(uintptr_t)&pg.words[w];
	s->addr2 = val;			/* WAIT: expected value; WAKE: count */
	s->addr3 = FUTEX_BITSET_MATCH_ANY;
	s->fd = FUTEX2_SIZE_U32;	/* shared, as kqring_fuzz's */
	s->user_data = op;
}

static void
prep_fixed(struct ring *r, uint8_t op, unsigned buf)
{
	struct io_uring_sqe *s = next_sqe(r);

	s->opcode = op;
	s->fd = filefd;
	s->addr = (uint64_t)(uintptr_t)(arena + buf * 8192);
	s->len = 4096;
	s->off = (uint64_t)buf * 4096;
	s->buf_index = (uint16_t)buf;
	s->user_data = op;
}

static void
submit(struct ring *r, unsigned n, unsigned want)
{
	unsigned flags = IORING_ENTER_GETEVENTS;

	atomic_store_explicit(r->sq_tail, r->tail, memory_order_release);
	if ((r->flags & IORING_SETUP_SQPOLL) != 0) {
		flags |= IORING_ENTER_SQ_WAKEUP;
		n = 0;
	}
	(void)kqring_enter(r->fd, n, want, flags, NULL, 0);
}

static unsigned
reap(struct ring *r)
{
	unsigned head = atomic_load(r->cq_head), tail, n = 0;

	tail = atomic_load_explicit(r->cq_tail, memory_order_acquire);
	for (; head != tail; head++)
		n++;
	atomic_store_explicit(r->cq_head, head, memory_order_release);
	return (n);
}

/* FUTEX_WAIT with the word's value and a stale one, then FUTEX_WAKE. */
static void *
ring_thread(void *arg)
{
	struct ring *r = arg;
	unsigned w = 0;
	int inflight = 0;

	/* Made here: a SINGLE_ISSUER ring's submitter is its creator. */
	ring_init(r, ring_flags[r - rings]);
	atomic_fetch_add(&ready, 1);
	while (!atomic_load(&stop)) {
		uint32_t v = atomic_load(&pg.words[w]);

		prep_futex(r, IORING_OP_FUTEX_WAIT, w, v);
		prep_futex(r, IORING_OP_FUTEX_WAIT, (w + 1) % NWORDS, v + 7);
		prep_futex(r, IORING_OP_FUTEX_WAKE, w, NRINGS * 4);
		inflight += 3;
		submit(r, 3, 1);
		inflight -= (int)reap(r);
		/* The children's completions land here too. */
		if (inflight < 0)
			inflight = 0;
		/* Waits a word change ends: re-checked on every enter. */
		while (inflight > 24 && !atomic_load(&stop)) {
			atomic_fetch_add(&pg.words[w], 1);
			submit(r, 0, 1);
			inflight -= (int)reap(r);
		}
		atomic_fetch_add(&pg.n_reqs, 3);
		w = (w + 1) % NWORDS;
	}
	return (NULL);
}

/* Change the words and the counter: the page is written all the time. */
static void *
writer_thread(void *arg)
{
	unsigned w = (unsigned)(uintptr_t)arg;

	while (!atomic_load(&stop)) {
		atomic_fetch_add(&pg.words[w % NWORDS], 1);
		atomic_fetch_add(&pg.ctr, 1);
		w++;
	}
	return (NULL);
}

/* What kqring_fuzz's chaos children do, on the rings without an owner. */
static void
child(uint64_t i)
{
	struct ring *r = &rings[i % 2];
	struct io_uring_params p;

	pg.words[i % NWORDS] += 1000;
	pg.ctr = 0;		/* the child's copy only */
	switch (i % 4) {
	case 0:
		prep_futex(r, IORING_OP_FUTEX_WAKE, (unsigned)(i % NWORDS), 1);
		submit(r, 1, 0);
		break;
	case 1:
		/* The registered buffer is the parent's memory. */
		prep_fixed(r, IORING_OP_WRITE_FIXED, (unsigned)(i % NBUFS));
		prep_fixed(r, IORING_OP_READ_FIXED,
		    (unsigned)((i + 1) % NBUFS));
		submit(r, 2, 0);
		break;
	}
	/* A wait on the parent's ring, for as long as it takes. */
	alarm(1);
	(void)kqring_enter(r->fd, 0, (unsigned)(i % 3),
	    (i & 4) != 0 ? IORING_ENTER_GETEVENTS : 0, NULL, 0);
	alarm(0);
	(void)kqring_register(r->fd, IORING_REGISTER_PROBE, arena, 0);
	if (i % 10 < 3) {
		memset(&p, 0, sizeof(p));
		p.flags = (i & 8) != 0 ? IORING_SETUP_SQPOLL : 0;
		if (kqring_setup(8, &p) >= 0)
			execl("/usr/bin/true", "true", (char *)NULL);
	}
	_exit(0);
}

/* fork(2) without waiting for the child, as kqring_fuzz does. */
static void *
fork_thread(void *arg)
{
	(void)arg;
	for (uint64_t i = 0; !atomic_load(&stop); i++) {
		pid_t pid;

		while (atomic_load(&kids) >= MAXKIDS && !atomic_load(&stop))
			usleep(100);
		atomic_fetch_add(&kids, 1);
		pid = fork();
		if (pid == 0)
			child(i);
		if (pid > 0)
			atomic_fetch_add(&pg.n_forks, 1);
		else
			atomic_fetch_sub(&kids, 1);
	}
	return (NULL);
}

static void *
reaper_thread(void *arg)
{
	(void)arg;
	for (;;) {
		if (waitpid(-1, NULL, 0) > 0)
			atomic_fetch_sub(&kids, 1);
		else if (atomic_load(&stop))
			return (NULL);
		else
			usleep(1000);
	}
}

static void
report_change(const char *what, uint64_t got, uint64_t want)
{
	unsigned bad = 0, zero = 0, first = NMAGIC;

	for (unsigned i = 0; i < NMAGIC; i++) {
		if (pg.magic[i] != magic_of(i)) {
			bad++;
			if (pg.magic[i] == 0)
				zero++;
			if (first == NMAGIC)
				first = i;
		}
	}
	fprintf(stderr, "kqring_pagecheck: FAIL: the page changed: %s %#jx, "
	    "expected %#jx; %u of %u known values wrong (%u zero), first at %u; "
	    "on the page: counter %ju, forks %ju, requests %ju, gen %ju\n",
	    what, (uintmax_t)got, (uintmax_t)want, bad, NMAGIC, zero, first,
	    (uintmax_t)atomic_load(&pg.ctr), (uintmax_t)atomic_load(&pg.n_forks),
	    (uintmax_t)atomic_load(&pg.n_reqs), (uintmax_t)atomic_load(&pg.gen));
	if (getenv("KQFUZZ_STOP_ON_RESET") != NULL)
		(void)kill(getpid(), SIGSTOP);
	_exit(1);
}

int
main(int argc, char **argv)
{
	pthread_t thr[NRINGS + 4];
	char path[] = "/tmp/kqring_pagecheck.XXXXXX";
	uint64_t gen = 0, ctr = 0, forks = 0, reqs = 0, v;
	unsigned secs = 600;
	int ch, nthr = 0;
	time_t t0, last;

	while ((ch = getopt(argc, argv, "Ft:")) != -1) {
		if (ch == 'F')
			nofutex = true;
		else if (ch == 't')
			secs = (unsigned)strtoul(optarg, NULL, 10);
		else {
			fprintf(stderr,
			    "usage: kqring_pagecheck [-F] [-t seconds]\n");
			return (2);
		}
	}
#ifdef PAGECHECK_LAST
	{
		extern char end[];

		if ((uintptr_t)(&pg + 1) > (uintptr_t)end ||
		    ((uintptr_t)end + 4095) / 4096 !=
		    (uintptr_t)&pg / 4096 + 1) {
			fprintf(stderr, "kqring_pagecheck: the page at %p is "
			    "not the last of .bss (end %p)\n", (void *)&pg,
			    (void *)end);
			return (2);
		}
	}
#endif
	for (unsigned i = 0; i < NMAGIC; i++)
		pg.magic[i] = magic_of(i);
	arena = mmap(NULL, NBUFS * 8192, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	CHECK(arena != MAP_FAILED);
	memset(arena, 0xa5, NBUFS * 8192);
	filefd = mkstemp(path);
	CHECK(filefd >= 0);
	(void)unlink(path);
	CHECK(ftruncate(filefd, NBUFS * 4096) == 0);
	printf("kqring_pagecheck: page at %p, %u s, %s\n", (void *)&pg, secs,
	    nofutex ? "NOPs (no futexes)" : "futexes");
	for (unsigned i = 0; i < NRINGS; i++)
		CHECK(pthread_create(&thr[nthr++], NULL, ring_thread,
		    &rings[i]) == 0);
	while (atomic_load(&ready) < NRINGS)
		usleep(1000);
	for (unsigned i = 0; i < 2; i++)
		CHECK(pthread_create(&thr[nthr++], NULL, writer_thread,
		    (void *)(uintptr_t)i) == 0);
	CHECK(pthread_create(&thr[nthr++], NULL, fork_thread, NULL) == 0);
	CHECK(pthread_create(&thr[nthr++], NULL, reaper_thread, NULL) == 0);

	t0 = last = time(NULL);
	while (time(NULL) - t0 < (time_t)secs) {
		for (unsigned i = 0; i < NMAGIC; i++) {
			if (pg.magic[i] != magic_of(i))
				report_change("a known value", pg.magic[i],
				    magic_of(i));
		}
		if ((v = atomic_load(&pg.gen)) != gen)
			report_change("the checker's value", v, gen);
		atomic_store(&pg.gen, ++gen);
		if ((v = atomic_load(&pg.ctr)) < ctr)
			report_change("the counter went back to", v, ctr);
		ctr = v;
		if ((v = atomic_load(&pg.n_forks)) < forks)
			report_change("the fork count went back to", v, forks);
		forks = v;
		if ((v = atomic_load(&pg.n_reqs)) < reqs)
			report_change("the request count went back to", v,
			    reqs);
		reqs = v;
		atomic_fetch_add(&n_checks, 1);
		if (time(NULL) - last >= 10) {
			last = time(NULL);
			printf("[%4ld s] forks %ju requests %ju counter %ju "
			    "children %d\n", (long)(last - t0),
			    (uintmax_t)forks, (uintmax_t)reqs, (uintmax_t)ctr,
			    atomic_load(&kids));
			fflush(stdout);
		}
		usleep(1000);
	}
	atomic_store(&stop, 1);
	/* Waits left armed end with their rings at exit. */
	for (int i = NRINGS; i < nthr; i++)
		pthread_join(thr[i], NULL);
	printf("[OK] kqring_pagecheck: the page held for %u s, %ju forks, "
	    "%ju requests, %ju checks\n", secs,
	    (uintmax_t)atomic_load(&pg.n_forks),
	    (uintmax_t)atomic_load(&pg.n_reqs),
	    (uintmax_t)atomic_load(&n_checks));
	return (0);
}
