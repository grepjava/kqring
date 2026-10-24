# `kqring` Architecture

`kqring.ko` is a loadable FreeBSD 14/15 kernel module implementing the Linux `io_uring` ABI over `/dev/kqring`, designed around the **`IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`** execution model.

---

## 1. Module & Source Layout

The kernel module (`kmod/`) is split into nine translation units sharing a single private header ([`kmod/kqring_var.h`](kmod/kqring_var.h)) and one public UAPI header ([`include/sys/kqring.h`](include/sys/kqring.h)):

```mermaid
flowchart TB
    subgraph Userspace["Userspace Process"]
        App["Application / Vega Runtime"]
        UAPI["include/sys/kqring.h<br/>(kqring_setup / kqring_enter / kqring_register)"]
        App --> UAPI
    end

    subgraph KMOD["kqring.ko (FreeBSD Kernel Module)"]
        DEV["kqring_dev.c<br/>Device, ioctl, mmap, setup,<br/>enter/wait, process hooks"]
        REG["kqring_register.c<br/>IORING_REGISTER_* dispatch,<br/>eventfd"]
        REQ["kqring_req.c<br/>SHM, CQ posting, SQE prep,<br/>lifecycle, chains, taskwork"]
        RSRC["kqring_rsrc.c<br/>Fixed files, reg bufs,<br/>PBUF_RING, personalities"]
        SOCK["kqring_sock.c<br/>Socket ops, upcalls,<br/>multishot, SEND_ZC"]
        FILE["kqring_file.c<br/>VFS/file/pipe ops,<br/>SPLICE / TEE"]
        SYNC["kqring_sync.c<br/>Futex, WAITID, timers,<br/>cancel, MSG_RING"]
        WQ["kqring_wq.c<br/>IO_WQ_BOUND / UNBOUND<br/>worker pool, SQPOLL"]
        ZC["kqring_zc.c<br/>Page wiring, RLIMIT_MEMLOCK,<br/>EXT_SFBUF, ZC notif"]
    end

    UAPI -- "ioctl / mmap" --> DEV
    DEV --> REG
    DEV --> REQ
    REG --> RSRC
    REG --> WQ
    REG --> SYNC
    REQ --> RSRC
    REQ --> SOCK
    REQ --> FILE
    REQ --> SYNC
    REQ --> WQ
    SOCK --> ZC
    SOCK --> RSRC
    FILE --> RSRC
    WQ --> REQ
    WQ --> SOCK
    WQ --> FILE
```

