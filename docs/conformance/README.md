# Differential conformance results

Raw output of [`tests/kqring_diff.c`](../../tests/kqring_diff.c) on Linux and on kqring, from the same source. To compare them:

```sh
python3 tests/diff_compare.py docs/conformance/linux.out docs/conformance/kqring.out
```

Each file repeated exactly over three runs, on 14 November 2026 with `kqring.ko` from the commit that added the restriction and filter cases (ROADMAP phase 14). Of 33 cases, 28 match and 5 differ on purpose, with the reasons in [`tests/diff_expected.txt`](../../tests/diff_expected.txt).

| | Linux | kqring |
|---|---|---|
| Kernel | 7.3.0-10-generic, Ubuntu 26.10 | FreeBSD 15.1-RELEASE, GENERIC, `releng/15.1-n283562-96841ea08dcf` |
| Machine | Intel i5-8250U, amd64 | AMD Ryzen 3 PRO 4350G, amd64 |
| Compiler | GCC 15.3.0, `cc -O2 -pthread` | FreeBSD Clang 19.1.7, `make -C tests kqring_diff` |
| File system | ext4 | ZFS |
| `RLIMIT_MEMLOCK` | 8 MiB | 8 MiB (`ulimit -l 8192`) |

Linux 7.3 is kqring's baseline, so these results compare it with the version it's measured against.
