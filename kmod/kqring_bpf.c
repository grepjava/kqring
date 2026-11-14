/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

/*
 * Classic BPF filters on requests (IORING_REGISTER_BPF_FILTER), and
 * restrictions registered for a process rather than a ring
 * (IORING_REGISTER_RESTRICTIONS with no ring), as Linux 7.3 has them.
 *
 * A filter is a classic BPF program for one request opcode.  It runs as
 * each such request is submitted, after the request has been checked, on
 * a struct io_uring_bpf_ctx: the request's user_data, opcode and flags,
 * and for SOCKET, OPENAT, OPENAT2 and CONNECT their arguments.  A result
 * of 0 fails the request with -EACCES; anything else lets it through.  An
 * opcode's filters all run, newest first, and the first 0 decides.  The
 * programs are checked as Linux checks them (bpf_check_classic() and
 * io_uring_check_cbpf_filter()): forward jumps only, a RET last, scratch
 * memory written before it is read, and of the loads only aligned 32-bit
 * words of the context and its length.  kqring runs them itself rather
 * than through bpf_filter(9), which loads words in network byte order;
 * io_uring's filters load them in host order.
 *
 * Both kinds can be registered for a process, with no ring: they then
 * apply to every ring it, and every process it forks afterwards, sets up.
 * That needs PROC_NO_NEW_PRIVS_CTL (procctl(2)), which, as Linux's
 * no_new_privs, keeps a set-ID program from running under restrictions
 * its caller chose, or root.  A process may register restrictions once;
 * filters it may add at any time, but never remove, and neither a ring
 * nor a child can loosen what it inherits.  Linux keeps these per thread;
 * kqring keeps them per process, as FreeBSD's no_new_privs is.  They
 * survive execve(2), as no_new_privs does.
 *
 * kqring's extension requests (an escape opcode the ring opted in to) are
 * refused on a ring with filters: they could run what the filters deny.
 */

#include "kqring_var.h"

#include <sys/sx.h>

#ifndef BPF_MOD
#define	BPF_MOD		0x90
#endif
#ifndef BPF_XOR
#define	BPF_XOR		0xa0
#endif

/* Linux's BPF_MAXINSNS; FreeBSD's is 512. */
#define	KQRING_BPF_MAXINSNS	4096
#define	KQRING_BPF_CTX_SIZE	((uint32_t)sizeof(struct io_uring_bpf_ctx))

/*
 * What a process registered.  Found by process, under kqring_trst_sx;
 * kqring_trst_count lets the common case (none at all) skip the lock.
 */
struct kqring_task_rst {
	LIST_ENTRY(kqring_task_rst) link;
	struct proc		*proc;
	struct kqring_restrictions rst;
	bool			has_rst;	/* restrictions registered */
	struct kqring_bpf_table	*bpf;
};

#define	KQRING_TRST_HASH	64
static LIST_HEAD(, kqring_task_rst) kqring_trst_hash[KQRING_TRST_HASH];
static struct sx kqring_trst_sx;
SX_SYSINIT(kqring_trst, &kqring_trst_sx, "kqring task restrictions");
static volatile u_int kqring_trst_count;

/* IO_URING_BPF_FILTER_DENY_REST's placeholder: never freed, not counted. */
static struct kqring_bpf_node kqring_bpf_deny;

#define	KQRING_TRST_BUCKET(p)	\
	(&kqring_trst_hash[(u_int)(p)->p_pid % KQRING_TRST_HASH])

static struct kqring_task_rst *
kqring_trst_find(const struct proc *p)
{
	struct kqring_task_rst *tr;

	sx_assert(&kqring_trst_sx, SA_LOCKED);
	LIST_FOREACH(tr, KQRING_TRST_BUCKET(p), link) {
		if (tr->proc == p)
			return (tr);
	}
	return (NULL);
}

