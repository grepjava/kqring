# Design: fewer worker wake-ups for work that doesn't sleep

**Status:** implemented in `kmod/kqring_wq.c`, phase 5 of the [roadmap](../ROADMAP.MD#phase-5-worker-path-scalability).

On Linux, a read that the page cache can serve completes in the thread that submitted it: io_uring tries it with `IOCB_NOWAIT` first, and io-wq only sees reads that would block. FreeBSD has no such path for ZFS, whose cache is the ARC, nor for `O_DIRECT` files, so kqring hands every one of those reads to a worker thread, cached or not. For a read that waits for the disk, the hand-off costs little next to the wait. For a read the ARC serves in a microsecond or two, the hand-off is most of the cost.

## The profile

Cached 4 KiB random reads of a 512 MiB ZFS file, 16 in flight per ring, on the machine in [BENCHMARK.MD](../BENCHMARK.MD) (i5-8250U, 4 cores, 8 threads), measured with fio, `getrusage(2)` and kernel stacks sampled by DTrace:

- **One ring** stopped near 490,000 IOPS, with its submitting thread using a whole CPU. The issuer, which copies SQEs, queues requests and posts nothing itself, is the limit there.
- **Four or more rings** used every CPU, at about 16 µs of CPU per read against about 6 µs for `pread(2)` in as many threads. The difference was thread switching: about 1.6 switches per read, because the pool woke a sleeping worker for each request it queued, even when workers already awake would have taken it a moment later.
- ZFS's own locking (the `dn_dbufs_mtx` of the file's dnode) contended about as much under `psync` as under kqring, so it isn't what separated them.

## The design

**Count what sleeps.** A bound worker (the kind that runs file I/O) notes for each request whether its thread slept while running it, by comparing the thread's voluntary context switch count (`td_ru.ru_nvcsw`) before and after. A read the ARC serves doesn't sleep; one that goes to the disk does. The pool keeps a running average out of 1,024 (`sleep_avg`). It falls by 1/64 for a request that didn't sleep and rises by 1/4 of the remaining distance for one that did, so about 90 non-sleeping requests in a row are needed to cap the pool and two or three sleeping ones end it. A mix of hits and misses doesn't cap by chance.

**Writes count as sleeping.** A buffered write to ZFS usually returns at once, but the filesystem's write throttle can stop every writer the next moment. Capped workers would then hold the queue back. An early version capped and uncapped thousands of times a run on random writes and lost 19% of their throughput on the second test machine; counting every write as one that slept removed it.

**Cap the awake workers.** Below 256 the pool caps; above 512 it stops (`KQRING_WQ_CPU_ENTER`, `KQRING_WQ_CPU_LEAVE`). While capped, at most `MAX(4, ncpu / 2)` bound workers are awake, running a request or spinning for one. A worker that finishes takes the next queued request itself. Queuing a request wakes a sleeping worker only if fewer than that are awake, counting the ones woken but not yet running (`woken`), since with every CPU taken they wait to run, and each request would otherwise wake another. Capped workers don't spin when idle; the awake ones take what is queued. The cap applies to bound work only: unbound work (sockets, pipes, waits) is unchanged.

**Workers are still created as before.** The pool starts a worker whenever requests outnumber idle workers, up to its limit, so the rest sleep as a reserve. If a capped pool's awake workers do sleep after all (a read that missed the ARC, a page fault on the buffer), the queued work could otherwise wait behind them.

**A stall lifts the cap.** When a request is queued without a wake-up, a 1 ms callout (`stall_co`) checks whether any bound request finished since. If none did and work is still queued, the pool stops capping and wakes every sleeping worker. The callout runs under the pool's mutex and is drained before the pool is freed.

Two counters show what happens: `kern.kqring.wq_capped`, the times a pool capped, and `kern.kqring.wq_stalls`, the times a stall lifted the cap.

## What shows it works

- `test_wq_cap_stall` in `tests/kqring_test.c` reads a cached file with `IOSQE_ASYNC` until `wq_capped` rises, then submits one more blocking `OPENAT` of a FIFO than the pool keeps awake, so every awake worker sleeps in it. A read queued behind them must complete within 5 seconds, which only the stall callout allows, and `wq_stalls` must have risen. A writer then opens the FIFO to let the `OPENAT`s finish.
- The suite, the fuzzer and the stress test pass on a `GENERIC-DEBUG` kernel (`INVARIANTS`, `WITNESS`). The fuzzer's file reads cap and uncap pools while it cancels, closes and forks around them.

## Measured

