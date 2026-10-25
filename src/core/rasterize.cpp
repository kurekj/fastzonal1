#include "rasterize.hpp"
#include "dispatch.hpp"
#include "validate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace fz {
namespace {

// Largest magnitude that is converted to an integer; anything beyond is clamped first. Rasters
// are at most INT32_MAX pixels wide, so a clamp of 2^40 keeps every later comparison exact.
constexpr double kClamp = 1099511627776.0;  // 2^40

double clamp_for_int(double v) {
    if (!(v > -kClamp)) return -kClamp;  // also catches NaN
    if (!(v < kClamp)) return kClamp;
    return v;
}

// Truncating world-to-pixel conversion of the reference implementation ((int) cast in C#).
int64_t trunc_to_int(double v) { return static_cast<int64_t>(clamp_for_int(v)); }

int64_t ceil_to_int(double v) { return static_cast<int64_t>(std::ceil(clamp_for_int(v))); }
int64_t floor_to_int(double v) { return static_cast<int64_t>(std::floor(clamp_for_int(v))); }

// Collects the x coordinates where the ring's edges cross the horizontal line y = wy, using the
// half-open rule of the reference ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy)). A crossing
// whose interpolation overflows to a non-finite value is dropped.
void collect_crossings(const double* xy, const Ring& ring, double wy, std::vector<double>& out) {
    const int64_t n = ring.end - ring.begin;
    if (n < 3) return;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t j = (i + 1 == n) ? 0 : i + 1;  // closing edge handled by wrapping
        const double x1 = xy[2 * (ring.begin + i)], y1 = xy[2 * (ring.begin + i) + 1];
        const double x2 = xy[2 * (ring.begin + j)], y2 = xy[2 * (ring.begin + j) + 1];
        if ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy)) {
            const double x = x1 + (wy - y1) / (y2 - y1) * (x2 - x1);
            if (std::isfinite(x)) out.push_back(x);
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
        for (int32_t k = p.ring_begin; k < p.ring_end; ++k)
            collect_crossings(set.xy, set.rings[static_cast<size_t>(k)], wy, crossings);
        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());
        int32_t* row_ptr = mask + static_cast<size_t>(row) * s.width;
        for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
            // columns whose centre x satisfies xa <= x < xb
            const double a = (crossings[k] - t.origin_x) / t.pixel_w - 0.5;
            const double b = (crossings[k + 1] - t.origin_x) / t.pixel_w - 0.5;
            const int64_t c0 = std::max<int64_t>(ceil_to_int(a), 0);
            const int64_t c1 = std::min<int64_t>(ceil_to_int(b) - 1, s.width - 1);
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
        collect_crossings(set.xy, set.rings[static_cast<size_t>(p.ring_begin)], wy, crossings);  // exterior only
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

namespace {

// Per-ring facts computed independently of every other ring.
struct RingInfo {
    double min_x, min_y, max_x, max_y;
    int32_t row_begin, row_end;
    uint8_t finite;
};

RingInfo ring_info(const double* xy, int64_t begin, int64_t end, const fz_transform& t, const fz_shape& s, int mode) {
    RingInfo r;
    r.min_x = r.min_y = std::numeric_limits<double>::infinity();
    r.max_x = r.max_y = -std::numeric_limits<double>::infinity();
    r.finite = 1;
    for (int64_t k = begin; k < end; ++k) {
        const double x = xy[2 * k], y = xy[2 * k + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) r.finite = 0;
        r.min_x = std::min(r.min_x, x);
        r.max_x = std::max(r.max_x, x);
        r.min_y = std::min(r.min_y, y);
        r.max_y = std::max(r.max_y, y);
    }
    if (end - begin < 3 || !r.finite) {
        r.row_begin = r.row_end = 0;  // fewer than three points: touches nothing
    } else if (mode == FZ_RASTER_COMPAT) {
        const int64_t r0 = std::max<int64_t>(0, trunc_to_int((t.origin_y - r.max_y) / t.pixel_h));
        const int64_t r1 = std::min<int64_t>(s.height - 1, trunc_to_int((t.origin_y - r.min_y) / t.pixel_h));
        r.row_begin = static_cast<int32_t>(std::min<int64_t>(r0, s.height));
        r.row_end = static_cast<int32_t>(std::max<int64_t>(r1 + 1, r.row_begin));
    } else {
        // rows whose centre y lies in [min_y, max_y]
        const double a = (t.origin_y - r.max_y) / t.pixel_h - 0.5;
        const double b = (t.origin_y - r.min_y) / t.pixel_h - 0.5;
        const int64_t r0 = std::max<int64_t>(0, ceil_to_int(a));
        const int64_t r1 = std::min<int64_t>(s.height - 1, floor_to_int(b));
        r.row_begin = static_cast<int32_t>(std::min<int64_t>(r0, s.height));
        r.row_end = static_cast<int32_t>(std::max<int64_t>(r1 + 1, r.row_begin));
    }
    return r;
}

}  // namespace

