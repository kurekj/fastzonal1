#!/usr/bin/env python3
"""Turn raw benchmark CSVs into the tables, figures and summary the manuscript inputs.

    python make_paper_assets.py --raw benchmarks/results/raw_<stamp>.csv --out paper_assets/

Every number in the paper comes from summary.json; the LaTeX tables are \\input by the manuscript.
A "point" is one (fixture, op, rung, threads, zones, raster, den) combination; its reported time is
the median of the repetitions, with min and max. Speedups divide the baseline median by the
point's median for the same workload. Every repetition's checksum is compared with the baseline's
for the same workload (relative 1e-6; non-finite values always count as mismatches); mismatches
and workloads without a baseline are listed, never silently dropped.

Several raw files may be pooled only when they come from the same run family (same commit,
machine, band size and fixture manifest, read from the env_<stamp>.json next to each raw file);
pass --allow-mixed to pool regardless, which the summary then records.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import statistics
import sys
from collections import defaultdict
from pathlib import Path

COLUMNS = ["fixture", "op", "rung", "threads", "rep", "seconds", "checksum", "zones", "raster", "den", "n_zones",
           "n_cells", "threads_eff", "band_rows", "load_seconds"]
EXACT_OPS = ("rasterize", "majority")  # integer results: checksums must match exactly
BASELINE = "csharp-baseline"
RUNG_ORDER = ["csharp-baseline", "csharp-optimised", "fastzonal-serial", "fastzonal-openmp", "fastzonal-cuda"]


def workload_key(r):
    return "|".join([r["fixture"], r["op"], r["zones"], r["raster"], r["den"]])


def point_key(r):
    return "|".join([r["fixture"], r["op"], r["rung"], r["threads"], r["zones"], r["raster"], r["den"]])


def env_of(raw_path: str):
    base = os.path.basename(raw_path)
    if not base.startswith("raw_"):
        return None
    env_path = os.path.join(os.path.dirname(raw_path), "env_" + base[4:-4] + ".json")
    return json.load(open(env_path)) if os.path.exists(env_path) else None


def read_raw(paths, allow_mixed):
    rows = []
    envs = []
    for path in paths:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            missing = [c for c in COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                sys.exit(f"{path}: missing columns {missing}")
            for r in reader:
                if not r.get("n_cells"):
                    continue  # truncated line
                r["threads"] = str(int(r["threads"]))
                r["seconds_f"] = float(r["seconds"])
                r["checksum_f"] = float(r["checksum"])
                r["source"] = os.path.basename(path)
                rows.append(r)
        envs.append((path, env_of(path)))
    if len(paths) > 1 and not allow_mixed:
        keys = ("git_commit", "cpu", "band_rows", "fixtures_manifest_sha256")
        first = envs[0][1]
        for path, env in envs[1:]:
            if first is None or env is None:
                sys.exit(f"{path}: no env_<stamp>.json next to it; pass --allow-mixed to pool anyway")
            for k in keys:
                if first.get(k) != env.get(k):
                    sys.exit(f"{path}: {k} differs from {envs[0][0]} ({env.get(k)!r} vs {first.get(k)!r}); "
                             "pass --allow-mixed to pool anyway")
    return rows, [{"raw": p, "env": e} for p, e in envs]


def summarise(rows):
    groups = defaultdict(list)
    for r in rows:
        groups[point_key(r)].append(r)
    points = {}
    for k, rs in groups.items():
        secs = [r["seconds_f"] for r in rs]
        r0 = rs[0]
        points[k] = {
            "fixture": r0["fixture"], "op": r0["op"], "rung": r0["rung"], "threads": int(r0["threads"]),
            "threads_eff": int(r0["threads_eff"]), "band_rows": int(r0["band_rows"]),
            "zones": r0["zones"], "raster": r0["raster"], "den": r0["den"],
            "n_zones": int(r0["n_zones"]), "n_cells": int(r0["n_cells"]),
            "n": len(secs), "median": statistics.median(secs), "min": min(secs), "max": max(secs),
            "load_seconds": float(r0.get("load_seconds") or 0.0),
            "checksums": [r["checksum_f"] for r in rs], "sources": sorted({r["source"] for r in rs}),
        }
    baseline = {}
    for p in points.values():
        if p["rung"] == BASELINE:
            baseline[workload_key(p)] = p
    speedup = defaultdict(lambda: defaultdict(dict))
    mismatches = []
    unbaselined = sorted({workload_key(p) for p in points.values() if workload_key(p) not in baseline})
    for p in points.values():
        w = workload_key(p)
        b = baseline.get(w)
        if b is None:
            continue
        if p["rung"] != BASELINE:
            speedup[w][p["rung"]][str(p["threads"])] = b["median"] / p["median"] if p["median"] > 0 else None
        bsum = b["checksums"][0]
        tol = 0.0 if p["op"] in EXACT_OPS else (1e-6 * max(1.0, abs(bsum)) if math.isfinite(bsum) else 0.0)
        for rep, c in enumerate(p["checksums"]):
            if not math.isfinite(c) or not math.isfinite(bsum) or abs(c - bsum) > tol:
                mismatches.append({"workload": w, "rung": p["rung"], "threads": p["threads"], "rep": rep,
                                   "checksum": c, "baseline": bsum})
    # the serial backend and the OpenMP backend at one thread run the same code path: a difference
    # beyond 15% between their medians is measurement variability and is flagged, never explained
    variability = []
    for p in points.values():
        if p["rung"] != "fastzonal-openmp" or p["threads"] != 1:
            continue
        q = points.get("|".join([p["fixture"], p["op"], "fastzonal-serial", "1", p["zones"], p["raster"], p["den"]]))
        if q is None or q["median"] <= 0:
            continue
        ratio = p["median"] / q["median"]
        if abs(ratio - 1.0) > 0.15:
            variability.append({"workload": workload_key(p), "openmp_1_median": p["median"],
                                "serial_median": q["median"], "ratio": ratio})
    return points, baseline, speedup, mismatches, unbaselined, variability


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


def rung_rank(r):
    return RUNG_ORDER.index(r) if r in RUNG_ORDER else 99


def wl_label(p):
    return f"{p['fixture'].replace('_', ' ')} {p['op']} {p['raster']}" + (f"/{p['den']}" if p["den"] else "")


def table_scaling_zones(points, out):
    """Median seconds per workload and rung versus the number of zones (best thread count per rung)."""
    best = {}
    for p in points.values():
        k = (workload_key(p), p["rung"])
        if k not in best or p["median"] < best[k]["median"]:
            best[k] = p
    rows = sorted(best.values(), key=lambda p: (p["fixture"], p["op"], p["raster"], p["den"], p["n_zones"],
                                                 rung_rank(p["rung"])))
    lines = ["\\begin{tabular}{lrrlrr}", "\\toprule",
             "Workload & Zones & Cells & Rung & Threads & Median [ms] \\\\", "\\midrule"]
    for p in rows:
        lines.append(f"{wl_label(p)} & {p['n_zones']:,} & {p['n_cells']:,} & {p['rung']} & {p['threads_eff']} & "
                     f"{fmt(p['median'] * 1000)} \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "scaling_zones.tex").write_text("\n".join(lines) + "\n")


def table_speedup_threads(points, baseline, out):
    """Speedup over the baseline versus thread count, per rung, for every workload with a baseline."""
    lines = ["\\begin{tabular}{lrlrrrr}", "\\toprule",
             "Workload & Zones & Rung & Requested & Effective & Median [ms] & Speedup \\\\", "\\midrule"]
    for p in sorted(points.values(), key=lambda p: (p["fixture"], p["op"], p["raster"], p["den"], p["n_zones"],
                                                     rung_rank(p["rung"]), p["threads"])):
        b = baseline.get(workload_key(p))
        if b is None:
            continue
        sp = b["median"] / p["median"] if p["median"] > 0 else None
        lines.append(f"{wl_label(p)} & {p['n_zones']:,} & {p['rung']} & {p['threads']} & {p['threads_eff']} & "
                     f"{fmt(p['median'] * 1000)} & {fmt(sp)}$\\times$ \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "speedup_threads.tex").write_text("\n".join(lines) + "\n")


def table_ladder(points, baseline, out):
    """The ladder: for every workload, each rung's best time and speedup over the baseline."""
    best = {}
    for p in points.values():
        k = (workload_key(p), p["rung"])
        if k not in best or p["median"] < best[k]["median"]:
            best[k] = p
    lines = ["\\begin{tabular}{lrlrrr}", "\\toprule",
             "Workload & Zones & Rung & Threads & Median [ms] & Speedup \\\\", "\\midrule"]
    for (w, rung), p in sorted(best.items(), key=lambda kv: (kv[1]["fixture"], kv[1]["op"], kv[1]["raster"],
                                                             kv[1]["den"], kv[1]["n_zones"], rung_rank(kv[0][1]))):
        b = baseline.get(w)
        sp = (b["median"] / p["median"]) if (b and p["median"] > 0) else None
        lines.append(f"{wl_label(p)} & {p['n_zones']:,} & {rung} & {p['threads_eff']} & {fmt(p['median'] * 1000)} & "
                     f"{fmt(sp)}$\\times$ \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "ladder.tex").write_text("\n".join(lines) + "\n")


def op_label(p):
    return f"{p['op']} {p['raster']}" + (f"/{p['den']}" if p["den"] else "")


def family_key(p):
    return (p["fixture"], p["op"], p["raster"], p["den"])


def best_per_rung(points, fam):
    """fam -> {rung: {n_zones: point with the smallest median}}"""
    best = defaultdict(dict)
    for p in points.values():
        if family_key(p) != fam:
            continue
        cur = best[p["rung"]].get(p["n_zones"])
        if cur is None or p["median"] < cur["median"]:
            best[p["rung"]][p["n_zones"]] = p
    return best


def headline(points, baseline):
    """Per family (fixture, op, raster, den): the zone range, the baseline time at the largest zone
    count and, per rung, the range over zone counts of the best speedup; the manuscript quotes these."""
    rows = []
    for fam in sorted({family_key(p) for p in points.values()}):
        best = best_per_rung(points, fam)
        if BASELINE not in best:
            continue
        zones = sorted(best[BASELINE])
        rec = {"fixture": fam[0], "op": fam[1], "raster": fam[2], "den": fam[3], "n_zones_min": zones[0],
               "n_zones_max": zones[-1], "baseline_seconds_at_max": best[BASELINE][zones[-1]]["median"], "rungs": {}}
        for rung, by_zone in best.items():
            if rung == BASELINE:
                continue
            sp = {z: best[BASELINE][z]["median"] / q["median"] for z, q in by_zone.items()
                  if z in best[BASELINE] and q["median"] > 0}
            if not sp:
                continue
            at_max = by_zone.get(zones[-1])
            rec["rungs"][rung] = {"speedup_min": min(sp.values()), "speedup_max": max(sp.values()),
                                  "speedup_at_max_zones": sp.get(zones[-1]),
                                  "threads_at_max_zones": at_max["threads_eff"] if at_max else None,
                                  "seconds_at_max_zones": at_max["median"] if at_max else None}
        rows.append(rec)
    return rows


def table_headline(rows, out):
    def rng(r, rung):
        s = r["rungs"].get(rung)
        if not s:
            return "--"
        lo, hi = s["speedup_min"], s["speedup_max"]
        return f"{fmt(lo)}--{fmt(hi)}$\\times$" if abs(hi - lo) > 0.05 * hi else f"{fmt(hi)}$\\times$"

    lines = ["\\begin{tabular}{llrrrrr}", "\\toprule",
             "Fixture & Operation & Zones & Baseline [ms] & C\\# optimised & fastzonal serial & fastzonal OpenMP \\\\",
             "\\midrule"]
    for r in rows:
        omp = r["rungs"].get("fastzonal-openmp")
        thr = f" ({omp['threads_at_max_zones']} thr.)" if omp and omp.get("threads_at_max_zones") else ""
        zones = f"{r['n_zones_max']:,}" if r["n_zones_min"] == r["n_zones_max"] else f"{r['n_zones_min']:,}--{r['n_zones_max']:,}"
        lines.append(f"{r['fixture'].replace('_', ' ')} & {op_label(r)} & {zones} & "
                     f"{fmt(r['baseline_seconds_at_max'] * 1000)} & {rng(r, 'csharp-optimised')} & "
                     f"{rng(r, 'fastzonal-serial')} & {rng(r, 'fastzonal-openmp')}{thr} \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (out / "tables" / "headline.tex").write_text("\n".join(lines) + "\n")


def paper_figures(points, baseline, out):
    """Compact figures for the manuscript: scaling with zones for the families measured at three or
    more zone counts (one row per fixture), and speedup against effective threads at the largest
    zone count of the fixture with the most zones."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fams = defaultdict(list)
    for fam in sorted({family_key(p) for p in points.values()}):
        if len({p["n_zones"] for p in points.values() if family_key(p) == fam}) >= 3:
            fams[fam[0]].append(fam)
    if fams:
        scaling_figure(fams, points, out, plt)
    top = max(baseline.values(), key=lambda b: b["n_zones"], default=None)
    if top is not None:
        threads_figure(top, points, baseline, out, plt)


def scaling_figure(fams, points, out, plt):
    ncol = max(len(v) for v in fams.values())
    fig, axes = plt.subplots(len(fams), ncol, figsize=(1.9 * ncol + 0.6, 2.3 * len(fams) + 0.3), squeeze=False)
    for row, (fixture, flist) in enumerate(sorted(fams.items())):
        for col in range(ncol):
            ax = axes[row][col]
            if col >= len(flist):
                ax.axis("off")
                continue
            fam = flist[col]
            best = best_per_rung(points, fam)
            for rung in sorted(best, key=rung_rank):
                xs = sorted(best[rung])
                ax.plot(xs, [best[rung][x]["median"] * 1000 for x in xs], marker="o", markersize=2.5, linewidth=1,
                        label=rung)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_title(f"{fixture.replace('_', ' ')}: {fam[1]} {fam[2]}" + (f"/{fam[3]}" if fam[3] else ""),
                         fontsize=7)
            ax.set_xlabel("zones", fontsize=7)
            if col == 0:
                ax.set_ylabel("median wall time [ms]", fontsize=7)
            ax.tick_params(labelsize=6)
            ax.grid(True, which="both", linewidth=0.3)
            if row == 0 and col == 0:
                ax.legend(fontsize=5)
    fig.tight_layout()
    fig.savefig(out / "figures" / "paper_scaling.png", dpi=220)
    plt.close(fig)


def threads_figure(top, points, baseline, out, plt):
    fixture = top["fixture"]
    wls = sorted((w for w, b in baseline.items() if b["fixture"] == fixture and b["n_zones"] == top["n_zones"]),
                 key=lambda w: baseline[w]["op"])
    if not wls:
        return
    fig, axes = plt.subplots(1, len(wls), figsize=(1.9 * len(wls) + 0.6, 2.5), squeeze=False)
    for ax, w in zip(axes[0], wls):
        series, chosen = defaultdict(dict), set()
        for p in sorted(points.values(), key=lambda q: q["threads"]):
            if workload_key(p) != w or p["rung"] == BASELINE or p["median"] <= 0 or (p["rung"], p["threads_eff"]) in chosen:
                continue
            chosen.add((p["rung"], p["threads_eff"]))
            series[p["rung"]][p["threads_eff"]] = baseline[w]["median"] / p["median"]
        for rung in sorted(series, key=rung_rank):
            xs = sorted(series[rung])
            ax.plot(xs, [series[rung][x] for x in xs], marker="o", markersize=2.5, linewidth=1, label=rung)
        ax.axhline(1.0, color="grey", linewidth=0.6, linestyle=":")
        ax.set_xscale("log", base=2)
        ax.set_title(f"{op_label(baseline[w])} ({baseline[w]['n_zones']:,} zones)", fontsize=7)
        ax.set_xlabel("effective threads", fontsize=7)
        ax.tick_params(labelsize=6)
        ax.grid(True, which="both", linewidth=0.3)
    axes[0][0].set_ylabel("speedup over C# baseline", fontsize=7)
    axes[0][0].legend(fontsize=5)
    fig.suptitle(fixture.replace("_", " "), fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "figures" / "paper_threads.png", dpi=220)
    plt.close(fig)


def figures(points, baseline, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # scaling with zones: one panel per (fixture, op, raster, den), one line per rung (best threads)
    families = sorted({(p["fixture"], p["op"], p["raster"], p["den"]) for p in points.values()})
    ncol = 2
    nrow = max(1, (len(families) + ncol - 1) // ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(7.0, 2.6 * nrow), squeeze=False)
    for ax, fam in zip(axes.flat, families):
        best = defaultdict(dict)
        for p in points.values():
            if (p["fixture"], p["op"], p["raster"], p["den"]) != fam:
                continue
            cur = best[p["rung"]].get(p["n_zones"])
            if cur is None or p["median"] < cur:
                best[p["rung"]][p["n_zones"]] = p["median"]
        for rung in sorted(best, key=rung_rank):
            xs = sorted(best[rung])
            ax.plot(xs, [best[rung][x] for x in xs], marker="o", markersize=3, linewidth=1, label=rung)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(f"{fam[0].replace('_', ' ')}: {fam[1]} {fam[2]}" + (f"/{fam[3]}" if fam[3] else ""), fontsize=7)
        ax.set_xlabel("zones", fontsize=7)
        ax.set_ylabel("median wall time [s]", fontsize=7)
        ax.tick_params(labelsize=6)
        ax.grid(True, which="both", linewidth=0.3)
        ax.legend(fontsize=5)
    for ax in list(axes.flat)[len(families):]:
        ax.axis("off")
    fig.tight_layout()
    fig.savefig(out / "figures" / "scaling_zones.png", dpi=200)
    plt.close(fig)

    # speedup vs effective threads: one panel per workload with a baseline, one line per rung
    workloads = sorted(w for w in baseline)
    nrow = max(1, (len(workloads) + ncol - 1) // ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(7.0, 2.6 * nrow), squeeze=False)
    for ax, w in zip(axes.flat, workloads):
        series = defaultdict(dict)
        chosen = {}
        for p in sorted(points.values(), key=lambda q: q["threads"]):
            if workload_key(p) != w or p["rung"] == BASELINE or p["median"] <= 0:
                continue
            # requested counts that collapse to the same effective count are the same configuration:
            # keep the smallest request, never the best of several equivalent measurements
            if (p["rung"], p["threads_eff"]) in chosen:
                continue
            chosen[(p["rung"], p["threads_eff"])] = True
            series[p["rung"]][p["threads_eff"]] = baseline[w]["median"] / p["median"]
        for rung in sorted(series, key=rung_rank):
            xs = sorted(series[rung])
            ax.plot(xs, [series[rung][x] for x in xs], marker="o", markersize=3, linewidth=1, label=rung)
        b = baseline[w]
        ax.set_xscale("log", base=2)
        ax.set_title(f"{wl_label(b)} ({b['n_zones']:,} zones)", fontsize=7)
        ax.set_xlabel("effective threads", fontsize=7)
        ax.set_ylabel("speedup over baseline", fontsize=7)
        ax.tick_params(labelsize=6)
        ax.grid(True, which="both", linewidth=0.3)
        ax.legend(fontsize=5)
    for ax in list(axes.flat)[len(workloads):]:
        ax.axis("off")
    fig.tight_layout()
    fig.savefig(out / "figures" / "speedup_threads.png", dpi=200)
    plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", nargs="+", required=True, help="raw CSV files or globs")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--allow-mixed", action="store_true", help="pool raw files from different runs")
    args = ap.parse_args(argv)
    paths = []
    for pattern in args.raw:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    rows, provenance = read_raw(paths, args.allow_mixed)
    if not rows:
        sys.exit("no rows")
    points, baseline, speedup, mismatches, unbaselined, variability = summarise(rows)
    (args.out / "tables").mkdir(parents=True, exist_ok=True)
    (args.out / "figures").mkdir(parents=True, exist_ok=True)
    table_scaling_zones(points, args.out)
    table_speedup_threads(points, baseline, args.out)
    table_ladder(points, baseline, args.out)
    head = headline(points, baseline)
    table_headline(head, args.out)
    figures(points, baseline, args.out)
    paper_figures(points, baseline, args.out)
    summary = {
        "inputs": provenance,
        "mixed_runs_allowed": args.allow_mixed,
        "headline": head,
        "points": points,
        "speedup": {w: {r: dict(t) for r, t in rs.items()} for w, rs in speedup.items()},
        "checksum": {"tolerance_relative_float_ops": 1e-6, "exact_ops": list(EXACT_OPS), "mismatches": mismatches,
                     "workloads_without_baseline": unbaselined},
        "variability_warnings": variability,
    }
    with open(args.out / "summary.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"{len(points)} points, {len(baseline)} baselined workloads, {len(unbaselined)} without baseline, "
          f"{len(mismatches)} checksum mismatches, {len(variability)} variability warnings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
