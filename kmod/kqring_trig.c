/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

/*
 * Event-driven chains (KQRING_EXT_TRIGGER; docs/event-chains-spec.md).
 *
 * A trigger waits on an internal POLL_ADD (its "waiter"), whose results
 * come here instead of to the CQ.  Each event runs the trigger's template
 * SQEs as a new chain, queued as a submission would be (a "run").  SERIAL
 * triggers wait with a one-shot poll and re-arm once their run is done;
 * CONCURRENT ones keep a multishot poll armed and start a run per event,
 * up to max_inflight.
 *
 * Everything here runs on the ring owner thread, like completions.  A
 * trigger is referenced by ctx->triggers (until its final CQE), by its
 * waiter, by each run, and while on ctx->trig_work; it is freed when the
 * last reference goes, which during ring teardown can be after
 * kqring_trig_teardown().  Work that would issue requests (re-arming,
 * a held-back run, stopping the waiter, the final CQE) is done from
 * kqring_flush_ready() through ctx->trig_work, never from inside a
 * completion or kqring_op_free().
 */

#define	KQRING_STEP_ALL	(KQRING_STEP_LEN_FROM_PREV | KQRING_STEP_STOP_ON_ZERO)

/* Most triggers alive on one ring. */
#define	KQRING_TRIG_MAX		1024U

/*
 * Firings in a row before a SERIAL re-arm goes through taskwork
 * (IOSQE_ASYNC) instead of completing at once: an event that stays
 * pending can't keep one enter in the kernel for ever.
 */
#define	KQRING_TRIG_BURST	16U

static void	kqring_trig_rele(struct kqring_trig *trig);

/* KQRING_IOC_EXT_SETUP: opt this ring in to kqring's extensions. */
int
kqring_ext_setup(struct kqring_ctx *ctx, struct kqring_ext_setup *es)
{
	uint8_t opcode;
	int error = 0;

	if (es->version != KQRING_EXT_VERSION || es->resv0[0] != 0 ||
	    es->resv0[1] != 0 || es->resv0[2] != 0 || es->resv[0] != 0 ||
	    es->resv[1] != 0)
		return (EINVAL);
	opcode = es->ext_opcode != 0 ? es->ext_opcode :
	    KQRING_EXT_OPCODE_DEFAULT;
	/* Never an opcode this kernel implements as io_uring's. */
	if (opcode < IORING_OP_LAST)
		return (EINVAL);
	sx_xlock(&ctx->sq_sx);
	if (!ctx->setup_done)
		error = EINVAL;
	else if (ctx->ext_on)
		/* Once per ring: stored templates name the escape opcode. */
		error = EBUSY;
	else {
		ctx->ext_opcode = opcode;
		ctx->ext_on = true;
	}
	sx_xunlock(&ctx->sq_sx);
	if (error != 0)
		return (error);
	es->ext_opcode = opcode;
	es->features = KQRING_EXT_F_TRIGGER;
	return (0);
}

/* Prep of the trigger SQE: copy in and check its struct kqring_trigger. */
int
kqring_trig_prep(struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_trigger *ta;
	bool concurrent;
	int error;

	if (sqe->len != sizeof(*ta) || sqe->addr == 0 || sqe->off != 0 ||
	    sqe->buf_index != 0 || sqe->file_index != 0)
		return (EINVAL);
	/* The templates are what it links to; it has no CQE to skip. */
	if ((sqe->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) == 0 ||
	    (sqe->flags & (IOSQE_IO_DRAIN | IOSQE_ASYNC |
	    IOSQE_CQE_SKIP_SUCCESS)) != 0)
		return (EINVAL);
	ta = kqring_args_alloc(op, sizeof(*ta), &error);
	if (ta == NULL)
		return (error);
	error = copyin((const void *)(uintptr_t)sqe->addr, ta, sizeof(*ta));
	if (error != 0)
		return (error);
	concurrent = (ta->flags & KQRING_TRIG_CONCURRENT) != 0;
	if ((ta->flags & ~(KQRING_TRIG_CONCURRENT | KQRING_TRIG_CQE_EACH |
	    KQRING_TRIG_STOP_ON_ERROR)) != 0 ||
	    ta->source != KQRING_TRIG_SRC_POLL ||
	    ta->nsteps == 0 || ta->nsteps > KQRING_TRIG_MAX_STEPS ||
	    (concurrent ? ta->max_inflight == 0 ||
	    ta->max_inflight > KQRING_TRIG_MAX_INFLIGHT :
	    ta->max_inflight != 0) ||
	    ta->resv[0] != 0 || ta->resv[1] != 0)
		return (EINVAL);
	/* Only steps with a step before them take step flags. */
	for (int i = 0; i < KQRING_TRIG_MAX_STEPS; i++) {
		if ((ta->step_flags[i] & ~KQRING_STEP_ALL) != 0 ||
		    ((i == 0 || i >= ta->nsteps) && ta->step_flags[i] != 0))
			return (EINVAL);
	}
	return (0);
}

