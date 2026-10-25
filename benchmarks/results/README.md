# Benchmark results

Raw measurements as produced by `run_all.sh`:

- `raw_<stamp>.csv`: one line per repetition with the columns
  `fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells`.
  `seconds` is the wall time of the operation alone (fixture loading, denominator alignment and
  the checksum are outside it); one untimed warm-up repetition precedes the measured ones.
  `threads` is the thread count the rung actually used. `checksum` is the sum of the per-zone
  results (for `rasterize`: number of assigned cells plus the sum of zone ids) and must agree
  with the baseline's for the same workload.
- `env_<stamp>.json`: machine, compiler, commit and flags of that run.
- `status.json`: progress of the current or last run (read with `status.sh`).

  `threads` is the requested count and `threads_eff` the workers the rung could use on that raster
  (the library and the optimised C# rung never use more workers than row bands; `band_rows`
  is recorded too).

`make_paper_assets.py --raw results/raw_<stamp>.csv --out paper_assets/` turns a raw file into the
tables and figures of the manuscript and `summary.json` (medians, min, max, speedups, checksum
comparison of every repetition). Absolute times depend on the machine; checksums and speedup
ratios should not.

What the timed region contains, per rung: the operation itself including its output allocation;
for the reductions, the centroid fallback for zones without a cell (the C# rungs compute those
centroids lazily with NetTopologySuite, the library rungs read the fixture's precomputed centroid
cells; both only for empty zones, so the cost is negligible either way). Fixture loading, mask
creation for the reduction ops, alignment of the ratio denominator and the checksum are outside
it. Known semantic corner: a computed ratio equal to -9999 is NoData for the reference and the
C# rungs but not for the library (see benchmarks/fixtures/FORMAT.md); no fixture contains one.
