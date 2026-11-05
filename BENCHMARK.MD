# kqring vs kqueue benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads. Where kqueue has no role (regular files, bulk sends into a socket with room), the comparison is the plain syscall.

> Follow-up (2026-11-02): [targeted optimization measurements](docs/bench/red-cases/README.md) show registered AF_UNIX 16 KiB sends now beating write(2); armed one-shot receives improve modestly, and single file reads remain slower than pread. The full results below predate that follow-up.

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is 18–21% faster.** When many sockets become ready at once, one `kqring_enter()` reaps every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`. Across 16, 256 and 1,024 connections, multishot `RECV` and batched single-shot `RECV` beat kqueue by 18–21% (`648–710 ns` vs `823–865 ns`).
- **One connection: faster across all message sizes.** On AF_UNIX, a multishot `RECV` is 18% faster than kqueue at 64 B (`915 ns` vs `1,118 ns`), 12% at 1 KiB and 4% at 4 KiB. Receiving with a provided-buffer ring is up to 15% faster (`954 ns`), and receiving into one reused buffer is 20–22% faster at every size tested (`869 ns` at 64 B).
- **Armed one-shot `RECV` overhead cut from +37% to +23%.** A one-shot `RECV` submitted before its data arrives costs 23% more than kqueue at 64 B (`1,378 ns` vs `1,118 ns`, down from `1,523 ns` / +37%), 31–34% more at 1–4 KiB, and 10% more on TCP loopback (`6,494 ns` vs `5,904 ns`), because it takes an extra enter and installs and removes a socket upcall each time. Detaching one-shot waiters inside `kqring_soupcall()` under `ctx->mtx` and recycling `kqring_sowatch` entries per ring cut 145 ns/op (~36% of the gap over kqueue). Use multishot `RECV` for steady-state receive.
- **Cached file reads beat `pread(2)` on UFS when batched; ZFS goes through a worker.** On UFS (`VOP_READ_PGCACHE`), 32 `READ`s per enter cost 1,041 ns each, 14% less than `pread(2)` (`1,217 ns`), and a single `READ` per enter costs 12–14% more than `pread(2)`. On ZFS (which caches in the ARC rather than `VOP_READ_PGCACHE`), file reads go to a worker thread (`2,151 ns` batched / `2,432 ns` single vs `1,657 ns` `pread(2)`).
- **Zero-copy send pays off from 32–64 KiB.** `SEND_ZC` from a registered buffer is 35–38% faster than `write(2)` for 256 KiB messages on AF_UNIX and 25–27% faster on TCP loopback, and 16–21% faster at 64 KiB. At 16–32 KiB it ranges from 6% faster to 10% slower than `write(2)`. On AF_UNIX, kqring copies small sends from unregistered buffers itself, because that's cheaper there.
- **Batching and worker hand-off.** 32 `NOP`s per enter cost 63 ns each, against 296 ns for an empty `kevent(2)` and 326 ns for a single `NOP` enter. Bounded adaptive spinning between the issuer and worker threads cut the `IOSQE_ASYNC` worker round trip from `4,371 ns` to `1,049 ns` (4.2× faster).
- **Timers are faster, accept is even (or 8% faster with busy-polling).** A 1 µs `TIMEOUT` completes 30–52% faster than a 1 µs `EVFILT_TIMER` (`4,904 ns`). A multishot `ACCEPT` is on par with kqueue plus `accept(2)`, and 8% faster with 50 µs `IORING_REGISTER_NAPI` busy-polling; see [Accept](#accept-a-tcp-loopback-connection-set-up-and-torn-down).
- **Disk devices go straight to the driver.** 4 KiB random reads on an md(4) disk cost 41% less than through a worker, and 32 in flight cost 975 ns each, 78% less than `pread(2)`.
- **On a real SSD, fio reaches what the drive can do with half of aio(4)'s CPU.** 98,876 IOPS at 32 in flight on the raw disk at 45% CPU, against 97% for aio. On a ZFS file it runs within 5% of aio at less than half the CPU.
- **Rings on several threads keep scaling.** Reads through the worker pool reach 532,000 IOPS at 8 jobs where they used to collapse to 108,000, ahead of aio at every job count. On md(4) disks, 4 jobs reach 1,031,000 against 323,000 for aio.
- **Queued storage I/O keeps up with aio(4).** At 32 in flight on an md(4) disk, kqring matches aio(4) on 4 KiB random reads and is 14% ahead on writes. `O_DIRECT` reads of a ZFS file now use several workers at once: 41–53% more IOPS at 4–64 in flight than the previous build, at ZFS's own ceiling with aio and `pread` threads. See [Storage workloads](#storage-workloads-queue-depth-aio4-and-pread2-threads).
- **Pipes: 18–19% faster than kqueue.** A `READ` on a pipe that has data runs inline, blocking or not (`832–840 ns` vs `1,024 ns`). Only a read that has to wait goes to a worker thread.
- **Rings scale with threads.** With one ring per thread, single-`NOP` enters reach 12.5 M/s on 8 threads (6.5 M/s before this build) and ping-pong stays 17–29% ahead of kqueue at every thread count; see [Multi-threaded scaling](#multi-threaded-scaling-one-ring-per-thread).

## What changed in this build

Against the previous published build:

1. **`kqring_enter(2)` scales across threads.** A module's system calls are counted in their `sysent` entry so that unloading can wait for them: two atomic compare-and-swaps per call on one word every CPU shares. kqring now counts enter calls per CPU instead; see [Multi-threaded scaling](#multi-threaded-scaling-one-ring-per-thread).
2. **Pipes with data are read inline, blocking or not**, as are writes to pipes with room. Before, a blocking pipe always went through a worker thread.
3. **`POLL_ADD` on pipes, eventfds and other non-socket files** waits as a knote on a kqueue the ring owns, served by one kernel thread per ring, instead of one blocking worker per poll. It isn't in these benchmarks.
4. **liburing compatibility work**: rings shared by the threads of a process, provided-buffer receives that fail `-ENOBUFS` on an empty group as on Linux, bundle sends, and ring descriptors that can't be passed over UNIX sockets. The `-ENOBUFS` check costs an idle socket's receive a few tens of nanoseconds, which shows in the armed `RECV` row.

5. **Socket-upcall detach, single-lock taskwork drain, KVA-pinned buffer rings, and adaptive worker spinning**:
   - One-shot single-direction socket waiters (`RECV`/`SEND`) are detached and their upcall cleared directly inside `kqring_soupcall()` while `SOCK_BUF_LOCK` and `ctx->mtx` are held, and `struct kqring_sowatch` entries are recycled per ring (`sw_cache`).
   - Single-issuer rings without active workers skip `ctx->bg_sx` locking on provided-buffer selection.
   - User-memory `PBUF_RING` pages are pinned into KVA at registration (`bg->ring_kva`, charged to `kern.kqring.user_ring_mem` and `vm.max_user_wired`, with a post-`fork` copy-on-write fallback to `copyin()`), replacing per-operation `copyin(9)` with a direct memory load.
   - Worker threads and synchronous `kqwqsy` waits spin adaptively before sleeping (`docs/worker-handoff-spec.md`), cutting `IOSQE_ASYNC` round-trip latency by 4.2×.

| Variant | Before | Previous | Now | Change (vs Before) |
|---|---:|---:|---:|---:|
| `NOP`, one per enter | 345 | 324 | **326** | −6% |
| `NOP`, 32 per enter | 77 | 64 | **63** | −18% |
| `NOP` with `IOSQE_ASYNC`, through a worker | 4,443 | 4,371 | **1,049** | **−76%** |
| AF_UNIX `RECV`, provided buffer, 64 B | 1,038 | 983 | **954** | −8% |
| AF_UNIX `RECV`, one reused buffer, 64 B | 886 | 863 | **869** | −2% |
| AF_UNIX multishot `RECV`, 64 B | 964 | 948 | **915** | **−5%** |
| AF_UNIX `RECV` armed before the data, 64 B | 1,474 | 1,523 | **1,378** | **−7% (−10% vs 1,523)** |
| Fan-in, 256 sockets, multishot `RECV` | 717 | 720 | **672** | **−6%** |
| `READ`, blocking pipe | 5,410 | 850 | **840** | **−84%** |
| `READ`, cached file (UFS), 32 per enter | 791 | 786 | **786** | −1% |
| `NOP`, one per enter, 8 threads (M/s, higher is better) | 6.5 | 12.5 | **12.5** | +92% |

## Setup

| | |
|---|---|
| CPU | Intel Core i5-8250U, 4 cores / 8 threads, 1.6 GHz base |
| Memory | 4 GiB |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; files on UFS |
| kqring | `kqring.ko` built from `main`, entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmark | [`tests/kqring_bench.c`](tests/kqring_bench.c), built with `cc -O2` |
| Limits | `ulimit -l 8192` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

The functional suite ([`tests/kqring_test.c`](tests/kqring_test.c)) passed 76/76 in each of 10 passes on the same build, with the module reloaded before each pass. After every pass and every benchmark run, `vmstat -m` showed no kqring memory in use, and `vm.stats.vm.v_user_wire_count` and `kern.openfiles` were back to their starting values.

Rechecked on `a94ff6d` with the same method: every single-threaded row is within 4.3% of these medians, most within 3%. A `NOP` enter costs 5–15 ns more (1.5–3%) at every thread count, and socket receive round trips 1–2% more, from the per-enter checks added since: CQ backpressure, and posting polls woken during the enter. The tables are from the earlier build.

## Method

- Each figure is the **median of 5 runs**. The module was unloaded and reloaded before each run.
- Each run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- Everything is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so this measures the per-operation cost of each mechanism, not throughput under load or wake-up latency across threads.
- Most rows vary less than 3% between runs. The exceptions are the single file reads (24%) and `pread` (15%), the TCP sends of 64 KiB and more (up to 13%), the kqueue timer (10%), the blocking `accept(2)` (5%), the 32 KiB and 256 KiB AF_UNIX sends (4%), the batched file reads (4%) and the empty `kevent` (4%).
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain syscall where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 296 | |
| kqring `NOP`, one per enter | 326 | +10% |
| kqring `NOP`, 32 per enter | 63 | −79% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 1,049 | +254% |

A single kqring enter costs 30 ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Through the `ioctl(2)` fallback it costs about 365 ns. Batching amortises the rest quickly. The `IOSQE_ASYNC` row is the cost of a round trip through a worker thread (down from 4,371 ns with bounded adaptive spinning).

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 722 | 752 | 838 |
| kqueue: `write` + `kevent` + `recv` | 1,118 | 1,156 | 1,270 |
| kqring `RECV`, provided buffer | 954 (−15%) | 1,060 (−8%) | 1,242 (−2%) |
| kqring `RECV`, one reused buffer | **869 (−22%)** | **908 (−21%)** | **1,019 (−20%)** |
| kqring multishot `RECV`, provided buffers | 915 (−18%) | 1,022 (−12%) | 1,214 (−4%) |
| kqring `RECV` armed before the data arrives | 1,378 (+23%) | 1,516 (+31%) | 1,702 (+34%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the resulting cache misses cost 85–225 ns per message against a reused buffer, more as messages grow. Applications that use fewer, smaller buffers should see results close to the one-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 5,904 | |
| kqring `RECV`, provided buffer | 6,089 | +3% |
| kqring `RECV`, one reused buffer | 6,007 | +2% |
| kqring multishot `RECV` | 5,824 | −1% |
| kqring `RECV` armed before the data arrives | 6,494 | +10% |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent. On loopback the data isn't always there the moment `write()` returns, so there's no no-wait floor for TCP.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 823 | 855 | 865 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **648 (−21%)** | **672 (−21%)** | **710 (−18%)** |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 666 (−19%) | 686 (−20%) | **705 (−18%)** |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as an application must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 9.49 | 11.18 | 11.90 | 10.12 |
| kqring `SEND` | 8.54 (+11%) | 10.41 (+7%) | 11.42 (+4%) | 10.00 (+1%) |
| kqring `SEND_ZC`, registered buffer | 8.53 (+11%) | 11.94 (−6%) | **14.99 (−21%)** | **16.35 (−38%)** |
| kqring `SEND_ZC`, unregistered buffer | 8.00 (+19%), copied | 10.38 (+8%) | 12.81 (−7%) | **14.11 (−28%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 2.18 | 3.84 | 4.35 | 4.62 |
| kqring `SEND` | 2.11 (+3%) | 3.85 (0%) | 4.26 (+2%) | 4.69 (−1%) |
| kqring `SEND_ZC`, registered buffer | 2.20 (−1%) | 3.74 (+3%) | **5.17 (−16%)** | **6.34 (−27%)** |
| kqring `SEND_ZC`, unregistered buffer | 2.11 (+3%) | 3.65 (+5%) | 4.84 (−10%) | **6.06 (−24%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default, chosen from these measurements: an unregistered 16 KiB send took 2,048–2,188 ns copied and 2,287–2,319 ns zero-copy, and at 32 KiB zero-copy won (3,158–3,257 against 3,379–3,536 ns). TCP always uses one page.
- **The small-message gap is the ring, not zero-copy.** At 16 KiB, every kqring variant on AF_UNIX costs about 190–320 ns more than `write(2)`, the same as a plain copying `SEND`. That's the fixed cost of the SQE, the CQE and the enter, which batching would amortise.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread (`netisr_queue()`), so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing. A benchmark with the receiver in its own thread would judge TCP zero-copy better.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 17,511 | −9% |
| kqueue: `kevent` + `accept` | 19,281 | |
| kqring multishot `ACCEPT` | 19,257 | 0% |
| kqring multishot `ACCEPT`, 50 µs busy-poll | **17,686** | **−8%** |

The faster enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays a wake-up of about 1 µs. With `IORING_REGISTER_NAPI` set to 50 µs (`kqring_multishot_busypoll`), multishot `ACCEPT` avoids the sleep and matches the blocking `accept(2)` floor.

### Pipes and files: inline or through a worker

![Pipes and files chart](docs/bench/worker.svg)

kqring runs a non-socket `READ` or `WRITE` inline, on the thread that submitted it, when that cannot block:
- **Regular files:** a positioned read whose pages are all in the page cache. It is served the way `read(2)` serves it first, through `VOP_READ_PGCACHE` (UFS and tmpfs).
- **Pipes:** a `READ` on a pipe that has data, or a `WRITE` to one with room, blocking or not.

Everything else goes to a worker thread, including a pipe `READ` that has to wait for data (even on an `O_NONBLOCK` pipe: like io_uring, kqring waits rather than fail with `-EAGAIN`).

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 616 | −40% |
| kqueue: `write` + `kevent` + `read` | 1,024 | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | **832** | **−19%** |
| kqring `READ`, blocking pipe, data ready (inline) | 840 | −18% |

**File: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`.

