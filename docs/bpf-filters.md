# Restrictions and BPF filters for a process

Status: implemented, November 2026. Roadmap phase 14.

Linux 7.3 lets a program limit what io_uring may do, beyond the per-ring `IORING_REGISTER_RESTRICTIONS` it has had since 5.10:

- **Restrictions for a task.** `io_uring_register(-1, IORING_REGISTER_RESTRICTIONS, ...)` with a `struct io_uring_task_restriction` takes the same entries as a ring's restrictions and applies them to every ring the task, and every task it forks, sets up afterwards.
- **Classic BPF filters.** `IORING_REGISTER_BPF_FILTER` attaches a classic BPF program (the `struct sock_filter` kind seccomp uses) to one request opcode, on a ring or, with no ring, for the task. Each request of that opcode runs the program as it is submitted, and a result of 0 fails it with `-EACCES`.

Both are what a sandbox needs: a supervisor sets them, then runs code it doesn't trust, which can add to them but not remove them. kqring has both, in `kmod/kqring_bpf.c`.

## What a filter sees

The program reads a 40-byte `struct io_uring_bpf_ctx` through 32-bit absolute loads (`BPF_LD|BPF_W|BPF_ABS`), in host byte order: the request's `user_data`, opcode and SQE flags, and for four opcodes their arguments, in a union at offset 16:

| Opcode | Fields | Bytes (`pdu_size`) |
|---|---|---:|
| `SOCKET` | family, type (with `SOCK_NONBLOCK` and `SOCK_CLOEXEC`), protocol | 12 |
| `OPENAT`, `OPENAT2` | flags, mode, resolve | 24 |
| `CONNECT` | family, port, IPv4 or IPv6 address, the latter only if the address is long enough for its family | 24 |

Values are FreeBSD's, as everywhere in kqring: `AF_INET6` is 28 and `O_CREAT` 0x200. A `struct sockaddr` starts with its length on FreeBSD, and kqring takes the family from its own field. The filter runs last, on a request otherwise valid, so a request that fails earlier (a `CONNECT` whose address is too short for even a family) fails with its own error.

Programs are checked as Linux checks them: `bpf_check_classic()`, which allows only forward jumps that stay in the program, a `RET` last, no division by a constant 0, shifts below 32 and scratch memory written on every path before it is read; then `io_uring_check_cbpf_filter()`, which allows only the loads and operations a filter on this context needs. In particular it refuses byte and halfword loads, indirect loads and `BPF_MOD`, which the generic check accepts. kqring runs the programs with a small interpreter of its own rather than `bpf_filter(9)`, whose word loads are in network byte order; as Linux, a division by an X of 0 makes the program return 0, which denies.

An opcode's filters all run, newest first, and the first 0 denies. `IO_URING_BPF_FILTER_DENY_REST` puts a filter that denies on every opcode that has none yet, and a filter added there later doesn't lift it. `IO_URING_BPF_FILTER_SZ_STRICT` and a `pdu_size` larger than the kernel's fail with `-EMSGSIZE`; either way the kernel's size is written back.

## Processes, not threads

Linux keeps both per task, that is per thread: a thread's registrations apply to the rings it sets up and to the threads and processes it creates afterwards, not to its siblings. kqring keeps them per process. A filter registered in one thread holds for rings any thread of the process sets up, and `fork(2)` gives the child a copy. FreeBSD's `no_new_privs` (`PROC_NO_NEW_PRIVS_CTL`), which registering for the process needs unless the caller is root, is itself per process, as is everything else kqring keeps for a process.

The state survives `execve(2)`, as `no_new_privs` does, and goes with the process at exit. A process may register restrictions once (`-EPERM` after restrictions or a filter); filters it may add at any time.

A ring set up by a process with restrictions has them as if it had registered them itself, so it can't register its own (`-EBUSY` on a disabled ring), and their register rules apply even before `IORING_REGISTER_ENABLE_RINGS`. Filters are shared between the process and its rings until either adds one; the ring's set and the process's then grow apart, each keeping all the filters it had.

## kqring's extensions

kqring's own requests (event-driven chains, kevent requests, the kqueue bridge) use an escape opcode a ring opts in to. They can run requests and wait for events a filter can't see, so a ring with BPF filters refuses them with `-EACCES`, as a ring with restrictions does ([event-chains-spec.md](event-chains-spec.md)).

## Implementation

- An opcode's filters are a list that is only prepended to, so lists share their tails and a node holds a reference on the next. A table of list heads by opcode is shared, by reference, between a process and the rings it sets up, and copied by whichever adds a filter first, as Linux's copy-on-write.
- A ring's submissions read its table without a lock. Registering on a ring changes its own table in place, one pointer at a time, or replaces a shared one; a replaced table stays with the ring until it is freed, so a submission can't see it go.
- The process state is found by process under an `sx` lock, which the fork and exit hooks and ring setup take only while some process has registered anything.
- Linux's restriction entries for requests and for registrations take effect separately: a ring whose restrictions name only opcodes keeps all its registrations. kqring had treated any restrictions as restricting both, which this work corrected.

## Tests

- `tests/kqring_diff.c` cases `restrictions_split`, `task_restrict` and `bpf_filter` give the same output on kqring as on Linux 7.3: the error for each invalid program and argument, `pdu_size` written back, a division by 0, `DENY_REST`, socket arguments, inheritance and the rings set up before and after.
- liburing's `task-restrict` and `cbpf_filter` pass. `cbpf_filter` is adapted in two places ([liburing/README.md](../liburing/README.md#adapted-tests)): `RESOLVE_IN_ROOT`, which kqring doesn't implement, gives way to `RESOLVE_BENEATH`, and a connect the filter allows to an address nobody answers gives up after a second instead of FreeBSD's 75.
- `tests/kqring_test.c`'s `test_bpf_process`: per process, extension requests refused, restrictions kept across `execve(2)`.
