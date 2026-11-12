# The cost of an enter

Status: implemented, November 2026. Roadmap phase 10.

On the one machine that ran both, one `NOP` per `io_uring_enter(2)` cost kqring 438 ns above FreeBSD's system call floor and io_uring 178 ns above Linux's ([linux-comparison.md](linux-comparison.md#the-rings-own-costs)). [worker-wakeups.md](worker-wakeups.md#cost-per-read) found the extra spread over about 25 functions. This note profiles the enter itself, without file I/O, to find where the time goes.

## How it is measured

[`tests/kqring_xbench.c`](../tests/kqring_xbench.c) times the ring's own costs, each against the plain system call it replaces. `kqring_xbench reps case` runs one case alone, which gives a profiler a steady loop. The profile is DTrace's `profile-4999` over 8 seconds of that loop, kernel stacks of the benchmark's threads only, and `fbt` counts of the calls per enter.

Profiles and A/Bs are from the Ryzen 3 PRO 4350G host ([BENCHMARK.MD](../BENCHMARK.MD)), FreeBSD 15.1 `GENERIC`, no PTI, where `getppid(2)` takes 53 ns, unless they say the i5. Times are medians of 5 one-second runs, and an A/B alternates the two modules three times.

| ns | Phase 9 build | Now |
|---|---:|---:|
| `getppid(2)` | 53 | 53 |
| 1 `NOP` per enter | 387 | 194 |
| — above the floor | 334 | 141 |
| 32 `NOP`s per enter, per `NOP` | 83.6 | 71.3 |
| Pipe: `write(2)` then `READ` | 629 | 475 |
| AF_UNIX: `write(2)` then `RECV` | 755 | 558 |

## What the profile found

### An idle kernel thread woken by every enter

45% of the samples of one `NOP` per enter were in `spinlock_exit` and `lock_delay`, the scheduler and a spinning lock, and a fifth of all of them in the ring's own kernel thread, which had nothing to do.

A ring without `IORING_SETUP_DEFER_TASKRUN` or `SQPOLL`, the default, has a kernel thread (`kqtask`) that posts the completions workers and socket upcalls queue as taskwork. Idle, it sleeps on the same channel as threads waiting in an enter for CQEs (`ctx->wakeup_chan`), and counts in the same count of sleepers (`ctx->sleeping`). An enter that posted CQEs ends with `kqring_evfd_flush()`, which on such a ring woke that channel whenever anything slept on it, to wake other threads waiting for CQEs. With no other waiters, the sleeper was the idle thread: every enter woke it, it took `sq_sx` to look for work, often while the next enter wanted it, found none and went back to sleep. With 32 `NOP`s per enter, half the samples were `lock_delay`, mostly that thread waiting for `sq_sx` on another CPU.

Threads waiting for CQEs count themselves in `ctx->cq_sleepers` as well, and only they need waking for a CQE: the other sleepers on the channel, the kernel thread among them, wait for taskwork, which wakes them when it is queued. `kqring_evfd_flush()` now wakes the channel only when `cq_sleepers` isn't 0. One `NOP` per enter went from 387 to 217 ns, a pipe `READ` from 629 to 505, an AF_UNIX `RECV` from 755 to 595; a batch of 32 was level.

This was every default ring's cost, whatever its requests: one wake-up of a kernel thread per enter that completed something inline, and a second CPU kept busy. liburing's `io_uring_queue_init()` makes such a ring. Rings with `DEFER_TASKRUN`, as `kqring_bench` sets up, have no such thread and weren't affected.

### Clearing the request

After that, 13% of the kernel samples were `memset_std`: each request clears its `struct kqring_op`, 872 bytes, up to the callout, 680 bytes, with `rep stos`. In user space on the same CPU, `rep stosq` takes 27 ns for 680 bytes and 20 ns for 456, and 8-byte stores 21 and 14 ns: the string instruction costs more to start than it saves at this size.

Of the 680 bytes, the request lists' linkage (128 bytes) never needs clearing, as what a request is on is kept in its flags and never read from its links, and three fields are always written before they are read: `wq_caps` with `wq_fp`, a timeout's `ts` by its prep, `cqe_extra` with `cqe_has_extra`. They now follow the cleared part, which `__builtin_memset_inline()` clears with stores: 456 bytes. One `NOP` per enter went from 218 to 200 ns, a batch of 32 from 83.4 to 71.6 ns per `NOP`.

### Against the phase 9 build

Both changes together (`d648c02`), alternated five times with the phase 9 build: `kqring_bench` and `kqring_mt_bench` are level over all their rows (median −0.1% and −0.02%), and their `NOP` rows, on `DEFER_TASKRUN` rings, gain from the smaller clear alone: one per enter 202 to 186 ns, 32 per enter 83.4 to 71.3 ns each. The rows that moved more than 5% either way are the zero-copy sends, which swing from round to round in both builds, and kqueue's own. fio's direct 4 KiB reads from an md(4) disk are level: 222,146 to 223,952 IOPS at one in flight, 693,838 to 691,521 at 32 (medians of three).

### Two locks and three calls

On the i5-8250U laptop that also runs Linux (below), the same profile put 8% of the kernel's samples in two lock and unlock pairs of `ctx->mtx`, about 25 ns per enter: locked instructions cost more there than on the Ryzen. Both went, and three calls with nothing to do:

- **The caller's context, on a ring without `SINGLE_ISSUER`.** The enter took `ctx->mtx` to find whether the calling process has a context there. The ring now keeps the process of its only context, while there is one and it isn't dead, in `ctx->solo_proc`, written under `ctx->mtx` wherever the list or a context's `dead` changes. An enter from that process compares the pointer and doesn't take the lock: a process's context ends only at its exit or exec, so it can't go while one of its threads is in an enter, and the pointer is never followed. Any other caller takes the lock as before.
- **`kqring_evfd_flush()` without the lock** when there is no eventfd, no poller and no waiter. Waiters and `poll(2)` count themselves (`cq_sleepers`, `pollers`) before they look at the CQ, with a full fence between, and the flush looks for them after a fence that follows the CQEs it posted: either it sees them or they see the CQEs. A ring that already has a waiter takes the lock as before, without the fence. The first version fenced first and looked after, and cost fio's direct reads at 32 in flight 1% (692,478 against 685,653 IOPS, six alternated rounds), since a ring that fio waits on nearly always has a waiter, which the fence only delayed; looking first without a fence made them level (686,850 against 687,909). On the Ryzen the flush gains nothing for one `NOP` per enter, where the fence costs what the uncontended lock did; on the i5 it gains 7 ns.
- **Whether reads may run in the enter** (`kqring_wq_inline_ok()`) was decided before every submission; it is now decided at the first read that asks.
- **Overflowed CQEs, the kqueue bridge and kicked polls** are checked inline before calling out to `kqring_flush_overflow()` (three times per enter), `kqring_kq_rescan()` and `kqring_kpoll_kicked()`.

On the i5, alternated three times each: the context lookup took one `NOP` per enter from 389.0 to 374.8 ns, and with the last two, 364.9 ns; the flush then took it to 357.4 ns. On the Ryzen, all four together against `d648c02`: one `NOP` per enter 200.9 to 193.7 ns, a pipe `READ` 483.7 to 474.8, an AF_UNIX `RECV` 594.2 to 557.7.

## On the machine that runs both

The i5-8250U laptop of [linux-comparison.md](linux-comparison.md) boots FreeBSD 15.1 and Ubuntu's Linux 7.3.0-10. Both ran the same `kqring_xbench`, three runs of five each, on 12 November 2026; each column is the median of the three runs' medians. Linux runs more mitigations on this CPU, so each system's ring is read against its own system call floor. The raw outputs are in [bench/enter/](bench/enter/).

| ns | Linux io_uring | kqring, phase 9 | kqring, `d648c02` | kqring, now |
|---|---:|---:|---:|---:|
| `getppid(2)`, the floor | 592.4 | 192.5 | 192.4 | 192.3 |
| 1 `NOP` per enter | 764.9 | 814.0 | 389.5 | 357.4 |
| — above the floor | **172.5** | 621.5 | 197.1 | **165.1** |
| 32 `NOP`s per enter, per `NOP` | 93.9 | 83.0 | 73.4 | 73.4 |
| Pipe: `READ` above `read(2)` | 265.8 | 529.5 | 280.8 | 255.0 |
| AF_UNIX: `RECV` above `recv(2)` | 328.3 | 455.7 | 208.7 | 181.1 |

One `NOP` per enter now costs kqring 165 ns above FreeBSD's floor, and io_uring 173 above Linux's. In phase 6 it was 438 against 178.

## Against the phase 9 build, at the end

The final build, alternated five times with the phase 9 build on the Ryzen: `kqring_bench` is 1.0% faster over its median row and `kqring_mt_bench` level (−0.06%), and every row that moved more than 5% is faster: one `NOP` per enter 198.8 to 181.9 ns, 32 per enter 83.4 to 71.0 ns each, one `NOP` per enter from 2 to 8 threads 8% faster. fio's direct 4 KiB reads from an md(4) disk are level (0.7% faster at one in flight, level at 32, medians of three and six). The unit tests, the cross-process test, stress, the fuzzer (seeds 7310 to 7312), the comparison with Linux 7.3 (29 cases, no problems) and liburing (220 of 245) pass as before. liburing's `accept-non-empty` failed once in 42 runs: it expects a connection that `connect(2)` has finished on loopback to be in the listen queue already, which the server's side of the handshake can still be behind.

## What is left

For one `NOP` per enter on the i5, the kernel's samples are spread over the system call's entry and exit, submission (`kqring_submit_sqes()`: the clear, the SQE copy, prep), the ring's descriptor (`fget`), `sq_sx`, and about 20 small functions of the enter. `sq_sx` serializes an enter with the ring's kernel thread and the ring's other submitters, as Linux's `uring_lock` does.

## Acceptance

One `NOP` per enter costs no more above FreeBSD's floor than io_uring above Linux's, on the same machine. The unit tests, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before, and `kqring_bench`, `kqring_mt_bench` and fio are level or better.
