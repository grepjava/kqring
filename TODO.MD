# kqring Zero-Copy Roadmap

This document tracks the phased implementation of genuine zero-copy transmit paths in `kqring` on FreeBSD 15.

---

## Phase A — `IORING_OP_SEND_ZC` with `IORING_RECVSEND_FIXED_BUF` (Implemented)

**Target:** Registered buffer zero-copy stream socket sends (`IORING_OP_SEND_ZC` + `IORING_RECVSEND_FIXED_BUF`).

### Design
1. **Page Holding & `RLIMIT_MEMLOCK` Accounting at Registration (`IORING_REGISTER_BUFFERS` / `BUFFERS2` / `BUFFERS_UPDATE` / `CLONE_BUFFERS`):**
   - For each registered buffer (up to `KQRING_MAX_ZC_BUF_LEN` = 16 MiB per buffer), pin and hold the backing physical pages via `vm_fault_quick_hold_pages(&td->td_proc->p_vmspace->vm_map, addr, len, VM_PROT_READ | VM_PROT_WRITE, zb->pages, npages)`.
   - Charge pinned bytes to a per-user `struct kqring_pin_acct` (keyed by `td->td_ucred->cr_ruidinfo` with `uihold`) against `lim_cur(td, RLIMIT_MEMLOCK)` and reserve pages in `vm_user_wire_count` against `vm_page_max_user_wired` (`vm.max_user_wired`), failing registration with `ENOMEM` when exceeded.
   - Store the held page array and accounting pointer in a reference-counted `struct kqring_zc_buf`. Pages remain held and charged until the final reference on `struct kqring_zc_buf` drops (both the buffer table slot and all in-flight `EXT_SFBUF` mbufs have released it).
   - Track `process_fork` via an eventhandler (`ctx->cow_gen`) so `SEND_ZC` refreshes copy-on-write pages in the parent before building `EXT_SFBUF` mbufs.

2. **External `EXT_SFBUF` Mbuf Construction:**
   - When `IORING_OP_SEND_ZC` is submitted with `IORING_RECVSEND_FIXED_BUF` on a connected stream socket (`SOCK_STREAM`, no `MSG_OOB`, no destination address override) and the selected registered buffer has held pages:
     - Check available socket send buffer space under `SOCK_SENDBUF_LOCK(so)`. If `sbspace(&so->so_snd) <= 0` (or below `sb_lowat`), return `EAGAIN` so the operation arms on `so_snd` upcall as usual.
     - Build a read-only external mbuf chain (`M_EXT | M_RDONLY`, `EXT_SFBUF`, `EXT_FLAG_EMBREF`) over the held `vm_page_t` array using `sf_buf_alloc(pg, SFB_DEFAULT)` and `sf_buf_kva(sf)`.
     - Attach a custom `m_ext.ext_free` callback (`kqring_zc_mext_free`) with `ext_arg1 = zc` (`struct kqring_zc_notif *`) and `ext_arg2 = sf`.
     - Submit the mbuf chain directly via `sosend(so, NULL, NULL, top, NULL, flags, td)`.

3. **Two-Stage Completion (`IORING_CQE_F_MORE` -> `IORING_CQE_F_NOTIF`):**
   - **Stage 1 (Primary CQE):** Posted immediately when `sosend()` accepts the mbuf chain, with `res = bytes_sent` and `flags = IORING_CQE_F_MORE`.
   - **Stage 2 (Notification CQE):** Each attached page mbuf holds a reference on `struct kqring_zc_notif` (which in turn holds references on `struct kqring_rsrc`, `struct kqring_ctx`, and `kqring_live_workers`). When the network stack or peer socket releases the final mbuf reference (including any TCP clones created by `m_copym`), `kqring_zc_mext_free` drops the last reference on `struct kqring_zc_notif`, queues the `IORING_CQE_F_NOTIF` CQE (`res = 0`), and wakes the ring.
   - **Fallback Path:** Requests without `IORING_RECVSEND_FIXED_BUF`, datagram sends with destination addresses, `MSG_OOB`, or buffers exceeding the hold limit fall back to standard kernel copy and post `IORING_CQE_F_NOTIF` with `res = IORING_NOTIF_USAGE_ZC_COPIED` when `IORING_SEND_ZC_REPORT_USAGE` is set.

---

## Phase B — Arbitrary User-Buffer `IORING_OP_SEND_ZC` / `IORING_OP_SENDMSG_ZC` (Implemented)

**Target:** Zero-copy sends from unregistered application buffers and scatter/gather `msghdr.msg_iov` arrays above a copy-vs-pin threshold (`>= 4 KiB`; on `AF_UNIX`, `kern.kqring.zc_local_min_len`, 32 KiB by default), plus `IORING_RECVSEND_FIXED_BUF` on `IORING_OP_SENDMSG_ZC`.

