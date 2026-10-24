/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The kqring Authors
 */

#include "kqring_var.h"

static int kqring_soupcall(struct socket *so, void *arg, int waitflag);

/* ============================================================================
 * Socket Upcall (DEFER_TASKRUN notification)
 * ============================================================================
 *
 * Runs with the socket buffer lock (or, for listening sockets, the listen
 * lock) held when data arrives, space opens, a connection is queued, or a
 * peer closes.
 * Never touches user memory: simply moves `op` onto ctx->taskwork_ops, sets
 * IORING_SQ_TASKRUN in shm->sq_flags, and wakes the owner if sleeping.
 */

static int
kqring_soupcall(struct socket *so __unused, void *arg, int waitflag __unused)
{
	struct kqring_op *op = arg;
	struct kqring_ctx *ctx = op->ctx;

	mtx_lock(&ctx->mtx);
	kqring_taskwork_add_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	return (SU_OK);
}


/*
 * Remove op's upcall from one socket buffer.  listen(2) may have turned the
 * socket into a listener since it was armed: solisten_proto() then reused
 * the sockbuf storage for the listen queues (wiping our upcall), and it did
 * so holding both sockbuf mutexes, so SOLISTENING() is stable here.
 */
void
kqring_sb_disarm(struct kqring_op *op, struct socket *so, int which)
{
	SOCK_BUF_LOCK(so, which);
	if (!SOLISTENING(so) && kqring_sb(so, which)->sb_upcallarg == op)
		soupcall_clear(so, which);
	SOCK_BUF_UNLOCK(so, which);
}


/*
 * Look up the socket an op works on.  For an armed op (taskwork),
 * `expect` is the socket it was armed on.  An op that would have blocked
 * keeps its file (kqring_put_sock()) and is retried on it, as io_uring
 * holds req->file: close(2) or dup2(2) over the descriptor no longer
 * affects it, and the Capsicum rights were checked when it was looked up.
 * Otherwise sqe->fd is resolved again, and if the descriptor was closed
 * or reused for another file in the meantime the op fails with EBADF
 * rather than operating on the new file.
 */
