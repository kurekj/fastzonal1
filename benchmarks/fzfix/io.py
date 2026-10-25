"""Read and write the raw fixture layout.

Rasters are row-major little-endian binaries beside a JSON header; zones are flat ring arrays in
the exact shape fz_rasterize consumes, so every harness (C++, C#, Python) reads the same bytes.
The layout is specified in benchmarks/fixtures/FORMAT.md.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np

DTYPES = {"f32": np.dtype("<f4"), "f64": np.dtype("<f8"), "i32": np.dtype("<i4")}
_NUMPY_TO_CODE = {np.dtype("float32"): "f32", np.dtype("float64"): "f64", np.dtype("int32"): "i32"}


def _write_text(path, text: str) -> None:
    """Writes with LF line endings on every platform so file hashes are portable."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _sidecar(json_path: Path, suffix: str) -> Path:
    """`raster_dem.json` -> `raster_dem<suffix>` (suffix starts with a dot)."""
    return json_path.with_suffix(suffix)


def write_raster(json_path, array, transform, nodata, meta=None):
    json_path = Path(json_path)
    array = np.ascontiguousarray(array)
    if array.ndim != 2:
        raise ValueError("raster must be 2-D (height, width)")
    if array.dtype not in _NUMPY_TO_CODE:
        raise ValueError(f"unsupported raster dtype {array.dtype}; use float32, float64 or int32")
    code = _NUMPY_TO_CODE[array.dtype]
    origin_x, origin_y, pixel_w, pixel_h = (float(v) for v in transform)
    if not (pixel_w > 0 and pixel_h > 0):
        raise ValueError("pixel sizes must be positive (north-up grid)")
    if nodata is not None:
        nodata = float(nodata)
        if math.isnan(nodata):
            nodata = None  # NaN cells are always excluded by the library; JSON has no NaN literal
        elif math.isinf(nodata):
            raise ValueError("nodata must be finite or NaN")
    header = {
        "dtype": code,
        "width": int(array.shape[1]),
        "height": int(array.shape[0]),
        "transform": [origin_x, origin_y, pixel_w, pixel_h],
        "nodata": nodata,
    }
    header.update(meta or {})
    array.astype(DTYPES[code], copy=False).tofile(_sidecar(json_path, ".bin"))
    _write_text(json_path, json.dumps(header, indent=2) + "\n")


def read_raster(json_path):
    json_path = Path(json_path)
    meta = json.loads(json_path.read_text(encoding="utf-8"))
    dtype = DTYPES[meta["dtype"]]
    data = np.fromfile(_sidecar(json_path, ".bin"), dtype=dtype)
    expected = meta["width"] * meta["height"]
    if data.size != expected:
        raise ValueError(f"{json_path}: expected {expected} values, found {data.size}")
    return data.reshape(meta["height"], meta["width"]), meta


def _polygon_parts(geom):
    """Yields shapely Polygon parts of a Polygon or MultiPolygon, skipping empties."""
    if geom is None or geom.is_empty:
        return
    gt = geom.geom_type
    if gt == "Polygon":
        yield geom
    elif gt == "MultiPolygon":
        for part in geom.geoms:
            if not part.is_empty:
                yield part
    elif gt == "GeometryCollection":
        for part in geom.geoms:
            yield from _polygon_parts(part)
    else:
        raise ValueError(f"unsupported geometry type {gt}")


def _ring_coords(ring):
    coords = np.asarray(ring.coords, dtype=np.float64)[:, :2]
    if len(coords) > 1 and np.array_equal(coords[0], coords[-1]):
        coords = coords[:-1]  # closing point is implied by fz_rasterize
    return coords


