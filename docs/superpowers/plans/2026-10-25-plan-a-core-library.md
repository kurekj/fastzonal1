# fastzonal Plan A: core library, parity fixtures, first benchmarks

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A buildable, tested `fastzonal` core (polygon rasterisation, zonal statistics, two-raster ratio) with serial and OpenMP backends, a C ABI, a .NET binding, public-data fixtures with C# baseline expected outputs, and a benchmark harness that produces the first scaling tables on deep3.

**Architecture:** C++17 shared library behind a C header; algorithms are templates in `src/core`, backends differ only in how row bands are scheduled and merged (fixed band order, so serial and OpenMP are bit-identical). All I/O stays outside the library: fixtures are raw little-endian binaries with JSON headers, produced once from GeoTIFF and shapefiles by a Python script, and read by the C++, C# and Python harnesses alike.

**Tech Stack:** C++17, CMake 3.20+, OpenMP, Catch2 v3 (FetchContent), .NET 10 class library and xUnit tests (one SDK everywhere), Python 3.11 with numpy, rasterio, pyogrio, shapely (conda), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-25-fastzonal-design.md` (sections 5, 6, 7, 12, 13).

## Global Constraints

- Repository `https://github.com/kurekj/fastzonal1`, everything on `main`, no feature branches. Rename to `fastzonal` only at the end (spec 13).
- Licence MIT; `csharp-baseline` files carry the header "Verbatim extract of the author's SpongeGIS code, SPONGEGIS2026.NET commit 8028b95f (2026-09-26), relicensed MIT by the author" (spec 13).
- No mention of AI tools anywhere: code, comments, commits, docs. No em dashes anywhere (spec 3). Commit messages are plain `type: summary` lines with no trailers.
- The author pushes; the implementer commits locally only.
- Accumulators are `double` for every raster type. Serial and OpenMP bit-identical; C# baseline vs fastzonal within relative 1e-9 per zone (spec 12).
- Rasterisation modes `FZ_RASTER_CENTER` (default) and `FZ_RASTER_COMPAT` (spec 12). NoData: the given value, and NaN always (spec 5.1, matches C# `IsNaN || == -9999`).
- Fixture data: public layers only (GEDTM30 terrain CC BY 4.0, CORINE Land Cover 2018, Copernicus Imperviousness 2018, HydroBASINS CC BY 4.0 boundaries). No partner-uploaded data (spec 12, 13).
- Reproducibility: every number comes from `benchmarks/run_all.sh` output; pinned conda `environment.yml` plus `conda-lock.yml`; Dockerfile for the CPU path (spec 13).
- deep3 work runs in `/data/fastzonal/` under `tmux`; never touches `spongegis.service`, the PostGIS container contents (read-only queries) or `/data/SpongegisStorage`.
- Build type for all measurements: Release, `-O3`, `FASTZONAL_NATIVE_ARCH=ON` on deep3 (documented in results metadata), OFF in CI.

## Review Focus

1. A polygon whose bounding box lies partly or wholly outside the raster must rasterise the inside part and never write out of bounds (test in Task 3).
2. A raster that is all NoData, or a zone that receives no pixel, must yield count 0 and a mean of 0 with the centroid fallback semantics, never NaN or a crash (Task 4).
3. Zone ids in the mask outside `[-1, n_zones)` must return `FZ_ERR_ARG` rather than index memory (Task 4).
4. `threads` larger than the number of row bands, or a raster with fewer rows than `band_rows`, must still produce bit-identical output (Task 5).
5. A fixture whose raster and polygons use different CRSs must be rejected by the prepare script with a clear message, since the library has no CRS awareness (Task 6).

---

### Task 1: Repository skeleton, CMake build, first test, CI

**Files:**
- Create: `LICENSE`, `README.md`, `.gitignore`, `CMakeLists.txt`, `cmake/fastzonalConfig.cmake.in`, `include/fastzonal/fastzonal.h`, `src/core/version.cpp`, `tests/CMakeLists.txt`, `tests/test_version.cpp`, `.github/workflows/ci.yml`, `CMakePresets.json`

**Interfaces:**
- Produces: shared library target `fastzonal` (output `libfastzonal.so` / `fastzonal.dll`), CMake options `FASTZONAL_BUILD_TESTS` (ON), `FASTZONAL_WITH_OPENMP` (ON), `FASTZONAL_WITH_CUDA` (OFF), `FASTZONAL_NATIVE_ARCH` (OFF); exported symbols use `FZ_API` (`__declspec(dllexport)` on MSVC, default visibility elsewhere); `const char* fz_version(void)` returns `"0.1.0"`; `int fz_has_openmp(void)`; `int fz_has_cuda(void)`.

- [ ] **Step 1: Write the failing test** in `tests/test_version.cpp` (Catch2 v3, fetched by `tests/CMakeLists.txt` with `FetchContent` pinned to tag `v3.7.1`):

```cpp
TEST_CASE("version string") { REQUIRE(std::string(fz_version()) == "0.1.0"); }
TEST_CASE("openmp flag matches build") {
#ifdef _OPENMP
    REQUIRE(fz_has_openmp() == 1);
#else
    REQUIRE(fz_has_openmp() == 0);
#endif
}
```

- [ ] **Step 2: Configure and build; expect the test to fail to compile** (no header yet):
Run: `cmake --preset release && cmake --build --preset release`
Expected: error, `fastzonal.h` not found.

- [ ] **Step 3: Create the header with only version functions, `version.cpp`, the top-level `CMakeLists.txt`** (project `fastzonal` VERSION 0.1.0 LANGUAGES C CXX; `add_library(fastzonal SHARED ...)`; `target_include_directories` PUBLIC `include`; OpenMP via `find_package(OpenMP)` linked when `FASTZONAL_WITH_OPENMP`; `-O3` in Release; `-march=native` when `FASTZONAL_NATIVE_ARCH`; `install` rules), `CMakePresets.json` with presets `release` and `debug` (generator Ninja if available, otherwise default), `.gitignore` (`build/`, `bin/`, `obj/`, `.venv/`, `*.user`, `benchmarks/results/*.csv` is NOT ignored).

- [ ] **Step 4: Build and run tests**
Run: `cmake --build --preset release && ctest --preset release --output-on-failure`
Expected: 2 tests passed.

- [ ] **Step 5: CI** `.github/workflows/ci.yml`: jobs `linux` (ubuntu-latest, gcc, OpenMP ON) and `windows` (windows-latest, MSVC, OpenMP ON); steps: checkout, configure Release, build, `ctest --output-on-failure`, upload `build/Testing` on failure. No CUDA in CI.

- [ ] **Step 6: `LICENSE` (MIT, copyright 2026 Jarosław Kurek), `README.md`** (name, one-paragraph purpose, build commands from Step 4, "status: pre-release"), commit:

```bash
git add -A && git commit -m "build: cmake skeleton, version API, catch2 test, ci"
```

---

### Task 2: C ABI header: types, options, errors

**Files:**
- Modify: `include/fastzonal/fastzonal.h`
- Create: `src/core/options.cpp`, `tests/test_options.cpp`

**Interfaces:**
- Produces (exact, copied by every later task):

```c
typedef struct fz_transform { double origin_x; double origin_y; double pixel_w; double pixel_h; } fz_transform;
/* north-up: x(col) = origin_x + (col + 0.5) * pixel_w ; y(row) = origin_y - (row + 0.5) * pixel_h ; pixel_h > 0 */
typedef struct fz_shape { int32_t width; int32_t height; } fz_shape;
typedef enum fz_backend { FZ_BACKEND_AUTO = 0, FZ_BACKEND_SERIAL = 1, FZ_BACKEND_OPENMP = 2, FZ_BACKEND_CUDA = 3 } fz_backend;
typedef enum fz_raster_mode { FZ_RASTER_CENTER = 0, FZ_RASTER_COMPAT = 1 } fz_raster_mode;
typedef struct fz_options { int backend; int threads; int device; int raster_mode; int band_rows; } fz_options;
/* defaults: AUTO, 0 (= all hardware threads), 0, FZ_RASTER_CENTER, 64 */
typedef struct fz_stats { int64_t count; double sum; double sumsq; double min; double max; } fz_stats;
enum { FZ_OK = 0, FZ_ERR_ARG = 1, FZ_ERR_BACKEND = 2, FZ_ERR_NOMEM = 3, FZ_ERR_INTERNAL = 4 };
FZ_API void fz_options_default(fz_options* opt);
FZ_API const char* fz_error_string(int code);
FZ_API double fz_stats_mean(const fz_stats* s);   /* 0.0 when count == 0 */
FZ_API double fz_stats_std(const fz_stats* s);    /* population std, 0.0 when count < 2 */
```

- [ ] **Step 1: Failing tests** in `tests/test_options.cpp`: `fz_options_default` yields `{0,0,0,0,64}`; `fz_error_string(FZ_ERR_ARG)` equals `"invalid argument"`; `fz_error_string(99)` equals `"unknown error"`; `fz_stats_mean` of `{count 4, sum 10}` is 2.5 and of `{count 0}` is 0.0; `fz_stats_std` of values 2,4,4,4,5,5,7,9 (count 8, sum 40, sumsq 232) is 2.0.
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Add the declarations above to the header (inside `extern "C"`, with `#include <stdint.h>`) and implement in `src/core/options.cpp`.** Std uses `sqrt(max(0, sumsq/count - mean*mean))`.
- [ ] **Step 4: Run tests, expect pass. Commit** `feat: c abi types, options and error strings`.

---

### Task 3: Polygon rasterisation (serial, both modes)

**Files:**
- Create: `src/core/rasterize.hpp`, `src/core/rasterize.cpp`, `tests/test_rasterize.cpp`
- Modify: `include/fastzonal/fastzonal.h`

**Interfaces:**
- Produces:

```c
/* xy: n_points * 2 doubles (x0,y0,x1,y1,...). ring_start: n_rings + 1 offsets into points; ring i is
   points [ring_start[i], ring_start[i+1]). Rings need not be closed. ring_is_hole[i]: 1 = hole of the
   nearest preceding exterior ring. ring_zone[i]: zone index written for the ring's polygon (0..n_zones-1);
   holes carry the same zone as their exterior. mask_out: height*width int32, row-major, written fully
   (-1 outside). Polygons are processed in ring order; later polygons overwrite earlier ones. */
FZ_API int fz_rasterize(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                        const int32_t* ring_zone, int64_t n_rings, fz_transform t, fz_shape s,
                        int32_t* mask_out, const fz_options* opt);
```
- Internal (used by Task 5): `namespace fz { void rasterize_band(const PolygonSet&, const fz_transform&, const fz_shape&, int32_t row0, int32_t row1, int mode, int32_t* mask); }` where `PolygonSet` groups rings per polygon with bounding boxes and is built once by `build_polygon_set(...)`.

Semantics to implement exactly:
- CENTER mode: a pixel belongs to a ring if its centre `(x(col), y(row))` is inside by the even-odd rule along the row scanline; crossings at `y(row)` computed with the half-open rule `(y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy)`; a span `[xa, xb)` covers columns whose centre satisfies `xa <= x(col) < xb`. Holes are applied after their exterior ring within the same polygon pass and reset covered pixels to the value they had before the polygon (so an overlapping earlier polygon shows through a hole).
- COMPAT mode: exterior rings only (holes ignored); row test as above; columns filled from `trunc((xa - origin_x)/pixel_w)` to `trunc((xb - origin_x)/pixel_w)` inclusive, clamped to the ring's bounding-box columns and to the raster; crossings sorted ascending and paired. This reproduces `RasterizeOnePolygon` in the reference.

- [ ] **Step 1: Failing tests** in `tests/test_rasterize.cpp`, raster 10x10, transform `{0, 10, 1, 1}` (unit pixels, origin top-left at (0,10)):
  - `square_center_mode`: square (2,2)-(5,5) (exclusive top-right): CENTER marks exactly columns 2..4 of rows 5..7 (pixel centres 2.5..4.5), count 9.
  - `square_compat_mode`: same square: COMPAT marks columns 2..5 and rows 5..7 (right crossing at x=5 truncates to column 5), count 12.
  - `triangle_partial_pixels`: triangle (0,0),(10,0),(0,10): CENTER count equals number of centres with x+y < 10 (= 45); COMPAT count 55.
  - `hole_shows_previous`: polygon A square (0,0)-(10,10) zone 0, polygon B square (2,2)-(8,8) with hole (4,4)-(6,6) zone 1: CENTER pixels of the hole read 0 (A shows through), pixels of B's ring read 1.
  - `later_polygon_wins`: two overlapping squares, zone 0 then 1: overlap pixels read 1.
  - `bbox_outside_raster`: square (-5,-5)-(3,3): only in-raster pixels written, no crash (run under ASan in CI debug preset).
  - `multipolygon_two_rings_same_zone`: two disjoint exterior rings with `ring_zone` 7 both: count equals sum of both.
  - `errors`: null `xy`, `n_rings < 0`, `s.width <= 0`, zone < 0 return `FZ_ERR_ARG`.
- [ ] **Step 2: Run, expect failures (symbol missing).**
- [ ] **Step 3: Implement `build_polygon_set`, `rasterize_band`, `fz_rasterize`** (serial loop over all rows as a single band; per polygon iterate its bbox rows only; crossings in a small reusable `std::vector<double>` per thread).
- [ ] **Step 4: Run tests, expect pass. Commit** `feat: polygon rasterisation, center and compat modes`.

---

### Task 4: Zonal statistics, majority, ratio, centroid fallback (serial)

**Files:**
- Create: `src/core/zonal.hpp`, `src/core/zonal.cpp`, `src/core/majority.cpp`, `tests/test_zonal.cpp`
- Modify: `include/fastzonal/fastzonal.h`

**Interfaces:**
- Produces (f32 and f64 variants of each; `_i32` for majority as well):

```c
FZ_API int fz_zonal_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones,
                        float nodata, fz_stats* out /* n_zones, fully written */, const fz_options* opt);
FZ_API int fz_zonal_f64(const double* raster, ..., double nodata, fz_stats* out, const fz_options* opt);
/* mean per zone with the reference fallback: sum/count when count > 0, else raster[centroid_row, centroid_col]
   when in range and not NoData, else 0.0. centroid arrays may be NULL (then 0.0). */
FZ_API int fz_zonal_mean_fallback_f32(const float* raster, fz_shape s, float nodata, const fz_stats* stats,
                                      int32_t n_zones, const int32_t* centroid_row, const int32_t* centroid_col,
                                      double* mean_out);
FZ_API int fz_zonal_mean_fallback_f64(...);
/* mode per zone; ties resolve to the smallest value; zones without pixels use the centroid fallback, else 0 and
   has_value[z] = 0 */
FZ_API int fz_zonal_majority_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones, float nodata,
                                 const int32_t* centroid_row, const int32_t* centroid_col,
                                 double* value_out, uint8_t* has_value_out, const fz_options* opt);
FZ_API int fz_zonal_majority_f64(...); FZ_API int fz_zonal_majority_i32(...);
/* cell-wise num/den on one grid, cells NoData in either or |den| < eps excluded, then zonal stats of the ratio */
FZ_API int fz_zonal_ratio_f32(const float* num, const float* den, const int32_t* mask, fz_shape s, int32_t n_zones,
                              float nodata_num, float nodata_den, double eps, fz_stats* out, const fz_options* opt);
FZ_API int fz_zonal_ratio_f64(...);
```
- Internal (for Task 5): templates `template<class T> void zonal_band(const T*, const int32_t* mask, fz_shape, int32_t row0, int32_t row1, T nodata, fz_stats* acc)` and `void stats_merge(fz_stats* into, const fz_stats* from, int32_t n)`; majority uses `std::unordered_map<double,int64_t>` per zone in a `std::vector`, merged by adding counts.
- NoData rule everywhere: `v == nodata || isnan(v)` (the reference also treats -9999 as NoData; callers pass -9999 when the file has no NoData tag).

- [ ] **Step 1: Failing tests** in `tests/test_zonal.cpp` on a 4x4 raster, mask with zones 0,1 and -1:
  - `mean_sum_count`: exact values from a hand-written raster.
  - `nodata_and_nan_skipped`: a NoData cell and a NaN cell inside zone 0 do not change count.
  - `empty_zone_fallback_centroid`: zone 1 has no mask pixels; `fz_zonal_mean_fallback_f32` returns `raster[centroid]`; with centroid out of range returns 0.0; with centroid on NoData returns 0.0.
  - `all_nodata_raster`: every zone count 0, mean 0.0, std 0.0, no NaN in `out`.
  - `invalid_zone_id`: mask containing 5 with `n_zones 2`, or -2, returns `FZ_ERR_ARG` and leaves `out` untouched.
  - `majority_tie_smallest`: zone with values {3,3,1,1} returns 1.0.
  - `majority_empty_zone_fallback`: as above for majority, `has_value` 0 when no fallback.
  - `ratio_excludes_small_denominator`: den 1e-13 cells excluded; mean equals mean of remaining ratios.
  - `f64_matches_f32_for_exact_values`: integers up to 2^20 give identical sums.
- [ ] **Step 2: Run, expect failures.**
- [ ] **Step 3: Implement** serial versions (single band over all rows, `stats` initialised to count 0, sum 0, sumsq 0, min +inf, max -inf; after accumulation zones with count 0 get min = max = 0.0). Mask validation is a separate full pass before accumulation (cheap, and it keeps the hot loop branch-light).
- [ ] **Step 4: Run tests, expect pass. Commit** `feat: zonal stats, majority, ratio, centroid fallback`.

---

### Task 5: OpenMP backend with fixed band order

**Files:**
- Create: `src/backends/openmp/openmp_backend.cpp`, `src/backends/serial/serial_backend.cpp`, `src/core/dispatch.hpp`, `src/core/dispatch.cpp`, `tests/test_parallel.cpp`
- Modify: `src/core/rasterize.cpp`, `src/core/zonal.cpp`, `src/core/majority.cpp` (route through dispatch)

**Interfaces:**
- Internal: `namespace fz { struct Plan { int backend; int threads; int band_rows; int32_t n_bands; }; Plan resolve(const fz_options*, fz_shape); }` with `backend AUTO` resolving to OPENMP when built with it and `threads != 1`, else SERIAL; `FZ_BACKEND_CUDA` returns `FZ_ERR_BACKEND` in this plan.
- Semantics: rows are split into bands of `band_rows` rows (last band shorter). Rasterisation: bands are independent (each band processes every polygon whose bbox touches it, in polygon order), so any schedule is bit-identical. Zonal: each band accumulates into its own `fz_stats[n_zones]` (or majority map); bands are merged in ascending band index on one thread. This is the fixed order that makes serial and OpenMP identical: the serial backend uses the same band split and the same merge.

- [ ] **Step 1: Failing tests** in `tests/test_parallel.cpp` using a 1000x1300 synthetic float raster (deterministic LCG values, 5% NoData) with 2,000 random rectangles as zones:
  - `serial_equals_openmp_bitwise` for threads 1, 2, 7, 64 and `band_rows` 1, 64, 5000 (> height): `memcmp` of `fz_stats` arrays is 0 and masks identical.
  - `majority_serial_equals_openmp` likewise.
  - `cuda_backend_not_available` returns `FZ_ERR_BACKEND`.
- [ ] **Step 2: Run, expect failures.**
- [ ] **Step 3: Implement dispatch and the two backends** (`#pragma omp parallel for schedule(dynamic,1) num_threads(plan.threads)` over bands; per-band accumulators in a flat `std::vector<fz_stats>(n_bands * n_zones)` allocated once; merge loop in band order; the serial backend reuses the exact same code path with the pragma disabled).
- [ ] **Step 4: Run tests, expect pass (also under the `debug` preset with `-fsanitize=address,undefined` on Linux). Commit** `feat: openmp backend with deterministic band merge`.

---

### Task 6: Fixture format, prepare script, synthetic generator

**Files:**
- Create: `benchmarks/fixtures/FORMAT.md`, `benchmarks/prepare_fixture.py`, `benchmarks/make_synthetic.py`, `benchmarks/fzfix/__init__.py`, `benchmarks/fzfix/io.py`, `benchmarks/tests/test_fzfix.py`, `environment.yml`, `benchmarks/README.md`

**Interfaces:**
- Fixture directory layout (one directory per fixture, name = fixture id):
  - `raster_<name>.json` `{ "dtype": "f32|f64|i32", "width", "height", "transform": [origin_x, origin_y, pixel_w, pixel_h], "nodata": number|null, "crs": "EPSG:3035", "source": "...", "licence": "..." }` plus `raster_<name>.bin` (row-major little-endian).
  - `zones_<name>.json` `{ "n_zones", "n_rings", "n_points", "crs", "source" }` plus `zones_<name>.xy.f64`, `zones_<name>.ring_start.i64`, `zones_<name>.ring_is_hole.u8`, `zones_<name>.ring_zone.i32`, `zones_<name>.centroid_rc_<rastername>.i32` (row, col pairs per zone, computed for each raster grid present; centroid = shapely `centroid`, converted with the truncating rule `col = trunc((x - origin_x)/pixel_w)`).
  - `fixture.json` `{ "id", "rasters": [...], "zones": [...], "area_km2", "notes" }`.
- `fzfix.io`: `read_raster(path_json) -> (np.ndarray, dict)`, `write_raster(path_json, array, transform, nodata, meta)`, `read_zones(path_json) -> dict of arrays`, `write_zones(path_json, polygons: list[shapely.Polygon|MultiPolygon], zone_ids, meta)`, `centroid_cells(polygons, transform) -> np.ndarray[(n,2)] int32`.
- `prepare_fixture.py --raster path.tif --name dem --zones path.gpkg|.shp|.zip --zone-field ID --out benchmarks/fixtures/<id>` reads with rasterio and pyogrio, refuses when raster CRS != zones CRS (`SystemExit("CRS mismatch: ... vs ...")`), writes the layout above. Polygon rings are emitted exterior first then its holes; multipolygons emit all parts under one zone; zone ids are 0..n-1 in file order (the `--zone-field` value is saved alongside in `zones_<name>.ids.i64` for traceability).
- `make_synthetic.py --width W --height H --zones N --seed S --out dir` writes a float32 raster (smooth noise, 5% NoData -9999) and N non-overlapping rectangles as zones; sizes used in tests: 200x300 with 50 zones.
- `environment.yml` (conda-forge): python 3.11, numpy, rasterio, pyogrio, shapely, pandas, matplotlib, gdal, exactextract, rasterstats, xarray-spatial, pytest, cmake, ninja, compilers (gcc 13 pinned), openmp. `conda-lock.yml` is generated in Task 12.

- [ ] **Step 1: Failing tests** `benchmarks/tests/test_fzfix.py`: round trip of `write_raster`/`read_raster` for f32 and f64; `write_zones` of a square with a hole produces `n_rings 2`, `ring_is_hole == [0,1]`; `centroid_cells` of a square (2,2)-(4,4) on transform `[0,10,1,1]` is `(row 7, col 3)`; `make_synthetic` output readable and has exactly N zones; `prepare_fixture` on a tiny GeoTIFF (made in the test with rasterio, EPSG:3035) and a GeoPackage in EPSG:4326 exits with "CRS mismatch".
- [ ] **Step 2: Run `pytest benchmarks/tests -q`, expect failures.**
- [ ] **Step 3: Implement the module and scripts; write `FORMAT.md` with the layout above verbatim.**
- [ ] **Step 4: Run tests, expect pass. Commit** `feat: fixture format, prepare and synthetic scripts`.

---

### Task 7: Public-data fixtures on deep3

**Files:**
- Create: `benchmarks/fixtures/manifest.json`, `benchmarks/fetch_fixtures.sh`, `benchmarks/deep3/build_fixtures.sh`, `benchmarks/deep3/clip_dem.sql`, `benchmarks/deep3/make_grid.py`, `tests/fixtures/leze_micro/` (committed, < 3 MB), `tests/fixtures/synthetic_small/` (committed)

**Interfaces:**
- Fixture ids and contents (all EPSG:3035):

| id | boundary (HydroBASINS lev07, CC BY 4.0) | rasters | zone sets |
|---|---|---|---|
| `leze_micro` | Lèze near Toulouse, about 17 km2 | `dem30` f32 | `grid100` (about 1.7e3 zones) |
| `biebrza_upper` | Upper Biebrza, about 863 km2 | `dem30`, `dem60`, `dem120` f32; `clc100` i32 (CLC 2018 class code rasterised at 100 m); `imp30` f32 (Copernicus imperviousness 2018, resampled to 30 m as in SpongeGIS) | `grid1000`, `grid500`, `grid250`, `grid100` |
| `gradascica` | Gradaščica, about 160 km2 | `dem30`, `clc100` | `grid250`, `grid100` |
| `mazovia_5000` | a contiguous HydroBASINS union of about 5,000 km2 in central Poland | `dem30`, `dem60`, `dem120`, `clc100` | `grid1000`, `grid500`, `grid250`, `grid100` (about 5e5 zones) |
| `synthetic_small` | none | 200x300 f32 | 50 rectangles |

- `build_fixtures.sh` runs on deep3 inside the conda env: downloads HydroBASINS Europe level 7 (`hybas_eu_lev07_v1c`) into `/data/fastzonal/src/`, selects basins by HYBAS_ID (ids listed in the script with the basin names), reprojects to 3035, clips `dem30/dem60/dem120` from the PostGIS tables via `clip_dem.sql` (`ST_AsTIFF(ST_Clip(ST_Union(rast), geom))`, read-only), rasterises CLC 2018 (download from Copernicus, GeoPackage) with `gdal_rasterize -a CODE_18 -tr 100 100`, resamples imperviousness with `gdalwarp -tr 30 30 -r bilinear`, builds square grids with `make_grid.py` (axis-aligned cells of the given size clipped to the boundary, cells with area < 1% of a full cell dropped, ids in row-major order), then calls `prepare_fixture.py` for every raster and zone set, and finally writes `manifest.json` (id, files, sha256, bytes, sources, licences).
- `fetch_fixtures.sh` downloads `fixtures-v1.tar.zst` from the URL in `manifest.json` (a placeholder `https://zenodo.org/record/TODO` until the DOI exists; until then the file is served from `/data/fastzonal/fixtures/` on deep3 for the author's runs) and verifies sha256.

- [ ] **Step 1: Create the conda env on deep3** (`conda env create -p /data/fastzonal/env -f environment.yml`), verify `gdalinfo --version` and `python -c "import rasterio, pyogrio, exactextract"`.
- [ ] **Step 2: Write `build_fixtures.sh`, `clip_dem.sql`, `make_grid.py`; run under `tmux -s fzfix` with output to `/data/fastzonal/logs/build_fixtures.log`.** Expected: `manifest.json` lists 5 fixtures; `biebrza_upper/raster_dem30.json` has about 960x1,000 pixels; `mazovia_5000/zones_grid100.json` has `n_zones` about 5e5.
- [ ] **Step 3: Copy `leze_micro` and `synthetic_small` into `tests/fixtures/`** (size check `du -sh tests/fixtures` < 5 MB).
- [ ] **Step 4: Commit** `data: fixture build scripts, manifest, two small fixtures`.

---

### Task 8: C# baseline harness and expected outputs

**Files:**
- Create: `benchmarks/csharp/FzBench.sln`, `benchmarks/csharp/FzBench/FzBench.csproj` (net10.0, references `MaxRev.Gdal.Core` 3.12.2.472 only for an optional GeoTIFF sanity check, not needed for raw fixtures), `benchmarks/csharp/FzBench/Baseline/RasterizePolygons.cs`, `Baseline/ComputeMeanRasterValueBatch.cs`, `Baseline/RasterAggregation.cs`, `Baseline/GeoTransform.cs` (verbatim extracts with the licence header from Global Constraints), `benchmarks/csharp/FzBench/Fixture.cs`, `benchmarks/csharp/FzBench/Program.cs`, `benchmarks/csharp/FzBench/Rungs/BaselineRung.cs`, `benchmarks/csharp/FzBench/IRung.cs`, `tests/fixtures/expected/README.md`

**Interfaces:**
- `IRung { string Name; int[] Rasterize(Fixture f, string zones, string raster); double[] Mean(...); double[] Majority(...); double[] Ratio(Fixture f, string zones, string num, string den); }`.
- `Fixture.Load(dir)` reads the Task 6 layout into `double[,]` (baseline needs `double[,]`; other rungs get `float[]`), and rebuilds `List<Geometry>` polygons with NetTopologySuite from the ring arrays (holes included, so CENTER-mode rungs can use them; the baseline ignores holes by construction).
- CLI: `FzBench expected <fixtureDir> <outDir>` writes `expected/<fixture>/<op>_<zones>_<raster>.csv` with one line per zone (`zone,value` with `R` round-trip formatting); `FzBench bench <fixtureDir> <rung> <op> <zones> <raster> --threads T --reps R --out results.csv` appends `fixture,op,rung,threads,rep,seconds,checksum` (checksum = sum of values formatted `G17`).
- Ops: `rasterize` (checksum = count of non-negative mask cells plus sum of ids), `mean`, `majority`, `ratio` (num = `imp30` resampled? no: ratio uses `dem30` / `dem60` upsampled to the dem30 grid with nearest neighbour inside the harness, documented as a synthetic ratio workload).

- [ ] **Step 1: Failing xUnit test** `benchmarks/csharp/FzBench.Tests/BaselineTests.cs`: on `tests/fixtures/synthetic_small`, baseline `Mean` of zone 0 equals the hand-computed mean from numpy saved in `tests/fixtures/synthetic_small/check_mean_zone0.txt` (written by `make_synthetic.py`) within 1e-9; `Rasterize` on `leze_micro` gives count of assigned cells > 0 and no id >= n_zones.
- [ ] **Step 2: Run `dotnet test benchmarks/csharp`, expect failure.**
- [ ] **Step 3: Copy the four baseline files verbatim from `F:\SGGW_2026\SPONGEGIS.NET\SPONGEGIS2026.NET\WebSPONGEGIS\Services\IndicatorCalculationService.cs` (lines 1387 to 1548) and `RasterAggregation.cs`, implement `Fixture`, `BaselineRung`, `Program`.**
- [ ] **Step 4: Run tests, expect pass. Generate expected outputs** for `leze_micro`, `synthetic_small`, `biebrza_upper` (`grid500` and `grid100`, rasters `dem30`, `clc100` majority): commit the small ones under `tests/fixtures/expected/`, keep `biebrza_upper` under `/data/fastzonal/expected/` and listed in `manifest.json`.
- [ ] **Step 5: Commit** `bench: csharp baseline harness and expected outputs`.

---

### Task 9: C++ parity tests against the baseline expected outputs

**Files:**
- Create: `tests/parity/fixture_reader.hpp`, `tests/parity/fixture_reader.cpp`, `tests/parity/test_parity.cpp`
- Modify: `tests/CMakeLists.txt` (target `fz_parity`, `FZ_FIXTURE_DIR` compile definition pointing at `tests/fixtures`, env override `FZ_FIXTURE_DIR`)

**Interfaces:**
- `fixture_reader`: `Raster<T> read_raster(dir, name)`, `Zones read_zones(dir, name)`, `std::vector<double> read_expected(csv)`.

- [ ] **Step 1: Failing tests**: for each `(fixture, zones, raster)` in `tests/fixtures/expected/`: `fz_rasterize` in COMPAT mode then `fz_zonal_f64` + `fz_zonal_mean_fallback_f64` equals expected mean within `1e-9 * max(1, |expected|)` for every zone; majority on `clc100` equals expected exactly; the same with `fz_zonal_f32` on the f32 copy within 1e-6 relative (documents the float32 effect); the rasterised COMPAT mask has the same per-zone counts as the baseline `rasterize` expected file.
- [ ] **Step 2: Run, expect failures where semantics differ; fix the library until all pass** (likely spots: truncation vs floor for negative offsets, inclusive right column in COMPAT, the -9999 NoData passed explicitly).
- [ ] **Step 3: Run `ctest -R parity`, expect pass. Commit** `test: parity with csharp baseline on committed fixtures`.

---

### Task 10: .NET binding

**Files:**
- Create: `bindings/dotnet/FastZonal/FastZonal.csproj` (net10.0, `AllowUnsafeBlocks`), `bindings/dotnet/FastZonal/Native.cs`, `bindings/dotnet/FastZonal/Options.cs`, `bindings/dotnet/FastZonal/Rasterizer.cs`, `bindings/dotnet/FastZonal/Zonal.cs`, `bindings/dotnet/FastZonal/ZoneStats.cs`, `bindings/dotnet/FastZonal.Tests/FastZonal.Tests.csproj` (net10.0, xUnit), `bindings/dotnet/FastZonal.Tests/ZonalTests.cs`, `bindings/dotnet/README.md`
- Modify: `CMakeLists.txt` (custom target `dotnet_native_copy` copying the built library into `bindings/dotnet/FastZonal/runtimes/<rid>/native/`)

**Interfaces:**
- `public readonly record struct Transform(double OriginX, double OriginY, double PixelW, double PixelH);`
- `public sealed class Options { Backend Backend = Auto; int Threads = 0; RasterMode RasterMode = Center; int BandRows = 64; }`
- `public struct ZoneStats { long Count; double Sum; double SumSq; double Min; double Max; double Mean => ...; double Std => ...; }` (layout identical to `fz_stats`, `[StructLayout(Sequential)]`).
- `public static class Rasterizer { static int[] Rasterize(ReadOnlySpan<double> xy, ReadOnlySpan<long> ringStart, ReadOnlySpan<byte> ringIsHole, ReadOnlySpan<int> ringZone, Transform t, int width, int height, Options? o = null); static void Rasterize(... Span<int> maskOut, ...); }`
- `public static class Zonal { static ZoneStats[] Stats(ReadOnlySpan<float> raster, ReadOnlySpan<int> mask, int width, int height, int nZones, float nodata, Options? o = null); static ZoneStats[] Stats(ReadOnlySpan<double> ...); static double[] MeanWithFallback(ReadOnlySpan<float> raster, int width, int height, float nodata, ReadOnlySpan<ZoneStats> stats, ReadOnlySpan<int> centroidRow, ReadOnlySpan<int> centroidCol); static double[] Majority(ReadOnlySpan<float> raster, ReadOnlySpan<int> mask, int width, int height, int nZones, float nodata, ReadOnlySpan<int> centroidRow, ReadOnlySpan<int> centroidCol, out bool[] hasValue, Options? o = null); static ZoneStats[] Ratio(ReadOnlySpan<float> num, ReadOnlySpan<float> den, ReadOnlySpan<int> mask, int width, int height, int nZones, float nodataNum, float nodataDen, double eps = 1e-12, Options? o = null); }`
- Errors: non-zero codes throw `FastZonalException(code, fz_error_string(code))`.
- Native resolution: `NativeLibrary.SetDllImportResolver` tries `FASTZONAL_NATIVE_PATH` env var, then `runtimes/<rid>/native/`, then the default search.

- [ ] **Step 1: Failing tests** `ZonalTests.cs`: `Version` returns "0.1.0"; `Stats` on a 4x4 array matches hand values; `Rasterize` of a unit square matches Task 3 `square_center_mode` count 9; parity: on `tests/fixtures/leze_micro` with COMPAT mode, `MeanWithFallback` equals `tests/fixtures/expected/leze_micro/mean_grid100_dem30.csv` within 1e-9; error code path throws `FastZonalException` with message "invalid argument".
- [ ] **Step 2: Run `dotnet test bindings/dotnet`, expect failure.**
- [ ] **Step 3: Implement the binding.**
- [ ] **Step 4: Run tests, expect pass; add the .NET test job to `ci.yml` (Linux and Windows, after the native build). Commit** `feat: dotnet binding with parity test`.

---

### Task 11: Optimised C# rung, fastzonal rungs, benchmark runner, first tables

**Files:**
- Create: `benchmarks/csharp/FzBench/Rungs/OptimisedRung.cs`, `benchmarks/csharp/FzBench/Rungs/FastZonalRung.cs`, `benchmarks/cpp/fz_bench.cpp`, `benchmarks/run_all.sh`, `benchmarks/status.sh`, `benchmarks/make_paper_assets.py`, `benchmarks/results/.gitkeep`, `benchmarks/results/README.md`
- Modify: `benchmarks/csharp/FzBench/Program.cs` (rung registry), `CMakeLists.txt` (target `fz_bench`, option `FASTZONAL_BUILD_BENCH` ON)

**Interfaces:**
- `OptimisedRung`: same algorithms as baseline but `float[]` raster, `int[]` mask, `Parallel.For` over row bands of 64 rows with thread-local `double` accumulators merged in band order, rasterisation parallel over bands as in Task 5, no per-row `List` allocation. Must satisfy the Task 9 parity assertions (add `OptimisedParityTests.cs` to `FzBench.Tests`).
- `FastZonalRung`: calls the Task 10 binding with `Options.Threads` from the CLI.
- `fz_bench <fixtureDir> <op> <zones> <raster> --backend serial|openmp --threads T --reps R --band-rows B --out results.csv` appends the same CSV columns as `FzBench` with `rung = fastzonal-serial|fastzonal-openmp`.
- `run_all.sh [--quick]`: builds Release with `FASTZONAL_NATIVE_ARCH=ON`, publishes `FzBench`, then runs the matrix: fixtures `biebrza_upper` (all grids, `dem30`, `clc100`), `mazovia_5000` (`grid1000..grid100`, `dem30/dem60/dem120`), ops `rasterize`, `mean`, `majority`, `ratio`, rungs `csharp-baseline` (1 thread), `csharp-optimised` and `fastzonal-openmp` at threads 1, 2, 4, 8, 16, 32, 56, 112, `fastzonal-serial`; 5 reps each (`--quick`: 1 rep, `biebrza_upper` only). Writes `benchmarks/results/raw_<timestamp>.csv`, `benchmarks/results/env_<timestamp>.json` (hostname, CPU model from `/proc/cpuinfo`, core count, compiler versions, `fz_version`, git commit, flags) and a `status.json` updated after every point (`done`, `total`, `elapsed_s`, `eta_s`, `current`). Resumable: a point already present in the raw CSV (same fixture, op, rung, threads, rep) is skipped.
- `status.sh` prints `status.json` as one line (`done/total, elapsed, ETA, current`), for the 5-minute reports.
- `make_paper_assets.py --raw results/raw_*.csv --out paper_assets/` writes `tables/scaling_zones.tex`, `tables/speedup_threads.tex`, `tables/ladder.tex`, `figures/scaling_zones.png`, `figures/speedup_threads.png`, `summary.json` (medians, min, max per point), using matplotlib with no seaborn.

- [ ] **Step 1: Failing tests**: `OptimisedParityTests` (as Task 9 assertions, C# side); C++ test `test_bench_csv.cpp` that `fz_bench` on `synthetic_small` appends exactly `reps` lines with the right columns; `benchmarks/tests/test_assets.py` that `make_paper_assets.py` on a 20-line hand-written raw CSV produces the three tables and `summary.json` with the expected medians.
- [ ] **Step 2: Run all three, expect failures.**
- [ ] **Step 3: Implement rungs, `fz_bench`, the runner, the status script and the asset script.**
- [ ] **Step 4: Run tests, expect pass. Run `run_all.sh --quick` locally on `biebrza_upper`** (needs the fixture fetched to the workstation or run on deep3): expected a raw CSV with every rung present and `csharp-optimised`/`fastzonal` checksums equal to `csharp-baseline` for `mean` and `majority` (the runner asserts this and exits non-zero otherwise).
- [ ] **Step 5: Full run on deep3** in `tmux -s fzbench` from `/data/fastzonal/fastzonal1`, log to `/data/fastzonal/logs/run_all_<timestamp>.log`; report `status.sh` output every 5 minutes to the author; on completion run `make_paper_assets.py` and commit `benchmarks/results/raw_*.csv`, `env_*.json` and `paper_assets/` with message `bench: first scaling and ladder results on deep3`.

---

### Task 12: Reproducibility package: DEVELOPMENT.md, README, locks, Dockerfile

**Files:**
- Create: `DEVELOPMENT.md`, `Dockerfile`, `conda-lock.yml`, `CITATION.cff`, `benchmarks/REPRODUCE.md`
- Modify: `README.md`

**Interfaces:**
- `Dockerfile`: `condaforge/miniforge3` base pinned by digest, `conda-lock install`, builds the library Release with OpenMP, runs `ctest`, `dotnet test` (installs the .NET 10 SDK from Microsoft's apt feed), and exposes `benchmarks/run_all.sh` as the entrypoint; CUDA deliberately excluded (documented).
- `REPRODUCE.md`: numbered steps from a clean Ubuntu machine to the tables in `paper_assets/`: clone, `fetch_fixtures.sh`, `docker build`, `docker run ... run_all.sh`, `make_paper_assets.py`; states expected wall time and that absolute times differ by machine while checksums and speedup ratios should not.
- `DEVELOPMENT.md`: layout, build presets, test commands (`ctest`, `dotnet test`, `pytest`), determinism rules (band merge order, double accumulators), how to add a backend, the no-AI-mention and no-em-dash rules.

- [ ] **Step 1: Generate `conda-lock.yml`** (`conda-lock -f environment.yml -p linux-64 -p win-64`); build the Docker image on deep3 (`docker build -t fastzonal:dev .`), run `docker run --rm fastzonal:dev ctest --test-dir build` and expect all tests to pass inside the container.
- [ ] **Step 2: Write the three documents and `CITATION.cff`** (authors in the order from the workspace `CLAUDE.md`, version 0.1.0, repository URL).
- [ ] **Step 3: Verification**: a fresh clone in a temporary directory, following `REPRODUCE.md` literally with `run_all.sh --quick`, reaches `paper_assets/summary.json`. Commit `docs: development guide, reproduce steps, docker and conda lock`.

---

## Follow-up plans (written after Plan A closes)

- **Plan B, terrain:** Horn slope (parity with `gdaldem slope`), Priority-Flood fill, D8 flow accumulation, classic TWI, and the SAGA Wetness Index (Wang and Liu fill plus Boehner modified catchment area, parity with SAGA 9.8 from the `spongegis-om-service-1` image on the same DEM), all reducible per zone; adds `fz_slope_f32`, `fz_fill_sinks_f32`, `fz_flow_accum_d8_f32`, `fz_twi_f32`, `fz_swi_f32`.
- **Plan C, CUDA:** zonal reductions and slope on one L40S; conda `cuda-nvcc` and matching GCC; tolerance 1e-6; adds the `fastzonal-cuda` rung.
- **Plan D, Python binding, CLI, external tools:** `fastzonal` ctypes package with pytest; `fastzonal` CLI (GDAL in, CSV out); benchmark rungs for `exactextract`, `rasterstats`, `xarray-spatial`, `gdaldem`, SAGA; capability table for the paper.
- **Plan E, SpongeGIS integration:** `Calculation:Engine` setting on SpongeGIS `main`, routing the five kernels through `FastZonal`; end-to-end timing of three sessions on a copy of the deployed build on deep3.
- **Plan F, manuscript:** `paper/v01/fastzonal-article.tex` on the textfaith/ampliconaudit template, assets from `make_paper_assets.py`, Codex verification of reproducibility and of every number, repository rename to `fastzonal`, tag v1.0.0, DOI for fixtures.
