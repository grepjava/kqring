#!/usr/bin/env python3
"""Turn kqring_bench runs into median tables and SVG charts.

usage: bench_report.py OUTDIR RUN.out [RUN.out ...]

Each RUN.out holds the "ROW <group> <variant> <ns/op>" lines of one
kqring_bench run.  Prints a median table (with the run-to-run spread) and
writes the charts used by BENCHMARK.MD and README.md into OUTDIR.  No
dependencies beyond the standard library.
"""
import math
import os
import statistics
import sys
from html import escape

# --- data -------------------------------------------------------------------

def load(paths):
    runs = {}
    for p in paths:
        for line in open(p, encoding="utf-8"):
            f = line.split()
            if len(f) == 4 and f[0] == "ROW":
                runs.setdefault((f[1], f[2]), []).append(float(f[3]))
    return {k: (statistics.median(v), (max(v) - min(v)) / statistics.median(v), len(v))
            for k, v in runs.items()}


# --- palette (validated with the dataviz validator, light and dark) ----------

STYLE = """
<style>
  .viz { --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
         --grid:#e1e0d9; --axis:#c3c2b7; --plain:#a8a69e; --kqueue:#eb6834;
         --s1:#2a78d6; --s2:#1baf7a; --s3:#eda100; --s4:#e87ba4;
         --faster:#2a78d6; --slower:#e34948; }
  @media (prefers-color-scheme: dark) {
    .viz { --surface:#1a1a19; --ink:#f0efec; --ink2:#c3c2b7; --muted:#898781;
           --grid:#2c2c2a; --axis:#383835; --plain:#6f6d67; --kqueue:#d95926;
           --s1:#3987e5; --s2:#199e70; --s3:#c98500; --s4:#d55181;
           --faster:#3987e5; --slower:#e66767; }
  }
  text { font-family: system-ui, -apple-system, "Segoe UI", sans-serif; fill: var(--ink); }
  .title { font-size: 15px; font-weight: 600; }
  .sub { font-size: 12px; fill: var(--ink2); }
  .lab { font-size: 12px; fill: var(--ink2); }
  .val { font-size: 11px; fill: var(--ink2); font-variant-numeric: tabular-nums; }
  .tick { font-size: 11px; fill: var(--muted); font-variant-numeric: tabular-nums; }
  .grid { stroke: var(--grid); stroke-width: 1; }
  .axis { stroke: var(--axis); stroke-width: 1; }
</style>
"""

FONT_W = 6.0  # rough average glyph width at 12px, for layout


def svg_open(w, h, title):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" class="viz" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}" role="img" aria-label="{escape(title)}">',
            f"<title>{escape(title)}</title>", STYLE,
            f'<rect width="{w}" height="{h}" rx="8" fill="var(--surface)"/>']


def hbar(x0, y, length, thick, color, tip):
    """A horizontal bar from x0, rounded (4px) at the data end only."""
    if length < 0.5:
        return ""
    r = min(4, length, thick / 2)
    if length >= 0:
        x1 = x0 + length
        d = (f"M{x0:.1f},{y:.1f} H{x1 - r:.1f} Q{x1:.1f},{y:.1f} {x1:.1f},{y + r:.1f} "
             f"V{y + thick - r:.1f} Q{x1:.1f},{y + thick:.1f} {x1 - r:.1f},{y + thick:.1f} "
             f"H{x0:.1f} Z")
    return f'<path d="{d}" fill="{color}"><title>{escape(tip)}</title></path>'


def hbar_left(x0, y, length, thick, color, tip):
    """A bar growing leftwards from x0, rounded at its left (data) end."""
    if length < 0.5:
        return ""
    r = min(4, length, thick / 2)
    x1 = x0 - length
    d = (f"M{x0:.1f},{y:.1f} H{x1 + r:.1f} Q{x1:.1f},{y:.1f} {x1:.1f},{y + r:.1f} "
         f"V{y + thick - r:.1f} Q{x1:.1f},{y + thick:.1f} {x1 + r:.1f},{y + thick:.1f} "
         f"H{x0:.1f} Z")
    return f'<path d="{d}" fill="{color}"><title>{escape(tip)}</title></path>'


