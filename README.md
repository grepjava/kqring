# `kqring` — `io_uring` (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING`) for FreeBSD

`kqring.ko` is a loadable FreeBSD kernel module implementing a subset of the Linux `io_uring` ABI, engineered specifically for modern per-worker thread-per-core runtimes like **Vega** (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING` + `RECV_MULTISHOT`).

## Layout

- [`include/sys/kqring.h`](include/sys/kqring.h) — Dual kernel/userspace header (`io_uring_sqe`, `io_uring_cqe`, `io_uring_buf_ring`, `/dev/kqring` ioctls, and inline `kqring_setup` / `kqring_enter` / `kqring_register` wrappers).
- [`kmod/`](kmod/) — FreeBSD 14/15 loadable kernel module (`style(9)` C), split by subsystem:
  - [`kqring_var.h`](kmod/kqring_var.h) — shared structures, limits and internal prototypes.
  - [`kqring_dev.c`](kmod/kqring_dev.c) — `/dev/kqring`, ioctls, ring setup and mapping, enter, module load/unload and process exit/exec hooks.
  - [`kqring_req.c`](kmod/kqring_req.c) — ring memory, SQE preparation and argument copy-in, request lifecycle, links, drains, CQE posting and taskwork.
  - [`kqring_sock.c`](kmod/kqring_sock.c) — socket operations and upcalls, multishot, `POLL_ADD` and `URING_CMD`.
  - [`kqring_file.c`](kmod/kqring_file.c) — file, path, xattr, `SPLICE`/`TEE` and other VFS operations.
  - [`kqring_sync.c`](kmod/kqring_sync.c) — timeouts, futexes, `WAITID`, `MSG_RING` and cancellation.
  - [`kqring_wq.c`](kmod/kqring_wq.c) — in-process worker pools and the `SQPOLL` thread.
  - [`kqring_rsrc.c`](kmod/kqring_rsrc.c) — fixed files, registered buffers, provided buffers and buffer rings, resource tags and personalities.
  - [`kqring_register.c`](kmod/kqring_register.c) — `KQRING_IOC_REGISTER` operations.
  - [`kqring_zc.c`](kmod/kqring_zc.c) — zero-copy send: page holding, `RLIMIT_MEMLOCK` accounting and `EXT_SFBUF` mbufs.
  - [`Makefile`](kmod/Makefile) — `<bsd.kmod.mk>` build definition for `kqring.ko`.
- [`tests/kqring_test.c`](tests/kqring_test.c) — Functional verification suite (70 test groups covering every supported opcode, registration, setup flags and the lifetime rules below), plus a short ping-pong comparison with `kqueue`.
- [`tests/kqring_bench.c`](tests/kqring_bench.c) — Benchmarks against `kqueue(2)` and plain syscalls; [`tests/bench_report.py`](tests/bench_report.py) turns its runs into the tables and charts in [`BENCHMARK.MD`](BENCHMARK.MD).
- [`TODO.MD`](TODO.MD) — zero-copy roadmap.

---

## Performance

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

On a 4-core i5-8250U running FreeBSD 15.1, as medians of 5 runs:

- **Many ready sockets:** 15–16% faster than kqueue, from 16 up to 1,024 connections, because one enter reaps every message.
- **One connection:** between 4% faster and 7% slower with multishot `RECV`, and 10–12% faster when receiving into one reused buffer.
- **Cached file reads:** 32 `READ`s per enter cost 29% less than `pread(2)`, served inline from the page cache.
- **Zero-copy send:** `SEND_ZC` is up to 35% faster than `write(2)` for 256 KiB messages, and 15–25% faster on TCP loopback from 16 KiB up.
- **Batching, accept and timers:** 32 `NOP`s per enter cost 78 ns each (an empty `kevent` costs 287 ns); multishot `ACCEPT` is 7% faster, and `TIMEOUT` is 41% faster.
- **Blocking pipes are 5× slower** than kqueue, because their reads go through a worker thread. On an `O_NONBLOCK` pipe, `READ` runs inline and is 9% faster than kqueue.

See [`BENCHMARK.MD`](BENCHMARK.MD) for the method, every result, and the known gaps.

---

## Resource lifetime and execution model

These are implementation constraints, not a claim of immunity to known CVE classes. They have been reviewed internally against each class (including the worker, `SQPOLL` and zero-copy paths), but kqring has not had an independent security audit.

| Linux `io_uring` CVE Class | `kqring.ko` behavior |
|---|---|
| **`io-wq` async worker UAFs** | In-process worker `kthread`s (`IORING_FEAT_NATIVE_WORKERS`) run inside `curproc` and never touch the CQ ring or the owner's fixed-file / registered-buffer tables (`ctx->files`, `ctx->bufs`). Requests retain resource and credential references. Workers execute I/O and queue results for the serialized issuer to complete. Without `SQPOLL`, the userspace owner drains taskwork in `KQRING_IOC_ENTER`; with `SQPOLL`, the in-process submission thread also drains it under `sq_sx`. |
| **`IOSQE_FIXED_FILE` reference cycles** | A ring can never be put in a fixed-file table (register, update, direct descriptors, `MSG_RING` `SEND_FD`), and a ring's fixed-file table is released when its owning process exits or calls `execve(2)` (breaking any `AF_UNIX` `SCM_RIGHTS` cycle where a ring descriptor is in flight on a socket held in its fixed table). Each slot holds a file reference and the file's Capsicum rights, which every request on it is checked against. Descriptors are re-resolved with rights checks on every attempt, and armed sockets hold `soref()`. |
| **Fixed-file / registered-buffer node use-after-free on update** | Slots are reference-counted nodes. A request takes a reference when it is issued and drops it when freed, so replacing or unregistering a slot never affects a request still using it. The tag CQE is posted once the last reference is gone. Table changes are serialized on the issuer path, including the `SQPOLL` thread when enabled. |
| **Pinned user-buffer page lifetime bugs** | Ordinary request buffers and user-memory buffer rings are not pinned (`READ_FIXED` and friends copy with `uiomove()`, and buffer rings with `copyin()`). Registered buffers hold their backing pages (`vm_fault_quick_hold_pages`) for zero-copy `SEND_ZC` (`IORING_RECVSEND_FIXED_BUF`), up to 16 MiB per buffer, charged against the user's `RLIMIT_MEMLOCK` and `vm.max_user_wired`. The charge is attached to the reference-counted page descriptor (`kqring_zc_buf`) and released only when the last reference (slot and all in-flight `EXT_SFBUF` mbufs) drops; copy-on-write mappings after `fork(2)` are refreshed before zero-copy transmit. |
| **Mapped buffer ring use-after-free (CVE-2024-0582)** | `IOU_PBUF_RING_MMAP` rings are VM objects, and each mapping holds its own reference. Unregistering a ring that is still mapped leaves the process with ordinary memory, never freed pages. The wired memory these rings use is capped per ring. |
| **Credentials outliving their context** | Personalities hold `crhold()` references, and so does each request using one. The executing issuer or worker switches credentials only for that request's work. A personality registered before `cap_enter(2)` is refused once the process is in capability mode, so it cannot be used to leave the sandbox. |
| **`eventfd` use-after-free** | The ring holds an `eventfd_get()` reference. Upcalls signal it under the ring lock, which unregistering also takes. |
| **EOF buffer-ID leak on `IOSQE_BUFFER_SELECT`** | Unlike Linux `io_uring` (which consumes a `PBUF_RING` buffer ID even when `recv` returns `0` at EOF), `kqring_pbuf_commit()` only advances the kernel ring head when `transferred > 0`. |
| **`MSG_RING` cross-ring CQ races** | CQ writes run on the serialized issuer path (owner or `SQPOLL` thread). `MSG_RING` queues the CQE on the target ring under its lock and wakes the owner, which posts it from taskwork. |
| **SQE argument TOCTOU / stale user pointers** | Everything an SQE points to (paths, iovecs, `msghdr`s, socket addresses, `open_how`, xattr names and values, futex vectors, timespecs) is copied in at submission (`IORING_FEAT_SUBMIT_STABLE`), charged to a per-ring budget, and live in-flight requests per ring are capped at `KQRING_MAX_REQS`. Only output buffers are touched later. |

---

## Supported Operations

Socket operations try the transfer inline first in `KQRING_IOC_ENTER` (unless `IOSQE_ASYNC` is set) and then wait on a socket upcall. A positioned file read that the page cache satisfies, and a `READ` or `WRITE` on an `O_NONBLOCK` pipe, also complete inline. Other file, pipe, VFS, `WAITID` and `IOSQE_ASYNC` requests run on per-ring in-process kernel worker threads (`IO_WQ_BOUND` and `IO_WQ_UNBOUND`, `IORING_FEAT_NATIVE_WORKERS`) that share the process's address space and descriptor table and queue completions as deferred taskwork for the ring owner. Futexes and timers are driven from the serialized issuer path. With `SQPOLL`, an in-process submission thread consumes SQEs and drains taskwork; `sq_sx` serializes it with enter and registration.

- **I/O:** `READ`, `WRITE`, `READV`, `WRITEV`, `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED`, `WRITEV_FIXED`, `FSYNC`, `SYNC_FILE_RANGE`, `FALLOCATE`, `FTRUNCATE`, `FADVISE`, `MADVISE`, `SPLICE`, `TEE`.
  - `READ` and `READV` (with one iovec) take a provided buffer.
  - On a socket, the read and write operations wait like `RECV` and `SEND` instead of blocking; on files, pipes and devices, they run on the in-process worker pool, except for the inline cases above.
  - `SPLICE` moves data between two descriptors where at least one is a pipe; `TEE` duplicates pipe buffer bytes to another pipe without consuming the source.
- **Sockets & Pipes:** `RECV`, `SEND`, `RECVMSG`, `SENDMSG`, `SEND_ZC`, `SENDMSG_ZC`, `ACCEPT`, `CONNECT`, `SOCKET`, `BIND`, `LISTEN`, `SHUTDOWN`, `READ_MULTISHOT`, `URING_CMD`.
  - Multishot is supported for `RECV`, `RECVMSG`, `ACCEPT` and `READ_MULTISHOT` (on both sockets and pipes).
  - `MSG_WAITALL` works on stream sockets, and `SEND` can take a destination address.
  - `SEND` can send from provided buffers, and `RECV` and `SEND` take bundles (`IORING_RECVSEND_BUNDLE`, up to 32 buffers).
  - `SEND_ZC` can send from a registered buffer (`IORING_RECVSEND_FIXED_BUF`).
  - `URING_CMD` supports `SIOCINQ`, `SIOCOUTQ`, `GETSOCKOPT` and `SETSOCKOPT`.
- **Files and paths:** `OPENAT`, `OPENAT2`, `CLOSE`, `PIPE`, `STATX`, `RENAMEAT`, `UNLINKAT`, `MKDIRAT`, `SYMLINKAT`, `LINKAT`, `GETXATTR`, `SETXATTR`, `FGETXATTR`, `FSETXATTR`.
- **Fixed files and buffers:** `FILES_UPDATE` (including `IORING_FILE_INDEX_ALLOC`), `FIXED_FD_INSTALL`, `PROVIDE_BUFFERS`, `REMOVE_BUFFERS`.
  - `OPENAT`, `OPENAT2`, `ACCEPT` (including multishot), `SOCKET` and `PIPE` can create direct descriptors (`file_index`), and `CLOSE` can close them.
- **Synchronisation:** `FUTEX_WAIT`, `FUTEX_WAITV`, `FUTEX_WAKE`, `WAITID`, `MSG_RING` (`IORING_MSG_DATA` and `IORING_MSG_SEND_FD`, including 32-byte `big_cqe` payload transfer on `SQE128` + `CQE32` rings).
- **Control:** `NOP`, `POLL_ADD` (including multishot and `IORING_POLL_ADD_LEVEL` on sockets, pipes, eventfds, and files), `POLL_REMOVE`, `TIMEOUT`, `TIMEOUT_REMOVE`, `LINK_TIMEOUT`, `ASYNC_CANCEL` (including `IORING_ASYNC_CANCEL_FD_FIXED`).
- **Setup flags:** `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_DEFER_TASKRUN`, `IORING_SETUP_CQSIZE`, `IORING_SETUP_CLAMP`, `IORING_SETUP_R_DISABLED`, `IORING_SETUP_SUBMIT_ALL`, `IORING_SETUP_COOP_TASKRUN`, `IORING_SETUP_TASKRUN_FLAG`, `IORING_SETUP_SQE128`, `IORING_SETUP_CQE32`, `IORING_SETUP_ATTACH_WQ`, `IORING_SETUP_SQPOLL`, `IORING_SETUP_SQ_AFF`, and `IORING_SETUP_IOPOLL`.
- **SQE flags:** `IOSQE_FIXED_FILE`, `IOSQE_IO_LINK`, `IOSQE_IO_HARDLINK`, `IOSQE_IO_DRAIN`, `IOSQE_ASYNC`, `IOSQE_CQE_SKIP_SUCCESS` and `IOSQE_BUFFER_SELECT` (any group), plus `sqe->personality`.
- **Registration:**
  - Files: `FILES`, `FILES2`, `FILES_UPDATE`, `FILES_UPDATE2`, `UNREGISTER_FILES` and `FILE_ALLOC_RANGE`, with sparse tables and tags.
  - Buffers: `BUFFERS`, `BUFFERS2`, `BUFFERS_UPDATE`, `UNREGISTER_BUFFERS` and `CLONE_BUFFERS` (including `IORING_REGISTER_SRC_REGISTERED`).
  - Buffer rings: `PBUF_RING` (including `IOU_PBUF_RING_MMAP` and `IOU_PBUF_RING_INC`), `UNREGISTER_PBUF_RING` and `PBUF_STATUS`.
  - Ring descriptors: `RING_FDS` and `UNREGISTER_RING_FDS` (with `IORING_ENTER_REGISTERED_RING`, `IORING_REGISTER_USE_REGISTERED_RING`, and `IORING_FEAT_REG_REG_RING`).
  - Workers: `IOWQ_AFF`, `UNREGISTER_IOWQ_AFF` and `IOWQ_MAX_WORKERS`.
  - Other: `EVENTFD`, `EVENTFD_ASYNC`, `UNREGISTER_EVENTFD`, `PERSONALITY`, `UNREGISTER_PERSONALITY`, `RESTRICTIONS`, `ENABLE_RINGS`, `PROBE`, `SYNC_CANCEL`, `SEND_MSG_RING` and `CLOCK`.

These Linux-only operations are rejected with `-EOPNOTSUPP` or `-EINVAL`:

- **No FreeBSD equivalent:** `EPOLL_CTL`, `EPOLL_WAIT`, `RECV_ZC`, and NAPI.

**Differences from Linux:**

- **Provided-buffer reservations.** Blocking worker I/O reserves a group's head without holding the ring-wide buffer-group lock. Other selections from that group fail with `-ENOBUFS`; removal or unregister fails with `EBUSY` until the transfer returns. Other groups and registration remain usable. EOF consumes no buffer.
- **Worker cancellation and CQE ordering.** Cancel (`ASYNC_CANCEL`, `POLL_REMOVE`) and `LINK_TIMEOUT` request an interrupt without waiting for running worker I/O. Because the cancel/remove/timeout SQE completes immediately while the target request's `-ECANCELED` CQE and resource release wait until its worker returns via taskwork, the cancel/remove/timeout CQE can arrive **before** the target's `-ECANCELED` CQE (whereas Linux posts the cancelled target's CQE first), and a cancellation CQE does not promise that the I/O had no side effects. Callers must wait for the target request's own CQE before freeing its buffers or state. Ring close detaches running workers without waiting and defers final context/pool deallocation until the last worker returns (module unload drains any remaining workers). `SYNC_CANCEL` accepts the cancellation but does not wait for the target CQE.
- **`execve(2)` disables open rings.** On `execve(2)`, any ring owned by the calling process is torn down (`ctx->closing = true`): its worker pool and `SQPOLL` thread are stopped, in-flight requests are disarmed and freed without posting `-ECANCELED` CQEs, and its fixed tables are released. A ring descriptor kept open across `execve(2)` (with `FD_CLOEXEC` cleared) therefore returns `EBADF` on subsequent `kqring_enter` and `kqring_register` calls, whereas Linux keeps the ring usable and posts `-ECANCELED` for cancelled requests.

- **Native values.** Flags and constants use their FreeBSD values: `O_*`, `AT_*`, `MSG_*`, `SOCK_*`, `POSIX_FADV_*`, `MADV_*`, `idtype_t` and `W*`. CQE errors are negated FreeBSD errnos.
- **Timeouts.** These complete with `-ETIME`, which the header defines as `ETIMEDOUT`.
- **Zero-copy send.** `SEND_ZC` and `SENDMSG_ZC` on connected stream sockets (`AF_UNIX` and `TCP`, for transfers of at least `PAGE_SIZE` bytes) attach the buffer's held physical pages (from a registered `IORING_RECVSEND_FIXED_BUF` slot or pinned on the fly from unregistered user buffers / `msg_iov` segments and charged against `RLIMIT_MEMLOCK` and `vm.max_user_wired`) to `EXT_SFBUF` mbufs and defer the `IORING_CQE_F_NOTIF` CQE (`res = 0`) until the network stack frees the mbufs. Smaller or ineligible `SEND_ZC` and `SENDMSG_ZC` requests (or requests that exceed `RLIMIT_MEMLOCK` / `vm.max_user_wired`) copy the data and post the notification CQE immediately (carrying `IORING_NOTIF_USAGE_ZC_COPIED` when `IORING_SEND_ZC_REPORT_USAGE` is set).
- **`STATX`.** It fills Linux's `struct statx` from `stat(2)`.
- **Xattrs.** Names are mapped to `extattr` namespaces: `user.` to user, and `system.` and `trusted.` to system.
- **`SYNC_FILE_RANGE`.** It syncs the whole file's data.
- **`SIGPIPE`.** It is never raised.
- **Registration is owner-only.** Once the ring is enabled, register calls that change it must come from the owner thread (`EEXIST` otherwise), as with enter. Before `IORING_REGISTER_ENABLE_RINGS`, one thread at a time may register. `PROBE` and `SEND_MSG_RING` work from any thread.
- **Fixed files keep Capsicum rights.** A slot carries the rights its descriptor had, and `FIXED_FD_INSTALL` gives them back. A direct descriptor opened relative to a directory with limited rights gets that directory's rights, as `openat(2)` would.
- **Registered buffers and pinned-memory limits.** `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED`, and `WRITEV_FIXED` copy through the user mapping with `uiomove()`, so unmapping a buffer makes those operations fail with `-EFAULT`. At registration, each buffer's backing pages (up to 16 MiB per buffer) are held for zero-copy `SEND_ZC` (`IORING_RECVSEND_FIXED_BUF`) and charged against the user's `RLIMIT_MEMLOCK` and `vm.max_user_wired` (`-ENOMEM` when exceeded). The charge is tied to the held pages (`kqring_zc_buf`) rather than the ring slot, so it stays charged until the slot is unregistered and all in-flight zero-copy mbufs are freed. FreeBSD's default `RLIMIT_MEMLOCK` is only 64 KiB (Linux's is 8 MiB), so applications that register more than that need a higher `memorylocked` in `login.conf(5)` or `ulimit -l`.
- **`IORING_MSG_SEND_FD` errors.** If the target cannot install the file (no table, bad slot, no free slot), the error is reported in the target's CQE. Linux reports it to the sender. Only the target's serialized issuer path writes to its table.
- **`CLONE_BUFFERS`.** Both rings must have the same owner thread.
- **Personalities and capability mode.** Personalities cannot be used to leave capability mode.
- **Eventfd.** It is signalled once per enter that posted CQEs, and when work is queued for an idle ring.
- **Direct `PIPE`.** The new pipe briefly has descriptors in the process before moving to its fixed slots, because FreeBSD has no way to create a pipe without them.
- **`SYNC_CANCEL` and `CLOCK`.** `SYNC_CANCEL`'s timeout is ignored, and target completion can be deferred until worker release. `CLOCK` is recorded, but timeouts are relative, so it has no effect.
- **Limits.** `PROVIDE_BUFFERS` holds at most 2^18 buffers per ring, and mapped buffer rings at most 16 MiB per ring. On Linux, both are left to memory accounting.

---

## Building & Running on FreeBSD 14 / 15

Ensure kernel sources are installed in `/usr/src` (`freebsd-version -ku`), then run:

```sh
# Build kqring.ko, tests/kqring_test and tests/kqring_bench
make

# Load the kernel module (creates /dev/kqring)
sudo kldload ./kmod/kqring.ko

# Run the functional tests (and a short kqring vs kqueue ping-pong)
./tests/kqring_test

# Run the full benchmarks (the send group needs ulimit -l 8192; see BENCHMARK.MD)
./tests/kqring_bench

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
