#include "zonal.hpp"
#include "dispatch.hpp"
#include "validate.hpp"

#include <atomic>
#include <limits>
#include <new>
#include <utility>

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
    int rc = validate_shape(s);
    if (rc != FZ_OK) return rc;
    rc = validate_zones(n_zones);
    if (rc != FZ_OK) return rc;
    if (opt != nullptr) {
        rc = validate_options(opt);
        if (rc != FZ_OK) return rc;
        if (opt->backend == FZ_BACKEND_CUDA) return FZ_ERR_BACKEND;
    }
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
    for (int32_t z = 0; z < n_zones; ++z) stats_merge_one(into[z], from[z]);
}

void stats_finalize(fz_stats* acc, int32_t n_zones) {
    for (int32_t z = 0; z < n_zones; ++z) {
        if (acc[z].count == 0) {
            acc[z].min = 0.0;
            acc[z].max = 0.0;
        }
    }
}

namespace {

// Sparse partial result of one band: the zones it touched and their statistics.
struct BandPartial {
    std::vector<int32_t> zones;
    std::vector<fz_stats> stats;
};

// Runs band_fn(row0, row1, scratch) -> bool over every band and merges the per-band partials
// into out in ascending band order. The partial of a band never depends on which thread ran
// it, so the result is independent of the thread count. A band that reports an invalid mask
// value stops the run and nothing is written to out.
template <class BandFn>
int reduce_stats(const Plan& plan, fz_shape s, int32_t n_zones, fz_stats* out, BandFn&& band_fn) {
    std::vector<BandPartial> partials(static_cast<size_t>(plan.n_bands));
    std::atomic<bool> bad{false};
    for_each_band_with_scratch<StatsScratch>(
        plan, s,
        [&]() {
            StatsScratch sc;
            sc.init(n_zones);
            return sc;
        },
        [&](StatsScratch& sc, int32_t band, int32_t row0, int32_t row1) {
            if (bad.load(std::memory_order_relaxed)) return;
            const bool ok = band_fn(row0, row1, sc);
            if (!ok) bad.store(true, std::memory_order_relaxed);
            BandPartial& p = partials[static_cast<size_t>(band)];
            if (ok) {
                p.zones = sc.touched;
                p.stats.resize(sc.touched.size());
                for (size_t i = 0; i < sc.touched.size(); ++i) p.stats[i] = sc.get(sc.touched[i]);
            }
            for (int32_t z : sc.touched) sc.reset(z);
            sc.touched.clear();
        });
    if (bad.load()) return FZ_ERR_ARG;
    stats_init(out, n_zones);
    for (const BandPartial& p : partials) {
        for (size_t i = 0; i < p.zones.size(); ++i) stats_merge_one(out[p.zones[i]], p.stats[i]);
    }
    stats_finalize(out, n_zones);
    return FZ_OK;
}

template <class T>
int zonal_impl(const T* raster, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata, fz_stats* out,
               const fz_options* opt) {
    fz_options defaults;
    fz_options_default(&defaults);
    if (opt == nullptr) opt = &defaults;
    int rc = validate_zonal_args(raster, mask, s, n_zones, out, opt);
    if (rc != FZ_OK) return rc;
    Plan plan;
    rc = resolve_plan(opt, s, plan);
    if (rc != FZ_OK) return rc;
    try {
        return reduce_stats(plan, s, n_zones, out, [&](int32_t row0, int32_t row1, StatsScratch& sc) {
            return zonal_band(raster, mask, s, row0, row1, nodata, n_zones, sc);
        });
    } catch (const std::bad_alloc&) {
        return FZ_ERR_NOMEM;
    } catch (...) {
        return FZ_ERR_INTERNAL;
    }
}

template <class T>
int ratio_impl(const T* num, const T* den, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata_num,
               T nodata_den, double eps, fz_stats* out, const fz_options* opt) {
    fz_options defaults;
    fz_options_default(&defaults);
    if (opt == nullptr) opt = &defaults;
    int rc = validate_zonal_args(num, mask, s, n_zones, out, opt);
    if (rc != FZ_OK) return rc;
    if (den == nullptr || !(eps > 0.0) || eps > std::numeric_limits<double>::max()) return FZ_ERR_ARG;
    Plan plan;
    rc = resolve_plan(opt, s, plan);
    if (rc != FZ_OK) return rc;
    try {
        return reduce_stats(plan, s, n_zones, out, [&](int32_t row0, int32_t row1, StatsScratch& sc) {
            return ratio_band(num, den, mask, s, row0, row1, nodata_num, nodata_den, eps, n_zones, sc);
        });
    } catch (const std::bad_alloc&) {
        return FZ_ERR_NOMEM;
    } catch (...) {
        return FZ_ERR_INTERNAL;
    }
}

template <class T>
int mean_fallback_impl(const T* raster, fz_shape s, T nodata, const fz_stats* stats, int32_t n_zones,
                       const int32_t* centroid_row, const int32_t* centroid_col, double* mean_out) {
    if (raster == nullptr || stats == nullptr || mean_out == nullptr) return FZ_ERR_ARG;
    int rc = validate_shape(s);
    if (rc != FZ_OK) return rc;
    rc = validate_zones(n_zones);
    if (rc != FZ_OK) return rc;
    if ((centroid_row == nullptr) != (centroid_col == nullptr)) return FZ_ERR_ARG;
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

// Per-band majority partials are merged by adding integer counts, which commutes, so the
// merge order does not matter and thread-private state can be flushed per band.
struct MajorityPartial {
    std::vector<int32_t> zones;
    std::vector<ValueCounts> maps;
};

template <class T>
int majority_impl(const T* raster, const int32_t* mask, fz_shape s, int32_t n_zones, T nodata,
                  const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                  uint8_t* has_value_out, const fz_options* opt) {
    fz_options defaults;
    fz_options_default(&defaults);
    if (opt == nullptr) opt = &defaults;
    int rc = validate_zonal_args(raster, mask, s, n_zones, value_out, opt);
    if (rc != FZ_OK) return rc;
    if (has_value_out == nullptr) return FZ_ERR_ARG;
    if ((centroid_row == nullptr) != (centroid_col == nullptr)) return FZ_ERR_ARG;
    Plan plan;
    rc = resolve_plan(opt, s, plan);
    if (rc != FZ_OK) return rc;
    try {
        std::vector<MajorityPartial> partials(static_cast<size_t>(plan.n_bands));
        std::atomic<bool> bad{false};
        for_each_band_with_scratch<MajorityScratch>(
            plan, s,
            [&]() {
                MajorityScratch sc;
                sc.slot.assign(static_cast<size_t>(n_zones), -1);
                return sc;
            },
            [&](MajorityScratch& sc, int32_t band, int32_t row0, int32_t row1) {
                if (bad.load(std::memory_order_relaxed)) return;
                const bool ok = majority_band(raster, mask, s, row0, row1, nodata, n_zones, sc);
                if (!ok) bad.store(true, std::memory_order_relaxed);
                MajorityPartial& p = partials[static_cast<size_t>(band)];
                for (int32_t z : sc.zones) sc.slot[static_cast<size_t>(z)] = -1;
                if (ok) {
                    p.zones = std::move(sc.zones);
                    p.maps = std::move(sc.maps);
                }
                sc.zones.clear();
                sc.maps.clear();
            });
        if (bad.load()) return FZ_ERR_ARG;
        ZoneCounts counts(static_cast<size_t>(n_zones));
        for (MajorityPartial& p : partials) {
            for (size_t i = 0; i < p.zones.size(); ++i) {
                ValueCounts& into = counts[static_cast<size_t>(p.zones[i])];
                if (into.empty()) {
                    into = std::move(p.maps[i]);
                } else {
                    p.maps[i].merge_into(into);
                }
            }
        }
        majority_finalize(raster, s, nodata, counts, n_zones, centroid_row, centroid_col, value_out, has_value_out);
        return FZ_OK;
    } catch (const std::bad_alloc&) {
        return FZ_ERR_NOMEM;
    } catch (...) {
        return FZ_ERR_INTERNAL;
    }
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

int fz_zonal_i32(const int32_t* raster, const int32_t* mask, fz_shape s, int32_t n_zones, int32_t nodata,
                 fz_stats* out, const fz_options* opt) {
    return fz::zonal_impl(raster, mask, s, n_zones, nodata, out, opt);
}

int fz_zonal_u8(const uint8_t* raster, const int32_t* mask, fz_shape s, int32_t n_zones, uint8_t nodata,
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

int fz_zonal_mean_fallback_i32(const int32_t* raster, fz_shape s, int32_t nodata, const fz_stats* stats,
                               int32_t n_zones, const int32_t* centroid_row, const int32_t* centroid_col,
                               double* mean_out) {
    return fz::mean_fallback_impl(raster, s, nodata, stats, n_zones, centroid_row, centroid_col, mean_out);
}

int fz_zonal_mean_fallback_u8(const uint8_t* raster, fz_shape s, uint8_t nodata, const fz_stats* stats,
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

int fz_zonal_majority_u8(const uint8_t* raster, const int32_t* mask, fz_shape s, int32_t n_zones, uint8_t nodata,
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
