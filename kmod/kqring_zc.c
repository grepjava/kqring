/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

#include "kqring_var.h"

static LIST_HEAD(, kqring_pin_acct) kqring_pin_list =
    LIST_HEAD_INITIALIZER(kqring_pin_list);

struct mtx kqring_zc_mtx;

SYSCTL_NODE(_kern, OID_AUTO, kqring, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "kqring io_uring rings");

u_int kqring_zc_local_min_len = KQRING_ZC_LOCAL_MIN_LEN;
SYSCTL_UINT(_kern_kqring, OID_AUTO, zc_local_min_len, CTLFLAG_RWTUN,
    &kqring_zc_local_min_len, 0,
    "Smallest AF_LOCAL SEND_ZC / SENDMSG_ZC from an unregistered buffer "
    "sent zero-copy (bytes, at least one page); smaller ones copy");

static void kqring_zc_rele_hold(void);

static int
kqring_pin_charge(struct thread *td, size_t npages, size_t discount_pages,
    struct kqring_pin_acct **acctp)
{
	struct uidinfo *uip = td->td_ucred->cr_ruidinfo;
	struct kqring_pin_acct *acct, *alloc_acct;
	rlim_t bytes = ptoa((rlim_t)npages);
	rlim_t discount = ptoa((rlim_t)discount_pages);
	rlim_t limit = lim_cur(td, RLIMIT_MEMLOCK);
	rlim_t cur_bytes;
	u_long wired, max_wired;

	if (npages == 0 || npages > KQRING_MAX_ZC_PAGES)
		return (ENOMEM);
	alloc_acct = malloc(sizeof(*alloc_acct), M_KQRING, M_WAITOK | M_ZERO);
	mtx_lock(&kqring_zc_mtx);
	LIST_FOREACH(acct, &kqring_pin_list, link) {
		if (acct->uip == uip)
			break;
	}
	cur_bytes = acct != NULL && acct->bytes > discount ?
	    acct->bytes - discount : 0;
	if (bytes > limit || cur_bytes > limit - bytes) {
		mtx_unlock(&kqring_zc_mtx);
		free(alloc_acct, M_KQRING);
		return (ENOMEM);
	}
	wired = atomic_load_long(&vm_user_wire_count);
	for (;;) {
		u_long eff_wired = wired > discount_pages ?
		    wired - discount_pages : 0;

		max_wired = vm_page_max_user_wired;
		if (npages > max_wired || eff_wired > max_wired - npages) {
			mtx_unlock(&kqring_zc_mtx);
			free(alloc_acct, M_KQRING);
			return (ENOMEM);
		}
		if (atomic_fcmpset_long(&vm_user_wire_count, &wired,
		    wired + npages))
			break;
	}
	if (acct == NULL) {
		acct = alloc_acct;
		alloc_acct = NULL;
		uihold(uip);
		acct->uip = uip;
		LIST_INSERT_HEAD(&kqring_pin_list, acct, link);
	}
	acct->bytes += bytes;
	acct->refs++;
	mtx_unlock(&kqring_zc_mtx);
	free(alloc_acct, M_KQRING);
	*acctp = acct;
	return (0);
}

static void
kqring_pin_uncharge(struct kqring_pin_acct *acct, size_t npages)
{
	rlim_t bytes = ptoa((rlim_t)npages);
	struct uidinfo *uip = NULL;

	atomic_subtract_long(&vm_user_wire_count, npages);
	mtx_lock(&kqring_zc_mtx);
	KASSERT(acct->bytes >= bytes && acct->refs > 0,
	    ("kqring: pin acct underflow"));
	acct->bytes -= bytes;
	if (--acct->refs == 0) {
		KASSERT(acct->bytes == 0,
		    ("kqring: pin acct nonzero bytes at zero refs"));
		LIST_REMOVE(acct, link);
		uip = acct->uip;
	} else {
		acct = NULL;
	}
	mtx_unlock(&kqring_zc_mtx);
	if (acct != NULL) {
		uifree(uip);
		free(acct, M_KQRING);
	}
}

int
kqring_zc_buf_alloc(struct thread *td, vm_offset_t start, size_t len,
    vm_prot_t prot, size_t discount_pages, struct kqring_zc_buf **zbp)
{
	vm_map_t map = &td->td_proc->p_vmspace->vm_map;
	struct kqring_pin_acct *acct;
	struct kqring_zc_buf *zb;
	vm_offset_t page_base, page_end;
	size_t npages;
	int error, held;

	if (start == 0 || len == 0 || start + len < start ||
	    start + len > vm_map_max(map))
		return (EFAULT);
	if (len > KQRING_MAX_ZC_BUF_LEN)
		return (ENOMEM);
	page_base = trunc_page(start);
	page_end = round_page(start + len);
	npages = atop(page_end - page_base);
	error = kqring_pin_charge(td, npages, discount_pages, &acct);
	if (error != 0)
		return (error);
	zb = malloc(sizeof(*zb) + npages * sizeof(vm_page_t), M_KQRING,
	    M_WAITOK | M_ZERO);
	held = vm_fault_quick_hold_pages(map, start, len, prot, zb->pages,
	    (int)npages);
	if (held < 0) {
		free(zb, M_KQRING);
		kqring_pin_uncharge(acct, npages);
		return (EFAULT);
	}
	refcount_init(&zb->refs, 1);
	zb->npages = held;
	zb->page_base = page_base;
	zb->acct = acct;
	*zbp = zb;
	return (0);
}

