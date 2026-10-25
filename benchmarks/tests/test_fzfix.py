import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
import rasterio
from rasterio.transform import from_origin
from shapely.geometry import MultiPolygon, Polygon, box

BENCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH))

from fzfix import io  # noqa: E402


def test_raster_round_trip_f32_and_f64(tmp_path):
    arr32 = np.arange(12, dtype=np.float32).reshape(3, 4)
    arr64 = np.arange(12, dtype=np.float64).reshape(3, 4) / 7.0
    for arr, dtype in ((arr32, "f32"), (arr64, "f64")):
        path = tmp_path / f"raster_{dtype}.json"
        io.write_raster(path, arr, (0.0, 10.0, 1.0, 1.0), -9999.0, {"crs": "EPSG:3035", "source": "test"})
        back, meta = io.read_raster(path)
        assert back.dtype == arr.dtype
        assert np.array_equal(back, arr)
        assert meta["dtype"] == dtype
        assert meta["width"] == 4 and meta["height"] == 3
        assert meta["transform"] == [0.0, 10.0, 1.0, 1.0]
        assert meta["nodata"] == -9999.0
        assert meta["crs"] == "EPSG:3035"


def test_raster_i32_and_null_nodata(tmp_path):
    arr = np.arange(6, dtype=np.int32).reshape(2, 3)
    path = tmp_path / "raster_c.json"
    io.write_raster(path, arr, (0.0, 2.0, 1.0, 1.0), None, {})
    back, meta = io.read_raster(path)
    assert back.dtype == np.int32
    assert meta["nodata"] is None


def test_zones_square_with_hole(tmp_path):
    poly = Polygon([(0, 0), (10, 0), (10, 10), (0, 10)], holes=[[(4, 4), (6, 4), (6, 6), (4, 6)]])
    path = tmp_path / "zones_a.json"
    io.write_zones(path, [poly], [0], {"crs": "EPSG:3035"})
    z = io.read_zones(path)
    assert z["meta"]["n_zones"] == 1
    assert z["meta"]["n_rings"] == 2
    assert list(z["ring_is_hole"]) == [0, 1]
    assert list(z["ring_zone"]) == [0, 0]
    assert z["ring_start"].dtype == np.int64 and z["ring_start"][0] == 0
    assert z["xy"].dtype == np.float64
    assert z["xy"].shape == (2 * z["ring_start"][-1],)
    # rings are stored without the repeated closing point
    assert z["ring_start"][1] == 4
    assert z["ring_start"][2] == 8


def test_zones_multipolygon_parts_share_zone(tmp_path):
    mp = MultiPolygon([box(0, 0, 2, 2), box(5, 5, 7, 7)])
    path = tmp_path / "zones_m.json"
    io.write_zones(path, [box(10, 10, 12, 12), mp], [0, 1], {})
    z = io.read_zones(path)
    assert list(z["ring_zone"]) == [0, 1, 1]
    assert list(z["ring_is_hole"]) == [0, 0, 0]
    assert z["meta"]["n_zones"] == 2


def test_centroid_cells_truncating_rule():
    cells = io.centroid_cells([box(2, 2, 4, 4)], (0.0, 10.0, 1.0, 1.0))
    assert cells.dtype == np.int32
    assert cells.shape == (1, 2)
    assert tuple(cells[0]) == (7, 3)  # centroid (3, 3): row trunc((10 - 3) / 1) = 7, col trunc(3 / 1) = 3


def test_make_synthetic_cli(tmp_path):
    out = tmp_path / "synthetic_small"
    subprocess.run(
        [sys.executable, str(BENCH / "make_synthetic.py"), "--width", "300", "--height", "200", "--zones", "50",
         "--seed", "7", "--out", str(out)],
        check=True,
    )
    arr, meta = io.read_raster(out / "raster_synth.json")
    assert arr.shape == (200, 300) and arr.dtype == np.float32
    assert meta["nodata"] == -9999.0
    nodata_share = np.mean(arr == -9999.0)
    assert 0.03 < nodata_share < 0.07
    z = io.read_zones(out / "zones_rect.json")
    assert z["meta"]["n_zones"] == 50
    fixture = json.loads((out / "fixture.json").read_text())
    assert fixture["id"] == "synthetic_small"
    assert (out / "zones_rect.centroid_rc_synth.i32").exists()
    assert (out / "check_mean_zone0.txt").exists()
    # the same seed reproduces the same bytes
    out2 = tmp_path / "again"
    subprocess.run(
        [sys.executable, str(BENCH / "make_synthetic.py"), "--width", "300", "--height", "200", "--zones", "50",
         "--seed", "7", "--out", str(out2)],
        check=True,
    )
    assert (out / "raster_synth.bin").read_bytes() == (out2 / "raster_synth.bin").read_bytes()


def _write_tif(path, crs, transform=from_origin(0, 10, 1, 1)):
    data = np.arange(100, dtype=np.float32).reshape(10, 10)
    with rasterio.open(path, "w", driver="GTiff", height=10, width=10, count=1, dtype="float32", crs=crs,
                       transform=transform, nodata=-9999.0) as dst:
        dst.write(data, 1)


def _write_geojson(path, epsg, ids, geoms):
    from shapely.geometry import mapping

    doc = {
        "type": "FeatureCollection",
        "crs": {"type": "name", "properties": {"name": f"urn:ogc:def:crs:EPSG::{epsg}"}},
        "features": [{"type": "Feature", "properties": {"ID": i}, "geometry": mapping(g)} for i, g in zip(ids, geoms)],
    }
    path.write_text(json.dumps(doc))


