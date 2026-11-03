# Changelog

## Unreleased

### Performance

- **Fewer wake-ups when waiting for several completions** on `DEFER_TASKRUN` rings. An enter with `min_complete` = N is now woken once N plain `READ`/`WRITE` completions from workers or disk drivers are queued, rather than for each one, as io_uring's lazy task-work wake does. Linked, drained, multishot and readiness requests still wake it at once. On an md(4) disk with 32 in flight and 16 waited for, context switches drop by 75–81% and IOPS rise 6–9%; see [BENCHMARK.MD](BENCHMARK.MD#storage-workloads-queue-depth-aio4-and-pread2-threads).
- **The `SQPOLL` thread polls while active**, for the whole `sq_thread_idle`, as io_uring's does, instead of sleeping in 50 µs naps that cost two wake-ups per request. On an md(4) disk, one 4 KiB random read at a time: 200,000 per second when the program polls the CQ (18% more than without `SQPOLL`, p50 4.2 µs), up from 107,000.
- **Poll wakeups from other processes wake a waiting enter directly.** A `write(2)` to a pipe polled with `POLL_ADD` used to reach the ring through its poller thread; now, when an enter is waiting, the knote wakes it and it collects the event itself. A cross-process poll round trip drops from 8.1 to 6.4 µs (`kevent(2)`: 5.7 µs). With no enter waiting, the poller still collects it.
- `kqring_diskbench -n N` waits for N completions per enter, and `-C` (with `-P`) polls the CQ instead of entering to wait.

## 0.2.0 — 2026-11-03

Still an **experimental preview**, for test systems only: the same caveats as 0.1.0 apply. This release fixes a deadlock and a blocking receive that 0.1.0 users can hit, and makes queued file and socket I/O faster. The ABI is unchanged; rebuild the module and reload it.

### Fixed

- **A process stopped while closing a ring could never stop.** Closing a ring under `SIGSTOP`, `truss(1)` or a debugger deadlocked the process. Ring teardown now finishes on a `kqteardown` kernel thread when a stop interrupts it, and ring kernel threads report a pending stop when they exit.
- **`RECV` and `RECVMSG` with `MSG_WAITALL` could block the submitting thread** once some data had been copied; kqring now never passes `MSG_WAITALL` to `soreceive()` and waits for the rest itself.
- **A batch of file reads that go to workers ran one at a time.** The submitting thread waited for each worker read before submitting the next, so `O_DIRECT` reads of a ZFS file got no more from 64 in flight than from one. Reads now run on several workers at once (41–53% more IOPS at 4–64 in flight), and only one idle worker spins for the next request. Writes keep their order, as io-wq's per-inode hashing does.
- A `READ` or `WRITE` on a bad descriptor now fails at submission, as on Linux, rather than on a worker after later requests.
- `READV` run inline accepts more than 16 iovecs, and `IORING_REGISTER_CLONE_BUFFERS` no longer trips a lock-order warning on kernels with `WITNESS`.
- Fifteen smaller fixes from a code review, in socket upcalls, buffer-ring pinning, taskwork and the worker pools.

### Performance

On the same Intel Core i5-8250U as 0.1.0 (details in [BENCHMARK.MD](BENCHMARK.MD)):

- A request through a worker thread round-trips in about 1.0 µs, down from 4.4 µs: the issuer and the worker each spin briefly before sleeping.
- An armed one-shot `RECV` costs 23% more than kqueue at 64 B, down from 37%: one-shot waiters are detached inside the socket upcall.
- User-memory buffer rings are mapped into the kernel at registration, so selecting a provided buffer is a memory load rather than a `copyin(9)`.
- 16 KiB AF_UNIX sends from registered buffers are 4% faster than `write(2)`.
- Storage: at 32 in flight on an md(4) disk, kqring matches `aio(4)` on 4 KiB random reads and is 14% ahead on writes; `O_DIRECT` reads of a ZFS file reach ZFS's ceiling along with `aio(4)` and `pread(2)` threads.

### Testing

