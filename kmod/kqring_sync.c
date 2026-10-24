/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

struct kqring_fwait_list kqring_futex_hash[KQRING_FUTEX_HASH];
struct mtx kqring_futex_mtx;

/*
 * TIMEOUT / LINK_TIMEOUT expiry.  The callout runs with ctx->mtx held
 * (callout_init_mtx), so callout_stop() under ctx->mtx guarantees it will
 * not run afterwards; like an upcall it only queues taskwork.
 */
static void
kqring_timer_cb(void *arg)
{
	struct kqring_op *op = arg;

	mtx_assert(&op->ctx->mtx, MA_OWNED);
	op->timer_pending = false;
	kqring_taskwork_add_locked(op->ctx, op);
}

/* Stop op's timer and unqueue an expiry not yet run. */
void
kqring_timer_stop_locked(struct kqring_op *op)
{
	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (op->timer_pending) {
		callout_stop(&op->callout);
		op->timer_pending = false;
	}
	if (op->in_taskwork) {
		TAILQ_REMOVE(&op->ctx->taskwork_ops, op, taskwork_link);
		op->in_taskwork = false;
	}
}


/*
 * Take op's FUTEX_WAIT(V) words off kqring_futex_hash.  Only the owner
 * thread hashes and unhashes, so ->hashed is stable without the lock.
 */
void
kqring_futex_unhash_locked(struct kqring_op *op)
{
	struct kqring_fwait *fw = op->args;
	uint32_t i;

	for (i = 0; i < op->nfwait && !fw[i].hashed; i++)
		;
	if (i == op->nfwait)
		return;
	/* Lock order: kqring_futex_mtx before ctx->mtx. */
	mtx_unlock(&op->ctx->mtx);
	mtx_lock(&kqring_futex_mtx);
	for (; i < op->nfwait; i++) {
		if (fw[i].hashed) {
			LIST_REMOVE(&fw[i], link);
			fw[i].hashed = false;
		}
	}
	mtx_unlock(&kqring_futex_mtx);
	mtx_lock(&op->ctx->mtx);
}


/*
 * Futexes.  A FUTEX_WAIT(V) is armed with its words hashed by umtx key
 * (the same key _umtx_op(2) uses: vmspace + address for FUTEX2_PRIVATE,
 * else the backing VM object + offset for shared mappings), so FUTEX_WAKE
 * from any ring, in any process sharing the memory, finds it.  The waker
 * only marks the request and queues it as taskwork for its ring's owner.
 * Armed words are also re-read on every enter (kqring_check_futexes()), so
 * a word changed without a FUTEX_WAKE still ends the wait, a spurious
 * wakeup futex users must already handle.
 */
static int
kqring_futex_key(uint64_t uaddr, uint32_t flags, struct umtx_key *key)
{
	return (umtx_key_get((void *)(uintptr_t)uaddr, TYPE_SIMPLE_WAIT,
	    (flags & FUTEX2_PRIVATE) != 0 ? THREAD_SHARE : AUTO_SHARE, key));
}


/* Arm FUTEX_WAIT / FUTEX_WAITV; returns false if completed instead. */
bool
kqring_futex_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_fwait *fw = op->args;
	uint32_t i, uval = 0;
	int32_t res;
	int error = 0;

	for (i = 0; i < op->nfwait; i++) {
		error = kqring_futex_key(fw[i].uaddr, fw[i].flags, &fw[i].key);
		if (error != 0) {
			kqring_req_complete(ctx, op, -error, 0);
			return (false);
		}
		fw[i].keyed = true;
	}

	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	op->futex_woken = false;
	ctx->nfutex++;
	kqring_op_arm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);

	/* Hash before reading the words, so a racing FUTEX_WAKE is seen. */
	mtx_lock(&kqring_futex_mtx);
	for (i = 0; i < op->nfwait; i++) {
		LIST_INSERT_HEAD(kqring_futex_bucket(&fw[i].key), &fw[i], link);
		fw[i].hashed = true;
	}
	mtx_unlock(&kqring_futex_mtx);

	for (i = 0; i < op->nfwait; i++) {
		error = copyin((const void *)(uintptr_t)fw[i].uaddr, &uval,
		    sizeof(uval));
		if (error != 0 || uval != (uint32_t)fw[i].val)
			break;
	}
	if (i == op->nfwait)
		return (true);

	mtx_lock(&ctx->mtx);
	/* A FUTEX_WAKE that already counted this waiter wins. */
	res = op->futex_woken ? op->res : (error != 0 ? -error : -EAGAIN);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, res, 0);
	return (false);
}

