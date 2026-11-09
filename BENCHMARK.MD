# kqring benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads, and with `aio(4)` and plain system calls on storage. Where kqueue has no role (regular files, disks, bulk sends into a socket with room), the comparison is the plain system call.

Every figure here is from one build, `7641bf4`, on the machine under [Setup](#setup), an 8-thread AMD Ryzen. The raw runs are in [`docs/bench/data/`](docs/bench/data/). Earlier editions were measured on an Intel laptop; how the two machines differ, and why it matters for kqring, is under [Why the results depend on the CPU](#why-the-results-depend-on-the-cpu). For kqring against Linux io_uring on one machine, see [docs/linux-comparison.md](docs/linux-comparison.md).

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Batching is where a ring wins.** 32 `NOP`s per enter cost 76 ns each, against 104 ns for an empty `kevent(2)`. One `NOP` per enter costs 187 ns, +80% against `kevent(2)`: that's kqring's fixed cost per enter, the SQE, the request and the CQE.
- **On this machine a system call is cheap, and saving one is worth less than kqring's own work per message.** Collecting many connections' messages (fan-in) is 26–66% slower than kqueue, though kqring makes one system call for N messages where kqueue makes N + 1. On one connection, `RECV` into one reused buffer is 2% slower than kqueue at 64 B, and a pipe `READ` with data ready 10% slower. On the Intel laptop, where a system call costs nearly three times as much, the same benchmarks put kqring ahead: see [below](#why-the-results-depend-on-the-cpu).
- **A one-shot `RECV` armed before its data arrives costs more than kqueue:** +84% at 64 B. It pays a second enter and installs and removes a socket upcall every time. Use multishot `RECV` for steady receive.
- **Disk devices go straight to the driver.** With 32 in flight on an md(4) disk, a 4 KiB read costs 1,463 ns, 57% less than `pread(2)`.
- **On a real SSD, kqring reaches what the drive can do with less CPU than aio(4):** 84,952 IOPS at 32 in flight on the raw disk at 25% CPU, against 31% for aio.
- **Rings on several threads scale.** On md(4) disks, 4 jobs reach 2,021,000 IOPS against 601,000 for aio. On a cached ZFS file, kqring reaches 1,502,000 at 8 jobs, ahead of aio; `psync` is ahead from 4 jobs.
- **Zero-copy send pays off for large messages.** From a registered buffer, `SEND_ZC` takes 54% less time than `write(2)` for 256 KiB on AF_UNIX, and 33% less on TCP loopback. At 64 KiB and below it costs more on this machine.
- **Cached file reads depend on the filesystem.** On UFS, 32 `READ`s per enter are 1% slower than `pread(2)`, and one per enter is +16%. ZFS has no page-cache read path, so its reads go through a worker or, once the CPUs are taken, run in the enter.
- **An event-driven chain needs less server CPU than kqueue:** 3–5% per message in an echo server.

## Why the results depend on the CPU

kqring saves system calls, and does more work per request than the call it replaces: it reads an SQE, sets up a request and posts a CQE. Which of the two wins depends on what a system call costs, and that depends on the CPU and its security mitigations. On the Intel i5-8250U laptop these tables used to come from, FreeBSD isolates the kernel's page tables (PTI) against Meltdown, and every system call pays for it; AMD's CPUs aren't affected, so the Ryzen doesn't. The same `kqring_bench` groups on both (the laptop's runs are from `v0.3.0`, in [`docs/bench/data-i5-8250u/`](docs/bench/data-i5-8250u/); ns per operation):

| | i5-8250U, PTI | Ryzen 3 PRO 4350G |
|---|---:|---:|
| Empty `kevent(2)` | 288 | 104 |
| kqring `NOP`, one per enter | 336 | 187 |
| kqring `NOP`, 32 per enter | 69 | 76 |
| Fan-in, N = 256: kqueue | 860 | 412 |
| Fan-in, N = 256: kqring multishot `RECV` | 699 | 527 |
| AF_UNIX ping-pong, 64 B: kqueue | 1,117 | 541 |
| AF_UNIX ping-pong, 64 B: kqring `RECV`, one buffer | 881 | 552 |

The kqueue rows, which are mostly system calls, got much cheaper on the Ryzen; kqring's work per request less so. So fan-in, 19% faster than kqueue on the laptop, is 28% slower on the Ryzen. It isn't a change in kqring: `v0.3.0` and `7641bf4`, alternated on the Ryzen 5 runs each, differed by a median of 0.7% per row over these groups ([raw runs](docs/bench/data-v030-vs-head/)). Where kqueue makes many system calls per unit of work and each costs a lot, as with PTI, kqring's saving shows. Where calls are cheap, its per-request cost decides, and part of that cost is what kqring does beyond Linux's io_uring: an enter costs about 250 ns more ([docs/linux-comparison.md](docs/linux-comparison.md)).

## Setup

| | |
|---|---|
| CPU | AMD Ryzen 3 PRO 4350G, 4 cores / 8 threads; no PTI (`vm.pmap.pti=0`) |
| Memory | 8 GB |
| Disk | SanDisk SD7TB6S 256 GB SATA SSD |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; root on ZFS (OpenZFS 2.4.2) |
| kqring | `kqring.ko` from `7641bf4`, entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmarks | [`tests/kqring_bench.c`](tests/kqring_bench.c), [`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) and [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c), built with `cc -O2` |
| Limits | `ulimit -l 65536` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

## Method

- Each `kqring_bench` and `kqring_mt_bench` figure is the **median of 5 runs**, and each `kqring_diskbench` figure the median of 3 runs of 10 seconds. The module was unloaded and reloaded before each `kqring_bench` and `kqring_mt_bench` run.
- A run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- `kqring_bench` is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so it measures the per-operation cost of each mechanism, not throughput under load. The echo group uses two threads.
- 99 of 113 rows vary less than 5% between runs. The exceptions are some sends of 16–64 KiB (up to 20%), AF_UNIX ping-pong at 1 KiB (up to 9%), and the file group's reads in `/tmp`, which is ZFS here and ran in ZFS's slow mode in some runs (see [Pipes and files](#pipes-and-files-inline-or-through-a-worker)).
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py), and this document is written from them by [`tests/bench_doc.py`](tests/bench_doc.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain system call where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 104 |  |
| kqring `NOP`, one per enter | 187 | +80% |
| kqring `NOP`, 32 per enter | 76 | −27% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 733 | +604% |

A single enter costs 83 ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching spreads the rest over many requests. The `IOSQE_ASYNC` row is a full round trip through a worker thread, kept short by both sides spinning briefly when a CPU is free.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 346 | 395 | 459 |
| kqueue: `write` + `kevent` + `recv` | 541 | 631 | 832 |
| kqring `RECV`, provided buffer | 713 (+32%) | 798 (+27%) | 1,135 (+36%) |
| kqring `RECV`, one reused buffer | 552 (+2%) | 638 (+1%) | 844 (+1%) |
| kqring multishot `RECV`, provided buffers | 625 (+16%) | 708 (+12%) | 1,048 (+26%) |
| kqring `RECV` armed before the data arrives | 992 (+84%) | 1,069 (+70%) | 1,417 (+70%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the cache misses that causes show as the gap to the reused-buffer row, wider as messages grow. Programs that use fewer, smaller buffers should see results close to the reused-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 3,015 | |
| kqring `RECV`, provided buffer | 3,069 | +2% |
| kqring `RECV`, one reused buffer | 2,913 | −3% |
| kqring multishot `RECV` | 3,061 | +2% |
| kqring `RECV` armed before the data arrives | 3,496 | +16% |

TCP is dominated by the loopback stack (about 3 µs per message), so the mechanisms differ by a few percent.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 402 | 412 | 487 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | 509 (+26%) | 527 (+28%) | 807 (+66%) |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 540 (+34%) | 547 (+33%) | 644 (+32%) |

This is the shape of a busy server. kqring makes one system call for N messages, kqueue makes N + 1. On this machine kqring's work per message, a CQE and a buffer, costs more than the system calls it saves; on the Intel laptop, where each call costs more, it was the other way ([Why the results depend on the CPU](#why-the-results-depend-on-the-cpu)).

### Echo server: an event-driven chain against kqueue

`kqring_bench echo`. A server thread echoes each message to a client in another thread, which sends one and waits for it. Each cell is round trip / server CPU per message, in ns.

![Echo server chart](docs/bench/echo.svg)

| Server | AF_UNIX 64 B | AF_UNIX 4 KiB | TCP 64 B |
|---|---:|---:|---:|
| kqueue: `kevent` + `read` + `write` | 3,590 / 1,845 | 3,946 / 2,044 | 6,348 / 2,358 |
| kqring io_uring style: each `RECV` CQE → submit `SEND` + next `RECV` | 3,710 / 2,007 | 4,047 / 2,177 | 6,519 / 2,546 |
| kqring trigger: `RECV` → `SEND`, `LEN_FROM_PREV`, no CQE per message | **3,388 / 1,791 | 3,718 / 1,939 | 6,166 / 2,280** |

Against kqueue, the trigger (an [event-driven chain](docs/event-chains-spec.md), opted into per ring) takes 3%, 5% and 3% less server CPU per message. The server never returns to user space between messages.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as a program must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 13.16 | 13.34 | 13.62 | 4.86 |
| kqring `SEND` | 11.29 (+17%) | 12.22 (+9%) | 12.50 (+9%) | 4.79 (+2%) |
| kqring `SEND_ZC`, registered buffer | 7.93 (+66%) | 9.67 (+38%) | 10.48 (+30%) | **10.66 (−54%)** |
| kqring `SEND_ZC`, unregistered buffer | 6.64 (+98%) | 8.91 (+50%) | 9.79 (+39%) | **10.03 (−51%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 3.79 | 5.61 | 5.87 | 3.87 |
| kqring `SEND` | 3.59 (+5%) | 5.38 (+4%) | 5.68 (+3%) | 3.81 (+1%) |
| kqring `SEND_ZC`, registered buffer | 3.20 (+18%) | 4.91 (+14%) | 5.38 (+9%) | **5.74 (−33%)** |
| kqring `SEND_ZC`, unregistered buffer | 3.06 (+24%) | 4.78 (+17%) | 5.16 (+14%) | **5.55 (−30%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default. TCP always uses one page.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread, so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 7,073 | −3% |
| kqueue: `kevent` + `accept` | 7,271 | |
| kqring multishot `ACCEPT` | 7,386 | +2% |
| kqring multishot `ACCEPT`, 50 µs busy-poll | 7,324 | +1% |

The enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays for a wake-up. With `IORING_REGISTER_NAPI` set to 50 µs, multishot `ACCEPT` avoids the sleep, at the cost of CPU time on waits that don't end quickly.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 4,010 | |
| kqring `TIMEOUT`, 1 µs | 3,542 | −12% |

Both are dominated by timer-interrupt scheduling, not by the API; the kqueue row varies by 1% between runs.

### Pipes and files: inline or through a worker

![Pipes and files chart](docs/bench/worker.svg)

kqring runs a non-socket `READ` or `WRITE` inline, on the thread that submitted it, when that cannot block:
- **Regular files:** a positioned read whose pages are all in the page cache, through `VOP_READ_PGCACHE` (UFS and tmpfs).
- **Pipes:** a `READ` on a pipe that has data, or a `WRITE` to one with room, blocking or not.

Everything else goes to a worker thread, including a pipe `READ` that has to wait for data (even on an `O_NONBLOCK` pipe: like io_uring, kqring waits rather than fail with `-EAGAIN`).

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 256 | −41% |
| kqueue: `write` + `kevent` + `read` | 436 | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | 478 | +10% |
| kqring `READ`, blocking pipe, data ready (inline) | 478 | +10% |

**File on UFS: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`. The file is on a UFS file system on an md(4) disk.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 827 | |
| kqring `READ`, one per enter | 962 | +16% |
| kqring `READ`, one per enter, registered file | 965 | +17% |
| kqring `READ`, 32 per enter | 834 | +1% |

A registered file saves almost nothing, so descriptor lookups aren't where the single-read gap comes from. It is the fixed cost of an enter, plus the SQE and CQE.

**File on ZFS.** ZFS caches in the ARC rather than the page cache, so `READ` goes to a worker, or runs in the enter once the readers take most of the CPUs. These reads don't give one stable figure on this machine: the same 64 MiB file in `/tmp` (lz4, 128 KiB records) runs in a fast mode or a slow one, and plain `pread(2)` does too. In the [`v0.3.0` against `7641bf4` runs](docs/bench/data-v030-vs-head/), `pread(2)` took up to 28,692 ns, in 2 of the 5 `7641bf4` runs and none of the `v0.3.0` ones. `pread(2)` runs first in the group, so it was slow before kqring had read the file in that run, and the slow mode is in ZFS, not kqring's read path; whether something earlier in the run sets it isn't known. Across the 5 runs here, `pread(2)` took 1,145–1,156 ns, one `READ` per enter 1,874–5,973 ns, and 32 per enter 740–8,545 ns. In the fast mode, 32 `READ`s per enter take 740 ns each against 1,145 for `pread(2)`. For ZFS, the [fio tables](#fio-on-a-real-ssd) are the better guide.

### Disk device: 4 KiB random reads and writes

A positioned `READ` or `WRITE` on a disk device goes straight to the driver as a bio over the held user pages, as `aio(4)` does, and completes from the bio callback. This is an md(4) malloc disk with every block written first, so it measures the I/O path, not media. A block that was never written costs md far less to read, so an empty disk gives figures that are too good.

| Variant | Read | vs `pread` | Write | vs `pwrite` |
|---|---:|---:|---:|---:|
| `pread(2)` / `pwrite(2)` | 3,422 | | 3,620 | |
| kqring, one per enter, to the driver | 3,772 | +10% | 3,896 | +8% |
| kqring, one per enter, `IOSQE_ASYNC` through a worker | 3,651 | +7% | 3,805 | +5% |
| kqring, 32 per enter, to the driver | **1,463** | **−57%** | **1,645** | **−55%** |
| kqring, 32 per enter, `IOSQE_ASYNC` through workers | 1,847 | −46% | | |

With one request per enter, kqring pays one sleep and wake-up more than `pread(2)`, since the owner waits in `kqring_enter()` for the bio callback's taskwork. With 32 in flight, the driver gets them all at once, which `pread(2)` can't do from one thread.

### Storage workloads: queue depth, aio(4) and `pread(2)` threads

[`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) runs storage-benchmark-style workloads: a fixed number of I/Os in flight per job, random or sequential offsets, for a fixed time after a 1 s ramp, through kqring, `aio(4)` or threads calling `pread(2)` / `pwrite(2)`. All figures are IOPS.

**A 1 GiB md(4) malloc disk, 4 KiB random I/O** (the bio path; md serves every bio from one thread). `+F+B` is a fixed file and registered buffers, `+P` `SQPOLL` and `+I` `IOPOLL`:

| Variant | Read, 1 | Read, 32 | Write, 1 | Write, 32 |
|---|---:|---:|---:|---:|
| `pread` / `pwrite` threads | 269,726 | 643,802 | 254,058 | 564,546 |
| aio | 253,639 | 646,520 | 242,859 | 555,760 |
| kqring | 253,792 | 649,078 | 243,668 | 554,390 |
| kqring `+F+B` | 253,262 | 652,808 | 244,129 | 556,087 |
| kqring `+P` | 189,917 | 687,143 | 182,813 | 582,006 |
| kqring `+I` | 253,117 | 650,347 | 243,941 | 552,122 |

At 32 in flight, kqring's reads and writes are level with aio's; rounds differed by at most 0.6%. One at a time, a thread calling `pread(2)` is 6% faster, as in the disk group above. With 32 in flight, 32 threads calling `pread(2)` or `pwrite(2)` are within 2% of both, for reads and for writes, which suggests md's one thread is the limit there.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring | kqring p99 µs |
|---:|---:|---:|---:|---:|
| 1 | 84,897 | 73,072 | 83,801 | 24 |
| 4 | 118,670 | 122,562 | 123,351 | 68 |
| 16 | 116,276 | 119,523 | 124,276 | 180 |
| 64 | 115,544 | 119,256 (at 32) | 123,796 | 623 |

From 4 in flight, kqring is 4–7% ahead of as many `pread(2)` threads; one at a time it's level with a thread calling `pread(2)`. aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`).

**Waiting for several completions.** An enter with `min_complete` = N is woken once N plain `READ`/`WRITE` completions are queued, rather than for each one, which saves context switches more than it adds IOPS. On the md disk at 32 in flight:

| Workload | Wait for 1 | Wait for 16 |
|---|---:|---:|
| Reads | 649,074 | 697,841 |
| Writes | 555,195 | 592,109 |

**`SQPOLL`.** The `SQPOLL` thread polls for the whole of `sq_thread_idle` while it's active, as io_uring's does. `-C` also polls the CQ from user space instead of entering to wait, which is how a latency-bound program would use it. 4 KiB random reads on the md disk:

| In flight | plain kqring | `SQPOLL` | `SQPOLL`, `-C` |
|---:|---:|---:|---:|
| 1 | 253,199 | 190,004 | **258,200** |
| 4 | 652,597 | 674,535 | **701,932** |
| 32 | 648,462 | 676,915 | **694,652** |

`kqring_diskbench -V` checks every read against what was last written to that block and re-reads the whole area at the end. Mixed random reads and writes passed it on both the md disk and the ZFS file on this build.

### Several threads: one ring per thread

[`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) runs each workload on 1, 2, 4 and 8 threads, each pinned to its own CPU (CPUs 2n and 2n+1 are the two hyperthreads of one core) with its own ring, kqueue or socket pair. That's the thread-per-core model `SINGLE_ISSUER` rings are made for. Figures are millions of operations per second for all threads together, from runs of 2 s.

![Ping-pong with one ring per thread](docs/bench/threads.svg)

| Workload | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| `kevent(2)`, nothing to do | 9.42 | 13.64 | 27.30 | 53.99 |
| kqring `NOP`, one per enter | 5.11 | 6.40 | 12.80 | 25.34 |
| kqring `NOP`, 32 per enter | 12.80 | 16.99 | 33.80 | 67.10 |
| AF_UNIX ping-pong, 64 B, kqueue | 1.86 | 2.54 | 5.08 | 10.05 |
| AF_UNIX ping-pong, 64 B, kqring `RECV` into one buffer | 1.77 | 2.43 | 4.80 | 9.53 |

Ping-pong is behind kqueue at every thread count, by 4–6%, as in the single-threaded group. Thread n runs on CPU n, so from 2 threads on, each pair shares a core: that is why 2 threads give well under twice the rate of 1, and 4 and 8 about double it each time. The benchmark's shared-file workload isn't shown: its file is in `/tmp`, which is ZFS here, with the two speeds described under [Pipes and files](#pipes-and-files-inline-or-through-a-worker).

### fio on a real SSD

[fio](fio/README.md) 3.43 with its own `io_uring` engine on kqring, against `posixaio` (`aio(4)`) and `psync`, on the SanDisk SSD. Each figure is the median of three 20-second runs after a 2-second ramp. `psync` runs one thread per I/O in flight. Before each read run, the script waits until the disk has had no writes for 5 seconds. ZFS keeps flushing random writes for a while after they return, and while it does, the SSD stalls reads for up to a second and more, whatever the engine: one in flight, the longest read took 1.4–3.1 ms on a quiet disk and 95–1,336 ms just after 20 seconds of ZFS writes, with kqring and with `psync` ([raw runs](docs/bench/data/stall/)). [`fio/bench.sh`](fio/bench.sh) reproduces them, and [`fio/summary.py`](fio/summary.py) turns its output into [`fio.csv`](docs/bench/data/fio.csv).

![CPU per engine on a SATA SSD](docs/bench/fio_cpu.svg)

**The raw disk, read-only** (the direct bio path), 4 KiB random reads:

| In flight | kqring IOPS | CPU | aio IOPS | CPU | `psync` IOPS |
|---:|---:|---:|---:|---:|---:|
| 1 | 8,696 | 3% | 8,682 | 3% | 8,744 |
| 8 | 52,547 | 17% | 52,383 | 22% | 52,642 |
| 32 | 84,952 | **25%** | 80,655 | 31% | 85,055 |

At 1 and 8 in flight, the three engines get the same IOPS from the disk, within 0.7% of each other. At 32, kqring and `psync` still do, and aio is 5% behind, at more CPU. At 32 in flight, `--fixedbufs --registerfiles` gives 85,042 IOPS and `--sqthread_poll` 85,001. Sequential 128 KiB reads run at 510 MB/s through kqring and 511 MB/s through aio, with kqring at 2% CPU against 3%.

**A file on ZFS** (the worker path), 8 GiB, 4 KiB records, `primarycache=metadata` so that reads reach the disk. FreeBSD 15.1 has ZFS direct I/O off by default (`vfs.zfs.dio_enabled=0`), so `--direct=1` still goes through ZFS's buffers:

| Random 4 KiB | In flight | kqring IOPS | CPU | p99 µs | aio IOPS | CPU | p99 µs | `psync` IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Reads | 1 | 7,519 | 28% | 132 | 7,506 | 2% | 165 | 7,642 |
| Reads | 32 | 58,368 | 14% | 709 | 58,305 | 23% | 717 | 58,376 |
| Writes | 1 | 18,065 | 22% | 1,057 | 12,794 | 4% | 1,106 | 8,083 |
| Writes | 32 | 8,600 | 2% | 21,365 | 7,816 | 3% | 25,035 | 6,461 |

Reads reach the same IOPS through every engine, within 2% of each other, and kqring uses less CPU than aio at 32 in flight. One at a time, kqring's issuer spins briefly for the worker rather than sleeping, which is the CPU in the rows for 1. Writes vary between runs, up to 36% of the median, against at most 2% for reads, so their differences between engines don't mean much. ZFS gathers writes into transaction groups, and the flushes of those to the disk most likely set the pace.

### fio with several rings

One ring per thread, 4 KiB random reads, on targets the SSD doesn't limit: [`fio/scale.sh`](fio/scale.sh). `psync` here is one thread per job, so it has 1/16 of the others' I/Os in flight. Medians of three 10-second runs, in thousands of IOPS.

<p align="center">
  <img src="docs/bench/fio_scale.svg" alt="fio on md(4) disks by jobs" width="49%">
  <img src="docs/bench/fio_cached.svg" alt="fio on a cached ZFS file by jobs" width="49%">
</p>

| Jobs | ZFS, in the ARC: kqring | aio | `psync` | `md(4)`, a disk per job: kqring | aio |
|---:|---:|---:|---:|---:|---:|
| 1 | 892 | 726 | 400 | 690 | 704 |
| 2 | 1,069 | 731 | 688 | 1,229 | 1,018 |
| 4 | 1,095 | 657 | 1,200 | 2,021 | 601 |
| 8 | 1,502 | 564 | 1,662 | | |

Below 8 jobs kqring's workers read a ring's requests in parallel; at 8 the reads run in the submitting threads, as the CPUs are taken ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)). From 4 jobs, `psync` is ahead, since a thread reading the ARC itself needs no hand-off and no ring; with several reads per enter, kqring passes it up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).

## Known gaps

- **A pipe read that has to wait goes through a worker thread**, as does a write to a full pipe. Waiting for data with `POLL_ADD` first avoids the worker.
- **Writes to one file run one at a time.** They keep their order by a bucket of the file, as io-wq's per-inode hashing does, and the issuer doesn't wait for them; see [docs/write-ordering.md](docs/write-ordering.md).
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers file reads that need the disk, ZFS, `O_DIRECT` on files, and reads using the file position (`off` = −1). Only disk devices themselves get the direct bio path: FreeBSD filesystems have no asynchronous read or write entry point. It also covers reads into a buffer that isn't mapped yet.
- **From 4 jobs on cached ZFS data, `psync` beats one read per enter.** A thread reading the ARC itself needs no hand-off. kqring runs the reads in the submitting thread once the readers take most of the CPUs, and with 4 to 16 reads per enter it's ahead of `psync` up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).
- **One operation per enter costs more than an empty `kevent`:** +80% on this machine. That's the `NOP` floor: the enter, the SQE, the request and the CQE. Where system calls are cheap, as here, that cost outweighs the calls kqring saves on single requests and on fan-in ([Why the results depend on the CPU](#why-the-results-depend-on-the-cpu)). Batching removes it.
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
./tests/kqring_diskbench -e psync -j 32 -w randwrite /dev/md9   # psync: -j threads, -q is ignored
dd if=/dev/random of=$HOME/dbench.dat bs=1m count=256
./tests/kqring_diskbench -e kqring -q 16 -w randread $HOME/dbench.dat
./tests/kqring_diskbench -V -e kqring -F -B -w randrw /dev/md9   # verify
```

It refuses to write to a device unless it is an md(4) disk or `-f` is given. `-B` registers `-q` × `-b` bytes per job, so raise `ulimit -l` for it.
