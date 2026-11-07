# Differential conformance results

Raw output of [`tests/kqring_diff.c`](../../tests/kqring_diff.c) on Linux and on kqring, from the same source. To compare them:

```sh
python3 tests/diff_compare.py docs/conformance/linux.out docs/conformance/kqring.out
```

Each file repeated exactly over three runs. Of 19 cases, 15 match and 4 differ on purpose, with the reasons in [`tests/diff_expected.txt`](../../tests/diff_expected.txt).

| | Linux | kqring |
|---|---|---|
| Kernel | 7.3.0-9-generic, Ubuntu 26.10 | FreeBSD 15.1-RELEASE, GENERIC, `releng/15.1-n283562-96841ea08dcf` |
| Machine | i5-8250U, amd64 (the same machine, dual-booted) | as Linux |
| Compiler | GCC 15.3.0, `cc -O2 -pthread` | FreeBSD Clang 19.1.7, `make -C tests kqring_diff` |
| File system | ext4 | ZFS |
| `RLIMIT_MEMLOCK` | 8 MiB | 64 KiB |

kqring's baseline is Linux 6.18, but this Linux has 7.3. Every behaviour that was in dispute was also read in the v6.18 source, with one exception: `SINGLE_ISSUER`'s `io_allowed_run_tw()`, whose body wasn't checked there, so the wait rule for `DEFER_TASKRUN` rests on the 7.3 run.
