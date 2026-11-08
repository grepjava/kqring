# kqring roadmap

This is the plan for moving kqring from an experimental preview towards predictable compatibility with Linux io_uring and scalable execution of what it already does. It is an engineering plan: it isn't a claim of complete Linux compatibility, and it isn't a release schedule.

It comes from a parity review of 6 November 2026, checked against the source at `3a5f36c`. Phases run in order, and each has to meet its acceptance criteria before it counts as done. Opcode coverage alone doesn't establish behavioral or performance parity.

See also [`TODO.MD`](TODO.MD) for individual open items, [Differences from Linux](README.md#differences-from-linux), [`BENCHMARK.MD`](BENCHMARK.MD#known-gaps) for known gaps, and the [liburing test accounting](liburing/README.md).

| Phase | Work | Status |
|---|---|---|
| 1 | Ordering and poll timing | Done |
| 2 | Make compatibility measurable | Done |
| 3 | Wait semantics | Not started |
| 4 | Per-file write ordering | Not started |
| 5 | Worker path scalability | Not started |
| 6 | Performance evidence | Not started |
| 7 | Newer interfaces and real programs | Not started |

## Where kqring stands

`IORING_REGISTER_PROBE` reports 60 opcodes. Coverage includes shared submission and completion rings, basic and vectored I/O, fixed files and direct descriptors, registered and provided buffers, incremental buffer consumption, send and receive bundles, multishot operations, zero-copy send, links, drains, cancellation, timers, futex operations, SQPOLL, worker pool controls, ring resizing, personalities and restrictions.

FreeBSD-native extensions add persistent event-driven chains, result and buffer forwarding between steps, native kevent requests and a kqueue bridge. These stay an opt-in interface alongside the Linux-compatible API.

The liburing 2.15 port passes 209 of the 245 tests it builds, with `RLIMIT_MEMLOCK` raised. That isn't a compatibility percentage:

- 13 more tests can't be built here, each for a recorded reason.
- The port carries portability shims and adapts some tests, 27 tests skip, and 8 of the 9 failures are intentional differences or the platform's.
- A test stops at its first failure, so the subtests after it haven't run.

The probe also can't speak for every variant of an operation. `SYNC_FILE_RANGE` syncs the whole file, `URING_CMD` supports four socket commands only, and `IORING_REGISTER_CLOCK` is recorded but has no effect.

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

**Done.** [`COMPATIBILITY.MD`](COMPATIBILITY.MD) lists all 215 capabilities against Linux 7.3 and liburing 2.15, and [`tests/compat_check.py`](tests/compat_check.py) fails if it stops matching the source. Its [validation gaps](COMPATIBILITY.MD#validation-gaps) list what is still unchecked.

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

**Done.** [`tests/kqring_diff.c`](tests/kqring_diff.c) has 21 cases that build and run on both systems, and [`tests/diff_compare.py`](tests/diff_compare.py) compares their output. On Linux 7.3 and kqring, 16 match and 5 differ on purpose, with the reasons in [`tests/diff_expected.txt`](tests/diff_expected.txt). The raw results and what they ran on are in [`docs/conformance/`](docs/conformance/README.md). The runs found `execve(2)` hanging on a ring with a kernel thread, and six differences that are now fixed: short transfers in links, `CANCEL_ALL` with nothing to cancel, `IORING_POLL_ADD_LEVEL`, a refused `FILES_UPDATE`, another thread's enter on a `SINGLE_ISSUER` ring, and which setup flags go together.

Run matched cases on Linux io_uring and on kqring, normalizing native constants and errors while keeping meaningful differences. Exercise cancellation, links, timeouts, buffer lifetime, CQ overflow, resource updates, fork and exec, fixed descriptors and several submitter threads. The test machine also boots Linux, so both sides can run on the same hardware.

Acceptance: kernel versions, commits, build options, filesystem, resource limits and raw results are kept with each run. Expected differences are listed, and an unexpected difference fails. Native tests and adapted upstream tests stay distinguishable.

## Phase 3: Wait semantics

This is the largest gap the review found in the existing liburing interface. Today `kqring_enter()` rejects a signal mask in both forms with `EINVAL`, timeouts are always relative, and `IORING_ENTER_ABS_TIMER`, `IORING_ENTER_EXT_ARG_REG` and `IORING_REGISTER_MEM_REGION` don't exist.

In order:

1. Signal masks, in the plain form and through `IORING_ENTER_EXT_ARG`.
2. Absolute deadlines (`IORING_ENTER_ABS_TIMER`), and giving `IORING_REGISTER_CLOCK` an effect.
3. Registered waits, which need `IORING_REGISTER_MEM_REGION`, and `io_uring_register_wait_reg()` with `reg-wait` built and running.

Acceptance: conformance tests for interruption, timeout expiry, repeated registered waits, clock choice, suspend behavior, invalid arguments and teardown. Whatever differs afterwards stays visible in the matrix.

## Phase 4: Per-file write ordering

Buffered writes wait for their worker, which keeps writes to one file in order but makes the issuer wait, where io-wq's per-inode hashing wouldn't. They are about 9% behind aio on a ZFS file. `O_APPEND` and file-position writes wait the same way.

Add per-file ordering to the worker pool, so that writes to one file stay in order, writes to different files run in parallel, and the issuer doesn't wait. This changes how the pool schedules work, so it starts with a design note and new fuzzer coverage of write ordering across files.

Acceptance: ordering and data integrity shown under concurrency, cancellation and close; parallel progress across files; CPU, throughput and latency compared with the current code.

## Phase 5: Worker path scalability

Cached reads through workers stop near 530,000 IOPS on a 4-core machine, and `psync` passes them from 4 jobs. On ZFS at 32 in flight, kqring is 4–5% behind aio with a longer p99, at less than half the CPU.

Profile first. These two may have different causes, and each request crossing to another thread may set a limit that can't be removed. If so, the outcome is a documented cost of that path, not a scheduling change.

Acceptance: a profile that names the limit, then either a measured improvement or the limit documented in `BENCHMARK.MD`.

## Phase 6: Performance evidence

- Re-measure the full set after phases 4 and 5, and regenerate `BENCHMARK.MD` with `tests/bench_doc.py`.
- Run comparable Linux io_uring workloads on the same machine. Wins against kqueue and aio don't establish parity with Linux.
- Measure UFS on a real disk, with a benchmark in which the receiver has its own thread, and explain ZFS's two cached-read speeds.
- NVMe measurements need hardware the project doesn't have yet.

Acceptance: hardware, kernel, filesystem settings, queue depths, batch sizes, CPU use, latency distributions and raw repeated runs are published. Device saturation, filesystem costs and ring or worker overhead are told apart.

## Phase 7: Newer interfaces and real programs

Qualify a newer pinned liburing release, and real applications alongside fio. Audit header layouts, feature bits, probing, unsupported flags and fallback behavior.

Evaluate mixed-size SQEs and CQEs, hybrid IOPOLL, SQ rewind, `NOP128`, `URING_CMD128`, newer query and filter registrations and extended buffer registration, according to what programs need and what FreeBSD allows. The upstream development header is something to compare with, not a promise that every deployed Linux kernel has these.

Acceptance: reproducible build and run instructions, and a result per program showing the patches it needs, the configurations supported and what still blocks it. New API additions need semantic tests, not just matching constants.

## Research

None of these is planned work. Each needs a design note covering kernel subsystem dependencies, lifetime rules, security implications and measurable benefit before it can become a phase.

- **Zero-copy receive:** check FreeBSD network-stack and driver support before committing to `RECV_ZC`. Linux's design depends on NIC queues, header and data splitting, steering and buffer recycling.
- **Driver commands:** assess a native driver contract if real programs need more than the existing socket commands. A matching opcode doesn't supply NVMe or other driver passthrough semantics.
- **Registered-buffer mapping independence:** assess using held pages for fixed I/O, including protection, copy-on-write and lifetime implications.
- **Cross-process rings and exec survival:** the current restrictions stay until a design preserves credentials, descriptor ownership, Capsicum rights and teardown safety.

Zero-copy `SPLICE` stays unplanned; see [`TODO.MD`](TODO.MD).

## Upstream references

- [Linux io_uring UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/io_uring.h)
- [liburing registered waits](https://github.com/axboe/liburing/blob/master/man/io_uring_register_wait_reg.3)
- [Linux zero-copy receive design](https://docs.kernel.org/networking/iou-zcrx.html)

These links move over time. Qualification work has to pin exact versions or commits.
