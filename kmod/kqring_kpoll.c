/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

/*
 * POLL_ADD on files other than sockets (pipes, FIFOs, eventfds, ttys,
 * kqueues, devices with a kqfilter, the ring descriptor itself).
 *
 * Each such request is a knote (EVFILT_READ and/or EVFILT_WRITE on its
 * descriptor) on a kqueue the ring owns but that has no descriptor of its
 * own, and one kthread per ring waits in kevent(2) on it.  As io_uring's
 * poll wait queue entries do, a knote catches every wakeup of the file
 * (counted as it happens, so a write and then a read before the poller
 * looks still counts: see kqring_kp_filt_event()), any number of requests
 * cost no threads, and a multishot request gets a CQE per wakeup.  The
 * poller only records events and queues the request as taskwork;
 * kqring_kpoll_deliver() posts the CQEs.
 *
 * A request on a fixed file (no descriptor to register), one whose file
 * has no kqfilter, or a second one on the same descriptor and filter (a
 * kqueue holds one knote per descriptor and filter) waits on a worker
 * instead (kqring_wq_poll_wait()).
 */

#include "kqring_var.h"

#include <sys/eventvar.h>
#include <sys/syscallsubr.h>

#define	KQRING_KP_NEVENTS	32

/* Rings with a poller (kp_running), so that most enters skip the lookup. */
static u_int kqring_kp_nrings;
/* How often the poller looks at ctx->closing while nothing happens. */
#define	KQRING_KP_IDLE_NS	(50 * 1000 * 1000)

struct kqring_kp_io {
	struct kevent	*kev;
	int		n;		/* in: to copy in; out: copied out */
	int		max;
};

static int
kqring_kp_copyin(void *arg, struct kevent *kevp, int count)
{
	struct kqring_kp_io *io = arg;

	memcpy(kevp, io->kev, count * sizeof(*kevp));
	io->kev += count;
	return (0);
}

static int
kqring_kp_copyout(void *arg, struct kevent *kevp, int count)
{
	struct kqring_kp_io *io = arg;

	if (count > io->max - io->n)
		count = io->max - io->n;
	memcpy(io->kev + io->n, kevp, count * sizeof(*kevp));
	io->n += count;
	return (0);
}

#define	KQRING_KP_BUCKET(ctx, fd)		(&(ctx)->kp_ops[(u_int)(fd) % KQRING_KP_HASH])

/*
 * Counting wakeups.  kqueue_scan() asks the filter again before it reports
 * a knote and drops the event if the file is no longer ready, so a pipe
 * written and drained again before the poller looks would post nothing,
 * where io_uring posts a CQE for each wakeup.  Each multishot request's
 * knotes therefore get a copy of their filterops whose f_event counts
 * wakeups as they happen, in the knote's kevent ext[2] and ext[3], which
 * filters leave alone:
 *
 *   ext[2]  wakeups so far (the poller takes the difference, kp_seen[])
 *   ext[3]  low half: the filter's kn_data when last asked, more if at
 *           EOF, or ~0 if the file wasn't ready; high half: the low half of ext[2] at the
 *           last scan
 *
 * A wakeup is f_event called from knote(), which holds the kq lock;
 * kqueue_scan() and kqueue_register() call it without.  All three hold
 * the knlist lock, which guards kn_kevent.  A pipe's knotes all hang on
 * one list and every read or write asks them all, so only a wakeup that
 * makes the file ready, or readier (more to read, more room to write),
 * counts and activates the knote, as io_uring's poll sees only the
 * wakeups meant for it.  A scan reports the knote while the file is
 * ready or wakeups came since the last one.
 */
static struct file *kqring_kp_getfp(struct kqring_ctx *ctx);

struct kqring_kp_fops {
	struct filterops	fo;
	const struct filterops	*orig;
};

/* One per filterops seen (pipe read and write, eventfd, tty, ...). */
static struct kqring_kp_fops kqring_kp_fops[16];
/* The end-of-input watches' (kqring_kpoll_soeof_watch()): sockets only. */
static struct kqring_kp_fops kqring_kp_eof_fops[4];
static struct mtx kqring_kp_fops_mtx;
MTX_SYSINIT(kqring_kp_fops, &kqring_kp_fops_mtx, "kqring kpoll fops",
    MTX_DEF);

#define	KQRING_KP_NOTREADY	UINT32_MAX
#define	KQRING_KP_EOF		((uint32_t)INT32_MAX + 1)	/* above any kn_data */

static int
kqring_kp_filt_event(struct knote *kn, long hint)
{
	const struct kqring_kp_fops *w = __containerof(kn->kn_fop,
	    struct kqring_kp_fops, fo);
	uint64_t *ext = kn->kn_kevent.ext;
	uint32_t last, prev;
	bool pending;
	int ready;

	ready = w->orig->f_event(kn, hint);
	if (!ready)
		last = KQRING_KP_NOTREADY;
	else if ((kn->kn_flags & EV_EOF) != 0)
		last = KQRING_KP_EOF;
	else
		last = (uint32_t)MIN(MAX(kn->kn_data, 0), INT32_MAX);
	prev = (uint32_t)ext[3];
	if (mtx_owned(&kn->kn_kq->kq_lock)) {
		struct kqring_ctx *ctx = (struct kqring_ctx *)(uintptr_t)ext[1];

		ext[3] = (ext[3] & ~(uint64_t)UINT32_MAX) | last;
		if (!ready || (prev != KQRING_KP_NOTREADY && last <= prev))
			return (0);
		ext[2]++;
		/*
		 * A wakeup from anywhere (another process's write(2), say):
		 * wake an enter waiting on the ring, which then takes the
		 * event itself, rather than wait for the poller to take it and
		 * wake the enter in turn.  Only a flag and wakeup(): the file's
		 * lock is held, which ctx->mtx must not nest inside.  A wakeup
		 * missed as the enter goes to sleep leaves it to the poller.
		 */
		if (ctx != NULL) {
			atomic_store_rel_int(&ctx->kp_kick, 1);
			if (atomic_load_int(&ctx->cq_sleepers) != 0)
				wakeup(&ctx->wakeup_chan);
		}
		return (1);
	}
	pending = (uint32_t)ext[2] != (uint32_t)(ext[3] >> 32);
	ext[3] = (uint64_t)(uint32_t)ext[2] << 32 | last;
	return (ready || pending);
}