/*
 * FUTEX_WAKE: wake up to sqe->addr2 waiters whose mask intersects
 * sqe->addr3, returning how many ring waiters were woken.  Threads
 * sleeping in _umtx_op(UMTX_OP_WAIT_UINT[_PRIVATE]) on the word are woken
 * too, within the same budget.
 */
int32_t
kqring_futex_wake(const struct io_uring_sqe *sqe)
{
	struct umtx_key key;
	struct kqring_fwait *fw;
	uint64_t nr = MIN(sqe->addr2, (uint64_t)INT32_MAX);
	uint32_t mask = sqe->addr3 != 0 ? (uint32_t)sqe->addr3 :
	    FUTEX_BITSET_MATCH_ANY;
	int32_t n = 0;
	int error;

	error = kqring_futex_key(sqe->addr, (uint32_t)sqe->fd, &key);
	if (error != 0)
		return (-error);
	mtx_lock(&kqring_futex_mtx);
	LIST_FOREACH(fw, kqring_futex_bucket(&key), link) {
		struct kqring_op *op = fw->op;

		if ((uint64_t)n >= nr)
			break;
		if (!umtx_key_match(&fw->key, &key) || (fw->mask & mask) == 0)
			continue;
		mtx_lock(&op->ctx->mtx);
		if (!op->cancelled && !op->futex_woken) {
			op->futex_woken = true;
			op->res = (int32_t)fw->idx;
			kqring_taskwork_add_locked(op->ctx, op);
			n++;
		}
		mtx_unlock(&op->ctx->mtx);
	}
	mtx_unlock(&kqring_futex_mtx);
	umtx_key_release(&key);

	if ((uint64_t)n < nr)
		(void)kern_umtx_wake(curthread, (void *)(uintptr_t)sqe->addr,
		    (int)(nr - (uint64_t)n), (sqe->fd & FUTEX2_PRIVATE) != 0);
	return (n);
}

/*
 * WAITID: wait6(2) on the op's child selector.  With `nohang`, poll once
 * with WNOHANG and return EAGAIN if a blocking wait is still needed;
 * without `nohang`, block in kern_wait6() on an unbound worker thread.
 */
int
kqring_exec_waitid(struct kqring_op *op, bool nohang)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	siginfo_t si;
	int options = (int)sqe->file_index;
	int status, error;

	if (nohang)
		options |= WNOHANG;
	bzero(&si, sizeof(si));
	td->td_retval[0] = 0;
	error = kern_wait6(td, (idtype_t)sqe->len, (id_t)sqe->fd, &status,
	    options, NULL, &si);
	if (error != 0) {
		if ((error == EINTR || error == ERESTART) &&
		    (op->wq_canceled || op->cancelled))
			return (kqring_done(op, -ECANCELED, 0));
		return (kqring_done(op, kqring_errno(error), 0));
	}
	if (td->td_retval[0] == 0) {
		if ((sqe->file_index & WNOHANG) == 0)
			return (EAGAIN);
		/*
		 * proc_to_reap() fills siginfo for a live child too; as
		 * libc's waitid(), report zeroes when nothing changed.
		 */
		bzero(&si, sizeof(si));
	}
	if (sqe->addr2 != 0) {
		error = copyout(&si, (void *)(uintptr_t)sqe->addr2,
		    sizeof(si));
		if (error != 0)
			return (kqring_done(op, -error, 0));
	}
	return (kqring_done(op, 0, 0));
}

