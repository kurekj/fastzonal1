include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(fz_tests
    test_version.cpp
    test_options.cpp
    test_rasterize.cpp
    test_zonal.cpp
    test_parallel.cpp
)
target_link_libraries(fz_tests PRIVATE fastzonal Catch2::Catch2WithMain)
target_compile_features(fz_tests PRIVATE cxx_std_17)
if(FASTZONAL_WITH_CUDA)
    target_compile_definitions(fz_tests PRIVATE FASTZONAL_TEST_EXPECT_CUDA=1)
endif()
if(WIN32)
    # Test discovery runs the executable right after linking; the DLL must sit beside it.
    add_custom_command(TARGET fz_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fastzonal> $<TARGET_FILE_DIR:fz_tests>)
endif()
if(FASTZONAL_WITH_OPENMP AND OpenMP_CXX_FOUND)
    target_link_libraries(fz_tests PRIVATE OpenMP::OpenMP_CXX)
endif()

catch_discover_tests(fz_tests)
