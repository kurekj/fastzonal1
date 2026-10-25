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
)
target_include_directories(fastzonal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(fastzonal
    PRIVATE FASTZONAL_BUILDING FASTZONAL_VERSION_STRING="${PROJECT_VERSION}"
)
if(MSVC)
    target_compile_options(fastzonal PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(fastzonal PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
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

if(FASTZONAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
