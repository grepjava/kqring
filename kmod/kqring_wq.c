/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

#include <sys/signalvar.h>
#include <sys/sysent.h>
#include <sys/umtxvar.h>

/* kqring_wq_enter_count(): threads in enters, per CPU. */
COUNTER_U64_DEFINE_EARLY(kqring_entered);

/* File reads run inline by kqring_wq_try_inline(). */
static COUNTER_U64_DEFINE_EARLY(kqring_inline_reads);
SYSCTL_COUNTER_U64(_kern_kqring, OID_AUTO, wq_inline, CTLFLAG_RD,
    &kqring_inline_reads,
    "File reads run by the thread that submitted them (pool's reads cached, CPUs busy)");

/*
 * Threads in enters take more than half the CPUs.  Summed at most once a
 * tick: the counts live on other CPUs' cache lines, which each enter
 * writes, and reading them all for every submission cost more than it saved.
 */
static u_int kqring_busy_tick;
static bool kqring_busy;

static bool
kqring_wq_cpus_busy(void)
{
	u_int t = (u_int)ticks;
	int64_t n;

	if (atomic_load_int(&kqring_busy_tick) == t)
		return (atomic_load_bool(&kqring_busy));
	n = (int64_t)counter_u64_fetch(kqring_entered);
	atomic_store_bool(&kqring_busy, n * 2 > mp_ncpus);
	atomic_store_int(&kqring_busy_tick, t);
	return (n * 2 > mp_ncpus);
}

/*
 * Whether ctx's enter runs its cached file reads itself
 * (kqring_wq_try_inline()): while its pool's file I/O rarely sleeps, and
 * threads in enters already take more than half the CPUs, so that workers
 * could only run in their place.  With CPUs to spare, workers run a ring's
 * reads in parallel, faster than its one thread could.
 */
bool
kqring_wq_inline_ok(struct kqring_ctx *ctx)
{
	struct kqring_tctx *t = kqring_tctx_cur(ctx);
	struct kqring_wq *wq = t != NULL ? t->wq : NULL;

	if (wq == NULL || !atomic_load_bool(&wq->cached))
		return (false);
	/*
	 * Threads come and go from enters all the time, so the count dips
	 * below half for moments while the CPUs stay taken.  A read sent to a
	 * worker then costs far more than its share: the worker fights the
	 * issuers for a CPU and spins on the locks they hold.  So once inline,
	 * a ring stays so for KQRING_WQ_INLINE_HOLD ticks after the CPUs last
	 * looked busy.  Only its issuer, under the enter lock, uses it.
	 */
	if (kqring_wq_cpus_busy()) {
		ctx->inline_until = ticks + KQRING_WQ_INLINE_HOLD;
		return (true);
	}
	/* 0: never set (ticks wrap, so compare only a set one). */
	return (ctx->inline_until != 0 && ctx->inline_until - ticks > 0);
}

static u_long kqring_wq_capped, kqring_wq_stalls;
SYSCTL_ULONG(_kern_kqring, OID_AUTO, wq_capped, CTLFLAG_RD,
    &kqring_wq_capped, 0,
    "Times a pool capped its awake bound workers (work rarely sleeping)");
SYSCTL_ULONG(_kern_kqring, OID_AUTO, wq_stalls, CTLFLAG_RD,
    &kqring_wq_stalls, 0,
    "Times capped bound work made no progress and the cap was lifted");

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
	struct kqring_op *op;
	bool any_running = false;

	mtx_assert(&ctx->mtx, MA_OWNED);
	/* Each on its submitter's pool. */
	TAILQ_FOREACH(op, &ctx->retired_ops, active_link) {
		struct kqring_wq *wq = op->tctx->wq;

		if (wq == NULL)
			continue;
		mtx_lock(&wq->mtx);
		if (op->wq_running) {
			any_running = true;
			wakeup(&op->in_taskwork);
			kqring_wq_abort_td(op->wq_td);
		}
		mtx_unlock(&wq->mtx);
	}
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
		kqring_wakeup_locked(ctx);
	mtx_unlock(&ctx->mtx);
}


/* ============================================================================
 * In-Process Worker Thread Pool (IO_WQ_BOUND / IO_WQ_UNBOUND)
 * ============================================================================
 */

void
kqring_worker_hold(void)
{
	mtx_lock(&kqring_proc_mtx);
	kqring_live_workers++;
	mtx_unlock(&kqring_proc_mtx);
}

