/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * kqring_stress: randomized multi-threaded stress for the races that unit
 * tests cannot time: socket upcalls against disarm and cancellation,
 * listen(2) and dup2(2) under armed requests, ring teardown with requests
 * in flight, CQ overflow, zero-copy notifications, and fork(2) with a
 * provided-buffer ring registered.
 *
 * Issuer threads each own a ring, recreated with random setup flags, and
 * submit a random mix of requests on a shared pool of sockets.  Chaos
 * threads meanwhile write to the pool, drain it, shut sockets down, turn
 * them into listeners and replace them with dup2(2).  It fails (exit 1) on
 *
 *  - a CQE that matches no outstanding request (a stale or second final
 *    CQE) or a result out of range;
 *  - requests that a cancel-all does not finish within 10 s (a hang);
 *  - a canary RECV, fed every millisecond by another thread, that sees no
 *    completion for 5 s (a lost wakeup, or a lost IORING_SQ_TASKRUN: on
 *    DEFER_TASKRUN rings the issuer then only enters when the flag is set,
 *    as liburing does);
 *  - an issuer that stays inside one system call for 30 s;
 *  - sockets no descriptor refers to, user-wired pages or kqring malloc
 *    memory still held once everything is closed (a leak).
 *
 * A kernel panic is a failure too: run it with debug.debugger_on_panic=0.
 * Raise RLIMIT_MEMLOCK (ulimit -l 8192) to cover registered buffers too.
 *
 * usage: kqring_stress [-t seconds] [-j issuers] [-s seed]
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <arpa/inet.h>

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
#include <unistd.h>

#include <sys/kqring.h>

#include "kqring_leak.h"

#define NSLOT		48		/* shared socket pool */
#define NCHAOS		2
#define MAXOPS		256		/* outstanding requests per ring */
#define RBUF		512		/* per-request receive buffer */
#define NBUF		32		/* provided buffers, group 0 */
#define BUFSZ		2048
#define ZC_LEN		(256 * 1024)
#define MAX_MULTI	4
#define MAX_SENDS	16
#define DRAIN_NS	(10 * 1000000000ULL)
#define CANARY_NS	(5 * 1000000000ULL)
#define STUCK_NS	(30 * 1000000000ULL)

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------
 */

static _Atomic bool stop;
static _Atomic int nrunning;	/* threads that have not returned yet */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void fail(bool hang, const char *fmt, ...) __dead2 __printflike(2, 3);

static void
fail(bool hang, const char *fmt, ...)
{
	char cmd[64];
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "kqring_stress: FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	if (hang) {
		/* Where each thread is waiting in the kernel. */
		snprintf(cmd, sizeof(cmd), "procstat -kk %d >&2", (int)getpid());
		(void)system(cmd);
	}
	fflush(stderr);
	_exit(1);
}

static uint32_t
rnd(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return (*s = x);
}

/* ------------------------------------------------------------------------
 * The shared socket pool.  A slot's two descriptor numbers never change:
 * chaos threads replace the sockets behind them with dup2(2), so requests
 * only ever name pool sockets (or the issuer's own), never a reused number.
 * ------------------------------------------------------------------------
 */

enum { SK_UNIX_STREAM, SK_UNIX_DGRAM, SK_TCP, SK_TCP_RAW, SK_N };

struct slot {
	pthread_mutex_t	mtx;
	int		fd[2];
	_Atomic int	kind;
	bool		listening;
};

static struct slot slots[NSLOT];
static pthread_mutex_t tcp_mtx = PTHREAD_MUTEX_INITIALIZER;
static int tcp_lfd = -1;
static struct sockaddr_in tcp_addr;
static _Atomic uint64_t st_recycle, st_listen, st_tcp_retry;

/*
 * A connected loopback TCP pair.  Returns false if connect(2) keeps
 * failing: a packet filter that keeps state (pf "keep state") drops a SYN
 * whose ports match a recently closed connection, with EACCES, and this
 * churns through many connections to one listener port.
 */
static bool
tcp_pair(int sv[2])
{
	int c, a;

	pthread_mutex_lock(&tcp_mtx);
	for (int tries = 0; tries < 20; tries++) {
		struct pollfd pfd;
		int err = 0;
		socklen_t elen = sizeof(err);

		/* Non-blocking: a silently dropped SYN would block for 75 s. */
		c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (c < 0)
			fail(false, "socket: %s", strerror(errno));
		if (connect(c, (struct sockaddr *)&tcp_addr,
		    sizeof(tcp_addr)) != 0) {
			err = errno;
			if (err == EINPROGRESS) {
				pfd.fd = c;
				pfd.events = POLLOUT;
				err = poll(&pfd, 1, 1000) == 1 &&
				    getsockopt(c, SOL_SOCKET, SO_ERROR, &err,
				    &elen) == 0 ? err : ETIMEDOUT;
			}
		}
		if (err == 0) {
			pfd.fd = tcp_lfd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 1000) == 1 &&
			    (a = accept(tcp_lfd, NULL, NULL)) >= 0) {
				pthread_mutex_unlock(&tcp_mtx);
				(void)fcntl(c, F_SETFL, 0);
				sv[0] = c;
				sv[1] = a;
				return (true);
			}
			err = ETIMEDOUT;
		}
		if (err != EACCES && err != EADDRNOTAVAIL &&
		    err != ECONNREFUSED && err != ECONNRESET &&
		    err != EADDRINUSE && err != ETIMEDOUT)
			fail(false, "tcp connect to %s:%u: %s",
			    inet_ntoa(tcp_addr.sin_addr),
			    ntohs(tcp_addr.sin_port), strerror(err));
		close(c);
		atomic_fetch_add(&st_tcp_retry, 1);
		usleep(1000);
	}
	pthread_mutex_unlock(&tcp_mtx);
	return (false);
}

