# kqring vs kqueue benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads. Where kqueue has no role (regular files, bulk sends into a socket with room), the comparison is the plain syscall.

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is 16% faster.** When many sockets become ready at once, one `KQRING_IOC_ENTER` reaps every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`. The gain is the same at 16, 256 and 1,024 connections.
- **One connection: about even.** A multishot `RECV` is between 5% faster and 5% slower than kqueue, depending on message size. Receiving into one reused buffer, kqring is 11–13% faster at every AF_UNIX size tested.
- **Zero-copy send pays off from 64 KiB.** `SEND_ZC` from a registered buffer moves 256 KiB messages 37% faster than `write(2)` on AF_UNIX and 24% faster on TCP loopback, and is 15–21% faster on TCP from 16 KiB up. Below 64 KiB on AF_UNIX, pinning and the notification cost more than the copy.
- **Batching is where kqring shines.** 32 `NOP`s per enter cost 78 ns each, against 287 ns for an empty `kevent(2)` and 440 ns for a single `NOP` enter.
- **Accept and timers are faster.** A multishot `ACCEPT` is 7% faster than kqueue plus `accept(2)`, and a 1 µs `TIMEOUT` completes 46% faster than a 1 µs `EVFILT_TIMER`.
- **Pipes and regular files are 5× slower.** These go through kqring's in-process worker threads, at about 4.5 µs per round trip, even when the data is already there. This is the main known gap; see [Known gaps](#known-gaps).
- **Arming costs a lot.** A `RECV` submitted before its data arrives costs 54–61% more than kqueue, because it takes three syscalls: submit, `write`, wait. Use multishot for steady-state receive.

## Setup

| | |
|---|---|
| CPU | Intel Core i5-8250U, 4 cores / 8 threads, 1.6 GHz base |
| Memory | 4 GiB |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64 |
| kqring | `kqring.ko` built from `main` at `0976b36` (modular source split; Phase A and B zero-copy) |
| Ring | `SINGLE_ISSUER \| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmark | [`tests/kqring_bench.c`](tests/kqring_bench.c), built with `cc -O2` |
| Limits | `ulimit -l 8192` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

The functional suite ([`tests/kqring_test.c`](tests/kqring_test.c)) passed 69/69 in each of 13 passes on the same build, with the module reloaded before each pass. After every pass and every benchmark run, `vmstat -m` showed no kqring memory in use, and `vm.stats.vm.v_user_wire_count` and `kern.openfiles` were back to their starting values.

## Method

- Each figure is the **median of 5 runs**. The module was unloaded and reloaded before each run.
- Each run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- Everything is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so this measures the per-operation cost of each mechanism, not throughput under load or wake-up latency across threads.
- Most rows vary less than 3% between runs. The exceptions are the 256 KiB TCP sends (5–11%), `pread` (9%), the kqueue timer (9%), the kqring accept (7%), and the 64 KiB and 256 KiB AF_UNIX sends (up to 4%).
- The charts are drawn from the raw runs by [`tests/bench_report.py`](tests/bench_report.py).

## Results

Times are in nanoseconds per operation, lower is better. Percentages are relative to the kqueue row of the same group, or to the plain syscall where there is no kqueue row.

### Floor: one empty round trip into the kernel

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `kevent(2)`, nothing to do | 287 | |
| kqring `NOP`, one per enter | 440 | +53% |
| kqring `NOP`, 32 per enter | 78 | −73% |
| kqring `NOP` with `IOSQE_ASYNC`, through a worker | 4,516 | +1,474% |

