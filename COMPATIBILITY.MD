# Compatibility matrix

This file lists every io_uring capability against one baseline, says how kqring behaves, and points at the code and the test behind that claim. [`tests/compat_check.py`](tests/compat_check.py) checks it against the source, so a capability can't be added or removed without this file changing too.

**Baseline:** Linux 7.3, whose UAPI header (`linux/io_uring.h`, from `linux-libc-dev` 7.3.0-9.9 on Ubuntu 26.10) lists the capabilities and whose running kernel is what [`tests/kqring_diff.c`](tests/kqring_diff.c) compares against, and liburing 2.15. kqring's opcode numbers match that header up to `IORING_OP_PIPE`, and what Linux has after it is refused. kqring 0.3.0 on FreeBSD 15.1-RELEASE, amd64.

This matrix records what kqring is designed to do and what its tests check. [Differential conformance](ROADMAP.MD#differential-conformance) compares the behaviour that matters most with a running Linux kernel, case by case; rows outside those cases rest on kqring's own tests and on liburing's.

## How to read it

| Status | Meaning |
|---|---|
| Equivalent | No known difference from Linux. |
| Partial | Works, with the limit given. |
| Different | Works, and differs from Linux on purpose. |
| Unsupported | Refused, with the error given. |

Three differences apply everywhere and aren't repeated in the rows:

- Flags passed through SQEs use FreeBSD's values, and `cqe->res` holds negated FreeBSD errnos. See [Differences from Linux](README.md#differences-from-linux).
- A ring belongs to one process. See [Boundaries](#boundaries).
- Worker cancellation can post the cancel's CQE before its target's. See [Boundaries](#boundaries).

In the **Tests** column, `test_*` is a function in [`tests/kqring_test.c`](tests/kqring_test.c), `liburing/*` is a test from liburing 2.15, which may be [adapted](liburing/README.md#adapted-tests), and `fuzz` is [`tests/kqring_fuzz.c`](tests/kqring_fuzz.c). A liburing test that fails appears only on a row that isn't `Equivalent`, and the [accounting](liburing/README.md#test-results) gives the reason. `gap` means nothing tests it yet; every gap is also in [Validation gaps](#validation-gaps). **Source** names the function or table that holds the behaviour.

Platform and filesystem limits are marked **Platform** in the contract, because they don't come from the module.

## Opcodes

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_OP_NOP` | Partial | Only `IORING_NOP_INJECT_RESULT` is accepted; see [Subflags](#subflags). | `kqring_exec_sync` | `test_probe`, `liburing/nop`, `fuzz` |
| `IORING_OP_READV` | Equivalent | One iovec can take a provided buffer. | `kqring_exec_file_rw` | `test_vectored_io`, `test_disk_bio`, `liburing/read-write`, `fuzz` |
| `IORING_OP_WRITEV` | Equivalent | | `kqring_exec_file_rw` | `test_vectored_io`, `liburing/read-write`, `fuzz` |
| `IORING_OP_FSYNC` | Equivalent | | `kqring_exec_fsync` | `test_file_rw`, `liburing/fsync`, `fuzz` |
| `IORING_OP_READ_FIXED` | Different | Copies through the user mapping at each I/O, so an unmapped buffer gives `-EFAULT`. | `kqring_exec_file_rw` | `test_registered_buffers`, `test_disk_bio`, `liburing/read-write`, `fuzz` |
| `IORING_OP_WRITE_FIXED` | Different | As `READ_FIXED`. | `kqring_exec_file_rw` | `test_registered_buffers`, `test_disk_bio`, `liburing/read-write`, `fuzz` |
| `IORING_OP_POLL_ADD` | Partial | With no enter waiting, a non-socket wakeup from another process is posted a moment later by the ring's poller thread. An AF_UNIX peer's `SHUT_WR` isn't seen through a fixed-file index alone. | `kqring_exec_poll`, `kqring_kpoll_arm` | `test_poll_add`, `test_nonsock_poll`, `test_kpoll`, `test_poll_before_return`, `test_unix_peer_shutdown`, `liburing/poll`, `fuzz` |
| `IORING_OP_POLL_REMOVE` | Equivalent | | `kqring_poll_remove` | `test_poll_remove`, `liburing/poll-cancel`, `liburing/poll-mshot-update`, `fuzz` |
| `IORING_OP_SYNC_FILE_RANGE` | Partial | Syncs the whole file's data. | `kqring_exec_fsync` | `test_path_ops`, `test_fixed_file_ops` |
| `IORING_OP_SENDMSG` | Equivalent | `SIGPIPE` is never raised. | `kqring_exec_sendmsg` | `test_msg_ops`, `test_compat_contracts`, `liburing/send_recvmsg`, `fuzz` |
| `IORING_OP_RECVMSG` | Equivalent | | `kqring_exec_recvmsg` | `test_msg_ops`, `test_recvmsg_multishot`, `test_waitall`, `liburing/send_recvmsg`, `fuzz` |
| `IORING_OP_TIMEOUT` | Equivalent | Completes with `-ETIME`, defined as `ETIMEDOUT`. | `kqring_timeout_prep`, `kqring_timeout_arm` | `test_timeouts`, `liburing/timeout`, `fuzz` |
| `IORING_OP_TIMEOUT_REMOVE` | Equivalent | | `kqring_timeout_remove` | `test_timeouts`, `test_link_timeout`, `liburing/timeout`, `fuzz` |
| `IORING_OP_ACCEPT` | Equivalent | | `kqring_exec_accept` | `test_accept`, `test_accept_multishot`, `liburing/accept`, `fuzz` |
| `IORING_OP_ASYNC_CANCEL` | Different | Doesn't wait for running worker I/O. In-flight disk I/O, and a regular file's read or write a worker has started, isn't cancelled: it completes with its own result and the cancel gets `-EALREADY`, as io-wq's running work does. | `kqring_cancel`, `kqring_op_cancel_busy_locked` | `test_cancel`, `test_cancel_flags`, `test_worker_cancel_and_timeout`, `liburing/io-cancel`, `fuzz` |
| `IORING_OP_LINK_TIMEOUT` | Different | Doesn't wait for running worker I/O. A futex wait or `WAITID` it ends completes with `-ETIME`. A regular file's read or write already running isn't cancelled: the timeout completes with `-EALREADY`. | `kqring_ltimeout_arm` | `test_link_timeout`, `test_worker_cancel_and_timeout`, `liburing/link-timeout`, `fuzz` |
| `IORING_OP_CONNECT` | Equivalent | | `kqring_exec_connect` | `test_socket_ops`, `liburing/connect`, `fuzz` |
| `IORING_OP_FALLOCATE` | Partial | Modes 0 and `KEEP_SIZE \| PUNCH_HOLE` only, `-EOPNOTSUPP` otherwise. **Platform:** ZFS has no preallocation, so mode 0 gives `-EOPNOTSUPP` there. | `kqring_exec_fallocate` | `test_path_ops`, `test_fixed_file_ops`, `test_compat_contracts`, `fuzz` |
| `IORING_OP_OPENAT` | Equivalent | | `kqring_exec_openat` | `test_file_rw`, `test_direct_descriptors`, `liburing/open-close`, `fuzz` |
| `IORING_OP_CLOSE` | Equivalent | | `kqring_exec_close` | `test_socket_ops`, `test_direct_descriptors`, `liburing/open-close`, `fuzz` |
| `IORING_OP_FILES_UPDATE` | Equivalent | | `kqring_files_update` | `test_fixed_files`, `liburing/file-update`, `fuzz` |
| `IORING_OP_STATX` | Partial | Fills `struct statx` from `stat(2)`, so it holds what `stat(2)` knows. | `kqring_exec_statx` | `test_path_ops`, `liburing/statx`, `fuzz` |
| `IORING_OP_READ` | Equivalent | | `kqring_exec_file_rw`, `kqring_exec_recv` | `test_file_rw`, `test_short_links`, `test_inline_file_pipe`, `test_pipe_read_order`, `test_fifo_read`, `liburing/read-write`, `fuzz` |
| `IORING_OP_WRITE` | Equivalent | Writes to one regular file run on workers one at a time, in the order they were issued, and the issuer doesn't wait for them: each has a bucket from its vnode, as io-wq hashes them per inode. Files sharing one of the 64 buckets share its order. The order holds within a ring's worker pool, which rings share only through `IORING_SETUP_ATTACH_WQ`; Linux's io-wq is per thread. | `kqring_exec_file_rw`, `kqring_exec_send`, `kqring_wq_next_locked` | `test_file_rw`, `test_file_write_order`, `test_write_order`, `liburing/read-write`, `fuzz` |
| `IORING_OP_FADVISE` | Equivalent | | `kqring_exec_fadvise` | `test_path_ops`, `test_fixed_file_ops`, `liburing/fadvise` |
| `IORING_OP_MADVISE` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/madvise`, `fuzz` |
| `IORING_OP_SEND` | Equivalent | Takes provided buffers, bundles and a destination address. `SIGPIPE` is never raised. | `kqring_exec_send` | `test_send_armed`, `liburing/send_recv`, `liburing/recvsend_bundle`, `fuzz` |
| `IORING_OP_RECV` | Equivalent | A read at end of file consumes no provided buffer. | `kqring_exec_recv` | `test_multishot_and_eof`, `test_waitall`, `test_short_links`, `liburing/send_recv`, `liburing/recv-multishot`, `fuzz` |
| `IORING_OP_OPENAT2` | Equivalent | | `kqring_exec_openat` | `test_path_ops`, `liburing/openat2`, `fuzz` |
| `IORING_OP_EPOLL_CTL` | Unsupported | `-EOPNOTSUPP`. **Platform:** FreeBSD has no epoll. | `kqring_req_prep` | `test_phase2_validation`, `liburing/poll-v-poll` |
| `IORING_OP_SPLICE` | Partial | One side must be a pipe. **Platform:** large pipe writes block until read, which stops `liburing/splice` early. | `kqring_exec_splice` | `test_splice_and_tee`, `liburing/splice`, `fuzz` |
| `IORING_OP_PROVIDE_BUFFERS` | Partial | At most 2^18 buffers per ring. | `kqring_provide_buffers` | `test_buffer_groups`, `test_worker_buffer_reservation` |
| `IORING_OP_REMOVE_BUFFERS` | Partial | `EBUSY` while a worker holds a reservation in the group. | `kqring_remove_buffers` | `test_buffer_groups`, `test_worker_buffer_reservation` |
| `IORING_OP_TEE` | Equivalent | | `kqring_exec_tee` | `test_splice_and_tee` |
| `IORING_OP_SHUTDOWN` | Equivalent | | `kqring_exec_shutdown` | `test_socket_ops`, `liburing/shutdown`, `fuzz` |
| `IORING_OP_RENAMEAT` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/rename`, `fuzz` |
| `IORING_OP_UNLINKAT` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/unlink`, `fuzz` |
| `IORING_OP_MKDIRAT` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/mkdir`, `fuzz` |
| `IORING_OP_SYMLINKAT` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/symlink`, `fuzz` |
| `IORING_OP_LINKAT` | Equivalent | | `kqring_exec_sync` | `test_path_ops`, `liburing/hardlink`, `fuzz` |
| `IORING_OP_MSG_RING` | Different | A failed `IORING_MSG_SEND_FD` is reported in the target's CQE, where Linux reports it to the sender. | `kqring_msg_ring` | `test_msg_ring`, `test_send_fd`, `liburing/msg-ring`, `fuzz` |
| `IORING_OP_FSETXATTR` | Partial | Names map to `extattr` namespaces: `user.` to user, `system.` and `trusted.` to system. | `kqring_exec_setxattr` | `test_xattr`, `liburing/xattr`, `fuzz` |
| `IORING_OP_SETXATTR` | Partial | As `FSETXATTR`. | `kqring_exec_setxattr` | `test_xattr`, `liburing/xattr`, `fuzz` |
| `IORING_OP_FGETXATTR` | Partial | As `FSETXATTR`. | `kqring_exec_getxattr` | `test_xattr`, `liburing/xattr`, `fuzz` |
| `IORING_OP_GETXATTR` | Partial | As `FSETXATTR`. | `kqring_exec_getxattr` | `test_xattr`, `liburing/xattr`, `fuzz` |
| `IORING_OP_SOCKET` | Equivalent | | `kqring_exec_socket` | `test_socket_ops`, `test_direct_descriptors`, `liburing/socket`, `fuzz` |
| `IORING_OP_URING_CMD` | Partial | Sockets only: `SIOCINQ`, `SIOCOUTQ`, `GETSOCKOPT`, `SETSOCKOPT`. Anything else gives `-EOPNOTSUPP`. | `kqring_exec_uring_cmd` | `test_uring_cmd`, `liburing/socket-getsetsock-cmd` |
| `IORING_OP_SEND_ZC` | Partial | Zero-copy on connected stream sockets for at least a page, otherwise copied with the notification posted at once. With `MSG_WAITALL`, a short send goes on zero-copy until the whole buffer is sent, with one notification. | `kqring_exec_send` | `test_send_zc`, `test_send_zc_spans`, `test_send_zc_waitall`, `liburing/send-zerocopy`, `fuzz` |
| `IORING_OP_SENDMSG_ZC` | Partial | As `SEND_ZC`. | `kqring_exec_sendmsg` | `test_send_zc`, `test_send_zc_waitall`, `liburing/send-zerocopy`, `liburing/sendzc-bug` |
| `IORING_OP_READ_MULTISHOT` | Equivalent | On a file that can't be polled gives `-EBADF`, where Linux gives `EBADFD`, which FreeBSD lacks. | `kqring_exec_read_multishot` | `test_read_multishot`, `test_pipe_multishot_drained`, `liburing/read-mshot`, `fuzz` |
| `IORING_OP_WAITID` | Different | Cancelling gives 0, where Linux gives 1; a link timeout gives `-ETIME`. | `kqring_exec_waitid` | `test_waitid`, `liburing/waitid` |
| `IORING_OP_FUTEX_WAIT` | Different | Also ends when the word changes with no `FUTEX_WAKE`. Cancelling gives 0, where Linux gives 1. | `kqring_futex_arm` | `test_futex_wait`, `liburing/futex`, `fuzz` |
| `IORING_OP_FUTEX_WAKE` | Partial | Wakes requests, not threads in `_umtx_op(2)`. | `kqring_futex_wake` | `test_futex_wake`, `liburing/futex`, `fuzz` |
| `IORING_OP_FUTEX_WAITV` | Different | As `FUTEX_WAIT`. | `kqring_futex_arm` | `test_futex_wake`, `liburing/futex` |
| `IORING_OP_FIXED_FD_INSTALL` | Equivalent | Gives back the Capsicum rights the slot kept. | `kqring_exec_fixed_fd_install` | `test_direct_descriptors`, `liburing/fd-install`, `fuzz` |
| `IORING_OP_FTRUNCATE` | Equivalent | | `kqring_exec_ftruncate` | `test_path_ops`, `test_fixed_file_ops`, `liburing/truncate`, `fuzz` |
| `IORING_OP_BIND` | Equivalent | | `kqring_exec_bind` | `test_socket_ops`, `test_direct_descriptors`, `liburing/bind-listen`, `fuzz` |
| `IORING_OP_LISTEN` | Equivalent | | `kqring_exec_listen` | `test_socket_ops`, `test_direct_descriptors`, `liburing/bind-listen` |
| `IORING_OP_RECV_ZC` | Unsupported | `-EOPNOTSUPP`. **Platform:** needs driver support FreeBSD lacks. | `kqring_req_prep` | `test_phase2_validation` |
| `IORING_OP_EPOLL_WAIT` | Unsupported | `-EOPNOTSUPP`. **Platform:** FreeBSD has no epoll. | `kqring_req_prep` | `test_phase2_validation` |
| `IORING_OP_READV_FIXED` | Different | As `READ_FIXED`. Every iovec must be inside the buffer. | `kqring_exec_file_rw` | `test_registered_buffers` |
| `IORING_OP_WRITEV_FIXED` | Different | As `READV_FIXED`. | `kqring_exec_file_rw` | `test_compat_gaps` |
| `IORING_OP_PIPE` | Partial | A direct pipe briefly has descriptors in the process. | `kqring_exec_pipe` | `test_direct_descriptors`, `fuzz` |
| `IORING_OP_NOP128` | Unsupported | `-EINVAL`, even with 128-byte SQEs. | `kqring_req_prep` | `test_compat_contracts` |
| `IORING_OP_URING_CMD128` | Unsupported | `-EINVAL`, even with 128-byte SQEs. | `kqring_req_prep` | `test_compat_contracts` |

## Setup flags

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_SETUP_IOPOLL` | Partial | Allowed on seekable vnodes, including disk devices. Other descriptors give `-EOPNOTSUPP`. Disk requests go to the driver as bios, others run as usual and are reaped by the enter. | `kqring_iopoll_op_ok`, `kqring_op_iopoll_check` | `test_sqpoll_and_iopoll`, `liburing/iopoll`, `fuzz` |
| `IORING_SETUP_SQPOLL` | Equivalent | The thread lives in the owner's process. | `kqring_sqpoll_thread` | `test_sqpoll_and_iopoll`, `liburing/sq-poll-kthread`, `fuzz` |
| `IORING_SETUP_SQ_AFF` | Equivalent | | `kqring_ioc_setup` | `test_setup_errors`, `test_sqpoll_and_iopoll` |
| `IORING_SETUP_CQSIZE` | Equivalent | | `kqring_ioc_setup` | `test_resize_rings`, `liburing/cq-size`, `fuzz` |
| `IORING_SETUP_CLAMP` | Equivalent | | `kqring_ioc_setup` | `test_setup_errors`, `fuzz` |
| `IORING_SETUP_ATTACH_WQ` | Equivalent | | `kqring_ioc_setup` | `test_setup_errors`, `test_sqe128_cqe32_attach_wq`, `fuzz` |
| `IORING_SETUP_R_DISABLED` | Equivalent | Before enabling, one thread at a time may register. | `kqring_ioc_setup` | `test_restrictions`, `liburing/register-restrictions`, `fuzz` |
| `IORING_SETUP_SUBMIT_ALL` | Equivalent | | `kqring_submit_sqes` | `test_compat_gaps`, `liburing/drop-submit`, `fuzz` |
| `IORING_SETUP_COOP_TASKRUN` | Equivalent | A hint with nothing to do here: kqring sends no interrupts to the owner. | `kqring_ioc_setup` | `test_phase5_security_audit`, `fuzz` |
| `IORING_SETUP_TASKRUN_FLAG` | Equivalent | | `kqring_kp_kick` | `test_taskrun_on_wakeup`, `fuzz` |
| `IORING_SETUP_SQE128` | Equivalent | | `kqring_ioc_setup` | `test_sqe128_cqe32_attach_wq`, `fuzz` |
| `IORING_SETUP_CQE32` | Equivalent | | `kqring_ioc_setup` | `test_sqe128_cqe32_attach_wq`, `fuzz` |
| `IORING_SETUP_SINGLE_ISSUER` | Partial | Another thread's wait fails with `EEXIST` unless the CQ can already satisfy it, where Linux waits: the ring's state is the owner's, without locks. | `kqring_is_owner` | `test_single_issuer`, `test_single_issuer_wait`, `liburing/single-issuer`, `fuzz` |
| `IORING_SETUP_DEFER_TASKRUN` | Equivalent | | `kqring_ioc_setup` | `test_setup_errors`, `liburing/defer-taskrun`, `fuzz` |
| `IORING_SETUP_NO_MMAP` | Equivalent | `fork(2)` shares these rings rather than copying them. | `kqring_ioc_setup` | `test_no_mmap`, `liburing/no-mmap-inval`, `fuzz` |
| `IORING_SETUP_REGISTERED_FD_ONLY` | Equivalent | | `kqring_ioc_setup` | `test_no_mmap`, `liburing/reg-fd-only`, `fuzz` |
| `IORING_SETUP_NO_SQARRAY` | Equivalent | | `kqring_ioc_setup` | `test_no_mmap`, `fuzz` |
| `IORING_SETUP_HYBRID_IOPOLL` | Unsupported | `EINVAL`. | `kqring_ioc_setup` | `test_compat_contracts` |
| `IORING_SETUP_CQE_MIXED` | Unsupported | `EINVAL`. | `kqring_ioc_setup` | `test_compat_contracts` |
| `IORING_SETUP_SQE_MIXED` | Unsupported | `EINVAL`. | `kqring_ioc_setup` | `test_compat_contracts` |
| `IORING_SETUP_SQ_REWIND` | Unsupported | `EINVAL`. | `kqring_ioc_setup` | `test_compat_contracts` |

## Enter flags

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_ENTER_GETEVENTS` | Equivalent | | `kqring_ioc_enter_body` | `test_enter_validation`, `liburing/submit-and-wait`, `fuzz` |
| `IORING_ENTER_SQ_WAKEUP` | Equivalent | A forked child may use it. | `kqring_ioc_enter_body` | `test_enter_validation`, `test_sqpoll_and_iopoll`, `fuzz` |
| `IORING_ENTER_SQ_WAIT` | Equivalent | | `kqring_ioc_enter_body` | `test_enter_validation`, `test_sqpoll_and_iopoll`, `liburing/sqwait`, `fuzz` |
| `IORING_ENTER_EXT_ARG` | Different | Timeouts, `min_wait_usec` and signal masks work. The argument is checked before anything is submitted, so a bad one fails the enter; Linux submits first and returns the count. | `kqring_ioc_enter_body` | `test_enter_validation`, `test_min_wait`, `test_enter_sigmask`, `liburing/min-timeout`, `fuzz` |
| `IORING_ENTER_REGISTERED_RING` | Equivalent | | `kqring_ioc_enter_body` | `test_enter_validation`, `test_registered_rings`, `liburing/reg-reg-ring`, `fuzz` |
| `IORING_ENTER_ABS_TIMER` | Equivalent | The timeout is a time of the ring's clock (`IORING_REGISTER_CLOCK`). | `kqring_enter_wait_sig` | `test_enter_abs_timer`, `test_reg_wait`, `liburing/wait-timeout`, `fuzz` |
| `IORING_ENTER_EXT_ARG_REG` | Equivalent | The wait is the `io_uring_reg_wait` at the argument's offset in the ring's region, which must be aligned to a `long` and inside it (`EFAULT`). After `fork(2)` it is read through its address, so the process's own writes count. | `kqring_reg_wait_get` | `test_reg_wait`, `liburing/reg-wait`, `fuzz` |
| `IORING_ENTER_NO_IOWAIT` | Equivalent | Accepted. **Platform:** FreeBSD doesn't count time waiting for I/O apart, so a wait never counts as such. | `kqring_ioc_enter_body` | `test_enter_abs_timer`, `liburing/iowait` |
| Signal mask | Equivalent | Plain or in `io_uring_getevents_arg`, as `ppoll(2)`: the thread waits with it if the CQ can't satisfy the wait already, and its size is checked only then. A signal it lets in ends the wait with `EINTR`, or success if CQEs are posted, and is handled with the mask still in place. IOPOLL rings ignore it. **Platform:** the mask is a FreeBSD `sigset_t`, so its size is 16 (Linux's is 8). | `kqring_enter_wait_sig` | `test_enter_sigmask`, `test_reg_wait`, `liburing/reg-wait`, `fuzz` |

## SQE flags

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IOSQE_FIXED_FILE` | Equivalent | A slot keeps the Capsicum rights its descriptor had. | `kqring_op_fget` | `test_fixed_files`, `test_compat_contracts`, `liburing/file-register`, `fuzz` |
| `IOSQE_IO_DRAIN` | Equivalent | | `kqring_submit_sqes` | `test_drain`, `liburing/link_drain`, `fuzz` |
| `IOSQE_IO_LINK` | Equivalent | A short read or write breaks the link, and so does `SPLICE`, `TEE` or a socket opcode with `MSG_WAITALL`. | `kqring_submit_sqes` | `test_links`, `test_short_links`, `liburing/link`, `fuzz` |
| `IOSQE_IO_HARDLINK` | Equivalent | | `kqring_submit_sqes` | `test_links`, `liburing/link`, `fuzz` |
| `IOSQE_ASYNC` | Equivalent | | `kqring_req_issue` | `test_sqe_validation`, `liburing/read-write`, `fuzz` |
| `IOSQE_BUFFER_SELECT` | Partial | Blocking worker I/O reserves the group's head buffer; other selections get `-ENOBUFS` meanwhile. | `kqring_buf_select` | `test_buffer_groups`, `test_worker_buffer_reservation`, `liburing/buf-ring`, `fuzz` |
| `IOSQE_CQE_SKIP_SUCCESS` | Equivalent | | `kqring_req_complete` | `test_cqe_skip`, `liburing/skip-cqe`, `fuzz` |

## Registrations

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_REGISTER_BUFFERS` | Different | Pages are held for zero-copy send only, up to 16 MiB per buffer, charged to `RLIMIT_MEMLOCK` and `vm.max_user_wired`. File-backed buffers are accepted, where Linux refuses them. | `kqring_bufs_register` | `test_registered_buffers`, `liburing/io_uring_register`, `fuzz` |
| `IORING_UNREGISTER_BUFFERS` | Equivalent | | `kqring_bufs_free` | `test_registered_buffers`, `liburing/rsrc_tags` |
| `IORING_REGISTER_FILES` | Equivalent | A ring can't be put in a table. | `kqring_files_register` | `test_fixed_files`, `liburing/file-register`, `fuzz` |
| `IORING_UNREGISTER_FILES` | Equivalent | | `kqring_files_free` | `test_fixed_files`, `liburing/file-register` |
| `IORING_REGISTER_EVENTFD` | Partial | Signalled once per enter that posted CQEs, and when work is queued for an idle ring. | `kqring_evfd_register` | `test_eventfd`, `liburing/eventfd` |
| `IORING_UNREGISTER_EVENTFD` | Equivalent | | `kqring_evfd_unregister` | `test_eventfd`, `liburing/eventfd` |
| `IORING_REGISTER_FILES_UPDATE` | Equivalent | | `kqring_files_update` | `test_fixed_files`, `liburing/file-update` |
| `IORING_REGISTER_EVENTFD_ASYNC` | Equivalent | | `kqring_evfd_register` | `test_eventfd`, `liburing/eventfd-reg` |
| `IORING_REGISTER_PROBE` | Equivalent | Works from any thread. | `kqring_register_probe` | `test_probe`, `liburing/probe`, `fuzz` |
| `IORING_REGISTER_PERSONALITY` | Different | Can't be used to leave capability mode. | `kqring_personality_register` | `test_personality`, `liburing/personality`, `fuzz` |
| `IORING_UNREGISTER_PERSONALITY` | Equivalent | | `kqring_personality_unregister` | `test_personality` |
| `IORING_REGISTER_RESTRICTIONS` | Equivalent | | `kqring_restrictions_register` | `test_restrictions`, `liburing/register-restrictions` |
| `IORING_REGISTER_ENABLE_RINGS` | Equivalent | | `kqring_register_op` | `test_restrictions`, `liburing/register-restrictions`, `fuzz` |
| `IORING_REGISTER_FILES2` | Equivalent | | `kqring_files_register` | `test_rsrc_tags`, `liburing/rsrc_tags` |
| `IORING_REGISTER_FILES_UPDATE2` | Equivalent | A refused descriptor leaves its slot empty. | `kqring_files_update` | `test_rsrc_tags`, `test_files_update_refused`, `liburing/rsrc_tags` |
| `IORING_REGISTER_BUFFERS2` | Equivalent | | `kqring_bufs_register` | `test_tag_failed_register`, `liburing/rsrc_tags` |
| `IORING_REGISTER_BUFFERS_UPDATE` | Equivalent | | `kqring_bufs_update` | `test_rsrc_tags`, `liburing/rsrc_tags` |
| `IORING_REGISTER_IOWQ_AFF` | Equivalent | | `kqring_iowq_aff` | `test_iowq_register` |
| `IORING_UNREGISTER_IOWQ_AFF` | Equivalent | | `kqring_iowq_aff` | `test_iowq_register` |
| `IORING_REGISTER_IOWQ_MAX_WORKERS` | Equivalent | | `kqring_iowq_max_workers` | `test_iowq_register`, `test_worker_max_workers` |
| `IORING_REGISTER_RING_FDS` | Equivalent | | `kqring_ring_fds_register` | `test_registered_rings`, `liburing/reg-reg-ring`, `fuzz` |
| `IORING_UNREGISTER_RING_FDS` | Equivalent | | `kqring_ring_fds_unregister` | `test_registered_rings`, `fuzz` |
| `IORING_REGISTER_PBUF_RING` | Partial | Mapped rings hold at most 16 MiB per ring. | `kqring_pbuf_register` | `test_buffer_groups`, `liburing/buf-ring`, `fuzz` |
| `IORING_UNREGISTER_PBUF_RING` | Partial | `EBUSY` while a worker holds a reservation in the group. | `kqring_pbuf_unregister` | `test_buffer_groups`, `liburing/buf-ring` |
| `IORING_REGISTER_SYNC_CANCEL` | Different | Waits for what it cancelled, as on Linux. A request whose worker was only waiting, for a pipe's data say, is cancelled within any timeout, as a poll is on Linux. Cancelling a futex wait or `WAITID` gives 0. | `kqring_sync_cancel`, `kqring_cancel_wait` | `test_sync_cancel_worker`, `test_sync_cancel_waiting`, `test_fixed_cancel`, `liburing/sync-cancel` |
| `IORING_REGISTER_FILE_ALLOC_RANGE` | Equivalent | | `kqring_file_alloc_range` | `test_direct_descriptors`, `liburing/file-register` |
| `IORING_REGISTER_PBUF_STATUS` | Equivalent | | `kqring_pbuf_status` | `test_buffer_groups`, `liburing/ringbuf-status` |
| `IORING_REGISTER_NAPI` | Partial | Sets only a busy-wait: a wait spins up to `busy_poll_to` µs. **Platform:** FreeBSD has no NAPI polling. | `kqring_register_napi` | `test_napi_busy_poll` |
| `IORING_UNREGISTER_NAPI` | Partial | As `REGISTER_NAPI`. | `kqring_register_napi` | `test_napi_busy_poll` |
| `IORING_REGISTER_CLOCK` | Equivalent | The clock of `IORING_ENTER_ABS_TIMER` deadlines. **Platform:** FreeBSD defines `CLOCK_BOOTTIME` as `CLOCK_MONOTONIC`, so there is one clock to choose. | `kqring_register_clock` | `test_fixed_file_ops`, `test_enter_abs_timer`, `liburing/wait-timeout` |
| `IORING_REGISTER_CLONE_BUFFERS` | Partial | Both rings must have the same owner thread. | `kqring_bufs_clone` | `test_registered_buffers`, `test_registered_rings`, `liburing/regbuf-clone` |
| `IORING_REGISTER_SEND_MSG_RING` | Equivalent | Works from any thread, or with no ring (descriptor -1). | `kqring_register_send_msg`, `kqring_register_blind` | `test_send_fd`, `test_compat_contracts`, `liburing/msg-ring` |
| `IORING_REGISTER_RESIZE_RINGS` | Equivalent | On `DEFER_TASKRUN` rings, as on Linux. | `kqring_resize_rings` | `test_resize_rings`, `liburing/resize-rings`, `fuzz` |
| `IORING_REGISTER_USE_REGISTERED_RING` | Equivalent | | `kqring_register_op` | `test_registered_rings`, `liburing/reg-reg-ring` |
| `IORING_REGISTER_ZCRX_IFQ` | Unsupported | `EOPNOTSUPP`. **Platform:** needs driver support FreeBSD lacks. | `kqring_register_op` | `test_compat_contracts` |
| `IORING_REGISTER_MEM_REGION` | Equivalent | One region per ring, the caller's pages (held, so they must be mapped writable) or the module's, mapped at the offset it returns. With `IORING_MEM_REGION_REG_WAIT_ARG` it holds registered waits, and needs a ring still disabled. The module's memory counts against `kern.kqring.user_ring_mem` (see Resource limits). | `kqring_register_mem_region` | `test_reg_wait`, `liburing/reg-wait`, `fuzz` |
| `IORING_REGISTER_QUERY` | Unsupported | `EOPNOTSUPP`. | `kqring_register_op` | `test_compat_contracts` |
| `IORING_REGISTER_ZCRX_CTRL` | Unsupported | `EOPNOTSUPP`. **Platform:** needs driver support FreeBSD lacks. | `kqring_register_op` | `test_compat_contracts` |
| `IORING_REGISTER_BPF_FILTER` | Unsupported | `EOPNOTSUPP`. | `kqring_register_op` | `test_compat_contracts` |
| `io_uring_task_restriction` | Unsupported | `EOPNOTSUPP`: restrictions apply to one ring. | `kqring_register_blind` | `test_compat_contracts` |
| `io_uring_buf_reg.min_left` | Equivalent | Only with `IOU_PBUF_RING_INC`, else `EINVAL`. A buffer with less than `min_left` bytes left is used up, without `IORING_CQE_F_BUF_MORE`. | `kqring_pbuf_register`, `kqring_buf_commit` | `test_buffer_groups`, `test_compat_contracts`, `liburing/recvmsg-inc-tail`, `fuzz` |

## Features

These are the `IORING_FEAT_*` bits setup returns.

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_FEAT_SINGLE_MMAP` | Equivalent | `IORING_OFF_CQ_RING` maps the same memory. | `kqring_ioc_setup` | `test_compat_gaps`, `liburing/ignore-single-mmap` |
| `IORING_FEAT_NODROP` | Partial | Enters stop submitting with `EBUSY` once 16,384 completions are waiting. | `kqring_post_cqe_common` | `test_cq_overflow`, `test_cq_backpressure`, `liburing/cq-overflow` |
| `IORING_FEAT_SUBMIT_STABLE` | Equivalent | | `kqring_req_prep` | `test_submit_stable` |
| `IORING_FEAT_RW_CUR_POS` | Equivalent | | `kqring_exec_file_rw` | `test_file_rw`, `liburing/socket-rw-offset` |
| `IORING_FEAT_CUR_PERSONALITY` | Equivalent | | `kqring_req_issue_cred` | `test_personality` |
| `IORING_FEAT_FAST_POLL` | Equivalent | | `kqring_op_attempt` | `test_send_armed`, `liburing/accept-link` |
| `IORING_FEAT_POLL_32BITS` | Equivalent | | `kqring_req_prep` | `test_nonsock_poll` |
| `IORING_FEAT_SQPOLL_NONFIXED` | Equivalent | | `kqring_sqpoll_thread` | `test_sqpoll_and_iopoll` |
| `IORING_FEAT_EXT_ARG` | Equivalent | See `IORING_ENTER_EXT_ARG`. | `kqring_ioc_enter_body` | `test_enter_validation`, `test_enter_sigmask`, `liburing/timeout-new` |
| `IORING_FEAT_NATIVE_WORKERS` | Equivalent | | `kqring_wq_enqueue` | `test_worker_process_lifecycle` |
| `IORING_FEAT_RSRC_TAGS` | Equivalent | | `kqring_rsrc_put` | `test_rsrc_tags`, `liburing/rsrc_tags` |
| `IORING_FEAT_CQE_SKIP` | Equivalent | | `kqring_req_complete` | `test_cqe_skip`, `liburing/skip-cqe` |
| `IORING_FEAT_LINKED_FILE` | Equivalent | | `kqring_op_fget` | `liburing/open-direct-link` |
| `IORING_FEAT_REG_REG_RING` | Equivalent | | `kqring_register_op` | `test_registered_rings`, `liburing/reg-reg-ring` |
| `IORING_FEAT_RECVSEND_BUNDLE` | Partial | Up to 32 buffers per CQE. | `kqring_exec_recv` | `liburing/recvsend_bundle`, `liburing/recvsend_bundle-inc` |
| `IORING_FEAT_MIN_TIMEOUT` | Equivalent | | `kqring_ioc_enter_body` | `test_min_wait`, `liburing/min-timeout` |
| `IORING_FEAT_NO_IOWAIT` | Equivalent | See `IORING_ENTER_NO_IOWAIT`. | `kqring_ioc_setup` | `test_enter_abs_timer`, `liburing/iowait` |
| `IORING_FEAT_RW_ATTR` | Unsupported | Not advertised. | `kqring_ioc_setup` | `gap` |

## Subflags

| Capability | Status | Contract | Source | Tests |
|---|---|---|---|---|
| `IORING_NOP_INJECT_RESULT` | Equivalent | | `kqring_exec_sync` | `test_trigger_concurrent` |
| `IORING_NOP_FILE` | Unsupported | `-EINVAL`, as are `IORING_NOP_FIXED_FILE`, `IORING_NOP_FIXED_BUFFER`, `IORING_NOP_TW` and `IORING_NOP_CQE32`. | `kqring_req_prep` | `test_compat_contracts` |
| `IORING_FSYNC_DATASYNC` | Equivalent | | `kqring_exec_fsync` | `liburing/fsync` |
| `IORING_TIMEOUT_ABS` | Equivalent | | `kqring_timeout_rel` | `test_timeouts` |
| `IORING_TIMEOUT_BOOTTIME` | Partial | Measured on the uptime clock, as the default is. | `kqring_timeout_rel` | `test_timeouts` |
| `IORING_TIMEOUT_REALTIME` | Partial | Read when the timer is armed, so a later clock step doesn't move it. | `kqring_timeout_rel` | `test_timeouts` |
| `IORING_TIMEOUT_UPDATE` | Equivalent | | `kqring_timeout_remove` | `test_timeouts`, `liburing/timeout` |
| `IORING_LINK_TIMEOUT_UPDATE` | Equivalent | | `kqring_timeout_remove` | `test_link_timeout`, `liburing/link-timeout` |
| `IORING_TIMEOUT_ETIME_SUCCESS` | Equivalent | | `kqring_timeout_prep` | `test_timeouts` |
| `IORING_TIMEOUT_MULTISHOT` | Equivalent | | `kqring_timeout_fire` | `test_timeouts`, `liburing/timeout` |
| `IORING_TIMEOUT_IMMEDIATE_ARG` | Unsupported | `-EINVAL`. | `kqring_timeout_prep` | `test_compat_contracts` |
| `IORING_POLL_ADD_MULTI` | Partial | On the ring's own eventfd, a wakeup after the ring signalled it for posted CQEs makes the poll one-shot, as Linux's `EPOLL_URING_WAKE` does. If a program's write and that signal wake it together, kqring posts one last CQE where Linux posts one with `F_MORE` and then the last. | `kqring_kpoll_deliver` | `test_nonsock_poll`, `test_poll_own_eventfd`, `liburing/evloop`, `liburing/poll-mshot-update`, `liburing/poll-mshot-wake` |
| `IORING_POLL_ADD_LEVEL` | Equivalent | In the UAPI, but refused with `EINVAL`, as on Linux. | `kqring_req_prep` | `test_compat_gaps` |
| `IORING_POLL_UPDATE_EVENTS` | Equivalent | | `kqring_poll_remove` | `liburing/poll-mshot-update` |
| `IORING_POLL_UPDATE_USER_DATA` | Equivalent | | `kqring_poll_remove` | `test_poll_remove`, `liburing/poll-mshot-update` |
| `IORING_ASYNC_CANCEL_ALL` | Equivalent | | `kqring_cancel` | `test_cancel_flags`, `liburing/poll-cancel-all` |
| `IORING_ASYNC_CANCEL_FD` | Equivalent | | `kqring_cancel_match_req` | `test_cancel_flags`, `liburing/sync-cancel` |
| `IORING_ASYNC_CANCEL_ANY` | Equivalent | | `kqring_cancel_match_req` | `test_cancel_flags` |
| `IORING_ASYNC_CANCEL_FD_FIXED` | Equivalent | | `kqring_cancel` | `test_fixed_cancel` |
| `IORING_ASYNC_CANCEL_USERDATA` | Equivalent | | `kqring_cancel_match_req` | `test_cancel` |
| `IORING_ASYNC_CANCEL_OP` | Equivalent | | `kqring_cancel_match_req` | `test_cancel_flags` |
| `IORING_RECVSEND_POLL_FIRST` | Equivalent | A hint. | `kqring_req_prep` | `test_multishot_and_eof`, `liburing/send-zerocopy` |
| `IORING_RECV_MULTISHOT` | Equivalent | `sqe->len` caps one run's bytes, after which the request yields to other work, and `sqe->optlen` caps the total. | `kqring_exec_recv`, `kqring_op_attempt` | `test_multishot_and_eof`, `liburing/recv-multishot`, `liburing/recv-mshot-fair` |
| `IORING_RECVSEND_FIXED_BUF` | Equivalent | | `kqring_req_resources` | `test_registered_buffers`, `liburing/send-zerocopy` |
| `IORING_RECVSEND_BUNDLE` | Partial | Up to 32 buffers per CQE. | `kqring_exec_recv` | `liburing/recvsend_bundle` |
| `IORING_SEND_ZC_REPORT_USAGE` | Equivalent | | `kqring_req_complete` | `test_send_zc`, `liburing/send-zerocopy` |
| `IORING_SEND_VECTORIZED` | Unsupported | `-EINVAL`. | `kqring_req_prep` | `test_compat_contracts` |
| `IORING_ACCEPT_MULTISHOT` | Equivalent | | `kqring_exec_accept` | `test_accept_multishot`, `liburing/accept` |
| `IORING_ACCEPT_DONTWAIT` | Equivalent | | `kqring_req_issue_cred` | `test_accept_multishot` |
| `IORING_ACCEPT_POLL_FIRST` | Equivalent | A hint. | `kqring_req_prep` | `test_compat_gaps` |
| `IORING_MSG_DATA` | Equivalent | | `kqring_msg_ring` | `test_msg_ring`, `liburing/msg-ring` |
| `IORING_MSG_SEND_FD` | Different | See `IORING_OP_MSG_RING`. | `kqring_msg_ring` | `test_send_fd`, `liburing/msg-ring-fd` |
| `IORING_MSG_RING_CQE_SKIP` | Equivalent | | `kqring_msg_ring` | `test_send_fd` |
| `IORING_MSG_RING_FLAGS_PASS` | Equivalent | | `kqring_msg_ring` | `test_msg_ring`, `liburing/msg-ring-flags` |
| `IORING_FILE_INDEX_ALLOC` | Equivalent | | `kqring_fixed_alloc` | `test_direct_descriptors`, `liburing/open-direct-pick` |
| `IORING_FIXED_FD_NO_CLOEXEC` | Equivalent | | `kqring_exec_fixed_fd_install` | `test_direct_descriptors`, `liburing/fd-install` |
| `IOU_PBUF_RING_MMAP` | Equivalent | | `kqring_pbuf_register` | `test_buffer_groups`, `liburing/buf-ring` |
| `IOU_PBUF_RING_INC` | Equivalent | A read at end of file reports the buffer it didn't use, with `IORING_CQE_F_BUF_MORE`. | `kqring_buf_commit`, `kqring_buf_eof` | `test_buffer_groups`, `liburing/read-inc-file`, `liburing/read-inc-buf-more` |
| `IORING_CQE_F_SOCK_NONEMPTY` | Equivalent | | `kqring_exec_accept` | `liburing/accept-non-empty` |
| `IORING_CQE_F_BUF_MORE` | Equivalent | | `kqring_buf_commit` | `test_buffer_groups` |
| `IORING_CQ_EVENTFD_DISABLED` | Equivalent | | `kqring_evfd_signal_locked` | `test_eventfd`, `liburing/eventfd-disable` |
| `IORING_RSRC_REGISTER_SPARSE` | Equivalent | | `kqring_register_op` | `test_rsrc_tags`, `liburing/rsrc_tags` |
| `IORING_REGISTER_SRC_REGISTERED` | Equivalent | | `kqring_bufs_clone` | `test_registered_rings` |
| `IORING_REGISTER_DST_REPLACE` | Equivalent | | `kqring_bufs_clone` | `test_registered_buffers` |
| `SPLICE_F_FD_IN_FIXED` | Equivalent | | `kqring_op_fget` | `test_splice_and_tee` |
| `SPLICE_F_NONBLOCK` | Equivalent | | `kqring_exec_splice` | `test_splice_and_tee` |
| `SPLICE_F_MOVE` | Equivalent | A hint, accepted and ignored, as are `SPLICE_F_MORE` and `SPLICE_F_GIFT`. | `kqring_req_prep` | `test_compat_contracts` |
| `SOCKET_URING_OP_SIOCINQ` | Equivalent | | `kqring_exec_uring_cmd` | `test_uring_cmd` |
| `SOCKET_URING_OP_SIOCOUTQ` | Equivalent | | `kqring_exec_uring_cmd` | `test_uring_cmd` |
| `SOCKET_URING_OP_GETSOCKOPT` | Equivalent | | `kqring_exec_uring_cmd` | `test_uring_cmd`, `liburing/socket-getsetsock-cmd` |
| `SOCKET_URING_OP_SETSOCKOPT` | Equivalent | | `kqring_exec_uring_cmd` | `test_uring_cmd`, `liburing/socket-getsetsock-cmd` |
| `SOCKET_URING_OP_TX_TIMESTAMP` | Unsupported | `-EOPNOTSUPP`. | `kqring_exec_uring_cmd` | `test_compat_contracts` |
| `SOCKET_URING_OP_GETSOCKNAME` | Unsupported | `-EOPNOTSUPP`. | `kqring_exec_uring_cmd` | `test_compat_contracts` |
| `IO_URING_NAPI_REGISTER_OP` | Partial | See `IORING_REGISTER_NAPI`. | `kqring_register_napi` | `test_napi_busy_poll` |
| `IO_URING_NAPI_STATIC_ADD_ID` | Unsupported | `EOPNOTSUPP`, as is `IO_URING_NAPI_STATIC_DEL_ID`. | `kqring_register_napi` | `test_napi_busy_poll` |

## Boundaries

Behaviour that no single flag covers. Each row is a contract, and [Differences from Linux](README.md#differences-from-linux) has the detail.

| Boundary | Status | Contract | Source | Tests |
|---|---|---|---|---|
| One process per ring | Different | Only the owning process submits, waits or registers. A forked child gets `EEXIST`, except for `IORING_ENTER_SQ_WAKEUP`. | `kqring_is_owner` | `test_phase5_security_audit`, `liburing/across-fork` |
| Rings inherited across `fork(2)` | Different | The child inherits the descriptor but can't use the ring. `NO_MMAP` ring memory is shared. | `kqring_is_owner` | `test_worker_process_lifecycle`, `test_no_mmap` |
| Teardown across `execve(2)` | Different | Rings are torn down and in-flight requests are freed with no CQE. The descriptor then gives `EBADF`. Linux keeps the ring usable. | `kqring_proc_exec` | `test_phase5_security_audit`, `test_exec_kthreads` |
| Worker cancellation and CQE order | Different | The cancel's CQE can come first. Wait for the target's own CQE before freeing its buffers. | `kqring_cancel` | `test_worker_cancel_and_timeout`, `test_sync_cancel_worker` |
| Timeout CQE order | Different | As cancellation, for `LINK_TIMEOUT`. | `kqring_ltimeout_arm` | `test_worker_cancel_and_timeout` |
| Fixed buffers after unmapping | Different | `-EFAULT`, because I/O goes through the mapping. Linux's pinned pages stay valid. | `kqring_exec_file_rw` | `test_registered_buffers` |
| Provided-buffer reservations | Different | See `IOSQE_BUFFER_SELECT`. | `kqring_buf_select` | `test_worker_buffer_reservation` |
| Provided buffers at end of file | Different | No buffer is consumed. | `kqring_buf_commit` | `test_multishot_and_eof` |
| Provided-buffer group removal | Different | `EBUSY` during a reservation. | `kqring_remove_buffers` | `test_worker_buffer_reservation` |
| Cross-ring messages | Different | Only the target's owner path writes its CQ. See `IORING_OP_MSG_RING`. | `kqring_msg_ring` | `test_msg_ring`, `test_send_fd` |
| Error reporting | Different | Linux's `EBADFD` is `EBADF`, and `EBADR` is `EOVERFLOW`. | `kqring_ioc_enter_body` | `test_read_multishot`, `gap` |
| Eventfd notification | Partial | See `IORING_REGISTER_EVENTFD`. | `kqring_evfd_flush` | `test_eventfd` |
| Poll timing | Partial | See `IORING_OP_POLL_ADD`. | `kqring_kp_kick` | `test_poll_before_return`, `test_poll_same_thread` |
| Ring descriptors over UNIX sockets | Different | `SCM_RIGHTS` fails with `EOPNOTSUPP`, where Linux 6.7 and later give `EINVAL`. | `kqring_fileops` | `test_msg_ops`, `test_phase5_security_audit` |
| Resource limits | Different | Ring memory is limited per user by `kern.kqring.user_ring_mem`, and registered buffers by `RLIMIT_MEMLOCK` and `vm.max_user_wired`. | `kqring_bufs_register` | `test_send_zc`, `test_buffer_groups`, `gap` |
| Capsicum rights | Different | Fixed files keep their rights, and every request is checked against them. | `kqring_op_fget_caps` | `test_compat_contracts` |
| Capability mode | Different | A personality registered before `cap_enter(2)` is refused afterwards. | `kqring_req_issue_cred` | `test_personality` |
| `SIGPIPE` | Different | Never raised. | `kqring_exec_send` | `test_compat_contracts` |
| Module unload | Different | `kldunload` fails with `EBUSY` while a ring is open. | `kqring_modevent` | `test_send_zc` |

## Validation gaps

Everything marked `gap` above, plus limits of the tests themselves:

- **A dropped completion.** Enters report one with `EOVERFLOW`, but the kernel keeps room for every completion still to come, so no test can make one happen.
- **`vm.max_user_wired`.** Only `RLIMIT_MEMLOCK` is exercised.
- **Features not advertised.** `IORING_FEAT_RW_ATTR` is simply not set.
- **Suspend.** No test suspends the machine, so how an `IORING_ENTER_ABS_TIMER` deadline or a wait's timeout behaves across a suspend is unchecked. `CLOCK_BOOTTIME` and `CLOCK_MONOTONIC` are the same clock on FreeBSD, so they can't differ there.
- **`liburing/pipe-reuse` and `liburing/splice`** stop early, so the subtests after their first failure haven't run.

`Equivalent` means no difference is known. Only the behaviour in [`tests/kqring_diff.c`](tests/kqring_diff.c) has been compared with a running Linux kernel. Some rows also rest on a test that covers more than the row names.