def nice_max(v):
    exp = 10 ** math.floor(math.log10(v))
    for m in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if m * exp >= v:
            return m * exp
    return 10 * exp


def fmt_num(v, unit):
    if unit == "GB/s":
        return f"{v:.2f}"
    if v >= 10000:
        return f"{v / 1000:.1f}k"
    return f"{v:,.0f}"


# --- grouped horizontal bars --------------------------------------------------

def grouped(path, title, subtitle, groups, series, unit, data, width=760):
    """groups: [(label, group_key)]; series: [(label, variant, css color)].
    data(group_key, variant) -> value or None."""
    thick, gap, ggap = 14, 2, 18
    label_w = max(len(g[0]) for g in groups) * FONT_W + 24
    top = 78
    legend_h = 22
    plot_x = label_w
    plot_w = width - plot_x - 70
    rows = [(g, [s for s in series if data(g[1], s[1]) is not None]) for g in groups]
    height = top + legend_h + sum(len(ss) * (thick + gap) - gap + ggap for _, ss in rows) + 34
    vmax = nice_max(max(data(g[1], s[1]) for g, ss in rows for s in ss))
    out = svg_open(width, height, title)
    out.append(f'<text x="20" y="28" class="title">{escape(title)}</text>')
    out.append(f'<text x="20" y="48" class="sub">{escape(subtitle)}</text>')
    lx = 20
    for lab, _, col in series:
        out.append(f'<rect x="{lx}" y="{top - 14}" width="10" height="10" rx="2" fill="{col}"/>')
        out.append(f'<text x="{lx + 15}" y="{top - 5}" class="lab">{escape(lab)}</text>')
        lx += 15 + len(lab) * FONT_W + 18
    y = top + legend_h
    plot_top = y - 6
    for i in range(0, 6):
        gx = plot_x + plot_w * i / 5
        out.append(f'<line x1="{gx:.1f}" y1="{plot_top}" x2="{gx:.1f}" y2="{height - 30}" class="grid"/>')
        out.append(f'<text x="{gx:.1f}" y="{height - 14}" class="tick" text-anchor="middle">'
                   f'{fmt_num(vmax * i / 5, unit)}</text>')
    out.append(f'<text x="{width - 20}" y="{height - 14}" class="tick" text-anchor="end">{escape(unit)}</text>')
    for (glabel, gkey), ss in rows:
        block = len(ss) * (thick + gap) - gap
        out.append(f'<text x="{plot_x - 10}" y="{y + block / 2 + 4:.1f}" class="lab" '
                   f'text-anchor="end">{escape(glabel)}</text>')
        for lab, var, col in ss:
            v = data(gkey, var)
            ln = plot_w * v / vmax
            out.append(hbar(plot_x, y, ln, thick, col, f"{glabel} — {lab}: {fmt_num(v, unit)} {unit}"))
            out.append(f'<text x="{plot_x + ln + 5:.1f}" y="{y + thick - 3}" class="val">'
                       f'{fmt_num(v, unit)}</text>')
            y += thick + gap
        y += ggap - gap
    out.append(f'<line x1="{plot_x}" y1="{plot_top}" x2="{plot_x}" y2="{height - 30}" class="axis"/>')
    out.append("</svg>")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


# --- headline: speed-up factor on a log scale, centred on 1x -----------------

