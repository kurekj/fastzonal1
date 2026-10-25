# fastzonal for Python

A ctypes binding of the fastzonal C ABI. It needs NumPy and the built shared library; shapely
is optional (only `polygons_to_rings` and the `polygons=` argument of `rasterize` use it).

```bash
cmake --preset release && cmake --build --preset release          # builds build/release/libfastzonal.so
pip install -e bindings/python[test]
export FASTZONAL_LIBRARY=$PWD/build/release/libfastzonal.so       # or copy it into bindings/python/fastzonal/lib/
python -m pytest bindings/python/tests -q
```

```python
import numpy as np, fastzonal as fz
from shapely.geometry import box

zones = [box(0, 0, 50, 50), box(50, 50, 100, 100)]
mask = fz.rasterize(zones, transform=(0, 100, 1, 1), width=100, height=100)   # int32, -1 outside
dem = np.random.default_rng(0).random((100, 100), dtype=np.float32)
stats = fz.zonal(dem, mask, len(zones), nodata=-9999.0)                       # count, sum, sumsq, min, max
mean = fz.mean(dem, mask, len(zones), nodata=-9999.0)
mode = fz.majority(dem.astype(np.int32), mask, len(zones), nodata=-1)
opts = fz.Options(backend="openmp", threads=8, band_rows=16)                   # fixed summation order
```

Semantics are those of the C ABI (`include/fastzonal/fastzonal.h`): `mode="compat"` reproduces the
SpongeGIS truncating rasterisation rule, `mode="center"` (default) uses pixel centres and honours
holes; `mean` and `majority` apply the reference fallback for empty zones (raster value at the zone
centroid cell when `centroids=(rows, cols)` is given, 0 otherwise); results are independent of the
backend and thread count for a fixed `band_rows`.
