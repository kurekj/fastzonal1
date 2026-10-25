cmake_minimum_required(VERSION 3.20)
project(fastzonal VERSION 0.1.0 LANGUAGES C CXX)

option(FASTZONAL_BUILD_TESTS "Build the Catch2 test suite" ON)
option(FASTZONAL_WITH_OPENMP "Build the OpenMP backend" ON)
option(FASTZONAL_WITH_CUDA "Build the CUDA backend" OFF)
option(FASTZONAL_NATIVE_ARCH "Compile for the host CPU (-march=native); OFF for portable binaries" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
include(GNUInstallDirs)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(fastzonal SHARED
    src/core/version.cpp
    src/core/options.cpp
    src/core/rasterize.cpp
    src/core/zonal.cpp
    src/core/dispatch.cpp
)
target_include_directories(fastzonal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(fastzonal
    PRIVATE FASTZONAL_BUILDING FASTZONAL_VERSION_STRING="${PROJECT_VERSION}"
)
# Contraction (fused multiply-add) is disabled everywhere: the determinism guarantee between
# backends and across builds requires every a*b+c to round the same way.
if(MSVC)
    target_compile_options(fastzonal PRIVATE /W4 /permissive- /fp:precise $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(fastzonal PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off $<$<CONFIG:Release>:-O3>)
    if(FASTZONAL_NATIVE_ARCH)
        target_compile_options(fastzonal PRIVATE -march=native)
    endif()
endif()

if(FASTZONAL_WITH_OPENMP)
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    target_link_libraries(fastzonal PUBLIC OpenMP::OpenMP_CXX)
endif()
if(FASTZONAL_WITH_CUDA)
    enable_language(CUDA)
    target_compile_definitions(fastzonal PRIVATE FASTZONAL_WITH_CUDA)
endif()

install(TARGETS fastzonal EXPORT fastzonalTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT fastzonalTargets NAMESPACE fastzonal:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastzonal)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/fastzonalConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fastzonalConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastzonal)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/fastzonalConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fastzonalConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/fastzonalConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastzonal)

# Fixture reader shared by the parity tests and the benchmark driver (not installed).
add_library(fz_fixture_io STATIC src/tools/fixture_io.cpp)
target_include_directories(fz_fixture_io PUBLIC src/tools)
target_link_libraries(fz_fixture_io PUBLIC fastzonal)
target_compile_features(fz_fixture_io PUBLIC cxx_std_17)

# Copies the built native library where the .NET binding and its tests look for it.
if(WIN32)
    set(FASTZONAL_DOTNET_RID "win-x64")
elseif(APPLE)
    set(FASTZONAL_DOTNET_RID "osx-x64")
else()
    set(FASTZONAL_DOTNET_RID "linux-x64")
endif()
add_custom_target(dotnet_native_copy
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/dotnet/FastZonal/runtimes/${FASTZONAL_DOTNET_RID}/native
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fastzonal>
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/dotnet/FastZonal/runtimes/${FASTZONAL_DOTNET_RID}/native/
    DEPENDS fastzonal
    COMMENT "Copying the native library into bindings/dotnet/FastZonal/runtimes/${FASTZONAL_DOTNET_RID}/native")

if(FASTZONAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
