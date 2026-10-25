/* fastzonal: zonal statistics and terrain indices over polygon units.
 *
 * C ABI. Every function returns an fz error code (FZ_OK on success) unless documented otherwise.
 *
 * Contract common to all functions:
 * - Arguments are validated before any output is written: a null pointer where one is required, a non-positive
 *   dimension, a shape whose pixel count does not fit the address space, an unknown option value, a mask value
 *   outside [-1, n_zones) or a non-finite coordinate returns FZ_ERR_ARG and leaves every output untouched.
 * - A backend that is not compiled in (or FZ_BACKEND_CUDA in this version) returns FZ_ERR_BACKEND.
 * - Memory exhaustion returns FZ_ERR_NOMEM; any other internal failure FZ_ERR_INTERNAL. After these two the
 *   contents of the output buffers are unspecified.
 * - The library never reads outside the arrays it is given, but it cannot know their sizes: the caller
 *   guarantees that every raster and mask holds width * height elements, every per-zone array n_zones
 *   elements, and every ring offset addresses points inside xy.
 * - A NULL fz_options pointer means the defaults of fz_options_default. */
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
/* Population standard deviation from sum and sumsq, or 0.0 when count < 2. Computed from the raw moments, so
 * values with a large common offset relative to their spread lose precision (use the mean for parity work). A
 * non-finite variance yields 0.0. */
FZ_API double fz_stats_std(const fz_stats* s);

/* ---- rasterisation ------------------------------------------------------------------- */

/* Rasterises polygons into a zone-index mask.
 *
 * xy:           n_points * 2 doubles (x0, y0, x1, y1, ...).
 * ring_start:   n_rings + 1 offsets into the points; ring i is the points [ring_start[i], ring_start[i+1]).
 *               Rings need not repeat their first point at the end.
 * ring_is_hole: 1 when ring i is a hole of the nearest preceding exterior ring, else 0.
 * ring_zone:    zone index (0 .. n_zones - 1) of the polygon the ring belongs to; a hole carries the zone of its
 *               exterior ring.
 * mask_out:     s.height * s.width int32 values, row-major, fully written on success: the zone of the polygon
 *               covering the pixel, or -1 when no polygon covers it. Polygons are processed in ring order and
 *               later polygons overwrite earlier ones. n_rings == 0 writes -1 everywhere.
 * Rejected with FZ_ERR_ARG: a hole before any exterior ring, a hole whose zone differs from its exterior ring, a
 *               negative zone, non-finite coordinates in any ring (holes included, in both modes), ring offsets
 *               that are not non-decreasing, a non-finite or non-positive pixel size. Rings with fewer than
 *               three points cover nothing. Coordinates beyond about 1e12 pixels from the origin are clamped
 *               and therefore cover nothing.
 *
 * FZ_RASTER_CENTER: a pixel belongs to a polygon when its centre is inside by the even-odd rule, holes honoured.
 * FZ_RASTER_COMPAT: the SpongeGIS reference rule: exterior rings only, centre test on rows, and on each row every
 *               column from the one containing the left crossing to the one containing the right crossing. */
FZ_API int fz_rasterize(const double* xy, const int64_t* ring_start, const uint8_t* ring_is_hole,
                        const int32_t* ring_zone, int64_t n_rings, fz_transform t, fz_shape s,
                        int32_t* mask_out, const fz_options* opt);

/* ---- zonal statistics ---------------------------------------------------------------- */

/* Accumulates count, sum, sum of squares, min and max of the raster per zone.
 * raster: s.height * s.width values, row-major. mask: zone index per pixel, -1 = outside; any other value
 * outside [-1, n_zones) makes the call fail with FZ_ERR_ARG before out is touched.
 * Cells equal to nodata, and NaN cells, are skipped. out: n_zones entries, fully written; a zone with no cell
 * has count 0 and min = max = 0. Accumulation is in double for every raster type. */
FZ_API int fz_zonal_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones, float nodata,
                        fz_stats* out, const fz_options* opt);
FZ_API int fz_zonal_f64(const double* raster, const int32_t* mask, fz_shape s, int32_t n_zones, double nodata,
                        fz_stats* out, const fz_options* opt);

/* Mean per zone with the reference fallback: sum / count when count > 0; otherwise the raster value at
 * (centroid_row[z], centroid_col[z]) when that cell is inside the raster and not NoData; otherwise 0.0.
 * centroid_row / centroid_col may both be NULL, in which case empty zones get 0.0. */
FZ_API int fz_zonal_mean_fallback_f32(const float* raster, fz_shape s, float nodata, const fz_stats* stats,
                                      int32_t n_zones, const int32_t* centroid_row, const int32_t* centroid_col,
                                      double* mean_out);
FZ_API int fz_zonal_mean_fallback_f64(const double* raster, fz_shape s, double nodata, const fz_stats* stats,
                                      int32_t n_zones, const int32_t* centroid_row, const int32_t* centroid_col,
                                      double* mean_out);

/* Most frequent value per zone (ties resolve to the smallest value). NoData and NaN cells are skipped.
 * A zone without cells takes the value at its centroid cell when centroid arrays are given and the cell is
 * inside the raster and not NoData; otherwise value_out[z] = 0 and has_value_out[z] = 0. */
FZ_API int fz_zonal_majority_f32(const float* raster, const int32_t* mask, fz_shape s, int32_t n_zones, float nodata,
                                 const int32_t* centroid_row, const int32_t* centroid_col, double* value_out,
                                 uint8_t* has_value_out, const fz_options* opt);
FZ_API int fz_zonal_majority_f64(const double* raster, const int32_t* mask, fz_shape s, int32_t n_zones,
                                 double nodata, const int32_t* centroid_row, const int32_t* centroid_col,
                                 double* value_out, uint8_t* has_value_out, const fz_options* opt);
FZ_API int fz_zonal_majority_i32(const int32_t* raster, const int32_t* mask, fz_shape s, int32_t n_zones,
                                 int32_t nodata, const int32_t* centroid_row, const int32_t* centroid_col,
                                 double* value_out, uint8_t* has_value_out, const fz_options* opt);

/* Statistics of the cell-wise ratio num / den of two rasters on the same grid. Cells where either raster is
 * NoData or NaN, or where |den| < eps, are excluded. out as in fz_zonal_*. */
FZ_API int fz_zonal_ratio_f32(const float* num, const float* den, const int32_t* mask, fz_shape s, int32_t n_zones,
                              float nodata_num, float nodata_den, double eps, fz_stats* out, const fz_options* opt);
FZ_API int fz_zonal_ratio_f64(const double* num, const double* den, const int32_t* mask, fz_shape s,
                              int32_t n_zones, double nodata_num, double nodata_den, double eps, fz_stats* out,
                              const fz_options* opt);

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
