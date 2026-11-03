/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * kqring_diskbench: block I/O workloads on a disk device or a file, through
 * kqring, aio(4) or plain pread(2)/pwrite(2), in the manner of a storage
 * benchmark: a fixed queue depth per job, any number of jobs (threads),
 * sequential or random offsets, and a read/write mix.  It reports IOPS,
 * bandwidth and completion-latency percentiles, and with -V checks every
 * read against what was last written there.
 *
 *	kqring_diskbench [-e engine] [-w workload] [-b bs] [-q depth]
 *	    [-j jobs] [-t secs] [-r ramp] [-s size] [-m read%] [-n wait] [-FBPIV]
 *	    [-f] [-L label] target
 *
 *	-e kqring | aio | psync		(default kqring; psync ignores -q)
 *	-w read | write | randread | randwrite | randrw	(default randread)
 *	-b block size, -s bytes used	(k/m/g suffixes; default 4k and all
 *					 of target, at most 1g)
 *	-q queue depth per job (32), -j jobs (1), -t seconds (10) after a
 *	   ramp of -r seconds (1) that is not counted, -m reads in randrw (50)
 *	-n		kqring: wait for up to this many completions per enter (1)
 *	-F -B -P -I	kqring: fixed file, fixed buffers, SQPOLL, IOPOLL
 *	-V		verify: fill the area first, check every read, and
 *			re-read every block at the end
 *	-f		allow writing to a device other than md(4)
 *	-L		label for the ROW lines (default: engine and flags)
 *
 * Jobs work on disjoint, equal slices of the area, so that -V knows what
 * each block holds.  Regular files are opened O_DIRECT (and extended to
 * -s if shorter); devices are used as they are.  Writes are refused on a
 * device unless it is an md(4) disk or -f is given.
 *
 * Latency runs from just before an I/O is submitted to when its completion
 * is seen, both clock readings taken once per batch, so it includes the
 * submission system call and the queueing behind a batch.  Besides the
 * summary, each direction gives one line for aggregation (see BENCHMARK.MD):
 *
 *	ROW diskbench <label> <read|write> <iops> <MiB/s> <p50> <p99> <p99.9>
 *
 * with latencies in microseconds.  It exits non-zero on any I/O error,
 * short transfer or verify mismatch.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/kqring.h>

#define MAX_DEPTH	1024
#define MAX_JOBS	256

enum engine { E_KQRING, E_AIO, E_PSYNC };
enum { D_READ, D_WRITE };

static struct {
	enum engine	engine;
	bool		rand, verify, force;
	bool		fixed_file, fixed_bufs, sqpoll, iopoll;
	int		readpct;	/* reads in 100 I/Os */
	unsigned	depth, jobs, wait_nr;
	uint64_t	bs, size;
	int		secs, ramp;
	const char	*target, *label;
	char		workload[16];
} cfg = {
	.engine = E_KQRING, .rand = true, .readpct = 100, .depth = 32,
	.jobs = 1, .wait_nr = 1, .bs = 4096, .secs = 10, .ramp = 1,
	.workload = "randread",
};

static int		dev_fd = -1;
static uint64_t		t_start, t_measure, t_end;	/* ns */
static pthread_barrier_t filled, started;	/* -V fill done; deadlines set */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void __dead2 __printflike(1, 2)
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "kqring_diskbench: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

/* ------------------------------------------------------------------------
 * Latency histogram: exact below 64 ns, then 32 steps per power of two
 * (about 3% resolution).
 * ------------------------------------------------------------------------
 */

#define HIST_SUB	32
#define HIST_N		(64 + (64 - 6) * HIST_SUB)

struct hist {
	uint64_t	n[HIST_N];
	uint64_t	count, max;
};

static unsigned
hist_idx(uint64_t v)
{
	int e;

	if (v < 64)
		return ((unsigned)v);
	e = flsll((long long)v) - 1;
	return (64 + (unsigned)(e - 6) * HIST_SUB +
	    (unsigned)((v >> (e - 5)) & (HIST_SUB - 1)));
}

