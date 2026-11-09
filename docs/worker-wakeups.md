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

## What's left

The cap removes wake-ups, not the hand-off. Every cached ZFS read still crosses from the thread that submitted it to a worker and back as a CQE, and a thread calling `pread(2)` never does. With one ring, the submitting thread is the limit. With more, the CPUs are, and the hand-off's share of each read is what keeps kqring near 1.1 million IOPS on this machine while `psync` keeps scaling. kqring stays ahead of aio at every job count, and ahead of `psync` until there are as many threads as cores to read with.

Removing the hand-off needs a read that doesn't go to a worker when it won't sleep. Two ways are being looked at:

- **Reads in the submitting thread.** When the pool's average says its reads don't sleep, run them in `io_uring_enter(2)` itself, as `pread(2)` would, but many per call. A read that does sleep then holds up that enter, as it would hold up a `pread(2)` caller, and the average sends the next ones back to workers. `IOSQE_ASYNC` still means a worker. This is close to Linux, which tries a read in the submitting thread with `IOCB_NOWAIT` before io-wq sees it.
- **ZFS reads that complete on their own.** ZFS reads are asynchronous inside: its I/O pipeline calls back when a disk read finishes, and only the layer above waits. A read issued below that layer would complete at once on a hit and from ZFS's callback on a miss, with no worker either way, as FreeBSD's asynchronous `sendfile(2)` does for the page cache. It depends on ZFS internals, so it needs a design note of its own first.
