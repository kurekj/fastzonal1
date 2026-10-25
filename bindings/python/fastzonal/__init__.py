"""fastzonal for Python: zonal statistics and polygon rasterisation over raster grids.

A thin ctypes wrapper over the C ABI (include/fastzonal/fastzonal.h). Arrays are NumPy arrays;
nothing is copied when they already have the expected dtype and are C-contiguous.

    import fastzonal as fz
    mask = fz.rasterize(polygons, transform=(x0, y0, pw, ph), width=w, height=h, mode="compat")
    stats = fz.zonal(raster, mask, n_zones, nodata=-9999)      # structured array: count, sum, sumsq, min, max
    mean = fz.mean(raster, mask, n_zones, nodata=-9999, centroids=(rows, cols))
    mode = fz.majority(raster, mask, n_zones, nodata=-9999)

The shared library is found through FASTZONAL_LIBRARY (a file path), then next to this package
(`fastzonal/lib/`), then the usual loader search path.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path
from typing import Iterable, Optional, Sequence, Tuple

import numpy as np

__all__ = ["version", "has_openmp", "Options", "rasterize", "zonal", "mean", "majority", "ratio",
           "polygons_to_rings", "FastZonalError", "STATS_DTYPE"]

STATS_DTYPE = np.dtype([("count", "<i8"), ("sum", "<f8"), ("sumsq", "<f8"), ("min", "<f8"), ("max", "<f8")])

BACKENDS = {"auto": 0, "serial": 1, "openmp": 2, "cuda": 3}
MODES = {"center": 0, "compat": 1}


class FastZonalError(RuntimeError):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


class _Transform(ctypes.Structure):
    _fields_ = [("origin_x", ctypes.c_double), ("origin_y", ctypes.c_double),
                ("pixel_w", ctypes.c_double), ("pixel_h", ctypes.c_double)]


class _Shape(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int32), ("height", ctypes.c_int32)]


class _Options(ctypes.Structure):
    _fields_ = [("backend", ctypes.c_int), ("threads", ctypes.c_int), ("device", ctypes.c_int),
                ("raster_mode", ctypes.c_int), ("band_rows", ctypes.c_int)]


def _library_candidates() -> Iterable[str]:
    env = os.environ.get("FASTZONAL_LIBRARY")
    if env:
        yield env
    names = {"win32": "fastzonal.dll", "darwin": "libfastzonal.dylib"}.get(sys.platform, "libfastzonal.so")
    here = Path(__file__).resolve().parent
    yield str(here / "lib" / names)
    yield str(here / names)
    found = ctypes.util.find_library("fastzonal")
    if found:
        yield found
    yield names


def _load():
    errors = []
    for candidate in _library_candidates():
        try:
            return ctypes.CDLL(candidate)
        except OSError as e:  # pragma: no cover - depends on the machine
            errors.append(f"{candidate}: {e}")
    raise OSError("fastzonal shared library not found; set FASTZONAL_LIBRARY. Tried:\n  " + "\n  ".join(errors))


_lib = _load()

_P = ctypes.POINTER
_lib.fz_version.restype = ctypes.c_char_p
_lib.fz_has_openmp.restype = ctypes.c_int
_lib.fz_error_string.restype = ctypes.c_char_p
_lib.fz_error_string.argtypes = [ctypes.c_int]
_lib.fz_options_default.argtypes = [_P(_Options)]
_lib.fz_rasterize.restype = ctypes.c_int
_lib.fz_rasterize.argtypes = [_P(ctypes.c_double), _P(ctypes.c_int64), _P(ctypes.c_uint8), _P(ctypes.c_int32),
                              ctypes.c_int64, _Transform, _Shape, _P(ctypes.c_int32), _P(_Options)]
for suffix, ctype in (("f32", ctypes.c_float), ("f64", ctypes.c_double), ("i32", ctypes.c_int32), ("u8", ctypes.c_uint8)):
    f = getattr(_lib, f"fz_zonal_{suffix}")
    f.restype = ctypes.c_int
    f.argtypes = [_P(ctype), _P(ctypes.c_int32), _Shape, ctypes.c_int32, ctype, ctypes.c_void_p, _P(_Options)]
    g = getattr(_lib, f"fz_zonal_mean_fallback_{suffix}")
    g.restype = ctypes.c_int
    g.argtypes = [_P(ctype), _Shape, ctype, ctypes.c_void_p, ctypes.c_int32, _P(ctypes.c_int32), _P(ctypes.c_int32),
                  _P(ctypes.c_double)]
for suffix, ctype in (("f32", ctypes.c_float), ("f64", ctypes.c_double), ("i32", ctypes.c_int32), ("u8", ctypes.c_uint8)):
    f = getattr(_lib, f"fz_zonal_majority_{suffix}")
    f.restype = ctypes.c_int
    f.argtypes = [_P(ctype), _P(ctypes.c_int32), _Shape, ctypes.c_int32, ctype, _P(ctypes.c_int32), _P(ctypes.c_int32),
                  _P(ctypes.c_double), _P(ctypes.c_uint8), _P(_Options)]
for suffix, ctype in (("f32", ctypes.c_float), ("f64", ctypes.c_double)):
    f = getattr(_lib, f"fz_zonal_ratio_{suffix}")
    f.restype = ctypes.c_int
    f.argtypes = [_P(ctype), _P(ctype), _P(ctypes.c_int32), _Shape, ctypes.c_int32, ctype, ctype, ctypes.c_double,
                  ctypes.c_void_p, _P(_Options)]

_SUFFIX = {np.dtype("float32"): ("f32", ctypes.c_float), np.dtype("float64"): ("f64", ctypes.c_double),
           np.dtype("int32"): ("i32", ctypes.c_int32), np.dtype("uint8"): ("u8", ctypes.c_uint8)}


def version() -> str:
    return _lib.fz_version().decode()


def has_openmp() -> bool:
    return bool(_lib.fz_has_openmp())


def _check(code: int) -> None:
    if code != 0:
        raise FastZonalError(code, _lib.fz_error_string(code).decode())


class Options:
    """Execution options (defaults as fz_options_default): backend auto|serial|openmp, threads (0 =
    all), rasterisation mode center|compat, band_rows (fixes the summation order)."""

    def __init__(self, backend: str = "auto", threads: int = 0, mode: str = "center", band_rows: int = 64,
                 device: int = 0):
        if backend not in BACKENDS or mode not in MODES:
            raise ValueError("backend must be auto|serial|openmp|cuda and mode center|compat")
        self.backend, self.threads, self.mode, self.band_rows, self.device = backend, threads, mode, band_rows, device

    def _native(self) -> _Options:
        o = _Options()
        _lib.fz_options_default(ctypes.byref(o))
        o.backend, o.threads, o.device = BACKENDS[self.backend], self.threads, self.device
        o.raster_mode, o.band_rows = MODES[self.mode], self.band_rows
        return o


def _raster(raster, name="raster"):
    arr = np.ascontiguousarray(raster)
    if arr.ndim != 2:
        raise ValueError(f"{name} must be 2-D (height, width)")
    if arr.dtype not in _SUFFIX:
        raise TypeError(f"{name} dtype must be float32, float64, int32 or uint8 (got {arr.dtype})")
    return arr


def _mask(mask, shape):
    m = np.ascontiguousarray(mask, dtype=np.int32)
    if m.shape != shape:
        raise ValueError("mask must have the raster's shape")
    return m


def _centroids(centroids, n_zones):
    if centroids is None:
        return None, None
    rows = np.ascontiguousarray(centroids[0], dtype=np.int32)
    cols = np.ascontiguousarray(centroids[1], dtype=np.int32)
    if rows.shape != (n_zones,) or cols.shape != (n_zones,):
        raise ValueError("centroid rows and cols must have n_zones entries")
    return rows, cols


def polygons_to_rings(polygons: Sequence) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Flat ring arrays (xy, ring_start, ring_is_hole, ring_zone) from shapely Polygons or
    MultiPolygons, one zone per input geometry, in input order."""
    xy_parts, ring_start, is_hole, ring_zone = [], [0], [], []
    n = 0
    for zone, geom in enumerate(polygons):
        parts = [geom] if geom.geom_type == "Polygon" else list(geom.geoms)
        for part in parts:
            for k, ring in enumerate([part.exterior, *part.interiors]):
                coords = np.asarray(ring.coords, dtype=np.float64)[:, :2]
                if len(coords) > 1 and np.array_equal(coords[0], coords[-1]):
                    coords = coords[:-1]
                xy_parts.append(coords.reshape(-1))
                n += len(coords)
                ring_start.append(n)
                is_hole.append(1 if k > 0 else 0)
                ring_zone.append(zone)
    xy = np.concatenate(xy_parts) if xy_parts else np.zeros(0, dtype=np.float64)
    return (xy, np.asarray(ring_start, dtype=np.int64), np.asarray(is_hole, dtype=np.uint8),
            np.asarray(ring_zone, dtype=np.int32))


