#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fastzonal/fastzonal.h>

#include <string>

TEST_CASE("options default") {
    fz_options opt;
    fz_options_default(&opt);
    REQUIRE(opt.backend == FZ_BACKEND_AUTO);
    REQUIRE(opt.threads == 0);
    REQUIRE(opt.device == 0);
    REQUIRE(opt.raster_mode == FZ_RASTER_CENTER);
    REQUIRE(opt.band_rows == 64);
}

TEST_CASE("error strings") {
    REQUIRE(std::string(fz_error_string(FZ_OK)) == "ok");
    REQUIRE(std::string(fz_error_string(FZ_ERR_ARG)) == "invalid argument");
    REQUIRE(std::string(fz_error_string(FZ_ERR_BACKEND)) == "backend not available");
    REQUIRE(std::string(fz_error_string(FZ_ERR_NOMEM)) == "out of memory");
    REQUIRE(std::string(fz_error_string(FZ_ERR_INTERNAL)) == "internal error");
    REQUIRE(std::string(fz_error_string(99)) == "unknown error");
}

TEST_CASE("stats mean") {
    fz_stats s{4, 10.0, 0.0, 0.0, 0.0};
    REQUIRE(fz_stats_mean(&s) == 2.5);
    fz_stats empty{0, 0.0, 0.0, 0.0, 0.0};
    REQUIRE(fz_stats_mean(&empty) == 0.0);
}

TEST_CASE("stats population std") {
    // values 2,4,4,4,5,5,7,9: count 8, sum 40, sum of squares 232, population std 2
    fz_stats s{8, 40.0, 232.0, 2.0, 9.0};
    REQUIRE_THAT(fz_stats_std(&s), Catch::Matchers::WithinAbs(2.0, 1e-12));
    fz_stats one{1, 3.0, 9.0, 3.0, 3.0};
    REQUIRE(fz_stats_std(&one) == 0.0);
    fz_stats none{0, 0.0, 0.0, 0.0, 0.0};
    REQUIRE(fz_stats_std(&none) == 0.0);
}
