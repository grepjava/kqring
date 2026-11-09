#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Turn fio/bench.sh and fio/scale.sh output into docs/bench/data/fio.csv.

    python3 fio/summary.py <bench.sh out dir> <scale.sh out dir> > fio.csv

Every value is the median of the repetitions.  Rows are
section,series,x,value, as tests/bench_report.py and tests/bench_doc.py read
them; series is the engine as BENCHMARK.MD names it (kqring for fio's
io_uring engine, aio for posixaio, psync), and CPU is fio's user plus system
time, in percent of one CPU.  The liburing rows (test counts) are carried
over from the current fio.csv, or the i5-8250U's, since they don't come
from fio.
"""
import csv
import glob
import json
import os
import re
import statistics
import sys

ENG = {"io_uring": "kqring", "posixaio": "aio", "psync": "psync"}


def runs(d, pattern):
    """{key tuple: [(iops, cpu, p99 us, MB/s), ...]} for files matching."""
    out = {}
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        m = re.fullmatch(pattern, os.path.basename(f))
        if not m:
            continue
        j = json.load(open(f))
        jobs = j["jobs"]
        rw = "write" if jobs[0]["write"]["iops"] > jobs[0]["read"]["iops"] \
            else "read"
        iops = sum(x[rw]["iops"] for x in jobs)
        mbs = sum(x[rw]["bw_bytes"] for x in jobs) / 1e6
        cpu = sum(x["usr_cpu"] + x["sys_cpu"] for x in jobs)
        p99 = jobs[0][rw]["clat_ns"]["percentile"].get("99.000000", 0) / 1e3
        out.setdefault(m.groups(), []).append((iops, cpu, p99, mbs))
    return out


def med(v, i):
    return statistics.median(x[i] for x in v)


def main():
    bench, scale = sys.argv[1], sys.argv[2]
    w = csv.writer(sys.stdout, lineterminator="\n")
    w.writerow(["section", "series", "x", "value"])

    s = runs(scale, r"(mdread|zfscached)\.(\w+)\.j(\d+)\.r\d+\.json")
    for name, sec in (("mdread", "md"), ("zfscached", "cached")):
        keys = sorted((k for k in s if k[0] == name),
                      key=lambda k: (list(ENG).index(k[1]), int(k[2])))
        for k in keys:
            w.writerow([sec, ENG[k[1]], k[2], round(med(s[k], 0) / 1000)])

    b = runs(bench, r"(\w+)\.(\w+)\.q(\d+)\.r\d+\.json")

    def get(name, eng, qd, i):
        v = b.get((name, eng, str(qd)))
        return med(v, i) if v else None

    for label, name in (("Raw disk reads", "rawread4k"),
                        ("ZFS file reads", "zfsread4k"),
                        ("ZFS file writes", "zfswrite4k")):
        for eng in ("io_uring", "posixaio"):
            w.writerow(["cpu", ENG[eng], label, round(get(name, eng, 32, 1))])
    for eng in ("io_uring", "posixaio", "psync"):
        for qd in (1, 8, 32):
            w.writerow(["raw", ENG[eng], qd, round(get("rawread4k", eng, qd, 0))])
            w.writerow(["rawcpu", ENG[eng], qd,
                        round(get("rawread4k", eng, qd, 1))])
    w.writerow(["raw_fixed", "kqring", 32,
                round(get("rawread4k_fixed", "io_uring", 32, 0))])
    w.writerow(["raw_sqpoll", "kqring", 32,
                round(get("rawread4k_sqpoll", "io_uring", 32, 0))])
    for eng in ("io_uring", "posixaio"):
        w.writerow(["seq", ENG[eng], "MB/s",
                    round(get("rawseq128k", eng, 32, 3))])
        w.writerow(["seq", ENG[eng], "cpu",
                    round(get("rawseq128k", eng, 32, 1))])
    for name, sec in (("zfsread4k", "zfsread"), ("zfswrite4k", "zfswrite")):
        for eng in ("io_uring", "posixaio", "psync"):
            for qd in (1, 32):
                w.writerow([sec, ENG[eng], qd, round(get(name, eng, qd, 0))])
                w.writerow([sec + "cpu", ENG[eng], qd,
                            round(get(name, eng, qd, 1))])
                w.writerow([sec + "p99", ENG[eng], qd,
                            round(get(name, eng, qd, 2))])
                # How far the repetitions are apart, in percent of the
                # median: ZFS writes vary much more than reads.
                v = [x[0] for x in b[(name, eng, str(qd))]]
                w.writerow([sec + "spread", ENG[eng], qd,
                            round((max(v) - min(v)) / statistics.median(v)
                                  * 100)])

    # The output usually replaces data/fio.csv, which the shell has emptied
    # by now, so take the rows from the first file that still has them.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for d in ("data", "data-i5-8250u"):
        p = os.path.join(root, "docs", "bench", d, "fio.csv")
        lib = [r for r in csv.DictReader(open(p, encoding="utf-8"))
               if r["section"] == "liburing"] if os.path.exists(p) else []
        if lib:
            break
    else:
        sys.exit("summary.py: no liburing rows in docs/bench/data*/fio.csv")
    for r in lib:
        w.writerow([r["section"], r["series"], r["x"], r["value"]])


if __name__ == "__main__":
    main()
