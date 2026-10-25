# Development guide

This is the contract for working on fastzonal. Read it before changing anything.

## Layout

```
include/fastzonal/fastzonal.h   the C ABI; the only public header
src/core/                        algorithms (rasterize, zonal, dispatch, validate, options, version)
src/tools/fixture_io.*           reader of the fixture layout, shared by tests and benchmarks
tests/                           Catch2 unit tests, parity tests, benchmark-driver test
bindings/dotnet/                 FastZonal class library (net10.0) and its xUnit tests
benchmarks/                      fixtures, C# harness, C++ driver, runner, paper assets
```

## Build and test

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug     # ASan + UBSan
cmake --build --preset release --target dotnet_native_copy && dotnet test bindings/dotnet/FastZonal.slnx
dotnet test benchmarks/csharp/FzBench.slnx
python -m pytest benchmarks/tests -q
FASTZONAL_LIBRARY=$PWD/build/release/libfastzonal.so python -m pytest bindings/python/tests -q
```

Run a single Catch2 test by name: `build/release/tests/fz_tests "square center mode"`. The parity
test accepts `FZ_PARITY_FIXTURE_DIR` and `FZ_PARITY_EXPECTED_DIR` to run on a larger fixture set.

Inside WSL without root, `bash tools/wsl-setup.sh` installs cmake and ninja into `~/.local` and
`bash tools/wsl-run.sh <command>` runs a command with them on `PATH`.

## Rules that keep the results reproducible

- **Determinism.** Every reduction splits the raster into bands of `band_rows` rows, computes each
  band independently and merges the partials in ascending band order on the calling thread. For a
  given `band_rows` the result is bit-identical between the serial and the OpenMP backend for any
  thread count. Keep it that way: no OpenMP reductions, no shared accumulators, no merge order that
  depends on scheduling. Changing `band_rows` changes the summation order (documented).
- **No floating-point contraction.** The library is compiled with `-ffp-contract=off` (GCC/Clang)
  and `/fp:precise` (MSVC). Do not add `-ffast-math` or `-march=native` to the default build;
  `FASTZONAL_NATIVE_ARCH=ON` is for benchmark builds and is recorded in the results metadata.
- **Accumulate in double** whatever the raster type; `fz_stats` holds doubles and an int64 count.
- **Validate before writing.** Every entry point checks arguments (and the whole mask) before any
  output is touched, so an error leaves the caller's buffers unchanged. Exceptions never cross the
  C ABI: allocation failures become `FZ_ERR_NOMEM`, anything else `FZ_ERR_INTERNAL`, and worker
  exceptions inside OpenMP regions are captured and rethrown on the calling thread.
- **Reference semantics.** `FZ_RASTER_COMPAT` reproduces the SpongeGIS rasteriser exactly
  (exterior rings only, truncating integer casts, inclusive right column); `FZ_RASTER_CENTER` is the
  clean pixel-centre rule with holes. NoData is `v == nodata || isnan(v)`. Majority ties resolve to
  the smallest value. Empty zones fall back to the centroid cell when centroid arrays are given.
- **Parity is the gate.** `tests/fixtures/expected/` holds the C# baseline's outputs; the parity
  tests must pass (counts exact, mean 1e-9, majority exact, f32 1e-6) before any benchmark number
  is quoted. Regenerate expected outputs only with `FzBench expected` and say why in the commit.

## Adding a backend

Implement the band scheduler in `src/core/dispatch.hpp` (`for_each_band`,
`for_each_band_with_scratch`), resolve it in `resolve_plan`, and add the same determinism tests
as `tests/test_parallel.cpp` runs for OpenMP. A backend that cannot keep the band merge order
(for example GPU reductions) must document its tolerance and be excluded from the bit-identity
tests.

## Style

C++17, 4-space indentation, `snake_case` in C and C++, `PascalCase` in C#, Google-style comments
that explain why rather than what. No em dashes in code, comments or documentation. Commit
messages are `type: summary` lines.
