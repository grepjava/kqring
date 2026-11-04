/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * Reproducer: shutdown(SHUT_WR) on an AF_UNIX stream socket doesn't run
 * the peer's receive-buffer upcall (soupcall_set(SO_RCV)), though it
 * marks the peer's receive side SBS_CANTRCVMORE and wakes select, poll
 * and kqueue.  uipc_cantrcvmore() sets the flag and calls selwakeuppri()
 * and KNOTE_LOCKED() itself, not sorwakeup_locked(), so sowakeup() and its
 * SB_UPCALL handling never run.  Data does reach the upcall (control).
 *
 * Loading the module runs the test once and prints the result:
 *
 *	make && kldload ./unix_shutdown_upcall.ko && kldunload unix_shutdown_upcall
 *	dmesg | tail -4
 *
 * The load fails with EIO if the bug is present, so it is not left loaded.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/uio.h>

static volatile int upcalls;

static int
count_upcall(struct socket *so __unused, void *arg __unused,
    int waitflag __unused)
{
	upcalls++;
	return (SU_OK);
}

static int
run_test(void)
{
	struct thread *td = curthread;
	struct socket *a = NULL, *b = NULL;
	char byte = 'x';
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct uio uio = {
		.uio_iov = &iov, .uio_iovcnt = 1, .uio_resid = 1,
		.uio_segflg = UIO_SYSSPACE, .uio_rw = UIO_WRITE, .uio_td = td,
	};
	int after_data, after_shut, eof, error;

	error = socreate(PF_LOCAL, &a, SOCK_STREAM, 0, td->td_ucred, td);
	if (error == 0)
		error = socreate(PF_LOCAL, &b, SOCK_STREAM, 0, td->td_ucred,
		    td);
	if (error == 0)
		error = soconnect2(a, b);	/* as kern_socketpair() */
	if (error != 0) {
		printf("unix_shutdown_upcall: setup failed: %d\n", error);
		goto out;
	}

	SOCK_RECVBUF_LOCK(b);
	soupcall_set(b, SO_RCV, count_upcall, NULL);
	SOCK_RECVBUF_UNLOCK(b);

	/* Control: data written by the peer runs b's upcall. */
	error = sosend(a, NULL, &uio, NULL, NULL, 0, td);
	after_data = upcalls;

	/* The bug: the peer shutting down writing doesn't. */
	if (error == 0)
		error = soshutdown(a, SHUT_WR);
	after_shut = upcalls;

	SOCK_RECVBUF_LOCK(b);
	eof = (b->so_rcv.sb_state & SBS_CANTRCVMORE) != 0;
	soupcall_clear(b, SO_RCV);
	SOCK_RECVBUF_UNLOCK(b);

	if (error != 0) {
		printf("unix_shutdown_upcall: sosend/soshutdown failed: %d\n",
		    error);
		goto out;
	}
	printf("unix_shutdown_upcall: upcalls after data: %d (expect >= 1)\n",
	    after_data);
	printf("unix_shutdown_upcall: after peer SHUT_WR: %d new, "
	    "SBS_CANTRCVMORE %s\n", after_shut - after_data,
	    eof ? "set" : "not set");
	if (after_data >= 1 && eof && after_shut == after_data) {
		printf("unix_shutdown_upcall: BUG: SHUT_WR set SBS_CANTRCVMORE "
		    "but ran no SO_RCV upcall\n");
		error = EIO;
	} else
		printf("unix_shutdown_upcall: OK\n");
out:
	if (a != NULL)
		soclose(a);
	if (b != NULL)
		soclose(b);
	return (error);
}

static int
unix_shutdown_upcall_modevent(module_t mod __unused, int type,
    void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		return (run_test());
	case MOD_UNLOAD:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t unix_shutdown_upcall_mod = {
	"unix_shutdown_upcall", unix_shutdown_upcall_modevent, NULL
};
DECLARE_MODULE(unix_shutdown_upcall, unix_shutdown_upcall_mod,
    SI_SUB_EXEC, SI_ORDER_ANY);
