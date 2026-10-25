# FastZonal (.NET binding)

A thin, allocation-light wrapper over the fastzonal C ABI for .NET 10.

```csharp
using FastZonal;

var mask = Rasterizer.Rasterize(xy, ringStart, ringIsHole, ringZone,
    new Transform(originX, originY, pixelW, pixelH), width, height,
    new Options { RasterMode = RasterMode.Compat });
var stats = Zonal.Stats(raster, mask, width, height, nZones, nodata: -9999f);
var mean = Zonal.MeanWithFallback(raster, width, height, -9999f, stats, centroidRow, centroidCol);
```

Inputs are `ReadOnlySpan<T>` so arrays, slices and pooled buffers all work without copies; every
call validates lengths before entering native code and throws `FastZonalException` (with the
library's error code and message) for errors the library reports.

## Native library

The managed assembly loads `fastzonal.dll` / `libfastzonal.so` from, in this order:

1. the `FASTZONAL_NATIVE_PATH` environment variable (a file or a directory),
2. `runtimes/<rid>/native/` next to the assembly (the CMake target `dotnet_native_copy` puts the
   freshly built library there: `cmake --build --preset release --target dotnet_native_copy`),
3. the default probing of `NativeLibrary`.

## Tests

```bash
cmake --build --preset release --target dotnet_native_copy
dotnet test bindings/dotnet/FastZonal.slnx
```

The suite includes a parity check against the C# baseline outputs of `tests/fixtures/leze_micro`.
