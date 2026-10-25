// Parity of the library with the C# baseline on the fixtures that carry expected outputs.
//
// For every (fixture, zones, raster) with files under <expected>/<fixture>/, the test rasterises in
// COMPAT mode and compares per-zone pixel counts (exact), the double-precision mean with the
// centroid fallback (relative 1e-9), the majority (exact), the float32 mean (relative 1e-6) and,
// where the expected ratio file exists, the ratio mean (relative 1e-9).
//
// Directories: FZ_PARITY_FIXTURE_DIR and FZ_PARITY_EXPECTED_DIR override the compiled defaults
// (tests/fixtures and tests/fixtures/expected), so the same binary runs on the full fixture set.
#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture_io.hpp"

namespace fs = std::filesystem;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

const std::string kFixtureRoot = env_or("FZ_PARITY_FIXTURE_DIR", FZ_FIXTURE_DIR);
const std::string kExpectedRoot = env_or("FZ_PARITY_EXPECTED_DIR", std::string(FZ_FIXTURE_DIR) + "/expected");

struct Case {
    std::string fixture, zones, raster;
};

// Every mean_<zones>_<raster>.csv under the expected root names one case.
std::vector<Case> discover() {
    std::vector<Case> cases;
    if (!fs::exists(kExpectedRoot)) return cases;
    for (const auto& fx : fs::directory_iterator(kExpectedRoot)) {
        if (!fx.is_directory()) continue;
        for (const auto& f : fs::directory_iterator(fx.path())) {
            const std::string name = f.path().filename().string();
            if (name.rfind("mean_", 0) != 0 || name.size() < 9) continue;
            const std::string body = name.substr(5, name.size() - 9);  // strip "mean_" and ".csv"
            const auto us = body.find('_');
            if (us == std::string::npos) continue;
            cases.push_back({fx.path().filename().string(), body.substr(0, us), body.substr(us + 1)});
        }
    }
    return cases;
}

void require_close(const std::vector<double>& got, const std::vector<double>& want, double rel,
                   const std::string& what) {
    REQUIRE(got.size() == want.size());
    size_t worst = 0;
    double worst_err = 0.0;
    for (size_t z = 0; z < got.size(); ++z) {
        const double tol = rel * std::max(1.0, std::fabs(want[z]));
        const double err = std::fabs(got[z] - want[z]);
        if (err > worst_err) {
            worst_err = err;
            worst = z;
        }
        if (err > tol) {
            INFO(what << ": zone " << z << " got " << got[z] << " want " << want[z]);
            REQUIRE(err <= tol);
        }
    }
    INFO(what << ": worst zone " << worst << " abs error " << worst_err);
    SUCCEED();
}

fz_options compat_opts() {
    fz_options o;
    fz_options_default(&o);
    o.raster_mode = FZ_RASTER_COMPAT;
    return o;
}

}  // namespace

TEST_CASE("parity cases exist") {
    const auto cases = discover();
    INFO("expected root " << kExpectedRoot);
    REQUIRE_FALSE(cases.empty());
}

