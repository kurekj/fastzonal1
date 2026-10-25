# fastzonal: design

Date: 2026-10-25. Status: draft for the author's approval.

## 1. Intent

`fastzonal` is a small native library that computes zonal statistics of rasters over polygon
units, together with the terrain indices (slope, wetness index) that multi-criteria spatial
analyses aggregate the same way. It exists because the workloads behind opportunity mapping,
suitability analysis and catchment valorisation are dominated by one operation repeated for
dozens of layers: rasterise the planning units, sweep the raster once, reduce per unit. In
the reference application (SpongeGIS, WULS-SGGW) that operation is written in C#, runs on a
single thread per layer, ignores polygon holes and holds every raster as `double[,]`; the
terrain stage is delegated to SAGA GIS in a separate container and takes 32 minutes for a
62-million-cell DEM. The library replaces those kernels with a C++17 core that has serial,
OpenMP and CUDA backends, exposes a C ABI, and ships bindings for .NET and Python.

The library is the subject of a SoftwareX paper. The paper's title is generic (the library,
not SpongeGIS): working title *fastzonal: a parallel C++ library for zonal statistics and
terrain indices over polygon units, with .NET and Python bindings*. SpongeGIS is the
illustrative example and the source of real workloads; it is not in the title.

## 2. Goals

- G1. Zonal statistics (count, sum, mean, min, max, standard deviation, majority) of a
  raster over N polygon zones, for `float32`, `float64`, `int32` and `uint8` rasters, with a
  NoData value and NaN handling.
- G2. Deterministic polygon rasterisation to a zone-index mask (`int32`, -1 outside), pixel-
  centre rule, exterior rings and holes, multipolygons, identical result on any thread count.
- G3. Two-raster ratio aggregated per zone (mean of cell ratios on a common grid), with
  nearest-neighbour regridding of the coarser raster, as SpongeGIS's `twoRasterToSPU` needs.
- G4. Terrain indices from a DEM: slope (Horn 3x3, degrees), sink filling (Priority-Flood),
  D8 flow accumulation and the topographic wetness index, each reducible per zone with G1.
- G5. Three backends behind one API: serial reference, OpenMP, CUDA. The backend is chosen
  at run time; results are bit-identical between serial and OpenMP, and within a documented
  floating-point tolerance for CUDA (summation order).
- G6. A C ABI (`fastzonal.h`), a .NET binding (`FastZonal` class library, P/Invoke over
  `Span<T>`), and a Python binding (`fastzonal` package, ctypes over NumPy arrays). The
  library itself does no file I/O; readers (GDAL) live in the CLI, the benchmarks and the
  host application.
- G7. A benchmark suite that reproduces every number in the paper from the command line.
- G8. A release the author can tag as v1.0.0 with tests, CI, README, DEVELOPMENT.md,
  CITATION.cff and MIT licence.

## 3. Non-goals and anti-goals

Non-goals (explicitly out of v1.0):
- Vector-to-zone statistics (line density, polygon area share, buffers). These stay in the
  host application on GEOS/NetTopologySuite. The paper says so.
- Exact area-weighted zonal statistics (the `exactextract` semantics). v1.0 uses the
  pixel-centre rule, because the reference application does and parity with it is a
  requirement. Area weighting is listed as future work.
- Opportunity-map lookup logic, SPU generation, web UI, databases.
- Multi-GPU and distributed execution.

Anti-goals (would make the paper fail even if the code works):
- A C# vs C++ comparison where the two implement different algorithms. The ladder in
  section 7 exists to separate algorithmic gains from language and hardware gains.
- Claims beyond the measured configurations. Every speedup is reported with the dataset,
  thread count, hardware and repetition count it was measured on.
- Any mention of AI tools in code, commits, documentation or manuscript; em dashes anywhere.

## 4. Constraints

- Build: C++17, CMake >= 3.20, GCC >= 11 or MSVC 2022; OpenMP; optional CUDA >= 12 behind
  `FASTZONAL_WITH_CUDA`. GDAL is optional and only for the CLI and benchmarks.
- deep3 (the benchmark machine): Ubuntu 26.04, 2x Xeon Gold 5420+ (112 threads), 1 TB RAM,
  2x NVIDIA L40S (driver 595.91, CUDA 13.2 capable), no sudo. CUDA toolkit, GDAL, GEOS and
  the Python comparison tools are installed through conda (miniforge is present).
- .NET 10 SDK is on deep3; the .NET binding and its tests target `net10.0` (LTS), the same as SpongeGIS, so one SDK serves the build, CI, Docker and reviewers.
- Python >= 3.11, NumPy >= 1.26; no compiled Python extension (ctypes), so a wheel is the
  shared library plus pure Python.