void
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
	wq->sleep_avg = 1024;
	wq->cache_avg = 1024;
	wq->cpu_cap = MAX(KQRING_WQ_CPU_MIN, (uint32_t)mp_ncpus / 2);
	callout_init_mtx(&wq->stall_co, &wq->mtx, 0);
	wq->base_cpuset = cpuset_ref(td->td_cpuset);
	return (wq);
}

/*
 * How many of type's workers may be awake (running a request or spinning
 * for one): all of them, unless bound work rarely sleeps.  Then a few,
 * which take the next request themselves when they finish, so that one
 * isn't woken and put back to sleep for each, as io-wq starts another
 * worker only when one blocks.  Workers are still created as before, and
 * sleep as a reserve that kqring_wq_stall_cb() can wake.
 */
static uint32_t
kqring_wq_awake_cap_locked(const struct kqring_wq *wq, int type)
{
	mtx_assert(&wq->mtx, MA_OWNED);
	if (type != IO_WQ_BOUND || !wq->cpu_bound)
		return (UINT32_MAX);
	return (MIN(wq->max[type], wq->cpu_cap));
}

/*
 * Bound work was capped and queued, and none ran for KQRING_WQ_STALL_SBT:
 * the awake workers are sleeping after all.  Stop capping and wake them.
 */
static void
kqring_wq_stall_cb(void *arg)
{
	struct kqring_wq *wq = arg;

	mtx_assert(&wq->mtx, MA_OWNED);
	if (wq->closing || !wq->cpu_bound || wq->pending[IO_WQ_BOUND] == 0)
		return;
	if (wq->bound_done != wq->stall_done) {
		wq->stall_done = wq->bound_done;
		callout_reset_sbt(&wq->stall_co, KQRING_WQ_STALL_SBT, 0,
		    kqring_wq_stall_cb, wq, 0);
		return;
	}
	wq->cpu_bound = false;
	wq->sleep_avg = 1024;
	atomic_add_long(&kqring_wq_stalls, 1);
	wakeup(&wq->ops[IO_WQ_BOUND]);
}

/*
 * A bound request ran: switched, if its thread switched out voluntarily
 * (and so might have run on a worker in its place); slept, if it also took
 * KQRING_WQ_SLEPT_US or more, as a read from the disk does and one waiting
 * briefly for a lock in the cache doesn't.  The first decides the cap on
 * awake workers, the second whether reads may run inline
 * (kqring_wq_try_inline()).
 */
static void
kqring_wq_note_sleep_locked(struct kqring_wq *wq, bool switched, bool slept)
{
	mtx_assert(&wq->mtx, MA_OWNED);
	wq->bound_done++;
	if (slept)
		wq->cache_avg += (1024 - wq->cache_avg) >> 2;
	else
		wq->cache_avg -= wq->cache_avg >> 6;
	if (!wq->cached && wq->cache_avg < KQRING_WQ_CPU_ENTER)
		wq->cached = true;
	else if (wq->cached && wq->cache_avg > KQRING_WQ_CPU_LEAVE)
		wq->cached = false;
	/*
	 * Slow to cap, quick to stop: about 90 requests in a row that don't
	 * sleep cap the pool, which a mix of cache hits and misses doesn't
	 * reach by chance; three that sleep stop it.
	 */
	if (switched)
		wq->sleep_avg += (1024 - wq->sleep_avg) >> 2;
	else
		wq->sleep_avg -= wq->sleep_avg >> 6;
	if (!wq->cpu_bound && wq->sleep_avg < KQRING_WQ_CPU_ENTER) {
		wq->cpu_bound = true;
		atomic_add_long(&kqring_wq_capped, 1);
	} else if (wq->cpu_bound && wq->sleep_avg > KQRING_WQ_CPU_LEAVE) {
		wq->cpu_bound = false;
		if (wq->pending[IO_WQ_BOUND] > wq->spinning[IO_WQ_BOUND])
			wakeup(&wq->ops[IO_WQ_BOUND]);
	}
}