| Variant | UFS (`VOP_READ_PGCACHE`) | vs `pread` | ZFS (ARC, via worker) | vs `pread` |
|---|---:|---:|---:|---:|
| `pread(2)` | 1,217 | | 1,657 | |
| kqring `READ`, one per enter | 1,386 | +14% | 2,432 | +47% |
| kqring `READ`, one per enter, registered file (`IOSQE_FIXED_FILE`) | 1,366 | +12% | 2,343 | +41% |
| kqring `READ`, 32 per enter | **1,041** | **−14%** | 2,151 | +30% |

Before inline reads and adaptive worker spinning, these were 5,540 ns and 4,696 ns, because every read made a sleepy round trip through a worker.

A registered file saves almost nothing on UFS (and ~90 ns on ZFS), so the descriptor lookups a `READ` makes aren't where the single-read gap comes from. On UFS it is the fixed cost of an enter, shown by the `NOP` row (326 ns against 296 ns for an empty `kevent`), plus the SQE and CQE; on ZFS (which caches in the ARC rather than `VOP_READ_PGCACHE`), it also includes the spinning worker hand-off.

### Disk device: 4 KiB random reads and writes

A positioned `READ` or `WRITE` on a disk device goes straight to the driver as a bio over the held user pages, as `aio(4)` does, and completes from the bio callback. Before this build, every one went through a worker thread; the `IOSQE_ASYNC` rows force that path. The device is a 64 MiB md(4) malloc disk, so these rows measure the I/O path itself, not media latency: md serves every bio from its own thread, so even `pread(2)` includes a hand-off. They are the median of 3 runs of the `disk` group, which runs only when `KQRING_BENCH_DISK` names a scratch device.