static void
kqring_trst_insert(struct kqring_task_rst *tr, struct proc *p)
{
	sx_assert(&kqring_trst_sx, SA_XLOCKED);
	tr->proc = p;
	LIST_INSERT_HEAD(KQRING_TRST_BUCKET(p), tr, link);
	atomic_add_int(&kqring_trst_count, 1);
}

/* ------------------------------------------------------------------------
 * Filter lists and tables.
 * ------------------------------------------------------------------------
 */

static void
kqring_bpf_node_ref(struct kqring_bpf_node *n)
{
	if (n != NULL && n != &kqring_bpf_deny)
		refcount_acquire(&n->refs);
}

static void
kqring_bpf_node_rele(struct kqring_bpf_node *n)
{
	struct kqring_bpf_node *next;

	while (n != NULL && n != &kqring_bpf_deny &&
	    refcount_release(&n->refs)) {
		next = n->next;
		free(n, M_KQRING);
		n = next;
	}
}

static void
kqring_bpf_table_rele(struct kqring_bpf_table *t)
{
	if (t == NULL || !refcount_release(&t->refs))
		return;
	for (u_int i = 0; i < IORING_OP_LAST; i++)
		kqring_bpf_node_rele(t->op[i]);
	free(t, M_KQRING);
}

/*
 * Prepend filter n to opcode's list in *tp, and with deny_rest put the
 * placeholder on every opcode that has no filter.  A table someone else
 * shares is copied first, and *oldp gets the one replaced (a reference the
 * caller drops, or a ring keeps); otherwise the table changes in place,
 * one pointer at a time, which a submission reading it without a lock
 * sees either way.  Takes n's reference.
 */
static void
kqring_bpf_table_add(struct kqring_bpf_table **tp, struct kqring_bpf_node *n,
    uint32_t opcode, bool deny_rest, struct kqring_bpf_table **oldp)
{
	struct kqring_bpf_table *t = *tp, *nt;

	*oldp = NULL;
	if (t == NULL || atomic_load_int(&t->refs) > 1) {
		nt = malloc(sizeof(*nt), M_KQRING, M_WAITOK | M_ZERO);
		refcount_init(&nt->refs, 1);
		if (t != NULL) {
			for (u_int i = 0; i < IORING_OP_LAST; i++) {
				nt->op[i] = t->op[i];
				kqring_bpf_node_ref(nt->op[i]);
			}
		}
		*oldp = t;
		t = nt;
	}
	n->next = t->op[opcode];
	atomic_store_rel_ptr((volatile uintptr_t *)&t->op[opcode],
	    (uintptr_t)n);
	if (deny_rest) {
		for (u_int i = 0; i < IORING_OP_LAST; i++) {
			if (t->op[i] == NULL)
				atomic_store_rel_ptr(
				    (volatile uintptr_t *)&t->op[i],
				    (uintptr_t)&kqring_bpf_deny);
		}
	}
	if (t != *tp)
		atomic_store_rel_ptr((volatile uintptr_t *)tp, (uintptr_t)t);
}

/* A ring's tables, the current one and those it replaced. */
void
kqring_bpf_ctx_free(struct kqring_ctx *ctx)
{
	struct kqring_bpf_table *t = ctx->bpf, *next;

	ctx->bpf = NULL;
	while (t != NULL) {
		next = t->retired;
		t->retired = NULL;
		kqring_bpf_table_rele(t);
		t = next;
	}
}

/* ------------------------------------------------------------------------
 * Checking and running programs.
 * ------------------------------------------------------------------------
 */

/* Bytes of the context's union an opcode fills in (filter_pdu_size). */
static uint8_t
kqring_bpf_pdu_size(uint32_t opcode)
{
	switch (opcode) {
	case IORING_OP_SOCKET:
		return (sizeof(((struct io_uring_bpf_ctx *)NULL)->socket));
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
		return (sizeof(((struct io_uring_bpf_ctx *)NULL)->open));
	case IORING_OP_CONNECT:
		return (sizeof(((struct io_uring_bpf_ctx *)NULL)->connect));
	default:
		return (0);
	}
}