- kqring's own suite has 88 tests, all passing in 10 consecutive passes with the module reloaded before each, a scratch disk and `RLIMIT_NOFILE` of 440,000, with no kqring memory, wired pages or open files left behind.
- New: [`tests/kqring_stress.c`](tests/kqring_stress.c), a randomized multi-threaded stress test, which passes 10-minute runs with no leaks; [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c), storage workloads through kqring, `aio(4)` or `pread(2)` threads with IOPS, latency percentiles and read verification; regression tests for stopping a process during ring close and for batches of `O_DIRECT` reads.
- liburing 2.9: of 168 tests that build, 142 pass, 7 skip and 19 fail (144 pass with `RLIMIT_MEMLOCK` raised). The one failure beyond 0.1.0's is `fallocate`, because ZFS doesn't support preallocation; see [`liburing/README.md`](liburing/README.md).
- Unloading is refused (`EBUSY`) while any ring is open, and succeeds as soon as the last one closes: 20 cycles of repeated unload attempts against 8 stress-test threads, each followed by a reload, all clean.

## 0.1.0 — 2026-10-25

First release: an **experimental preview**. kqring is an out-of-tree kernel module that hasn't had an independent security audit, and a bug in it can crash the machine. Try it on test systems, not in production.

### Supported system

FreeBSD 15.1-RELEASE, amd64, `GENERIC` kernel, built against the matching kernel sources in `/usr/src`. Other versions and architectures haven't been tested.

### What works

- The io_uring ABI as-is: shared SQ and CQ rings, `struct io_uring_sqe`, `struct io_uring_cqe` and `struct io_uring_params`, the `IORING_*` constants, entered through kqring's own `kqring_setup()`, `kqring_enter()` and `kqring_register()` system calls (or ioctls on `/dev/kqring`).
- About 60 opcodes: socket, file, pipe and disk I/O; multishot `RECV`, `ACCEPT` and `POLL_ADD`; linked requests, timeouts and cancellation; zero-copy send.
- Fixed files, registered buffers, provided buffers and buffer rings, resource tags, personalities and restrictions.
- `SINGLE_ISSUER`, `DEFER_TASKRUN`, `SQPOLL`, `NO_MMAP`, `NO_SQARRAY`, `REGISTERED_FD_ONLY`, `RESIZE_RINGS`, `min_wait_usec` and `IORING_FEAT_NODROP`.
- Disk devices go straight to the driver as bios. Cached file reads, and pipes that can proceed, run inline. Everything else goes to in-process worker threads.

See the [README](README.md) for the full list, and its *Differences from Linux* section for where behaviour differs.

### Testing

- kqring's own suite has 86 tests. Two need extra setup: a scratch disk (`KQRING_TEST_DISK`) and `RLIMIT_NOFILE` of at least 440,000. All pass, in 10 consecutive passes with the module reloaded before each, with no kqring memory, wired pages or open files left behind.
- liburing 2.9 builds against kqring. Of its 168 tests that build, 143 pass, 7 skip and 18 fail; two more pass with `RLIMIT_MEMLOCK` raised. See [`liburing/README.md`](liburing/README.md) for each failure's cause.
- 20 unload and reload cycles with 8 threads in `kqring_enter(2)` throughout all succeed cleanly.

### Performance

On an Intel Core i5-8250U, against `kqueue(2)` and plain system calls (details in [BENCHMARK.MD](BENCHMARK.MD)):

- 14–17% faster when many sockets are ready at once;
- 32 `NOP`s per enter at 64 ns each, against 289 ns for an empty `kevent(2)`;
- zero-copy send 27–38% faster than `write(2)` at 256 KiB;
- 12.5 M single-`NOP` enters per second on 8 threads, one ring per thread.

### Known limits

- **Poll wakeups from other sources come a moment later.** A `POLL_ADD` on a non-socket file woken by anything other than another ring of the same process (a `write(2)` to a polled pipe, for instance) is reported by a kernel thread, not before the waker's system call returns as on Linux.
- **Cancellation order.** An `ASYNC_CANCEL` CQE can arrive before the target's `-ECANCELED` CQE. Wait for the target's own CQE before freeing its buffers.
- **No signal masks** in `kqring_enter()`, and `IORING_REGISTER_CLOCK` has no effect.
- **`execve(2)` tears down the process's rings**, where Linux keeps them usable.
- **Unload** fails with `EBUSY` while any ring is open, or if a thread that called into kqring doesn't return to user space within 10 s (a stopped process).
- **Per-request kernel memory** isn't charged to the per-user ring memory budget (`kern.kqring.user_ring_mem`); ring memory is.
- **`RLIMIT_MEMLOCK` defaults to 64 KiB** on FreeBSD, which is too small to register most buffers. Raise it with `ulimit -l` or `login.conf(5)`.
