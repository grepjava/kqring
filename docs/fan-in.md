# Fan-in against kqueue

Status: done, November 2026. Roadmap phases 12 and 16.

[BENCHMARK.MD](../BENCHMARK.MD#fan-in-many-connections-each-get-one-message-and-all-of-them-are-collected) had kqring's fan-in, many connections each getting one message and all of them collected, 26–66% slower than kqueue on the Ryzen, where a system call is cheap. This note measures where the time went. Since phase 13, when `kqring_bench` stopped handing kqring's receives a 16 MiB pool of buffers, BENCHMARK.MD has it level with kqueue.

## How it is measured

[`tests/kqring_xbench.c`](../tests/kqring_xbench.c) has three fan-in cases, which build on Linux as well: N AF_UNIX stream connections each get one 64-byte `write(2)`, and all N messages are collected, by

- `fanin_kevent`: one `kevent(2)` (on Linux `epoll_wait(2)`) and N `recv(2)`;
- `fanin_ring_batch`: N single-shot `RECV`s, each into its own buffer, submitted and waited for in one enter;
- `fanin_ring_multishot`: N multishot `RECV`s armed once on a provided buffer ring of 64-byte buffers, one enter per round to wait for the N CQEs.

Times are per message and include its `write(2)`, medians of 5 one-second runs (3 in an A/B, which alternates the two modules three times). `XBENCH_FANIN_N` sets N, and `XBENCH_DEFER=1` sets the ring up with `SINGLE_ISSUER | DEFER_TASKRUN`, as `kqring_bench` does; without it the ring has the default flags, and its kernel thread completes the receives the socket upcalls queue, on another CPU. Ryzen 3 PRO 4350G, FreeBSD 15.1 `GENERIC` ([BENCHMARK.MD](../BENCHMARK.MD)), and for ping-pong also the i5-8250U that boots FreeBSD 15.1 and Ubuntu with Linux 7.3. Raw outputs are in [bench/fanin/](bench/fanin/).

## Where kqring stood

| ns per message | N = 16 | N = 256 | N = 1,024 |
|---|---:|---:|---:|
| kqueue | 397 | 411 | 485 |
| kqring, `DEFER_TASKRUN`: single-shot batch | 397 | 400 | 471 |
| kqring, `DEFER_TASKRUN`: multishot | 381 | 395 | 752 |
| kqring, default ring: multishot | 359 | 323 | 667 |
| `kqring_bench`: kqueue | 396 | 410 | 503 |
| `kqring_bench`: kqring multishot | 511 | 536 | 908 |

Three things are in this table.

- **With buffers that stay in cache, kqring is level with kqueue or ahead, up to 256 connections.** `kqring_bench` gives its ring 4,096 provided buffers of 4 KiB, 16 MiB, and the ring hands them out in turn, so each receive lands in memory that isn't in cache, where its kqueue variant reads every message into one buffer. A program that reads into many buffers pays for that as well, but it isn't the ring's cost.
- **On a default ring, kqring's lead is a second CPU's.** Its kernel thread runs the receives the upcalls queue while the writer goes on writing: the multishot case took 58,441 kernel samples in 8 seconds of DTrace's `profile-4999`, kqueue 34,411. Per message that is about 537 ns of CPU against kqueue's 420, as much as the `DEFER_TASKRUN` ring takes on one. A third of that ring's kernel samples were the writer waking the kernel thread for each message (`spinlock_exit` under `sousrsend()`), the thread going back to sleep, and both contending for the ring's mutex (`lock_delay`).
- **At 1,024 connections multishot falls behind**, while the single-shot batch doesn't.

## A thousand armed requests

The CPU's counters (`hwpmc`, over the whole run, divided by the messages) at N = 1,024, on a `DEFER_TASKRUN` ring:

| per message | kqueue | single-shot batch | multishot |
|---|---:|---:|---:|
| instructions | ~3,000 | ~3,300 | ~3,100 |
| cycles | ~1,900 | ~2,000 | ~3,200 |
| L2 data misses | ~44 | ~33 | ~53 |
| lines filled from memory | ~0.5 | ~0.5 | ~5 |
| L1 TLB misses | ~3.2 | ~2.9 | ~3.9 |

Multishot did as much work as the others and waited for memory: about five cache lines per message came from DRAM. The 1,024 socket pairs, their files and kqueue's knotes already take most of the 4 MiB L3; a multishot receive adds the request armed on each socket, 872 bytes, which its cycle walked from the socket's upcall to its CQE through 11 of the request's 14 cache lines. The single-shot batch reuses a few requests from the head of the free list, which stay in cache.

The fields the cycle reads and writes (in `kqring_soupcall()`, the taskwork queue, `kqring_taskwork_run_one()`, `kqring_op_attempt()`, `kqring_exec_recv()`, the buffer pick and commit and the CQE) now come first in `struct kqring_op`, in two cache lines; the taskwork and socket-waiter links start the part that isn't cleared on reuse; and the SQE copy, which started half way through a line, has a line of its own. The cycle touches 4 lines. The request grew from 864 to 896 bytes, still within `malloc(9)`'s 1 KiB bucket.

Alternated three times with the build before:

| ns per message | before | after | |
|---|---:|---:|---:|
| `DEFER_TASKRUN`, multishot, N = 1,024 | 812.4 | 578.6 | −28.8% |
| default ring, multishot, N = 1,024 | 685.9 | 588.2 | −14.2% |
| `kqring_bench`, multishot, N = 1,024 | 928.6 | 770.4 | −17.0% |
| default ring, multishot, N = 16 | 340.6 | 362.8 | +6.5% |
| default ring, multishot, N = 256 | 347.5 | 359.2 | +3.4% |
| one `NOP` per enter | 185.8 | 184.1 | −0.9% |

Every other row moved less than 2%. The default ring at 16 and 256 connections, where the writer and the ring's kernel thread work on the same requests from two CPUs, lost in all three rounds: the fields both write are closer together now. It is still ahead of kqueue there.

### Where the requests lie

After that change multishot still brought about 3.7 times as many lines from memory per message as kqueue at 1,024 connections, on fewer instructions. Sampling those fills (`pmcstat -S ls_refills_from_sys.ls_mabresp_lcl_dram`, one sample per 500, summed per function; [bench/fanin/ryzen-dram-samples.txt](bench/fanin/ryzen-dram-samples.txt)) shows where they land:

| DRAM-fill samples | kqueue | multishot |
|---|---:|---:|
| all | 4,633 | 19,091 |
| `uipc_sosend_stream_or_seqpacket()` | 523 | 4,129 |
| `uipc_lock_peer()` | 276 | 1,533 |
| `fget_unlocked_flags()` | 56 | 779 |
| kqring's functions, together | | about 1,700 |

Most of the extra misses are in the writer's `write(2)`, on the sending socket, its file and its peer, which the kqueue case touches as well; kqring's own code takes under a tenth. At 1,024 connections the working set has outgrown the 4 MiB L3, and the lines that miss are the ones longest unused, whoever added the lines that pushed them out. The gap grows with N (raw outputs `ryzen-eof-ab.steps`, `ryzen-zone-ab.steps`): at 512 connections multishot is ahead of kqueue (396–407 against 431–442 ns), at 2,048 both come from memory and multishot is 25% behind (1,371–1,398 against 1,090–1,113).

So what counts is what each armed receive adds per connection: the request's lines, the socket's waiter list, and, on AF_UNIX stream sockets, the end-of-input knote (`kqring_kpoll_soeof_watch()`), which `sorwakeup()` asks on every message. An experiment build without the knote was about 3% faster at 2,048 connections (medians 1,329 against 1,372 ns) and within the noise at 1,024; the knote stays, since FreeBSD 15 runs no upcall when the peer shuts down writing.

The requests were where the placement went wrong. They came from `malloc(9)`'s 1 KiB bucket, so each started on a 1 KiB boundary and the 4 lines a receive uses sat at the same offsets in every one: the 4,096 lines a thousand requests use mapped to a quarter of the sets of the L2 and L3. Requests now come from a zone of their own (`kqring_op`, 896-byte items), whose items fall at every offset. Alternated three times with the build before ([bench/fanin/ryzen-zone-unrelated.steps](bench/fanin/ryzen-zone-unrelated.steps), 2,048 from `ryzen-zone-ab.steps`):

| ns per message | before | after | |
|---|---:|---:|---:|
| multishot, `DEFER_TASKRUN`, N = 1,024 | 645–660 | 562–583 | −10.0% |
| multishot, default ring, N = 1,024 | 635–646 | 560–565 | −12.3% |
| multishot, `DEFER_TASKRUN`, N = 2,048 | 1,375–1,388 | 1,298–1,312 | −5.6% |
| one `NOP` per enter, ping-pong, `RECV`, fan-in at 16 and 256, batch | | | within ±2.2% |

kqueue measured 500–534 ns at 1,024 connections in the same runs, so multishot is now about 10% behind there, from 16–20%. The tests' leak check, which counted kqring's `malloc(9)` memory, also counts the zone's requests in use.

## A multishot receive out of buffers

A multishot `RECV` whose receive took the last buffer of its group went straight round again, failed to get a buffer, and ended with `-ENOBUFS`, even with nothing more to receive. Linux receives again at once only if more is buffered, and keeps the request armed otherwise: a buffer provided before more data comes keeps it going, and data with no buffer for it ends it. `tests/kqring_diff.c`'s `recv_mshot_drained` shows both, and kqring now matches Linux 7.3's output. `RECVMSG` had the same rule and the same change. A bundle still ends with the batch that empties its group, as in Linux.

## A receive that waits

`kqring_bench`'s ping-pong has a `RECV` submitted before its data 85% slower than kqueue on the Ryzen (991 against 536 ns at 64 bytes). It is how a server usually waits: the `RECV` finds nothing, arms the socket's upcall, the peer's write runs the upcall, which queues the request as taskwork, and the next enter runs the receive again and posts the CQE. `kqring_xbench` has the same loop, `pp_ring_armed`, with one buffer, against `pp_kevent`, which waits in `kevent(2)` (on Linux `epoll_wait(2)`, edge-triggered) and then calls `recv(2)`. Both make three system calls per message. `unix_recv_ring` is a `RECV` submitted after the write, which finds the data there; `getppid` is the system call floor.

| ns per message | Ryzen | i5-8250U, FreeBSD | i5-8250U, Linux 7.3 |
|---|---:|---:|---:|
| `getppid(2)` | 53 | 193 | 573 |
| kqueue or epoll, then `recv(2)` | 531 | 1,127 | 3,022 |
| `RECV` with the data there | 544 | 888 | 2,443 |
| `RECV` armed first, `DEFER_TASKRUN` ring | 847 | 1,332 | 3,667 |
| `RECV` armed first, default ring | 1,107 | 1,895 | 3,639 |
| armed less data-there less one system call | 249 | 252 | 651 |

All from 13 November, FreeBSD with the module of the multishot layout change; raw outputs in [bench/fanin/](bench/fanin/) (`*-pingpong.steps`). Linux's floor was measured after a reboot, with the same build.

- **On a `DEFER_TASKRUN` ring, waiting costs kqring about 250 ns on both machines, and io_uring 651 on the same i5.** Arming, the upcall, the taskwork and the second receive take under 40% of what they take on Linux, and on the i5 the armed `RECV` is 1.18 times kqueue's loop where io_uring's is 1.21 times epoll's. On the Ryzen, where a system call costs 53 ns, the same 250 ns is 60% of kqueue's whole loop.
- **A default ring adds 260 ns on the Ryzen and 563 on the i5,** for the hop to the ring's kernel thread, which runs the taskwork, and back. Linux runs a default ring's taskwork in the submitting task, and its two rows are the same.

Where the Ryzen's 300 ns or so between the armed `RECV` and the one that finds its data go (310 in the profiled runs), from DTrace's `profile-4999` (self time per function, scaled to each case's time; [bench/fanin/ryzen-pingpong-profile.txt](bench/fanin/ryzen-pingpong-profile.txt)):

| ns per message | |
|---|---:|
| the second enter's system call: user mode, entry, `amd64_syscall()` | ~92 |
| the enter's own work with nothing to submit: `kqring_sys_enter()`, `kqring_ioc_enter()`, taskwork, checks, eventfd | ~70 |
| arming: `kqring_arm_socket_op()`, `kqring_sb_arm()` | ~33 |
| the ring's mutex, four times: arming, the upcall, taking the taskwork, and once more after it | ~32 |
| the upcall, queueing and running the taskwork, disarming | ~25 |
| the end-of-file knote on AF_UNIX sockets, which `knote()` runs for each wake-up | 9 |

The CPU's counters agree: about 6,000 instructions per message against 4,100, at the same rate (1.69 instructions per cycle against 1.74), and no more lines from memory. The time is in instructions and atomic operations, spread over the path; nothing in it is waiting or repeated. What could go without changing how it works is about 20 ns: the two locked increments of the per-CPU count of enters, which unload uses, and the ring's mutex taken again after the last taskwork item only to clear `IORING_SQ_TASKRUN`. That is under 3%, and neither change was made.

## The default ring's kernel thread

A ring without `DEFER_TASKRUN` or `SQPOLL` has a kernel thread that posts its completions, so that they appear with nobody in an enter. The thread slept on the same channel as the enters waiting for CQEs, and new taskwork woke both: a thread asleep in an enter, which runs its process's taskwork when woken, and the kernel thread, which then fought it for `sq_sx`, the lock that serialises a ring's submissions and completions. The kernel thread now sleeps on a channel of its own. Taskwork wakes it only when no thread of the request's process is asleep in an enter (`tw_waiters`); every other wake-up still wakes both. A waiter that leaves without running the taskwork (interrupted by a signal, or `sq_sx` not taken) wakes the kernel thread itself. `test_default_ring_waiter_handoff` covers both: a waiter interrupted 20 times leaves the receive to the kernel thread, and a sleeping waiter runs it itself.

`kqring_xbench` has a ping-pong with a peer process, which echoes each message, so the reply comes while the ring's owner sleeps in its enter: `xp_kevent` and `xp_ring_armed`. Alternated three times on the Ryzen:

| ns per round trip | before | after |
|---|---:|---:|
| kqueue | 3,616–3,632 | 3,617–3,638 |
| `RECV` armed first, default ring | 3,976–3,989 | 3,925–3,929 |
| `RECV` armed first, `DEFER_TASKRUN` ring | 3,914–3,925 | 3,919–3,932 |

The default ring now costs what a `DEFER_TASKRUN` ring does when its owner waits, and the armed `RECV` is 1.08 times kqueue's loop. On the i5 under Linux 7.3 the same loop is noisier, and io_uring is further behind epoll: 7,973–9,426 ns for epoll, 9,986–10,032 for a `DEFER_TASKRUN` ring and 10,548–11,526 for a default one (raw outputs `ryzen-waiter-*.steps`, `i5-linux-xproc.steps`). The single-thread ping-pong above didn't change (1,088–1,110 against 1,085–1,120 ns), nor did any fan-in case: there the write comes before the enter that waits, so nobody waits when the upcall queues the receive, and the kernel thread runs it.

### The writer's return to user space

io_uring runs a default ring's taskwork in the submitting task, as it returns to user space. kqring now does that on rings set up with `IORING_SETUP_COOP_TASKRUN` (and without `DEFER_TASKRUN`), through a module AST (`ast_register()`, slot `TDA_MOD1`). When a request's completion is queued in a thread of its own process that may enter the ring, which is how a `write(2)` completes a `RECV` on the other end of a socket pair, `kqring_ast_try()` leaves it to that thread instead of waking the ring's kernel thread: the ring's context goes on a per-thread bucket, and `kqring_ast()` runs its taskwork as the thread leaves the kernel, so the CQE is there when `write(2)` returns. A completion queued anywhere else (an interrupt, a worker, another process) still goes to the kernel thread.

- **A thread that blocks.** Taskwork left to a thread's return waits while that thread's system call blocks, where the kernel thread would have run it. Linux has the same rule under `COOP_TASKRUN`, which asks it not to interrupt the task, and interrupts it otherwise, so rings without `COOP_TASKRUN` keep the kernel thread for everything.
- **A thread that never returns.** Another thread's `exit(2)` or `execve(2)` makes the rest exit in the kernel. The process's exit and exec hooks hand whatever was left to them to the kernel thread, and nothing is left to a thread once its process has `P_WEXIT` or `P_INEXEC` set.
- **Unload.** kqring had such a hook until October ([`5780521`](https://github.com/grepjava/kqring/commit/5780521)) and dropped it because `ast_deregister()` doesn't wait for a thread already calling the handler. Unload now stops queueing, hands on what is queued, deregisters the hook, and waits until no thread is inside it (`kqring_ast_active`, the only part that can block). A thread can be on its way into the handler, or out of it, only while it runs or waits to run, so unload then makes every user thread that isn't blocked go through `ast()` once more (`kqring_sys_drain_threads()`, as for the system calls); after 10 seconds it gives up with `EBUSY` and registers the hook again.

`test_default_ring_waiter_handoff` checks that on a `COOP_TASKRUN` ring the CQE is there when `write(2)` returns, from this thread or another of the process, and the stress test and fuzzer set up `COOP_TASKRUN` rings among others.

Alternated three times on the Ryzen with the module before both changes (`XBENCH_COOP=1` sets up the ring with `COOP_TASKRUN`):

| ns per message | before | after |
|---|---:|---:|
| ping-pong, `RECV` armed first, `COOP_TASKRUN` | 1,083–1,094 | **981–1,028** |
| the same, default ring | 1,062–1,090 | 1,097–1,149 |
| the same, `DEFER_TASKRUN` | 814–824 | 844–874 |
| peer process echoes, `COOP_TASKRUN` | 3,959–3,980 | 3,919–3,955 |
| fan-in, multishot, `COOP_TASKRUN`, N = 16 | 361–368 | 539–564 |
| fan-in, multishot, `COOP_TASKRUN`, N = 256 | 349–363 | 548–577 |
| fan-in, multishot, `COOP_TASKRUN`, N = 1,024 | 552–562 | 723–763 |
| fan-in, single-shot batch, `COOP_TASKRUN`, any N | 390–453 | 391–461 |

- **Ping-pong gains 10%, half of the default ring's 260 ns.** The rest is the hook's own cost. Against a `DEFER_TASKRUN` ring, which runs its taskwork in the enter that waits (DTrace's `profile-4999`; [bench/fanin/ryzen-hook-profile.txt](bench/fanin/ryzen-hook-profile.txt)): about 45 ns in the kernel's AST path (`ast_handler()` calls every registered handler's check, the thread lock twice, `ast_sig()`, `userret()`), 20 ns for `sq_sx`, which a ring with a kernel thread takes in the hook and again in the enter, 25 ns for a second pass of the taskwork, checks and eventfd, and 30 ns for queueing.
- **Multishot fan-in in one thread loses 50%.** The thread that writes the N messages now runs each receive as its `write(2)` returns, one hook each, where the kernel thread ran them on a second CPU, many per wake-up. Linux's `COOP_TASKRUN` does the same. A fan-in whose messages come from other processes, as a server's do, is unaffected, since the hook only takes completions queued in the ring's own process; the single-shot batch, whose receives find their data, is too.
- **Paths that don't use the hook are 1–2% slower** (`DEFER_TASKRUN` ping-pong, a `RECV` that finds its data, one `NOP` per enter; six alternations each, [bench/fanin/ryzen-hook-unrelated.steps](bench/fanin/ryzen-hook-unrelated.steps)). The same build with the hook never registered is as slow, and the waiter change alone was level, so it is where the module's code now lies rather than anything these paths run; the hook's fields of `struct kqring_tctx` were moved last, which didn't change it.

## A thousand connections, again

Roadmap phase 16 took up the gap at a thousand connections. The build from phase 15 had multishot fan-in on a `DEFER_TASKRUN` ring 7–16% behind kqueue at 1,024 connections and 9–16% at 2,048, depending on the run, and its DRAM fills per message (sampled as above, per 500, divided by the messages in the window) were 7.6 at 2,048 against kqueue's 5.7, 1.3 of them in kqring's own functions ([bench/fanin/ryzen-p16-baseline.steps](bench/fanin/ryzen-p16-baseline.steps)). Each message touched up to six of the request's lines, scattered: the two hot lines, the taskwork and waiter links on line 7, the SQE on line 12, and sometimes lines 2 and 4; and the socket's waiter list, an allocation of its own.

Each change below was alternated three times with the build before it on the Ryzen (`kqring_xbench`, medians of 5 one-second runs per round); raw outputs are the `ryzen-p16-*.steps` files.

| Change | multishot, `DEFER_TASKRUN`, N = 1,024 / 2,048 | Kept |
|---|---|---|
| The request's links, contexts and hot fields in adjacent lines, the SQE first | −6.6% / −3.1% | yes, the SQE moved back (below) |
| Prefetch the next queued request's lines in the taskwork loop | +1.8% / +0.1% | no |
| A socket's only waiter as the upcall's argument, with no waiter list | −2.6% / −3.3% | yes |
| All the upcall touches in one line, apart from what the kernel thread writes | 0.0% / −0.4%, and the default ring at 16 and 256 connections −11% and −9% | yes |
| Prefetch the next buffer-ring line, and the hot fields in three lines | +0.1% / +2.2%, the default ring at 16 and 256 +8% | no |
| The hot fields in three lines alone | −0.9% / +0.6% | no |
| Clearing on reuse from a line's start | +2.6% / +2.6% | no |
| The SQE in a line of its own at the end, as before | −0.1% / +2.3%, one `NOP` per enter −2.1% | yes |

The first change put the upcall's writes in the line the ring's kernel thread writes (`res`, the flags), and on a default ring, where the writer's CPU queues requests that the kernel thread runs, that line went back and forth: fan-in at 16 and 256 connections lost 4% to it, and as much again with the waiter list gone, until the upcall's fields (`taskwork_link`, `ctx`, `tctx`, `in_taskwork`, `cancelled`, `oneshot_sb`) had a line of their own. With the SQE first, one `NOP` per enter cost 2.2–2.6% more over five alternated rounds ([ryzen-p16-nop-ab.steps](bench/fanin/ryzen-p16-nop-ab.steps)); with it last again that is gone, for 2.3% at 2,048 connections.

### The lone waiter's bug

The lone waiter first failed the stress test, three panics in 20 minutes: page faults in the upcall, in `sowwakeup()` and in arming, a request on its socket's list twice. When a second request arms on a socket whose upcall argument is a lone waiter, `kqring_sb_arm()` puts both on a new list, and it set the first's `sw[i]` to NULL before linking it. That request's own thread reads the field without the sockbuf lock, in `kqring_taskwork_run_one()`, to ask whether the request is still waiting, and in `kqring_sb_disarm()`. Reading NULL in that window, it armed the request a second time, or could have freed it still on the list. A build that printed a stack wherever a request was armed while still on a list found it within a minute, from the SQPOLL thread. The field now goes from the tag to the list in one store, and the stress test, the fuzzer and the rest pass.

Without the lone waiter the layout alone gives less, and costs elsewhere. With the upcall's line holding `sw_link`, as the list needs, multishot fan-in at 1,024 and 2,048 connections was 4–7% faster than phase 15, but the single-shot batch at 1,024 was 4–5% slower and fan-in at 16 and 256 on a default ring about 3%; with the layout above, everything was within the noise ([ryzen-p16-layout-only-o.steps](bench/fanin/ryzen-p16-layout-only-o.steps), [-q](bench/fanin/ryzen-p16-layout-only-q.steps); `-m` and `-n` are two more layouts, one clearing from the middle of a line, which cost one `NOP` per enter 4–5%).

### How much is noise

Moving the module's code is enough to change these numbers. The build from phase 15 with its request code moved by 16 to 144 bytes, or its socket code by 64 to 512 (an unused block in front), measured 1,064–1,120 ns per message at 2,048 connections on a default ring and 433–452 for the single-shot batch at 1,024; one `NOP` per enter stayed within 3 ns ([ryzen-p16-placement-req.steps](bench/fanin/ryzen-p16-placement-req.steps), [-sock](bench/fanin/ryzen-p16-placement-sock.steps)). A difference of 3% at a thousand connections can be where the code lies rather than what it does; the changes kept here are beyond that.

### Against phase 15

Alternated three times with phase 15's build ([ryzen-p16-final-ab.steps](bench/fanin/ryzen-p16-final-ab.steps); the same layout before the fix, [ryzen-p16-before-fix-ab.steps](bench/fanin/ryzen-p16-before-fix-ab.steps)):

| ns per message | phase 15 | phase 16 | |
|---|---:|---:|---:|
| multishot, `DEFER_TASKRUN`, N = 1,024 | 556 | 512 | −7.9% |
| multishot, `DEFER_TASKRUN`, N = 2,048 | 1,296 | 1,204 | −7.1% |
| multishot, default ring, N = 1,024 | 537 | 505 | −5.9% |
| multishot, default ring, N = 2,048 | 1,073 | 1,006 | −6.3% |
| one `NOP` per enter, ping-pong, a `RECV` with its data, fan-in at 16 and 256, the single-shot batch | | | +1.2% to −1.8% |
| kqueue, N = 1,024 / 2,048 | 502 / 1,115 | 501 / 1,111 | |
| kqueue, a buffer per connection, N = 1,024 / 2,048 | 514 / 1,122 | 513 / 1,123 | |

kqueue's fan-in reads every message into one buffer, which stays in cache, where each multishot receive takes a provided buffer of its own. `XBENCH_FANIN_OWNBUF=1` gives kqueue a buffer per connection as well, as a server keeps one per connection: that costs it up to 2%.

So on a `DEFER_TASKRUN` ring multishot is now 2% behind kqueue at 1,024 connections and level with kqueue reading into a buffer per connection, from 11%; at 2,048 it is 8% behind (7% with a buffer per connection), from 16%. A default ring is level at 1,024 and 9% ahead at 2,048, and both are ahead at 512 (399 and 348 ns against kqueue's 434, [ryzen-p16-final-profile.steps](bench/fanin/ryzen-p16-final-profile.steps)).

### What stops it at 2,048

At 2,048 connections the working set still outgrows the L3. DRAM fills per message, sampled as above in the same runs:

| | kqueue | multishot, `DEFER_TASKRUN` | of which kqring's functions |
|---|---:|---:|---:|
| phase 15, N = 2,048 | 5.7 | 7.6 | 1.3 |
| phase 16, N = 2,048 | 4.8 | 6.2 | 1.0 |
| phase 16, N = 1,024 | 0.2 | 0.5 | 0.04 |

At 1,024 almost nothing comes from memory any more. At 2,048 multishot still brings 1.4 lines per message more than kqueue: 0.4 in `kqring_recv_pick_buf()`, the provided buffer's ring entry and the buffer itself, which kqueue's one buffer doesn't need; 0.1–0.2 each in `kqring_exec_recv()` and the taskwork loop, the request's lines; and the rest in the socket's own code, where an armed receive's lines have pushed the socket's out. Closing that would take fewer lines per armed receive than kqring's request, its SQE and a provided buffer, for instance a receive that doesn't keep its SQE ([What is left](#what-is-left)).

## What is left

- At 2,048 connections a multishot receive on a `DEFER_TASKRUN` ring is still 8% slower than kqueue, and 2% at 1,024 ([above](#what-stops-it-at-2048)): an armed receive still touches more lines per message than a knote does (the request's three lines and its SQE, the provided buffer and its ring entry, and on AF_UNIX the end-of-input knote as well), and at 2,048 the working set doesn't fit the L3.
- When nobody waits in an enter, a default ring without `COOP_TASKRUN` still wakes its kernel thread for each message a socket upcall queues: 260 ns per message in ping-pong on the Ryzen, 563 on the i5. With `COOP_TASKRUN` the writer's return runs it, about 135 ns more than a `DEFER_TASKRUN` ring, and multishot fan-in written by the ring's own thread is 50% slower than with the kernel thread.
- `kqring_bench`'s fan-in and ping-pong measure kqring's provided buffers, 4,096 of 4 KiB in turn and so out of cache, against kqueue's one.