/*
 * Linux's bpf_check_classic() and check_load_and_stores(), then
 * io_uring_check_cbpf_filter(): only what a filter on the context needs,
 * with the length loads made the context's size.
 */
static int
kqring_cbpf_check(struct bpf_insn *f, u_int flen)
{
	uint16_t *masks, memvalid = 0;
	int error = 0;

	for (u_int pc = 0; pc < flen; pc++) {
		struct bpf_insn *in = &f[pc];

		switch (in->code) {
		case BPF_LD | BPF_W | BPF_ABS:
			if (in->k >= KQRING_BPF_CTX_SIZE || (in->k & 3) != 0)
				return (EINVAL);
			break;
		case BPF_LD | BPF_W | BPF_LEN:
			in->code = BPF_LD | BPF_IMM;
			in->k = KQRING_BPF_CTX_SIZE;
			break;
		case BPF_LDX | BPF_W | BPF_LEN:
			in->code = BPF_LDX | BPF_IMM;
			in->k = KQRING_BPF_CTX_SIZE;
			break;
		case BPF_ALU | BPF_DIV | BPF_K:
			if (in->k == 0)
				return (EINVAL);
			break;
		case BPF_ALU | BPF_LSH | BPF_K:
		case BPF_ALU | BPF_RSH | BPF_K:
			if (in->k >= 32)
				return (EINVAL);
			break;
		case BPF_LD | BPF_MEM:
		case BPF_LDX | BPF_MEM:
		case BPF_ST:
		case BPF_STX:
			if (in->k >= BPF_MEMWORDS)
				return (EINVAL);
			break;
		case BPF_JMP | BPF_JA:
			if (in->k >= flen - pc - 1)
				return (EINVAL);
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JEQ | BPF_X:
		case BPF_JMP | BPF_JGE | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_X:
		case BPF_JMP | BPF_JGT | BPF_K:
		case BPF_JMP | BPF_JGT | BPF_X:
		case BPF_JMP | BPF_JSET | BPF_K:
		case BPF_JMP | BPF_JSET | BPF_X:
			if (pc + in->jt + 1 >= flen || pc + in->jf + 1 >= flen)
				return (EINVAL);
			break;
		case BPF_RET | BPF_K:
		case BPF_RET | BPF_A:
		case BPF_ALU | BPF_ADD | BPF_K:
		case BPF_ALU | BPF_ADD | BPF_X:
		case BPF_ALU | BPF_SUB | BPF_K:
		case BPF_ALU | BPF_SUB | BPF_X:
		case BPF_ALU | BPF_MUL | BPF_K:
		case BPF_ALU | BPF_MUL | BPF_X:
		case BPF_ALU | BPF_DIV | BPF_X:
		case BPF_ALU | BPF_AND | BPF_K:
		case BPF_ALU | BPF_AND | BPF_X:
		case BPF_ALU | BPF_OR | BPF_K:
		case BPF_ALU | BPF_OR | BPF_X:
		case BPF_ALU | BPF_XOR | BPF_K:
		case BPF_ALU | BPF_XOR | BPF_X:
		case BPF_ALU | BPF_LSH | BPF_X:
		case BPF_ALU | BPF_RSH | BPF_X:
		case BPF_ALU | BPF_NEG:
		case BPF_LD | BPF_IMM:
		case BPF_LDX | BPF_IMM:
		case BPF_MISC | BPF_TAX:
		case BPF_MISC | BPF_TXA:
			break;
		default:
			/* MOD, byte and halfword loads, IND, MSH. */
			return (EINVAL);
		}
	}
	if (f[flen - 1].code != (BPF_RET | BPF_K) &&
	    f[flen - 1].code != (BPF_RET | BPF_A))
		return (EINVAL);

	/* Scratch memory is read only where every path wrote it. */
	masks = malloc(flen * sizeof(*masks), M_KQRING, M_WAITOK);
	memset(masks, 0xff, flen * sizeof(*masks));
	for (u_int pc = 0; pc < flen && error == 0; pc++) {
		const struct bpf_insn *in = &f[pc];

		memvalid &= masks[pc];
		switch (in->code) {
		case BPF_ST:
		case BPF_STX:
			memvalid |= 1 << in->k;
			break;
		case BPF_LD | BPF_MEM:
		case BPF_LDX | BPF_MEM:
			if ((memvalid & (1 << in->k)) == 0)
				error = EINVAL;
			break;
		case BPF_JMP | BPF_JA:
			masks[pc + 1 + in->k] &= memvalid;
			memvalid = 0xffff;
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JEQ | BPF_X:
		case BPF_JMP | BPF_JGE | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_X:
		case BPF_JMP | BPF_JGT | BPF_K:
		case BPF_JMP | BPF_JGT | BPF_X:
		case BPF_JMP | BPF_JSET | BPF_K:
		case BPF_JMP | BPF_JSET | BPF_X:
			masks[pc + 1 + in->jt] &= memvalid;
			masks[pc + 1 + in->jf] &= memvalid;
			memvalid = 0xffff;
			break;
		}
	}
	free(masks, M_KQRING);
	return (error);
}