- The author does every git commit, tag and push; the work here supplies the commands.
- Repository: `https://github.com/kurekj/fastzonal1` (author's choice; renaming to
  `fastzonal` before the first tag is recommended so the permanent link in the paper's
  metadata table is clean).

## 5. Architecture

```
fastzonal1/
  include/fastzonal/fastzonal.h      C ABI (stable), versioned
  src/core/                          C++17: rasterize, zonal, ratio, terrain
  src/backends/serial/               reference implementations
  src/backends/openmp/               parallel implementations
  src/backends/cuda/                 .cu kernels (optional)
  src/cli/                           `fastzonal` CLI (GDAL in, CSV out)
  bindings/dotnet/FastZonal/         class library + xUnit tests
  bindings/python/fastzonal/         ctypes package + pytest
  benchmarks/                        drivers, datasets manifest, results/, plots
  tests/                             Catch2 unit tests and parity fixtures
  docs/                              DEVELOPMENT.md, API reference, paper notes
  .github/workflows/ci.yml           Linux + Windows build and tests
```

### 5.1 Core API (C ABI, sketch)

```c
typedef struct { double origin_x, origin_y, pixel_w, pixel_h; } fz_transform;   /* north-up */
typedef struct { int32_t width, height; } fz_shape;

/* Polygons as a flat coordinate array with ring offsets, zone index per polygon.
   Holes follow their exterior ring; ring_is_hole marks them. */
int fz_rasterize(const double* xy, const int64_t* ring_offsets, const uint8_t* ring_is_hole,
                 const int32_t* ring_zone, int64_t n_rings,
                 fz_transform t, fz_shape s, int32_t* mask_out, fz_options* opt);

int fz_zonal_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones,
                 float nodata, fz_stats* stats_out /* count,sum,mean,min,max,std */, fz_options* opt);
int fz_zonal_majority_f32(...);                 /* mode with smallest-value tie break */
int fz_zonal_ratio_f32(const float* num, fz_transform tn, fz_shape sn,
                       const float* den, fz_transform td, fz_shape sd, ...);  /* regrid + divide + mean */

int fz_slope_horn_f32(const float* dem, fz_shape s, double dx, double dy, float nodata, float* out, fz_options*);
int fz_fill_sinks_f32(...);                     /* Priority-Flood (Barnes et al. 2014) */
int fz_flow_accum_d8_f32(...);
int fz_twi_f32(...);                            /* ln(a / tan(beta)) with the SAGA convention */

typedef struct { int backend; /* 0 auto, 1 serial, 2 openmp, 3 cuda */ int threads; int device; } fz_options;
const char* fz_version(void); int fz_has_cuda(void);
```

Centroid fallback for zones that receive no pixel centre (zone smaller than one cell) is an
option on `fz_zonal_*`, with centroid cells supplied by the caller, because the reference
application relies on it.

### 5.2 Algorithms

- Rasterisation: scanline with pixel-centre test, per polygon bounding box, parallel over
  horizontal bands; within a band polygons are processed in index order, so "last polygon
  wins" on shared boundary pixels exactly as the serial loop does. Holes are filled after
  their exterior ring with the "outside" value of the same polygon pass.
- Zonal reduction: one pass over the raster. OpenMP: each thread owns a private accumulator
  array (n_zones x stats), merged at the end; for n_zones above a threshold the pass is
  tiled by rows and accumulators are merged pairwise to stay cache resident. SIMD comes
  from the compiler on the inner loop (no intrinsics in v1.0). Majority uses a compact
  per-zone hash of value -> count for float rasters and a dense histogram for `uint8`.
- CUDA: mask and raster uploaded once per session, reductions with shared-memory histograms
  and atomics for small n_zones, segmented reduction after a counting sort of pixels by
  zone for large n_zones. Slope is a straightforward stencil kernel. Flow accumulation
  stays on the CPU in v1.0 (its dependency structure does not map well to a GPU and the
  paper should not claim otherwise).
- Terrain: Horn slope as in GDAL `gdaldem slope`; Priority-Flood sink filling; D8 flow
  directions and accumulation by topological order; TWI = ln((a + cell) / tan(beta)), with
  SAGA's wetness-index convention documented as the comparison target.

### 5.3 Bindings

- .NET: `FastZonal.Rasterizer`, `FastZonal.Zonal`, `FastZonal.Terrain` static classes over
  `ReadOnlySpan<T>`; native library resolved with `NativeLibrary.SetDllImportResolver`;
  packaged as a NuGet-style project with `runtimes/linux-x64`, `runtimes/win-x64`.