int build_polygon_set(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                      const int32_t* ring_zone, int64_t n_rings, const fz_transform& t, const fz_shape& s,
                      int mode, int threads, PolygonSet& out) {
    out.xy = xy;
    out.polygons.clear();
    out.rings.clear();
    if (n_rings == 0) return FZ_OK;
    if (xy == nullptr || ring_start == nullptr || ring_is_hole == nullptr || ring_zone == nullptr) return FZ_ERR_ARG;
    if (ring_start[0] < 0) return FZ_ERR_ARG;
    // Offsets are doubled to index xy; keep that arithmetic in range. The offsets are checked
    // sequentially before any coordinate is read.
    constexpr int64_t kMaxOffset = std::numeric_limits<int64_t>::max() / 4;
    for (int64_t i = 0; i < n_rings; ++i)
        if (ring_start[i + 1] < ring_start[i] || ring_start[i + 1] > kMaxOffset) return FZ_ERR_ARG;

    // pass 1 (parallel): per-ring facts, in uninitialised storage (every entry is written)
    std::unique_ptr<RingInfo[]> info(new RingInfo[static_cast<size_t>(n_rings)]);
#ifdef _OPENMP
    if (threads > 1) {
#pragma omp parallel for schedule(static) num_threads(threads)
        for (int64_t i = 0; i < n_rings; ++i)
            info[static_cast<size_t>(i)] = ring_info(xy, ring_start[i], ring_start[i + 1], t, s, mode);
    } else
#else
    (void)threads;
#endif
    {
        for (int64_t i = 0; i < n_rings; ++i)
            info[static_cast<size_t>(i)] = ring_info(xy, ring_start[i], ring_start[i + 1], t, s, mode);
    }
    for (int64_t i = 0; i < n_rings; ++i)
        if (!info[static_cast<size_t>(i)].finite) return FZ_ERR_ARG;

    // pass 2 (sequential): assemble polygons in ring order
    out.rings.reserve(static_cast<size_t>(n_rings));
    out.polygons.reserve(static_cast<size_t>(n_rings));
    for (int64_t i = 0; i < n_rings; ++i) {
        const Ring ring{ring_start[i], ring_start[i + 1]};
        const RingInfo& ri = info[static_cast<size_t>(i)];
        if (ring_is_hole[i]) {
            if (out.polygons.empty()) return FZ_ERR_ARG;          // a hole needs an exterior ring before it
            if (ring_zone[i] != out.polygons.back().zone) return FZ_ERR_ARG;
            if (mode == FZ_RASTER_COMPAT) continue;                // the reference ignores holes
            out.rings.push_back(ring);
            out.polygons.back().ring_end = static_cast<int32_t>(out.rings.size());
            continue;
        }
        if (ring_zone[i] < 0) return FZ_ERR_ARG;
        Polygon p;
        p.zone = ring_zone[i];
        p.ring_begin = static_cast<int32_t>(out.rings.size());
        out.rings.push_back(ring);
        p.ring_end = static_cast<int32_t>(out.rings.size());
        p.min_x = ri.min_x;
        p.min_y = ri.min_y;
        p.max_x = ri.max_x;
        p.max_y = ri.max_y;
        p.row_begin = ri.row_begin;
        p.row_end = ri.row_end;
        out.polygons.push_back(p);
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

void rasterize_band(const PolygonSet& set, const BandBuckets& buckets, int32_t band, const fz_transform& t,
                    const fz_shape& s, int32_t row0, int32_t row1, int mode, int32_t* mask) {
    std::fill(mask + static_cast<size_t>(row0) * s.width, mask + static_cast<size_t>(row1) * s.width, -1);
    std::vector<double> crossings;
    crossings.reserve(64);
    const int64_t from = buckets.offset[static_cast<size_t>(band)];
    const int64_t to = buckets.offset[static_cast<size_t>(band) + 1];
    for (int64_t k = from; k < to; ++k) {
        const Polygon& p = set.polygons[static_cast<size_t>(buckets.index[static_cast<size_t>(k)])];
        if (mode == FZ_RASTER_COMPAT)
            fill_compat(set, p, t, s, row0, row1, mask, crossings);
        else
            fill_center(set, p, t, s, row0, row1, mask, crossings);
    }
}

BandBuckets bucket_by_band(const PolygonSet& set, int32_t band_rows, int32_t n_bands) {
    BandBuckets b;
    b.offset.assign(static_cast<size_t>(n_bands) + 1, 0);
    // pass 1: how many polygons touch each band
    for (const Polygon& p : set.polygons) {
        if (p.row_end <= p.row_begin) continue;
        const int32_t b0 = p.row_begin / band_rows;
        const int32_t b1 = std::min<int32_t>((p.row_end - 1) / band_rows, n_bands - 1);
        for (int32_t band = b0; band <= b1; ++band) ++b.offset[static_cast<size_t>(band) + 1];
    }
    for (size_t band = 0; band < static_cast<size_t>(n_bands); ++band) b.offset[band + 1] += b.offset[band];
    // pass 2: place polygon indices in ascending order within each band
    b.index.resize(static_cast<size_t>(b.offset.back()));
    std::vector<int64_t> cursor(b.offset.begin(), b.offset.end() - 1);
    for (size_t i = 0; i < set.polygons.size(); ++i) {
        const Polygon& p = set.polygons[i];
        if (p.row_end <= p.row_begin) continue;
        const int32_t b0 = p.row_begin / band_rows;
        const int32_t b1 = std::min<int32_t>((p.row_end - 1) / band_rows, n_bands - 1);
        for (int32_t band = b0; band <= b1; ++band)
            b.index[static_cast<size_t>(cursor[static_cast<size_t>(band)]++)] = static_cast<int32_t>(i);
    }
    return b;
}

}  // namespace fz

extern "C" {

int fz_rasterize(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole, const int32_t* ring_zone,
                 int64_t n_rings, fz_transform t, fz_shape s, int32_t* mask_out, const fz_options* opt) {
    fz_options defaults;
    fz_options_default(&defaults);
    if (opt == nullptr) opt = &defaults;
    int rc = fz::validate_options(opt);
    if (rc != FZ_OK) return rc;
    if (n_rings < 0 || mask_out == nullptr) return FZ_ERR_ARG;
    rc = fz::validate_shape(s);
    if (rc != FZ_OK) return rc;
    if (!std::isfinite(t.pixel_w) || !std::isfinite(t.pixel_h) || !(t.pixel_w > 0.0) || !(t.pixel_h > 0.0) ||
        !std::isfinite(t.origin_x) || !std::isfinite(t.origin_y))
        return FZ_ERR_ARG;
    fz::Plan plan;
    rc = fz::resolve_plan(opt, s, plan);
    if (rc != FZ_OK) return rc;

    try {
        fz::PolygonSet set;
        rc = fz::build_polygon_set(xy, ring_start, ring_is_hole, ring_zone, n_rings, t, s, opt->raster_mode,
                                   plan.threads, set);
        if (rc != FZ_OK) return rc;
        const int mode = opt->raster_mode;
        const fz::BandBuckets buckets = fz::bucket_by_band(set, plan.band_rows, plan.n_bands);
        fz::for_each_band(plan, s, [&](int32_t band, int32_t row0, int32_t row1) {
            fz::rasterize_band(set, buckets, band, t, s, row0, row1, mode, mask_out);
        });
        return FZ_OK;
    } catch (const std::bad_alloc&) {
        return FZ_ERR_NOMEM;
    } catch (...) {
        return FZ_ERR_INTERNAL;
    }
}

}  // extern "C"
