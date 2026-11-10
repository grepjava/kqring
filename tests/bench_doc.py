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
main = glob.glob(D + 'bench.*.out') + glob.glob(D + 'disk.*.out') + glob.glob(D + 'ufs.*.out') + \
    glob.glob(D + 'stream.*.out')
m = br.load(main)
mt = br.load_mt(glob.glob(D + 'mt.*.out'))
iops, p99 = br.load_disk(glob.glob(D + 'diskbench.*'))
fio = br.load_csv([D + 'fio.csv'])
# The same benchmarks on the i5-8250U laptop, v0.3.0, kept for the comparison
# in "Why the results depend on the CPU".
I5 = R + 'docs/bench/data-i5-8250u/'
mi5 = br.load(glob.glob(I5 + 'bench.*.out'))
mti5 = br.load_mt(glob.glob(I5 + 'mt.*.out'))
# v0.3.0 against HEAD on this machine, alternated: the median change per row,
# leaving out the sends and the timer, which vary most between runs.
AB = R + 'docs/bench/data-v030-vs-head/'
ab_old = br.load(glob.glob(AB + 'kqring-v030.*.out'))
ab_new = br.load(glob.glob(AB + 'kqring-p6.*.out'))
ab_med = statistics.median(ab_new[k][0] / ab_old[k][0] - 1 for k in ab_old
                           if not k[0].startswith(('send', 'timer')))


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


def win(g, k, base):
    """The ns and percentage cells of k against base, bold only if k wins."""
    a, b = n(g, k), rel(g, k, base)
    return f"**{a}** | **{b}**" if v(g, k) < v(g, base) else f"{a} | {b}"


def less(a, b):
    return f"{(b - a) / b * 100:.0f}%"


def spread(*keys):
    vals = [v(*k) for k in keys]
    return min(vals), max(vals)


def gbs(g, k):
    return int(g.rsplit('_', 1)[1].rstrip('k')) * 1024 / v(g, k)


def faster(a, b):
    """Time a against time b: "N% faster", "N% slower" or "level"."""
    p = round((b - a) / b * 100)
    return "level" if p == 0 else (f"{p}% faster" if p > 0 else f"{-p}% slower")


def fv(section, series, x):
    """A value from fio.csv."""
    for s_, x_, val in fio[section]:
        if s_ == series and x_ == str(x):
            return val
    raise KeyError((section, series, x))


def f0(section, series, x):
    return f"{fv(section, series, x):,.0f}"


def raw_runs(g, k):
    out = []
    for f in glob.glob(D + 'bench.*.out'):
        for line in open(f, encoding='utf-8'):
            x = line.split()
            if len(x) == 4 and x[0] == 'ROW' and x[1] == g and x[2] == k:
                out.append(float(x[3]))
    return sorted(out)


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
        # Bold marks a win only.
        cells.append(f"**{s}**" if bold and v(g, k) < v(g, 'kqueue') else s)
    return " | ".join(cells)


def fan(k, bold=False):
    cells = []
    for g in ('fanin_16', 'fanin_256', 'fanin_1024'):
        s = f"{n(g, k)} ({rel(g, k, 'kqueue')})"
        # Bold marks a win only.
        cells.append(f"**{s}**" if bold and v(g, k) < v(g, 'kqueue') else s)
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


def stream(tr, k):
    cells = []
    for sz in ('4k', '16k', '64k', '256k'):
        g = f"stream_{tr}_{sz}"
        s = f"{gbs(g, k):.2f}"
        if k != 'plain_write':
            s = f"{s} ({rel(g, k, 'plain_write')})"
            if v(g, k) < v(g, 'plain_write') * 0.9:
                s = f"**{s}**"
        cells.append(s)
    return " | ".join(cells)


def scpu(k):
    """The sending thread's CPU per message, in µs, at 64 and 256 KiB."""
    return " | ".join(f"{v(f'stream_{tr}_{sz}', k + '_sender_cpu') / 1000:.1f}"
                      for tr in ('unix', 'tcp') for sz in ('64k', '256k'))


def stream_sends(g, k):
    """Median SQEs per message where sends came back short, from the
    '# <group> <variant>: S sends for M messages, C copied' lines."""
    r = []
    for f in glob.glob(D + 'stream.*.out'):
        for line in open(f, encoding='utf-8'):
            p = line.split()
            if line.startswith('# ') and p[1] == g and p[2] == k + ':':
                r.append((int(p[3]) / int(p[6]), int(p[8]) / int(p[6])))
    return (statistics.median(x[0] for x in r), statistics.median(x[1] for x in r)) if r else (1.0, 0.0)


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
fan_w = (f"{fan_lo * 100:.0f}–{fan_hi * 100:.0f}% faster than kqueue" if fan_lo > 0 else
         f"{-fan_hi * 100:.0f}–{-fan_lo * 100:.0f}% slower than kqueue" if fan_hi < 0 else
         f"between {-fan_lo * 100:.0f}% slower and {fan_hi * 100:.0f}% faster than kqueue")
fan_note = ("That saving is where the ring model pays off." if fan_lo > 0 else
            "On this machine kqring's work per message, a CQE and a buffer, costs more than the system calls it saves; on the Intel laptop, where each call costs more, it was the other way ([Why the results depend on the CPU](#why-the-results-depend-on-the-cpu)).")
pp_note = (f"Ping-pong stays ahead of kqueue at every thread count, by {max(ppg) * 100:.0f}% on 1 thread and {min(ppg[1:]) * 100:.0f}–{max(ppg[1:]) * 100:.0f}% on more." if min(ppg) > 0 else
           f"Ping-pong is behind kqueue at every thread count, by {-max(ppg) * 100:.0f}–{-min(ppg) * 100:.0f}%, as in the single-threaded group." if max(ppg) < 0 else
           f"Ping-pong is within {max(abs(x) for x in ppg) * 100:.0f}% of kqueue at every thread count.")
