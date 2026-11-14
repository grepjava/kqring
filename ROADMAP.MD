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
| 7 | Newer interfaces and real programs | Done |
| 8 | Several processes submitting to one ring | Done |
| 9 | Registered buffers on held pages | Done |
| 10 | The cost of an enter | Done |
| 11 | Cached file reads | Done |
| 12 | Fan-in against kqueue | Done |
| 13 | Refresh the evidence | Done |
| 14 | liburing's remaining failures and skips | Done |
| 15 | A worker that can't hang, and small gaps | Done |
| 16 | Fan-in at a thousand connections | Done |

## Where kqring stands

`IORING_REGISTER_PROBE` reports 60 opcodes. Coverage includes shared submission and completion rings, basic and vectored I/O, fixed files and direct descriptors, registered and provided buffers, incremental buffer consumption, send and receive bundles, multishot operations, zero-copy send, links, drains, cancellation, timers, futex operations, SQPOLL, worker pool controls, ring resizing, personalities and restrictions.

FreeBSD-native extensions add persistent event-driven chains, result and buffer forwarding between steps, native kevent requests and a kqueue bridge. These stay an opt-in interface alongside the Linux-compatible API.

The liburing 2.15 port passes 223 of the 245 tests it builds, with `RLIMIT_MEMLOCK` raised. That isn't a compatibility percentage:

- 13 more tests can't be built here, each for a recorded reason.
- The port carries portability shims and adapts some tests, 17 tests skip, and the 5 failures are the platform's.
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

**Done** (11 November 2026). Two real programs, PostgreSQL and RocksDB, run their io_uring paths on kqring, each with instructions, its patches, its configuration and its tests. The newer interfaces programs and liburing's tests use are in, and liburing 2.15, still the newest release, passes 219 of 245. What still differs for programs is phase 8 (several processes submitting to one ring, exit and `exec`) and, under Research, zero-copy receive.

Qualify a newer pinned liburing release, and real applications alongside fio. Audit header layouts, feature bits, probing, unsupported flags and fallback behavior.

Evaluate mixed-size SQEs and CQEs, hybrid IOPOLL, SQ rewind, `NOP128`, `URING_CMD128`, newer query and filter registrations and extended buffer registration, according to what programs need and what FreeBSD allows. The upstream development header is something to compare with, not a promise that every deployed Linux kernel has these.

- **liburing:** 2.15 is still the newest release (2.16 isn't tagged; checked 2026-11-10), and the port is on it.
- **Header audit:** against Linux's UAPI header at `21662435a5ee` (2026-11-04), the only capability added since the 7.3 baseline is `IORING_FEAT_REGBUF_TYPES`, for dma-buf registered buffers, which has no FreeBSD counterpart.
- **PostgreSQL 18.6:** **Done.** [`postgresql/`](postgresql/) builds it unpatched and runs it with `io_method=io_uring`. It needed rings that move between processes, which kqring now supports ([docs/cross-process-rings.md](docs/cross-process-rings.md)); before that, every backend PANICked on its first submission with `EEXIST`. All 231 regression tests pass, and pgbench runs level with PostgreSQL's own `worker` and `sync` methods. The same change makes liburing's `across-fork` pass.
- **RocksDB 11.13.2:** **Done.** [`rocksdb/`](rocksdb/) builds it against the liburing port with a two-line patch for RocksDB's own Linux assumptions (a `pthread_t` cast, a test's include), and a fix to the port's `<linux/fs.h>`, which brought `<sys/param.h>` into programs. Its io_uring tests pass (7467 in 5 programs). `MultiGet` reads from the SSD 28% faster with io_uring than with `pread(2)` at one thread and 17% at four; `MultiRead` only batches the blocks a batch needs from one file.
- **Mixed-size SQEs and CQEs:** **Done.** `IORING_SETUP_CQE_MIXED`, `IORING_SETUP_SQE_MIXED` and `IORING_SETUP_SQ_REWIND`, with `NOP128`, `URING_CMD128`, `IORING_NOP_CQE32` and `IORING_CQE_F_SKIP`, as in Linux; `test_mixed_sizes` covers the wrap, overflow and resize cases. The 7 liburing tests that skipped without them pass, and liburing is at 218 of 245.
- **`IORING_REGISTER_QUERY`:** **Done.** With or without a ring, it reports kqring's opcode counts, setup, enter and SQE flags and features, and the rings' header; zero-copy receive's query says it is missing. liburing's `ring-query` passes, and liburing is at 219 of 245. What liburing's tests still skip for is BPF filters and task restrictions, used only by BPF tests, and zero-copy receive (see Research). Hybrid IOPOLL doesn't apply, since kqring has no `IOPOLL` on `O_DIRECT` files.