The second test machine is a Ryzen 3 PRO 4350G (4 cores, 8 threads, 8 GB) with a SATA SSD under ZFS, FreeBSD 15.1. The module before this change (`f7575b9`) and after it were loaded in turn within each repetition.

**Cached reads through the worker pool**, a 512 MiB file in the ARC, 4 KiB random reads, 16 in flight per ring, 5 seconds, three repetitions:

| Rings | Before, IOPS | After, IOPS | Change | CPU per read, before → after | Voluntary switches per read |
|---:|---:|---:|---:|---|---|
| 1 | 654,000–659,000 | 692,000–693,000 | +5.5% | 10.4 → 7.5 µs | 0.97 → 0.88 |
| 4 | 647,000–655,000 | 683,000–686,000 | +5% | 10.7 → 10.2 µs | 0.93 → 0.36 |
| 8 | 633,000–637,000 | 668,000–671,000 | +5% | 11.0 → 10.5 µs | 0.98 → 0.38 |

On the first machine, the i5-8250U, an earlier version of the change (the same cap, with a symmetric average) gave +39%, +15% and +12% on 1, 4 and 8 rings, and 40%, 15% and 11% less CPU per read. That machine had memory errors later, unrelated to kqring, so it wasn't used for the final measurement.

The benchmark above is a small program of kqring's own. **fio's cached reads** ([`fio/scale.sh`](../fio/scale.sh): the same kind of file, one ring per job, 16 in flight, `psync` one read per thread, 10 seconds) show less. Medians of three repetitions, in thousands of IOPS; runs of a case differ by about 1%:

| Jobs | kqring before | kqring after | aio | `psync` |
|---:|---:|---:|---:|---:|
| 1 | 874 | **896** | 722 | 393 |
| 2 | **1,067** | 1,054 | 732 | 680 |
| 4 | 1,080 | 1,077 | 655 | **1,189** |
| 8 | 1,100 | 1,102 | 564 | **1,634** |

One job gains 2.5%. From two jobs on, kqring stays near 1.1 million either way, while `psync` passes it at 4 jobs and reaches 1.6 million at 8.

**Work that waits for the SSD** is unchanged, as it should be: the pool never capped during these runs (`wq_capped` stayed at 0). fio 3.43, 20 seconds, five repetitions, medians:

| Case | In flight | Before | After |
|---|---:|---:|---:|
| Random reads, 8 GiB file, `primarycache=metadata` | 1 | 7,675 | 7,714 |
| Random reads, same file | 32 | 58,619 | 58,693 |
| Random writes, same file | 32 | 11,550 | 11,705 |
| Random reads, 12 GiB file, ARC hits and misses | 32 | 69,936 | 64,090 |

Writes ranged from 8,600 to 22,100 IOPS between runs with either module, as the SSD's garbage collection came and went, and the hit-and-miss mix from 46,800 to 72,700. Its means are 64,416 before and 65,128 after. p99 latency is the same within the spread.

## Reads in the submitting thread

The cap removed wake-ups, not the hand-off. Every cached ZFS read still crossed from the thread that submitted it to a worker and back as a CQE, and a thread calling `pread(2)` never does. With fio, kqring stayed near 1.1 million IOPS from two jobs on, while `psync` passed it at four and reached 1.6 million at eight. So a read that won't sleep now runs in `io_uring_enter(2)` itself, as `pread(2)` would and as Linux runs a cached read before io-wq sees it.

**Which reads.** A read of a regular file on a local filesystem (`MNT_LOCAL`), at issue in an enter's own submission, without `IOSQE_ASYNC` or `RWF_NOWAIT`. A filesystem over the network could hold the enter for as long as its server takes, so its reads still go to workers. The read completes before the enter returns, and counts toward the pool's averages as a worker's read would.

**When the pool's reads look cached.** FreeBSD can't ask ZFS whether a read would block, as Linux asks the page cache with `IOCB_NOWAIT`, so kqring goes by what the pool's reads did lately. A second average (`cache_avg`), kept like the first, counts a read as one that slept only if its thread switched out *and* it took 30 µs or more (`KQRING_WQ_SLEPT_SBT`). A plain switch count also counts brief waits for ZFS's locks, and fio's pools never looked cached by it. A read from the SSD takes longer than 30 µs, so a pool whose reads start to miss the ARC goes back to workers within a few reads. One that misses while inline holds up that enter, as it would hold up a `pread(2)` caller.

**Only when the CPUs are taken.** With CPUs to spare, workers run one ring's reads in parallel, faster than its one thread can: an early version that ran reads inline whenever they looked cached gave 373,000 IOPS at one job, against 900,000 through workers. Each enter adds one to a per-CPU count of threads in enters, and takes it off as it leaves. Reads run inline only while those threads take more than half the CPUs, so that workers could only run in their place. The counts live on other CPUs' cache lines, which each enter writes, and summing them for every submission made one job slower, so they're summed at most once a clock tick.

