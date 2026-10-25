#include "zonal.hpp"

#include <limits>

namespace fz {

int validate_mask(const int32_t* mask, size_t n, int32_t n_zones) {
    for (size_t i = 0; i < n; ++i) {
        const int32_t z = mask[i];
        if (z < -1 || z >= n_zones) return FZ_ERR_ARG;
    }
    return FZ_OK;
}

int validate_zonal_args(const void* raster, const int32_t* mask, fz_shape s, int32_t n_zones, const void* out,
                        const fz_options* opt) {
    if (raster == nullptr || mask == nullptr || out == nullptr) return FZ_ERR_ARG;
    if (s.width <= 0 || s.height <= 0 || n_zones <= 0) return FZ_ERR_ARG;
    if (opt != nullptr && opt->backend == FZ_BACKEND_CUDA) return FZ_ERR_BACKEND;
    return FZ_OK;
}

void stats_init(fz_stats* acc, int32_t n_zones) {
    for (int32_t z = 0; z < n_zones; ++z) {
        acc[z].count = 0;
        acc[z].sum = 0.0;
        acc[z].sumsq = 0.0;
        acc[z].min = std::numeric_limits<double>::infinity();
        acc[z].max = -std::numeric_limits<double>::infinity();
    }
}

void stats_merge(fz_stats* into, const fz_stats* from, int32_t n_zones) {
    for (int32_t z = 0; z < n_zones; ++z) {
        into[z].count += from[z].count;
        into[z].sum += from[z].sum;
        into[z].sumsq += from[z].sumsq;
        if (from[z].min < into[z].min) into[z].min = from[z].min;
        if (from[z].max > into[z].max) into[z].max = from[z].max;
    }
}

void stats_finalize(fz_stats* acc, int32_t n_zones) {
    for (int32_t z = 0; z < n_zones; ++z) {
        if (acc[z].count == 0) {
            acc[z].min = 0.0;
            acc[z].max = 0.0;
        }
    }
}

void majority_merge(ZoneCounts& into, const ZoneCounts& from) {
    for (size_t z = 0; z < into.size(); ++z) {
        for (const auto& kv : from[z]) into[z][kv.first] += kv.second;
    }
}

namespace {

template <class T>
int zonal_impl(const T* raster, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata, fz_stats* out,
               const fz_options* opt) {
    int rc = validate_zonal_args(raster, mask, s, n_zones, out, opt);
    if (rc != FZ_OK) return rc;
    const size_t n = static_cast<size_t>(s.width) * static_cast<size_t>(s.height);
    rc = validate_mask(mask, n, n_zones);
    if (rc != FZ_OK) return rc;
    stats_init(out, n_zones);
    zonal_band(raster, mask, s, 0, s.height, nodata, out);
    stats_finalize(out, n_zones);
    return FZ_OK;
}

template <class T>
int mean_fallback_impl(const T* raster, fz_shape s, T nodata, const fz_stats* stats, int32_t n_zones,
                       const int32_t* centroid_row, const int32_t* centroid_col, double* mean_out) {
    if (raster == nullptr || stats == nullptr || mean_out == nullptr) return FZ_ERR_ARG;
    if (s.width <= 0 || s.height <= 0 || n_zones <= 0) return FZ_ERR_ARG;
    for (int32_t z = 0; z < n_zones; ++z) {
        if (stats[z].count > 0) {
            mean_out[z] = stats[z].sum / static_cast<double>(stats[z].count);
            continue;
        }
        mean_out[z] = 0.0;
        if (centroid_row == nullptr || centroid_col == nullptr) continue;
        const int32_t r = centroid_row[z], c = centroid_col[z];
        if (r < 0 || r >= s.height || c < 0 || c >= s.width) continue;
        const T v = raster[static_cast<size_t>(r) * s.width + c];
        if (is_nodata(v, nodata)) continue;
        mean_out[z] = static_cast<double>(v);
    }
    return FZ_OK;
}

template <class T>
int majority_impl(const T* raster, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata,
                  const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                  uint8_t* has_value_out, const fz_options* opt) {
    int rc = validate_zonal_args(raster, mask, s, n_zones, value_out, opt);
    if (rc != FZ_OK) return rc;
    if (has_value_out == nullptr) return FZ_ERR_ARG;
    const size_t n = static_cast<size_t>(s.width) * static_cast<size_t>(s.height);
    rc = validate_mask(mask, n, n_zones);
    if (rc != FZ_OK) return rc;
    ZoneCounts counts(static_cast<size_t>(n_zones));
    majority_band(raster, mask, s, 0, s.height, nodata, counts);
    majority_finalize(raster, s, nodata, counts, n_zones, centroid_row, centroid_col, value_out, has_value_out);
    return FZ_OK;
}

template <class T>
int ratio_impl(const T* num, const T* den, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata_num,
               T nodata_den, double eps, fz_stats* out, const fz_options* opt) {
    int rc = validate_zonal_args(num, mask, s, n_zones, out, opt);
    if (rc != FZ_OK) return rc;
    if (den == nullptr || !(eps >= 0.0)) return FZ_ERR_ARG;
    const size_t n = static_cast<size_t>(s.width) * static_cast<size_t>(s.height);
    rc = validate_mask(mask, n, n_zones);
    if (rc != FZ_OK) return rc;
    stats_init(out, n_zones);
    ratio_band(num, den, mask, s, 0, s.height, nodata_num, nodata_den, eps, out);
    stats_finalize(out, n_zones);
    return FZ_OK;
}

}  // namespace
}  // namespace fz