/*
 * Run a checked program.  Jumps go forward and it ends in a RET, so it
 * ends.  As Linux runs classic programs, a division by an X of 0 returns
 * 0, and shifts by X take its low 5 bits.
 */
static uint32_t
kqring_cbpf_run(const struct bpf_insn *pc, const struct io_uring_bpf_ctx *c)
{
	uint32_t A = 0, X = 0, M[BPF_MEMWORDS];

	for (;; pc++) {
		switch (pc->code) {
		case BPF_RET | BPF_K:
			return (pc->k);
		case BPF_RET | BPF_A:
			return (A);
		case BPF_LD | BPF_W | BPF_ABS:
			memcpy(&A, (const char *)c + pc->k, sizeof(A));
			break;
		case BPF_LD | BPF_IMM:
			A = pc->k;
			break;
		case BPF_LDX | BPF_IMM:
			X = pc->k;
			break;
		case BPF_LD | BPF_MEM:
			A = M[pc->k];
			break;
		case BPF_LDX | BPF_MEM:
			X = M[pc->k];
			break;
		case BPF_ST:
			M[pc->k] = A;
			break;
		case BPF_STX:
			M[pc->k] = X;
			break;
		case BPF_MISC | BPF_TAX:
			X = A;
			break;
		case BPF_MISC | BPF_TXA:
			A = X;
			break;
		case BPF_ALU | BPF_ADD | BPF_K:
			A += pc->k;
			break;
		case BPF_ALU | BPF_ADD | BPF_X:
			A += X;
			break;
		case BPF_ALU | BPF_SUB | BPF_K:
			A -= pc->k;
			break;
		case BPF_ALU | BPF_SUB | BPF_X:
			A -= X;
			break;
		case BPF_ALU | BPF_MUL | BPF_K:
			A *= pc->k;
			break;
		case BPF_ALU | BPF_MUL | BPF_X:
			A *= X;
			break;
		case BPF_ALU | BPF_DIV | BPF_K:
			A /= pc->k;
			break;
		case BPF_ALU | BPF_DIV | BPF_X:
			if (X == 0)
				return (0);
			A /= X;
			break;
		case BPF_ALU | BPF_AND | BPF_K:
			A &= pc->k;
			break;
		case BPF_ALU | BPF_AND | BPF_X:
			A &= X;
			break;
		case BPF_ALU | BPF_OR | BPF_K:
			A |= pc->k;
			break;
		case BPF_ALU | BPF_OR | BPF_X:
			A |= X;
			break;
		case BPF_ALU | BPF_XOR | BPF_K:
			A ^= pc->k;
			break;
		case BPF_ALU | BPF_XOR | BPF_X:
			A ^= X;
			break;
		case BPF_ALU | BPF_LSH | BPF_K:
			A <<= pc->k;
			break;
		case BPF_ALU | BPF_LSH | BPF_X:
			A <<= X & 31;
			break;
		case BPF_ALU | BPF_RSH | BPF_K:
			A >>= pc->k;
			break;
		case BPF_ALU | BPF_RSH | BPF_X:
			A >>= X & 31;
			break;
		case BPF_ALU | BPF_NEG:
			A = -A;
			break;
		case BPF_JMP | BPF_JA:
			pc += pc->k;
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
			pc += A == pc->k ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JEQ | BPF_X:
			pc += A == X ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JGE | BPF_K:
			pc += A >= pc->k ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JGE | BPF_X:
			pc += A >= X ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JGT | BPF_K:
			pc += A > pc->k ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JGT | BPF_X:
			pc += A > X ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JSET | BPF_K:
			pc += (A & pc->k) != 0 ? pc->jt : pc->jf;
			break;
		case BPF_JMP | BPF_JSET | BPF_X:
			pc += (A & X) != 0 ? pc->jt : pc->jf;
			break;
		default:
			/* kqring_cbpf_check() let nothing else through. */
			return (0);
		}
	}
}