- Python: `fastzonal.rasterize(polygons, transform, shape)`, `fastzonal.zonal(raster,
  mask, n_zones, nodata=..., stats=(...))`, `fastzonal.terrain.slope/twi`, accepting NumPy
  arrays and shapely geometries (shapely optional, used only to flatten rings).

## 6. Parity and tests

- Parity fixtures: ten rasters and zone sets exported from real SpongeGIS sessions on deep3
  (Upper Biebrza 863 km2, Leze 17 km2, Evenlode 16 km2, Gradascica 160 km2, one 5,000 km2
  area), plus synthetic grids. For each, the C# baseline output (mean, majority, ratio,
  slope-mean) is stored as the expected result; serial and OpenMP must match bit for bit,
  CUDA within 1e-6 relative.
- Terrain parity: slope against `gdaldem slope`; TWI against SAGA 9.8 `ta_hydrology` on the
  same DEM, reported as correlation and zonal-mean difference, since conventions differ.
- Unit tests: Catch2 (core), xUnit (.NET), pytest (Python). CI runs all three on Linux and
  the core plus .NET on Windows; CUDA is built but not run in CI.

## 7. Benchmark design (the paper's evidence)

Implementations on the ladder, all computing the same mean/majority/ratio per zone:

1. `csharp-baseline`: the SpongeGIS code copied verbatim into a console harness.
2. `csharp-optimised`: same algorithm, `Span<T>`, `float`, `Parallel.For` over row bands with
   thread-local accumulators. Shows what C# alone can do.
3. `fastzonal-serial`, 4. `fastzonal-openmp` at 1, 2, 4, 8, 16, 32, 56, 112 threads,
5. `fastzonal-cuda` on one L40S.
6. External tools on the same inputs: `exactextract` (C++, area-weighted, also run in its
   centre-point mode where available), `rasterstats` (Python), `xarray-spatial` zonal,
   `gdaldem slope`, SAGA `ta_hydrology` for TWI.

Axes: number of zones (grids of 1,000 / 500 / 250 / 100 m over the same areas, about 1e3 to
1e6 zones), raster size (dem120 / dem60 / dem30 extracts, 1e6 to 3.5e8 cells), thread
count. Each point is the median of five runs with min and max; wall time measured inside the
driver, excluding file reading, with the reading time reported separately. One end-to-end
SpongeGIS session timing (managed vs fastzonal engine) for three catchments, taken from the
application's own phase logs.

Everything runs on deep3 under `tmux`, from `benchmarks/run_all.sh`, writing
`benchmarks/results/*.csv` and a `summary.json`; `benchmarks/make_paper_assets.py` turns
those into the tables and figures the manuscript inputs, so the manuscript contains no
hand-typed measurement.

## 8. Integration with SpongeGIS

Out of scope for this repository but required for the end-to-end measurement: a
`Calculation:Engine` setting in SpongeGIS (`managed` | `fastzonal`) that routes
`RasterizePolygons`, `ComputeMeanRasterValueBatch`, `PerSpuMajority`, `TwoRasterRatioBySpu`
and the slope cache through the `FastZonal` binding. The SpongeGIS change is a separate
branch in the SpongeGIS repository and is described, not shipped, in the paper.

## 9. Decisions already taken by the author

- The paper is about a new generic library; title in the `name: subtitle` form with the
  library name first.
- Rewrite the compute kernels in C++ and compare with the C# implementation.
- Repository created by the author: `kurekj/fastzonal1`.
- Benchmarks on deep3, detached from the SSH session, with status and ETA every 5 minutes.

## 10. Calls made in this design (the author can overturn them)

- Library name `fastzonal` (checked free on GitHub, PyPI and NuGet on 2026-10-25).
- CUDA backend included, limited to zonal reductions and slope; flow accumulation CPU only.
- Python binding through ctypes rather than pybind11, to keep the wheel dependency-free.
- Pixel-centre semantics with parity to the reference application; area weighting deferred.
- C ABI as the stable surface; C++ classes are internal.
- Catch2 for C++ tests.
- `exactextract`, `rasterstats`, `xarray-spatial`, GDAL and SAGA as the external comparison
  set; QGIS and ArcGIS zonal tools are cited but not benchmarked.

## 11. Left to the builder

- Tiling thresholds, accumulator layout and the large-n_zones strategy on CUDA.
- Exact CMake layout, packaging of the native library for .NET and Python.
- Benchmark driver structure, as long as section 7 is reproducible from one script.
- Order of implementation, as long as parity tests exist before any benchmark number is
  quoted.

