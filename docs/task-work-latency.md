# A completion whose thread then blocks

On a ring without `DEFER_TASKRUN`, a completion made in a thread of the ring's process, as a socket upcall runs inside the `write(2)` that feeds a waiting `RECV`, runs as that thread returns to user space (phase 18, [fan-in.md](fan-in.md#phase-18)). The thread's call may block before it returns: a `write(2)` larger than the socket's buffers waits for space that only reading the socket frees. Linux runs such work as task work of the task that submitted the request, and interrupts that task to run it. Before phase 20, kqring left it to the thread that made it, and a watcher handed it to the ring's kernel thread after one to two ticks.

## Phase 20

### How it is measured

`tests/kqring_twlat.c` builds on both systems. A `RECV` is armed on one end of an AF_UNIX stream socket pair, and a second thread of the process writes 1 MiB to the other end: the first bytes complete the `RECV` from inside that `write(2)`, which then blocks. The main thread watches for the CQE by spinning on the CQ ring (`spin`), in `poll(2)` on the ring's descriptor (`poll`) or in an enter (`enter`), notes how long after the write began it appeared, then reads the socket dry so that the write can finish, and records what `write(2)` returned. With `self-`, the writing thread submits the `RECV` itself, just before its write, so that the submitter is the thread that blocks. 200 rounds per run, three runs, on the i5-8250U (`hz=1000`), FreeBSD 15.1 and Ubuntu's Linux 7.3.0-10 on the same machine. The outputs are in [bench/twlat/](bench/twlat/).

### Before

| CQE after the write began, µs: median (p99) | kqring, phase 19 | Linux 7.3 |
|---|---:|---:|
| default ring, another thread writes, spin | 1,921–1,925 (1,988) | 8–13 (22–33) |
| default ring, another thread writes, `poll(2)` | 1,891–1,894 (1,906–1,959) | 12–18 (61–70) |
| `COOP_TASKRUN`, another thread writes, spin | 1,924 (1,930–1,989) | 607–710 (757–931) |
| `COOP_TASKRUN`, another thread writes, `poll(2)` | 1,890–1,894 (1,901–1,969) | 15–19 (50–60) |
| any ring, waiting in an enter | 7.8–8.4 (11–33) | 12–28 (37–91) |
| default or `COOP_TASKRUN`, the submitter writes, spin | 1,923–1,926 (1,934–1,990) | 24–25 (33–38) |
| default or `COOP_TASKRUN`, the submitter writes, `poll(2)` | 1,891–1,895 (1,901–1,965) | 26–35 (72–84) |

Every write completed whole on kqring. On Linux, every one completed whole when another thread wrote, and **every one returned short** when the submitter wrote: Linux's task work goes to the submitting task (an IPI if it runs in user space, a wake-up if it sleeps; `COOP_TASKRUN` drops the IPI, hence its 0.7 ms when spinning), and when that task is the one blocked in `write(2)`, it interrupts the write.

### What changed

- **Only the submitting thread's return runs a completion.** On a ring without `SINGLE_ISSUER`, each process's context remembers the thread that last submitted (`submit_td`, set in the enter, compared and never dereferenced); a completion made by any other thread goes to the ring's kernel thread at once, as Linux's goes to the submitter. On a `SINGLE_ISSUER` ring the owner check already meant this.
- **The submitter's own sleep is interrupted.** When the submitting thread queues a completion for its own return, its next interruptible sleep in that system call returns at once (FreeBSD's `TDP_WAKEUP`, the flag `thr_wake(2)` sets), as Linux's task work interrupts the task: the call returns, short if it had moved data, and its return posts the CQE. The flag is set only for a thread outside kqring's own calls (an enter runs the work itself), only if it isn't set already, and only with a free slot in its bucket to remember it by; the return-to-user hook clears it if no sleep took it, before anything of its own can sleep, and the process's exit and exec hooks forget threads killed in the kernel. A sleep that can't be interrupted, or a call that runs long without sleeping, is still left to the watcher.

