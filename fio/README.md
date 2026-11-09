# fio for kqring

This directory builds [fio](https://github.com/axboe/fio) 3.43, the standard storage benchmark, with its own `io_uring` engine running on `kqring.ko`. Upstream fio builds that engine only on Linux.

The port is a small overlay, not a fork:

- [`Makefile`](Makefile) fetches the pinned fio release, checks its SHA-256 and applies the overlay.
- [`freebsd/fio.diff`](freebsd/fio.diff) adds the engine to the FreeBSD build and includes the shim below: 3 changed lines.
- [`freebsd/kqring-syscall.h`](freebsd/kqring-syscall.h) routes the engine's three system calls to `kqring_setup()`, `kqring_enter()` and `kqring_register()`.
- [`freebsd/include/linux/`](freebsd/include/linux/) holds the few Linux header definitions the engine needs to compile.
- [`freebsd/bsg.c`](freebsd/bsg.c) replaces fio's SCSI passthrough code, which needs a Linux driver.

The `io_uring_cmd` engine compiles but can't work: kqring has no `URING_CMD` for NVMe or SCSI.

## Building

Needs `gmake` (`pkg install gmake`), which fio's own build requires. `kqring.ko` must be loaded to run the engine, but not to build it.

```sh
cd fio
make            # work/fio-fio-3.43/fio
make clean
```

## Running

```sh
work/fio-fio-3.43/fio --name=t --ioengine=io_uring --iodepth=32 --direct=1 \
    --rw=randread --bs=4k --size=1g --filename=/path/to/file
```

Options that use registered resources need `RLIMIT_MEMLOCK` raised, for example `--fixedbufs` after `ulimit -l 65536` as root.

## The benchmarks

[`bench.sh`](bench.sh) runs the storage tables in [BENCHMARK.MD](../BENCHMARK.MD), comparing the `io_uring` engine with `posixaio` and `psync`:

```sh
sudo sh bench.sh $PWD/work/fio-fio-3.43/fio /dev/ada0p5 zroot/fiobench $PWD/out 3 20
```

The arguments are fio, a device to read, a ZFS dataset to create, an output directory, the repeats and the seconds per run. The device is opened read-only. All writes go to a file in the dataset, which the script creates and destroys, with data caching off so that reads reach the disk. It stops if the dataset isn't mounted or doesn't hold the test file.

[`scale.sh`](scale.sh) runs one ring per thread on targets the disk doesn't limit, a file held in ZFS's cache and `md(4)` malloc disks, which it creates and removes:

```sh
sudo sh scale.sh $PWD/work/fio-fio-3.43/fio zroot/fioscale $PWD/out 3 10
```

It writes each run's JSON to the output directory, and the ARC's hits, misses and size around each cached run to `arc.txt`: cached reads that come out slow with misses there were partly read from the disk.

On FreeBSD 15.1 `vfs.zfs.dio_enabled` is 0 by default, so ZFS reads an `O_DIRECT` file through the ARC as uncached I/O: a hit is copied from the ARC, and the dbuf it made is dropped after each read rather than kept in the dbuf cache. On the Ryzen this was about as fast as buffered reads.
