# Several processes submitting to one ring

Status: implemented, November 2026, with the changes in [What the implementation changed](#what-the-implementation-changed). Roadmap phase 8. It follows [cross-process-rings.md](cross-process-rings.md), which let a ring move between processes, and lifts the limits that note left.

## What is still different

Since phase 7 a ring set up without `SINGLE_ISSUER` belongs to one process at a time and moves to another when that one submits while the ring is idle. That is enough for PostgreSQL, whose backends take turns on a ring. It leaves four differences from Linux:

1. **One submitting process at a time.** A second process that submits while the first has requests in flight gets `EEXIST`. On Linux any task holding the ring submits, and each task's requests run in its own context.
2. **Exit drops requests without CQEs.** A process that exits with requests in flight cancels them and posts nothing. Linux posts a CQE for each, `-ECANCELED` for those it cancelled. A program that waits for a CQE per request, which is how most programs count, never gets them.
3. **`exec` closes the ring.** The new image can't use a ring whose descriptor it kept. On Linux, `exec` cancels the task's requests and the ring stays, unless `O_CLOEXEC` closes the descriptor; liburing's rings have it by default.
4. **Registered buffers, personalities, provided buffers and polls pin a ring** to its process, and exit drops the ring's registered files and buffers. On Linux they belong to the ring.

## How Linux does it

Linux keeps per-ring state (the SQ and CQ, the tables, the lists of requests, all under `ctx->uring_lock`) apart from per-task state (`struct io_uring_task`):

- **Each task has its own io-wq.** A request that goes to a worker goes to a worker of the task that submitted it, which uses that task's files, memory and credentials.
- **Task work runs in the submitting task.** A request's completion work, such as a receive that became ready, is queued with `task_work_add()` on the task that submitted it and runs there when that task next enters or returns to user space. A task sleeping in user space is woken with `TWA_SIGNAL`.
- **Exit and exec cancel the task's requests** (`io_uring_cancel_generic()`), in the task, before its files and memory go: each request completes, with `-ECANCELED` if it was cancelled, and its CQE is posted. The ring stays open while any descriptor holds it.

## Design

### 1. A context per process on each ring

A `struct kqring_tctx` holds what a ring needs in one process:

| Field | Now in | Role |
|---|---|---|
| `proc` | `ctx->owner_proc` | the process |
| `wq` | `ctx->wq` | its worker pool, created in that process |
| `kqtask_td` | `ctx->sqpoll_td` (`taskrun_only`) | the thread that runs its taskwork while it doesn't enter |
| `taskwork` | `ctx->taskwork_ops` | requests whose next step must run in that process |
| `kp_*` | `ctx->kp_*` | its poller and kqueue for `POLL_ADD` on files that aren't sockets |
| `nreqs` | `ctx->nreqs` (part) | its live requests |
| `cow_gen` | `ctx->cow_gen` | its forks, for zero-copy sends |

A ring has a list of them, under `ctx->mtx`. The process that sets the ring up gets one at setup. Any other process gets one when it first submits, registers or waits. `fork(2)` doesn't make one: the child gets its own when it first uses the ring.

A request records its submitter's context (`op->tctx`) and holds a reference to it until it is freed. Everything that runs a request in a process looks there instead of at the ring:

- `kqring_wq_enqueue()` queues it on `op->tctx->wq`.
- `kqring_taskwork_add()` queues it on `op->tctx->taskwork` and wakes that process's enters or `kqtask` thread.
- `kqring_run_taskwork()` takes curthread's context and runs only its list. It still posts MSG_RING CQEs queued on the ring, which copy nothing to user memory.
- `kqring_flush_ready()` issues a link successor or a chain released from `IOSQE_IO_DRAIN` only if it belongs to curthread's process. Otherwise it queues it as taskwork for its own process, as Linux's `io_req_task_queue()` does.
- The checks of futex and `WAITID` requests (`kqring_run_checks()`) look at the waiter's memory, so each process checks its own. Timeouts don't touch user memory, so any process checks them.

What stays per ring is what Linux keeps per ring: the SQ and CQ, the overflow list, the fixed-file table, provided buffer groups, personalities, restrictions, the eventfd, the lists of armed and deferred requests, and the CQ sequence. All of it is changed only under `sq_sx`, which every submitter and every taskwork run already takes on a ring without `SINGLE_ISSUER`. Linux serializes the same work with `uring_lock`. So this design adds no new lock order. The requests of several processes are in flight at once, but only one thread at a time submits or completes them.

`SINGLE_ISSUER` rings, and so `DEFER_TASKRUN` rings, keep a single context: only one thread may submit. `SQPOLL` rings keep theirs too, since their thread is the only issuer; any process may already wake it and wait.

### 2. Submitting from any process

A process holding a ring without `SINGLE_ISSUER` submits to it whatever other processes have in flight. The handover (`kqring_ctx_takeover()`, `detaching`, `release`, `handover_td`) goes, along with its conditions and the `EEXIST` it returned.

Most of what pinned a ring had a per-process meaning on Linux all along:

- **Provided buffers** are addresses, read and written in the submitter's address space, which is what Linux does. A buffer ring's entries are in pages the ring holds, so any process sees them. Only the buffers they point to are in the submitter's memory.
- **Personalities** are credentials the ring holds, which any process holding the ring may use, as on Linux. A ring descriptor can only reach another process through `fork(2)` or `exec`, since it can't be passed over a socket. A set-ID `exec` still closes the ring (section 4).
- **Polls on files that aren't sockets** go to the submitter's poller (`tctx->kp_*`). A poll is registered on a kqueue by descriptor, and a kqueue belongs to one descriptor table.

Two things stay pinned:

- **Registered buffers.** kqring reads and writes a registered buffer through its address in the process that registered it, where Linux holds its pages. A request that uses one from another process completes with `-EFAULT`, until registered buffers hold their pages (the roadmap's research item on mapping independence).
- **kqring's extensions.** Event-driven chains, kevent requests and the kqueue bridge (`KQRING_IOC_EXT_SETUP`) keep the ring to the process that set them up. Another process's submission gets `EEXIST`, as all of them do now. They have no Linux counterpart to follow, and no program needs them across processes yet.

### 3. Exit

The `process_exit` handler runs in `exit1()` after `thread_single(SINGLE_EXIT)` has stopped the process's other threads, but before its descriptor table (`fdescfree()`) and address space (`vmspace_exit()`) are freed. That is where Linux cancels a task's requests, and it is enough to complete them properly here too. For each ring where the process has a context, the exiting thread:

1. takes `sq_sx`;
2. cancels the process's requests: armed requests are unarmed, and running workers, which exit has already interrupted, are waited for;
3. completes each one with its result, or `-ECANCELED` if it didn't finish. It runs their taskwork itself, which may copy out to its own memory, which still exists. It posts their CQEs, which go to the ring's memory, not the process's;
4. stops the context's `kqtask` thread and poller, closes its worker pool, and frees the context.

The ring stays open while any descriptor holds it. The fixed-file table, registered buffers and personalities stay with the ring. A ring can't be held in its own or another ring's fixed table, and can't be passed over a socket, so no reference cycle needs exit to break it. They are freed when the ring is closed. The `kqring_owner_exiting()` check, which drops CQEs from a released ring, goes.

Requests of other processes go on. A link chain's requests all belong to the process that submitted its head, so a chain never spans processes.

### 4. Exec

The `process_exec` handler runs before `vmspace_exec()` replaces the address space, and before `fdcloseexec()` closes descriptors with `O_CLOEXEC`. It cancels and completes the process's requests as exit does (section 3), and frees its context. Then:

- If the ring's descriptor has `O_CLOEXEC`, as liburing's do by default, `fdcloseexec()` closes it; the ring closes with its last descriptor, as now.
- Otherwise the new image keeps the ring. It has to map the rings again, as on Linux, and gets a new context when it uses it.
- A set-ID `exec` (`imgp->credential_setid`) still closes the rings the process holds, as kqring does now: a new image that runs with other credentials mustn't inherit personalities or a ring set up by the old ones. Linux keeps them; this is kept as a deliberate difference.

### 5. Close

The last `close(2)` cancels every context's requests, as it cancels the owner's now: without CQEs, since nobody can reap them. It closes the worker pools of every context. A pool in another process is closed the way a pool shared through `IORING_SETUP_ATTACH_WQ` with an exited process is closed now. `ATTACH_WQ` shares a pool between rings of one process, so it applies to the creating process's context only.

## What the implementation changed

Writing and testing it changed five things in the design above:

- **Exit and exec finish on a kernel task.** During `execve(2)` the process's other threads, kqring's workers and taskwork thread among them, are parked rather than gone when the `process_exec` handler runs, and leave only after it returns, so the handler can't wait for them. Both handlers therefore only mark the process's context dead: its threads find no context from then on, and a new image that keeps the ring gets a fresh one. The rest runs on kqring's teardown taskqueue once those threads are gone. It needs nothing of the process: a request a worker or the disk finished completes with its result, and the rest with `-ECANCELED`. The CQEs come a moment after the process is gone.
- **Standalone `TIMEOUT`s outlive their process,** as on Linux, where liburing's `test_cancel_inflight_exit` expects one to fire after its submitter exits. The context stays, dead, until the last of them has posted its CQE; the teardown task looks every 100 ms.
- **The poller is in one process.** A context's own poller (section 1) is not built yet. The ring's poller, its thread and kqueue, belongs to the first process whose request needs it, and a non-socket `POLL_ADD` from another process fails with `-EOPNOTSUPP`. The end of input on an AF_UNIX stream, which runs no socket upcall and which the poller watches for, is polled every 50 µs instead while such a receive from another process is armed. The poller goes with its process, and the next process that polls gets it.
- **A ring set up or enabled by another process** than the one that opened it (a descriptor inherited across `fork(2)`, as in liburing's `single-issuer` test) gets its first context in that process, since nothing has used it yet. Before, such a ring's workers ran in the opener's process.
- **Registered buffers belong to a context,** by a number never reused, rather than a process: `IORING_REGISTER_CLONE_BUFFERS` re-tags the caller's own buffers for the ring they are cloned to.

## Costs

- **A `kqtask` thread per process per ring** without `DEFER_TASKRUN`, where there was one per ring. It starts with the context, when the process first submits or registers; a process that only waits creates neither.
- **A context lookup per enter** from a process other than the setup process: the ring's list is short, and a one-entry cache (the last process seen) keeps the common case to a pointer comparison.
- **Single-process rings** run as now, with `op->tctx` one indirection away. `kqring_bench` before and after has to show no change beyond noise.

## Security

- **Credentials:** a request runs with its submitter's credentials or a registered personality's, as on Linux. A set-ID `exec` closes the rings.
- **Memory and descriptors:** a request's workers and taskwork run in its submitter's process, so a process never reads or writes another's memory through a ring. Registered buffers, the one case where that would happen, fail with `-EFAULT` from another process.
- **Capsicum:** fixed-file slots keep the rights they were registered with, as now. A process in capability mode can use the slots of the ring it holds, as on Linux.
- **Teardown:** exit and exec cancel only their own process's requests, under `sq_sx`, with the paths close uses. Module unload still waits for every ring file to close and every worker to exit.

## Order of work

Each step keeps the module working and is validated before the next:

1. **Contexts:** add `struct kqring_tctx`, and give the setup process one. Move the worker pool, `kqtask` thread, taskwork list and poller into it, and route requests through `op->tctx`. With one process nothing changes: unit tests, stress, fuzzer, liburing and `kqring_bench` show it.
2. **Submitters:** any process gets a context on first use. Remove the handover. Fail registered-buffer requests from other processes with `-EFAULT`, and keep extension rings to one process.
3. **Exit and exec:** cancel and complete a process's requests with CQEs; keep the ring open; close it on set-ID `exec`.
4. **Validation:** tests below, the fuzzer forking submitters, PostgreSQL's regression tests again, and the documentation (README's differences, `COMPATIBILITY.MD`, the TODO items).

## Measure of success

- New tests in `tests/kqring_test.c`:
  - two processes with reads and socket receives in flight on one ring at once, each landing in its own memory;
  - a drained chain from one process released by another's completions, and issued in its own process;
  - a process that exits with a receive armed and a read in a worker: a CQE for each, and the other process's requests unaffected;
  - `exec` with a ring without `O_CLOEXEC`: the requests' CQEs, and the new image submits on the ring; with `O_CLOEXEC`, the ring closes; a set-ID `exec` closes it;
  - registered buffers from another process: `-EFAULT`;
  - `ASYNC_CANCEL` from one process of another's request.
- The stress test, and the fuzzer with forked processes submitting to shared rings and exiting with requests in flight.
- liburing's tests that fork or exec with a ring (`across-fork`, `io-cancel`, `poll-cancel`, `ring-leak`, `teardowns`, `sqpoll-exec` among them) pass or keep their recorded reasons, and the rest stay at 219.
- PostgreSQL 18.6's regression tests and pgbench still pass and run level.
- `tests/kqring_diff.c` cases for concurrent submitters and for CQEs at exit and exec, matching Linux 7.3.
