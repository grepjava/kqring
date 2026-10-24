# liburing for kqring

This directory builds [liburing](https://github.com/axboe/liburing) 2.9, the standard userspace io_uring library, against `kqring.ko`. Programs written for liburing compile unchanged and run on FreeBSD through kqring's system calls.

The port is a small overlay, not a fork:

- [`Makefile`](Makefile) fetches the pinned liburing release, checks its SHA-256 and applies the overlay. It needs no GNU make or `configure`.
- [`freebsd/freebsd.diff`](freebsd/freebsd.diff) is a 35-line patch to liburing's `src/syscall.h`, `queue.c` and `register.c`.
- [`freebsd/syscall.h`](freebsd/syscall.h) routes liburing's three system calls to `kqring_setup()`, `kqring_enter()` and `kqring_register()`.
- [`freebsd/include/`](freebsd/include/) holds a few small Linux headers (`linux/types.h`, `linux/time_types.h`, …), `liburing/compat.h`, and `config-host.h`, which stands in for liburing's `configure` output.
- [`freebsd/include/test-compat.h`](freebsd/include/test-compat.h) is included into liburing's test programs only, never the library. It supplies two pieces of Linux behaviour some tests depend on (see [Test results](#test-results)).

## Building

`kqring.ko` must be loaded to run anything, but not to build.

```sh
cd liburing
make            # work/out/liburing.a and the examples
make tests      # also liburing's own test programs
make check      # run them (summary in work/out/check.log, output in work/out/logs/)
make clean
```

Link your own programs with `-I work/liburing-liburing-2.9/src/include -I freebsd/include -include freebsd/include/config-host.h` and `work/out/liburing.a`.

As with the raw interface, flag values passed through SQEs (`O_*`, `MSG_*`, `SOCK_*`, …) and errors in `cqe->res` are FreeBSD's. See [Differences from Linux](../README.md#differences-from-linux) in the main README.

## Test results

liburing's test suite, run on FreeBSD 15.1 with `kqring.ko` from this tree (October 2026):

| | Tests |
|---|---|
| Built | 168 (32 more need Linux-only headers or system calls) |
| Passed | **136** (138 with `RLIMIT_MEMLOCK` raised) |
| Skipped | 12 (the test found a feature missing and said so) |
| Failed | 20, explained below |

liburing's two AddressSanitizer tests (`xfail_*`) aren't built. liburing itself only builds them with ASan, and without it they wait forever by design.

### Failures that are not kqring's

| Tests | Why they fail |
|---|---|
| `fixed-buf-merge`, `read-write` | They register more buffer memory than FreeBSD's default `RLIMIT_MEMLOCK` (64 KiB; Linux uses 8 MiB). Both pass with the limit raised: `ulimit -l 65536` as root, or `memorylocked` in `login.conf(5)`. |
| `socket`, `send_recv`, `send_recvmsg`, `recv-msgall-stream`, `across-fork` | The test unlocks a `pthread_mutex` from a thread that doesn't own it. glibc allows this; FreeBSD's libthr rejects it, and the test then crashes. |
| `232c93d07b74`, `accept-test` | Linux's abstract AF_UNIX addresses (a name starting with `\0`). |
| `recv-multishot` | Passes a one-byte `bool` to `setsockopt(2)`, which FreeBSD rejects with `EINVAL`. |
| `bind-listen` | Uses an uninitialised `struct __kernel_timespec`. |
| `self` | Reads `/proc/self`. |
| `truncate` | Passes Linux-only flags to `mkostemp(3)`. |
| `pipe-reuse`, `splice` | FreeBSD writes large blocks to a pipe directly from the writer's pages, and such a `write(2)` blocks until a reader has taken all of it, which these tests don't expect. |

### Differences in kqring

| Tests | Why they fail |
|---|---|
| `io-cancel`, `timeout` | A forked child submits to, or cancels on, its parent's ring. kqring rings belong to one process: workers resolve descriptors in the owner's table, so allowing another process could let it reach files it can't see, for example from inside a Capsicum sandbox. The child gets `EEXIST`. |
| `poll` | Its "lazy" test expects a poll on another ring's descriptor to complete inside the waker's system call. kqring's wakeup arrives through a kernel thread, a moment later. |
| `waitid` | Expects Linux's result for a `WAITID` cancelled by a link timeout, which reflects an implementation detail of Linux rather than a documented rule. |
| `resize-rings` | `IORING_REGISTER_RESIZE_RINGS` isn't implemented. |

### Skipped tests

The 12 skips are tests that check for a feature and report its absence:

- **Not implemented in kqring:**
  - `IORING_SETUP_NO_MMAP` (`buf-ring-nommap`, `no-mmap-inval`);
  - `IORING_SETUP_REGISTERED_FD_ONLY` (`reg-fd-only`);
  - `min_wait_usec` waits (`min-timeout`, `min-timeout-wait`);
  - `IOPOLL` on an `O_DIRECT` file in the test's directory, which completes with `-EOPNOTSUPP` (`500f9fbadef8`).
- **Linux-only facilities:** fault injection (`fc2a85cb02ef`), fsnotify (`fsnotify`), ublk (`uring_cmd_ublk`), and `/proc` paths (`wq-aff`).
- **`ring-leak`** skips because a ring descriptor can't be sent with `SCM_RIGHTS`, as on Linux 6.7 and later. FreeBSD refuses with `EOPNOTSUPP` where Linux uses `EINVAL`, and `test-compat.h` maps one to the other for the tests.
- **`read-mshot-stdin`** only runs when given an argument; it skips in an unattended run, as on Linux.

To run a single test: `cd work/out/test && ./<name>.t` (exit 0 = pass, 77 = skip).
