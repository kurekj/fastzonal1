// Argument checks shared by every entry point of the C ABI.
#pragma once

#include <fastzonal/fastzonal.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace fz {

// Backend in [AUTO, CUDA], threads >= 0, band_rows > 0, a known raster mode.
inline int validate_options(const fz_options* opt) {
    if (opt == nullptr) return FZ_ERR_ARG;
    if (opt->backend < FZ_BACKEND_AUTO || opt->backend > FZ_BACKEND_CUDA) return FZ_ERR_ARG;
    if (opt->threads < 0 || opt->band_rows <= 0 || opt->device < 0) return FZ_ERR_ARG;
    if (opt->raster_mode != FZ_RASTER_CENTER && opt->raster_mode != FZ_RASTER_COMPAT) return FZ_ERR_ARG;
    return FZ_OK;
}

// Positive dimensions whose pixel count fits the address space of this build (int32 and double
// buffers of that many elements must be representable).
inline int validate_shape(fz_shape s) {
    if (s.width <= 0 || s.height <= 0) return FZ_ERR_ARG;
    const uint64_t n = static_cast<uint64_t>(s.width) * static_cast<uint64_t>(s.height);
    if (n > std::numeric_limits<size_t>::max() / sizeof(double)) return FZ_ERR_ARG;
    return FZ_OK;
}

inline size_t pixel_count(fz_shape s) { return static_cast<size_t>(s.width) * static_cast<size_t>(s.height); }

// Positive zone count whose per-zone arrays (fz_stats, hash maps, slots) fit the address space.
inline int validate_zones(int32_t n_zones) {
    if (n_zones <= 0) return FZ_ERR_ARG;
    constexpr uint64_t kBytesPerZone = 128;  // upper bound over every per-zone structure used
    if (static_cast<uint64_t>(n_zones) > std::numeric_limits<size_t>::max() / kBytesPerZone) return FZ_ERR_ARG;
    return FZ_OK;
}

}  // namespace fz
