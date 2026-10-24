/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

/*
 * POLL_ADD on files other than sockets (pipes, FIFOs, eventfds, ttys,
 * kqueues, devices with a kqfilter, the ring descriptor itself).
 *
 * Each such request is a knote (EVFILT_READ and/or EVFILT_WRITE on its
 * descriptor) on a kqueue the ring owns but that has no descriptor of its
 * own, and one kthread per ring waits in kevent(2) on it.  As io_uring's
 * poll wait queue entries do, a knote catches every wakeup of the file
 * (EV_CLEAR: a write, then a read before the poller looks, still counts),
 * any number of requests cost no threads, and a multishot request gets a
 * CQE per event.  The poller only records events and queues the request
 * as taskwork; kqring_kpoll_deliver() posts the CQEs.
 *
 * A request on a fixed file (no descriptor to register), one whose file
 * has no kqfilter, or a second one on the same descriptor and filter (a
 * kqueue holds one knote per descriptor and filter) waits on a worker
 * instead (kqring_wq_poll_wait()).
 */

#include "kqring_var.h"

#include <sys/syscallsubr.h>

#define	KQRING_KP_NEVENTS	32
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

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, KQRING_KP_BUCKET(ctx, kev->ident), kp_link) {
		if (op->kp_id == id)
			break;
	}
	if (op != NULL && op->kp_on && !op->cancelled) {
		op->kp_revents |= kqring_kp_revents(kev);
		op->kp_nev++;
		kqring_taskwork_add_locked(ctx, op);
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
	wakeup(&ctx->kp_running);
	mtx_unlock(&ctx->mtx);
	kqring_ctx_rele(ctx);
	kqring_worker_rele();
	kthread_exit();
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
		mtx_unlock(&ctx->mtx);
		/* As the poller would on exit: see kqring_kp_thread(). */
		kqring_kpoll_close(ctx);
		kqring_ctx_rele(ctx);
	}
	return (error);
}

static int
kqring_kp_kevents(const struct kqring_op *op, struct kevent *kev,
    u_short flags)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	void *udata = (void *)(uintptr_t)op->kp_id;
	int n = 0;

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
	op->kp_on = true;
	op->cancelled = false;
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
	if (error == 0)
		return (0);
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

/* Ring teardown: wait for the poller to see ctx->closing and exit. */
void
kqring_kpoll_stop(struct kqring_ctx *ctx)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	while (ctx->kp_running && curthread != ctx->kp_td)
		msleep(&ctx->kp_running, &ctx->mtx, 0, "kqkpdt", 0);
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