TEST_CASE("parity with the csharp baseline") {
    int checks = 0;
    for (const Case& c : discover()) {
        const std::string fdir = kFixtureRoot + "/" + c.fixture;
        const std::string edir = kExpectedRoot + "/" + c.fixture;
        INFO(c.fixture << " " << c.zones << " x " << c.raster);
        const fzio::Raster raster = fzio::read_raster(fdir, c.raster);
        const fzio::Zones zones = fzio::read_zones(fdir, c.zones);
        const fz_shape shape{raster.width, raster.height};
        const fz_transform t = raster.transform;
        const auto opts = compat_opts();

        std::vector<int32_t> mask(raster.pixels64.size());
        REQUIRE(fz_rasterize(zones.xy.data(), zones.ring_start.data(), zones.ring_is_hole.data(),
                             zones.ring_zone.data(), zones.n_rings(), t, shape, mask.data(), &opts) == FZ_OK);

        {  // rasterize counts
            const auto want = fzio::read_expected(edir + "/rasterize_" + c.zones + "_" + c.raster + ".csv");
            std::vector<double> counts(static_cast<size_t>(zones.n_zones), 0.0);
            for (int32_t v : mask)
                if (v >= 0) counts[static_cast<size_t>(v)] += 1.0;
            require_close(counts, want, 0.0, "rasterize");
            ++checks;
        }

        const auto centroids = fzio::read_centroids(fdir, c.zones, c.raster);
        const int32_t* crow = centroids.empty() ? nullptr : centroids.rows.data();
        const int32_t* ccol = centroids.empty() ? nullptr : centroids.cols.data();

        {  // mean f64
            std::vector<fz_stats> st(static_cast<size_t>(zones.n_zones));
            REQUIRE(fz_zonal_f64(raster.pixels64.data(), mask.data(), shape, zones.n_zones, -9999.0, st.data(),
                                 &opts) == FZ_OK);
            std::vector<double> mean(static_cast<size_t>(zones.n_zones));
            REQUIRE(fz_zonal_mean_fallback_f64(raster.pixels64.data(), shape, -9999.0, st.data(), zones.n_zones,
                                               crow, ccol, mean.data()) == FZ_OK);
            const auto want = fzio::read_expected(edir + "/mean_" + c.zones + "_" + c.raster + ".csv");
            require_close(mean, want, 1e-9, "mean f64");
            ++checks;
        }

        {  // mean f32
            std::vector<fz_stats> st(static_cast<size_t>(zones.n_zones));
            REQUIRE(fz_zonal_f32(raster.pixels32.data(), mask.data(), shape, zones.n_zones, -9999.0f, st.data(),
                                 &opts) == FZ_OK);
            std::vector<double> mean(static_cast<size_t>(zones.n_zones));
            REQUIRE(fz_zonal_mean_fallback_f32(raster.pixels32.data(), shape, -9999.0f, st.data(), zones.n_zones,
                                               crow, ccol, mean.data()) == FZ_OK);
            const auto want = fzio::read_expected(edir + "/mean_" + c.zones + "_" + c.raster + ".csv");
            require_close(mean, want, 1e-6, "mean f32");
            ++checks;
        }

        {  // majority
            std::vector<double> val(static_cast<size_t>(zones.n_zones));
            std::vector<uint8_t> has(static_cast<size_t>(zones.n_zones));
            REQUIRE(fz_zonal_majority_f64(raster.pixels64.data(), mask.data(), shape, zones.n_zones, -9999.0, crow,
                                          ccol, val.data(), has.data(), &opts) == FZ_OK);
            const auto want = fzio::read_expected(edir + "/majority_" + c.zones + "_" + c.raster + ".csv");
            require_close(val, want, 0.0, "majority");
            ++checks;
        }

        {  // ratio
            // the expected ratio of a fixture is <first raster> / <second raster> (or itself)
            const fzio::FixtureMeta meta = fzio::read_fixture_meta(fdir);
            if (!meta.rasters.empty() && c.raster == meta.rasters[0]) {
                const std::string den_name = meta.rasters.size() > 1 ? meta.rasters[1] : meta.rasters[0];
                const std::string path = edir + "/ratio_" + c.zones + "_" + c.raster + "_" + den_name + ".csv";
                REQUIRE(fs::exists(path));
                const fzio::Raster den = (den_name == c.raster) ? raster : fzio::read_raster(fdir, den_name);
                const std::vector<double> den_on_grid =
                    (den_name == c.raster) ? raster.pixels64 : fzio::resample_nearest(den, raster);
                // the baseline forms the ratio raster first (NaN where excluded) and takes the mean
                // with the centroid fallback on that raster
                std::vector<double> ratio_raster(raster.pixels64.size());
                for (size_t i = 0; i < ratio_raster.size(); ++i) {
                    const double a = raster.pixels64[i], b = den_on_grid[i];
                    const bool bad = std::isnan(a) || a == -9999.0 || std::isnan(b) || b == -9999.0 ||
                                     std::fabs(b) < 1e-12;
                    ratio_raster[i] = bad ? std::nan("") : a / b;
                }
                std::vector<fz_stats> st(static_cast<size_t>(zones.n_zones));
                REQUIRE(fz_zonal_ratio_f64(raster.pixels64.data(), den_on_grid.data(), mask.data(), shape,
                                           zones.n_zones, -9999.0, -9999.0, 1e-12, st.data(), &opts) == FZ_OK);
                std::vector<double> mean(static_cast<size_t>(zones.n_zones));
                REQUIRE(fz_zonal_mean_fallback_f64(ratio_raster.data(), shape, -9999.0, st.data(), zones.n_zones,
                                                   crow, ccol, mean.data()) == FZ_OK);
                require_close(mean, fzio::read_expected(path), 1e-9, "ratio");
                ++checks;
            }
        }
    }
    INFO("checks run: " << checks);
    REQUIRE(checks >= 5 * 2);  // at least the two committed fixtures, five checks each
}