/* Bound work queued without waking a worker: watch for a stall. */
static void
kqring_wq_stall_arm_locked(struct kqring_wq *wq)
{
	mtx_assert(&wq->mtx, MA_OWNED);
	if (wq->closing || callout_pending(&wq->stall_co))
		return;
	wq->stall_done = wq->bound_done;
	callout_reset_sbt(&wq->stall_co, KQRING_WQ_STALL_SBT, 0,
	    kqring_wq_stall_cb, wq, 0);
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
	wq->closing = true;		/* the stall callout doesn't rearm */
	mtx_unlock(&wq->mtx);
	callout_drain(&wq->stall_co);
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
 * and the caller must kqring_kthread_exit().
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
kqring_wq_suspend_point(struct kqring_op *op)
{
	struct kqring_wq *wq = op->tctx->wq;
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

/*
 * Queue op for type's workers: after its bucket's last queued write, if it
 * is an ordered write (op->wq_hash), so that a bucket's writes are together.
 */
static void
kqring_wq_queue_locked(struct kqring_wq *wq, struct kqring_op *op, int type)
{
	struct kqring_op **tailp;

	mtx_assert(&wq->mtx, MA_OWNED);
	op->wq_queued = true;
	wq->pending[type]++;
	if (op->wq_hash < 0) {
		TAILQ_INSERT_TAIL(&wq->ops[type], op, wq_link);
		return;
	}
	tailp = &wq->hash_tail[type][op->wq_hash];
	if (*tailp != NULL)
		TAILQ_INSERT_AFTER(&wq->ops[type], *tailp, op, wq_link);
	else
		TAILQ_INSERT_TAIL(&wq->ops[type], op, wq_link);
	*tailp = op;
}

void
kqring_wq_unqueue_locked(struct kqring_wq *wq, struct kqring_op *op)
{
	int type = op->wq_type;

	mtx_assert(&wq->mtx, MA_OWNED);
	KASSERT(op->wq_queued, ("kqring: op %p not queued", op));
	if (op->wq_hash >= 0 && wq->hash_tail[type][op->wq_hash] == op) {
		struct kqring_op *prev = TAILQ_PREV(op, kqring_op_list,
		    wq_link);

		wq->hash_tail[type][op->wq_hash] =
		    prev != NULL && prev->wq_hash == op->wq_hash ? prev : NULL;
	}
	TAILQ_REMOVE(&wq->ops[type], op, wq_link);
	op->wq_queued = false;
	wq->pending[type]--;
}

/*
 * The first op a worker of type may run: the first queued, passing over
 * the writes of a bucket that has one running (one step per bucket, as
 * they are together).
 */
static struct kqring_op *
kqring_wq_next_locked(struct kqring_wq *wq, int type)
{
	struct kqring_op *op;

	mtx_assert(&wq->mtx, MA_OWNED);
	op = TAILQ_FIRST(&wq->ops[type]);
	while (op != NULL && op->wq_hash >= 0 &&
	    (wq->hash_busy & (1ULL << op->wq_hash)) != 0)
		op = TAILQ_NEXT(wq->hash_tail[type][op->wq_hash], wq_link);
	return (op);
}

/*
 * A read of a regular file, at issue in an enter's own submission, while
 * ctx's pool's file I/O rarely sleeps: run it here, as pread(2) would and
 * as Linux runs a cached read in io_uring_enter(2), rather than hand it to
 * a worker and back.  It counts toward the pool's average as a worker's
 * read would, so reads that start to sleep go back to the workers.  Only
 * on a local filesystem: one over a network could hold the enter for as
 * long as its server takes.  Returns whether it ran, with op->res and
 * op->cflags set.
 */
bool
kqring_wq_try_inline(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_wq *wq = op->tctx->wq;
	struct thread *td = curthread;
	uint64_t t0;
	long nvcsw;
	bool punt, slept, switched;

	if (ctx->submit_inline < 0)
		ctx->submit_inline = kqring_wq_inline_ok(ctx) ? 1 : 0;
	if (ctx->submit_inline == 0 || wq == NULL ||
	    !atomic_load_bool(&wq->cached))
		return (false);
	nvcsw = td->td_ru.ru_nvcsw;
	t0 = cpu_ticks();
	(void)kqring_exec_file_read_local(ctx, op, &punt);
	if (punt)
		return (false);
	switched = td->td_ru.ru_nvcsw != nvcsw;
	slept = switched &&
	    cputick2usec(cpu_ticks() - t0) >= KQRING_WQ_SLEPT_US;
	mtx_lock(&wq->mtx);
	kqring_wq_note_sleep_locked(wq, switched, slept);
	mtx_unlock(&wq->mtx);
	counter_u64_add(kqring_inline_reads, 1);
	return (true);
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
	bool capped;
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
		/* Enough others awake (kqring_wq_awake_cap_locked()): sleep. */
		capped = wq->busy[type] >= kqring_wq_awake_cap_locked(wq, type);
		op = capped ? NULL : kqring_wq_next_locked(wq, type);
		if (op != NULL) {
			uint64_t t0;
			bool added = false, deferred = false, slept, switched;
			int hash = op->wq_hash;
			long nvcsw;

			kqring_wq_unqueue_locked(wq, op);
			if (op->cancelled || op->wq_canceled)
				continue;
			if (hash >= 0)
				wq->hash_busy |= 1ULL << hash;
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

			/*
			 * The CPU's tick counter, which is cheaper to read than
			 * sbinuptime(), converted only for a request that
			 * switched: this is on every worker request.
			 */
			nvcsw = td->td_ru.ru_nvcsw;
			t0 = cpu_ticks();
			kqring_wq_exec_op(ctx, op);
			switched = td->td_ru.ru_nvcsw != nvcsw;
			slept = switched && cputick2usec(cpu_ticks() - t0) >=
			    KQRING_WQ_SLEPT_US;

			mtx_lock(&ctx->mtx);
			mtx_lock(&wq->mtx);
			op->wq_running = false;
			op->wq_td = NULL;
			if (!op->wq_wait_ready)
				op->wq_done = true;
			wq->busy[type]--;
			/*
			 * A write counts as one that slept: one that returned
			 * at once may stall in the filesystem's write throttle
			 * with all the others the next moment, and capped
			 * workers would then hold the queue back.
			 */
			if (type == IO_WQ_BOUND)
				kqring_wq_note_sleep_locked(wq,
				    switched || kqring_op_is_write(op->sqe.opcode),
				    slept || kqring_op_is_write(op->sqe.opcode));
			/*
			 * The bucket's next write may run: this worker takes
			 * it next, unless it is leaving, so wake another.
			 */
			if (hash >= 0) {
				wq->hash_busy &= ~(1ULL << hash);
				if (wq->hash_tail[type][hash] != NULL)
					wakeup_one(&wq->ops[type]);
			}
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
					kqring_tw_insert_locked(ctx, op);
				}
				if (ctx->shm != NULL)
					atomic_set_32(&ctx->shm->sq_flags,
					    IORING_SQ_TASKRUN);
				kqring_evfd_signal_locked(ctx);
			} else if (!op->cancelled && !ctx->closing &&
			    (op->wq_blocking || op->wq_wait_ready)) {
				/* A finished read or write may batch its wake. */
				if (op->wq_wait_ready)
					kqring_taskwork_add_locked(ctx, op);
				else
					deferred =
					    kqring_taskwork_add_lazy_locked(ctx, op);
				added = true;
			}
			/*
			 * Taskwork added above woke the kthread if it must;
			 * a retired op's, queued directly, needs it woken.
			 */
			if (ctx->sleeping == 0 || deferred) {
				/* Nobody to wake, or the wake is batched. */
			} else if (!added) {
				kqring_wakeup_locked(ctx);
			} else if (ctx->sleeping != ctx->task_sleeping) {
				wakeup(&ctx->wakeup_chan);
			}
			mtx_unlock(&ctx->mtx);
			/* A read of a pipe: see kqring_file_rw(). */
			if (sx_xlocked(&ctx->pipe_sx))
				sx_xunlock(&ctx->pipe_sx);
			kqring_ctx_rele(ctx);
			mtx_lock(&wq->mtx);
			continue;
		}

		/*
		 * One idle worker spins for the next op; the rest sleep.  With
		 * every idle worker spinning, each op sent them all at wq->mtx.
		 * Not with every CPU taken, or with issuers taking most of
		 * them (kqring_wq_cpus_busy()): a spinner then only keeps the
		 * thread with the next op, or its issuer, from running.  Nor
		 * when capped: the awake workers take what is queued.
		 */
		if (!capped && mp_ncpus > 1 && wq->spinning[type] == 0 &&
		    sched_load() < mp_ncpus && !kqring_wq_cpus_busy()) {
			wq->spinning[type]++;
			mtx_unlock(&wq->mtx);
			for (int s = 0; s < 2048; s++) {
				if (atomic_load_32(&wq->pending[type]) != 0 ||
				    wq->closing ||
				    kqring_wq_suspend_pending())
					break;
				cpu_spinwait();
				__compiler_membar();
			}
			mtx_lock(&wq->mtx);
			wq->spinning[type]--;
			if (kqring_wq_next_locked(wq, type) != NULL ||
			    wq->closing || kqring_wq_check_suspend(wq))
				continue;
		}
		if (type == IO_WQ_BOUND)
			wq->sleepers++;
		error = msleep_sbt(&wq->ops[type], &wq->mtx, PCATCH,
		    "kqwqid", KQRING_WQ_IDLE_SBT, 0, 0);
		if (type == IO_WQ_BOUND) {
			wq->sleepers--;
			if (wq->woken > 0)
				wq->woken--;
		}
		if (error == EWOULDBLOCK &&
		    TAILQ_EMPTY(&wq->ops[type]))
			break;
	}
	wq->total[type]--;
	kqring_wq_rele_locked(wq);
	kqring_kthread_exit();
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