| Variant | Read | vs `pread` | Write | vs `pwrite` |
|---|---:|---:|---:|---:|
| `pread(2)` / `pwrite(2)` | 4,371 | | 5,504 | |
| kqring, one per enter, through a worker (before) | 8,368 | +91% | 9,589 | +74% |
| kqring, one per enter, to the driver | 4,908 | +12% | 6,124 | +11% |
| kqring, 32 per enter, through workers (before) | 7,705 | +76% | | |
| kqring, 32 per enter, to the driver | **975** | **−78%** | **2,301** | **−58%** |

With one request per enter, kqring pays one sleep and wake-up more than `pread(2)`, since the owner waits in `kqring_enter()` for the bio callback's taskwork. With 32 in flight, the driver gets them all at once, which `pread(2)` can't do from one thread.

### Storage workloads: queue depth, aio(4) and `pread(2)` threads

[`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) runs storage-benchmark-style workloads: a fixed number of I/Os in flight per job, random or sequential offsets, for a fixed time after a 1 s ramp. It runs the same workload through kqring, through FreeBSD's `aio(4)` (`aio_read`/`aio_write`, `aio_waitcomplete`), or through `pread(2)`/`pwrite(2)` threads, one per I/O in flight. The figures are IOPS (higher is better) and the 99th-percentile completion latency in µs, the median of 3 runs of 5 s. The "before" column is the module built from the previous commit, on the same machine and in the same session.

Latency runs from just before an I/O is submitted to when its completion is seen, both clock readings taken once per batch. For kqring it therefore includes waiting behind the rest of a batch, which `aio_waitcomplete()` (one completion per call) doesn't, so compare kqring's p99 with its own earlier rows rather than with aio's.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring before | kqring now | p99 before → now |
|---:|---:|---:|---:|---:|---:|
| 1 | 67,860 | 51,716 | 70,778 | **71,069** | 27.6 → 27.6 |
| 4 | 113,292 | 107,617 | 72,084 | **110,415** (+53%) | 73.7 → 63.5 |
| 16 | 112,625 | 109,552 | 71,875 | **106,977** (+49%) | 258 → 270 |
| 64 | 123,522 | 108,978 (32) | 72,269 | **101,781** (+41%) | 983 → 836 |

aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`). Before this build, kqring got no more from 64 reads in flight than from one: the issuer waited for each worker op before submitting the next, which saves a wake-up for a lone request but ran a batch one read at a time. Now a read doesn't wait while more SQEs follow in the same submission or another read is on the workers, and only one idle worker spins for the next op (16 spinning workers had made every hand-off a fight for the pool's mutex). Writes still wait their turn, keeping writes to one file in order as io-wq does. ZFS itself tops out at about 110,000 reads per second on this machine, whatever the API; random writes are bound by ZFS's copy-on-write at about 1,000–2,000 per second for every engine, so they aren't in the table.

