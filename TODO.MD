# kqring: what's open

Known limits are in [BENCHMARK.MD](BENCHMARK.MD#known-gaps) and [Differences from Linux](README.md#differences-from-linux). This is the work that could close some of them; [ROADMAP.MD](ROADMAP.MD) puts it in order.

## Performance

- **Cached ZFS reads, `psync` still ahead at 8 jobs.** With one read per enter kqring is 9% behind at 4 jobs and 11% at 8; with 4 to 16 reads per enter it's ahead up to 4 jobs and 2% behind at 8. What's left is the per-call path, spread thinly over about 25 functions, and fio's io_uring engine ([docs/worker-wakeups.md](docs/worker-wakeups.md#where-the-rest-of-the-gap-is-and-batching)). A ZFS read that can't block would remove the guess about cached reads but not raise throughput ([docs/async-zfs-reads.md](docs/async-zfs-reads.md#measured-before-building)).
- **1 to 4 jobs lose up to 0.6% to the inline-read change.** No read runs in the enter there. What's left is spread over several small pieces (the count of threads in enters, the `inline_ok` check, the spinner's check, the second average), none of which alone made a measurable difference ([docs/worker-wakeups.md](docs/worker-wakeups.md#reads-in-the-submitting-thread)).
- **Where system calls are cheap, single requests and fan-in trail kqueue.** On the Ryzen, without PTI, an empty `kevent(2)` costs 104 ns and one `NOP` per enter 187 ns; fan-in is 26–66% slower than kqueue and AF_UNIX ping-pong 2–16%, where on the i5-8250U with PTI both were 16–21% faster ([BENCHMARK.MD](BENCHMARK.MD#why-the-results-depend-on-the-cpu)). Phase 10 cut the enter's own cost ([docs/per-request-cost.md](docs/per-request-cost.md)); `kqring_bench`, on `DEFER_TASKRUN` rings, gained 8% on one `NOP` per enter.
- **What is left of an enter's cost** is spread over the system call's entry and exit, submission, the ring's descriptor (`fget`), `sq_sx` and about 20 small functions. One `NOP` per enter now costs no more above FreeBSD's floor than io_uring above Linux's on the same machine ([docs/per-request-cost.md](docs/per-request-cost.md#on-the-machine-that-runs-both)); fan-in and single requests still trail kqueue on the Ryzen.
- **Cached file reads from several threads trail io_uring.** On tmpfs, 4 jobs at 32 in flight reach 34% of FreeBSD's own `psync`, where io_uring reaches 58% of Linux's: Linux reads a cached page in the submitting thread and only hands a read that would block to a worker. FreeBSD filesystems have no read that fails rather than block.

## Compatibility

- **A new mapping over a registered buffer.** FreeBSD keeps a held page in its memory object when its range is unmapped, and an anonymous mapping that extends the object over the same offsets maps the page again, zeroed, so the buffer and the new memory can be one. Linux keeps them apart. It would take the VM letting go of held pages at unmap ([docs/held-buffers.md](docs/held-buffers.md#what-the-implementation-changed)).
- **A fixed read in flight during `fork(2)`** lands in the child's copy of the buffer, as the parent's buffer is held again at the fork; on Linux it lands in the parent's.

## Measurement

- **Run on UFS with a real disk, and on NVMe.** Storage results so far are from a SATA SSD with ZFS, and md(4) disks.
- **fio's cached ZFS reads at 250,000–330,000 IOPS.** Seen once on the Ryzen on 2026-11-09, with kqring at 1 and 2 jobs, where 890,000 is usual, and not reproduced since: not with or without `zpool sync` after writing the file, nor with the ARC target squeezed to 755 MB. It is not the dbuf cache that gave `kqring_bench`'s file group two speeds ([BENCHMARK.MD](BENCHMARK.MD#pipes-and-files-inline-or-through-a-worker)): fio's dataset has 4 KiB records, and its `--direct=1` reads are ZFS's uncached reads, which don't use that cache. `fio/scale.sh` writes the ARC hits, misses and size for each cached run to `arc.txt`.

## Not planned

- **Zero-copy `SPLICE`.** `IORING_OP_SPLICE` needs a pipe at one end, so it can't express a direct file-to-socket transfer. Programs on FreeBSD use `sendfile(2)`, or `READ_FIXED` with `SEND_ZC`.