static void
make_pair(int kind, int sv[2])
{
	switch (kind) {
	case SK_UNIX_STREAM:
	case SK_UNIX_DGRAM:
		if (socketpair(AF_UNIX, kind == SK_UNIX_STREAM ? SOCK_STREAM :
		    SOCK_DGRAM, 0, sv) != 0)
			fail(false, "socketpair: %s", strerror(errno));
		break;
	case SK_TCP:
		if (!tcp_pair(sv) && socketpair(AF_UNIX, SOCK_STREAM, 0,
		    sv) != 0)
			fail(false, "socketpair: %s", strerror(errno));
		break;
	default:
		/* Unconnected: a chaos thread may make one a listener. */
		sv[0] = socket(AF_INET, SOCK_STREAM, 0);
		sv[1] = socket(AF_INET, SOCK_STREAM, 0);
		if (sv[0] < 0 || sv[1] < 0)
			fail(false, "socket: %s", strerror(errno));
		break;
	}
}

static void
slot_recycle(struct slot *s, int kind)
{
	int sv[2];

	make_pair(kind, sv);
	pthread_mutex_lock(&s->mtx);
	if (s->fd[0] < 0) {
		s->fd[0] = sv[0];
		s->fd[1] = sv[1];
	} else {
		/* Atomically replaces the socket under any armed request. */
		if (dup2(sv[0], s->fd[0]) < 0 || dup2(sv[1], s->fd[1]) < 0)
			fail(false, "dup2: %s", strerror(errno));
		close(sv[0]);
		close(sv[1]);
	}
	s->listening = false;
	atomic_store(&s->kind, kind);
	pthread_mutex_unlock(&s->mtx);
}

/* listen(2) on an unconnected slot under whatever is armed on it. */
static void
slot_listen(struct slot *s, uint32_t *seed)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	int n;

	pthread_mutex_lock(&s->mtx);
	if (atomic_load(&s->kind) != SK_TCP_RAW || s->listening) {
		pthread_mutex_unlock(&s->mtx);
		return;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s->fd[0], (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
	    listen(s->fd[0], 16) != 0 ||
	    getsockname(s->fd[0], (struct sockaddr *)&sin, &len) != 0) {
		pthread_mutex_unlock(&s->mtx);
		return;
	}
	s->listening = true;
	pthread_mutex_unlock(&s->mtx);
	atomic_fetch_add(&st_listen, 1);
	/* A few connections for ACCEPTs to take; never wait for them. */
	n = (int)(rnd(seed) % 4);
	for (int i = 0; i < n; i++) {
		int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

		if (c < 0)
			break;
		(void)connect(c, (struct sockaddr *)&sin, sizeof(sin));
		if ((rnd(seed) & 1) != 0)
			usleep(100);
		close(c);
	}
}

static void *
chaos_main(void *arg)
{
	uint32_t seed = (uint32_t)(uintptr_t)arg;
	static char junk[16384];
	char sink[16384];

	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		struct slot *s = &slots[rnd(&seed) % NSLOT];
		int side = (int)(rnd(&seed) & 1);
		uint32_t a = rnd(&seed) % 100;

		if (a < 55) {
			(void)send(s->fd[side], junk, 1 + rnd(&seed) % 8192,
			    MSG_DONTWAIT);
		} else if (a < 85) {
			for (int i = 0; i < 4; i++)
				if (recv(s->fd[side], sink, sizeof(sink),
				    MSG_DONTWAIT) <= 0)
					break;
		} else if (a < 93) {
			slot_recycle(s, (int)(rnd(&seed) % SK_N));
			atomic_fetch_add(&st_recycle, 1);
		} else if (a < 97) {
			slot_listen(s, &seed);
		} else if (a < 99) {
			(void)shutdown(s->fd[side], (int)(rnd(&seed) % 3));
		} else {
			usleep(rnd(&seed) % 500);
		}
	}
	atomic_fetch_sub(&nrunning, 1);
	return (NULL);
}

/* ------------------------------------------------------------------------
 * Rings
 * ------------------------------------------------------------------------
 */

struct ring {
	int			fd;
	bool			defer;
	unsigned		sq_entries;
	void			*map;
	size_t			map_len;
	struct io_uring_sqe	*sqes;
	size_t			sqes_len;
	_Atomic unsigned	*sq_khead, *sq_ktail, *sq_kflags;
	_Atomic unsigned	*cq_khead, *cq_ktail;
	unsigned		sq_mask, cq_mask, sq_tail;
	struct io_uring_cqe	*cqes;
};

