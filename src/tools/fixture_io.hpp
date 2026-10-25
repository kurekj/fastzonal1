// Reader for the raw fixture layout (benchmarks/fixtures/FORMAT.md), shared by the parity tests
// and the C++ benchmark driver. Not part of the library's public API.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fzio {

struct Raster {
    std::string name;
    int32_t width = 0;
    int32_t height = 0;
    fz_transform transform{};
    std::string dtype;            // f32, f64 or i32 as stored
    bool has_nodata = false;
    double nodata = 0.0;          // as stored
    std::vector<float> pixels32;  // NoData remapped to -9999, like the reference application
    std::vector<double> pixels64;
};

struct Zones {
    std::string name;
    int32_t n_zones = 0;
    std::vector<double> xy;
    std::vector<int64_t> ring_start;
    std::vector<uint8_t> ring_is_hole;
    std::vector<int32_t> ring_zone;
    int64_t n_rings() const { return static_cast<int64_t>(ring_zone.size()); }
};

struct Centroids {
    std::vector<int32_t> rows, cols;
    bool empty() const { return rows.empty(); }
};

struct FixtureMeta {
    std::string id;
    std::vector<std::string> rasters;
    std::vector<std::string> zones;
};

Raster read_raster(const std::string& dir, const std::string& name);
Zones read_zones(const std::string& dir, const std::string& name);
// Empty when the centroid file for this raster does not exist.
Centroids read_centroids(const std::string& dir, const std::string& zones, const std::string& raster);
FixtureMeta read_fixture_meta(const std::string& dir);
// Values of a `zone,value` CSV in zone order (header skipped).
std::vector<double> read_expected(const std::string& path);
// Nearest-neighbour resampling of src onto the grid of target; -9999 outside the source extent.
// Mirrors Fixture.ResampleNearest of the C# harness.
std::vector<double> resample_nearest(const Raster& src, const Raster& target);

}  // namespace fzio
