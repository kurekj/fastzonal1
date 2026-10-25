#!/usr/bin/env python3
"""Turn raw benchmark CSVs into the tables, figures and summary the manuscript inputs.

    python make_paper_assets.py --raw benchmarks/results/raw_*.csv --out paper_assets/

Every number in the paper comes from summary.json; the LaTeX tables are \\input by the manuscript.
A "point" is one (fixture, op, rung, threads, zones, raster, den) combination; its reported time is
the median of the repetitions, with min and max. Speedups divide the baseline median by the
point's median for the same workload. Checksums of every point are compared with the baseline's
checksum for the same workload (relative 1e-6); mismatches are listed, never silently dropped.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

COLUMNS = ["fixture", "op", "rung", "threads", "rep", "seconds", "checksum", "zones", "raster", "den", "n_zones",
           "n_cells"]
BASELINE = "csharp-baseline"
RUNG_ORDER = ["csharp-baseline", "csharp-optimised", "fastzonal-serial", "fastzonal-openmp", "fastzonal-cuda"]


def workload_key(r):
    return "|".join([r["fixture"], r["op"], r["zones"], r["raster"], r["den"]])


def point_key(r):
    return "|".join([r["fixture"], r["op"], r["rung"], r["threads"], r["zones"], r["raster"], r["den"]])


def read_raw(paths):
    rows = []
    for path in paths:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            missing = [c for c in COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                sys.exit(f"{path}: missing columns {missing}")
            for r in reader:
                r["threads"] = str(int(r["threads"]))
                r["seconds_f"] = float(r["seconds"])
                r["checksum_f"] = float(r["checksum"])
                rows.append(r)
    return rows


def summarise(rows):
    groups = defaultdict(list)
    meta = {}
    for r in rows:
        k = point_key(r)
        groups[k].append(r)
        meta[k] = r
    points = {}
    for k, rs in groups.items():
        secs = [r["seconds_f"] for r in rs]
        r0 = meta[k]
        points[k] = {
            "fixture": r0["fixture"], "op": r0["op"], "rung": r0["rung"], "threads": int(r0["threads"]),
            "zones": r0["zones"], "raster": r0["raster"], "den": r0["den"],
            "n_zones": int(r0["n_zones"]), "n_cells": int(r0["n_cells"]),
            "n": len(secs), "median": statistics.median(secs), "min": min(secs), "max": max(secs),
            "checksum": rs[0]["checksum_f"],
        }
    # baseline per workload
    baseline = {}
    for k, p in points.items():
        if p["rung"] == BASELINE:
            baseline[workload_key(p)] = p
    speedup = defaultdict(lambda: defaultdict(dict))
    mismatches = []
    for k, p in points.items():
        w = workload_key(p)
        b = baseline.get(w)
        if b is None:
            continue
        if p["rung"] != BASELINE:
            speedup[w][p["rung"]][str(p["threads"])] = b["median"] / p["median"] if p["median"] > 0 else None
        tol = 1e-6 * max(1.0, abs(b["checksum"]))
        if abs(p["checksum"] - b["checksum"]) > tol:
            mismatches.append({"workload": w, "rung": p["rung"], "threads": p["threads"], "checksum": p["checksum"],
                               "baseline": b["checksum"]})
    return points, baseline, speedup, mismatches


def fmt(x):
    if x is None:
        return "--"
    if x >= 100:
        return f"{x:.0f}"
    if x >= 10:
        return f"{x:.1f}"
    if x >= 1:
        return f"{x:.2f}"
    return f"{x:.3f}"


def table_scaling_zones(points, out):
    """Median seconds per op and rung versus the number of zones (best thread count per rung)."""
    best = {}
    for p in points.values():
        k = (p["fixture"], p["op"], p["raster"], p["den"], p["zones"], p["rung"])
        if k not in best or p["median"] < best[k]["median"]:
            best[k] = p
    rows = sorted(best.values(), key=lambda p: (p["fixture"], p["op"], p["raster"], p["den"], p["n_zones"],
                                                 RUNG_ORDER.index(p["rung"]) if p["rung"] in RUNG_ORDER else 99))
    lines = ["\\begin{tabular}{llrrlrr}", "\\toprule",
             "Fixture & Op & Zones & Cells & Rung & Threads & Median [s] \\\\", "\\midrule"]
    for p in rows:
        lines.append(f"{p['fixture'].replace('_', ' ')} & {p['op']} & {p['n_zones']:,} & {p['n_cells']:,} & "
                     f"{p['rung']} & {p['threads']} & {fmt(p['median'])} \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "scaling_zones.tex").write_text("\n".join(lines) + "\n")


def table_speedup_threads(points, baseline, out):
    """Speedup over the baseline versus thread count, per rung, for every workload with a baseline."""
    lines = ["\\begin{tabular}{llrrlrr}", "\\toprule",
             "Fixture & Op & Zones & Rung & Threads & Median [s] & Speedup \\\\", "\\midrule"]
    for p in sorted(points.values(), key=lambda p: (p["fixture"], p["op"], p["n_zones"], p["raster"],
                                                     RUNG_ORDER.index(p["rung"]) if p["rung"] in RUNG_ORDER else 99,
                                                     p["threads"])):
        b = baseline.get(workload_key(p))
        if b is None:
            continue
        sp = b["median"] / p["median"] if p["median"] > 0 else None
        lines.append(f"{p['fixture'].replace('_', ' ')} & {p['op']} & {p['n_zones']:,} & {p['rung']} & "
                     f"{p['threads']} & {fmt(p['median'])} & {fmt(sp)}$\\times$ \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "speedup_threads.tex").write_text("\n".join(lines) + "\n")


def table_ladder(points, baseline, out):
    """The ladder: for every workload, each rung's best time and speedup over the baseline."""
    best = {}
    for p in points.values():
        k = (workload_key(p), p["rung"])
        if k not in best or p["median"] < best[k]["median"]:
            best[k] = p
    lines = ["\\begin{tabular}{llrlrrr}", "\\toprule",
             "Fixture & Op & Zones & Rung & Threads & Median [s] & Speedup \\\\", "\\midrule"]
    for (w, rung), p in sorted(best.items(), key=lambda kv: (kv[1]["fixture"], kv[1]["op"], kv[1]["n_zones"],
                                                             kv[1]["raster"],
                                                             RUNG_ORDER.index(kv[0][1]) if kv[0][1] in RUNG_ORDER else 99)):
        b = baseline.get(w)
        sp = (b["median"] / p["median"]) if (b and p["median"] > 0) else None
        lines.append(f"{p['fixture'].replace('_', ' ')} & {p['op']} & {p['n_zones']:,} & {rung} & {p['threads']} & "
                     f"{fmt(p['median'])} & {fmt(sp)}$\\times$ \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "ladder.tex").write_text("\n".join(lines) + "\n")


def figures(points, baseline, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # scaling with zones: per op, best time of each rung vs n_zones (one line per rung)
    fig, axes = plt.subplots(1, 1, figsize=(6.0, 4.0))
    best = defaultdict(dict)
    for p in points.values():
        k = (p["op"], p["rung"])
        cur = best[k].get(p["n_zones"])
        if cur is None or p["median"] < cur:
            best[k][p["n_zones"]] = p["median"]
    for (op, rung), series in sorted(best.items()):
        xs = sorted(series)
        axes.plot(xs, [series[x] for x in xs], marker="o", label=f"{op} / {rung}")
    axes.set_xscale("log")
    axes.set_yscale("log")
    axes.set_xlabel("zones")
    axes.set_ylabel("median wall time [s]")
    axes.grid(True, which="both", linewidth=0.3)
    axes.legend(fontsize=6)
    fig.tight_layout()
    fig.savefig(out / "figures" / "scaling_zones.png", dpi=200)
    plt.close(fig)

    # speedup vs threads: per workload with a baseline, one line per rung
    fig, axes = plt.subplots(1, 1, figsize=(6.0, 4.0))
    series = defaultdict(dict)
    for p in points.values():
        b = baseline.get(workload_key(p))
        if b is None or p["rung"] == BASELINE or p["median"] <= 0:
            continue
        series[(workload_key(p), p["rung"])][p["threads"]] = b["median"] / p["median"]
    for (w, rung), s in sorted(series.items()):
        xs = sorted(s)
        axes.plot(xs, [s[x] for x in xs], marker="o", label=f"{w.split('|')[1]} {w.split('|')[2]} / {rung}")
    axes.set_xscale("log", base=2)
    axes.set_xlabel("threads")
    axes.set_ylabel("speedup over csharp-baseline")
    axes.grid(True, which="both", linewidth=0.3)
    axes.legend(fontsize=5)
    fig.tight_layout()
    fig.savefig(out / "figures" / "speedup_threads.png", dpi=200)
    plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", nargs="+", required=True, help="raw CSV files or globs")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    paths = []
    for pattern in args.raw:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    rows = read_raw(paths)
    if not rows:
        sys.exit("no rows")
    points, baseline, speedup, mismatches = summarise(rows)
    (args.out / "tables").mkdir(parents=True, exist_ok=True)
    (args.out / "figures").mkdir(parents=True, exist_ok=True)
    table_scaling_zones(points, args.out)
    table_speedup_threads(points, baseline, args.out)
    table_ladder(points, baseline, args.out)
    figures(points, baseline, args.out)
    summary = {
        "inputs": [str(p) for p in paths],
        "points": points,
        "speedup": {w: {r: dict(t) for r, t in rs.items()} for w, rs in speedup.items()},
        "checksum": {"tolerance_relative": 1e-6, "mismatches": mismatches},
    }
    with open(args.out / "summary.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"{len(points)} points, {len(baseline)} baselined workloads, {len(mismatches)} checksum mismatches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
