# Changelog

## Unreleased

### Added

- Registered buffers on held pages, as Linux pins them. `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED`, `WRITEV_FIXED` and zero-copy sends from a registered buffer go through its pages, held at registration and mapped in the kernel, rather than through its user addresses: any process holding the ring uses any buffer, a buffer still works after `munmap(2)`, a buffer may be up to 1 GiB (was 16 MiB), and a direct disk request gives the bio the held pages instead of holding the user's pages per request. When the registering process forks, its buffers are held again for writing, so that they stay its memory and the child gets copies, as on Linux. A shared file mapping is refused with `-EFAULT`, as on Linux. `tests/kqring_diff.c` gains `buffer_fork` and `buffer_backing`, which now match Linux 7.3. See [docs/held-buffers.md](docs/held-buffers.md).
- Several processes submitting to one ring, as on Linux. On a ring without `SINGLE_ISSUER` or `SQPOLL`, every process that holds it may submit whatever the others have in flight, and its requests run in that process, with its own workers, taskwork and poller threads, descriptors, memory and credentials. When a process exits or execs, its requests complete with CQEs, `-ECANCELED` for those it hadn't finished, its standalone `TIMEOUT`s still fire, and the ring stays open, with its fixed files and buffers, for the others; a shared ring whose descriptor survives an `exec` stays usable by the new image. Before, a ring moved between processes only while idle, the second process got `EEXIST`, an exiting owner's requests ended without CQEs, and `exec` closed the ring. A set-ID `exec` closes the rings the process holds, and kqring's extensions keep a ring to one process. liburing's `test_cancel_req_across_fork`, `test_cancel_inflight_exit` and `test_timeout_link_cancel`, which the port left out, now run and pass. See [docs/concurrent-submitters.md](docs/concurrent-submitters.md).
- [`rocksdb/`](rocksdb/): RocksDB 11.13.2 with its io_uring reads on kqring, with scripts to build it, run its tests that use io_uring, and benchmark `MultiGet` with io_uring on and off. Its tests pass, and on a SATA SSD under ZFS with 4 KiB records `MultiGet` reads 28% more keys per second with io_uring than with `pread(2)` at one thread, 17% at four.
- `IORING_REGISTER_QUERY`, with or without a ring: `IO_URING_QUERY_OPCODES` reports the number of request and registration opcodes and the setup, enter and SQE flags and features kqring implements, `IO_URING_QUERY_SCQ` the size and alignment of the rings' header, and `IO_URING_QUERY_ZCRX` `-EOPNOTSUPP`. Answers are cut to the caller's size or zeroed past it, and a chain of more than 1000 queries fails with `ERANGE`, as in Linux. liburing's `ring-query` passes.
- Mixed-size SQEs and CQEs, and `IORING_SETUP_SQ_REWIND`, as in Linux. On an `IORING_SETUP_CQE_MIXED` ring, a CQE with extra words takes two 16-byte slots and has `IORING_CQE_F_32`, and one that would wrap has an `IORING_CQE_F_SKIP` entry before it; `IORING_NOP_CQE32` posts one, and kevent and kqueue-bridge CQEs are 32 bytes there. On an `IORING_SETUP_SQE_MIXED` ring, `IORING_OP_NOP128` and `IORING_OP_URING_CMD128` take two 64-byte slots; they also work on `SQE128` rings. `SQ_REWIND` reads each enter's SQEs from slot 0. Overflowed CQEs keep their size, and `IORING_REGISTER_RESIZE_RINGS` moves both kinds. liburing's `nop32`, `nop32-overflow`, `cq-peek-batch-mixed`, `sqe-mixed-nop`, `sqe-mixed-bad-wrap`, `sqe-mixed-boundary` and `large-resize`, which skipped, now pass.
- Rings used by more than one process, as PostgreSQL 18's `io_method=io_uring` uses them. A ring set up without `SINGLE_ISSUER` or `SQPOLL` moves to another process that holds it, such as a forked child, when that process submits or registers while nothing is in flight and the ring has no registered or provided buffers, personalities, polls or kqring extension state; otherwise it still gets `EEXIST`. Any process holding such a ring may wait on it. When the owner exits, the ring stays open for the others instead of closing. PostgreSQL 18.6 with `io_method=io_uring` passes all 231 of its regression tests on kqring, where every backend's first submission used to fail; [`postgresql/`](postgresql/) builds and checks it. liburing's `across-fork` test passes. See [docs/cross-process-rings.md](docs/cross-process-rings.md).
- `fio/summary.py` turns `fio/bench.sh` and `fio/scale.sh` output into the `fio.csv` that `BENCHMARK.MD` is written from.
- Signal masks in `kqring_enter()`, plain and in `io_uring_getevents_arg`: as with `ppoll(2)`, the thread waits with the mask, and a signal it lets in ends the wait and is handled with the mask still in place. They were refused with `EINVAL`. The mask is a FreeBSD `sigset_t`, 16 bytes.
- `IORING_ENTER_ABS_TIMER`: the wait's timeout is a time of the ring's clock, which `IORING_REGISTER_CLOCK` now chooses. On FreeBSD `CLOCK_BOOTTIME` is `CLOCK_MONOTONIC`, so the two give the same deadlines.
- `IORING_REGISTER_MEM_REGION` and registered waits (`IORING_ENTER_EXT_ARG_REG`): a region of the program's memory or the module's, and waits named by their offset in it, read at each enter without a copy.
- `IORING_ENTER_NO_IOWAIT` and `IORING_FEAT_NO_IOWAIT`. FreeBSD doesn't count time waiting for I/O apart, so the flag changes nothing.
- `io_uring_buf_reg.min_left`, from Linux 7.3: on an `IOU_PBUF_RING_INC` ring, a buffer with less than that many bytes left is used up rather than kept for the next transfer. kqring refused it with `EINVAL` before.

