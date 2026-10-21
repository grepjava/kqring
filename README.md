# `kqring` — CVE-Free `io_uring` (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING`) for FreeBSD

`kqring.ko` is a loadable FreeBSD kernel module implementing a zero-CVE-surface subset of the Linux `io_uring` ABI, engineered specifically for modern per-worker thread-per-core runtimes like **Vega** (`SINGLE_ISSUER | DEFER_TASKRUN` + `PBUF_RING` + `RECV_MULTISHOT`).

## Layout

- [`include/sys/kqring.h`](include/sys/kqring.h) — Dual kernel/userspace header (`io_uring_sqe`, `io_uring_cqe`, `io_uring_buf_ring`, `/dev/kqring` ioctls, and inline `kqring_setup` / `kqring_enter` / `kqring_register` wrappers).
- [`kmod/kqring.c`](kmod/kqring.c) — FreeBSD 14/15 loadable kernel module (`style(9)` C).
- [`kmod/Makefile`](kmod/Makefile) — `<bsd.kmod.mk>` build definition for `kqring.ko`.
- [`tests/kqring_test.c`](tests/kqring_test.c) — Functional verification suite (`IORING_REGISTER_PROBE`, `PBUF_RING` + `IORING_RECV_MULTISHOT` + zero-leak EOF) and 50,000-round ping-pong latency benchmark vs. `kqueue`.

---

## Security Architecture (Why Zero `io_uring` CVEs Apply)

| Linux `io_uring` CVE Class | `kqring.ko` Guarantee |
|---|---|
| **`io-wq` async worker UAFs** | No kernel worker threads exist. All SQE submissions and deferred socket re-arms run strictly in the owning thread's `sys_ioctl(KQRING_IOC_ENTER)` syscall context (`curthread->td_tid == ring->owner_tid`). |
| **`IOSQE_FIXED_FILE` reference cycles** | No fixed-file table or `unix_gc` bypass. Every SQE resolves standard file descriptors with Capsicum rights checks (`fget_read` / `fget_write`) and holds explicit `soref()` / `sorele()` references on armed sockets. |
| **Pinned user-buffer page lifetime bugs** | `PBUF_RING` descriptor pages (`struct io_uring_buf`) are wired kernel/user shared memory, while data buffers remain ordinary userspace virtual addresses copied via standard `uiomove()` inside `soreceive()` in `curthread` context. |
| **EOF buffer-ID leak on `IOSQE_BUFFER_SELECT`** | Unlike Linux `io_uring` (which consumes a `PBUF_RING` buffer ID even when `recv` returns `0` at EOF), `kqring_pbuf_commit()` only advances the kernel ring head when `transferred > 0`. |

---

## Building & Running on FreeBSD 14 / 15

Ensure kernel sources are installed in `/usr/src` (`freebsd-version -ku`), then run:

```sh
# Build both kqring.ko and tests/kqring_test
make

# Load the kernel module (creates /dev/kqring)
sudo kldload ./kmod/kqring.ko

# Run functional tests + kqring vs kqueue ping-pong benchmark
./tests/kqring_test

# Unload when finished
sudo kldunload kqring
```

---

## Wiring Vega (`Sources/CVega/cvega.c`) to `kqring`

Because [`include/sys/kqring.h`](include/sys/kqring.h) exposes the exact `struct io_uring_sqe`, `struct io_uring_cqe`, `struct io_uring_params`, `struct io_uring_buf_ring`, and `IORING_*` constants used by `cvega.c`, porting Vega's `io_uring` backend to FreeBSD requires only swapping the three syscall wrappers (`uring_setup`, `uring_enter`, `uring_register`):

```c
#if defined(__linux__)
#include <linux/io_uring.h>
/* existing syscall(__NR_io_uring_*) wrappers */
#elif defined(__FreeBSD__)
#include <sys/kqring.h>
#define uring_setup(entries, params)       kqring_setup((entries), (params))
#define uring_enter(fd, to_sub, min_c, fl) kqring_enter((fd), (to_sub), (min_c), (fl))
#define uring_register(fd, op, arg, nr)    kqring_register((fd), (op), (arg), (nr))
#endif
```
