#!/usr/bin/env python3
"""Compare fastzonal (Python binding) with other open-source zonal-statistics tools on the same
fixtures: rasterstats, exactextract, xarray-spatial, rasterio/GDAL rasterisation and plain NumPy.

Three stages are timed separately because the tools do not split the work the same way:
  pipeline   shapely polygons -> per-zone mean (rasterstats, exactextract, fastzonal)
  rasterize  shapely polygons -> zone-index mask (rasterio.features.rasterize, fastzonal)
  stats      zone-index mask + raster -> per-zone mean (numpy bincount, xarray-spatial, fastzonal)
Every row carries the wall time of the whole call (`seconds`, from the inputs the tool takes to
the per-zone result as a NumPy array) and of the part after input conversion (`compute_seconds`),
plus an agreement measure of that repetition's result against the fastzonal reference (one thread)
on the same stage: `max_abs_diff` of the per-zone means over zones both tools filled,
`n_only_tool` / `n_only_fastzonal` for zones filled by one side only, or `n_diff_cells` for masks.
One untimed warm-up call precedes the timed repetitions of every tool (first calls pay JIT
compilation or cache initialisation). Results append to a CSV and a run is resumable (finished
(case, tool, stage, threads) combinations are skipped; a stage whose tools are all done is skipped
without recomputing the reference).

    python benchmarks/compare_tools.py run --fixtures /data/fastzonal/fixtures \\
        --cases biebrza_upper:grid1000:dem30,mazovia_5000:grid100:dem30 --threads 1,8,56 \\
        --out benchmarks/results/tools_<stamp>.csv
    python benchmarks/compare_tools.py summarise --raw benchmarks/results/tools_<stamp>.csv --out paper_assets
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "bindings" / "python"))

COLUMNS = ["fixture", "zones", "raster", "n_zones", "n_cells", "stage", "tool", "threads", "rep", "seconds",
           "compute_seconds", "max_abs_diff", "n_compared", "n_only_tool", "n_only_fastzonal", "n_diff_cells",
           "note"]


@dataclass
class Case:
    fixture: str
    zones: str
    raster: str

    @property
    def key(self) -> str:
        return f"{self.fixture}:{self.zones}:{self.raster}"


@dataclass
class CaseData:
    case: Case
    raster: np.ndarray          # float32, nodata as in the fixture
    nodata: float               # NaN when the fixture has none
    transform: Tuple[float, float, float, float]
    polygons: np.ndarray        # object array of shapely geometries, one per zone
    n_zones: int
    extras: dict = field(default_factory=dict)   # masks and reference results shared between tools


@dataclass
class ToolResult:
    value: object               # per-zone means (float64, NaN = empty) or an int32 mask
    seconds: float
    compute_seconds: float
    note: str = ""


def parse_cases(text: str) -> List[Case]:
    out = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        parts = item.split(":")
        if len(parts) != 3:
            raise ValueError(f"case must be fixture:zones:raster, got {item!r}")
        out.append(Case(*parts))
    return out


def compare_means(tool: np.ndarray, ref: np.ndarray) -> dict:
    """max |tool - ref| over zones both filled; counts of zones filled by only one side."""
    tool = np.asarray(tool, dtype=np.float64)
    ref = np.asarray(ref, dtype=np.float64)
    if tool.shape != ref.shape:
        raise ValueError("per-zone arrays differ in length")
    both = ~np.isnan(tool) & ~np.isnan(ref)
    diff = float(np.max(np.abs(tool[both] - ref[both]))) if both.any() else 0.0
    return {"max_abs_diff": diff, "n_compared": int(both.sum()),
            "n_only_tool": int((~np.isnan(tool) & np.isnan(ref)).sum()),
            "n_only_fastzonal": int((np.isnan(tool) & ~np.isnan(ref)).sum())}


def compare_masks(tool: np.ndarray, ref: np.ndarray) -> dict:
    if tool.shape != ref.shape:
        raise ValueError("masks differ in shape")
    return {"n_diff_cells": int(np.count_nonzero(np.asarray(tool) != np.asarray(ref)))}


def means_from_stats(stats: np.ndarray) -> np.ndarray:
    out = np.full(len(stats), np.nan, dtype=np.float64)
    filled = stats["count"] > 0
    out[filled] = stats["sum"][filled] / stats["count"][filled]
    return out


# ----------------------------------------------------------------------------------------------
# fixture loading


def load_case(fixtures: Path, case: Case) -> CaseData:
    import fastzonal as fz
    from fzfix import io

    d = fixtures / case.fixture
    raster, meta = io.read_raster(d / f"raster_{case.raster}.json")
    raster = np.ascontiguousarray(raster.astype(np.float32))
    nodata = float("nan") if meta["nodata"] is None else float(meta["nodata"])
    z = io.read_zones(d / f"zones_{case.zones}.json")
    n_zones = int(z["meta"]["n_zones"])
    polygons = fz.rings_to_polygons((z["xy"], z["ring_start"], z["ring_is_hole"], z["ring_zone"]), n_zones)
    return CaseData(case, raster, nodata, tuple(float(v) for v in meta["transform"]), polygons, n_zones)


def _affine(cd: CaseData):
    from affine import Affine
    x0, y0, pw, ph = cd.transform
    return Affine(pw, 0.0, x0, 0.0, -ph, y0)


def _fz_options(threads: int):
    import fastzonal as fz
    return fz.Options(backend="serial" if threads == 1 else "openmp", threads=threads, band_rows=16)


# ----------------------------------------------------------------------------------------------
# tools: each takes (CaseData, threads) and returns a ToolResult


def fastzonal_pipeline(cd: CaseData, threads: int) -> ToolResult:
    import fastzonal as fz
    t0 = time.perf_counter()
    rings = fz.polygons_to_rings(cd.polygons)
    t1 = time.perf_counter()
    h, w = cd.raster.shape
    mask = fz.rasterize(rings=rings, transform=cd.transform, width=w, height=h, mode="center",
                        options=_fz_options(threads))
    stats = fz.zonal(cd.raster, mask, cd.n_zones, nodata=cd.nodata, options=_fz_options(threads))
    means = means_from_stats(stats)
    t2 = time.perf_counter()
    cd.extras.setdefault("fz_mask", mask)
    return ToolResult(means, t2 - t0, t2 - t1)


def rasterstats_pipeline(cd: CaseData, threads: int) -> ToolResult:
    from rasterstats import zonal_stats
    t0 = time.perf_counter()
    # nodata is passed as given (NaN included): with None rasterstats would substitute -999
    rows = zonal_stats(list(cd.polygons), cd.raster, affine=_affine(cd), nodata=cd.nodata, stats=["mean"])
    out = np.array([np.nan if r["mean"] is None else float(r["mean"]) for r in rows], dtype=np.float64)
    t1 = time.perf_counter()
    return ToolResult(out, t1 - t0, t1 - t0, "pixel-centre rule, all_touched=False")


def exactextract_pipeline(cd: CaseData, threads: int) -> ToolResult:
    from exactextract import exact_extract
    from exactextract.raster import NumPyRasterSource
    from shapely.geometry import mapping
    t0 = time.perf_counter()
    feats = [{"type": "Feature", "properties": {"fid": i}, "geometry": mapping(g)} for i, g in enumerate(cd.polygons)]
    t1 = time.perf_counter()
    x0, y0, pw, ph = cd.transform
    h, w = cd.raster.shape
    src = NumPyRasterSource(cd.raster, x0, y0 - h * ph, x0 + w * pw, y0,
                            nodata=None if math.isnan(cd.nodata) else cd.nodata)
    df = exact_extract(src, feats, ["mean"], include_cols=["fid"], output="pandas")
    out = np.full(cd.n_zones, np.nan, dtype=np.float64)
    out[df["fid"].to_numpy()] = df["mean"].to_numpy(dtype=np.float64)
    t2 = time.perf_counter()
    return ToolResult(out, t2 - t0, t2 - t1, "coverage-fraction weighted mean")


def fastzonal_rasterize(cd: CaseData, threads: int) -> ToolResult:
    import fastzonal as fz
    t0 = time.perf_counter()
    rings = fz.polygons_to_rings(cd.polygons)
    t1 = time.perf_counter()
    h, w = cd.raster.shape
    mask = fz.rasterize(rings=rings, transform=cd.transform, width=w, height=h, mode="center",
                        options=_fz_options(threads))
    t2 = time.perf_counter()
    cd.extras.setdefault("fz_mask", mask)
    return ToolResult(mask, t2 - t0, t2 - t1)


def gdal_rasterize(cd: CaseData, threads: int) -> ToolResult:
    from rasterio import features
    t0 = time.perf_counter()
    shapes = [(g, i) for i, g in enumerate(cd.polygons) if not g.is_empty]
    t1 = time.perf_counter()
    mask = features.rasterize(shapes, out_shape=cd.raster.shape, transform=_affine(cd), fill=-1, dtype="int32",
                              all_touched=False)
    t2 = time.perf_counter()
    return ToolResult(mask, t2 - t0, t2 - t1, "rasterio.features.rasterize (GDAL), all_touched=False")


def _mask(cd: CaseData) -> np.ndarray:
    if "fz_mask" not in cd.extras:
        fastzonal_rasterize(cd, 1)
    return cd.extras["fz_mask"]


def fastzonal_stats(cd: CaseData, threads: int) -> ToolResult:
    import fastzonal as fz
    mask = _mask(cd)
    t0 = time.perf_counter()
    stats = fz.zonal(cd.raster, mask, cd.n_zones, nodata=cd.nodata, options=_fz_options(threads))
    means = means_from_stats(stats)
    t1 = time.perf_counter()
    return ToolResult(means, t1 - t0, t1 - t0)


def numpy_stats(cd: CaseData, threads: int) -> ToolResult:
    mask = _mask(cd)
    t0 = time.perf_counter()
    valid = mask >= 0
    if not math.isnan(cd.nodata):
        valid &= cd.raster != np.float32(cd.nodata)
    valid &= ~np.isnan(cd.raster)
    ids = mask[valid]
    sums = np.bincount(ids, weights=cd.raster[valid].astype(np.float64), minlength=cd.n_zones)
    counts = np.bincount(ids, minlength=cd.n_zones)
    out = np.full(cd.n_zones, np.nan, dtype=np.float64)
    out[counts > 0] = sums[counts > 0] / counts[counts > 0]
    t1 = time.perf_counter()
    return ToolResult(out, t1 - t0, t1 - t0, "np.bincount with float64 weights")


def xrspatial_stats(cd: CaseData, threads: int) -> ToolResult:
    import xarray as xr
    from xrspatial import zonal
    mask = _mask(cd)
    t0 = time.perf_counter()
    values = xr.DataArray(cd.raster)
    zones = xr.DataArray(mask)
    df = zonal.stats(zones=zones, values=values, stats_funcs=["mean"],
                     nodata_values=None if math.isnan(cd.nodata) else cd.nodata)
    out = np.full(cd.n_zones, np.nan, dtype=np.float64)
    zid = df["zone"].to_numpy().astype(np.int64)
    keep = zid >= 0
    out[zid[keep]] = df["mean"].to_numpy(dtype=np.float64)[keep]
    t1 = time.perf_counter()
    return ToolResult(out, t1 - t0, t1 - t0, "xrspatial.zonal.stats on the fastzonal mask")


STAGES: Dict[str, Dict[str, Callable[[CaseData, int], ToolResult]]] = {
    "pipeline": {"fastzonal": fastzonal_pipeline, "rasterstats": rasterstats_pipeline,
                 "exactextract": exactextract_pipeline},
    "rasterize": {"fastzonal": fastzonal_rasterize, "gdal": gdal_rasterize},
    "stats": {"fastzonal": fastzonal_stats, "numpy": numpy_stats, "xrspatial": xrspatial_stats},
}


# ----------------------------------------------------------------------------------------------
# runner


def done_keys(path: Path) -> set:
    if not path.exists():
        return set()
    with open(path, newline="") as f:
        return {(r["fixture"], r["zones"], r["raster"], r["stage"], r["tool"], int(r["threads"])) for r in csv.DictReader(f)}


def append_rows(path: Path, rows: List[dict]) -> None:
    new = not path.exists() or path.stat().st_size == 0
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        if new:
            w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, "") for c in COLUMNS})


def agreement(stage: str, value, reference) -> dict:
    return compare_masks(value, reference) if stage == "rasterize" else compare_means(value, reference)


def run_case(cd: CaseData, stages: Dict[str, Dict[str, Callable]], threads: Sequence[int], reps: int,
             max_seconds: float, out: Path, done: set, log=print) -> None:
    c = cd.case
    n_cells = int(cd.raster.size)
    for stage, tools in stages.items():
        plan = [(tool, t) for tool in ["fastzonal"] + [u for u in tools if u != "fastzonal"] if tool in tools
                for t in (threads if tool == "fastzonal" else [1])]
        if all((c.fixture, c.zones, c.raster, stage, tool, t) in done for tool, t in plan):
            continue
        # the one-thread fastzonal result is the reference every repetition is compared with; it is
        # recomputed (untimed) when the stage resumes with the reference rows already written
        reference = None
        if "fastzonal" in tools and (c.fixture, c.zones, c.raster, stage, "fastzonal", 1) in done:
            reference = tools["fastzonal"](cd, 1).value
        for tool, t in plan:
            key = (c.fixture, c.zones, c.raster, stage, tool, t)
            if key in done:
                continue
            tools[tool](cd, t)  # warm-up, untimed
            results = []
            for rep in range(reps):
                r = tools[tool](cd, t)
                results.append(r)
                log(f"{c.key} {stage:9s} {tool:12s} t={t:<3d} rep {rep}: {r.seconds:.4f} s (compute {r.compute_seconds:.4f})")
                if r.seconds > max_seconds:
                    break
            if reference is None:
                reference = results[0].value
            rows = [dict(fixture=c.fixture, zones=c.zones, raster=c.raster, n_zones=cd.n_zones, n_cells=n_cells,
                         stage=stage, tool=tool, threads=t, rep=i, seconds=f"{r.seconds:.6f}",
                         compute_seconds=f"{r.compute_seconds:.6f}", note=r.note,
                         **agreement(stage, r.value, reference))
                    for i, r in enumerate(results)]
            append_rows(out, rows)
            done.add(key)


def write_env(path: Path, args) -> None:
    def sh(cmd):
        try:
            return subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return ""
    versions = {}
    for mod in ("numpy", "shapely", "rasterio", "rasterstats", "exactextract", "xrspatial", "xarray", "fastzonal"):
        try:
            m = __import__(mod)
            versions[mod] = getattr(m, "__version__", None) or (m.version() if hasattr(m, "version") else "")
        except Exception as e:  # pragma: no cover
            versions[mod] = f"unavailable: {e}"
    try:
        from osgeo import gdal
        versions["gdal"] = gdal.__version__
    except Exception:
        import rasterio
        versions["gdal"] = rasterio.__gdal_version__
    cpu = ""
    if os.path.exists("/proc/cpuinfo"):
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    repo = HERE.parent
    record = {
        "stamp": path.stem.split("_", 1)[1] if "_" in path.stem else path.stem, "hostname": platform.node(),
        "os": platform.platform(), "python": sys.version.split()[0], "cpu": cpu, "cores": os.cpu_count(),
        "git_commit": sh(f"git -C '{repo}' rev-parse HEAD"), "git_dirty": bool(sh(f"git -C '{repo}' status --porcelain --untracked-files=no")),
        "fastzonal_library": os.environ.get("FASTZONAL_LIBRARY"), "omp_proc_bind": os.environ.get("OMP_PROC_BIND"),
        "omp_places": os.environ.get("OMP_PLACES"),
        "cpu_governor": (open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read().strip()
                         if os.path.exists("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") else None),
        "versions": versions, "cases": args.cases, "threads": args.threads, "reps": args.reps,
        "max_seconds": args.max_seconds, "band_rows": 16, "raster_mode": "center",
        "numactl": os.environ.get("FZ_NUMACTL", ""),
    }
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def cmd_run(args) -> int:
    fixtures = Path(args.fixtures)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    threads = [int(t) for t in args.threads.split(",")]
    stages = {s: dict(STAGES[s]) for s in args.stages.split(",")}
    if args.tools:
        keep = set(args.tools.split(","))
        stages = {s: {t: f for t, f in tools.items() if t in keep or t == "fastzonal"} for s, tools in stages.items()}
    env_path = out.with_name(out.stem.replace("tools_", "tools_env_") + ".json")
    if not env_path.exists():
        write_env(env_path, args)
    done = done_keys(out)
    for case in parse_cases(args.cases):
        t0 = time.perf_counter()
        cd = load_case(fixtures, case)
        print(f"loaded {case.key}: {cd.n_zones} zones, {cd.raster.shape[1]}x{cd.raster.shape[0]} cells "
              f"({time.perf_counter() - t0:.1f} s)", flush=True)
        run_case(cd, stages, threads, args.reps, args.max_seconds, out, done,
                 log=lambda s: print(s, flush=True))
    print(f"done: {out}")
    return 0


# ----------------------------------------------------------------------------------------------
# summary


def load_rows(paths: Sequence[Path]) -> List[dict]:
    rows = []
    for p in paths:
        with open(p, newline="") as f:
            rows.extend(csv.DictReader(f))
    return rows


def summarise(rows: List[dict]) -> dict:
    """Median seconds per (fixture, zones, raster, stage, tool, threads) with the agreement columns."""
    groups: Dict[tuple, List[dict]] = defaultdict(list)
    for r in rows:
        groups[(r["fixture"], r["zones"], r["raster"], r["stage"], r["tool"], int(r["threads"]))].append(r)
    out = []
    for key, rs in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2], kv[0][3], kv[0][4], kv[0][5])):
        fixture, zones, raster, stage, tool, threads = key
        rec = {"fixture": fixture, "zones": zones, "raster": raster, "stage": stage, "tool": tool, "threads": threads,
               "n_zones": int(rs[0]["n_zones"]), "n_cells": int(rs[0]["n_cells"]), "reps": len(rs),
               "median_seconds": statistics.median(float(r["seconds"]) for r in rs),
               "median_compute_seconds": statistics.median(float(r["compute_seconds"]) for r in rs),
               "note": rs[0]["note"]}
        # the worst agreement over the repetitions (a later repetition may differ from the first)
        for c in ("max_abs_diff", "n_only_tool", "n_only_fastzonal", "n_diff_cells"):
            vals = [r.get(c, "") for r in rs]
            vals = [float(v) if c == "max_abs_diff" else int(v) for v in vals if v not in ("", None)]
            rec[c] = max(vals) if vals else None
        vals = [int(r["n_compared"]) for r in rs if r.get("n_compared", "") not in ("", None)]
        rec["n_compared"] = min(vals) if vals else None
        out.append(rec)
    return {"groups": out}


def _fmt_s(x: Optional[float]) -> str:
    if x is None:
        return "--"
    if x >= 100:
        return f"{x:.0f}"
    if x >= 10:
        return f"{x:.1f}"
    if x >= 1:
        return f"{x:.2f}"
    return f"{x:.3f}"


def _tex(text: str) -> str:
    return "".join("\\" + ch if ch in "&%$#_{}" else ch for ch in str(text))


def write_tables(summary: dict, out_dir: Path, fast_threads: int) -> None:
    """Two tables of total wall time (seconds) per call: polygons to means, and the rasterise and
    stats stages. All columns are `median_seconds` (whole call, input conversion included)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    g = {(r["fixture"], r["zones"], r["raster"], r["stage"], r["tool"], r["threads"]): r for r in summary["groups"]}
    cases = sorted({(r["fixture"], r["zones"], r["raster"], r["n_zones"]) for r in summary["groups"]},
                   key=lambda c: (c[0], c[2], c[3]))

    def sec(fixture, zones, raster, stage, tool, threads):
        r = g.get((fixture, zones, raster, stage, tool, threads))
        return None if r is None else r["median_seconds"]

    def speed(a, b):
        return "--" if a is None or b is None or b == 0 else f"{a / b:.1f}"

    def label(fixture, zones, raster, nz):
        return f"{_tex(fixture.replace('_', ' '))} & {_tex(zones)} & {_tex(raster)} & {nz:,}"

    lines = [r"\begin{tabular}{lllrrrrrrr}", r"\toprule",
             r"Fixture & Zones & Raster & $n_z$ & rasterstats & exactextract & fastzonal (1) & fastzonal (%d) & $\times$ vs rasterstats & $\times$ vs exactextract \\" % fast_threads,
             r"\midrule"]
    for fixture, zones, raster, nz in cases:
        rs = sec(fixture, zones, raster, "pipeline", "rasterstats", 1)
        ee = sec(fixture, zones, raster, "pipeline", "exactextract", 1)
        f1 = sec(fixture, zones, raster, "pipeline", "fastzonal", 1)
        ft = sec(fixture, zones, raster, "pipeline", "fastzonal", fast_threads)
        lines.append(f"{label(fixture, zones, raster, nz)} & {_fmt_s(rs)} & {_fmt_s(ee)} & {_fmt_s(f1)} & {_fmt_s(ft)} & {speed(rs, ft)} & {speed(ee, ft)} \\\\")
    lines += [r"\bottomrule", r"\end{tabular}"]
    (out_dir / "tools_pipeline.tex").write_text("\n".join(lines) + "\n", encoding="utf-8")

    lines = [r"\begin{tabular}{lllrrrrrrrr}", r"\toprule",
             r"Fixture & Zones & Raster & $n_z$ & GDAL rasterize & fastzonal rasterize (1) & fastzonal rasterize (%d) & NumPy stats & xarray-spatial stats & fastzonal stats (1) & fastzonal stats (%d) \\" % (fast_threads, fast_threads),
             r"\midrule"]
    for fixture, zones, raster, nz in cases:
        vals = [sec(fixture, zones, raster, "rasterize", "gdal", 1),
                sec(fixture, zones, raster, "rasterize", "fastzonal", 1),
                sec(fixture, zones, raster, "rasterize", "fastzonal", fast_threads),
                sec(fixture, zones, raster, "stats", "numpy", 1),
                sec(fixture, zones, raster, "stats", "xrspatial", 1),
                sec(fixture, zones, raster, "stats", "fastzonal", 1),
                sec(fixture, zones, raster, "stats", "fastzonal", fast_threads)]
        lines.append(f"{label(fixture, zones, raster, nz)} & " + " & ".join(_fmt_s(v) for v in vals) + r" \\")
    lines += [r"\bottomrule", r"\end{tabular}"]
    (out_dir / "tools_stages.tex").write_text("\n".join(lines) + "\n", encoding="utf-8")


