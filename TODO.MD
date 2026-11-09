# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them; [ROADMAP.MD](ROADMAP.MD) puts it in order.

## Performance

- **Cached ZFS reads, `psync` still ahead from 4 jobs.** On an 8-thread machine kqring is 11% behind at 4 jobs, where its reads still go through workers, and 10% at 8, where they run in the submitting thread ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)). Next: ZFS reads that complete from ZFS's own I/O callback, without a worker or a guess ([docs/async-zfs-reads.md](docs/async-zfs-reads.md)).
- **1 to 4 jobs lost 1.3–3% to the inline-read change.** No read runs in the enter there, so it's the bookkeeping: a clock read per request on the workers or the count per enter. Not yet traced.
- **ZFS at 32 in flight.** kqring is 4–5% behind aio in IOPS with a longer p99, at less than half the CPU.
- **A zero-copy benchmark with the receiver in its own thread.** Loopback delivers through the netisr thread, so a single-threaded send-then-receive mostly measures that hand-off for small sends.

## Correctness

- **`test_kqueue_bridge_level` fails now and then.** After the pipe it watches has been read empty, an enter sometimes reports the level-triggered read event once more within 30 ms, where the test expects none. Seen on the Ryzen test machine on 2026-11-09, once in eight runs, with the module from before and after the inline-read change alike. Either a stale level report in the kqueue bridge or a race in the test; not yet known which.
- **liburing's `evloop` fails on the Ryzen.** It arms a multishot poll on the ring's own registered eventfd and expects one CQE from it after a `NOP`; kqring sometimes posts another, about 60% of runs, as often with the module from before the inline-read change (123 and 125 failures in 200 each). On Linux the CQE posted from the eventfd's wakeup doesn't signal the eventfd again.
- **The fuzzer seldom runs reads inline.** Its mix of requests rarely makes a pool's reads look cached, so it ran 18 inline in 10 minutes with 8 threads. `test_wq_inline` covers the path; the fuzzer should too.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **Explain ZFS's two speeds.** The same cached file reads at about 1.5 µs or 6–14 µs from run to run, with plain `pread(2)` too.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