bool
kqring_waitid_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if ((op->sqe.flags & IOSQE_ASYNC) == 0 &&
	    kqring_exec_waitid(op, true) == 0) {
		kqring_req_complete(ctx, op, op->res, 0);
		return (false);
	}
	return (kqring_wq_enqueue(ctx, op, IO_WQ_UNBOUND));
}


/* Free a queued MSG_RING CQE, with the file it may carry. */
void
kqring_rcqe_free(struct kqring_rcqe *rc)
{
	if (rc->fp != NULL) {
		filecaps_free(&rc->caps);
		fdrop(rc->fp, curthread);
	}
	free(rc, M_KQRING);
}

/*
 * Queue rc on ring tctx for its owner to post (and for SEND_FD, first
 * install its file), and wake the owner.
 */
static int
kqring_rcqe_queue(struct kqring_ctx *tctx, struct kqring_rcqe *rc)
{
	int error = 0;

	mtx_lock(&tctx->mtx);
	if (!tctx->setup_done || !tctx->enabled || tctx->closing) {
		error = EBADF;
	} else if (tctx->nrcq >= KQRING_MAX_REMOTE) {
		error = EOVERFLOW;
	} else {
		if (STAILQ_EMPTY(&tctx->rcq))
			kqring_evfd_signal_locked(tctx);
		STAILQ_INSERT_TAIL(&tctx->rcq, rc, link);
		tctx->nrcq++;
		atomic_set_32(&tctx->shm->sq_flags, IORING_SQ_TASKRUN);
		if (tctx->sleeping)
			wakeup(&tctx->wakeup_chan);
	}
	mtx_unlock(&tctx->mtx);
	return (error);
}

/*
 * MSG_RING: post a CQE to the ring sqe->fd (IORING_MSG_DATA, possibly this
 * ring), or pass it our fixed file sqe->addr3 for its fixed slot
 * sqe->file_index (IORING_MSG_SEND_FD).  Both are only queued on the
 * target, whose owner posts the CQE, and installs the file, from its own
 * taskwork: no thread ever touches another ring's CQ or file table.  So a
 * SEND_FD that the target cannot install (no table, bad slot) reports the
 * error in the target's CQE, where io_uring reports it to the sender.
 * Holding the target's file keeps its context alive.
 */
int32_t
kqring_msg_ring(struct kqring_ctx *ctx, const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct kqring_ctx *tctx;
	struct kqring_rcqe *rc;
	struct kqring_rsrc *src;
	struct file *fp;
	cap_rights_t rights;
	int error;

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_IOCTL), 0,
	    &fp);
	if (error != 0)
		return (-error);
	error = kqring_fp_ctx(fp, &tctx);
	if (error != 0) {
		fdrop(fp, td);
		return (-error);
	}

	rc = malloc(sizeof(*rc), M_KQRING, M_WAITOK | M_ZERO);
	rc->cqe.user_data = sqe->off;
	if (sqe->addr == IORING_MSG_SEND_FD) {
		if (tctx == ctx) {
			error = EINVAL;
		} else if (ctx->files == NULL || sqe->addr3 >= ctx->nfiles ||
		    (src = ctx->files[sqe->addr3]) == NULL) {
			error = EBADF;
		} else if (!fhold(src->fp)) {
			error = EBADF;
		} else {
			rc->fp = src->fp;
			(void)filecaps_copy(&src->caps, &rc->caps, false);
			rc->file_index = sqe->file_index;
			rc->skip_cqe = (sqe->msg_ring_flags &
			    IORING_MSG_RING_CQE_SKIP) != 0;
		}
	} else {
		rc->cqe.res = (int32_t)sqe->len;
		rc->cqe.flags = (sqe->msg_ring_flags &
		    IORING_MSG_RING_FLAGS_PASS) != 0 ? sqe->file_index : 0;
		if ((ctx->flags & IORING_SETUP_SQE128) != 0) {
			memcpy(rc->extra, op->sqe_extra, sizeof(rc->extra));
			rc->has_extra = true;
		}
	}
	if (error == 0)
		error = kqring_rcqe_queue(tctx, rc);
	if (error != 0)
		kqring_rcqe_free(rc);
	fdrop(fp, td);
	return (-error);
}