static void
ring_open(struct ring *r, unsigned flags, unsigned sq, unsigned cq)
{
	struct io_uring_params p;
	char *base;

	memset(&p, 0, sizeof(p));
	p.flags = flags;
	if (cq != 0) {
		p.flags |= IORING_SETUP_CQSIZE;
		p.cq_entries = cq;
	}
	memset(r, 0, sizeof(*r));
	r->fd = (int)kqring_setup(sq, &p);
	if (r->fd < 0)
		fail(false, "kqring_setup(%u, %#x): %s", sq, p.flags,
		    strerror(errno));
	r->defer = (flags & IORING_SETUP_DEFER_TASKRUN) != 0;
	r->sq_entries = p.sq_entries;
	r->map_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	r->map = mmap(NULL, r->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    r->fd, IORING_OFF_SQ_RING);
	r->sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
	r->sqes = mmap(NULL, r->sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED,
	    r->fd, IORING_OFF_SQES);
	if (r->map == MAP_FAILED || r->sqes == MAP_FAILED)
		fail(false, "ring mmap: %s", strerror(errno));
	base = r->map;
	r->sq_khead = (_Atomic unsigned *)(base + p.sq_off.head);
	r->sq_ktail = (_Atomic unsigned *)(base + p.sq_off.tail);
	r->sq_kflags = (_Atomic unsigned *)(base + p.sq_off.flags);
	r->sq_mask = *(unsigned *)(base + p.sq_off.ring_mask);
	unsigned *arr = (unsigned *)(base + p.sq_off.array);
	for (unsigned i = 0; i < p.sq_entries; i++)
		arr[i] = i;
	r->cq_khead = (_Atomic unsigned *)(base + p.cq_off.head);
	r->cq_ktail = (_Atomic unsigned *)(base + p.cq_off.tail);
	r->cq_mask = *(unsigned *)(base + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
}

static void
ring_close(struct ring *r)
{
	munmap(r->map, r->map_len);
	munmap(r->sqes, r->sqes_len);
	close(r->fd);
	r->fd = -1;
}

static unsigned
ring_sq_space(struct ring *r)
{
	return (r->sq_entries - (r->sq_tail -
	    atomic_load_explicit(r->sq_khead, memory_order_acquire)));
}

static struct io_uring_sqe *
ring_sqe(struct ring *r, uint8_t opcode, int fd, uint64_t ud)
{
	struct io_uring_sqe *sqe = &r->sqes[r->sq_tail++ & r->sq_mask];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = opcode;
	sqe->fd = fd;
	sqe->user_data = ud;
	return (sqe);
}

/*
 * Submit what is queued and, with min > 0, wait up to ns for CQEs.
 * getevents with min 0 only runs deferred task work (DEFER_TASKRUN).
 */
static void
ring_enter(struct ring *r, unsigned min, uint64_t ns, bool getevents)
{
	struct __kernel_timespec ts = {
		.tv_sec = (int64_t)(ns / 1000000000ULL),
		.tv_nsec = (long long)(ns % 1000000000ULL),
	};
	struct io_uring_getevents_arg ga = {
		.ts = (uint64_t)(uintptr_t)&ts,
	};
	unsigned n, flags = 0;
	long rc;

	atomic_store_explicit(r->sq_ktail, r->sq_tail, memory_order_release);
	n = r->sq_tail - atomic_load_explicit(r->sq_khead,
	    memory_order_acquire);
	if (min > 0)
		flags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
	else if (getevents)
		flags = IORING_ENTER_GETEVENTS;
	rc = kqring_enter(r->fd, n, min, flags, min > 0 ? &ga : NULL,
	    min > 0 ? sizeof(ga) : 0);
	if (rc < 0 && errno != ETIMEDOUT && errno != EINTR &&
	    errno != EBUSY && errno != EAGAIN && errno != EOVERFLOW
#ifdef ETIME
	    && errno != ETIME
#endif
	    )
		fail(false, "io_uring_enter(%u, %u, %#x): %s", n, min, flags,
		    strerror(errno));
}

/* ------------------------------------------------------------------------
 * Issuers
 * ------------------------------------------------------------------------
 */

enum {
	K_RECV, K_RECV_WAITALL, K_RECV_BS, K_RECV_MS, K_POLL, K_POLL_MS,
	K_SEND, K_SEND_ZC, K_CANCEL, K_LTO, K_ACCEPT, K_NOP, K_TIMEOUT,
	K_CANARY, K_N
};

static const char *const kname[K_N] = {
	"recv", "recv_waitall", "recv_bufsel", "recv_multishot", "poll",
	"poll_multishot", "send", "send_zc", "cancel", "link_timeout",
	"accept", "nop", "timeout", "canary",
};

static _Atomic uint64_t st_cqe[K_N], st_rings, st_abrupt, st_drains,
    st_forks;

struct op {
	bool		used, got_res, notif;
	uint8_t		kind;
	uint16_t	gen;
	uint32_t	len;
};

struct issuer {
	int			id;
	pthread_t		td;
	uint32_t		seed;
	struct ring		r;
	struct op		ops[MAXOPS];
	struct __kernel_timespec ts[MAXOPS];
	unsigned		nout, nmulti, nsend;
	uint16_t		gen;
	char			*rbuf, *zcbuf;
	/* Provided-buffer ring, group 0. */
	bool			has_br, br_user;
	struct io_uring_buf_ring *br, *br_user_mem;
	size_t			br_len;
	char			*bufs;
	uint16_t		br_tail;
	bool			fixed_buf;
	int			priv[2];	/* SEND_ZC target, drained here */
	int			canary[2];	/* fed by the poker thread */
	bool			canary_out;
	uint64_t		canary_since, canary_hits;
	_Atomic uint64_t	in_call;	/* when it entered, 0 if out */
};

static struct issuer *issuers;
static int nissuers;

#define RN(it, n)	(rnd(&(it)->seed) % (n))

static int
op_alloc(struct issuer *it, uint8_t kind, uint32_t len)
{
	int start = (int)RN(it, MAXOPS);

	/* A random free slot, so that indices (and user_data) get reused. */
	for (int n = 0; n < MAXOPS; n++) {
		int i = (start + n) % MAXOPS;

		if (!it->ops[i].used) {
			struct op *o = &it->ops[i];

			o->used = true;
			o->got_res = o->notif = false;
			o->kind = kind;
			o->gen = ++it->gen;
			o->len = len;
			it->nout++;
			if (kind == K_RECV_MS || kind == K_POLL_MS)
				it->nmulti++;
			if (kind == K_SEND || kind == K_SEND_ZC)
				it->nsend++;
			return (i);
		}
	}
	return (-1);
}

static uint64_t
op_ud(struct issuer *it, int i)
{
	return (((uint64_t)it->ops[i].gen << 16) | (uint64_t)i);
}

static void
op_free(struct issuer *it, int i)
{
	struct op *o = &it->ops[i];

	if (o->kind == K_RECV_MS || o->kind == K_POLL_MS)
		it->nmulti--;
	if (o->kind == K_SEND || o->kind == K_SEND_ZC)
		it->nsend--;
	if (o->kind == K_CANARY) {
		it->canary_out = false;
		it->canary_hits++;
	}
	o->used = false;
	it->nout--;
}

static void
br_offer(struct issuer *it, uint16_t bid)
{
	struct io_uring_buf *b = &it->br->bufs[it->br_tail & (NBUF - 1)];

	b->addr = (uint64_t)(uintptr_t)(it->bufs + (size_t)bid * BUFSZ);
	b->len = BUFSZ;
	b->bid = bid;
	it->br_tail++;
	atomic_store_explicit((_Atomic uint16_t *)&it->br->tail, it->br_tail,
	    memory_order_release);
}

static void
br_setup(struct issuer *it)
{
	struct io_uring_buf_reg reg;

	it->has_br = false;
	memset(&reg, 0, sizeof(reg));
	reg.ring_entries = NBUF;
	it->br_user = RN(it, 2) == 0;
	if (it->br_user) {
		/* The same user page backs every ring this thread makes. */
		memset(it->br_user_mem, 0, it->br_len);
		reg.ring_addr = (uint64_t)(uintptr_t)it->br_user_mem;
		if (kqring_register(it->r.fd, IORING_REGISTER_PBUF_RING, &reg,
		    1) != 0)
			return;
		it->br = it->br_user_mem;
	} else {
		reg.flags = IOU_PBUF_RING_MMAP;
		if (kqring_register(it->r.fd, IORING_REGISTER_PBUF_RING, &reg,
		    1) != 0)
			return;
		it->br = mmap(NULL, it->br_len, PROT_READ | PROT_WRITE,
		    MAP_SHARED, it->r.fd, (off_t)IORING_OFF_PBUF_RING);
		if (it->br == MAP_FAILED)
			fail(false, "pbuf ring mmap: %s", strerror(errno));
	}
	it->br_tail = 0;
	for (uint16_t i = 0; i < NBUF; i++)
		br_offer(it, i);
	it->has_br = true;
}

static void
issuer_ring_open(struct issuer *it)
{
	static const unsigned modes[] = {
		0,
		IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
		    IORING_SETUP_TASKRUN_FLAG,
		IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG,
		IORING_SETUP_SINGLE_ISSUER,
	};
	unsigned flags = modes[RN(it, nitems(modes))];
	bool small = RN(it, 4) == 0;	/* a CQ that overflows */

	ring_open(&it->r, flags, small ? 8 : 64, small ? 16 : 0);
	memset(it->ops, 0, sizeof(it->ops));
	it->nout = it->nmulti = it->nsend = 0;
	it->canary_out = false;
	if (RN(it, 10) < 7)
		br_setup(it);
	else
		it->has_br = false;
	it->fixed_buf = false;
	if (RN(it, 3) == 0) {
		struct iovec iov = { .iov_base = it->zcbuf, .iov_len = ZC_LEN };

		/* Needs RLIMIT_MEMLOCK above ZC_LEN; fine if it fails. */
		it->fixed_buf = kqring_register(it->r.fd,
		    IORING_REGISTER_BUFFERS, &iov, 1) == 0;
	}
	atomic_fetch_add(&st_rings, 1);
}

static void
issuer_ring_close(struct issuer *it)
{
	if (it->has_br && !it->br_user)
		munmap(it->br, it->br_len);
	it->has_br = false;
	/* Teardown with requests in flight: timed like an enter. */
	atomic_store_explicit(&it->in_call, now_ns(), memory_order_relaxed);
	ring_close(&it->r);
	atomic_store_explicit(&it->in_call, 0, memory_order_relaxed);
	memset(it->ops, 0, sizeof(it->ops));
	it->nout = it->nmulti = it->nsend = 0;
	it->canary_out = false;
}

static void
drain_fd(int fd)
{
	char sink[65536];

	for (int i = 0; i < 64; i++)
		if (recv(fd, sink, sizeof(sink), MSG_DONTWAIT) <= 0)
			break;
}

static void
process_cqe(struct issuer *it, const struct io_uring_cqe *c)
{
	unsigned i = (unsigned)(c->user_data & 0xffff);
	uint16_t gen = (uint16_t)(c->user_data >> 16);
	struct op *o;

	if (i >= MAXOPS || !it->ops[i].used || it->ops[i].gen != gen)
		fail(false, "issuer %d: CQE for no outstanding request: "
		    "user_data %#jx res %d flags %#x", it->id,
		    (uintmax_t)c->user_data, c->res, c->flags);
	o = &it->ops[i];
	atomic_fetch_add_explicit(&st_cqe[o->kind], 1, memory_order_relaxed);
	if ((c->flags & IORING_CQE_F_BUFFER) != 0) {
		uint16_t bid = (uint16_t)(c->flags >> IORING_CQE_BUFFER_SHIFT);

		if (!it->has_br || bid >= NBUF)
			fail(false, "issuer %d: %s: buffer id %u", it->id,
			    kname[o->kind], bid);
		if (c->res > BUFSZ)
			fail(false, "issuer %d: %s: %d bytes in a %d-byte "
			    "buffer", it->id, kname[o->kind], c->res, BUFSZ);
		br_offer(it, bid);
	}
	switch (o->kind) {
	case K_RECV:
	case K_RECV_WAITALL:
	case K_CANARY:
		if (c->res > (int32_t)o->len)
			fail(false, "issuer %d: %s: %d bytes for %u asked",
			    it->id, kname[o->kind], c->res, o->len);
		break;
	case K_ACCEPT:
		if (c->res >= 0)
			close(c->res);
		break;
	case K_SEND_ZC:
		/* A result CQE (with F_MORE if a NOTIF follows), a NOTIF. */
		if ((c->flags & IORING_CQE_F_NOTIF) != 0) {
			if (!o->got_res || !o->notif)
				fail(false, "issuer %d: send_zc: unexpected "
				    "NOTIF", it->id);
			o->notif = false;
		} else {
			if (o->got_res)
				fail(false, "issuer %d: send_zc: second "
				    "result %d", it->id, c->res);
			o->got_res = true;
			o->notif = (c->flags & IORING_CQE_F_MORE) != 0;
		}
		if (!o->notif)
			op_free(it, (int)i);
		return;
	default:
		break;
	}
	if (o->kind == K_CANARY)
		drain_fd(it->canary[0]);
	if ((c->flags & IORING_CQE_F_MORE) != 0) {
		if (o->kind != K_RECV_MS && o->kind != K_POLL_MS)
			fail(false, "issuer %d: %s: F_MORE on a single-shot "
			    "request", it->id, kname[o->kind]);
		return;
	}
	op_free(it, (int)i);
}

static unsigned
reap(struct issuer *it)
{
	struct ring *r = &it->r;
	unsigned h = atomic_load_explicit(r->cq_khead, memory_order_relaxed);
	unsigned t = atomic_load_explicit(r->cq_ktail, memory_order_acquire);
	unsigned n = 0;

	for (; h != t; h++, n++) {
		struct io_uring_cqe c = r->cqes[h & r->cq_mask];

		atomic_store_explicit(r->cq_khead, h + 1, memory_order_release);
		process_cqe(it, &c);
	}
	return (n);
}

/* Each enter is timed, to find an issuer stuck in the kernel. */
static void
enter(struct issuer *it, unsigned min, uint64_t ns)
{
	atomic_store_explicit(&it->in_call, now_ns(), memory_order_relaxed);
	ring_enter(&it->r, min, ns, true);
	atomic_store_explicit(&it->in_call, 0, memory_order_relaxed);
}

static int
pool_fd(struct issuer *it)
{
	return (slots[RN(it, NSLOT)].fd[RN(it, 2)]);
}

static void
arm_canary(struct issuer *it)
{
	struct io_uring_sqe *sqe;
	int i;

	if (it->canary_out || ring_sq_space(&it->r) == 0 ||
	    (i = op_alloc(it, K_CANARY, 64)) < 0)
		return;
	if (RN(it, 3) == 0) {
		sqe = ring_sqe(&it->r, IORING_OP_POLL_ADD, it->canary[0],
		    op_ud(it, i));
		sqe->poll32_events = POLLIN;
	} else {
		sqe = ring_sqe(&it->r, IORING_OP_RECV, it->canary[0],
		    op_ud(it, i));
		sqe->addr = (uint64_t)(uintptr_t)(it->rbuf + (size_t)i * RBUF);
		sqe->len = 64;
	}
	it->canary_out = true;
	it->canary_since = now_ns();
}

static void
check_canary(struct issuer *it)
{
	if (it->canary_out && now_ns() - it->canary_since > CANARY_NS)
		fail(true, "issuer %d: canary request (ring %s) got no "
		    "completion in %llu s: lost wakeup; sq_flags %#x, "
		    "%u outstanding", it->id, it->r.defer ? "DEFER_TASKRUN" :
		    "plain", (unsigned long long)(CANARY_NS / 1000000000ULL),
		    atomic_load(it->r.sq_kflags), it->nout);
}

/* Queue one random request (or a short NOP burst). */
static void
issue_random(struct issuer *it)
{
	struct ring *r = &it->r;
	struct io_uring_sqe *sqe;
	uint32_t a = RN(it, 100), len;
	int i, j, fd;

	if (ring_sq_space(r) < 2 || it->nout + 2 > MAXOPS)
		return;
	fd = pool_fd(it);
	if (a < 22) {
		bool waitall = RN(it, 5) == 0;

		len = 1 + RN(it, RBUF);
		if ((i = op_alloc(it, waitall ? K_RECV_WAITALL : K_RECV,
		    len)) < 0)
			return;
		sqe = ring_sqe(r, IORING_OP_RECV, fd, op_ud(it, i));
		sqe->addr = (uint64_t)(uintptr_t)(it->rbuf + (size_t)i * RBUF);
		sqe->len = len;
		if (waitall)
			sqe->msg_flags = MSG_WAITALL;
		if (RN(it, 10) == 0)
			sqe->flags |= IOSQE_ASYNC;
		/* A link timeout racing the upcall. */
		if (RN(it, 3) == 0 && (j = op_alloc(it, K_LTO, 0)) >= 0) {
			sqe->flags |= IOSQE_IO_LINK;
			it->ts[j].tv_sec = 0;
			it->ts[j].tv_nsec = (long long)RN(it, 3000000);
			sqe = ring_sqe(r, IORING_OP_LINK_TIMEOUT, -1,
			    op_ud(it, j));
			sqe->addr = (uint64_t)(uintptr_t)&it->ts[j];
			sqe->len = 1;
		}
	} else if (a < 32 && it->has_br) {
		bool multi = it->nmulti < MAX_MULTI && RN(it, 2) == 0;

		if ((i = op_alloc(it, multi ? K_RECV_MS : K_RECV_BS,
		    BUFSZ)) < 0)
			return;
		sqe = ring_sqe(r, IORING_OP_RECV, fd, op_ud(it, i));
		sqe->flags = IOSQE_BUFFER_SELECT;
		sqe->buf_group = 0;
		if (multi)
			sqe->ioprio = IORING_RECV_MULTISHOT;
	} else if (a < 44) {
		bool multi = it->nmulti < MAX_MULTI && RN(it, 3) == 0;

		if ((i = op_alloc(it, multi ? K_POLL_MS : K_POLL, 0)) < 0)
			return;
		sqe = ring_sqe(r, IORING_OP_POLL_ADD, fd, op_ud(it, i));
		sqe->poll32_events = multi ? POLLIN :
		    (RN(it, 2) == 0 ? POLLIN : POLLIN | POLLOUT);
		if (multi)
			sqe->len = IORING_POLL_ADD_MULTI;
	} else if (a < 56 && it->nsend < MAX_SENDS) {
		len = 1 + RN(it, 16384);
		if ((i = op_alloc(it, K_SEND, len)) < 0)
			return;
		sqe = ring_sqe(r, IORING_OP_SEND, fd, op_ud(it, i));
		sqe->addr = (uint64_t)(uintptr_t)it->zcbuf;
		sqe->len = len;
		sqe->msg_flags = MSG_NOSIGNAL;
		if (RN(it, 10) == 0)
			sqe->flags |= IOSQE_ASYNC;
	} else if (a < 64 && it->nsend < MAX_SENDS) {
		/* Page-crossing zero-copy sends on the private pair. */
		size_t off = RN(it, ZC_LEN / 2);

		len = 1 + RN(it, (uint32_t)(ZC_LEN - off - 1));
		if ((i = op_alloc(it, K_SEND_ZC, len)) < 0)
			return;
		sqe = ring_sqe(r, IORING_OP_SEND_ZC, it->priv[0],
		    op_ud(it, i));
		sqe->addr = (uint64_t)(uintptr_t)(it->zcbuf + off);
		sqe->len = len;
		sqe->msg_flags = MSG_NOSIGNAL;
		if (it->fixed_buf && RN(it, 2) == 0) {
			sqe->ioprio = IORING_RECVSEND_FIXED_BUF;
			sqe->buf_index = 0;
		}
	} else if (a < 76) {
		/* Cancel one outstanding request, or all on a socket. */
		int victim = -1;

		for (int tries = 0; tries < 8 && victim < 0; tries++) {
			int v = (int)RN(it, MAXOPS);

			if (it->ops[v].used && it->ops[v].kind != K_CANARY &&
			    it->ops[v].kind != K_CANCEL)
				victim = v;
		}
		if ((i = op_alloc(it, K_CANCEL, 0)) < 0)
			return;
		if (victim >= 0 && RN(it, 4) != 0) {
			sqe = ring_sqe(r, IORING_OP_ASYNC_CANCEL, -1,
			    op_ud(it, i));
			sqe->addr = op_ud(it, victim);
		} else {
			sqe = ring_sqe(r, IORING_OP_ASYNC_CANCEL, fd,
			    op_ud(it, i));
			sqe->cancel_flags = IORING_ASYNC_CANCEL_FD |
			    IORING_ASYNC_CANCEL_ALL;
		}
	} else if (a < 82) {
		if ((i = op_alloc(it, K_ACCEPT, 0)) < 0)
			return;
		(void)ring_sqe(r, IORING_OP_ACCEPT, fd, op_ud(it, i));
	} else if (a < 90) {
		if ((i = op_alloc(it, K_TIMEOUT, 0)) < 0)
			return;
		it->ts[i].tv_sec = 0;
		it->ts[i].tv_nsec = (long long)RN(it, 2000000);
		sqe = ring_sqe(r, IORING_OP_TIMEOUT, -1, op_ud(it, i));
		sqe->addr = (uint64_t)(uintptr_t)&it->ts[i];
		sqe->len = 1;
	} else {
		/* Enough NOPs to overflow a small CQ. */
		unsigned n = 1 + RN(it, 32);

		for (unsigned k = 0; k < n && ring_sq_space(r) > 0; k++) {
			if ((i = op_alloc(it, K_NOP, 0)) < 0)
				break;
			(void)ring_sqe(r, IORING_OP_NOP, -1, op_ud(it, i));
		}
	}
}

/*
 * Wait for completions.  On a DEFER_TASKRUN ring half the time only enter
 * when IORING_SQ_TASKRUN (or the CQ) says there is something, as liburing's
 * peek does: a lost flag then shows as a canary that never completes.
 */
static void
wait_some(struct issuer *it)
{
	struct ring *r = &it->r;

	if (r->defer && RN(it, 2) == 0) {
		bool queued = r->sq_tail != atomic_load_explicit(r->sq_ktail,
		    memory_order_relaxed);

		if (queued || (atomic_load_explicit(r->sq_kflags,
		    memory_order_acquire) & (IORING_SQ_TASKRUN |
		    IORING_SQ_CQ_OVERFLOW)) != 0)
			enter(it, 0, 0);
		else if (atomic_load_explicit(r->cq_ktail,
		    memory_order_acquire) == atomic_load_explicit(r->cq_khead,
		    memory_order_relaxed))
			usleep(50);
	} else {
		enter(it, 1, RN(it, 3000000));
	}
	(void)reap(it);
}

/* Cancel everything and wait: whatever is left after 10 s is a hang. */
static void
drain_all(struct issuer *it)
{
	uint64_t deadline = now_ns() + DRAIN_NS, recancel = 0;

	atomic_fetch_add(&st_drains, 1);
	while (it->nout > 0) {
		uint64_t now = now_ns();

		if (now > deadline) {
			char kinds[512];
			size_t off = 0;

			kinds[0] = '\0';
			for (int i = 0; i < MAXOPS && off < sizeof(kinds) - 32;
			    i++)
				if (it->ops[i].used)
					off += (size_t)snprintf(kinds + off,
					    sizeof(kinds) - off, " %s%s",
					    kname[it->ops[i].kind],
					    it->ops[i].kind == K_SEND_ZC &&
					    it->ops[i].got_res ? "(notif)" : "");
			fail(true, "issuer %d: %u requests still outstanding "
			    "10 s after cancel-all (ring %s):%s", it->id,
			    it->nout, it->r.defer ? "DEFER_TASKRUN" : "plain",
			    kinds);
		}
		if (now >= recancel && ring_sq_space(&it->r) > 0) {
			int i = op_alloc(it, K_CANCEL, 0);

			if (i >= 0) {
				struct io_uring_sqe *sqe = ring_sqe(&it->r,
				    IORING_OP_ASYNC_CANCEL, -1, op_ud(it, i));

				sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY |
				    IORING_ASYNC_CANCEL_ALL;
			}
			recancel = now + 500000000ULL;
		}
		/* Zero-copy NOTIFs wait for the receiver. */
		drain_fd(it->priv[1]);
		enter(it, 1, 10000000);
		(void)reap(it);
	}
}

/*
 * fork(2) with a user-backed provided-buffer ring registered.  The child
 * scribbles over the ring's page; the parent's copy must not change, and
 * the parent's ring must keep working (the kernel's COW fallback).
 */
static void
fork_test(struct issuer *it)
{
	uint16_t tail = it->br_tail;
	pid_t pid;
	int st;

	if (!it->has_br || !it->br_user)
		return;
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		memset(it->br_user_mem, 0xff, it->br_len);
		_exit(0);
	}
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (atomic_load((_Atomic uint16_t *)&it->br->tail) != tail ||
	    it->br->bufs[0].len == 0xffffffffU)
		fail(false, "issuer %d: the child's writes reached the "
		    "parent's PBUF_RING page (shared across fork)", it->id);
	atomic_fetch_add(&st_forks, 1);
}