**A 1 GiB md(4) malloc disk, 4 KiB random I/O** (the bio path; md serves every bio from one thread, so this measures the I/O path, not media). `+F+B` is a fixed file and registered buffers, `+P` `SQPOLL` and `+I` `IOPOLL`:

| Variant | Read, 1 | Read, 32 | Write, 1 | Write, 32 |
|---|---:|---:|---:|---:|
| `pread` / `pwrite` threads | 186,856 | 566,020 | 192,065 | 540,831 |
| aio | 173,481 | 585,620 | 178,058 | 607,197 |
| kqring | 169,262 | 578,572 | 174,659 | **694,807** |
| kqring `+F+B` | 168,533 | 580,432 | 175,349 | 687,089 |
| kqring `+P` | 106,642 | 536,900 | 108,485 | 593,678 |
| kqring `+I` | 169,119 | 581,580 | 174,232 | 679,507 |

Every md row is within 1% of the previous build. At 32 in flight, kqring reads match aio and writes are 14% ahead of it. One at a time, a thread calling `pread(2)` is 10% faster, as in the `disk` group above. `SQPOLL` was 37% slower one at a time in 0.2.0, because its thread slept between requests; see the `SQPOLL` table below. Registered files and buffers make no measurable difference on md. 128 KiB sequential reads, 8 in flight, run at about 36,000 per second (4.4 GiB/s) for both kqring and aio.