/*
 * An end-of-input watch: report only end of input or an error, never
 * data, which the socket's upcall reports.  From knote() (kq lock held),
 * wake an enter waiting on the ring, as kqring_kp_filt_event() does.
 */
static int
kqring_kp_eof_event(struct knote *kn, long hint)
{
	const struct kqring_kp_fops *w = __containerof(kn->kn_fop,
	    struct kqring_kp_fops, fo);
	struct kqring_ctx *ctx;

	if (!w->orig->f_event(kn, hint) ||
	    ((kn->kn_flags & EV_EOF) == 0 && kn->kn_data > 0))
		return (0);
	if (mtx_owned(&kn->kn_kq->kq_lock)) {
		ctx = (struct kqring_ctx *)(uintptr_t)kn->kn_kevent.ext[1];
		if (ctx != NULL) {
			atomic_store_rel_int(&ctx->kp_kick, 1);
			if (atomic_load_int(&ctx->cq_sleepers) != 0)
				wakeup(&ctx->wakeup_chan);
		}
	}
	return (1);
}

static const struct filterops *
kqring_kp_fops_get(const struct filterops *orig, bool eof)
{
	struct kqring_kp_fops *tab = eof ? kqring_kp_eof_fops : kqring_kp_fops;
	u_int ntab = eof ? nitems(kqring_kp_eof_fops) : nitems(kqring_kp_fops);
	struct kqring_kp_fops *w = NULL;

	mtx_lock(&kqring_kp_fops_mtx);
	for (u_int i = 0; i < ntab; i++) {
		if (tab[i].orig == orig) {
			w = &tab[i];
			break;
		}
		if (tab[i].orig == NULL) {
			w = &tab[i];
			w->fo = *orig;
			w->fo.f_event = eof ? kqring_kp_eof_event :
			    kqring_kp_filt_event;
			w->orig = orig;
			break;
		}
	}
	mtx_unlock(&kqring_kp_fops_mtx);
	return (w != NULL ? &w->fo : NULL);
}

static bool
kqring_kp_fops_eof(const struct filterops *fo)
{
	return (fo >= &kqring_kp_eof_fops[0].fo &&
	    fo <= &kqring_kp_eof_fops[nitems(kqring_kp_eof_fops) - 1].fo);
}

static bool
kqring_kp_fops_ours(const struct filterops *fo)
{
	return ((fo >= &kqring_kp_fops[0].fo &&
	    fo <= &kqring_kp_fops[nitems(kqring_kp_fops) - 1].fo) ||
	    kqring_kp_fops_eof(fo));
}

/* The knote kev registered on kq, under the kq lock. */
static struct knote *
kqring_kp_knote(struct kqueue *kq, const struct kevent *kev)
{
	struct knote *kn;

	mtx_assert(&kq->kq_lock, MA_OWNED);
	if (kev->ident >= (uintptr_t)kq->kq_knlistsize)
		return (NULL);
	SLIST_FOREACH(kn, &kq->kq_knlist[kev->ident], kn_link) {
		if (kn->kn_filter == kev->filter &&
		    kn->kn_kevent.udata == kev->udata)
			return (kn);
	}
	return (NULL);
}

/*
 * Give the knotes just registered for kev[0..n) the counting filterops.
 * Every caller of f_event or f_detach holds the kq lock or has the knote
 * in flux (entered under the kq lock), so the swap is made under the kq
 * lock on a knote not in flux.  If that can't be had, the knote keeps its
 * filterops and only misses wakeups drained before the poller looks.
 */
static void
kqring_kp_wrap(struct kqring_ctx *ctx, const struct kevent *kev, int n,
    bool eof)
{
	const struct filterops *orig, *fo;
	struct kqueue *kq;
	struct knote *kn;
	struct file *fp;

	if ((fp = kqring_kp_getfp(ctx)) == NULL)
		return;
	kq = fp->f_data;
	for (int i = 0; i < n; i++) {
		mtx_lock(&kq->kq_lock);
		kn = kqring_kp_knote(kq, &kev[i]);
		orig = kn != NULL ? kn->kn_fop : NULL;
		mtx_unlock(&kq->kq_lock);
		if (orig == NULL || kqring_kp_fops_ours(orig) ||
		    orig->f_event == NULL)
			continue;
		if ((fo = kqring_kp_fops_get(orig, eof)) == NULL)
			continue;
		for (int tries = 0; tries < 100; tries++) {
			mtx_lock(&kq->kq_lock);
			kn = kqring_kp_knote(kq, &kev[i]);
			if (kn == NULL || kn->kn_fop != orig) {
				mtx_unlock(&kq->kq_lock);
				break;
			}
			if (kn->kn_influx == 0) {
				/*
				 * ext[1] names the ring for the wakeup; the
				 * ring closes this kqueue (and so its knotes)
				 * before it is freed.
				 */
				kn->kn_kevent.ext[1] = (uint64_t)(uintptr_t)ctx;
				kn->kn_kevent.ext[2] = 0;
				kn->kn_kevent.ext[3] = KQRING_KP_NOTREADY;
				kn->kn_fop = fo;
				mtx_unlock(&kq->kq_lock);
				break;
			}
			mtx_unlock(&kq->kq_lock);
			kern_yield(PRI_USER);
		}
	}
	fdrop(fp, curthread);
}

/* A reference to the ring's kqueue, or NULL once it is closed. */
static struct file *
kqring_kp_getfp(struct kqring_ctx *ctx)
{
	struct file *fp;

	mtx_lock(&ctx->mtx);
	fp = ctx->kp_fp;
	if (fp != NULL && !fhold(fp))
		fp = NULL;
	mtx_unlock(&ctx->mtx);
	return (fp);
}

