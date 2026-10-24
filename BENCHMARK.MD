# kqring vs kqueue benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads. Where kqueue has no role (regular files, bulk sends into a socket with room), the comparison is the plain syscall.

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is 15–16% faster.** When many sockets become ready at once, one `KQRING_IOC_ENTER` reaps every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`. The gain is the same at 16, 256 and 1,024 connections.
- **One connection: about even.** A multishot `RECV` is between 4% faster and 7% slower than kqueue, depending on message size. Receiving into one reused buffer, kqring is 10–12% faster at every AF_UNIX size tested.
- **Cached file reads beat `pread(2)` when batched.** 32 `READ`s per enter cost 780 ns each, 29% less than `pread(2)`. A single `READ` per enter costs 24% more than `pread(2)`, the price of the extra SQE and CQE. Both are served inline from the page cache, without a worker thread.
- **Zero-copy send pays off from 64 KiB.** `SEND_ZC` from a registered buffer is 38% faster than `write(2)` for 256 KiB messages on AF_UNIX and 26% faster on TCP loopback, and 15–16% faster at 64 KiB. At 16–32 KiB it ranges from 12% faster to 6% slower than kqring's copying `SEND`. On AF_UNIX, kqring copies small sends from unregistered buffers itself, because that's cheaper there.
- **Batching is where kqring shines.** 32 `NOP`s per enter cost 78 ns each, against 287 ns for an empty `kevent(2)` and 439 ns for a single `NOP` enter.
- **Accept and timers are faster.** A multishot `ACCEPT` is 8% faster than kqueue plus `accept(2)`, and a 1 µs `TIMEOUT` completes 41% faster than a 1 µs `EVFILT_TIMER`.
- **Pipes: fast when non-blocking, 5× slower when blocking.** A `READ` on an `O_NONBLOCK` pipe runs inline and is 10% faster than kqueue. On a blocking pipe it still goes through a worker thread, at about 4.5 µs per round trip; see [Known gaps](#known-gaps).
- **Arming costs a lot.** A `RECV` submitted before its data arrives costs 53–60% more than kqueue, because it takes three syscalls (submit, `write`, wait) and installs and removes a socket upcall each time. Use multishot for steady-state receive.

## Setup

| | |
|---|---|
| CPU | Intel Core i5-8250U, 4 cores / 8 threads, 1.6 GHz base |
| Memory | 4 GiB |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; files on UFS |
| kqring | `kqring.ko` built from `main`, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmark | [`tests/kqring_bench.c`](tests/kqring_bench.c), built with `cc -O2` |
| Limits | `ulimit -l 8192` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

The functional suite ([`tests/kqring_test.c`](tests/kqring_test.c)) passed 70/70 in each of 10 passes on the same build, with the module reloaded before each pass. After every pass and every benchmark run, `vmstat -m` showed no kqring memory in use, and `vm.stats.vm.v_user_wire_count` and `kern.openfiles` were back to their starting values.

## Method

- Each figure is the **median of 5 runs**. The module was unloaded and reloaded before each run.
- Each run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- Everything is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so this measures the per-operation cost of each mechanism, not throughput under load or wake-up latency across threads.
- Most rows vary less than 3% between runs. The exceptions are `pread` (21%), the kqueue timer (14%), the TCP sends of 32 KiB and more (up to 12%), the blocking `accept(2)` (8%), the 64 KiB AF_UNIX `write` and `SEND` (6%), the kqring timer (5%) and the kqring accept (4%).
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain syscall where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 287 | |
| kqring `NOP`, one per enter | 439 | +53% |
| kqring `NOP`, 32 per enter | 78 | −73% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 4,531 | +1,479% |

A single kqring enter costs more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching amortises that quickly. The `IOSQE_ASYNC` row is the cost of a round trip through a worker thread.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 720 | 753 | 834 |
| kqueue: `write` + `kevent` + `recv` | 1,112 | 1,152 | 1,249 |
| kqring `RECV`, provided buffer | 1,086 (−2%) | 1,190 (+3%) | 1,358 (+9%) |
| kqring `RECV`, one reused buffer | **981 (−12%)** | **1,021 (−11%)** | **1,118 (−10%)** |
| kqring multishot `RECV`, provided buffers | 1,065 (−4%) | 1,163 (+1%) | 1,334 (+7%) |
| kqring `RECV` armed before the data arrives | 1,698 (+53%) | 1,834 (+59%) | 2,000 (+60%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the resulting cache misses cost 100–240 ns per message against a reused buffer, more as messages grow. Applications that use fewer, smaller buffers should see results close to the one-buffer row.

An armed `RECV` no longer calls `soreceive()` on a socket with nothing to read; it goes straight to installing the upcall. That saved 1.4–2.2% against the previous build.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 5,897 | |
| kqring `RECV`, provided buffer | 6,200 | +5% |
| kqring `RECV`, one reused buffer | 6,032 | +2% |
| kqring multishot `RECV` | 6,028 | +2% |
| kqring `RECV` armed before the data arrives | 6,691 | +13% |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent. On loopback the data isn't always there the moment `write()` returns, so there's no no-wait floor for TCP.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 828 | 860 | 861 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **702 (−15%)** | **725 (−16%)** | **730 (−15%)** |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 717 (−13%) | 737 (−14%) | 740 (−14%) |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as an application must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 9.54 | 11.57 | 12.64 | 10.30 |
| kqring `SEND` | 8.06 (+18%) | 10.29 (+12%) | 11.72 (+8%) | 10.10 (+2%) |
| kqring `SEND_ZC`, registered buffer | 8.04 (+19%) | 11.76 (−2%) | **14.89 (−15%)** | **16.50 (−38%)** |
| kqring `SEND_ZC`, unregistered buffer | 7.64 (+25%), copied | 10.23 (+13%) | 12.73 (−1%) | **14.20 (−27%)** |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | 2.23 | 3.94 | 4.73 | 4.76 |
| kqring `SEND` | 2.11 (+6%) | 3.93 (0%) | 4.63 (+2%) | 4.76 (0%) |
| kqring `SEND_ZC`, registered buffer | 2.30 (−3%) | 3.71 (+6%) | **5.65 (−16%)** | **6.40 (−26%)** |
| kqring `SEND_ZC`, unregistered buffer | 2.20 (+2%) | 3.67 (+7%) | 5.14 (−8%) | **6.07 (−22%)** |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default, chosen from these measurements: an unregistered 16 KiB send took 2,147–2,188 ns copied and 2,287–2,319 ns zero-copy, and at 32 KiB zero-copy won (3,230–3,257 against 3,379–3,536 ns). TCP always uses one page.
- **The small-message gap is the ring, not zero-copy.** At 16 KiB, every kqring variant on AF_UNIX costs about 300–400 ns more than `write(2)`, the same as a plain copying `SEND`. That's the fixed cost of the SQE, the CQE and the enter, which batching would amortise.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread (`netisr_queue()`), so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing. A benchmark with the receiver in its own thread would judge TCP zero-copy better.
- **Correction.** Before this build, the zero-copy loop always made a second enter after submitting a send, even when the send's CQE was already there. That cost about 440 ns per send that `write` and `SEND` didn't pay, and understated AF_UNIX zero-copy, for example by about 10% at 64 KiB. On TCP the same wasted syscall happened to overlap with the netisr delivery and flattered small sends; at 16 KiB, the old loop showed `SEND_ZC` 21% faster than `write(2)`.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 14,389 | −11% |
| kqueue: `kevent` + `accept` | 16,128 | |
| kqring multishot `ACCEPT` | 14,838 | −8% |

### Pipes and files: inline or through a worker

![Pipes and files chart](docs/bench/worker.svg)

kqring runs a non-socket `READ` or `WRITE` inline, on the thread that submitted it, when that cannot block:
- **Regular files:** a positioned read whose pages are all in the page cache. It is served the way `read(2)` serves it first, through `VOP_READ_PGCACHE` (UFS and tmpfs).
- **Pipes:** any `READ` or `WRITE` on an `O_NONBLOCK` pipe.

Everything else goes to a worker thread, as before.

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 616 | −39% |
| kqueue: `write` + `kevent` + `read` | 1,017 | |
| kqring `READ`, `O_NONBLOCK` pipe (inline) | **919** | **−10%** |
| kqring `READ`, blocking pipe (worker) | 5,514 | +442% |

**File: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 1,097 | |
| kqring `READ`, one per enter | 1,356 | +24% |
| kqring `READ`, one per enter, registered file (`IOSQE_FIXED_FILE`) | 1,343 | +22% |
| kqring `READ`, 32 per enter | **780** | **−29%** |

Before inline reads, these were 5,540 ns and 4,696 ns, because every read made a round trip through a worker.

A registered file saves only about 13 ns, so the two descriptor lookups a `READ` makes aren't where the single-read gap comes from. Most of it is the fixed cost of an enter, shown by the `NOP` row (439 ns against 287 ns for an empty `kevent`).

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 8,660 | |
| kqring `TIMEOUT`, 1 µs | 5,078 | −41% |

Both are dominated by timer-interrupt scheduling, not by the API.

## Known gaps

- **Blocking pipes still go through a worker thread.** FreeBSD's `pipe_read()` decides whether to block from the descriptor's `O_NONBLOCK` flag, and has no per-call non-blocking read. kqring can't safely read a blocking pipe inline: another reader could drain it between a readiness check and the read, and the ring owner would then block. A worker round trip costs about 4.5 µs (the `IOSQE_ASYNC` floor row). Applications can set `O_NONBLOCK` on the pipe to get the inline path. A `READ` on an empty non-blocking pipe then completes with `-EAGAIN`, as `read(2)` would, so wait for data with `POLL_ADD` first.
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers reads that need the disk, ZFS (which caches in the ARC, not the page cache), `O_DIRECT`, and reads using the file position (`off` = −1). It also covers reads into a buffer that isn't mapped yet: the inline copy runs with page faults disabled, so the buffer's first use goes through the worker.
- **One operation per enter costs about 150 ns more than the equivalent syscall.** That's the `NOP` floor: entering through `ioctl(2)` on `/dev/kqring`, parsing an SQE and posting a CQE. Batching removes it; where it can't be batched, profiling the enter path is the next step.
- **An armed `RECV` is still 53–60% dearer than kqueue.** Each one installs and removes a socket upcall, where kqueue's filter stays registered. Multishot `RECV` avoids it. Keeping the upcall installed between single-shot receives would close most of the gap, but needs care with the one upcall slot per socket buffer.
- **`RLIMIT_MEMLOCK` defaults to 64 KiB on FreeBSD.** Registering buffers holds and charges their pages, so `IORING_REGISTER_BUFFERS` fails with `ENOMEM` beyond 64 KiB in total unless the limit is raised, through `login.conf(5)` (`memorylocked`) or `ulimit -l`. Unregistered `SEND_ZC` silently falls back to copying when the limit is reached. Linux's default is 8 MiB.
- **Provided-buffer working set.** The multishot and buffer-select numbers include cache misses from rotating through 16 MiB of buffers. That's a property of this benchmark's buffer ring size, not a fixed cost, but applications should size buffer rings with the cache in mind.

## Reproducing

On the FreeBSD host, from the repository root:

```sh
make
sudo kldload ./kmod/kqring.ko
ulimit -l 8192                    # the send group registers 2 MiB of buffers
./tests/kqring_bench > bench.1.out           # all groups
./tests/kqring_bench fanin send   # selected groups: floor pingpong fanin accept pipe file send timer
sudo kldunload kqring
```

If the hard limit is 64 KiB, raise it as root first, for example: `sudo sh -c 'ulimit -l 8192; exec sudo -u "$USER" ./tests/kqring_bench'`.

Each line of output is `ROW <group> <variant> <ns/op>`. To match these tables, run it 5 times, reloading the module before each run. Then produce the medians and charts on any machine with Python 3:

```sh
python3 tests/bench_report.py docs/bench bench.*.out
```

The `file` group writes a 64 MiB temporary file in `/tmp`.
