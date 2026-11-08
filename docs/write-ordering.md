# Design: per-file write ordering on the worker pool

**Status:** implemented in `kmod/kqring_wq.c`, phase 4 of the [roadmap](../ROADMAP.MD#phase-4-per-file-write-ordering).

A write to a regular file goes to kqring's worker pool, because FreeBSD filesystems have no asynchronous write entry point. Two writes to one file must land in the order they were issued, or `O_APPEND` and file-position writes interleave wrongly and a later write to the same range can be overwritten by an earlier one. Linux keeps that order in io-wq by hashing buffered writes by inode: io-wq runs the writes in one hash bucket one at a time, in order, and the writes in other buckets in parallel.

## What kqring did before

The issuer, the thread in `io_uring_enter(2)`, waited for each such write to finish before it issued the next request. That kept the order, but at a price:

- The rest of the submission waited too. A small write to one file sat behind a slow write to another, and so did every request after it.
- Writes to different files could never run at once.

## The design

It follows io-wq.

**Which writes are ordered.** Every write to a regular file (`IORING_OP_WRITE`, `WRITEV` and the `_FIXED` forms) except an `O_DIRECT` write at an offset of its own without `RWF_*` flags, which the filesystem can run in parallel, as io-wq leaves `O_DIRECT` writes unhashed. Pipes and sockets aren't ordered this way: a write that waits for a reader must not hold back the files that share its bucket, and Linux doesn't hash them either. `kqring_op_wq_order()` decides, when the request is queued.

**Buckets.** An ordered write gets a bucket from its vnode pointer, one of 64 (`KQRING_WQ_HASHES`), as io-wq hashes the inode into 64. Files that share a bucket share its order, which costs parallelism but never correctness.

**The queue.** Each worker type has one FIFO queue. A new ordered write goes in just after its bucket's last queued write (`hash_tail`), so each bucket's writes are next to each other, as io-wq keeps them. While a worker runs one write of a bucket (`hash_busy`, a 64-bit mask), the other workers pass over the rest of that bucket in one step, from its first queued write to `TAILQ_NEXT(hash_tail)`, and take the next request they may run (`kqring_wq_next_locked()`). When the write finishes, the bucket is free again, and the worker that ran it takes the next one, or wakes another if it is leaving.

**The issuer.** It no longer waits. An ordered write is posted by its worker, as a read on a worker already was. The only wait left is the short spin a program in lockstep gets, with one request in flight and nothing else to do. A bucket with nothing else queued can't be overtaken then.

**Cancellation.** Once writes no longer completed before the next submission, a cancel could find one running. kqring used to report such a request as cancelled when its worker returned, although its write had happened, so a program told `-ECANCELED` might write again. A regular file's read or write that a worker has started or finished is now left alone, as io-wq leaves running work: it completes with its own result, and the `ASYNC_CANCEL`, `LINK_TIMEOUT` or trigger cancel that found it gets `-EALREADY` (`kqring_op_cancel_busy_locked()`). One still queued is taken off the queue under the pool's lock before it is cancelled, so no worker can start it in between. A synchronous cancel waits for such I/O, as it waits for disk bios.

## Differences from Linux

- **The order holds within a worker pool.** io-wq belongs to a thread, so a thread's rings share one. kqring's pool belongs to a ring, unless rings share one with `IORING_SETUP_ATTACH_WQ`. Two rings writing one file without sharing a pool can interleave, which io_uring doesn't promise against either.
- **Requests that aren't writes still make the issuer wait.** `FSYNC`, `FALLOCATE` and other requests that go to workers are unchanged. An `FSYNC` that must follow writes has to be linked to them, as on Linux, where io-wq doesn't hash `FSYNC`.

## What shows it works

- `test_write_order` in `tests/kqring_test.c`:
  - Two files' writes, alternating in one submission, mixing `IOSQE_ASYNC`, `O_APPEND`, the file position and offsets.
  - A burst cancelled while it runs.
  - The ring closed with writes out.
  - A small write to one file that finishes before a 32 MiB `O_SYNC` write to another, submitted first.
- `tests/kqring_fuzz.c`: each thread appends numbered records in bursts to a file of its own, with the fuzzer's cancellations, closes, forks and descriptor changes going on around them. Once every ring is gone, each ring's records must be whole and in the order it submitted them.

Every order failure the check reported while it was being built came from the fuzzer, not this code:
- `MIN()` drew its random batch size twice, so a batch could be larger than the SQ's free space and overwrite SQEs the kernel hadn't read yet.
- The fuzzer sometimes gives the SQ array a random entry, which runs whatever SQE is in that slot. Ordered writes now skip the rings where it does that.
- The fuzzer closes and writes to the descriptors its requests create, which it reads from CQEs, and a CQE it takes for the wrong request's can name any descriptor. At a small number, a thread's file of records was among them, so other requests wrote into it. The files now sit at descriptor numbers the fuzzer never takes as a request's, as its log does.

## Measured

The test machine is the one in [BENCHMARK.MD](../BENCHMARK.MD): an i5-8250U and a SATA SSD under ZFS. Each case was run with fio 3.43, random 4 KiB writes for 20 seconds, three times, with the module before this change (`b43f479`) and after it alternating within each repetition. The figures are medians, and the spread is between the runs of one case.

| Case | In flight | Before | After | aio | Spread |
|---|---:|---:|---:|---:|---:|
| Buffered, one 8 GiB file | 1 | 8,584 | 8,521 | 9,103 | 18–21% |
| Buffered, one 8 GiB file | 32 | 10,048 | 10,470 | 10,265 | 8–15% |
| Buffered, four 1 GiB files | 32 | 54,891 | 51,074 | 54,896 | 21–31% |
| `O_SYNC`, one file | 32 | 2,683 | 2,566 | 2,665 | 12–17% |
| `O_SYNC`, four files, one dataset | 32 | 3,728 | 3,578 | 3,933 | 4–14% |
| `O_SYNC`, four files, four datasets | 32 | 2,106 | 2,289 | 2,796 | 8–27% |

Throughput is unchanged on this machine, and so is CPU: about 15% at 32 in flight, buffered, before and after. Buffered writes to ZFS return once the data is in memory, so running them one at a time cost little, and ZFS's write throttle sets the pace. `O_SYNC` writes are limited by how many synchronous writes the SSD completes, about 2,000–3,000 a second, whichever way they are issued.

What changed is latency when files differ: a small write to one file no longer waits for a slow one to another. With a 32 MiB `O_SYNC` write to one file submitted first, the small write completed second in 5 of 5 rounds before the change and first in 5 of 5 after it. A faster device, or files on more than one disk, would show the parallelism in throughput too; this machine has one SATA SSD.
