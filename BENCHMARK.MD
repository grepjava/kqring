# kqring vs kqueue benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads. Where kqueue has no role (regular files, bulk sends into a socket with room), the comparison is the plain syscall.

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is 17–22% faster.** When many sockets become ready at once, one `kqring_enter()` reaps every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`. Across 16, 256 and 1,024 connections, multishot `RECV` and batched single-shot `RECV` beat kqueue by 17–22% (`647–742 ns` vs `826–871 ns`).
- **One connection: faster across all message sizes.** On AF_UNIX, a multishot `RECV` is 18% faster than kqueue at 64 B (`912 ns` vs `1,111 ns`), 12% at 1 KiB and 4% at 4 KiB. Receiving with a provided-buffer ring is up to 14% faster (`958 ns`), and receiving into one reused buffer is 20–22% faster at every size tested (`865 ns` at 64 B).
- **Armed one-shot `RECV` overhead cut from +37% to +22%.** A one-shot `RECV` submitted before its data arrives costs 22% more than kqueue at 64 B (`1,352 ns` vs `1,111 ns`, down from `1,523 ns` / +37%), 28–31% more at 1–4 KiB, and 8% more on TCP loopback (`6,427 ns` vs `5,928 ns`), because it takes an extra enter and installs and removes a socket upcall each time. Detaching one-shot waiters inside `kqring_soupcall()`, recycling `kqring_sowatch` entries per ring, and draining taskwork under a single `ctx->mtx` acquisition cut 171 ns/op (~41% of the gap over kqueue). Use multishot `RECV` for steady-state receive.
- **Cached file reads beat `pread(2)` on UFS when batched; ZFS goes through a worker.** On UFS/tmpfs (`VOP_READ_PGCACHE`), 32 `READ`s per enter cost 786 ns each, 30% less than `pread(2)`, and a single `READ` per enter costs 14% more than `pread(2)`. On ZFS (which caches in the ARC rather than `VOP_READ_PGCACHE`), file reads go to a worker thread (`2,184 ns` batched / `2,461 ns` single vs `1,684 ns` `pread(2)`).
- **Zero-copy send pays off from 32–64 KiB.** `SEND_ZC` from a registered buffer is 35–39% faster than `write(2)` for 256 KiB messages on AF_UNIX and 25–27% faster on TCP loopback, and 15–19% faster at 64 KiB. At 16–32 KiB it ranges from 8% faster to 9% slower than `write(2)`. On AF_UNIX, kqring copies small sends from unregistered buffers itself, because that's cheaper there.
- **Batching and worker hand-off.** 32 `NOP`s per enter cost 62 ns each, against 297 ns for an empty `kevent(2)` and 321 ns for a single `NOP` enter. Bounded adaptive spinning between the issuer and worker threads cut the `IOSQE_ASYNC` worker round trip from `4,371 ns` to `1,017 ns` (4.3× faster).
- **Timers are faster, accept is even (or 8% faster with busy-polling).** A 1 µs `TIMEOUT` completes 30–45% faster than a 1 µs `EVFILT_TIMER` (`4,821 ns`). A multishot `ACCEPT` is on par with kqueue plus `accept(2)`, and 8% faster with 50 µs `IORING_REGISTER_NAPI` busy-polling; see [Accept](#accept-a-tcp-loopback-connection-set-up-and-torn-down).
- **Disk devices go straight to the driver.** 4 KiB random reads on an md(4) disk cost 41% less than through a worker, and 32 in flight cost 975 ns each, 78% less than `pread(2)`.
- **Pipes: 18–19% faster than kqueue.** A `READ` on a pipe that has data runs inline, blocking or not (`830–836 ns` vs `1,021 ns`). Only a read that has to wait goes to a worker thread.
- **Rings scale with threads.** With one ring per thread, single-`NOP` enters reach 12.5 M/s on 8 threads (6.5 M/s before this build) and ping-pong stays 17–29% ahead of kqueue at every thread count; see [Multi-threaded scaling](#multi-threaded-scaling-one-ring-per-thread).

## What changed in this build

Against the previous published build:

1. **`kqring_enter(2)` scales across threads.** A module's system calls are counted in their `sysent` entry so that unloading can wait for them: two atomic compare-and-swaps per call on one word every CPU shares. kqring now counts enter calls per CPU instead; see [Multi-threaded scaling](#multi-threaded-scaling-one-ring-per-thread).
2. **Pipes with data are read inline, blocking or not**, as are writes to pipes with room. Before, a blocking pipe always went through a worker thread.
3. **`POLL_ADD` on pipes, eventfds and other non-socket files** waits as a knote on a kqueue the ring owns, served by one kernel thread per ring, instead of one blocking worker per poll. It isn't in these benchmarks.
4. **liburing compatibility work**: rings shared by the threads of a process, provided-buffer receives that fail `-ENOBUFS` on an empty group as on Linux, bundle sends, and ring descriptors that can't be passed over UNIX sockets. The `-ENOBUFS` check costs an idle socket's receive a few tens of nanoseconds, which shows in the armed `RECV` row.

5. **Socket-upcall detach, single-lock taskwork drain, KVA-pinned buffer rings, and adaptive worker spinning**:
   - One-shot single-direction socket waiters (`RECV`/`SEND`) are detached and their upcall cleared directly inside `kqring_soupcall()` while `SOCK_BUF_LOCK` is already held, and `struct kqring_sowatch` entries are recycled per ring (`sw_cache`).
   - `kqring_run_taskwork()` drains single-item taskwork under a single `ctx->mtx` acquisition (down from 3), and single-issuer rings without active workers skip `ctx->bg_sx` locking on provided-buffer selection.
   - User-memory `PBUF_RING` pages are pinned into KVA at registration (`bg->ring_kva`), replacing per-operation `copyin(9)` with a direct memory load.
   - Worker threads and synchronous `kqwqsy` waits spin adaptively before sleeping (`docs/worker-handoff-spec.md`), cutting `IOSQE_ASYNC` round-trip latency by 4.3×.

| Variant | Before | Previous | Now | Change (vs Before) |
|---|---:|---:|---:|---:|
| `NOP`, one per enter | 345 | 324 | **321** | −7% |
| `NOP`, 32 per enter | 77 | 64 | **62** | −19% |
| `NOP` with `IOSQE_ASYNC`, through a worker | 4,443 | 4,371 | **1,017** | **−77%** |
| AF_UNIX `RECV`, provided buffer, 64 B | 1,038 | 983 | **958** | −8% |
| AF_UNIX `RECV`, one reused buffer, 64 B | 886 | 863 | **865** | −2% |
| AF_UNIX multishot `RECV`, 64 B | 964 | 948 | **912** | **−5%** |
| AF_UNIX `RECV` armed before the data, 64 B | 1,474 | 1,523 | **1,352** | **−8% (−11% vs 1,523)** |
| Fan-in, 256 sockets, multishot `RECV` | 717 | 720 | **673** | **−6%** |
| `READ`, blocking pipe | 5,410 | 850 | **836** | **−85%** |
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
| `kevent(2)`, nothing to do | 297 | |
| kqring `NOP`, one per enter | 321 | +8% |
| kqring `NOP`, 32 per enter | 62 | −79% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 1,017 | +242% |

A single kqring enter costs 24 ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Through the `ioctl(2)` fallback it costs about 365 ns. Batching amortises the rest quickly. The `IOSQE_ASYNC` row is the cost of a round trip through a worker thread (down from 4,371 ns with bounded adaptive spinning).

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 720 | 749 | 838 |
| kqueue: `write` + `kevent` + `recv` | 1,111 | 1,151 | 1,269 |
| kqring `RECV`, provided buffer | 958 (−14%) | 1,072 (−7%) | 1,265 (0%) |
| kqring `RECV`, one reused buffer | **865 (−22%)** | **900 (−22%)** | **1,016 (−20%)** |
| kqring multishot `RECV`, provided buffers | 912 (−18%) | 1,014 (−12%) | 1,218 (−4%) |
| kqring `RECV` armed before the data arrives | 1,352 (+22%) | 1,476 (+28%) | 1,669 (+31%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the resulting cache misses cost 90–250 ns per message against a reused buffer, more as messages grow. Applications that use fewer, smaller buffers should see results close to the one-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 5,928 | |
| kqring `RECV`, provided buffer | 6,022 | +2% |
| kqring `RECV`, one reused buffer | 5,921 | 0% |
| kqring multishot `RECV` | 5,823 | −2% |
| kqring `RECV` armed before the data arrives | 6,427 | +8% |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent. On loopback the data isn't always there the moment `write()` returns, so there's no no-wait floor for TCP.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 826 | 861 | 871 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **647 (−22%)** | **673 (−22%)** | **742 (−15%)** |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 666 (−19%) | 693 (−19%) | **701 (−20%)** |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as an application must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 9.43 | 11.24 | 12.16 | 9.90 |
| kqring `SEND` | 8.59 (+10%) | 10.60 (+6%) | 11.53 (+5%) | 9.88 (0%) |
| kqring `SEND_ZC`, registered buffer | 8.57 (+10%) | 12.12 (−7%) | **14.93 (−19%)** | **16.02 (−38%)** |
| kqring `SEND_ZC`, unregistered buffer | 8.07 (+17%), copied | 10.55 (+7%) | 12.83 (−5%) | **13.86 (−29%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 2.23 | 3.92 | 4.72 | 5.12 |
| kqring `SEND` | 2.15 (+3%) | 3.92 (0%) | 4.66 (+1%) | 5.13 (0%) |
| kqring `SEND_ZC`, registered buffer | 2.30 (−3%) | 3.66 (+7%) | **5.69 (−17%)** | **6.99 (−27%)** |
| kqring `SEND_ZC`, unregistered buffer | 2.20 (+1%) | 3.60 (+9%) | 5.42 (−13%) | **6.22 (−18%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default, chosen from these measurements: an unregistered 16 KiB send took 2,147–2,188 ns copied and 2,287–2,319 ns zero-copy, and at 32 KiB zero-copy won (3,230–3,257 against 3,379–3,536 ns). TCP always uses one page.
- **The small-message gap is the ring, not zero-copy.** At 16 KiB, every kqring variant on AF_UNIX costs about 200–340 ns more than `write(2)`, the same as a plain copying `SEND`. That's the fixed cost of the SQE, the CQE and the enter, which batching would amortise.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread (`netisr_queue()`), so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing. A benchmark with the receiver in its own thread would judge TCP zero-copy better.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 14,486 | −11% |
| kqueue: `kevent` + `accept` | 16,297 | |
| kqring multishot `ACCEPT` | 16,047 | −2% |

The previous build measured 14,838 ns here. The faster enter is what made it slower: the connection is completed by the loopback netisr thread, and the enter now usually checks before that thread has queued it. So nearly every iteration sleeps and pays a wake-up of about 1 µs. DTrace counted about 20,000 more sleeps per run than with the `ioctl(2)` fallback, one per connection. Forcing the fallback on this build gives 15,529–16,146 ns.

### Pipes and files: inline or through a worker

![Pipes and files chart](docs/bench/worker.svg)

kqring runs a non-socket `READ` or `WRITE` inline, on the thread that submitted it, when that cannot block:
- **Regular files:** a positioned read whose pages are all in the page cache. It is served the way `read(2)` serves it first, through `VOP_READ_PGCACHE` (UFS and tmpfs).
- **Pipes:** a `READ` on a pipe that has data, or a `WRITE` to one with room, blocking or not.

Everything else goes to a worker thread, including a pipe `READ` that has to wait for data (even on an `O_NONBLOCK` pipe: like io_uring, kqring waits rather than fail with `-EAGAIN`).

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 620 | −39% |
| kqueue: `write` + `kevent` + `read` | 1,024 | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | **834** | **−19%** |
| kqring `READ`, blocking pipe, data ready (inline) | 850 | −17% |

**File: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 1,116 | |
| kqring `READ`, one per enter | 1,273 | +14% |
| kqring `READ`, one per enter, registered file (`IOSQE_FIXED_FILE`) | 1,255 | +12% |
| kqring `READ`, 32 per enter | **786** | **−30%** |

Before inline reads, these were 5,540 ns and 4,696 ns, because every read made a round trip through a worker.

A registered file saves almost nothing, so the descriptor lookups a `READ` makes aren't where the single-read gap comes from. It is the fixed cost of an enter, shown by the `NOP` row (324 ns against 289 ns for an empty `kevent`), plus the SQE and CQE.

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

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 8,806 | |
| kqring `TIMEOUT`, 1 µs | 4,820 | −45% |

Both are dominated by timer-interrupt scheduling, not by the API.

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