def rasterize(polygons=None, *, transform, width: int, height: int, mode: str = "center", rings=None,
              options: Optional[Options] = None) -> np.ndarray:
    """Zone-index mask (int32, -1 outside) of `polygons` (shapely geometries, zone = position) or of
    prepared `rings` = (xy, ring_start, ring_is_hole, ring_zone), on the north-up grid `transform` =
    (origin_x, origin_y, pixel_w, pixel_h)."""
    if rings is None:
        if polygons is None:
            raise ValueError("give polygons or rings")
        rings = polygons_to_rings(polygons)
    xy, rs, hole, zone = (np.ascontiguousarray(rings[0], dtype=np.float64), np.ascontiguousarray(rings[1], dtype=np.int64),
                          np.ascontiguousarray(rings[2], dtype=np.uint8), np.ascontiguousarray(rings[3], dtype=np.int32))
    n_rings = len(zone)
    if len(rs) != n_rings + 1 or len(hole) != n_rings:
        raise ValueError("ring_start must have n_rings + 1 entries and ring_is_hole n_rings entries")
    if n_rings and (rs[0] < 0 or np.any(np.diff(rs) < 0) or rs[-1] * 2 > xy.size):
        raise ValueError("ring offsets must be non-decreasing and inside the coordinate array")
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    opt = (options or Options(mode=mode))._native()
    if options is None:
        opt.raster_mode = MODES[mode]
    out = np.empty((height, width), dtype=np.int32)
    t = _Transform(*[float(v) for v in transform])
    _check(_lib.fz_rasterize(xy.ctypes.data_as(_P(ctypes.c_double)), rs.ctypes.data_as(_P(ctypes.c_int64)),
                             hole.ctypes.data_as(_P(ctypes.c_uint8)), zone.ctypes.data_as(_P(ctypes.c_int32)),
                             n_rings, t, _Shape(width, height), out.ctypes.data_as(_P(ctypes.c_int32)),
                             ctypes.byref(opt)))
    return out