/* ============================================================================
 * Timeouts (TIMEOUT, LINK_TIMEOUT, TIMEOUT_REMOVE)
 * ============================================================================
 */

int
kqring_copyin_ts(uint64_t uaddr, struct timespec *ts)
{
	struct __kernel_timespec kts;
	int error;

	error = copyin((const void *)(uintptr_t)uaddr, &kts, sizeof(kts));
	if (error != 0)
		return (error);
	if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000LL)
		return (EINVAL);
	ts->tv_sec = kts.tv_sec;
	ts->tv_nsec = (long)kts.tv_nsec;
	return (0);
}

sbintime_t
kqring_ts_to_sbt(const struct timespec *ts)
{
	struct timespec t = *ts;

	/* sbintime_t holds 31 bits of seconds. */
	if (t.tv_sec > INT32_MAX)
		t.tv_sec = INT32_MAX;
	return (tstosbt(t));
}

/* Time left until op's timeout, from its timespec and timeout_flags. */
static sbintime_t
kqring_timeout_rel(const struct kqring_op *op)
{
	struct timespec now, rel;
	uint32_t tflags = op->sqe.timeout_flags;

	if ((tflags & IORING_TIMEOUT_ABS) == 0)
		return (kqring_ts_to_sbt(&op->ts));
	if ((tflags & IORING_TIMEOUT_REALTIME) != 0)
		nanotime(&now);
	else
		nanouptime(&now);
	if (timespeccmp(&op->ts, &now, <=))
		return (0);
	timespecsub(&op->ts, &now, &rel);
	return (kqring_ts_to_sbt(&rel));
}

static void
kqring_timer_start_locked(struct kqring_op *op)
{
	mtx_assert(&op->ctx->mtx, MA_OWNED);
	if (!op->timer_init) {
		callout_init_mtx(&op->callout, &op->ctx->mtx, 0);
		op->timer_init = true;
	}
	callout_reset_sbt(&op->callout, kqring_timeout_rel(op), 0,
	    kqring_timer_cb, op, 0);
	op->timer_pending = true;
}

int
kqring_timeout_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	uint32_t tflags = sqe->timeout_flags;

	if (sqe->buf_index != 0 || sqe->len != 1 || sqe->splice_fd_in != 0)
		return (EINVAL);
	if ((tflags & ~(IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK |
	    IORING_TIMEOUT_ETIME_SUCCESS | IORING_TIMEOUT_MULTISHOT)) != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_CLOCK_MASK) == IORING_TIMEOUT_CLOCK_MASK)
		return (EINVAL);
	/* A multishot timeout repeats a relative interval. */
	if ((tflags & IORING_TIMEOUT_MULTISHOT) != 0 &&
	    (tflags & IORING_TIMEOUT_ABS) != 0)
		return (EINVAL);
	if (sqe->opcode == IORING_OP_LINK_TIMEOUT &&
	    (sqe->off != 0 || (tflags & IORING_TIMEOUT_MULTISHOT) != 0))
		return (EINVAL);
	return (kqring_copyin_ts(sqe->addr, &op->ts));
}

int
kqring_timeout_remove_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	uint32_t tflags = sqe->timeout_flags;

	if (sqe->buf_index != 0 || sqe->len != 0 || sqe->splice_fd_in != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) == 0)
		return (tflags != 0 ? EINVAL : 0);
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) == IORING_TIMEOUT_UPDATE_MASK)
		return (EINVAL);
	if ((tflags & ~(IORING_TIMEOUT_UPDATE_MASK | IORING_TIMEOUT_ABS |
	    IORING_TIMEOUT_CLOCK_MASK)) != 0)
		return (EINVAL);
	if ((tflags & IORING_TIMEOUT_CLOCK_MASK) == IORING_TIMEOUT_CLOCK_MASK)
		return (EINVAL);
	return (kqring_copyin_ts(sqe->addr2, &op->ts));
}