static void
kqring_trig_queue_work(struct kqring_trig *trig)
{
	struct kqring_ctx *ctx = trig->ctx;

	if (trig->on_work || ctx->closing)
		return;
	trig->on_work = true;
	trig->refs++;
	TAILQ_INSERT_TAIL(&ctx->trig_work, trig, work_link);
}

/* No more runs: the final CQE once the waiter and every run are done. */
static void
kqring_trig_end(struct kqring_trig *trig, int32_t res)
{
	if (trig->ending)
		return;
	trig->ending = true;
	trig->end_res = res;
	kqring_trig_queue_work(trig);
}

/* Issue the trigger's poll; it reports through kqring_trig_waiter_event(). */
static void
kqring_trig_arm(struct kqring_trig *trig)
{
	struct kqring_ctx *ctx = trig->ctx;
	struct kqring_op *w;
	int error;

	KASSERT(trig->waiter == NULL, ("kqring: trigger armed twice"));
	if (ctx->nreqs >= KQRING_MAX_REQS) {
		kqring_trig_end(trig, -EBUSY);
		return;
	}
	w = kqring_op_alloc(ctx);
	w->sqe = trig->wsqe;
	if ((trig->args.flags & KQRING_TRIG_CONCURRENT) == 0 &&
	    trig->runs != 0 && trig->runs % KQRING_TRIG_BURST == 0)
		w->sqe.flags |= IOSQE_ASYNC;
	error = kqring_req_prep(w, NULL);
	if (error != 0) {
		kqring_op_free(w);
		kqring_trig_end(trig, -error);
		return;
	}
	w->trig_wait = trig;
	trig->waiter = w;
	trig->refs++;
	w->nchain = 1;
	kqring_req_issue(ctx, w);
}

/* Start one run: a new chain built from the templates. */
static void
kqring_trig_run(struct kqring_trig *trig)
{
	struct kqring_ctx *ctx = trig->ctx;
	struct kqring_op *head = NULL, *prev = NULL, *o;
	struct kqring_trun *tr;
	int error = 0;

	if (ctx->nreqs + trig->args.nsteps > KQRING_MAX_REQS) {
		kqring_trig_end(trig, -EBUSY);
		return;
	}
	tr = malloc(sizeof(*tr), M_KQRING, M_WAITOK | M_ZERO);
	tr->trig = trig;
	trig->refs++;
	trig->inflight++;
	trig->runs++;
	for (u_int i = 0; i < trig->args.nsteps; i++) {
		o = kqring_op_alloc(ctx);
		o->sqe = trig->tmpl[i];
		memcpy(o->sqe_extra, trig->tmpl_extra[i], sizeof(o->sqe_extra));
		o->trun = tr;
		o->tstep = trig->args.step_flags[i];
		tr->refs++;
		/* Checked again each run: fixed files and buffers can change. */
		o->prep_err = kqring_req_prep(o, prev);
		if (o->prep_err != 0 && error == 0)
			error = o->prep_err;
		if (head == NULL)
			head = o;
		else
			prev->link = o;
		prev = o;
		head->nchain++;
	}
	if (error != 0) {
		/* Each request posts its error or -ECANCELED, as at submit. */
		kqring_trun_failed(tr, -error);
		kqring_chain_fail(ctx, head, false);
		return;
	}
	kqring_chain_queue(ctx, head);
}