/*
 * Whether the issuer should spin for an op that needs no order.  Only for a
 * program in lockstep, which has taken every CQE and now waits for this one:
 * one that is filling a queue gains nothing, and the spin would run its
 * requests one at a time.  Not with every CPU taken, as in kqring_wq_thread().
 */
static bool
kqring_wq_spin_locked(struct kqring_ctx *ctx)
{
	mtx_assert(&ctx->mtx, MA_OWNED);
	return (ctx->shm != NULL &&
	    atomic_load_acq_32(&ctx->shm->cq_tail) ==
	    atomic_load_acq_32(&ctx->shm->cq_head) &&
	    sched_load() < mp_ncpus);
}

/* A bucket for a vnode, as io-wq's hash_ptr() of the inode. */
static int
kqring_wq_hash(const void *p)
{
	return ((int)(((uintptr_t)p * 0x9E3779B97F4A7C15ULL) >> 58));
}

/*
 * Whether op needs no order against other requests on its file: a read, or
 * an O_DIRECT write at an offset of its own, as io-wq, which hashes buffered
 * writes per inode, but not O_DIRECT ones where the filesystem can run them
 * in parallel.  Any other write to a regular file (buffered, at the file
 * position, appending, with RWF_* flags) gets its vnode's bucket in *hashp,
 * which runs its writes one at a time; other ordered ops get -1.  Pipes and
 * sockets get none, as in io-wq: a write that waits for a reader must not
 * hold back the files that share its bucket.
 */
