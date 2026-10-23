# `kqring` — CVE-Free `io_uring` (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING`) for FreeBSD

`kqring.ko` is a loadable FreeBSD kernel module implementing a zero-CVE-surface subset of the Linux `io_uring` ABI, engineered specifically for modern per-worker thread-per-core runtimes like **Vega** (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING` + `RECV_MULTISHOT`).

## Layout

- [`include/sys/kqring.h`](include/sys/kqring.h) — Dual kernel/userspace header (`io_uring_sqe`, `io_uring_cqe`, `io_uring_buf_ring`, `/dev/kqring` ioctls, and inline `kqring_setup` / `kqring_enter` / `kqring_register` wrappers).
- [`kmod/kqring.c`](kmod/kqring.c) — FreeBSD 14/15 loadable kernel module (`style(9)` C).
- [`kmod/Makefile`](kmod/Makefile) — `<bsd.kmod.mk>` build definition for `kqring.ko`.
- [`tests/kqring_test.c`](tests/kqring_test.c) — Functional verification suite (`IORING_REGISTER_PROBE`, `PBUF_RING` + `IORING_RECV_MULTISHOT` + zero-leak EOF) and 50,000-round ping-pong latency benchmark vs. `kqueue`.

---

## Security Architecture (Why Zero `io_uring` CVEs Apply)

| Linux `io_uring` CVE Class | `kqring.ko` Guarantee |
|---|---|
| **`io-wq` async worker UAFs** | No kernel worker threads exist. All SQE submissions and deferred socket re-arms run strictly in the owning thread's `sys_ioctl(KQRING_IOC_ENTER)` syscall context (`curthread->td_tid == ring->owner_tid`). |
| **`IOSQE_FIXED_FILE` reference cycles** | No fixed-file table or `unix_gc` bypass. Every SQE resolves standard file descriptors with Capsicum rights checks (`fget_read` / `fget_write`) and holds explicit `soref()` / `sorele()` references on armed sockets. |
| **Pinned user-buffer page lifetime bugs** | `PBUF_RING` descriptor pages (`struct io_uring_buf`) are wired kernel/user shared memory, while data buffers remain ordinary userspace virtual addresses copied via standard `uiomove()` inside `soreceive()` in `curthread` context. |
| **EOF buffer-ID leak on `IOSQE_BUFFER_SELECT`** | Unlike Linux `io_uring` (which consumes a `PBUF_RING` buffer ID even when `recv` returns `0` at EOF), `kqring_pbuf_commit()` only advances the kernel ring head when `transferred > 0`. |
| **`MSG_RING` cross-ring CQ races** | A ring's CQ is only ever written by its owner thread. `MSG_RING` queues the CQE on the target ring under its lock and wakes the owner, which posts it from its own taskwork. |
| **SQE argument TOCTOU / stale user pointers** | Everything an SQE points to (paths, iovecs, `msghdr`s, socket addresses, `open_how`, xattr names and values, futex vectors, timespecs) is copied in at submission (`IORING_FEAT_SUBMIT_STABLE`), charged to a per-ring budget. Only output buffers are touched later. |

---

## Supported Operations

All I/O runs in the ring owner's thread inside `KQRING_IOC_ENTER`. Socket operations try the transfer inline first, and then wait on a socket upcall. Futexes, `WAITID` and timers are re-checked from the same thread.

- **I/O:** `READ`, `WRITE`, `READV`, `WRITEV`, `FSYNC`, `SYNC_FILE_RANGE`, `FALLOCATE`, `FTRUNCATE`, `FADVISE`, `MADVISE`.
  - `READ` takes a provided buffer.
  - On a socket, the read and write operations wait like `RECV` and `SEND` instead of blocking.
- **Sockets:** `RECV`, `SEND`, `RECVMSG`, `SENDMSG`, `SEND_ZC`, `SENDMSG_ZC`, `ACCEPT`, `CONNECT`, `SOCKET`, `BIND`, `LISTEN`, `SHUTDOWN`, `READ_MULTISHOT`, `URING_CMD`.
  - Multishot is supported for `RECV`, `RECVMSG`, `ACCEPT` and `READ_MULTISHOT`.
  - `MSG_WAITALL` works on stream sockets, and `SEND` can take a destination address.
  - `URING_CMD` supports `SIOCINQ`, `SIOCOUTQ`, `GETSOCKOPT` and `SETSOCKOPT`.
- **Files and paths:** `OPENAT`, `OPENAT2`, `CLOSE`, `STATX`, `RENAMEAT`, `UNLINKAT`, `MKDIRAT`, `SYMLINKAT`, `LINKAT`, `GETXATTR`, `SETXATTR`, `FGETXATTR`, `FSETXATTR`.
- **Synchronisation:** `FUTEX_WAIT`, `FUTEX_WAITV`, `FUTEX_WAKE`, `WAITID`, `MSG_RING` (`IORING_MSG_DATA`).
- **Control:** `NOP`, `POLL_ADD` (including multishot), `POLL_REMOVE`, `TIMEOUT`, `TIMEOUT_REMOVE`, `LINK_TIMEOUT`, `ASYNC_CANCEL`.
- **SQE flags:** `IOSQE_IO_LINK`, `IOSQE_IO_HARDLINK`, `IOSQE_IO_DRAIN`, `IOSQE_CQE_SKIP_SUCCESS` and `IOSQE_BUFFER_SELECT` (buffer group 0).

These operations and flags are rejected for now, with `-EOPNOTSUPP` or `-EINVAL`:

- **Fixed files and registered buffers:** `READ_FIXED`, `WRITE_FIXED`, `FILES_UPDATE`, `FIXED_FD_INSTALL`, `PROVIDE_BUFFERS`, `REMOVE_BUFFERS`, `IORING_MSG_SEND_FD`, `IOSQE_FIXED_FILE`, and `file_index` (direct descriptors).
- **Pipe and other non-socket readiness:** `SPLICE`, `TEE`, and `READ_MULTISHOT` on anything other than a socket.
- **No FreeBSD equivalent:** `EPOLL_CTL`.

**Differences from Linux:**

- **Native values.** Flags and constants use their FreeBSD values: `O_*`, `AT_*`, `MSG_*`, `SOCK_*`, `POSIX_FADV_*`, `MADV_*`, `idtype_t` and `W*`. CQE errors are negated FreeBSD errnos.
- **Timeouts.** These complete with `-ETIME`, which the header defines as `ETIMEDOUT`.
- **Zero-copy send.** `SEND_ZC` copies the data, so its notification CQE follows at once. With `IORING_SEND_ZC_REPORT_USAGE`, the notification carries `IORING_NOTIF_USAGE_ZC_COPIED`.
- **`STATX`.** It fills Linux's `struct statx` from `stat(2)`.
- **Xattrs.** Names are mapped to `extattr` namespaces: `user.` to user, and `system.` and `trusted.` to system.
- **`SYNC_FILE_RANGE`.** It syncs the whole file's data.
- **`SIGPIPE`.** It is never raised.

---

## Building & Running on FreeBSD 14 / 15

Ensure kernel sources are installed in `/usr/src` (`freebsd-version -ku`), then run:

```sh
# Build both kqring.ko and tests/kqring_test
make

# Load the kernel module (creates /dev/kqring)
sudo kldload ./kmod/kqring.ko

# Run functional tests + kqring vs kqueue ping-pong benchmark
./tests/kqring_test

# Unload when finished
sudo kldunload kqring
```

---

## Wiring Vega (`Sources/CVega/cvega.c`) to `kqring`

Because [`include/sys/kqring.h`](include/sys/kqring.h) exposes the exact `struct io_uring_sqe`, `struct io_uring_cqe`, `struct io_uring_params`, `struct io_uring_buf_ring`, and `IORING_*` constants used by `cvega.c`, porting Vega's `io_uring` backend to FreeBSD requires only swapping the three syscall wrappers (`uring_setup`, `uring_enter`, `uring_register`):

```c
#if defined(__linux__)
#include <linux/io_uring.h>
/* existing syscall(__NR_io_uring_*) wrappers */
#elif defined(__FreeBSD__)
#include <sys/kqring.h>
#define uring_setup(entries, params)       kqring_setup((entries), (params))
#define uring_enter(fd, to_sub, min_c, fl) kqring_enter((fd), (to_sub), (min_c), (fl))
#define uring_register(fd, op, arg, nr)    kqring_register((fd), (op), (arg), (nr))
#endif
```