static int
kqring_getsock(const struct kqring_op *op, struct socket *expect,
    cap_rights_t *rights, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (op->sock_fp != NULL) {
		if (!fhold(op->sock_fp))
			return (EBADF);
		*fpp = op->sock_fp;
		return (0);
	}
	error = kqring_op_fget(op, rights, 0, &fp);
	if (error != 0)
		return (error);
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		return (ENOTSOCK);
	}
	if (expect != NULL && fp->f_data != expect) {
		fdrop(fp, td);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

/*
 * Release fp after an attempt, unless the attempt would have blocked
 * (rc == EAGAIN): then the op keeps the reference, taken by
 * kqring_getsock() with the op's own rights, for arming and retrying.
 */
static void
kqring_put_sock(struct kqring_op *op, struct file *fp, int rc)
{
	if (rc == EAGAIN && op->sock_fp == NULL)
		op->sock_fp = fp;
	else
		fdrop(fp, curthread);
}


/*
 * Whether a receive would not block: bytes buffered, EOF or a pending
 * error.  ENOTCONN for a listening socket.  Checked before popping a
 * provided buffer, which is only consumed when bytes are received.
 */
static int
kqring_so_readable(struct socket *so, bool *readable)
{
	SOCK_RECVBUF_LOCK(so);
	/* Checked under the sockbuf lock; see kqring_sb_disarm(). */
	if (SOLISTENING(so)) {
		SOCK_RECVBUF_UNLOCK(so);
		return (ENOTCONN);
	}
	*readable = sbavail(&so->so_rcv) > 0 ||
	    (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0 ||
	    so->so_error != 0;
	SOCK_RECVBUF_UNLOCK(so);
	return (0);
}


/*
 * Pick provided buffers (from sqe->buf_group) for a receive of at most
 * `len` bytes (0: the buffers' size): one, or up to `nmax` for a bundle.
 * Returns EAGAIN, without consuming a buffer, if nothing is there to
 * receive.
 */
static int
kqring_recv_pick_buf(struct kqring_ctx *ctx, const struct kqring_op *op,
    struct socket *so, uint32_t len, struct iovec *iov, u_int nmax,
    struct kqring_sel *sel)
{
	bool readable;
	int error;

	error = kqring_so_readable(so, &readable);
	if (error != 0)
		return (error);
	if (!readable)
		return (EAGAIN);
	return (kqring_buf_select(ctx, op->sqe.buf_group, len, iov, nmax,
	    sel));
}


/*
 * End one attempt that transferred `n` bytes with `error`.  An error after
 * earlier MSG_WAITALL progress reports the progress, as in io_uring.
 * Returns EAGAIN to wait (nothing transferred yet), else 0 with the
 * result in op->res.
 */
static int
kqring_xfer_end(struct kqring_op *op, int error, size_t n)
{
	if (n == 0 && error == EWOULDBLOCK)
		return (EAGAIN);
	if (n == 0 && error != 0) {
		if (op->done_io != 0)
			return (kqring_done(op, kqring_xfer_res(op->done_io), 0));
		return (kqring_done(op, -error, 0));
	}
	return (-1);
}

/*
 * MSG_WAITALL on a stream socket: keep a short transfer's progress and
 * wait for the rest (EAGAIN), as io_uring retries.  Returns the total so
 * far once done.
 */
static int
kqring_waitall(struct kqring_op *op, size_t n, size_t want, size_t *total)
{
	op->done_io += n;
	if (n > 0 && op->done_io < want)
		return (EAGAIN);
	*total = op->done_io;
	return (0);
}

static size_t
kqring_iov_len(const struct iovec *iov, u_int iovcnt)
{
	size_t len = 0;

	/* kqring_req_prep() checked that the sum fits in a ssize_t. */
	for (u_int i = 0; i < iovcnt; i++)
		len += iov[i].iov_len;
	return (len);
}

void
kqring_uio_init(struct uio *auio, struct iovec *iov, u_int iovcnt,
    enum uio_rw rw)
{
	auio->uio_iov = iov;
	auio->uio_iovcnt = iovcnt;
	auio->uio_offset = 0;
	auio->uio_resid = kqring_iov_len(iov, iovcnt);
	auio->uio_segflg = UIO_USERSPACE;
	auio->uio_rw = rw;
	auio->uio_td = curthread;
}

/*
 * Whether a receive on so would certainly find nothing: a connected
 * stream socket with no data, control messages, error or EOF.  The fields
 * read are the part of struct sockbuf shared by every buffer
 * implementation (the one kevent(2) reads), so this holds for TCP and
 * AF_UNIX alike.  Read unlocked, it is only a hint: if data arrives
 * meanwhile, kqring_sb_arm() sees it under the sockbuf lock once the
 * upcall is installed and queues the op, so nothing is missed.
 */
static bool
kqring_so_rcv_idle(struct socket *so)
{
	return (so->so_type == SOCK_STREAM && !SOLISTENING(so) &&
	    (so->so_state & SS_ISCONNECTED) != 0 &&
	    atomic_load_short(&so->so_error) == 0 &&
	    atomic_load_int(&so->so_rcv.sb_acc) == 0 &&
	    atomic_load_int(&so->so_rcv.sb_ctl) == 0 &&
	    (atomic_load_short(&so->so_rcv.sb_state) & SBS_CANTRCVMORE) == 0);
}

/*
 * Socket receive for RECV, READ_MULTISHOT, and READ / READV on a socket:
 * into the SQE's buffer, its iovec, or a provided buffer
 * (IOSQE_BUFFER_SELECT).  Returns:
 *   0: the attempt finished, with its result in op->res / op->cflags;
 *      IORING_CQE_F_MORE in op->cflags means a multishot receive goes on.
 *   EAGAIN: socket would block; caller should arm (or keep armed) upcall.
 * *more_out is an unlocked hint that another receive would not block
 * (bytes still buffered or EOF), used to end multishot drain loops without
 * a final EAGAIN soreceive(); a stale value is harmless because the upcall
 * stays armed.
 */
int
kqring_exec_recv(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	struct iovec biov[KQRING_MAX_BUNDLE];
	struct kqring_sel sel;
	cap_rights_t rights;
	u_int iovcnt = 1;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool multishot = sqe->opcode == IORING_OP_READ_MULTISHOT ||
	    (sqe->opcode == IORING_OP_RECV &&
	    (sqe->ioprio & IORING_RECV_MULTISHOT) != 0);
	bool bundle = sqe->opcode == IORING_OP_RECV &&
	    (sqe->ioprio & IORING_RECVSEND_BUNDLE) != 0;
	bool waitall;
	size_t n;
	int error, flags, rc;

	*more_out = false;

	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_RECV), &fp);
	if (error != 0) {
		/* READ_MULTISHOT needs readiness, which only sockets have yet. */
		if (error == ENOTSOCK && sqe->opcode == IORING_OP_READ_MULTISHOT)
			error = EOPNOTSUPP;
		return (kqring_done(op, -error, 0));
	}
	so = fp->f_data;
#ifdef KQRING_MAC
	error = mac_socket_check_receive(td->td_ucred, so);
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
#endif
	/*
	 * Nothing to read: say so without soreceive() or taking a provided
	 * buffer, as soreceive() would end (EWOULDBLOCK, nothing moved).
	 * Out-of-band data is not in the receive buffer, so MSG_OOB asks.
	 */
	if ((sqe->opcode != IORING_OP_RECV ||
	    (sqe->msg_flags & MSG_OOB) == 0) && kqring_so_rcv_idle(so)) {
		kqring_put_sock(op, fp, EAGAIN);
		return (EAGAIN);
	}

	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_recv_pick_buf(ctx, op, so, kqring_buf_cap(op),
		    biov, bundle ? KQRING_MAX_BUNDLE : 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			kqring_put_sock(op, fp, error);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		iov = biov;
		iovcnt = sel.nbufs;
	} else if (kqring_op_readv(sqe->opcode)) {
		/* uiomove() advances the kernel iovec copy in place. */
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_READ);
	ssize_t want = auio.uio_resid;

	flags = MSG_DONTWAIT;
	if (sqe->opcode == IORING_OP_RECV)
		flags |= (int)sqe->msg_flags;
	waitall = (flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	error = soreceive(so, NULL, &auio, NULL, NULL, &flags);
	/* MSG_TRUNC on a datagram: uio_resid underflows to the real size. */
	n = (size_t)(want - auio.uio_resid);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		kqring_put_sock(op, fp, rc);
		return (rc);
	}

	/*
	 * Commit the provided buffer ONLY when n > 0!  On EOF (n == 0) the
	 * buffer stays in its group and res = 0 has no IORING_CQE_F_BUFFER.
	 */
	if (buf_select) {
		if (n > 0)
			cqe_flags |= kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
	}
	if (multishot && n > 0) {
		cqe_flags |= IORING_CQE_F_MORE;
		*more_out = kqring_so_rcv_pending(so, true);
	} else if (kqring_so_rcv_pending(so, false)) {
		cqe_flags |= IORING_CQE_F_SOCK_NONEMPTY;
	}
	if (waitall && kqring_waitall(op, n, sqe->len, &n) == EAGAIN) {
		kqring_put_sock(op, fp, EAGAIN);
		return (EAGAIN);
	}
	fdrop(fp, td);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/*
 * Zero-copy send for IORING_OP_SEND_ZC and IORING_OP_SENDMSG_ZC (both
 * registered IORING_RECVSEND_FIXED_BUF buffers and unregistered user
 * buffers / scatter-gather iovecs) on stream sockets (AF_LOCAL and TCP)
 * when the transfer is at least KQRING_ZC_MIN_LEN bytes, or for an
 * unregistered buffer on AF_LOCAL, kern.kqring.zc_local_min_len.  Builds an
 * EXT_SFBUF mbuf chain over the held physical pages and submits it via
 * pr_send.  Returns EOPNOTSUPP if the caller should fall back to copying.
 */
static int
kqring_send_zc_iov(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *so, const struct iovec *iov, u_int iovcnt, size_t want,
    int flags, struct thread *td, size_t *np)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_zc_buf *zbuf = NULL;
	struct kqring_zc_notif *zc;
	struct kqring_rcqe *rcqe;
	struct mbuf *top = NULL, *mtail = NULL;
	vm_offset_t cur;
	size_t avail = 0, send_len = 0, rem;
	u_int nzbufs = 0;
	bool fixed = (sqe->ioprio & IORING_RECVSEND_FIXED_BUF) != 0;
	int dom = so->so_proto->pr_domain->dom_family;
	size_t min_len = dom == AF_LOCAL && !fixed ?
	    MAX(atomic_load_int(&kqring_zc_local_min_len), KQRING_ZC_MIN_LEN) :
	    KQRING_ZC_MIN_LEN;
	int error = 0, pr_flags;

	*np = 0;
	if (so->so_type != SOCK_STREAM ||
	    (dom != AF_LOCAL &&
	    !((dom == AF_INET || dom == AF_INET6) &&
	    so->so_proto->pr_protocol == IPPROTO_TCP)))
		return (EOPNOTSUPP);
	if (want < min_len)
		return (EOPNOTSUPP);

	if (fixed) {
		if (op->fbuf == NULL || op->fbuf->zbuf == NULL)
			return (EOPNOTSUPP);
		error = kqring_zc_refresh_cow(ctx, op->fbuf, td);
		if (error != 0)
			return (error);
		zbuf = op->fbuf->zbuf;
	}

	zc = malloc(sizeof(*zc), M_KQRING, M_WAITOK | M_ZERO);
	rcqe = malloc(sizeof(*rcqe), M_KQRING, M_WAITOK | M_ZERO);

	error = SOCK_IO_SEND_LOCK(so, SBL_WAIT);
	if (error != 0) {
		free(rcqe, M_KQRING);
		free(zc, M_KQRING);
		return (error);
	}

	if (dom == AF_LOCAL) {
		struct socket *so2;
		struct sockbuf *sb;
		u_int sb_space = 0, mb_space = 0, max_pages = 0;

		SOCK_RECVBUF_LOCK(so);
		if (SOLISTENING(so)) {
			SOCK_RECVBUF_UNLOCK(so);
			SOCK_IO_SEND_UNLOCK(so);
			free(rcqe, M_KQRING);
			free(zc, M_KQRING);
			return (ENOTCONN);
		}
		so2 = so->so_rcv.uxst_peer;
		if (so2 == NULL) {
			error = (so->so_state & SS_ISDISCONNECTED) != 0 ?
			    EPIPE : ENOTCONN;
			SOCK_RECVBUF_UNLOCK(so);
			SOCK_IO_SEND_UNLOCK(so);
			free(rcqe, M_KQRING);
			free(zc, M_KQRING);
			return (error);
		}
		soref(so2);
		SOCK_RECVBUF_UNLOCK(so);

		sb = &so2->so_rcv;
		SOCK_RECVBUF_LOCK(so2);
		if ((sb->sb_state & SBS_CANTRCVMORE) != 0) {
			error = EPIPE;
		} else if (so->so_error != 0) {
			error = so->so_error;
			so->so_error = 0;
		} else {
			if (sb->sb_hiwat > sb->sb_ccc + sb->sb_ctl)
				sb_space = sb->sb_hiwat - sb->sb_ccc -
				    sb->sb_ctl;
			if (sb->sb_mbmax > sb->sb_mbcnt)
				mb_space = sb->sb_mbmax - sb->sb_mbcnt;
			avail = MIN(sb_space, mb_space);
			max_pages = mb_space / (MSIZE + PAGE_SIZE);
			if (max_pages == 0)
				avail = 0;
			else if (avail > (size_t)max_pages * PAGE_SIZE)
				avail = (size_t)max_pages * PAGE_SIZE;
			if (avail == 0 ||
			    (avail < want && avail < (size_t)sb->sb_lowat))
				error = EWOULDBLOCK;
		}
		SOCK_RECVBUF_UNLOCK(so2);
		sorele(so2);

		if (error == 0) {
			size_t capped = 0, rem_cap = MIN(want, avail);
			u_int mbs = 0;

			for (u_int i = 0; i < iovcnt && rem_cap > 0 &&
			    mbs < max_pages; i++) {
				vm_offset_t c = (vm_offset_t)iov[i].iov_base;
				size_t srem = MIN(iov[i].iov_len, rem_cap);

				while (srem > 0 && mbs < max_pages) {
					size_t chunk = MIN((size_t)PAGE_SIZE -
					    (c & PAGE_MASK), srem);

					capped += chunk;
					c += chunk;
					srem -= chunk;
					rem_cap -= chunk;
					mbs++;
				}
			}
			avail = capped;
			if (avail == 0)
				error = EWOULDBLOCK;
		}
	} else {
		long space;

		SOCK_SENDBUF_LOCK(so);
		if (SOLISTENING(so)) {
			error = ENOTCONN;
		} else if (so->so_snd.sb_tls_info != NULL) {
			error = EOPNOTSUPP;
		} else if ((so->so_snd.sb_state & SBS_CANTSENDMORE) != 0) {
			error = EPIPE;
		} else if (so->so_error != 0) {
			error = so->so_error;
			so->so_error = 0;
		} else if ((so->so_state & SS_ISCONNECTED) == 0) {
			error = ENOTCONN;
		} else {
			space = sbspace(&so->so_snd);
			if (space <= 0 || (space < (long)want &&
			    space < (long)so->so_snd.sb_lowat))
				error = EWOULDBLOCK;
			else
				avail = (size_t)space;
		}
		SOCK_SENDBUF_UNLOCK(so);
	}

	if (error == 0) {
		send_len = MIN(want, avail);
		if (send_len < min_len)
			error = EOPNOTSUPP;
	}
	if (error == 0 && !fixed) {
		size_t rem_cnt = send_len, rem_pin = send_len;
		u_int k = 0;

		for (u_int i = 0; i < iovcnt && rem_cnt > 0; i++) {
			if (iov[i].iov_len == 0)
				continue;
			nzbufs++;
			if (iov[i].iov_len >= rem_cnt)
				break;
			rem_cnt -= iov[i].iov_len;
		}
		if (nzbufs > 1) {
			zc->zbufs = malloc(nzbufs * sizeof(*zc->zbufs),
			    M_KQRING, M_WAITOK | M_ZERO);
			zc->nzbufs = nzbufs;
		}
		for (u_int i = 0; i < iovcnt && rem_pin > 0; i++) {
			size_t seg_len = MIN(iov[i].iov_len, rem_pin);
			struct kqring_zc_buf *zb = NULL;

			if (seg_len == 0)
				continue;
			error = kqring_zc_buf_alloc(td,
			    (vm_offset_t)iov[i].iov_base, seg_len,
			    VM_PROT_READ, 0, &zb);
			if (error != 0) {
				if (error == ENOMEM || k > 0)
					error = EOPNOTSUPP;
				break;
			}
			if (nzbufs == 1)
				zc->zbuf = zb;
			else
				zc->zbufs[k] = zb;
			k++;
			rem_pin -= seg_len;
		}
	}
	if (error != 0) {
		SOCK_IO_SEND_UNLOCK(so);
		kqring_zc_notif_free_bufs(zc);
		free(rcqe, M_KQRING);
		free(zc, M_KQRING);
		return (error);
	}

	if (!kqring_zc_hold()) {
		SOCK_IO_SEND_UNLOCK(so);
		kqring_zc_notif_free_bufs(zc);
		free(rcqe, M_KQRING);
		free(zc, M_KQRING);
		return (EOPNOTSUPP);
	}

	rcqe->cqe.user_data = sqe->user_data;
	rcqe->cqe.res = 0;
	rcqe->cqe.flags = IORING_CQE_F_NOTIF;

	refcount_init(&zc->refs, 1);
	if (fixed) {
		refcount_acquire(&zbuf->refs);
		zc->zbuf = zbuf;
	}
	zc->ctx = ctx;
	zc->rcqe = rcqe;

	mtx_lock(&kqring_zc_mtx);
	mtx_lock(&ctx->mtx);
	if (!ctx->closing) {
		TAILQ_INSERT_TAIL(&ctx->zc_pending, zc, link);
		zc->on_ctx = true;
	} else {
		zc->ctx = NULL;
	}
	mtx_unlock(&ctx->mtx);
	mtx_unlock(&kqring_zc_mtx);

	rem = send_len;
	for (u_int i = 0, k = 0; i < iovcnt && rem > 0; i++) {
		size_t seg_rem = MIN(iov[i].iov_len, rem);
		struct kqring_zc_buf *seg_zb;

		if (seg_rem == 0)
			continue;
		seg_zb = fixed ? zbuf :
		    (nzbufs == 1 ? zc->zbuf : zc->zbufs[k]);
		k++;
		cur = (vm_offset_t)iov[i].iov_base;
		while (seg_rem > 0) {
			size_t pg_idx = (cur - seg_zb->page_base) >> PAGE_SHIFT;
			size_t pg_off = cur & PAGE_MASK;
			size_t chunk = MIN((size_t)PAGE_SIZE - pg_off, seg_rem);
			struct sf_buf *sf;
			struct mbuf *m0;

			KASSERT(pg_idx < (size_t)seg_zb->npages,
			    ("kqring: zc page index out of bounds"));
			sf = sf_buf_alloc(seg_zb->pages[pg_idx], SFB_DEFAULT);
			m0 = m_get(M_WAITOK, MT_DATA);
			refcount_acquire(&zc->refs);
			m0->m_ext.ext_buf = (char *)sf_buf_kva(sf);
			m0->m_ext.ext_size = PAGE_SIZE;
			m0->m_ext.ext_arg1 = zc;
			m0->m_ext.ext_arg2 = sf;
			m0->m_ext.ext_type = EXT_SFBUF;
			m0->m_ext.ext_flags = EXT_FLAG_EMBREF;
			m0->m_ext.ext_free = kqring_zc_mext_free;
			m0->m_ext.ext_count = 1;
			m0->m_flags |= (M_EXT | M_RDONLY);
			m0->m_data = (char *)sf_buf_kva(sf) + pg_off;
			m0->m_len = (int)chunk;

			if (mtail != NULL)
				mtail->m_next = m0;
			else
				top = m0;
			mtail = m0;
			cur += chunk;
			seg_rem -= chunk;
			rem -= chunk;
		}
	}

	pr_flags = (flags & MSG_MORETOCOME) != 0 ? PRUS_MORETOCOME : 0;
	CURVNET_SET(so->so_vnet);
	error = so->so_proto->pr_send(so, pr_flags, top, NULL, NULL, td);
	CURVNET_RESTORE();
	SOCK_IO_SEND_UNLOCK(so);

	if (error != 0) {
		kqring_zc_abort(zc);
		return (error);
	}
	td->td_ru.ru_msgsnd++;
	op->zc_notif = zc;
	*np = send_len;
	return (0);
}

