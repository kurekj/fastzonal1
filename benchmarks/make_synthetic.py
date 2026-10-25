#!/usr/bin/env python3
"""Deterministic synthetic fixture: a smooth float32 raster with 5% NoData and N non-overlapping
rectangles as zones. Used by the unit tests and as the scaling workload that needs no download.

    python make_synthetic.py --width 300 --height 200 --zones 50 --seed 7 --out benchmarks/fixtures/synthetic_small
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from shapely.geometry import box

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fzfix import io  # noqa: E402

NODATA = -9999.0


def smooth_noise(rng: np.random.Generator, height: int, width: int) -> np.ndarray:
    coarse = rng.random((max(2, height // 16 + 2), max(2, width // 16 + 2)))
    rows = np.linspace(0, coarse.shape[0] - 1, height)
    cols = np.linspace(0, coarse.shape[1] - 1, width)
    r0 = np.floor(rows).astype(int)
    c0 = np.floor(cols).astype(int)
    r1 = np.minimum(r0 + 1, coarse.shape[0] - 1)
    c1 = np.minimum(c0 + 1, coarse.shape[1] - 1)
    fr = (rows - r0)[:, None]
    fc = (cols - c0)[None, :]
    top = coarse[r0][:, c0] * (1 - fc) + coarse[r0][:, c1] * fc
    bottom = coarse[r1][:, c0] * (1 - fc) + coarse[r1][:, c1] * fc
    return (top * (1 - fr) + bottom * fr) * 1000.0


def rectangles(rng: np.random.Generator, height: int, width: int, n: int):
    """n axis-aligned rectangles on a coarse grid, so they never overlap."""
    cells_x = int(np.ceil(np.sqrt(n * width / height)))
    cells_y = int(np.ceil(n / cells_x))
    cw, ch = width / cells_x, height / cells_y
    polys = []
    for k in range(n):
        gx, gy = k % cells_x, k // cells_x
        x0 = gx * cw + rng.uniform(0, cw * 0.2)
        y0 = gy * ch + rng.uniform(0, ch * 0.2)
        x1 = (gx + 1) * cw - rng.uniform(0, cw * 0.2)
        y1 = (gy + 1) * ch - rng.uniform(0, ch * 0.2)
        polys.append(box(x0, y0, x1, y1))
    return polys


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--zones", type=int, required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--id", default=None, help="fixture id (default: output directory name)")
    args = ap.parse_args(argv)

    rng = np.random.Generator(np.random.PCG64(args.seed))  # fixed generator, fixed stream
    raster = smooth_noise(rng, args.height, args.width).astype(np.float32)
    raster[rng.random(raster.shape) < 0.05] = NODATA
    transform = (0.0, float(args.height), 1.0, 1.0)  # unit pixels, origin top-left
    polys = rectangles(rng, args.height, args.width, args.zones)

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    io.write_raster(out / "raster_synth.json", raster, transform, NODATA,
                    {"crs": "none", "source": "make_synthetic.py", "licence": "MIT", "seed": args.seed})
    io.write_zones(out / "zones_rect.json", polys, list(range(len(polys))),
                   {"crs": "none", "source": "make_synthetic.py", "licence": "MIT", "seed": args.seed})
    io.write_centroids(out, "rect", "synth", io.centroid_cells(polys, transform))
    io.write_fixture_json(out, args.id or out.name, ["synth"], ["rect"], None,
                          f"synthetic {args.width}x{args.height}, {args.zones} rectangles, seed {args.seed}")

    # Independent check value for the harnesses: mean of zone 0 by the pixel-centre rule.
    x0, y0, x1, y1 = polys[0].bounds
    rows = np.arange(args.height)
    cols = np.arange(args.width)
    cy = transform[1] - (rows + 0.5) * transform[3]
    cx = transform[0] + (cols + 0.5) * transform[2]
    inside = (cy[:, None] >= y0) & (cy[:, None] < y1) & (cx[None, :] >= x0) & (cx[None, :] < x1)
    vals = raster[inside]
    vals = vals[vals != NODATA].astype(np.float64)
    with open(out / "check_mean_zone0.txt", "w", newline="\n") as f:
        f.write(f"{vals.mean():.17g}\n" if vals.size else "0\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
