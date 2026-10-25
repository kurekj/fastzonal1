#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fastzonal/fastzonal.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const fz_shape kShape{4, 4};
const float kNoData = -9999.0f;
const float kNaN = std::numeric_limits<float>::quiet_NaN();

// zone layout: zone 0 = top two rows, zone 1 = row 2, row 3 = outside (-1)
std::vector<int32_t> mask_two_zones() {
    return {0, 0, 0, 0,
            0, 0, 0, 0,
            1, 1, 1, 1,
            -1, -1, -1, -1};
}

fz_options serial() {
    fz_options o;
    fz_options_default(&o);
    o.backend = FZ_BACKEND_SERIAL;
    return o;
}

}  // namespace

TEST_CASE("mean sum count") {
    std::vector<float> r{1, 2, 3, 4,
                         5, 6, 7, 8,
                         10, 20, 30, 40,
                         99, 99, 99, 99};
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_OK);
    REQUIRE(out[0].count == 8);
    REQUIRE(out[0].sum == 36.0);
    REQUIRE(fz_stats_mean(&out[0]) == 4.5);
    REQUIRE(out[0].min == 1.0);
    REQUIRE(out[0].max == 8.0);
    REQUIRE(out[0].sumsq == 204.0);
    REQUIRE(out[1].count == 4);
    REQUIRE(fz_stats_mean(&out[1]) == 25.0);
}

TEST_CASE("nodata and nan skipped") {
    std::vector<float> r{1, kNoData, 3, kNaN,
                         5, 6, 7, 8,
                         10, 20, 30, 40,
                         0, 0, 0, 0};
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_OK);
    REQUIRE(out[0].count == 6);
    REQUIRE(out[0].sum == 30.0);
}

TEST_CASE("empty zone fallback centroid") {
    std::vector<float> r(16, 2.0f);
    r[3 * 4 + 1] = 7.0f;  // row 3, col 1
    std::vector<int32_t> mask(16, 0);
    std::vector<fz_stats> st(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, st.data(), &o) == FZ_OK);
    REQUIRE(st[1].count == 0);
    REQUIRE(fz_stats_mean(&st[1]) == 0.0);

    std::vector<int32_t> crow{0, 3}, ccol{0, 1};
    std::vector<double> mean(2);
    REQUIRE(fz_zonal_mean_fallback_f32(r.data(), kShape, kNoData, st.data(), 2, crow.data(), ccol.data(),
                                       mean.data()) == FZ_OK);
    REQUIRE(mean[0] == 37.0 / 16.0);  // fifteen cells of 2 and one of 7
    REQUIRE(mean[1] == 7.0);  // centroid sample

    std::vector<int32_t> crow_out{0, 9}, ccol_out{0, 1};
    REQUIRE(fz_zonal_mean_fallback_f32(r.data(), kShape, kNoData, st.data(), 2, crow_out.data(), ccol_out.data(),
                                       mean.data()) == FZ_OK);
    REQUIRE(mean[1] == 0.0);  // out of range

    r[3 * 4 + 1] = kNoData;
    REQUIRE(fz_zonal_mean_fallback_f32(r.data(), kShape, kNoData, st.data(), 2, crow.data(), ccol.data(),
                                       mean.data()) == FZ_OK);
    REQUIRE(mean[1] == 0.0);  // centroid on NoData

    REQUIRE(fz_zonal_mean_fallback_f32(r.data(), kShape, kNoData, st.data(), 2, nullptr, nullptr, mean.data()) ==
            FZ_OK);
    REQUIRE(mean[1] == 0.0);  // no centroids given
}

TEST_CASE("all nodata raster") {
    std::vector<float> r(16, kNoData);
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_OK);
    for (const auto& s : out) {
        REQUIRE(s.count == 0);
        REQUIRE(fz_stats_mean(&s) == 0.0);
        REQUIRE(fz_stats_std(&s) == 0.0);
        REQUIRE(s.min == 0.0);
        REQUIRE(s.max == 0.0);
        REQUIRE_FALSE(std::isnan(s.sum));
    }
}

TEST_CASE("invalid zone id") {
    std::vector<float> r(16, 1.0f);
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2, fz_stats{-7, -7.0, -7.0, -7.0, -7.0});
    auto o = serial();
    mask[5] = 5;
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(out[0].count == -7);
    mask[5] = -2;
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(out[1].sum == -7.0);
}

TEST_CASE("zonal argument errors") {
    std::vector<float> r(16, 1.0f);
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(nullptr, mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_f32(r.data(), nullptr, kShape, 2, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), fz_shape{0, 4}, 2, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 0, kNoData, out.data(), &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, &o) == FZ_ERR_ARG);
    o.backend = FZ_BACKEND_CUDA;
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, out.data(), &o) == FZ_ERR_BACKEND);
}

