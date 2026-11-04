# Spec: event-driven chains

**Status:** phase 1 implemented (`kmod/kqring_trig.c`, tests in `kqring_test trig`). Of phase 2, `LEN_FROM_PREV` and `STOP_ON_ZERO` are implemented, with the echo benchmark; `BUF_FROM_PREV` is not yet. Phase 3 not yet.

In io_uring, linked requests run once: the next request in a chain starts when the one before it completes. An *event-driven chain* runs its linked requests each time an event fires. "Whenever this socket is readable, receive into a provided buffer and write what arrived to this file" becomes one submission. After that, the kernel runs the chain on every event with no round trip to user space, until the program cancels it.

This is what kqueue's `EV_DISPATCH` and a user-space reactor loop do, moved into the kernel. io_uring has multishot `RECV`, `ACCEPT` and `POLL_ADD`, but each of those only posts CQEs. None of them can start other requests, and nothing in io_uring passes one request's result to the next. (A BPF-programmable io_uring has been proposed upstream but has not been merged as of mid-2026.)

## User-facing model

A chain is a **trigger** SQE followed by up to 8 **template** SQEs linked to it with `IOSQE_IO_LINK` or `IOSQE_IO_HARDLINK`, submitted like any linked chain. The ring must have opted in to kqring's extensions (see "ABI").

```c
kqring_queue_init_ext(64, ring, 0);        /* io_uring_queue_init + opt-in */

sqe = io_uring_get_sqe(ring);              /* the trigger */
kqring_prep_trigger_poll(ring, sqe, sockfd, POLLIN, &trig);
sqe->flags |= IOSQE_IO_LINK;
sqe->user_data = 1;

sqe = io_uring_get_sqe(ring);              /* template 1 */
io_uring_prep_recv(sqe, sockfd, NULL, 0, 0);
sqe->flags |= IOSQE_BUFFER_SELECT | IOSQE_IO_LINK;
sqe->buf_group = 7;
sqe->user_data = 2;

sqe = io_uring_get_sqe(ring);              /* template 2 */
io_uring_prep_write(sqe, logfd, NULL, 0, -1);
sqe->user_data = 3;                         /* buffer and length from template 1 */
io_uring_submit(ring);
```

- The trigger arms an event source. Phase 1 supports readiness of a descriptor, as `POLL_ADD` does: sockets through upcalls, and other files through the ring's knotes. Phase 2 adds every kqueue filter, once a kevent extension exists (process exit, file changes, signals, user events, timers).
- The template SQEs are **not issued at submission**. kqring validates them as it would any SQE and keeps a copy. A bad template, or a trigger that refuses its templates (a nested trigger, a multishot or `IOSQE_IO_DRAIN` template, a `LINK_TIMEOUT` first, `nsteps` not matching), fails the trigger with its own error, and the templates complete with `-ECANCELED`.
- Each run validates the templates again, as a new submission would, so a fixed file or buffer unregistered in the meantime fails that run's requests.
- Each time the event fires, kqring builds a fresh chain from the templates and issues it as if the program had just submitted it. Every request posts its CQE as usual, with the template's `user_data`, so existing CQE handling works. `IOSQE_CQE_SKIP_SUCCESS` on a template hides successes and leaves only errors visible.
- The trigger posts a CQE with `IORING_CQE_F_MORE` for each firing only if asked (`KQRING_TRIG_CQE_EACH`). It always posts a final CQE without `F_MORE` when it ends.

### Firing modes

| Mode | Behaviour | Use |
|---|---|---|
| `KQRING_TRIG_SERIAL` (default) | After firing, the trigger is disabled until the chain it started has fully completed, then re-armed. If the event is still pending (level-triggered), it fires again at once. | Back-pressure for free: one chain per socket at a time, in order. kqueue's `EV_DISPATCH` plus an automatic re-enable. |
| `KQRING_TRIG_CONCURRENT` | Each event starts a chain at once, up to `max_inflight` (1–64) chains. Past that, events merge until a chain finishes. | Independent work per event, for example accept → recv on each new connection. |

### Passing results between requests

The trigger's argument struct gives each template step flags that take an input from the step before it:

| Step flag | Effect on this template's copy |
|---|---|
| `KQRING_STEP_LEN_FROM_PREV` | `len` = the previous request's `res` (bytes moved), never more than the template's own `len`, and 0 if the previous request failed (reached through `IOSQE_IO_HARDLINK`). For `READ`, `WRITE`, `READ_FIXED`, `WRITE_FIXED`, `SEND`, `SEND_ZC`, `RECV`, `SPLICE` and `TEE`; on any other opcode it fails the trigger with `-EINVAL`. Because `len` only shrinks, every check made of it when the template was prepared still holds. |
| `KQRING_STEP_STOP_ON_ZERO` | If the previous `res` was 0 (end of file), the rest of this run is skipped without CQEs, and the trigger ends with `res = 0` in its final CQE once the run is done. |
| `KQRING_STEP_BUF_FROM_PREV` (bit 1, reserved, not implemented yet) | Use the provided buffer the previous request selected, and give it back when this request completes, so the previous CQE doesn't hand it to user space. |

Step flags go on the step that takes the result, so `step_flags[0]` must be 0, as must the flags of steps past `nsteps`. A `LINK_TIMEOUT` template takes none.

Together these make an in-kernel copy loop. An echo server is a `SERIAL` trigger on `POLLIN` with two templates: `RECV` into a buffer, then `SEND` of that buffer with `LEN_FROM_PREV | STOP_ON_ZERO`, both with `IOSQE_CQE_SKIP_SUCCESS`. Use `MSG_WAITALL` on the `SEND`, so a slow peer doesn't cut a message short. The ring then posts nothing per message, and user space does nothing at all until the trigger ends. A `READ` → `WRITE` pair with `off = -1` copies a file in the same way. Nothing in io_uring can express this, because its links cannot pass a value from one request to the next.

**Why `BUF_FROM_PREV` waits.** Giving a buffer back to a buffer ring from the kernel would mean writing the ring's tail, which belongs to user space. It needs a small kernel-side store of returned buffers, which selection would take from before the ring, and the selected address carried from each receive path to the request that uses it. Until then, use a `SERIAL` trigger per connection with its own buffer, as above. The runs of a `CONCURRENT` trigger all use the templates' addresses, so they would share one buffer.

### Ending a trigger

- `ASYNC_CANCEL` on the trigger's `user_data` (or with `IORING_ASYNC_CANCEL_FD` on its fd, or `CANCEL_ANY`) ends it. Chains already running finish first; the final CQE, `-ECANCELED`, comes after their CQEs. (`IORING_ASYNC_CANCEL_OP` never matches a trigger, and `POLL_REMOVE` doesn't touch its poll.) Cancelling the runs themselves too (`KQRING_TRIG_CANCEL_RUNNING`) is left for later.
- **The last event.** A hang-up (`POLLHUP`), an error (`POLLERR`) or, for a trigger watching `POLLIN`, end of input (`POLLRDHUP`: the peer shut down writing) runs one last time, then ends the trigger with `res = 0`. Without this, input that has ended would stay readable and fire for ever. A descriptor closed under the trigger (`POLLNVAL`) ends it with `-EBADF`, without a run.
- A chain run that fails ends the trigger when `KQRING_TRIG_STOP_ON_ERROR` is set. The final CQE then carries the error. The failed request has already posted its own CQE under its own `user_data`, which tells the program which template it was. By default, failures only show in that request's own CQE and the trigger keeps going.
- `STOP_ON_ZERO` ends it with `res = 0` after the run that read end of file.
- Closing the ring ends it with no CQE.
- The CQ being full when the event fires ends it with `-EOVERFLOW`, as a multishot request ends. An event that a run doesn't consume (the templates don't read what made the descriptor ready) fires again at once, so it fills the CQ quickly.
- Running out of request slots (`KQRING_MAX_REQS`) ends it with `-EBUSY`.

### Limits and interactions

- At most 1,024 triggers per ring (`-EBUSY` past that).
- A `SERIAL` trigger whose event is still pending re-arms at once, but after 16 firings in a row its re-arm goes through taskwork. So the work one enter does is bounded even if the event never stops.
- Rings with restrictions (`IORING_REGISTER_RESTRICTIONS`) refuse the escape opcode with `-EACCES`, and `IORING_SETUP_IOPOLL` rings with `-EINVAL`.
- **AF_UNIX stream sockets on FreeBSD 15:** when the peer calls `shutdown(SHUT_WR)`, the kernel runs no socket upcall ([PR 300494](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300494)). kqring watches the socket with a knote instead, so a trigger on a descriptor sees end of input at once. A trigger on a fixed file doesn't, until more happens on the socket.
- Don't combine triggers with `IOSQE_IO_DRAIN`. A trigger's poll stays in flight, as an armed multishot request does, so a drained chain submitted later waits for it, and runs queued behind that chain wait too.