/*
 * Register (or delete) knotes on the ring's kqueue as curthread.  kevent
 * reports through td_retval, which belongs to the system call curthread
 * is in, so keep it.
 */
static int
kqring_kp_change(struct kqring_ctx *ctx, struct kevent *kev, int n)
{
	struct thread *td = curthread;
	struct kqring_kp_io io = { .kev = kev };
	struct kevent_copyops ops = {
		.arg = &io,
		.k_copyin = kqring_kp_copyin,
		.k_copyout = kqring_kp_copyout,
		.kevent_size = sizeof(struct kevent),
	};
	register_t saved[2] = { td->td_retval[0], td->td_retval[1] };
	struct file *fp;
	int error;

	if ((fp = kqring_kp_getfp(ctx)) == NULL)
		return (EBADF);
	error = kern_kevent_fp(td, fp, n, 0, &ops, NULL);
	td->td_retval[0] = saved[0];
	td->td_retval[1] = saved[1];
	fdrop(fp, td);
	return (error);
}

/* The poll mask an event reports, as the file's poll would. */
static int
kqring_kp_revents(const struct kevent *kev)
{
	int rev;

	rev = kev->filter == EVFILT_READ ? POLLIN | POLLRDNORM :
	    POLLOUT | POLLWRNORM;
	if ((kev->flags & EV_EOF) != 0)
		rev |= kev->filter == EVFILT_READ ? POLLHUP | POLLRDHUP : POLLHUP;
	if ((kev->flags & EV_ERROR) != 0 || kev->fflags != 0)
		rev |= POLLERR;
	return (rev);
}

/* Record an event for the request it belongs to, if it is still there. */
static void
kqring_kp_event(struct kqring_ctx *ctx, const struct kevent *kev)
{
	uint64_t id = (uint64_t)(uintptr_t)kev->udata;
	struct kqring_op *op;

	if (kev->udata == KQRING_KP_SOEOF) {
		kqring_so_eof_kick(curthread, (int)kev->ident);
		return;
	}
	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, KQRING_KP_BUCKET(ctx, kev->ident), kp_link) {
		if (op->kp_id == id)
			break;
	}
	if (op != NULL && op->kp_on && !op->cancelled && op->kp_kev) {
		/* Events merge until delivered; signals add up. */
		op->kp_kfflags |= kev->fflags;
		if (kev->filter == EVFILT_SIGNAL)
			op->kp_kdata += kev->data;
		else
			op->kp_kdata = kev->data;
		if ((kev->flags & EV_EOF) != 0)
			op->kp_keof = true;
		op->kp_nev++;
		kqring_taskwork_add_locked(ctx, op);
	} else if (op != NULL && op->kp_on && !op->cancelled) {
		/*
		 * The wakeups kqring_kp_filt_event() counted since the last
		 * event, or one.  The poller and kqring_kpoll_run() both take
		 * events and record them here, so an event can arrive after a
		 * later one: its count is older than kp_seen, and the later
		 * event has counted its wakeups already.
		 */
		int f = kev->filter == EVFILT_READ ? 0 : 1;
		int32_t nev = (int32_t)((uint32_t)kev->ext[2] - op->kp_seen[f]);

		if (nev >= 0) {
			if (nev > 0)
				op->kp_seen[f] = (uint32_t)kev->ext[2];
			op->kp_revents |= kqring_kp_revents(kev);
			op->kp_nev += nev != 0 ? MIN(nev, 1024) : 1;
			kqring_taskwork_add_locked(ctx, op);
		}
	}
	mtx_unlock(&ctx->mtx);
}

static void
kqring_kp_thread(void *arg)
{
	struct kqring_ctx *ctx = arg;
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct kevent kev[KQRING_KP_NEVENTS];
	struct timespec idle = { 0, KQRING_KP_IDLE_NS };

	PROC_LOCK(p);
	SIGFILLSET(td->td_sigmask);
	SIG_CANTMASK(td->td_sigmask);
	PROC_UNLOCK(p);

	for (;;) {
		struct kqring_kp_io io = { .kev = kev, .max = nitems(kev) };
		struct kevent_copyops ops = {
			.arg = &io,
			.k_copyin = kqring_kp_copyin,
			.k_copyout = kqring_kp_copyout,
			.kevent_size = sizeof(struct kevent),
		};
		struct file *fp;

		if (kqring_kthread_check_suspend(ctx))
			break;
		if ((fp = kqring_kp_getfp(ctx)) == NULL)
			break;
		/*
		 * Sleeps interruptibly: stop and exit requests end it, and
		 * the loop then suspends or exits above.
		 */
		(void)kern_kevent_fp(td, fp, 0, nitems(kev), &ops, &idle);
		fdrop(fp, td);
		for (int i = 0; i < io.n; i++)
			kqring_kp_event(ctx, &kev[i]);
	}

	/*
	 * The process is exiting or exec'ing, not the ring closing.  The
	 * kqueue is on the process's descriptor table, which exit frees once
	 * this thread is gone and which the ring can outlive (its descriptor
	 * passed to another process), so close it now.  Requests still armed
	 * on it never fire; they complete when cancelled or the ring closes.
	 */
	if (!ctx->closing)
		kqring_kpoll_close(ctx);

	mtx_lock(&ctx->mtx);
	ctx->kp_running = false;
	ctx->kp_td = NULL;
	atomic_subtract_int(&kqring_kp_nrings, 1);
	wakeup(&ctx->kp_running);
	mtx_unlock(&ctx->mtx);
	kqring_ctx_rele(ctx);
	kqring_kthread_exit();
}

/*
 * The ring's kqueue and poller, made on first use by a thread of the
 * owner process.  kern_kqueue() installs a descriptor; it is closed again
 * at once (a process closing descriptors it did not open could race this,
 * as with any descriptor).  The kqueue, on the process's descriptor
 * table, is closed in ring teardown, which runs before the table is freed
 * at exit and exec.
 */