### Design
1. **Per-Request Page Holding & Accounting:**
   - For `IORING_OP_SEND_ZC` and `IORING_OP_SENDMSG_ZC` without `IORING_RECVSEND_FIXED_BUF` where the transfer length is `>= KQRING_ZC_MIN_LEN` (`4096` bytes / `PAGE_SIZE`), pin and hold the backing physical pages per non-empty iovec segment via `vm_fault_quick_hold_pages(..., VM_PROT_READ, ...)` and charge them to the user's `struct kqring_pin_acct` (`RLIMIT_MEMLOCK` and `vm.max_user_wired`).
   - Payloads `< 4096` bytes (on `AF_UNIX`, below `kern.kqring.zc_local_min_len`), or sends where `RLIMIT_MEMLOCK` / `vm.max_user_wired` is exhausted, fall back to the copy path (`IORING_NOTIF_USAGE_ZC_COPIED` when `IORING_SEND_ZC_REPORT_USAGE` is set).
2. **Scatter/Gather (`IORING_OP_SENDMSG_ZC`):**
   - Supports both `IORING_RECVSEND_FIXED_BUF` (all `msg_iov` segments validated within `sqe->buf_index`) and unregistered `msg_iov` arrays (`struct kqring_zc_notif` tracks either a single `zbuf` or a `zbufs[]` array across segments).
   - Walks the `msghdr.msg_iov` segments, builds a single `EXT_SFBUF` mbuf chain across all segments (`M_EXT | M_RDONLY`), submits via `pr_send`, and releases all held `struct kqring_zc_buf` references inside `kqring_zc_rele()` when the final mbuf reference drops.

---

## Phase C — Zero-Copy `IORING_OP_SPLICE` (N/A — Not Planned)

**Status:** N/A (not applicable to `io_uring` `SPLICE` semantics).

### Rationale
1. **ABI Constraint:** `io_uring` `IORING_OP_SPLICE` requires at least one endpoint to be a pipe (`DTYPE_PIPE`), returning `-EINVAL` for direct `file -> socket` transfers.
2. **Existing Zero-Copy Alternatives:** Applications transferring file or `shm_open(2)` data to sockets on FreeBSD either call native `sendfile(2)` directly or pair `IORING_OP_READ_FIXED` with Phase A `IORING_OP_SEND_ZC | IORING_RECVSEND_FIXED_BUF`.


---

## Measured Results and Follow-ups

Measured on FreeBSD 15.1 (i5-8250U), as medians of 5 runs; see [`BENCHMARK.MD`](BENCHMARK.MD#bulk-send-one-message-sent-then-read-back-in-full).

| Message | AF_UNIX, registered | AF_UNIX, unregistered | TCP loopback, registered |
|---|---:|---:|---:|
| 16 KiB | 13% slower | 20% slower (copied) | 2% faster |
| 32 KiB | 5% faster | 9% slower | 9% slower |
| 64 KiB | 17% faster | 3% faster | 15% faster |
| 256 KiB | 37% faster | 27% faster | 23% faster |

All against `write(2)`. At 16 KiB on AF_UNIX every kqring send, copying `SEND` included, is 12–20% slower than `write(2)`: that's the ring's fixed cost per operation, not zero-copy.

- **AF_UNIX zero-copy threshold.** Done. `SEND_ZC` / `SENDMSG_ZC` from an unregistered buffer on AF_UNIX copies below `sysctl kern.kqring.zc_local_min_len` (32 KiB by default, the measured crossover), because pinning per send costs more than the `memcpy` there. Registered buffers and TCP go zero-copy from one page.
- **Carry several pages per mbuf.** Each zero-copy page gets its own mbuf and `sf_buf`. Unmapped `M_EXTPG` mbufs, as `sendfile(2)` and KTLS use, would cut the per-page cost. Check that AF_UNIX accepts them on 15.1 first.
- **Measure TCP zero-copy with the receiver in its own thread.** Loopback delivers through the netisr thread, so a single-threaded send-then-receive benchmark mostly measures that hand-off for small sends.
- **Document `RLIMIT_MEMLOCK`.** FreeBSD's default is 64 KiB, so registering buffers for zero-copy needs `memorylocked` raised in `login.conf(5)`. Done in [`README.md`](README.md) and `BENCHMARK.MD`.
- **Inline non-socket I/O.** Done for positioned page-cache reads (UFS, tmpfs) and `O_NONBLOCK` pipes; see [`BENCHMARK.MD`](BENCHMARK.MD#pipes-and-files-inline-or-through-a-worker). Blocking pipes still need a worker, because `pipe_read()` has no per-call non-blocking mode. Inline file reads at the file position (`off` = −1) would need the file offset lock (`foffset_lock`).