def zonal(raster, mask, n_zones: int, nodata, options: Optional[Options] = None) -> np.ndarray:
    """count, sum, sumsq, min, max per zone as a structured array of STATS_DTYPE."""
    arr = _raster(raster)
    m = _mask(mask, arr.shape)
    if n_zones <= 0:
        raise ValueError("n_zones must be positive")
    suffix, ctype = _SUFFIX[arr.dtype]
    out = np.zeros(n_zones, dtype=STATS_DTYPE)
    opt = (options or Options())._native()
    f = getattr(_lib, f"fz_zonal_{suffix}")
    _check(f(arr.ctypes.data_as(_P(ctype)), m.ctypes.data_as(_P(ctypes.c_int32)), _Shape(arr.shape[1], arr.shape[0]),
             n_zones, ctype(nodata), out.ctypes.data, ctypes.byref(opt)))
    return out


def mean(raster, mask, n_zones: int, nodata, centroids=None, options: Optional[Options] = None,
         stats: Optional[np.ndarray] = None) -> np.ndarray:
    """Mean per zone with the reference fallback: an empty zone takes the raster value at its
    centroid cell (`centroids` = (rows, cols)) or 0."""
    arr = _raster(raster)
    if stats is None:
        stats = zonal(arr, mask, n_zones, nodata, options)
    suffix, ctype = _SUFFIX[arr.dtype]
    rows, cols = _centroids(centroids, n_zones)
    out = np.zeros(n_zones, dtype=np.float64)
    g = getattr(_lib, f"fz_zonal_mean_fallback_{suffix}")
    _check(g(arr.ctypes.data_as(_P(ctype)), _Shape(arr.shape[1], arr.shape[0]), ctype(nodata), stats.ctypes.data,
             n_zones, None if rows is None else rows.ctypes.data_as(_P(ctypes.c_int32)),
             None if cols is None else cols.ctypes.data_as(_P(ctypes.c_int32)), out.ctypes.data_as(_P(ctypes.c_double))))
    return out