def vs_aio(x):
    return "level with" if round(abs(x) * 100) == 0 else f"{abs(x) * 100:.0f}% {'ahead of' if x > 0 else 'behind'}"


ZD = D + 'zfs-dbuf/'
zfit = br.load(glob.glob(ZD + 'file-s3.*.out'))
zthr = br.load(glob.glob(ZD + 'file-s5.*.out'))
def zd_note(shift, field):
    """MiB figure from the '# file_4k start:' lines of one regime's runs."""
    vals = {int(l.split(field + ' ')[1].split()[0]) for f in glob.glob(ZD + f'file-s{shift}.*.out')
            for l in open(f, encoding='utf-8') if l.startswith('# file_4k start:')}
    assert len(vals) == 1, (shift, field, vals)
    return vals.pop()
z_arc, z_t5, z_t3 = zd_note(5, 'ARC target'), zd_note(5, 'dbuf cache target'), zd_note(3, 'dbuf cache target')
def zd_series(name):
    rows = [l.split() for l in open(ZD + name, encoding='utf-8') if not l.startswith('#')]
    return [(float(r[0]), float(r[1])) for r in rows]
zs_f = zd_series('zfs_dbuf.f.out')
zs_r = zd_series('zfs_dbuf.random.out')
z_before = statistics.median(ns for t, ns in zs_f + zs_r if ns < 2000)
z_after = statistics.median(ns for _, ns in zs_f[-3:] + zs_r[-3:])
z_switch = sorted(max(t for t, ns in z if ns < 2000) for z in (zs_f, zs_r))
def zd_prof(name, fn):
    c = t = None
    for l in open(ZD + name, encoding='utf-8'):
        x = l.split()
        if len(x) == 2 and x[0].endswith(fn):
            c = int(x[1])
        elif len(x) == 2 and x[0] == 'total':
            t = int(x[1])
    return 100 * c / t
z_lz4 = zd_prof('profile.f.txt', 'LZ4_uncompress_unknownOutputSize')
z_cpy = zd_prof('profile.random.txt', 'memcpy_std')
AB_SLOW = sorted(os.path.basename(f) for f in glob.glob(AB + '*.out')
                 if any(l.startswith('ROW file_4k') and float(l.split()[3]) > 5000 for l in open(f, encoding='utf-8')))
def zrow(label, k):
    return (f"| {label} | {zfit[('file_4k', k)][0]:,.0f} | {'' if k == 'pread' else faster(zfit[('file_4k', k)][0], zfit[('file_4k', 'pread')][0])} "
            f"| {zthr[('file_4k', k)][0]:,.0f} | {'' if k == 'pread' else faster(zthr[('file_4k', k)][0], zthr[('file_4k', 'pread')][0])} |")
zspread = max(x[1] for x in list(zfit.values()) + list(zthr.values())) * 100


def eng_gap(sec, qds):
    return max((max(v) - min(v)) / min(v) for v in
               ([fv(sec, e, q) for e in ('kqring', 'aio', 'psync')] for q in qds))


raw_eng = eng_gap('raw', (1, 8))
zfs_eng = eng_gap('zfsread', (1, 32))
wspread = max(fv('zfswritespread', e, q) for e in ('kqring', 'aio', 'psync') for q in (1, 32))
rspread = max(fv('zfsreadspread', e, q) for e in ('kqring', 'aio', 'psync') for q in (1, 32))
dspread = max(iops[(k, 'read')][1] for k in ('md_kqring_32', 'md_aio_32'))
stable = sum(1 for k, x in m.items() if x[1] < 0.05 and not k[0].startswith('stream_'))
nstable = sum(1 for k in m if not k[0].startswith('stream_'))
spread_send = max(x[1] for k, x in m.items() if k[0].startswith('send_') and x[1] >= 0.05) * 100
spread_pp = max(x[1] for k, x in m.items() if k[0].startswith('pingpong_')) * 100
zfsr = {q: (i0(f'zfs_psync_{q}'), i0(f'zfs_aio_{q}'), i0(f'zfs_kqring_{q}'), lat(f'zfs_kqring_{q}')) for q in (1, 4, 16, 64)}
w32 = io('md_kqring_32', 'write') / io('md_aio_32', 'write') - 1
r32 = io('md_kqring_32') / io('md_aio_32') - 1
q1psync = io('md_psync_1') / io('md_kqring_1') - 1
md32spread = max((max(x) - min(x)) / min(x) for x in
                 ([io(f'md_{e}_32', rw) for e in ('psync', 'aio', 'kqring')] for rw in ('read', 'write')))
zq = [io(f'zfs_kqring_{q}') / io(f'zfs_psync_{q}') - 1 for q in (4, 16, 64)]
zq1 = io('zfs_kqring_1') / io('zfs_psync_1') - 1

STREAM_BUILD = '9918fea'
s_old = br.load(glob.glob(R + 'docs/bench/data-stream-7641bf4/stream.*.out'))
s_diff = [abs(m[k][0] / s_old[k][0] - 1) * 100 for k in s_old if not k[1].endswith('_cpu') and k in m]
s_med, s_max = statistics.median(s_diff), max(s_diff)
s_out = [abs(m[k][0] / s_old[k][0] - 1) * 100 for k in s_old if not k[1].endswith('_cpu') and k in m
         and abs(m[k][0] / s_old[k][0] - 1) > max(m[k][1], s_old[k][1])]