def cmd_summarise(args) -> int:
    rows = load_rows([Path(p) for p in args.raw])
    summary = summarise(rows)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "tools_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    write_tables(summary, out / "tables", args.fast_threads)
    worst = defaultdict(float)
    for r in summary["groups"]:
        if r["tool"] != "fastzonal" and r["max_abs_diff"] is not None:
            worst[(r["stage"], r["tool"])] = max(worst[(r["stage"], r["tool"])], r["max_abs_diff"])
    for k, v in sorted(worst.items()):
        print(f"max |diff| vs fastzonal, {k[0]}/{k[1]}: {v:.3e}")
    mism = [(r["fixture"], r["zones"], r["tool"], r["n_diff_cells"]) for r in summary["groups"]
            if r["stage"] == "rasterize" and r["tool"] != "fastzonal" and r["n_diff_cells"]]
    print("mask cell differences:", mism or "none")
    print(f"wrote {out / 'tools_summary.json'} and {out / 'tables'}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--fixtures", required=True)
    r.add_argument("--cases", required=True, help="fixture:zones:raster, comma separated")
    r.add_argument("--threads", default="1", help="fastzonal thread counts, comma separated")
    r.add_argument("--reps", type=int, default=3)
    r.add_argument("--max-seconds", type=float, default=300.0, help="stop repeating a tool once one call exceeds this")
    r.add_argument("--stages", default="pipeline,rasterize,stats")
    r.add_argument("--tools", default="", help="restrict to these tools (fastzonal always runs)")
    r.add_argument("--out", required=True)
    r.set_defaults(func=cmd_run)
    s = sub.add_parser("summarise")
    s.add_argument("--raw", nargs="+", required=True)
    s.add_argument("--out", default=str(HERE.parent / "paper_assets"))
    s.add_argument("--fast-threads", type=int, default=56)
    s.set_defaults(func=cmd_summarise)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
