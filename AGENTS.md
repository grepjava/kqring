# kqring — Agent Notes

## FreeBSD Test Host

`kqring.ko` must be built and tested on FreeBSD (the Windows dev PC cannot build it).
Use the remote test machine below.

| Item | Value |
|---|---|
| Host | `192.168.100.100` (hostname `freebsd`) |
| OS | FreeBSD 15.1-RELEASE, GENERIC kernel, amd64 |
| User | `grepjava` (uid 1001, member of `wheel`) |
| Private key (Windows) | `C:/Users/grepj/.ssh/weft_linux` — **never read, print, or copy it** |
| Known hosts file | `C:/Users/grepj/.ssh/known_hosts_freebsd` (dedicated; do **not** edit the main `known_hosts`, which has a stale entry for this IP) |
| sudo | `(ALL : ALL) NOPASSWD: ALL` — `sudo -n` works non-interactively |
| Kernel sources | Installed 2026-10-21: `FreeBSD-src-sys-15.1p6` (pkgbase) |

### Connecting

Always use exactly these options (BatchMode so it never hangs on a prompt):

```powershell
ssh -o BatchMode=yes -o UserKnownHostsFile=C:/Users/grepj/.ssh/known_hosts_freebsd -i C:/Users/grepj/.ssh/weft_linux grepjava@192.168.100.100 "<command>"
```

Copying files (same options with `scp`):

```powershell
scp -o BatchMode=yes -o UserKnownHostsFile=C:/Users/grepj/.ssh/known_hosts_freebsd -i C:/Users/grepj/.ssh/weft_linux -r include kmod tests Makefile grepjava@192.168.100.100:~/kqring/
```

If host key verification fails, **stop and ask the user** — do not disable
`StrictHostKeyChecking` or rewrite known_hosts files.

### Installing kernel sources (already done; for reinstall)

The host is pkgbase-managed but the `FreeBSD-base` repo is disabled in
`/etc/pkg/FreeBSD.conf`, so select it explicitly with `-r` (no config change):

```sh
sudo pkg install -y -r FreeBSD-base FreeBSD-src-sys
# or: sudo git clone --depth 1 -b releng/15.1 https://git.FreeBSD.org/src.git /usr/src
```

### Build & test workflow

```sh
cd ~/kqring && make
sudo kldload ./kmod/kqring.ko
./tests/kqring_test
sudo kldunload kqring
```

> Loading `kqring.ko` runs code in the kernel; a bug can panic the test host.
> Confirm with the user before the first `kldload` of a new/untested build.

Run the test unbuffered so progress survives a panic:
`timeout 180 stdbuf -o0 ./tests/kqring_test 2>&1 | tee test.log`

### After a panic

- By default the host stops at `db>` and needs a manual reset. Before risky
  runs: `sudo sysctl debug.debugger_on_panic=0` (runtime only) so it dumps
  and reboots on its own.
- Dumps land in `/var/crash/vmcore.N` (no kgdb installed). Panic message +
  backtrace: `sudo dmesg -M /var/crash/vmcore.0 -N /boot/kernel/kernel`
- Map `kqring_ioctl+0xNNN` with `objdump -d kmod/kqring.ko.full` and kernel
  addresses with `nm -n /boot/kernel/kernel`.

### FreeBSD 15 KPI gotchas (already hit)

- Never use `SOCKBUF_LOCK(&so->so_rcv)`: `sb_mtx` is in a union that
  AF_UNIX stream sockets reuse (NULL deref). Use `SOCK_BUF_LOCK(so, which)`.
- Listening sockets have no `so_rcv`; use `solisten_upcall_set()` under
  `SOCK_LOCK`, never `soupcall_set()`.
- `kern_accept4()` takes 5 args and `SOCK_*` flags; `SS_CANTRCVMORE` is gone.
