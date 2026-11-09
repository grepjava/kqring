# Design note: ZFS reads that complete without a worker

**Status:** research, not implemented. Listed under [Research](../ROADMAP.MD#research) in the roadmap. Line references are to OpenZFS as shipped in FreeBSD 15.1 (`sys/contrib/openzfs`).

## Why

kqring has two ways to run a read of a ZFS file, and both have a cost that `pread(2)` and Linux don't:

- **On a worker.** Every read crosses to a worker thread and back as a CQE. For a read the ARC serves in a microsecond or two, the hand-off is most of the cost. On an 8-thread Ryzen, fio's cached reads through workers stay near 1.1 million IOPS from 2 jobs on, where `psync` reaches 1.6 million ([worker-wakeups.md](worker-wakeups.md)).
- **In the submitting thread.** Since 7a34dcc, while a ring's recent reads didn't sleep and the CPUs are taken, reads run in `io_uring_enter(2)` itself. That reached 1.46 million at 8 jobs. But it's a guess from history: ZFS can't say whether a read will wait for the disk, so one that misses the ARC holds up the enter until the disk answers, and only after a few such misses do the ring's reads go back to workers.

Linux has neither cost for the page cache. It tries a read in the submitting thread with `IOCB_NOWAIT`, which fails with `EAGAIN` rather than block, and only those go to io-wq. A ZFS read that could do the same, and better, finish from ZFS's own I/O completion instead of a worker, would remove both.

The same gap affects FreeBSD outside kqring. `sendfile(2)` has been asynchronous since FreeBSD 11: it starts the page-ins with `VOP_GETPAGES_ASYNC` and sends the data from the I/O's completion, so a web server's thread never waits for the disk. ZFS doesn't implement `VOP_GETPAGES_ASYNC`, so it gets `vop_stdgetpages_async()` (`sys/kern/vfs_default.c`), which calls the synchronous `VOP_GETPAGES` and then the completion. **`sendfile(2)` from a ZFS file blocks its caller on every ARC miss.** An asynchronous ZFS read path would fix that for every `sendfile(2)` user, not only for kqring.

## What ZFS already does

The bottom of ZFS's read path is already asynchronous. `zfs_read()` (`module/zfs/zfs_vnops.c`) takes the range lock as a reader and calls `dmu_read_uio_dbuf()`. That calls `dmu_buf_hold_array_by_dnode()` (`module/zfs/dmu.c`), which:

1. creates a root zio, `zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL)`;
2. holds each block's dbuf (`dbuf_hold()`), and calls `dbuf_read(db, zio, ... DB_RF_NEVERWAIT ...)` on it. On an ARC hit the dbuf is `DB_CACHED` at once. On a miss, `dbuf_read()` starts an ARC read as a child of the root zio and returns without waiting;
3. **waits**: `zio_wait(zio)` for its own reads, then `cv_wait(&db->db_changed)` on any dbuf still `DB_READ` or `DB_FILL`, which another thread is reading or filling;
4. returns the held dbufs, and `dmu_read_uio_dnode()` copies from them to the caller's memory.

Only step 3 waits. A root zio can take a completion callback (`zio_root(spa, done, private, flags)`) and be started with `zio_nowait()`, so ZFS can already tell its caller that a read finished. What it has no interface for is a caller that wants that.

## The proposal, in two steps

### Step 1: a read that doesn't wait (`IOCB_NOWAIT` for ZFS)

A flag on the read, say `IO_NOWAIT` in the VFS `ioflag` and a matching DMU flag, meaning: if any part of this read would wait, return `EWOULDBLOCK` without having read anything.

- In `dmu_buf_hold_array_by_dnode()`, check each dbuf after `dbuf_read()`. If any isn't `DB_CACHED`, release them all and fail. The ARC reads already started finish on their own and warm the cache, as prefetch does, so the retry on a worker is likely a hit.
- Fail as well, before waiting, anywhere else the path would sleep (next section).

kqring would then run every read without `IOSQE_ASYNC` in the enter with the flag, and hand only the `EWOULDBLOCK` ones to workers, as Linux does. The `cache_avg` guess goes. A miss never holds up the enter, and a hit never pays the hand-off. Whether to run reads in the enter at all while CPUs are idle stays a policy question: with CPUs to spare, workers read one ring's requests in parallel, which one submitting thread can't.

This step is small. It touches one DMU function and `zfs_read()`, changes nothing for callers without the flag, and is useful on its own: `aio(4)`, or a future `RWF_NOWAIT` for `preadv2`-style calls, could use it too.

### Step 2: a read that completes on its own

A DMU call that starts the read and returns, calling back when the data is there:

```c
/*
 * Returns 0 with the data copied, if every block was cached;
 * EINPROGRESS if done(arg, error) will be called once it is;
 * an error otherwise.
 */
int dmu_read_async(dnode_t *dn, uint64_t off, uint64_t len,
    struct dmu_read_dest *dst, dmu_flags_t flags,
    void (*done)(void *arg, int error), void *arg);
```

It would do steps 1 and 2 above with `zio_root(spa, dmu_read_async_done, state, ...)`, then `zio_nowait()` instead of `zio_wait()`. The callback runs in a ZIO completion taskq. It copies from the held dbufs into the destination, releases them, and calls `done`.

In the ZPL, a `zfs_read_async()` would wrap it, holding the range lock and the `zfs_enter()` reference until the callback, and a ZFS `VOP_GETPAGES_ASYNC` would use the same call to fill the vnode's pages, which makes `sendfile(2)` asynchronous on ZFS.

In kqring the read would complete as reads of a disk device already do (`kqring_bio_done()`, `kmod/kqring_file.c`): the user's pages are held at issue with `vm_fault_quick_hold_pages()`, the completion copies into them through the direct map and posts the CQE, and no worker runs.

## Where the path still waits

Each of these needs either a non-waiting form or, in step 1, a fail-fast with `EWOULDBLOCK` (the read then goes to a worker as it does today):

| Wait | Where | First answer |
|---|---|---|
| Indirect block not in the ARC | `dbuf_hold()` → `dbuf_findbp()` reads the parent with `dbuf_read(parent, NULL, ...)`, synchronously (`module/zfs/dbuf.c`) | Fail fast. In a cached workload the indirect blocks are hotter than the data |
| Block another thread is reading | `cv_wait(&db->db_changed)` in `dbuf_read()` and in `dmu_buf_hold_array_by_dnode()` | Fail fast in step 1. Step 2 needs a waiter list on the dbuf, or an ARC read with a callback, which `arc_read()` chains onto the read already in flight for that block |
| A writer holds the range | `zfs_rangelock_enter()` in `zfs_read()` | `zfs_rangelock_tryenter()`, which `zfs_getpages()` already uses |
| Parent dbuf's lock | `dmu_buf_lock_parent()` in `dbuf_read()` | A reader lock held briefly. Measure before changing |
| `sync=always`, or `FRSYNC` | `zil_commit()` in `zfs_read()` | Fail fast |
| Pages of the file are mapped | `zn_has_cached_data()` → `mappedread()` | Fail fast. Mapped files are rare among what's read this way |
| Compressed or encrypted block | `arc_untransform()` in `dbuf_read()` | CPU work, not a wait. Allowed, as `pread(2)` does it too |
| Dataset rollback or unmount | `zfs_enter()`'s teardown lock | Taken as a reader and held until the callback, as a synchronous read holds it across its disk wait |

## Lifetime and cancellation

- **Holds.** Between issue and callback the read holds the dbufs, the range lock, the `zfs_enter()` reference and the vnode. That's what a synchronous read holds while it sleeps, just without a thread. Unmount and rollback wait for it as they wait for a sleeping reader.
- **Close and exit.** The ring can close and the process can exit while a read is in flight. As with kqring's disk bios, the request is detached rather than cancelled: the callback still runs, finds the request orphaned, releases what it holds and frees it. A zio can't be cancelled, so `ASYNC_CANCEL` gets `-EALREADY`, as for running block I/O on Linux.
- **User memory.** The callback never touches the process's address space. It copies into pages held at issue, so a buffer unmapped mid-read stays valid until the copy, as with the disk path.
- **Errors.** A checksum failure ZFS can't repair completes the read with `-EIO`, as `zfs_read()` would. The `O_DIRECT` checksum retry stays on the synchronous path.

## How it would be judged

- **Step 1:** fio's cached reads with the in-enter policy and no history guess match 7a34dcc's numbers, and a hit-and-miss mix never has a read held up behind a miss in the enter (p99 of the hits stays at hit latency). At 1 to 4 jobs there's no regression.
- **Step 2:** SSD reads at 1 and 32 in flight with no worker running (`wq_inline` and the worker counters at 0), the same or better IOPS and p99 than through workers, and less CPU per read. `sendfile(2)` of an uncached ZFS file from one thread to many sockets no longer serializes on the disk.
- In both: the suite, fuzzer and stress test on a `GENERIC-DEBUG` kernel, plus ZFS's own test suite for anything changed in ZFS.

## Open questions

- **Where it lives.** Steps 1 and 2 change OpenZFS, which FreeBSD imports from upstream. They would go to OpenZFS first, as a cross-platform DMU interface that Linux's `IOCB_NOWAIT` support could use too. A kqring-only patch to ZFS isn't worth carrying.
- **The VFS interface.** Step 1 fits `VOP_READ` with a new `ioflag`. Step 2 needs a new VOP, or `VOP_GETPAGES_ASYNC` with the data passing through the page cache, which ZFS keeps apart from the ARC and would then hold twice. Which one FreeBSD would take is for its VFS maintainers.
- **What step 2 saves over step 1.** With step 1, a miss goes to a worker that sleeps for the disk, and a sleeping worker costs little. Step 2 mainly saves the threads and switches of many misses in flight at once, which matters most for `sendfile(2)`. It should be measured before it's built.