/* The waiter saw the event (revents in res). */
static void
kqring_trig_fire(struct kqring_trig *trig, int32_t res)
{
	struct kqring_ctx *ctx = trig->ctx;

	if (trig->ending)
		return;
	if ((res & POLLNVAL) != 0) {
		kqring_trig_end(trig, -EBADF);
		return;
	}
	/* As a multishot request: stop rather than overflow the CQ. */
	if (kqring_cq_full(ctx)) {
		kqring_trig_end(trig, -EOVERFLOW);
		return;
	}
	if ((trig->args.flags & KQRING_TRIG_CQE_EACH) != 0)
		kqring_post_cqe(ctx, trig->user_data, res, IORING_CQE_F_MORE);
	if ((trig->args.flags & KQRING_TRIG_CONCURRENT) != 0 &&
	    trig->inflight >= trig->args.max_inflight)
		trig->pending = true;
	else
		kqring_trig_run(trig);
	/* A hangup, end of input or error is the last event: run it, end. */
	if ((res & (POLLHUP | POLLRDHUP | POLLERR)) != 0)
		kqring_trig_end(trig, 0);
	else if ((trig->args.flags & KQRING_TRIG_CONCURRENT) == 0)
		trig->want_arm = true;
}

/*
 * A result of the trigger's poll: an event of the multishot (CONCURRENT)
 * poll, which stays armed (more), or its completion.  Also called from
 * kqring_op_free() for a poll freed without completing (teardown).
 */
void
kqring_trig_waiter_event(struct kqring_ctx *ctx, struct kqring_op *op,
    int32_t res, bool more)
{
	struct kqring_trig *trig = op->trig_wait;

	KASSERT(trig != NULL && trig->waiter == op,
	    ("kqring: event of a stale trigger poll"));
	if (more) {
		if (!ctx->closing)
			kqring_trig_fire(trig, res);
		return;
	}
	op->trig_wait = NULL;
	trig->waiter = NULL;
	if (!ctx->closing) {
		if (res < 0)
			kqring_trig_end(trig, res);
		else if ((trig->args.flags & KQRING_TRIG_CONCURRENT) != 0)
			/* The multishot poll ended: the CQ was full. */
			kqring_trig_end(trig, -EOVERFLOW);
		else
			kqring_trig_fire(trig, res);
		kqring_trig_queue_work(trig);
	}
	kqring_trig_rele(trig);
}

/* A request of the run failed (res < 0): the first failure is the run's. */
void
kqring_trun_failed(struct kqring_trun *tr, int32_t res)
{
	if (!tr->failed) {
		tr->failed = true;
		tr->err = res;
	}
}

/* Whether LEN_FROM_PREV can set this opcode's len. */
static bool
kqring_step_len_ok(uint8_t opcode)
{
	switch (opcode) {
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_SEND:
	case IORING_OP_SEND_ZC:
	case IORING_OP_RECV:
	case IORING_OP_SPLICE:
	case IORING_OP_TEE:
		return (true);
	default:
		return (false);
	}
}

/*
 * From kqring_req_complete(): op of a run completed with res and next is
 * the step after it (not yet issued).  Apply next's step flags.  Returns
 * false if the rest of the run is to be skipped (STOP_ON_ZERO).
 */
bool
kqring_trun_step(struct kqring_op *op, struct kqring_op *next, int32_t res)
{
	struct kqring_trun *tr = op->trun;

	if (next->trun != tr || next->tstep == 0)
		return (true);
	if ((next->tstep & KQRING_STEP_STOP_ON_ZERO) != 0 && res == 0) {
		tr->stopped = true;
		return (false);
	}
	/* Only shrinks len, so what prep checked of it still holds. */
	if ((next->tstep & KQRING_STEP_LEN_FROM_PREV) != 0)
		next->sqe.len = res < 0 ? 0 : MIN((uint32_t)res, next->sqe.len);
	return (true);
}

/* A request of the run was freed; the last one ends the run. */
void
kqring_trun_put(struct kqring_trun *tr)
{
	struct kqring_trig *trig = tr->trig;
	struct kqring_ctx *ctx = trig->ctx;

	KASSERT(tr->refs > 0, ("kqring: trigger run over-released"));
	if (--tr->refs > 0)
		return;
	KASSERT(trig->inflight > 0, ("kqring: trigger run count"));
	trig->inflight--;
	if (!ctx->closing) {
		if (tr->failed &&
		    (trig->args.flags & KQRING_TRIG_STOP_ON_ERROR) != 0)
			kqring_trig_end(trig, tr->err);
		else if (tr->stopped)
			kqring_trig_end(trig, 0);
		kqring_trig_queue_work(trig);
	}
	free(tr, M_KQRING);
	kqring_trig_rele(trig);
}

