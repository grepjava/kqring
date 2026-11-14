# Rings used by more than one process

Status: implemented, November 2026, and superseded the same month by [concurrent-submitters.md](concurrent-submitters.md): processes no longer take turns on a ring, and the handover described here is gone. This note is kept for the reasoning about PostgreSQL's use of rings.

## Why

PostgreSQL 18's `io_method=io_uring` ([`method_io_uring.c`](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/storage/aio/method_io_uring.c)) is the most widely deployed program built on liburing. It uses rings across processes in three ways:

- **The postmaster creates every ring.** It makes one ring per backend slot at startup, with `io_uring_queue_init_mem()` (`IORING_SETUP_NO_MMAP`, ring memory in its shared memory), and never submits to or waits on any of them.
- **Forked backends submit.** Each backend submits on the ring of its slot. When a backend exits, the next one forked into that slot uses the same ring.
- **Other backends wait.** A backend that needs an I/O another backend issued may reap that backend's ring itself: under an LWLock, it calls `io_uring_wait_cqes()` on the other ring and processes what it finds.

On kqring today, all three fail with `EEXIST`: only the process that created a ring may submit to it, register with it, or wait on it for completions not yet posted. PostgreSQL 18.6 built against the liburing port starts, and then every backend PANICs on its first submission ("io_uring submit failed: File exists"), so the postmaster restarts in a loop.

Linux lets any task holding the ring submit and wait. It runs each task's requests in that task's own io-wq, with its own descriptor table, address space and credentials. A task's exit cancels the requests it submitted and leaves the ring to the others.

## Why rings belong to one process

kqring runs a ring's deferred work in kernel threads of its owner process:

- **Workers** (`kqring_wq`) are `kthread_add()`ed to the owner's `struct proc`. They look descriptors up in its table and copy to and from its address space.
- **The `kqtask` thread** of a ring without `DEFER_TASKRUN` runs taskwork, including copy-outs that some completions need (a `RECVMSG` header, for example), as requests finish.
- **The poller** behind non-socket `POLL_ADD`, kevent requests and the kqueue bridge belongs to the owner too.

A second process can't use those threads: their addresses and descriptors are the owner's. Giving every process its own threads for a ring, as Linux does, would split a ring's execution state per process throughout the module. This design doesn't do that. Instead, a ring still belongs to one process at a time, and the owner can change.

## Design

### 1. Waiting from another process

Any process holding a ring set up without `SINGLE_ISSUER` (and so without `DEFER_TASKRUN`, which needs it) may wait on it with `IORING_ENTER_GETEVENTS`, unless the ring is `IOPOLL`, whose waits drive the polling. On a `SINGLE_ISSUER` ring another thread or process may wait the same way, as on Linux, unless the ring is `DEFER_TASKRUN`, whose completions only the owner runs (`EEXIST`), or an `IOPOLL` wait the CQ can't already satisfy (`EEXIST`; phase 15). The wait:

- sleeps until the CQ holds `min_complete` entries or has overflowed, or a `TIMEOUT` request completes, or its timeout or a signal ends it, with the same arguments as the owner's wait (`EXT_ARG`, registered waits, signal mask, `min_wait_usec`);
- registers as a CQ sleeper, so the batched wake-ups the owner's completions make reach it;
- never submits, runs taskwork, polls, or touches the owner's requests.

The owner's `kqtask` thread posts completions while nobody enters, as Linux's task_work does, so the waiter doesn't depend on the owner entering. A waiter that comes in for an overflowed CQ only returns: flushing the overflow list is the owner's.

### 2. Handing a ring over

An enter that submits (`to_submit > 0`), or a `kqring_register()`, from a process that isn't the ring's owner takes the ring over if all of these hold. Otherwise it fails with `EEXIST`, as now.

| Condition | Why |
|---|---|
| Not `SINGLE_ISSUER` | That ring belongs to its submitter task, on Linux too. |
| Not `SQPOLL` | The SQ thread is the issuer. Another process may wake it and wait already. |
| Not closing | |
| Nothing in flight: no requests, retired requests, disk I/O, taskwork or pending zero-copy notifications | Each of these runs in, or copies to, the old owner. |
| No registered buffers | kqring reads and writes registered buffers through their addresses in the owner's address space. |
| No personalities | They hold credentials the new process might not have. |
| No provided buffers (`PROVIDE_BUFFERS` or buffer rings) | Their addresses, and a buffer ring's held pages, are the owner's. |
| No polls, kevent requests, chains or kqueue bridge | They live in the owner's poller and its kqueue. |