## ABI

kqring's own operations must never collide with a number Linux assigns later, now or in the future. Two rules ensure that (see "Numbering" below): every kqring operation goes through **one escape opcode** with a sub-opcode, and a ring understands that escape opcode **only if the program opted in** through a kqring-only setup call.

```c
/* Opt-in, on the ring fd before IORING_REGISTER_ENABLE_RINGS or the first
 * submission: an ioctl in /dev/kqring's own number space, not io_uring's. */
struct kqring_ext_setup {
	__u32	version;	/* KQRING_EXT_VERSION (1) */
	__u8	ext_opcode;	/* escape opcode for this ring; 0 = default 0xFF */
	__u8	resv0[3];
	__u64	features;	/* out: KQRING_EXT_F_* this kernel supports */
	__u64	resv[2];	/* must be 0 */
};
#define KQRING_IOC_EXT_SETUP	_IOWR(KQRING_IOC_MAGIC, 4, struct kqring_ext_setup)
#define KQRING_EXT_F_TRIGGER	(1ULL << 0)

/* The escape opcode's SQE: opcode = the ring's ext_opcode, and
 * sqe->ioprio = the sub-opcode.  Every other field keeps its io_uring
 * meaning (fd, flags, user_data, personality, file_index), so links,
 * fixed files and cancellation work unchanged. */
#define KQRING_EXT_TRIGGER	1	/* sub-opcode, u16 */

/* KQRING_EXT_TRIGGER:
 * sqe->fd: the descriptor to watch (IOSQE_FIXED_FILE allowed)
 * sqe->poll32_events: POLLIN / POLLOUT / ...  (phase 1)
 * sqe->addr: struct kqring_trigger *, sqe->len = sizeof(struct kqring_trigger)
 * sqe->user_data: identifies the trigger (cancel, final CQE)
 */
struct kqring_trigger {
	__u32	flags;		/* KQRING_TRIG_* */
	__u16	max_inflight;	/* CONCURRENT: 1..64; SERIAL: 0 */
	__u8	nsteps;		/* number of template SQEs that follow, 1..8 */
	__u8	source;		/* KQRING_TRIG_SRC_POLL (phase 1), _KEVENT (phase 2) */
	__u8	step_flags[8];	/* KQRING_STEP_* per template */
	__u64	resv[2];	/* must be 0 */
};

#define KQRING_TRIG_SERIAL		0
#define KQRING_TRIG_CONCURRENT		(1U << 0)
#define KQRING_TRIG_CQE_EACH		(1U << 1)
#define KQRING_TRIG_STOP_ON_ERROR	(1U << 2)
#define KQRING_TRIG_CANCEL_RUNNING	(1U << 3)

#define KQRING_STEP_LEN_FROM_PREV	(1U << 0)
#define KQRING_STEP_BUF_FROM_PREV	(1U << 1)
#define KQRING_STEP_STOP_ON_ZERO	(1U << 2)
```

- **Final CQE:** `res` = 0 for a normal end (EOF with `STOP_ON_ZERO`), `-ECANCELED` for a cancel, or the failing request's error with `STOP_ON_ERROR`. It uses no CQE flag bits of its own. With a 32-byte CQE (`CQE32`), `big_cqe[0]` holds the number of runs.
- **Discovery** goes through the opt-in call (`features`), not through `IORING_REGISTER_PROBE` or `io_uring_params.features`, which belong to io_uring's number space.
- **Rejected with `-EINVAL`:** the escape opcode on a ring that hasn't opted in (as an unknown opcode is on Linux); an unknown sub-opcode; templates that are themselves triggers, or multishot, or that use `IOSQE_IO_DRAIN`; `nsteps` that doesn't match the linked SQEs that follow; nonzero `resv`.
- **Allowed:** `LINK_TIMEOUT` among the templates, which guards each run.

### Numbering