/*
 * SEND, SEND_ZC, and WRITE / WRITEV on a socket.  SEND may send from
 * provided buffers (IOSQE_BUFFER_SELECT, several with
 * IORING_RECVSEND_BUNDLE), consuming those the bytes sent came from.
 */
int
kqring_exec_send(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	struct sockaddr *to = NULL;
	struct uio auio;
	struct iovec aiov, *iov = &aiov;
	struct iovec biov[KQRING_MAX_BUNDLE];
	struct kqring_sel sel;
	cap_rights_t rights;
	u_int iovcnt = 1;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool waitall = false;
	size_t n;
	int error, flags = MSG_DONTWAIT | MSG_NOSIGNAL, rc;

	if (sqe->opcode == IORING_OP_SEND || sqe->opcode == IORING_OP_SEND_ZC) {
		flags |= (int)sqe->msg_flags;
		to = op->args;
	}
	/* As sendto(2): no addressed sends in capability mode. */
	if (to != NULL && IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	if (to != NULL)
		cap_rights_init(&rights, CAP_SEND, CAP_CONNECT);
	else
		cap_rights_init_one(&rights, CAP_SEND);
	error = kqring_getsock(op, expect, &rights, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;
#ifdef KQRING_MAC
	if (to != NULL)
		error = mac_socket_check_connect(td->td_ucred, so, to);
	if (error == 0)
		error = mac_socket_check_send(td->td_ucred, so);
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, 0));
	}
#endif

	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_buf_select(ctx, sqe->buf_group, sqe->len, biov,
		    (sqe->ioprio & IORING_RECVSEND_BUNDLE) != 0 ?
		    KQRING_MAX_BUNDLE : 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
		iov = biov;
		iovcnt = sel.nbufs;
	} else if (kqring_op_writev(sqe->opcode)) {
		iov = op->args;
		iovcnt = sqe->len;
	} else {
		aiov.iov_base = (char *)(uintptr_t)sqe->addr + op->done_io;
		aiov.iov_len = sqe->len - op->done_io;
	}
	kqring_uio_init(&auio, iov, iovcnt, UIO_WRITE);
	ssize_t want = auio.uio_resid;

	waitall = (flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	flags &= ~MSG_WAITALL;
	if (sqe->opcode == IORING_OP_SEND_ZC &&
	    to == NULL && !waitall &&
	    (sqe->msg_flags & (MSG_OOB | MSG_EOR | MSG_EOF |
	    MSG_DONTROUTE)) == 0 &&
	    want >= (ssize_t)KQRING_ZC_MIN_LEN) {
		error = kqring_send_zc_iov(ctx, op, so, iov, iovcnt,
		    (size_t)want, flags, td, &n);
		if (error != EOPNOTSUPP) {
			fdrop(fp, td);
			if ((rc = kqring_xfer_end(op, error, n)) >= 0)
				return (rc);
			return (kqring_done(op, kqring_xfer_res(n), 0));
		}
	}
	error = sosend(so, to, &auio, NULL, NULL, flags, td);
	n = (size_t)(want - auio.uio_resid);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		kqring_put_sock(op, fp, rc);
		return (rc);
	}
	fdrop(fp, td);
	if (buf_select) {
		cqe_flags = kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
	}
	if (waitall && kqring_waitall(op, n, sqe->len, &n) == EAGAIN)
		return (EAGAIN);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/*
 * Copy a received message's control data (SCM_RIGHTS already installed
 * as descriptors) to `ubuf`, as recvmsg(2) does; what does not fit is
 * disposed of (closing such descriptors) and flagged MSG_CTRUNC.
 */