A single kqring enter costs more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching amortises that quickly. The `IOSQE_ASYNC` row is the cost of a round trip through a worker thread.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | 722 | 753 | 835 |
| kqueue: `write` + `kevent` + `recv` | 1,123 | 1,156 | 1,268 |
| kqring `RECV`, provided buffer | 1,085 (−3%) | 1,191 (+3%) | 1,366 (+8%) |
| kqring `RECV`, one reused buffer | **977 (−13%)** | **1,016 (−12%)** | **1,127 (−11%)** |
| kqring multishot `RECV`, provided buffers | 1,065 (−5%) | 1,160 (0%) | 1,334 (+5%) |
| kqring `RECV` armed before the data arrives | 1,734 (+54%) | 1,863 (+61%) | 2,036 (+61%) |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the resulting cache misses cost 100–240 ns per message against a reused buffer, more as messages grow. Applications that use fewer, smaller buffers should see results close to the one-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | 5,895 | |
| kqring `RECV`, provided buffer | 6,210 | +5% |
| kqring `RECV`, one reused buffer | 6,106 | +4% |
| kqring multishot `RECV` | 6,036 | +2% |
| kqring `RECV` armed before the data arrives | 6,748 | +14% |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent. On loopback the data isn't always there the moment `write()` returns, so there's no no-wait floor for TCP.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | 831 | 857 | 860 |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | **702 (−16%)** | **720 (−16%)** | **726 (−16%)** |
| kqring: N single-shot `RECV`s submitted and completed in one enter | 717 (−14%) | 734 (−14%) | 738 (−14%) |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as an application must. Every notification is checked to confirm the send really was zero-copy (`IORING_SEND_ZC_REPORT_USAGE` never reports a copy). kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

| Variant | AF_UNIX 16 KiB | AF_UNIX 64 KiB | AF_UNIX 256 KiB | TCP 16 KiB | TCP 64 KiB | TCP 256 KiB |
|---|---:|---:|---:|---:|---:|---:|
| `write(2)` | 9.54 | 12.53 | 10.00 | 2.23 | 4.72 | 5.13 |
| kqring `SEND` | 7.91 (+21%) | 11.57 (+8%) | 10.05 (−1%) | 2.11 (+6%) | 4.60 (+3%) | 5.06 (+1%) |
| kqring `SEND_ZC`, registered buffer | 6.79 (+41%) | **13.39 (−6%)** | **15.90 (−37%)** | **2.81 (−21%)** | **5.55 (−15%)** | **6.74 (−24%)** |
| kqring `SEND_ZC`, unregistered buffer | 6.15 (+55%) | 11.63 (+8%) | 13.71 (−27%) | 2.71 (−18%) | 5.28 (−11%) | 6.44 (−20%) |

- **AF_UNIX:** the copy is cheap (one `memcpy` into the peer's receive buffer), so zero-copy only wins once messages are large enough that the copy dominates: from 64 KiB with a registered buffer, and by 256 KiB from any buffer.
- **TCP:** zero-copy wins at every size tested, because it saves a copy on a path that is much slower to begin with.
- **Registered vs unregistered:** an unregistered buffer is pinned and charged to `RLIMIT_MEMLOCK` on every send. That costs 3–14% against a registered buffer, whose pages are held once at registration.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | 14,506 | −10% |
| kqueue: `kevent` + `accept` | 16,134 | |
| kqring multishot `ACCEPT` | 14,984 | −7% |

### Pipes and files: the worker-thread hand-off

![Worker-thread chart](docs/bench/worker.svg)

**Pipe: a 64-byte message**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| `write` + `read`, no waiting (floor) | 616 | −40% |
| kqueue: `write` + `kevent` + `read` | 1,019 | |
| kqring `READ` | 5,547 | +445% |

**File: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | 1,106 | |
| kqring `READ`, one per enter | 5,540 | +401% |
| kqring `READ`, 32 per enter | 4,696 | +325% |

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | 9,272 | |
| kqring `TIMEOUT`, 1 µs | 5,051 | −46% |

Both are dominated by timer-interrupt scheduling, not by the API.

## Known gaps

- **Pipe and regular-file I/O always goes through a worker thread.** kqring hands every non-socket `READ`/`WRITE` to an in-process worker, even when the data is already in the pipe or the page cache. A worker round trip costs about 4.5 µs (the `IOSQE_ASYNC` floor row). Linux io_uring tries the I/O inline first and only hands it to a worker if it would block. Doing the same here, at least for reads that would complete from the page cache and for pipes with data waiting, should bring these rows close to `pread`/`read`.
- **Batched file reads barely scale.** 32 reads per enter only improve from 5.5 µs to 4.7 µs per read. That suggests the reads are mostly running one after another on the workers, not in parallel.
- **Small zero-copy sends on AF_UNIX are slower than copying.** Below 64 KiB the page holding and the deferred notification cost more than the `memcpy` they save. The threshold for falling back to a copy (`KQRING_ZC_MIN_LEN`, one page) could be raised for AF_UNIX.
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