## 12. Decisions after the builder check (2026-10-25)

An independent reader of this document checked it against the SpongeGIS source. These are the
calls made in response; they refine sections 2, 5 and 6.

- Accumulators are `double` for every raster type; `fz_stats` holds doubles. Serial and
  OpenMP backends sum in the same fixed order (row bands of a fixed height, partials merged
  in band order), so they are bit-identical. The C# baseline sums sequentially in a different
  order, so parity against it is asserted at a relative tolerance of 1e-9 per zone, not bit
  for bit. CUDA: 1e-6 relative.
- Rasterisation has two modes selected in `fz_options`: `FZ_RASTER_CENTER` (default: pixel
  centre test on rows and columns, holes honoured, multipolygons) and `FZ_RASTER_COMPAT`
  (reproduces the reference rule exactly: exterior ring only, row centre test, column fill from
  the pixel containing the left crossing to the one containing the right crossing inclusive,
  truncating `int` casts in world-to-pixel). Parity fixtures run in compat mode; the paper
  reports the default semantics and states the compat mode exists for migration.
- Two-raster ratio: the library does not warp. `fz_zonal_ratio` takes two rasters already on
  one grid, divides cell by cell (NoData or |den| < 1e-12 excluded) and reduces. The caller
  aligns them (SpongeGIS keeps its GDAL bilinear warp onto the intersection extent at the finer
  resolution; the CLI and benchmarks use the same GDAL call). A nearest-neighbour regrid helper
  is provided for callers without GDAL but is not on the parity path.
- Terrain: slope follows `gdaldem slope` (Horn, `-compute_edges` semantics on the border) and is
  parity-tested against it. The wetness index deliverable depends on the author's answer to the
  first open question below; until then the plan budgets for the SAGA Wetness Index
  (Boehner's modified catchment area after Wang and Liu sink filling, `SLOPE_OFF 0.1`,
  `SLOPE_WEIGHT 1`, `SLOPE_MIN 0`), parity-tested against SAGA 9.8 on the same DEM, because
  that is what the 32-minute production stage computes and the application's domain expert
  rejected plain D8 TWI in September 2026.
- CUDA toolkit on deep3 comes from conda (`nvidia` channel); the builder picks the newest
  toolkit whose host-compiler support matches an available GCC (conda can supply GCC 13 if
  GCC 15 is not supported). CMake minimum stays 3.20.
- .NET binding and tests target `net10.0` (author's decision: use the newest LTS, one SDK everywhere).
- Benchmark fixtures are built only from openly licensed layers (GEDTM30 terrain, CORINE
  land cover, Copernicus imperviousness, ERA5-Land derived rasters) clipped to public catchment
  outlines, plus a synthetic generator (`benchmarks/make_synthetic.py`) for the scaling axes.
  User-uploaded partner data (national DTMs, local soil and land-use maps) never leaves deep3
  and is not used for any published number. Fixture archives are published with a DOI
  (Zenodo, pending the author's approval) and fetched by `benchmarks/fetch_fixtures.sh`.

## 13. Author's decisions (2026-10-25, closing the open questions)

- Work continues in `kurekj/fastzonal1`; the repository is renamed to `fastzonal` at the end,
  before the tag that the paper cites. All work goes to `main`, no feature branches.
- Benchmark and fixture data: public layers only.
- Reviewers must be able to reproduce everything. This is a hard requirement: pinned
  environments (conda lock file and a Dockerfile for the CPU path), fixtures fetched by script
  from a DOI, one command per benchmark table, and a final independent verification pass by
  Codex that follows the README on a clean machine and checks every number in the manuscript
  against `benchmarks/results/`.
- Everything else is left to the builder. The builder's resulting calls:
  - Wetness index: option (a), the SAGA Wetness Index, so the SAGA stage can be replaced.
    Classic D8 TWI is also exposed because it is cheap once flow accumulation exists.
  - SpongeGIS integration: a `Calculation:Engine` setting on SpongeGIS `main`, default
    `managed`, so production is unchanged until the author flips it; used for the end-to-end
    timing on deep3 against a copy of the deployed build, never against the live service.
  - The `csharp-baseline` rung is published in this repository under MIT with a header noting
    it is a verbatim extract of the author's own SpongeGIS code (2026-09-26, commit 8028b95f).
  - CUDA backend: yes, scoped as in section 5.2.

## 14. Open questions for the author (remaining)

- ORCID and affiliation text for Dorota Puslowska-Tyszewska, and the affiliation text for
  the Institute of Environmental Engineering co-authors.
- Which three catchments for the end-to-end timing (design assumes Upper Biebrza, Leze,
  Gradascica).