static void *
issuer_main(void *arg)
{
	struct issuer *it = arg;

	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		unsigned rounds = 1 + RN(it, 12);

		issuer_ring_open(it);
		for (unsigned k = 0; k < rounds &&
		    !atomic_load_explicit(&stop, memory_order_relaxed); k++) {
			bool quiet = RN(it, 4) == 0;

			for (int step = 0; step < 100; step++) {
				arm_canary(it);
				if (!quiet) {
					unsigned n = RN(it, 8);

					for (unsigned m = 0; m < n; m++)
						issue_random(it);
				} else if (it->r.defer && it->canary_hits >=
				    20 && step > 20) {
					break;
				}
				wait_some(it);
				drain_fd(it->priv[1]);
				check_canary(it);
			}
			if (RN(it, 4) == 0)
				drain_all(it);
			if (RN(it, 40) == 0)
				fork_test(it);
		}
		/* Teardown with requests armed, or after a clean drain. */
		if (RN(it, 3) == 0) {
			atomic_fetch_add(&st_abrupt, 1);
		} else {
			drain_all(it);
		}
		issuer_ring_close(it);
		it->canary_hits = 0;
	}
	atomic_fetch_sub(&nrunning, 1);
	return (NULL);
}

/* Feeds every issuer's canary socket about once a millisecond. */
static void *
poker_main(void *arg __unused)
{
	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		for (int i = 0; i < nissuers; i++)
			(void)send(issuers[i].canary[1], "c", 1, MSG_DONTWAIT);
		usleep(1000);
	}
	atomic_fetch_sub(&nrunning, 1);
	return (NULL);
}

