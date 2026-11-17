/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Does a process's memory stay its own under kqring futexes and fork(2)?
 * kqring_fuzz found the page of its .bss holding its counters and the
 * words its futex requests wait on zeroed mid-run, a few times in many
 * runs.  This narrows that down: one page-aligned block of .bss holds
 * known values and the futex words; rings submit FUTEX_WAIT (with the
 * word's value, and with a stale one) and FUTEX_WAKE on those words, shared
 * futexes as the fuzzer's are (no FUTEX2_PRIVATE); threads change the
 * words and a counter on the same page; a thread forks children that
 * touch the page and exit, some of them submitting a FUTEX_WAKE on an
 * inherited ring first.  A checker reads the known values every
 * millisecond.
 *
 *   kqring_pagecheck [-t seconds]
 *
 * Exits 0 if the page held for the whole run, 1 as soon as it changed
 * (with what it found; KQFUZZ_STOP_ON_RESET=1 stops the process there,
 * to be looked at), 2 on a setup error.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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

#define	NMAGIC	480
#define	NWORDS	8
#define	NRINGS	3

/* The page under test: known values, the futex words, a counter. */
static struct {
	uint64_t		magic[NMAGIC];
	_Atomic uint32_t	words[NWORDS];
	_Atomic uint64_t	ctr;
} pg __attribute__((aligned(4096)));

static _Atomic int stop, ready;
static const unsigned ring_flags[NRINGS] = { 0, IORING_SETUP_SQPOLL,
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN };
static _Atomic uint64_t n_forks, n_futex, n_checks;

static uint64_t
magic_of(unsigned i)
{
	return (0x5a17c0de00000000ULL ^ ((uint64_t)i * 0x9e3779b97f4a7c15ULL));
}

struct ring {
	int			fd;
	unsigned		flags;
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
}

static void
prep_futex(struct ring *r, uint8_t op, unsigned w, uint64_t val)
{
	struct io_uring_sqe *s = &r->sqes[r->tail & r->sq_mask];

	memset(s, 0, sizeof(*s));
	s->opcode = op;
	s->addr = (uint64_t)(uintptr_t)&pg.words[w];
	s->addr2 = val;			/* WAIT: expected value; WAKE: count */
	s->addr3 = FUTEX_BITSET_MATCH_ANY;
	s->fd = FUTEX2_SIZE_U32;	/* shared, as kqring_fuzz's */
	s->user_data = op;
	r->tail++;
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
	unsigned w = 0, inflight = 0;

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
		inflight -= reap(r);
		/* Waits a word change ends: re-checked on every enter. */
		while (inflight > 24 && !atomic_load(&stop)) {
			atomic_fetch_add(&pg.words[w], 1);
			submit(r, 0, 1);
			inflight -= reap(r);
		}
		atomic_fetch_add(&n_futex, 3);
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

/*
 * fork(2) without a pause: each child writes the page (its own copy) and
 * exits; every third submits a FUTEX_WAKE on an inherited ring first.
 */
static void *
fork_thread(void *arg)
{
	(void)arg;
	for (uint64_t i = 0; !atomic_load(&stop); i++) {
		pid_t pid = fork();

		if (pid == 0) {
			pg.words[i % NWORDS] += 1000;
			if (i % 3 == 0) {
				struct ring *r = &rings[i % NRINGS];

				if ((r->flags & IORING_SETUP_SINGLE_ISSUER) ==
				    0) {
					prep_futex(r, IORING_OP_FUTEX_WAKE,
					    (unsigned)(i % NWORDS), 1);
					submit(r, 1, 0);
				}
			}
			_exit(0);
		}
		if (pid > 0) {
			(void)waitpid(pid, NULL, 0);
			atomic_fetch_add(&n_forks, 1);
		}
	}
	return (NULL);
}

static void
report_change(void)
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
	fprintf(stderr, "kqring_pagecheck: FAIL: the page changed: %u of %u "
	    "known values wrong (%u zero), first at %u (%#jx), counter %ju, "
	    "after %ju forks, %ju futex requests\n", bad, NMAGIC, zero, first,
	    first < NMAGIC ? (uintmax_t)pg.magic[first] : 0,
	    (uintmax_t)atomic_load(&pg.ctr), (uintmax_t)atomic_load(&n_forks),
	    (uintmax_t)atomic_load(&n_futex));
	if (getenv("KQFUZZ_STOP_ON_RESET") != NULL)
		(void)kill(getpid(), SIGSTOP);
	_exit(1);
}

int
main(int argc, char **argv)
{
	pthread_t thr[NRINGS + 3];
	unsigned secs = 600;
	int ch, nthr = 0;
	time_t t0, last;

	while ((ch = getopt(argc, argv, "t:")) != -1) {
		if (ch == 't')
			secs = (unsigned)strtoul(optarg, NULL, 10);
		else {
			fprintf(stderr, "usage: kqring_pagecheck [-t seconds]\n");
			return (2);
		}
	}
	for (unsigned i = 0; i < NMAGIC; i++)
		pg.magic[i] = magic_of(i);
	printf("kqring_pagecheck: page at %p, %u s\n", (void *)&pg, secs);
	for (unsigned i = 0; i < NRINGS; i++)
		CHECK(pthread_create(&thr[nthr++], NULL, ring_thread,
		    &rings[i]) == 0);
	while (atomic_load(&ready) < NRINGS)
		usleep(1000);
	for (unsigned i = 0; i < 2; i++)
		CHECK(pthread_create(&thr[nthr++], NULL, writer_thread,
		    (void *)(uintptr_t)i) == 0);
	CHECK(pthread_create(&thr[nthr++], NULL, fork_thread, NULL) == 0);

	t0 = last = time(NULL);
	while (time(NULL) - t0 < (time_t)secs) {
		for (unsigned i = 0; i < NMAGIC; i++) {
			if (pg.magic[i] != magic_of(i))
				report_change();
		}
		atomic_fetch_add(&n_checks, 1);
		if (time(NULL) - last >= 10) {
			last = time(NULL);
			printf("[%4ld s] forks %ju futex %ju counter %ju\n",
			    (long)(last - t0), (uintmax_t)atomic_load(&n_forks),
			    (uintmax_t)atomic_load(&n_futex),
			    (uintmax_t)atomic_load(&pg.ctr));
			fflush(stdout);
		}
		usleep(1000);
	}
	atomic_store(&stop, 1);
	/* Waits left armed end with their rings at exit. */
	for (int i = NRINGS; i < nthr; i++)
		pthread_join(thr[i], NULL);
	printf("[OK] kqring_pagecheck: the page held for %u s, %ju forks, "
	    "%ju futex requests, %ju checks\n", secs,
	    (uintmax_t)atomic_load(&n_forks), (uintmax_t)atomic_load(&n_futex),
	    (uintmax_t)atomic_load(&n_checks));
	return (0);
}