Registered files, direct descriptors, restrictions, the eventfd, NAPI settings, the clock and registered wait regions stay: they hold files or values, not addresses or threads. So do CQEs queued for posting (from `MSG_RING`, say) and overflowed ones, which are only records. Capsicum rights stay with the fixed-file slots that snapshotted them. A worker pool shared with another of the old owner's rings (`IORING_SETUP_ATTACH_WQ`) stays with that ring: the handover only drops this ring's reference to it.

The handover runs under the ring's `sq_sx`, which serializes submitters, so the old owner can't submit during it:

1. It checks the conditions above under `sq_sx` and `ctx->mtx`.
2. It stops the old owner's `kqtask` thread and closes its worker pool. A detach flag, not `closing`, tells them to exit.
3. It sets the owner to the calling thread's process, and marks the handover under way: the new owner's other threads wait for it, and any thread that passed the ownership check before it re-checks under `sq_sx` and gets `EEXIST`.
4. It makes a new worker pool in the new process, which creates workers as requests need them, and starts its `kqtask` thread there.

Taskwork and the checks of futex, `WAITID` and timeout requests run only in the owner's process (`kqring_run_taskwork()` and `kqring_run_checks()` return at once elsewhere), so a thread of the old owner that was waiting when the ring moved can't copy a completion into its own memory.

Requests submitted afterwards run with the new process's descriptors, address space and credentials. The `cow_gen` count, which makes zero-copy sends re-hold pages after a `fork(2)`, starts again with the new owner's buffers.

### 3. When the owner exits

Today the owner's exit closes the ring for everyone who holds it: it sets `ctx->closing`, as `close(2)` of the last descriptor does. Instead, exit releases the ring:

- It cancels the owner's requests and waits for their workers, as now. As on close, they complete without CQEs. So does one that finishes first: exit interrupts the process's threads, the ring's workers among them, before the release runs, and a request completing once exit has begun (`P2_WEXIT`) posts nothing.
- It stops the `kqtask` thread and the poller, and closes the worker pool.
- It frees the fixed-file and registered-buffer tables, as now. That still breaks a cycle through a ring's own descriptor in its fixed table, and a released ring then meets the handover's conditions.
- It clears the owner and leaves `closing` unset.

A released ring is unowned. The next process holding it that submits or registers takes it over (section 2), and waits work as in section 1. If the exiting owner held the last descriptor, the ring is closed right after, as now.

`execve(2)` keeps closing the owner's rings. A new program image must not inherit requests, personalities or a ring set up with the old credentials. Linux leaves that to `O_CLOEXEC`, which ring descriptors have by default.

## What stays different from Linux

- **One submitting process at a time.** Two processes can't have requests in flight on one ring: the second gets `EEXIST` until the first's have completed. Linux allows it.
- **Exit drops the owner's registered files and buffers.** On Linux they belong to the ring and stay.
- **Requests in flight at the owner's exit end without CQEs.** Linux cancels them with `-ECANCELED` CQEs.
- **Registered buffers and personalities pin a ring to its process.** On Linux they don't.

## Security

- **Credentials:** a request runs with its submitter's credentials (`op->cred`), as now. A ring with personalities can't change process, so a process can't use another's credentials through it.
- **Descriptors:** workers resolve descriptors in their own process, which is the submitter's. Fixed files are references the ring holds, as on Linux. A process can only take a ring over if it has the ring's descriptor, which it can get only by `fork(2)` (ring descriptors can't be passed with `SCM_RIGHTS`).
- **Capsicum:** the rights snapshotted in fixed-file slots go with the ring. A process in capability mode holding the ring can use them, as the creator could. Personalities, which capability mode refuses, block the handover anyway.
- **Memory:** ring memory (`mmap` or `NO_MMAP`) is shared across `fork(2)`, as now. Request buffers are the submitter's. Registered buffers block the handover.
- **Teardown:** the handover and the release reuse the cancel paths that close uses, under `sq_sx` and the same locks. Module unload still waits for every ring file to be closed.

## Measure of success

- PostgreSQL 18.6 with `io_method=io_uring` passes its regression tests (`make check`) and `pgbench` on kqring.
- New tests in `tests/kqring_test.c`:
  - a forked child submits to an idle ring and its reads land in its own memory;
  - another process waits on a busy ring and wakes on its completions;
  - a ring survives its owner's exit and serves the next process;
  - each condition in section 2 refuses the handover;
  - `exec` still closes the ring.
- The stress test and fuzzer pass, and the fuzzer forks and hands rings over.
- liburing's `across-fork` test, which expects `EEXIST` from a child that submits to a ring its parent still uses, behaves as it does on Linux for what both allow.
