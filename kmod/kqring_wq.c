/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

void
kqring_wq_abort_td(struct thread *wtd)
{
	if (wtd == NULL)
		return;
	thread_lock(wtd);
	if (TD_CAN_ABORT(wtd))
		sleepq_abort(wtd, EINTR);	/* unlocks wtd */
	else
		thread_unlock(wtd);
}

bool
kqring_wq_abort_retired_locked(struct kqring_ctx *ctx)
{
	struct kqring_wq *wq = ctx->wq;
	struct kqring_op *op;
	bool any_running = false;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (wq == NULL || TAILQ_EMPTY(&ctx->retired_ops))
		return (false);
	mtx_lock(&wq->mtx);
	TAILQ_FOREACH(op, &ctx->retired_ops, active_link) {
		if (op->wq_running) {
			any_running = true;
			wakeup(&op->in_taskwork);
			kqring_wq_abort_td(op->wq_td);
		}
	}
	mtx_unlock(&wq->mtx);
	return (any_running);
}

void
kqring_wq_abort_cb(void *arg)
{
	struct kqring_ctx *ctx = arg;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (kqring_wq_abort_retired_locked(ctx)) {
		callout_reset_sbt(&ctx->wq_abort_callout, 50 * SBT_1US, 0,
		    kqring_wq_abort_cb, ctx, 0);
	} else {
		ctx->wq_abort_pending = false;
	}
}


void
kqring_wq_mark_blocking(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (!op->wq_running || op->wq_blocking)
		return;
	mtx_lock(&ctx->mtx);
	op->wq_blocking = true;
	if (ctx->sleeping)
		wakeup(&ctx->wakeup_chan);
	mtx_unlock(&ctx->mtx);
}


/* ============================================================================
 * In-Process Worker Thread Pool (IO_WQ_BOUND / IO_WQ_UNBOUND)
 * ============================================================================
 */

static void
kqring_worker_hold(void)
{
	mtx_lock(&kqring_proc_mtx);
	kqring_live_workers++;
	mtx_unlock(&kqring_proc_mtx);
}

static void
kqring_worker_rele(void)
{
	mtx_lock(&kqring_proc_mtx);
	KASSERT(kqring_live_workers > 0, ("kqring: worker count underflow"));
	if (--kqring_live_workers == 0)
		wakeup(&kqring_live_workers);
	mtx_unlock(&kqring_proc_mtx);
}


struct kqring_wq *
kqring_wq_alloc(struct thread *td)
{
	struct kqring_wq *wq;

	wq = malloc(sizeof(*wq), M_KQRING, M_WAITOK | M_ZERO);
	mtx_init(&wq->mtx, "kqring wq", NULL, MTX_DEF);
	wq->refs = 1;
	wq->ring_refs = 1;
	wq->proc = td->td_proc;
	for (int t = 0; t < KQRING_WQ_TYPES; t++) {
		wq->arg[t].wq = wq;
		wq->arg[t].type = t;
		TAILQ_INIT(&wq->ops[t]);
	}
	wq->max[IO_WQ_BOUND] = MIN((uint32_t)mp_ncpus * 4,
	    KQRING_WQ_MAX_WORKERS);
	wq->max[IO_WQ_UNBOUND] = KQRING_WQ_MAX_WORKERS;
	wq->base_cpuset = cpuset_ref(td->td_cpuset);
	return (wq);
}

static void
kqring_wq_rele_locked(struct kqring_wq *wq)
{
	mtx_assert(&wq->mtx, MA_OWNED);
	KASSERT(wq->refs > 0, ("kqring: wq refcount underflow"));
	if (--wq->refs != 0) {
		mtx_unlock(&wq->mtx);
		return;
	}
	mtx_unlock(&wq->mtx);
	if (wq->base_cpuset != NULL)
		cpuset_rel(wq->base_cpuset);
	mtx_destroy(&wq->mtx);
	free(wq, M_KQRING);
}

void
kqring_wq_rele(struct kqring_wq *wq)
{
	if (wq == NULL)
		return;
	mtx_lock(&wq->mtx);
	kqring_wq_rele_locked(wq);
}