/* The top of bucket i. */
static uint64_t
hist_val(unsigned i)
{
	unsigned e, m;

	if (i < 64)
		return (i);
	e = (i - 64) / HIST_SUB + 6;
	m = (i - 64) % HIST_SUB;
	return (((1ULL << e) | ((uint64_t)m << (e - 5))) +
	    (1ULL << (e - 5)) - 1);
}

static void
hist_add(struct hist *h, uint64_t v)
{
	h->n[hist_idx(v)]++;
	h->count++;
	if (v > h->max)
		h->max = v;
}

static void
hist_merge(struct hist *to, const struct hist *from)
{
	for (unsigned i = 0; i < HIST_N; i++)
		to->n[i] += from->n[i];
	to->count += from->count;
	if (from->max > to->max)
		to->max = from->max;
}

static double
hist_pct_us(const struct hist *h, double pct)
{
	uint64_t want = (uint64_t)((double)h->count * pct / 100.0 + 0.5), seen = 0;

	if (want == 0)
		want = 1;
	for (unsigned i = 0; i < HIST_N; i++) {
		seen += h->n[i];
		if (seen >= want)
			return (MIN(hist_val(i), h->max) / 1000.0);
	}
	return (h->max / 1000.0);
}

/* ------------------------------------------------------------------------
 * Jobs, slots and the verify pattern.
 * ------------------------------------------------------------------------
 */

struct slot {
	uint64_t	blk;		/* block within the job's slice */
	uint64_t	t0;
	uint32_t	gen;		/* -V: the generation written or read */
	uint8_t		dir;
	struct aiocb	cb;
};

struct job {
	unsigned	id;
	pthread_t	thr;
	uint64_t	base, nblk;	/* slice: first byte, blocks */
	uint64_t	seq, rng;
	char		*bufs;		/* depth * bs, page aligned */
	struct slot	slots[MAX_DEPTH];
	uint32_t	*gen;		/* -V: per block, 0 = never written */
	uint8_t		*busy;		/* -V: per block, I/O in flight */
	struct hist	lat[2];
	uint64_t	ios[2];
	uint64_t	checked;
	/* kqring */
	int		ring_fd;
	struct io_uring_sqe *sqes;
	_Atomic unsigned *sq_khead, *sq_ktail, *sq_kflags;
	_Atomic unsigned *cq_khead, *cq_ktail;
	unsigned	sq_mask, cq_mask, sq_tail, sq_entries;
	struct io_uring_cqe *cqes;
};

static struct job	*jobs;

static uint64_t
xorshift(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return (*s = x);
}

static uint64_t
pattern_seed(const struct job *j, uint64_t blk, uint32_t gen)
{
	return (((j->base / cfg.bs + blk) * 0x9e3779b97f4a7c15ULL) ^
	    ((uint64_t)gen << 32) ^ 0x5bd1e995ULL) | 1;
}

static void
pattern_fill(const struct job *j, char *buf, uint64_t blk, uint32_t gen)
{
	uint64_t s = pattern_seed(j, blk, gen), *w = (uint64_t *)(void *)buf;

	for (uint64_t i = 0; i < cfg.bs / 8; i++)
		w[i] = xorshift(&s);
}

static void
pattern_check(struct job *j, const char *buf, uint64_t blk, uint32_t gen,
    const char *when)
{
	uint64_t s = pattern_seed(j, blk, gen);
	const uint64_t *w = (const uint64_t *)(const void *)buf;

	for (uint64_t i = 0; i < cfg.bs / 8; i++)
		if (w[i] != xorshift(&s))
			die("verify (%s): job %u, offset %ju, generation %u: "
			    "word %ju is %#jx", when, j->id,
			    (uintmax_t)(j->base + blk * cfg.bs), gen,
			    (uintmax_t)i, (uintmax_t)w[i]);
	j->checked++;
}

