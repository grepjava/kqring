# kqring against Linux io_uring, on one machine

Phase 6 of the [roadmap](../ROADMAP.MD#phase-6-performance-evidence) asks for Linux io_uring workloads on the same machine as kqring's, because wins against kqueue and aio don't establish parity with Linux. This is the first such comparison: one laptop that boots both systems, the same fio build and the same tests on each.

## Setup

| | FreeBSD | Linux |
|---|---|---|
| Machine | Intel Core i5-8250U (4 cores, 8 threads), 1 TB WD Blue SATA SSD (`WDS100T2B0A`) | the same |
| Kernel | 15.1-RELEASE `GENERIC`, kqring `7641bf4` | Ubuntu 7.3.0-9-generic |
| Memory seen | 4.1 GB | 3.4 GB (the rest reserved for crash dumps) |
| fio | 3.43, kqring's port ([fio/](../fio/README.md)) | 3.43 from the same tarball, upstream |
| CPU mitigations | PTI; IBRS off; MDS buffer clearing off | PTI, IBRS, MDS buffer clearing, and others: [system.txt](bench/linux/linux/system.txt) |

[`fio/compare.sh`](../fio/compare.sh) ran everything, 3 rounds of 10 seconds after a 1-second ramp, 4 KiB random reads. [`tests/kqring_xbench.c`](../tests/kqring_xbench.c) builds on both systems and times the ring's own costs, each against the plain system call it replaces. The raw results, fio's JSON and each system's details, are in [`docs/bench/linux/`](bench/linux/). Measured on 2026-11-09.

**The two systems don't run the same mitigations.** Linux enables more of them on this CPU, and they make every system call dearer: `getppid(2)` takes 577 ns on Linux and 192 ns on FreeBSD. So absolute times favour FreeBSD for reasons that aren't kqring's. Below, each ring is read against its own system's plain system call, and only the SSD figures compare the two directly.

## The ring's own costs

`kqring_xbench`, medians of 5 one-second runs, nanoseconds per operation:

| | FreeBSD | Linux |
|---|---:|---:|
| `getppid(2)`, the system call floor | 192 | 577 |
| 1 `NOP` per enter, waited for | 630 | 755 |
| — above the floor | **+438** | **+178** |
| 32 `NOP`s per enter, per `NOP` | **75** | 94 |
| Pipe: `write(2)` then `read(2)` | 617 | 1,499 |
| Pipe: `write(2)` then `READ` | 1,124 | 1,788 |
| — `READ` above `read(2)` | +506 | +289 |
| AF_UNIX: `write(2)` then `recv(2)` | 727 | 2,184 |
| AF_UNIX: `write(2)` then `RECV` | 1,145 | 2,432 |
| — `RECV` above `recv(2)` | +419 | +248 |

**An enter costs kqring about 250 ns more than io_uring**, above each system's own floor. That's the fixed cost per call the [cached-read profile](worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching) found spread over the enter, the SQE, the request and the CQE. **Per operation in a batch, kqring is cheaper**: 75 ns per `NOP` at 32 per enter, against 94.

## fio

Medians of 3 rounds, thousands of IOPS, CPU as fio reports it (user and system time of its threads), and in brackets each ring against its own system's `psync` at the same jobs. Rounds varied by 7% or less, except Linux on the SSD: 34% at 1 in flight, and 10% with `posixaio`.

**The SSD, raw, read-only.** The one place the two systems drive the same hardware the same way, so the figures compare directly:

| In flight | FreeBSD kqring | CPU | Linux io_uring | CPU | FreeBSD `psync` | Linux `psync` |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | **7.7** | 13% | 5.3 | 22% | 7.8 | 6.7 |
| 32 | **97.1** | **43%** | 88.6 | 57% | | |

kqring reaches more of the drive with less CPU. One at a time, Linux is slower with plain `pread(2)` too, so part of that row is how each system drives the disk, not the ring; why isn't known.

**A 256 MiB file on tmpfs**, cached file reads on the same kind of filesystem:

| Jobs | In flight | FreeBSD kqring | vs `psync` | Linux io_uring | vs `psync` | FreeBSD `psync` | Linux `psync` |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 390 | 0.58 | 150 | 0.27 | 675 | 555 |
| 1 | 32 | 530 | 0.78 | 597 | 1.08 | | |
| 4 | 1 | 721 | 0.31 | 395 | 0.20 | 2,298 | 1,961 |
| 4 | 32 | 782 | **0.34** | 1,138 | **0.58** | | |

With 32 in flight, Linux reads a cached page in the submitting thread and only sends a read that would block to its workers. kqring sends tmpfs reads to workers until the readers take most of the CPUs ([the policy](worker-wakeups.md#reads-in-the-submitting-thread)), and with 4 jobs on 8 threads they don't, so it reaches a third of its own `psync` where Linux reaches more than half of its. One read at a time, kqring is ahead: its issuer spins briefly for the worker rather than sleep.

**A 256 MiB RAM disk**, md(4) against Linux's brd. These mostly compare the drivers: md(4) serves every request from one thread, brd in the caller's, so FreeBSD's `psync` is 2.7–3.6 times slower than Linux's.

| Jobs | In flight | FreeBSD kqring | vs `psync` | Linux io_uring | vs `psync` |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 160 | 0.90 | 393 | 0.83 |
| 1 | 32 | **548** | **3.09** | 394 | 0.83 |
| 4 | 32 | 484 | 1.03 | 1,455 | 0.86 |

Within each system, kqring's direct bio path gets three times `psync`'s IOPS from one thread with 32 in flight, because md works on them in parallel with the submitter; Linux's brd does each read inline, so more in flight changes nothing.

**`posixaio` isn't comparable.** On FreeBSD it is `aio(4)` in the kernel; on Linux it's glibc's emulation with threads, which handles one request at a time per file: 8,000 IOPS on the SSD at 32 in flight, against 92,400 for `aio(4)`. The raw results include it.

## What it shows

- kqring matches or beats io_uring where the device sets the pace, and does it with less CPU.
- Its fixed cost per enter is about 250 ns above io_uring's. Batching hides it, and per batched operation kqring is the cheaper of the two.
- Cached file reads from several threads are where io_uring is ahead, because Linux can try a read without blocking and kqring can't on FreeBSD's filesystems. On tmpfs and UFS that would need a non-blocking `VOP_READ`, on ZFS the change described in [async-zfs-reads.md](async-zfs-reads.md).

## Not covered

- The same filesystem on both: Linux has no UFS, and ZFS on Linux would be a different code base. tmpfs is the closest pair.
- Networking beyond the AF_UNIX and pipe rows above, and the thread-per-core and echo-server workloads of [BENCHMARK.MD](../BENCHMARK.MD).
- Frequency scaling: both let the CPU choose its clock, as they do by default: Linux with `intel_pstate`'s `powersave` governor, FreeBSD with `hwpstate_intel` at an energy-performance preference of 50 and no `powerd`. Neither was pinned.

## Reproducing

On each system, as root, with the same fio 3.43 tarball built for it, the same spare partition, and on FreeBSD with `kqring.ko` loaded:

```sh
cc -O2 -pthread -o kqring_xbench tests/kqring_xbench.c   # Linux; on FreeBSD: make -C tests kqring_xbench
sh fio/compare.sh /path/to/fio ./kqring_xbench /dev/<partition> out 3 10
```