void
kqring_wq_close(struct kqring_wq *wq)
{
	if (wq == NULL)
		return;
	mtx_lock(&wq->mtx);
	KASSERT(wq->ring_refs > 0, ("kqring: wq ring_refs underflow"));
	if (--wq->ring_refs == 0) {
		wq->closing = true;
		for (int t = 0; t < KQRING_WQ_TYPES; t++)
			wakeup(&wq->ops[t]);
	}
	mtx_unlock(&wq->mtx);
}

void
kqring_wq_put(struct kqring_wq *wq)
{
	if (wq == NULL)
		return;
	mtx_lock(&wq->mtx);
	KASSERT(wq->ring_refs > 0, ("kqring: wq ring_refs underflow"));
	if (--wq->ring_refs == 0) {
		wq->closing = true;
		for (int t = 0; t < KQRING_WQ_TYPES; t++)
			wakeup(&wq->ops[t]);
	}
	kqring_wq_rele_locked(wq);
}

/*
 * Handle process-wide stop/single-threading requests (fork, execve,
 * exit1, SIGSTOP) on an in-process worker kthread.  Called with
 * wq->mtx held; returns true if the process is exiting or exec'ing
 * and the caller must kthread_exit().
 */
static bool
kqring_wq_check_suspend(struct kqring_wq *wq)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	mtx_assert(&wq->mtx, MA_OWNED);
	if (!P_SHOULDSTOP(p) && (p->p_flag2 & P2_WEXIT) == 0 &&
	    !td_ast_pending(td, TDA_SUSPEND))
		return (false);
	mtx_unlock(&wq->mtx);
	PROC_LOCK(p);
	while (thread_suspend_check_needed() ||
	    (p->p_flag2 & P2_WEXIT) != 0) {
		if ((p->p_flag & P_SINGLE_EXIT) != 0 ||
		    (p->p_flag2 & P2_WEXIT) != 0) {
			PROC_UNLOCK(p);
			mtx_lock(&wq->mtx);
			return (true);
		}
		PROC_SLOCK(p);
		thread_stopped(p);
		if (P_SHOULDSTOP(p) == P_STOPPED_SINGLE &&
		    p->p_numthreads == p->p_suspcount + 1) {
			thread_lock(p->p_singlethread);
			if (TD_IS_SUSPENDED(p->p_singlethread)) {
				TD_CLR_SUSPENDED(p->p_singlethread);
				p->p_singlethread->td_flags &= ~TDF_ALLPROCSUSP;
				p->p_suspcount--;
				setrunnable(p->p_singlethread, 0);
			} else {
				thread_unlock(p->p_singlethread);
			}
		}
		PROC_UNLOCK(p);
		thread_lock(td);
		thread_suspend_one(td);
		p->p_boundary_count++;
		td->td_flags |= TDF_BOUNDARY;
		PROC_SUNLOCK(p);
		mi_switch(SW_INVOL | SWT_SUSPEND);
		PROC_LOCK(p);
	}
	PROC_UNLOCK(p);
	mtx_lock(&wq->mtx);
	return (false);
}

/* Whether the worker's process asks it to stop, exit or exec. */
bool
kqring_wq_suspend_pending(void)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	return (P_SHOULDSTOP(p) || (p->p_flag2 & P2_WEXIT) != 0 ||
	    td_ast_pending(td, TDA_SUSPEND));
}

/*
 * A suspension point for a worker waiting inside a request, called without
 * locks: stop here for SIGSTOP or single-threading, as at the top of the
 * worker loop.  A wait that only ends when the ring owner runs taskwork
 * would otherwise keep exit1() (which suspends the owner first) waiting
 * forever.  Returns true if the process is exiting or exec'ing: the caller
 * stops waiting, and the worker exits at the top of its loop.
 */
bool
kqring_wq_suspend_point(struct kqring_ctx *ctx)
{
	struct kqring_wq *wq = ctx->wq;
	bool exiting;

	if (wq == NULL || !kqring_wq_suspend_pending())
		return (false);
	mtx_lock(&wq->mtx);
	exiting = kqring_wq_check_suspend(wq);
	mtx_unlock(&wq->mtx);
	return (exiting);
}