kqring keeps io_uring's numbers so that unmodified liburing programs work, and Linux keeps adding opcodes at the end (`IORING_OP_LAST` is 63 today). Choosing numbers from the top of the range would only make a collision unlikely, so kqring does better:

- **A ring that hasn't opted in is pure io_uring.** Every opcode, flag and field means what it means on Linux, including any opcode Linux adds later at any number, which kqring can then implement at its Linux number. A program written for Linux can never reach a kqring operation.
- **The opt-in uses kqring's own number space** (an ioctl on the ring), never an io_uring setup flag, register opcode or `resv` field, any of which Linux may claim later.
- **One escape opcode, chosen per ring.** It defaults to 0xFF. If Linux ever assigns that number, a kqring program can pick another one at setup, such as any number the probe reports as unused. Nothing in kqring's ABI is a fixed number that Linux could take.
- **Everything else stays inside our own structures:** sub-opcodes in `sqe->ioprio` of the escape opcode, options in `struct kqring_trigger`, features in `struct kqring_ext_setup`. No kqring meaning is added to an io_uring flag bit, CQE flag or `io_uring_params` field.

A liburing-style helper, `kqring_queue_init_ext()`, hides the opt-in.

## Implementation sketch

- **Opt-in** (`kqring_dev.c`). `KQRING_IOC_EXT_SETUP` sets `ctx->ext_opcode`, refused with `EBUSY` once the ring has taken a submission. Submission checks `sqe->opcode == ctx->ext_opcode && ctx->ext_on` before the io_uring opcode switch.
- **Submission** (`kqring_req.c`). When a trigger SQE starts a chain, the `nsteps` linked SQEs that follow are copied into `op->args` (8 × 64 bytes, charged to the ring's argument budget), not turned into requests. Each template goes through the normal validation once (prep, then free) to catch errors at submission. The fixed-file and buffer indexes are checked again on each run, so unregistering behaves exactly as it does for ordinary requests.
- **Arming** reuses the multishot `POLL_ADD` path (`kqring_exec_poll`, socket upcalls, `kqring_kpoll.c` knotes). The trigger stays on `active_ops` while armed.
- **Firing** happens in taskwork on the issuer, as for every completion. In `SERIAL` mode the trigger is disarmed first. kqring then builds the chain from the templates (`kqring_op_alloc` and the normal SQE prep, as at submission), applies the step flags and calls `kqring_chain_submit()`. Each request of the run records the trigger it belongs to (`op->trig`, with a reference held).
- **Completion:** `kqring_req_complete()` passes `res` and the selected buffer id to `op->link` before issuing it (the step flags). When the last request of a run completes, the trigger's count of running chains drops. In `SERIAL` mode that re-arms the trigger. With `STOP_ON_ERROR`, a failed run ends it.
- **Cancellation and teardown** walk the trigger's running chains (`CANCEL_RUNNING`) through the existing cancel code. The trigger holds a reference until its last chain is done, so a chain never refers to a trigger that has been freed.
- **Limits:** templates count against the ring's argument budget; each run's requests count against the ring's request limit, as if submitted; and a per-ring cap on armed triggers (`kern.kqring.max_triggers`, default 1024) applies.

## Phases

1. **Opt-in and escape opcode; readiness triggers, `SERIAL` and `CONCURRENT` modes, `CQE_EACH`, `STOP_ON_ERROR`, cancellation.** Tests in `kqring_test`: the escape opcode rejected without opt-in, echo server loop, EOF, cancel while running, fd closed, CQ overflow, bad templates, unregister of a fixed file used by a template. Fuzzer: opted-in rings with random trigger setups and templates.
2. **Step flags.** Done: `LEN_FROM_PREV` and `STOP_ON_ZERO` (tests: a `RECV` → `SEND` echo with no CQE per message, an event-driven file copy that stops at end of file, and bad step flags). Not yet: `BUF_FROM_PREV`.
3. **Kevent triggers**, once the kevent extension lands: process exit, file change and signal as trigger sources.

**Benchmark** (`kqring_bench echo`): a server thread echoes each message to a client that sends one and waits for it. Three servers are compared: kqueue (`kevent(2)`, `read(2)`, `write(2)`), io_uring style (each `RECV` CQE makes user space submit the `SEND` and the next `RECV`), and a trigger (`RECV` → `SEND` with `LEN_FROM_PREV`, no CQE or system call per message). It reports the round trip and the server thread's CPU time per message.

Medians of 3 runs on an Intel Core i5-8250U, FreeBSD 15.1 `GENERIC` (round trip / server CPU, in µs):

| Messages | kqueue | io_uring style | Trigger |
|---|---|---|---|
| AF_UNIX, 64 B | 5.70 / 3.09 | 6.06 / 3.02 | 5.46 / 2.62 |
| AF_UNIX, 4 KiB | 6.89 / 3.63 | 7.17 / 3.55 | 6.62 / 3.17 |
| TCP, 64 B | 11.96 / 4.35 | 12.36 / 4.37 | 11.90 / 4.01 |

The trigger uses 8–15% less server CPU per message than kqueue and is the fastest round trip in each case. One message at a time, most of each round trip is waking the other thread, which no server can avoid, so the gains are modest. The trigger's advantage grows with the work it saves per message: many connections, or chains of more than two steps.

## Roadmap for kqring's extensions

Event-driven chains are the first of three extensions, all behind the same opt-in and escape opcode. Each is a sub-opcode, and none duplicates an operation kqring already has.

1. **Event-driven chains** (`KQRING_EXT_TRIGGER`, this spec): phases 1 and 2 above.
2. **Kevent operation** (`KQRING_EXT_KEVENT`, **implemented** for `VNODE`, `SIGNAL` and `PROC`; `include/sys/kqring.h` has the interface). One-shot or multishot (`KQRING_KEV_MULTISHOT` in `sqe->rw_flags`), with a CQE per event. `sqe->addr` points to a `struct kevent` with the ident, filter and fflags. With a 32-byte CQE (`CQE32`), `big_cqe[0]` holds the kevent's `data` and `big_cqe[1]` the fflags that fired. Events that fire before a CQE is posted merge into it.
   - `EVFILT_VNODE`: a file or directory was written, extended, renamed, deleted, or had its attributes or links changed. `res` is the `NOTE_*` that fired. io_uring has no operation for this; Linux needs inotify plus a read.
   - `EVFILT_SIGNAL`: a signal arrived. `res` is how many times since the last CQE, even for a signal that's ignored. io_uring has no operation for this; Linux needs signalfd plus a read.
   - `EVFILT_PROC`: any process (not only a child, unlike `WAITID`) exited, forked or ran exec. `res` is the `NOTE_*` shifted right by 16 (`KQRING_KEV_PROC_NOTE()`), because they are the top bits and `res` must stay positive. The exit status is `big_cqe[0]`, so it needs `CQE32`. An exit is the process's last event and ends a multishot request.
   - Rules: `sqe->fd` is -1 and the ident is in the kevent; `VNODE` needs a descriptor, not a fixed file; a ring can have one request per ident and filter (`-EBUSY` for a second, as a kqueue holds one knote per pair); only a thread of the ring's owner process can issue one (`-EOPNOTSUPP`), because the kqueue and descriptors are that process's.
   - **Not yet:** `EVFILT_READ` / `EVFILT_WRITE` for the byte count. A socket's read knote on the ring's kqueue is already taken by kqring's end-of-input watch (FreeBSD PR 300494), and a non-socket file's by `POLL_ADD`, so these need the two to share a knote first.
   - Left out because kqring already covers them: `EVFILT_TIMER` (multishot `TIMEOUT`), `EVFILT_USER` (`MSG_RING`), `EVFILT_AIO` (the ring does the I/O).
   Still to do here: phase 3 of event-driven chains, kevent sources as triggers ("when this file changes, read it").
3. **kqueue bridge** (`KQRING_EXT_KQUEUE`): attach an existing kqueue to a ring, and its events, registered by any code such as libevent or libuv, arrive as CQEs with their `udata`. This is a migration aid, so programs can move to kqring one part at a time. A `POLL_ADD` on a kqueue fd only says that the kqueue has events.

## Open questions

- Should the trigger's final CQE report how many runs it did, or is a 32-byte CQE (`CQE32`) too much to require? The proposal puts it in `big_cqe[0]` only when the ring uses `CQE32`.
- Should `BUF_FROM_PREV` also work with registered (fixed) buffers, as "the same buffer index" rather than a provided-buffer id? It's simple, but the user's program would have to know no run overlaps (`SERIAL` only).