/* The context a request's filters see. */
static void
kqring_bpf_fill(const struct kqring_op *op, uint8_t opcode,
    struct io_uring_bpf_ctx *c)
{
	const struct io_uring_sqe *sqe = &op->sqe;
	const struct kqring_path_args *pa;
	const struct sockaddr *sa;

	memset(c, 0, sizeof(*c));
	c->user_data = sqe->user_data;
	c->opcode = opcode;
	c->sqe_flags = sqe->flags & KQRING_SQE_FLAGS;
	c->pdu_size = kqring_bpf_pdu_size(opcode);
	switch (opcode) {
	case IORING_OP_SOCKET:
		/* Domain, type (with SOCK_NONBLOCK, SOCK_CLOEXEC), protocol. */
		c->socket.family = (uint32_t)sqe->fd;
		c->socket.type = (uint32_t)sqe->off;
		c->socket.protocol = sqe->len;
		break;
	case IORING_OP_OPENAT:
	case IORING_OP_OPENAT2:
		pa = op->args;
		c->open.flags = pa->how.flags;
		c->open.mode = pa->how.mode;
		c->open.resolve = pa->how.resolve;
		break;
	case IORING_OP_CONNECT:
		/*
		 * kqring_prep_sockaddr() copied sa_len bytes, at least up to
		 * the family; the rest only if the family's whole address is
		 * there, so a short one can't pass for another.
		 */
		sa = op->args;
		c->connect.family = sa->sa_family;
		if (sa->sa_family == AF_INET &&
		    sa->sa_len >= sizeof(struct sockaddr_in)) {
			const struct sockaddr_in *sin = (const void *)sa;

			c->connect.port = sin->sin_port;
			c->connect.v4_addr = sin->sin_addr.s_addr;
		} else if (sa->sa_family == AF_INET6 &&
		    sa->sa_len >= sizeof(struct sockaddr_in6)) {
			const struct sockaddr_in6 *sin6 = (const void *)sa;

			c->connect.port = sin6->sin6_port;
			memcpy(c->connect.v6_addr, &sin6->sin6_addr,
			    sizeof(c->connect.v6_addr));
		}
		break;
	}
}

/*
 * At the end of kqring_req_prep(), on a ring with filters: EACCES if any
 * of the request's opcode's filters returns 0.  opcode is the SQE's.
 */
int
kqring_bpf_check(struct kqring_ctx *ctx, const struct kqring_op *op,
    uint8_t opcode, bool ext)
{
	struct kqring_bpf_table *t;
	struct kqring_bpf_node *n;
	struct io_uring_bpf_ctx c;
	bool filled = false;

	t = (void *)atomic_load_acq_ptr((volatile uintptr_t *)&ctx->bpf);
	if (t == NULL)
		return (0);
	if (ext || opcode >= IORING_OP_LAST)
		return (EACCES);
	n = (void *)atomic_load_acq_ptr((volatile uintptr_t *)&t->op[opcode]);
	for (; n != NULL; n = n->next) {
		if (n == &kqring_bpf_deny)
			return (EACCES);
		if (!filled) {
			kqring_bpf_fill(op, opcode, &c);
			filled = true;
		}
		if (kqring_cbpf_run(n->insns, &c) == 0)
			return (EACCES);
	}
	return (0);
}

