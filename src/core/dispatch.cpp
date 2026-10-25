#include "dispatch.hpp"

#include <thread>

namespace fz {

int resolve_plan(const fz_options* opt, fz_shape s, Plan& plan) {
    int backend = opt->backend;
    if (backend == FZ_BACKEND_CUDA) return FZ_ERR_BACKEND;
    if (backend == FZ_BACKEND_AUTO) {
#ifdef _OPENMP
        backend = (opt->threads == 1) ? FZ_BACKEND_SERIAL : FZ_BACKEND_OPENMP;
#else
        backend = FZ_BACKEND_SERIAL;
#endif
    }
#ifndef _OPENMP
    if (backend == FZ_BACKEND_OPENMP) return FZ_ERR_BACKEND;
#endif
    int threads = opt->threads;
    if (backend == FZ_BACKEND_SERIAL) {
        threads = 1;
    } else if (threads == 0) {
#ifdef _OPENMP
        threads = omp_get_max_threads();
#else
        threads = static_cast<int>(std::thread::hardware_concurrency());
#endif
        if (threads < 1) threads = 1;
    }
    const int64_t n_bands = (static_cast<int64_t>(s.height) + opt->band_rows - 1) / opt->band_rows;
    // More workers than bands would only cost scratch memory (one dense per-zone array each).
    if (threads > n_bands) threads = static_cast<int>(n_bands);
    plan.backend = backend;
    plan.threads = threads;
    plan.band_rows = opt->band_rows;
    plan.n_bands = static_cast<int32_t>(n_bands);
    return FZ_OK;
}

}  // namespace fz