static bool
kqring_op_wq_order(const struct kqring_op *op, int *hashp, bool *vregp)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp;
	bool read, direct, vreg;

	*hashp = -1;
	*vregp = false;
	read = kqring_op_is_read(sqe->opcode);
	if (!read && !kqring_op_is_write(sqe->opcode))
		return (false);
	if (op->ffile != NULL)
		fp = op->ffile->fp;
	else if (fget(td, sqe->fd, &cap_no_rights, &fp) != 0)
		return (read);
	vreg = fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL &&
	    fp->f_vnode->v_type == VREG;
	direct = vreg && (off_t)sqe->off != (off_t)-1 && sqe->rw_flags == 0 &&
	    (fp->f_flag & (O_DIRECT | O_APPEND)) == O_DIRECT;
	if (vreg && !read && !direct)
		*hashp = kqring_wq_hash(fp->f_vnode);
	*vregp = vreg;
	if (op->ffile == NULL)
		fdrop(fp, td);
	return (read || direct);
}

/*
 * Whether a cancel must leave op be: disk I/O in flight, or a regular
 * file's read or write that a worker has started or finished, which can't
 * be undone, as io-wq can't cancel running work.  The op then completes with
 * what it did, and the cancel finds nothing to cancel (-EALREADY).  One
 * still queued is taken off the queue here, so that no worker starts it
 * after this said it could be cancelled.
 */
bool
kqring_op_cancel_busy_locked(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_wq *wq = op->tctx->wq;
	bool busy;

	mtx_assert(&ctx->mtx, MA_OWNED);
	if (op->bio)
		return (true);
	if (!op->wq_vreg || wq == NULL)
		return (false);
	mtx_lock(&wq->mtx);
	if (op->wq_queued)
		kqring_wq_unqueue_locked(wq, op);
	busy = op->wq_running || op->wq_done;
	mtx_unlock(&wq->mtx);
	return (busy);
}

