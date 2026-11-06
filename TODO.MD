# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them.

## Correctness

- **Post a poll on another ring before the enter that fired it returns, every time.** The gap is an event the poller has taken from the kqueue and not yet recorded.

## Performance

- **Per-file ordering in the worker pool**, as io-wq hashes buffered writes per inode, so that writes to different files run in parallel and the issuer doesn't wait. Buffered writes are about 9% behind aio on a ZFS file.
- **The worker hand-off ceiling.** Cached reads through workers stop near 530,000 IOPS on a 4-core machine, and `psync` passes them from 4 jobs.
- **ZFS at 32 in flight.** kqring is 4–5% behind aio in IOPS with a longer p99, at less than half the CPU.
- **A zero-copy benchmark with the receiver in its own thread.** Loopback delivers through the netisr thread, so a single-threaded send-then-receive mostly measures that hand-off for small sends.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **Explain ZFS's two speeds.** The same cached file reads at about 1.5 µs or 6–14 µs from run to run, with plain `pread(2)` too.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
