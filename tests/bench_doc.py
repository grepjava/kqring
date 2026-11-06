#!/usr/bin/env python3
"""Write BENCHMARK.MD from the raw runs in docs/bench/data/.

Every measured figure comes from the data. The prose around the figures is in
this file, so edit it here, not in BENCHMARK.MD. Run from anywhere:

    python3 tests/bench_doc.py

After new runs, also check the prose: it names the build measured, and its
conclusions were written for these results.
"""
import glob
import importlib.util
import os
import statistics

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'
spec = importlib.util.spec_from_file_location('br', R + 'tests/bench_report.py')
br = importlib.util.module_from_spec(spec)
spec.loader.exec_module(br)
D = R + 'docs/bench/data/'
main = glob.glob(D + 'bench.*.out') + glob.glob(D + 'disk.*.out') + glob.glob(D + 'ufs.*.out')
m = br.load(main)
mt = br.load_mt(glob.glob(D + 'mt.*.out'))
iops, p99 = br.load_disk(glob.glob(D + 'diskbench.*'))


def v(g, k):
    return m[(g, k)][0]


def n(g, k):
    return f"{v(g, k):,.0f}"


def d(a, b):
    """a against b, as a signed percentage with a real minus sign."""
    p = (a - b) / b * 100
    r = round(p)
    return "0%" if r == 0 else (f"+{r}%" if r > 0 else f"−{-r}%")


def rel(g, k, base):
    return d(v(g, k), v(g, base))


def less(a, b):
    return f"{(b - a) / b * 100:.0f}%"


def spread(*keys):
    vals = [v(*k) for k in keys]
    return min(vals), max(vals)


def gbs(g, k):
    return int(g.rsplit('_', 1)[1].rstrip('k')) * 1024 / v(g, k)


def io(label, dirn='read'):
    return iops[(label, dirn)][0]


def i0(label, dirn='read'):
    return f"{io(label, dirn):,.0f}"


def mtv(w, k, t):
    return mt[(w, k, t)][0]


def lat(label, dirn='read'):
    return f"{p99[(label, dirn)][0]:,.0f}"


def floor_row(label, k, base=True):
    return f"| {label} | {n('floor', k)} | {'' if k == 'kqueue_empty_kevent' else rel('floor', k, 'kqueue_empty_kevent')} |"


def pp(k, bold=False):
    cells = []
    for g in ('pingpong_unix_64', 'pingpong_unix_1024', 'pingpong_unix_4096'):
        s = f"{n(g, k)} ({rel(g, k, 'kqueue')})"
        cells.append(f"**{s}**" if bold else s)
    return " | ".join(cells)


def fan(k, bold=False):
    cells = []
    for g in ('fanin_16', 'fanin_256', 'fanin_1024'):
        s = f"{n(g, k)} ({rel(g, k, 'kqueue')})"
        cells.append(f"**{s}**" if bold else s)
    return " | ".join(cells)


def send(tr, k):
    cells = []
    for sz in ('16k', '32k', '64k', '256k'):
        g = f"send_{tr}_{sz}"
        s = f"{gbs(g, k):.2f}"
        if k != 'plain_write':
            r = rel(g, k, 'plain_write')
            s = f"{s} ({r})"
            if v(g, k) < v(g, 'plain_write') * 0.9:
                s = f"**{s}**"
        cells.append(s)
    return " | ".join(cells)


def echo(k):
    cells = []
    for g in ('echo_unix_64', 'echo_unix_4096', 'echo_tcp_64'):
        cells.append(f"{n(g, k)} / {n(g, k + '_server_cpu')}")
    return " | ".join(cells)


def mtrow(label, w, k):
    return f"| {label} | " + " | ".join(f"{mtv(w, k, t):.2f}" for t in (1, 2, 4, 8)) + " |"


def mdrow(label, key):
    return (f"| {label} | {i0(key + '_1')} | {i0(key + '_32')} | "
            f"{i0(key + '_1', 'write')} | {i0(key + '_32', 'write')} |")


fan_lo = min(1 - v(g, k) / v(g, 'kqueue') for g in ('fanin_16', 'fanin_256', 'fanin_1024')
             for k in ('kqring_multishot', 'kqring_recv_batch'))
fan_hi = max(1 - v(g, k) / v(g, 'kqueue') for g in ('fanin_16', 'fanin_256', 'fanin_1024')
             for k in ('kqring_multishot', 'kqring_recv_batch'))
