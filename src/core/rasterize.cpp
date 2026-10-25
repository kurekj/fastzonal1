#include "rasterize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fz {
namespace {

// Truncating world-to-pixel conversion of the reference implementation ((int) cast in C#), kept
// in double so that far-away coordinates cannot overflow before clamping.
int64_t trunc_to_int(double v) {
    constexpr double kLimit = 1e15;
    if (!(v > -kLimit)) return static_cast<int64_t>(-kLimit);
    if (!(v < kLimit)) return static_cast<int64_t>(kLimit);
    return static_cast<int64_t>(v);  // truncation toward zero
}

// Collects the x coordinates where the ring's edges cross the horizontal line y = wy, using the
// half-open rule of the reference ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy)).
void collect_crossings(const double* xy, const Ring& ring, double wy, std::vector<double>& out) {
    const int64_t n = ring.end - ring.begin;
    if (n < 3) return;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t j = (i + 1 == n) ? 0 : i + 1;  // closing edge handled by wrapping
        const double x1 = xy[2 * (ring.begin + i)], y1 = xy[2 * (ring.begin + i) + 1];
        const double x2 = xy[2 * (ring.begin + j)], y2 = xy[2 * (ring.begin + j) + 1];
        if ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy)) {
            out.push_back(x1 + (wy - y1) / (y2 - y1) * (x2 - x1));
        }
    }
}

void fill_center(const PolygonSet& set, const Polygon& p, const fz_transform& t, const fz_shape& s,
                 int32_t row0, int32_t row1, int32_t* mask, std::vector<double>& crossings) {
    const int32_t rb = std::max(p.row_begin, row0);
    const int32_t re = std::min(p.row_end, row1);
    for (int32_t row = rb; row < re; ++row) {
        const double wy = t.origin_y - (row + 0.5) * t.pixel_h;
        crossings.clear();
        for (const Ring& ring : p.rings) collect_crossings(set.xy, ring, wy, crossings);
        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());
        int32_t* row_ptr = mask + static_cast<size_t>(row) * s.width;
        for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
            // columns whose centre x satisfies xa <= x < xb
            const double a = (crossings[k] - t.origin_x) / t.pixel_w - 0.5;
            const double b = (crossings[k + 1] - t.origin_x) / t.pixel_w - 0.5;
            int64_t c0 = static_cast<int64_t>(std::ceil(std::max(a, -1.0)));
            int64_t c1 = static_cast<int64_t>(std::ceil(std::min(b, static_cast<double>(s.width) + 1.0))) - 1;
            c0 = std::max<int64_t>(c0, 0);
            c1 = std::min<int64_t>(c1, s.width - 1);
            for (int64_t col = c0; col <= c1; ++col) row_ptr[col] = p.zone;
        }
    }
}

void fill_compat(const PolygonSet& set, const Polygon& p, const fz_transform& t, const fz_shape& s,
                 int32_t row0, int32_t row1, int32_t* mask, std::vector<double>& crossings) {
    const int64_t start_col = std::max<int64_t>(0, trunc_to_int((p.min_x - t.origin_x) / t.pixel_w));
    const int64_t end_col = std::min<int64_t>(s.width - 1, trunc_to_int((p.max_x - t.origin_x) / t.pixel_w));
    const int32_t rb = std::max(p.row_begin, row0);
    const int32_t re = std::min(p.row_end, row1);
    for (int32_t row = rb; row < re; ++row) {
        const double wy = t.origin_y - (row + 0.5) * t.pixel_h;
        crossings.clear();
        collect_crossings(set.xy, p.rings[0], wy, crossings);  // exterior ring only
        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());
        int32_t* row_ptr = mask + static_cast<size_t>(row) * s.width;
        for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
            const int64_t c0 = std::max(start_col, trunc_to_int((crossings[k] - t.origin_x) / t.pixel_w));
            const int64_t c1 = std::min(end_col, trunc_to_int((crossings[k + 1] - t.origin_x) / t.pixel_w));
            for (int64_t col = c0; col <= c1; ++col) row_ptr[col] = p.zone;
        }
    }
}

}  // namespace