extern "C" {

int fz_zonal_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones, float nodata, fz_stats* out,
                 const fz_options* opt) {
    return fz::zonal_impl(raster, mask, s, n_zones, nodata, out, opt);
}

int fz_zonal_f64(const double* raster, const int32_t* mask, fz_shape s, int32_t n_zones, double nodata,
                 fz_stats* out, const fz_options* opt) {
    return fz::zonal_impl(raster, mask, s, n_zones, nodata, out, opt);
}

int fz_zonal_mean_fallback_f32(const float* raster, fz_shape s, float nodata, const fz_stats* stats, int32_t n_zones,
                               const int32_t* centroid_row, const int32_t* centroid_col, double* mean_out) {
    return fz::mean_fallback_impl(raster, s, nodata, stats, n_zones, centroid_row, centroid_col, mean_out);
}

int fz_zonal_mean_fallback_f64(const double* raster, fz_shape s, double nodata, const fz_stats* stats,
                               int32_t n_zones, const int32_t* centroid_row, const int32_t* centroid_col,
                               double* mean_out) {
    return fz::mean_fallback_impl(raster, s, nodata, stats, n_zones, centroid_row, centroid_col, mean_out);
}

int fz_zonal_majority_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones, float nodata,
                          const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                          uint8_t* has_value_out, const fz_options* opt) {
    return fz::majority_impl(raster, mask, s, n_zones, nodata, centroid_row, centroid_col, value_out, has_value_out,
                             opt);
}

int fz_zonal_majority_f64(const double* raster, const int32_t* mask, fz_shape s, int32_t n_zones, double nodata,
                          const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                          uint8_t* has_value_out, const fz_options* opt) {
    return fz::majority_impl(raster, mask, s, n_zones, nodata, centroid_row, centroid_col, value_out, has_value_out,
                             opt);
}

int fz_zonal_majority_i32(const int32_t* raster, const int32_t* mask, fz_shape s, int32_t n_zones, int32_t nodata,
                          const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                          uint8_t* has_value_out, const fz_options* opt) {
    return fz::majority_impl(raster, mask, s, n_zones, nodata, centroid_row, centroid_col, value_out, has_value_out,
                             opt);
}

int fz_zonal_ratio_f32(const float* num, const float* den, const int32_t* mask, fz_shape s, int32_t n_zones,
                       float nodata_num, float nodata_den, double eps, fz_stats* out, const fz_options* opt) {
    return fz::ratio_impl(num, den, mask, s, n_zones, nodata_num, nodata_den, eps, out, opt);
}

int fz_zonal_ratio_f64(const double* num, const double* den, const int32_t* mask, fz_shape s, int32_t n_zones,
                       double nodata_num, double nodata_den, double eps, fz_stats* out, const fz_options* opt) {
    return fz::ratio_impl(num, den, mask, s, n_zones, nodata_num, nodata_den, eps, out, opt);
}

}  // extern "C"