def speedup(path, title, subtitle, items, width=760):
    """items: [(label, baseline_ns, kqring_ns)]; > 1 means kqring is faster."""
    thick, step = 16, 26
    label_w = max(len(i[0]) for i in items) * FONT_W + 24
    top = 70
    plot_x0 = label_w + 10
    plot_w = width - plot_x0 - 30
    lo, hi = math.log2(1 / 6), math.log2(6)
    height = top + len(items) * step + 46

    def xof(f):
        return plot_x0 + plot_w * (math.log2(f) - lo) / (hi - lo)

    out = svg_open(width, height, title)
    out.append(f'<text x="20" y="28" class="title">{escape(title)}</text>')
    out.append(f'<text x="20" y="48" class="sub">{escape(subtitle)}</text>')
    for f, lab in ((1 / 5, "5× slower"), (1 / 2, "2× slower"), (1, "same"),
                   (2, "2× faster"), (5, "5× faster")):
        gx = xof(f)
        out.append(f'<line x1="{gx:.1f}" y1="{top - 6}" x2="{gx:.1f}" y2="{height - 34}" '
                   f'class="{"axis" if f == 1 else "grid"}"/>')
        out.append(f'<text x="{gx:.1f}" y="{height - 16}" class="tick" text-anchor="middle">{lab}</text>')
    x1 = xof(1)
    y = top
    for lab, base, kq in items:
        f = base / kq
        out.append(f'<text x="{label_w}" y="{y + thick - 3}" class="lab" text-anchor="end">{escape(lab)}</text>')
        if f >= 1:
            txt = f"{f:.2f}× faster"
            out.append(hbar(x1, y, xof(f) - x1, thick, "var(--faster)",
                            f"{lab}: {txt} ({base:,.0f} → {kq:,.0f} ns)"))
            tx, anchor = xof(f) + 6, "start"
        else:
            txt = f"{1 / f:.2f}× slower"
            out.append(hbar_left(x1, y, x1 - xof(f), thick, "var(--slower)",
                                 f"{lab}: {txt} ({base:,.0f} → {kq:,.0f} ns)"))
            tx, anchor = x1 + 6, "start"  # clear of the long bars
        out.append(f'<text x="{tx:.1f}" y="{y + thick - 3}" class="val" text-anchor="{anchor}">{txt}</text>')
        y += step
    out.append("</svg>")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


# --- main ---------------------------------------------------------------------

