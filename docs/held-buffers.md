# Registered buffers on held pages

Status: design, November 2026. Roadmap phase 9. It turns the roadmap's research item "registered-buffer mapping independence" into a plan, and lifts the limit [concurrent-submitters.md](concurrent-submitters.md) left on registered buffers.

## What is still different

`IORING_REGISTER_BUFFERS` already holds a buffer's pages (`vm_fault_quick_hold_pages()`, charged to `RLIMIT_MEMLOCK` and `vm.max_user_wired`), but only zero-copy sends use them. `READ_FIXED`, `WRITE_FIXED`, `READV_FIXED` and `WRITEV_FIXED` copy through the buffer's user addresses in the process that runs the request, as an unregistered `READ` would. That leaves five differences from Linux, each shown by a case of `tests/kqring_diff.c`:

1. **Other processes can't use a buffer.** A request from any process but the registering one fails with `-EFAULT`, since the addresses mean nothing in its address space (`buffer_fork`: Linux reads into the registering process's memory).
2. **The buffer follows the mapping, not the pages.** After `munmap(2)` a fixed request fails with `-EFAULT`, and after a new mapping at the same address it uses the new memory (`buffer_lifetime`). Linux keeps using the pages it pinned.
3. **`fork(2)` splits the buffer from the memory.** The held pages become copy-on-write between parent and child, and the parent's next write gives it a new page. Zero-copy sends re-hold the pages when a fork has happened (`ctx->cow_gen`); other fixed requests don't need to, since they go through the addresses.
4. **Shared file mappings are accepted** (`buffer_backing`). Linux refuses them with `-EFAULT`, because a filesystem can't track writes made through pinned page-cache pages.
5. **A buffer is at most 16 MiB,** the zero-copy hold's limit, where the registration check and Linux allow 1 GiB; a larger one fails with `ENOMEM`.

Each fixed request also pays what an unregistered one pays to reach the memory: `copyin`/`copyout` with their fault handling, and for a direct read from a disk, `vm_fault_quick_hold_pages()` and its release per request. Saving that is what registered buffers are for on Linux.

## How Linux does it

- **Registration pins the pages** (`pin_user_pages(FOLL_WRITE | FOLL_LONGTERM)`), charged to the user's `RLIMIT_MEMLOCK`, and keeps them as a `bio_vec` array. A buffer is up to 1 GiB. File-backed shared mappings are refused (`FOLL_LONGTERM` on page cache), as are read-only ones (`FOLL_WRITE`); anonymous memory, private file mappings, shmem and memfd are accepted.
- **I/O goes through the pages:** a fixed request builds an `iov_iter` of type `ITER_BVEC` over the pinned pages. The submitter's address space plays no part, so any task holding the ring uses any buffer, the mapping can go away, and a new mapping at the address changes nothing.
- **`fork(2)` copies pinned pages to the child at once** (`page_maybe_dma_pinned()` in `copy_present_page()`), so the parent keeps the pinned pages and its memory stays the buffer. Measured on Linux 7.3 (`buffer_fork`): a child's `READ_FIXED` lands in the parent's memory, the child's own copy stays as it was, and the parent's writes after the fork go into the buffer.

## Design

### 1. A kernel mapping of each buffer

