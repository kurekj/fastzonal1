# Benchmark results

Raw measurements as produced by `run_all.sh`:

- `raw_<stamp>.csv`: one line per repetition with the columns
  `fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells,threads_eff,band_rows,load_seconds`
  (`load_seconds` is the fixture loading time of that invocation, reported separately and never
  part of `seconds`).
  `seconds` is the wall time of the operation alone (fixture loading, denominator alignment and
  the checksum are outside it); one untimed warm-up repetition precedes the measured ones.
  `threads` is the requested count and `threads_eff` the workers the rung could use on that raster
  (the library and the optimised C# rung never use more workers than row bands; `band_rows` is
  recorded too). `checksum` is the sum of the per-zone results (for `rasterize`: number of
  assigned cells plus the sum of zone ids) and must agree with the baseline's for the same
  workload.
- `env_<stamp>.json`: machine, compiler, commit, build flags, fixture manifest hash and settings
  of that run; a resumed stamp must match it.
- `status.json`: progress of the current or last run (read with `status.sh`).

`make_paper_assets.py --raw results/raw_<stamp>.csv --out paper_assets/` turns a raw file into the
tables and figures of the manuscript and `summary.json` (medians, min, max, speedups, checksum
comparison of every repetition). Absolute times depend on the machine; checksums and speedup
ratios should not.

Rungs and precision: `csharp-baseline` is the SpongeGIS code as deployed and works on `double[,]`
rasters; every other rung reads the same fixture as `float32` (the values are float32 in the
files, so no information is lost, but the summation runs over float32 inputs promoted to double
per cell). `threads_eff` is the configured worker limit (never more than row bands), not a
measurement of active workers. Runs are pinned: `OMP_PROC_BIND=close OMP_PLACES=cores`, and every
invocation with at most one socket's worth of threads runs under `numactl --cpunodebind=0
--membind=0`; the CPU governor and the OpenMP environment are recorded in `env_<stamp>.json`.
`summary.json` lists `variability_warnings`: workloads where the serial backend and the OpenMP
backend at one thread (the same code path) differ by more than 15%.

What the timed region contains, per rung: the operation itself including its output allocation;
for the reductions, the centroid fallback for zones without a cell (the C# rungs compute those
centroids lazily with NetTopologySuite, the library rungs read the fixture's precomputed centroid
cells; both only for empty zones, so the cost is negligible either way). Fixture loading, mask
creation for the reduction ops, alignment of the ratio denominator and the checksum are outside
it. Known semantic corner: a computed ratio equal to -9999 is NoData for the reference and the
C# rungs but not for the library (see benchmarks/fixtures/FORMAT.md); no fixture contains one.
