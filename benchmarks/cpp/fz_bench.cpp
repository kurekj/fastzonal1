// Benchmark driver for the library itself: times one operation of one fixture with the serial or
// OpenMP backend and appends CSV lines with the same columns and checksum definitions as the C#
// harness (benchmarks/csharp/FzBench), so both feed one results file.
//
//   fz_bench <fixtureDir> <op> <zones> <raster> [--den <raster>] [--backend serial|openmp]
//            [--threads T] [--reps R] [--band-rows B] [--out results.csv]
//
// One untimed warm-up repetition precedes the R measured ones. Reading the fixture, preparing the
// denominator of the ratio workload and the checksum are outside the timed region; rasterisation
// for the reduction ops is done once before timing.
#include <fastzonal/fastzonal.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fixture_io.hpp"

namespace {

struct Args {
    std::string fixture, op, zones, raster, den, backend = "openmp", out = "results.csv";
    int threads = 0, reps = 5, band_rows = 64;
};

int usage() {
    std::cerr << "usage: fz_bench <fixtureDir> <op> <zones> <raster> [--den <raster>] [--backend serial|openmp]\n"
                 "                [--threads T] [--reps R] [--band-rows B] [--out results.csv]\n"
                 "ops: rasterize, mean, majority, ratio\n";
    return 1;
}

bool parse(int argc, char** argv, Args& a) {
    if (argc < 5) return false;
    a.fixture = argv[1];
    a.op = argv[2];
    a.zones = argv[3];
    a.raster = argv[4];
    a.den = a.raster;
    for (int i = 5; i < argc; ++i) {
        const std::string k = argv[i];
        if (i + 1 >= argc) return false;
        const std::string v = argv[++i];
        if (k == "--den") a.den = v;
        else if (k == "--backend") a.backend = v;
        else if (k == "--threads") a.threads = std::atoi(v.c_str());
        else if (k == "--reps") a.reps = std::atoi(v.c_str());
        else if (k == "--band-rows") a.band_rows = std::atoi(v.c_str());
        else if (k == "--out") a.out = v;
        else return false;
    }
    return a.backend == "serial" || a.backend == "openmp";
}

double mask_checksum(const std::vector<int32_t>& mask) {
    long long assigned = 0, sum = 0;
    for (int32_t v : mask)
        if (v >= 0) {
            ++assigned;
            sum += v;
        }
    return static_cast<double>(assigned) + static_cast<double>(sum);
}

double sum_of(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x;
    return s;
}

void check(int rc, const char* what) {
    if (rc != FZ_OK) {
        std::cerr << what << ": " << fz_error_string(rc) << "\n";
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) return usage();
    const bool serial = a.backend == "serial";

    fz_options opt;
    fz_options_default(&opt);
    opt.backend = serial ? FZ_BACKEND_SERIAL : FZ_BACKEND_OPENMP;
    opt.threads = serial ? 1 : a.threads;
    opt.raster_mode = FZ_RASTER_COMPAT;
    opt.band_rows = a.band_rows;
    int effective_threads = 1;
    if (!serial) {
        effective_threads = a.threads;
        if (effective_threads <= 0) {
#ifdef _OPENMP
            effective_threads = static_cast<int>(std::thread::hardware_concurrency());
#else
            effective_threads = 1;
#endif
        }
    }

    const fzio::Raster raster = fzio::read_raster(a.fixture, a.raster);
    const fzio::Zones zones = fzio::read_zones(a.fixture, a.zones);
    const fzio::Centroids centroids = fzio::read_centroids(a.fixture, a.zones, a.raster);
    const int32_t* crow = centroids.empty() ? nullptr : centroids.rows.data();
    const int32_t* ccol = centroids.empty() ? nullptr : centroids.cols.data();
    const fz_shape shape{raster.width, raster.height};
    const fz_transform t = raster.transform;
    const size_t n = raster.pixels32.size();
    const int32_t nz = zones.n_zones;

    std::vector<float> den_on_grid;
    if (a.op == "ratio") {
        if (a.den == a.raster) {
            den_on_grid = raster.pixels32;
        } else {
            const fzio::Raster den = fzio::read_raster(a.fixture, a.den);
            const std::vector<double> d = fzio::resample_nearest(den, raster);
            den_on_grid.resize(d.size());
            for (size_t i = 0; i < d.size(); ++i) den_on_grid[i] = static_cast<float>(d[i]);
        }
    }

    std::vector<int32_t> mask(n);
    if (a.op != "rasterize") {
        check(fz_rasterize(zones.xy.data(), zones.ring_start.data(), zones.ring_is_hole.data(), zones.ring_zone.data(),
                           zones.n_rings(), t, shape, mask.data(), &opt), "rasterize");
    }
    std::vector<fz_stats> stats(static_cast<size_t>(nz));
    std::vector<double> values(static_cast<size_t>(nz));
    std::vector<uint8_t> has(static_cast<size_t>(nz));

    const std::string fixture_id = fzio::read_fixture_meta(a.fixture).id;
    const std::string rung = serial ? "fastzonal-serial" : "fastzonal-openmp";
    std::string lines;
    for (int rep = -1; rep < a.reps; ++rep) {
        double checksum = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        if (a.op == "rasterize") {
            check(fz_rasterize(zones.xy.data(), zones.ring_start.data(), zones.ring_is_hole.data(),
                               zones.ring_zone.data(), zones.n_rings(), t, shape, mask.data(), &opt), "rasterize");
            const auto t1 = std::chrono::steady_clock::now();
            checksum = mask_checksum(mask);
            if (rep >= 0) {
                const double s = std::chrono::duration<double>(t1 - t0).count();
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", s);
                lines += fixture_id + "," + a.op + "," + rung + "," + std::to_string(effective_threads) + "," +
                         std::to_string(rep) + "," + buf + ",";
                std::snprintf(buf, sizeof buf, "%.17g", checksum);
                lines += std::string(buf) + "," + a.zones + "," + a.raster + ",," + std::to_string(nz) + "," +
                         std::to_string(n) + "\n";
            }
            continue;
        }
        if (a.op == "mean") {
            check(fz_zonal_f32(raster.pixels32.data(), mask.data(), shape, nz, -9999.0f, stats.data(), &opt), "zonal");
            check(fz_zonal_mean_fallback_f32(raster.pixels32.data(), shape, -9999.0f, stats.data(), nz, crow, ccol,
                                             values.data()), "mean");
        } else if (a.op == "majority") {
            check(fz_zonal_majority_f32(raster.pixels32.data(), mask.data(), shape, nz, -9999.0f, crow, ccol,
                                        values.data(), has.data(), &opt), "majority");
        } else if (a.op == "ratio") {
            check(fz_zonal_ratio_f32(raster.pixels32.data(), den_on_grid.data(), mask.data(), shape, nz, -9999.0f,
                                     -9999.0f, 1e-12, stats.data(), &opt), "ratio");
            // reference semantics: mean of the ratio with the centroid fallback on the ratio raster
            for (int32_t z = 0; z < nz; ++z) {
                if (stats[z].count > 0) {
                    values[z] = fz_stats_mean(&stats[z]);
                    continue;
                }
                values[z] = 0.0;
                if (crow == nullptr) continue;
                const int32_t r = crow[z], c = ccol[z];
                if (r < 0 || r >= shape.height || c < 0 || c >= shape.width) continue;
                const size_t i = static_cast<size_t>(r) * shape.width + c;
                const float x = raster.pixels32[i], y = den_on_grid[i];
                if (std::isnan(x) || x == -9999.0f || std::isnan(y) || y == -9999.0f || std::fabs(y) < 1e-12) continue;
                values[z] = static_cast<double>(x) / y;
            }
        } else {
            return usage();
        }
        const auto t1 = std::chrono::steady_clock::now();
        checksum = sum_of(values);
        if (rep < 0) continue;
        const double s = std::chrono::duration<double>(t1 - t0).count();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", s);
        lines += fixture_id + "," + a.op + "," + rung + "," + std::to_string(effective_threads) + "," +
                 std::to_string(rep) + "," + buf + ",";
        std::snprintf(buf, sizeof buf, "%.17g", checksum);
        lines += std::string(buf) + "," + a.zones + "," + a.raster + "," + (a.op == "ratio" ? a.den : "") + "," +
                 std::to_string(nz) + "," + std::to_string(n) + "\n";
    }

    const bool exists = std::ifstream(a.out).good();
    std::ofstream out(a.out, std::ios::app);
    if (!exists) out << "fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells\n";
    out << lines;
    std::cout << lines;
    return 0;
}