def test_prepare_fixture_rejects_crs_mismatch(tmp_path):
    tif = tmp_path / "dem.tif"
    _write_tif(tif, "EPSG:3035")
    gpkg = tmp_path / "zones.geojson"
    _write_geojson(gpkg, 4326, [1, 2], [box(1, 1, 3, 3), box(5, 5, 8, 8)])
    proc = subprocess.run(
        [sys.executable, str(BENCH / "prepare_fixture.py"), "--raster", str(tif), "--name", "dem",
         "--zones", str(gpkg), "--zone-field", "ID", "--zones-name", "grid", "--out", str(tmp_path / "fx")],
        capture_output=True, text=True,
    )
    assert proc.returncode != 0
    assert "CRS mismatch" in proc.stderr


def test_prepare_fixture_writes_layout(tmp_path):
    tif = tmp_path / "dem.tif"
    _write_tif(tif, "EPSG:3035")
    gpkg = tmp_path / "zones.geojson"
    _write_geojson(gpkg, 3035, [7, 9], [box(1, 1, 3, 3), box(5, 5, 8, 8)])
    out = tmp_path / "fx"
    subprocess.run(
        [sys.executable, str(BENCH / "prepare_fixture.py"), "--raster", str(tif), "--name", "dem",
         "--zones", str(gpkg), "--zone-field", "ID", "--zones-name", "grid", "--out", str(out), "--id", "fx"],
        check=True,
    )
    arr, meta = io.read_raster(out / "raster_dem.json")
    assert arr.shape == (10, 10) and meta["crs"] == "EPSG:3035" and meta["nodata"] == -9999.0
    z = io.read_zones(out / "zones_grid.json")
    assert z["meta"]["n_zones"] == 2
    ids = np.fromfile(out / "zones_grid.ids.i64", dtype=np.int64)
    assert list(ids) == [7, 9]
    cells = np.fromfile(out / "zones_grid.centroid_rc_dem.i32", dtype=np.int32).reshape(-1, 2)
    assert tuple(cells[0]) == (8, 2)  # centroid (2, 2) -> row trunc(8), col 2
    fixture = json.loads((out / "fixture.json").read_text())
    assert fixture["id"] == "fx"
    assert "dem" in fixture["rasters"] and "grid" in fixture["zones"]


# ---- cases added after the independent review of the fixture tooling ----

def test_nan_nodata_is_written_as_null(tmp_path):
    arr = np.array([[1.0, np.nan], [3.0, 4.0]], dtype=np.float32)
    path = tmp_path / "raster_n.json"
    io.write_raster(path, arr, (0.0, 2.0, 1.0, 1.0), float("nan"), {})
    text = path.read_text()
    assert "NaN" not in text and "nan" not in text
    json.loads(text)  # strict parsers must accept the header
    back, meta = io.read_raster(path)
    assert meta["nodata"] is None
    assert np.isnan(back[0, 1])


def test_infinite_nodata_is_rejected(tmp_path):
    arr = np.zeros((2, 2), dtype=np.float32)
    with pytest.raises(ValueError):
        io.write_raster(tmp_path / "raster_i.json", arr, (0.0, 2.0, 1.0, 1.0), float("inf"), {})


def test_synthetic_small_bytes_are_fixed(tmp_path):
    """The committed synthetic fixture must be reproducible bit for bit on every platform."""
    import hashlib

    out = tmp_path / "synthetic_small"
    subprocess.run([sys.executable, str(BENCH / "make_synthetic.py"), "--width", "300", "--height", "200",
                    "--zones", "50", "--seed", "7", "--out", str(out)], check=True)
    raster_hash = hashlib.sha256((out / "raster_synth.bin").read_bytes()).hexdigest()
    zones_hash = hashlib.sha256((out / "zones_rect.xy.f64").read_bytes()).hexdigest()
    assert raster_hash == "c80c0b64e2c77324bd1d961ce3347f6c730be1575f1e3c4fb87dce38743361a9"
    assert zones_hash == "7a25330799da48e1548e657c38d7033ec3d2fcc5cf93967eebe0595664dc3f06"
    z = io.read_zones(out / "zones_rect.json")
    assert z["meta"]["licence"] == "MIT"


def test_prepare_fixture_remaps_integer_nodata_and_rejects_wide_values(tmp_path):
    data = np.array([[1, 2], [4294967295, 3]], dtype=np.uint32)
    tif = tmp_path / "classes.tif"
    with rasterio.open(tif, "w", driver="GTiff", height=2, width=2, count=1, dtype="uint32", crs="EPSG:3035",
                       transform=from_origin(0, 2, 1, 1), nodata=4294967295) as dst:
        dst.write(data, 1)
    out = tmp_path / "fx"
    subprocess.run([sys.executable, str(BENCH / "prepare_fixture.py"), "--raster", str(tif), "--name", "cls",
                    "--out", str(out)], check=True)
    arr, meta = io.read_raster(out / "raster_cls.json")
    assert arr.dtype == np.int32 and meta["nodata"] == -9999
    assert arr[1, 0] == -9999 and arr[0, 1] == 2
    # a valid value that is not exact in float32 is refused
    wide = np.array([[1, 20000000], [3, 4]], dtype=np.uint32)
    tif2 = tmp_path / "wide.tif"
    with rasterio.open(tif2, "w", driver="GTiff", height=2, width=2, count=1, dtype="uint32", crs="EPSG:3035",
                       transform=from_origin(0, 2, 1, 1)) as dst:
        dst.write(wide, 1)
    proc = subprocess.run([sys.executable, str(BENCH / "prepare_fixture.py"), "--raster", str(tif2), "--name", "w",
                           "--out", str(tmp_path / "fx2")], capture_output=True, text=True)
    assert proc.returncode != 0 and "2^24" in proc.stderr