static int
kqring_kp_start(struct kqring_ctx *ctx)
{
	struct thread *td = curthread;
	register_t saved[2] = { td->td_retval[0], td->td_retval[1] };
	struct file *fp;
	int error, fd;
	bool need;

	if (ctx->kp_running)
		return (0);
	if (ctx->owner_proc != td->td_proc)
		return (EOPNOTSUPP);
	mtx_lock(&ctx->mtx);
	need = ctx->kp_fp == NULL;
	mtx_unlock(&ctx->mtx);
	if (need) {
		error = kern_kqueue(td, O_CLOEXEC, false, NULL);
		fd = (int)td->td_retval[0];
		td->td_retval[0] = saved[0];
		td->td_retval[1] = saved[1];
		if (error != 0)
			return (error);
		error = fget(td, fd, &cap_no_rights, &fp);
		if (error == 0 && fp->f_type != DTYPE_KQUEUE) {
			fdrop(fp, td);
			error = EBADF;
		}
		(void)kern_close(td, fd);
		td->td_retval[0] = saved[0];
		td->td_retval[1] = saved[1];
		if (error != 0)
			return (error);
		mtx_lock(&ctx->mtx);
		if (ctx->kp_fp == NULL && !ctx->closing) {
			ctx->kp_fp = fp;
			fp = NULL;
		}
		mtx_unlock(&ctx->mtx);
		if (fp != NULL)
			fdrop(fp, td);
	}

	mtx_lock(&ctx->mtx);
	if (ctx->kp_running || ctx->closing) {
		error = ctx->closing ? EBADF : 0;
		mtx_unlock(&ctx->mtx);
		return (error);
	}
	ctx->kp_running = true;
	atomic_add_int(&kqring_kp_nrings, 1);
	refcount_acquire(&ctx->refs);
	mtx_unlock(&ctx->mtx);
	kqring_worker_hold();
	error = kthread_add(kqring_kp_thread, ctx, td->td_proc, &ctx->kp_td,
	    0, 0, "kqpoll");
	if (error != 0) {
		kqring_worker_rele();
		mtx_lock(&ctx->mtx);
		ctx->kp_running = false;
		ctx->kp_td = NULL;
		atomic_subtract_int(&kqring_kp_nrings, 1);
		mtx_unlock(&ctx->mtx);
		/* As the poller would on exit: see kqring_kp_thread(). */
		kqring_kpoll_close(ctx);
		kqring_ctx_rele(ctx);
	}
	return (error);
}

/*
 * End of input on AF_UNIX stream sockets.  When the peer shuts down
 * writing, FreeBSD 15 (uipc_cantrcvmore()) wakes select, poll and kqueue
 * but runs no socket upcall, so a request waiting on kqring's upcall
 * would never hear of it.  A socket a request waits to read from through
 * a descriptor therefore gets a knote on the ring's kqueue, once per
 * descriptor until it is closed, with filterops that report only end of
 * input (kqring_kp_eof_event()).  The poller, or an enter's scan, then
 * runs the upcall (kqring_so_eof_kick()).  Requests on fixed files have
 * no descriptor to watch.  so (watched through fd, generation gen) is
 * only compared, never used: the request may be gone.  No locks held.
 *
 * ctx->soeof[] saves the kqueue lookup on each arm.  It can't tell when
 * fd was closed (dropping the knote) and the same socket put back on the
 * same number (dup2(2)); that socket then goes unwatched until the entry
 * is replaced.
 */
void
kqring_kpoll_soeof_watch(struct kqring_ctx *ctx, int fd,
    const struct socket *so, so_gen_t gen)
{
	struct kqring_soeof *c;
	struct kevent kev;
	struct file *fp;
	struct knote *kn;
	bool have;

	/* The caller found fd missing from ctx->soeof[]. */
	if (fd < 0 || ctx->owner_proc != curproc ||
	    kqring_kp_start(ctx) != 0)
		return;
	c = &ctx->soeof[(u_int)fd % nitems(ctx->soeof)];
	EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, KQRING_KP_SOEOF);
	if ((fp = kqring_kp_getfp(ctx)) == NULL)
		return;
	mtx_lock(&((struct kqueue *)fp->f_data)->kq_lock);
	kn = kqring_kp_knote(fp->f_data, &kev);
	have = kn != NULL && kn->kn_fp != NULL && kn->kn_fp->f_data == so &&
	    kqring_kp_fops_eof(kn->kn_fop);
	mtx_unlock(&((struct kqueue *)fp->f_data)->kq_lock);
	fdrop(fp, curthread);
	if (!have) {
		if (kqring_kp_change(ctx, &kev, 1) != 0)
			return;
		kqring_kp_wrap(ctx, &kev, 1, true);
	}
	mtx_lock(&ctx->mtx);
	c->fd = fd;
	c->so = so;
	c->gen = gen;
	mtx_unlock(&ctx->mtx);
}

static int
kqring_kp_kevents(const struct kqring_op *op, struct kevent *kev,
    u_short flags)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	void *udata = (void *)(uintptr_t)op->kp_id;
	int n = 0;

	if (op->kp_kev) {
		const struct kevent *k = op->args;

		EV_SET(&kev[n++], k->ident, k->filter, flags, k->fflags, 0,
		    udata);
		return (n);
	}
	if ((op->kp_filters & KQRING_KP_READ) != 0)
		EV_SET(&kev[n++], sqe->fd, EVFILT_READ, flags, 0, 0, udata);
	if ((op->kp_filters & KQRING_KP_WRITE) != 0)
		EV_SET(&kev[n++], sqe->fd, EVFILT_WRITE, flags, 0, 0, udata);
	return (n);
}

/*
 * Arm a non-socket POLL_ADD as knotes.  Returns 0 if armed, EOPNOTSUPP if
 * it should wait on a worker instead, or the error to complete it with.
 */