static int
kqring_copyout_control(struct mbuf *control, void *ubuf, size_t space,
    size_t *lenp, int *flagsp)
{
	struct mbuf *m;
	size_t len = 0;
	int error = 0;

	for (m = control; m != NULL && space - len >= (size_t)m->m_len;
	    m = m->m_next) {
		error = copyout(mtod(m, void *), (char *)ubuf + len, m->m_len);
		if (error != 0)
			break;
		len += m->m_len;
	}
	if (m != NULL) {
		if (error == 0)
			*flagsp |= MSG_CTRUNC;
		m_dispose_extcontrolm(m);
	}
	m_freem(control);
	*lenp = len;
	return (error);
}

/*
 * kern_recvit() on a socket rather than a descriptor (fixed files have
 * none).  mp->msg_iov is a kernel copy of user iovecs; the name goes to
 * `fromseg` memory, and without controlp the control data to user memory.
 * The byte count goes to *np.
 */
static int
kqring_recvit(struct thread *td, struct socket *so, struct msghdr *mp,
    enum uio_seg fromseg, struct mbuf **controlp, size_t *np)
{
	struct uio auio;
	struct mbuf *control = NULL, *m;
	struct sockaddr *fromsa = NULL;
	caddr_t ctlbuf;
	ssize_t len;
	int error;

	if (controlp != NULL)
		*controlp = NULL;
#ifdef KQRING_MAC
	error = mac_socket_check_receive(td->td_ucred, so);
	if (error != 0)
		return (error);
#endif
	kqring_uio_init(&auio, mp->msg_iov, mp->msg_iovlen, UIO_READ);
	len = auio.uio_resid;
	error = soreceive(so, &fromsa, &auio, NULL,
	    (mp->msg_control != NULL || controlp != NULL) ? &control : NULL,
	    &mp->msg_flags);
	if (error != 0 && auio.uio_resid != len && (error == ERESTART ||
	    error == EINTR || error == EWOULDBLOCK))
		error = 0;
	if (error != 0)
		goto out;
	*np = (size_t)(len - auio.uio_resid);
	if (mp->msg_name != NULL) {
		len = mp->msg_namelen;
		if (len <= 0 || fromsa == NULL) {
			len = 0;
		} else {
			len = MIN(len, fromsa->sa_len);
			if (fromseg == UIO_USERSPACE) {
				error = copyout(fromsa, mp->msg_name,
				    (unsigned)len);
				if (error != 0)
					goto out;
			} else {
				bcopy(fromsa, mp->msg_name, len);
			}
		}
		mp->msg_namelen = len;
	}
	if (mp->msg_control != NULL && controlp == NULL) {
		ctlbuf = mp->msg_control;
		len = mp->msg_controllen;
		mp->msg_controllen = 0;
		for (m = control; m != NULL && len >= m->m_len;
		    m = m->m_next) {
			error = copyout(mtod(m, caddr_t), ctlbuf, m->m_len);
			if (error != 0)
				goto out;
			ctlbuf += m->m_len;
			len -= m->m_len;
			mp->msg_controllen += m->m_len;
		}
		if (m != NULL) {
			mp->msg_flags |= MSG_CTRUNC;
			m_dispose_extcontrolm(m);
		}
	}
out:
	free(fromsa, M_SONAME);
	if (error == 0 && controlp != NULL) {
		*controlp = control;
	} else if (control != NULL) {
		if (error != 0)
			m_dispose_extcontrolm(control);
		m_freem(control);
	}
	return (error);
}

/*
 * Multishot RECVMSG: one message per provided buffer, laid out as
 * struct io_uring_recvmsg_out, then the name and control areas sized by
 * the submitted msghdr, then the payload.
 */
static int
kqring_exec_recvmsg_multi(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *so, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct io_uring_recvmsg_out out;
	struct kqring_sel sel;
	struct msghdr m;
	struct iovec aiov, biov;
	struct mbuf *control = NULL;
	uint8_t name[SOCK_MAXADDRLEN];
	size_t namesz = ma->msg.msg_namelen;
	size_t ctlsz = ma->msg.msg_controllen;
	size_t hdr = sizeof(out) + namesz + ctlsz, clen, n;
	char *ubuf;
	int error;

	error = kqring_recv_pick_buf(ctx, op, so, kqring_msg_buf_cap(ma),
	    &biov, 1, &sel);
	if (error != 0) {
		if (error == EAGAIN)
			return (EAGAIN);
		return (kqring_done(op, -error, 0));
	}
	if (biov.iov_len < hdr)
		return (kqring_done(op, -EFAULT, 0));
	ubuf = biov.iov_base;

	/* Ask for the whole name, to report its real length. */
	bzero(&m, sizeof(m));
	m.msg_name = name;
	m.msg_namelen = sizeof(name);
	aiov.iov_base = ubuf + hdr;
	aiov.iov_len = biov.iov_len - hdr;
	m.msg_iov = &aiov;
	m.msg_iovlen = 1;
	m.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	error = kqring_recvit(td, so, &m, UIO_SYSSPACE,
	    ctlsz != 0 ? &control : NULL, &n);
	if (error == EWOULDBLOCK)
		return (EAGAIN);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (n == 0 && kqring_so_stream(so)) {
		/* EOF: ends the multishot, buffer not consumed. */
		m_freem(control);
		return (kqring_done(op, 0, 0));
	}

	bzero(&out, sizeof(out));
	out.namelen = m.msg_namelen;
	if (MIN(out.namelen, namesz) != 0)
		error = copyout(name, ubuf + sizeof(out),
		    MIN(out.namelen, namesz));
	clen = 0;
	if (control != NULL) {
		if (error == 0)
			error = kqring_copyout_control(control,
			    ubuf + sizeof(out) + namesz, ctlsz, &clen,
			    &m.msg_flags);
		else
			m_freem(control);
	}
	out.controllen = (uint32_t)clen;
	out.payloadlen = (uint32_t)MIN(n, UINT32_MAX);
	out.flags = (uint32_t)(m.msg_flags & ~MSG_DONTWAIT);
	if (error == 0)
		error = copyout(&out, ubuf, sizeof(out));
	if (error != 0)
		return (kqring_done(op, -error, 0));

	n = hdr + MIN(n, aiov.iov_len);
	*more_out = kqring_so_rcv_pending(so, true);
	return (kqring_done(op, kqring_xfer_res(n),
	    kqring_buf_commit(ctx, &sel, n) | IORING_CQE_F_MORE));
}

/* RECVMSG: kqring_recvit() on the kernel copy of the msghdr's iovec. */
int
kqring_exec_recvmsg(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct msghdr *umsg = (struct msghdr *)(uintptr_t)sqe->addr;
	struct kqring_sel sel;
	struct file *fp;
	struct socket *so;
	struct msghdr msg;
	struct iovec aiov;
	cap_rights_t rights;
	uint32_t cqe_flags = 0;
	bool buf_select = (sqe->flags & IOSQE_BUFFER_SELECT) != 0;
	bool waitall;
	size_t n;
	int error, rc;

	*more_out = false;
	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_RECV), &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;

	if ((sqe->ioprio & IORING_RECV_MULTISHOT) != 0) {
		sx_xlock(&ctx->bg_sx);
		rc = kqring_exec_recvmsg_multi(ctx, op, so, more_out);
		sx_xunlock(&ctx->bg_sx);
		fdrop(fp, td);
		return (rc);
	}

	msg = ma->msg;
	if (buf_select) {
		sx_xlock(&ctx->bg_sx);
		error = kqring_recv_pick_buf(ctx, op, so,
		    kqring_msg_buf_cap(ma), &aiov, 1, &sel);
		if (error != 0) {
			sx_xunlock(&ctx->bg_sx);
			kqring_put_sock(op, fp, error);
			if (error == EAGAIN)
				return (EAGAIN);
			return (kqring_done(op, -error, 0));
		}
		msg.msg_iov = &aiov;
		msg.msg_iovlen = 1;
	}
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	msg.msg_flags = (int)sqe->msg_flags | MSG_DONTWAIT;
	waitall = (msg.msg_flags & MSG_WAITALL) != 0 && !buf_select &&
	    kqring_so_stream(so);
	n = 0;
	error = kqring_recvit(td, so, &msg, UIO_USERSPACE, NULL, &n);
	if ((rc = kqring_xfer_end(op, error, n)) >= 0) {
		if (buf_select)
			sx_xunlock(&ctx->bg_sx);
		kqring_put_sock(op, fp, rc);
		return (rc);
	}

	/* As recvmsg(2), report the name, control and flags lengths. */
	int oflags = msg.msg_flags & ~(MSG_DONTWAIT & ~(int)sqe->msg_flags);
	error = copyout(&msg.msg_namelen, &umsg->msg_namelen,
	    sizeof(msg.msg_namelen));
	if (error == 0)
		error = copyout(&msg.msg_controllen, &umsg->msg_controllen,
		    sizeof(msg.msg_controllen));
	if (error == 0)
		error = copyout(&oflags, &umsg->msg_flags, sizeof(oflags));
	if (buf_select) {
		if (n > 0)
			cqe_flags |= kqring_buf_commit(ctx, &sel, n);
		sx_xunlock(&ctx->bg_sx);
	}
	if (error != 0) {
		fdrop(fp, td);
		return (kqring_done(op, -error, cqe_flags));
	}
	if (kqring_so_rcv_pending(so, false))
		cqe_flags |= IORING_CQE_F_SOCK_NONEMPTY;
	if (waitall && kqring_waitall(op, n, want + op->done_io, &n) ==
	    EAGAIN) {
		kqring_put_sock(op, fp, EAGAIN);
		return (EAGAIN);
	}
	fdrop(fp, td);
	return (kqring_done(op, kqring_xfer_res(n), cqe_flags));
}

