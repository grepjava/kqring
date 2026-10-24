# kqring — io_uring for FreeBSD

`kqring` brings Linux's [`io_uring`](https://kernel.dk/io_uring.pdf) asynchronous I/O interface to FreeBSD, as a loadable kernel module (`kqring.ko`) for FreeBSD 14 and 15.

It uses the io_uring ABI as-is, with the same structure layouts and `IORING_*` constants:
- submission and completion queues shared with the kernel through `mmap(2)`;
- `struct io_uring_sqe`, `struct io_uring_cqe` and `struct io_uring_params`;
- provided-buffer rings.

Code written against `<linux/io_uring.h>` builds against [`<sys/kqring.h>`](include/sys/kqring.h) instead, and needs only its three syscall wrappers replaced.

**Status.** kqring implements most of io_uring:
- about 60 opcodes, multishot operations, linked requests, timeouts and cancellation;
- fixed files, registered and provided buffers;
- zero-copy send, `SQPOLL` and in-process worker threads.

It has its own functional test suite and benchmarks, and has been reviewed against the classes of bugs behind Linux io_uring's CVEs. [liburing](liburing/) 2.9 builds against it, and most of liburing's own test suite passes (see [`liburing/README.md`](liburing/README.md)). It has **not** had an independent security audit, and it is an out-of-tree kernel module: a bug in it can crash the machine.

---

## Quick start

Kernel sources must be installed in `/usr/src` and match the running kernel (`freebsd-version -ku`).

```sh
make                                   # builds kmod/kqring.ko, tests/kqring_test, tests/kqring_bench
sudo kldload ./kmod/kqring.ko          # creates /dev/kqring
./tests/kqring_test                    # functional test suite
./tests/kqring_bench                   # benchmarks (the send group needs ulimit -l 8192; see BENCHMARK.MD)
sudo kldunload kqring
```

---

## Using kqring from io_uring code

[`<sys/kqring.h>`](include/sys/kqring.h) provides the io_uring structures and constants, plus three wrappers with the same arguments as Linux's system calls:

| Linux system call | kqring wrapper |
|---|---|
| `io_uring_setup(entries, params)` | `kqring_setup(entries, params)` |
| `io_uring_enter(fd, to_submit, min_complete, flags, arg, argsz)` | `kqring_enter(fd, to_submit, min_complete, flags, arg, argsz)` |
| `io_uring_register(fd, opcode, arg, nr_args)` | `kqring_register(fd, opcode, arg, nr_args)` |

The rings are mapped exactly as on Linux: `mmap(2)` on the ring descriptor at `IORING_OFF_SQ_RING`, `IORING_OFF_SQES` and `IORING_OFF_PBUF_RING`, using the offsets returned in `struct io_uring_params`. So a program with its own syscall wrappers ports like this:

```c
#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#define uring_setup(entries, p) \
	syscall(__NR_io_uring_setup, (entries), (p))
#define uring_enter(fd, submit, wait, flags, arg, argsz) \
	syscall(__NR_io_uring_enter, (fd), (submit), (wait), (flags), (arg), (argsz))
#define uring_register(fd, op, arg, nr) \
	syscall(__NR_io_uring_register, (fd), (op), (arg), (nr))
#elif defined(__FreeBSD__)
#include <sys/kqring.h>
#define uring_setup(entries, p)                          kqring_setup((entries), (p))
#define uring_enter(fd, submit, wait, flags, arg, argsz) kqring_enter((fd), (submit), (wait), (flags), (arg), (argsz))
#define uring_register(fd, op, arg, nr)                  kqring_register((fd), (op), (arg), (nr))
#endif
```

The wrappers make native system calls, which the module registers in free system-call slots when it loads; `sysctl kern.kqring.sys_setup`, `sys_enter` and `sys_register` give their numbers. `kqring_setup()` opens `/dev/kqring` and returns the new ring's descriptor. Where no slot was free, the wrappers fall back to `ioctl(2)` on the ring descriptor, which costs about 90 ns more per call. Like the Linux system calls, they return −1 and set `errno` on failure. Completion results (`cqe->res`) are negated **FreeBSD** errno values, and flags passed through SQEs (`O_*`, `MSG_*` and so on) use their FreeBSD values; see [Differences from Linux](#differences-from-linux).