def write_zones(json_path, polygons: Sequence, zone_ids: Sequence[int], meta=None, source_ids: Iterable | None = None):
    json_path = Path(json_path)
    for i, geom in enumerate(polygons):
        if geom is None or geom.is_empty or not list(_polygon_parts(geom)):
            raise ValueError(f"zone {i} has no polygonal geometry; every zone needs at least one polygon")
    xy_parts, ring_start, is_hole, ring_zone = [], [0], [], []
    n_points = 0
    for geom, zone in zip(polygons, zone_ids):
        for part in _polygon_parts(geom):
            for k, ring in enumerate([part.exterior, *part.interiors]):
                coords = _ring_coords(ring)
                xy_parts.append(coords.reshape(-1))
                n_points += len(coords)
                ring_start.append(n_points)
                is_hole.append(1 if k > 0 else 0)
                ring_zone.append(int(zone))
    xy = np.concatenate(xy_parts) if xy_parts else np.zeros(0, dtype=np.float64)
    stem = json_path.name[: -len(".json")]
    base = json_path.parent / stem
    xy.astype("<f8").tofile(f"{base}.xy.f64")
    np.asarray(ring_start, dtype="<i8").tofile(f"{base}.ring_start.i64")
    np.asarray(is_hole, dtype="u1").tofile(f"{base}.ring_is_hole.u8")
    np.asarray(ring_zone, dtype="<i4").tofile(f"{base}.ring_zone.i32")
    if source_ids is not None:
        np.asarray(list(source_ids), dtype="<i8").tofile(f"{base}.ids.i64")
    header = {"n_zones": int(len(zone_ids)), "n_rings": int(len(ring_zone)), "n_points": int(n_points)}
    header.update(meta or {})
    _write_text(json_path, json.dumps(header, indent=2) + "\n")


def read_zones(json_path):
    json_path = Path(json_path)
    meta = json.loads(json_path.read_text(encoding="utf-8"))
    base = json_path.parent / json_path.name[: -len(".json")]
    return {
        "meta": meta,
        "xy": np.fromfile(f"{base}.xy.f64", dtype="<f8"),
        "ring_start": np.fromfile(f"{base}.ring_start.i64", dtype="<i8"),
        "ring_is_hole": np.fromfile(f"{base}.ring_is_hole.u8", dtype="u1"),
        "ring_zone": np.fromfile(f"{base}.ring_zone.i32", dtype="<i4"),
    }


def zones_to_polygons(zones: dict):
    """Rebuilds shapely geometries (one per zone) from the flat ring arrays of read_zones."""
    import shapely
    from shapely.geometry import MultiPolygon, Polygon

    xy = zones["xy"].reshape(-1, 2)
    rs = zones["ring_start"]
    per_zone: dict[int, list] = {}
    current = None
    for i, (hole, zone) in enumerate(zip(zones["ring_is_hole"], zones["ring_zone"])):
        ring = xy[rs[i]:rs[i + 1]]
        if not hole:
            current = [ring, []]
            per_zone.setdefault(int(zone), []).append(current)
        else:
            current[1].append(ring)
    out = []
    for z in range(int(zones["meta"]["n_zones"])):
        parts = [Polygon(shell, holes) for shell, holes in per_zone.get(z, [])]
        out.append(parts[0] if len(parts) == 1 else MultiPolygon(parts) if parts else Polygon())
    return out


def centroid_cells(polygons: Sequence, transform) -> np.ndarray:
    """(row, col) of each polygon centroid with the truncating rule of the reference application."""
    origin_x, origin_y, pixel_w, pixel_h = (float(v) for v in transform)
    out = np.empty((len(polygons), 2), dtype=np.int32)
    for i, geom in enumerate(polygons):
        c = geom.centroid
        out[i, 0] = int(math.trunc((origin_y - c.y) / pixel_h))
        out[i, 1] = int(math.trunc((c.x - origin_x) / pixel_w))
    return out


def write_centroids(dir_path, zones_name, raster_name, cells: np.ndarray):
    np.ascontiguousarray(cells, dtype="<i4").tofile(Path(dir_path) / f"zones_{zones_name}.centroid_rc_{raster_name}.i32")


def read_centroids(dir_path, zones_name, raster_name) -> np.ndarray:
    return np.fromfile(Path(dir_path) / f"zones_{zones_name}.centroid_rc_{raster_name}.i32", dtype="<i4").reshape(-1, 2)


def write_fixture_json(dir_path, fixture_id, rasters, zones, area_km2=None, notes=""):
    doc = {"id": fixture_id, "rasters": list(rasters), "zones": list(zones), "area_km2": area_km2, "notes": notes}
    _write_text(Path(dir_path) / "fixture.json", json.dumps(doc, indent=2) + "\n")