/*
 * Arm a TIMEOUT.  With off != 0 (and no MULTISHOT) it also completes, with
 * res 0, once `off` more CQEs have been posted; for MULTISHOT, off is the
 * number of expirations (0: forever).
 */
void
kqring_timeout_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	mtx_lock(&ctx->mtx);
	op->cancelled = false;
	if ((op->sqe.timeout_flags & IORING_TIMEOUT_MULTISHOT) != 0) {
		op->repeats = (uint32_t)op->sqe.off;
	} else if (op->sqe.off != 0) {
		op->target_seq = ctx->cq_seq + (uint32_t)op->sqe.off;
		op->seq_timeout = true;
		ctx->nseqto++;
	}
	kqring_op_arm_locked(ctx, op);
	kqring_timer_start_locked(op);
	mtx_unlock(&ctx->mtx);
}

/*
 * Start the LINK_TIMEOUT that follows op in its chain, now that op is
 * armed.  It moves out of the chain into op->ltimeout; op's successors are
 * those that followed it.
 */
void
kqring_ltimeout_arm(struct kqring_ctx *ctx, struct kqring_op *op)
{
	struct kqring_op *lt = op->link;

	if (lt == NULL || lt->sqe.opcode != IORING_OP_LINK_TIMEOUT)
		return;
	op->link = lt->link;
	lt->link = NULL;
	op->ltimeout = lt;
	lt->ltarget = op;
	mtx_lock(&ctx->mtx);
	kqring_timer_start_locked(lt);
	mtx_unlock(&ctx->mtx);
}

/* A timer expired (run as taskwork). */
void
kqring_timeout_fire(struct kqring_ctx *ctx, struct kqring_op *op)
{
	if (op->sqe.opcode == IORING_OP_LINK_TIMEOUT) {
		struct kqring_op *target = op->ltarget;

		/*
		 * Detach first (completing target would cancel us), then post
		 * -ETIME before target's -ECANCELED and its chain's, as
		 * io_uring does.
		 */
		mtx_lock(&ctx->mtx);
		target->ltimeout = NULL;
		op->ltarget = NULL;
		kqring_op_unarm_locked(ctx, target);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, -ETIME, 0);
		kqring_req_complete(ctx, target, -ECANCELED, 0);
		return;
	}
	if ((op->sqe.timeout_flags & IORING_TIMEOUT_MULTISHOT) != 0 &&
	    (op->repeats == 0 || --op->repeats > 0)) {
		kqring_post_cqe_uncounted(ctx, op->sqe.user_data, -ETIME,
		    IORING_CQE_F_MORE);
		mtx_lock(&ctx->mtx);
		kqring_timer_start_locked(op);
		mtx_unlock(&ctx->mtx);
		return;
	}
	mtx_lock(&ctx->mtx);
	kqring_op_unarm_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, op, -ETIME, 0);
}