/* Stop the trigger's poll, if it is armed. */
static void
kqring_trig_stop_waiter(struct kqring_trig *trig)
{
	struct kqring_ctx *ctx = trig->ctx;
	struct kqring_op *w = trig->waiter;
	bool armed;

	if (w == NULL)
		return;
	mtx_lock(&ctx->mtx);
	armed = w->armed && !w->cancelled;
	if (armed)
		kqring_op_unarm_locked(ctx, w);
	mtx_unlock(&ctx->mtx);
	/* Completing it reports to kqring_trig_waiter_event() (ending). */
	if (armed)
		kqring_req_complete(ctx, w, -ECANCELED, 0);
}

/* Act on one trigger taken off ctx->trig_work. */
static void
kqring_trig_step(struct kqring_trig *trig)
{
	struct kqring_ctx *ctx = trig->ctx;

	if (trig->ending) {
		kqring_trig_stop_waiter(trig);
		if (trig->waiter != NULL || trig->inflight != 0 ||
		    trig->posted)
			return;
		/* Last: after every CQE of its runs. */
		trig->posted = true;
		kqring_post_cqe(ctx, trig->user_data, trig->end_res, 0);
		LIST_REMOVE(trig, link);
		trig->listed = false;
		ctx->ntriggers--;
		kqring_trig_rele(trig);
		return;
	}
	if ((trig->args.flags & KQRING_TRIG_CONCURRENT) != 0) {
		if (trig->pending && trig->inflight < trig->args.max_inflight) {
			trig->pending = false;
			kqring_trig_run(trig);
		}
		return;
	}
	if (trig->want_arm && trig->inflight == 0 && trig->waiter == NULL) {
		trig->want_arm = false;
		kqring_trig_arm(trig);
	}
}

/* From kqring_flush_ready(): the triggers with work queued now. */
void
kqring_trig_work(struct kqring_ctx *ctx)
{
	TAILQ_HEAD(, kqring_trig) q = TAILQ_HEAD_INITIALIZER(q);
	struct kqring_trig *trig;

	/* Work queued while stepping waits for the next call. */
	TAILQ_CONCAT(&q, &ctx->trig_work, work_link);
	while ((trig = TAILQ_FIRST(&q)) != NULL) {
		TAILQ_REMOVE(&q, trig, work_link);
		trig->on_work = false;
		if (!ctx->closing)
			kqring_trig_step(trig);
		kqring_trig_rele(trig);
	}
}

static void
kqring_trig_rele(struct kqring_trig *trig)
{
	KASSERT(trig->refs > 0, ("kqring: trigger over-released"));
	if (--trig->refs > 0)
		return;
	KASSERT(!trig->listed && !trig->on_work && trig->waiter == NULL &&
	    trig->inflight == 0, ("kqring: freeing a live trigger"));
	free(trig, M_KQRING);
}

/*
 * Issue of the trigger SQE: check its templates (the requests linked to
 * it), keep their SQEs and free them unissued, then arm.  The trigger
 * itself posts no CQE until it ends.
 */
