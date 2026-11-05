# FreeBSD bugs found while building kqring

FreeBSD kernel bugs that kqring's development turned up: what each one is, how kqring copes with it, and where it stands upstream. Each entry has a draft report ready to file at [bugs.freebsd.org](https://bugs.freebsd.org/bugzilla/enter_bug.cgi?product=Base%20System&component=kern). Reproducers live in [`tests/bsdbugs/`](tests/bsdbugs/).

| # | Summary | Found | Upstream | kqring |
|---|---|---|---|---|
| 1 | [AF_UNIX stream `shutdown(SHUT_WR)` runs no `SO_RCV` upcall on the peer](#1-af_unix-stream-shutdownshut_wr-runs-no-so_rcv-upcall-on-the-peer) | 2026-11-04 | [PR 300494](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300494), open | Worked around (descriptors; not fixed files) |
| 2 | [`EVFILT_USER`: `NOTE_TRIGGER` given with `EV_ADD` is ignored](#2-evfilt_user-note_trigger-given-with-ev_add-is-ignored) | 2026-11-05 | Not filed | Not affected (its tests add, then trigger) |

How to add one: give it the next number, a row in the table, and a section below with the same headings. Put any reproducer in `tests/bsdbugs/<name>/`. Once it's filed, record the PR number and its URL, and keep the status up to date.

---

## 1. AF_UNIX stream `shutdown(SHUT_WR)` runs no `SO_RCV` upcall on the peer

- **Found:** 2026-11-04, by kqring's event-driven chain tests (an armed `RECV` never completed after the peer half-closed).
- **Affects:** FreeBSD 15.1-RELEASE (reproduced on 15.1-RELEASE-p6, amd64: kqring on `GENERIC` and `GENERIC-DEBUG`, the reproducer module on `GENERIC-DEBUG`). The same code is in `main` as of 2026-11-04.
- **Upstream:** [PR 300494](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=300494), filed 2026-11-04 (Base System / kern, 15.1-RELEASE, Affects Some People). Status: open. Follows up commit 52c0aa086d6b (June 2025), which fixed the same event for select, poll and kqueue but not for upcalls; the regression came with the unix/stream rewrite, d15792780760.
- **kqring workaround** (since 2026-11-04): each AF_UNIX stream socket a request waits to read from through a descriptor gets a knote on the ring's private kqueue that reports only end of input (`kqring_kpoll_soeof_watch()` in `kmod/kqring_kpoll.c`). The ring's poller, or an enter's scan, then runs kqring's upcall (`kqring_so_eof_kick()`). It's registered once per descriptor and removed by kqueue when the descriptor closes. Requests that name the socket only by a fixed-file index are still affected. Regression test: `test_unix_peer_shutdown` in `kqring_test`. README, *Differences from Linux*, *Polling*. Remove the workaround once a fixed release is the oldest one kqring supports.
- **Reproducer:** [`tests/bsdbugs/unix_shutdown_upcall/`](tests/bsdbugs/unix_shutdown_upcall/), a small kernel module that prints the result when loaded.

### Report as filed

**Product / Component:** Base System / kern
**Version:** 15.1-RELEASE (15.1-RELEASE-p6 tested; the code is unchanged in main)
**Hardware:** amd64 (not architecture-specific)
**Summary:** `unix(4)`: `shutdown(SHUT_WR)` on a stream socket doesn't run the peer's receive upcall (`soupcall_set(SO_RCV)`)

**Description:**

When one end of a connected `SOCK_STREAM` (or `SOCK_SEQPACKET`) `AF_UNIX` socket pair calls `shutdown(SHUT_WR)`, the peer's receive buffer gets `SBS_CANTRCVMORE`, and `select(2)`, `poll(2)` and kqueue waiters on the peer are woken. A receive upcall installed on the peer with `soupcall_set(so, SO_RCV, ...)` is not called, though. A consumer that waits on the upcall therefore never learns that the input ended, and waits until something else happens on the socket. Data sent by the peer does run the upcall, and so does closing the peer.

`uipc_shutdown()` handles `SHUT_WR` on a stream socket by calling `uipc_cantrcvmore()` on the peer. That function sets `SBS_CANTRCVMORE` and calls `selwakeuppri()` and `KNOTE_LOCKED()` itself, and then `uipc_wakeup_writer()` if there is a `uxst_peer`. It never goes through `sorwakeup_locked()`/`sowakeup()`, which is where `SB_UPCALL` is handled. `socantrcvmore()`, which other protocols use for the same event, goes through `sorwakeup_locked()`. The data paths in `uipc_usrreq.c` call `sorwakeup_locked()` too, so the upcall works for them.

History: the unix/stream rewrite (d15792780760) lost the read wakeup on `SHUT_WR`. Commit 52c0aa086d6b ("unix/stream: shutdown(SHUT_WR) shall trigger read event on peer", June 2025) restored it for select, poll and kqueue by adding the open-coded `selwakeuppri()` and `KNOTE_LOCKED()` to `uipc_cantrcvmore()`, but not the `SB_UPCALL` half of `sowakeup()`. So upcall consumers are still affected; this report is a follow-up to that commit.

Every in-kernel consumer of `SO_RCV` upcalls on local stream sockets is affected. We hit it with an out-of-tree io_uring-style module, and krpc's use of `AF_LOCAL` transports is a likely in-tree candidate. Userland `aio_read(2)` on such a socket does complete, so the bug isn't visible from userland directly.

**How to reproduce:**

The attached module (`unix_shutdown_upcall.c`, about 100 lines) creates a socket pair with `socreate()` and `soconnect2()`, installs a counting upcall on one end's `SO_RCV`, sends one byte from the other end, and then calls `soshutdown(SHUT_WR)` on that end. Build it with this `Makefile`:

```
KMOD=	unix_shutdown_upcall
SRCS=	unix_shutdown_upcall.c

.include <bsd.kmod.mk>
```

On 15.1-RELEASE-p6 (`GENERIC-DEBUG`):

```
# make && kldload ./unix_shutdown_upcall.ko
kldload: an error occurred while loading module ./unix_shutdown_upcall.ko. Please check dmesg(8) for more details.
# dmesg | tail -4
unix_shutdown_upcall: upcalls after data: 1 (expect >= 1)
unix_shutdown_upcall: after peer SHUT_WR: 0 new, SBS_CANTRCVMORE set
unix_shutdown_upcall: BUG: SHUT_WR set SBS_CANTRCVMORE but ran no SO_RCV upcall
module_register_init: MOD_LOAD (unix_shutdown_upcall, 0xffffffff84248000, 0) error 5
```

(The module fails its load with `EIO` when the bug is present, so it's never left loaded. It prints `OK` and loads when the bug is fixed.)

**Expected:** the peer's `SO_RCV` upcall runs once its receive side is marked `SBS_CANTRCVMORE`, as it does with `socantrcvmore()` for TCP, and as it does on `AF_UNIX` for data and for `close(2)`.

**Suggested fix:** have `uipc_cantrcvmore()` run the full receive wakeup. For example, keep its locking and replace the two lines 52c0aa086d6b added with what `sorwakeup_locked()` does, or call `sowakeup(so, SO_RCV)` there (it drops the receive buffer lock), and then take the `uipc_wakeup_writer()` step under the lock again. We haven't tested a patch.

---

## 2. `EVFILT_USER`: `NOTE_TRIGGER` given with `EV_ADD` is ignored

- **Found:** 2026-11-05, by a test of kqring's kqueue bridge that expected an event added and triggered in one `kevent(2)` call.
- **Affects:** FreeBSD 15.1-RELEASE (seen on `GENERIC`, amd64). Not checked against `main`.
- **Upstream:** not filed. It may be long-standing behaviour that programs rely on, so check `main` and other BSDs first.
- **kqring workaround:** none needed. kqring passes events through as `kevent(2)` reports them; its tests and fuzzer add an event, then trigger it in a second call.
- **Reproducer:** add with `EV_SET(&k, 2, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0, 0)`, then `kevent(2)` with a zero timeout returns 0. A second change with `flags` 0 and `NOTE_TRIGGER` makes it return 1.

**Cause:** `filt_userattach()` tests `kn->kn_fflags & NOTE_TRIGGER`, so it means to honour the flag at attach. But `kqueue_register()` stores the flags in `kn_sfflags`, clears `kev->fflags`, and then copies `kev` into `kn_kevent`, so `kn_fflags` is always 0 at that point.

**Possible fix:** test `kn->kn_sfflags` in `filt_userattach()`, or remove the check if ignoring the flag is the intended behaviour, and say which in `kqueue(2)`.
