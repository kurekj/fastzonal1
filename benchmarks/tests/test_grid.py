import sys
from pathlib import Path

import numpy as np
from shapely.geometry import Polygon, box

BENCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH))

from fzfix import grid  # noqa: E402


def test_square_grid_aligned_and_row_major():
    boundary = box(105, 203, 395, 497)  # not aligned to the 100 m cells
    cells, ids = grid.square_grid(boundary, 100.0, min_area_share=0.01)
    # cells cover multiples of 100 from 100 to 400 (x) and 200 to 500 (y): 3 columns x 3 rows
    assert len(cells) == 9
    assert ids == list(range(9))
    # row-major from the top (north) row: first cell is the north-west one
    b0 = cells[0].bounds
    assert b0[0] == 105 and b0[3] == 497  # clipped to the boundary
    assert cells[1].bounds[0] == 200
    assert cells[3].bounds[3] == 400
    # interior cells are full squares, edge cells are clipped
    assert abs(cells[4].area - 100 * 100) < 1e-9
    assert cells[0].area < 100 * 100


def test_small_slivers_dropped():
    boundary = box(0, 0, 100.5, 100)  # a 0.5 m sliver column on the right
    cells, _ = grid.square_grid(boundary, 100.0, min_area_share=0.01)
    assert len(cells) == 1
    cells, _ = grid.square_grid(boundary, 100.0, min_area_share=0.0)
    assert len(cells) == 2


def test_non_rectangular_boundary_clips_cells():
    tri = Polygon([(0, 0), (300, 0), (0, 300)])
    cells, _ = grid.square_grid(tri, 100.0, min_area_share=0.01)
    # 6 cells touch the triangle with area >= 1% of a cell (the corner cells are halves)
    assert len(cells) == 6
    assert all(c.area <= 100 * 100 + 1e-9 for c in cells)
    assert abs(sum(c.area for c in cells) - tri.area) < 1e-6


def test_multipolygon_cells_keep_one_id():
    # a boundary with a notch so that one cell is split into two parts
    notched = box(0, 0, 100, 100).difference(box(40, -1, 60, 60))
    cells, ids = grid.square_grid(notched, 100.0, min_area_share=0.01)
    assert len(cells) == 1
    assert cells[0].geom_type in ("Polygon", "MultiPolygon")
    assert ids == [0]


def test_zero_threshold_never_returns_non_polygons():
    tri = Polygon([(0, 0), (300, 0), (0, 300)])
    cells, _ = grid.square_grid(tri, 100.0, min_area_share=0.0)
    assert cells
    for c in cells:
        assert c.geom_type in ("Polygon", "MultiPolygon")
        assert c.area > 0
