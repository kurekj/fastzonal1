// Band scheduling shared by the serial and OpenMP backends.
//
// Every reduction splits the raster into horizontal bands of band_rows rows. Bands are computed
// independently (on one thread in the serial backend, on a thread pool with OpenMP) and their
// partial results are merged in ascending band order on the calling thread. Because the band
// split depends only on band_rows and the merge order is fixed, serial and OpenMP results are
// bit-identical for any thread count.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstdint>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace fz {

struct Plan {
    int backend;      // FZ_BACKEND_SERIAL or FZ_BACKEND_OPENMP after resolution
    int threads;      // >= 1
    int band_rows;    // >= 1
    int32_t n_bands;  // ceil(height / band_rows)
};

// Resolves AUTO and the thread count. Returns FZ_ERR_BACKEND for backends that are not built in.
int resolve_plan(const fz_options* opt, fz_shape s, Plan& plan);

inline int32_t band_begin(const Plan& plan, int32_t band) { return band * plan.band_rows; }
inline int32_t band_end(const Plan& plan, fz_shape s, int32_t band) {
    const int64_t e = static_cast<int64_t>(band + 1) * plan.band_rows;
    return e < s.height ? static_cast<int32_t>(e) : s.height;
}

// Runs fn(band, row0, row1) for every band. With the OpenMP backend the bands run on
// plan.threads threads with dynamic scheduling; fn must only write to band-private state.
template <class Fn>
void for_each_band(const Plan& plan, fz_shape s, Fn&& fn) {
#ifdef _OPENMP
    if (plan.backend == FZ_BACKEND_OPENMP && plan.threads > 1) {
#pragma omp parallel for schedule(dynamic, 1) num_threads(plan.threads)
        for (int32_t b = 0; b < plan.n_bands; ++b) fn(b, band_begin(plan, b), band_end(plan, s, b));
        return;
    }
#endif
    for (int32_t b = 0; b < plan.n_bands; ++b) fn(b, band_begin(plan, b), band_end(plan, s, b));
}

// Runs init() once per worker thread to create thread-private scratch state, then
// fn(scratch, band, row0, row1) for every band. The serial backend has one worker.
template <class Scratch, class Init, class Fn>
void for_each_band_with_scratch(const Plan& plan, fz_shape s, Init&& init, Fn&& fn) {
#ifdef _OPENMP
    if (plan.backend == FZ_BACKEND_OPENMP && plan.threads > 1) {
#pragma omp parallel num_threads(plan.threads)
        {
            Scratch scratch = init();
#pragma omp for schedule(dynamic, 1)
            for (int32_t b = 0; b < plan.n_bands; ++b) fn(scratch, b, band_begin(plan, b), band_end(plan, s, b));
        }
        return;
    }
#endif
    Scratch scratch = init();
    for (int32_t b = 0; b < plan.n_bands; ++b) fn(scratch, b, band_begin(plan, b), band_end(plan, s, b));
}

}  // namespace fz