/* Whether op is such I/O, for a synchronous cancel to wait for it. */
bool
kqring_op_in_flight(const struct kqring_op *op)
{
	return (op->bio || (op->wq_vreg && (op->wq_running || op->wq_done)));
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
	struct kqring_wq *wq = op->tctx->wq;
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
	struct kqring_wq *wq = op->tctx->wq;
	bool need_spawn = false, sync, unordered, vreg;
	int hash;

	if (op->cred == NULL)
		op->cred = crhold(td->td_ucred);
	op->wq_type = (uint8_t)type;
	op->wq_blocking = kqring_op_will_block(op);
	/* An ordered write's order is its bucket's; the issuer needn't wait. */
	unordered = kqring_op_wq_order(op, &hash, &vreg) || hash >= 0;
	op->wq_hash = (int8_t)hash;
	op->wq_vreg = vreg;

	mtx_lock(&ctx->mtx);
	mtx_lock(&wq->mtx);
	op->cancelled = false;
	op->wq_canceled = false;
	op->wq_done = false;
	kqring_op_arm_locked(ctx, op);
	kqring_wq_queue_locked(wq, op, type);
	/*
	 * A read, an O_DIRECT write or an ordered write to a regular file
	 * (kqring_op_wq_order()) waits for its worker below only when nothing
	 * else could run meanwhile: no more SQEs in this submission and no
	 * other op on its type's workers.  Otherwise a batch of them would go
	 * to the workers one at a time; the issuer collects completions in its
	 * enter wait.  Ordered writes keep their order by bucket, as io-wq's
	 * per-inode hashing keeps it.  Other ops always wait.  An op not waited
	 * for is posted by its worker, as a blocking one is (set before any
	 * worker can take it).
	 */
	sync = !unordered ||
	    (!(ctx->submitting && ctx->submit_more) &&
	    wq->pending[type] + wq->busy[type] == 1);
	if (!sync)
		op->wq_blocking = true;
	if (!wq->closing && wq->total[type] < wq->pending[type] + wq->busy[type] &&
	    wq->total[type] < wq->max[type]) {
		wq->total[type]++;
		wq->refs++;
		need_spawn = true;
	}
	if (wq->spinning[type] < wq->pending[type]) {
		uint32_t cap = kqring_wq_awake_cap_locked(wq, type);

		if (cap == UINT32_MAX) {
			wakeup_one(&wq->ops[type]);
		} else if (wq->busy[type] + wq->spinning[type] + wq->woken <
		    cap) {
			/*
			 * Count the woken until they run: with every CPU
			 * taken they wait, and each op would wake another.
			 */
			if (wq->sleepers > wq->woken) {
				wq->woken++;
				wakeup_one(&wq->ops[type]);
			}
		} else {
			kqring_wq_stall_arm_locked(wq);
		}
	}
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
	if (sync && mp_ncpus > 1 && kqring_wq_can_wait_op_locked(ctx, op) &&
	    (!unordered || kqring_wq_spin_locked(ctx))) {
		mtx_unlock(&ctx->mtx);
		for (int s = 0; s < 2048; s++) {
			if (op->wq_done || op->wq_blocking || op->cancelled ||
			    ctx->closing)
				break;
			cpu_spinwait();
			__compiler_membar();
		}
		mtx_lock(&ctx->mtx);
	}
	/*
	 * One still running after the spin is waiting for the disk.  Leave it
	 * to its worker: waiting here would hold back the next enter, and a
	 * program submitting one per enter would then only ever have one in
	 * flight, however many it allows.
	 */
	if (sync && unordered && !op->wq_done) {
		op->wq_blocking = true;
		sync = false;
	}
	while (sync && kqring_wq_can_wait_op_locked(ctx, op)) {
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
	/* A worker posts an op marked blocking itself (taskwork). */
	if (op->wq_done && !op->wq_blocking && !op->cancelled &&
	    !ctx->closing) {
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
		if (error == 0) {
			/* As knotes on the ring's kqueue, or else a worker. */
			error = kqring_kpoll_arm(ctx, op, fp);
			fdrop(fp, td);
			if (error == 0)
				return (true);
			if (error != EOPNOTSUPP) {
				kqring_req_complete(ctx, op, -error, 0);
				return (false);
			}
			/* A bridge has no worker: the ring has this kqueue. */
			if (op->kq_bridge) {
				kqring_req_complete(ctx, op, -EBUSY, 0);
				return (false);
			}
			error = kqring_op_fget(op,
			    cap_rights_init_one(&rights, CAP_POLL_EVENT), 0,
			    &fp);
		}
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
	struct kqring_wq *wq = op->tctx->wq;
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
	op->wq_hash = -1;
	op->wq_vreg = false;
	op->wq_blocking = true;
	op->wq_canceled = false;
	op->wq_done = false;
	kqring_wq_queue_locked(wq, op, IO_WQ_UNBOUND);
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


/*
 * Handle stop and single-threading requests (fork, exec, exit, SIGSTOP)
 * on a ring kthread (SQPOLL, completions, poller).  Returns true if the
 * ring is closing or the process exiting: the thread then exits.
 */
bool
kqring_kthread_check_suspend(struct kqring_ctx *ctx)
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

/*
 * kthread_exit() for a ring kthread (a worker, SQPOLL or completions
 * thread, the poller), which also drops its kqring_worker_hold().
 *
 * These threads live in a user process.  A thread exiting while SIGSTOP or
 * a debugger stops the process can be the last one the stop waits for,
 * but kthread_exit() does not report the stop then, so it never completes:
 * the parent or debugger never hears of it and never continues the
 * process, which waits forever (under ptrace not even SIGKILL frees it).
 * thr_exit(2) calls thread_stopped() for that, under the process locks up
 * to thread_exit(), and so does this; otherwise it is kthread_exit().
 */
void
kqring_kthread_exit(void)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	/* As late as can be: the module may unload once the count is 0. */
	kqring_worker_rele();
	wakeup(td);
	PROC_LOCK(p);
	KASSERT(p->p_numthreads > 1, ("kqring: last thread is a ring kthread"));
	if (p->p_sysent->sv_ontdexit != NULL)
		p->p_sysent->sv_ontdexit(td);
	tidhash_remove(td);
	umtx_thread_exit(td);
	tdsigcleanup(td);
	PROC_SLOCK(p);
	thread_stopped(p);
	thread_exit();
}

static void
kqring_sqpoll_thread(void *arg)
{
	struct kqring_tctx *tctx = arg;
	struct kqring_ctx *ctx = tctx->ctx;
	struct kqring_op_list *tw = &tctx->taskwork;
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
		if (kqring_kthread_check_suspend(ctx))
			break;

		/*
		 * Interrupted only by a stop or exit (signals are masked): go
		 * and suspend or exit above, rather than wait uninterruptibly
		 * on an issuer that was stopped holding sq_sx.
		 */
		if (sx_xlock_sig(&ctx->sq_sx) != 0)
			continue;
		/* Closing, or its process exited (or exits). */
		if (ctx->closing || tctx->dead) {
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
			/*
			 * Completions only: enter waiters sleep on wakeup_chan
			 * (kqring_wait_cqes() re-checks the CQ under mtx).
			 */
			if (taskrun) {
				mtx_lock(&ctx->mtx);
				if (ctx->sleeping)
					wakeup(&ctx->wakeup_chan);
				mtx_unlock(&ctx->mtx);
			}
			continue;
		}

		mtx_lock(&ctx->mtx);
		if (ctx->closing || tctx->dead) {
			mtx_unlock(&ctx->mtx);
			break;
		}
		if (ctx->npolled > 0)
			kqring_poll_fallback_locked(ctx);
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if ((!taskrun && atomic_load_acq_32(&shm->sq_tail) !=
		    shm->sq_head) || !TAILQ_EMPTY(tw) ||
		    !KQRING_STQ_EMPTY_HINT(&ctx->rcq)) {
			mtx_unlock(&ctx->mtx);
			continue;
		}

		sbintime_t now = getsbinuptime();
		sbintime_t elapsed = now - last_active;
		if (elapsed < ctx->sqpoll_idle_sbt && !taskrun) {
			/*
			 * Within sq_thread_idle, poll for SQEs and completions
			 * as io_uring's SQPOLL thread does, without sleeping:
			 * a sleep here cost a wake-up for every request, to
			 * submit it and again to complete it.  Slices of 50 us
			 * keep the checks above running, and a stop, exit or
			 * close ends the spin at once.
			 */
			mtx_unlock(&ctx->mtx);
			for (sbintime_t start = now; now - start < 50 * SBT_1US;
			    now = getsbinuptime()) {
				if (atomic_load_acq_32(&shm->sq_tail) !=
				    shm->sq_head ||
				    !TAILQ_EMPTY(tw) ||
				    !KQRING_STQ_EMPTY_HINT(&ctx->rcq) || ctx->closing ||
				    tctx->dead || kqring_wq_suspend_pending())
					break;
				for (int s = 0; s < 64; s++)
					cpu_spinwait();
			}
			continue;
		}
		if (elapsed < ctx->sqpoll_idle_sbt) {
			ctx->sleeping++;
			ctx->task_sleeping++;
			(void)msleep_sbt(&ctx->task_chan, &ctx->mtx, PCATCH,
			    "kqsqpl", 50 * SBT_1US, 0, 0);
			ctx->task_sleeping--;
			ctx->sleeping--;
			mtx_unlock(&ctx->mtx);
			continue;
		}

		/* Only SQPOLL asks to be woken for submissions. */
		if (!taskrun) {
			atomic_set_32(&shm->sq_flags, IORING_SQ_NEED_WAKEUP);
			if (atomic_load_acq_32(&shm->sq_tail) != shm->sq_head ||
			    !TAILQ_EMPTY(tw) ||
			    !KQRING_STQ_EMPTY_HINT(&ctx->rcq) || ctx->closing ||
			    tctx->dead) {
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
		 * then suspends or exits in kqring_kthread_check_suspend().
		 */
		ctx->sleeping++;
		ctx->task_sleeping++;
		(void)msleep_sbt(&ctx->task_chan, &ctx->mtx, PCATCH, "kqsqid",
		    period, 0, 0);
		ctx->task_sleeping--;
		ctx->sleeping--;
		if (!taskrun)
			atomic_clear_32(&shm->sq_flags, IORING_SQ_NEED_WAKEUP);
		mtx_unlock(&ctx->mtx);
		last_active = getsbinuptime();
	}

	mtx_lock(&ctx->mtx);
	tctx->task_running = false;
	tctx->task_td = NULL;
	wakeup(&tctx->task_running);
	bool finish_cancel = ctx->closing && !ctx->cancelling &&
	    !ctx->cancelled;
	mtx_unlock(&ctx->mtx);
	if (finish_cancel)
		kqring_ctx_cancel_all(ctx);
	kqring_tctx_rele(tctx);
	kqring_ctx_rele(ctx);
	kqring_kthread_exit();
}

int
kqring_sqpoll_start(struct kqring_ctx *ctx, struct kqring_tctx *tctx,
    struct thread *td)
{
	int error;

	mtx_lock(&ctx->mtx);
	if (ctx->closing) {
		mtx_unlock(&ctx->mtx);
		return (EBADF);
	}
	if (tctx->task_running) {
		mtx_unlock(&ctx->mtx);
		return (0);
	}
	tctx->task_running = true;
	refcount_acquire(&ctx->refs);
	refcount_acquire(&tctx->refs);
	mtx_unlock(&ctx->mtx);

	kqring_worker_hold();
	error = kthread_add(kqring_sqpoll_thread, tctx, td->td_proc,
	    &tctx->task_td, 0, 0, ctx->taskrun_only ? "kqtask" : "kqsqp");
	if (error != 0) {
		kqring_worker_rele();
		mtx_lock(&ctx->mtx);
		tctx->task_running = false;
		tctx->task_td = NULL;
		mtx_unlock(&ctx->mtx);
		kqring_tctx_rele(tctx);
		kqring_ctx_rele(ctx);
	}
	return (error);
}

int
kqring_sqpoll_wait_cqes(struct kqring_ctx *ctx, uint32_t min_complete,
    bool has_timeout, sbintime_t sbt)
{
	struct kqring_shm_header *shm = ctx->shm;
	sbintime_t deadline = has_timeout ? kqring_deadline(sbt) : 0;
	int error;

	for (;;) {
		if (has_timeout)
			sbt = kqring_until(deadline);
		if ((atomic_load_32(&shm->sq_flags) &
		    IORING_SQ_CQ_OVERFLOW) != 0) {
			if (sx_xlock_sig(&ctx->sq_sx) != 0)
				return (EINTR);
			kqring_flush_overflow(ctx);
			sx_xunlock(&ctx->sq_sx);
		}
		uint32_t cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= min_complete || !KQRING_STQ_EMPTY_HINT(&ctx->ocq))
			return (0);
		if (has_timeout && sbt == 0)
			return (ETIMEDOUT);

		mtx_lock(&ctx->mtx);
		cq_count = atomic_load_acq_32(&shm->cq_tail) -
		    atomic_load_acq_32(&shm->cq_head);
		if (cq_count >= min_complete || !KQRING_STQ_EMPTY_HINT(&ctx->ocq)) {
			mtx_unlock(&ctx->mtx);
			return (0);
		}
		if (!TAILQ_EMPTY(&ctx->retired_ops))
			(void)kqring_wq_abort_retired_locked(ctx);
		if ((ctx->ntaskwork != 0 ||
		    !KQRING_STQ_EMPTY_HINT(&ctx->rcq) ||
		    atomic_load_acq_32(&shm->sq_tail) !=
		    atomic_load_acq_32(&shm->sq_head)) && ctx->sleeping)
			kqring_wakeup_locked(ctx);

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
	}
}
