# kqring benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads, and with `aio(4)` and plain system calls on storage. Where kqueue has no role (regular files, disks, bulk sends into a socket with room), the comparison is the plain system call.

Every figure here is from one build, [`v0.3.0`](https://github.com/grepjava/kqring/releases/tag/v0.3.0), on the machine under [Setup](#setup), except the two fio sections, which say which build they used. The raw runs are in [`docs/bench/data/`](docs/bench/data/).

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is 13–19% faster.** When many sockets are ready at once, one `kqring_enter()` collects every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`.
- **One connection: faster at every message size, if the request is already waiting or the buffer is reused.** On AF_UNIX, `RECV` into one reused buffer is 18%–21% faster than kqueue, and multishot `RECV` 3%–16%. On TCP loopback the stack dominates, and all of them are within a few percent.
- **A one-shot `RECV` armed before its data arrives costs more than kqueue:** +25% at 64 B. It pays a second enter and installs and removes a socket upcall every time. Use multishot `RECV` for steady receive.
- **Batching is where a ring wins most.** 32 `NOP`s per enter cost 69 ns each, against 288 ns for an empty `kevent(2)`. One `NOP` per enter costs 336 ns, +16% against `kevent(2)`.
- **Disk devices go straight to the driver.** With 32 in flight on an md(4) disk, a 4 KiB read costs 1,837 ns, 65% less than `pread(2)`.
- **On a real SSD, kqring reaches what the drive can do with half of aio(4)'s CPU:** 98,876 IOPS at 32 in flight on the raw disk at 45% CPU, against 97% for aio. On a ZFS file it runs within 5% of aio at less than half the CPU.
- **Rings on several threads scale.** On md(4) disks, 4 jobs reach 1,031,000 IOPS against 323,000 for aio. On a cached ZFS file, kqring stays near 530,000 at any job count, ahead of aio; `psync` passes it from 4 jobs.
- **Zero-copy send pays off from 32–64 KiB.** From a registered buffer, `SEND_ZC` takes 57% less time than `write(2)` for 256 KiB on AF_UNIX, and 25% less on TCP loopback.
- **Pipes are 17% faster than kqueue when data is there.** Only a read that has to wait goes to a worker.
- **Cached file reads depend on the filesystem.** On UFS, 32 `READ`s per enter are 13% faster than `pread(2)`, and one per enter is +12%. ZFS has no page-cache read path, so its reads go through a worker and lose to `pread(2)`.
- **An event-driven chain needs less server CPU than kqueue:** 9–15% per message in an echo server.

## Setup

| | |
|---|---|
| CPU | Intel Core i5-8250U, 4 cores / 8 threads, 1.6 GHz base |
| Memory | 4 GiB |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; root on ZFS |
| kqring | `kqring.ko` from `v0.3.0`, entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmarks | [`tests/kqring_bench.c`](tests/kqring_bench.c), [`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) and [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c), built with `cc -O2` |
| Limits | `ulimit -l 65536` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

## Method

