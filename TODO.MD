# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them; [ROADMAP.MD](ROADMAP.MD) puts it in order.

## Performance

- **Cached ZFS reads, `psync` still ahead at 8 jobs.** With one read per enter kqring is 9% behind at 4 jobs and 11% at 8; with 4 to 16 reads per enter it's ahead up to 4 jobs and 2% behind at 8. What's left is the per-call path, spread thinly over about 25 functions, and fio's io_uring engine ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)). A ZFS read that can't block would remove the guess about cached reads but not raise throughput ([docs/async-zfs-reads.md](docs/async-zfs-reads.md#measured-before-building)).
- **1 to 4 jobs lose up to 0.6% to the inline-read change.** No read runs in the enter there. What's left is spread over several small pieces (the count of threads in enters, the `inline_ok` check, the spinner's check, the second average), none of which alone made a measurable difference ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)).
- **Fan-in with a thousand connections trails kqueue.** On the Ryzen a multishot receive is about 10% slower than kqueue at 1,024 AF_UNIX connections and 18% at 2,048, and level or ahead up to 512: an armed receive keeps more memory per connection than a knote (the request's four cache lines, the socket's waiter list, and on AF_UNIX the end-of-input knote), and there the working set outgrows the L3. Closing it would mean fewer lines per armed receive, for example the upcall queueing without touching the request ([docs/fan-in.md](docs/fan-in.md#where-the-requests-lie)).
- **A receive that waits costs about 250 ns more than one that finds its data**, on a `DEFER_TASKRUN` ring, on both test machines (io_uring: 651 ns on the i5). On the Ryzen, where a system call costs 53 ns, that is 60% of kqueue's ping-pong loop; the time is spread over arming, waking and the second enter ([docs/fan-in.md](docs/fan-in.md#a-receive-that-waits)).
- **A default ring wakes its kernel thread per message** when nobody waits in an enter: 260 ns per message in ping-pong on the Ryzen, 563 on the i5. With `COOP_TASKRUN` the writer's return to user space runs the completion instead, about 135 ns more than a `DEFER_TASKRUN` ring, and multishot fan-in written by the ring's own thread is then 50% slower ([docs/fan-in.md](docs/fan-in.md#the-writers-return-to-user-space)).
- **What is left of an enter's cost** is spread over the system call's entry and exit, submission, the ring's descriptor (`fget`), `sq_sx` and about 20 small functions. One `NOP` per enter now costs no more above FreeBSD's floor than io_uring above Linux's on the same machine ([docs/per-request-cost.md](docs/per-request-cost.md#on-the-machine-that-runs-both)); single requests still trail kqueue on the Ryzen, where fan-in is level with it.
- **A cached read in the enter costs more than `pread(2)`.** On tmpfs, one fio job reads 0.83 of `psync` on the i5 (io_uring: 1.05 of Linux's) and 0.74 on the Ryzen: fio's engine enters once per read, and kqring's own functions take 190 ns of each ([docs/cached-reads.md](docs/cached-reads.md#where-the-time-of-one-read-goes)). With spare CPUs, workers read one job's 32 in flight 13% faster, on three CPUs.

## Compatibility

- **A new mapping over a registered buffer.** FreeBSD keeps a held page in its memory object when its range is unmapped, and an anonymous mapping that extends the object over the same offsets maps the page again, zeroed, so the buffer and the new memory can be one. Linux keeps them apart. It would take the VM letting go of held pages at unmap ([docs/held-buffers.md](docs/held-buffers.md#what-the-implementation-changed)).
- **A fixed read in flight during `fork(2)`** lands in the child's copy of the buffer, as the parent's buffer is held again at the fork; on Linux it lands in the parent's.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **fio's cached ZFS reads at 250,000–330,000 IOPS.** Seen once on the Ryzen on 2026-11-09, with kqring at 1 and 2 jobs, where 890,000 is usual, and not reproduced since: not with or without `zpool sync` after writing the file, nor with the ARC target squeezed to 755 MB. It is not the dbuf cache that gave `kqring_bench`'s file group two speeds ([BENCHMARK.MD](BENCHMARK.MD#pipes-and-files-inline-or-through-a-worker)): fio's dataset has 4 KiB records, and its `--direct=1` reads are ZFS's uncached reads, which don't use that cache. `fio/scale.sh` writes the ARC hits, misses and size for each cached run to `arc.txt`.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
