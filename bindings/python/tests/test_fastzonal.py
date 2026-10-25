import csv
import os
import sys
from pathlib import Path

import numpy as np
import pytest
from shapely.geometry import Polygon, box

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "bindings" / "python"))
sys.path.insert(0, str(ROOT / "benchmarks"))

import fastzonal as fz  # noqa: E402
from fzfix import io  # noqa: E402


def test_version_and_errors():
    assert fz.version() == "0.1.0"
    raster = np.zeros((4, 4), dtype=np.float32)
    mask = np.zeros((4, 4), dtype=np.int32)
    mask[0, 0] = 5
    with pytest.raises(fz.FastZonalError) as e:
        fz.zonal(raster, mask, 2, nodata=-9999.0)
    assert e.value.code == 1 and "invalid argument" in str(e.value)


def _rings_reference(polygons):
    # straightforward per-ring loop, the specification polygons_to_rings must reproduce
    xy, ring_start, is_hole, ring_zone, n = [], [0], [], [], 0
    for zone, geom in enumerate(polygons):
        parts = [geom] if geom.geom_type == "Polygon" else list(geom.geoms)
        for part in parts:
            for k, ring in enumerate([part.exterior, *part.interiors]):
                coords = np.asarray(ring.coords, dtype=np.float64)[:-1, :2]
                xy.append(coords.reshape(-1))
                n += len(coords)
                ring_start.append(n)
                is_hole.append(1 if k > 0 else 0)
                ring_zone.append(zone)
    return (np.concatenate(xy), np.asarray(ring_start, dtype=np.int64), np.asarray(is_hole, dtype=np.uint8),
            np.asarray(ring_zone, dtype=np.int32))


def test_polygons_to_rings_matches_reference_and_roundtrips():
    from shapely.geometry import MultiPolygon
    holed = Polygon([(0, 0), (10, 0), (10, 10), (0, 10)], holes=[[(4, 4), (6, 4), (6, 6), (4, 6)], [(1, 1), (2, 1), (2, 2)]])
    multi = MultiPolygon([box(20, 20, 25, 25), Polygon([(30, 30), (40, 30), (40, 40), (30, 40)], holes=[[(33, 33), (35, 33), (35, 35)]])])
    polys = [box(0, 0, 1, 1), holed, multi, box(50, 50, 51, 52)]
    got = fz.polygons_to_rings(polys)
    want = _rings_reference(polys)
    for g, w in zip(got, want):
        assert g.dtype == w.dtype and np.array_equal(g, w)
    assert got[2].tolist() == [0, 0, 1, 1, 0, 0, 1, 0] and got[3].tolist() == [0, 1, 1, 1, 2, 2, 2, 3]
    back = fz.rings_to_polygons(got, 5)
    assert [g.geom_type for g in back] == ["Polygon", "Polygon", "MultiPolygon", "Polygon", "Polygon"]
    assert all(a.equals(b) for a, b in zip(back[:4], polys)) and back[4].is_empty
    again = fz.polygons_to_rings(list(back[:4]))
    assert all(np.array_equal(a, b) for a, b in zip(again, got))


def test_rasterize_square_center_and_compat():
    sq = box(2, 2, 5, 5)
    m = fz.rasterize([sq], transform=(0, 10, 1, 1), width=10, height=10)
    assert m.shape == (10, 10) and m.dtype == np.int32
    assert int((m == 0).sum()) == 9 and m[5, 2] == 0 and m[4, 2] == -1
    c = fz.rasterize([sq], transform=(0, 10, 1, 1), width=10, height=10, mode="compat")
    assert int((c == 0).sum()) == 12


def test_hole_and_multipolygon():
    outer = Polygon([(0, 0), (10, 0), (10, 10), (0, 10)], holes=[[(4, 4), (6, 4), (6, 6), (4, 6)]])
    m = fz.rasterize([outer], transform=(0, 10, 1, 1), width=10, height=10)
    assert m[5, 5] == -1 and m[1, 1] == 0


def test_zonal_mean_majority_ratio():
    raster = np.array([[1, 2, 3, 4], [5, 6, 7, 8], [10, 20, 30, 40], [99, 99, 99, 99]], dtype=np.float32)
    mask = np.array([[0] * 4, [0] * 4, [1] * 4, [-1] * 4], dtype=np.int32)
    st = fz.zonal(raster, mask, 2, nodata=-9999.0)
    assert st["count"][0] == 8 and st["sum"][0] == 36.0 and st["min"][0] == 1.0 and st["max"][1] == 40.0
    mean = fz.mean(raster, mask, 2, nodata=-9999.0)
    assert mean[0] == 4.5 and mean[1] == 25.0
    classes = np.array([[3, 3, 1, 1], [5, 5, 5, 2], [7, 7, 8, 8], [0, 0, 0, 0]], dtype=np.int32)
    mode = fz.majority(classes, mask, 2, nodata=-1)
    assert mode[0] == 5.0 and mode[1] == 7.0
    byte = classes.astype(np.uint8)
    assert fz.majority(byte, mask, 2, nodata=255)[1] == 7.0
    den = np.ones_like(raster)
    r = fz.ratio(raster, den, mask, 2, nodata_num=-9999.0, nodata_den=-9999.0)
    assert r["count"][0] == 8 and r["sum"][0] == 36.0


def test_empty_zone_centroid_fallback():
    raster = np.full((4, 4), 2.0, dtype=np.float32)
    raster[3, 1] = 7.0
    mask = np.zeros((4, 4), dtype=np.int32)
    mean = fz.mean(raster, mask, 2, nodata=-9999.0, centroids=(np.array([0, 3]), np.array([0, 1])))
    assert mean[1] == 7.0


def test_options_openmp_equals_serial():
    rng = np.random.default_rng(1)
    raster = rng.random((300, 400), dtype=np.float32) * 100
    zones = [box(x, y, x + 37, y + 23) for x in range(0, 400, 40) for y in range(0, 300, 25)]
    mask = fz.rasterize(zones, transform=(0, 300, 1, 1), width=400, height=300)
    a = fz.zonal(raster, mask, len(zones), nodata=-9999.0, options=fz.Options(backend="serial", band_rows=16))
    if fz.has_openmp():
        b = fz.zonal(raster, mask, len(zones), nodata=-9999.0, options=fz.Options(backend="openmp", threads=3, band_rows=16))
        assert np.array_equal(a.view(np.uint8), b.view(np.uint8))


def test_parity_with_csharp_baseline_on_leze():
    d = ROOT / "tests" / "fixtures" / "leze_micro"
    raster, meta = io.read_raster(d / "raster_dem30.json")
    raster = raster.astype(np.float32)
    if meta["nodata"] is not None:
        raster[raster == np.float32(meta["nodata"])] = -9999.0
    z = io.read_zones(d / "zones_grid250.json")
    rings = (z["xy"], z["ring_start"], z["ring_is_hole"], z["ring_zone"])
    mask = fz.rasterize(rings=rings, transform=meta["transform"], width=meta["width"], height=meta["height"],
                        mode="compat")
    cells = io.read_centroids(d, "grid250", "dem30")
    mean = fz.mean(raster, mask, z["meta"]["n_zones"], nodata=-9999.0, centroids=(cells[:, 0], cells[:, 1]))
    with open(ROOT / "tests" / "fixtures" / "expected" / "leze_micro" / "mean_grid250_dem30.csv") as f:
        want = np.array([float(r["value"]) for r in csv.DictReader(f)])
    assert np.allclose(mean, want, rtol=1e-9, atol=1e-9)
