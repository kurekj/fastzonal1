#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace {

// Deterministic synthetic workload: a 1000 x 1300 float raster with 5% NoData and 2,000 random
// rectangles as zones (overlaps allowed, later rectangles win).
struct Workload {
    static constexpr int32_t kW = 1300, kH = 1000, kZones = 2000;
    const fz_transform t{0.0, static_cast<double>(kH), 1.0, 1.0};
    const fz_shape s{kW, kH};
    std::vector<float> raster;
    std::vector<float> raster2;    // independent values for a non-unit ratio
    std::vector<double> raster64;  // values whose squares need rounding
    std::vector<double> xy;
    std::vector<int64_t> ring_start{0};
    std::vector<uint8_t> is_hole;
    std::vector<int32_t> zone;

    Workload() {
        uint64_t state = 0x9E3779B97F4A7C15ull;
        auto next = [&state]() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
        raster.resize(static_cast<size_t>(kW) * kH);
        raster2.resize(raster.size());
        raster64.resize(raster.size());
        for (size_t i = 0; i < raster.size(); ++i) {
            const uint64_t r = next();
            raster[i] = (r % 100 < 5) ? -9999.0f : static_cast<float>((r >> 8) % 100000) / 7.0f;
            const uint64_t q = next();
            raster2[i] = (q % 100 < 3) ? -9999.0f : 0.25f + static_cast<float>((q >> 8) % 1000) / 13.0f;
            raster64[i] = (r % 100 < 5) ? -9999.0 : 1.0 + static_cast<double>((r >> 16) % 1000003) / 3.0;
        }
        for (int32_t z = 0; z < kZones; ++z) {
            const double x0 = static_cast<double>(next() % kW), y0 = static_cast<double>(next() % kH);
            const double w = 1.0 + static_cast<double>(next() % 60), h = 1.0 + static_cast<double>(next() % 60);
            const double x1 = x0 + w, y1 = y0 + h;
            xy.insert(xy.end(), {x0, y0, x1, y0, x1, y1, x0, y1});
            ring_start.push_back(static_cast<int64_t>(xy.size() / 2));
            is_hole.push_back(0);
            zone.push_back(z);
            if (w > 8 && h > 8) {  // a hole straddling band boundaries in most splits
                const double hx0 = x0 + 2, hy0 = y0 + 2, hx1 = x1 - 2, hy1 = y1 - 2;
                xy.insert(xy.end(), {hx0, hy0, hx1, hy0, hx1, hy1, hx0, hy1});
                ring_start.push_back(static_cast<int64_t>(xy.size() / 2));
                is_hole.push_back(1);
                zone.push_back(z);
            }
        }
    }

    fz_options options(int backend, int threads, int band_rows) const {
        fz_options o;
        fz_options_default(&o);
        o.backend = backend;
        o.threads = threads;
        o.band_rows = band_rows;
        return o;
    }

    int64_t n_rings() const { return static_cast<int64_t>(zone.size()); }

    std::vector<int32_t> mask(const fz_options& o) const {
        std::vector<int32_t> m(raster.size());
        REQUIRE(fz_rasterize(xy.data(), ring_start.data(), is_hole.data(), zone.data(), n_rings(), t, s, m.data(),
                             &o) == FZ_OK);
        return m;
    }

    std::vector<fz_stats> stats64(const std::vector<int32_t>& m, const fz_options& o) const {
        std::vector<fz_stats> st(kZones);
        REQUIRE(fz_zonal_f64(raster64.data(), m.data(), s, kZones, -9999.0, st.data(), &o) == FZ_OK);
        return st;
    }

    std::vector<fz_stats> stats(const std::vector<int32_t>& m, const fz_options& o) const {
        std::vector<fz_stats> st(kZones);
        REQUIRE(fz_zonal_f32(raster.data(), m.data(), s, kZones, -9999.0f, st.data(), &o) == FZ_OK);
        return st;
    }

    std::vector<fz_stats> ratio(const std::vector<int32_t>& m, const fz_options& o) const {
        std::vector<fz_stats> st(kZones);
        REQUIRE(fz_zonal_ratio_f32(raster.data(), raster2.data(), m.data(), s, kZones, -9999.0f, -9999.0f, 1e-12,
                                   st.data(), &o) == FZ_OK);
        return st;
    }

    std::pair<std::vector<double>, std::vector<uint8_t>> majority(const std::vector<int32_t>& m,
                                                                  const fz_options& o) const {
        std::vector<double> v(kZones);
        std::vector<uint8_t> has(kZones);
        REQUIRE(fz_zonal_majority_f32(raster.data(), m.data(), s, kZones, -9999.0f, nullptr, nullptr, v.data(),
                                      has.data(), &o) == FZ_OK);
        return {v, has};
    }
};

bool same_stats(const std::vector<fz_stats>& a, const std::vector<fz_stats>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(fz_stats)) == 0;
}

}  // namespace

TEST_CASE("openmp backend reports availability") {
    const Workload w;
    const auto o = w.options(FZ_BACKEND_OPENMP, 2, 64);
    std::vector<int32_t> m(w.raster.size());
    const int rc = fz_rasterize(w.xy.data(), w.ring_start.data(), w.is_hole.data(), w.zone.data(), w.n_rings(), w.t,
                                w.s, m.data(), &o);
    if (fz_has_openmp())
        REQUIRE(rc == FZ_OK);
    else
        REQUIRE(rc == FZ_ERR_BACKEND);
}

