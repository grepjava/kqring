# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them; [ROADMAP.MD](ROADMAP.MD) puts it in order.

## Performance

- **Cached ZFS reads, `psync` still ahead from 4 jobs.** On an 8-thread machine kqring is 11% behind at 4 jobs, where its reads still go through workers, and 10% at 8, where they run in the submitting thread ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)). Next: ZFS reads that complete from ZFS's own I/O callback, without a worker or a guess ([docs/async-zfs-reads.md](docs/async-zfs-reads.md)).
- **2 and 4 jobs lose 1–1.5% to the inline-read change.** No read runs in the enter there. At 1 job the count of threads in enters was the cost, and `counter(9)` cut it to 0.6%; at 2 jobs the loss stays without the count, and neither the clock read per worker request nor the `inline_ok` check alone explains it. Next: the spin check and the second average under the pool mutex, or code layout.
- **ZFS at 32 in flight.** kqring is 4–5% behind aio in IOPS with a longer p99, at less than half the CPU.
- **A zero-copy benchmark with the receiver in its own thread.** Loopback delivers through the netisr thread, so a single-threaded send-then-receive mostly measures that hand-off for small sends.

## Correctness

- **`test_kqueue_bridge_level` fails now and then.** After the pipe it watches has been read empty, an enter sometimes reports the level-triggered read event once more within 30 ms, where the test expects none. Seen on the Ryzen test machine on 2026-11-09, once in eight runs, with the module from before and after the inline-read change alike. Either a stale level report in the kqueue bridge or a race in the test; not yet known which.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **Explain ZFS's two speeds.** The same cached file reads at about 1.5 µs or 6–14 µs from run to run, with plain `pread(2)` too. Seen again on 2026-11-09 on the Ryzen: 250,000–330,000 IOPS with kqring at 1 and 2 jobs, slower at 2 than at 1, with the module from before the inline-read change and after, where 890,000 is usual. That run skipped `zpool sync` after writing the file, but three trials with and without the sync were all fast, and so were three after squeezing the ARC target to 755 MB, with no ARC misses, so neither reproduces it. With `vfs.zfs.dio_enabled=0`, the default on 15.1, fio's `--direct=1` reads are ZFS's uncached reads, which build and drop a dbuf for every read; buffered reads, which keep a dbuf cache of 1/32 of the ARC target, were about as fast. `fio/scale.sh` now writes the ARC hits, misses and size for each cached run to `arc.txt`, so the next slow run shows whether it went to the disk.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