def majority(raster, mask, n_zones: int, nodata, centroids=None, options: Optional[Options] = None,
             return_has_value: bool = False):
    """Mode per zone (ties to the smallest value), with the centroid fallback for empty zones."""
    arr = _raster(raster)
    m = _mask(mask, arr.shape)
    if n_zones <= 0:
        raise ValueError("n_zones must be positive")
    suffix, ctype = _SUFFIX[arr.dtype]
    rows, cols = _centroids(centroids, n_zones)
    value = np.zeros(n_zones, dtype=np.float64)
    has = np.zeros(n_zones, dtype=np.uint8)
    opt = (options or Options())._native()
    f = getattr(_lib, f"fz_zonal_majority_{suffix}")
    _check(f(arr.ctypes.data_as(_P(ctype)), m.ctypes.data_as(_P(ctypes.c_int32)), _Shape(arr.shape[1], arr.shape[0]),
             n_zones, ctype(nodata), None if rows is None else rows.ctypes.data_as(_P(ctypes.c_int32)),
             None if cols is None else cols.ctypes.data_as(_P(ctypes.c_int32)),
             value.ctypes.data_as(_P(ctypes.c_double)), has.ctypes.data_as(_P(ctypes.c_uint8)), ctypes.byref(opt)))
    return (value, has.astype(bool)) if return_has_value else value


def ratio(num, den, mask, n_zones: int, nodata_num, nodata_den, eps: float = 1e-12,
          options: Optional[Options] = None) -> np.ndarray:
    """Statistics (STATS_DTYPE) of the cell-wise ratio num / den on one grid; cells with NoData in
    either raster or |den| < eps are excluded."""
    a = _raster(num, "num")
    b = _raster(den, "den")
    if a.dtype != b.dtype or a.shape != b.shape or a.dtype not in (np.dtype("float32"), np.dtype("float64")):
        raise ValueError("num and den must be float32 or float64 arrays of the same dtype and shape")
    m = _mask(mask, a.shape)
    if n_zones <= 0:
        raise ValueError("n_zones must be positive")
    suffix, ctype = _SUFFIX[a.dtype]
    out = np.zeros(n_zones, dtype=STATS_DTYPE)
    opt = (options or Options())._native()
    f = getattr(_lib, f"fz_zonal_ratio_{suffix}")
    _check(f(a.ctypes.data_as(_P(ctype)), b.ctypes.data_as(_P(ctype)), m.ctypes.data_as(_P(ctypes.c_int32)),
             _Shape(a.shape[1], a.shape[0]), n_zones, ctype(nodata_num), ctype(nodata_den), float(eps), out.ctypes.data,
             ctypes.byref(opt)))
    return out
