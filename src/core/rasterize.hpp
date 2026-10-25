// Polygon rasterisation shared by the serial and OpenMP backends.
//
// A PolygonSet groups the flat ring arrays of the C ABI into polygons (one exterior ring plus its
// holes) with precomputed pixel row ranges. Everything is stored in flat arrays: with hundreds of
// thousands of small planning units, one heap allocation per polygon would cost more than the
// rasterisation itself. rasterize_band fills the rows [row0, row1) of the mask; bands are
// independent, so a backend may run them on any thread in any order and the result is identical
// to the serial sweep.
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
    int32_t ring_begin, ring_end;       // range in PolygonSet::rings; rings[ring_begin] is the exterior ring
    double min_x, min_y, max_x, max_y;  // bounding box of the exterior ring
    int32_t row_begin, row_end;         // pixel rows [row_begin, row_end) touched, already clamped
};

struct PolygonSet {
    const double* xy = nullptr;
    std::vector<Ring> rings;        // exterior ring of a polygon followed by its holes
    std::vector<Polygon> polygons;
};

// Polygons grouped by the bands their row ranges touch: polygon indices of band b are
// index[offset[b], offset[b + 1]), ascending within a band so later polygons still win.
struct BandBuckets {
    std::vector<int64_t> offset;  // n_bands + 1
    std::vector<int32_t> index;
};

// Validates the flat arrays and builds the polygon set. The per-ring work (coordinate checks,
// bounding boxes, row ranges) is independent per ring and runs on `threads` OpenMP threads when
// built with OpenMP; the assembly pass that depends on ring order stays sequential. No
// floating-point summation is involved, so the result is identical for any thread count.
// Returns an fz error code.
int build_polygon_set(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                      const int32_t* ring_zone, int64_t n_rings, const fz_transform& t, const fz_shape& s,
                      int mode, int threads, PolygonSet& out);

// Fills rows [row0, row1) of mask (row-major, s.width columns) with -1 and then with the polygons.
void rasterize_band(const PolygonSet& set, const fz_transform& t, const fz_shape& s, int32_t row0,
                    int32_t row1, int mode, int32_t* mask);

// Same, visiting only the polygons of one band of the buckets.
void rasterize_band(const PolygonSet& set, const BandBuckets& buckets, int32_t band, const fz_transform& t,
                    const fz_shape& s, int32_t row0, int32_t row1, int mode, int32_t* mask);

// Buckets built once per call with two counting passes (no per-band allocations).
BandBuckets bucket_by_band(const PolygonSet& set, int32_t band_rows, int32_t n_bands);

}  // namespace fz