static char *
slot_buf(struct job *j, unsigned i)
{
	return (j->bufs + (size_t)i * cfg.bs);
}

/* Choose the next I/O for slot i and fill its buffer if it writes. */
static void
slot_next(struct job *j, unsigned i)
{
	struct slot *s = &j->slots[i];
	uint64_t blk;

	s->dir = (int)(xorshift(&j->rng) % 100) < cfg.readpct ? D_READ :
	    D_WRITE;
	if (cfg.rand)
		blk = xorshift(&j->rng) % j->nblk;
	else
		blk = j->seq++ % j->nblk;
	if (cfg.verify) {
		while (j->busy[blk])	/* depth < nblk, checked in main() */
			blk = (blk + 1) % j->nblk;
		j->busy[blk] = 1;
		if (s->dir == D_WRITE)
			pattern_fill(j, slot_buf(j, i), blk, ++j->gen[blk]);
		s->gen = j->gen[blk];
	}
	s->blk = blk;
}

/* Slot i's I/O completed with res at time t. */
static void
slot_done(struct job *j, unsigned i, int64_t res, uint64_t t)
{
	struct slot *s = &j->slots[i];

	if (res < 0)
		die("job %u: %s at offset %ju: %s", j->id,
		    s->dir == D_READ ? "read" : "write",
		    (uintmax_t)(j->base + s->blk * cfg.bs), strerror((int)-res));
	if ((uint64_t)res != cfg.bs)
		die("job %u: short %s at offset %ju: %jd of %ju", j->id,
		    s->dir == D_READ ? "read" : "write",
		    (uintmax_t)(j->base + s->blk * cfg.bs), (intmax_t)res,
		    (uintmax_t)cfg.bs);
	if (cfg.verify) {
		if (s->dir == D_READ)
			pattern_check(j, slot_buf(j, i), s->blk, s->gen,
			    "read");
		j->busy[s->blk] = 0;
	}
	if (s->t0 >= t_measure && t <= t_end) {
		hist_add(&j->lat[s->dir], t - s->t0);
		j->ios[s->dir]++;
	}
}

static off_t
slot_off(const struct job *j, unsigned i)
{
	return ((off_t)(j->base + j->slots[i].blk * cfg.bs));
}

/* ------------------------------------------------------------------------
 * Engines.  Each runs until t_end and returns with nothing in flight.
 * ------------------------------------------------------------------------
 */

static void
psync_run(struct job *j)
{
	for (;;) {
		uint64_t t = now_ns();
		ssize_t n;

		if (t >= t_end)
			return;
		slot_next(j, 0);
		j->slots[0].t0 = t;
		if (j->slots[0].dir == D_READ)
			n = pread(dev_fd, slot_buf(j, 0), cfg.bs, slot_off(j, 0));
		else
			n = pwrite(dev_fd, slot_buf(j, 0), cfg.bs,
			    slot_off(j, 0));
		slot_done(j, 0, n < 0 ? -errno : n, now_ns());
	}
}

static void
aio_submit(struct job *j, unsigned i, uint64_t t)
{
	struct aiocb *cb = &j->slots[i].cb;
	int rc;

	memset(cb, 0, sizeof(*cb));
	cb->aio_fildes = dev_fd;
	cb->aio_buf = slot_buf(j, i);
	cb->aio_nbytes = cfg.bs;
	cb->aio_offset = slot_off(j, i);
	j->slots[i].t0 = t;
	rc = j->slots[i].dir == D_READ ? aio_read(cb) : aio_write(cb);
	if (rc != 0)
		die("aio_%s: %s%s", j->slots[i].dir == D_READ ? "read" :
		    "write", strerror(errno), errno == EAGAIN ?
		    " (raise vfs.aio.max_aio_per_proc and "
		    "vfs.aio.max_aio_queue_per_proc)" : "");
}

