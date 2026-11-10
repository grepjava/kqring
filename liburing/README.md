# liburing for kqring

This directory builds [liburing](https://github.com/axboe/liburing) 2.15, the standard userspace io_uring library, against `kqring.ko`. Programs written for liburing compile unchanged and run on FreeBSD through kqring's system calls.

The port is a small overlay, not a fork:

- [`Makefile`](Makefile) fetches the pinned liburing release, checks its SHA-256 and applies the overlay. It needs no GNU make or `configure`.
- [`freebsd/freebsd.diff`](freebsd/freebsd.diff) is a 35-line patch to liburing's `src/syscall.h`, `queue.c` and `register.c`.
- [`freebsd/syscall.h`](freebsd/syscall.h) routes liburing's three system calls to `kqring_setup()`, `kqring_enter()` and `kqring_register()`.
- [`freebsd/include/`](freebsd/include/) holds a few small Linux headers (`linux/types.h`, `linux/time_types.h`, …), `liburing/compat.h`, and `config-host.h`, which stands in for liburing's `configure` output.
- [`freebsd/include/test-compat.h`](freebsd/include/test-compat.h) and [`freebsd/test-include/`](freebsd/test-include/) go into liburing's test programs only, never the library. They supply the Linux headers and behaviour the tests depend on.
- [`freebsd/tests.diff`](freebsd/tests.diff) changes a few lines in some tests (see [Adapted tests](#adapted-tests)).

## Building

`kqring.ko` must be loaded to run anything, but not to build.

```sh
cd liburing
make            # work/out/liburing.a and the examples
make tests      # also liburing's own test programs
make check      # run them (summary in work/out/check.log, output in work/out/logs/)
make clean
```

Link your own programs with `-I work/liburing-liburing-2.15/src/include -I freebsd/include -include freebsd/include/config-host.h` and `work/out/liburing.a`.

As with the raw interface, flag values passed through SQEs (`O_*`, `MSG_*`, `SOCK_*`, …) and errors in `cqe->res` are FreeBSD's. See [Differences from Linux](../README.md#differences-from-linux) in the main README.

## Test results

liburing's test suite, run on FreeBSD 15.1 with `kqring.ko` from this tree (November 2026, with the test directory on ZFS and `RLIMIT_MEMLOCK` raised):

| | Tests |
|---|---|
| In liburing 2.15 | 258 |
| Built | 245 |
| Passed | **218** |
| Skipped | 20 (the test found a feature missing and said so) |
| Failed | 7, explained below |
| Not built | 9 not portable, 2 BPF, 2 AddressSanitizer only, explained below |

`fixed-buf-merge` and `read-write` register more buffer memory than FreeBSD's default `RLIMIT_MEMLOCK` (64 KiB; Linux uses 8 MiB), so they need it raised: `ulimit -l 65536` as root, or `memorylocked` in `login.conf(5)`.

These results aren't a compatibility percentage. Some of the tests are adapted, as listed below, and a test stops at its first failure, so the subtests after it haven't run. That applies to `pipe-reuse` and `splice` in particular, which stop early. Without procfs, `fdinfo-sqpoll` passes without reading what it tests: its reader thread can't open `/proc/self/fdinfo`, and the test only creates and tears down `SQPOLL` rings while it is killed. With `SIGINT` ignored, as in a backgrounded job, it hangs.

### Failures that are not kqring's

| Tests | Why they fail |
|---|---|
| `self` | Reads `/proc/self`, and the test host has no procfs mounted. |
| `pipe-reuse`, `splice` | FreeBSD writes large blocks to a pipe directly from the writer's pages, and such a `write(2)` blocks until a reader has taken all of it, which these tests don't expect. |
| `recv-bundle-short-ooo` | Writes 1 MiB to a TCP socket before reading any of it. FreeBSD takes about 48 KiB on an unread loopback socket, so the write blocks. |
| `vec-regbuf` | Expects a 49,185-byte zero-copy send on a new TCP socket to complete in full. A new FreeBSD TCP socket's send buffer takes 48,972 bytes, and a plain non-blocking `send(2)` stops there too. |
| `iopoll-sync` | Compares a socket option with 1. FreeBSD's `getsockopt(2)` returns the option's bit. |

### Differences in kqring

| Tests | Why they fail |
|---|---|
| `io_uring_register` | Registers a file-backed buffer and expects a refusal. Linux refuses because it pins the pages for as long as the buffer is registered. kqring goes through the mapping at each I/O, so it has no such problem, and accepts the buffer. |

### Skipped tests

The 20 skips are tests that check for a feature and report its absence:

- **Not implemented in kqring** (see [`COMPATIBILITY.MD`](../COMPATIBILITY.MD)):
  - `IOPOLL` on an `O_DIRECT` file in the test's directory, which completes with `-EOPNOTSUPP` (`500f9fbadef8`).
  - `IORING_REGISTER_QUERY` (`ring-query`, and `zcrx`, which also needs zero-copy receive).
  - BPF filters on a ring (`cbpf_filter`) and task restrictions (`task-restrict`), refused with `EOPNOTSUPP`.
- **`epwait`** needs `IORING_OP_EPOLL_WAIT`, and FreeBSD has no epoll.
- **Linux-only facilities:** fault injection (`fc2a85cb02ef`), fsnotify (`fsnotify`), ublk (`uring_cmd_ublk`), the kernel's io_uring mock files (`mock_file`), requested huge pages (`fixed-hugepage`), and `/proc` (`wq-aff`, `io-wq-unused-exit`, `resize-mmap-fail`). `iowait` runs its waits with and without `IORING_ENTER_NO_IOWAIT`, then skips where it would read the time spent waiting for I/O from `/proc/stat`: FreeBSD doesn't count that time apart.
- **`fallocate`** skips when the test directory is on ZFS, which doesn't support preallocation: the `FALLOCATE` completes with `EOPNOTSUPP`, which the test takes as unsupported.
- **`napi-test`** needs root and a script that sets up Linux network namespaces.
- **`ring-leak`** skips because a ring descriptor can't be sent with `SCM_RIGHTS`, as on Linux 6.7 and later. FreeBSD refuses with `EOPNOTSUPP` where Linux uses `EINVAL`, and `test-compat.h` maps one to the other for the tests.
- **`read-mshot-stdin`** only runs when given an argument; it skips in an unattended run, as on Linux.
- **`socket-io-cmd`** skips its last part without root, which needs a raw socket. The rest passes first, and the whole test passes as root.

### Tests that aren't built

| Tests | Why |
|---|---|
| `pollfree`, `sigfd-deadlock` | They test `signalfd(2)`, which FreeBSD doesn't have. |
| `cmd-discard` | Linux's block-device commands through `URING_CMD`, which kqring doesn't support. |
| `io_uring_passthrough`, `sqe-mixed-uring_cmd` | NVMe passthrough through `URING_CMD`, which kqring doesn't support. |
| `timestamp`, `timestamp-bug` | Linux's `SO_TIMESTAMPING` and socket error queue. |
| `timens-abs-timer` | Linux time namespaces (`CLONE_NEWTIME`). |
| `conn-unreach` | Linux's `TCP_SYNCNT` and `TCP_USER_TIMEOUT`, which FreeBSD doesn't have. |
| `bpf_*` (2) | They need a BPF toolchain and libbpf, as in liburing's own build. |
| `xfail_*` (2) | liburing only builds these with AddressSanitizer. Without it, they wait forever by design. |

### Adapted tests

Everything the tests need beyond liburing's own sources is in the overlay, so what differs from upstream can be read there. [`test-include/`](freebsd/test-include/) and `test-compat.h` supply Linux headers and calls: an epoll over kqueue, `prctl(PR_SET_PDEATHSIG)` and `prctl(PR_SET_NO_NEW_PRIVS)` through `procctl(2)`, futex calls through `_umtx_op(2)`, and `EBADFD` and `EBADR` as the errors kqring returns for them.

The tests that are changed, or that test something else, are:

| Tests | Change | Why |
|---|---|---|
| `statx` | Compares with `fstatat(2)`, through `test-compat.h` | FreeBSD has no `statx(2)`. Without a reference, the test would pass without comparing anything. |
| `regbuf-merge` | Linux's system call numbers go to kqring, through `test-compat.h` | Otherwise it makes whichever FreeBSD system calls have those numbers, and never reaches kqring. |
| `socket-getsetsock-cmd` | Uses `TCP_KEEPIDLE` for `TCP_USER_TIMEOUT`, `SO_TYPE` for `SO_PEERNAME`, and compares `SO_REUSEPORT` as true or false | FreeBSD has neither option, and `getsockopt(2)` returns the option's bit rather than 1. |
| `socket-io-cmd` | Waits 300 ms after sending | FreeBSD counts data as sent until a delayed ACK comes back, so it shows on both ends at first. |
| `futex` | A wake doesn't change the word, and a cancel expects 0 | See [Differences from Linux](../README.md#differences-from-linux). |
| `waitid` | Expects `-ETIME` from a link timeout and 0 from a cancel | Linux's 1 for both is what cancelling a `WAITID` returns inside the kernel, not a documented result. |
| `io-cancel`, `timeout` | Leave out three subtests in which a forked child uses its parent's ring | A ring runs one process's requests at a time ([docs/cross-process-rings.md](../docs/cross-process-rings.md)). In `test_cancel_req_across_fork` the child submits while the parent's read is in flight, and gets `EEXIST`. In `test_cancel_inflight_exit` the child exits with requests in flight, which kqring drops without CQEs, where Linux cancels them with `-ECANCELED` and lets a standalone `TIMEOUT` fire. In `test_timeout_link_cancel` the child `exec`s, which closes a ring with requests in flight. |
| `poll-v-poll`, `epwait` | Take `EOPNOTSUPP` as epoll not being supported, as they do `EINVAL` | FreeBSD has no epoll. `poll-v-poll` polls a kqueue in place of an epoll descriptor. |
| `cbpf_filter`, `task-restrict` | Take `EOPNOTSUPP` as the feature not being supported, as they do `EINVAL` | kqring refuses these registrations with `EOPNOTSUPP`, where a Linux kernel without them gives `EINVAL`. |
| `reg-wait` | Gives a signal mask the size of FreeBSD's `sigset_t` (`_NSIG / 8`, 16 bytes) where it expects `EFAULT` from a bad pointer | Linux's is 8 bytes. With 8, kqring fails the size check first, with `EINVAL`. |
| `iowait` | Skips where it would read `/proc/stat`, after its waits | FreeBSD has no `/proc/stat`, and doesn't count time waiting for I/O apart. Without the change it reads 0 and fails. |
| `sendzc-bug` | Doesn't set `SO_ZEROCOPY` where the platform has no such option, as liburing's own helpers already don't | io_uring's zero-copy sends don't need it, and FreeBSD has no such option. |
| `socket`, `send_recv`, `send_recvmsg`, `recv-msgall-stream` | A semaphore replaces a mutex used as a signal ([`mutex-signal.h`](freebsd/test-include/mutex-signal.h)) | The tests unlock a mutex from a thread that doesn't own it, which glibc allows and FreeBSD's libthr refuses. |
| `232c93d07b74`, `accept-test` | Socket paths in `/tmp` | FreeBSD has no abstract AF_UNIX addresses. |
| `across-fork` | `mmap(2)` with a descriptor of -1 | FreeBSD refuses descriptor 0 for an anonymous mapping. |
| `recv-multishot` | An `int` for a one-byte `bool` passed to `setsockopt(2)` | FreeBSD refuses the `bool` with `EINVAL`. |
| `truncate` | `mkstemp(3)` for `mkostemp(3)` | The test passes flags that FreeBSD's `mkostemp(3)` refuses. |

To run a single test: `cd work/out/test && ./<name>.t` (exit 0 = pass, 77 = skip).