**And then for a while.** Threads come and go from enters, so the count dips below half for moments while the CPUs stay taken. A read sent to a worker then costs far more than its share: the worker fights the issuers for a CPU and spins on the ZFS locks they hold. In one version 2% of the reads reached workers that way and took about 4 CPUs between them. So once its reads run inline, a ring keeps them inline for 10 ticks (`KQRING_WQ_INLINE_HOLD`) after the CPUs last looked busy. For the same reason, idle workers don't spin for work while the CPUs are busy.

Designs that queued the reads and had the submitting thread help the workers run them cost more than running each read directly; none of them was kept. `kern.kqring.wq_inline` counts the reads run inline.

### What shows it works

- `test_wq_inline` starts a thread per CPU, each with its own ring and 16 random 4 KiB reads in flight from a cached 1 MiB file whose every block is different. Every read's data is checked, and some read must have run inline within 15 seconds.
- On a `GENERIC-DEBUG` kernel (`INVARIANTS`, `WITNESS`), the suite passed three times (1.5 to 4.2 million reads inline between them), the fuzzer for 30 minutes with 4 threads and 10 with 8, and the stress test for 10 minutes, with no leaks and no `WITNESS` warnings. The fuzzer ran few reads inline (1 and 18), because its mix rarely makes a pool's reads look cached; [TODO.MD](../TODO.MD#correctness) has that.
- On the stock kernel, the suite passed three times, and the differential conformance test ran three times with the same output each time. liburing's `evloop` failed, but it fails as often with the module from before the change (123 and 125 runs in 200); TODO.MD has it.

### Measured

The Ryzen 3 PRO 4350G again, with the module from before (`075669e`) and after loaded in turn within each repetition. fio's cached reads ([`fio/scale.sh`](../fio/scale.sh)), medians of three repetitions in thousands of IOPS; runs of a case differ by about 1%:

| Jobs | kqring before | kqring after | Change | aio | `psync` |
|---:|---:|---:|---:|---:|---:|
| 1 | 901 | 874 | −3% | 720 | 394 |
| 2 | 1,058 | 1,044 | −1.3% | 727 | 681 |
| 4 | 1,083 | 1,067 | −1.5% | 655 | **1,198** |
| 8 | 1,105 | 1,456 | **+32%** | 562 | **1,626** |

aio and `psync` are from the runs with the module from before; with the new one they were within 1%. At 8 jobs, kqring is 10% behind `psync` where it was 32% behind. Its reads there are run by the 8 submitting threads, not by the 4 workers the cap keeps awake. The rest of the gap is what kqring adds to each read, about 0.4 µs in the kernel to copy the SQE, set up the request and post the CQE, plus fio's io_uring engine in user space; fio submits one read per enter in these runs, so there's no batch to spread it over.

At 1 to 4 jobs no read runs inline, and kqring is 1.3–3% slower. The change adds a clock read to each request on the workers and a count to each enter; which of them costs the 3% hasn't been traced. kqring stays ahead of aio at every job count, and ahead of `psync` up to 2 jobs.

md(4) reads, through kqring's direct bio path, are unchanged. So is work that waits for the SSD, with no read run inline (fio 3.43, 20 seconds, three repetitions, medians):

| Case | In flight | Before | After |
|---|---:|---:|---:|
| Random reads, 8 GiB file, `primarycache=metadata` | 1 | 7,800 | 7,800 |
| Random reads, same file | 32 | 58,700 | 58,800 |
| Random writes, same file | 32 | 11,800 | 11,700 |
| Random reads, 12 GiB file, ARC hits and misses | 32 | 84,700 | 77,400 |

The hit-and-miss mix ranged from 78,600 to 89,800 before and 76,600 to 85,800 after, as the SSD's garbage collection came and went; p99 latency was the same.

## What's left

- **The last 10% to `psync`.** Batching would spread kqring's cost per read, but only for programs that submit many reads per enter.
- **ZFS reads that complete on their own.** ZFS reads are asynchronous inside: its I/O pipeline calls back when a disk read finishes, and only the layer above waits. A read issued below that layer would complete at once on a hit and from ZFS's callback on a miss, with no worker and no guess about whether it will sleep, as FreeBSD's asynchronous `sendfile(2)` does for the page cache. It depends on ZFS internals, so it's under research: [async-zfs-reads.md](async-zfs-reads.md).
