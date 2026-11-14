# Cached file reads

Status: implemented, November 2026. Roadmap phase 11.

On the i5 laptop of [linux-comparison.md](linux-comparison.md), fio's 4 KiB random reads of a file on tmpfs, 4 jobs at 32 in flight, reached 34% of `pread(2)`'s rate on kqring and 58% on Linux's io_uring. Linux reads a page that is cached in the thread that submitted it, and only hands a read to io-wq when it would block. This note finds why kqring didn't, and makes it.

## How it is measured

fio 3.43, `--rw=randread --bs=4k --direct=0 --norandommap --randrepeat=0`, over a 256 MiB file written beforehand, on tmpfs mounted with `size=300m`, 10 seconds after 1 of ramp, `--thread`. `psync` is `pread(2)`, one at a time per job. Medians of three rounds; Ryzen 3 PRO 4350G ([BENCHMARK.MD](../BENCHMARK.MD)), FreeBSD 15.1 `GENERIC`, unless they say the i5.

## Why tmpfs reads went to workers

kqring already reads a cached page in the enter (`kqring_file_rw_inline()`, since phase 5), with the filesystem's `VOP_READ_PGCACHE`: it grabs resident pages without sleeping and gives up otherwise, the way `vn_read()` tries it before `VOP_READ`. A vnode offers it only with `VIRF_PGREAD` set, and tmpfs sets that only on a mount with `-o pgread`, which is off by default. Without it, every read went to a worker unless the CPUs looked busy (`kqring_wq_inline_ok()`).

DTrace on the 4-job, 32-deep run: on a default mount, `kqring_file_rw_inline()` handed all of 6,340,602 reads in 4 seconds to a worker; with `-o pgread`, it completed all of 8,734,054 itself.

| IOPS | `psync`, 4 jobs | kqring, 4 jobs, 32 deep | 1 job, 32 deep | 1 job, 1 deep | `psync`, 1 job |
|---|---:|---:|---:|---:|---:|
| tmpfs, default | 3,042,101 | 1,610,545 (53%) | 920,342 | 577,262 | 1,091,230 |
| tmpfs, `-o pgread` | 3,068,860 | 2,683,234 (87%) | 790,314 | 800,927 | 1,093,085 |

Read in the enter, 4 jobs went from 53% of `psync` to 87%. One job 32 deep is the exception: workers are faster there (920,342 against 790,314), but only by keeping about three CPUs busy (82,167 kernel samples in 8 seconds against 27,132), where the read in the enter uses one. Linux makes the same choice.

`tmpfs_read_pgcache()` isn't quite non-blocking, either: a page that was swapped out is paged back in (`uiomove_object()` grabs it with `VM_ALLOC_NORMAL`), which sleeps, in the submitter's enter.

## kqring's own tmpfs read

`kqring_tmpfs_read()` is `vn_read_from_obj()` for tmpfs, and needs no mount option:

- The node, its swap object and its size (`tn_size`) are read under SMR, as `tmpfs_read_pgcache()` reads them. vm_objects are type-stable, so the object is held (`vm_object_pip_add()`) after leaving SMR and checked for `OBJ_DEAD` once held.
- Pages are grabbed one at a time with `VM_ALLOC_NOCREAT | VM_ALLOC_NOWAIT`, shared-busied, and copied with `uiomove_fromphys()`; a page that isn't resident and valid ends the copy, and the read goes to a worker, which redoes it whole. A busied page keeps `vgonel()` from terminating the object past it, so the vnode is checked for doom after each grab.
- The range lock the enter already takes keeps the file from being truncated under the copy, and the copy runs with page faults disabled, as before: a destination that isn't mapped goes to the worker.

A tmpfs vnode is recognised by its operations vector (`tmpfs_vnodeop_entries`, or the `nonc` one); tmpfs is part of `GENERIC`. Other filesystems still need `VIRF_PGREAD`.

## Results

On the Ryzen, with the new module: DTrace counted 2.4 to 8.7 million `kqring_tmpfs_read()` calls in 4 seconds of each of 1 and 4 jobs at 1 and 32 in flight on a default mount, every one complete, and no read sent to a worker.