int
kqring_kpoll_arm(struct kqring_ctx *ctx, struct kqring_op *op,
    const struct file *fp)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	uint32_t events = sqe->poll32_events ? sqe->poll32_events :
	    sqe->poll_events;
	bool multi = (sqe->len & IORING_POLL_ADD_MULTI) != 0;
	bool level = (sqe->len & IORING_POLL_ADD_LEVEL) != 0;
	struct kevent kev[2];
	struct kqring_op *o;
	uint8_t filters = 0;
	int error, n;

	if (op->ffile != NULL)
		return (EOPNOTSUPP);
	/* The descriptor may have changed since kqring_kq_check(). */
	if (op->kq_bridge && fp->f_type != DTYPE_KQUEUE)
		return (EINVAL);
	if ((events & (POLLOUT | POLLWRNORM)) != 0)
		filters |= KQRING_KP_WRITE;
	if ((events & ~(uint32_t)(POLLOUT | POLLWRNORM)) != 0 || filters == 0)
		filters |= KQRING_KP_READ;
	error = kqring_kp_start(ctx);
	if (error != 0)
		return (EOPNOTSUPP);

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(o, KQRING_KP_BUCKET(ctx, sqe->fd), kp_link) {
		if (o->kp_fd == sqe->fd && (o->kp_filters & filters) != 0)
			break;
	}
	if (o != NULL) {
		mtx_unlock(&ctx->mtx);
		return (EOPNOTSUPP);
	}
	op->kp_id = ++ctx->kp_next_id;
	op->kp_fd = sqe->fd;
	op->kp_filters = filters;
	op->kp_data = fp->f_data;
	op->kp_revents = 0;
	op->kp_nev = 0;
	op->kp_seen[0] = op->kp_seen[1] = 0;
	op->kp_on = true;
	op->cancelled = false;
	if (op->kq_bridge)
		ctx->kq_bridges++;
	TAILQ_INSERT_TAIL(KQRING_KP_BUCKET(ctx, op->kp_fd), op, kp_link);
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);

	/*
	 * A single-shot poll fires once; a multishot one on each wakeup
	 * (EV_CLEAR) or, LEVEL, while the file stays ready.
	 */
	n = kqring_kp_kevents(op, kev, EV_ADD | (!multi ? EV_ONESHOT : 0) |
	    (!level ? EV_CLEAR : 0));
	error = kqring_kp_change(ctx, kev, n);
	if (error == 0) {
		/*
		 * Not a bridge's: the counting filter drops a wakeup that shows
		 * no more than it last saw, and taking a kqueue's events tells
		 * no filter that the count fell.
		 */
		if (!op->kq_bridge)
			kqring_kp_wrap(ctx, kev, n, false);
		return (0);
	}
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	/* No kqfilter for this file: poll it from a worker instead. */
	if (error == EINVAL || error == ENODEV || error == EOPNOTSUPP) {
		op->cancelled = false;
		return (EOPNOTSUPP);
	}
	return (error);
}

/*
 * Take op off the ring's knotes (from kqring_op_disarm_locked(); drops and
 * retakes ctx->mtx).  The knotes are deleted by descriptor, so only a
 * thread of the owner process can; elsewhere (teardown from another
 * process) they stay until the kqueue is closed, and events for them find
 * no request.
 */
void
kqring_kpoll_disarm_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kevent kev[2];
	int n;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (!op->kp_on)
		return;
	op->kp_on = false;
	if (op->kq_bridge)
		ctx->kq_bridges--;
	TAILQ_REMOVE(KQRING_KP_BUCKET(ctx, op->kp_fd), op, kp_link);
	if (curthread->td_proc != ctx->owner_proc)
		return;
	n = kqring_kp_kevents(op, kev, EV_DELETE);
	mtx_unlock(&ctx->mtx);
	/* One at a time: a ONESHOT knote that fired is gone (ENOENT). */
	for (int i = 0; i < n; i++)
		(void)kqring_kp_change(ctx, &kev[i], 1);
	mtx_lock(&ctx->mtx);
}

/* ------------------------------------------------------------------------
 * KQRING_EXT_KEVENT: a kqueue event as a request (include/sys/kqring.h).
 * The kevent is registered on the ring's kqueue with the request's kp_id
 * as udata, like a non-socket POLL_ADD's knotes, and the poller records
 * what fires (kqring_kp_event()); taskwork posts it.
 * ------------------------------------------------------------------------
 */

#define	KQRING_KEV_VNODE_ALL	(NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | \
	NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_REVOKE | NOTE_OPEN | \
	NOTE_CLOSE | NOTE_CLOSE_WRITE | NOTE_READ)
#define	KQRING_KEV_PROC_ALL	(NOTE_EXIT | NOTE_FORK | NOTE_EXEC)

int
kqring_kev_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kevent *k;
	int error;

	/* A trigger's waiter (op->trig_kev) has its kevent in the kernel. */
	if (sqe->len != sizeof(*k) ||
	    (sqe->addr == 0) != (op->trig_kev != NULL) || sqe->fd != -1 ||
	    sqe->off != 0 || sqe->buf_index != 0 || sqe->file_index != 0 ||
	    (sqe->rw_flags & ~KQRING_KEV_MULTISHOT) != 0 ||
	    (sqe->flags & (IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT)) != 0)
		return (EINVAL);
	k = kqring_args_alloc(op, sizeof(*k), &error);
	if (k == NULL)
		return (error);
	if (op->trig_kev != NULL) {
		*k = *op->trig_kev;
		return (0);
	}
	error = copyin((const void *)(uintptr_t)sqe->addr, k, sizeof(*k));
	if (error != 0)
		return (error);
	if (k->filter == EVFILT_READ || k->filter == EVFILT_WRITE) {
		bool multi = (sqe->rw_flags & KQRING_KEV_MULTISHOT) != 0;

		if (k->ident > INT_MAX || k->fflags != 0 || k->flags != 0 ||
		    k->data != 0 || k->udata != NULL || k->ext[0] != 0 ||
		    k->ext[1] != 0 || k->ext[2] != 0 || k->ext[3] != 0)
			return (EINVAL);
		/*
		 * Readiness, as a POLL_ADD of ident; its result becomes the
		 * count (kqring_kev_count()).  The kevent in args is kept.
		 */
		op->kev_count = k->filter;
		op->sqe.opcode = IORING_OP_POLL_ADD;
		op->sqe.ioprio = 0;
		op->sqe.fd = (int)k->ident;
		op->sqe.addr = 0;
		op->sqe.len = multi ? IORING_POLL_ADD_MULTI : 0;
		op->sqe.poll32_events = k->filter == EVFILT_READ ?
		    POLLIN | POLLRDNORM : POLLOUT | POLLWRNORM;
		return (0);
	}
	return (kqring_kev_check(k));
}