/*
 * kern_sendit() on a socket rather than a descriptor (see kqring_recvit()).
 * Consumes `control`.  The byte count goes to *np.
 */
static int
kqring_sendit(struct thread *td, struct socket *so, struct msghdr *mp,
    int flags, struct mbuf *control, size_t *np)
{
	struct uio auio;
	ssize_t len;
	int error;

	*np = 0;
#ifdef KQRING_MAC
	error = 0;
	if (mp->msg_name != NULL)
		error = mac_socket_check_connect(td->td_ucred, so,
		    mp->msg_name);
	if (error == 0)
		error = mac_socket_check_send(td->td_ucred, so);
	if (error != 0) {
		m_freem(control);
		return (error);
	}
#endif
	kqring_uio_init(&auio, mp->msg_iov, mp->msg_iovlen, UIO_WRITE);
	len = auio.uio_resid;
	error = sousrsend(so, mp->msg_name, &auio, control, flags, NULL);
	if (error == 0)
		*np = (size_t)(len - auio.uio_resid);
	return (error);
}

/* SENDMSG / SENDMSG_ZC: kqring_sendit() on the kernel copy of the msghdr. */
int
kqring_exec_sendmsg(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct kqring_msg_args *ma = op->args;
	struct thread *td = curthread;
	struct mbuf *control = NULL;
	struct file *fp;
	struct socket *so;
	struct msghdr msg;
	cap_rights_t rights;
	bool waitall;
	size_t n = 0;
	int error, flags, rc;

	/* As sendmsg(2): no addressed sends in capability mode. */
	if (ma->msg.msg_name != NULL && IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	if (ma->msg.msg_name != NULL)
		cap_rights_init(&rights, CAP_SEND, CAP_CONNECT);
	else
		cap_rights_init_one(&rights, CAP_SEND);
	error = kqring_getsock(op, expect, &rights, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;
	waitall = (sqe->msg_flags & MSG_WAITALL) != 0 && kqring_so_stream(so);

	msg = ma->msg;
	size_t want = kqring_iov_len(msg.msg_iov, msg.msg_iovlen);
	flags = ((int)sqe->msg_flags & ~MSG_WAITALL) | MSG_DONTWAIT |
	    MSG_NOSIGNAL;
	if (sqe->opcode == IORING_OP_SENDMSG_ZC &&
	    msg.msg_name == NULL && msg.msg_controllen == 0 && !waitall &&
	    (sqe->msg_flags & (MSG_OOB | MSG_EOR | MSG_EOF |
	    MSG_DONTROUTE)) == 0 &&
	    want >= KQRING_ZC_MIN_LEN) {
#ifdef KQRING_MAC
		error = mac_socket_check_send(td->td_ucred, so);
		if (error == 0)
#endif
			error = kqring_send_zc_iov(ctx, op, so, msg.msg_iov,
			    (u_int)msg.msg_iovlen, want, flags, td, &n);
		if (error != EOPNOTSUPP) {
			fdrop(fp, td);
			if ((rc = kqring_xfer_end(op, error, n)) >= 0)
				return (rc);
			return (kqring_done(op, kqring_xfer_res(n), 0));
		}
	}
	if (msg.msg_controllen != 0) {
		/* kqring_sendit() consumes the mbuf: rebuild it each attempt. */
		control = m_get2(msg.msg_controllen, M_WAITOK, MT_CONTROL, 0);
		control->m_len = msg.msg_controllen;
		bcopy(msg.msg_control, mtod(control, void *),
		    msg.msg_controllen);
	}
	error = kqring_sendit(td, so, &msg, flags, control, &n);
	fdrop(fp, td);

	if ((rc = kqring_xfer_end(op, error, n)) >= 0)
		return (rc);
	/* The control data went with the first part, as in io_uring. */
	if (n > 0)
		ma->msg.msg_controllen = 0;
	if (waitall && kqring_waitall(op, n, want + op->done_io, &n) ==
	    EAGAIN)
		return (EAGAIN);
	return (kqring_done(op, kqring_xfer_res(n), 0));
}

/*
 * CONNECT: start a non-blocking connect, then (armed on so_snd, which
 * soisconnected() and soisdisconnected() wake) wait for it to finish.
 * The checks mirror kern_connectat(), which would sleep.
 */
int
kqring_exec_connect(struct kqring_op *op, struct socket *expect)
{
	struct thread *td = curthread;
	struct sockaddr *sa = op->args;
	struct file *fp;
	struct socket *so;
	cap_rights_t rights;
	int error;

	if (IN_CAPABILITY_MODE(td))
		return (kqring_done(op, -ECAPMODE, 0));
	error = kqring_getsock(op, expect,
	    cap_rights_init_one(&rights, CAP_CONNECT), &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	so = fp->f_data;

	if (!op->connecting) {
		if ((so->so_state & SS_ISCONNECTING) != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -EALREADY, 0));
		}
#ifdef KQRING_MAC
		error = mac_socket_check_connect(td->td_ucred, so, sa);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
#endif
		error = soconnect(so, sa, td);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
		op->connecting = true;
	}

	SOCK_LOCK(so);
	if ((so->so_state & SS_ISCONNECTING) != 0 && so->so_error == 0) {
		SOCK_UNLOCK(so);
		fdrop(fp, td);
		return (EAGAIN);
	}
	error = so->so_error;
	so->so_error = 0;
	SOCK_UNLOCK(so);
	fdrop(fp, td);
	return (kqring_done(op, -error, 0));
}

/*
 * ACCEPT: kern_accept4() on the listening socket's file, so that it works
 * on a fixed file, and can create a direct descriptor (sqe->file_index).
 * The new file inherits the listener's Capsicum rights, as accept(2)'s
 * descriptor does, and gets only the flags asked for, as accept4(2).
 */
int
kqring_exec_accept(struct kqring_ctx *ctx, struct kqring_op *op,
    struct socket *expect, bool *more_out)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *headfp = NULL, *nfp = NULL;
	struct filecaps hcaps, ncaps;
	struct socket *head, *so;
	struct sockaddr_storage ss = { .ss_len = sizeof(ss) };
	cap_rights_t rights;
	bool direct = sqe->file_index != 0;
	u_int fflag;
	int error, nfd = -1, tmp;
	int aflags = (int)sqe->accept_flags;
	int32_t res;

	error = kqring_op_fget_caps(op, cap_rights_init_one(&rights,
	    CAP_ACCEPT), &headfp, &hcaps);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (headfp->f_type != DTYPE_SOCKET) {
		error = ENOTSOCK;
		goto fail;
	}
	head = headfp->f_data;
	if (expect != NULL && head != expect) {
		error = EBADF;
		goto fail;
	}
	if (!SOLISTENING(head)) {
		error = EINVAL;
		goto fail;
	}
	/* Listening sockets have no so_rcv; only sol_comp/so_error apply. */
	if (TAILQ_EMPTY(&head->sol_comp) && head->so_error == 0) {
		filecaps_free(&hcaps);
		fdrop(headfp, td);
		return (EAGAIN);
	}
#ifdef KQRING_MAC
	error = mac_socket_check_accept(td->td_ucred, head);
	if (error != 0)
		goto fail;
#endif

	if (direct) {
		error = falloc_noinstall(td, &nfp);
	} else {
		(void)filecaps_copy(&hcaps, &ncaps, false);
		error = falloc_caps(td, &nfp, &nfd,
		    ((aflags & SOCK_CLOEXEC) != 0 ? O_CLOEXEC : 0) |
		    ((aflags & SOCK_CLOFORK) != 0 ? O_CLOFORK : 0), &ncaps);
		if (error != 0)
			filecaps_free(&ncaps);
	}
	if (error != 0) {
		nfp = NULL;
		goto fail;
	}
	SOCK_LOCK(head);
	if (!SOLISTENING(head)) {
		SOCK_UNLOCK(head);
		error = EINVAL;
		goto fail;
	}
	/*
	 * Re-check under the listen lock: another accepter may have taken
	 * the connection, and solisten_dequeue() would then sleep (for a
	 * blocking listener) in the ring owner.
	 */
	if (TAILQ_EMPTY(&head->sol_comp) && head->so_error == 0) {
		SOCK_UNLOCK(head);
		error = EWOULDBLOCK;
		goto fail;
	}
	error = solisten_dequeue(head, &so, aflags);	/* unlocks head */
	if (error != 0)
		goto fail;
	KNOTE_UNLOCKED(&head->so_rdsel.si_note, 0);

	fflag = atomic_load_int(&headfp->f_flag) & ~(FNONBLOCK | FASYNC);
	if ((aflags & SOCK_NONBLOCK) != 0)
		fflag |= FNONBLOCK;
	finit(nfp, fflag, DTYPE_SOCKET, so, &socketops);
	/* Sync socket nonblocking/async state with file flags. */
	tmp = fflag & FNONBLOCK;
	(void)fo_ioctl(nfp, FIONBIO, &tmp, td->td_ucred, td);
	tmp = fflag & FASYNC;
	(void)fo_ioctl(nfp, FIOASYNC, &tmp, td->td_ucred, td);
	error = soaccept(so, (struct sockaddr *)&ss);
	if (error != 0)
		goto fail;

	/* As accept(2): the peer address to addr, its length to *addr2. */
	if (sqe->addr != 0) {
		socklen_t len;

		error = copyin((const void *)(uintptr_t)sqe->addr2, &len,
		    sizeof(len));
		if (error == 0) {
			len = MIN(len, ss.ss_len);
			error = copyout(&ss, (void *)(uintptr_t)sqe->addr, len);
		}
		if (error == 0) {
			len = ss.ss_len;
			error = copyout(&len, (void *)(uintptr_t)sqe->addr2,
			    sizeof(len));
		}
		if (error != 0)
			goto fail;
	}

	if (direct) {
		/* The fixed table takes nfp's reference and hcaps. */
		res = kqring_fixed_install(ctx, nfp, &hcaps,
		    sqe->file_index);
		nfp = NULL;
	} else {
		filecaps_free(&hcaps);
		fdrop(nfp, td);		/* falloc_caps()'s extra reference */
		res = nfd;
	}
	if (res >= 0 && (sqe->ioprio & IORING_ACCEPT_MULTISHOT) != 0) {
		*more_out = !TAILQ_EMPTY(&head->sol_comp);
		fdrop(headfp, td);
		return (kqring_done(op, res, IORING_CQE_F_MORE));
	}
	fdrop(headfp, td);
	return (kqring_done(op, res, 0));

fail:
	if (nfp != NULL) {
		/* Closes the accepted socket too, if finit() ran. */
		if (!direct)
			fdclose(td, nfp, nfd);
		fdrop(nfp, td);
	}
	filecaps_free(&hcaps);
	fdrop(headfp, td);
	if (error == EWOULDBLOCK)
		return (EAGAIN);
	return (kqring_done(op, -error, 0));
}