/* ------------------------------------------------------------------------
 * Registration.
 * ------------------------------------------------------------------------
 */

/* Linux's no_new_privs or CAP_SYS_ADMIN: PROC_NO_NEW_PRIVS_CTL or root. */
static bool
kqring_may_restrict(struct thread *td)
{
	return ((td->td_proc->p_flag2 & P2_NO_NEW_PRIVS) != 0 ||
	    td->td_ucred->cr_uid == 0);
}

/*
 * Restriction entries at uarg, as Linux's io_parse_restrictions(), into a
 * zeroed r.  Opcode and flag entries make requests restricted, register
 * entries registrations; none makes both, to nothing.  On error r may hold
 * part of them.
 */
int
kqring_restrictions_parse(struct kqring_restrictions *r, const void *uarg,
    uint32_t nr)
{
	struct io_uring_restriction res;
	int error;

	if (uarg == NULL ||
	    nr > IORING_RESTRICTION_LAST + IORING_REGISTER_LAST + IORING_OP_LAST)
		return (EINVAL);
	if (nr == 0)
		r->op_registered = r->reg_registered = true;
	for (uint32_t i = 0; i < nr; i++) {
		error = copyin((const char *)uarg + i * sizeof(res), &res,
		    sizeof(res));
		if (error != 0)
			return (error);
		switch (res.opcode) {
		case IORING_RESTRICTION_REGISTER_OP:
			if (res.register_op >= IORING_REGISTER_LAST)
				return (EINVAL);
			kqring_bit_set(r->register_op, res.register_op);
			r->reg_registered = true;
			break;
		case IORING_RESTRICTION_SQE_OP:
			if (res.sqe_op >= IORING_OP_LAST)
				return (EINVAL);
			kqring_bit_set(r->sqe_op, res.sqe_op);
			r->op_registered = true;
			break;
		case IORING_RESTRICTION_SQE_FLAGS_ALLOWED:
			r->sqe_flags_allowed = res.sqe_flags;
			r->op_registered = true;
			break;
		case IORING_RESTRICTION_SQE_FLAGS_REQUIRED:
			r->sqe_flags_required = res.sqe_flags;
			r->op_registered = true;
			break;
		default:
			return (EINVAL);
		}
	}
	return (0);
}

/*
 * IORING_REGISTER_RESTRICTIONS with no ring: once per process (EPERM
 * after restrictions or a filter), and only with no_new_privs or as root
 * (EACCES).  arg is a struct io_uring_task_restriction, nr_args 1.
 */
int
kqring_task_restrictions_register(struct thread *td,
    const struct kqring_register_arg *ra)
{
	struct io_uring_task_restriction hdr;
	struct kqring_task_rst *tr;
	int error;

	tr = malloc(sizeof(*tr), M_KQRING, M_WAITOK | M_ZERO);
	sx_xlock(&kqring_trst_sx);
	if (kqring_trst_find(td->td_proc) != NULL)
		error = EPERM;
	else if (!kqring_may_restrict(td))
		error = EACCES;
	else if (ra->nr_args != 1 || ra->arg == NULL)
		error = EINVAL;
	else if ((error = copyin(ra->arg, &hdr, sizeof(hdr))) != 0)
		;
	else if (hdr.flags != 0 || hdr.resv[0] != 0 || hdr.resv[1] != 0 ||
	    hdr.resv[2] != 0)
		error = EINVAL;
	else
		error = kqring_restrictions_parse(&tr->rst,
		    (const char *)ra->arg + sizeof(hdr), hdr.nr_res);
	if (error == 0) {
		tr->has_rst = true;
		kqring_trst_insert(tr, td->td_proc);
		tr = NULL;
	}
	sx_xunlock(&kqring_trst_sx);
	free(tr, M_KQRING);
	return (error);
}