/* The kevents a KQRING_EXT_KEVENT request (or trigger) may wait for. */
int
kqring_kev_check(const struct kevent *k)
{
	if (k->flags != 0 || k->data != 0 || k->udata != NULL ||
	    k->ext[0] != 0 || k->ext[1] != 0 || k->ext[2] != 0 ||
	    k->ext[3] != 0)
		return (EINVAL);
	switch (k->filter) {
	case EVFILT_VNODE:
		if (k->ident > INT_MAX || k->fflags == 0 ||
		    (k->fflags & ~KQRING_KEV_VNODE_ALL) != 0)
			return (EINVAL);
		break;
	case EVFILT_PROC:
		if (k->ident == 0 || k->ident > PID_MAX || k->fflags == 0 ||
		    (k->fflags & ~KQRING_KEV_PROC_ALL) != 0)
			return (EINVAL);
		break;
	case EVFILT_SIGNAL:
		if (k->ident == 0 || k->ident > _SIG_MAXSIG || k->fflags != 0)
			return (EINVAL);
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

/* Issue: register the kevent, or complete with the error. */
void
kqring_kev_issue(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct kevent *k = op->args;
	bool multi = (op->sqe.rw_flags & KQRING_KEV_MULTISHOT) != 0;
	struct kevent kev;
	struct kqring_op *o;
	int error;

	/* The kqueue and its descriptors are the owner process's. */
	if (ctx->owner_proc != curproc) {
		kqring_req_complete(ctx, op, -EOPNOTSUPP, 0);
		return;
	}
	error = kqring_kp_start(ctx);
	if (error != 0) {
		kqring_req_complete(ctx, op, -error, 0);
		return;
	}
	mtx_lock(&ctx->mtx);
	/* A kqueue holds one knote per ident and filter. */
	TAILQ_FOREACH(o, KQRING_KP_BUCKET(ctx, (int)k->ident), kp_link) {
		if (o->kp_kev && o->kp_fd == (int)k->ident &&
		    ((const struct kevent *)o->args)->filter == k->filter)
			break;
	}
	if (o != NULL) {
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, -EBUSY, 0);
		return;
	}
	op->kp_id = ++ctx->kp_next_id;
	op->kp_fd = (int)k->ident;
	op->kp_kev = true;
	op->kp_keof = false;
	op->kp_kfflags = 0;
	op->kp_kdata = 0;
	op->kp_nev = 0;
	op->kp_on = true;
	op->cancelled = false;
	TAILQ_INSERT_TAIL(KQRING_KP_BUCKET(ctx, op->kp_fd), op, kp_link);
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);

	(void)kqring_kp_kevents(op, &kev, EV_ADD | (multi ? EV_CLEAR :
	    EV_ONESHOT));
	error = kqring_kp_change(ctx, &kev, 1);
	if (error == 0)
		return;
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, -error, 0);
}

/* Post what fired for a kevent request (taskwork). */
static void
kqring_kev_deliver(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct kevent *k = op->args;
	bool multi = (op->sqe.rw_flags & KQRING_KEV_MULTISHOT) != 0;
	uint64_t extra[2];
	uint32_t fflags;
	int64_t data;
	int32_t res;
	bool eof;
	u_int nev;

	mtx_lock(&ctx->mtx);
	nev = op->kp_nev;
	fflags = op->kp_kfflags;
	data = op->kp_kdata;
	eof = op->kp_keof;
	op->kp_nev = 0;
	op->kp_kfflags = 0;
	op->kp_kdata = 0;
	mtx_unlock(&ctx->mtx);
	if (nev == 0)
		return;
	if (k->filter == EVFILT_SIGNAL)
		res = (int32_t)MIN(data, INT32_MAX);
	else if (k->filter == EVFILT_PROC)
		res = KQRING_KEV_PROC_NOTE(fflags);
	else
		res = (int32_t)(fflags & INT32_MAX);
	extra[0] = (uint64_t)data;
	extra[1] = fflags;
	/* A CONCURRENT trigger's waiter: the event fires the trigger. */
	if (multi && !eof && op->trig_wait != NULL) {
		kqring_trig_waiter_event(ctx, op, res, true);
		return;
	}
	if (multi && !eof && !kqring_cq_full(ctx)) {
		ctx->cq_seq++;
		kqring_post_cqe_extra(ctx, op->sqe.user_data, res,
		    IORING_CQE_F_MORE, extra);
		return;
	}
	op->cqe_has_extra = true;
	op->cqe_extra[0] = extra[0];
	op->cqe_extra[1] = extra[1];
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, res, 0);
}

/* ------------------------------------------------------------------------
 * KQRING_EXT_KQUEUE: a kqueue's events as CQEs (include/sys/kqring.h).
 * The request is a multishot POLL_ADD of the kqueue, whose knote says that
 * events came; delivery takes them with kevent(2)'s own scan.
 *
 * A kqueue only wakes its pollers when an event is queued, and an event
 * that isn't EV_CLEAR is queued again by each scan that reports it, so new
 * activity on it wakes nothing.  So an enter looks too, as a kevent(2)
 * call would (kqring_kq_rescan()), which also drops such an event once
 * its condition has ended, and the next activity queues it afresh.
 * ------------------------------------------------------------------------
 */