static void
aio_run(struct job *j)
{
	unsigned inflight = 0;
	uint64_t t = now_ns();

	for (unsigned i = 0; i < cfg.depth; i++) {
		slot_next(j, i);
		aio_submit(j, i, t);
		inflight++;
	}
	while (inflight > 0) {
		struct aiocb *cb;
		ssize_t n = aio_waitcomplete(&cb, NULL);
		unsigned i;

		if (cb == NULL)
			die("aio_waitcomplete: %s", strerror(errno));
		i = (unsigned)(__containerof(cb, struct slot, cb) - j->slots);
		t = now_ns();
		slot_done(j, i, n < 0 ? -errno : n, t);
		inflight--;
		if (t < t_end) {
			slot_next(j, i);
			aio_submit(j, i, t);
			inflight++;
		}
	}
}

static void
kq_init(struct job *j)
{
	struct io_uring_params p;
	char *base;
	size_t len;

	memset(&p, 0, sizeof(p));
	if (cfg.sqpoll) {
		p.flags = IORING_SETUP_SQPOLL;
		p.sq_thread_idle = 1000;
	} else
		p.flags = IORING_SETUP_SINGLE_ISSUER |
		    IORING_SETUP_DEFER_TASKRUN;
	if (cfg.iopoll)
		p.flags |= IORING_SETUP_IOPOLL;
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = 2 * cfg.depth;
	j->ring_fd = (int)kqring_setup(cfg.depth, &p);
	if (j->ring_fd < 0)
		die("kqring_setup: %s (is kqring.ko loaded?)", strerror(errno));

	len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	if ((base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    j->ring_fd, IORING_OFF_SQ_RING)) == MAP_FAILED)
		die("mmap ring: %s", strerror(errno));
	if ((j->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
	    PROT_READ | PROT_WRITE, MAP_SHARED, j->ring_fd,
	    IORING_OFF_SQES)) == MAP_FAILED)
		die("mmap sqes: %s", strerror(errno));
	j->sq_khead = (_Atomic unsigned *)(void *)(base + p.sq_off.head);
	j->sq_ktail = (_Atomic unsigned *)(void *)(base + p.sq_off.tail);
	j->sq_kflags = (_Atomic unsigned *)(void *)(base + p.sq_off.flags);
	j->sq_mask = *(unsigned *)(void *)(base + p.sq_off.ring_mask);
	j->sq_entries = p.sq_entries;
	for (unsigned i = 0; i < p.sq_entries; i++)
		((unsigned *)(void *)(base + p.sq_off.array))[i] = i;
	j->cq_khead = (_Atomic unsigned *)(void *)(base + p.cq_off.head);
	j->cq_ktail = (_Atomic unsigned *)(void *)(base + p.cq_off.tail);
	j->cq_mask = *(unsigned *)(void *)(base + p.cq_off.ring_mask);
	j->cqes = (struct io_uring_cqe *)(void *)(base + p.cq_off.cqes);

	if (cfg.fixed_file && kqring_register(j->ring_fd,
	    IORING_REGISTER_FILES, &dev_fd, 1) != 0)
		die("IORING_REGISTER_FILES: %s", strerror(errno));
	if (cfg.fixed_bufs) {
		struct iovec iov[MAX_DEPTH];

		for (unsigned i = 0; i < cfg.depth; i++) {
			iov[i].iov_base = slot_buf(j, i);
			iov[i].iov_len = cfg.bs;
		}
		if (kqring_register(j->ring_fd, IORING_REGISTER_BUFFERS, iov,
		    cfg.depth) != 0)
			die("IORING_REGISTER_BUFFERS: %s%s", strerror(errno),
			    errno == ENOMEM ? " (raise RLIMIT_MEMLOCK: "
			    "ulimit -l)" : "");
	}
}

