# The cost of an enter

Status: in progress, November 2026. Roadmap phase 10.

On the one machine that ran both, one `NOP` per `io_uring_enter(2)` cost kqring 438 ns above FreeBSD's system call floor and io_uring 178 ns above Linux's ([linux-comparison.md](linux-comparison.md#the-rings-own-costs)). [worker-wakeups.md](worker-wakeups.md#cost-per-read) found the extra spread over about 25 functions. This note profiles the enter itself, without file I/O, to find where the time goes.

## How it is measured

[`tests/kqring_xbench.c`](../tests/kqring_xbench.c) times the ring's own costs, each against the plain system call it replaces. `kqring_xbench reps case` runs one case alone, which gives a profiler a steady loop. The profile is DTrace's `profile-4999` over 8 seconds of that loop, kernel stacks of the benchmark's threads only, and `fbt` counts of the calls per enter.

Everything here is from the Ryzen 3 PRO 4350G host ([BENCHMARK.MD](../BENCHMARK.MD)), FreeBSD 15.1 `GENERIC`, no PTI, where `getppid(2)` takes 53 ns. Times are medians of 5 one-second runs, and an A/B alternates the two modules three times.

| ns | Phase 9 build | Now |
|---|---:|---:|
| `getppid(2)` | 53 | 53 |
| 1 `NOP` per enter | 387 | 200 |
| — above the floor | 334 | 147 |
| 32 `NOP`s per enter, per `NOP` | 83.6 | 71.6 |
| Pipe: `write(2)` then `READ` | 629 | 491 |
| AF_UNIX: `write(2)` then `RECV` | 755 | 590 |

## What the profile found

### An idle kernel thread woken by every enter

45% of the samples of one `NOP` per enter were in `spinlock_exit` and `lock_delay`, the scheduler and a spinning lock, and a fifth of all of them in the ring's own kernel thread, which had nothing to do.

A ring without `IORING_SETUP_DEFER_TASKRUN` or `SQPOLL`, the default, has a kernel thread (`kqtask`) that posts the completions workers and socket upcalls queue as taskwork. Idle, it sleeps on the same channel as threads waiting in an enter for CQEs (`ctx->wakeup_chan`), and counts in the same count of sleepers (`ctx->sleeping`). An enter that posted CQEs ends with `kqring_evfd_flush()`, which on such a ring woke that channel whenever anything slept on it, to wake other threads waiting for CQEs. With no other waiters, the sleeper was the idle thread: every enter woke it, it took `sq_sx` to look for work, often while the next enter wanted it, found none and went back to sleep. With 32 `NOP`s per enter, half the samples were `lock_delay`, mostly that thread waiting for `sq_sx` on another CPU.

Threads waiting for CQEs count themselves in `ctx->cq_sleepers` as well, and only they need waking for a CQE: the other sleepers on the channel, the kernel thread among them, wait for taskwork, which wakes them when it is queued. `kqring_evfd_flush()` now wakes the channel only when `cq_sleepers` isn't 0. One `NOP` per enter went from 387 to 217 ns, a pipe `READ` from 629 to 505, an AF_UNIX `RECV` from 755 to 595; a batch of 32 was level.

This was every default ring's cost, whatever its requests: one wake-up of a kernel thread per enter that completed something inline, and a second CPU kept busy.

### Clearing the request

After that, 13% of the kernel samples were `memset_std`: each request clears its `struct kqring_op`, 872 bytes, up to the callout, 680 bytes, with `rep stos`. In user space on the same CPU, `rep stosq` takes 27 ns for 680 bytes and 20 ns for 456, and 8-byte stores 21 and 14 ns: the string instruction costs more to start than it saves at this size.

Of the 680 bytes, the request lists' linkage (128 bytes) never needs clearing, as what a request is on is kept in its flags and never read from its links, and three fields are always written before they are read: `wq_caps` with `wq_fp`, a timeout's `ts` by its prep, `cqe_extra` with `cqe_has_extra`. They now follow the cleared part, which `__builtin_memset_inline()` clears with stores: 456 bytes. One `NOP` per enter went from 218 to 200 ns, a batch of 32 from 83.4 to 71.6 ns per `NOP`.

### What is left

The rest is spread out. For one `NOP` per enter, of the kernel's samples:

| | |
|---|---:|
| Submission (`kqring_submit_sqes()`: the clear, the SQE copy, prep) | 19% |
| Entry and exit of the system call | 15% |
| Two lock and unlock pairs of `ctx->mtx` | 6% |
| The ring's descriptor (`fget`) | 4% |
| `sq_sx` | 2% |

- **`ctx->mtx` twice per enter.** A ring without `SINGLE_ISSUER` (`shared`) takes it to find the caller's process context, and `kqring_evfd_flush()` takes it to signal the eventfd and pollers and wake waiters. Skipping the second without losing a wake-up needs the waiters and `poll(2)` to announce themselves before they look at the CQ, with a fence on both sides. The first needs a lookup that can't read a context another process is freeing.
- **`sq_sx` on every enter** of a ring with a kernel thread or more than one submitter, which serializes them.

## Acceptance

One `NOP` per enter costs no more above FreeBSD's floor than io_uring above Linux's, on the same machine. The unit tests, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before, and `kqring_bench`, `kqring_mt_bench` and fio are level or better.