def main():
    outdir, paths = sys.argv[1], sys.argv[2:]
    m = load(paths)
    os.makedirs(outdir, exist_ok=True)

    print(f"{'group':<22} {'variant':<22} {'median':>10} {'spread':>7} runs")
    for (g, v), (med, spread, n) in sorted(m.items()):
        print(f"{g:<22} {v:<22} {med:>10.1f} {spread * 100:>6.1f}% {n}")

    def ns(g, v):
        return m[(g, v)][0] if (g, v) in m else None

    P, K = "var(--plain)", "var(--kqueue)"
    S1, S2, S3, S4 = "var(--s1)", "var(--s2)", "var(--s3)", "var(--s4)"

    speedup(os.path.join(outdir, "summary.svg"),
            "kqring vs kqueue: speed-up per operation",
            "Median of 5 runs; baseline is kqueue, or the plain syscall where kqueue has no role",
            [
                ("Fan-in, 1,024 sockets (multishot RECV)", ns("fanin_1024", "kqueue"), ns("fanin_1024", "kqring_multishot")),
                ("Fan-in, 16 sockets (multishot RECV)", ns("fanin_16", "kqueue"), ns("fanin_16", "kqring_multishot")),
                ("AF_UNIX 64 B, RECV into one buffer", ns("pingpong_unix_64", "kqueue"), ns("pingpong_unix_64", "kqring_recv_onebuf")),
                ("AF_UNIX 64 B, multishot RECV", ns("pingpong_unix_64", "kqueue"), ns("pingpong_unix_64", "kqring_multishot")),
                ("AF_UNIX 4 KiB, multishot RECV", ns("pingpong_unix_4096", "kqueue"), ns("pingpong_unix_4096", "kqring_multishot")),
                ("TCP 64 B, multishot RECV", ns("pingpong_tcp_64", "kqueue"), ns("pingpong_tcp_64", "kqring_multishot")),
                ("TCP accept (multishot ACCEPT)", ns("accept", "kqueue"), ns("accept", "kqring_multishot")),
                ("1 µs timer (TIMEOUT)", ns("timer_1us", "kqueue"), ns("timer_1us", "kqring_timeout")),
                ("Empty kevent vs 32 NOPs per enter", ns("floor", "kqueue_empty_kevent"), ns("floor", "kqring_nop_batch32")),
                ("AF_UNIX 256 KiB send, SEND_ZC vs write", ns("send_unix_256k", "plain_write"), ns("send_unix_256k", "kqring_send_zc_fixed")),
                ("TCP 64 KiB send, SEND_ZC vs write", ns("send_tcp_64k", "plain_write"), ns("send_tcp_64k", "kqring_send_zc_fixed")),
                ("AF_UNIX 16 KiB send, SEND_ZC vs write", ns("send_unix_16k", "plain_write"), ns("send_unix_16k", "kqring_send_zc_fixed")),
                ("AF_UNIX 64 B, RECV armed before data", ns("pingpong_unix_64", "kqueue"), ns("pingpong_unix_64", "kqring_recv_armed")),
                ("File 4 KiB, 32 READs per enter vs pread", ns("file_4k", "pread"), ns("file_4k", "kqring_read_batch32")),
                ("File 4 KiB READ vs pread", ns("file_4k", "pread"), ns("file_4k", "kqring_read")),
                ("Pipe 64 B READ, O_NONBLOCK", ns("pipe", "kqueue"), ns("pipe", "kqring_read_nonblock")),
                ("Pipe 64 B READ, blocking (worker)", ns("pipe", "kqueue"), ns("pipe", "kqring_read")),
            ])

    grouped(os.path.join(outdir, "fanin.svg"),
            "Fan-in: N sockets each get one 64 B message",
            "ns per message, lower is better",
            [("16 sockets", "fanin_16"), ("256 sockets", "fanin_256"), ("1,024 sockets", "fanin_1024")],
            [("kqueue + recv", "kqueue", K), ("kqring multishot RECV", "kqring_multishot", S1),
             ("kqring RECV batch", "kqring_recv_batch", S2)],
            "ns", ns)

    pp = [("AF_UNIX 64 B", "pingpong_unix_64"), ("AF_UNIX 1 KiB", "pingpong_unix_1024"),
          ("AF_UNIX 4 KiB", "pingpong_unix_4096")]
    grouped(os.path.join(outdir, "pingpong.svg"),
            "Ping-pong: one message written, then received",
            "ns per message, lower is better; the no-wait write + recv floor in gray",
            pp,
            [("write + recv floor", "plain_recv", P), ("kqueue", "kqueue", K),
             ("kqring RECV, one buffer", "kqring_recv_onebuf", S1),
             ("kqring multishot", "kqring_multishot", S2),
             ("kqring RECV armed", "kqring_recv_armed", S3)],
            "ns", ns)

    def gbps(g, v):
        t = ns(g, v)
        if t is None:
            return None
        size = int(g.rsplit("_", 1)[1].rstrip("k")) * 1024
        return size / t  # bytes per ns == GB/s

    sizes = [("16 KiB", "16k"), ("32 KiB", "32k"), ("64 KiB", "64k"), ("256 KiB", "256k")]
    send_series = [("write(2)", "plain_write", P), ("kqring SEND", "kqring_send", K),
                   ("SEND_ZC, registered buffer", "kqring_send_zc_fixed", S1),
                   ("SEND_ZC, unregistered", "kqring_send_zc", S2)]
    for tr, name in (("unix", "AF_UNIX"), ("tcp", "TCP loopback")):
        grouped(os.path.join(outdir, f"send_{tr}.svg"),
                f"Bulk send, {name}: send a message, read it back in full",
                "GB/s, higher is better",
                [(lab, f"send_{tr}_{k}") for lab, k in sizes],
                send_series, "GB/s", gbps)

    # Series are roles; each group maps a role to its own variant.
    roles = {
        "pipe": {"plain": "plain_read", "kqueue": "kqueue", "inline": "kqring_read_nonblock",
                 "worker": "kqring_read"},
        "file_4k": {"plain": "pread", "inline": "kqring_read", "batch": "kqring_read_batch32"},
        "floor": {"kqueue": "kqueue_empty_kevent", "inline": "kqring_nop",
                  "worker": "kqring_nop_async"},
    }
    grouped(os.path.join(outdir, "worker.svg"),
            "Pipes and files: inline vs worker thread",
            "ns per operation, lower is better; a blocking pipe still goes through a worker",
            [("Pipe READ, 64 B", "pipe"), ("File READ, 4 KiB", "file_4k"), ("Empty op (NOP)", "floor")],
            [("read(2) / pread(2)", "plain", P), ("kqueue", "kqueue", K),
             ("kqring, inline", "inline", S1), ("kqring, via worker", "worker", S2),
             ("kqring, inline, 32 per enter", "batch", S3)],
            "ns", lambda g, r: ns(g, roles[g][r]) if r in roles[g] else None)


if __name__ == "__main__":
    main()