s_out_text = ("Every row's difference is within its spread between runs." if not s_out else
              f"All but {len(s_out)} of the rows differ by less than their spread between runs; "
              f"{'that one' if len(s_out) == 1 else 'those'} by {max(s_out):.1f}%.")
zc_rel = [1 - v(f'stream_{tr}_{sz}', 'kqring_send_zc_fixed') / v(f'stream_{tr}_{sz}', 'plain_write')
          for tr in ('unix', 'tcp') for sz in ('16k', '64k', '256k')]
zc_lo, zc_hi = min(zc_rel) * 100, max(zc_rel) * 100
rx_busy = v('stream_unix_256k', 'kqring_send_zc_fixed_receiver_cpu') / v('stream_unix_256k', 'kqring_send_zc_fixed') * 100
tcp_sends, tcp_copied = stream_sends('stream_tcp_256k', 'kqring_send_zc_fixed')
_, wa_copied = stream_sends('stream_tcp_256k', 'kqring_send_zc_fixed_waitall')
stream_spread = max(x[1] for k, x in m.items() if k[0].startswith('stream_') and not k[1].endswith('_cpu')) * 100
stream_spread_plain = max(x[1] for k, x in m.items() if k[0].startswith('stream_') and k[1] in ('plain_write', 'kqring_send')) * 100

