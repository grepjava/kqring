# Follow-up: the three red benchmark cases

Measured on 2026-11-02 against baseline `2af2adf`, on FreeBSD 15.1 amd64.

**The 16 KiB registered AF_UNIX send now beats write(2).** The armed receive improves modestly but remains slower than kqueue; the single-file result is effectively unchanged.

| Case | Baseline median ns/op | Candidate median ns/op | Change | Candidate vs comparison |
|---|---:|---:|---:|---:|
| AF_UNIX 16 KiB registered SEND_ZC | 1,915.9 | 1,681.0 | -12.26% | 4.3% less time |
| AF_UNIX 64 B armed RECV | 1,386.4 | 1,355.0 | -2.26% | 22.0% more time |
| UFS cached 4 KiB READ | 1,342.0 | 1,335.7 | -0.47% | 11.1% more time |

![Three target cases after optimization](summary.svg)

## Implementation

- On amd64 AF_UNIX zero-copy sends, physically consecutive pinned pages share an mbuf, up to a 64 KiB run. The amd64 sf_buf mapping uses the direct map, which makes these physical runs contiguous kernel addresses. Fragmented pages split into separate mbufs. Each whole pinned buffer remains held until the last mbuf releases its notification reference. TCP and other architectures retain page-sized mbufs. References for the private chain are reserved together before pr_send publishes it. The speedup depends on physical contiguity; fragmented buffers can retain the earlier overhead.
- Detached one-shot RECV requests leave active_ops while taskwork is dequeued under the mutex already held there, saving a lock/unlock at completion. This is restricted to requests without timers, linked timeouts, listening sockets, polling or worker readiness state. A spurious wakeup rearms the request.
- The first pass also removed a duplicate file descriptor lookup and an unlocked idle hint on awakened receives. The file change does not close the gap behind pread.
- `KQRING_BENCH_FILE_DIR` selects the file benchmark filesystem, and the optional `send-unix` group runs only AF_UNIX sends. The default groups and `send` still include TCP.

## Measurements

Five alternating baseline/candidate pairs, reloading the module before each run. Both modules used the same benchmark binary and `pingpong file send-unix` groups. Each file `bench.round2.{base,opt}.{1..5}.txt` contains all 43 measurement rows from one run. Percentages use medians, not the best observed runs.

The test host now mounts /tmp on ZFS. File tests instead used a temporary 128 MiB swap-backed md(4) disk formatted as UFS with soft updates at `/mnt/kqring-perf`, matching the filesystem path measured in the original chart. The 0.5% file difference is too small to claim a meaningful speedup. Socket tests measure the same single-threaded loopback workloads as the published benchmark.

The original full benchmark charts remain historical. This focused chart uses only the newly measured target cases; it does not mix old and new rows. [First-pass results](round1.md) and their `bench.compare.*.txt` runs are retained for comparison.

## Validation

FreeBSD kernel and test builds passed with warnings treated as errors. All 84 functional test groups passed both with the normal memlock limit and with an 8 MiB limit. The expanded zero-copy tests check byte-for-byte output and deferred notifications with unaligned first/last pages, repeated physical-page aliases (forcing separate mbufs), and, with the larger limit, transfers across the 64 KiB run cap. The existing duplicate-waiter test checks that spurious RECV readiness rearms correctly; the suite also exercises cancellation, links/timeouts, descriptor reuse, teardown, fork and zero-copy lifetime.

Logs: [normal limit](test.round2.normal.log), [larger limit](test.round2.large.log). A full send-group run, including TCP, also passed before the final receive change. All assertions passed in the five paired focused runs. After testing the module was unloaded, the temporary UFS filesystem and md device were removed, and vmstat -m showed no remaining kqring allocation row.

## Remaining gaps

The armed one-shot receive still submits and later reaps in separate enters, and installs/removes a waiter each time. The existing multishot receive avoids repeated arming and is already faster than kqueue. Single 4 KiB reads still pay request and completion bookkeeping beyond pread; batching remains the effective way to amortize that overhead. These two cases have not turned blue.
