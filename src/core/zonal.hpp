// Zonal reductions shared by the backends. Every routine works on a band of rows [row0, row1)
// and accumulates into thread-private scratch, so a backend can run bands on any thread and
// merge the per-band partial results in a fixed order.
//
// The scratch is a structure of arrays (count, sum, sumsq, min, max) with branch-free min/max:
// on a 100k-zone mask this runs about three times faster than updating a 40-byte fz_stats
// record per cell, and close to the speed of a plain sum/count loop.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fz {

template <class T>
inline bool is_nodata(T v, T nodata) {
    return v == nodata || v != v;  // v != v catches NaN for floating types
}

// FZ_OK when every mask value lies in [-1, n_zones), else FZ_ERR_ARG.
int validate_mask(const int32_t* mask, size_t n, int32_t n_zones);

// Validation shared by the zonal entry points (null pointers, shape, n_zones, options).
int validate_zonal_args(const void* raster, const int32_t* mask, fz_shape s, int32_t n_zones, const void* out,
                        const fz_options* opt);

void stats_init(fz_stats* acc, int32_t n_zones);
inline void stats_merge_one(fz_stats& into, const fz_stats& from) {
    into.count += from.count;
    into.sum += from.sum;
    into.sumsq += from.sumsq;
    if (from.min < into.min) into.min = from.min;
    if (from.max > into.max) into.max = from.max;
}
// into[z] += from[z] for every zone, in zone order.
void stats_merge(fz_stats* into, const fz_stats* from, int32_t n_zones);
// Zones with count 0 get min = max = 0 so no infinity leaves the library.
void stats_finalize(fz_stats* acc, int32_t n_zones);

// Thread-private dense accumulators plus the list of zones touched since the last reset.
struct StatsScratch {
    std::vector<int64_t> count;
    std::vector<double> sum, sumsq, min, max;
    std::vector<uint8_t> seen;     // zone already in touched (its count may still be zero)
    std::vector<int32_t> touched;

    void init(int32_t n_zones) {
        const size_t n = static_cast<size_t>(n_zones);
        count.assign(n, 0);
        sum.assign(n, 0.0);
        sumsq.assign(n, 0.0);
        min.assign(n, std::numeric_limits<double>::infinity());
        max.assign(n, -std::numeric_limits<double>::infinity());
        seen.assign(n, 0);
        touched.clear();
    }
    void touch(int32_t z) {
        if (!seen[static_cast<size_t>(z)]) {
            seen[static_cast<size_t>(z)] = 1;
            touched.push_back(z);
        }
    }
    void reset(int32_t z) {
        const size_t i = static_cast<size_t>(z);
        seen[i] = 0;
        count[i] = 0;
        sum[i] = 0.0;
        sumsq[i] = 0.0;
        min[i] = std::numeric_limits<double>::infinity();
        max[i] = -std::numeric_limits<double>::infinity();
    }
    fz_stats get(int32_t z) const {
        const size_t i = static_cast<size_t>(z);
        return fz_stats{count[i], sum[i], sumsq[i], min[i], max[i]};
    }
};

// Accumulates rows [row0, row1) into sc. Returns false (and stops) when a mask value lies outside
// [-1, n_zones); the caller then discards every partial, so no output is written for bad input.
// A zone is appended to sc.touched the first time it is met (even when all its cells turn out
// to be NoData); the check happens only where the zone changes between neighbouring cells,
// which is where a new zone can appear, and the seen flag keeps the list free of duplicates.
template <class T>
bool zonal_band(const T* raster, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1, T nodata,
                int32_t n_zones, StatsScratch& sc) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    int64_t* count = sc.count.data();
    double* sum = sc.sum.data();
    double* sumsq = sc.sumsq.data();
    double* mn = sc.min.data();
    double* mx = sc.max.data();
    int32_t prev = -1;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z != prev) {
            if (z < 0) {
                if (z != -1) return false;
            } else {
                if (z >= n_zones) return false;
                sc.touch(z);
            }
            prev = z;
        }
        if (z < 0) continue;
        const T v = raster[i];
        if (is_nodata(v, nodata)) continue;
        const double d = static_cast<double>(v);
        count[z] += 1;
        sum[z] += d;
        sumsq[z] += d * d;
        mn[z] = d < mn[z] ? d : mn[z];
        mx[z] = d > mx[z] ? d : mx[z];
    }
    return true;
}