### Changed

- `BENCHMARK.MD` is measured on a Ryzen 3 PRO 4350G, where a system call costs about a third of what it does on the i5-8250U with PTI that earlier editions used, and single requests and fan-in now trail kqueue; it explains why. The laptop's runs are kept in `docs/bench/data-i5-8250u/`.
- `kqring_bench`'s `file` group waits for ZFS to sync the file it wrote before reading it, and prints the dbuf cache's target. Reads of a file just written are fast until the sync, and after it slow by an order of magnitude if the file is bigger than the dbuf cache (1/32 of the ARC target); the sync landed inside the group in some runs, which gave the group's ZFS reads two speeds. `BENCHMARK.MD` explains it and shows both, and `tests/zfs_dbuf` shows it with `pread(2)` alone.
- `fio/bench.sh` waits for the disk to have had no writes for 5 seconds before each read run. Reads that met the flush of the previous ZFS writes stalled for up to 1.3 s, whatever the engine, and made whichever ran first look slower.
- Writes to a regular file no longer make the issuer wait. They run on workers one at a time per file, in the order they were issued, as io-wq's per-inode hashing runs them, and writes to different files run in parallel. Before, the issuer waited for each write, which kept them in order but held up the rest of the submission. A program that relied on an unlinked `FSYNC` or other request coming after its writes must link them, as on Linux.
- File reads that don't sleep, such as ZFS reads the ARC serves, no longer wake a worker for each request. When a pool's file I/O has rarely slept lately, at most `MAX(4, ncpu / 2)` of its workers stay awake and take the queued requests themselves. Reads and writes that wait for the disk, and all writes, are unaffected, and queued work that makes no progress for 1 ms wakes the rest. On a Ryzen 3 PRO 4350G, cached ZFS reads through the pool took 60% fewer thread switches and gained 5%, or 2.5% with fio at one job and nothing at more; the hand-off to a worker is still their limit. See [docs/worker-wakeups.md](docs/worker-wakeups.md). `kern.kqring.wq_capped` and `kern.kqring.wq_stalls` count the caps and the stalls.
- Reads of a file on a local filesystem now run in `io_uring_enter(2)` itself, as `pread(2)` would, while the ring's recent reads didn't sleep and threads in enters take more than half the CPUs. That leaves workers for reads that wait for the disk, and for spare CPUs, where they read in parallel faster than one submitting thread. A read the guess gets wrong, such as a ZFS read that misses the ARC, holds up its enter until the disk answers, and after a few the ring's reads go back to workers; `IOSQE_ASYNC` always means a worker. On a Ryzen 3 PRO 4350G, fio's cached ZFS reads at 8 jobs gained 32%, to 1.46 million IOPS, 10% behind `psync` where they were 32% behind; at 1 to 4 jobs no read runs in the enter and they lose up to 0.6%. Reads that wait for the SSD are unchanged. `kern.kqring.wq_inline` counts the reads run in the enter. See [docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread).