int
kqring_kq_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;

	if (sqe->addr != 0 || sqe->len != 0 || sqe->off != 0 ||
	    sqe->rw_flags != 0 || sqe->buf_index != 0 ||
	    sqe->file_index != 0 ||
	    (sqe->flags & (IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT)) != 0)
		return (EINVAL);
	op->kq_bridge = true;
	op->sqe.opcode = IORING_OP_POLL_ADD;
	op->sqe.ioprio = 0;
	op->sqe.len = IORING_POLL_ADD_MULTI;
	op->sqe.poll32_events = POLLIN | POLLRDNORM;
	return (0);
}

/*
 * Take fp's events as CQEs while the CQ has room.  Returns 0, or what ends
 * the request.  Stops short of overflowing: what isn't taken stays on the
 * kqueue, so no event is lost.
 */
static int32_t
kqring_kq_take(struct kqring_ctx *ctx, struct file *fp)
{
	struct thread *td = curthread;
	struct kevent kev[KQRING_KP_NEVENTS];
	struct kqring_kp_io io;
	struct kevent_copyops ops = {
		.arg = &io,
		.k_copyin = kqring_kp_copyin,
		.k_copyout = kqring_kp_copyout,
		.kevent_size = sizeof(struct kevent),
	};
	struct timespec zero = { 0, 0 };
	const struct kqring_shm_header *shm = ctx->shm;
	register_t saved[2] = { td->td_retval[0], td->td_retval[1] };
	uint32_t used;
	int error;

	for (;;) {
		used = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (kqring_cq_full(ctx) || used >= ctx->cq_entries)
			return (((struct kqueue *)fp->f_data)->kq_count > 0 ?
			    -EOVERFLOW : 0);
		io.kev = kev;
		io.n = 0;
		io.max = (int)MIN(nitems(kev), ctx->cq_entries - used);
		error = kern_kevent_fp(td, fp, 0, io.max, &ops, &zero);
		td->td_retval[0] = saved[0];
		td->td_retval[1] = saved[1];
		if (error != 0)
			return (-error);
		for (int i = 0; i < io.n; i++) {
			const struct kevent *k = &kev[i];
			uint64_t extra[2];

			extra[0] = k->ident;
			extra[1] = (uint64_t)(uint16_t)k->filter |
			    (uint64_t)k->flags << 16 |
			    (uint64_t)k->fflags << 32;
			ctx->cq_seq++;
			kqring_post_cqe_extra(ctx,
			    (uint64_t)(uintptr_t)k->udata,
			    (int32_t)MIN(MAX(k->data, 0), INT32_MAX),
			    IORING_CQE_F_MORE, extra);
		}
		if (io.n < io.max)
			return (0);
	}
}

/*
 * Issue: can op attach what its descriptor names?  Before it is armed,
 * which a multishot POLL_ADD is with no inline attempt, and on a socket
 * would be by upcall.  What is armed takes the events a kqueue has by then:
 * its knote fires as it is added.
 */
int
kqring_kq_check(struct kqring_ctx *ctx, const struct kqring_op *op)
{
	struct thread *td = curthread;
	cap_rights_t rights;
	struct file *fp;
	int error;

	if (ctx->owner_proc != td->td_proc)
		return (EOPNOTSUPP);
	error = fget(td, op->sqe.fd,
	    cap_rights_init_one(&rights, CAP_KQUEUE_EVENT), &fp);
	if (error != 0)
		return (error);
	error = fp->f_type == DTYPE_KQUEUE ? 0 : EINVAL;
	fdrop(fp, td);
	return (error);
}

/* The knote fired or an enter looked (taskwork). */
static void
kqring_kq_deliver(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct thread *td = curthread;
	cap_rights_t rights;
	struct file *fp;
	int32_t res;

	/*
	 * Another process sharing the ring has none of the owner's
	 * descriptors, and the owner's next enter looks anyway.
	 */
	if (td->td_proc != ctx->owner_proc)
		return;
	if (fget(td, op->kp_fd, cap_rights_init_one(&rights, CAP_KQUEUE_EVENT),
	    &fp) != 0)
		res = -EBADF;
	else {
		/* The descriptor may name something else by now. */
		res = fp->f_type != DTYPE_KQUEUE || fp->f_data != op->kp_data ?
		    -EBADF : kqring_kq_take(ctx, fp);
		fdrop(fp, td);
	}
	if (res == 0)
		return;
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, res, 0);
}

/* An enter by the owner: its attached kqueues are looked at, as taskwork. */
void
kqring_kq_rescan(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	if (atomic_load_int(&ctx->kq_bridges) == 0 ||
	    ctx->owner_proc != curproc)
		return;
	mtx_lock(&ctx->mtx);
	for (int i = 0; i < KQRING_KP_HASH; i++) {
		TAILQ_FOREACH(op, &ctx->kp_ops[i], kp_link) {
			if (!op->kq_bridge || !op->kp_on || op->cancelled)
				continue;
			op->kp_revents |= POLLIN;
			op->kp_nev++;
			kqring_taskwork_add_locked(ctx, op);
		}
	}
	mtx_unlock(&ctx->mtx);
}

/*
 * Post what the poller saw for op (run as taskwork): its CQE, or for a
 * multishot request one per event, with F_MORE.  A multishot request
 * whose next CQE would overflow the CQ ends instead, as io_uring stops it.
 */