void
kqring_trig_issue(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct kqring_trigger *ta = op->args;
	struct kqring_op *t, *tmpl = op->link;
	struct kqring_trig *trig;
	int32_t err = 0;
	u_int n = 0;

	for (t = tmpl; t != NULL; t = t->link) {
		/* Nested triggers, multishot, drain; LINK_TIMEOUT guards a step. */
		if (t->sqe.opcode == KQRING_IOP_TRIGGER || t->multishot ||
		    (t->sqe.flags & IOSQE_IO_DRAIN) != 0 ||
		    (n == 0 && t->sqe.opcode == IORING_OP_LINK_TIMEOUT))
			err = -EINVAL;
		/* A step flag names a step that a result is passed to. */
		if (n < KQRING_TRIG_MAX_STEPS && ta->step_flags[n] != 0 &&
		    (t->sqe.opcode == IORING_OP_LINK_TIMEOUT ||
		    ((ta->step_flags[n] & KQRING_STEP_LEN_FROM_PREV) != 0 &&
		    !kqring_step_len_ok(t->sqe.opcode))))
			err = -EINVAL;
		n++;
	}
	if (n != ta->nsteps)
		err = -EINVAL;
	if (err == 0 && ctx->ntriggers >= KQRING_TRIG_MAX)
		err = -EBUSY;
	if (err != 0) {
		/* The templates never run (even behind IOSQE_IO_HARDLINK). */
		op->link = NULL;
		kqring_chain_fail(ctx, tmpl, false);
		kqring_req_complete(ctx, op, err, 0);
		return;
	}

	trig = malloc(sizeof(*trig), M_KQRING, M_WAITOK | M_ZERO);
	trig->ctx = ctx;
	trig->user_data = op->sqe.user_data;
	trig->args = *ta;
	trig->wsqe.opcode = IORING_OP_POLL_ADD;
	trig->wsqe.flags = op->sqe.flags & IOSQE_FIXED_FILE;
	trig->wsqe.fd = op->sqe.fd;
	trig->wsqe.personality = op->sqe.personality;
	trig->wsqe.user_data = op->sqe.user_data;
	trig->wsqe.poll32_events = op->sqe.poll32_events;
	/*
	 * Input that has ended (the peer shut down writing) stays readable
	 * for ever: watch for it, so that it ends the trigger.
	 */
	if ((op->sqe.poll32_events & (POLLIN | POLLRDNORM)) != 0)
		trig->wsqe.poll32_events |= POLLRDHUP;
	if ((ta->flags & KQRING_TRIG_CONCURRENT) != 0)
		trig->wsqe.len = IORING_POLL_ADD_MULTI;
	n = 0;
	for (t = tmpl; t != NULL; t = t->link, n++) {
		trig->tmpl[n] = t->sqe;
		memcpy(trig->tmpl_extra[n], t->sqe_extra,
		    sizeof(trig->tmpl_extra[n]));
	}
	op->link = NULL;
	kqring_req_destroy(ctx, tmpl);
	kqring_op_free(op);

	trig->refs = 1;			/* ctx->triggers */
	trig->listed = true;
	LIST_INSERT_HEAD(&ctx->triggers, trig, link);
	ctx->ntriggers++;
	kqring_trig_arm(trig);
}

/*
 * ASYNC_CANCEL of triggers that have no poll armed (a SERIAL trigger
 * whose run is in flight); those with one are cancelled through it.
 * Matched as kqring_cancel_match_req() matches a request; never by
 * opcode.  Returns how many were ended.
 */
int32_t
kqring_trig_cancel(struct kqring_ctx *ctx, const struct io_uring_sqe *sqe,
    const struct file *cfp, bool all)
{
	uint32_t cflags = sqe->cancel_flags;
	struct kqring_trig *trig;
	int32_t n = 0;

	LIST_FOREACH(trig, &ctx->triggers, link) {
		bool fixed = (trig->wsqe.flags & IOSQE_FIXED_FILE) != 0;

		if (trig->ending || trig->waiter != NULL)
			continue;
		if ((cflags & IORING_ASYNC_CANCEL_ANY) == 0) {
			if ((cflags & IORING_ASYNC_CANCEL_OP) != 0)
				continue;
			if ((cflags & IORING_ASYNC_CANCEL_FD) != 0) {
				if (cfp != NULL ? !fixed ||
				    (uint32_t)trig->wsqe.fd >= ctx->nfiles ||
				    ctx->files[trig->wsqe.fd] == NULL ||
				    ctx->files[trig->wsqe.fd]->fp != cfp :
				    fixed || trig->wsqe.fd != sqe->fd)
					continue;
			}
			if (((cflags & IORING_ASYNC_CANCEL_FD) == 0 ||
			    (cflags & IORING_ASYNC_CANCEL_USERDATA) != 0) &&
			    trig->user_data != sqe->addr)
				continue;
		}
		kqring_trig_end(trig, -ECANCELED);
		n++;
		if (!all)
			break;
	}
	return (n);
}

/*
 * Ring teardown, after every request is gone: drop the triggers.  One
 * still referenced by a request freed later (a retired worker's) goes
 * with it.
 */
void
kqring_trig_teardown(struct kqring_ctx *ctx)
{
	struct kqring_trig *trig;

	while ((trig = TAILQ_FIRST(&ctx->trig_work)) != NULL) {
		TAILQ_REMOVE(&ctx->trig_work, trig, work_link);
		trig->on_work = false;
		kqring_trig_rele(trig);
	}
	while ((trig = LIST_FIRST(&ctx->triggers)) != NULL) {
		LIST_REMOVE(trig, link);
		trig->listed = false;
		ctx->ntriggers--;
		kqring_trig_rele(trig);
	}
}
