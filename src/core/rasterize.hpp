// Polygon rasterisation shared by the serial and OpenMP backends.
//
// A PolygonSet groups the flat ring arrays of the C ABI into polygons (one exterior ring plus its
// holes) with precomputed pixel bounding boxes. rasterize_band fills the rows [row0, row1) of the
// mask; bands are independent, so a backend may run them on any thread in any order and the result
// is identical to the serial sweep.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstdint>
#include <vector>

namespace fz {

struct Ring {
    int64_t begin;  // first point index
    int64_t end;    // one past the last point index
};

struct Polygon {
    int32_t zone;
    std::vector<Ring> rings;  // rings[0] is the exterior ring, the rest are holes
    double min_x, min_y, max_x, max_y;  // bounding box of the exterior ring
    int32_t row_begin, row_end;         // pixel rows [row_begin, row_end) touched, already clamped
};

struct PolygonSet {
    const double* xy = nullptr;
    std::vector<Polygon> polygons;
};

// Validates the flat arrays and builds the polygon set. Returns an fz error code.
int build_polygon_set(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                      const int32_t* ring_zone, int64_t n_rings, const fz_transform& t, const fz_shape& s,
                      int mode, PolygonSet& out);

// Fills rows [row0, row1) of mask (row-major, s.width columns) with -1 and then with the polygons.
void rasterize_band(const PolygonSet& set, const fz_transform& t, const fz_shape& s, int32_t row0,
                    int32_t row1, int mode, int32_t* mask);

// Same, visiting only the polygons listed in `indices` (ascending polygon order, so later
// polygons still win); the lists come from bucket_by_band.
void rasterize_band(const PolygonSet& set, const std::vector<int32_t>& indices, const fz_transform& t,
                    const fz_shape& s, int32_t row0, int32_t row1, int mode, int32_t* mask);

// For every band of band_rows rows, the ascending indices of the polygons whose row range touches
// it. Built once per call so a band costs its own polygons, not all of them.
std::vector<std::vector<int32_t>> bucket_by_band(const PolygonSet& set, const fz_shape& s, int32_t band_rows,
                                                 int32_t n_bands);

}  // namespace fz