void
kqring_kpoll_deliver(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	int events = sqe->poll32_events ? (int)sqe->poll32_events :
	    sqe->poll_events;
	bool multi = (sqe->len & IORING_POLL_ADD_MULTI) != 0;
	int rev;
	u_int nev;

	if (op->kp_kev) {
		kqring_kev_deliver(ctx, op);
		return;
	}
	mtx_lock(&ctx->mtx);
	/*
	 * A multishot poll on the ring's own eventfd would be woken by the
	 * signal for its own CQE, for ever.  io_uring ends one woken by its
	 * eventfd signal after that CQE; any wakeup of it ends it here.
	 */
	if (op->kp_data != NULL && op->kp_data == ctx->evfd)
		multi = false;
	rev = op->kp_revents & (events | POLLERR | POLLHUP | POLLNVAL);
	nev = op->kp_nev;
	op->kp_revents = 0;
	op->kp_nev = 0;
	mtx_unlock(&ctx->mtx);
	if (rev == 0)
		return;
	if (op->kq_bridge) {
		kqring_kq_deliver(ctx, op);
		return;
	}
	/* A byte-count kevent request: how much, not which events. */
	if (op->kev_count != 0) {
		struct file *fp;

		if (fget(curthread, op->kp_fd, &cap_no_rights, &fp) == 0) {
			rev = kqring_kev_count(op, fp);
			fdrop(fp, curthread);
		} else
			rev = 0;
	}
	if (multi && op->trig_wait != NULL) {
		/* Events taken together fire a trigger once. */
		kqring_trig_waiter_event(ctx, op, rev, true);
		return;
	}
	if (multi) {
		for (u_int i = 0; i < nev; i++) {
			if (kqring_cq_full(ctx))
				goto last;
			kqring_post_cqe(ctx, sqe->user_data, rev,
			    IORING_CQE_F_MORE);
		}
		return;
	}
last:
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, rev, 0);
}

/*
 * The events waiting on ctx's kqueue, taken now (without waiting) as the
 * poller would take them.
 */
static void
kqring_kp_scan(struct kqring_ctx *ctx, struct thread *td)
{
	struct kevent kev[KQRING_KP_NEVENTS];
	struct kqring_kp_io io = { .kev = kev, .max = nitems(kev) };
	struct kevent_copyops ops = {
		.arg = &io,
		.k_copyin = kqring_kp_copyin,
		.k_copyout = kqring_kp_copyout,
		.kevent_size = sizeof(struct kevent),
	};
	struct timespec zero = { 0, 0 };
	register_t saved[2] = { td->td_retval[0], td->td_retval[1] };
	struct file *fp;

	if ((fp = kqring_kp_getfp(ctx)) == NULL)
		return;
	(void)kern_kevent_fp(td, fp, 0, nitems(kev), &ops, &zero);
	td->td_retval[0] = saved[0];
	td->td_retval[1] = saved[1];
	fdrop(fp, td);
	for (int i = 0; i < io.n; i++)
		kqring_kp_event(ctx, &kev[i]);
}

/*
 * An enter woken by kqring_kp_filt_event(): take the ring's events now, as
 * the poller would, so that their requests are taskwork for this enter.
 * Called with no ring lock held.
 */
void
kqring_kpoll_kicked(struct kqring_ctx *ctx)
{
	if (atomic_load_acq_int(&ctx->kp_kick) == 0 ||
	    atomic_swap_int(&ctx->kp_kick, 0) == 0)
		return;
	kqring_kp_scan(ctx, curthread);
}

/*
 * At the end of an enter that woke pollers of its ring (a CQE made the
 * ring readable, or its eventfd was signalled): take the events of the
 * process's rings now, and on a ring whose completions a kernel thread
 * would otherwise post (no DEFER_TASKRUN) and td may enter, post them, so
 * that a poll on this ring by another of the process's rings completes
 * before the enter returns, as io_uring's poll task_work runs on return
 * to user space.  A DEFER_TASKRUN ring posts them at its owner's next
 * enter, as io_uring does.  CQEs posted here may wake pollers again; they
 * wait for the next enter or the poller.
 */
void
kqring_kpoll_run(struct thread *td)
{
	struct kqring_ctx *rings[8], *ctx;
	int n = 0;

	if (atomic_load_int(&kqring_kp_nrings) == 0)
		return;
	mtx_lock(&kqring_proc_mtx);
	LIST_FOREACH(ctx, &kqring_ctx_list, proc_link) {
		if (n == nitems(rings))
			break;
		if (ctx->owner_proc == td->td_proc && ctx->kp_running &&
		    !ctx->closing) {
			refcount_acquire(&ctx->refs);
			rings[n++] = ctx;
		}
	}
	mtx_unlock(&kqring_proc_mtx);

	for (int i = 0; i < n; i++) {
		ctx = rings[i];
		kqring_kp_scan(ctx, td);
		if (ctx->taskrun_only && (ctx->shared ||
		    kqring_is_owner(ctx, td)) && sx_try_xlock(&ctx->sq_sx)) {
			if (!ctx->closing) {
				kqring_run_taskwork(ctx);
				kqring_flush_overflow(ctx);
				kqring_evfd_flush(ctx);
			}
			sx_xunlock(&ctx->sq_sx);
			mtx_lock(&ctx->mtx);
			if (ctx->sleeping)
				wakeup(&ctx->wakeup_chan);
			mtx_unlock(&ctx->mtx);
		}
		kqring_ctx_rele(ctx);
	}
}

/*
 * Ring teardown: wait for the poller to see ctx->closing and exit.  As
 * kqring_teardown_sleep(): EINTR to defer, or with nowait EWOULDBLOCK.
 */
int
kqring_kpoll_stop(struct kqring_ctx *ctx, bool nowait)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	while (ctx->kp_running && curthread != ctx->kp_td) {
		if (nowait)
			return (EWOULDBLOCK);
		if (kqring_teardown_sleep(&ctx->kp_running, &ctx->mtx,
		    "kqkpdt") != 0)
			return (EINTR);
	}
	return (0);
}

/* Close the ring's kqueue (its knotes go with it). */
void
kqring_kpoll_close(struct kqring_ctx *ctx)
{
	struct file *fp;

	mtx_lock(&ctx->mtx);
	fp = ctx->kp_fp;
	ctx->kp_fp = NULL;
	mtx_unlock(&ctx->mtx);
	if (fp != NULL)
		fdrop(fp, curthread);
}
