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

- The node, its swap object and its size (`tn_size`) are read under SMR, as `tmpfs_read_pgcache()` reads them. vm_objects are type-stable, so the object is held (`vm_object_pip_add()`) after leaving SMR and checked for `OBJ_DEAD` once held. (Since phase 19 the read takes the vnode lock instead, which `read(2)` takes and tmpfs writes respect; see [below](#what-changed).)
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

## Phase 19

A `READ` of a cached page, one per enter, against `pread(2)` of the same page: how much of the difference is kqring's own, and how much of that can go.

### How it is measured

On the i5-8250U under FreeBSD 15.1 (the Ryzen was taken by other work). `kqring_xbench` gained two cases, `file_pread_syscall` and `file_read_ring`, which read 4 KiB at a random page-aligned offset of the file named by `XBENCH_FILE`, here a 256 MiB file on a fresh tmpfs mount. Each comparison alternates the builds in one run, five rounds, with each round's order rotated, and includes a control: the base build with 48 bytes of padding at the start of `kqring_wq.c`'s code, which moves everything after it. `pread(2)` itself moves by up to 20 ns from one run's file to the next, so a read's cost is given as its distance above `pread(2)` in the same round. The step logs are `bench/cached/i5-p19-*.steps`.

### Where the 186 ns went

A kernel profile (`profile-4999`, full stacks, 8 seconds of one thread) of each, in ns per read, before any change:

| ns per read | `pread(2)` | `READ`, `DEFER_TASKRUN` ring |
|---|---:|---:|
| all | 1,202 | 1,402 |
| the copy (`copyout`) | 525 | 464 |
| finding the page (`pctrie`) | 187 | 152 |
| taking and dropping the file | 32 | 55 |
| locking the file for the copy | 21 (vnode lock) | 30 (range lock) + 18 (object held) + 10 (SMR) |
| kqring's own functions (allocating, preparing, issuing, completing and freeing the request, the enter, the CQE) | | about 250 |

The copy and the lookup cost the same or less; the difference is the request's own path. A profile per instruction showed it spread thin: most of it on the first touch of a cache line (the request's 14 lines, the ring's, the SQE and CQE, a context's) that the 4 KiB copy has evicted, with no single function above 35 ns.

### What changed

- **The tmpfs read takes the vnode lock**, shared and without waiting (`LK_NOWAIT`; a busy lock sends the read to a worker), as `read(2)` does on tmpfs, in place of the range lock, the SMR section and the hold on the file's object. A held vnode lock already keeps the node from being reclaimed, truncated or written, so the object needs no hold and the per-page check for a dead vnode goes. This also fixes a torn read: a write to tmpfs takes the vnode lock but not the range lock (tmpfs doesn't set `MNTK_NO_IOPF`, so `vn_write()` doesn't take it), so a `READ` holding only the range lock could see part of a concurrent `write(2)`, which `pread(2)` never does. `test_inline_read_atomic` reads a four-page file while another thread rewrites it with one of two patterns, `pread(2)` first as the control, then `READ`. On tmpfs, with the build before, 5,995 of 20,000 `READ`s, all completed in the enter, held parts of both patterns, the first change at byte 9,087, inside a page; with the new one none of 20,000 do (19,950 in the enter), as none of the `pread(2)`s do. On UFS both pass; on ZFS the writer leaves too few reads in the test's 3 seconds to judge, and it skips. Other filesystems keep `VOP_READ_PGCACHE` under the range lock, which their writes take too.
- **An enter whose submissions already filled the CQ to `min_complete` doesn't enter the wait.** `kqring_wait_cqes()` returned at its first check in that case, through three calls; the result is the same.

Tried and dropped: keeping the worker pool's process in the context, so that the enter's check for taking the ring's lock reads one line less (no measurable change); clearing fewer of a request's fields when it is reused. A build that cleared only the six a `READ` or `NOP` writes, for measurement only, made a `NOP` per enter 16 ns faster and a `READ` no faster: with the copy evicting the request's lines anyway, the clearing's stores only moved the misses to the next touch.

### Results

The final comparison, five rounds, of the build before the poll-teardown fix that validation found (it changes only how a poller ends); in brackets against the build before:

| i5-8250U | HEAD (6da505b) | phase 19 | phase 19, padded |
|---|---:|---:|---:|
| `READ` above `pread(2)`, default ring | 196 ns | 180 ns | 180 ns |
| `READ` above `pread(2)`, `DEFER_TASKRUN` | 184 ns | 174 ns | 171 ns |
| one `NOP` per enter, default / `DEFER_TASKRUN` | 358 / 347 ns | 349 / 334 ns (−2.6 / −3.7%) | 348 / 332 ns |
| fio, 1 job, 1 deep, against `psync` | 0.849 | 0.852 | 0.854 |
| fio, 4 jobs, 32 deep, IOPS | 1,798,470 | 1,903,650 (+5.8%) | 1,932,740 (+7.5%) |

In earlier runs of the same builds, with other files, the tmpfs lock alone took the `READ` from 180 to 154 ns above `pread(2)` on a default ring and from 163 to 145 on a `DEFER_TASKRUN` one, and fio's 4 jobs from 1.81 to 1.98 million (+9.4%). Every other `kqring_xbench` row, on both kinds of ring, is within 1.3% of before, most of them faster.

### What stops 100 ns

The acceptance asked for at most 100 ns above `pread(2)`, or what stops it measured; it stays at 170–180 ns. What is left is a request's machinery, which `pread(2)` doesn't have, spread over its path a few ns at a time, and the cache lines it touches. In a profile per source line of the phase 19 build, the largest pieces are: clearing a reused request, 23 ns (12 in a `NOP` loop, where no copy evicts its lines); the references to the file and to the ring's own descriptor, taken and dropped once per read and once per enter, 15; copying the SQE, 8; taking the request off the free list, 7; the enter's check for taking the ring's lock, 7–9 (a chase through three structures). Each is a line or two of the request or the ring that the copy pushed out. The ring's descriptor is one Linux avoids: its `fdget()` takes no reference in a single-threaded process, and a registered ring takes none at all, where kqring's registered rings still do, because the table is per process and another thread may unregister. fio's one job stays at 0.85 of `psync`, against the 0.90 asked: it enters once per read too, and pays the same.

## Acceptance

A cached read of a tmpfs file, on a default mount, completes in the enter without a worker and without sleeping. On the machine that runs both, kqring's 4 jobs 32 deep reach at least the share of `psync` that io_uring's reach of Linux's. The unit tests (now also in a tmpfs directory), fio's verified writes and reads, the cross-process test, stress, the fuzzer, liburing and the comparison with Linux 7.3 pass as before.