/* TIMEOUT_REMOVE: cancel, or (UPDATE flags) re-arm, a pending timeout. */
int32_t
kqring_timeout_remove(struct kqring_ctx *ctx, const struct kqring_op *rop)
{
	const struct io_uring_sqe *sqe = &rop->sqe;
	uint32_t tflags = sqe->timeout_flags;
	struct kqring_op *op, *found = NULL;

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
		if ((tflags & IORING_LINK_TIMEOUT_UPDATE) != 0) {
			if (op->ltimeout != NULL &&
			    op->ltimeout->sqe.user_data == sqe->addr) {
				found = op->ltimeout;
				break;
			}
		} else if (op->sqe.opcode == IORING_OP_TIMEOUT &&
		    !op->cancelled && op->sqe.user_data == sqe->addr) {
			found = op;
			break;
		}
	}
	if (found == NULL) {
		mtx_unlock(&ctx->mtx);
		return (-ENOENT);
	}
	if ((tflags & IORING_TIMEOUT_UPDATE_MASK) != 0) {
		/* Already expired: its completion is queued. */
		if (found->in_taskwork) {
			mtx_unlock(&ctx->mtx);
			return (-EALREADY);
		}
		found->ts = rop->ts;
		found->sqe.timeout_flags = (found->sqe.timeout_flags &
		    ~(IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK)) |
		    (tflags & (IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK));
		kqring_timer_start_locked(found);
		mtx_unlock(&ctx->mtx);
		return (0);
	}
	kqring_op_unarm_locked(ctx, found);
	mtx_unlock(&ctx->mtx);
	kqring_req_complete(ctx, found, -ECANCELED, 0);
	return (0);
}

/*
 * Complete TIMEOUTs whose completion count has been reached.  Only the
 * owner thread completes CQEs, so this runs after each batch of them.
 */
void
kqring_check_timeouts(struct kqring_ctx *ctx)
{
	struct kqring_op *op;

	while (ctx->nseqto > 0) {
		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (op->seq_timeout &&
			    (int32_t)(ctx->cq_seq - op->target_seq) >= 0)
				break;
		}
		if (op != NULL)
			kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		if (op == NULL)
			break;
		kqring_req_complete(ctx, op, 0, 0);
	}
}

/* ============================================================================
 * Cancellation (ASYNC_CANCEL, POLL_REMOVE)
 * ============================================================================
 */

/*
 * Whether armed request op matches a cancellation.  CANCEL_FD matches
 * requests on descriptor sqe->fd, or with CANCEL_FD_FIXED those on the
 * file in fixed slot sqe->fd (`cfp`); a fixed request's sqe->fd is a slot
 * number, never compared with a descriptor.
 */
static bool
kqring_cancel_match(const struct kqring_op *op, const struct io_uring_sqe *sqe,
    const struct file *cfp)
{
	uint32_t cflags = sqe->cancel_flags;

	if (op->cancelled)
		return (false);
	if ((cflags & IORING_ASYNC_CANCEL_ANY) != 0)
		return (true);
	if ((cflags & IORING_ASYNC_CANCEL_FD) != 0) {
		if (cfp != NULL ? op->ffile == NULL || op->ffile->fp != cfp :
		    op->ffile != NULL || op->sqe.fd != sqe->fd)
			return (false);
	}
	if ((cflags & IORING_ASYNC_CANCEL_OP) != 0 &&
	    op->sqe.opcode != sqe->len)
		return (false);
	/* user_data is matched unless only FD and/or OP were asked for. */
	if (((cflags & (IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_OP)) == 0 ||
	    (cflags & IORING_ASYNC_CANCEL_USERDATA) != 0) &&
	    op->sqe.user_data != sqe->addr)
		return (false);
	return (true);
}

/*
 * Cancel the first matching armed request, or with CANCEL_ALL / CANCEL_ANY
 * every one, returning how many.  Cancelled requests complete -ECANCELED
 * (breaking their link chains); requests not yet issued (linked, deferred)
 * are not found, as in io_uring.
 */
int32_t
kqring_cancel(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	bool all = (sqe->cancel_flags &
	    (IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY)) != 0;
	struct kqring_op *op;
	struct file *cfp = NULL;
	int32_t n = 0;

	if ((sqe->cancel_flags & IORING_ASYNC_CANCEL_FD_FIXED) != 0) {
		if (ctx->files == NULL || (uint32_t)sqe->fd >= ctx->nfiles ||
		    ctx->files[sqe->fd] == NULL)
			return (-EBADF);
		/* Only compared: the slot's reference is not needed. */
		cfp = ctx->files[sqe->fd]->fp;
	}
	for (;;) {
		mtx_lock(&ctx->mtx);
		TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
			if (kqring_cancel_match(op, sqe, cfp))
				break;
		}
		if (op != NULL)
			kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		if (op == NULL)
			break;
		kqring_req_complete(ctx, op, -ECANCELED, 0);
		n++;
		if (!all)
			break;
	}
	if (n == 0)
		return (-ENOENT);
	return (all ? n : 0);
}