int build_polygon_set(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                      const int32_t* ring_zone, int64_t n_rings, const fz_transform& t, const fz_shape& s,
                      int mode, PolygonSet& out) {
    out.xy = xy;
    out.polygons.clear();
    if (n_rings == 0) return FZ_OK;
    if (xy == nullptr || ring_start == nullptr || ring_is_hole == nullptr || ring_zone == nullptr) return FZ_ERR_ARG;
    if (ring_start[0] < 0) return FZ_ERR_ARG;

    for (int64_t i = 0; i < n_rings; ++i) {
        if (ring_start[i + 1] < ring_start[i]) return FZ_ERR_ARG;
        const Ring ring{ring_start[i], ring_start[i + 1]};
        if (ring_is_hole[i]) {
            if (out.polygons.empty()) return FZ_ERR_ARG;  // a hole needs an exterior ring before it
            if (mode == FZ_RASTER_COMPAT) continue;        // the reference ignores holes
            out.polygons.back().rings.push_back(ring);
            continue;
        }
        if (ring_zone[i] < 0) return FZ_ERR_ARG;
        Polygon p;
        p.zone = ring_zone[i];
        p.rings.push_back(ring);
        p.min_x = p.min_y = std::numeric_limits<double>::infinity();
        p.max_x = p.max_y = -std::numeric_limits<double>::infinity();
        for (int64_t k = ring.begin; k < ring.end; ++k) {
            const double x = xy[2 * k], y = xy[2 * k + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) return FZ_ERR_ARG;
            p.min_x = std::min(p.min_x, x);
            p.max_x = std::max(p.max_x, x);
            p.min_y = std::min(p.min_y, y);
            p.max_y = std::max(p.max_y, y);
        }
        if (ring.end - ring.begin < 3) {
            p.row_begin = p.row_end = 0;  // degenerate ring: touches nothing
        } else if (mode == FZ_RASTER_COMPAT) {
            const int64_t r0 = std::max<int64_t>(0, trunc_to_int((t.origin_y - p.max_y) / t.pixel_h));
            const int64_t r1 = std::min<int64_t>(s.height - 1, trunc_to_int((t.origin_y - p.min_y) / t.pixel_h));
            p.row_begin = static_cast<int32_t>(std::min<int64_t>(r0, s.height));
            p.row_end = static_cast<int32_t>(std::max<int64_t>(r1 + 1, p.row_begin));
        } else {
            // rows whose centre y lies in [min_y, max_y]
            const double a = (t.origin_y - p.max_y) / t.pixel_h - 0.5;
            const double b = (t.origin_y - p.min_y) / t.pixel_h - 0.5;
            const int64_t r0 = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(std::max(a, -1.0))));
            const int64_t r1 = std::min<int64_t>(s.height - 1,
                static_cast<int64_t>(std::floor(std::min(b, static_cast<double>(s.height) + 1.0))));
            p.row_begin = static_cast<int32_t>(std::min<int64_t>(r0, s.height));
            p.row_end = static_cast<int32_t>(std::max<int64_t>(r1 + 1, p.row_begin));
        }
        out.polygons.push_back(std::move(p));
    }
    return FZ_OK;
}

void rasterize_band(const PolygonSet& set, const fz_transform& t, const fz_shape& s, int32_t row0, int32_t row1,
                    int mode, int32_t* mask) {
    std::fill(mask + static_cast<size_t>(row0) * s.width, mask + static_cast<size_t>(row1) * s.width, -1);
    std::vector<double> crossings;
    crossings.reserve(64);
    for (const Polygon& p : set.polygons) {
        if (p.row_end <= row0 || p.row_begin >= row1) continue;
        if (mode == FZ_RASTER_COMPAT)
            fill_compat(set, p, t, s, row0, row1, mask, crossings);
        else
            fill_center(set, p, t, s, row0, row1, mask, crossings);
    }
}

}  // namespace fz

extern "C" {

int fz_rasterize(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole, const int32_t* ring_zone,
                 int64_t n_rings, fz_transform t, fz_shape s, int32_t* mask_out, const fz_options* opt) {
    fz_options defaults;
    fz_options_default(&defaults);
    if (opt == nullptr) opt = &defaults;
    if (n_rings < 0 || mask_out == nullptr) return FZ_ERR_ARG;
    if (s.width <= 0 || s.height <= 0) return FZ_ERR_ARG;
    if (!(t.pixel_w > 0.0) || !(t.pixel_h > 0.0) || !std::isfinite(t.origin_x) || !std::isfinite(t.origin_y))
        return FZ_ERR_ARG;
    if (opt->raster_mode != FZ_RASTER_CENTER && opt->raster_mode != FZ_RASTER_COMPAT) return FZ_ERR_ARG;
    if (opt->backend == FZ_BACKEND_CUDA) return FZ_ERR_BACKEND;

    fz::PolygonSet set;
    const int rc = fz::build_polygon_set(xy, ring_start, ring_is_hole, ring_zone, n_rings, t, s, opt->raster_mode, set);
    if (rc != FZ_OK) return rc;
    fz::rasterize_band(set, t, s, 0, s.height, opt->raster_mode, mask_out);
    return FZ_OK;
}

}  // extern "C"
