#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <string>

TEST_CASE("version string") { REQUIRE(std::string(fz_version()) == "0.1.0"); }

TEST_CASE("openmp flag matches build") {
#ifdef _OPENMP
    REQUIRE(fz_has_openmp() == 1);
#else
    REQUIRE(fz_has_openmp() == 0);
#endif
}

TEST_CASE("cuda flag is off in a cpu build") { REQUIRE(fz_has_cuda() == 0); }
