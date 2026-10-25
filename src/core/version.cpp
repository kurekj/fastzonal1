#include <fastzonal/fastzonal.h>

extern "C" {

const char* fz_version(void) { return FASTZONAL_VERSION_STRING; }

int fz_has_openmp(void) {
#ifdef _OPENMP
    return 1;
#else
    return 0;
#endif
}

int fz_has_cuda(void) {
#ifdef FASTZONAL_WITH_CUDA
    return 1;
#else
    return 0;
#endif
}

}  // extern "C"