TEST_CASE("majority tie smallest") {
    std::vector<float> r{3, 3, 1, 1,
                         5, 5, 5, 2,
                         7, 7, 8, 8,
                         0, 0, 0, 0};
    auto mask = mask_two_zones();
    std::vector<double> val(2);
    std::vector<uint8_t> has(2);
    auto o = serial();
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, nullptr, val.data(), has.data(),
                                  &o) == FZ_OK);
    REQUIRE(val[0] == 5.0);  // 5 appears three times
    REQUIRE(has[0] == 1);
    REQUIRE(val[1] == 7.0);  // tie between 7 and 8 resolves to the smaller
    REQUIRE(has[1] == 1);
}

TEST_CASE("majority skips nodata and nan") {
    std::vector<float> r{kNoData, kNoData, kNoData, 1,
                         kNaN, kNaN, 2, 2,
                         7, 7, 8, 8,
                         0, 0, 0, 0};
    auto mask = mask_two_zones();
    std::vector<double> val(2);
    std::vector<uint8_t> has(2);
    auto o = serial();
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, nullptr, val.data(), has.data(),
                                  &o) == FZ_OK);
    REQUIRE(val[0] == 2.0);
}

TEST_CASE("majority empty zone fallback") {
    std::vector<float> r(16, 4.0f);
    r[3 * 4 + 2] = 9.0f;
    std::vector<int32_t> mask(16, 0);
    std::vector<double> val(2);
    std::vector<uint8_t> has(2);
    auto o = serial();
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, nullptr, val.data(), has.data(),
                                  &o) == FZ_OK);
    REQUIRE(val[1] == 0.0);
    REQUIRE(has[1] == 0);
    std::vector<int32_t> crow{0, 3}, ccol{0, 2};
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, crow.data(), ccol.data(), val.data(),
                                  has.data(), &o) == FZ_OK);
    REQUIRE(val[1] == 9.0);
    REQUIRE(has[1] == 1);
}

TEST_CASE("majority i32") {
    std::vector<int32_t> r{3, 3, 1, 1,
                           5, 5, 5, 2,
                           7, 7, 8, 8,
                           0, 0, 0, 0};
    auto mask = mask_two_zones();
    std::vector<double> val(2);
    std::vector<uint8_t> has(2);
    auto o = serial();
    REQUIRE(fz_zonal_majority_i32(r.data(), mask.data(), kShape, 2, -1, nullptr, nullptr, val.data(), has.data(), &o) ==
            FZ_OK);
    REQUIRE(val[0] == 5.0);
    REQUIRE(val[1] == 7.0);
}

TEST_CASE("ratio excludes small denominator") {
    std::vector<float> num{2, 4, 6, 8,
                           1, 1, 1, 1,
                           9, 9, 9, 9,
                           0, 0, 0, 0};
    std::vector<float> den{1, 2, 1e-13f, kNoData,
                           kNaN, 1, 1, 1,
                           3, 3, 3, 3,
                           1, 1, 1, 1};
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_ratio_f32(num.data(), den.data(), mask.data(), kShape, 2, kNoData, kNoData, 1e-12, out.data(),
                               &o) == FZ_OK);
    // zone 0 keeps cells (2/1, 4/2, 1/1, 1/1, 1/1): mean 7/5
    REQUIRE(out[0].count == 5);
    REQUIRE_THAT(fz_stats_mean(&out[0]), WithinAbs(1.4, 1e-12));
    REQUIRE(out[1].count == 4);
    REQUIRE_THAT(fz_stats_mean(&out[1]), WithinAbs(3.0, 1e-12));
}

TEST_CASE("ratio nodata in numerator excluded") {
    std::vector<float> num(16, kNoData);
    std::vector<float> den(16, 2.0f);
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    REQUIRE(fz_zonal_ratio_f32(num.data(), den.data(), mask.data(), kShape, 2, kNoData, kNoData, 1e-12, out.data(),
                               &o) == FZ_OK);
    REQUIRE(out[0].count == 0);
}