**Waiting for several completions** (after 0.2.0). An enter with `min_complete` = N used to be woken for every worker or bio completion, and posted one CQE and slept again until N were there. Now a plain `READ`/`WRITE` completion wakes it only once N are queued, as io_uring's lazy task-work wake does (see ARCHITECTURE.MD). The md disk again, 4 KiB random I/O with 32 in flight, `kqring_diskbench -n` completions waited for per enter, median of 3 runs of 4 s alternating with the 0.2.0 module; context switches are the process's voluntary ones per run:

| Workload | IOPS 0.2.0 → now | Context switches 0.2.0 → now | p99 µs 0.2.0 → now |
|---|---:|---:|---:|
| Reads, wait for 1 | 571,714 → 567,003 | 92,978 → 118,218 | 129 → 129 |
| Reads, wait for 16 | 544,886 → **576,624** (+6%) | 866,396 → **168,475** (−81%) | 113 → 109 |
| Writes, wait for 1 | 729,499 → 726,606 | 47,509 → 62,736 | 104 → 104 |
| Writes, wait for 16 | 679,002 → **736,737** (+9%) | 842,941 → **213,961** (−75%) | 94 → 90 |

Waiting for one completion can't batch, and those rows are unchanged within run-to-run noise (the context-switch counts vary by ±30% between runs). On a ZFS file whose data had to come from the disk, 16 reads in flight with 8 waited for ran at 4,860 IOPS either way, bound by the drive, with 31% fewer context switches (65,021 → 44,855).

**`SQPOLL`** (after 0.2.0). In 0.2.0 the `SQPOLL` thread slept in 50 µs naps between requests even while active, so each request cost a wake-up to submit it and another to complete it. It now polls for the whole `sq_thread_idle`, as io_uring's does. The md disk, 4 KiB random reads, single runs of 3 s; `-C` polls the CQ in userspace instead of entering to wait, entering only to wake an idle `SQPOLL` thread:

| In flight | plain kqring | `SQPOLL`, 0.2.0 | `SQPOLL` now | `SQPOLL` now, `-C` |
|---:|---:|---:|---:|---:|
| 1 | 168,633 | 107,298 | 123,629 | **199,844** (p50 4.2 µs) |
| 4 | 553,795 | 400,019 | 375,146 | **580,194** |
| 32 | 572,077 | | 595,881 | **605,323** |

Polling the CQ, `SQPOLL` one at a time is 18% faster than plain kqring (p50 4.2 µs against 5.8 µs), with almost no context switches; that's how a latency-bound program would use it. An enter that waits for each completion still pays a sleep and a wake-up per request, as on Linux. The polling thread uses a CPU for as long as it stays active.

`kqring_diskbench -V` checks every read against what was last written to that block and re-reads the whole area at the end. On this build, 3 s of mixed random reads and writes passed it on both the md disk and the ZFS file at 4 KiB for `pread`, aio (16 in flight), and kqring plain, `+F+B`, `+P`, `+I` and all four together (32 in flight), and for 4 kqring jobs of 64 in flight at 16 KiB.

### fio on a real SSD

[fio](fio/README.md) 3.43 with its own `io_uring` engine on kqring, against `posixaio` (`aio(4)`) and `psync`, on a 1 TB WD Blue 3D NAND SATA SSD (`WDS100T2B0A`, about 98,000 IOPS and 540 MB/s here). Each figure is the median of three 20-second runs after a 2-second ramp. `psync` runs one thread per I/O in flight. [`fio/bench.sh`](fio/bench.sh) reproduces them.

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

fio found a bug here. It submits one I/O per enter, and a read that found nothing else in flight waited for its worker, so the next read found the same: 32 in flight ran one at a time, at 7,753 IOPS. A read still running after the short spin now goes on without the issuer, which gives the 63,898 above, from 7,753. Benchmarks that submit in batches, such as `kqring_diskbench`, didn't show it.