### Two more ways the submitter's CQE came late

`test_default_ring_waiter_handoff` (phase 18) asserts that on a `COOP_TASKRUN` ring the CQE of a `RECV` completed by the submitting thread's `write(2)` is there when the write returns. Run 20 times on the first phase 20 build it failed once or twice; on the build before, about once in 40. A DTrace trace of a failing round showed the thread's return-to-user hook finding nothing to run, and the kernel thread posting the CQE 5 µs later:

- **A race in the watcher (since phase 18).** A pass took the generation counter with a fetch-and-add, which returns the old value, and handed on every context whose generation wasn't that value. A context queued while the pass was still going had read the new one, so the same pass took it at once, microseconds old, from under the thread about to run it. The pass now hands on only contexts older than its generation, compared by age so that the counter may wrap.
- **The kernel thread awake.** A completion queued while the ring's kernel thread was running (not asleep) was left to it, without trying the submitter's return at all. Phase 20 makes that likelier, since other threads' completions now wake it. Now the return is tried either way; it runs what is left under `sq_sx`, so after the kernel thread if that has begun, and the CQE is there when the call returns, whichever of the two posts it.

With both, the test passed 60 runs of 60. The part of it that has another thread write now waits up to 100 ms for the CQE instead of asserting it at once: that thread's completion goes to the kernel thread, which posts it within microseconds but not necessarily before the thread has been joined. Linux makes the same promise only for the submitting task.

### After

| CQE after the write began, µs: median (p99) | kqring, phase 20 | Linux 7.3 |
|---|---:|---:|
| default ring, another thread writes, spin | 7.8–10.8 (20–22) | 8–13 (22–33) |
| default ring, another thread writes, `poll(2)` | 8.6–9.4 (20–27) | 12–18 (61–70) |
| `COOP_TASKRUN`, another thread writes, spin | 7.4–7.9 (15–21) | 607–710 (757–931) |
| `COOP_TASKRUN`, another thread writes, `poll(2)` | 8.6–11.0 (22–32) | 15–19 (50–60) |
| any ring, waiting in an enter | 8.0–8.4 (14–23) | 12–28 (37–91) |
| default or `COOP_TASKRUN`, the submitter writes, spin | 8.5–8.6 (25–30) | 24–25 (33–38) |
| default or `COOP_TASKRUN`, the submitter writes, `poll(2)` | 11.5–12.1 (28–37) | 26–35 (72–84) |

As on Linux, the submitter's writes now return short (200 of 200), and others' complete whole. One difference remains: with a thread waiting in an enter, that thread posts the completion and the submitter's write isn't interrupted, where Linux interrupts it anyway.

`test_tw_wake` checks the three cases on both kinds of ring: another thread's completion and the submitter's reach a thread spinning on the CQ within a 500 µs median (about 1,900 µs before), the submitter's blocked write is interrupted, and a submitter whose write completed a request without blocking gets its byte from the next blocking `read(2)`, not `EINTR`.

### What it costs

Phase 19's build (HEAD), the same with 48 bytes of padding at the start of `kqring_wq.c`'s code (a control for code placement), and phase 20's final build, alternated, five rounds of every `kqring_xbench` case on a default and a `DEFER_TASKRUN` ring and fio's cached random reads ([bench/twlat/i5-p20-xbench-final.steps](bench/twlat/i5-p20-xbench-final.steps)). No row is 2% slower than HEAD. The worst: the cross-process ping-pong with a `RECV` armed on a default ring +1.7% (control +1.3%), 32 `NOP`s per enter on a `DEFER_TASKRUN` ring +1.6% (control 0.0%), one `NOP` per enter there +1.4% (control +0.2%); the enter now also looks at the trigger work list, and the submitter's completion records its thread in a slot and clears its flag at return, under the bucket lock the hook already takes. Ping-pong with a `RECV` armed first on the same thread is within 0.4%, and fan-in within 0.5%.