static void
kq_prep(struct job *j, unsigned i, uint64_t t)
{
	struct io_uring_sqe *sqe = &j->sqes[j->sq_tail++ & j->sq_mask];
	bool rd = j->slots[i].dir == D_READ;

	memset(sqe, 0, sizeof(*sqe));
	if (cfg.fixed_bufs) {
		sqe->opcode = rd ? IORING_OP_READ_FIXED : IORING_OP_WRITE_FIXED;
		sqe->buf_index = (uint16_t)i;
	} else
		sqe->opcode = rd ? IORING_OP_READ : IORING_OP_WRITE;
	if (cfg.fixed_file) {
		sqe->fd = 0;
		sqe->flags = IOSQE_FIXED_FILE;
	} else
		sqe->fd = dev_fd;
	sqe->addr = (uint64_t)(uintptr_t)slot_buf(j, i);
	sqe->len = (uint32_t)cfg.bs;
	sqe->off = (uint64_t)slot_off(j, i);
	sqe->user_data = i;
	j->slots[i].t0 = t;
}

/* Submit what is prepared and wait for -n completions (fewer in flight). */
static void
kq_enter(struct job *j, unsigned inflight)
{
	unsigned flags = IORING_ENTER_GETEVENTS, submit = 0;
	long rc;

	atomic_store_explicit(j->sq_ktail, j->sq_tail, memory_order_release);
	if (cfg.sqpoll) {
		atomic_thread_fence(memory_order_seq_cst);
		if (atomic_load_explicit(j->sq_kflags, memory_order_relaxed) &
		    IORING_SQ_NEED_WAKEUP)
			flags |= IORING_ENTER_SQ_WAKEUP;
	} else
		submit = j->sq_tail -
		    atomic_load_explicit(j->sq_khead, memory_order_acquire);
	do
		rc = kqring_enter(j->ring_fd, submit, MIN(cfg.wait_nr, inflight),
		    flags, NULL, 0);
	while (rc < 0 && errno == EINTR);
	if (rc < 0)
		die("kqring_enter: %s", strerror(errno));
	if (rc != (long)submit)
		die("kqring_enter: submitted %ld of %u", rc, submit);
}

static void
kq_run(struct job *j)
{
	unsigned inflight = 0;
	uint64_t t = now_ns();

	for (unsigned i = 0; i < cfg.depth; i++) {
		slot_next(j, i);
		kq_prep(j, i, t);
		inflight++;
	}
	while (inflight > 0) {
		unsigned h, tail;

		kq_enter(j, inflight);
		h = atomic_load_explicit(j->cq_khead, memory_order_relaxed);
		tail = atomic_load_explicit(j->cq_ktail, memory_order_acquire);
		t = now_ns();
		for (; h != tail; h++) {
			struct io_uring_cqe *c = &j->cqes[h & j->cq_mask];
			unsigned i = (unsigned)c->user_data;

			if (i >= cfg.depth)
				die("kqring: CQE with user_data %ju",
				    (uintmax_t)c->user_data);
			slot_done(j, i, c->res, t);
			inflight--;
			if (t < t_end) {
				slot_next(j, i);
				kq_prep(j, i, t);
				inflight++;
			}
		}
		atomic_store_explicit(j->cq_khead, h, memory_order_release);
	}
	close(j->ring_fd);
}

/* ------------------------------------------------------------------------
 * Setup, verify passes and reporting.
 * ------------------------------------------------------------------------
 */

/* -V: write every block of the slice once (generation 1), synchronously. */
static void
verify_fill(struct job *j)
{
	for (uint64_t b = 0; b < j->nblk; b++) {
		pattern_fill(j, j->bufs, b, 1);
		if (pwrite(dev_fd, j->bufs, cfg.bs,
		    (off_t)(j->base + b * cfg.bs)) != (ssize_t)cfg.bs)
			die("verify fill at %ju: %s",
			    (uintmax_t)(j->base + b * cfg.bs), strerror(errno));
		j->gen[b] = 1;
	}
}