/*
 * Copy in and check a struct io_uring_bpf, as Linux's
 * io_bpf_filter_import(), and its program.  This kernel's pdu_size for
 * the opcode is written back in any case once the struct is valid;
 * EMSGSIZE if the caller's is larger, or differs under
 * IO_URING_BPF_FILTER_SZ_STRICT.
 */
static int
kqring_bpf_import(const struct kqring_register_arg *ra,
    struct kqring_bpf_node **np, uint32_t *opcodep, bool *deny_restp)
{
	struct io_uring_bpf b;
	struct io_uring_bpf_filter *f = &b.filter;
	struct kqring_bpf_node *n;
	uint8_t ksz;
	bool mismatch;
	int error;

	if ((error = copyin(ra->arg, &b, sizeof(b))) != 0)
		return (error);
	if (b.cmd_type != IO_URING_BPF_CMD_FILTER)
		return (EINVAL);
	if (b.cmd_flags != 0 || b.resv != 0)
		return (EINVAL);
	if (f->opcode >= IORING_OP_LAST)
		return (EINVAL);
	if ((f->flags & ~(uint32_t)(IO_URING_BPF_FILTER_DENY_REST |
	    IO_URING_BPF_FILTER_SZ_STRICT)) != 0)
		return (EINVAL);
	for (u_int i = 0; i < nitems(f->resv); i++)
		if (f->resv[i] != 0)
			return (EINVAL);
	for (u_int i = 0; i < nitems(f->resv2); i++)
		if (f->resv2[i] != 0)
			return (EINVAL);
	if (f->filter_len == 0 || f->filter_len > KQRING_BPF_MAXINSNS)
		return (EINVAL);
	ksz = kqring_bpf_pdu_size(f->opcode);
	mismatch = f->pdu_size != ksz &&
	    ((f->flags & IO_URING_BPF_FILTER_SZ_STRICT) != 0 ||
	    f->pdu_size > ksz);
	error = copyout(&ksz, (char *)ra->arg +
	    offsetof(struct io_uring_bpf, filter.pdu_size), sizeof(ksz));
	if (error != 0)
		return (error);
	if (mismatch)
		return (EMSGSIZE);

	n = malloc(sizeof(*n) + f->filter_len * sizeof(n->insns[0]),
	    M_KQRING, M_WAITOK | M_ZERO);
	error = copyin((const void *)(uintptr_t)f->filter_ptr, n->insns,
	    f->filter_len * sizeof(n->insns[0]));
	if (error == 0)
		error = kqring_cbpf_check(n->insns, f->filter_len);
	if (error != 0) {
		free(n, M_KQRING);
		return (error);
	}
	refcount_init(&n->refs, 1);
	n->len = f->filter_len;
	*np = n;
	*opcodep = f->opcode;
	*deny_restp = (f->flags & IO_URING_BPF_FILTER_DENY_REST) != 0;
	return (0);
}

/*
 * IORING_REGISTER_BPF_FILTER on ring ctx (under its sq_sx), or with ctx
 * NULL for the process (no_new_privs or root, else EACCES).  nr_args 1.
 */
int
kqring_bpf_register(struct kqring_ctx *ctx, struct thread *td,
    const struct kqring_register_arg *ra)
{
	struct kqring_task_rst *tr, *ntr = NULL;
	struct kqring_bpf_table *old;
	struct kqring_bpf_node *n;
	uint32_t opcode;
	bool deny_rest;
	int error;

	if (ctx == NULL && !kqring_may_restrict(td))
		return (EACCES);
	if (ra->nr_args != 1)
		return (EINVAL);
	error = kqring_bpf_import(ra, &n, &opcode, &deny_rest);
	if (error != 0)
		return (error);

	if (ctx != NULL) {
		sx_assert(&ctx->sq_sx, SA_XLOCKED);
		kqring_bpf_table_add(&ctx->bpf, n, opcode, deny_rest, &old);
		/* Submissions may still be reading it: keep it till the end. */
		if (old != NULL)
			ctx->bpf->retired = old;
		return (0);
	}

	ntr = malloc(sizeof(*ntr), M_KQRING, M_WAITOK | M_ZERO);
	sx_xlock(&kqring_trst_sx);
	if ((tr = kqring_trst_find(td->td_proc)) == NULL) {
		tr = ntr;
		ntr = NULL;
		kqring_trst_insert(tr, td->td_proc);
	}
	kqring_bpf_table_add(&tr->bpf, n, opcode, deny_rest, &old);
	sx_xunlock(&kqring_trst_sx);
	kqring_bpf_table_rele(old);
	free(ntr, M_KQRING);
	return (0);
}

