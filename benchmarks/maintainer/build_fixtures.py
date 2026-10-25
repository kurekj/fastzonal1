#!/usr/bin/env python3
"""Build the public-data benchmark fixtures.

Runs on the machine that hosts the SpongeGIS PostGIS database (the open pan-European layers
GEDTM30 terrain, CORINE Land Cover 2018 and Copernicus imperviousness are read from it with
read-only queries through `docker exec`). Catchment outlines come from HydroBASINS (CC BY 4.0),
downloaded on first use. Every file written is described in FORMAT.md; `manifest.json` records
sizes, hashes, sources and licences.

    python build_fixtures.py --out /data/fastzonal/fixtures --src /data/fastzonal/src [--only ID ...]

The database is never modified. Nothing here depends on the fastzonal library.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io as pyio
import json
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import os

import numpy as np
import pyogrio
import rasterio
import rasterio.features
import shapely
from pyproj import Transformer
from shapely.geometry import Point, box

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fzfix import grid, io  # noqa: E402

HYBAS_URL = "https://data.hydrosheds.org/file/HydroBASINS/standard/hybas_eu_lev{level:02d}_v1c.zip"
PSQL = ["docker", "exec", "spongegis-postgis-1", "psql", "-U", "postgis", "-d", "postgis", "-At", "-F", "|"]
MARGIN_M = 1000.0  # rasters extend this far beyond the boundary so every grid cell is covered

# id, HydroBASINS level, a point inside the basin (lon, lat), expected area, rasters, grid sizes
FIXTURES = [
    dict(id="leze_micro", level=10, lon=1.3385, lat=43.2590, target_km2=160, rasters=["dem30"],
         grids=[250], note="Leze (Ariege, France), the micro-scale case study region; small enough to commit"),
    dict(id="biebrza_upper", level=8, lon=23.3400, lat=53.6900, target_km2=863,
         rasters=["dem30", "dem60", "dem120", "clc100", "imp30"], grids=[1000, 500, 250, 100],
         note="Upper Biebrza (Podlaskie, Poland), the end-to-end case study"),
    dict(id="gradascica", level=10, lon=14.3300, lat=46.0600, target_km2=160, rasters=["dem30", "clc100"],
         grids=[250, 100], note="Gradascica (Slovenia), alpine case study"),
    dict(id="mazovia_5000", level=7, lon=20.3600, lat=52.6700, target_km2=5000,
         rasters=["dem30", "dem60", "dem120", "clc100"], grids=[1000, 500, 250, 100],
         note="Wkra basin (Mazovia, Poland), the 5,000 km2 scaling workload"),
]

# Upstream products behind the PostGIS layers. The layers were loaded into the SpongeGIS database
# by its import scripts (SPONGEGIS2026.NET, scripts/data-imports, May and September 2026): the
# terrain tiles are GEDTM30 reprojected to EPSG:3035 and generalised by averaging to 60 m and
# 120 m, CLC 2018 is the vector product in EPSG:3035, imperviousness is the 10 m product resampled
# to 30 m (bilinear). The fixture archive published with the paper is the reviewer's route; the
# database is only needed to rebuild it.
SOURCES = {
    "dem30": ("GEDTM30 global ensemble digital terrain model, 30 m, OpenGeoHub 2024, "
              "https://github.com/openlandmap/GEDTM30 (PostGIS table dem30, EPSG:3035)", "CC BY 4.0"),
    "dem60": ("GEDTM30 averaged to 60 m during the SpongeGIS import (PostGIS table dem60)", "CC BY 4.0"),
    "dem120": ("GEDTM30 averaged to 120 m during the SpongeGIS import (PostGIS table dem120)", "CC BY 4.0"),
    "clc100": ("CORINE Land Cover 2018 (CLC2018 v2020_20u1, vector), class code CODE_18 rasterised at 100 m "
               "from PostGIS table landuse_clc; https://land.copernicus.eu/en/products/corine-land-cover/clc2018",
               "Copernicus Land Monitoring Service: free, full and open access with attribution"),
    "imp30": ("Copernicus High Resolution Layer Imperviousness Density 2018 (10 m) resampled to 30 m during the "
              "SpongeGIS import (PostGIS table urban_imp30); 255 (unclassifiable) and the declared NoData -> -9999; "
              "https://land.copernicus.eu/en/products/high-resolution-layer-imperviousness/imperviousness-density-2018",
              "Copernicus Land Monitoring Service: free, full and open access with attribution"),
    "grid": ("square grid aligned to the cell size, clipped to a HydroBASINS v1c basin outline "
             "(https://www.hydrosheds.org/products/hydrobasins), reprojected to EPSG:3035", "CC BY 4.0"),
}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def psql(sql: str) -> str:
    proc = subprocess.run(PSQL + ["-c", sql], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"psql failed: {proc.stderr.strip()}")
    return proc.stdout


def hydrobasins(src: Path, level: int):
    zpath = src / f"hybas_eu_lev{level:02d}_v1c.zip"
    if not zpath.exists():
        log(f"downloading {zpath.name}")
        subprocess.run(["curl", "-sSL", "-o", str(zpath), HYBAS_URL.format(level=level)], check=True)
    with zipfile.ZipFile(zpath) as z:
        shp = [n for n in z.namelist() if n.endswith(".shp")][0]
    return pyogrio.read_dataframe(f"zip://{zpath}!{shp}") if _has_geopandas() else _read_raw(zpath, shp)


def _has_geopandas():
    try:
        import geopandas  # noqa: F401
        return True
    except ImportError:
        return False


def _read_raw(zpath, shp):
    meta, index, geometry, fields = pyogrio.raw.read(f"zip://{zpath}!{shp}", columns=["HYBAS_ID", "SUB_AREA"])
    geoms = shapely.from_wkb(geometry)
    return {"geometry": geoms, "HYBAS_ID": list(fields[0]), "SUB_AREA": list(fields[1])}


def select_basin(table, lon, lat):
    pt = Point(lon, lat)
    if isinstance(table, dict):
        geoms = table["geometry"]
        hit = np.flatnonzero(shapely.contains(geoms, pt))
        if hit.size == 0:
            raise RuntimeError(f"no basin contains ({lon}, {lat})")
        i = int(hit[0])
        return geoms[i], int(table["HYBAS_ID"][i]), float(table["SUB_AREA"][i])
    hit = table[table.geometry.contains(pt)]
    if hit.empty:
        raise RuntimeError(f"no basin contains ({lon}, {lat})")
    row = hit.iloc[0]
    return row.geometry, int(row["HYBAS_ID"]), float(row["SUB_AREA"])


def to_3035(geom):
    tr = Transformer.from_crs("EPSG:4326", "EPSG:3035", always_xy=True)

    def project(coords):
        x, y = tr.transform(coords[:, 0], coords[:, 1])
        return np.column_stack([x, y])

    return shapely.transform(geom, project)


def snapped_envelope(bounds, cell):
    """Envelope expanded by MARGIN_M and snapped outward to multiples of `cell`."""
    minx, miny, maxx, maxy = bounds
    minx = np.floor((minx - MARGIN_M) / cell) * cell
    miny = np.floor((miny - MARGIN_M) / cell) * cell
    maxx = np.ceil((maxx + MARGIN_M) / cell) * cell
    maxy = np.ceil((maxy + MARGIN_M) / cell) * cell
    return float(minx), float(miny), float(maxx), float(maxy)


def fetch_raster_table(table: str, env) -> tuple[np.ndarray, tuple, float | None]:
    """Clips the PostGIS raster table to the envelope and returns (array, transform, nodata)."""
    minx, miny, maxx, maxy = env
    sql = (f"select encode(ST_AsTIFF(ST_Union(ST_Clip(rast, ST_MakeEnvelope({minx},{miny},{maxx},{maxy},3035), true))), "
           f"'base64') from {table} where ST_Intersects(rast, ST_MakeEnvelope({minx},{miny},{maxx},{maxy},3035))")
    b64 = psql(sql).strip()
    if not b64:
        raise RuntimeError(f"{table}: no tiles in envelope {env}")
    data = base64.b64decode(b64)
    with rasterio.open(pyio.BytesIO(data)) as src:
        arr = src.read(1)
        t = src.transform
        return arr, (t.c, t.f, t.a, -t.e), src.nodata


def fetch_clc_raster(env, cell=100.0) -> tuple[np.ndarray, tuple]:
    minx, miny, maxx, maxy = env
    sql = (f"select code_18, encode(ST_AsBinary(geom),'hex') from landuse_clc "
           f"where geom && ST_MakeEnvelope({minx},{miny},{maxx},{maxy},3035)")
    rows = [line.split("|", 1) for line in psql(sql).splitlines() if line]
    shapes = [(shapely.from_wkb(bytes.fromhex(wkb)), int(code)) for code, wkb in rows]
    width = int(round((maxx - minx) / cell))
    height = int(round((maxy - miny) / cell))
    transform = rasterio.transform.from_origin(minx, maxy, cell, cell)
    arr = rasterio.features.rasterize(shapes, out_shape=(height, width), transform=transform, fill=0,
                                      dtype="int32", all_touched=False)
    return arr, (minx, maxy, cell, cell)


def write_raster(fdir: Path, name: str, arr, transform, nodata):
    src, lic = SOURCES[name]
    io.write_raster(fdir / f"raster_{name}.json", arr, transform, nodata,
                    {"crs": "EPSG:3035", "source": src, "licence": lic})


def build_fixture(spec, src: Path, out: Path, basins_cache: dict):
    fid = spec["id"]
    fdir = out / fid
    fdir.mkdir(parents=True, exist_ok=True)
    table = basins_cache.setdefault(spec["level"], hydrobasins(src, spec["level"]))
    geom_wgs, hybas_id, area_km2 = select_basin(table, spec["lon"], spec["lat"])
    boundary = to_3035(geom_wgs)
    area_3035 = boundary.area / 1e6
    log(f"{fid}: HYBAS_ID {hybas_id}, level {spec['level']}, area {area_3035:.1f} km2 (target {spec['target_km2']})")
    if not (0.4 * spec["target_km2"] <= area_3035 <= 2.5 * spec["target_km2"]):
        log(f"  warning: area differs from the target by more than the expected tolerance")

    rasters = []
    for name in spec["rasters"]:
        t0 = time.time()
        if name == "clc100":
            arr, tr = fetch_clc_raster(snapped_envelope(boundary.bounds, 100.0))
            write_raster(fdir, name, arr, tr, 0)
        elif name == "imp30":
            arr, tr, nd = fetch_raster_table("urban_imp30", snapped_envelope(boundary.bounds, 30.0))
            arr = arr.astype(np.float32)
            invalid = arr == 255  # the product's unclassifiable code, independent of the declared NoData
            if nd is not None:
                invalid |= arr == np.float32(nd)
            invalid |= ~np.isfinite(arr)
            arr[invalid] = -9999.0
            write_raster(fdir, name, arr, tr, -9999.0)
        else:
            cell = {"dem30": 30.0, "dem60": 60.0, "dem120": 120.0}[name]
            arr, tr, nd = fetch_raster_table(name, snapped_envelope(boundary.bounds, cell))
            arr = arr.astype(np.float32)
            if nd is not None:
                arr[arr == np.float32(nd)] = -9999.0
            arr[~np.isfinite(arr)] = -9999.0
            write_raster(fdir, name, arr, tr, -9999.0)
        rasters.append(name)
        log(f"  {name}: {arr.shape[1]}x{arr.shape[0]} in {time.time() - t0:.1f}s")

    zones = []
    for size in spec["grids"]:
        t0 = time.time()
        cells, ids = grid.square_grid(boundary, float(size), 0.01)
        zname = f"grid{size}"
        src_txt, lic = SOURCES["grid"]
        io.write_zones(fdir / f"zones_{zname}.json", cells, ids,
                       {"crs": "EPSG:3035", "source": f"{src_txt}; HYBAS_ID {hybas_id}", "licence": lic,
                        "cell_size_m": size}, source_ids=ids)
        for name in rasters:
            meta = json.loads((fdir / f"raster_{name}.json").read_text())
            io.write_centroids(fdir, zname, name, io.centroid_cells(cells, meta["transform"]))
        zones.append(zname)
        log(f"  {zname}: {len(cells)} zones in {time.time() - t0:.1f}s")

    io.write_fixture_json(fdir, fid, rasters, zones, round(area_3035, 2),
                          f"{spec['note']}; HydroBASINS v1c level {spec['level']} HYBAS_ID {hybas_id}")
    return fdir


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_table(directory: Path) -> dict:
    return {p.name: {"bytes": p.stat().st_size, "sha256": sha256(p)} for p in sorted(directory.iterdir()) if p.is_file()}


def write_manifest(out: Path, built: list[Path], expected: Path | None, generated: list[Path]):
    manifest_path = out / "manifest.json"
    doc = json.loads(manifest_path.read_text()) if manifest_path.exists() else {
        "archive": {"url": "https://zenodo.org/record/TODO/files/fixtures-v1.tar.zst", "sha256": None},
        "fixtures": {}, "expected": {}, "generated": {}}
    doc.setdefault("expected", {})
    doc.setdefault("generated", {})
    for fdir in built:
        doc["fixtures"][fdir.name] = {"fixture": json.loads((fdir / "fixture.json").read_text()),
                                      "files": file_table(fdir), "sources": SOURCES}
    if expected is not None and expected.exists():
        for edir in sorted(p for p in expected.iterdir() if p.is_dir()):
            doc["expected"][edir.name] = {"files": file_table(edir),
                                          "note": "C# baseline outputs (FzBench expected), stored with the archive"}
    for gdir in generated:
        doc["generated"][gdir.name] = {"files": file_table(gdir), "fixture": json.loads((gdir / "fixture.json").read_text()),
                                       "note": "reproduced by benchmarks/make_synthetic.py; hashes pin the bytes"}
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(doc, indent=2) + "\n")
    log(f"manifest: {manifest_path} ({len(doc['fixtures'])} fixtures, {len(doc['expected'])} expected sets, "
        f"{len(doc['generated'])} generated)")


def main(argv=None):
    # GDAL inside rasterio/pyogrio looks for PROJ data in the environment; point it at pyproj's copy
    if "PROJ_DATA" not in os.environ:
        import pyproj.datadir
        os.environ["PROJ_DATA"] = pyproj.datadir.get_data_dir()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--src", type=Path, required=True, help="download cache for HydroBASINS")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--expected", type=Path, default=None, help="directory of C# expected outputs to list")
    ap.add_argument("--generated", type=Path, nargs="*", default=[], help="generated fixture dirs to list")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    args.src.mkdir(parents=True, exist_ok=True)
    basins = {}
    built = []
    for spec in FIXTURES:
        if args.only and spec["id"] not in args.only:
            continue
        built.append(build_fixture(spec, args.src, args.out, basins))
    write_manifest(args.out, built, args.expected, args.generated)
    return 0


if __name__ == "__main__":
    sys.exit(main())
