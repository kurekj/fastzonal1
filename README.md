# fastzonal

Zonal statistics and terrain indices of rasters over polygon units: a C++17 core with serial
and OpenMP backends (CUDA planned), a C ABI, and bindings for .NET and Python.

Status: pre-release (0.1.0). The API is not stable yet.

## Build and test

Requires CMake 3.20 or newer, a C++17 compiler and OpenMP.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

On Windows with Visual Studio 2022 use the `release-msvc` preset instead.

## Layout

- `include/fastzonal/fastzonal.h`: the C ABI
- `src/core`: algorithms shared by all backends
- `src/backends`: serial, OpenMP and (later) CUDA schedulers
- `tests`: Catch2 unit and parity tests
- `benchmarks`: fixtures, harnesses and the scripts that produce every published number
- `docs`: design and development notes

## Licence

MIT, see `LICENSE`.