/*
 * Socket readiness without fo_poll(): when nothing is ready, sopoll() calls
 * selrecord(td, ...), which requires the select/poll per-thread state
 * (td->td_sel) that only exists inside select(2)/poll(2).
 */
static int
kqring_so_revents(struct socket *so, int events)
{
	int revents;

again:
	revents = 0;
	SOCK_LOCK(so);
	if (SOLISTENING(so)) {
		if (!TAILQ_EMPTY(&so->sol_comp) || so->so_error != 0)
			revents |= events & (POLLIN | POLLRDNORM);
		if (so->so_error != 0)
			revents |= POLLERR;
		SOCK_UNLOCK(so);
		return (revents);
	}
	SOCK_UNLOCK(so);

	/* A racing listen(2) reuses the sockbufs; see kqring_sb_disarm(). */
	SOCK_RECVBUF_LOCK(so);
	if (SOLISTENING(so)) {
		SOCK_RECVBUF_UNLOCK(so);
		goto again;
	}
	bool rd_eof = (so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0;
	if (sbavail(&so->so_rcv) >= MAX(so->so_rcv.sb_lowat, 1) || rd_eof ||
	    so->so_error != 0)
		revents |= events & (POLLIN | POLLRDNORM);
	SOCK_RECVBUF_UNLOCK(so);

	SOCK_SENDBUF_LOCK(so);
	if (SOLISTENING(so)) {
		SOCK_SENDBUF_UNLOCK(so);
		goto again;
	}
	bool wr_eof = (so->so_snd.sb_state & SBS_CANTSENDMORE) != 0;
	if ((sbspace(&so->so_snd) >= so->so_snd.sb_lowat &&
	    (so->so_state & SS_ISCONNECTED) != 0) || wr_eof ||
	    so->so_error != 0)
		revents |= events & (POLLOUT | POLLWRNORM);
	SOCK_SENDBUF_UNLOCK(so);

	if (so->so_error != 0)
		revents |= POLLERR;
	if (rd_eof && wr_eof)
		revents |= POLLHUP;
	return (revents);
}

static struct seltd *
kqring_seltd_ensure(struct thread *td)
{
	if (td->td_sel == NULL) {
		struct timespec zts = { 0, 0 };

		(void)kern_poll_kfds(td, NULL, 0, &zts, NULL);
	}
	return (td->td_sel);
}

static int
kqring_fo_poll_nonblock(struct file *fp, int events, struct thread *td)
{
	struct seltd *stp;
	int revents;

	stp = kqring_seltd_ensure(td);
	stp->st_flags = KQRING_SELTD_RESCAN;
	revents = fo_poll(fp, events, td->td_ucred, td);
	stp->st_flags = 0;
	if ((revents & POLLHUP) != 0)
		revents &= ~(POLLOUT | POLLWRNORM);
	return (revents);
}

static void
kqring_selfd_alloc(struct seltd *stp)
{
	if (stp->st_free1 == NULL)
		stp->st_free1 = malloc(sizeof(*stp->st_free1), M_SELFD,
		    M_WAITOK | M_ZERO);
	stp->st_free1->sf_td = stp;
	stp->st_free1->sf_cookie = NULL;
	if (stp->st_free2 == NULL)
		stp->st_free2 = malloc(sizeof(*stp->st_free2), M_SELFD,
		    M_WAITOK | M_ZERO);
	stp->st_free2->sf_td = stp;
	stp->st_free2->sf_cookie = NULL;
}

static void
kqring_seltd_clear(struct seltd *stp)
{
	struct selfd *sfp, *sfn;

	STAILQ_FOREACH_SAFE(sfp, &stp->st_selq, sf_link, sfn) {
		STAILQ_REMOVE(&stp->st_selq, sfp, selfd, sf_link);
		if (atomic_load_acq_ptr((uintptr_t *)&sfp->sf_si) !=
		    (uintptr_t)NULL) {
			mtx_lock(sfp->sf_mtx);
			if (sfp->sf_si != NULL)
				TAILQ_REMOVE(&sfp->sf_si->si_tdlist, sfp,
				    sf_threads);
			mtx_unlock(sfp->sf_mtx);
		}
		free(sfp, M_SELFD);
	}
	stp->st_flags = 0;
}

static int
kqring_seltd_wait(struct kqring_ctx *ctx, struct kqring_op *op,
    struct seltd *stp)
{
	int error = 0;

	mtx_lock(&stp->st_mtx);
	stp->st_flags |= KQRING_SELTD_RESCAN;
	if ((stp->st_flags & KQRING_SELTD_PENDING) == 0) {
		if (op->wq_canceled || op->cancelled || ctx->closing)
			error = ECANCELED;
		else
			error = cv_wait_sig(&stp->st_wait, &stp->st_mtx);
	}
	mtx_unlock(&stp->st_mtx);
	return (error);
}

void
kqring_wq_poll_wait(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct seltd *stp;
	struct file *fp = NULL;
	cap_rights_t rights;
	bool is_poll = sqe->opcode == IORING_OP_POLL_ADD;
	bool multi = is_poll && (sqe->len & IORING_POLL_ADD_MULTI) != 0;
	bool level = is_poll && (sqe->len & IORING_POLL_ADD_LEVEL) != 0;
	bool no_selq;
	int error, events, revents;

	stp = kqring_seltd_ensure(td);
	for (;;) {
		if (op->wq_canceled || op->cancelled || ctx->closing)
			break;
		if (is_poll) {
			events = sqe->poll32_events ? (int)sqe->poll32_events :
			    sqe->poll_events;
			error = kqring_op_fget(op,
			    cap_rights_init_one(&rights, CAP_POLL_EVENT), 0,
			    &fp);
		} else {
			events = POLLIN;
			error = kqring_op_fget(op,
			    cap_rights_init_one(&rights, CAP_READ), FREAD, &fp);
		}
		if (error != 0) {
			kqring_wq_mark_blocking(ctx, op);
			break;
		}
		kqring_selfd_alloc(stp);
		revents = fo_poll(fp, events, td->td_ucred, td);
		if ((revents & POLLHUP) != 0)
			revents &= ~(POLLOUT | POLLWRNORM);
		if (revents != 0) {
			kqring_seltd_clear(stp);
			if (!multi) {
				fdrop(fp, td);
				kqring_wq_mark_blocking(ctx, op);
				break;
			}
			if (!level) {
				kqring_selfd_alloc(stp);
				(void)fo_poll(fp, POLLINIGNEOF, td->td_ucred,
				    td);
			}
			no_selq = STAILQ_EMPTY(&stp->st_selq);
			fdrop(fp, td);
			mtx_lock(&ctx->mtx);
			if (!op->cancelled && !op->wq_canceled &&
			    !ctx->closing) {
				kqring_taskwork_add_locked(ctx, op);
				if (ctx->sleeping)
					wakeup(&ctx->wakeup_chan);
			}
			mtx_unlock(&ctx->mtx);
			kqring_wq_mark_blocking(ctx, op);
			if (level || no_selq) {
				kqring_seltd_clear(stp);
				mtx_lock(&ctx->mtx);
				while (op->in_taskwork && !op->cancelled &&
				    !op->wq_canceled && !ctx->closing)
					msleep_sbt(&op->in_taskwork, &ctx->mtx,
					    PCATCH, "kqwqlv", SBT_1MS, 0, 0);
				mtx_unlock(&ctx->mtx);
				if (no_selq && !level)
					break;
				continue;
			}
		} else {
			no_selq = STAILQ_EMPTY(&stp->st_selq);
			fdrop(fp, td);
			kqring_wq_mark_blocking(ctx, op);
			if (no_selq)
				break;
		}
		error = kqring_seltd_wait(ctx, op, stp);
		kqring_seltd_clear(stp);
		if (error != 0)
			break;
		if (multi) {
			mtx_lock(&ctx->mtx);
			while (op->in_taskwork && !op->cancelled &&
			    !op->wq_canceled && !ctx->closing)
				msleep_sbt(&op->in_taskwork, &ctx->mtx, PCATCH,
				    "kqwqtw", SBT_1MS, 0, 0);
			mtx_unlock(&ctx->mtx);
		}
	}
	kqring_seltd_clear(stp);
}

int
kqring_exec_poll(struct kqring_op *op, struct socket *expect)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct file *fp = NULL;
	cap_rights_t rights;
	int error, revents;
	int events = sqe->poll32_events ? (int)sqe->poll32_events :
	    sqe->poll_events;

	error = kqring_op_fget(op, cap_rights_init_one(&rights, CAP_POLL_EVENT),
	    0, &fp);
	if (error != 0)
		return (kqring_done(op, -error, 0));
	if (fp->f_type == DTYPE_SOCKET) {
		if (expect != NULL && fp->f_data != expect) {
			fdrop(fp, td);
			return (kqring_done(op, -EBADF, 0));
		}
#ifdef KQRING_MAC
		error = mac_socket_check_poll(td->td_ucred, fp->f_data);
		if (error != 0) {
			fdrop(fp, td);
			return (kqring_done(op, -error, 0));
		}
#endif
		revents = kqring_so_revents(fp->f_data, events);
	} else {
		revents = kqring_fo_poll_nonblock(fp, events, td);
	}
	fdrop(fp, td);

	if (revents == 0)
		return (EAGAIN);
	return (kqring_done(op, revents,
	    (sqe->len & IORING_POLL_ADD_MULTI) != 0 ? IORING_CQE_F_MORE : 0));
}


