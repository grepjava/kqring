# kqring benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads, and with `aio(4)` and plain system calls on storage. Where kqueue has no role (regular files, disks, bulk sends into a socket with room), the comparison is the plain system call.

Every figure here is from one build, `28e67b2`, on the machine under [Setup](#setup), an 8-thread AMD Ryzen. The raw runs are in [`docs/bench/data/`](docs/bench/data/). Earlier editions were measured on an Intel laptop; how the two machines differ, and why it matters for kqring, is under [Why the results depend on the CPU](#why-the-results-depend-on-the-cpu). For kqring against Linux io_uring on one machine, see [docs/linux-comparison.md](docs/linux-comparison.md).

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Batching is where a ring wins.** 32 `NOP`s per enter cost 71 ns each, against 105 ns for an empty `kevent(2)`. One `NOP` per enter costs 182 ns, +74% against `kevent(2)`: that's kqring's fixed cost per enter, the SQE, the request and the CQE.
- **Where a system call is cheap, as on this machine, kqring is about level with kqueue on sockets.** Collecting many connections' messages (fan-in) is between 4% slower and 6% faster than kqueue: the system calls kqring saves, one for N messages where kqueue makes N + 1, pay for about what it does per message. On one connection, `RECV` into one reused buffer is level with kqueue at 64 B, and a pipe `READ` with data ready 9% slower. On the Intel laptop, where a system call costs nearly three times as much, kqring was ahead: see [below](#why-the-results-depend-on-the-cpu).
- **A one-shot `RECV` armed before its data arrives costs more than kqueue:** +68% at 64 B. It pays a second enter and installs and removes a socket upcall every time. Use multishot `RECV` for steady receive.
- **Disk devices go straight to the driver.** With 32 in flight on an md(4) disk, a 4 KiB read costs 1,475 ns, 57% less than `pread(2)`.
- **On a real SSD, kqring reaches what the drive can do with less CPU than aio(4):** 84,924 IOPS at 32 in flight on the raw disk at 25% CPU, against 31% for aio.
- **Rings on several threads scale.** On md(4) disks, 4 jobs reach 2,014,000 IOPS against 601,000 for aio. On a cached ZFS file, kqring reaches 1,501,000 at 8 jobs, ahead of aio; `psync` is ahead from 4 jobs.
- **Zero-copy send pays off for large messages.** From a registered buffer, `SEND_ZC` takes 53% less time than `write(2)` for 256 KiB on AF_UNIX, and 32% less on TCP loopback. When one thread sends and then reads, it costs more at 64 KiB and below; with the receiver in its own thread, it takes 35–54% less time from 16 KiB up ([Streaming](#streaming-the-receiver-in-its-own-thread)).
- **Cached file reads depend on the filesystem.** On UFS, 32 `READ`s per enter are level with `pread(2)`, and one per enter is +14%. ZFS has no page-cache read path, so its reads go through a worker or, once the CPUs are taken, run in the enter.
- **An event-driven chain needs less server CPU than kqueue:** 4–6% per message in an echo server.

## Why the results depend on the CPU

kqring saves system calls, and does more work per request than the call it replaces: it reads an SQE, sets up a request and posts a CQE. Which of the two wins depends on what a system call costs, and that depends on the CPU and its security mitigations. On the Intel i5-8250U laptop these tables used to come from, FreeBSD isolates the kernel's page tables (PTI) against Meltdown, and every system call pays for it; AMD's CPUs aren't affected, so the Ryzen doesn't. The same `kqring_bench` groups on both (the laptop's runs are from `v0.3.0`, in [`docs/bench/data-i5-8250u/`](docs/bench/data-i5-8250u/); ns per operation):

| | i5-8250U, PTI | Ryzen 3 PRO 4350G |
|---|---:|---:|
| Empty `kevent(2)` | 288 | 105 |
| kqring `NOP`, one per enter | 336 | 182 |
| kqring `NOP`, 32 per enter | 69 | 71 |
| AF_UNIX ping-pong, 64 B: kqueue | 1,117 | 541 |
| AF_UNIX ping-pong, 64 B: kqring `RECV`, one buffer | 881 | 543 |

The kqueue rows, which are mostly system calls, got much cheaper on the Ryzen; kqring's work per request less so. So `RECV` into one buffer, 21% faster than kqueue on the laptop, is level with kqueue on the Ryzen. That is the CPU more than kqring's changes since `v0.3.0`: `v0.3.0` and `7641bf4`, alternated on the Ryzen 5 runs each, differed by a median of 0.7% per row over these groups ([raw runs](docs/bench/data-v030-vs-head/)), and from `7641bf4` to this build one `NOP` per enter went from 187 to 182 ns and the one-buffer `RECV` from 550 to 543. Where kqueue makes many system calls per unit of work and each costs a lot, as with PTI, kqring's saving shows. Where calls are cheap, its fixed cost per enter decides; that is now level with io_uring's above each system's own floor ([docs/per-request-cost.md](docs/per-request-cost.md#on-the-machine-that-runs-both)). The laptop's fan-in isn't in the table: in `v0.3.0` `kqring_bench` gave kqring's receives 4,096 provided buffers of 4 KiB in turn, and its fan-in rows measured cache misses as much as the ring.

## Setup

| | |
|---|---|
| CPU | AMD Ryzen 3 PRO 4350G, 4 cores / 8 threads; no PTI (`vm.pmap.pti=0`) |
| Memory | 8 GB |
| Disk | SanDisk SD7TB6S 256 GB SATA SSD |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; root on ZFS (OpenZFS 2.4.2) |
| kqring | `kqring.ko` from `28e67b2`, entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries; one provided-buffer ring of 16 × 4 KiB for ping-pong, one 64-byte buffer per connection for fan-in, and 4,096 × 4 KiB for the other groups |
| Benchmarks | [`tests/kqring_bench.c`](tests/kqring_bench.c), [`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) and [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c), built with `cc -O2` |
| Limits | `ulimit -l 65536` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

## Method

- Each `kqring_bench` and `kqring_mt_bench` figure is the **median of 5 runs**, and each `kqring_diskbench` figure the median of 3 runs of 10 seconds. The module was unloaded and reloaded before each `kqring_bench` and `kqring_mt_bench` run.
- A run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- `kqring_bench` is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so it measures the per-operation cost of each mechanism, not throughput under load. The echo group uses two threads.
- 96 of 113 rows outside the streaming group vary less than 5% between runs. The others are in the floor group (up to 27%), sends (up to 13%), ping-pong (up to 9%), the pipe group (up to 5%) and fan-in (up to 5%). The file group's reads in `/tmp`, which is ZFS here, wait for the file's transaction group to sync and then read in ZFS's slow mode in every run; the ZFS table under [Pipes and files](#pipes-and-files-inline-or-through-a-worker) comes from separate runs at two dbuf cache sizes.
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py), and this document is written from them by [`tests/bench_doc.py`](tests/bench_doc.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain system call where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 105 |  |
| kqring `NOP`, one per enter | 182 | +74% |
| kqring `NOP`, 32 per enter | 71 | −33% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 750 | +615% |

A single enter costs 78 ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching spreads the rest over many requests. The `IOSQE_ASYNC` row is a full round trip through a worker thread, kept short by both sides spinning briefly when a CPU is free.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 350 | 399 | 462 |
| kqueue: `write` + `kevent` + `recv` | 541 | 601 | 831 |
| kqring `RECV`, provided buffer | 586 (+8%) | 636 (+6%) | 712 (−14%) |
| kqring `RECV`, one reused buffer | 543 (0%) | 599 (0%) | 829 (0%) |
| kqring multishot `RECV`, provided buffers | 544 (+1%) | 592 (−2%) | 677 (−19%) |
| kqring `RECV` armed before the data arrives | 910 (+68%) | 949 (+58%) | 1,036 (+25%) |

The provided-buffer variants take their buffers in turn from a ring of 16 × 4 KiB. Until phase 13 it was 4,096 × 4 KiB, 16 MiB, and every receive landed in memory out of cache; that was most of what these rows lost to kqueue.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 3,005 | |
| kqring `RECV`, provided buffer | 2,963 | −1% |
| kqring `RECV`, one reused buffer | 2,927 | −3% |
| kqring multishot `RECV` | 2,927 | −3% |
| kqring `RECV` armed before the data arrives | 3,366 | +12% |

TCP is dominated by the loopback stack (about 3 µs per message), so the mechanisms differ by a few percent.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 400 | 410 | 444 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **381 (−5%)** | **386 (−6%)** | 464 (+4%) |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 416 (+4%) | 413 (+1%) | **437 (−2%)** |

This is the shape of a busy server. kqring makes one system call for N messages, kqueue makes N + 1. On this machine, where a system call is cheap, that saving about pays for kqring's work per message, a CQE and a buffer; multishot is ahead up to 256 connections, and at 1,024 the armed receives' memory no longer fits the cache ([docs/fan-in.md](docs/fan-in.md#where-the-requests-lie)). Each receive gets a buffer of the message's size, the least a ring can receive N messages into; a group much larger than what is in flight would send every receive to memory out of cache.

### Echo server: an event-driven chain against kqueue

`kqring_bench echo`. A server thread echoes each message to a client in another thread, which sends one and waits for it. Each cell is round trip / server CPU per message, in ns.

![Echo server chart](docs/bench/echo.svg)

| Server | AF_UNIX 64 B | AF_UNIX 4 KiB | TCP 64 B |
|---|---:|---:|---:|
| kqueue: `kevent` + `read` + `write` | 3,606 / 1,855 | 3,949 / 2,065 | 6,295 / 2,353 |
| kqring io_uring style: each `RECV` CQE → submit `SEND` + next `RECV` | 3,673 / 2,012 | 3,983 / 2,141 | 6,501 / 2,553 |
| kqring trigger: `RECV` → `SEND`, `LEN_FROM_PREV`, no CQE per message | **3,395 / 1,780 | 3,692 / 1,932 | 6,122 / 2,267** |

Against kqueue, the trigger (an [event-driven chain](docs/event-chains-spec.md), opted into per ring) takes 4%, 6% and 4% less server CPU per message. The server never returns to user space between messages.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as a program must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 13.20 | 13.27 | 13.74 | 4.99 |
| kqring `SEND` | 11.25 (+17%) | 11.86 (+12%) | 12.35 (+11%) | 4.87 (+2%) |
| kqring `SEND_ZC`, registered buffer | 8.08 (+63%) | 9.78 (+36%) | 10.56 (+30%) | **10.57 (−53%)** |
| kqring `SEND_ZC`, unregistered buffer | 7.05 (+87%) | 8.87 (+50%) | 9.80 (+40%) | **9.95 (−50%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 3.75 | 5.54 | 6.05 | 3.86 |
| kqring `SEND` | 3.61 (+4%) | 5.34 (+4%) | 5.75 (+5%) | 3.80 (+2%) |
| kqring `SEND_ZC`, registered buffer | 3.17 (+18%) | 4.94 (+12%) | 5.35 (+13%) | **5.72 (−32%)** |
| kqring `SEND_ZC`, unregistered buffer | 3.05 (+23%) | 4.74 (+17%) | 5.14 (+18%) | **5.54 (−30%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default. TCP always uses one page.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread, so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing.

### Streaming: the receiver in its own thread

`kqring_bench stream`. The send group's variants, with a second thread draining the connection with `recv(2)` while the sender sends back to back. In the send group one thread sends and then reads, so on TCP loopback a small send mostly measures the hand-off to the netisr thread; here the two sides overlap, as in a real stream. `SEND_ZC` with `MSG_WAITALL` sends zero-copy across as many attempts as the socket needs.

Throughput in GB/s, with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 5.28 | 5.65 | 5.87 | 5.12 |
| kqring `SEND` | 4.61 (+15%) | 5.28 (+7%) | 5.76 (+2%) | 5.00 (+2%) |
| kqring `SEND_ZC`, registered buffer | **7.08 (−25%)** | **11.70 (−52%)** | **11.24 (−48%)** | **11.01 (−54%)** |
| kqring `SEND_ZC`, unregistered buffer | 3.52 (+50%) | 4.28 (+32%) | **12.21 (−52%)** | **10.98 (−53%)** |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | **7.02 (−25%)** | **11.91 (−53%)** | **12.79 (−54%)** | **11.43 (−55%)** |

**TCP loopback**

| Variant | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 3.66 | 5.05 | 5.06 | 4.57 |
| kqring `SEND` | 2.99 (+22%) | 4.40 (+15%) | 4.99 (+1%) | 4.49 (+2%) |
| kqring `SEND_ZC`, registered buffer | 3.93 (−7%) | **7.76 (−35%)** | **8.14 (−38%)** | **8.01 (−43%)** |
| kqring `SEND_ZC`, unregistered buffer | 3.38 (+8%) | **7.65 (−34%)** | **7.93 (−36%)** | **7.92 (−42%)** |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | 3.90 (−6%) | **8.52 (−41%)** | **8.26 (−39%)** | **8.04 (−43%)** |

The sending thread's CPU time per message, in µs:

| Variant | AF_UNIX 64 KiB | AF_UNIX 256 KiB | TCP 64 KiB | TCP 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 11.2 | 51.2 | 13.0 | 57.4 |
| kqring `SEND` | 11.4 | 52.5 | 13.1 | 58.4 |
| kqring `SEND_ZC`, registered buffer | 1.6 | 2.8 | 3.7 | 14.4 |
| kqring `SEND_ZC`, unregistered buffer | 2.0 | 3.9 | 4.1 | 14.8 |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | 1.6 | 2.8 | 3.7 | 11.6 |

- **With the receiver on its own thread, zero-copy pays off from small messages.** From a registered buffer, `SEND_ZC` takes 35–54% less time than `write(2)` from 16 KiB up, and 25% less on AF_UNIX and 7% less on TCP at 4 KiB, where in the send group it cost more up to 64 KiB. The receiver's copy then sets the pace: at 256 KiB on AF_UNIX the receiving thread is busy 100% of the time, and the sender spends 2.8 µs of CPU per message, against 51.2 µs for `write(2)`.
- **An unregistered buffer below 32 KiB on AF_UNIX is copied**, as in the send group, and costs more than `SEND`. From 64 KiB it is pinned and sent zero-copy, as fast as a registered one.
- **Without `MSG_WAITALL`, a send sends what the socket has room for.** On TCP at 256 KiB, a message took 3.2 `SEND_ZC`s, the program sending the rest each time, and 6% of messages had a remainder below a page, which kqring copies. With `MSG_WAITALL`, kqring sends the whole message from one SQE, zero-copy across as many attempts as the socket needs, with one notification; 6% of notifications reported a part copied. On TCP at 256 KiB it also needs the least of the sender, 11.6 µs per message against 14.4 µs, as it makes one submission per message.
- Rows vary up to 20% between runs; `write(2)` and `SEND` within 6%.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 7,044 | −3% |
| kqueue: `kevent` + `accept` | 7,271 | |
| kqring multishot `ACCEPT` | 7,430 | +2% |
| kqring multishot `ACCEPT`, 50 µs busy-poll | 7,285 | 0% |

The enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays for a wake-up. With `IORING_REGISTER_NAPI` set to 50 µs, multishot `ACCEPT` avoids the sleep, at the cost of CPU time on waits that don't end quickly.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 4,005 | |
| kqring `TIMEOUT`, 1 µs | 3,544 | −12% |

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
| `write` + `read`, no waiting (floor) | 260 | −40% |
| kqueue: `write` + `kevent` + `read` | 437 | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | 470 | +8% |
| kqring `READ`, blocking pipe, data ready (inline) | 476 | +9% |

**File on UFS: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`. The file is on a UFS file system on an md(4) disk.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 809 | |
| kqring `READ`, one per enter | 925 | +14% |
| kqring `READ`, one per enter, registered file | 930 | +15% |
| kqring `READ`, 32 per enter | **807** | **0%** |

A registered file saves almost nothing, so descriptor lookups aren't where the single-read gap comes from. It is the fixed cost of an enter, plus the SQE and CQE.

**File on ZFS: the same reads from a 64 MiB file in `/tmp`** (lz4, 128 KiB records). ZFS caches in the ARC rather than the page cache, so `READ` goes to a worker, or runs in the enter once the readers take most of the CPUs. ZFS serves these reads at one of two speeds, depending on whether the file fits in its dbuf cache, which holds records ready to read and is capped at 1/32 of the ARC target (`vfs.zfs.dbuf.cache_shift=5`). Here the ARC target was 1,111 MiB, so the cache held 34 MiB, and the file didn't fit; at 1/8 (`cache_shift=3`, 138 MiB) it does. The runs alternated between the two, 5 of each, with `kqring.ko` from `28e67b2` ([raw runs](docs/bench/data/zfs-dbuf/)); every row varies less than 4% between runs.

| Variant | Fits: ns/op | vs `pread` | Doesn't fit: ns/op | vs `pread` |
|---|---:|---:|---:|---:|
| `pread(2)` | 1,284 |  | 16,849 |  |
| kqring `READ`, one per enter | 2,099 | 64% slower | 17,652 | 5% slower |
| kqring `READ`, one per enter, registered file | 2,060 | 61% slower | 17,647 | 5% slower |
| kqring `READ`, 32 per enter | 779 | 39% faster | 10,448 | 38% faster |

When the file fits, one `READ` per enter pays the worker hand-off on top of `pread(2)`, and 32 per enter more than make it up. When it doesn't, each 4 KiB read evicts a record from the cache and rebuilds a whole 128 KiB one from the ARC, and that dominates every variant; 32 per enter still win, since the rebuilds run on several workers at once. A kernel profile of `pread(2)` alone in that state ([`tests/zfs_dbuf.c`](tests/zfs_dbuf.c)) spends 88% of its samples in lz4 decompression for this all-`'f'` file and 85% in `memcpy` for incompressible data, both at about the same time per read.

A file just written reads fast either way until its transaction group syncs (`vfs.zfs.txg.timeout`, 5 s): its dirty records are still in memory whole. `zfs_dbuf` shows the switch at the sync, from 973 to 17,574 ns a read; in its two runs, reads were still fast 1.8 and 3.9 s after the write and slow at the next sample. Earlier editions of this group read within a second of writing the file, so whether the sync landed inside the group was luck. That was the fast and slow mode of earlier runs, including the [`v0.3.0` against `7641bf4` runs](docs/bench/data-v030-vs-head/), where reads were slow from some point in the group on in 4 of the 10 runs, with both builds. The group now waits for the sync on ZFS and prints the dbuf cache target with each run. The ARC target, and with it the dbuf cache, shrinks under memory pressure and grows back only as the ARC fills: when these runs were made, after the other benchmarks, the ARC held about 170 MiB, its target was 1,111 MiB, and its ceiling 6.3 GiB. For ZFS under load, the [fio tables](#fio-on-a-real-ssd) are the better guide.

### Disk device: 4 KiB random reads and writes

A positioned `READ` or `WRITE` on a disk device goes straight to the driver as a bio over the held user pages, as `aio(4)` does, and completes from the bio callback. This is an md(4) malloc disk with every block written first, so it measures the I/O path, not media. A block that was never written costs md far less to read, so an empty disk gives figures that are too good.

| Variant | Read | vs `pread` | Write | vs `pwrite` |
|---|---:|---:|---:|---:|
| `pread(2)` / `pwrite(2)` | 3,442 | | 3,629 | |
| kqring, one per enter, to the driver | 3,794 | +10% | 3,899 | +7% |
| kqring, one per enter, `IOSQE_ASYNC` through a worker | 3,667 | +7% | 3,856 | +6% |
| kqring, 32 per enter, to the driver | **1,475** | **−57%** | **1,664** | **−54%** |
| kqring, 32 per enter, `IOSQE_ASYNC` through workers | 1,869 | −46% | | |

With one request per enter, kqring pays one sleep and wake-up more than `pread(2)`, since the owner waits in `kqring_enter()` for the bio callback's taskwork. With 32 in flight, the driver gets them all at once, which `pread(2)` can't do from one thread.

### Storage workloads: queue depth, aio(4) and `pread(2)` threads

[`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) runs storage-benchmark-style workloads: a fixed number of I/Os in flight per job, random or sequential offsets, for a fixed time after a 1 s ramp, through kqring, `aio(4)` or threads calling `pread(2)` / `pwrite(2)`. All figures are IOPS.

**A 1 GiB md(4) malloc disk, 4 KiB random I/O** (the bio path; md serves every bio from one thread). `+F+B` is a fixed file and registered buffers, `+P` `SQPOLL` and `+I` `IOPOLL`:

| Variant | Read, 1 | Read, 32 | Write, 1 | Write, 32 |
|---|---:|---:|---:|---:|
| `pread` / `pwrite` threads | 271,462 | 271,716 | 255,270 | 254,916 |
| aio | 253,524 | 649,718 | 245,869 | 570,828 |
| kqring | 254,221 | 649,998 | 246,388 | 571,596 |
| kqring `+F+B` | 259,908 | 662,780 | 252,844 | 581,158 |
| kqring `+P` | 190,891 | 681,586 | 185,618 | 600,002 |
| kqring `+I` | 254,068 | 650,759 | 246,822 | 573,140 |

At 32 in flight, kqring's reads and writes are level with aio's; rounds differed by at most 0.4%. One at a time, a thread calling `pread(2)` is 7% faster, as in the disk group above. With 32 in flight, 32 threads calling `pread(2)` or `pwrite(2)` are within 139% of both, for reads and for writes, which suggests md's one thread is the limit there.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring | kqring p99 µs |
|---:|---:|---:|---:|---:|
| 1 | 80,629 | 67,629 | 75,965 | 24 |
| 4 | 80,734 | 111,106 | 111,817 | 68 |
| 16 | 81,072 | 108,381 | 112,771 | 188 |
| 64 | 80,866 | 107,801 (at 32) | 112,083 | 655 |

From 4 in flight, kqring is 39–39% ahead of as many `pread(2)` threads; one at a time it's 6% behind a thread calling `pread(2)`. aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`).

**Waiting for several completions.** An enter with `min_complete` = N is woken once N plain `READ`/`WRITE` completions are queued, rather than for each one, which saves context switches more than it adds IOPS. On the md disk at 32 in flight:

| Workload | Wait for 1 | Wait for 16 |
|---|---:|---:|
| Reads | 650,817 | 702,010 |
| Writes | 570,296 | 606,669 |

**`SQPOLL`.** The `SQPOLL` thread polls for the whole of `sq_thread_idle` while it's active, as io_uring's does. `-C` also polls the CQ from user space instead of entering to wait, which is how a latency-bound program would use it. 4 KiB random reads on the md disk:

| In flight | plain kqring | `SQPOLL` | `SQPOLL`, `-C` |
|---:|---:|---:|---:|
| 1 | 254,376 | 190,873 | **264,601** |
| 4 | 656,666 | 675,346 | **705,209** |
| 32 | 651,008 | 680,533 | **693,319** |

`kqring_diskbench -V` checks every read against what was last written to that block and re-reads the whole area at the end. Mixed random reads and writes passed it on both the md disk and the ZFS file on this build.

### Several threads: one ring per thread

[`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) runs each workload on 1, 2, 4 and 8 threads, each pinned to its own CPU (CPUs 2n and 2n+1 are the two hyperthreads of one core) with its own ring, kqueue or socket pair. That's the thread-per-core model `SINGLE_ISSUER` rings are made for. Figures are millions of operations per second for all threads together, from runs of 2 s.

![Ping-pong with one ring per thread](docs/bench/threads.svg)

| Workload | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| `kevent(2)`, nothing to do | 9.46 | 13.65 | 27.30 | 54.12 |
| kqring `NOP`, one per enter | 5.53 | 6.78 | 13.48 | 26.80 |
| kqring `NOP`, 32 per enter | 13.92 | 15.29 | 30.57 | 60.41 |
| AF_UNIX ping-pong, 64 B, kqueue | 1.85 | 2.54 | 5.06 | 10.06 |
| AF_UNIX ping-pong, 64 B, kqring `RECV` into one buffer | 1.84 | 2.52 | 5.01 | 9.93 |

Ping-pong is behind kqueue at every thread count, by 1–1%, as in the single-threaded group. Thread n runs on CPU n, so from 2 threads on, each pair shares a core: that is why 2 threads give well under twice the rate of 1, and 4 and 8 about double it each time. The benchmark's shared-file workload isn't shown: its file is in `/tmp`, which is ZFS here, with the two speeds described under [Pipes and files](#pipes-and-files-inline-or-through-a-worker).

### fio on a real SSD

[fio](fio/README.md) 3.43 with its own `io_uring` engine on kqring, against `posixaio` (`aio(4)`) and `psync`, on the SanDisk SSD. Each figure is the median of three 20-second runs after a 2-second ramp. `psync` runs one thread per I/O in flight. Before each read run, the script waits until the disk has had no writes for 5 seconds. ZFS keeps flushing random writes for a while after they return, and while it does, the SSD stalls reads for up to a second and more, whatever the engine: one in flight, the longest read took 1.4–3.1 ms on a quiet disk and 95–1,336 ms just after 20 seconds of ZFS writes, with kqring and with `psync` ([raw runs](docs/bench/data/stall/)). [`fio/bench.sh`](fio/bench.sh) reproduces them, and [`fio/summary.py`](fio/summary.py) turns its output into [`fio.csv`](docs/bench/data/fio.csv).

![CPU per engine on a SATA SSD](docs/bench/fio_cpu.svg)

**The raw disk, read-only** (the direct bio path), 4 KiB random reads:

| In flight | kqring IOPS | CPU | aio IOPS | CPU | `psync` IOPS |
|---:|---:|---:|---:|---:|---:|
| 1 | 8,662 | 3% | 8,649 | 3% | 8,701 |
| 8 | 52,355 | 16% | 52,153 | 21% | 52,479 |
| 32 | 84,924 | **25%** | 80,243 | 31% | 84,837 |

At 1 and 8 in flight, the three engines get the same IOPS from the disk, within 0.6% of each other. At 32, kqring and `psync` still do, and aio is 6% behind, at more CPU. At 32 in flight, `--fixedbufs --registerfiles` gives 84,892 IOPS and `--sqthread_poll` 84,892. Sequential 128 KiB reads run at 513 MB/s through kqring and 512 MB/s through aio, both at 2% CPU.

**A file on ZFS** (the worker path), 8 GiB, 4 KiB records, `primarycache=metadata` so that reads reach the disk. FreeBSD 15.1 has ZFS direct I/O off by default (`vfs.zfs.dio_enabled=0`), so `--direct=1` still goes through ZFS's buffers:

| Random 4 KiB | In flight | kqring IOPS | CPU | p99 µs | aio IOPS | CPU | p99 µs | `psync` IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Reads | 1 | 7,564 | 28% | 132 | 7,546 | 2% | 165 | 7,647 |
| Reads | 32 | 58,404 | 14% | 709 | 58,364 | 23% | 717 | 58,430 |
| Writes | 1 | 19,505 | 22% | 1,057 | 10,765 | 3% | 1,106 | 10,252 |
| Writes | 32 | 10,837 | 3% | 19,268 | 9,904 | 4% | 19,530 | 8,948 |

Reads reach the same IOPS through every engine, within 1% of each other, and kqring uses less CPU than aio at 32 in flight. One at a time, kqring's issuer spins briefly for the worker rather than sleeping, which is the CPU in the rows for 1. Writes vary between runs, up to 36% of the median, against at most 3% for reads, so their differences between engines don't mean much. ZFS gathers writes into transaction groups, and the flushes of those to the disk most likely set the pace.

### fio with several rings

One ring per thread, 4 KiB random reads, on targets the SSD doesn't limit: [`fio/scale.sh`](fio/scale.sh). `psync` here is one thread per job, so it has 1/16 of the others' I/Os in flight. Medians of three 10-second runs, in thousands of IOPS.

<p align="center">
  <img src="docs/bench/fio_scale.svg" alt="fio on md(4) disks by jobs" width="49%">
  <img src="docs/bench/fio_cached.svg" alt="fio on a cached ZFS file by jobs" width="49%">
</p>

| Jobs | ZFS, in the ARC: kqring | aio | `psync` | `md(4)`, a disk per job: kqring | aio |
|---:|---:|---:|---:|---:|---:|
| 1 | 893 | 725 | 396 | 687 | 703 |
| 2 | 1,066 | 727 | 681 | 1,239 | 1,018 |
| 4 | 1,090 | 654 | 1,210 | 2,014 | 601 |
| 8 | 1,501 | 564 | 1,664 | | |

Below 8 jobs kqring's workers read a ring's requests in parallel; at 8 the reads run in the submitting threads, as the CPUs are taken ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)). From 4 jobs, `psync` is ahead, since a thread reading the ARC itself needs no hand-off and no ring; with several reads per enter, kqring passes it up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).

## Known gaps

- **A pipe read that has to wait goes through a worker thread**, as does a write to a full pipe. Waiting for data with `POLL_ADD` first avoids the worker.
- **Writes to one file run one at a time.** They keep their order by a bucket of the file, as io-wq's per-inode hashing does, and the issuer doesn't wait for them; see [docs/write-ordering.md](docs/write-ordering.md).
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers file reads that need the disk, ZFS, `O_DIRECT` on files, and reads using the file position (`off` = −1). Only disk devices themselves get the direct bio path: FreeBSD filesystems have no asynchronous read or write entry point. It also covers reads into a buffer that isn't mapped yet.
- **From 4 jobs on cached ZFS data, `psync` beats one read per enter.** A thread reading the ARC itself needs no hand-off. kqring runs the reads in the submitting thread once the readers take most of the CPUs, and with 4 to 16 reads per enter it's ahead of `psync` up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).
- **One operation per enter costs more than an empty `kevent`:** +74% on this machine. That's the `NOP` floor: the enter, the SQE, the request and the CQE. Where system calls are cheap, as here, that cost outweighs the call kqring saves on a single request, and on fan-in it about pays for the calls saved ([Why the results depend on the CPU](#why-the-results-depend-on-the-cpu)). Batching removes it.
- **A wait that just misses its completion pays a full sleep.** When the completion comes from another thread (the TCP loopback netisr, for instance) a moment after the enter checks, the enter sleeps and pays for a wake-up. `IORING_REGISTER_NAPI` opts in to a busy-wait before sleeping.
- **An armed one-shot `RECV` costs more than kqueue.** What remains is the extra `kqring_enter()` the pattern makes, plus the `EAGAIN` probe before arming. Use multishot `RECV` for steady receive.
- **`RLIMIT_MEMLOCK` defaults to 64 KiB on FreeBSD.** Registering buffers holds and charges their pages, so `IORING_REGISTER_BUFFERS` fails with `ENOMEM` beyond 64 KiB in total unless the limit is raised, through `login.conf(5)` (`memorylocked`) or `ulimit -l`. Unregistered `SEND_ZC` silently falls back to copying when the limit is reached. Linux's default is 8 MiB.
- **Provided buffers are handed out in turn.** A buffer group much larger than what is in flight sends every receive to memory out of cache; programs should size buffer rings with the cache in mind.

## Reproducing

On the FreeBSD host, from the repository root:

```sh
make
sudo kldload ./kmod/kqring.ko
ulimit -l 65536                   # the send group registers 2 MiB of buffers
./tests/kqring_bench > bench.1.out           # all groups
./tests/kqring_bench fanin send   # selected groups: floor pingpong fanin accept pipe file send timer disk echo stream
./tests/kqring_mt_bench 2 > mt.1.out         # several threads, 2 s per run
sudo kldunload kqring
```

If the hard limit is 64 KiB, raise it as root first, for example: `sudo sh -c 'ulimit -l 65536; exec sudo -u "$USER" ./tests/kqring_bench'`.

Each line of output is `ROW <group> <variant> <ns/op>`. To match these tables, run it 5 times, reloading the module before each run. Then produce the medians and charts on any machine with Python 3:

```sh
python3 tests/bench_report.py docs/bench docs/bench/data/*
python3 tests/bench_doc.py
```

Runs on other file systems and disks are selected by name and environment: `KQRING_BENCH_FILE_DIR` puts the `file` group's 64 MiB temporary file in a directory of your choice (these tables use a UFS file system on an md(4) disk, saved as `ufs.*.out`), and the `disk` group runs only with `KQRING_BENCH_DISK` set to a device it may overwrite (`sudo mdconfig -a -t malloc -s 64m -u 9 && sudo chown $USER /dev/md9`, filled with `dd if=/dev/urandom of=/dev/md9 bs=1m`, saved as `disk.*.out`). On ZFS the `file` group first waits for the file's transaction group to sync, and prints the dbuf cache target: the ZFS table's two columns are runs of `./tests/kqring_bench file` with `vfs.zfs.dbuf.cache_shift` at 3 and at 5, alternated ([`run.sh`](docs/bench/data/zfs-dbuf/run.sh)). `./tests/zfs_dbuf [-r]` shows the switch at the sync with `pread(2)` alone, without kqring.

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