template <class T>
bool ratio_band(const T* num, const T* den, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1,
                T nodata_num, T nodata_den, double eps, int32_t n_zones, StatsScratch& sc) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    int64_t* count = sc.count.data();
    double* sum = sc.sum.data();
    double* sumsq = sc.sumsq.data();
    double* mn = sc.min.data();
    double* mx = sc.max.data();
    int32_t prev = -1;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z != prev) {
            if (z < 0) {
                if (z != -1) return false;
            } else {
                if (z >= n_zones) return false;
                sc.touch(z);
            }
            prev = z;
        }
        if (z < 0) continue;
        const T a = num[i], b = den[i];
        if (is_nodata(a, nodata_num) || is_nodata(b, nodata_den)) continue;
        const double bd = static_cast<double>(b);
        if (bd < eps && bd > -eps) continue;
        const double d = static_cast<double>(a) / bd;
        count[z] += 1;
        sum[z] += d;
        sumsq[z] += d * d;
        mn[z] = d < mn[z] ? d : mn[z];
        mx[z] = d > mx[z] ? d : mx[z];
    }
    return true;
}

// Value -> count of one zone. Class rasters have a handful of distinct values per zone, so a
// small vector with linear search beats a hash map; it switches to a map past kSmallLimit.
class ValueCounts {
public:
    static constexpr size_t kSmallLimit = 48;

    bool empty() const { return small_.empty() && !big_; }

    void add(double v, int64_t n) {
        if (big_) {
            (*big_)[v] += n;
            return;
        }
        for (auto& kv : small_)
            if (kv.first == v) {
                kv.second += n;
                return;
            }
        if (small_.size() < kSmallLimit) {
            small_.emplace_back(v, n);
            return;
        }
        big_ = std::make_unique<std::unordered_map<double, int64_t>>();
        big_->reserve(small_.size() * 4);
        for (const auto& kv : small_) (*big_)[kv.first] += kv.second;
        small_.clear();
        (*big_)[v] += n;
    }

    template <class Fn>
    void for_each(Fn&& fn) const {
        if (big_) {
            for (const auto& kv : *big_) fn(kv.first, kv.second);
        } else {
            for (const auto& kv : small_) fn(kv.first, kv.second);
        }
    }

    void merge_into(ValueCounts& into) const {
        for_each([&](double v, int64_t n) { into.add(v, n); });
    }

    // The mode; ties resolve to the smallest value.
    double best() const {
        int64_t best_n = -1;
        double best_v = 0.0;
        for_each([&](double v, int64_t n) {
            if (n > best_n || (n == best_n && v < best_v)) {
                best_n = n;
                best_v = v;
            }
        });
        return best_v;
    }

private:
    std::vector<std::pair<double, int64_t>> small_;
    std::unique_ptr<std::unordered_map<double, int64_t>> big_;
};

using ZoneCounts = std::vector<ValueCounts>;

// Sparse per-thread majority state: slot[z] is the index of zone z in maps, or -1.
struct MajorityScratch {
    std::vector<int32_t> slot;
    std::vector<int32_t> zones;
    std::vector<ValueCounts> maps;
};

// Returns false on a mask value outside [-1, n_zones), as zonal_band does.
template <class T>
bool majority_band(const T* raster, const int32_t* mask, fz_shape s, int32_t row0, int32_t row1, T nodata,
                   int32_t n_zones, MajorityScratch& sc) {
    const size_t begin = static_cast<size_t>(row0) * s.width;
    const size_t end = static_cast<size_t>(row1) * s.width;
    int32_t prev = -1;
    int32_t idx = -1;
    for (size_t i = begin; i < end; ++i) {
        const int32_t z = mask[i];
        if (z != prev) {
            if (z < 0) {
                if (z != -1) return false;
            } else {
                if (z >= n_zones) return false;
                idx = sc.slot[z];
                if (idx < 0) {
                    idx = static_cast<int32_t>(sc.maps.size());
                    sc.slot[z] = idx;
                    sc.zones.push_back(z);
                    sc.maps.emplace_back();
                }
            }
            prev = z;
        }
        if (z < 0) continue;
        const T v = raster[i];
        if (is_nodata(v, nodata)) continue;
        sc.maps[static_cast<size_t>(idx)].add(static_cast<double>(v), 1);
    }
    return true;
}

// Picks the mode per zone (ties -> smallest value) with the centroid fallback for empty zones.
template <class T>
void majority_finalize(const T* raster, fz_shape s, T nodata, const ZoneCounts& counts, int32_t n_zones,
                       const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                       uint8_t* has_value_out) {
    for (int32_t z = 0; z < n_zones; ++z) {
        const auto& m = counts[static_cast<size_t>(z)];
        if (!m.empty()) {
            value_out[z] = m.best();
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