static void
kqring_wq_apply_affinity(struct kqring_wq *wq, uint32_t *genp)
{
	cpuset_t mask;
	uint32_t gen;

	mtx_lock(&wq->mtx);
	gen = wq->aff_gen;
	if (gen == *genp && gen != 0) {
		mtx_unlock(&wq->mtx);
		return;
	}
	if (wq->aff_set)
		CPU_COPY(&wq->cpumask, &mask);
	else if (wq->base_cpuset != NULL)
		CPU_COPY(&wq->base_cpuset->cs_mask, &mask);
	else
		CPU_COPY(&all_cpus, &mask);
	*genp = gen;
	mtx_unlock(&wq->mtx);
	(void)cpuset_setthread(curthread->td_tid, &mask);
}

static void
kqring_wq_exec_op(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct ucred *saved;

	if (op->wq_canceled || op->cancelled || ctx->closing) {
		op->res = -ECANCELED;
		op->cflags = 0;
		return;
	}
	saved = kqring_cred_enter(op);
	if (op->wq_wait_ready)
		kqring_wq_poll_wait(ctx, op);
	else if (op->sqe.opcode == IORING_OP_WAITID)
		(void)kqring_exec_waitid(op, false);
	else
		kqring_exec_sync(ctx, op);
	kqring_cred_exit(saved);
}

static void
kqring_wq_thread(void *arg)
{
	struct {
		struct kqring_wq	*wq;
		int			type;
	} *wa = arg;
	struct kqring_wq *wq = wa->wq;
	int type = wa->type;
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct kqring_ctx *ctx;
	struct kqring_op *op;
	uint32_t aff_gen = UINT32_MAX;
	int error;

	/*
	 * Mask all maskable signals on this worker kthread so process-wide
	 * user signals (SIGINT, SIGALRM, SIGUSR1, ...) are delivered to
	 * user threads rather than interrupting background ring I/O.
	 */
	PROC_LOCK(p);
	SIGFILLSET(td->td_sigmask);
	SIG_CANTMASK(td->td_sigmask);
	PROC_UNLOCK(p);

	kqring_wq_apply_affinity(wq, &aff_gen);

	mtx_lock(&wq->mtx);
	for (;;) {
		if (wq->closing || kqring_wq_check_suspend(wq))
			break;
		if (wq->total[type] > wq->max[type])
			break;
		if (aff_gen != wq->aff_gen) {
			mtx_unlock(&wq->mtx);
			kqring_wq_apply_affinity(wq, &aff_gen);
			mtx_lock(&wq->mtx);
			continue;
		}
		op = TAILQ_FIRST(&wq->ops[type]);
		if (op != NULL) {
			TAILQ_REMOVE(&wq->ops[type], op, wq_link);
			op->wq_queued = false;
			wq->pending[type]--;
			if (op->cancelled || op->wq_canceled)
				continue;
			ctx = op->ctx;
			refcount_acquire(&ctx->refs);
			op->wq_running = true;
			op->wq_td = td;
			wq->busy[type]++;
			if (!wq->closing && wq->total[type] <
			    wq->pending[type] + wq->busy[type] &&
			    wq->total[type] < wq->max[type]) {
				wq->total[type]++;
				wq->refs++;
				mtx_unlock(&wq->mtx);
				kqring_worker_hold();
				if (kthread_add(kqring_wq_thread,
				    &wq->arg[type], p, NULL, 0, 0,
				    "kqwq%c", type == IO_WQ_BOUND ? 'b' : 'u')
				    != 0) {
					kqring_worker_rele();
					mtx_lock(&wq->mtx);
					wq->total[type]--;
					kqring_wq_rele_locked(wq);
				}
			} else {
				mtx_unlock(&wq->mtx);
			}

			kqring_wq_exec_op(ctx, op);

			mtx_lock(&ctx->mtx);
			mtx_lock(&wq->mtx);
			op->wq_running = false;
			op->wq_td = NULL;
			if (!op->wq_wait_ready)
				op->wq_done = true;
			wq->busy[type]--;
			wakeup(&op->wq_running);
			mtx_unlock(&wq->mtx);
			if (ctx->wq_abort_pending) {
				struct kqring_op *rop;
				bool any_run = false;

				TAILQ_FOREACH(rop, &ctx->retired_ops,
				    active_link) {
					if (rop->wq_running) {
						any_run = true;
						break;
					}
				}
				if (!any_run) {
					callout_stop(&ctx->wq_abort_callout);
					ctx->wq_abort_pending = false;
				}
			}
			if (op->wq_retired && op->wq_complete_pending &&
			    !ctx->closing) {
				/* Only the issuer posts CQEs and releases resources. */
				if (!op->in_taskwork) {
					op->in_taskwork = true;
					TAILQ_INSERT_TAIL(&ctx->taskwork_ops, op,
					    taskwork_link);
				}
				if (ctx->shm != NULL)
					atomic_set_32(&ctx->shm->sq_flags,
					    IORING_SQ_TASKRUN);
				kqring_evfd_signal_locked(ctx);
			} else if (!op->cancelled && !ctx->closing &&
			    (op->wq_blocking || op->wq_wait_ready))
				kqring_taskwork_add_locked(ctx, op);
			if (ctx->sleeping)
				wakeup(&ctx->wakeup_chan);
			mtx_unlock(&ctx->mtx);
			kqring_ctx_rele(ctx);
			mtx_lock(&wq->mtx);
			continue;
		}

		error = msleep_sbt(&wq->ops[type], &wq->mtx, PCATCH,
		    "kqwqid", KQRING_WQ_IDLE_SBT, 0, 0);
		if (error == EWOULDBLOCK &&
		    TAILQ_EMPTY(&wq->ops[type]))
			break;
	}
	wq->total[type]--;
	kqring_wq_rele_locked(wq);
	kqring_worker_rele();
	kthread_exit();
}

