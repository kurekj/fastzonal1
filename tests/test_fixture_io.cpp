// The fixture reader must refuse inconsistent files and must remap NoData in the storage type.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixture_io.hpp"

namespace fs = std::filesystem;

namespace {

struct TempFixture {
    fs::path dir;
    TempFixture() {
        dir = fs::temp_directory_path() / ("fz_fixture_io_" + std::to_string(std::rand()));
        fs::create_directories(dir);
    }
    ~TempFixture() { fs::remove_all(dir); }

    template <class T>
    void bin(const std::string& name, const std::vector<T>& v) const {
        std::ofstream out(dir / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
    }
    void text(const std::string& name, const std::string& s) const {
        std::ofstream out(dir / name);
        out << s;
    }
    void raster_header(const std::string& name, const std::string& dtype, int w, int h, const std::string& nodata) const {
        text("raster_" + name + ".json", "{\"dtype\": \"" + dtype + "\", \"width\": " + std::to_string(w) +
                                            ", \"height\": " + std::to_string(h) +
                                            ", \"transform\": [0.0, 2.0, 1.0, 1.0], \"nodata\": " + nodata + "}\n");
    }
    void zones(int n_zones, const std::vector<double>& xy, const std::vector<int64_t>& start,
               const std::vector<uint8_t>& hole, const std::vector<int32_t>& zone) const {
        text("zones_g.json", "{\"n_zones\": " + std::to_string(n_zones) + ", \"n_rings\": " +
                                 std::to_string(zone.size()) + ", \"n_points\": " + std::to_string(xy.size() / 2) + "}\n");
        bin("zones_g.xy.f64", xy);
        bin("zones_g.ring_start.i64", start);
        bin("zones_g.ring_is_hole.u8", hole);
        bin("zones_g.ring_zone.i32", zone);
    }
};

const std::vector<double> kSquare{0, 0, 2, 0, 2, 2, 0, 2};

}  // namespace

TEST_CASE("f32 nodata is compared in the storage type") {
    TempFixture t;
    t.raster_header("a", "f32", 2, 2, "0.1");
    t.bin<float>("raster_a.bin", {0.1f, 2.0f, 3.0f, 4.0f});
    const auto r = fzio::read_raster(t.dir.string(), "a");
    REQUIRE(r.pixels64[0] == -9999.0);
    REQUIRE(r.pixels64[1] == 2.0);
    REQUIRE(r.pixels32[0] == -9999.0f);
}

TEST_CASE("i32 nodata is remapped before any range check") {
    TempFixture t;
    t.raster_header("c", "i32", 2, 2, "-2147483648");
    t.bin<int32_t>("raster_c.bin", {1, 2, INT32_MIN, 3});
    const auto r = fzio::read_raster(t.dir.string(), "c");
    REQUIRE(r.pixels64[2] == -9999.0);
    REQUIRE(r.pixels64[0] == 1.0);
}

TEST_CASE("raster of the wrong size is rejected") {
    TempFixture t;
    t.raster_header("a", "f32", 2, 2, "null");
    t.bin<float>("raster_a.bin", {1.0f, 2.0f, 3.0f});
    REQUIRE_THROWS(fzio::read_raster(t.dir.string(), "a"));
}

TEST_CASE("inconsistent zone arrays are rejected") {
    TempFixture t;
    SECTION("hole array shorter than the ring count") {
        t.zones(1, kSquare, {0, 4}, {}, {0});
        REQUIRE_THROWS(fzio::read_zones(t.dir.string(), "g"));
    }
    SECTION("offset beyond the coordinate array") {
        t.zones(1, kSquare, {0, 5}, {0}, {0});
        REQUIRE_THROWS(fzio::read_zones(t.dir.string(), "g"));
    }
    SECTION("decreasing offsets") {
        t.zones(2, kSquare, {0, 4, 3}, {0, 0}, {0, 1});
        REQUIRE_THROWS(fzio::read_zones(t.dir.string(), "g"));
    }
    SECTION("zone id outside n_zones") {
        t.zones(1, kSquare, {0, 4}, {0}, {3});
        REQUIRE_THROWS(fzio::read_zones(t.dir.string(), "g"));
    }
    SECTION("a consistent set is accepted") {
        t.zones(1, kSquare, {0, 4}, {0}, {0});
        const auto z = fzio::read_zones(t.dir.string(), "g");
        REQUIRE(z.n_zones == 1);
        REQUIRE(z.n_rings() == 1);
    }
}

TEST_CASE("centroid file must hold exactly n_zones pairs") {
    TempFixture t;
    t.zones(2, kSquare, {0, 4}, {0}, {0});
    t.bin<int32_t>("zones_g.centroid_rc_a.i32", {1, 1});  // one pair for two zones
    REQUIRE_THROWS(fzio::read_centroids(t.dir.string(), "g", "a", 2));
    t.bin<int32_t>("zones_g.centroid_rc_a.i32", {1, 1, 0, 0});
    const auto c = fzio::read_centroids(t.dir.string(), "g", "a", 2);
    REQUIRE(c.rows.size() == 2);
    REQUIRE(fzio::read_centroids(t.dir.string(), "g", "missing", 2).empty());
}