At registration, after holding the pages, kqring maps them contiguously in kernel address space (`kva_alloc()` and `pmap_qenter()`), and keeps the address with the held pages (`struct kqring_zc_buf`, which becomes the buffer's page set, `kva`). A fixed request then addresses the buffer at `kva + (addr - r->addr)`, with `UIO_SYSSPACE`:

- **Files, pipes and sockets** take a kernel `uio` with one `iovec` per user `iovec`, as they take a user one. `vn_io_fault()`, which exists to fault user pages without holding the vnode lock, has nothing to do.
- **Direct reads and writes on a disk** (`kqring_file_bio()`) give the bio the held pages (`bio_ma`) instead of holding the user's pages per request.
- **Zero-copy sends** use the held pages, as now.

A contiguous mapping keeps every path a single `iovec` per user `iovec`, with no per-request work. `iovec`s per page through the direct map would avoid the kernel address space but cost an array per request; kernel address space is plentiful on amd64 and arm64, and a buffer's mapping is counted against nothing else, as its pages are already charged. The mapping goes when the last reference to the page set goes.

The pages are dirtied when held with `VM_PROT_WRITE` (`vm_fault_hold_pages()` dirties them for this reason: writes through another mapping aren't seen by the pmap), and a held page is not laundered, so writes through the kernel mapping are kept. Registration asks for write access, as Linux's `FOLL_WRITE` does, and fails on read-only memory, as now.

### 2. What may be registered

Registration refuses pages whose object is a vnode's: a shared file mapping, whose writes the filesystem couldn't see or order against truncation, `-EFAULT` as on Linux. A private file mapping is held for writing, which gives it private anonymous copies first, so it is accepted, as on Linux. Anonymous memory, shared anonymous memory, `shm_open(2)` and `memfd_create(2)` objects are swap-backed and accepted.

The per-buffer limit becomes 1 GiB, as the registration check and Linux have it. The total stays bounded by `RLIMIT_MEMLOCK` and `vm.max_user_wired`, as now.

### 3. Any process, any mapping

With requests addressing the pages, `r->owner_id` and its `-EFAULT` go: a buffer belongs to the ring, as on Linux. A request still names its buffer by user address, which is checked to lie inside the registered range, as now; that address is only an offset into the buffer, whatever the submitter has mapped there. `munmap(2)` or a new mapping over the range changes nothing until the buffer is unregistered.

`IORING_REGISTER_CLONE_BUFFERS` shares the page set between rings, as now, with no re-tagging, so it works between rings of different processes.

### 4. `fork(2)`

FreeBSD makes a forked child share the parent's private memory copy-on-write, and the parent's first write moves it to a new page. Linux copies pinned pages to the child instead. Two ways to get Linux's result were considered and rejected:

- **Wiring the map range,** which makes `vm_map_copy_entry()` copy wired, writable entries to the child at once. System wiring (`VM_MAP_WIRE_SYSTEM`, as `vslock()` does) makes `munmap(2)` of the range wait until it is unwired, so a program that unmaps a registered buffer, as Linux allows, would hang. User wiring (`VM_MAP_WIRE_USER`) is one flag per map entry, shared with `mlock(2)`: unregistering would undo the program's own `mlock(2)`, and its `munlock(2)` would undo ours.
- **Sharing the range with the child** (`VM_INHERIT_SHARE`), which would turn a private mapping into one the child writes into the parent's memory through.

Instead, the `process_fork` handler, which runs in the parent after its address space is copied and before the child runs, re-holds each buffer the forking process registered, for writing. The write fault gives the parent a private copy of each page now, and the buffer then holds the parent's pages, as on Linux, while the child keeps the old ones. A buffer belongs to the address space it was registered from (the page set records the `vmspace`, cleared when that process exits or execs, so a freed address space is never matched); a fork by any other process changes nothing. A re-hold that fails (the range was unmapped or made read-only since) keeps the old pages, which the parent no longer maps writable, so nothing diverges. This replaces `ctx->cow_gen` for registered buffers; provided buffer rings and the registered wait region keep it.

What this doesn't match: a fixed read in flight while its process forks completes into the old pages, which are now the child's, so the parent doesn't see its data. On Linux it lands in the parent's. Waiting in `fork(2)` for such requests isn't an option, as a receive may wait indefinitely. It is listed as a difference.

### 5. Exit and exec

The pages stay held until the buffer is unregistered or the ring closes, whatever happens to the process that registered them, as on Linux. Exit and exec only clear the page set's `vmspace`, so that a later fork can't match it.

## Costs

- **Kernel address space:** as much as is registered, at most what `RLIMIT_MEMLOCK` allows each user.
- **`fork(2)`:** a process with registered buffers copies them at fork, page by page, as Linux does.
- **Registration:** `pmap_qenter()` over the pages, once.
- **Requests:** none added. A fixed request no longer faults through user mappings, and a direct disk request no longer holds and releases pages.

## Security

- **Any process holding the ring reads and writes every registered buffer,** which is Linux's rule. A ring is shared only by inheriting or passing its descriptor, and a ring can't be passed over a socket.
- **Shared file mappings are refused,** so no write reaches page-cache pages behind the filesystem's back.
- **Held memory stays charged** to the registering user until it is released, also after `munmap(2)`.
- **A set-ID `exec` closes the rings the process holds,** as now, so a new image never shares buffers with a ring it didn't open.

## Order of work

1. **The kernel mapping:** map the held pages, route fixed reads and writes through `UIO_SYSSPACE`, drop `owner_id`, refuse vnode-backed pages, allow 1 GiB. `buffer_lifetime`, `buffer_backing` and the cross-process half of `buffer_fork` then match Linux.
2. **Fork:** re-hold in the `process_fork` handler. `buffer_fork` matches Linux.
3. **Disk:** direct disk requests from the held pages, measured against today's hold per request.
4. **Fuzzer and tests:** fixed requests from forked children, fork with fixed requests in flight, unregister and `munmap(2)` while requests run.

## Measure of success

`buffer_lifetime`, `buffer_fork` and `buffer_backing` match Linux 7.3; liburing's fixed-buffer tests pass as before or better; the unit tests, stress and fuzzer pass; single-process performance is unchanged, and direct reads into fixed buffers on a disk are faster or level; what still differs (the read in flight during a fork) is in the matrix.
