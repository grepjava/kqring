# Changelog

## Unreleased

### Added

- **fio's `io_uring` engine runs on kqring:** [`fio/`](fio/README.md) builds fio 3.43 with a 3-line patch and a small overlay, and `fio/bench.sh` and `fio/scale.sh` run the storage and scaling benchmarks in `BENCHMARK.MD`.
- **kqring extensions**, opted into per ring with the new `KQRING_IOC_EXT_SETUP` ioctl, through one escape opcode (default `0xFF`) that never collides with an io_uring opcode. Rings that don't opt in are unchanged. See the README's *kqring extensions*.
- **Event-driven chains** (`KQRING_EXT_TRIGGER`): a trigger watches a descriptor and runs the requests linked after it each time the descriptor is ready, with no system call per event, until it's cancelled or the descriptor hangs up. Serial and concurrent modes, a CQE per firing if asked, and an end on the first failed run if asked. [docs/event-chains-spec.md](docs/event-chains-spec.md) has the interface and the next phases.
- `POLL_ADD` on a socket reports `POLLRDHUP` when the peer has shut down writing, as `poll(2)` and Linux do.
- **Kevent requests** (`KQRING_EXT_KEVENT`), the second kqring extension: a request waits for an `EVFILT_VNODE` (file and directory changes), `EVFILT_SIGNAL` or `EVFILT_PROC` (any process's exit, fork or exec) event, one-shot or multishot, through the ring's own kqueue. With `CQE32` the CQE carries the kevent's data, such as an exit status. io_uring has no request for any of these. `KQRING_EXT_F_KEVENT` in the opt-in's features says it's there.
- **The kqueue bridge** (`KQRING_EXT_KQUEUE`), the third kqring extension: attach an existing kqueue to a ring, and its events arrive as CQEs with their `udata`, whatever code registered them, so a program using libevent or libuv can move to kqring one part at a time. `KQRING_EXT_F_KQUEUE` in the opt-in's features says it's there.
- **Kevent triggers** (`KQRING_TRIG_SRC_KEVENT`): an event-driven chain can run on a kqueue event rather than readiness, such as a file changing, a signal or a process's exit, and a process's exit is its last event. Tested by "when this file is written, read it", a child's exit, and a signal-driven `CONCURRENT` trigger; the fuzzer makes a quarter of its triggers kevent triggers.
- **Kevent byte counts:** an `EVFILT_READ` or `EVFILT_WRITE` kevent request completes when its descriptor is readable or writable, with `res` the bytes waiting (connections waiting, on a listening socket) or the room to write.
- **`KQRING_TRIG_CANCEL_RUNNING`:** a trigger that ends with an error (a cancel, `STOP_ON_ERROR`, the CQ full) also cancels its runs still in flight, rather than let them finish.
- **`KQRING_STEP_BUF_FROM_PREV`:** a `SEND` or `WRITE` step uses the provided buffer the step before it selected and filled, and gives it back to its group when done, so connections share a pool of provided buffers and user space never recycles one. Works with buffer rings and `PROVIDE_BUFFERS` groups.
- **Step flags for event-driven chains:** `KQRING_STEP_LEN_FROM_PREV` gives a step the byte count of the step before it, and `KQRING_STEP_STOP_ON_ZERO` ends the trigger at end of file. A `RECV` → `SEND` echo server then runs entirely in the kernel, with no CQE or system call per message. In `kqring_bench echo` it uses 8–15% less server CPU per message than a kqueue server, and has the shortest round trip.

### Fixed

- **Several rings on a machine with no CPUs to spare ran far slower than one.** Workers and issuers spinning for each other took the CPU from the threads with work to do: in fio, 8 threads reading a ZFS file from the ARC ran at 108,000 IOPS where 4 ran at 471,000. They now spin only with a CPU to spare, and an issuer only for a program that has taken every CQE. The same run now gives 532,000, and 1 thread 424,000 rather than 243,000.
- **Reads that had to wait for a disk ran one at a time if submitted one per enter.** A read that found nothing else in flight made the enter wait for it, so the next read found the same, however many the program allowed in flight. In fio on ZFS, 32 in flight went from 7,753 to 63,898 IOPS. Programs that submit in batches weren't affected.
- **`O_DIRECT` writes to a file ran one at a time.** Every write made the enter wait for its worker, to keep writes in order. One at an explicit offset needs no order, as on Linux, and now runs in parallel: in fio on ZFS, 32 in flight went from 56% CPU to 14% at 9,400 IOPS or more. Buffered writes, and those that append or use the file position, stay in order.
- **Closing a ring could take up to 50 ms** when its poller thread was running, which any ring waiting on an AF_UNIX socket now has. Closing now wakes the poller at once.
- **An armed `RECV`, `POLL_ADD` or trigger on an AF_UNIX stream socket never completed when the peer called `shutdown(SHUT_WR)`.** FreeBSD 15 marks the input ended and wakes `poll(2)` and kqueue, but runs no socket upcall, which kqring waits on ([FreeBSD PR 300494](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300494)). kqring now also watches such sockets with a knote that reports only end of input. Requests on fixed files only are still affected. [BSDBUG.MD](BSDBUG.MD) tracks this and other FreeBSD bugs kqring has found.
- `IORING_UNREGISTER_RING_FDS` stopped with `EBADF` at the first empty slot, so unregistering a range left the slots after it registered (and their rings alive until exit). An empty slot now counts as done, as in io_uring. Found by the new fuzzer.
- **A `READ` of an `O_NONBLOCK` pipe could hang its process for good.** If another reader drained the pipe after kqring saw it ready, the submitting thread spun in `kqring_enter()` until more data came, and the process couldn't exit until then. It now goes to a worker. A worker that lost the data the same way failed the read with `EAGAIN`; it now waits again. Found by the fuzzer.
- **A `READ_MULTISHOT` of a pipe could stall its ring.** If another reader drained the pipe between kqring seeing data and reading it, the read blocked in whatever ran it: an enter, or the `SQPOLL` thread, which then served no other request, and a process sharing the ring couldn't exit. It now re-arms instead. Found by the fuzzer.
- **Kernels with `INVARIANTS` panicked** when a ring was set up (`vm_page_valid()` on a page that wasn't busied) and on `FALLOCATE` (`RLIMIT_FSIZE` read without the process lock, which could also see a worker's stale limit), and under `SQPOLL` or `MSG_RING` load (an unlocked emptiness check of the `MSG_RING` queue, which `INVARIANTS` asserts on). The module also builds against such kernels now. kqring's suite, stress test and fuzzer pass on a `GENERIC-DEBUG` kernel (`INVARIANTS`, `WITNESS`) with no lock-order warnings.

### Testing

- [`tests/kqring_fuzz.c`](tests/kqring_fuzz.c), a fuzzer for the whole interface: random setup parameters, SQEs, enter flags and arguments, and register calls, with descriptors, memory and rings closed under requests in flight. Most rings are also set up properly (registered files and buffers, a buffer ring, a personality) and get mostly well-formed requests and link chains, so the fuzzer gets past argument checks into the code behind them. A regression test for each bug it found is in `kqring_test`.
- Event-driven chains have their own tests (`kqring_test trig` runs only those): serial and concurrent runs, per-firing CQEs, an event never consumed, hang-up, bad templates, `STOP_ON_ERROR`, and closing the ring mid-run. The fuzzer opts half its well-formed rings in and submits triggers with random modes and templates: a 30-minute run on a `GENERIC-DEBUG` kernel submitted 119,000 triggers with no failures or lock-order warnings.
- Kevent requests have tests of their own (a file written, a signal delivered three times, a child's exit with its status, bad arguments), and the fuzzer submits them on rings that opted in: a 30-minute run on `GENERIC-DEBUG` sent 68,000 with no failures or leaks.
- Step flags have tests of their own: an echo server (300 messages and a 256 KiB stream, no CQE per message), an event-driven file copy that stops at end of file, and bad step flags. The fuzzer gives its triggers valid step flags 40% of the time; a 30-minute run on `GENERIC-DEBUG` submitted 124,000 triggers with no failures.
- `test_send_armed` no longer fails now and then on slow kernels when its `POLLIN | POLLOUT` poll rightly completes for writability alone.

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
