# OPENAT2's resolve flags

Status: implemented, November 2026. Roadmap phase 15.

Linux's `openat2(2)` takes a `struct open_how` whose `resolve` field limits how the path is looked up. kqring's `IORING_OP_OPENAT2` takes all of Linux 7.3's flags:

| Flag | What it does | How kqring does it |
|---|---|---|
| `RESOLVE_BENEATH` | The lookup must stay below the directory: `..` past it, an absolute path or an absolute symlink fails with `EXDEV`. | Alone, FreeBSD's `O_RESOLVE_BENEATH`, whose `ENOTCAPABLE` becomes Linux's `EXDEV`. With the flags below, the walk. |
| `RESOLVE_IN_ROOT` | The directory is the root: `/` and `..` stop there, for the path and for every symlink in it. Not with `RESOLVE_BENEATH` (`EINVAL`). | The walk. |
| `RESOLVE_NO_XDEV` | No mount point may be crossed, down, up through `..`, or by a symlink to `/` on another mount (`EXDEV`). A path may still start at `/`. | The walk. |
| `RESOLVE_NO_SYMLINKS` | No symlink may be followed, anywhere in the path (`ELOOP`). | The walk. |
| `RESOLVE_NO_MAGICLINKS` | No `/proc` magic links. | Nothing: FreeBSD has none. |
| `RESOLVE_CACHED` | Fail with `EAGAIN` rather than block; always with `O_CREAT` or `O_TRUNC`. | A normal lookup, and `EAGAIN` with `O_CREAT` or `O_TRUNC`. |

## The walk

FreeBSD's `namei()` takes its root from the process (`pwd_rdir`), and can't refuse a symlink or a mount point along the way, so `kqring_open_walk()` in `kmod/kqring_file.c` walks the path itself, a component at a time:

- Each component is looked up with `namei()` from the directory reached so far (`NDINIT_ATVP`), without following it. A mount point is crossed by that lookup, and `RESOLVE_NO_XDEV` then compares the mounts.
- A symlink is read (`VOP_READLINK`, after the MAC check), and its target goes in front of what is left of the path; an absolute one starts again at the root. A trailing symlink is followed unless `O_NOFOLLOW` or `O_CREAT | O_EXCL` says not to, as Linux. After 40 symlinks, Linux's limit, the open fails with `ELOOP`.
- `..` goes back to the directory the walk came from, kept on a stack, and stops at the root. Only past the starting directory of a walk that isn't scoped does it look up the real parent.
- Search permission is checked on each directory, for `.` and `..` too, as Linux's `may_lookup()`.
- The last component is opened from its directory with `vn_open_cred()`, as `openat(2)` opens it, without following a symlink: the walk has followed those it should. A file it creates gets the process's umask. Into the descriptor table, the file gets the directory descriptor's Capsicum rights if they are limited; into a fixed slot, as other opens.

## Differences from Linux

- **`..` and renames.** Linux takes the real parent for `..`, and fails a scoped lookup with `EAGAIN` if a rename or mount meanwhile might have moved it above the root, so that the caller retries. The walk's `..` goes back the way it came, so a directory renamed meanwhile is left as if it hadn't moved, and the walk can't leave its root.
- **A symlink put in place meanwhile.** The last component is opened without following a symlink. If one replaced it after the walk looked, the open fails (FreeBSD's `EMLINK`, `ELOOP` for a symlink in the middle) where Linux would follow it.
- **Capability mode.** In Capsicum's capability mode, `AT_FDCWD` fails with `ECAPMODE`, and a walk that isn't scoped is held beneath its start, failing with `ENOTCAPABLE`, as `namei()` holds any lookup there.

## Tests

- `tests/kqring_diff.c` case `openat2_resolve` opens 18 paths, with relative, absolute, upward, dangling and looping symlinks, under 8 combinations of flags, then creates through symlinks, and crosses `/dev`'s mount from `/`. Its 161 lines are the same on kqring and Linux 7.3.
- `tests/kqring_test.c`'s `test_openat2_resolve` checks the main cases, and an open into a fixed slot.
- liburing's `cbpf_filter` now opens with `RESOLVE_IN_ROOT` itself.