/* ------------------------------------------------------------------------
 * Processes.
 * ------------------------------------------------------------------------
 */

/*
 * A ring p sets up gets what p registered: its restrictions as if the ring
 * had registered them (so it can't register its own: EBUSY), with the
 * register rules in force even before IORING_REGISTER_ENABLE_RINGS, and
 * its filters, shared until one side adds to them.
 */
void
kqring_task_rst_apply(struct kqring_ctx *ctx, struct proc *p)
{
	struct kqring_task_rst *tr;

	if (atomic_load_int(&kqring_trst_count) == 0)
		return;
	sx_slock(&kqring_trst_sx);
	if ((tr = kqring_trst_find(p)) != NULL) {
		if (tr->has_rst) {
			ctx->rst = tr->rst;
			ctx->op_restricted = tr->rst.op_registered;
			ctx->reg_restricted = ctx->reg_inherited =
			    tr->rst.reg_registered;
		}
		if (tr->bpf != NULL) {
			refcount_acquire(&tr->bpf->refs);
			ctx->bpf = tr->bpf;
		}
	}
	sx_sunlock(&kqring_trst_sx);
}

/* p1 forked p2: p2 starts with what p1 registered. */
void
kqring_task_rst_fork(struct proc *p1, struct proc *p2)
{
	struct kqring_task_rst *tr, *ntr;

	if (atomic_load_int(&kqring_trst_count) == 0)
		return;
	ntr = malloc(sizeof(*ntr), M_KQRING, M_WAITOK | M_ZERO);
	sx_xlock(&kqring_trst_sx);
	if ((tr = kqring_trst_find(p1)) != NULL) {
		ntr->rst = tr->rst;
		ntr->has_rst = tr->has_rst;
		if ((ntr->bpf = tr->bpf) != NULL)
			refcount_acquire(&ntr->bpf->refs);
		kqring_trst_insert(ntr, p2);
		ntr = NULL;
	}
	sx_xunlock(&kqring_trst_sx);
	free(ntr, M_KQRING);
}

static void
kqring_trst_free(struct kqring_task_rst *tr)
{
	sx_assert(&kqring_trst_sx, SA_XLOCKED);
	LIST_REMOVE(tr, link);
	atomic_subtract_int(&kqring_trst_count, 1);
	kqring_bpf_table_rele(tr->bpf);
	free(tr, M_KQRING);
}

void
kqring_task_rst_exit(struct proc *p)
{
	struct kqring_task_rst *tr;

	if (atomic_load_int(&kqring_trst_count) == 0)
		return;
	sx_xlock(&kqring_trst_sx);
	if ((tr = kqring_trst_find(p)) != NULL)
		kqring_trst_free(tr);
	sx_xunlock(&kqring_trst_sx);
}

/* Module unload: no ring is left, and with the module go the processes'. */
void
kqring_task_rst_unload(void)
{
	struct kqring_task_rst *tr;

	sx_xlock(&kqring_trst_sx);
	for (u_int i = 0; i < KQRING_TRST_HASH; i++) {
		while ((tr = LIST_FIRST(&kqring_trst_hash[i])) != NULL)
			kqring_trst_free(tr);
	}
	sx_xunlock(&kqring_trst_sx);
}