/* -V: read every block back and check it holds its last generation. */
static void
verify_final(struct job *j)
{
	for (uint64_t b = 0; b < j->nblk; b++) {
		if (pread(dev_fd, j->bufs, cfg.bs,
		    (off_t)(j->base + b * cfg.bs)) != (ssize_t)cfg.bs)
			die("verify re-read at %ju: %s",
			    (uintmax_t)(j->base + b * cfg.bs), strerror(errno));
		pattern_check(j, j->bufs, b, j->gen[b], "final re-read");
	}
}

static void *
job_main(void *arg)
{
	struct job *j = arg;

	if (cfg.verify)
		verify_fill(j);
	if (cfg.engine == E_KQRING)
		kq_init(j);
	pthread_barrier_wait(&filled);
	pthread_barrier_wait(&started);
	switch (cfg.engine) {
	case E_KQRING:
		kq_run(j);
		break;
	case E_AIO:
		aio_run(j);
		break;
	case E_PSYNC:
		psync_run(j);
		break;
	}
	if (cfg.verify)
		verify_final(j);
	return (NULL);
}

static uint64_t
parse_size(const char *s)
{
	char *end;
	uint64_t v = strtoull(s, &end, 0);

	switch (*end) {
	case 'g': case 'G':
		v <<= 10;
		/* FALLTHROUGH */
	case 'm': case 'M':
		v <<= 10;
		/* FALLTHROUGH */
	case 'k': case 'K':
		v <<= 10;
		end++;
		break;
	}
	if (*end != '\0' || v == 0)
		die("bad size: %s", s);
	return (v);
}

static void __dead2
usage(void)
{
	fprintf(stderr, "usage: kqring_diskbench [-e kqring|aio|psync] "
	    "[-w read|write|randread|randwrite|randrw]\n"
	    "\t[-b bs] [-q depth] [-j jobs] [-t secs] [-r ramp] [-s size] "
	    "[-m read%%] [-n wait]\n\t[-FBPIV] [-f] [-L label] target\n");
	exit(2);
}

static void
report(const char *label, int dir, const struct hist *h, uint64_t ios,
    double secs)
{
	double iops = (double)ios / secs;
	double mibs = iops * (double)cfg.bs / (1024.0 * 1024.0);

	printf("  %-5s IOPS %.0f, %.1f MiB/s, latency (us) p50 %.1f, "
	    "p99 %.1f, p99.9 %.1f, max %.1f\n", dir == D_READ ? "read" :
	    "write", iops, mibs, hist_pct_us(h, 50), hist_pct_us(h, 99),
	    hist_pct_us(h, 99.9), h->max / 1000.0);
	printf("ROW diskbench %s %s %.0f %.1f %.1f %.1f %.1f\n", label,
	    dir == D_READ ? "read" : "write", iops, mibs, hist_pct_us(h, 50),
	    hist_pct_us(h, 99), hist_pct_us(h, 99.9));
}

