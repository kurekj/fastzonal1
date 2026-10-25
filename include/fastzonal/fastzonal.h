/* fastzonal: zonal statistics and terrain indices over polygon units.
 * C ABI. Every function returns an fz error code (FZ_OK on success) unless documented otherwise.
 */
#ifndef FASTZONAL_H
#define FASTZONAL_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(FASTZONAL_BUILDING)
#    define FZ_API __declspec(dllexport)
#  else
#    define FZ_API __declspec(dllimport)
#  endif
#else
#  define FZ_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- common types -------------------------------------------------------------------- */

/* North-up affine grid. Pixel (row, col) has its centre at
 *   x = origin_x + (col + 0.5) * pixel_w
 *   y = origin_y - (row + 0.5) * pixel_h
 * pixel_w and pixel_h are positive. */
typedef struct fz_transform {
    double origin_x;
    double origin_y;
    double pixel_w;
    double pixel_h;
} fz_transform;

typedef struct fz_shape {
    int32_t width;
    int32_t height;
} fz_shape;

typedef enum fz_backend {
    FZ_BACKEND_AUTO = 0,
    FZ_BACKEND_SERIAL = 1,
    FZ_BACKEND_OPENMP = 2,
    FZ_BACKEND_CUDA = 3
} fz_backend;

typedef enum fz_raster_mode {
    FZ_RASTER_CENTER = 0, /* pixel-centre rule on rows and columns, holes honoured */
    FZ_RASTER_COMPAT = 1  /* reproduces the SpongeGIS reference rule, exterior rings only */
} fz_raster_mode;

typedef struct fz_options {
    int backend;     /* fz_backend; AUTO picks OpenMP when built with it, else serial */
    int threads;     /* 0 = all hardware threads */
    int device;      /* CUDA device index */
    int raster_mode; /* fz_raster_mode */
    int band_rows;   /* rows per band; the band split fixes the summation order */
} fz_options;

/* Accumulated statistics of one zone. mean and std are derived with fz_stats_mean / fz_stats_std. */
typedef struct fz_stats {
    int64_t count;
    double sum;
    double sumsq;
    double min;
    double max;
} fz_stats;

enum {
    FZ_OK = 0,
    FZ_ERR_ARG = 1,
    FZ_ERR_BACKEND = 2,
    FZ_ERR_NOMEM = 3,
    FZ_ERR_INTERNAL = 4
};

/* Fills opt with the defaults: AUTO backend, all threads, device 0, FZ_RASTER_CENTER, 64 rows per band. */
FZ_API void fz_options_default(fz_options* opt);
/* Static description of an error code; "unknown error" for codes the library does not define. */
FZ_API const char* fz_error_string(int code);
/* sum / count, or 0.0 when count is 0. */
FZ_API double fz_stats_mean(const fz_stats* s);
/* Population standard deviation from sum and sumsq, or 0.0 when count < 2. */
FZ_API double fz_stats_std(const fz_stats* s);

/* ---- library information ------------------------------------------------------------- */

/* Library version, e.g. "0.1.0". */
FZ_API const char* fz_version(void);
/* 1 when the library was built with OpenMP, else 0. */
FZ_API int fz_has_openmp(void);
/* 1 when the library was built with CUDA support, else 0. */
FZ_API int fz_has_cuda(void);

#ifdef __cplusplus
}
#endif

#endif /* FASTZONAL_H */
