# Three slower benchmark cases

Measured on 2026-11-02 against baseline `2af2adf`, on the FreeBSD 15.1 test host.

These are small overhead reductions, not a resolution of the three red bars. All three remain slower than their comparison. Changes this small, especially the armed receive, overlap run-to-run variation.

| Case | Baseline median ns/op | Candidate median ns/op | Change | Candidate vs comparison |
|---|---:|---:|---:|---:|
| AF_UNIX 16 KiB registered SEND_ZC | 1,917.9 | 1,899.9 | -0.94% | 8.6% slower |
| AF_UNIX 64 B armed RECV | 1,377.6 | 1,374.6 | -0.22% | 23.2% slower |
| UFS cached 4 KiB READ | 1,351.2 | 1,335.4 | -1.17% | 11.3% slower |

## Changes

- READ/WRITE inline dispatch classifies the file during its rights-checked lookup instead of doing a separate descriptor lookup first. Socket requests still take the nonblocking socket path; IOSQE_ASYNC retains its existing dispatch.
- Awakened socket receives go directly to the locked readiness check or soreceive instead of repeating the unlocked idle hint. Spurious wakeups still return EAGAIN and rearm.
- Zero-copy sends reserve mbuf notification references once per iovec segment, replacing one atomic reference acquisition per page. The release count and notification lifetime are unchanged.
- The benchmark accepts `KQRING_BENCH_FILE_DIR` (default `/tmp`) to select the filesystem explicitly.

A tested candidate that copied registered AF_UNIX sends below 32 KiB was rejected: its 16 KiB result was 2,057.5 ns/op, versus 1,901.5 ns/op for baseline registered zero-copy in the initial exploratory runs. The existing threshold behavior and tests are preserved.

## Method and validation

Five alternating baseline/candidate pairs, each running `kqring_bench pingpong file send`, with a module unload/reload between every run. Both modules used the same benchmark binary. Send tests ran as grepjava with an 8 MiB memlock limit. Raw runs are the adjacent `bench.compare.*.txt` files.

The host now uses ZFS for `/tmp`. The comparison above instead used a temporary 128 MiB swap-backed md(4) disk formatted as UFS with soft updates, mounted at `/mnt/kqring-perf`, selected using `KQRING_BENCH_FILE_DIR`. This exercises the cached-file path from the published chart; it is not a claim about ZFS or physical-disk latency.

The final FreeBSD build passed with warnings treated as errors. The functional suite passed all 84 groups with zero failures on both the revised candidate and final build; the final log is [functional.log](functional.log). All benchmark assertions passed. The module was unloaded and the temporary UFS mount and md device removed. No kqring allocation row remained in `vmstat -m` after unload.

The armed receive still needs two enters plus a write and installs/removes its one-shot waiter each time. The existing multishot receive benchmark avoids that repeated setup. Cached single reads and small sends still pay submission/completion bookkeeping that pread/write do not. The published charts have been left unchanged.
