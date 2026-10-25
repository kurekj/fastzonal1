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

inline void stats_reset(fz_stats& a) {
    a.count = 0;
    a.sum = 0.0;
    a.sumsq = 0.0;
    a.min = std::numeric_limits<double>::infinity();
    a.max = -std::numeric_limits<double>::infinity();
}

void stats_init(fz_stats* acc, int32_t n_zones) {
    for (int32_t z = 0; z < n_zones; ++z) stats_reset(acc[z]);
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

// Mask validation over all bands; any invalid value fails the whole call.
int validate_mask_planned(const Plan& plan, const int32_t* mask, fz_shape s, int32_t n_zones) {
    std::atomic<int> bad{0};
    for_each_band(plan, s, [&](int32_t, int32_t row0, int32_t row1) {
        if (bad.load(std::memory_order_relaxed)) return;
        const size_t begin = static_cast<size_t>(row0) * s.width;
        const size_t n = static_cast<size_t>(row1 - row0) * s.width;
        if (validate_mask(mask + begin, n, n_zones) != FZ_OK) bad.store(1, std::memory_order_relaxed);
    });
    return bad.load() ? FZ_ERR_ARG : FZ_OK;
}

// Sparse partial result of one band: the zones it touched and their statistics.
struct BandPartial {
    std::vector<int32_t> zones;
    std::vector<fz_stats> stats;
};

struct StatsScratch {
    std::vector<fz_stats> dense;
    std::vector<int32_t> touched;
};

// Runs band_fn(row0, row1, dense, touched) over every band and merges the per-band partials
// into out in ascending band order. The partial of a band never depends on which thread ran
// it, so the result is independent of the thread count.
template <class BandFn>
void reduce_stats(const Plan& plan, fz_shape s, int32_t n_zones, fz_stats* out, BandFn&& band_fn) {
    std::vector<BandPartial> partials(static_cast<size_t>(plan.n_bands));
    for_each_band_with_scratch<StatsScratch>(
        plan, s,
        [&]() {
            StatsScratch sc;
            sc.dense.resize(static_cast<size_t>(n_zones));
            stats_init(sc.dense.data(), n_zones);
            return sc;
        },
        [&](StatsScratch& sc, int32_t band, int32_t row0, int32_t row1) {
            band_fn(row0, row1, sc.dense.data(), &sc.touched);
            BandPartial& p = partials[static_cast<size_t>(band)];
            p.zones = sc.touched;
            p.stats.resize(sc.touched.size());
            for (size_t i = 0; i < sc.touched.size(); ++i) {
                fz_stats& d = sc.dense[static_cast<size_t>(sc.touched[i])];
                p.stats[i] = d;
                stats_reset(d);
            }
            sc.touched.clear();
        });
    stats_init(out, n_zones);
    for (const BandPartial& p : partials) {
        for (size_t i = 0; i < p.zones.size(); ++i) stats_merge_one(out[p.zones[i]], p.stats[i]);
    }
    stats_finalize(out, n_zones);
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
        rc = validate_mask_planned(plan, mask, s, n_zones);
        if (rc != FZ_OK) return rc;
        reduce_stats(plan, s, n_zones, out, [&](int32_t row0, int32_t row1, fz_stats* dense, std::vector<int32_t>* touched) {
            zonal_band(raster, mask, s, row0, row1, nodata, dense, touched);
        });
        return FZ_OK;
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
        rc = validate_mask_planned(plan, mask, s, n_zones);
        if (rc != FZ_OK) return rc;
        reduce_stats(plan, s, n_zones, out, [&](int32_t row0, int32_t row1, fz_stats* dense, std::vector<int32_t>* touched) {
            ratio_band(num, den, mask, s, row0, row1, nodata_num, nodata_den, eps, dense, touched);
        });
        return FZ_OK;
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
        rc = validate_mask_planned(plan, mask, s, n_zones);
        if (rc != FZ_OK) return rc;
        std::vector<MajorityPartial> partials(static_cast<size_t>(plan.n_bands));
        for_each_band_with_scratch<MajorityScratch>(
            plan, s,
            [&]() {
                MajorityScratch sc;
                sc.slot.assign(static_cast<size_t>(n_zones), -1);
                return sc;
            },
            [&](MajorityScratch& sc, int32_t band, int32_t row0, int32_t row1) {
                majority_band(raster, mask, s, row0, row1, nodata, sc);
                MajorityPartial& p = partials[static_cast<size_t>(band)];
                p.zones = std::move(sc.zones);
                p.maps = std::move(sc.maps);
                for (int32_t z : p.zones) sc.slot[static_cast<size_t>(z)] = -1;
                sc.zones.clear();
                sc.maps.clear();
            });
        ZoneCounts counts(static_cast<size_t>(n_zones));
        for (MajorityPartial& p : partials) {
            for (size_t i = 0; i < p.zones.size(); ++i) {
                ValueCounts& into = counts[static_cast<size_t>(p.zones[i])];
                if (into.empty()) {
                    into = std::move(p.maps[i]);
                } else {
                    for (const auto& kv : p.maps[i]) into[kv.first] += kv.second;
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
