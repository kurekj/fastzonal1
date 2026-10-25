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
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "fixture_io.hpp"

namespace {

struct Args {
    std::string fixture, op, zones, raster, den, backend = "openmp", out = "results.csv";
    int threads = 0, reps = 5, band_rows = 16;
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
    const fzio::Raster raster = fzio::read_raster(a.fixture, a.raster);
    const fzio::Zones zones = fzio::read_zones(a.fixture, a.zones);
    const fzio::Centroids centroids = fzio::read_centroids(a.fixture, a.zones, a.raster, zones.n_zones);
    if (a.op != "rasterize" && centroids.empty()) {
        std::cerr << "no centroid file for " << a.zones << " on " << a.raster << "; the reference fallback needs it\n";
        return 2;
    }
    const int32_t* crow = centroids.empty() ? nullptr : centroids.rows.data();
    const int32_t* ccol = centroids.empty() ? nullptr : centroids.cols.data();
    // requested threads go to the CSV as `threads`; the workers the library can use (never more
    // than bands) as `threads_eff`
    int requested_threads = 1;
    int effective_threads = 1;
    if (!serial) {
        requested_threads = a.threads;
        if (requested_threads <= 0) {
#ifdef _OPENMP
            requested_threads = omp_get_max_threads();
#else
            requested_threads = 1;
#endif
        }
        const int bands = (raster.height + a.band_rows - 1) / a.band_rows;
        effective_threads = requested_threads < bands ? requested_threads : bands;
    }
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
    const std::string fixture_id = fzio::read_fixture_meta(a.fixture).id;
    const std::string rung = serial ? "fastzonal-serial" : "fastzonal-openmp";
    const std::string tail = "," + a.zones + "," + a.raster + "," + (a.op == "ratio" ? a.den : "") + "," +
                             std::to_string(nz) + "," + std::to_string(n) + "," + std::to_string(effective_threads) +
                             "," + std::to_string(a.band_rows) + "\n";
    std::string lines;
    for (int rep = -1; rep < a.reps; ++rep) {
        double checksum = 0.0;
        // output buffers are allocated inside the timed region, as the managed rungs allocate theirs
        std::vector<fz_stats> stats;
        std::vector<double> values;
        std::vector<uint8_t> has;
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
                lines += fixture_id + "," + a.op + "," + rung + "," + std::to_string(requested_threads) + "," +
                         std::to_string(rep) + "," + buf + ",";
                std::snprintf(buf, sizeof buf, "%.17g", checksum);
                lines += std::string(buf) + tail;
            }
            continue;
        }
        stats.resize(static_cast<size_t>(nz));
        values.resize(static_cast<size_t>(nz));
        has.resize(static_cast<size_t>(nz));
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
                const double x = raster.pixels32[i], y = den_on_grid[i];
                if (std::isnan(x) || x == -9999.0 || std::isnan(y) || y == -9999.0 || std::fabs(y) < 1e-12) continue;
                const double q = x / y;
                if (std::isnan(q) || q == -9999.0) continue;  // the reference filters the computed ratio too
                values[z] = q;
            }
        } else {
            return usage();
        }
        const auto t1 = std::chrono::steady_clock::now();
        checksum = sum_of(values);
        if (rep < 0) continue;
        if (!std::isfinite(checksum)) {
            std::cerr << "non-finite checksum for " << a.op << "\n";
            return 2;
        }
        const double s = std::chrono::duration<double>(t1 - t0).count();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", s);
        lines += fixture_id + "," + a.op + "," + rung + "," + std::to_string(requested_threads) + "," +
                 std::to_string(rep) + "," + buf + ",";
        std::snprintf(buf, sizeof buf, "%.17g", checksum);
        lines += std::string(buf) + tail;
    }

    bool has_header = false;
    {
        std::ifstream probe(a.out, std::ios::binary | std::ios::ate);
        has_header = probe.good() && probe.tellg() > 0;
    }
    std::ofstream out(a.out, std::ios::app);
    if (!has_header)
        out << "fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells,threads_eff,band_rows\n";
    out << lines;
    std::cout << lines;
    return 0;
}
