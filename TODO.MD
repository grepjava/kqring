# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them; [ROADMAP.MD](ROADMAP.MD) puts it in order.

## Performance

- **Cached ZFS reads, `psync` still ahead at 8 jobs.** With one read per enter kqring is 9% behind at 4 jobs and 11% at 8; with 4 to 16 reads per enter it's ahead up to 4 jobs and 2% behind at 8. What's left is the per-call path, spread thinly over about 25 functions, and fio's io_uring engine ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)). A ZFS read that can't block would remove the guess about cached reads but not raise throughput ([docs/async-zfs-reads.md](docs/async-zfs-reads.md#measured-before-building)).
- **1 to 4 jobs lose up to 0.6% to the inline-read change.** No read runs in the enter there. What's left is spread over several small pieces (the count of threads in enters, the `inline_ok` check, the spinner's check, the second average), none of which alone made a measurable difference ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)).
- **ZFS at 32 in flight.** kqring is 4–5% behind aio in IOPS with a longer p99, at less than half the CPU.
- **A zero-copy benchmark with the receiver in its own thread.** Loopback delivers through the netisr thread, so a single-threaded send-then-receive mostly measures that hand-off for small sends.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **Explain ZFS's two speeds.** The same cached file reads at about 1.5 µs or 6–14 µs from run to run, with plain `pread(2)` too. Seen again on 2026-11-09 on the Ryzen: 250,000–330,000 IOPS with kqring at 1 and 2 jobs, slower at 2 than at 1, with the module from before the inline-read change and after, where 890,000 is usual. That run skipped `zpool sync` after writing the file, but three trials with and without the sync were all fast, and so were three after squeezing the ARC target to 755 MB, with no ARC misses, so neither reproduces it. With `vfs.zfs.dio_enabled=0`, the default on 15.1, fio's `--direct=1` reads are ZFS's uncached reads, which build and drop a dbuf for every read; buffered reads, which keep a dbuf cache of 1/32 of the ARC target, were about as fast. `fio/scale.sh` now writes the ARC hits, misses and size for each cached run to `arc.txt`, so the next slow run shows whether it went to the disk.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
