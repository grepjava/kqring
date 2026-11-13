# Fan-in against kqueue

Status: in progress, November 2026. Roadmap phase 12.

[BENCHMARK.MD](../BENCHMARK.MD#fan-in-many-connections-each-get-one-message-and-all-of-them-are-collected) has kqring's fan-in, many connections each getting one message and all of them collected, 26–66% slower than kqueue on the Ryzen, where a system call is cheap. This note measures where the time goes.

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

## What is left

- At 1,024 connections a multishot receive is still about 16% slower than kqueue on a `DEFER_TASKRUN` ring (579 against 497 ns).
- A default ring wakes its kernel thread for each message a socket upcall queues, and the thread and the writer contend for the ring's mutex. Linux runs a default ring's completions in the submitting task instead. In ping-pong this costs 260 ns per message on the Ryzen and 563 on the i5, where Linux's default ring costs what its `DEFER_TASKRUN` ring does.
- `kqring_bench`'s fan-in and ping-pong measure kqring's provided buffers, 4,096 of 4 KiB in turn and so out of cache, against kqueue's one.