static bool
kqring_op_will_block(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	size_t want = sqe->len;
	bool is_write = false, block = false;

	switch (sqe->opcode) {
	case IORING_OP_WRITE:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITEV:
	case IORING_OP_WRITEV_FIXED:
		is_write = true;
		/* FALLTHROUGH */
	case IORING_OP_READ:
	case IORING_OP_READ_FIXED:
	case IORING_OP_READV:
	case IORING_OP_READV_FIXED:
		if ((off_t)sqe->off != (off_t)-1)
			return (false);
		if (kqring_op_readv(sqe->opcode) ||
		    kqring_op_writev(sqe->opcode)) {
			const struct iovec *iov = op->args;

			want = 0;
			for (u_int i = 0; i < sqe->len; i++)
				want += iov[i].iov_len;
		}
		if (want == 0 && !is_write &&
		    (sqe->flags & IOSQE_BUFFER_SELECT) == 0)
			return (false);
		if (op->ffile != NULL)
			return (kqring_pipe_will_block(op->ffile->fp, is_write,
			    want));
		if (fget(td, sqe->fd, &cap_no_rights, &fp) != 0)
			return (false);
		block = kqring_pipe_will_block(fp, is_write, want);
		fdrop(fp, td);
		return (block);
	case IORING_OP_WAITID:
		return ((sqe->file_index & WNOHANG) == 0);
	default:
		return (false);
	}
}

int
kqring_op_wq_type(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	bool vreg;

	switch (sqe->opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_READV_FIXED:
	case IORING_OP_WRITEV_FIXED:
		if (op->ffile != NULL) {
			fp = op->ffile->fp;
			vreg = fp->f_type == DTYPE_VNODE &&
			    fp->f_vnode != NULL && fp->f_vnode->v_type == VREG;
			return (vreg ? IO_WQ_BOUND : IO_WQ_UNBOUND);
		}
		if (fget(td, sqe->fd, &cap_no_rights, &fp) != 0)
			return (IO_WQ_BOUND);
		vreg = fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
		    fp->f_vnode->v_type == VREG;
		fdrop(fp, td);
		return (vreg ? IO_WQ_BOUND : IO_WQ_UNBOUND);
	case IORING_OP_SPLICE:
	case IORING_OP_TEE:
	case IORING_OP_WAITID:
		return (IO_WQ_UNBOUND);
	default:
		return (IO_WQ_BOUND);
	}
}