TEST_CASE("serial equals openmp bitwise") {
    if (!fz_has_openmp()) {
        SUCCEED("serial-only build");
        return;
    }
    const Workload w;
    const auto serial_opt = w.options(FZ_BACKEND_SERIAL, 1, 64);
    const auto mask_ref = w.mask(serial_opt);
    const auto stats_ref = w.stats(mask_ref, serial_opt);
    for (int band_rows : {1, 64, 5000}) {
        // the band split fixes the summation order: the serial result for this split is the reference
        const auto so = w.options(FZ_BACKEND_SERIAL, 1, band_rows);
        const auto stats_split = w.stats(mask_ref, so);
        const auto stats64_split = w.stats64(mask_ref, so);
        const auto ratio_split = w.ratio(mask_ref, so);
        for (int threads : {1, 2, 7, 64}) {
            const auto o = w.options(FZ_BACKEND_OPENMP, threads, band_rows);
            INFO("threads " << threads << " band_rows " << band_rows);
            REQUIRE(w.mask(o) == mask_ref);  // the mask never depends on the split
            REQUIRE(same_stats(w.stats(mask_ref, o), stats_split));
            REQUIRE(same_stats(w.stats64(mask_ref, o), stats64_split));
            REQUIRE(same_stats(w.ratio(mask_ref, o), ratio_split));
        }
        // different splits change only the rounding order
        for (int32_t z = 0; z < Workload::kZones; ++z) {
            REQUIRE(stats_split[z].count == stats_ref[z].count);
            REQUIRE(stats_split[z].min == stats_ref[z].min);
            REQUIRE(stats_split[z].max == stats_ref[z].max);
            REQUIRE(std::fabs(stats_split[z].sum - stats_ref[z].sum) <= 1e-12 * std::fabs(stats_ref[z].sum) + 1e-300);
        }
    }
}

TEST_CASE("compat mask with holes equals serial across splits") {
    if (!fz_has_openmp()) return;
    const Workload w;
    auto so = w.options(FZ_BACKEND_SERIAL, 1, 64);
    so.raster_mode = FZ_RASTER_COMPAT;
    const auto ref = w.mask(so);
    for (int band_rows : {1, 3, 64, 5000}) {
        auto o = w.options(FZ_BACKEND_OPENMP, 5, band_rows);
        o.raster_mode = FZ_RASTER_COMPAT;
        REQUIRE(w.mask(o) == ref);
    }
}

TEST_CASE("majority serial equals openmp") {
    if (!fz_has_openmp()) return;
    const Workload w;
    const auto serial_opt = w.options(FZ_BACKEND_SERIAL, 1, 64);
    const auto mask_ref = w.mask(serial_opt);
    const auto ref = w.majority(mask_ref, serial_opt);
    for (int threads : {2, 7}) {
        for (int band_rows : {1, 64, 5000}) {
            const auto o = w.options(FZ_BACKEND_OPENMP, threads, band_rows);
            REQUIRE(w.majority(mask_ref, o) == ref);
        }
    }
}

TEST_CASE("auto backend equals serial") {
    const Workload w;
    const auto serial_opt = w.options(FZ_BACKEND_SERIAL, 1, 64);
    const auto auto_opt = w.options(FZ_BACKEND_AUTO, 0, 64);
    const auto m = w.mask(serial_opt);
    REQUIRE(w.mask(auto_opt) == m);
    REQUIRE(same_stats(w.stats(m, auto_opt), w.stats(m, serial_opt)));
}

TEST_CASE("cuda backend not available") {
    const Workload w;
    const auto o = w.options(FZ_BACKEND_CUDA, 0, 64);
    std::vector<int32_t> m(w.raster.size());
    REQUIRE(fz_rasterize(w.xy.data(), w.ring_start.data(), w.is_hole.data(), w.zone.data(), w.n_rings(), w.t,
                         w.s, m.data(), &o) == FZ_ERR_BACKEND);
    std::vector<fz_stats> st(Workload::kZones);
    REQUIRE(fz_zonal_f32(w.raster.data(), m.data(), w.s, Workload::kZones, -9999.0f, st.data(), &o) ==
            FZ_ERR_BACKEND);
}

TEST_CASE("invalid options are rejected") {
    const Workload w;
    auto o = w.options(FZ_BACKEND_OPENMP, 2, 0);
    std::vector<int32_t> m(w.raster.size());
    REQUIRE(fz_rasterize(w.xy.data(), w.ring_start.data(), w.is_hole.data(), w.zone.data(), w.n_rings(), w.t,
                         w.s, m.data(), &o) == FZ_ERR_ARG);
    o = w.options(FZ_BACKEND_OPENMP, -3, 64);
    REQUIRE(fz_rasterize(w.xy.data(), w.ring_start.data(), w.is_hole.data(), w.zone.data(), w.n_rings(), w.t,
                         w.s, m.data(), &o) == FZ_ERR_ARG);
    o = w.options(42, 1, 64);
    REQUIRE(fz_rasterize(w.xy.data(), w.ring_start.data(), w.is_hole.data(), w.zone.data(), w.n_rings(), w.t,
                         w.s, m.data(), &o) == FZ_ERR_ARG);
}