int
main(int argc, char **argv)
{
	static struct hist total[2];
	uint64_t ios[2] = { 0, 0 }, checked = 0, avail, slice;
	char label[64];
	const char *name;
	struct stat st;
	bool writes, isfile;
	int ch;

	while ((ch = getopt(argc, argv, "b:e:fj:m:n:q:r:s:t:w:BFIL:PV")) != -1) {
		switch (ch) {
		case 'b':
			cfg.bs = parse_size(optarg);
			break;
		case 'e':
			if (strcmp(optarg, "kqring") == 0)
				cfg.engine = E_KQRING;
			else if (strcmp(optarg, "aio") == 0)
				cfg.engine = E_AIO;
			else if (strcmp(optarg, "psync") == 0)
				cfg.engine = E_PSYNC;
			else
				usage();
			break;
		case 'f':
			cfg.force = true;
			break;
		case 'j':
			cfg.jobs = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 'm':
			cfg.readpct = atoi(optarg);
			break;
		case 'n':
			cfg.wait_nr = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 'q':
			cfg.depth = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 'r':
			cfg.ramp = atoi(optarg);
			break;
		case 's':
			cfg.size = parse_size(optarg);
			break;
		case 't':
			cfg.secs = atoi(optarg);
			break;
		case 'w':
			strlcpy(cfg.workload, optarg, sizeof(cfg.workload));
			break;
		case 'B':
			cfg.fixed_bufs = true;
			break;
		case 'F':
			cfg.fixed_file = true;
			break;
		case 'I':
			cfg.iopoll = true;
			break;
		case 'L':
			cfg.label = optarg;
			break;
		case 'P':
			cfg.sqpoll = true;
			break;
		case 'V':
			cfg.verify = true;
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1)
		usage();
	cfg.target = argv[optind];

	if (strcmp(cfg.workload, "randrw") == 0) {
		if (cfg.readpct == 100)
			cfg.readpct = 50;
	} else if (strcmp(cfg.workload, "read") == 0 ||
	    strcmp(cfg.workload, "randread") == 0)
		cfg.readpct = 100;
	else if (strcmp(cfg.workload, "write") == 0 ||
	    strcmp(cfg.workload, "randwrite") == 0)
		cfg.readpct = 0;
	else
		usage();
	cfg.rand = strncmp(cfg.workload, "rand", 4) == 0;
	if (cfg.readpct < 0 || cfg.readpct > 100 || cfg.secs <= 0 ||
	    cfg.ramp < 0 || cfg.jobs == 0 || cfg.jobs > MAX_JOBS ||
	    cfg.wait_nr == 0)
		usage();
	if (cfg.engine == E_PSYNC)
		cfg.depth = 1;
	if (cfg.depth == 0 || cfg.depth > MAX_DEPTH)
		die("queue depth must be 1 to %d", MAX_DEPTH);
	if (cfg.bs < 512 || cfg.bs % 512 != 0 || cfg.bs > (16 << 20))
		die("block size must be a multiple of 512, at most 16m");
	if (cfg.engine != E_KQRING && (cfg.fixed_file || cfg.fixed_bufs ||
	    cfg.sqpoll || cfg.iopoll))
		die("-F, -B, -P and -I are kqring options");

	writes = cfg.readpct < 100 || cfg.verify;
	if (stat(cfg.target, &st) == 0)
		isfile = S_ISREG(st.st_mode);
	else if (errno == ENOENT && cfg.size != 0)
		isfile = true;		/* created below */
	else
		die("%s: %s", cfg.target, strerror(errno));
	name = strrchr(cfg.target, '/');
	name = name != NULL ? name + 1 : cfg.target;
	if (writes && !isfile && !cfg.force && (strncmp(name, "md", 2) != 0 ||
	    name[2] < '0' || name[2] > '9'))
		die("%s is not an md(4) disk: refusing to write without -f",
		    cfg.target);
	/* A file it may have to create or extend needs O_RDWR too. */
	dev_fd = open(cfg.target, (writes || (isfile && cfg.size != 0) ?
	    O_RDWR : O_RDONLY) |
	    (isfile ? O_DIRECT | O_CREAT : 0), 0644);
	if (dev_fd < 0)
		die("%s: %s", cfg.target, strerror(errno));
	if (isfile) {
		if (fstat(dev_fd, &st) != 0)
			die("fstat: %s", strerror(errno));
		if (cfg.size != 0 && (uint64_t)st.st_size < cfg.size &&
		    ftruncate(dev_fd, (off_t)cfg.size) != 0)
			die("ftruncate: %s", strerror(errno));
		avail = cfg.size != 0 ? cfg.size : (uint64_t)st.st_size;
	} else {
		off_t media;

		if (ioctl(dev_fd, DIOCGMEDIASIZE, &media) != 0)
			die("%s: DIOCGMEDIASIZE: %s", cfg.target,
			    strerror(errno));
		avail = (uint64_t)media;
		if (cfg.size != 0)
			avail = MIN(avail, cfg.size);
	}
	if (cfg.size == 0)
		avail = MIN(avail, (uint64_t)1 << 30);
	slice = avail / cfg.jobs / cfg.bs;
	if (slice == 0 || (cfg.verify && slice <= cfg.depth))
		die("%ju bytes is too small for %u jobs of %ju-byte blocks%s",
		    (uintmax_t)avail, cfg.jobs, (uintmax_t)cfg.bs,
		    cfg.verify ? " at this queue depth" : "");

	if ((jobs = calloc(cfg.jobs, sizeof(*jobs))) == NULL)
		die("calloc: %s", strerror(errno));
	for (unsigned k = 0; k < cfg.jobs; k++) {
		struct job *j = &jobs[k];

		j->id = k;
		j->base = (uint64_t)k * slice * cfg.bs;
		j->nblk = slice;
		j->rng = 0x2545f4914f6cdd1dULL * (k + 1);
		j->bufs = mmap(NULL, (size_t)cfg.depth * cfg.bs,
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (j->bufs == MAP_FAILED)
			die("mmap buffers: %s", strerror(errno));
		for (size_t b = 0; b < (size_t)cfg.depth * cfg.bs; b += 8)
			*(uint64_t *)(void *)(j->bufs + b) = xorshift(&j->rng);
		if (cfg.verify && ((j->gen = calloc(slice, sizeof(*j->gen))) ==
		    NULL || (j->busy = calloc(slice, 1)) == NULL))
			die("calloc: %s", strerror(errno));
	}

	if (cfg.label != NULL)
		strlcpy(label, cfg.label, sizeof(label));
	else
		snprintf(label, sizeof(label), "%s%s%s%s%s",
		    cfg.engine == E_KQRING ? "kqring" : cfg.engine == E_AIO ?
		    "aio" : "psync", cfg.fixed_file ? "+F" : "",
		    cfg.fixed_bufs ? "+B" : "", cfg.sqpoll ? "+P" : "",
		    cfg.iopoll ? "+I" : "");
	printf("%s: %s %s bs=%ju qd=%u jobs=%u area=%ju MiB%s\n", label,
	    cfg.target, cfg.workload, (uintmax_t)cfg.bs, cfg.depth, cfg.jobs,
	    (uintmax_t)(slice * cfg.bs * cfg.jobs >> 20),
	    cfg.verify ? " verify" : "");
	fflush(stdout);

	if (pthread_barrier_init(&filled, NULL, cfg.jobs + 1) != 0 ||
	    pthread_barrier_init(&started, NULL, cfg.jobs + 1) != 0)
		die("pthread_barrier_init");
	for (unsigned k = 0; k < cfg.jobs; k++)
		if (pthread_create(&jobs[k].thr, NULL, job_main, &jobs[k]) != 0)
			die("pthread_create");
	/* Every job is set up (and -V filled); the clock starts now. */
	pthread_barrier_wait(&filled);
	t_start = now_ns();
	t_measure = t_start + (uint64_t)cfg.ramp * 1000000000ULL;
	t_end = t_measure + (uint64_t)cfg.secs * 1000000000ULL;
	pthread_barrier_wait(&started);
	for (unsigned k = 0; k < cfg.jobs; k++) {
		pthread_join(jobs[k].thr, NULL);
		for (int d = 0; d < 2; d++) {
			hist_merge(&total[d], &jobs[k].lat[d]);
			ios[d] += jobs[k].ios[d];
		}
		checked += jobs[k].checked;
	}

	for (int d = 0; d < 2; d++)
		if (ios[d] != 0)
			report(label, d, &total[d], ios[d], cfg.secs);
	if (cfg.verify)
		printf("  verify: %ju blocks checked, all as written\n",
		    (uintmax_t)checked);
	return (0);
}