static void
issuer_init(struct issuer *it, int id, uint32_t seed)
{
	long page = sysconf(_SC_PAGESIZE);

	memset(it, 0, sizeof(*it));
	it->id = id;
	it->seed = seed ? seed : 1;
	it->rbuf = malloc((size_t)MAXOPS * RBUF);
	it->bufs = malloc((size_t)NBUF * BUFSZ);
	it->zcbuf = mmap(NULL, ZC_LEN, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	it->br_len = (size_t)page;
	it->br_user_mem = mmap(NULL, it->br_len, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (it->rbuf == NULL || it->bufs == NULL || it->zcbuf == MAP_FAILED ||
	    it->br_user_mem == MAP_FAILED)
		fail(false, "out of memory");
	for (size_t i = 0; i < ZC_LEN; i++)
		it->zcbuf[i] = (char)i;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, it->priv) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, it->canary) != 0)
		fail(false, "socketpair: %s", strerror(errno));
}

static void
issuer_fini(struct issuer *it)
{
	close(it->priv[0]);
	close(it->priv[1]);
	close(it->canary[0]);
	close(it->canary[1]);
	free(it->rbuf);
	free(it->bufs);
	munmap(it->zcbuf, ZC_LEN);
	munmap(it->br_user_mem, it->br_len);
}