/*
 * URING_CMD on a socket: SIOCINQ, SIOCOUTQ, getsockopt, setsockopt, with
 * the Capsicum checks ioctl(2) and kern_getsockopt() make.
 */
int32_t
kqring_exec_uring_cmd(const struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct filecaps caps;
	struct sockopt sopt;
	struct file *fp;
	cap_rights_t rights;
	u_long cmd;
	int error, n;

	switch (sqe->cmd_op) {
	case SOCKET_URING_OP_SIOCINQ:
	case SOCKET_URING_OP_SIOCOUTQ:
		cap_rights_init_one(&rights, CAP_IOCTL);
		break;
	case SOCKET_URING_OP_GETSOCKOPT:
		/* As in io_uring, only SOL_SOCKET options. */
		if (sqe->level != SOL_SOCKET)
			return (-EOPNOTSUPP);
		cap_rights_init_one(&rights, CAP_GETSOCKOPT);
		break;
	case SOCKET_URING_OP_SETSOCKOPT:
		cap_rights_init_one(&rights, CAP_SETSOCKOPT);
		break;
	default:
		return (-EOPNOTSUPP);
	}
	error = kqring_op_fget_caps(op, &rights, &fp, &caps);
	if (error != 0)
		return (-error);
	/* No other file type implements commands. */
	if (fp->f_type != DTYPE_SOCKET) {
		error = EOPNOTSUPP;
		goto out;
	}

	switch (sqe->cmd_op) {
	case SOCKET_URING_OP_SIOCINQ:
	case SOCKET_URING_OP_SIOCOUTQ:
		cmd = sqe->cmd_op == SOCKET_URING_OP_SIOCINQ ? FIONREAD :
		    FIONWRITE;
		if (caps.fc_nioctls != -1) {
			int i;

			for (i = 0; i < caps.fc_nioctls; i++)
				if (caps.fc_ioctls[i] == cmd)
					break;
			if (i == caps.fc_nioctls) {
				error = ENOTCAPABLE;
				goto out;
			}
		}
		error = fo_ioctl(fp, cmd, (caddr_t)&n, td->td_ucred, td);
		if (error == 0) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return (n);
		}
		break;
	default:
		bzero(&sopt, sizeof(sopt));
		sopt.sopt_dir = sqe->cmd_op == SOCKET_URING_OP_GETSOCKOPT ?
		    SOPT_GET : SOPT_SET;
		sopt.sopt_level = (int)sqe->level;
		sopt.sopt_name = (int)sqe->optname;
		sopt.sopt_val = (void *)(uintptr_t)sqe->optval;
		sopt.sopt_valsize = sqe->optlen;
		sopt.sopt_td = td;
		sopt.sopt_rights = &caps.fc_rights;
		/* The checks of kern_getsockopt() / kern_setsockopt(). */
		if (sopt.sopt_dir == SOPT_GET) {
			if (sopt.sopt_val == NULL)
				sopt.sopt_valsize = 0;
			else if ((int)sqe->optlen < 0)
				error = EINVAL;
			if (error == 0)
				error = sogetopt(fp->f_data, &sopt);
		} else {
			if (sopt.sopt_val == NULL && sqe->optlen != 0)
				error = EFAULT;
			else if ((int)sqe->optlen < 0)
				error = EINVAL;
			if (error == 0)
				error = sosetopt(fp->f_data, &sopt);
		}
		if (error == 0 && sopt.sopt_dir == SOPT_GET) {
			filecaps_free(&caps);
			fdrop(fp, td);
			return ((int32_t)MIN(sopt.sopt_valsize, INT32_MAX));
		}
		break;
	}
out:
	filecaps_free(&caps);
	fdrop(fp, td);
	return (-error);
}

/* The socket of a synchronous op (BIND, LISTEN, SHUTDOWN). */
static int
kqring_op_sock(const struct kqring_op *op, cap_rights_t *rights,
    struct file **fpp)
{
	return (kqring_getsock(op, NULL, rights, fpp));
}

int32_t
kqring_exec_bind(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct sockaddr *sa = op->args;
	struct file *fp;
	cap_rights_t rights;
	int error;

	/* As bind(2): only with a directory descriptor in capability mode. */
	if (IN_CAPABILITY_MODE(td))
		return (-ECAPMODE);
	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_BIND), &fp);
	if (error != 0)
		return (-error);
#ifdef KQRING_MAC
	error = mac_socket_check_bind(td->td_ucred, fp->f_data, sa);
	if (error == 0)
#endif
		error = sobind(fp->f_data, sa, td);
	fdrop(fp, td);
	return (-error);
}

int32_t
kqring_exec_listen(const struct kqring_op *op)
{
	struct thread *td = curthread;
	struct file *fp;
	cap_rights_t rights;
	int error;

	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_LISTEN),
	    &fp);
	if (error != 0)
		return (-error);