static bool
kqring_wq_can_wait_op_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_wq *wq = ctx->wq;
	bool can_wait = false;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->cancelled || op->wq_done || op->wq_blocking || ctx->closing ||
	    wq == NULL)
		return (false);
	mtx_lock(&wq->mtx);
	if (op->wq_running) {
		can_wait = true;
		if (op->wq_td != NULL) {
			bool abortable;

			thread_lock(op->wq_td);
			abortable = TD_CAN_ABORT(op->wq_td);
			thread_unlock(op->wq_td);
			if (abortable) {
				op->wq_blocking = true;
				can_wait = false;
			}
		}
	} else if (op->wq_queued &&
	    wq->pending[op->wq_type] + wq->busy[op->wq_type] <=
	    wq->total[op->wq_type]) {
		can_wait = true;
	} else if (!op->wq_done) {
		op->wq_blocking = true;
	}
	mtx_unlock(&wq->mtx);
	return (can_wait);
}

bool
kqring_wq_enqueue(struct kqring_ctx *ctx, struct kqring_op *op, int type)
{
	struct thread *td = curthread;
	struct kqring_wq *wq = ctx->wq;
	bool need_spawn = false;

	if (op->cred == NULL)
		op->cred = crhold(td->td_ucred);
	op->wq_type = (uint8_t)type;
	op->wq_blocking = kqring_op_will_block(op);

	mtx_lock(&ctx->mtx);
	mtx_lock(&wq->mtx);
	op->cancelled = false;
	op->wq_canceled = false;
	op->wq_done = false;
	op->wq_queued = true;
	kqring_op_arm_locked(ctx, op);
	TAILQ_INSERT_TAIL(&wq->ops[type], op, wq_link);
	wq->pending[type]++;
	if (!wq->closing && wq->total[type] < wq->pending[type] + wq->busy[type] &&
	    wq->total[type] < wq->max[type]) {
		wq->total[type]++;
		wq->refs++;
		need_spawn = true;
	}
	wakeup_one(&wq->ops[type]);
	mtx_unlock(&wq->mtx);
	if (need_spawn) {
		mtx_unlock(&ctx->mtx);
		kqring_worker_hold();
		if (kthread_add(kqring_wq_thread, &wq->arg[type],
		    wq->proc, NULL, 0, 0, "kqwq%c",
		    type == IO_WQ_BOUND ? 'b' : 'u') != 0) {
			kqring_worker_rele();
			mtx_lock(&wq->mtx);
			wq->total[type]--;
			kqring_wq_rele_locked(wq);
		}
		mtx_lock(&ctx->mtx);
	}
	while (kqring_wq_can_wait_op_locked(ctx, op)) {
		int error;

		ctx->sleeping++;
		error = msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
		    "kqwqsy", 100 * SBT_1US, 0, 0);
		ctx->sleeping--;
		if ((error == EINTR || error == ERESTART) && !op->wq_done) {
			op->wq_blocking = true;
			break;
		}
	}
	if (op->wq_done && !op->cancelled && !ctx->closing) {
		int32_t res;

		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		res = op->res;
		if (op->wq_fp != NULL) {
			struct file *fp = op->wq_fp;

			op->wq_fp = NULL;
			res = kqring_fixed_install(ctx, fp, &op->wq_caps,
			    op->sqe.file_index);
		}
		kqring_req_complete(ctx, op, res, op->cflags);
		return (false);
	}
	mtx_unlock(&ctx->mtx);
	return (true);
}



bool
kqring_arm_nonsock_op(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	cap_rights_t rights;
	int error;

	if (sqe->opcode == IORING_OP_POLL_ADD) {
		error = kqring_op_fget(op,
		    cap_rights_init_one(&rights, CAP_POLL_EVENT), 0, &fp);
	} else if (sqe->opcode == IORING_OP_READ_MULTISHOT) {
		error = kqring_op_fget(op,
		    cap_rights_init_one(&rights, CAP_READ), FREAD, &fp);
		/* A pipe is a stream: the offset is ignored, as in io_uring. */
		if (error == 0 && fp->f_type != DTYPE_PIPE) {
			fdrop(fp, td);
			error = EOPNOTSUPP;
		}
	} else {
		error = ENOTSOCK;
	}
	if (error != 0) {
		kqring_req_complete(ctx, op, -error, 0);
		return (false);
	}
	op->armed_fp = fp;
	fdrop(fp, td);
	op->wq_wait_ready = true;
	op->wq_blocking = false;
	return (kqring_wq_enqueue(ctx, op, IO_WQ_UNBOUND));
}