- Each `kqring_bench` and `kqring_mt_bench` figure is the **median of 5 runs**, and each `kqring_diskbench` figure the median of 3 runs of 5 seconds. The module was unloaded and reloaded before each run.
- A run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- `kqring_bench` is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so it measures the per-operation cost of each mechanism, not throughput under load. The echo group uses two threads.
- Most rows vary less than 5% between runs. The exceptions are sends of 16–64 KiB (up to 10%), the kqueue timer (19%), 32 disk reads through workers (13%), one fan-in row with a single slow run (35%, at N = 16 batched), and ZFS file reads (see [Pipes and files](#pipes-and-files-inline-or-through-a-worker)).
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py), and this document is written from them by [`tests/bench_doc.py`](tests/bench_doc.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain system call where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 288 |  |
| kqring `NOP`, one per enter | 336 | +16% |
| kqring `NOP`, 32 per enter | 69 | −76% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 1,067 | +270% |

A single enter costs 47 ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching spreads the rest over many requests. The `IOSQE_ASYNC` row is a full round trip through a worker thread, kept short by both sides spinning briefly when a CPU is free.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 725 | 754 | 834 |
| kqueue: `write` + `kevent` + `recv` | 1,117 | 1,155 | 1,267 |
| kqring `RECV`, provided buffer | 964 (−14%) | 1,065 (−8%) | 1,256 (−1%) |
| kqring `RECV`, one reused buffer | **881 (−21%)** | **920 (−20%)** | **1,037 (−18%)** |
| kqring multishot `RECV`, provided buffers | 939 (−16%) | 1,038 (−10%) | 1,232 (−3%) |
| kqring `RECV` armed before the data arrives | 1,393 (+25%) | 1,503 (+30%) | 1,700 (+34%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the cache misses that causes show as the gap to the reused-buffer row, wider as messages grow. Programs that use fewer, smaller buffers should see results close to the reused-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 5,948 | |
| kqring `RECV`, provided buffer | 6,129 | +3% |
| kqring `RECV`, one reused buffer | 5,982 | +1% |
| kqring multishot `RECV` | 5,891 | −1% |
| kqring `RECV` armed before the data arrives | 6,566 | +10% |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 831 | 860 | 870 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **676 (−19%)** | **699 (−19%)** | **757 (−13%)** |
| kqring: N single-shot `RECV`s submitted and completed in one enter | **678 (−18%)** | **698 (−19%)** | **709 (−18%)** |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Echo server: an event-driven chain against kqueue

`kqring_bench echo`. A server thread echoes each message to a client in another thread, which sends one and waits for it. Each cell is round trip / server CPU per message, in ns.

![Echo server chart](docs/bench/echo.svg)

| Server | AF_UNIX 64 B | AF_UNIX 4 KiB | TCP 64 B |
|---|---:|---:|---:|
| kqueue: `kevent` + `read` + `write` | 5,813 / 3,112 | 6,951 / 3,636 | 11,760 / 4,284 |
| kqring io_uring style: each `RECV` CQE → submit `SEND` + next `RECV` | 6,110 / 2,998 | 7,187 / 3,574 | 12,089 / 4,234 |
| kqring trigger: `RECV` → `SEND`, `LEN_FROM_PREV`, no CQE per message | **5,592 / 2,638 | 6,734 / 3,215 | 11,625 / 3,900** |

Against kqueue, the trigger (an [event-driven chain](docs/event-chains-spec.md), opted into per ring) takes 15%, 12% and 9% less server CPU per message. The server never returns to user space between messages.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as a program must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 9.38 | 11.25 | 12.08 | 10.18 |
| kqring `SEND` | 8.54 (+10%) | 10.53 (+7%) | 11.57 (+4%) | 10.07 (+1%) |
| kqring `SEND_ZC`, registered buffer | 9.73 (−4%) | **15.19 (−26%)** | **21.17 (−43%)** | **23.68 (−57%)** |
| kqring `SEND_ZC`, unregistered buffer | 8.00 (+17%) | **12.78 (−12%)** | **17.06 (−29%)** | **19.24 (−47%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 2.20 | 3.64 | 4.29 | 4.66 |
| kqring `SEND` | 2.13 (+3%) | 3.57 (+2%) | 4.16 (+3%) | 4.69 (0%) |
| kqring `SEND_ZC`, registered buffer | 2.27 (−3%) | 3.40 (+7%) | **5.13 (−16%)** | **6.25 (−25%)** |
| kqring `SEND_ZC`, unregistered buffer | 2.07 (+6%) | 3.29 (+11%) | **4.90 (−12%)** | **5.96 (−22%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default. TCP always uses one page.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread, so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 14,793 | −9% |
| kqueue: `kevent` + `accept` | 16,283 | |
| kqring multishot `ACCEPT` | 16,113 | −1% |
| kqring multishot `ACCEPT`, 50 µs busy-poll | **14,320** | **−12%** |

The enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays for a wake-up. With `IORING_REGISTER_NAPI` set to 50 µs, multishot `ACCEPT` avoids the sleep, at the cost of CPU time on waits that don't end quickly.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 9,412 | |
| kqring `TIMEOUT`, 1 µs | 4,898 | −48% |

Both are dominated by timer-interrupt scheduling, not by the API, and the kqueue row varies by 19% between runs.

### Pipes and files: inline or through a worker

![Pipes and files chart](docs/bench/worker.svg)

kqring runs a non-socket `READ` or `WRITE` inline, on the thread that submitted it, when that cannot block:
- **Regular files:** a positioned read whose pages are all in the page cache, through `VOP_READ_PGCACHE` (UFS and tmpfs).
- **Pipes:** a `READ` on a pipe that has data, or a `WRITE` to one with room, blocking or not.

Everything else goes to a worker thread, including a pipe `READ` that has to wait for data (even on an `O_NONBLOCK` pipe: like io_uring, kqring waits rather than fail with `-EAGAIN`).

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 619 | −40% |
| kqueue: `write` + `kevent` + `read` | 1,026 | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | **846** | **−18%** |
| kqring `READ`, blocking pipe, data ready (inline) | 848 | −17% |

**File on UFS: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`. The file is on a UFS file system on an md(4) disk.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 1,189 | |
| kqring `READ`, one per enter | 1,328 | +12% |
| kqring `READ`, one per enter, registered file | 1,323 | +11% |
| kqring `READ`, 32 per enter | **1,028** | **−13%** |

A registered file saves almost nothing, so descriptor lookups aren't where the single-read gap comes from. It is the fixed cost of an enter, plus the SQE and CQE.

**File on ZFS.** ZFS caches in the ARC rather than the page cache, so `READ` goes to a worker. These reads don't give one stable figure on this machine: the same file in `/tmp` (lz4, 128 KiB records) runs in a fast mode or a slow one from run to run, and plain `pread(2)` does too. In the fast mode, `pread(2)` takes about 1,510 ns and one `READ` per enter about 2,490 ns. In the slow mode, `pread(2)` takes 6,000–14,000 ns. 32 `READ`s per enter have measured anywhere from about 2,250 to 20,000 ns, so no single figure is given for them. For ZFS, the [fio tables](#fio-on-a-real-ssd) are the better guide.

### Disk device: 4 KiB random reads and writes

A positioned `READ` or `WRITE` on a disk device goes straight to the driver as a bio over the held user pages, as `aio(4)` does, and completes from the bio callback. This is an md(4) malloc disk with every block written first, so it measures the I/O path, not media. A block that was never written costs md far less to read, so an empty disk gives figures that are too good.

| Variant | Read | vs `pread` | Write | vs `pwrite` |
|---|---:|---:|---:|---:|
| `pread(2)` / `pwrite(2)` | 5,280 | | 5,023 | |
| kqring, one per enter, to the driver | 5,842 | +11% | 5,594 | +11% |
| kqring, one per enter, `IOSQE_ASYNC` through a worker | 5,804 | +10% | 5,570 | +11% |
| kqring, 32 per enter, to the driver | **1,837** | **−65%** | **1,456** | **−71%** |
| kqring, 32 per enter, `IOSQE_ASYNC` through workers | 2,509 | −52% | | |

With one request per enter, kqring pays one sleep and wake-up more than `pread(2)`, since the owner waits in `kqring_enter()` for the bio callback's taskwork. With 32 in flight, the driver gets them all at once, which `pread(2)` can't do from one thread.

### Storage workloads: queue depth, aio(4) and `pread(2)` threads

[`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) runs storage-benchmark-style workloads: a fixed number of I/Os in flight per job, random or sequential offsets, for a fixed time after a 1 s ramp, through kqring, `aio(4)` or threads calling `pread(2)` / `pwrite(2)`. All figures are IOPS.

**A 1 GiB md(4) malloc disk, 4 KiB random I/O** (the bio path; md serves every bio from one thread). `+F+B` is a fixed file and registered buffers, `+P` `SQPOLL` and `+I` `IOPOLL`:

| Variant | Read, 1 | Read, 32 | Write, 1 | Write, 32 |
|---|---:|---:|---:|---:|
| `pread` / `pwrite` threads | 182,614 | 489,297 | 190,441 | 570,456 |
| aio | 172,570 | 578,331 | 180,042 | 611,494 |
| kqring | 166,332 | 570,702 | 174,082 | 684,833 |
| kqring `+F+B` | 167,632 | 578,959 | 175,072 | 697,128 |
| kqring `+P` | 123,215 | 661,329 | 123,393 | 679,681 |
| kqring `+I` | 167,796 | 667,533 | 174,477 | 692,434 |

At 32 in flight, kqring writes are 12% ahead of aio. Reads at 32 in flight vary by up to 24% between runs for every engine, so the differences in that column aren't meaningful: kqring and aio are level. One at a time, a thread calling `pread(2)` is 10% faster, as in the disk group above.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring | kqring p99 µs |
|---:|---:|---:|---:|---:|
| 1 | 70,514 | 53,345 | 73,158 | 28 |
| 4 | 116,850 | 111,388 | 113,764 | 61 |
| 16 | 116,372 | 113,881 | 112,930 | 246 |
| 64 | 128,310 | 113,547 (at 32) | 107,067 | 868 |

aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`).

**Waiting for several completions.** An enter with `min_complete` = N is woken once N plain `READ`/`WRITE` completions are queued, rather than for each one, which saves context switches more than it adds IOPS. On the md disk at 32 in flight:

| Workload | Wait for 1 | Wait for 16 |
|---|---:|---:|
| Reads | 674,607 | 705,426 |
| Writes | 761,818 | 764,695 |

**`SQPOLL`.** The `SQPOLL` thread polls for the whole of `sq_thread_idle` while it's active, as io_uring's does. `-C` also polls the CQ from user space instead of entering to wait, which is how a latency-bound program would use it. 4 KiB random reads on the md disk:

| In flight | plain kqring | `SQPOLL` | `SQPOLL`, `-C` |
|---:|---:|---:|---:|
| 1 | 168,446 | 120,733 | **194,178** |
| 4 | 543,281 | 366,453 | **520,941** |
| 32 | 565,587 | 725,865 | **622,971** |

`kqring_diskbench -V` checks every read against what was last written to that block and re-reads the whole area at the end. Mixed random reads and writes passed it on both the md disk and the ZFS file on this build.

### Several threads: one ring per thread

[`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) runs each workload on 1, 2, 4 and 8 threads, each pinned to its own CPU (CPUs 2n and 2n+1 are the two hyperthreads of one core) with its own ring, kqueue or socket pair. That's the thread-per-core model `SINGLE_ISSUER` rings are made for. Figures are millions of operations per second for all threads together, from runs of 2 s.

![Ping-pong with one ring per thread](docs/bench/threads.svg)

| Workload | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| `kevent(2)`, nothing to do | 3.49 | 5.97 | 11.94 | 22.36 |
| kqring `NOP`, one per enter | 2.88 | 4.27 | 7.23 | 11.62 |
| kqring `NOP`, 32 per enter | 13.64 | 13.28 | 22.82 | 36.77 |
| AF_UNIX ping-pong, 64 B, kqueue | 0.86 | 1.40 | 2.52 | 3.93 |
| AF_UNIX ping-pong, 64 B, kqring `RECV` into one buffer | 1.10 | 1.64 | 2.76 | 4.45 |

Ping-pong stays ahead of kqueue at every thread count, by 27% on 1 thread and 10–17% on more. What's left at 8 threads is per-request work, since two threads share each core. The benchmark's shared-file workload isn't shown: its file is in `/tmp`, which is ZFS here, and it ran in the slow mode described under [Pipes and files](#pipes-and-files-inline-or-through-a-worker).

### fio on a real SSD

[fio](fio/README.md) 3.43 with its own `io_uring` engine on kqring, against `posixaio` (`aio(4)`) and `psync`, on a 1 TB WD Blue 3D NAND SATA SSD (`WDS100T2B0A`, about 98,000 IOPS and 540 MB/s here). Each figure is the median of three 20-second runs after a 2-second ramp. `psync` runs one thread per I/O in flight. [`fio/bench.sh`](fio/bench.sh) reproduces them. Measured on `a3925b9`; the only kernel change since then is to reads of pipes.

![CPU per engine on a SATA SSD](docs/bench/fio_cpu.svg)

**The raw disk, read-only** (the direct bio path), 4 KiB random reads:

| In flight | kqring IOPS | CPU | aio IOPS | CPU | `psync` IOPS |
|---:|---:|---:|---:|---:|---:|
| 1 | 9,748 | 17% | 9,712 | 18% | 10,072 |
| 8 | 76,606 | 43% | 76,177 | 64% | 74,250 |
| 32 | 98,876 | **45%** | 98,775 | 97% | 98,809 |

Every engine reaches what the drive can do; kqring gets there with half of aio's CPU time. At 32 in flight, `--fixedbufs --registerfiles` (98,569) and `--sqthread_poll` (98,538) change nothing, since the drive is the limit. Sequential 128 KiB reads run at 541 MB/s through both kqring and aio, with kqring at 11% CPU against 16%.

**A file on ZFS** (the worker path), 8 GiB, 4 KiB records, `primarycache=metadata` so that reads reach the disk. FreeBSD 15.1 has ZFS direct I/O off by default (`vfs.zfs.dio_enabled=0`), so `--direct=1` still goes through ZFS's buffers:

| Random 4 KiB | In flight | kqring IOPS | CPU | aio IOPS | CPU | `psync` IOPS |
|---|---:|---:|---:|---:|---:|---:|
| Reads | 1 | **7,802** | 76% | 4,509 | 8% | 5,009 |
| Reads | 32 | 63,898 | **33%** | 66,409 | 72% | 65,551 |
| Writes | 1 | 9,392 | 48% | 8,755 | 13% | 9,967 |
| Writes | 32 | 9,404 | **14%** | 9,869 | 30% | 10,551 |

One read at a time, kqring is 73% ahead of aio, spending a CPU on it: the issuer spins briefly for the worker rather than sleeping. At 32 in flight it is 4–5% behind aio at less than half the CPU, and 11% behind `psync` on writes. Writes vary by about 10% from run to run. Latency has a longer tail than aio's: a p99 of 1.27 ms against 0.78 ms for reads, and 14.0 ms against 11.9 ms for writes.

### fio with several rings

One ring per thread, 4 KiB random reads, on targets the SSD doesn't limit: [`fio/scale.sh`](fio/scale.sh). `psync` here is one thread per job, so it has 1/16 of the others' I/Os in flight. Medians of three 10-second runs on `a3925b9`, in thousands of IOPS.

<p align="center">
  <img src="docs/bench/fio_scale.svg" alt="fio on md(4) disks by jobs" width="49%">
  <img src="docs/bench/fio_cached.svg" alt="fio on a cached ZFS file by jobs" width="49%">
</p>

| Jobs | ZFS, in the ARC: kqring | aio | `psync` | `md(4)`, a disk per job: kqring | aio |
|---:|---:|---:|---:|---:|---:|
| 1 | **424** | 312 | 290 | **544** | 453 |
| 2 | **528** | 325 | 452 | **965** | 558 |
| 4 | 501 | 304 | **705** | **1,031** | 323 |
| 8 | 532 | 272 | **784** | | |

The direct bio path scales until the 4 cores run out, while aio falls back. Through the worker pool, kqring stays near 530,000 at any number of jobs, ahead of aio; from 4 jobs on, `psync` passes it, since a thread reading the ARC itself needs no hand-off.

## Known gaps

- **A pipe read that has to wait goes through a worker thread**, as does a write to a full pipe. Waiting for data with `POLL_ADD` first avoids the worker.
- **A poll on another ring can complete just after the enter that fired it.** The CQE normally comes before the enter returns, but when the ring's poller has just taken the event from its kqueue and not yet recorded it, the enter finds nothing to post, and the CQE follows moments later. It is never lost. It has only been seen on a slow kernel, a KASAN one, where it failed `test_poll_same_thread`.
- **Reads of a FIFO that have to wait can complete out of order.** Reads of a pipe post their CQEs in the order of the data, but a named FIFO's reads block inside `fo_read()` on their workers, so one can read first and post second. Each read gets correct data.
- **Buffered writes to a file wait for their worker.** That keeps writes to one file in order, as io-wq's per-inode hashing does, but the issuer waits too, where io-wq's hashing wouldn't make it. So do `O_DIRECT` writes that use the file position, `O_APPEND` or per-write flags such as `RWF_DSYNC`.
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers file reads that need the disk, ZFS, `O_DIRECT` on files, and reads using the file position (`off` = −1). Only disk devices themselves get the direct bio path: FreeBSD filesystems have no asynchronous read or write entry point. It also covers reads into a buffer that isn't mapped yet.
- **Past 4 jobs on cached data, `psync` beats the worker path.** Workers top out near 530,000 IOPS on this machine, because each request is handed to another thread.
- **One operation per enter costs a little more than an empty `kevent`.** That's the `NOP` floor: parsing an SQE and posting a CQE. Batching removes it.
- **A wait that just misses its completion pays a full sleep.** When the completion comes from another thread (the TCP loopback netisr, for instance) a moment after the enter checks, the enter sleeps and pays for a wake-up. `IORING_REGISTER_NAPI` opts in to a busy-wait before sleeping.
- **An armed one-shot `RECV` costs more than kqueue.** What remains is the extra `kqring_enter()` the pattern makes, plus the `EAGAIN` probe before arming. Use multishot `RECV` for steady receive.
- **`RLIMIT_MEMLOCK` defaults to 64 KiB on FreeBSD.** Registering buffers holds and charges their pages, so `IORING_REGISTER_BUFFERS` fails with `ENOMEM` beyond 64 KiB in total unless the limit is raised, through `login.conf(5)` (`memorylocked`) or `ulimit -l`. Unregistered `SEND_ZC` silently falls back to copying when the limit is reached. Linux's default is 8 MiB.
- **Provided-buffer working set.** The multishot and buffer-select figures include cache misses from rotating through 16 MiB of buffers. That's a property of this benchmark's buffer ring size, not a fixed cost, but programs should size buffer rings with the cache in mind.

## Reproducing

On the FreeBSD host, from the repository root:

```sh
make
sudo kldload ./kmod/kqring.ko
ulimit -l 65536                   # the send group registers 2 MiB of buffers
./tests/kqring_bench > bench.1.out           # all groups
./tests/kqring_bench fanin send   # selected groups: floor pingpong fanin accept pipe file send timer disk echo
./tests/kqring_mt_bench 2 > mt.1.out         # several threads, 2 s per run
sudo kldunload kqring
```

If the hard limit is 64 KiB, raise it as root first, for example: `sudo sh -c 'ulimit -l 65536; exec sudo -u "$USER" ./tests/kqring_bench'`.

Each line of output is `ROW <group> <variant> <ns/op>`. To match these tables, run it 5 times, reloading the module before each run. Then produce the medians and charts on any machine with Python 3:

```sh
python3 tests/bench_report.py docs/bench docs/bench/data/*
python3 tests/bench_doc.py
```

Runs on other file systems and disks are selected by name and environment: `KQRING_BENCH_FILE_DIR` puts the `file` group's 64 MiB temporary file in a directory of your choice (these tables use a UFS file system on an md(4) disk, saved as `ufs.*.out`), and the `disk` group runs only with `KQRING_BENCH_DISK` set to a device it may overwrite (`sudo mdconfig -a -t malloc -s 64m -u 9 && sudo chown $USER /dev/md9`, filled with `dd if=/dev/urandom of=/dev/md9 bs=1m`, saved as `disk.*.out`).

The storage tables come from `kqring_diskbench`; the comment at the top of [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) lists its options. For example, on a 1 GiB md disk and a file on ZFS:

```sh
sudo mdconfig -a -t malloc -s 1g -u 9 && sudo chown $USER /dev/md9
dd if=/dev/urandom of=/dev/md9 bs=1m   # unwritten blocks read far faster
./tests/kqring_diskbench -e kqring -q 32 -w randwrite /dev/md9
./tests/kqring_diskbench -e aio -q 32 -w randwrite /dev/md9
dd if=/dev/random of=$HOME/dbench.dat bs=1m count=256
./tests/kqring_diskbench -e kqring -q 16 -w randread $HOME/dbench.dat
./tests/kqring_diskbench -V -e kqring -F -B -w randrw /dev/md9   # verify
```

It refuses to write to a device unless it is an md(4) disk or `-f` is given. `-B` registers `-q` × `-b` bytes per job, so raise `ulimit -l` for it.
