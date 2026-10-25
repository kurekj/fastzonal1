#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <cstdint>
#include <vector>

namespace {

// Flat polygon set builder for tests: one exterior ring per call, optional holes.
struct Rings {
    std::vector<double> xy;
    std::vector<int64_t> ring_start{0};
    std::vector<uint8_t> is_hole;
    std::vector<int32_t> zone;

    void add(const std::vector<double>& pts, int32_t z, bool hole = false) {
        xy.insert(xy.end(), pts.begin(), pts.end());
        ring_start.push_back(static_cast<int64_t>(xy.size() / 2));
        is_hole.push_back(hole ? 1 : 0);
        zone.push_back(z);
    }
    int64_t n_rings() const { return static_cast<int64_t>(zone.size()); }
};

std::vector<double> square(double x0, double y0, double x1, double y1) {
    return {x0, y0, x1, y0, x1, y1, x0, y1};
}

const fz_transform kUnit{0.0, 10.0, 1.0, 1.0};  // 10x10 unit pixels, origin top-left (0,10)
const fz_shape kShape{10, 10};

std::vector<int32_t> run(const Rings& r, int mode, int* code = nullptr,
                         fz_shape shape = kShape, fz_transform t = kUnit) {
    fz_options opt;
    fz_options_default(&opt);
    opt.raster_mode = mode;
    opt.backend = FZ_BACKEND_SERIAL;
    std::vector<int32_t> mask(static_cast<size_t>(shape.width) * shape.height, 99);
    int rc = fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(),
                          r.n_rings(), t, shape, mask.data(), &opt);
    if (code) *code = rc;
    return mask;
}

int64_t count_zone(const std::vector<int32_t>& m, int32_t z) {
    int64_t n = 0;
    for (auto v : m) n += (v == z);
    return n;
}

int64_t count_assigned(const std::vector<int32_t>& m) {
    int64_t n = 0;
    for (auto v : m) n += (v >= 0);
    return n;
}

int32_t at(const std::vector<int32_t>& m, int row, int col) { return m[static_cast<size_t>(row) * 10 + col]; }

}  // namespace

TEST_CASE("square center mode") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    int rc;
    auto m = run(r, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_OK);
    REQUIRE(count_zone(m, 0) == 9);
    REQUIRE(count_assigned(m) == 9);
    // centres 2.5, 3.5, 4.5 -> columns 2..4; y centres 2.5..4.5 -> rows 7..5
    for (int row = 5; row <= 7; ++row)
        for (int col = 2; col <= 4; ++col) REQUIRE(at(m, row, col) == 0);
    REQUIRE(at(m, 4, 2) == -1);
    REQUIRE(at(m, 5, 5) == -1);
}

TEST_CASE("square compat mode") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    auto m = run(r, FZ_RASTER_COMPAT);
    REQUIRE(count_zone(m, 0) == 12);
    for (int row = 5; row <= 7; ++row)
        for (int col = 2; col <= 5; ++col) REQUIRE(at(m, row, col) == 0);
}

TEST_CASE("triangle partial pixels") {
    Rings r;
    r.add({0, 0, 10, 0, 0, 10}, 3);
    auto c = run(r, FZ_RASTER_CENTER);
    REQUIRE(count_zone(c, 3) == 45);
    auto k = run(r, FZ_RASTER_COMPAT);
    REQUIRE(count_zone(k, 3) == 55);
}

TEST_CASE("hole shows previous polygon") {
    Rings r;
    r.add(square(0, 0, 10, 10), 0);
    r.add(square(2, 2, 8, 8), 1);
    r.add(square(4, 4, 6, 6), 1, true);
    auto m = run(r, FZ_RASTER_CENTER);
    REQUIRE(at(m, 5, 5) == 0);  // inside the hole: A shows through
    REQUIRE(at(m, 4, 4) == 0);
    REQUIRE(at(m, 3, 3) == 1);  // inside B's ring, outside the hole
    REQUIRE(at(m, 6, 6) == 1);
    REQUIRE(at(m, 0, 0) == 0);
    REQUIRE(count_zone(m, 1) == 36 - 4);
    REQUIRE(count_zone(m, 0) == 100 - 32);
}

TEST_CASE("compat mode ignores holes") {
    Rings r;
    r.add(square(2, 2, 8, 8), 1);
    r.add(square(4, 4, 6, 6), 1, true);
    auto m = run(r, FZ_RASTER_COMPAT);
    REQUIRE(at(m, 5, 5) == 1);
}

TEST_CASE("later polygon wins") {
    Rings r;
    r.add(square(0, 0, 6, 6), 0);
    r.add(square(3, 3, 9, 9), 1);
    auto m = run(r, FZ_RASTER_CENTER);
    REQUIRE(at(m, 5, 4) == 1);  // overlap region (3..6 x 3..6)
    REQUIRE(at(m, 8, 1) == 0);
    REQUIRE(at(m, 2, 8) == 1);
}

TEST_CASE("bbox outside raster") {
    Rings r;
    r.add(square(-5, -5, 3, 3), 2);
    int rc;
    auto m = run(r, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_OK);
    REQUIRE(count_zone(m, 2) == 9);  // columns 0..2, rows 7..9
    REQUIRE(at(m, 9, 0) == 2);
    REQUIRE(at(m, 6, 0) == -1);
    r = Rings();
    r.add(square(20, 20, 30, 30), 2);
    m = run(r, FZ_RASTER_COMPAT, &rc);
    REQUIRE(rc == FZ_OK);
    REQUIRE(count_assigned(m) == 0);
}

TEST_CASE("multipolygon two rings same zone") {
    Rings r;
    r.add(square(0, 0, 2, 2), 7);
    r.add(square(6, 6, 9, 9), 7);
    auto m = run(r, FZ_RASTER_CENTER);
    REQUIRE(count_zone(m, 7) == 4 + 9);
}

TEST_CASE("mask is fully written") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    auto m = run(r, FZ_RASTER_CENTER);
    for (auto v : m) REQUIRE((v == 0 || v == -1));
}

TEST_CASE("rasterize argument errors") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    fz_options opt;
    fz_options_default(&opt);
    std::vector<int32_t> mask(100);
    REQUIRE(fz_rasterize(nullptr, r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1, kUnit, kShape,
                         mask.data(), &opt) == FZ_ERR_ARG);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), -1, kUnit, kShape,
                         mask.data(), &opt) == FZ_ERR_ARG);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1, kUnit,
                         fz_shape{0, 10}, mask.data(), &opt) == FZ_ERR_ARG);
    std::vector<int32_t> bad_zone{-1};
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), bad_zone.data(), 1, kUnit, kShape,
                         mask.data(), &opt) == FZ_ERR_ARG);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1,
                         fz_transform{0, 10, 0.0, 1.0}, kShape, mask.data(), &opt) == FZ_ERR_ARG);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1, kUnit, kShape,
                         nullptr, &opt) == FZ_ERR_ARG);
}

TEST_CASE("zero rings clears the mask") {
    Rings r;
    fz_options opt;
    fz_options_default(&opt);
    std::vector<int32_t> mask(100, 5);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 0, kUnit, kShape,
                         mask.data(), &opt) == FZ_OK);
    REQUIRE(count_assigned(mask) == 0);
}