#ifdef KQRING_MAC
	error = mac_socket_check_listen(td->td_ucred, fp->f_data);
	if (error == 0)
#endif
		error = solisten(fp->f_data, (int)op->sqe.len, td);
	fdrop(fp, td);
	return (-error);
}

int32_t
kqring_exec_shutdown(const struct kqring_op *op)
{
	struct file *fp;
	cap_rights_t rights;
	int how = (int)op->sqe.len, error;

	if (how < SHUT_RD || how > SHUT_RDWR)
		return (-EINVAL);
	error = kqring_op_sock(op, cap_rights_init_one(&rights, CAP_SHUTDOWN),
	    &fp);
	if (error != 0)
		return (-error);
	error = soshutdown(fp->f_data, how);
	fdrop(fp, curthread);
	return (-error);
}

/*
 * SOCKET: socket(2), or with file_index kern_socket() without a
 * descriptor, into the fixed table.
 */
int32_t
kqring_exec_socket(struct kqring_ctx *ctx, struct kqring_op *op)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	struct thread *td = curthread;
	struct filecaps caps;
	struct socket *so;
	struct file *fp;
	int domain = sqe->fd, type = (int)sqe->off, proto = (int)sqe->len;
	int error, fflag = 0;

	if (sqe->file_index == 0) {
		error = kern_socket(td, domain, type, proto);
		return (error == 0 ? (int32_t)td->td_retval[0] : -error);
	}
	/* Close-on-exec/fork mean nothing without a descriptor. */
	type &= ~SOCK_CLOFORK;
	if ((type & SOCK_NONBLOCK) != 0) {
		type &= ~SOCK_NONBLOCK;
		fflag |= FNONBLOCK;
	}
#ifdef KQRING_MAC
	error = mac_socket_check_create(td->td_ucred, domain, type, proto);
	if (error != 0)
		return (-error);
#endif
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (-error);
	error = socreate(domain, &so, type, proto, td->td_ucred, td);
	if (error != 0) {
		fdrop(fp, td);
		return (-error);
	}
	finit(fp, FREAD | FWRITE | fflag, DTYPE_SOCKET, so, &socketops);
	if ((fflag & FNONBLOCK) != 0)
		(void)fo_ioctl(fp, FIONBIO, &fflag, td->td_ucred, td);
	kqring_caps_all(&caps);
	if (op->wq_running) {
		op->wq_fp = fp;
		filecaps_move(&caps, &op->wq_caps);
		return (0);
	}
	return (kqring_fixed_install(ctx, fp, &caps, sqe->file_index));
}


/* ============================================================================
 * Arming Asynchronous Socket & Futex Operations
 * ============================================================================
 */

/*
 * Install op's upcall on one socket buffer and OR its readiness into *ready.
 * A sockbuf has a single upcall slot that soupcall_set() silently
 * overwrites, so fail with EBUSY if it is taken (another op armed on the
 * same socket and direction, on this ring or another).  ERESTART means the
 * socket became a listener; see kqring_sb_disarm().
 */
static int
kqring_sb_arm(struct kqring_op *op, struct socket *so, int which, bool *ready)
{
	struct sockbuf *sb;

	SOCK_BUF_LOCK(so, which);
	if (SOLISTENING(so)) {
		SOCK_BUF_UNLOCK(so, which);
		return (ERESTART);
	}
	sb = kqring_sb(so, which);
	if (sb->sb_upcall != NULL) {
		SOCK_BUF_UNLOCK(so, which);
		return (EBUSY);
	}
	soupcall_set(so, which, kqring_soupcall, op);
	if (which == SO_RCV) {
		op->dirs |= KQ_DIR_RCV;
		if (sbavail(sb) > 0 || (sb->sb_state & SBS_CANTRCVMORE) != 0 ||
		    so->so_error != 0)
			*ready = true;
	} else {
		op->dirs |= KQ_DIR_SND;
		if (sbspace(sb) > 0 || (sb->sb_state & SBS_CANTSENDMORE) != 0 ||
		    so->so_error != 0)
			*ready = true;
	}
	SOCK_BUF_UNLOCK(so, which);
	return (0);
}

/* Socket buffer directions an op waits on. */
int
kqring_op_dirs(const struct io_uring_sqe *sqe)
{
	uint32_t events;
	int dirs = 0;

	switch (sqe->opcode) {
	case IORING_OP_SEND:
	case IORING_OP_SEND_ZC:
	case IORING_OP_SENDMSG:
	case IORING_OP_SENDMSG_ZC:
	case IORING_OP_WRITE:
	case IORING_OP_WRITEV:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITEV_FIXED:
	case IORING_OP_CONNECT:
		/* soisconnected() / soisdisconnected() also wake so_snd. */
		return (KQ_DIR_SND);
	case IORING_OP_POLL_ADD:
		/*
		 * Watch every direction asked for; anything but POLLOUT
		 * (POLLIN, POLLHUP, ...) shows up on so_rcv.
		 */
		events = sqe->poll32_events ? sqe->poll32_events :
		    sqe->poll_events;
		if ((events & (POLLOUT | POLLWRNORM)) != 0)
			dirs |= KQ_DIR_SND;
		if ((events & ~(uint32_t)(POLLOUT | POLLWRNORM)) != 0 ||
		    dirs == 0)
			dirs |= KQ_DIR_RCV;
		return (dirs);
	default:
		return (KQ_DIR_RCV);
	}
}

/*
 * Arm op on its socket for the KQ_DIR_* directions in `dirs` (ignored for
 * listening sockets).  Returns true if armed; on failure op is completed
 * with an error CQE.
 */
bool
kqring_arm_socket_op(struct kqring_ctx *ctx, struct kqring_op *op, int dirs)
{
	struct thread *td = curthread;
	struct file *fp = NULL;
	struct socket *so;
	cap_rights_t rights;
	int error;

	if (op->sock_fp != NULL) {
		/* Kept by an attempt that would have blocked. */
		so = op->sock_fp->f_data;
		soref(so);
	} else {
		error = kqring_op_fget(op,
		    cap_rights_init_one(&rights, CAP_EVENT), 0, &fp);
		if (error != 0) {
			kqring_req_complete(ctx, op, -error, 0);
			return (false);
		}
		if (fp->f_type != DTYPE_SOCKET) {
			fdrop(fp, td);
			kqring_req_complete(ctx, op, -ENOTSOCK, 0);
			return (false);
		}
		so = fp->f_data;
		soref(so);
		fdrop(fp, td);
	}

	/* A re-armed op (POLL_REMOVE update) was cancelled when unarmed. */
	op->cancelled = false;
	op->listening = false;
	op->so = so;

	/*
	 * Install the upcall(s), then check whether the socket became ready
	 * before they were installed.  Listening sockets have no so_rcv/so_snd
	 * (that storage is reused for the listen queues), so they use the
	 * dedicated listen upcall under the listen lock.
	 */
	bool ready_now;
retry:
	ready_now = false;
	error = 0;
	SOCK_LOCK(so);
	if (SOLISTENING(so)) {
		if (so->sol_upcall != NULL) {
			error = EBUSY;
		} else {
			op->listening = true;
			solisten_upcall_set(so, kqring_soupcall, op);
			ready_now = !TAILQ_EMPTY(&so->sol_comp) ||
			    so->so_error != 0;
		}
		SOCK_UNLOCK(so);
	} else {
		SOCK_UNLOCK(so);
		if ((dirs & KQ_DIR_RCV) != 0)
			error = kqring_sb_arm(op, so, SO_RCV, &ready_now);
		if (error == 0 && (dirs & KQ_DIR_SND) != 0)
			error = kqring_sb_arm(op, so, SO_SND, &ready_now);
		if (error == ERESTART) {
			/* solisten_proto() wiped any upcall we installed. */
			op->dirs = 0;
			goto retry;
		}
	}

	if (error != 0) {
		/*
		 * An upcall installed before the failure may already have
		 * queued op as taskwork; cancelling under ctx->mtx stops
		 * that, and disarm unqueues it and drops the socket.
		 */
		mtx_lock(&ctx->mtx);
		op->cancelled = true;
		kqring_op_disarm_locked(op);
		mtx_unlock(&ctx->mtx);
		kqring_req_complete(ctx, op, -error, 0);
		return (false);
	}
	op->polled = (op->dirs & KQ_DIR_SND) != 0 &&
	    so->so_proto->pr_domain->dom_family == AF_LOCAL &&
	    so->so_type != SOCK_DGRAM;

	mtx_lock(&ctx->mtx);
	kqring_op_arm_locked(ctx, op);
	if (op->polled)
		ctx->npolled++;
	if (ready_now)
		kqring_taskwork_add_locked(ctx, op);
	mtx_unlock(&ctx->mtx);
	return (true);
}
