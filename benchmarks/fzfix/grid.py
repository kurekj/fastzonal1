"""Square planning-unit grids clipped to a boundary, as the reference application generates them."""

from __future__ import annotations

import math

import numpy as np
import shapely


def square_grid(boundary, size: float, min_area_share: float = 0.01):
    """Axis-aligned square cells of `size` (map units) aligned to multiples of `size`, clipped to
    `boundary`. Cells whose clipped area is below `min_area_share` of a full cell are dropped.
    Returns (cells, ids): shapely geometries in row-major order from the north-west corner and
    their ids 0..n-1 in that order."""
    if size <= 0:
        raise ValueError("size must be positive")
    minx, miny, maxx, maxy = boundary.bounds
    x_start = math.floor(minx / size) * size
    x_stop = math.ceil(maxx / size) * size
    y_top = math.ceil(maxy / size) * size
    y_bottom = math.floor(miny / size) * size
    n_cols = max(1, int(round((x_stop - x_start) / size)))
    n_rows = max(1, int(round((y_top - y_bottom) / size)))

    cols = np.arange(n_cols)
    rows = np.arange(n_rows)
    cc, rr = np.meshgrid(cols, rows)  # row-major: rr varies slowest
    x0 = x_start + cc.ravel() * size
    y1 = y_top - rr.ravel() * size
    cells = shapely.box(x0, y1 - size, x0 + size, y1)
    clipped = shapely.intersection(cells, boundary)
    area = shapely.area(clipped)
    keep = area >= min_area_share * size * size
    keep &= ~shapely.is_empty(clipped)
    kept = clipped[keep]
    # intersections of a box with a polygon may yield collections with line or point parts
    out = []
    for g in kept:
        if g.geom_type == "GeometryCollection":
            polys = [p for p in g.geoms if p.geom_type in ("Polygon", "MultiPolygon")]
            g = shapely.union_all(polys) if polys else shapely.Polygon()
        out.append(g)
    return out, list(range(len(out)))