### Fixed

- A `SEND` or `SPLICE` to a datagram socket that the protocol refused after copying the data, such as a `SEND` with a destination on a connected UDP socket (`EISCONN`), completed with the full length as if sent. It now completes with the error, as `sendto(2)` returns it. liburing's `send-zerocopy` reached this case once 32 MiB buffers could be registered, and waited forever for the datagram.
- A ring opened by one process and set up or enabled by another (one that inherited the descriptor, as liburing's `single-issuer` test does) ran its requests on workers in the process that opened it. They now run in the process that set it up or enabled it.
- The liburing port's `<liburing.h>` no longer brings in `<sys/param.h>`. Its `<linux/fs.h>` included `<sys/disk.h>` for two ioctl numbers, and with it `<sys/param.h>`'s `CACHE_LINE_SIZE`, `MIN`, `MAX` and the like, which programs define for themselves: RocksDB didn't compile against it. It now defines the two ioctls itself. liburing's socket tests, which got `<netinet/in.h>` the same way, have it from `test-compat.h`.

- A request in flight when its ring's owner exits no longer completes with `-EINTR` on the ring that outlives it. Exit interrupts the ring's workers in that process before the ring is released, and a read waiting on a pipe then posted its interrupted result, where its CQE should be dropped with the rest of the owner's requests. Of 10 runs of 30 rounds of the cross-process test, 4 failed this way before and none after.
- `IORING_CQE_F_32` in `<sys/kqring.h>` is `1U << 15`, as in Linux; it was `1U << 5`, which is `IORING_CQE_F_SKIP`. The module didn't set it before mixed-size CQEs.

- `SEND_ZC` and `SENDMSG_ZC` with `MSG_WAITALL` on a stream socket are now zero-copy. They copied the whole buffer, so a program asking for complete zero-copy sends, as liburing's `send-zerocopy` example does, got none. A short send now waits for room and goes on zero-copy from where it stopped, with one notification for the request, as in io_uring; it reports `IORING_NOTIF_USAGE_ZC_COPIED` if any part was copied. A send cancelled part-way still gets its notification, which its CQE announces with `IORING_CQE_F_MORE`.
- A kqueue attached with `KQRING_EXT_KQUEUE` now reports an event that isn't `EV_CLEAR` once per enter, as one `kevent(2)` call does. An enter could take the kqueue twice, once for its own look and once for the wakeup from when the kqueue was attached, and post the event twice. In a loop of attach, two enters and a read on a Ryzen 3 PRO 4350G, 106 of 3000 rounds had a second CQE still queued after the read; this made `test_kqueue_bridge_level` fail now and then.
- A cancel or `LINK_TIMEOUT` that finds a regular file's read or write already running on a worker no longer reports it as cancelled. The I/O had happened, so a program told `-ECANCELED` could do it again. It now completes with its own result and the cancel gets `-EALREADY`, as in io_uring.
- Reads of a named FIFO now complete in the order of the data, as reads of a pipe do. They also read as a pipe's do otherwise: one with data ready completes without a worker, and an empty `O_NONBLOCK` FIFO waits for data, as in io_uring, rather than failing with `EAGAIN`. `RWF_NOWAIT` still fails it.

- A poll on another ring now always completes before the enter that fired it returns, on a ring without `DEFER_TASKRUN` or SQPOLL. It used to complete just after, about once in tens of thousands of rounds, when the ring's poller had taken the event and not yet recorded it, or when the ring's completion thread was posting it. Past 8 polling rings in a process, or 32 events on one ring, the rest also had to wait for the poller.

- `FUTEX_WAKE` now wakes waiters in the order they came, where it woke the newest first.
- `IORING_REGISTER_RESTRICTIONS` now accepts an empty set, which denies everything, as on Linux. It used to fail with `EINVAL`.
- `IORING_SQ_TASKRUN` is now set by whatever fires a poll, on a ring with `IORING_SETUP_TASKRUN_FLAG`, and an enter that doesn't wait now posts that poll's CQE. A program that peeked at the CQ straight after firing a poll could find nothing and no sign that it should enter, until the ring's poller had looked.
- `execve(2)` by a ring's owner no longer hangs. A ring with a completions, `SQPOLL` or poller thread left the process stuck until killed, because exec parks those threads and teardown waited for them. Teardown now finishes after the exec.
- A short read or write now breaks its link, as on Linux, and shows its CQE despite `IOSQE_CQE_SKIP_SUCCESS`. So does a short `SPLICE` or `TEE`, or a socket opcode with `MSG_WAITALL`. The link used to go on.
- `ASYNC_CANCEL` with `IORING_ASYNC_CANCEL_ALL` or `IORING_ASYNC_CANCEL_ANY` now returns 0 when nothing matches, as on Linux, rather than `ENOENT`.
- `IORING_POLL_ADD_LEVEL` is now refused with `EINVAL`, as on Linux, which has the flag in its UAPI but doesn't accept it.
- A refused `FILES_UPDATE` now leaves its slot empty, as on Linux, rather than keeping the old file.
- With `IORING_SETUP_SINGLE_ISSUER`, another thread's enter that submits nothing now succeeds, as on Linux, rather than failing with `EEXIST`. A wait by another thread still fails unless the CQ can already satisfy it, or always on a `DEFER_TASKRUN` ring.
- `IORING_SETUP_TASKRUN_FLAG` now needs `COOP_TASKRUN` or `DEFER_TASKRUN`, and `SQPOLL` refuses all three, with `EINVAL`, as on Linux. Programs that set them another way will now fail at setup.
- `io_uring_register(-1, IORING_REGISTER_SEND_MSG_RING, ...)` now sends a message without a ring of one's own, as on Linux 6.13 and later. The other ring-less registrations, the task restrictions and BPF filters, and `IORING_REGISTER_QUERY` are refused with `EOPNOTSUPP`, and the rest with `EINVAL`.
- A provided-buffer read that reaches end of file from an `IOU_PBUF_RING_INC` ring now completes with `IORING_CQE_F_BUFFER` and `IORING_CQE_F_BUF_MORE`, as on Linux: the buffer is still the program's to reuse. Its CQE used to carry no buffer.
- Multishot `RECV` now stops a run at the SQE's `len` bytes, or after 32 completions, and yields to other work, so one busy socket can't starve the others. With `optlen` set it ends after that many bytes in all. Both are as on Linux. It used to read for as long as data came.
- Reads on workers that select from the same buffer group no longer wait a clock tick for each other. Each holds the group's head buffer while it reads, and one that found it held slept a full tick before looking again, so 8 such reads at once took about 5 ms rather than 30 µs. liburing's `buf-ring-stress` took 28 s, close to its 30 s limit, and now takes under a second.
- `IORING_REGISTER_SYNC_CANCEL` with a short timeout no longer fails with `ETIME` when what it cancels is a read waiting for a pipe's data, or anything else whose worker is only waiting. Linux has such a request armed as a poll and cancels it at once; the worker kqring uses for it could take longer to stop than the timeout. liburing's `sync-cancel` failed on this, rarely.
- A multishot poll on the ring's own registered eventfd now ends when the ring's signal for posted CQEs wakes it, and that CQE, without `IORING_CQE_F_MORE`, is its last, as Linux does with `EPOLL_URING_WAKE`. A write by the program still gives a CQE with `F_MORE`. It used to fire on its own signal for ever. A first fix ended it one wakeup later, which posted a CQE too many when another request's CQE woke it; liburing's `evloop` failed on that in about 60% of runs.
- `READ_MULTISHOT` of a file that can't be polled now completes with `EBADF`, where Linux has `EBADFD`, rather than `EOPNOTSUPP`, with or without `IOSQE_ASYNC`.
- A wait that a signal ends now succeeds if there are CQEs to reap, as on Linux, rather than failing with `EINTR`.

### Testing

- Linux 7.3 and kqring now match in 19 of the 25 cases of `tests/kqring_diff.c`, and the other 6 differ on purpose. The results are in `docs/conformance/`.
- `tests/kqring_diff.c` runs the same cases on Linux and on kqring, and `tests/diff_compare.py` fails on a difference that isn't listed with its reason.
- `tests/kqring_test.c` now covers `WRITEV_FIXED`, level-triggered polls, `ACCEPT_POLL_FIRST`, `IORING_OFF_CQ_RING`, how a failed SQE ends a batch, the flags kqring refuses, `SIGPIPE`, hole punching and the Capsicum rights of fixed files.
- `tests/kqring_fuzz.c` now unmaps each ring's SQE array. It left them all mapped, a few thousand more every 10 s, and with tens of thousands each `fork(2)` and page fault slowed until a run could stop making progress.
- `tests/kqring_test.c` checks the order of writes to one file in a submission, with another file's in between, after a cancel and after the ring closes.
- `tests/kqring_fuzz.c` now appends numbered records to a file per thread in bursts of writes, and fails if a ring's records are torn or out of order.
- `tests/kqring_fuzz.c` now submits runs of 64 to 255 reads of its scratch file, long enough that a ring's reads look cached and run in the enter itself, among its cancels, closes and forks. It ran 75,000 reads inline in 5 minutes with 8 threads, where it ran 18 in 10 minutes before. It also no longer reports an ordered write as out of order when an SQE the kernel hadn't taken yet, queued before it with a link, held it back; it clears that link, as it did within a batch.
- `tests/kqring_fuzz.c` no longer prepares more SQEs than the SQ has room for. `MIN()` drew its random batch size twice, so a batch could overwrite SQEs the kernel hadn't consumed. Each file of records is also kept at a descriptor number of its own, which the fuzzer never takes as a request's: at a small number, other requests wrote into it and closed it.
- `tests/kqring_fuzz.c` no longer puts requests that create descriptors in a trigger's chain. A chain runs again on every event, its CQEs come back as the trigger's, so the fuzzer never closed what they made, and a socket that stayed readable filled the descriptor table in minutes.
- `tests/kqring_fuzz.c` no longer closes its own log. It closes the descriptors requests create, taken from their CQEs, and a CQE taken for another request's, whose `user_data` slot had been reused, could name its log; a run then went on silently. It now refuses the numbers of its own descriptors, says when, and fails if its log stops being its log.
- `tests/kqring_fuzz.c` no longer reports an overrun CQ when another thread's new ring took a ring's descriptor number before it was mapped: it mapped that other ring, whose CQ is larger.
- `tests/kqring_test.c` covers signal masks in both forms, absolute deadlines on either clock, and memory regions with registered waits: their checks, offsets, repeated waits, masks, and a region read after `fork(2)`.
- `tests/kqring_diff.c` has cases for signal masks, absolute deadlines, registered waits, and the order in which an enter checks its wait argument and submits.
- `tests/kqring_fuzz.c` now waits with signal masks, absolute deadlines and registered waits, in its own memory or the module's, sometimes with bad arguments.
- `tests/kqring_fuzz.c` now also registers without a ring, uses incremental buffer rings with and without `min_left`, caps multishot `RECV` by `len` and `optlen`, polls the ring's own eventfd and cancels synchronously.
- `tests/kqring_fuzz.c` now really exercises futexes, whose wake masks all failed validation before, and resizes rings with requests in flight.
- liburing 2.9: 179 of 196 tests pass, up from 144 of 168. 28 of the 32 tests that weren't built now are, and the other 4 are listed as not portable. Tests that stopped on portability problems, or on an early failure, before they reached what they test now run, and `regbuf-merge` now really calls kqring: it used to pass by making unrelated FreeBSD system calls. Every adapted test is listed in [`liburing/README.md`](liburing/README.md#adapted-tests).
- The liburing port moves to 2.15: 210 of its 245 tests that build pass, 27 skip and 8 fail, each for a reason given in [`liburing/README.md`](liburing/README.md). Its tests led to `min_left` and to the fixes above to ring-less registration, incremental buffers at end of file, multishot `RECV` limits, polls on a ring's own eventfd and `SYNC_CANCEL`. `liburing/compat.h` no longer turns liburing's 2 MB huge-page request into a FreeBSD alignment that `mmap(2)` refuses.

### Documentation

- `COMPATIBILITY.MD` lists every io_uring capability against Linux 7.3 and liburing 2.9, with its contract, source and tests, and `tests/compat_check.py` checks it against the source.
- `README.md` now explains what kqring is and why it exists, and `ARCHITECTURE.MD` covers kernel threads, where a request goes, disk I/O, read ordering, the extensions and testing, and no longer describes names the code has dropped. Its description of the ring header was wrong: all the indices share one 64-byte cache line.
- Every table in `BENCHMARK.MD` is re-measured on `v0.3.0`, with the raw runs in `docs/bench/data/`, and the charts are redrawn. md(4) disks are now filled first. Reads of unwritten blocks are far cheaper, so 32 reads per enter are about 2.9× `pread(2)`, not 4.5×. `tests/bench_doc.py` writes the document from those runs.

## 0.3.0 — 2026-11-06

Still an **experimental preview**, for test systems only: the same caveats as 0.1.0 apply. This release adds kqring's first extensions beyond io_uring (event-driven chains, kevent requests and the kqueue bridge), runs fio's `io_uring` engine, and fixes what fio on a real SSD and on several rings exposed. The ABI only gains additions; rebuild the module and reload it.

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

- **Reads waiting on one pipe could complete out of order.** One worker could read first and post its CQE second, in about 30% of five-read bursts on a ring with warm workers. CQEs now follow the order of the data, as on Linux, which also ends the rare failure of liburing's `read-write`. A read of a pipe costs about 20 ns more.
- **A read with a provided buffer could fail with `-ENOBUFS` with buffers free.** It happened when a read on a ready pipe arrived while a worker was reading into the same group. The read now goes to a worker.
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
- kqring's own suite has 120 tests, and liburing's results are unchanged at 144 passed with `RLIMIT_MEMLOCK` raised, with all 17 failures explained in [`liburing/README.md`](liburing/README.md).
- The suite, fuzzer and stress tests were run on a KASAN kernel with `kqring.ko` instrumented: 7 full 30-minute fuzz runs, with no report from kqring. A deliberate overread in a test module confirmed that reports reach a module.
- The fuzzer no longer dies with `SIGBUS` now and then. Its requests close other threads' rings on purpose, and a file that took a new ring's descriptor number was mapped in the ring's place, beyond the file's end. It now maps a checked descriptor of its own, and reports where it faulted if it ever does.
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