void
kqring_wq_rearm_ready(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_wq *wq = ctx->wq;
	bool need_spawn = false;

	mtx_lock(&ctx->mtx);
	if (op->cancelled || ctx->closing || wq == NULL) {
		mtx_unlock(&ctx->mtx);
		return;
	}
	mtx_lock(&wq->mtx);
	if (op->wq_running || op->wq_queued) {
		wakeup(&op->in_taskwork);
		mtx_unlock(&wq->mtx);
		mtx_unlock(&ctx->mtx);
		return;
	}
	op->wq_type = IO_WQ_UNBOUND;
	op->wq_blocking = true;
	op->wq_canceled = false;
	op->wq_done = false;
	op->wq_queued = true;
	TAILQ_INSERT_TAIL(&wq->ops[IO_WQ_UNBOUND], op, wq_link);
	wq->pending[IO_WQ_UNBOUND]++;
	if (!wq->closing && wq->total[IO_WQ_UNBOUND] <
	    wq->pending[IO_WQ_UNBOUND] + wq->busy[IO_WQ_UNBOUND] &&
	    wq->total[IO_WQ_UNBOUND] < wq->max[IO_WQ_UNBOUND]) {
		wq->total[IO_WQ_UNBOUND]++;
		wq->refs++;
		need_spawn = true;
	}
	wakeup_one(&wq->ops[IO_WQ_UNBOUND]);
	mtx_unlock(&wq->mtx);
	mtx_unlock(&ctx->mtx);
	if (need_spawn) {
		kqring_worker_hold();
		if (kthread_add(kqring_wq_thread,
		    &wq->arg[IO_WQ_UNBOUND], wq->proc, NULL, 0, 0,
		    "kqwqu") != 0) {
			kqring_worker_rele();
			mtx_lock(&wq->mtx);
			wq->total[IO_WQ_UNBOUND]--;
			kqring_wq_rele_locked(wq);
		}
	}
}


static bool
kqring_sqpoll_check_suspend(struct kqring_ctx *ctx)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	if (ctx->closing)
		return (true);
	if (!P_SHOULDSTOP(p) && (p->p_flag2 & P2_WEXIT) == 0 &&
	    !td_ast_pending(td, TDA_SUSPEND))
		return (false);
	PROC_LOCK(p);
	while (thread_suspend_check_needed() ||
	    (p->p_flag2 & P2_WEXIT) != 0) {
		if ((p->p_flag & P_SINGLE_EXIT) != 0 ||
		    (p->p_flag2 & P2_WEXIT) != 0) {
			PROC_UNLOCK(p);
			return (true);
		}
		PROC_SLOCK(p);
		thread_stopped(p);
		if (P_SHOULDSTOP(p) == P_STOPPED_SINGLE &&
		    p->p_numthreads == p->p_suspcount + 1) {
			thread_lock(p->p_singlethread);
			if (TD_IS_SUSPENDED(p->p_singlethread)) {
				TD_CLR_SUSPENDED(p->p_singlethread);
				p->p_singlethread->td_flags &= ~TDF_ALLPROCSUSP;
				p->p_suspcount--;
				setrunnable(p->p_singlethread, 0);
			} else {
				thread_unlock(p->p_singlethread);
			}
		}
		PROC_UNLOCK(p);
		thread_lock(td);
		thread_suspend_one(td);
		p->p_boundary_count++;
		td->td_flags |= TDF_BOUNDARY;
		PROC_SUNLOCK(p);
		mi_switch(SW_INVOL | SWT_SUSPEND);
		PROC_LOCK(p);
	}
	PROC_UNLOCK(p);
	return (ctx->closing);
}

