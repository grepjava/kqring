# Spec: cheaper worker hand-off

**Status:** not implemented. This is for the implementer of the worker-pool changes.

A request that runs on a kqring worker thread (blocking pipes, page-cache misses, `IOSQE_ASYNC`, and the other worker-only operations) costs about 4.4 µs more than running it on the issuer. This spec measures where that time goes and proposes how to cut it. It touches only the hand-off: how work reaches an idle worker and how the issuer learns it's done. Worker signal, suspend and exit handling must not change.

## Measurements

Host: i5-8250U, FreeBSD 15.1 `GENERIC`. Workload: `NOP` with `IOSQE_ASYNC`, one per enter, which measures the round trip with no I/O. It costs 4,443 ns per op (median of 5, `floor` group in `tests/kqring_bench.c`).

DTrace, time from `sched:::wakeup` to `sched:::on-cpu` for the threads involved, about 260,000 round trips:

| Step | Mean | Distribution |
|---|---:|---|
| Worker woken by the issuer, until it runs | 2.0 µs | 61% 1–2 µs, 36% 2–4 µs |
| Issuer woken by the worker, until it runs | 2.2 µs | 9% 1–2 µs, 91% 2–4 µs |

The two wake-ups account for about 4.2 µs of the 4.4 µs. The work itself (dequeue, run, queue the result) is a few hundred ns. Each wake-up crosses CPUs: the sleeping thread's CPU is idle, so it takes an IPI and an exit from the idle state.

## Where the waits are

- **Issuer side.** `kqring_wq_enqueue()` (`kmod/kqring_wq.c`) queues the op, does `wakeup_one(&wq->ops[type])`, and for ops expected not to block, waits in the `kqwqsy` loop: `msleep_sbt(&ctx->wakeup_chan, ..., 100 µs)` until `op->wq_done`. The worker's completion wakes it (`wakeup(&ctx->wakeup_chan)` when `ctx->sleeping`).
- **Worker side.** An idle worker sleeps on `&wq->ops[type]` in `kqring_wq_thread()` and is woken per request.

## Proposed changes, in order of expected gain

1. **The issuer spins before sleeping in the `kqwqsy` wait** (saves the 2.2 µs issuer wake-up when the worker finishes quickly). Before `msleep_sbt()`, spin up to a bound (default about 5 µs; maybe the ring's `IORING_REGISTER_NAPI` `busy_poll_to` when that is larger) on `op->wq_done` read without `ctx->mtx`. Use `cpu_spinwait()` with periodic `getsbinuptime()`, as `kqring_wait_cqes()` does for the NAPI busy-wait. Fall back to the existing sleep, so correctness doesn't depend on the spin. Keep `ctx->sleeping == false` while spinning, so the worker skips `wakeup()`.
2. **Idle workers spin briefly before sleeping** (saves the 2.0 µs worker wake-up for back-to-back requests). Before sleeping on `&wq->ops[type]`, an idle worker spins up to a few µs on `wq->pending[type]`. The issuer then skips `wakeup_one()` when a worker is known to be spinning (a `wq->spinning[type]` count under `wq->mtx`, or an atomic). This must not touch the worker's signal, suspend or exit checks. Spin only between requests, never while holding `wq->mtx`, and re-check the existing exit conditions (`wq->closing`, and so on) exactly as the sleep path does.
3. **Optional: prefer the issuer's CPU's neighbour.** Waking a worker on an idle SMT sibling, or the same core, is cheaper than on a distant core in a deep C-state. That calls for scheduler hints (`sched_bind()`, or cpuset affinity through the existing `IORING_REGISTER_IOWQ_AFF`), so measure 1 and 2 first.

Not proposed: changing how workers handle signals, suspension, process exit or exec; running worker ops on the issuer; or system tuning such as `dev.cpu.N.cx_lowest`, which is the administrator's choice. That tuning would also shorten both wake-ups.

## Acceptance

- The `floor` group's `kqring_nop_async` drops from about 4.4 µs. The target is under 2 µs with change 1 alone, and under 1 µs with 1 and 2.
- The blocking-pipe row (`pipe` group, `kqring_read`, 5.4 µs) improves by a similar amount.
- No change in CPU use when idle: spinning is bounded, and happens only while a request is in flight or just finished.
- The full suite passes 10/10 on the host with the module reloaded before each pass (`tests/kqring_test`, including `test_worker_*`, `test_worker_process_lifecycle` and the cancellation tests), with no leaks (`vmstat -m`, `kern.openfiles`, `vm.stats.vm.v_user_wire_count`).
- Unloading the module and killing the process mid-flight behave as before.

## Reproducing the measurement

```sh
# the floor group's kqring_nop_async row
./tests/kqring_bench floor
# wake-up latencies (needs dtraceall loaded)
sudo dtrace -s handoff.d -c "./prof nopasync 2"
```

`handoff.d`:

```d
sched:::wakeup /pid == $target || args[1]->p_pid == $target/ { w[args[0]->td_tid] = timestamp; }
sched:::on-cpu /w[curthread->td_tid]/ {
	@[curthread->td_name == "kqwqb" || curthread->td_name == "kqwqu" ?
	    "worker wake->run" : "owner wake->run"] = quantize(timestamp - w[curthread->td_tid]);
	w[curthread->td_tid] = 0;
}
```
