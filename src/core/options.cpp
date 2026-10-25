#include <fastzonal/fastzonal.h>

#include <cmath>

extern "C" {

void fz_options_default(fz_options* opt) {
    if (opt == nullptr) return;
    opt->backend = FZ_BACKEND_AUTO;
    opt->threads = 0;
    opt->device = 0;
    opt->raster_mode = FZ_RASTER_CENTER;
    opt->band_rows = 64;
}

const char* fz_error_string(int code) {
    switch (code) {
        case FZ_OK: return "ok";
        case FZ_ERR_ARG: return "invalid argument";
        case FZ_ERR_BACKEND: return "backend not available";
        case FZ_ERR_NOMEM: return "out of memory";
        case FZ_ERR_INTERNAL: return "internal error";
        default: return "unknown error";
    }
}

double fz_stats_mean(const fz_stats* s) {
    if (s == nullptr || s->count <= 0) return 0.0;
    return s->sum / static_cast<double>(s->count);
}

double fz_stats_std(const fz_stats* s) {
    if (s == nullptr || s->count < 2) return 0.0;
    const double n = static_cast<double>(s->count);
    const double mean = s->sum / n;
    const double variance = s->sumsq / n - mean * mean;
    // Rounding can push a zero variance slightly negative; clamp before the square root.
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

}  // extern "C"