static void
kqring_sqpoll_thread(void *arg)
{
	struct kqring_ctx *ctx = arg;
	struct kqring_shm_header *shm = ctx->shm;
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	sbintime_t last_active;
	/* Completions only (no DEFER_TASKRUN): never submits or spins. */
	bool taskrun = ctx->taskrun_only;

	PROC_LOCK(p);
	SIGFILLSET(td->td_sigmask);
	SIG_CANTMASK(td->td_sigmask);
	PROC_UNLOCK(p);

	if ((ctx->flags & IORING_SETUP_SQ_AFF) != 0) {
		cpuset_t mask;

		CPU_SETOF((int)ctx->sqpoll_cpu, &mask);
		(void)cpuset_setthread(td->td_tid, &mask);
	}

	last_active = getsbinuptime();
	for (;;) {
		if (kqring_sqpoll_check_suspend(ctx))
			break;

		sx_xlock(&ctx->sq_sx);
		if (ctx->closing) {
			sx_xunlock(&ctx->sq_sx);
			break;
		}
		uint32_t submitted = 0;
		uint32_t cq_before = shm->cq_tail;
		uint32_t ocq_before = ctx->nocq;

		kqring_flush_overflow(ctx);
		uint32_t pending = taskrun ? 0 :
		    atomic_load_acq_32(&shm->sq_tail) - shm->sq_head;
		if (pending != 0)
			kqring_submit_sqes(ctx, pending, &submitted);
		kqring_run_taskwork(ctx);
		kqring_run_checks(ctx);
		kqring_flush_overflow(ctx);
		kqring_evfd_flush(ctx);
		bool did_work = submitted > 0 || shm->cq_tail != cq_before ||
		    ctx->nocq != ocq_before;
		sx_xunlock(&ctx->sq_sx);

		if (did_work) {
			last_active = getsbinuptime();
			if (ctx->sqpoll_waiting) {
				mtx_lock(&ctx->mtx);
				if (ctx->sqpoll_waiting)
					wakeup(&ctx->sqpoll_wait_chan);
				mtx_unlock(&ctx->mtx);
			}
			continue;
		}

		mtx_lock(&ctx->mtx);
		if (ctx->closing) {
			mtx_unlock(&ctx->mtx);
			break;
		}
		if (ctx->npolled > 0)
			kqring_poll_fallback_locked(ctx);
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if ((!taskrun && atomic_load_acq_32(&shm->sq_tail) !=
		    shm->sq_head) || !TAILQ_EMPTY(&ctx->taskwork_ops) ||
		    !STAILQ_EMPTY(&ctx->rcq)) {
			mtx_unlock(&ctx->mtx);
			continue;
		}

		sbintime_t now = getsbinuptime();
		sbintime_t elapsed = now - last_active;
		if (elapsed < ctx->sqpoll_idle_sbt) {
			ctx->sleeping++;
			(void)msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH,
			    "kqsqpl", 50 * SBT_1US, 0, 0);
			ctx->sleeping--;
			mtx_unlock(&ctx->mtx);
			continue;
		}

		/* Only SQPOLL asks to be woken for submissions. */
		if (!taskrun) {
			atomic_set_32(&shm->sq_flags, IORING_SQ_NEED_WAKEUP);
			if (atomic_load_acq_32(&shm->sq_tail) != shm->sq_head ||
			    !TAILQ_EMPTY(&ctx->taskwork_ops) ||
			    !STAILQ_EMPTY(&ctx->rcq) || ctx->closing) {
				atomic_clear_32(&shm->sq_flags,
				    IORING_SQ_NEED_WAKEUP);
				mtx_unlock(&ctx->mtx);
				last_active = getsbinuptime();
				continue;
			}
		}
		sbintime_t period = 0;
		if (ctx->nfutex > 0 || ctx->npolled > 0 ||
		    !TAILQ_EMPTY(&ctx->retired_ops))
			period = 50 * SBT_1US;
		else if (ctx->nwaitid > 0)
			period = SBT_1MS;
		/*
		 * Interruptible, as the workers' sleeps are: thread_single()
		 * (exit, exec, SIGSTOP) only aborts sleeps with PCATCH, and an
		 * idle ring sleeps here without a timeout.  Signals are masked,
		 * so only stop and exit requests interrupt it, and the loop
		 * then suspends or exits in kqring_sqpoll_check_suspend().
		 */
		ctx->sleeping++;
		(void)msleep_sbt(&ctx->wakeup_chan, &ctx->mtx, PCATCH, "kqsqid",
		    period, 0, 0);
		ctx->sleeping--;
		if (!taskrun)
			atomic_clear_32(&shm->sq_flags, IORING_SQ_NEED_WAKEUP);
		mtx_unlock(&ctx->mtx);
		last_active = getsbinuptime();
	}

	mtx_lock(&ctx->mtx);
	ctx->sqpoll_running = false;
	ctx->sqpoll_td = NULL;
	wakeup(&ctx->sqpoll_running);
	bool finish_cancel = ctx->closing && !ctx->cancelling &&
	    !ctx->cancelled;
	mtx_unlock(&ctx->mtx);
	if (finish_cancel)
		kqring_ctx_cancel_all(ctx);
	kqring_ctx_rele(ctx);
	kqring_worker_rele();
	kthread_exit();
}