doc = f"""# kqring benchmarks

How `kqring` compares with FreeBSD's `kqueue(2)` on the same workloads, and with `aio(4)` and plain system calls on storage. Where kqueue has no role (regular files, disks, bulk sends into a socket with room), the comparison is the plain system call.

Every figure here is from one build, `7641bf4`, on the machine under [Setup](#setup), an 8-thread AMD Ryzen, except the [streaming](#streaming-the-receiver-in-its-own-thread) group's and the [file on ZFS](#pipes-and-files-inline-or-through-a-worker) table, from `{STREAM_BUILD}`. The raw runs are in [`docs/bench/data/`](docs/bench/data/). Earlier editions were measured on an Intel laptop; how the two machines differ, and why it matters for kqring, is under [Why the results depend on the CPU](#why-the-results-depend-on-the-cpu). For kqring against Linux io_uring on one machine, see [docs/linux-comparison.md](docs/linux-comparison.md).

![kqring vs kqueue: speed-up per operation](docs/bench/summary.svg)

## Summary

- **Batching is where a ring wins.** 32 `NOP`s per enter cost {n('floor', 'kqring_nop_batch32')} ns each, against {n('floor', 'kqueue_empty_kevent')} ns for an empty `kevent(2)`. One `NOP` per enter costs {n('floor', 'kqring_nop')} ns, {rel('floor', 'kqring_nop', 'kqueue_empty_kevent')} against `kevent(2)`: that's kqring's fixed cost per enter, the SQE, the request and the CQE.
- **On this machine a system call is cheap, and saving one is worth less than kqring's own work per message.** Collecting many connections' messages (fan-in) is {fan_w}, though kqring makes one system call for N messages where kqueue makes N + 1. On one connection, `RECV` into one reused buffer is {faster(v('pingpong_unix_64', 'kqring_recv_onebuf'), v('pingpong_unix_64', 'kqueue'))} than kqueue at 64 B, and a pipe `READ` with data ready {faster(v('pipe', 'kqring_read'), v('pipe', 'kqueue'))}. On the Intel laptop, where a system call costs nearly three times as much, the same benchmarks put kqring ahead: see [below](#why-the-results-depend-on-the-cpu).
- **A one-shot `RECV` armed before its data arrives costs more than kqueue:** {rel('pingpong_unix_64', 'kqring_recv_armed', 'kqueue')} at 64 B. It pays a second enter and installs and removes a socket upcall every time. Use multishot `RECV` for steady receive.
- **Disk devices go straight to the driver.** With 32 in flight on an md(4) disk, a 4 KiB read costs {n('disk_read_4k', 'kqring_read_batch32')} ns, {less(v('disk_read_4k', 'kqring_read_batch32'), v('disk_read_4k', 'pread'))} less than `pread(2)`.
- **On a real SSD, kqring reaches what the drive can do with less CPU than aio(4):** {f0('raw', 'kqring', 32)} IOPS at 32 in flight on the raw disk at {f0('rawcpu', 'kqring', 32)}% CPU, against {f0('rawcpu', 'aio', 32)}% for aio.
- **Rings on several threads scale.** On md(4) disks, 4 jobs reach {fv('md', 'kqring', 4) * 1000:,.0f} IOPS against {fv('md', 'aio', 4) * 1000:,.0f} for aio. On a cached ZFS file, kqring reaches {fv('cached', 'kqring', 8) * 1000:,.0f} at 8 jobs, ahead of aio; `psync` is ahead from 4 jobs.
- **Zero-copy send pays off for large messages.** From a registered buffer, `SEND_ZC` takes {less(v('send_unix_256k', 'kqring_send_zc_fixed'), v('send_unix_256k', 'plain_write'))} less time than `write(2)` for 256 KiB on AF_UNIX, and {less(v('send_tcp_256k', 'kqring_send_zc_fixed'), v('send_tcp_256k', 'plain_write'))} less on TCP loopback. When one thread sends and then reads, it costs more at 64 KiB and below; with the receiver in its own thread, it takes {zc_lo:.0f}–{zc_hi:.0f}% less time from 16 KiB up ([Streaming](#streaming-the-receiver-in-its-own-thread)).
- **Cached file reads depend on the filesystem.** On UFS, 32 `READ`s per enter are {faster(v('ufs_file_4k', 'kqring_read_batch32'), v('ufs_file_4k', 'pread'))} than `pread(2)`, and one per enter is {rel('ufs_file_4k', 'kqring_read', 'pread')}. ZFS has no page-cache read path, so its reads go through a worker or, once the CPUs are taken, run in the enter.
- **An event-driven chain needs less server CPU than kqueue:** {min(trig) * 100:.0f}–{max(trig) * 100:.0f}% per message in an echo server.

## Why the results depend on the CPU

kqring saves system calls, and does more work per request than the call it replaces: it reads an SQE, sets up a request and posts a CQE. Which of the two wins depends on what a system call costs, and that depends on the CPU and its security mitigations. On the Intel i5-8250U laptop these tables used to come from, FreeBSD isolates the kernel's page tables (PTI) against Meltdown, and every system call pays for it; AMD's CPUs aren't affected, so the Ryzen doesn't. The same `kqring_bench` groups on both (the laptop's runs are from `v0.3.0`, in [`docs/bench/data-i5-8250u/`](docs/bench/data-i5-8250u/); ns per operation):

| | i5-8250U, PTI | Ryzen 3 PRO 4350G |
|---|---:|---:|
| Empty `kevent(2)` | {mi5[('floor', 'kqueue_empty_kevent')][0]:,.0f} | {n('floor', 'kqueue_empty_kevent')} |
| kqring `NOP`, one per enter | {mi5[('floor', 'kqring_nop')][0]:,.0f} | {n('floor', 'kqring_nop')} |
| kqring `NOP`, 32 per enter | {mi5[('floor', 'kqring_nop_batch32')][0]:,.0f} | {n('floor', 'kqring_nop_batch32')} |
| Fan-in, N = 256: kqueue | {mi5[('fanin_256', 'kqueue')][0]:,.0f} | {n('fanin_256', 'kqueue')} |
| Fan-in, N = 256: kqring multishot `RECV` | {mi5[('fanin_256', 'kqring_multishot')][0]:,.0f} | {n('fanin_256', 'kqring_multishot')} |
| AF_UNIX ping-pong, 64 B: kqueue | {mi5[('pingpong_unix_64', 'kqueue')][0]:,.0f} | {n('pingpong_unix_64', 'kqueue')} |
| AF_UNIX ping-pong, 64 B: kqring `RECV`, one buffer | {mi5[('pingpong_unix_64', 'kqring_recv_onebuf')][0]:,.0f} | {n('pingpong_unix_64', 'kqring_recv_onebuf')} |

The kqueue rows, which are mostly system calls, got much cheaper on the Ryzen; kqring's work per request less so. So fan-in, {faster(mi5[('fanin_256', 'kqring_multishot')][0], mi5[('fanin_256', 'kqueue')][0])} than kqueue on the laptop, is {faster(v('fanin_256', 'kqring_multishot'), v('fanin_256', 'kqueue'))} on the Ryzen. It isn't a change in kqring: `v0.3.0` and `7641bf4`, alternated on the Ryzen 5 runs each, differed by a median of {ab_med * 100:.1f}% per row over these groups ([raw runs](docs/bench/data-v030-vs-head/)). Where kqueue makes many system calls per unit of work and each costs a lot, as with PTI, kqring's saving shows. Where calls are cheap, its per-request cost decides, and part of that cost is what kqring does beyond Linux's io_uring: an enter costs about 250 ns more ([docs/linux-comparison.md](docs/linux-comparison.md)).

## Setup

| | |
|---|---|
| CPU | AMD Ryzen 3 PRO 4350G, 4 cores / 8 threads; no PTI (`vm.pmap.pti=0`) |
| Memory | 8 GB |
| Disk | SanDisk SD7TB6S 256 GB SATA SSD |
| OS | FreeBSD 15.1-RELEASE, `GENERIC` kernel, amd64; root on ZFS (OpenZFS 2.4.2) |
| kqring | `kqring.ko` from `7641bf4` (the stream group and the file on ZFS from `{STREAM_BUILD}`), entered through its native system calls, with `kern.kqring.zc_local_min_len` at its default of 32 KiB |
| Ring | `SINGLE_ISSUER \\| DEFER_TASKRUN`, 1,024 SQ / 8,192 CQ entries, one buffer ring of 4,096 × 4 KiB |
| Benchmarks | [`tests/kqring_bench.c`](tests/kqring_bench.c), [`tests/kqring_mt_bench.c`](tests/kqring_mt_bench.c) and [`tests/kqring_diskbench.c`](tests/kqring_diskbench.c), built with `cc -O2` |
| Limits | `ulimit -l 65536` (the FreeBSD default of 64 KiB is too small to register the send buffers) |

## Method

- Each `kqring_bench` and `kqring_mt_bench` figure is the **median of 5 runs**, and each `kqring_diskbench` figure the median of 3 runs of 10 seconds. The module was unloaded and reloaded before each `kqring_bench` and `kqring_mt_bench` run.
- A run measures the mean wall-clock time per operation over 20,000–524,288 operations. An operation is one message received or sent, one connection accepted, one read or one timer.
- `kqring_bench` is **single-threaded on loopback**, and the process isn't pinned to a CPU. The sender and receiver are the same thread, so it measures the per-operation cost of each mechanism, not throughput under load. The echo group uses two threads.
- {stable} of {nstable} rows outside the streaming group vary less than 5% between runs. The exceptions are some sends of 16–64 KiB (up to {spread_send:.0f}%), AF_UNIX ping-pong at 1 KiB (up to {spread_pp:.0f}%), and the file group's reads in `/tmp`, which is ZFS here: in some runs ZFS synced the file partway through the group, which slows its reads by an order of magnitude (see [Pipes and files](#pipes-and-files-inline-or-through-a-worker); the ZFS table there comes from separate runs that wait for the sync).
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

TCP is dominated by the loopback stack (about {v('pingpong_tcp_64', 'kqueue') / 1000:.0f} µs per message), so the mechanisms differ by a few percent.

### Fan-in: many connections each get one message, and all of them are collected

![Fan-in chart](docs/bench/fanin.svg)

Each round writes one 64-byte message to each of N AF_UNIX connections and then collects all N. The figures are per message.

| Variant | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue: one `kevent` returns N events, then N `recv` | {n('fanin_16', 'kqueue')} | {n('fanin_256', 'kqueue')} | {n('fanin_1024', 'kqueue')} |
| kqring: N multishot `RECV`s, one enter reaps N CQEs | {fan('kqring_multishot', True)} |
| kqring: N single-shot `RECV`s submitted and completed in one enter | {fan('kqring_recv_batch', True)} |

This is the shape of a busy server. kqring makes one system call for N messages, kqueue makes N + 1. {fan_note}

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

### Streaming: the receiver in its own thread

`kqring_bench stream`. The send group's variants, with a second thread draining the connection with `recv(2)` while the sender sends back to back. In the send group one thread sends and then reads, so on TCP loopback a small send mostly measures the hand-off to the netisr thread; here the two sides overlap, as in a real stream. This group is from `{STREAM_BUILD}`, the build after `7641bf4` that sends `SEND_ZC` with `MSG_WAITALL` zero-copy. The same group without that variant, run on `7641bf4` ([raw runs](docs/bench/data-stream-7641bf4/)), differs from these by a median of {s_med:.1f}% per row and at most {s_max:.0f}%. {s_out_text}

Throughput in GB/s, with the time per message against `write(2)`:

**AF_UNIX**

| Variant | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | {stream('unix', 'plain_write')} |
| kqring `SEND` | {stream('unix', 'kqring_send')} |
| kqring `SEND_ZC`, registered buffer | {stream('unix', 'kqring_send_zc_fixed')} |
| kqring `SEND_ZC`, unregistered buffer | {stream('unix', 'kqring_send_zc')} |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | {stream('unix', 'kqring_send_zc_fixed_waitall')} |

**TCP loopback**

| Variant | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | {stream('tcp', 'plain_write')} |
| kqring `SEND` | {stream('tcp', 'kqring_send')} |
| kqring `SEND_ZC`, registered buffer | {stream('tcp', 'kqring_send_zc_fixed')} |
| kqring `SEND_ZC`, unregistered buffer | {stream('tcp', 'kqring_send_zc')} |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | {stream('tcp', 'kqring_send_zc_fixed_waitall')} |

The sending thread's CPU time per message, in µs:

| Variant | AF_UNIX 64 KiB | AF_UNIX 256 KiB | TCP 64 KiB | TCP 256 KiB |
|---|---:|---:|---:|---:|
| `write(2)` | {scpu('plain_write')} |
| kqring `SEND` | {scpu('kqring_send')} |
| kqring `SEND_ZC`, registered buffer | {scpu('kqring_send_zc_fixed')} |
| kqring `SEND_ZC`, unregistered buffer | {scpu('kqring_send_zc')} |
| kqring `SEND_ZC`, registered, `MSG_WAITALL` | {scpu('kqring_send_zc_fixed_waitall')} |

- **With the receiver on its own thread, zero-copy pays off from small messages.** From a registered buffer, `SEND_ZC` takes {zc_lo:.0f}–{zc_hi:.0f}% less time than `write(2)` from 16 KiB up, and {less(v('stream_unix_4k', 'kqring_send_zc_fixed'), v('stream_unix_4k', 'plain_write'))} less on AF_UNIX and {less(v('stream_tcp_4k', 'kqring_send_zc_fixed'), v('stream_tcp_4k', 'plain_write'))} less on TCP at 4 KiB, where in the send group it cost more up to 64 KiB. The receiver's copy then sets the pace: at 256 KiB on AF_UNIX the receiving thread is busy {rx_busy:.0f}% of the time, and the sender spends {v('stream_unix_256k', 'kqring_send_zc_fixed_sender_cpu') / 1000:.1f} µs of CPU per message, against {v('stream_unix_256k', 'plain_write_sender_cpu') / 1000:.1f} µs for `write(2)`.
- **An unregistered buffer below 32 KiB on AF_UNIX is copied**, as in the send group, and costs more than `SEND`. From 64 KiB it is pinned and sent zero-copy, as fast as a registered one.
- **Without `MSG_WAITALL`, a send sends what the socket has room for.** On TCP at 256 KiB, a message took {tcp_sends:.1f} `SEND_ZC`s, the program sending the rest each time, and {tcp_copied * 100:.0f}% of messages had a remainder below a page, which kqring copies. With `MSG_WAITALL`, kqring sends the whole message from one SQE, zero-copy across as many attempts as the socket needs, with one notification; {wa_copied * 100:.0f}% of notifications reported a part copied. On TCP at 256 KiB it also needs the least of the sender, {v('stream_tcp_256k', 'kqring_send_zc_fixed_waitall_sender_cpu') / 1000:.1f} µs per message against {v('stream_tcp_256k', 'kqring_send_zc_fixed_sender_cpu') / 1000:.1f} µs, as it makes one submission per message.
- Rows vary up to {stream_spread:.0f}% between runs; `write(2)` and `SEND` within {stream_spread_plain:.0f}%.

### Accept: a TCP loopback connection set up and torn down

The client resets on close (`SO_LINGER` 0), so no `TIME_WAIT` builds up.

| Variant | ns/connection | vs kqueue |
|---|---:|---:|
| Blocking `accept(2)` (floor) | {n('accept', 'plain_accept')} | {rel('accept', 'plain_accept', 'kqueue')} |
| kqueue: `kevent` + `accept` | {n('accept', 'kqueue')} | |
| kqring multishot `ACCEPT` | {n('accept', 'kqring_multishot')} | {rel('accept', 'kqring_multishot', 'kqueue')} |
| kqring multishot `ACCEPT`, 50 µs busy-poll | {win('accept', 'kqring_multishot_busypoll', 'kqueue')} |

The enter checks before the loopback netisr thread has completed the connection, so without busy-polling nearly every iteration sleeps and pays for a wake-up. With `IORING_REGISTER_NAPI` set to 50 µs, multishot `ACCEPT` avoids the sleep, at the cost of CPU time on waits that don't end quickly.

### Timer: a 1 µs one-shot timer, armed and fired

| Variant | ns/op | vs kqueue |
|---|---:|---:|
| kqueue `EVFILT_TIMER`, `NOTE_USECONDS` 1 | {n('timer_1us', 'kqueue')} | |
| kqring `TIMEOUT`, 1 µs | {n('timer_1us', 'kqring_timeout')} | {rel('timer_1us', 'kqring_timeout', 'kqueue')} |

Both are dominated by timer-interrupt scheduling, not by the API; the kqueue row varies by {m[('timer_1us', 'kqueue')][1] * 100:.0f}% between runs.

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
| kqring `READ`, `O_NONBLOCK` pipe, data ready (inline) | {win('pipe', 'kqring_read_nonblock', 'kqueue')} |
| kqring `READ`, blocking pipe, data ready (inline) | {n('pipe', 'kqring_read')} | {rel('pipe', 'kqring_read', 'kqueue')} |

**File on UFS: 4 KiB random reads from a 64 MiB file in the page cache.** kqueue can't wait on regular files, so the comparison is `pread(2)`. The file is on a UFS file system on an md(4) disk.

| Variant | ns/op | vs `pread` |
|---|---:|---:|
| `pread(2)` | {n('ufs_file_4k', 'pread')} | |
| kqring `READ`, one per enter | {n('ufs_file_4k', 'kqring_read')} | {rel('ufs_file_4k', 'kqring_read', 'pread')} |
| kqring `READ`, one per enter, registered file | {n('ufs_file_4k', 'kqring_read_fixed')} | {rel('ufs_file_4k', 'kqring_read_fixed', 'pread')} |
| kqring `READ`, 32 per enter | {win('ufs_file_4k', 'kqring_read_batch32', 'pread')} |

A registered file saves almost nothing, so descriptor lookups aren't where the single-read gap comes from. It is the fixed cost of an enter, plus the SQE and CQE.

**File on ZFS: the same reads from a 64 MiB file in `/tmp`** (lz4, 128 KiB records). ZFS caches in the ARC rather than the page cache, so `READ` goes to a worker, or runs in the enter once the readers take most of the CPUs. ZFS serves these reads at one of two speeds, depending on whether the file fits in its dbuf cache, which holds records ready to read and is capped at 1/32 of the ARC target (`vfs.zfs.dbuf.cache_shift=5`). Here the ARC target was {z_arc:,} MiB, so the cache held {z_t5} MiB, and the file didn't fit; at 1/8 (`cache_shift=3`, {z_t3} MiB) it does. The runs alternated between the two, 5 of each, with `kqring.ko` from `{STREAM_BUILD}` ([raw runs](docs/bench/data/zfs-dbuf/)); every row varies less than {max(1, round(zspread))}% between runs.

| Variant | Fits: ns/op | vs `pread` | Doesn't fit: ns/op | vs `pread` |
|---|---:|---:|---:|---:|
{zrow('`pread(2)`', 'pread')}
{zrow('kqring `READ`, one per enter', 'kqring_read')}
{zrow('kqring `READ`, one per enter, registered file', 'kqring_read_fixed')}
{zrow('kqring `READ`, 32 per enter', 'kqring_read_batch32')}

When the file fits, one `READ` per enter pays the worker hand-off on top of `pread(2)`, and 32 per enter more than make it up. When it doesn't, each 4 KiB read evicts a record from the cache and rebuilds a whole 128 KiB one from the ARC, and that dominates every variant; 32 per enter still win, since the rebuilds run on several workers at once. A kernel profile of `pread(2)` alone in that state ([`tests/zfs_dbuf.c`](tests/zfs_dbuf.c)) spends {z_lz4:.0f}% of its samples in lz4 decompression for this all-`'f'` file and {z_cpy:.0f}% in `memcpy` for incompressible data, both at about the same time per read.

A file just written reads fast either way until its transaction group syncs (`vfs.zfs.txg.timeout`, 5 s): its dirty records are still in memory whole. `zfs_dbuf` shows the switch at the sync, from {z_before:,.0f} to {z_after:,.0f} ns a read; in its two runs, reads were still fast {z_switch[0]:.1f} and {z_switch[1]:.1f} s after the write and slow at the next sample. Earlier editions of this group read within a second of writing the file, so whether the sync landed inside the group was luck. That was the fast and slow mode of earlier runs, including the [`v0.3.0` against `7641bf4` runs](docs/bench/data-v030-vs-head/), where reads were slow from some point in the group on in {len(AB_SLOW)} of the 10 runs, with both builds. The group now waits for the sync on ZFS and prints the dbuf cache target with each run. The ARC target, and with it the dbuf cache, shrinks under memory pressure and grows back only as the ARC fills: when these runs were made, after the other benchmarks, the ARC held about 250 MB, its target was {z_arc:,} MiB, and its ceiling 6.3 GiB. For ZFS under load, the [fio tables](#fio-on-a-real-ssd) are the better guide.

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

At 32 in flight, {"kqring's reads and writes are " + vs_aio(r32) + " aio's" if vs_aio(r32) == vs_aio(w32) else "kqring's reads are " + vs_aio(r32) + " aio's and its writes " + vs_aio(w32) + " aio's"}; rounds differed by at most {dspread * 100:.1f}%. One at a time, a thread calling `pread(2)` is {q1psync * 100:.0f}% faster, as in the disk group above. With 32 in flight, 32 threads calling `pread(2)` or `pwrite(2)` are within {md32spread * 100:.0f}% of both, for reads and for writes, which suggests md's one thread is the limit there.

**A 256 MiB file on ZFS, `O_DIRECT`, 4 KiB random reads** (the worker path: FreeBSD filesystems have no asynchronous read entry point):

| In flight | `pread` threads | aio | kqring | kqring p99 µs |
|---:|---:|---:|---:|---:|
| 1 | {zfsr[1][0]} | {zfsr[1][1]} | {zfsr[1][2]} | {zfsr[1][3]} |
| 4 | {zfsr[4][0]} | {zfsr[4][1]} | {zfsr[4][2]} | {zfsr[4][3]} |
| 16 | {zfsr[16][0]} | {zfsr[16][1]} | {zfsr[16][2]} | {zfsr[16][3]} |
| 64 | {zfsr[64][0]} | {zfsr[64][1]} (at 32) | {zfsr[64][2]} | {zfsr[64][3]} |

From 4 in flight, kqring is {min(zq) * 100:.0f}–{max(zq) * 100:.0f}% ahead of as many `pread(2)` threads; one at a time it's {'level with' if abs(zq1) < 0.02 else f'{abs(zq1) * 100:.0f}% ' + ('ahead of' if zq1 > 0 else 'behind')} a thread calling `pread(2)`. aio(4) is capped at 32 in flight per process by default (`vfs.aio.max_aio_per_proc`).

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

{pp_note} Thread n runs on CPU n, so from 2 threads on, each pair shares a core: that is why 2 threads give well under twice the rate of 1, and 4 and 8 about double it each time. The benchmark's shared-file workload isn't shown: its file is in `/tmp`, which is ZFS here, with the two speeds described under [Pipes and files](#pipes-and-files-inline-or-through-a-worker).

### fio on a real SSD

[fio](fio/README.md) 3.43 with its own `io_uring` engine on kqring, against `posixaio` (`aio(4)`) and `psync`, on the SanDisk SSD. Each figure is the median of three 20-second runs after a 2-second ramp. `psync` runs one thread per I/O in flight. Before each read run, the script waits until the disk has had no writes for 5 seconds. ZFS keeps flushing random writes for a while after they return, and while it does, the SSD stalls reads for up to a second and more, whatever the engine: one in flight, the longest read took 1.4–3.1 ms on a quiet disk and 95–1,336 ms just after 20 seconds of ZFS writes, with kqring and with `psync` ([raw runs](docs/bench/data/stall/)). [`fio/bench.sh`](fio/bench.sh) reproduces them, and [`fio/summary.py`](fio/summary.py) turns its output into [`fio.csv`](docs/bench/data/fio.csv).

![CPU per engine on a SATA SSD](docs/bench/fio_cpu.svg)

**The raw disk, read-only** (the direct bio path), 4 KiB random reads:

| In flight | kqring IOPS | CPU | aio IOPS | CPU | `psync` IOPS |
|---:|---:|---:|---:|---:|---:|
| 1 | {f0('raw', 'kqring', 1)} | {f0('rawcpu', 'kqring', 1)}% | {f0('raw', 'aio', 1)} | {f0('rawcpu', 'aio', 1)}% | {f0('raw', 'psync', 1)} |
| 8 | {f0('raw', 'kqring', 8)} | {f0('rawcpu', 'kqring', 8)}% | {f0('raw', 'aio', 8)} | {f0('rawcpu', 'aio', 8)}% | {f0('raw', 'psync', 8)} |
| 32 | {f0('raw', 'kqring', 32)} | **{f0('rawcpu', 'kqring', 32)}%** | {f0('raw', 'aio', 32)} | {f0('rawcpu', 'aio', 32)}% | {f0('raw', 'psync', 32)} |

At 1 and 8 in flight, the three engines get the same IOPS from the disk, within {raw_eng * 100:.1f}% of each other. At 32, kqring and `psync` still do, and aio is {(1 - fv('raw', 'aio', 32) / fv('raw', 'kqring', 32)) * 100:.0f}% behind, at more CPU. At 32 in flight, `--fixedbufs --registerfiles` gives {f0('raw_fixed', 'kqring', 32)} IOPS and `--sqthread_poll` {f0('raw_sqpoll', 'kqring', 32)}. Sequential 128 KiB reads run at {f0('seq', 'kqring', 'MB/s')} MB/s through kqring and {f0('seq', 'aio', 'MB/s')} MB/s through aio, with kqring at {f0('seq', 'kqring', 'cpu')}% CPU against {f0('seq', 'aio', 'cpu')}%.

**A file on ZFS** (the worker path), 8 GiB, 4 KiB records, `primarycache=metadata` so that reads reach the disk. FreeBSD 15.1 has ZFS direct I/O off by default (`vfs.zfs.dio_enabled=0`), so `--direct=1` still goes through ZFS's buffers:

| Random 4 KiB | In flight | kqring IOPS | CPU | p99 µs | aio IOPS | CPU | p99 µs | `psync` IOPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Reads | 1 | {f0('zfsread', 'kqring', 1)} | {f0('zfsreadcpu', 'kqring', 1)}% | {f0('zfsreadp99', 'kqring', 1)} | {f0('zfsread', 'aio', 1)} | {f0('zfsreadcpu', 'aio', 1)}% | {f0('zfsreadp99', 'aio', 1)} | {f0('zfsread', 'psync', 1)} |
| Reads | 32 | {f0('zfsread', 'kqring', 32)} | {f0('zfsreadcpu', 'kqring', 32)}% | {f0('zfsreadp99', 'kqring', 32)} | {f0('zfsread', 'aio', 32)} | {f0('zfsreadcpu', 'aio', 32)}% | {f0('zfsreadp99', 'aio', 32)} | {f0('zfsread', 'psync', 32)} |
| Writes | 1 | {f0('zfswrite', 'kqring', 1)} | {f0('zfswritecpu', 'kqring', 1)}% | {f0('zfswritep99', 'kqring', 1)} | {f0('zfswrite', 'aio', 1)} | {f0('zfswritecpu', 'aio', 1)}% | {f0('zfswritep99', 'aio', 1)} | {f0('zfswrite', 'psync', 1)} |
| Writes | 32 | {f0('zfswrite', 'kqring', 32)} | {f0('zfswritecpu', 'kqring', 32)}% | {f0('zfswritep99', 'kqring', 32)} | {f0('zfswrite', 'aio', 32)} | {f0('zfswritecpu', 'aio', 32)}% | {f0('zfswritep99', 'aio', 32)} | {f0('zfswrite', 'psync', 32)} |

Reads reach the same IOPS through every engine, within {zfs_eng * 100:.0f}% of each other, and kqring uses less CPU than aio at 32 in flight. One at a time, kqring's issuer spins briefly for the worker rather than sleeping, which is the CPU in the rows for 1. Writes vary between runs, up to {wspread:.0f}% of the median, against at most {rspread:.0f}% for reads, so their differences between engines don't mean much. ZFS gathers writes into transaction groups, and the flushes of those to the disk most likely set the pace.

### fio with several rings

One ring per thread, 4 KiB random reads, on targets the SSD doesn't limit: [`fio/scale.sh`](fio/scale.sh). `psync` here is one thread per job, so it has 1/16 of the others' I/Os in flight. Medians of three 10-second runs, in thousands of IOPS.

<p align="center">
  <img src="docs/bench/fio_scale.svg" alt="fio on md(4) disks by jobs" width="49%">
  <img src="docs/bench/fio_cached.svg" alt="fio on a cached ZFS file by jobs" width="49%">
</p>

| Jobs | ZFS, in the ARC: kqring | aio | `psync` | `md(4)`, a disk per job: kqring | aio |
|---:|---:|---:|---:|---:|---:|
| 1 | {f0('cached', 'kqring', 1)} | {f0('cached', 'aio', 1)} | {f0('cached', 'psync', 1)} | {f0('md', 'kqring', 1)} | {f0('md', 'aio', 1)} |
| 2 | {f0('cached', 'kqring', 2)} | {f0('cached', 'aio', 2)} | {f0('cached', 'psync', 2)} | {f0('md', 'kqring', 2)} | {f0('md', 'aio', 2)} |
| 4 | {f0('cached', 'kqring', 4)} | {f0('cached', 'aio', 4)} | {f0('cached', 'psync', 4)} | {f0('md', 'kqring', 4)} | {f0('md', 'aio', 4)} |
| 8 | {f0('cached', 'kqring', 8)} | {f0('cached', 'aio', 8)} | {f0('cached', 'psync', 8)} | | |

Below 8 jobs kqring's workers read a ring's requests in parallel; at 8 the reads run in the submitting threads, as the CPUs are taken ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)). From 4 jobs, `psync` is ahead, since a thread reading the ARC itself needs no hand-off and no ring; with several reads per enter, kqring passes it up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).

## Known gaps

- **A pipe read that has to wait goes through a worker thread**, as does a write to a full pipe. Waiting for data with `POLL_ADD` first avoids the worker.
- **Writes to one file run one at a time.** They keep their order by a bucket of the file, as io-wq's per-inode hashing does, and the issuer doesn't wait for them; see [docs/write-ordering.md](docs/write-ordering.md).
- **Page-cache misses, and filesystems without the page-cache read path, go through a worker.** This covers file reads that need the disk, ZFS, `O_DIRECT` on files, and reads using the file position (`off` = −1). Only disk devices themselves get the direct bio path: FreeBSD filesystems have no asynchronous read or write entry point. It also covers reads into a buffer that isn't mapped yet.
- **From 4 jobs on cached ZFS data, `psync` beats one read per enter.** A thread reading the ARC itself needs no hand-off. kqring runs the reads in the submitting thread once the readers take most of the CPUs, and with 4 to 16 reads per enter it's ahead of `psync` up to 4 jobs ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)).
- **One operation per enter costs more than an empty `kevent`:** {rel('floor', 'kqring_nop', 'kqueue_empty_kevent')} on this machine. That's the `NOP` floor: the enter, the SQE, the request and the CQE. Where system calls are cheap, as here, that cost outweighs the calls kqring saves on single requests and on fan-in ([Why the results depend on the CPU](#why-the-results-depend-on-the-cpu)). Batching removes it.
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
"""
open(R + 'BENCHMARK.MD', 'wb').write(doc.encode())
print('ok', len(doc))