| File | Responsibility |
|---|---|
| [`include/sys/kqring.h`](include/sys/kqring.h) | Dual kernel/userspace UAPI header: `io_uring_sqe`, `io_uring_cqe`, `io_uring_params`, `io_uring_buf_ring`, `/dev/kqring` ioctls, opcode/flag constants, and inline userspace wrappers. |
| [`kmod/kqring_var.h`](kmod/kqring_var.h) | Kernel-internal structs (`kqring_ctx`, `kqring_op`, `kqring_wq`, `kqring_rsrc`, `kqring_bgroup`, `kqring_zc_buf`, `kqring_zc_notif`), limits, cross-TU prototypes, and hot-path `static inline` helpers. |
| [`kmod/kqring_dev.c`](kmod/kqring_dev.c) | `/dev/kqring` `cdevsw`, `kqring_open`, `kqring_ioctl`, `kqring_mmap_single`, ring setup/teardown, `kqring_wait_cqes`, process `exit`/`exec`/`fork` eventhandlers, and module load/unload. |
| [`kmod/kqring_req.c`](kmod/kqring_req.c) | OBJT_DEFAULT shared memory allocation, CQ ring posting and overflow queue, SQE preparation (`SUBMIT_STABLE` copyin), request allocation/free, chain/drain scheduling, and deferred taskwork execution. |
| [`kmod/kqring_rsrc.c`](kmod/kqring_rsrc.c) | Reference-counted fixed files (`ctx->files`) with Capsicum rights, registered buffers (`ctx->bufs`), classic & mmap provided-buffer rings (`ctx->bgroups`), and credential personalities. |
| [`kmod/kqring_register.c`](kmod/kqring_register.c) | `KQRING_IOC_REGISTER` dispatcher (`IORING_REGISTER_*`), restrictions, `IORING_REGISTER_PROBE`, and `eventfd` registration/signalling. |
| [`kmod/kqring_sock.c`](kmod/kqring_sock.c) | Non-blocking inline socket execution, `soupcall_set` / `solisten_upcall_set` arming, multishot `RECV`/`RECVMSG`/`ACCEPT`, `POLL_ADD`, `URING_CMD`, and `SEND_ZC`/`SENDMSG_ZC` mbuf construction. |
| [`kmod/kqring_file.c`](kmod/kqring_file.c) | Synchronous and worker-offloaded VFS/file/pipe operations (`READ`/`WRITE`, `READ_MULTISHOT`, `SPLICE`, `TEE`, `OPENAT`/`OPENAT2`, `STATX`, `*XATTR`, `FSYNC`, `FALLOCATE`, `FTRUNCATE`, `FADVISE`, `MADVISE`). |
| [`kmod/kqring_sync.c`](kmod/kqring_sync.c) | `FUTEX_WAIT`/`FUTEX_WAITV`/`FUTEX_WAKE` (`umtx_key`), `WAITID`, `callout`-backed `TIMEOUT`/`LINK_TIMEOUT`/`TIMEOUT_REMOVE`, `ASYNC_CANCEL`/`SYNC_CANCEL`, `POLL_REMOVE`, and cross-ring `MSG_RING`. |
| [`kmod/kqring_wq.c`](kmod/kqring_wq.c) | Per-ring or shared (`IORING_SETUP_ATTACH_WQ`) in-process kernel worker pools (`IO_WQ_BOUND` / `IO_WQ_UNBOUND`), non-blocking worker retirement/abort callout, and `IORING_SETUP_SQPOLL`. |
| [`kmod/kqring_zc.c`](kmod/kqring_zc.c) | Physical page holding (`vm_fault_quick_hold_pages`), per-UID `RLIMIT_MEMLOCK` & `vm.max_user_wired` accounting, `EXT_SFBUF` completion callbacks, and COW refresh on `fork(2)`. |

---

## 2. Shared-Memory Layout (`mmap_single`)

Each ring context (`struct kqring_ctx`) allocates two wired `vm_object_t` regions during `KQRING_IOC_SETUP` and optional per-group regions during `IORING_REGISTER_PBUF_RING` (`IOU_PBUF_RING_MMAP`). Each `mmap(2)` call acquires its own `vm_object_reference()`, preventing use-after-free if a buffer ring is unregistered while still mapped in userspace.

```mermaid
flowchart LR
    subgraph SHM1["SHM Region 1: IORING_OFF_SQ_RING / IORING_OFF_CQ_RING (ctx->shm_obj)"]
        direction TB
        H1["Cacheline 0 (0..63): SQ Consumer<br/>sq_head, sq_flags, sq_dropped"]
        H2["Cacheline 1 (64..127): SQ Producer<br/>sq_tail (userspace written)"]
        H3["Cacheline 2 (128..191): CQ Producer<br/>cq_tail, cq_overflow, cq_flags"]
        H4["Cacheline 3 (192..255): CQ Consumer<br/>cq_head (userspace written)"]
        SQA["SQ Index Array<br/>uint32_t sq_array[sq_entries]"]
        CQA["CQ Ring Array<br/>struct io_uring_cqe cqes[cq_entries]<br/>(16B standard or 32B with IORING_SETUP_CQE32)"]
        H1 --> H2 --> H3 --> H4 --> SQA --> CQA
    end

    subgraph SHM2["SHM Region 2: IORING_OFF_SQES (ctx->sqes_obj)"]
        SQES["SQE Array<br/>struct io_uring_sqe sqes[sq_entries]<br/>(64B standard or 128B with IORING_SETUP_SQE128)"]
    end

    subgraph SHM3["SHM Region 3: IORING_OFF_PBUF_RING | (bgid << 16) (bg->ring_obj)"]
        PBUF["Mapped Provided-Buffer Ring<br/>struct io_uring_buf_ring<br/>(shared tail + io_uring_buf entries)"]
    end
```

