// Zonal reductions shared by the backends. Every routine works on a band of rows [row0, row1)
// and accumulates into caller-provided per-zone state, so a backend can run bands on any
// thread and merge the partial results in a fixed order.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fz {

template <class T>
inline bool is_nodata(T v, T nodata) {
    return v == nodata || v != v;  // v != v catches NaN for floating types
}

// FZ_OK when every mask value lies in [-1, n_zones), else FZ_ERR_ARG.
int validate_mask(const int32_t* mask, size_t n, int32_t n_zones);

// Validation shared by the zonal entry points (null pointers, shape, n_zones, backend).
int validate_zonal_args(const void* raster, const int32_t* mask, fz_shape s, int32_t n_zones, const void* out,
                        const fz_options* opt);

void stats_init(fz_stats* acc, int32_t n_zones);
// into[z] += from[z] for every zone, in zone order.
void stats_merge(fz_stats* into, const fz_stats* from, int32_t n_zones);
// Zones with count 0 get min = max = 0 so no infinity leaves the library.
void stats_finalize(fz_stats* acc, int32_t n_zones);

template <class T>
void zonal_band(const T* raster, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1, T nodata,
                fz_stats* acc) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z < 0) continue;
        const T v = raster[i];
        if (is_nodata(v, nodata)) continue;
        const double d = static_cast<double>(v);
        fz_stats& a = acc[z];
        a.count += 1;
        a.sum += d;
        a.sumsq += d * d;
        if (d < a.min) a.min = d;
        if (d > a.max) a.max = d;
    }
}

template <class T>
void ratio_band(const T* num, const T* den, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1,
                T nodata_num, T nodata_den, double eps, fz_stats* acc) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z < 0) continue;
        const T a = num[i], b = den[i];
        if (is_nodata(a, nodata_num) || is_nodata(b, nodata_den)) continue;
        const double bd = static_cast<double>(b);
        if (bd < eps && bd > -eps) continue;
        const double d = static_cast<double>(a) / bd;
        fz_stats& st = acc[z];
        st.count += 1;
        st.sum += d;
        st.sumsq += d * d;
        if (d < st.min) st.min = d;
        if (d > st.max) st.max = d;
    }
}

// Value -> count per zone. Keys are doubles so one structure serves every raster type.
using ZoneCounts = std::vector<std::unordered_map<double, int64_t>>;

template <class T>
void majority_band(const T* raster, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1, T nodata,
                   ZoneCounts& counts) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z < 0) continue;
        const T v = raster[i];
        if (is_nodata(v, nodata)) continue;
        ++counts[z][static_cast<double>(v)];
    }
}

void majority_merge(ZoneCounts& into, const ZoneCounts& from);

// Picks the mode per zone (ties -> smallest value) with the centroid fallback for empty zones.
template <class T>
void majority_finalize(const T* raster, fz_shape s, T nodata, const ZoneCounts& counts, int32_t n_zones,
                       const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                       uint8_t* has_value_out) {
    for (int32_t z = 0; z < n_zones; ++z) {
        const auto& m = counts[z];
        if (!m.empty()) {
            int64_t best = -1;
            double best_val = 0.0;
            for (const auto& kv : m) {
                if (kv.second > best || (kv.second == best && kv.first < best_val)) {
                    best = kv.second;
                    best_val = kv.first;
                }
            }
            value_out[z] = best_val;
            has_value_out[z] = 1;
            continue;
        }
        value_out[z] = 0.0;
        has_value_out[z] = 0;
        if (centroid_row == nullptr || centroid_col == nullptr) continue;
        const int32_t r = centroid_row[z], c = centroid_col[z];
        if (r < 0 || r >= s.height || c < 0 || c >= s.width) continue;
        const T v = raster[static_cast<size_t>(r) * s.width + c];
        if (is_nodata(v, nodata)) continue;
        value_out[z] = static_cast<double>(v);
        has_value_out[z] = 1;
    }
}

}  // namespace fz