| IOPS | `psync`, 4 jobs | kqring, 4 jobs, 32 deep | 1 job, 32 deep | 1 job, 1 deep | `psync`, 1 job |
|---|---:|---:|---:|---:|---:|
| tmpfs, default, before | 3,042,101 | 1,610,545 (53%) | 920,342 | 577,262 | 1,091,230 |
| tmpfs, default, now | 3,022,044 | 2,716,440 (90%) | 802,447 | 805,921 | 1,086,735 |
| tmpfs, `-o pgread`, now | 3,114,081 | 2,692,998 (86%) | 805,076 | 811,900 | 1,108,930 |

A default mount now reads as fast as a `pgread` one. One job 32 deep is 13% slower than on workers, at a third of their CPU, as above.

## On the machine that runs both

The i5-8250U boots FreeBSD 15.1 and Ubuntu's Linux 7.3.0-10, with the same fio build; same cases, three rounds each, the two FreeBSD modules alternated on one boot. In brackets, each ring against its own system's `psync` at the same jobs. The raw outputs are in [bench/cached/](bench/cached/).

| IOPS | `psync`, 4 jobs | ring, 4 jobs, 32 deep | 1 job, 32 deep | 1 job, 1 deep | `psync`, 1 job |
|---|---:|---:|---:|---:|---:|
| Linux io_uring | 2,003,246 | 1,338,926 (0.67) | 593,419 (1.05) | 140,974 (0.25) | 562,811 |
| kqring, phase 10 | 2,270,518 | 891,966 (0.39) | 521,652 (0.77) | 389,777 (0.58) | 674,389 |
| kqring, now | 2,242,347 | **1,790,144 (0.80)** | 562,661 (0.83) | 574,814 (0.85) | 674,808 |

With 4 jobs 32 deep, kqring now reads 0.80 of `psync` where io_uring reads 0.67, and 1.34 times as many pages a second. One job 32 deep, io_uring is 5% ahead: Linux's `pread(2)` costs more than FreeBSD's, so the enter it saves is worth more there.

## Where the time of one read goes

One job, `-o pgread`, a kernel and user profile of fio's threads (`profile-4999`, 8 seconds). Both keep one CPU busy, so samples per read are time per read:

| ns per read | `pread(2)` | kqring, 32 deep |
|---|---:|---:|
| all | 935 | 1,270 |
| the copy (`copyout`) | 330 | 392 |
| finding the page (`pctrie`) | 97 | 107 |
| the rest of the kernel | 180 | 363 |
| fio, in user space | 327 | 405 |

kqring's own functions take 190 ns of each read, the whole of the difference in the kernel, which is why one job reads less than `psync` does: it is what is left to cut, with the enter's own cost ([per-request-cost.md](per-request-cost.md)). 32 deep costs the same as 1 deep: fio's engine enters once per read, so each pays an enter. The copy costs more as 32 destination buffers in flight miss the cache more often than one.

## A FreeBSD hang this found

`read(2)` of a tmpfs file into a writable `MAP_SHARED` mapping of the same page never returns, on FreeBSD 15.1 without kqring. tmpfs doesn't set `MNTK_NO_IOPF`, so `vn_io_fault()` doesn't hold the destination's pages first; `tmpfs_read()` busies the file's page, and the copy faults on its mapping, which waits for the page to be unbusied: by itself. The wait can't be interrupted, and the machine can't finish a shutdown. ZFS and UFS hold the pages first and return. kqring's read in the enter gives up with `EFAULT` (faults are disabled), and the worker it goes to used to read as `read(2)` does, and hang as it does. Reported as [FreeBSD bug 300734](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300734).

Since phase 15 a worker's read or write of a regular file on a filesystem without `MNTK_NO_IOPF` (tmpfs, and nullfs over it) holds the user's pages first, up to 1 MiB at a time, as `vn_io_fault()` does for UFS and ZFS, and the filesystem copies through their direct-map addresses, which can't fault (`kqring_vn_rw_held()`). The unit test reads and writes a self-mapped tmpfs file, as it does on ZFS and UFS. `read(2)` itself still hangs until FreeBSD fixes it.

## Acceptance

A cached read of a tmpfs file, on a default mount, completes in the enter without a worker and without sleeping. On the machine that runs both, kqring's 4 jobs 32 deep reach at least the share of `psync` that io_uring's reach of Linux's. The unit tests (now also in a tmpfs directory), fio's verified writes and reads, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before.