- **Cacheline separation:** [`struct kqring_shm_header`](kmod/kqring_var.h#L168-L185) places kernel-written and user-written indices on separate 64-byte cachelines to eliminate false sharing between userspace submission/reaping and kernel completion.
- **Overflow resilience:** If `cq_tail - cq_head >= cq_entries`, completions are queued in `ctx->overflow` (`TAILQ`) and `shm->cq_overflow` is incremented. `kqring_flush_overflow()` drains queued entries into the shared CQ ring as soon as userspace advances `cq_head`.

---

## 3. Execution Model (`SINGLE_ISSUER | DEFER_TASKRUN`)

`kqring` enforces a strict single-writer invariant for the Completion Queue (CQ), request lifecycle, and resource tables:

1. **Asynchronous contexts never write CQEs or mutate resource tables.** Socket upcalls, `callout` timer callbacks, `MSG_RING` senders, and in-process worker threads only enqueue work onto `ctx->taskwork_ops` (or `ctx->rcq` for `MSG_RING`), set `IORING_SQ_TASKRUN`, and wake the ring sleeper (`&ctx->cq_wait` / `ctx->sqpoll_Wait` / `ctx->evfd`).
2. **All completions and table mutations run on the serialized issuer path:**
   - **Standard mode:** The owning thread (`ctx->owner_tid`) inside `KQRING_IOC_ENTER`.
   - **`IORING_SETUP_SQPOLL` mode:** Either the in-process `kqring_sqpoll_thread` or the owning thread, mutually serialized by `sx_xlock(&ctx->sq_sx)`.

```mermaid
flowchart LR
    subgraph AsyncProducers["Asynchronous Event Producers (under ctx->mtx)"]
        UP["Socket Upcall<br/>(kqring_soupcall)"]
        TM["Timer Callout<br/>(kqring_timer_cb)"]
        WK["Worker Thread<br/>(kqring_wq_thread)"]
        ZC_CB["Network mbuf Free<br/>(kqring_zc_mext_free)"]
        MR["Cross-Ring MSG_RING<br/>(kqring_rcqe_queue)"]
    end

    subgraph Queues["Context Deferred Queues (ctx)"]
        TW["ctx->taskwork_ops<br/>(TAILQ of kqring_op)"]
        RCQ["ctx->rcq<br/>(STAILQ of kqring_rcqe)"]
    end

    subgraph SerializedIssuer["Serialized Issuer Context (Owner Thread or SQPOLL under sq_sx)"]
        RUN["kqring_run_taskwork()"]
        SUB["kqring_submit_sqes()"]
        CHK["kqring_run_checks()<br/>(timeouts / futexes / waitids)"]
        COMP["kqring_req_complete()<br/>& kqring_post_cqe_extra()"]
        CQR["Shared CQ Ring<br/>(ctx->cqes + ctx->overflow)"]
    end

    UP --> TW
    TM --> TW
    WK --> TW
    ZC_CB --> RCQ
    MR --> RCQ

    TW --> RUN
    RCQ --> RUN
    SUB --> COMP
    RUN --> COMP
    CHK --> COMP
    COMP --> CQR
```

---

## 4. Request Lifecycle & State Machine

Every submitted SQE is represented by a `struct kqring_op` allocated in `kqring_submit_sqes()`.

```mermaid
stateDiagram-v2
    [*] --> Allocated: kqring_op_alloc()
    Allocated --> Prepped: kqring_req_prep()<br/>(SUBMIT_STABLE copyin of iov/path/msghdr/ts)
    Prepped --> Deferred: IOSQE_IO_DRAIN active<br/>or predecessor linked
    Prepped --> Ready: No barrier (ready_q)
    Deferred --> Ready: Drain barrier cleared /<br/>predecessor completed

    Ready --> Issuing: kqring_req_issue()<br/>+ kqring_req_resources()

    Issuing --> Completed: Inline completion<br/>(NOP, sync VFS, ready socket)
    Issuing --> ArmedSocket: EAGAIN on socket<br/>(kqring_arm_socket_op)
    Issuing --> ArmedWorker: Blocking file/pipe/VFS or IOSQE_ASYNC<br/>(kqring_wq_enqueue)
    Issuing --> ArmedSync: FUTEX_WAIT / WAITID / TIMEOUT<br/>(active_ops + subsystem list)

    ArmedSocket --> Taskwork: Socket upcall fires<br/>(kqring_taskwork_add_locked)
    ArmedWorker --> Taskwork: Worker finishes I/O<br/>(op->wq_done = true)
    ArmedWorker --> Retired: Cancelled / timed out while running<br/>(op->wq_retired = true)
    ArmedSync --> Taskwork: Futex woken / timer expired

    Taskwork --> ArmedSocket: Spurious wakeup or<br/>multishot burst continues (EAGAIN)<br/>(posts intermediate CQE_F_MORE)
    Taskwork --> Completed: Final result ready<br/>(kqring_op_unarm_locked)

    Retired --> Completed: Cancelled CQE (-ECANCELED / -ETIME)<br/>posted on issuer path
    Completed --> [*]: kqring_op_free()<br/>(after worker drops op if wq_running)
```

### Key Invariants
- **`IORING_FEAT_SUBMIT_STABLE`:** `kqring_req_prep()` copies all indirect user arguments (`iovec` arrays, `msghdr`, `sockaddr`, file paths, `open_how`, xattr names/values, futex vectors, and `timespec`s) into kernel memory (`op->args`, charged against `ctx->arg_bytes <= KQRING_MAX_ARG_BYTES`) at submission time. Userspace can reuse or free argument memory immediately after `kqring_submit_sqes()` consumes the SQE.
- **Stack-bounded chain execution:** Linked requests (`IOSQE_IO_LINK`, `IOSQE_IO_HARDLINK`) and `IOSQE_IO_DRAIN` barriers are queued on `ctx->ready_q` and `ctx->defer_q` and driven iteratively by `kqring_flush_ready()`, never by recursive calls.

---

## 5. Socket Fast Path & Upcall Arming (`kqring_sock.c`)

Socket operations (`RECV`, `SEND`, `RECVMSG`, `SENDMSG`, `SEND_ZC`, `SENDMSG_ZC`, `ACCEPT`, `CONNECT`, `POLL_ADD`, and socket `READ`/`WRITE`) follow an **inline-first, upcall-fallback** pipeline:

```mermaid
sequenceDiagram
    participant App as Userspace (Owner)
    participant Req as kqring_req.c
    participant Sock as kqring_sock.c
    participant SB as FreeBSD Socket Buffer (so_rcv / so_snd)

    App->>Req: KQRING_IOC_ENTER (submit RECV)
    Req->>Sock: kqring_op_attempt(ctx, op, NULL)
    Sock->>SB: soreceive(MSG_DONTWAIT)
    alt Data already in so_rcv
        SB-->>Sock: bytes > 0 (or EOF)
        Sock-->>Req: 0 (op->res = bytes)
        Req->>App: Post CQE inline (1 syscall total)
    else Socket would block (EWOULDBLOCK)
        SB-->>Sock: EWOULDBLOCK
        Sock-->>Req: EAGAIN
        Req->>Sock: kqring_arm_socket_op()
        Sock->>SB: SOCK_BUF_LOCK + soupcall_set(kqring_soupcall)
        Note over Sock,SB: Re-checks readiness under SOCK_BUF_LOCK<br/>to close race between soreceive and arm
        SB-->>Sock: Armed (or ready_now)
        Note over SB: Peer writes data to socket
        SB->>Sock: kqring_soupcall(so, op, ...)
        Sock->>Req: kqring_taskwork_add_locked(ctx, op)<br/>+ wakeup(&ctx->cq_wait)
        App->>Req: Next KQRING_IOC_ENTER (or wakeup from wait)
        Req->>Sock: kqring_taskwork_run_one() -> kqring_op_attempt()
        Sock->>SB: soreceive(MSG_DONTWAIT)
        SB-->>Sock: bytes > 0
        Req->>Sock: kqring_op_unarm_locked() -> soupcall_clear()
        Req->>App: Post CQE
    end
```

- **One-op-per-direction guard:** FreeBSD sockets provide a single `so_upcall` slot per sockbuf (`so_rcv`, `so_snd`). `kqring_arm_socket_op()` checks `sb->sb_upcall != NULL` under `SOCK_BUF_LOCK` and fails a conflicting second armed request on the same socket and direction with `-EBUSY` rather than silently overwriting the existing upcall.
- **`listen(2)` race protection:** On FreeBSD 15, `solisten_proto()` converts a socket's `so_rcv`/`so_snd` union into listen queues under both sockbuf locks. Every sockbuf lock acquisition in `kqring_sock.c` re-checks `SOLISTENING(so)` before touching `so_rcv` or `so_snd`.

---

## 6. Zero-Copy Transmit (`SEND_ZC` / `SENDMSG_ZC`)

`kqring` implements true zero-copy transmit for connected `AF_UNIX` and `AF_INET`/`AF_INET6` TCP stream sockets on transfers of at least `PAGE_SIZE` (4 KiB), supporting both **Phase A** (`IORING_RECVSEND_FIXED_BUF` registered buffers) and **Phase B** (unregistered user buffers and multi-segment scatter/gather `SENDMSG_ZC`).

```mermaid
sequenceDiagram
    participant App as Userspace
    participant Sock as kqring_send_zc_iov()
    participant ZC as kqring_zc.c (VM & Accounting)
    participant Net as FreeBSD Socket / TCP / AF_UNIX Stack

    App->>Sock: Submit SEND_ZC / SENDMSG_ZC (>= PAGE_SIZE)
    alt IORING_RECVSEND_FIXED_BUF
        Sock->>ZC: kqring_zc_refresh_cow() (re-pin if ctx->cow_gen changed after fork)
        ZC-->>Sock: Held kqring_zc_buf from ctx->bufs[buf_index]
    else Unregistered Buffer / msg_iov
        Sock->>ZC: kqring_zc_buf_alloc() per active iovec segment
        ZC->>ZC: Charge RLIMIT_MEMLOCK + vm_user_wire_count<br/>vm_fault_quick_hold_pages(VM_PROT_READ)
        ZC-->>Sock: Per-segment kqring_zc_buf(s)
    end

    Sock->>Sock: Allocate kqring_zc_notif (refs = 1)<br/>+ pre-allocate kqring_rcqe (IORING_CQE_F_NOTIF)
    loop For each page slice across iovecs
        Sock->>Sock: sf_buf_alloc(vm_page) + m_get()<br/>Attach EXT_SFBUF (ext_free = kqring_zc_mext_free, ext_arg1 = zc)<br/>refcount_acquire(&zc->refs)
    end
    Sock->>Net: so->so_proto->pr_send(so, ..., top_mbuf)
    Sock->>App: Post primary CQE (res = bytes_sent, flags = IORING_CQE_F_MORE)
    Sock->>ZC: kqring_zc_rele(zc, in_issuer=true) (drops issuer ref)

    Note over Net: Peer drains AF_UNIX recvbuf or TCP ACK arrives
    Net->>ZC: kqring_zc_mext_free(mbuf) -> sf_buf_free() + kqring_zc_rele(zc, false)
    Note over ZC: Last mbuf drops zc->refs to 0
    ZC->>ZC: vm_page_unhold_pages() + kqring_pin_uncharge()
    ZC->>App: Queue pre-allocated IORING_CQE_F_NOTIF CQE via kqring_rcqe_queue()
```

### Zero-Copy Ownership & Accounting Model

```mermaid
classDiagram
    class kqring_pin_acct {
        +uidinfo *uip
        +rlim_t bytes
        +u_int refs
    }
    class kqring_zc_buf {
        +kqring_pin_acct *acct
        +vm_offset_t page_base
        +int npages
        +u_int refs
        +vm_page_t pages[]
    }
    class kqring_rsrc {
        +uintptr_t addr
        +size_t len
        +kqring_zc_buf *zbuf
        +int cow_gen
    }
    class kqring_zc_notif {
        +kqring_zc_buf *zbuf
        +kqring_zc_buf **zbufs
        +u_int nzbufs
        +kqring_ctx *ctx
        +kqring_rcqe *rcqe
        +u_int refs
        +bool on_ctx
    }
    class mbuf_EXT_SFBUF {
        +ext_arg1: kqring_zc_notif *
        +ext_arg2: sf_buf *
        +ext_free: kqring_zc_mext_free
    }

    kqring_rsrc "0..1" --> "1" kqring_zc_buf : slot ref
    kqring_zc_notif "1" --> "1..*" kqring_zc_buf : in-flight ref(s)
    mbuf_EXT_SFBUF "1..*" --> "1" kqring_zc_notif : zc->refs
    kqring_zc_buf "1..*" --> "1" kqring_pin_acct : per-UID charge
```

- **Decoupled page lifetime:** `RLIMIT_MEMLOCK` and `vm_user_wire_count` charges live on `kqring_zc_buf`, not on the ring slot. Unregistering a buffer or closing the ring while zero-copy mbufs are still unread in a peer socket or awaiting TCP ACK keeps the pages held and charged until the last `EXT_SFBUF` mbuf is freed.
- **Copy-on-write safety (`fork`):** `kqring_proc_fork` increments `ctx->cow_gen` on every ring in the parent process. Before a registered buffer is used for `SEND_ZC` or `SENDMSG_ZC`, `kqring_zc_refresh_cow()` compares `r->cow_gen` with `ctx->cow_gen` and re-pins the post-fork physical pages (discounting the old buffer's pages from the `RLIMIT_MEMLOCK` check if no unread mbuf holds an extra reference to the old `kqring_zc_buf`).
- **Module unload barrier:** Each active `kqring_zc_notif` increments `kqring_live_zc` via `kqring_zc_hold()`. `MOD_QUIESCE` rejects `kldunload` with `EBUSY` while `kqring_live_zc > 0`, and `MOD_UNLOAD` sets `kqring_unloading = true` and drains `kqring_live_zc` so no `EXT_SFBUF` mbuf can ever invoke `kqring_zc_mext_free` after `kqring.ko` is unmapped.

---

## 7. In-Process Worker Pool & Detached Cancellation (`kqring_wq.c`)

Blocking operations on regular files, pipes, FIFOs, devices, VFS paths, `WAITID`, non-socket `POLL_ADD`, and any SQE with `IOSQE_ASYNC` execute on an in-process kernel worker pool (`struct kqring_wq`, `IORING_FEAT_NATIVE_WORKERS`):

- **Two sub-pools per `kqring_wq`:**
  - `IO_WQ_BOUND` (default cap 64): regular file/device I/O.
  - `IO_WQ_UNBOUND` (default cap 256): pipes, FIFOs, VFS metadata/path operations, `SPLICE`/`TEE`, `WAITID`, and non-socket readiness waits.
- **Address-space sharing:** Workers are created with `kproc_kthread_add(..., td->td_proc)` inside the ring owner's `struct proc`, so `copyin`/`copyout`/`uiomove` and credential switching (`kqring_cred_enter`) work directly on user addresses without cross-process VM mapping.
- **Non-blocking cancellation & detached close:**
  - When an armed worker request is cancelled (`ASYNC_CANCEL`, `SYNC_CANCEL`, `POLL_REMOVE`, `LINK_TIMEOUT`, or ring close), `kqring` never blocks the issuer waiting for the worker thread.
  - Instead, the op is moved to `ctx->retired_ops` (`op->wq_retired = true`), its `-ECANCELED` (or `-ETIME`) CQE is posted via taskwork, `kqring_wq_abort_td()` sends `sleepq_abort(wtd, EINTR)` to interrupt any interruptible sleep, and `ctx->wq_abort_callout` periodically re-sends the interrupt every 1 ms until the worker exits its syscall and releases the op.
  - On `close(ring_fd)`, `kqring_dtor()` detaches any still-running workers (`op->wq_abandoned = true`) and defers final `kqring_ctx` / `kqring_wq` destruction until the last detached worker drops its `ctx->refs` reference (`kqring_ctx_rele()`).

---

## 8. Locking Hierarchy & Concurrency Invariants

To prevent deadlocks across synchronous submission, socket upcalls, callouts, worker threads, and process lifecycle hooks, `kqring` enforces a strict lock acquisition order:

```mermaid
flowchart TD
    L1["1. Process / Global Registry Locks<br/>kqring_proc_mtx (MTX_DEF)"]
    L2["2. Serialization SX Locks (sleepable)<br/>ctx->sq_sx (SQPOLL vs enter/register)<br/>ctx->bg_sx (provided-buffer groups)"]
    L3["3. Socket I/O Lock (sleepable)<br/>SOCK_IO_SEND_LOCK(so) / SOCK_IO_RECV_LOCK(so)"]
    L4["4. Subsystem Leaf Mutexes (MTX_DEF)<br/>kqring_futex_mtx / kqring_zc_mtx / SOCK_LOCK(so) / SOCK_BUF_LOCK(so, which)"]
    L5["5. Ring Context Mutex (MTX_DEF)<br/>ctx->mtx"]
    L6["6. Worker Pool Mutex (MTX_DEF)<br/>wq->mtx"]

    L1 --> L4
    L2 --> L3 --> L4 --> L5 --> L6
```

| Lock | Type | Protects | Ordering Rules |
|---|---|---|---|
| `kqring_proc_mtx` | `MTX_DEF` | `kqring_proc_hash`, `kqring_ctx_list`, `kqring_live_workers`, `kqring_live_zc`, `kqring_unloading` | Acquired before `ctx->mtx` or `wq->mtx` during process `exit`/`exec`/`fork` and module unload. |
| `ctx->sq_sx` | `struct sx` | Serializes `kqring_sqpoll_thread`, `KQRING_IOC_ENTER` submission/taskwork, and `KQRING_IOC_REGISTER` | Sleepable outer lock; never held by upcalls, callouts, or workers. |
| `ctx->bg_sx` | `struct sx` | `ctx->bgroups` hash, `PROVIDED_BUFFERS`, `PBUF_RING` selection/commit | Sleepable lock; nests inside `ctx->sq_sx` and outside `SOCK_BUF_LOCK` / `ctx->mtx`. |
| `SOCK_IO_SEND_LOCK` | `struct sx` | Serializes socket send operations (`kqring_send_zc_iov`, `sosend`) | Acquired before `SOCK_BUF_LOCK`, `kqring_zc_mtx`, and `ctx->mtx`. |
| `SOCK_BUF_LOCK` | `MTX_DEF` | Socket buffer state (`so_rcv` / `so_snd`) and `sb_upcall` | Acquired **before** `ctx->mtx` (matching the order in which `sowakeup` -> `kqring_soupcall` enters `kqring`). |
| `kqring_futex_mtx` | `MTX_DEF` | Global `kqring_futex_hash` table | Acquired **before** `ctx->mtx`. |
| `kqring_zc_mtx` | `MTX_DEF` | `kqring_pin_acct_hash` and `zc_notif->ctx` detachment | Acquired **before** `ctx->mtx`. |
| `ctx->mtx` | `MTX_DEF` | `taskwork_ops`, `active_ops`, `retired_ops`, `rcq`, `zc_pending`, `cq_wait`, `evfd` | Leaf lock relative to socket/futex/ZC locks; only `wq->mtx` may nest inside `ctx->mtx`. |
| `wq->mtx` | `MTX_DEF` | Worker pool queues (`q[2]`), thread lists, worker counts, `wq->refs` | Innermost mutex; never acquires `ctx->mtx` while held. |

---

## 9. Process Lifecycle & Security Boundaries

- **Process exit (`kqring_proc_exit`):** Releases `ctx->files` and `ctx->bufs` and stops workers/SQPOLL for every ring created by the exiting process. This breaks any `AF_UNIX` `SCM_RIGHTS` reference cycle where a ring descriptor is in flight inside an unread socket buffer whose endpoint is also installed in `ctx->files`.
- **Process `execve(2)` (`kqring_proc_exec`):** Marks all rings owned by the process as `ctx->closing = true`, stops `SQPOLL` and workers, disarms all in-flight operations, and frees fixed resource tables so a new executable image cannot inherit active asynchronous kernel operations or pre-execve credentials.
- **Process `fork(2)` (`kqring_proc_fork`):** Increments `ctx->cow_gen` on all rings in the parent process so any registered buffer (`IORING_RECVSEND_FIXED_BUF`) refreshes its held physical pages on the next zero-copy send after copy-on-write mappings are established.
- **Capsicum (`CAP_RIGHTS` & `IN_CAPABILITY_MODE`):**
  - Fixed-file slots snapshot the `struct filecaps` of the descriptor at registration/install time and validate every operation against those rights.
  - Direct descriptors created via `OPENAT`/`OPENAT2` inherit the directory descriptor's Capsicum rights.
  - Personalities registered prior to `cap_enter(2)` are rejected with `-ECAPMODE` once the process enters capability mode, preventing sandbox escape via `sqe->personality`.

