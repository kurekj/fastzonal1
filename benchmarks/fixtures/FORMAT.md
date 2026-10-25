# Fixture layout

A fixture is one directory, named by its id, holding rasters and zone sets in a raw binary form
that the C++, C# and Python harnesses read identically. All binaries are little-endian, rasters
are row-major (north-up, row 0 at the top). Nothing in a fixture depends on GDAL.

```
<id>/
  fixture.json                           { "id", "rasters": [...], "zones": [...], "area_km2", "notes" }
  raster_<name>.json                     header, see below
  raster_<name>.bin                      width * height values of the header's dtype
  zones_<name>.json                      { "n_zones", "n_rings", "n_points", "crs", "source", "licence" }
  zones_<name>.xy.f64                    n_points * 2 doubles: x0 y0 x1 y1 ...
  zones_<name>.ring_start.i64            n_rings + 1 offsets into the points
  zones_<name>.ring_is_hole.u8           1 for a hole of the preceding exterior ring, else 0
  zones_<name>.ring_zone.i32             zone index of each ring (0 .. n_zones - 1)
  zones_<name>.ids.i64                   optional: the source attribute of each zone (traceability)
  zones_<name>.centroid_rc_<raster>.i32  n_zones pairs (row, col): the centroid cell on that raster's grid
```

Raster header:

```json
{ "dtype": "f32 | f64 | i32", "width": W, "height": H,
  "transform": [origin_x, origin_y, pixel_w, pixel_h], "nodata": number or null,
  "crs": "EPSG:3035", "source": "...", "licence": "..." }
```

The transform is the north-up affine grid of `fz_transform`: pixel `(row, col)` has its centre at
`x = origin_x + (col + 0.5) * pixel_w`, `y = origin_y - (row + 0.5) * pixel_h`.

Rings are stored without the repeated closing point. A multipolygon zone contributes one exterior
ring per part, each followed by that part's holes, all carrying the same zone index. Zone indices
are `0 .. n_zones - 1` in the order of the source layer.

Centroid cells use the truncating rule of the reference application:
`row = trunc((origin_y - cy) / pixel_h)`, `col = trunc((cx - origin_x) / pixel_w)`, where
`(cx, cy)` is the shapely centroid. Cells may lie outside the raster; the library checks the range.

`prepare_fixture.py` creates these files from a GeoTIFF and any GDAL-readable polygon layer and
refuses inputs whose CRS differ. `make_synthetic.py` creates a download-free fixture.