/* POLL_REMOVE: cancel an armed POLL_ADD, or update its events/user_data. */
int32_t
kqring_poll_remove(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe)
{
	uint32_t pflags = sqe->len;
	struct kqring_op *op;
	bool retired = false;

	mtx_lock(&ctx->mtx);
	TAILQ_FOREACH(op, &ctx->active_ops, active_link) {
		if (op->sqe.opcode == IORING_OP_POLL_ADD && !op->cancelled &&
		    op->sqe.user_data == sqe->addr)
			break;
	}
	if (op != NULL) {
		kqring_op_unarm_locked(ctx, op);
		retired = op->wq_retired;
	}
	mtx_unlock(&ctx->mtx);
	if (op == NULL)
		return (-ENOENT);

	if ((pflags & (IORING_POLL_UPDATE_EVENTS |
	    IORING_POLL_UPDATE_USER_DATA)) == 0) {
		kqring_req_complete(ctx, op, -ECANCELED, 0);
		return (0);
	}
	if (retired) {
		kqring_req_complete(ctx, op, -ECANCELED, 0);
		return (-EALREADY);
	}
	/* Update: re-issue the same request with the new parameters. */
	if ((pflags & IORING_POLL_UPDATE_EVENTS) != 0) {
		op->sqe.poll32_events = sqe->poll32_events;
		op->sqe.len = pflags & (IORING_POLL_ADD_MULTI |
		    IORING_POLL_ADD_LEVEL);
	}
	if ((pflags & IORING_POLL_UPDATE_USER_DATA) != 0)
		op->sqe.user_data = sqe->off;
	kqring_req_issue(ctx, op);
	return (0);
}


/*
 * Complete armed FUTEX_WAIT(V) ops one of whose words no longer holds the
 * expected value (or can no longer be read), with that word's index.
 * Only the owner thread (this one) modifies active_ops, so it is walked
 * without ctx->mtx, which must not be held across copyin().
 */
void
kqring_check_futexes(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *tmp;
	struct kqring_fwait *fw;
	uint32_t i, uval;
	int32_t res;
	int error = 0;

	if (ctx->nfutex == 0)
		return;
	TAILQ_FOREACH_SAFE(op, &ctx->active_ops, active_link, tmp) {
		if (op->nfwait == 0 || op->cancelled)
			continue;
		fw = op->args;
		for (i = 0; i < op->nfwait; i++) {
			error = copyin((const void *)(uintptr_t)fw[i].uaddr,
			    &uval, sizeof(uval));
			if (error != 0 || uval != (uint32_t)fw[i].val)
				break;
		}
		if (i == op->nfwait)
			continue;
		/*
		 * Completion only queues link successors (never touches
		 * active_ops), so tmp stays valid.
		 */
		mtx_lock(&ctx->mtx);
		res = op->futex_woken ? op->res :
		    (error != 0 ? -error : (int32_t)i);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, res, 0);
	}
}

/* Complete armed WAITIDs whose child changed state. */
void
kqring_check_waitids(struct kqring_ctx *ctx)
{
	struct kqring_op *op, *tmp;

	if (ctx->nwaitid == 0)
		return;
	TAILQ_FOREACH_SAFE(op, &ctx->active_ops, active_link, tmp) {
		if (op->sqe.opcode != IORING_OP_WAITID || op->cancelled ||
		    op->wq_queued || op->wq_running || op->wq_done ||
		    kqring_exec_waitid(op, true) == EAGAIN)
			continue;
		mtx_lock(&ctx->mtx);
		kqring_op_unarm_locked(ctx, op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, op->res, 0);
	}
}
