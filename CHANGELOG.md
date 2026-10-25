# Changelog

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