void
kqring_zc_buf_rele(struct kqring_zc_buf *zb)
{
	if (zb == NULL)
		return;
	if (refcount_release(&zb->refs)) {
		vm_page_unhold_pages(zb->pages, zb->npages);
		kqring_pin_uncharge(zb->acct, (size_t)zb->npages);
		free(zb, M_KQRING);
	}
}

void
kqring_zc_notif_free_bufs(struct kqring_zc_notif *zc)
{
	if (zc->zbuf != NULL) {
		kqring_zc_buf_rele(zc->zbuf);
		zc->zbuf = NULL;
	}
	if (zc->zbufs != NULL) {
		for (u_int i = 0; i < zc->nzbufs; i++) {
			if (zc->zbufs[i] != NULL)
				kqring_zc_buf_rele(zc->zbufs[i]);
		}
		free(zc->zbufs, M_KQRING);
		zc->zbufs = NULL;
		zc->nzbufs = 0;
	}
}

int
kqring_zc_refresh_cow(struct kqring_ctx *ctx, struct kqring_rsrc *r,
    struct thread *td)
{
	struct kqring_zc_buf *old_zb = r->zbuf, *new_zb;
	uint32_t cg = atomic_load_int(&ctx->cow_gen);
	size_t discount_pages = 0;
	int error;

	if (old_zb == NULL || r->cow_gen == cg)
		return (0);
	if (atomic_load_int(&old_zb->refs) == 1 &&
	    old_zb->acct->uip == td->td_ucred->cr_ruidinfo)
		discount_pages = (size_t)old_zb->npages;
	error = kqring_zc_buf_alloc(td, (vm_offset_t)r->addr, r->len,
	    VM_PROT_READ | VM_PROT_WRITE, discount_pages, &new_zb);
	if (error != 0)
		return (error == ENOMEM ? EOPNOTSUPP : error);
	r->zbuf = new_zb;
	r->cow_gen = cg;
	kqring_zc_buf_rele(old_zb);
	return (0);
}

void
kqring_zc_rele(struct kqring_zc_notif *zc, bool in_issuer)
{
	struct kqring_ctx *ctx;
	struct kqring_rcqe *rcqe;

	if (!refcount_release(&zc->refs))
		return;

	mtx_lock(&kqring_zc_mtx);
	ctx = zc->ctx;
	rcqe = zc->rcqe;
	zc->rcqe = NULL;
	if (ctx != NULL) {
		mtx_lock(&ctx->mtx);
		if (zc->on_ctx) {
			TAILQ_REMOVE(&ctx->zc_pending, zc, link);
			zc->on_ctx = false;
		}
		zc->ctx = NULL;
		if (!ctx->closing && rcqe != NULL && !in_issuer) {
			if (STAILQ_EMPTY(&ctx->rcq))
				kqring_evfd_signal_locked(ctx);
			STAILQ_INSERT_TAIL(&ctx->rcq, rcqe, link);
			ctx->nrcq++;
			atomic_set_32(&ctx->shm->sq_flags, IORING_SQ_TASKRUN);
			if (ctx->sleeping)
				wakeup(&ctx->wakeup_chan);
			if (ctx->sqpoll_waiting)
				wakeup(&ctx->sqpoll_wait_chan);
			rcqe = NULL;
		}
		mtx_unlock(&ctx->mtx);
	}
	mtx_unlock(&kqring_zc_mtx);

	if (in_issuer && ctx != NULL && !ctx->closing && rcqe != NULL)
		kqring_post_cqe(ctx, rcqe->cqe.user_data, rcqe->cqe.res,
		    rcqe->cqe.flags);
	if (rcqe != NULL)
		free(rcqe, M_KQRING);
	kqring_zc_notif_free_bufs(zc);
	free(zc, M_KQRING);
	kqring_zc_rele_hold();
}

void
kqring_zc_abort(struct kqring_zc_notif *zc)
{
	struct kqring_rcqe *rcqe;

	mtx_lock(&kqring_zc_mtx);
	rcqe = zc->rcqe;
	zc->rcqe = NULL;
	mtx_unlock(&kqring_zc_mtx);
	if (rcqe != NULL)
		free(rcqe, M_KQRING);
	kqring_zc_rele(zc, false);
}

void
kqring_zc_mext_free(struct mbuf *m)
{
	struct kqring_zc_notif *zc = m->m_ext.ext_arg1;
	struct sf_buf *sf = m->m_ext.ext_arg2;

	sf_buf_free(sf);
	kqring_zc_rele(zc, false);
}


bool
kqring_zc_hold(void)
{
	mtx_lock(&kqring_proc_mtx);
	if (kqring_unloading) {
		mtx_unlock(&kqring_proc_mtx);
		return (false);
	}
	kqring_live_zc++;
	kqring_live_workers++;
	mtx_unlock(&kqring_proc_mtx);
	return (true);
}

static void
kqring_zc_rele_hold(void)
{
	mtx_lock(&kqring_proc_mtx);
	KASSERT(kqring_live_zc > 0 && kqring_live_workers > 0,
	    ("kqring: zc hold count underflow"));
	if (--kqring_live_zc == 0)
		wakeup(&kqring_live_zc);
	if (--kqring_live_workers == 0)
		wakeup(&kqring_live_workers);
	mtx_unlock(&kqring_proc_mtx);
}