TEST_CASE("f64 matches f32 for exact values") {
    std::vector<float> r32(16);
    std::vector<double> r64(16);
    for (int i = 0; i < 16; ++i) {
        r32[i] = static_cast<float>((i * 65537) % 1048576);
        r64[i] = r32[i];
    }
    auto mask = mask_two_zones();
    std::vector<fz_stats> a(2), b(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r32.data(), mask.data(), kShape, 2, kNoData, a.data(), &o) == FZ_OK);
    REQUIRE(fz_zonal_f64(r64.data(), mask.data(), kShape, 2, -9999.0, b.data(), &o) == FZ_OK);
    for (int z = 0; z < 2; ++z) {
        REQUIRE(a[z].count == b[z].count);
        REQUIRE(a[z].sum == b[z].sum);
        REQUIRE(a[z].sumsq == b[z].sumsq);
        REQUIRE(a[z].min == b[z].min);
        REQUIRE(a[z].max == b[z].max);
    }
    std::vector<double> ma(2), mb(2);
    REQUIRE(fz_zonal_mean_fallback_f32(r32.data(), kShape, kNoData, a.data(), 2, nullptr, nullptr, ma.data()) == FZ_OK);
    REQUIRE(fz_zonal_mean_fallback_f64(r64.data(), kShape, -9999.0, b.data(), 2, nullptr, nullptr, mb.data()) == FZ_OK);
    REQUIRE(ma == mb);
    std::vector<double> va(2), vb(2);
    std::vector<uint8_t> ha(2), hb(2);
    REQUIRE(fz_zonal_majority_f64(r64.data(), mask.data(), kShape, 2, -9999.0, nullptr, nullptr, vb.data(), hb.data(),
                                  &o) == FZ_OK);
    REQUIRE(fz_zonal_majority_f32(r32.data(), mask.data(), kShape, 2, kNoData, nullptr, nullptr, va.data(), ha.data(),
                                  &o) == FZ_OK);
    REQUIRE(va == vb);
    std::vector<fz_stats> ra(2), rb(2);
    REQUIRE(fz_zonal_ratio_f64(r64.data(), r64.data(), mask.data(), kShape, 2, -9999.0, -9999.0, 1e-12, rb.data(),
                               &o) == FZ_OK);
    REQUIRE(fz_zonal_ratio_f32(r32.data(), r32.data(), mask.data(), kShape, 2, kNoData, kNoData, 1e-12, ra.data(),
                               &o) == FZ_OK);
    REQUIRE(ra[0].count == rb[0].count);
    REQUIRE(ra[0].sum == rb[0].sum);
}

// ---- cases added after the independent review of the reductions ----

TEST_CASE("mismatched centroid pointers are rejected before output") {
    std::vector<float> r(16, 1.0f);
    auto mask = mask_two_zones();
    std::vector<fz_stats> st(2);
    auto o = serial();
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, st.data(), &o) == FZ_OK);
    std::vector<int32_t> crow{0, 0};
    std::vector<double> mean(2, -5.0);
    REQUIRE(fz_zonal_mean_fallback_f32(r.data(), kShape, kNoData, st.data(), 2, crow.data(), nullptr, mean.data()) ==
            FZ_ERR_ARG);
    REQUIRE(mean[0] == -5.0);
    std::vector<double> val(2, -5.0);
    std::vector<uint8_t> has(2, 7);
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, crow.data(), val.data(),
                                  has.data(), &o) == FZ_ERR_ARG);
    REQUIRE(val[0] == -5.0);
    REQUIRE(has[0] == 7);
}

TEST_CASE("ratio eps boundaries") {
    std::vector<float> num(16, 1.0f);
    std::vector<float> den{1e-3f, -1e-3f, 0.015625f, -0.015625f,
                           0.5f, 0.5f, 0.5f, 0.5f,
                           1, 1, 1, 1,
                           1, 1, 1, 1};
    auto mask = mask_two_zones();
    std::vector<fz_stats> out(2);
    auto o = serial();
    // eps = 2^-6 (exact in float): |den| < eps excludes the two 1e-3 cells, keeps the two cells exactly at eps
    const double eps = 0.015625;
    REQUIRE(fz_zonal_ratio_f32(num.data(), den.data(), mask.data(), kShape, 2, kNoData, kNoData, eps, out.data(),
                               &o) == FZ_OK);
    REQUIRE(out[0].count == 6);
    REQUIRE(out[0].min == -64.0);
    REQUIRE(out[0].max == 64.0);
    REQUIRE(fz_zonal_ratio_f32(num.data(), den.data(), mask.data(), kShape, 2, kNoData, kNoData, -1.0, out.data(),
                               &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_ratio_f32(num.data(), den.data(), mask.data(), kShape, 2, kNoData, kNoData,
                               std::numeric_limits<double>::quiet_NaN(), out.data(), &o) == FZ_ERR_ARG);
}

TEST_CASE("invalid mask leaves every output untouched") {
    std::vector<float> r(16, 1.0f);
    auto mask = mask_two_zones();
    mask[9] = 2;
    fz_options o;
    fz_options_default(&o);
    o.band_rows = 1;
    std::vector<fz_stats> st(2, fz_stats{-7, -7.0, -7.0, -7.0, -7.0});
    REQUIRE(fz_zonal_f32(r.data(), mask.data(), kShape, 2, kNoData, st.data(), &o) == FZ_ERR_ARG);
    REQUIRE(fz_zonal_ratio_f32(r.data(), r.data(), mask.data(), kShape, 2, kNoData, kNoData, 1e-12, st.data(), &o) ==
            FZ_ERR_ARG);
    for (const auto& s : st) {
        REQUIRE(s.count == -7);
        REQUIRE(s.sum == -7.0);
        REQUIRE(s.min == -7.0);
    }
    std::vector<double> val(2, -5.0);
    std::vector<uint8_t> has(2, 7);
    REQUIRE(fz_zonal_majority_f32(r.data(), mask.data(), kShape, 2, kNoData, nullptr, nullptr, val.data(), has.data(),
                                  &o) == FZ_ERR_ARG);
    REQUIRE(val[1] == -5.0);
    REQUIRE(has[1] == 7);
}
