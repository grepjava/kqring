# PostgreSQL on kqring

PostgreSQL 18's `io_method=io_uring` runs its asynchronous reads and writes through liburing. This directory builds PostgreSQL 18.6 against [liburing on kqring](../liburing/) and checks it. PostgreSQL needs no patch: its `configure` takes liburing from `pkg-config`, and `build.sh` writes a `liburing.pc` that points at the port.

## What it needs from kqring

PostgreSQL uses rings across processes:

- The postmaster creates one ring per backend slot, in shared memory with `IORING_SETUP_NO_MMAP` (`io_uring_queue_init_mem()`), and never uses them.
- Each forked backend submits on the ring of its slot. The next backend in that slot reuses the ring.
- A backend that needs an I/O another backend issued may wait on that backend's ring and process its completions.

kqring has supported this since rings could [move between processes](../docs/cross-process-rings.md), and since November 2026 [several processes submit to a ring at once](../docs/concurrent-submitters.md), as on Linux. Before the first, every backend's first submission failed with `EEXIST` ("io_uring submit failed: File exists"), the backend PANICked, and the postmaster restarted in a loop.

## Building

On FreeBSD with `kqring.ko` built, after `make` in [`../liburing`](../liburing/):

```sh
pkg install gmake bison flex perl5 pkgconf
cd postgresql
sh build.sh          # fetches postgresql-18.6.tar.bz2, checks its SHA-256, builds into work/inst
```

`build.sh` configures with `--with-liburing --enable-cassert --enable-debug --without-icu`, and fails if `configure` didn't enable liburing.

## Running

```sh
sudo kldload ../kmod/kqring.ko
sh check.sh          # a scratch cluster with io_method=io_uring, then make installcheck-parallel
sh bench.sh          # pgbench: io_uring against PostgreSQL's worker and sync methods
```

Any cluster runs on kqring with `io_method = io_uring` in `postgresql.conf`. `check.sh` and `bench.sh` listen only on a Unix socket in `/tmp`, on port 5499.

## Results

On a Ryzen 3 PRO 4350G (8 threads, 7.9 GB) with FreeBSD 15.1, root on ZFS, and `kqring.ko` from this tree, November 2026:

- **Regression tests:** `check.sh` (`make installcheck-parallel` with `io_method=io_uring`) passes all 231 tests, with no PANIC in the server log ([output](results/check.tap)). `ktrace(1)` of the backends during a cold sequential scan shows their reads going through kqring's system calls.
- **pgbench:** [`bench.sh`](bench.sh) at scale 40, with the default 128 MB of shared buffers, 8 clients for 30 s, 3 rounds, alternating the methods ([raw output](results/bench.out)):

| Method | select-only, tps | TPC-B-like, tps |
|---|---:|---:|
| `io_uring` (kqring) | 15,158–15,391 | 1,743–2,274 |
| `worker` | 15,278–15,339 | 2,014–2,279 |
| `sync` | 15,181–15,523 | 2,020–2,250 |

The three methods are level, within each one's spread between rounds, so these runs show that `io_uring` on kqring holds up under concurrent backends, not that it is faster. `io_uring`'s 1,743 is the first TPC-B-like run after loading the data; the other two rounds gave 2,267 and 2,274. The script's cold sequential scans aren't shown. They took from 0.15 to 10.8 s within the same method, depending on how much of the table ZFS still held in memory, so they don't compare the methods.

Run again with the kqring that lets several processes submit to a ring at once (`docs/concurrent-submitters.md`), on the same machine and cluster: `check.sh` passed all 231 tests again, with no PANIC, and pgbench stayed level ([raw output](results/bench-phase8.out)): select-only 14,681–14,995 tps with `io_uring`, 14,852–14,945 with `worker` and 14,794–14,921 with `sync`; TPC-B-like 1,934–2,330, 2,053–2,311 and 2,013–2,278.

What isn't tested: `io_method=io_uring` with `debug_io_direct`, and PostgreSQL's own AIO tests in `src/test/modules/test_aio`, which need TAP support (`--enable-tap-tests` and Perl's `IPC::Run`).