Programs that use liburing build against the port in [`liburing/`](liburing/), which fetches liburing 2.9 and builds it with kqring's system calls; see [`liburing/README.md`](liburing/README.md).

### Requirements

- **Rings belong to one process.** Without `IORING_SETUP_SINGLE_ISSUER`, any thread of the process that created the ring may submit, wait and register; with it, only the creating thread may (`EEXIST` otherwise), as on Linux. Another process, such as a forked child, cannot use the ring except to wake an `SQPOLL` thread (see [Differences from Linux](#differences-from-linux)).
- **`IORING_SETUP_DEFER_TASKRUN` is the fastest mode.** It requires `SINGLE_ISSUER`, as on Linux, and completions are posted when the owner enters the ring. Without it, a kernel thread in the process posts completions as they happen, which costs a thread switch per batch.
- **Raise `RLIMIT_MEMLOCK` to register buffers.** Registered buffers' pages are held for zero-copy send and charged to the user's locked-memory limit, which FreeBSD sets to only 64 KiB by default (Linux uses 8 MiB). Set `memorylocked` in `login.conf(5)`, or use `ulimit -l`.

---

## Performance

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

Compared with `kqueue(2)`, or the plain system call where kqueue has no role, on a 4-core i5-8250U running FreeBSD 15.1 (medians of 5 runs):

- **Many ready sockets:** 14–17% faster than kqueue, from 16 up to 1,024 connections, because one enter reaps every message.
- **One connection:** multishot `RECV` is 4–15% faster than kqueue on AF_UNIX, and receiving into one reused buffer is 19–22% faster.
- **Cached file reads:** 32 `READ`s per enter cost 30% less than `pread(2)`, served inline from the page cache. One `READ` per enter costs 14% more.
- **Zero-copy send:** `SEND_ZC` from a registered buffer is 38% faster than `write(2)` for 256 KiB messages on AF_UNIX and 27% faster on TCP loopback, and 17–19% faster at 64 KiB.
- **Batching and timers:** 32 `NOP`s per enter cost 64 ns each, and a single `NOP` enter 324 ns (an empty `kevent` costs 289 ns). `TIMEOUT` is 45% faster; multishot `ACCEPT` is on par with kqueue.
- **Disk devices:** 4 KiB random reads on an md(4) disk cost 41% less than through a worker, and 32 per enter run 4.5× faster than `pread(2)`.
- **Pipes:** a `READ` on a pipe with data runs inline, blocking or not, and is 17–19% faster than kqueue. A read that has to wait goes through a worker thread.
- **Threads:** with one ring per thread, enters scale across CPUs: 12.5 M single-`NOP` enters per second on 8 threads, against 3.1 M on one.

Except for the thread scaling, these are single-threaded loopback measurements. See [`BENCHMARK.MD`](BENCHMARK.MD) for the method, every result, and the known gaps.

---

## Supported operations

**How requests run.**
- **Sockets** are tried inline in `kqring_enter()` (unless `IOSQE_ASYNC` is set); if the socket isn't ready, the request waits on a socket upcall.
- **Also inline:** positioned file reads that the page cache satisfies, and a `READ` on a pipe that has data or a `WRITE` to one with room, blocking or not.
- **Straight to the disk driver:** positioned `READ` and `WRITE` (plain, vectored and `_FIXED`) on a disk device (`/dev/ada0`, `nda0`, `md0`, GEOM partitions) go to the driver as bios over the held user pages, as `aio(4)` does, with no worker thread and any number in flight. Lengths must be whole sectors; anything else runs on a worker. In-flight disk I/O can't be cancelled (`ASYNC_CANCEL` returns `-EALREADY`, as io_uring does for running block I/O).
- **On workers:** other file, pipe and VFS requests, `WAITID` and `IOSQE_ASYNC` requests run on per-ring kernel worker threads (`IO_WQ_BOUND` and `IO_WQ_UNBOUND`, `IORING_FEAT_NATIVE_WORKERS`). These live in the ring owner's process, sharing its address space and descriptor table, and hand completions back to the owner.
- **On the owner's path:** futexes and timers.
- **With `IORING_SETUP_SQPOLL`:** a kernel thread in the process consumes SQEs, so the owner need not enter the ring to submit.

- **I/O:** `READ`, `WRITE`, `READV`, `WRITEV`, `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED`, `WRITEV_FIXED`, `FSYNC`, `SYNC_FILE_RANGE`, `FALLOCATE`, `FTRUNCATE`, `FADVISE`, `MADVISE`, `SPLICE`, `TEE`.
  - `READ` and `READV` (with one iovec) take a provided buffer.
  - On a socket, the read and write operations wait like `RECV` and `SEND` instead of blocking.
  - `SPLICE` moves data between two descriptors where at least one is a pipe; `TEE` duplicates pipe buffer bytes to another pipe without consuming the source.
- **Sockets & Pipes:** `RECV`, `SEND`, `RECVMSG`, `SENDMSG`, `SEND_ZC`, `SENDMSG_ZC`, `ACCEPT`, `CONNECT`, `SOCKET`, `BIND`, `LISTEN`, `SHUTDOWN`, `READ_MULTISHOT`, `URING_CMD`.
  - Multishot is supported for `RECV`, `RECVMSG`, `ACCEPT` and `READ_MULTISHOT` (on both sockets and pipes).
  - `MSG_WAITALL` works on stream sockets, and `SEND` can take a destination address.
  - `SEND` can send from provided buffers, and `RECV` and `SEND` take bundles (`IORING_RECVSEND_BUNDLE`, up to 32 buffers per CQE, `IORING_FEAT_RECVSEND_BUNDLE`). As in Linux, a bundle `SEND` on a stream socket sends each batch whole and goes on with `IORING_CQE_F_MORE` until the group is empty.
  - `ACCEPT` CQEs carry `IORING_CQE_F_SOCK_NONEMPTY` when more connections are queued.
  - `SEND_ZC` can send from a registered buffer (`IORING_RECVSEND_FIXED_BUF`).
  - `URING_CMD` supports `SIOCINQ`, `SIOCOUTQ`, `GETSOCKOPT` and `SETSOCKOPT`.
- **Files and paths:** `OPENAT`, `OPENAT2`, `CLOSE`, `PIPE`, `STATX`, `RENAMEAT`, `UNLINKAT`, `MKDIRAT`, `SYMLINKAT`, `LINKAT`, `GETXATTR`, `SETXATTR`, `FGETXATTR`, `FSETXATTR`.
- **Fixed files and buffers:** `FILES_UPDATE` (including `IORING_FILE_INDEX_ALLOC`), `FIXED_FD_INSTALL`, `PROVIDE_BUFFERS`, `REMOVE_BUFFERS`.
  - `OPENAT`, `OPENAT2`, `ACCEPT` (including multishot), `SOCKET` and `PIPE` can create direct descriptors (`file_index`), and `CLOSE` can close them.
- **Synchronisation:** `FUTEX_WAIT`, `FUTEX_WAITV`, `FUTEX_WAKE`, `WAITID`, `MSG_RING` (`IORING_MSG_DATA` and `IORING_MSG_SEND_FD`, including 32-byte `big_cqe` payload transfer on `SQE128` + `CQE32` rings).
- **Control:** `NOP`, `POLL_ADD` (including multishot and `IORING_POLL_ADD_LEVEL` on sockets, pipes, eventfds, and files; on anything but a socket it is a knote on a kqueue the ring owns, served by one kernel thread per ring), `POLL_REMOVE`, `TIMEOUT`, `TIMEOUT_REMOVE`, `LINK_TIMEOUT`, `ASYNC_CANCEL` (including `IORING_ASYNC_CANCEL_FD_FIXED`).
- **Setup flags:**
  - `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_DEFER_TASKRUN`, `IORING_SETUP_CQSIZE`, `IORING_SETUP_CLAMP`, `IORING_SETUP_R_DISABLED`, `IORING_SETUP_SUBMIT_ALL`, `IORING_SETUP_COOP_TASKRUN`, `IORING_SETUP_TASKRUN_FLAG`, `IORING_SETUP_SQE128`, `IORING_SETUP_CQE32`, `IORING_SETUP_ATTACH_WQ`, `IORING_SETUP_SQPOLL`, `IORING_SETUP_SQ_AFF`, `IORING_SETUP_IOPOLL`, `IORING_SETUP_NO_MMAP` (rings in the caller's memory, which `fork(2)` then shares rather than copies), `IORING_SETUP_NO_SQARRAY` and `IORING_SETUP_REGISTERED_FD_ONLY`.
- **SQE flags:** `IOSQE_FIXED_FILE`, `IOSQE_IO_LINK`, `IOSQE_IO_HARDLINK`, `IOSQE_IO_DRAIN`, `IOSQE_ASYNC`, `IOSQE_CQE_SKIP_SUCCESS` and `IOSQE_BUFFER_SELECT` (any group), plus `sqe->personality`.
- **Registration:**
  - Files: `FILES`, `FILES2`, `FILES_UPDATE`, `FILES_UPDATE2`, `UNREGISTER_FILES` and `FILE_ALLOC_RANGE`, with sparse tables and tags.
  - Buffers: `BUFFERS`, `BUFFERS2`, `BUFFERS_UPDATE`, `UNREGISTER_BUFFERS` and `CLONE_BUFFERS` (including `IORING_REGISTER_SRC_REGISTERED`).
  - Buffer rings: `PBUF_RING` (including `IOU_PBUF_RING_MMAP` and `IOU_PBUF_RING_INC`), `UNREGISTER_PBUF_RING` and `PBUF_STATUS`.
  - Ring descriptors: `RING_FDS` and `UNREGISTER_RING_FDS` (with `IORING_ENTER_REGISTERED_RING`, `IORING_REGISTER_USE_REGISTERED_RING`, and `IORING_FEAT_REG_REG_RING`).
  - Workers: `IOWQ_AFF`, `UNREGISTER_IOWQ_AFF` and `IOWQ_MAX_WORKERS`.
  - Rings: `RESIZE_RINGS` (on `DEFER_TASKRUN` rings, as on Linux; queued SQEs and CQEs move to the new rings, which are mapped afresh).
  - Other: `EVENTFD`, `EVENTFD_ASYNC`, `UNREGISTER_EVENTFD`, `PERSONALITY`, `UNREGISTER_PERSONALITY`, `RESTRICTIONS`, `ENABLE_RINGS`, `PROBE`, `SYNC_CANCEL`, `SEND_MSG_RING` and `CLOCK`.

`IORING_REGISTER_PROBE` reports exactly which opcodes are available. These Linux operations have no FreeBSD equivalent and are rejected with `-EOPNOTSUPP` or `-EINVAL`: `EPOLL_CTL`, `EPOLL_WAIT` and `RECV_ZC`. `IORING_REGISTER_NAPI` is accepted, but FreeBSD has no NAPI polling for a module to call, so it sets only a busy-wait: a wait spins up to `busy_poll_to` µs for completions before it sleeps. The static NAPI id operations fail with `EOPNOTSUPP`.

---

## Differences from Linux

**Threading and signals**
- **One process per ring.** Workers look files up in the owner's descriptor table and run with its credentials, so a forked child that inherits a ring can't submit to it, wait on it or register with it (`EEXIST`); on Linux it can. It can still wake an `SQPOLL` thread with `IORING_ENTER_SQ_WAKEUP`. With `SINGLE_ISSUER`, only the owner thread may enter or register, as on Linux. Before `IORING_REGISTER_ENABLE_RINGS`, one thread at a time may register. `PROBE` and `SEND_MSG_RING` work from any thread.
- **Ring descriptors can't be passed over UNIX sockets.** `sendmsg(2)` with `SCM_RIGHTS` fails with `EOPNOTSUPP` (Linux 6.7 and later refuse with `EINVAL`), so a ring can't end up in flight on a socket held in its own fixed-file table. A child still inherits a ring across `fork(2)`.
- **No signal masks.** `kqring_enter()` rejects a signal mask, both the plain form and `struct io_uring_getevents_arg.sigmask` with `IORING_ENTER_EXT_ARG`, with `EINVAL`. Timeouts and `min_wait_usec` (`IORING_FEAT_MIN_TIMEOUT`) through `IORING_ENTER_EXT_ARG` work.
- **`SIGPIPE`** is never raised.

**Values and formats**
- **Native values.** Flags and constants use their FreeBSD values: `O_*`, `AT_*`, `MSG_*`, `SOCK_*`, `POSIX_FADV_*`, `MADV_*`, `idtype_t` and `W*`. CQE errors are negated FreeBSD errnos.
- **Timeouts** complete with `-ETIME`, which the header defines as `ETIMEDOUT`.
- **`STATX`** fills Linux's `struct statx` from `stat(2)`.
- **Xattrs.** Names are mapped to `extattr` namespaces: `user.` to user, and `system.` and `trusted.` to system.
- **`SYNC_FILE_RANGE`** syncs the whole file's data.

**Cancellation and process lifecycle**
- **Worker cancellation and CQE ordering.** Cancel (`ASYNC_CANCEL`, `POLL_REMOVE`) and `LINK_TIMEOUT` request an interrupt without waiting for running worker I/O.
  - The cancel, remove or timeout CQE can therefore arrive **before** the target's `-ECANCELED` CQE; Linux posts the target's CQE first.
  - A cancellation CQE doesn't promise the I/O had no side effects. Wait for the target request's own CQE before freeing its buffers or state.
  - Ring close detaches running workers without waiting. Final cleanup waits until the last worker returns, and module unload drains any that remain.
- **`SYNC_CANCEL` and `CLOCK`.** `SYNC_CANCEL` accepts the cancellation but doesn't wait for the target's CQE, and ignores its timeout. `CLOCK` is recorded, but timeouts are relative, so it has no effect.
- **`execve(2)` disables open rings.** Any ring owned by the calling process is torn down:
  - its worker pool and `SQPOLL` thread are stopped;
  - in-flight requests are freed without posting `-ECANCELED` CQEs;
  - its fixed tables are released.

  A ring descriptor kept open across `execve(2)` then returns `EBADF`. Linux keeps the ring usable and posts `-ECANCELED` for cancelled requests.

**Buffers and memory**
- **Provided-buffer reservations.** Blocking worker I/O reserves a group's head buffer while it waits. Meanwhile, other selections from that group fail with `-ENOBUFS`, and removing or unregistering the group fails with `EBUSY`. Other groups and registration remain usable. A read at end of file consumes no buffer.
- **Registered buffers.** `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED` and `WRITEV_FIXED` copy through the user mapping with `uiomove()`, so unmapping a buffer makes them fail with `-EFAULT`. At registration, each buffer's pages (up to 16 MiB per buffer) are held for zero-copy send. They are charged to `RLIMIT_MEMLOCK` and `vm.max_user_wired` (`-ENOMEM` when exceeded) until the slot is unregistered and every in-flight zero-copy mbuf has been freed.
- **Zero-copy send.**
  - **Eligibility:** `SEND_ZC` and `SENDMSG_ZC` go zero-copy on connected stream sockets for transfers of at least one page. On AF_UNIX, a send from an unregistered buffer must also reach `kern.kqring.zc_local_min_len` (32 KiB by default), because below that pinning the buffer costs more than copying it.
  - **Notification:** the buffer's pages are attached to the outgoing mbufs, and the `IORING_CQE_F_NOTIF` CQE (`res = 0`) waits until the network stack frees them.
  - **Fallback:** other sends copy the data and post the notification at once, with `IORING_NOTIF_USAGE_ZC_COPIED` when `IORING_SEND_ZC_REPORT_USAGE` is set. That includes sends that would exceed `RLIMIT_MEMLOCK` or `vm.max_user_wired`.
- **Limits.** `PROVIDE_BUFFERS` holds at most 2^18 buffers per ring, and mapped buffer rings at most 16 MiB per ring. Linux leaves both to memory accounting.

**Descriptors and messages**
- **Fixed files keep Capsicum rights.** A slot carries the rights its descriptor had, and `FIXED_FD_INSTALL` gives them back. A direct descriptor opened relative to a directory with limited rights gets that directory's rights, as `openat(2)` would.
- **Personalities** cannot be used to leave capability mode.
- **`IORING_MSG_SEND_FD` errors.** If the target ring can't install the file (no table, bad slot, no free slot), the error is reported in the target's CQE. Linux reports it to the sender.
- **`CLONE_BUFFERS`.** Both rings must have the same owner thread.
- **Direct `PIPE`.** The new pipe briefly has descriptors in the process before moving to its fixed slots, because FreeBSD has no way to create a pipe without them.
- **Eventfd** is signalled once per enter that posted CQEs, and when work is queued for an idle ring. A multishot `POLL_ADD` on the ring's own registered eventfd ends after its first CQE; Linux ends it only when the wakeup came from the ring's own signal.

**Polling**
- **Wakeups are seen by a kernel thread.** A `POLL_ADD` on a non-socket file is reported by the ring's poller thread, not in the waker's context, so its CQE appears a moment after the wakeup. Linux posts it before the waker's system call returns. Each wakeup still gets its CQE, even if the file is drained again before the poller looks.

**Module**
- **`kldunload` waits for open rings.** Unloading fails with `EBUSY` while any ring descriptor is open, and no ring can be opened while an unload is in progress.

---

## Resource lifetime and security

io_uring has been a frequent source of Linux kernel vulnerabilities. kqring was designed against each of those bug classes, as summarised below. This is a description of the design, not a claim of immunity: it has been reviewed internally (including the worker, `SQPOLL` and zero-copy paths), but has not had an independent security audit.

| Linux `io_uring` bug class | kqring behaviour |
|---|---|
| **`io-wq` async worker use-after-free** | Worker threads run inside the ring owner's process. They never touch the completion queue or the owner's fixed-file and registered-buffer tables. Requests hold references to their resources and credentials. Workers do the I/O and queue the result; the owner (or the `SQPOLL` thread, serialised with it) completes it. |
| **`IOSQE_FIXED_FILE` reference cycles** | A ring can never be put in a fixed-file table: not by registration, update, direct descriptors or `MSG_RING` `SEND_FD`. A ring descriptor can't be sent over a UNIX socket either (as in Linux 6.7 and later), so it can't be in flight on a socket held in its own table. A ring's fixed-file table is also released when its owning process exits or calls `execve(2)`. Each slot holds a file reference and the file's Capsicum rights, which every request on it is checked against. Armed sockets hold a socket reference. |
| **Fixed-file / registered-buffer use-after-free on update** | Slots are reference-counted. A request takes a reference when it is issued and drops it when freed, so replacing or unregistering a slot never affects a request still using it. The tag CQE is posted once the last reference is gone. Table changes are serialised on the owner's path. |
| **Pinned user-page lifetime bugs** | Ordinary request buffers and buffer rings in user memory are never pinned; they are copied with `uiomove()` and `copyin()`. Registered buffers hold their pages for zero-copy send, charged to `RLIMIT_MEMLOCK` and `vm.max_user_wired`. The charge belongs to a reference-counted page descriptor, released only when the slot and every in-flight zero-copy mbuf have let go. Copy-on-write mappings after `fork(2)` are refreshed before a zero-copy send. |
| **Mapped buffer-ring use-after-free (CVE-2024-0582)** | `IOU_PBUF_RING_MMAP` rings are VM objects, and each mapping holds its own reference. Unregistering a ring that is still mapped leaves the process with ordinary memory, never freed pages. The wired memory these rings use is capped per ring. |
| **Credentials outliving their context** | Personalities hold credential references, and so does each request using one. The issuer or worker switches credentials only for that request's work. A personality registered before `cap_enter(2)` is refused once the process is in capability mode. |
| **`eventfd` use-after-free** | The ring holds a reference to its eventfd, and signals it under the ring lock, which unregistering also takes. |
| **Buffer-ID leak at end of file** | Linux consumes a provided buffer even when a receive returns 0 at end of file. kqring advances the buffer ring only when bytes were transferred. |
| **`MSG_RING` cross-ring completion races** | Completions are only ever written by the target ring's own owner path. `MSG_RING` queues the CQE on the target under its lock and wakes the owner, which posts it. |
| **SQE argument TOCTOU / stale user pointers** | Everything an SQE points to (paths, iovecs, `msghdr`s, socket addresses, `open_how`, xattr names and values, futex vectors, timespecs) is copied in at submission (`IORING_FEAT_SUBMIT_STABLE`). That memory is charged to a per-ring budget, and in-flight requests per ring are capped. Only output buffers are accessed later. |

---

## Tunables

| Name | Default | Meaning |
|---|---|---|
| `kern.kqring.zc_local_min_len` (sysctl, loader tunable) | 32768 | Smallest AF_UNIX `SEND_ZC` / `SENDMSG_ZC` from an unregistered buffer that is sent zero-copy; smaller ones are copied. |

---

## Source layout

- [`include/sys/kqring.h`](include/sys/kqring.h) — the io_uring ABI (`io_uring_sqe`, `io_uring_cqe`, `io_uring_params`, `io_uring_buf_ring`, `IORING_*` constants), the `/dev/kqring` ioctls, and the `kqring_setup` / `kqring_enter` / `kqring_register` wrappers. Shared by the kernel and userspace.
- [`liburing/`](liburing/) — the liburing port: a Makefile that fetches liburing 2.9, a small patch and Linux header shims; see its [README](liburing/README.md).
- [`kmod/`](kmod/) — the kernel module (`style(9)` C), split by subsystem:
  - [`kqring_var.h`](kmod/kqring_var.h) — shared structures, limits and internal prototypes.
  - [`kqring_dev.c`](kmod/kqring_dev.c) — `/dev/kqring` and the ring file's operations, the system calls and ioctls, ring setup and mapping, enter, module load/unload and process exit/exec hooks.
  - [`kqring_req.c`](kmod/kqring_req.c) — ring memory, SQE preparation and argument copy-in, request lifecycle, links, drains, CQE posting and deferred work.
  - [`kqring_sock.c`](kmod/kqring_sock.c) — socket operations and upcalls, multishot, `POLL_ADD` and `URING_CMD`.
  - [`kqring_kpoll.c`](kmod/kqring_kpoll.c) — `POLL_ADD` on files other than sockets: knotes on a per-ring kqueue and its poller thread.
  - [`kqring_file.c`](kmod/kqring_file.c) — file, path, xattr, `SPLICE`/`TEE` and other VFS operations, including the inline page-cache read path.
  - [`kqring_sync.c`](kmod/kqring_sync.c) — timeouts, futexes, `WAITID`, `MSG_RING` and cancellation.
  - [`kqring_wq.c`](kmod/kqring_wq.c) — worker pools and the `SQPOLL` thread.
  - [`kqring_rsrc.c`](kmod/kqring_rsrc.c) — fixed files, registered buffers, provided buffers and buffer rings, resource tags and personalities.
  - [`kqring_register.c`](kmod/kqring_register.c) — `io_uring_register` operations.
  - [`kqring_zc.c`](kmod/kqring_zc.c) — zero-copy send: page holding, `RLIMIT_MEMLOCK` accounting and mbuf construction.
- [`tests/kqring_test.c`](tests/kqring_test.c) — functional test suite: 77 test groups covering every supported opcode, registration, setup flags and the lifetime rules above.
- [`tests/kqring_bench.c`](tests/kqring_bench.c) — benchmarks against `kqueue(2)` and plain system calls; [`tests/bench_report.py`](tests/bench_report.py) turns runs into the tables and charts in [`BENCHMARK.MD`](BENCHMARK.MD).

## Further reading

- [`ARCHITECTURE.MD`](ARCHITECTURE.MD) — how kqring works inside: shared memory layout, execution model, request lifecycle, zero-copy, workers and locking.
- [`BENCHMARK.MD`](BENCHMARK.MD) — performance against `kqueue(2)` and plain system calls, with method and known gaps.
- [`TODO.MD`](TODO.MD) — zero-copy roadmap and measured follow-ups.