int
kqring_sqpoll_start(struct kqring_ctx *ctx, struct thread *td)
{
	int error;

	mtx_lock(&ctx->mtx);
	if (ctx->closing) {
		mtx_unlock(&ctx->mtx);
		return (EBADF);
	}
	if (ctx->sqpoll_running) {
		mtx_unlock(&ctx->mtx);
		return (0);
	}
	ctx->sqpoll_running = true;
	refcount_acquire(&ctx->refs);
	mtx_unlock(&ctx->mtx);

	kqring_worker_hold();
	error = kthread_add(kqring_sqpoll_thread, ctx, td->td_proc,
	    &ctx->sqpoll_td, 0, 0, ctx->taskrun_only ? "kqtask" : "kqsqp");
	if (error != 0) {
		kqring_worker_rele();
		mtx_lock(&ctx->mtx);
		ctx->sqpoll_running = false;
		ctx->sqpoll_td = NULL;
		mtx_unlock(&ctx->mtx);
		kqring_ctx_rele(ctx);
	}
	return (error);
}

int
kqring_sqpoll_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt)
{
	struct kqring_shm_header *shm = ctx->shm;
	int error;

	for (;;) {
		if ((atomic_load_32(&shm->sq_flags) &
		    IORING_SQ_CQ_OVERFLOW) != 0) {
			sx_xlock(&ctx->sq_sx);
			kqring_flush_overflow(ctx);
			sx_xunlock(&ctx->sq_sx);
		}
		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= min_complete || !STAILQ_EMPTY(&ctx->ocq))
			return (0);
		if (has_timeout && sbt == 0)
			return (ETIMEDOUT);

		mtx_lock(&ctx->mtx);
		cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= min_complete || !STAILQ_EMPTY(&ctx->ocq)) {
			mtx_unlock(&ctx->mtx);
			return (0);
		}
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if ((!TAILQ_EMPTY(&ctx->taskwork_ops) ||
		    !STAILQ_EMPTY(&ctx->rcq) ||
		    atomic_load_acq_32(&shm->sq_tail) !=
		    atomic_load_acq_32(&shm->sq_head)) && ctx->sleeping)
			wakeup(&ctx->wakeup_chan);

		sbintime_t sleep_sbt = has_timeout ? MIN(sbt, SBT_1MS) :
		    SBT_1MS;
		if (sleep_sbt == 0)
			sleep_sbt = 1;
		ctx->sqpoll_waiting = true;
		error = msleep_sbt(&ctx->sqpoll_wait_chan, &ctx->mtx, PCATCH,
		    "kqsqcq", sleep_sbt, 0, 0);
		ctx->sqpoll_waiting = false;
		mtx_unlock(&ctx->mtx);

		if (error == EINTR || error == ERESTART)
			return (EINTR);
		if (error == EWOULDBLOCK && has_timeout) {
			if (sbt <= sleep_sbt)
				return (ETIMEDOUT);
			sbt -= sleep_sbt;
		}
	}
}