Acceptance: reproducible build and run instructions, and a result per program showing the patches it needs, the configurations supported and what still blocks it. New API additions need semantic tests, not just matching constants.

## Phase 8: Several processes submitting to one ring

A ring moves between processes since phase 7, but only one may have requests in flight, an exiting process's requests end without CQEs, and `exec` closes the ring. Linux runs each task's requests in its own context, posts a CQE for every request at exit and exec, and keeps the ring open.

[docs/concurrent-submitters.md](docs/concurrent-submitters.md) is the design: a context per process on each ring, with its own worker pool, taskwork and poller, while the ring's shared state stays serialized as it is now. Exit and exec complete their process's requests with CQEs, and the ring stays open while a descriptor holds it. Registered buffers and kqring's extensions stay with one process, and a set-ID `exec` still closes the ring.

- **Steps 1 to 3:** **Done.** Each ring keeps a context per process, with its own worker pool, taskwork and kernel thread; any process holding a shared ring submits to it whatever the others have in flight; exit and `exec` complete a process's requests with CQEs and leave the ring open. [What the implementation changed](docs/concurrent-submitters.md#what-the-implementation-changed) from the design: exit and exec finish on a kernel task, standalone `TIMEOUT`s outlive their process, and the poller is still in one process. liburing's three subtests in which a forked child uses its parent's ring run again, and the suite stays at 219. The fuzzer's forked children submit requests of their own and exit with them in flight. On the Ryzen, `kqring_bench` and `kqring_mt_bench` alternated five times with the build before: the median row moved by 0.5%, a lone `NOP` by 1%, within the runs' spread, and `NOP`s in batches of 32 are 2% (2 ns) slower.
- **Step 4:** **Done.** PostgreSQL 18.6 again: all 231 regression tests pass, and pgbench stays level with its `worker` and `sync` methods ([postgresql/README.md](postgresql/README.md)). Each context now has its own poller, so any process sharing a ring polls pipes and other files that aren't sockets, where only the first could before. `tests/kqring_diff.c` gained `fork_inflight` (two processes' polls in flight, at the same descriptor number, and a `TIMEOUT` outliving its process) and `exit_armed`, and `exec` now shows the CQ the new image sees: `fork`, `fork_inflight` and `exec` match Linux 7.3, where `fork` and `exec` were listed differences. `exit_armed` showed that Linux cancels at exit only requests on io_uring files and ends the rest when they would next run, where kqring cancels them all at once; it is listed. On the Ryzen, alternated five times with the step 3 build, the median row of `kqring_bench` and `kqring_mt_bench` moved by 0.1%.

Acceptance: the tests the design lists, the fuzzer with forked submitters, liburing and PostgreSQL unchanged or better, single-process performance unchanged, and the differences that remain listed in the matrix.

