# RocksDB on kqring

RocksDB reads through io_uring where liburing is found: `MultiGet` submits the block reads of a batch together (`MultiRead`), and its asynchronous reads and prefetches (`ReadAsync`, `Poll`, `AbortIO`) go through a ring per thread, set up with `IORING_SETUP_SINGLE_ISSUER` and `IORING_SETUP_DEFER_TASKRUN`. This directory builds RocksDB 11.13.2 against [liburing on kqring](../liburing/), runs its tests that use io_uring, and benchmarks `MultiGet` with io_uring on and off.

## What it needs

RocksDB's CMake uses liburing on any system where `find_package(uring)` finds it, but its io_uring code had only been compiled on Linux. [`freebsd.diff`](freebsd.diff) changes two lines:

- Two messages cast `pthread_self()` to `unsigned long` with `static_cast`, which works where `pthread_t` is an integer, as on Linux. On FreeBSD it is a pointer.
- `env_test` includes `env/io_posix.h` only on Linux, but runs its io_uring tests wherever io_uring is.

Neither is about kqring, and both would be needed by any io_uring on a system other than Linux. RocksDB also needed a fix in the liburing port: its `<linux/fs.h>` included `<sys/disk.h>`, which brings `<sys/param.h>`'s `CACHE_LINE_SIZE` into every program that includes `<liburing.h>`, and RocksDB defines its own.

The port's `liburing.a` isn't position-independent, so `build.sh` builds RocksDB's static library only.

## Building

On FreeBSD, after `make` in [`../liburing`](../liburing/):

```sh
pkg install cmake gflags snappy liblz4 zstd
cd rocksdb
sh build.sh          # fetches rocksdb 11.13.2, checks its SHA-256, applies freebsd.diff
```

`build.sh` makes `db_bench` in a Release build (`work/build`) and the tests in a Debug build (`work/build-debug`): RocksDB builds its tests only in Debug, which also checks its assertions. It fails if CMake didn't take the port's liburing.

## Running

```sh
sudo kldload ../kmod/kqring.ko
sh check.sh          # the tests that use io_uring
sudo sh bench.sh     # db_bench MultiGet, io_uring on and off
```

`check.sh` runs `env_test`, `prefetch_test`, `block_based_table_reader_test`, `db_tailing_iter_test` and `db_basic_test`, the RocksDB tests that turn io_uring on (`io_dispatcher_test` is in RocksDB's Makefile but not its CMake build). It first runs one test that reads through real io_uring under `ktrace(1)`, and fails unless kqring's system calls show up on its rings.

`bench.sh` makes a ZFS dataset for the run, as [`../fio/bench.sh`](../fio/bench.sh) does, and destroys it afterwards: 4 KiB records, RocksDB's block size, and only metadata cached, so that every block read goes to the disk. It fills 4 million 1000-byte values, compacts them, and then runs `multireadrandom` (`MultiGet` of 32 keys) for 60 s at 1 and 4 threads, with `--io_uring_enabled` true and false, 3 rounds, alternating. RocksDB checks every block it reads against its checksum, and the script fails on any corruption or I/O error.

## Results

On a Ryzen 3 PRO 4350G (8 threads, 7.9 GB) with FreeBSD 15.1, a SanDisk SATA SSD under ZFS, and `kqring.ko` from this tree, November 2026:

- **Tests:** all pass ([summary](results/check.txt)): `env_test` 116, `prefetch_test` 122, `block_based_table_reader_test` 6905, `db_tailing_iter_test` 24 and `db_basic_test` 300.
- **`MultiGet`** ([raw output](results/bench.out)), keys read per second, the range over 3 rounds:

| Threads | io_uring (kqring) | `pread(2)` | |
|---:|---:|---:|---:|
| 1 | 8,229–8,286 | 6,424–6,443 | +28% |
| 4 | 24,017–24,130 | 20,425–20,535 | +17% |

No read failed its checksum. The gain is what `MultiRead` can give here: it submits together only the blocks a batch needs from the same file, and 32 random keys over a 2.5 GB database fall mostly in different files, so most of a batch's reads still wait for each other. RocksDB reads across files in parallel with its coroutine-based `MultiGet` (`USE_COROUTINES`, which needs folly), not built here.

With ZFS's default 128 KiB records, each 4 KiB block read reads a whole record. A first run that way, on a 7.4 GB database in the home dataset ([output](results/bench-128k.out), stopped after its first round), read about 455 MB/s from the SSD by `iostat(8)`, its bandwidth, and the two methods were level at about 1,950 keys/s.

## What isn't tested

RocksDB's coroutine `MultiGet` and its `db_stress` crash tests: in `db_stress`'s default workload the reads come from memory and the block cache, and a traced run made no io_uring reads.
