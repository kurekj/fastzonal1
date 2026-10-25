# fastzonal

Zonal statistics and polygon rasterisation over raster grids: a C++17 core with serial and
OpenMP backends, a C ABI, and a .NET binding. Built for the workloads behind opportunity
mapping and suitability analysis, where dozens of raster layers are aggregated to the same set of
planning units.

Status: pre-release (0.1.0). The API is not stable yet.

## What it does

- `fz_rasterize`: polygons (with holes, multipolygons) to a zone-index mask on a north-up grid,
  with a pixel-centre rule or a mode that reproduces the SpongeGIS reference rasteriser exactly.
- `fz_zonal_f32/f64`: count, sum, sum of squares, min and max per zone in one pass, with
  NoData and NaN handling; `fz_zonal_mean_fallback_*` adds the centroid fallback for zones
  smaller than a cell.
- `fz_zonal_majority_f32/f64/i32`: the mode per zone (ties to the smallest value).
- `fz_zonal_ratio_f32/f64`: statistics of the cell-wise ratio of two rasters on one grid.

Results are bit-identical between the serial and the OpenMP backend for any thread count (the
band split fixes the summation order), and they reproduce the C# reference application's
outputs on real catchment fixtures (see `tests/parity`).

## Build and test

Requires CMake 3.20 or newer, Ninja, Git, a C++17 compiler with OpenMP, and network access on
the first configure (the Catch2 test framework is fetched by CMake).

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

On Windows with Visual Studio 2022 use the `release-msvc` preset instead. Without Ninja, the
same build with the default generator is:

```bash
cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=Release
cmake --build build/plain --config Release
ctest --test-dir build/plain -C Release --output-on-failure
```

Inside WSL (Ubuntu) without root, `bash tools/wsl-setup.sh` installs cmake and ninja into
`~/.local` and `bash tools/wsl-run.sh <command>` runs a command with them on `PATH`.

## .NET binding

```bash
cmake --build --preset release --target dotnet_native_copy
dotnet test bindings/dotnet/FastZonal.slnx
```

See `bindings/dotnet/README.md` for the API and the native-library resolution rules.

## Benchmarks

`benchmarks/` holds the fixtures (public data only), the C# harness with the reference code as
deployed, the C++ driver, the runner and the script that turns raw measurements into the tables
and figures of the paper. `benchmarks/REPRODUCE.md` lists the steps from a clean machine to the
tables.

## Layout

- `include/fastzonal/fastzonal.h`: the C ABI
- `src/core`: algorithms shared by all backends; `src/tools`: fixture reader
- `tests`: Catch2 unit and parity tests, committed fixtures with the reference's expected outputs
- `bindings/dotnet`: the FastZonal class library and its tests
- `benchmarks`: fixtures, harnesses, runner, paper assets
- `DEVELOPMENT.md`: the rules that keep results reproducible

## Licence

The code is MIT, see `LICENSE`. The benchmark fixtures are derived data with their own terms
(GEDTM30 and HydroBASINS CC BY 4.0, Copernicus Land Monitoring Service open access with
attribution), recorded per file in `benchmarks/fixtures/manifest.json`. Cite with `CITATION.cff`.