/* ------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------
 */

static void
print_stats(uint64_t t0)
{
	uint64_t total = 0;

	for (int k = 0; k < K_N; k++)
		total += atomic_load(&st_cqe[k]);
	printf("[%4llus] cqes %ju rings %ju (abrupt %ju) drains %ju "
	    "forks %ju recycles %ju listens %ju tcp-retries %ju\n",
	    (unsigned long long)((now_ns() - t0) / 1000000000ULL),
	    (uintmax_t)total, (uintmax_t)atomic_load(&st_rings),
	    (uintmax_t)atomic_load(&st_abrupt),
	    (uintmax_t)atomic_load(&st_drains),
	    (uintmax_t)atomic_load(&st_forks),
	    (uintmax_t)atomic_load(&st_recycle),
	    (uintmax_t)atomic_load(&st_listen),
	    (uintmax_t)atomic_load(&st_tcp_retry));
	fflush(stdout);
}

static void
warm_up(void)
{
	struct ring r;

	/* Anything the module sets up once, so it is in the baseline. */
	ring_open(&r, 0, 8, 0);
	ring_sqe(&r, IORING_OP_NOP, -1, 1);
	ring_enter(&r, 1, 100000000, true);
	ring_close(&r);
}

int
main(int argc, char **argv)
{
	pthread_t chaos[NCHAOS], poker;
	unsigned secs = 60;
	uint32_t seed = (uint32_t)time(NULL);
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	long socks0, wired0, mem0;
	uint64_t t0, last;
	int ch, rc;

	nissuers = (int)sysconf(_SC_NPROCESSORS_ONLN);
	while ((ch = getopt(argc, argv, "t:j:s:")) != -1) {
		switch (ch) {
		case 't':
			secs = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 'j':
			nissuers = atoi(optarg);
			break;
		case 's':
			seed = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr, "usage: kqring_stress [-t seconds] "
			    "[-j issuers] [-s seed]\n");
			return (2);
		}
	}
	if (nissuers < 1)
		nissuers = 1;
	signal(SIGPIPE, SIG_IGN);
	printf("kqring_stress: %u s, %d issuers, %d chaos threads, seed %#x\n",
	    secs, nissuers, NCHAOS, seed);

	warm_up();
	socks0 = open_sockets();
	wired0 = sysctl_num("vm.stats.vm.v_user_wire_count");
	mem0 = kqring_malloc_inuse();

	tcp_lfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (tcp_lfd < 0 || bind(tcp_lfd, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0 || listen(tcp_lfd, 64) != 0 ||
	    getsockname(tcp_lfd, (struct sockaddr *)&tcp_addr, &slen) != 0)
		fail(false, "tcp listener: %s", strerror(errno));
	for (int i = 0; i < NSLOT; i++) {
		pthread_mutex_init(&slots[i].mtx, NULL);
		slots[i].fd[0] = slots[i].fd[1] = -1;
		slot_recycle(&slots[i], i % SK_N);
	}

	issuers = calloc((size_t)nissuers, sizeof(*issuers));
	if (issuers == NULL)
		fail(false, "out of memory");
	for (int i = 0; i < nissuers; i++)
		issuer_init(&issuers[i], i, seed * 2654435761U + (uint32_t)i);
	atomic_store(&nrunning, nissuers + NCHAOS + 1);
	for (int i = 0; i < nissuers; i++)
		if (pthread_create(&issuers[i].td, NULL, issuer_main,
		    &issuers[i]) != 0)
			fail(false, "pthread_create");
	for (int i = 0; i < NCHAOS; i++)
		if (pthread_create(&chaos[i], NULL, chaos_main,
		    (void *)(uintptr_t)(seed ^ (0x9e3779b9U * (unsigned)(i + 1)))) != 0)
			fail(false, "pthread_create");
	if (pthread_create(&poker, NULL, poker_main, NULL) != 0)
		fail(false, "pthread_create");

	t0 = last = now_ns();
	while (now_ns() - t0 < (uint64_t)secs * 1000000000ULL) {
		uint64_t now;

		sleep(1);
		now = now_ns();
		for (int i = 0; i < nissuers; i++) {
			uint64_t since = atomic_load(&issuers[i].in_call);

			/* An issuer may stamp in_call after now was read. */
			if (since != 0 && now > since && now - since > STUCK_NS)
				fail(true, "issuer %d stuck in io_uring_enter "
				    "or close for %llu s", i, (unsigned long long)
				    ((now - since) / 1000000000ULL));
		}
		if (now - last >= 10 * 1000000000ULL) {
			print_stats(t0);
			last = now;
		}
	}
	atomic_store(&stop, true);
	/* Issuers finish their round and close their ring: watch that too. */
	for (uint64_t t1 = now_ns(); atomic_load(&nrunning) > 0;) {
		uint64_t now = now_ns();

		if (now - t1 > 60 * 1000000000ULL)
			fail(true, "%d threads still running 60 s after the "
			    "end of the run", atomic_load(&nrunning));
		for (int i = 0; i < nissuers; i++) {
			uint64_t since = atomic_load(&issuers[i].in_call);

			/* An issuer may stamp in_call after now was read. */
			if (since != 0 && now > since && now - since > STUCK_NS)
				fail(true, "issuer %d stuck in io_uring_enter "
				    "or close for %llu s", i, (unsigned long long)
				    ((now - since) / 1000000000ULL));
		}
		usleep(100000);
	}
	for (int i = 0; i < nissuers; i++)
		pthread_join(issuers[i].td, NULL);
	for (int i = 0; i < NCHAOS; i++)
		pthread_join(chaos[i], NULL);
	pthread_join(poker, NULL);
	print_stats(t0);

	for (int k = 0; k < K_N; k++)
		printf("  %-16s %ju\n", kname[k], (uintmax_t)atomic_load(&st_cqe[k]));
	for (int i = 0; i < nissuers; i++)
		issuer_fini(&issuers[i]);
	for (int i = 0; i < NSLOT; i++) {
		close(slots[i].fd[0]);
		close(slots[i].fd[1]);
	}
	close(tcp_lfd);
	rc = check_leaks(socks0, wired0, mem0);
	printf(rc == 0 ? "[OK] kqring_stress\n" : "[FAIL] kqring_stress\n");
	return (rc);
}
