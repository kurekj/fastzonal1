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
