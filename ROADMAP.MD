# kqring roadmap

This is the plan for moving kqring from an experimental preview towards predictable compatibility with Linux io_uring and scalable execution of what it already does. It is an engineering plan: it isn't a claim of complete Linux compatibility, and it isn't a release schedule.

It comes from a parity review of 6 November 2026, checked against the source at `3a5f36c`. Phases run in order, and each has to meet its acceptance criteria before it counts as done. Opcode coverage alone doesn't establish behavioral or performance parity.

See also [`TODO.MD`](TODO.MD) for individual open items, [Differences from Linux](README.md#differences-from-linux), [`BENCHMARK.MD`](BENCHMARK.MD#known-gaps) for known gaps, and the [liburing test accounting](liburing/README.md).

| Phase | Work | Status |
|---|---|---|
| 1 | Ordering and poll timing | Done |
| 2 | Make compatibility measurable | Done |
| 3 | Wait semantics | Done |
| 4 | Per-file write ordering | Done |
| 5 | Worker path scalability | Done |
| 6 | Performance evidence | Done |
| 7 | Newer interfaces and real programs | In progress |

## Where kqring stands

`IORING_REGISTER_PROBE` reports 60 opcodes. Coverage includes shared submission and completion rings, basic and vectored I/O, fixed files and direct descriptors, registered and provided buffers, incremental buffer consumption, send and receive bundles, multishot operations, zero-copy send, links, drains, cancellation, timers, futex operations, SQPOLL, worker pool controls, ring resizing, personalities and restrictions.

FreeBSD-native extensions add persistent event-driven chains, result and buffer forwarding between steps, native kevent requests and a kqueue bridge. These stay an opt-in interface alongside the Linux-compatible API.

The liburing 2.15 port passes 218 of the 245 tests it builds, with `RLIMIT_MEMLOCK` raised. That isn't a compatibility percentage:

- 13 more tests can't be built here, each for a recorded reason.
- The port carries portability shims and adapts some tests, 27 tests skip, and the 7 failures are intentional differences or the platform's.
- A test stops at its first failure, so the subtests after it haven't run.

The probe also can't speak for every variant of an operation. `SYNC_FILE_RANGE` syncs the whole file, and `URING_CMD` supports four socket commands only.

## Phase 1: Ordering and poll timing

- **Done:** reads of a named FIFO complete in the order of the data, as reads of a pipe do (`3a5f36c`).
- **Done:** a poll on another ring completes before the enter that fired it returns, however many rings poll and however many events fire. The poller no longer holds events an enter can't see, and `test_poll_before_return` forces the old race on debug kernels.

Acceptance: a deterministic regression test, repeated debug-kernel runs, and no lost wakeups, hangs or teardown regressions.

## Phase 2: Make compatibility measurable

This comes before new features, so that what follows is measured against something.

### Build the rest of the liburing tests

**Done.** Of the 32 unbuilt tests, 28 now build, and 4 are listed as not portable. Tests that used to stop on portability problems, or on an early failure, before reaching what they test now run too, and `regbuf-merge`, which passed without ever calling kqring, now calls it. This found a wake order bug in `FUTEX_WAKE`, a refused empty set of restrictions, and `IORING_SQ_TASKRUN` being set late. See the [accounting](liburing/README.md#test-results), which lists every adapted test.

Acceptance: every liburing test is passing, skipped, failing with a documented reason, or listed as not portable with a reason.

### Versioned compatibility matrix

**Done.** [`COMPATIBILITY.MD`](COMPATIBILITY.MD) lists all 216 capabilities against Linux 7.3 and liburing 2.15, and [`tests/compat_check.py`](tests/compat_check.py) fails if it stops matching the source. Its [validation gaps](COMPATIBILITY.MD#validation-gaps) list what is still unchecked.

Document every supported opcode, setup flag, enter flag, registration and important subflag against a stated Linux kernel and liburing baseline. Classify each as equivalent, partial, unsupported or intentionally different, and record platform and filesystem dependencies separately from limits of the module.

Cover these boundaries explicitly:

- One process per ring, inherited rings after fork, and teardown across exec.
- Worker cancellation and timeout CQE ordering, including when application buffers can safely be released.
- Fixed-buffer access after unmapping or changing the user mapping.
- Provided-buffer reservations, end-of-file consumption and group removal.
- Cross-ring messages, error reporting, eventfd notification and poll timing.
- Native FreeBSD flags, errors, resource limits and Capsicum rights.
- Whole-file `SYNC_FILE_RANGE`, supported `FALLOCATE` modes, socket-only `URING_CMD`, busy-wait-only NAPI registration and the IOPOLL execution paths.

Acceptance: every advertised capability has a documented contract, a source reference, and a test or a recorded validation gap.

### Differential conformance

**Done.** [`tests/kqring_diff.c`](tests/kqring_diff.c) has 25 cases that build and run on both systems, and [`tests/diff_compare.py`](tests/diff_compare.py) compares their output. On Linux 7.3 and kqring, 19 match and 6 differ on purpose, with the reasons in [`tests/diff_expected.txt`](tests/diff_expected.txt). The raw results and what they ran on are in [`docs/conformance/`](docs/conformance/README.md). The runs found `execve(2)` hanging on a ring with a kernel thread, and six differences that are now fixed: short transfers in links, `CANCEL_ALL` with nothing to cancel, `IORING_POLL_ADD_LEVEL`, a refused `FILES_UPDATE`, another thread's enter on a `SINGLE_ISSUER` ring, and which setup flags go together.

Run matched cases on Linux io_uring and on kqring, normalizing native constants and errors while keeping meaningful differences. Exercise cancellation, links, timeouts, buffer lifetime, CQ overflow, resource updates, fork and exec, fixed descriptors and several submitter threads. The test machine also boots Linux, so both sides can run on the same hardware.

Acceptance: kernel versions, commits, build options, filesystem, resource limits and raw results are kept with each run. Expected differences are listed, and an unexpected difference fails. Native tests and adapted upstream tests stay distinguishable.

## Phase 3: Wait semantics

This was the largest gap the review found in the existing liburing interface: `kqring_enter()` rejected a signal mask in both forms with `EINVAL`, timeouts were always relative, and `IORING_ENTER_ABS_TIMER`, `IORING_ENTER_EXT_ARG_REG` and `IORING_REGISTER_MEM_REGION` didn't exist.

1. **Done:** signal masks, in the plain form and through `IORING_ENTER_EXT_ARG`. As in Linux, the thread waits with the mask only if it will sleep, and the size is checked only then. A signal it lets in ends the wait and is handled with the mask still in place, and a wait a signal ends succeeds if there are CQEs. The mask is FreeBSD's `sigset_t`, 16 bytes where Linux's is 8.
2. **Done:** absolute deadlines (`IORING_ENTER_ABS_TIMER`) on the clock `IORING_REGISTER_CLOCK` chooses. FreeBSD defines `CLOCK_BOOTTIME` as `CLOCK_MONOTONIC`, so the choice can't change a deadline here.
3. **Done:** registered waits, in a region of the program's memory or the module's (`IORING_REGISTER_MEM_REGION`), with Linux's checks of the region and of each wait's offset. liburing's `reg-wait` passes, and `wait-timeout` now runs its absolute-deadline and clock parts.

`IORING_ENTER_NO_IOWAIT` came with them: it is accepted and advertised, and changes nothing, because FreeBSD doesn't count time waiting for I/O apart.

`tests/kqring_test.c` covers interruption, timeout expiry, repeated registered waits, both clocks, invalid arguments, a region read after `fork(2)`, and the module's region outliving its ring. `tests/kqring_diff.c` compares signal masks, absolute deadlines and registered waits with Linux 7.3, and they match. One difference remains, and it is older than this phase: kqring checks a wait's argument before it submits anything, where Linux submits first, so a bad argument fails the whole enter. The matrix lists it under `IORING_ENTER_EXT_ARG`. The fuzzer waits with masks, deadlines and registered waits, and its leak check covers the regions' memory at teardown.

Not covered: suspend. The test machine isn't suspended during runs, so no test shows how a deadline behaves across one. Both clocks are the same one on FreeBSD, so they can't differ there, but whether it counts the time suspended isn't checked. The matrix lists this as a validation gap.

Acceptance: conformance tests for interruption, timeout expiry, repeated registered waits, clock choice, suspend behavior, invalid arguments and teardown. Whatever differs afterwards stays visible in the matrix.

## Phase 4: Per-file write ordering

Buffered writes wait for their worker, which keeps writes to one file in order but makes the issuer wait, where io-wq's per-inode hashing wouldn't. They are about 9% behind aio on a ZFS file. `O_APPEND` and file-position writes wait the same way.

Add per-file ordering to the worker pool, so that writes to one file stay in order, writes to different files run in parallel, and the issuer doesn't wait. This changes how the pool schedules work, so it starts with a design note and new fuzzer coverage of write ordering across files.

**Done.** [docs/write-ordering.md](docs/write-ordering.md) describes it. Writes to a regular file get one of 64 buckets from their vnode, as io-wq hashes inodes. The pool runs a bucket's writes one at a time, in order, and other buckets' in parallel, and the issuer no longer waits for them. `test_write_order` checks the order with another file's writes in between, after a cancel and after the ring closes. The fuzzer checks it under its cancellations, closes and descriptor changes. A cancel or link timeout that finds a regular file's I/O already running now leaves it to complete with its own result and gets `-EALREADY`, as in io_uring, where kqring used to report the I/O as cancelled after it had happened.

On this machine, throughput and CPU are unchanged, buffered or `O_SYNC`, one file or four: ZFS and the SATA SSD set the pace. What changed is latency across files. A small write to one file no longer waits behind a slow `O_SYNC` write to another: it finished first in 5 of 5 rounds, against 0 of 5 before.

Acceptance: ordering and data integrity shown under concurrency, cancellation and close; parallel progress across files; CPU, throughput and latency compared with the current code.

## Phase 5: Worker path scalability

Cached reads through workers stop near 530,000 IOPS on a 4-core machine, and `psync` passes them from 4 jobs. On ZFS at 32 in flight, kqring is 4–5% behind aio with a longer p99, at less than half the CPU.

Profile first. These two may have different causes, and each request crossing to another thread may set a limit that can't be removed. If so, the outcome is a documented cost of that path, not a scheduling change.

**Done.** [docs/worker-wakeups.md](docs/worker-wakeups.md) has the profile and the change. One ring is limited by its submitting thread. Several rings are limited by CPU, and what they spent beyond `pread(2)` went on about 1.6 thread switches per read, because the pool woke a worker for each request. The pool now notes whether its file I/O sleeps, and while it rarely does, keeps only `MAX(4, ncpu / 2)` workers awake to take the queued requests themselves. Writes count as sleeping, and queued work that makes no progress for 1 ms lifts the cap. `test_wq_cap_stall` checks the stall path.

On the second test machine (Ryzen 3 PRO 4350G, 8 threads), thread switches per read fell by 60% and kqring's own pool benchmark gained 5%. fio gained 2.5% at one job and nothing at more: kqring stays near 1.1 million cached IOPS from two jobs on, while `psync` passes it at four and reaches 1.6 million at eight. The hand-off itself is the limit, and `BENCHMARK.MD` lists it under known gaps. SSD reads and writes are unchanged.

**Follow-up: reads in the submitting thread.** While a ring's recent reads didn't sleep and threads in enters take more than half the CPUs, its reads of local files run in the enter, as `pread(2)` would, with no hand-off. `test_wq_inline` checks them. fio's cached reads at 8 jobs gained 32%, to 1.46 million IOPS, 10% behind `psync` where they were 32% behind; at 1 to 4 jobs no read runs in the enter, and they lose up to 0.6%. SSD reads and writes are unchanged. ZFS reads that complete without a worker, and without a guess, are under research ([docs/async-zfs-reads.md](docs/async-zfs-reads.md)).

Acceptance: a profile that names the limit, then either a measured improvement or the limit documented in `BENCHMARK.MD`.

## Phase 6: Performance evidence

- Re-measure the full set after phases 4 and 5, and regenerate `BENCHMARK.MD` with `tests/bench_doc.py`. Now on the Ryzen 3 PRO 4350G, the machine the later phases were measured on. **Done:** `BENCHMARK.MD` is from `7641bf4` on the Ryzen, with the laptop's `v0.3.0` runs kept in `docs/bench/data-i5-8250u/`. Without PTI a system call is cheap, so single requests and fan-in trail kqueue there, where they led on the laptop; `v0.3.0` and `7641bf4` alternated on the Ryzen differ by a median of 0.7%, so the change is the machine's. On the SSD, the three engines are level once reads no longer meet the flush of earlier ZFS writes, which `fio/bench.sh` now waits out, and from 8 in flight kqring uses less CPU than aio.
- Run comparable Linux io_uring workloads on the same machine. Wins against kqueue and aio don't establish parity with Linux. **Done:** [docs/linux-comparison.md](docs/linux-comparison.md), on a laptop that boots both, with the raw results. kqring is ahead on the SSD with less CPU and per batched operation, an enter costs it about 250 ns more than io_uring's, and io_uring is ahead on cached file reads from several threads, which it can try without blocking.
- A benchmark in which the receiver has its own thread. **Done:** `kqring_bench stream`. From a registered buffer, `SEND_ZC` takes 37–55% less time than `write(2)` from 16 KiB up, where the single-threaded send group showed a loss up to 64 KiB. It also showed that `SEND_ZC` with `MSG_WAITALL` copied everything; it is now zero-copy (`9918fea`).
- An explanation of ZFS's two cached-read speeds. **Done:** they are ZFS's dbuf cache, capped at 1/32 of the ARC target. A file that doesn't fit makes each 4 KiB read rebuild a 128 KiB record from the ARC, at about 17.6 µs against 1.3 µs; one just written stays fast until its transaction group syncs, which the file group's earlier runs sometimes met partway through. The group now waits for the sync, and `BENCHMARK.MD` shows both speeds. kqring's reads slow down with `pread(2)`'s, so it was never the cause.
- Deferred: UFS on a real disk, and NVMe, which needs hardware the project doesn't have.

Acceptance: hardware, kernel, filesystem settings, queue depths, batch sizes, CPU use, latency distributions and raw repeated runs are published. Device saturation, filesystem costs and ring or worker overhead are told apart.

## Phase 7: Newer interfaces and real programs

Qualify a newer pinned liburing release, and real applications alongside fio. Audit header layouts, feature bits, probing, unsupported flags and fallback behavior.

Evaluate mixed-size SQEs and CQEs, hybrid IOPOLL, SQ rewind, `NOP128`, `URING_CMD128`, newer query and filter registrations and extended buffer registration, according to what programs need and what FreeBSD allows. The upstream development header is something to compare with, not a promise that every deployed Linux kernel has these.

- **liburing:** 2.15 is still the newest release (2.16 isn't tagged; checked 2026-11-10), and the port is on it.
- **Header audit:** against Linux's UAPI header at `21662435a5ee` (2026-11-04), the only capability added since the 7.3 baseline is `IORING_FEAT_REGBUF_TYPES`, for dma-buf registered buffers, which has no FreeBSD counterpart.
- **PostgreSQL 18.6:** **Done.** [`postgresql/`](postgresql/) builds it unpatched and runs it with `io_method=io_uring`. It needed rings that move between processes, which kqring now supports ([docs/cross-process-rings.md](docs/cross-process-rings.md)); before that, every backend PANICked on its first submission with `EEXIST`. All 231 regression tests pass, and pgbench runs level with PostgreSQL's own `worker` and `sync` methods. The same change makes liburing's `across-fork` pass.
- **Mixed-size SQEs and CQEs:** **Done.** `IORING_SETUP_CQE_MIXED`, `IORING_SETUP_SQE_MIXED` and `IORING_SETUP_SQ_REWIND`, with `NOP128`, `URING_CMD128`, `IORING_NOP_CQE32` and `IORING_CQE_F_SKIP`, as in Linux; `test_mixed_sizes` covers the wrap, overflow and resize cases. The 7 liburing tests that skipped without them pass, and liburing is at 218 of 245.
- **Next, by what liburing's tests need:** `IORING_REGISTER_QUERY` (2 tests). BPF filters and task restrictions are used only by BPF tests. Hybrid IOPOLL doesn't apply, since kqring has no `IOPOLL` on `O_DIRECT` files.

Acceptance: reproducible build and run instructions, and a result per program showing the patches it needs, the configurations supported and what still blocks it. New API additions need semantic tests, not just matching constants.

## Research

None of these is planned work. Each needs a design note covering kernel subsystem dependencies, lifetime rules, security implications and measurable benefit before it can become a phase.

- **Zero-copy receive:** check FreeBSD network-stack and driver support before committing to `RECV_ZC`. Linux's design depends on NIC queues, header and data splitting, steering and buffer recycling.
- **Driver commands:** assess a native driver contract if real programs need more than the existing socket commands. A matching opcode doesn't supply NVMe or other driver passthrough semantics.
- **Registered-buffer mapping independence:** assess using held pages for fixed I/O, including protection, copy-on-write and lifetime implications.
- **Asynchronous ZFS reads:** ZFS's I/O pipeline is asynchronous below the DMU, which waits for it. A read issued with a parent `zio` and completed from `zio_done` would finish at once on an ARC hit and from ZFS's callback on a miss, with no worker and no guess, as FreeBSD's asynchronous `sendfile(2)` does for the page cache. ZFS has no `VOP_GETPAGES_ASYNC` either, so `sendfile(2)` from ZFS blocks on every ARC miss, and the same work would fix it. [docs/async-zfs-reads.md](docs/async-zfs-reads.md) proposes two steps: a read that fails rather than wait, as Linux's `IOCB_NOWAIT`, then one that completes from ZFS's callback. Both change OpenZFS, so they would go there first.
- **Concurrent submitters and exec survival:** a ring now moves to another process when it is idle ([docs/cross-process-rings.md](docs/cross-process-rings.md)), but two processes can't have requests in flight on one ring at once, an owner's exit drops its requests without CQEs, and `exec` closes the ring. Lifting those needs per-process workers, taskwork and pollers for one ring.

Zero-copy `SPLICE` stays unplanned; see [`TODO.MD`](TODO.MD).

## Upstream references

- [Linux io_uring UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/io_uring.h)
- [liburing registered waits](https://github.com/axboe/liburing/blob/master/man/io_uring_register_wait_reg.3)
- [Linux zero-copy receive design](https://docs.kernel.org/networking/iou-zcrx.html)

These links move over time. Qualification work has to pin exact versions or commits.
