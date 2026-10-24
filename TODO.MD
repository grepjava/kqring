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

**Target:** Zero-copy sends from unregistered application buffers and scatter/gather `msghdr.msg_iov` arrays above a copy-vs-pin threshold (`>= 4 KiB`), plus `IORING_RECVSEND_FIXED_BUF` on `IORING_OP_SENDMSG_ZC`.

### Design
1. **Per-Request Page Holding & Accounting:**
   - For `IORING_OP_SEND_ZC` and `IORING_OP_SENDMSG_ZC` without `IORING_RECVSEND_FIXED_BUF` where the transfer length is `>= KQRING_ZC_MIN_LEN` (`4096` bytes / `PAGE_SIZE`), pin and hold the backing physical pages per non-empty iovec segment via `vm_fault_quick_hold_pages(..., VM_PROT_READ, ...)` and charge them to the user's `struct kqring_pin_acct` (`RLIMIT_MEMLOCK` and `vm.max_user_wired`).
   - Payloads `< 4096` bytes (or sends where `RLIMIT_MEMLOCK` / `vm.max_user_wired` is exhausted) fall back to the copy path (`IORING_NOTIF_USAGE_ZC_COPIED` when `IORING_SEND_ZC_REPORT_USAGE` is set).
2. **Scatter/Gather (`IORING_OP_SENDMSG_ZC`):**
   - Supports both `IORING_RECVSEND_FIXED_BUF` (all `msg_iov` segments validated within `sqe->buf_index`) and unregistered `msg_iov` arrays (`struct kqring_zc_notif` tracks either a single `zbuf` or a `zbufs[]` array across segments).
   - Walks the `msghdr.msg_iov` segments, builds a single `EXT_SFBUF` mbuf chain across all segments (`M_EXT | M_RDONLY`), submits via `pr_send`, and releases all held `struct kqring_zc_buf` references inside `kqring_zc_rele()` when the final mbuf reference drops.

---

## Phase C — Zero-Copy `IORING_OP_SPLICE` (N/A — Not Planned)

**Status:** N/A (not applicable to `io_uring` `SPLICE` semantics).

### Rationale
1. **ABI Constraint:** `io_uring` `IORING_OP_SPLICE` requires at least one endpoint to be a pipe (`DTYPE_PIPE`), returning `-EINVAL` for direct `file -> socket` transfers.
2. **Existing Zero-Copy Alternatives:** Applications transferring file or `shm_open(2)` data to sockets on FreeBSD either call native `sendfile(2)` directly or pair `IORING_OP_READ_FIXED` with Phase A `IORING_OP_SEND_ZC | IORING_RECVSEND_FIXED_BUF`.