Phase 8 is done: every criterion is met. What still differs is in the [matrix](COMPATIBILITY.MD#boundaries): registered buffers work only in the process that registered them, kqring's extensions keep a ring to one process, a set-ID `exec` closes the rings the process holds, and exit cancels a process's requests sooner than Linux.

## Phase 9: Registered buffers on held pages

Registered buffers hold their pages, but only zero-copy sends use them: fixed reads and writes copy through the buffer's user addresses. So a buffer works only in the process that registered it, follows its mapping rather than its pages after `munmap(2)` or a new mapping, splits from the parent's memory after `fork(2)`, may be a shared file mapping, and is at most 16 MiB. Linux pins the pages and does I/O through them.

[docs/held-buffers.md](docs/held-buffers.md) is the design: map each buffer's held pages in kernel address space and do fixed I/O through that mapping, refuse vnode-backed pages, re-hold a forking process's buffers for writing so that the parent keeps them, and give direct disk I/O the held pages. `tests/kqring_diff.c` has `buffer_lifetime`, `buffer_fork` and `buffer_backing` against Linux 7.3.

Acceptance: those three cases match Linux 7.3; liburing's fixed-buffer tests pass as before or better; the unit tests, stress and fuzzer pass, the fuzzer with fixed requests from forked children; single-process performance unchanged, and direct disk reads into fixed buffers faster or level; the differences that remain listed in the matrix.

Progress (12 November 2026): implemented, with the order of work's steps 1 and 3 as one change, and the acceptance met on the Ryzen host:

- `buffer_backing` and `buffer_fork` match Linux 7.3, and `buffer_lifetime` matches after `munmap(2)`. A new anonymous mapping over the unmapped range can share the buffer's pages, which is listed with its reason, as is the read in flight during a fork. The diff has 29 cases and no problems.
- liburing passes 220 of 245 (was 219). `io_uring_register` passes, since a shared file mapping is now refused. `send-zerocopy` passes now that its 32 MiB buffer registers. Its UDP cases found a bug: a `SEND` that the protocol refused after copying the datagram completed with the full length. That is fixed, and the test skips the two UDP cases FreeBSD refuses.
- The unit tests (144, plus the held-buffer test with a 20 MiB buffer), the cross-process test, stress, and the fuzzer (seeds 7301 and 7302, with fixed reads and writes from forked children) pass.
- Direct reads from an md(4) disk with `--fixedbufs` are 0.5% faster at 4 KiB and 1.4% at 64 KiB (fio, median of three); without fixed buffers they're level. `kqring_bench` and `kqring_mt_bench` are level (median +0.2% over five alternated rounds).

Phase 9 is done: every criterion is met. What still differs is in the [matrix](COMPATIBILITY.MD#boundaries): a new anonymous mapping over an unmapped buffer can share its pages, and a fixed read in flight during `fork(2)` lands in the child's copy.

## Phase 10: The cost of an enter

One `NOP` per enter cost kqring 438 ns above FreeBSD's system call floor on the laptop that ran both systems, and io_uring 178 ns above Linux's ([docs/linux-comparison.md](docs/linux-comparison.md#the-rings-own-costs)). That cost is paid by every request a program submits on its own, and it is why single requests and fan-in trail kqueue on the Ryzen ([BENCHMARK.MD](BENCHMARK.MD#why-the-results-depend-on-the-cpu)).

[docs/per-request-cost.md](docs/per-request-cost.md) profiles the enter and records what each change gained.

Acceptance: one `NOP` per enter costs no more above FreeBSD's floor than io_uring above Linux's on the same machine; the unit tests, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before; `kqring_bench`, `kqring_mt_bench` and fio are level or better.

Progress (12 November 2026), on the Ryzen:

- Every enter that completed something woke the ring's idle kernel thread, which then contended for `sq_sx`. Now only threads waiting for CQEs are woken: one `NOP` per enter went from 387 to 217 ns, a pipe `READ` from 629 to 505 ns, an AF_UNIX `RECV` from 755 to 595 ns.
- Each request cleared 680 bytes with `rep stos`; it now clears 456 bytes with stores: one `NOP` per enter 218 to 200 ns, 32 per enter 83.4 to 71.6 ns per `NOP`.
- An enter on a ring without `SINGLE_ISSUER` no longer takes `ctx->mtx` to find its process's context when that process has the ring's only one, nor to signal completions when nothing waits or polls; and three calls that usually had nothing to do are checked inline first. One `NOP` per enter: 200.9 to 193.7 ns on the Ryzen.
- Acceptance met. Against the phase 9 build on the Ryzen, `kqring_bench` is 1.0% faster (median row) and `kqring_mt_bench` level, with no row more than 5% slower; fio's direct reads are level; the unit tests, cross-process test, stress, fuzzer, liburing (220 of 245) and the diff with Linux 7.3 pass.
- On the i5-8250U laptop, booting each system in turn: one `NOP` per enter costs kqring 165 ns above FreeBSD's system call floor, where it cost 622 ns with the phase 9 build, and io_uring 173 ns above Linux 7.3's. A pipe `READ` costs 255 ns above `read(2)` (Linux: 266), an AF_UNIX `RECV` 181 ns above `recv(2)` (Linux: 328).

Phase 10 is done: every criterion is met. What is left of an enter's cost is spread thinly, and fan-in and single requests still trail kqueue on the Ryzen (see [`TODO.MD`](TODO.MD)).

## Phase 11: Cached file reads

On the laptop that ran both systems, fio's cached reads of a tmpfs file, 4 jobs at 32 in flight, reached 34% of FreeBSD's own `psync` on kqring and 58% of Linux's on io_uring ([docs/linux-comparison.md](docs/linux-comparison.md#fio)). Linux reads a cached page in the submitting thread and hands only a read that would block to a worker.

[docs/cached-reads.md](docs/cached-reads.md) finds why kqring didn't on tmpfs and records what changed.

Acceptance: a cached read of a tmpfs file on a default mount completes in the enter, without a worker and without sleeping; on the same machine, kqring's 4 jobs at 32 in flight reach at least the share of `psync` that io_uring's reach of Linux's; the unit tests, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before.

Progress (13 November 2026):

- kqring already read cached pages in the enter, through `VOP_READ_PGCACHE`, but tmpfs offers it only on a mount with `-o pgread`, which is off by default, so every tmpfs read went to a worker. On the Ryzen, `-o pgread` alone took 4 jobs at 32 in flight from 53% of `psync` to 87%.
- kqring now reads tmpfs pages itself, with or without the option, and never pages one in from swap in the enter, which tmpfs's own read does. A default mount reads as fast as a `pgread` one: 90% of `psync` on the Ryzen. No read goes to a worker.
- On the i5-8250U, booting each system: kqring's 4 jobs at 32 in flight reach 0.80 of FreeBSD's `psync` (0.39 before), io_uring's 0.67 of Linux's, and kqring reads 1.34 times as many pages a second. One job at 32 in flight, io_uring is 5% ahead.
- `read(2)` of a tmpfs file into a writable shared mapping of the same page hangs on FreeBSD 15.1, without kqring; a worker reading it would hang the same way.
- The fuzzer, with a new seed, found a panic from phase 9: a `fork(2)` in one thread released a registered buffer's old pages while the `SINGLE_ISSUER` ring's owner, which submits without `sq_sx`, was taking them for a request. The submitter now releases them ([docs/held-buffers.md](docs/held-buffers.md#4-fork2)).
- Acceptance met. The unit tests (also in a tmpfs directory), the cross-process test, stress, the fuzzer (seed 7313), liburing (220 of 245) and the comparison with Linux 7.3 pass.

Phase 11 is done: every criterion is met. One job reading cached pages in the enter is still slower than `pread(2)`: kqring's own functions cost about 190 ns per read (see [`TODO.MD`](TODO.MD)).

## Phase 12: Fan-in against kqueue

On the Ryzen, where system calls are cheap, `kqring_bench`'s fan-in (many connections each get one message, and all are collected) was 26–66% slower than kqueue, and AF_UNIX ping-pong 2–16% ([BENCHMARK.MD](BENCHMARK.MD#why-the-results-depend-on-the-cpu)). [docs/fan-in.md](docs/fan-in.md) measures where the time goes.

Progress (13 November 2026), on the Ryzen:

- `tests/kqring_xbench.c` gains fan-in cases, on Linux too: kqueue (or epoll), single-shot `RECV`s in one enter, and multishot `RECV`s. With buffers that stay in cache, a `DEFER_TASKRUN` ring is level with kqueue or ahead at 16 and 256 connections. `kqring_bench`'s fan-in gives kqring 4,096 buffers of 4 KiB in turn, where its kqueue variant reads into one: part of its gap is that.
- At 1,024 connections a multishot receive was about 65% slower than kqueue: each message brought about five cache lines from memory, where kqueue's brought half of one. Its fields now take 4 of a request's cache lines instead of 11: 812 to 579 ns per message, against kqueue's 497.
- A multishot receive that took its group's last buffer ended with `-ENOBUFS` at once; as in Linux, it now waits for data with no buffer for it.
- A `RECV` submitted before its data, on a `DEFER_TASKRUN` ring, spends about 250 ns arming, waking and receiving again on both the Ryzen and the i5, where io_uring spends 651 ns on the same i5; it is 1.18 times kqueue's loop there, as io_uring is 1.21 times epoll's. On the Ryzen, where a system call costs 53 ns, the same 250 ns is 60% of kqueue's loop. The time is spread over the path, and what could go without a redesign is under 3%. A default ring adds 260 ns (Ryzen) and 563 ns (i5) per message for its kernel thread, where Linux runs the taskwork in the submitter.
- A default ring's kernel thread now sleeps apart from the enters waiting for CQEs, and taskwork wakes it only when no thread of the process waits in an enter, which runs it itself. With a peer process echoing (`xp_ring_armed`), a default ring now costs what a `DEFER_TASKRUN` ring does: 3,925 ns per round trip, from 3,985; kqueue 3,620. On a `COOP_TASKRUN` ring a completion queued in a thread of the ring's process now runs as that thread returns to user space, as Linux's task_work does: single-thread ping-pong 1,083 to 981–1,028 ns, half of the default ring's gap, and multishot fan-in written by the ring's own thread 50% slower, as each `write(2)` runs its receive. Paths that don't use it are 1–2% slower, from where the code now lies.
- At 1,024 connections the remaining gap was the working set outgrowing the L3: the extra lines from memory were mostly in the writer's `write(2)`, not kqring's code. Requests came from `malloc(9)`'s 1 KiB bucket, which put the lines a receive uses in a quarter of the cache's sets; a zone of their own makes multishot 10–12% faster there (562–583 ns per message against kqueue's 500–534), with other paths unchanged.
- Validated on the Ryzen with each change: the unit tests, the cross-process test, the comparison with Linux 7.3, liburing (220 of 245), stress and the fuzzer pass.

Phase 12 is done. With buffers that stay in cache, fan-in on a `DEFER_TASKRUN` ring is level with kqueue or ahead up to 512 connections, and about 10% behind at 1,024, where the working set outgrows the L3. What is left is in [`TODO.MD`](TODO.MD).

## Phase 13: Refresh the evidence

Every figure in [BENCHMARK.MD](BENCHMARK.MD) is from `7641bf4` or `9918fea`, before phases 7 to 12, and [`docs/conformance/`](docs/conformance/) still has the 25-case comparison with Linux where `tests/kqring_diff.c` now has 30. `kqring_bench` also gave kqring's receives 4,096 provided buffers of 4 KiB in turn, out of cache, where kqueue read into one buffer, and most of the fan-in gap it reported was that.

Order of work:

1. `kqring_bench` sizes each group's provided buffers to what it has in flight: ping-pong 16 of 4 KiB, fan-in one buffer of the message's size per connection. Other groups keep the 4,096 × 4 KiB pool.
2. Every `kqring_bench`, `kqring_mt_bench` and `kqring_diskbench` run behind BENCHMARK.MD repeated on the Ryzen from one build, by its method; the document regenerated and its prose checked against the new figures.
3. `docs/conformance/` with Linux 7.3's and kqring's output of all 30 cases.

Acceptance: BENCHMARK.MD names one build for every figure and its raw runs are kept; no claim in it, the README's summary or this roadmap contradicts the new figures; `docs/conformance/` holds the 30-case outputs and `tests/diff_compare.py` passes on them.

Progress (14 November 2026):

- Step 1: on the Ryzen, with the same module, the kqueue rows didn't move, and kqring's multishot fan-in went from 510–614 to 379–471 ns per message, now ahead of kqueue at 16 and 256 connections and level at 1,024; multishot ping-pong at 64 B went from 622–629 to 535–540, level with kqueue's 537–544.
- Step 2: every `kqring_bench` group (5 runs, the module reloaded before each), its disk group on md(4), its file group on UFS, its stream group, `kqring_mt_bench`, `kqring_diskbench` and fio's `bench.sh` and `scale.sh`, from `28e67b2` on the Ryzen ([`system.txt`](docs/bench/data/system.txt)). The ZFS file table was run again after fio: run early, with the ARC target at 241 MiB, the file fit neither dbuf cache size, and `zfs-dbuf/run.sh` now fails when that happens. 75 of the 100 rows the old set shares with the new moved less than 4%; the rest are the buffer change, and ZFS writes in fio, which vary between runs. `kqring_diskbench -V` passed on the md disk and a ZFS file.
- Step 3: `kqring_diff` three times on Linux 7.3.0-10 (i5) and on kqring (Ryzen), each identical across runs; of 30 cases 25 match and 5 differ on purpose.
- BENCHMARK.MD's generator now fails if its summary's claims about fan-in and ping-pong stop holding. Its comparison with the i5 leaves out the laptop's fan-in, measured with the old buffer pool, and the README marks those rows. The claim that an enter costs kqring 250 ns more than io_uring, from before phase 10, is corrected in the README and docs/linux-comparison.md.

Phase 13 is done: BENCHMARK.MD names one build for every figure, with its raw runs; on the Ryzen kqring is now level with kqueue on sockets, ahead on fan-in up to 256 connections, and behind on a single request per enter.

## Phase 14: liburing's remaining failures and skips

Every liburing test that fails or skips has a written reason ([liburing/README.md](liburing/README.md#test-results)). This phase checks each reason again and turns into a pass whatever kqring can.

Acceptance: each of the 6 failures and 19 skips passes, or its reason is still true and written down; what kqring adds matches Linux 7.3 in `tests/kqring_diff.c`; the unit tests, the cross-process test, stress, the fuzzer and liburing pass as before or better.

Progress (14 November 2026), on the Ryzen:

- Restrictions and classic BPF filters for a process (`IORING_REGISTER_RESTRICTIONS` without a ring, `IORING_REGISTER_BPF_FILTER`), from Linux 7.3's source: `task-restrict` and `cbpf_filter`, which skipped, pass, and three new `kqring_diff` cases match Linux 7.3 line for line ([docs/bpf-filters.md](docs/bpf-filters.md)). kqring keeps them per process rather than per thread. Along the way, a ring's restrictions that named only request opcodes no longer refuse its registrations.
- `iopoll-sync` failed only on `getsockopt(2)` returning the option's bit; adapted as `socket-getsetsock-cmd` is, it passes.
- liburing: 223 of 245 pass (was 220), 17 skip, 5 fail. The failures (`self`, `pipe-reuse`, `splice`, `recv-bundle-short-ooo`, `vec-regbuf`) and the remaining skips were checked again against their output, and still hold: `self` can't open `/proc/self/comm`; `vec-regbuf` reads 48,972 bytes of 49,185 from ZFS; `pipe-reuse` and `splice` block in the test's own `write(2)` to a pipe (`pipe_direct_write`), and `recv-bundle-short-ooo` in its `write(2)` to a full loopback socket (`sosend`); each skip prints the missing facility its row names, or skips before printing anything.
- Validation of the build: unit tests 147 of 147, the cross-process test 20 rounds, `kqring_diff` 33 cases without a problem, stress 600 s and the fuzzer 1,800 s (seed 7319) passed, with no new crash dump.

Phase 14 is done: the two skips kqring could turn into passes, and one failure, now pass; every remaining failure and skip has a reason confirmed on this build.

## Phase 15: A worker that can't hang, and small gaps

A kqring worker reading a tmpfs file into a shared mapping of the same page hangs for good, as `read(2)` does on FreeBSD 15.1 ([bug 300734](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300734)): the thread can't be killed and the machine can't finish a shutdown. Any program can cause it. Four smaller differences from Linux 7.3 are also left in [COMPATIBILITY.MD](COMPATIBILITY.MD).

Order of work:

1. A worker's tmpfs read holds the destination's pages first and copies into them through the direct map, so the read never faults on a page tmpfs holds busied. The unit test that skips this case on tmpfs runs it.
2. `OPENAT2` takes `RESOLVE_IN_ROOT`, `RESOLVE_NO_XDEV` and `RESOLVE_NO_SYMLINKS`. FreeBSD's lookup takes its root from the process, so kqring walks the path a component at a time with these, as Linux's `openat2(2)` defines them.
3. `IORING_OP_NOP` takes `IORING_NOP_FILE`, `IORING_NOP_FIXED_FILE`, `IORING_NOP_FIXED_BUFFER` and `IORING_NOP_TW`, as Linux 7.3's `io_nop()`.
4. `IORING_SETUP_HYBRID_IOPOLL` is accepted with `IORING_SETUP_IOPOLL`, as Linux.
5. On a `SINGLE_ISSUER` ring without `DEFER_TASKRUN`, another thread's wait waits, as on Linux, rather than fail with `EEXIST`.

Acceptance: the tmpfs case completes in the unit tests on tmpfs, ZFS and UFS; each new behaviour matches Linux 7.3 in `tests/kqring_diff.c`; COMPATIBILITY.MD rows move to Equivalent or name what is left; the unit tests, the cross-process test, stress, the fuzzer and liburing pass as before or better.

Progress (14 November 2026), on the Ryzen:

- Step 1: a worker's read or write of a regular file on a filesystem without `MNTK_NO_IOPF` holds the user's pages, 1 MiB at a time, and the filesystem copies through their direct-map addresses ([docs/cached-reads.md](docs/cached-reads.md)). The unit test reads, writes and reads vectored into a fresh shared mapping of the same file on tmpfs, UFS on md(4) and ZFS; on tmpfs it used to be skipped, because it hung.
- Step 2: `OPENAT2` walks the path itself for `RESOLVE_IN_ROOT`, `RESOLVE_NO_XDEV` and `RESOLVE_NO_SYMLINKS` ([docs/openat2-resolve.md](docs/openat2-resolve.md)); `RESOLVE_BENEATH` fails with Linux's `EXDEV`. The new `openat2_resolve` case, 161 opens over symlinks of every kind and `/dev`'s mount, matches Linux 7.3 line for line, and liburing's `cbpf_filter` keeps `RESOLVE_IN_ROOT`.
- Steps 3 and 4: NOP's four flags and `HYBRID_IOPOLL` match Linux 7.3 (`nop_flags`, `setup_flags`).
- Step 5: another thread's wait on a `SINGLE_ISSUER` ring waits, for the owner's, the kthread's and `COOP_TASKRUN`'s completions (`single_issuer_wait`). Still `EEXIST`: a `DEFER_TASKRUN` ring, as Linux, and a polled `IOPOLL` wait.
- Found by comparing with Linux, and fixed: a `DEFER_TASKRUN` ring ran its taskwork in every enter, where Linux runs it only with `IORING_ENTER_GETEVENTS`; a `MSG_RING` to its own ring posted after the request's CQE; `IOSQE_FIXED_FILE` on an opcode that takes no file failed with `EINVAL`, where Linux ignores it, or fails a path opcode with `EBADF`. New cases `fixed_flag` and `msg_ring_self` match Linux 7.3. Unit tests that relied on the old taskwork timing now wait for their CQEs, and those that asserted the old errors expect Linux's.
- Validation: unit tests 151 of 151, the cross-process test 20 rounds, `kqring_diff` 38 cases without a problem (33 match, 5 differ on purpose; [docs/conformance/](docs/conformance/) from three identical runs on each system), liburing 223 passed, 17 skipped, 5 failed as before, stress 600 s and the fuzzer 1,800 s (seed 1515) passed, with no new crash dump.

Phase 15 is done: no kqring worker hangs on a self-mapped tmpfs file, and the four gaps are closed except a polled wait from another thread on a `SINGLE_ISSUER` `IOPOLL` ring.

## Phase 16: Fan-in at a thousand connections

With multishot receives armed on 1,024 AF_UNIX connections, kqring collects a message about 10% slower than kqueue on the Ryzen, and 18% slower at 2,048; up to 512 it is level or ahead ([docs/fan-in.md](docs/fan-in.md#what-is-left)). Most of the extra time is the writer's `write(2)` waiting for the socket's own lines from memory: the working set has outgrown the 4 MiB L3, and what each armed receive adds per connection decides how far. Each message touches the request's four hot lines, the socket's waiter list (a separate allocation), on AF_UNIX the end-of-input knote, and the provided buffer, where kqueue touches a knote.

Order of work:

1. Measure the build from phase 15 at 512, 1,024 and 2,048 connections, kqueue against multishot on both kinds of ring, with the DRAM fills per function, to know what each line is worth.
2. Fewer lines per armed receive, each change measured on its own against the one before: the request's cycle in fewer lines; the waiter list in the request when it is the socket's only waiter; on AF_UNIX, one structure per connection rather than an upcall and a knote.
3. Keep what helps, and check what it might slow: one `NOP` per enter, ping-pong, fan-in at 16 and 256, the single-shot batch.

Acceptance: multishot fan-in at 1,024 and 2,048 connections on a `DEFER_TASKRUN` ring within 5% of kqueue or ahead, or what stops it measured and written down; nothing else in `kqring_xbench` more than 2% slower; the unit tests, the cross-process test, `kqring_diff`, liburing, stress and the fuzzer pass as before.

Progress (14 November 2026), on the Ryzen ([docs/fan-in.md](docs/fan-in.md#a-thousand-connections-again)):

- The request's upcall fields have a cache line of their own, its hot fields sit in the next lines, and a socket's only waiter is the upcall's argument, with no waiter list. Multishot fan-in is 6–8% faster at 1,024 and 2,048 connections on both kinds of ring; nothing else in `kqring_xbench` moved more than 1.2%.
- On a `DEFER_TASKRUN` ring multishot is now 2% behind kqueue at 1,024 connections, from 11%, and 8% at 2,048, from 16%; a default ring is level at 1,024 and 9% ahead at 2,048. At 2,048 the working set still outgrows the L3: each message brings 1.4 more lines from memory than kqueue's, 0.4 of them the provided buffer and its ring entry.
- The lone waiter first panicked the stress test: moving a lone waiter onto a list set its `sw[i]` to NULL for a moment, which its own thread reads without the sockbuf lock, and it was armed twice. It now changes in one store.
- Moving the module's code alone changes fan-in at 2,048 by up to 5% and the single-shot batch by 4%; layouts without the lone waiter gave less, or cost the batch.

Phase 16 is done: within 5% of kqueue at 1,024 connections, and what stops it at 2,048 measured and written down.

## Research

None of these is planned work. Each needs a design note covering kernel subsystem dependencies, lifetime rules, security implications and measurable benefit before it can become a phase.

- **Zero-copy receive:** check FreeBSD network-stack and driver support before committing to `RECV_ZC`. Linux's design depends on NIC queues, header and data splitting, steering and buffer recycling.
- **Driver commands:** assess a native driver contract if real programs need more than the existing socket commands. A matching opcode doesn't supply NVMe or other driver passthrough semantics.
- **Asynchronous ZFS reads:** ZFS's I/O pipeline is asynchronous below the DMU, which waits for it. A read issued with a parent `zio` and completed from `zio_done` would finish at once on an ARC hit and from ZFS's callback on a miss, with no worker and no guess, as FreeBSD's asynchronous `sendfile(2)` does for the page cache. ZFS has no `VOP_GETPAGES_ASYNC` either, so `sendfile(2)` from ZFS blocks on every ARC miss, and the same work would fix it. [docs/async-zfs-reads.md](docs/async-zfs-reads.md) proposes two steps: a read that fails rather than wait, as Linux's `IOCB_NOWAIT`, then one that completes from ZFS's callback. Both change OpenZFS, so they would go there first.

Zero-copy `SPLICE` stays unplanned; see [`TODO.MD`](TODO.MD).

## Upstream references

- [Linux io_uring UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/io_uring.h)
- [liburing registered waits](https://github.com/axboe/liburing/blob/master/man/io_uring_register_wait_reg.3)
- [Linux zero-copy receive design](https://docs.kernel.org/networking/iou-zcrx.html)

These links move over time. Qualification work has to pin exact versions or commits.