Writes had the same problem for a second reason: every write waited for its worker, to stay in order. An `O_DIRECT` write at its own offset needs no order, as on Linux, and now runs like a read: 56% CPU became 14%, for the same or slightly more IOPS. See [Known gaps](#known-gaps) for the writes that still wait.

### fio with several rings

One ring per thread, 4 KiB random reads, on targets the SSD doesn't limit: [`fio/scale.sh`](fio/scale.sh). `psync` here is one thread per job, so it has 1/16 of the others' I/Os in flight. Medians of three 10-second runs, in thousands of IOPS.

| Jobs | ZFS, in the ARC: kqring before | kqring | aio | `psync` | `md(4)`, a disk per job: kqring | aio |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 243 | **424** | 312 | 290 | **544** | 453 |
| 2 | 367 | **528** | 325 | 452 | **965** | 558 |
| 4 | 471 | 501 | 304 | **705** | **1,031** | 323 |
| 8 | 108 | 532 | 272 | **784** | | |

The direct bio path scales until the 4 cores run out, while aio falls back. Through the worker pool, kqring stays near 530,000 at any number of jobs, ahead of aio; from 4 jobs on, `psync` passes it, since a thread reading the ARC itself needs no hand-off.

This fio run also found the collapse at 8 jobs. Past the 4 cores, throughput fell with every job added, with CPUs idle: each ring had a worker spinning for work and an issuer spinning for its worker, taking the CPU from the threads doing the work. A worker now spins only with a CPU to spare (`sched_load()`). An issuer spins only then, and only for a program in lockstep, which has taken every CQE: spinning for one that is filling a queue also ran its requests one at a time, which is why 1 job gains 74%. One read at a time from the ARC costs about 4% (246,000 to 237,000 IOPS), and `kqring_bench`'s other rows are unchanged.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 10,185 | |
| kqring `TIMEOUT`, 1 µs | 4,904 | −52% |

Both are dominated by timer-interrupt scheduling, not by the API.

### Echo server: a kqring extension against kqueue and io_uring style

`kqring_bench echo`. A server thread echoes each message to a client in another thread that sends one and waits for it. Each cell is round trip / server CPU per message, in ns, the median of 3 runs.

| Server | AF_UNIX 64 B | AF_UNIX 4 KiB | TCP 64 B |
|---|---:|---:|---:|
| kqueue: `kevent` + `read` + `write` | 5,697 / 3,091 | 6,890 / 3,632 | 11,957 / 4,346 |
| kqring io_uring style: each `RECV` CQE → submit `SEND` + next `RECV` | 6,060 / 3,019 | 7,169 / 3,553 | 12,361 / 4,373 |
| kqring trigger: `RECV` → `SEND`, `LEN_FROM_PREV`, no CQE per message | **5,462 / 2,621** | **6,621 / 3,168** | **11,902 / 4,006** |

Against kqueue, the trigger (an [event-driven chain](docs/event-chains-spec.md), opted into per ring) takes 15%, 13% and 8% less server CPU per message, and its round trip is 4%, 4% and 0.5% shorter. The server never returns to user space between messages. One message at a time, most of each round trip is the cross-thread wakeup, which none of the servers can avoid.

### Multi-threaded scaling: one ring per thread

[`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) runs each workload on 1, 2, 4 and 8 threads, each pinned to its own CPU (CPUs 2n and 2n+1 are the two hyperthreads of one core) with its own ring, kqueue, socket pair or file offset. That's the thread-per-core model `SINGLE_ISSUER` rings are made for. Figures are millions of operations per second for all threads together, the median of 5 runs of 2 s each, with the module reloaded before each run.

| Workload | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| `kevent(2)`, nothing to do | 3.49 | 5.97 | 11.93 | 23.86 |
| kqring `NOP`, one per enter | 3.05 | 4.45 | 8.91 | 12.49 |
| kqring `NOP`, 32 per enter | 14.80 | 15.18 | 25.97 | 41.34 |
| AF_UNIX ping-pong, 64 B, kqueue | 0.86 | 1.40 | 2.55 | 3.95 |
| AF_UNIX ping-pong, 64 B, kqring `RECV` into one buffer | 1.11 | 1.69 | 2.98 | 4.61 |
| 4 KiB cached reads of one shared file, `pread(2)` | 0.90 | 1.74 | 2.42 | 3.37 |
| the same, kqring `READ`, one per enter | 0.82 | 1.55 | 2.25 | 2.68 |
| the same, kqring `READ`, 32 per enter | 1.33 | 1.83 | 2.40 | 2.65 |

- **Enter scales.** In the first run of this benchmark, one `NOP` per enter reached only 6.5 M/s on 8 threads, while `kevent(2)` reached 23.8 M/s. DTrace showed why: a module's system calls are counted in their `sysent` entry so that unloading can wait for them, and that is two atomic compare-and-swaps per call on one word that every CPU shares. `kevent(2)`, compiled into the kernel, isn't counted. kqring now counts its enter calls per CPU instead (see `kqring_sys_enter()`), and the same loop reaches 12–19 M/s on 8 threads. Per-thread cost is flat from 2 to 4 threads.
- **Ping-pong stays ahead of kqueue on every thread count**, by 29% on 1 thread and 17% on 8.
- **What's left is per-request work, not sharing.** At 8 threads, two threads share each core, and DTrace shows the time in submitting, completing and clearing requests, not in any lock. Runs on 8 threads vary more (the single-`NOP` row by up to 43%), because this is a 15 W laptop CPU whose all-core clock moves with load.
- **Reads of one shared file are bound by the file's page cache**, for `pread(2)` too. Batching stops helping past 4 threads.

## Known gaps

- **A pipe read that has to wait goes through a worker thread**, as does a write to a full pipe. kqring first tries the pipe as if it were `O_NONBLOCK`: FreeBSD's `pipe_read()` and `pipe_write()` take that from the file's flags, so it calls them through a private file sharing the pipe, leaving the descriptor's own flags alone. On `EAGAIN` the request goes to a worker, and a partial write takes its progress with it, so another thread draining the pipe can't make the ring owner wait. A worker round trip costs about 4.4 µs (the `IOSQE_ASYNC` floor row). Waiting for data with `POLL_ADD` first avoids the worker.
- **Reads of a FIFO that have to wait can complete out of order.** Reads of a pipe post their CQEs in the order of the data, but a named FIFO's reads block inside `fo_read()` on their workers, so one can read first and post second. Each read gets correct data.
- **Buffered writes to a file wait for their worker.** That keeps writes to one file in order, as io-wq's per-inode hashing does, but the issuer waits too, where io-wq's hashing wouldn't make it. So do `O_DIRECT` writes that use the file position, `O_APPEND` or per-write flags such as `RWF_DSYNC`.
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers file reads that need the disk, ZFS (which caches in the ARC, not the page cache), `O_DIRECT` on files, and reads using the file position (`off` = −1). Only disk devices themselves get the direct bio path: FreeBSD filesystems have no asynchronous read or write entry point, and sending bios under a filesystem would bypass its block allocation and locking. It also covers reads into a buffer that isn't mapped yet: the inline copy runs with page faults disabled, so the buffer's first use goes through the worker.
- **One operation per enter costs about 60 ns more than an empty `kevent`.** That's the `NOP` floor: parsing an SQE and posting a CQE. Batching removes it.
- **A wait that just misses its completion pays a full sleep.** When the completion comes from another thread (the TCP loopback netisr, for instance) a moment after the enter checks, the enter sleeps and pays a wake-up of about 1 µs; see [Accept](#accept-a-tcp-loopback-connection-set-up-and-torn-down). `IORING_REGISTER_NAPI` opts in to a busy-wait before sleeping. With 50 µs, multishot `ACCEPT` drops from about 15.9 to 14.1 µs, below the blocking `accept(2)` floor, at the cost of CPU time on waits that don't end quickly.
- **An armed one-shot `RECV` is 22–31% dearer than kqueue on AF_UNIX (and 8% on TCP).** Detaching one-shot waiters inside `kqring_soupcall()`, recycling `kqring_sowatch` entries per ring, and draining taskwork under one `ctx->mtx` acquisition cut this down from 37–43%. What remains is the extra `kqring_enter()` syscall the pattern makes plus the `EAGAIN` probe before arming. Use multishot `RECV` for steady-state receive (which is 4–18% faster than kqueue).
- **A worker round trip is about 1.0 µs with bounded adaptive spinning (down from 4.4 µs).** Before adaptive spinning, DTrace put 2.0 µs on waking the worker and 2.2 µs on waking the issuer back, each across CPUs. [`docs/worker-handoff-spec.md`](docs/worker-handoff-spec.md) describes the bounded spinning on both sides that removed those wake-ups for short worker ops.
- **`RLIMIT_MEMLOCK` defaults to 64 KiB on FreeBSD.** Registering buffers holds and charges their pages, so `IORING_REGISTER_BUFFERS` fails with `ENOMEM` beyond 64 KiB in total unless the limit is raised, through `login.conf(5)` (`memorylocked`) or `ulimit -l`. Unregistered `SEND_ZC` silently falls back to copying when the limit is reached. Linux's default is 8 MiB.
- **Provided-buffer working set.** The multishot and buffer-select numbers include cache misses from rotating through 16 MiB of buffers. That's a property of this benchmark's buffer ring size, not a fixed cost, but applications should size buffer rings with the cache in mind.

## Reproducing

On the FreeBSD host, from the repository root:

```sh
make
sudo kldload ./kmod/kqring.ko
ulimit -l 8192                    # the send group registers 2 MiB of buffers
./tests/kqring_bench > bench.1.out           # all groups
./tests/kqring_bench fanin send   # selected groups: floor pingpong fanin accept pipe file send timer disk
./tests/kqring_mt_bench 2 > mt.1.out         # multi-threaded scaling, 2 s per run
sudo kldunload kqring
```

If the hard limit is 64 KiB, raise it as root first, for example: `sudo sh -c 'ulimit -l 8192; exec sudo -u "$USER" ./tests/kqring_bench'`.

Each line of output is `ROW <group> <variant> <ns/op>`. To match these tables, run it 5 times, reloading the module before each run. Then produce the medians and charts on any machine with Python 3:

```sh
python3 tests/bench_report.py docs/bench bench.*.out
```

The `file` group writes a 64 MiB temporary file in `/tmp`. The `disk` group runs only with `KQRING_BENCH_DISK` set to a device it may overwrite, for example `sudo mdconfig -a -t malloc -s 64m -u 9 && sudo chown $USER /dev/md9`; the test suite's disk test uses `KQRING_TEST_DISK` the same way.

The storage tables come from `kqring_diskbench`; the comment at the top of [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) lists its options. For example, on a 1 GiB md disk and a file on ZFS:

```sh
sudo mdconfig -a -t malloc -s 1g -u 9 && sudo chown $USER /dev/md9
./tests/kqring_diskbench -e kqring -q 32 -w randwrite /dev/md9
./tests/kqring_diskbench -e aio -q 32 -w randwrite /dev/md9
dd if=/dev/random of=$HOME/dbench.dat bs=1m count=256
./tests/kqring_diskbench -e kqring -q 16 -w randread $HOME/dbench.dat
./tests/kqring_diskbench -V -e kqring -F -B -w randrw /dev/md9   # verify
```

It refuses to write to a device unless it is an md(4) disk or `-f` is given. `-B` registers `-q` × `-b` bytes per job, so raise `ulimit -l` for it.
