#!/usr/bin/env python3
"""Convert a GeoTIFF and a polygon layer into the raw fixture layout.

    python prepare_fixture.py --raster dem.tif --name dem --zones grid.gpkg --zone-field ID \
        --zones-name grid100 --out benchmarks/fixtures/<id> [--id <id>]

Both inputs must share one CRS; the library has no CRS awareness, so a mismatch is refused.
Zone ids are assigned 0..n-1 in file order; the values of --zone-field are kept in
zones_<name>.ids.i64 for traceability. The script may be run once per raster and once per zone
set on the same output directory; fixture.json accumulates the names.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fzfix import io  # noqa: E402


def crs_key(crs) -> str:
    """A comparable CRS string: EPSG code when known, else the WKT."""
    if crs is None:
        return "none"
    try:
        epsg = crs.to_epsg()
    except Exception:  # pragma: no cover - exotic CRS objects
        epsg = None
    return f"EPSG:{epsg}" if epsg else crs.to_wkt()


def load_raster(path: Path):
    import rasterio

    with rasterio.open(path) as src:
        if src.count != 1:
            sys.exit(f"{path}: expected a single band, found {src.count}")
        t = src.transform
        if t.b != 0 or t.d != 0 or t.e >= 0 or t.a <= 0:
            sys.exit(f"{path}: raster must be north-up with positive pixel sizes")
        data = src.read(1)
        nodata = src.nodata
        if data.dtype in (np.float32, np.float64):
            pass
        elif np.issubdtype(data.dtype, np.integer):
            # class codes: NoData becomes the reserved -9999 before narrowing, and every valid value
            # must be exact in float32 (FORMAT.md), so nothing is silently changed
            valid = np.ones(data.shape, dtype=bool) if nodata is None else data != data.dtype.type(nodata)
            if valid.any():
                lo, hi = int(data[valid].min()), int(data[valid].max())
                if lo <= -(1 << 24) or hi >= (1 << 24):
                    sys.exit(f"{path}: integer values must be below 2^24 in magnitude (found {lo}..{hi})")
            out = np.full(data.shape, -9999, dtype=np.int32)
            out[valid] = data[valid].astype(np.int32)
            data, nodata = out, -9999
        else:
            sys.exit(f"{path}: unsupported dtype {data.dtype}")
        return data, (t.c, t.f, t.a, -t.e), nodata, crs_key(src.crs)


def load_zones(path: Path, zone_field: str | None):
    import pyogrio
    from shapely import from_wkb

    table = pyogrio.read_dataframe(path) if _has_geopandas() else None
    if table is not None:
        geoms = list(table.geometry)
        ids = list(table[zone_field]) if zone_field else list(range(len(geoms)))
        return geoms, ids, crs_key(table.crs)
    meta, index, geometry, fields = pyogrio.raw.read(path, columns=[zone_field] if zone_field else [])
    geoms = [from_wkb(g) for g in geometry]
    ids = list(fields[0]) if zone_field else list(range(len(geoms)))
    from pyproj import CRS

    crs = CRS.from_user_input(meta["crs"]) if meta.get("crs") else None
    return geoms, ids, crs_key(crs)


def _has_geopandas() -> bool:
    try:
        import geopandas  # noqa: F401

        return True
    except ImportError:
        return False


def update_fixture_json(out: Path, fixture_id: str, raster_name: str | None, zones_name: str | None, notes: str):
    path = out / "fixture.json"
    doc = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {
        "id": fixture_id, "rasters": [], "zones": [], "area_km2": None, "notes": notes}
    if raster_name and raster_name not in doc["rasters"]:
        doc["rasters"].append(raster_name)
    if zones_name and zones_name not in doc["zones"]:
        doc["zones"].append(zones_name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(doc, indent=2) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raster", type=Path, help="GeoTIFF (single band)")
    ap.add_argument("--name", help="raster name inside the fixture, e.g. dem30")
    ap.add_argument("--zones", type=Path, help="polygon layer readable by GDAL (gpkg, shp, zip, geojson)")
    ap.add_argument("--zone-field", default=None, help="attribute kept as the source id")
    ap.add_argument("--zones-name", help="zone set name inside the fixture, e.g. grid100")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--id", default=None)
    ap.add_argument("--source", default="", help="provenance note stored in the headers")
    ap.add_argument("--licence", default="", help="licence note stored in the headers")
    ap.add_argument("--notes", default="")
    args = ap.parse_args(argv)
    if bool(args.raster) != bool(args.name) or bool(args.zones) != bool(args.zones_name):
        sys.exit("--raster needs --name, --zones needs --zones-name")
    if not args.raster and not args.zones:
        sys.exit("nothing to do: give --raster and/or --zones")

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    fixture_id = args.id or out.name

    raster = zones = None
    if args.raster:
        raster = load_raster(args.raster)
    if args.zones:
        zones = load_zones(args.zones, args.zone_field)
    new_crs = raster[3] if raster else zones[2]
    if raster and zones and raster[3] != zones[2]:
        sys.exit(f"CRS mismatch: raster {raster[3]} vs zones {zones[2]}")
    if zones:
        for i, g in enumerate(zones[0]):
            if g is None or g.is_empty:
                sys.exit(f"zone {i} has an empty or null geometry; remove it from the layer first")
    # every header already in the fixture must agree with the new data before anything is written
    for hj in sorted(out.glob("raster_*.json")) + sorted(out.glob("zones_*.json")):
        meta = json.loads(hj.read_text(encoding="utf-8"))
        if meta.get("crs") != new_crs:
            sys.exit(f"CRS mismatch: {hj.name} is {meta.get('crs')}, new data is {new_crs}")

    if raster:
        data, transform, nodata, crs = raster
        io.write_raster(out / f"raster_{args.name}.json", data, transform, nodata,
                        {"crs": crs, "source": args.source or str(args.raster.name), "licence": args.licence})
    if zones:
        geoms, ids, crs = zones
        io.write_zones(out / f"zones_{args.zones_name}.json", geoms, list(range(len(geoms))),
                       {"crs": crs, "source": args.source or str(args.zones.name), "licence": args.licence},
                       source_ids=ids)
    # centroid cells for every (zone set, raster) pair of the fixture; zone sets written earlier are
    # rebuilt from their ring arrays so a raster added later gets its centroid file too
    rasters = {rj.name[len("raster_"):-len(".json")]: json.loads(rj.read_text(encoding="utf-8"))
               for rj in sorted(out.glob("raster_*.json"))}
    for zj in sorted(out.glob("zones_*.json")):
        zname = zj.name[len("zones_"):-len(".json")]
        geoms = zones[0] if (zones and zname == args.zones_name) else io.zones_to_polygons(io.read_zones(zj))
        for rname, meta in rasters.items():
            io.write_centroids(out, zname, rname, io.centroid_cells(geoms, meta["transform"]))
    update_fixture_json(out, fixture_id, args.name, args.zones_name, args.notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