trig = [1 - v(g, 'kqring_trigger_server_cpu') / v(g, 'kqueue_server_cpu')
        for g in ('echo_unix_64', 'echo_unix_4096', 'echo_tcp_64')]
ppm = [1 - mtv('pingpong_unix_64', 'kqring_recv_onebuf', t) ** -1 * mtv('pingpong_unix_64', 'kqueue', t) for t in (1, 2, 4, 8)]
ppg = [mtv('pingpong_unix_64', 'kqring_recv_onebuf', t) / mtv('pingpong_unix_64', 'kqueue', t) - 1 for t in (1, 2, 4, 8)]
zfsr = {q: (i0(f'zfs_psync_{q}'), i0(f'zfs_aio_{q}'), i0(f'zfs_kqring_{q}'), lat(f'zfs_kqring_{q}')) for q in (1, 4, 16, 64)}
w32 = io('md_kqring_32', 'write') / io('md_aio_32', 'write') - 1
r32 = io('md_kqring_32') / io('md_aio_32') - 1
q1psync = io('md_psync_1') / io('md_kqring_1') - 1

doc = f"""# kqring benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads, and with `aio(4)` and plain system calls on storage. Where kqueue has no role (regular files, disks, bulk sends into a socket with room), the comparison is the plain system call.

Every figure here is from one build, [`v0.3.0`](https://github.com/grepjava/kqring/releases/tag/v0.3.0), on the machine under [Setup](#setup), except the two fio sections, which say which build they used. The raw runs are in [`docs/bench/data/`](docs/bench/data/).

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Many connections: kqring is {fan_lo * 100:.0f}–{fan_hi * 100:.0f}% faster.** When many sockets are ready at once, one `kqring_enter()` collects every message, where kqueue needs one `recv(2)` per message after the `kevent(2)`.
- **One connection: faster at every message size, if the request is already waiting or the buffer is reused.** On AF_UNIX, `RECV` into one reused buffer is {less(v('pingpong_unix_4096', 'kqring_recv_onebuf'), v('pingpong_unix_4096', 'kqueue'))}–{less(v('pingpong_unix_64', 'kqring_recv_onebuf'), v('pingpong_unix_64', 'kqueue'))} faster than kqueue, and multishot `RECV` {less(v('pingpong_unix_4096', 'kqring_multishot'), v('pingpong_unix_4096', 'kqueue'))}–{less(v('pingpong_unix_64', 'kqring_multishot'), v('pingpong_unix_64', 'kqueue'))}. On TCP loopback the stack dominates, and all of them are within a few percent.
- **A one-shot `RECV` armed before its data arrives costs more than kqueue:** {rel('pingpong_unix_64', 'kqring_recv_armed', 'kqueue')} at 64 B. It pays a second enter and installs and removes a socket upcall every time. Use multishot `RECV` for steady receive.
- **Batching is where a ring wins most.** 32 `NOP`s per enter cost {n('floor', 'kqring_nop_batch32')} ns each, against {n('floor', 'kqueue_empty_kevent')} ns for an empty `kevent(2)`. One `NOP` per enter costs {n('floor', 'kqring_nop')} ns, {rel('floor', 'kqring_nop', 'kqueue_empty_kevent')} against `kevent(2)`.
- **Disk devices go straight to the driver.** With 32 in flight on an md(4) disk, a 4 KiB read costs {n('disk_read_4k', 'kqring_read_batch32')} ns, {less(v('disk_read_4k', 'kqring_read_batch32'), v('disk_read_4k', 'pread'))} less than `pread(2)`.
- **On a real SSD, kqring reaches what the drive can do with half of aio(4)'s CPU:** 98,876 IOPS at 32 in flight on the raw disk at 45% CPU, against 97% for aio. On a ZFS file it runs within 5% of aio at less than half the CPU.
- **Rings on several threads scale.** On md(4) disks, 4 jobs reach 1,031,000 IOPS against 323,000 for aio. On a cached ZFS file, kqring stays near 530,000 at any job count, ahead of aio; `psync` passes it from 4 jobs.
- **Zero-copy send pays off from 32–64 KiB.** From a registered buffer, `SEND_ZC` takes {less(v('send_unix_256k', 'kqring_send_zc_fixed'), v('send_unix_256k', 'plain_write'))} less time than `write(2)` for 256 KiB on AF_UNIX, and {less(v('send_tcp_256k', 'kqring_send_zc_fixed'), v('send_tcp_256k', 'plain_write'))} less on TCP loopback.
- **Pipes are {less(v('pipe', 'kqring_read'), v('pipe', 'kqueue'))} faster than kqueue when data is there.** Only a read that has to wait goes to a worker.
- **Cached file reads depend on the filesystem.** On UFS, 32 `READ`s per enter are {less(v('ufs_file_4k', 'kqring_read_batch32'), v('ufs_file_4k', 'pread'))} faster than `pread(2)`, and one per enter is {rel('ufs_file_4k', 'kqring_read', 'pread')}. ZFS has no page-cache read path, so its reads go through a worker and lose to `pread(2)`.
- **An event-driven chain needs less server CPU than kqueue:** {min(trig) * 100:.0f}–{max(trig) * 100:.0f}% per message in an echo server.

## Setup

| | |
|---|---|
| CPU | Intel Core i5-8250U, 4 cores / 8 threads, 1.6 GHz base |
| Memory | 4 GiB |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; root on ZFS |
| kqring | `kqring.ko` from `v0.3.0`, entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \\| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
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
{floor_row('`kevent(2)`, nothing to do', 'kqueue_empty_kevent')}
{floor_row('kqring `NOP`, one per enter', 'kqring_nop')}
{floor_row('kqring `NOP`, 32 per enter', 'kqring_nop_batch32')}
{floor_row('kqring `NOP` with `IOSQE_ASYNC`, through a worker', 'kqring_nop_async')}

A single enter costs {v('floor', 'kqring_nop') - v('floor', 'kqueue_empty_kevent'):.0f} ns more than an empty `kevent`, because it parses an SQE and posts a CQE. Batching spreads the rest over many requests. The `IOSQE_ASYNC` row is a full round trip through a worker thread, kept short by both sides spinning briefly when a CPU is free.

### Ping-pong: one message written, then received, on one connection

![Ping-pong chart](docs/bench/pingpong.svg)

**AF_UNIX stream**

| Variant | 64 B | 1 KiB | 4 KiB |
|---|---:|---:|---:|
| `write` + `recv`, no waiting (floor) | {n('pingpong_unix_64', 'plain_recv')} | {n('pingpong_unix_1024', 'plain_recv')} | {n('pingpong_unix_4096', 'plain_recv')} |
| kqueue: `write` + `kevent` + `recv` | {n('pingpong_unix_64', 'kqueue')} | {n('pingpong_unix_1024', 'kqueue')} | {n('pingpong_unix_4096', 'kqueue')} |
| kqring `RECV`, provided buffer | {pp('kqring_recv')} |
| kqring `RECV`, one reused buffer | {pp('kqring_recv_onebuf', True)} |
| kqring multishot `RECV`, provided buffers | {pp('kqring_multishot')} |
| kqring `RECV` armed before the data arrives | {pp('kqring_recv_armed')} |

The provided-buffer variants rotate through a 16 MiB buffer ring (4,096 × 4 KiB), and the cache misses that causes show as the gap to the reused-buffer row, wider as messages grow. Programs that use fewer, smaller buffers should see results close to the reused-buffer row.

**TCP loopback, 64 B, `TCP_NODELAY`**

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue: `write` + `kevent` + `recv` | {n('pingpong_tcp_64', 'kqueue')} | |
| kqring `RECV`, provided buffer | {n('pingpong_tcp_64', 'kqring_recv')} | {rel('pingpong_tcp_64', 'kqring_recv', 'kqueue')} |
| kqring `RECV`, one reused buffer | {n('pingpong_tcp_64', 'kqring_recv_onebuf')} | {rel('pingpong_tcp_64', 'kqring_recv_onebuf', 'kqueue')} |
| kqring multishot `RECV` | {n('pingpong_tcp_64', 'kqring_multishot')} | {rel('pingpong_tcp_64', 'kqring_multishot', 'kqueue')} |
| kqring `RECV` armed before the data arrives | {n('pingpong_tcp_64', 'kqring_recv_armed')} | {rel('pingpong_tcp_64', 'kqring_recv_armed', 'kqueue')} |

TCP is dominated by the loopback stack (about 5 µs per message), so the mechanisms differ by a few percent.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | {n('fanin_16', 'kqueue')} | {n('fanin_256', 'kqueue')} | {n('fanin_1024', 'kqueue')} |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | {fan('kqring_multishot', True)} |
| kqring: N single-shot `RECV`s submitted and completed in one enter | {fan('kqring_recv_batch', True)} |

This is the shape of a busy server, and where the ring model pays off: kqring makes one system call for N messages, kqueue makes N + 1.

### Echo server: an event-driven chain against kqueue

`kqring_bench echo`. A server thread echoes each message to a client in another thread, which sends one and waits for it. Each cell is round trip / server CPU per message, in ns.

![Echo server chart](docs/bench/echo.svg)

| Server | AF_UNIX 64 B | AF_UNIX 4 KiB | TCP 64 B |
|---|---:|---:|---:|
| kqueue: `kevent` + `read` + `write` | {echo('kqueue')} |
| kqring io_uring style: each `RECV` CQE → submit `SEND` + next `RECV` | {echo('kqring_recv_send')} |
| kqring trigger: `RECV` → `SEND`, `LEN_FROM_PREV`, no CQE per message | **{echo('kqring_trigger')}** |

Against kqueue, the trigger (an [event-driven chain](docs/event-chains-spec.md), opted into per ring) takes {trig[0] * 100:.0f}%, {trig[1] * 100:.0f}% and {trig[2] * 100:.0f}% less server CPU per message. The server never returns to user space between messages.

### Bulk send: one message sent, then read back in full

![Bulk send over AF_UNIX](docs/bench/send_unix.svg)
![Bulk send over TCP](docs/bench/send_tcp.svg)

Each iteration sends one message on a connection with 1 MiB socket buffers, then reads it back with `recv(MSG_WAITALL)`. The zero-copy variants rotate through 8 buffers and reuse one only after its `IORING_CQE_F_NOTIF` has arrived, as a program must. kqueue adds nothing while the send buffer has room, so the baseline is `write(2)`.

Throughput in GB/s (higher is better), with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | {send('unix', 'plain_write')} |
| kqring `SEND` | {send('unix', 'kqring_send')} |
| kqring `SEND_ZC`, registered buffer | {send('unix', 'kqring_send_zc_fixed')} |
| kqring `SEND_ZC`, unregistered buffer | {send('unix', 'kqring_send_zc')} |

**TCP loopback**

| Variant | 16 KiB | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | {send('tcp', 'plain_write')} |
| kqring `SEND` | {send('tcp', 'kqring_send')} |
| kqring `SEND_ZC`, registered buffer | {send('tcp', 'kqring_send_zc_fixed')} |
| kqring `SEND_ZC`, unregistered buffer | {send('tcp', 'kqring_send_zc')} |

- **When kqring sends zero-copy.** A registered buffer's pages are held once, at registration, so a send from one goes zero-copy from one page up. An unregistered buffer has to be pinned and charged to `RLIMIT_MEMLOCK` on every send. On AF_UNIX, where the alternative is a single `memcpy`, that costs more than copying below about 32 KiB. So kqring copies those sends itself and reports `IORING_NOTIF_USAGE_ZC_COPIED`, as io_uring allows. The threshold is `sysctl kern.kqring.zc_local_min_len`, 32 KiB by default. TCP always uses one page.
- **TCP loopback delivers through another thread.** The loopback interface hands every packet to a netisr thread, so the benchmark's `recv()` right after the send often has to wait for that thread. For small TCP sends that wait dominates, and the results depend on timing.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | {n('accept', 'plain_accept')} | {rel('accept', 'plain_accept', 'kqueue')} |
| kqueue: `kevent` + `accept` | {n('accept', 'kqueue')} | |
| kqring multishot `ACCEPT` | {n('accept', 'kqring_multishot')} | {rel('accept', 'kqring_multishot', 'kqueue')} |
| kqring multishot `ACCEPT`, 50 µs busy-poll | **{n('accept', 'kqring_multishot_busypoll')}** | **{rel('accept', 'kqring_multishot_busypoll', 'kqueue')}** |

The enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays for a wake-up. With `IORING_REGISTER_NAPI` set to 50 µs, multishot `ACCEPT` avoids the sleep, at the cost of CPU time on waits that don't end quickly.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | {n('timer_1us', 'kqueue')} | |
| kqring `TIMEOUT`, 1 µs | {n('timer_1us', 'kqring_timeout')} | {rel('timer_1us', 'kqring_timeout', 'kqueue')} |

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
| `write` + `read`, no waiting (floor) | {n('pipe', 'plain_read')} | {rel('pipe', 'plain_read', 'kqueue')} |
| kqueue: `write` + `kevent` + `read` | {n('pipe', 'kqueue')} | |
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | **{n('pipe', 'kqring_read_nonblock')}** | **{rel('pipe', 'kqring_read_nonblock', 'kqueue')}** |
| kqring `READ`, blocking pipe, data ready (inline) | {n('pipe', 'kqring_read')} | {rel('pipe', 'kqring_read', 'kqueue')} |

**File on UFS: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`. The file is on a UFS file system on an md(4) disk.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | {n('ufs_file_4k', 'pread')} | |
| kqring `READ`, one per enter | {n('ufs_file_4k', 'kqring_read')} | {rel('ufs_file_4k', 'kqring_read', 'pread')} |
| kqring `READ`, one per enter, registered file | {n('ufs_file_4k', 'kqring_read_fixed')} | {rel('ufs_file_4k', 'kqring_read_fixed', 'pread')} |
| kqring `READ`, 32 per enter | **{n('ufs_file_4k', 'kqring_read_batch32')}** | **{rel('ufs_file_4k', 'kqring_read_batch32', 'pread')}** |

A registered file saves almost nothing, so descriptor lookups aren't where the single-read gap comes from. It is the fixed cost of an enter, plus the SQE and CQE.

**File on ZFS.** ZFS caches in the ARC rather than the page cache, so `READ` goes to a worker. These reads don't give one stable figure on this machine: the same file in `/tmp` (lz4, 128 KiB records) runs in a fast mode or a slow one from run to run, and plain `pread(2)` does too. In the fast mode, `pread(2)` takes about 1,510 ns and one `READ` per enter about 2,490 ns. In the slow mode, `pread(2)` takes 6,000–14,000 ns. 32 `READ`s per enter have measured anywhere from about 2,250 to 20,000 ns, so no single figure is given for them. For ZFS, the [fio tables](#fio-on-a-real-ssd) are the better guide.

### Disk device: 4 KiB random reads and writes

A positioned `READ` or `WRITE` on a disk device goes straight to the driver as a bio over the held user pages, as `aio(4)` does, and completes from the bio callback. This is an md(4) malloc disk with every block written first, so it measures the I/O path, not media. A block that was never written costs md far less to read, so an empty disk gives figures that are too good.

| Variant | Read | vs `pread` | Write | vs `pwrite` |
|---|---:|---:|---:|---:|
| `pread(2)` / `pwrite(2)` | {n('disk_read_4k', 'pread')} | | {n('disk_write_4k', 'pwrite')} | |
| kqring, one per enter, to the driver | {n('disk_read_4k', 'kqring_read')} | {rel('disk_read_4k', 'kqring_read', 'pread')} | {n('disk_write_4k', 'kqring_write')} | {rel('disk_write_4k', 'kqring_write', 'pwrite')} |
| kqring, one per enter, `IOSQE_ASYNC` through a worker | {n('disk_read_4k', 'kqring_read_worker')} | {rel('disk_read_4k', 'kqring_read_worker', 'pread')} | {n('disk_write_4k', 'kqring_write_worker')} | {rel('disk_write_4k', 'kqring_write_worker', 'pwrite')} |
| kqring, 32 per enter, to the driver | **{n('disk_read_4k', 'kqring_read_batch32')}** | **{rel('disk_read_4k', 'kqring_read_batch32', 'pread')}** | **{n('disk_write_4k', 'kqring_write_batch32')}** | **{rel('disk_write_4k', 'kqring_write_batch32', 'pwrite')}** |
| kqring, 32 per enter, `IOSQE_ASYNC` through workers | {n('disk_read_4k', 'kqring_read_worker_batch32')} | {rel('disk_read_4k', 'kqring_read_worker_batch32', 'pread')} | | |

With one request per enter, kqring pays one sleep and wake-up more than `pread(2)`, since the owner waits in `kqring_enter()` for the bio callback's taskwork. With 32 in flight, the driver gets them all at once, which `pread(2)` can't do from one thread.

### Storage workloads: queue depth, aio(4) and `pread(2)` threads

[`tests/kqring_diskbench.c`](tests/kqring_diskbench.c) runs storage-benchmark-style workloads: a fixed number of I/Os in flight per job, random or sequential offsets, for a fixed time after a 1 s ramp, through kqring, `aio(4)` or threads calling `pread(2)` / `pwrite(2)`. All figures are IOPS.

**A 1 GiB md(4) malloc disk, 4 KiB random I/O** (the bio path; md serves every bio from one thread). `+F+B` is a fixed file and registered buffers, `+P` `SQPOLL` and `+I` `IOPOLL`:

| Variant | Read, 1 | Read, 32 | Write, 1 | Write, 32 |
|---|---:|---:|---:|---:|
{mdrow('`pread` / `pwrite` threads', 'md_psync')}
{mdrow('aio', 'md_aio')}
{mdrow('kqring', 'md_kqring')}
{mdrow('kqring `+F+B`', 'md_kqringFB')}
{mdrow('kqring `+P`', 'md_kqringP')}
{mdrow('kqring `+I`', 'md_kqringI')}

At 32 in flight, kqring writes are {abs(w32) * 100:.0f}% {'ahead of' if w32 > 0 else 'behind'} aio. Reads at 32 in flight vary by up to 24% between runs for every engine, so the differences in that column aren't meaningful: kqring and aio are level. One at a time, a thread calling `pread(2)` is {q1psync * 100:.0f}% faster, as in the disk group above.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring | kqring p99 µs |
|---:|---:|---:|---:|---:|
| 1 | {zfsr[1][0]} | {zfsr[1][1]} | {zfsr[1][2]} | {zfsr[1][3]} |
| 4 | {zfsr[4][0]} | {zfsr[4][1]} | {zfsr[4][2]} | {zfsr[4][3]} |
| 16 | {zfsr[16][0]} | {zfsr[16][1]} | {zfsr[16][2]} | {zfsr[16][3]} |
| 64 | {zfsr[64][0]} | {zfsr[64][1]} (at 32) | {zfsr[64][2]} | {zfsr[64][3]} |

aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`).

**Waiting for several completions.** An enter with `min_complete` = N is woken once N plain `READ`/`WRITE` completions are queued, rather than for each one, which saves context switches more than it adds IOPS. On the md disk at 32 in flight:

| Workload | Wait for 1 | Wait for 16 |
|---|---:|---:|
| Reads | {i0('wait_1')} | {i0('wait_16')} |
| Writes | {i0('wait_1', 'write')} | {i0('wait_16', 'write')} |

**`SQPOLL`.** The `SQPOLL` thread polls for the whole of `sq_thread_idle` while it's active, as io_uring's does. `-C` also polls the CQ from user space instead of entering to wait, which is how a latency-bound program would use it. 4 KiB random reads on the md disk:

| In flight | plain kqring | `SQPOLL` | `SQPOLL`, `-C` |
|---:|---:|---:|---:|
| 1 | {i0('sq_plain_1')} | {i0('sq_P_1')} | **{i0('sq_PC_1')}** |
| 4 | {i0('sq_plain_4')} | {i0('sq_P_4')} | **{i0('sq_PC_4')}** |
| 32 | {i0('sq_plain_32')} | {i0('sq_P_32')} | **{i0('sq_PC_32')}** |

`kqring_diskbench -V` checks every read against what was last written to that block and re-reads the whole area at the end. Mixed random reads and writes passed it on both the md disk and the ZFS file on this build.

### Several threads: one ring per thread

[`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) runs each workload on 1, 2, 4 and 8 threads, each pinned to its own CPU (CPUs 2n and 2n+1 are the two hyperthreads of one core) with its own ring, kqueue or socket pair. That's the thread-per-core model `SINGLE_ISSUER` rings are made for. Figures are millions of operations per second for all threads together, from runs of 2 s.

![Ping-pong with one ring per thread](docs/bench/threads.svg)

| Workload | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
{mtrow('`kevent(2)`, nothing to do', 'floor', 'kqueue_empty_kevent')}
{mtrow('kqring `NOP`, one per enter', 'floor', 'kqring_nop')}
{mtrow('kqring `NOP`, 32 per enter', 'floor', 'kqring_nop_batch32')}
{mtrow('AF_UNIX ping-pong, 64 B, kqueue', 'pingpong_unix_64', 'kqueue')}
{mtrow('AF_UNIX ping-pong, 64 B, kqring `RECV` into one buffer', 'pingpong_unix_64', 'kqring_recv_onebuf')}

Ping-pong stays ahead of kqueue at every thread count, by {max(ppg) * 100:.0f}% on 1 thread and {min(ppg[1:]) * 100:.0f}–{sorted(ppg)[2] * 100:.0f}% on more. What's left at 8 threads is per-request work, since two threads share each core. The benchmark's shared-file workload isn't shown: its file is in `/tmp`, which is ZFS here, and it ran in the slow mode described under [Pipes and files](#pipes-and-files-inline-or-through-a-worker).

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
"""
open(R + 'BENCHMARK.MD', 'wb').write(doc.encode())
print('ok', len(doc))
