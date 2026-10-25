#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <cstdint>
#include <limits>
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

// ---- cases added after the independent review of the rasteriser ----

TEST_CASE("compat negative coordinates truncate toward zero") {
    Rings r;
    r.add(square(-0.8, 2, -0.2, 5), 4);
    auto m = run(r, FZ_RASTER_COMPAT);
    REQUIRE(count_zone(m, 4) == 3);  // column 0 on rows 5..7
    for (int row = 5; row <= 7; ++row) REQUIRE(at(m, row, 0) == 4);
    auto c = run(r, FZ_RASTER_CENTER);
    REQUIRE(count_assigned(c) == 0);  // no pixel centre inside
}

TEST_CASE("huge coordinates give an empty mask") {
    for (int mode : {FZ_RASTER_CENTER, FZ_RASTER_COMPAT}) {
        Rings r;
        r.add(square(1e30, 2, 2e30, 5), 0);
        r.add(square(2, -2e30, 5, -1e30), 1);
        r.add(square(-2e30, -2e30, -1e30, -1e30), 2);
        int rc;
        auto m = run(r, mode, &rc);
        REQUIRE(rc == FZ_OK);
        REQUIRE(count_assigned(m) == 0);
    }
}

TEST_CASE("extreme finite coordinates do not crash") {
    for (int mode : {FZ_RASTER_CENTER, FZ_RASTER_COMPAT}) {
        Rings r;
        r.add({-1e308, 2, 1e308, 8, 0, 2}, 0);
        int rc;
        auto m = run(r, mode, &rc);
        REQUIRE(rc == FZ_OK);
        for (auto v : m) REQUIRE((v == 0 || v == -1));
    }
}

TEST_CASE("non finite coordinates are rejected in every ring") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Rings r;
    r.add(square(0, 0, 10, 10), 0);
    r.add({nan, 2, 6, 2, 6, 6, 4, 6}, 0, true);
    int rc;
    run(r, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_ERR_ARG);
    run(r, FZ_RASTER_COMPAT, &rc);
    REQUIRE(rc == FZ_ERR_ARG);  // holes are validated even where they are ignored
    Rings e;
    e.add({std::numeric_limits<double>::infinity(), 0, 10, 0, 10, 10}, 0);
    run(e, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_ERR_ARG);
}

TEST_CASE("non finite pixel size is rejected") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    int rc;
    run(r, FZ_RASTER_CENTER, &rc, kShape, fz_transform{0, 10, std::numeric_limits<double>::infinity(), 1});
    REQUIRE(rc == FZ_ERR_ARG);
    run(r, FZ_RASTER_CENTER, &rc, kShape, fz_transform{0, 10, 1, std::numeric_limits<double>::quiet_NaN()});
    REQUIRE(rc == FZ_ERR_ARG);
}

TEST_CASE("rings with fewer than three points are skipped") {
    for (int mode : {FZ_RASTER_CENTER, FZ_RASTER_COMPAT}) {
        Rings r;
        r.add({2, 2, 2, 5}, 0);
        r.add({3, 3}, 1);
        int rc;
        auto m = run(r, mode, &rc);
        REQUIRE(rc == FZ_OK);
        REQUIRE(count_assigned(m) == 0);
    }
}

TEST_CASE("closed and unclosed rings are equivalent") {
    Rings open, closed;
    open.add({1, 1, 7, 2, 6, 8, 2, 6}, 0);
    closed.add({1, 1, 7, 2, 6, 8, 2, 6, 1, 1}, 0);
    for (int mode : {FZ_RASTER_CENTER, FZ_RASTER_COMPAT}) REQUIRE(run(open, mode) == run(closed, mode));
}

TEST_CASE("vertex exactly on a scanline") {
    // diamond with vertices on pixel-centre rows (y = 2.5 and 6.5) and columns (x = 2.5, 6.5)
    Rings r;
    r.add({4.5, 2.5, 6.5, 4.5, 4.5, 6.5, 2.5, 4.5}, 0);
    auto c = run(r, FZ_RASTER_CENTER);
    // half-open rule: the rows through the top and bottom vertices get no crossing pair, the row
    // through the side vertices spans [2.5, 6.5) and the two rows between span [3.5, 5.5)
    REQUIRE(at(c, 5, 4) == 0);  // centre (4.5, 4.5)
    REQUIRE(count_assigned(c) == 0 + 2 + 4 + 2 + 0);
    REQUIRE(at(c, 3, 4) == -1);
    REQUIRE(at(c, 7, 4) == -1);
    REQUIRE(at(c, 5, 2) == 0);
    REQUIRE(at(c, 5, 5) == 0);
    auto k = run(r, FZ_RASTER_COMPAT);
    REQUIRE(at(k, 5, 4) == 0);
    REQUIRE(at(k, 7, 4) == 0);  // the bottom vertex row: both crossings at x = 4.5 truncate to column 4
    // inclusive right column: rows 4..7 cover 3, 5, 3 and 1 columns
    REQUIRE(count_assigned(k) == 3 + 5 + 3 + 1);
}

TEST_CASE("compat later polygon wins and clips at every edge") {
    Rings r;
    r.add(square(-3, -3, 4, 4), 0);
    r.add(square(2, 2, 13, 13), 1);
    auto m = run(r, FZ_RASTER_COMPAT);
    REQUIRE(at(m, 9, 0) == 0);
    REQUIRE(at(m, 7, 3) == 1);  // overlap region
    REQUIRE(at(m, 0, 9) == 1);
    // A: rows 6..9 x columns 0..4 (20 cells); B: rows 0..7 x columns 2..9 (64 cells); overlap 6
    REQUIRE(count_zone(m, 1) == 8 * 8);
    REQUIRE(count_zone(m, 0) == 20 - 6);
    REQUIRE(count_assigned(m) == 78);
}

TEST_CASE("orphan hole and mismatched hole zone are rejected") {
    Rings orphan;
    orphan.add(square(1, 1, 3, 3), 0, true);
    int rc;
    run(orphan, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_ERR_ARG);
    Rings mismatched;
    mismatched.add(square(0, 0, 10, 10), 0);
    mismatched.add(square(2, 2, 4, 4), 3, true);
    run(mismatched, FZ_RASTER_CENTER, &rc);
    REQUIRE(rc == FZ_ERR_ARG);
}

TEST_CASE("invalid mode and backend are rejected") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    int rc;
    run(r, 7, &rc);
    REQUIRE(rc == FZ_ERR_ARG);
    fz_options opt;
    fz_options_default(&opt);
    opt.backend = 99;
    std::vector<int32_t> mask(100);
    REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1, kUnit, kShape,
                         mask.data(), &opt) == FZ_ERR_ARG);
}

TEST_CASE("shape too large for the address space is rejected") {
    Rings r;
    r.add(square(2, 2, 5, 5), 0);
    int rc;
    std::vector<int32_t> tiny(1);
    fz_options opt;
    fz_options_default(&opt);
    if (sizeof(size_t) == 4) {
        REQUIRE(fz_rasterize(r.xy.data(), r.ring_start.data(), r.is_hole.data(), r.zone.data(), 1, kUnit,
                             fz_shape{65536, 65536}, tiny.data(), &opt) == FZ_ERR_ARG);
    } else {
        (void)rc;
        SUCCEED("64-bit: every int32 shape is addressable");
    }
}
