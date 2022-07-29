# Installation scripts can be take from here:
# https://arrow.apache.org/install/
if(${UKV_PREINSTALLED_ARROW})
    find_package(Arrow REQUIRED)

else()
    include(ExternalProject)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    ExternalProject_Add(
        Arrow-external
        GIT_REPOSITORY https://github.com/apache/arrow.git
        GIT_TAG "apache-arrow-8.0.1"
        GIT_SHALLOW TRUE

        DOWNLOAD_DIR "_deps/arrow-src"
        LOG_DIR "_deps/arrow-log"
        STAMP_DIR "_deps/arrow-stamp"
        TMP_DIR "_deps/arrow-tmp"
        SOURCE_DIR "_deps/arrow-src"
        BINARY_DIR "_deps/arrow-build"
        INSTALL_DIR "_deps/arrow-install"
        SOURCE_SUBDIR "cpp"

        CMAKE_ARGS ${ARROW_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX=stage
        -DCMAKE_BUILD_TYPE=Release
        -DTHREADS_PREFER_PTHREAD_FLAG=ON

        -DARROW_DEPENDENCY_SOURCE=BUNDLED
        -DARROW_BUILD_STATIC=ON
        -DARROW_BUILD_SHARED=OFF
        -DARROW_SIMD_LEVEL=AVX2
        -DARROW_DEPENDENCY_USE_SHARED=ON

        -DARROW_BUILD_TESTS=OFF
        -DARROW_ENABLE_TIMING_TESTS=OFF
        -DARROW_BUILD_EXAMPLES=OFF
        -DARROW_BUILD_BENCHMARKS=OFF
        -DARROW_BUILD_INTEGRATION=OFF
        -DARROW_EXTRA_ERROR_CONTEXT=OFF

        -DARROW_DATASET=OFF
        -DARROW_CUDA=OFF
        -DARROW_IPC=OFF
        -DARROW_COMPUTE=OFF
        -DARROW_JEMALLOC=OFF

        -DARROW_JSON=OFF
        -DARROW_CSV=OFF
        -DARROW_PYTHON=OFF
        -DARROW_PARQUET=OFF
        -DARROW_FLIGHT=OFF
        -DARROW_FLIGHT_SQL=OFF
        -DARROW_WITH_UCX=OFF
        -DARROW_WITH_RE2=OFF
        -DARROW_WITH_UTF8PROC=OFF
        -DARROW_BUILD_UTILITIES=OFF
        -DARROW_GANDIVA=OFF
    )

    ExternalProject_Get_Property(Arrow-external SOURCE_DIR)
    ExternalProject_Get_Property(Arrow-external BINARY_DIR)
    ExternalProject_Get_Property(Arrow-external INSTALL_DIR)

    set(ARROW_INCLUDE_DIR "${BINARY_DIR}/stage/include")
    set(ARROW_INCLUDE_GEN_DIR "${BINARY_DIR}/stage/src")
    include_directories(${ARROW_INCLUDE_DIR} ${ARROW_INCLUDE_GEN_DIR})
    message("ARROW_INCLUDE_DIR: " ${ARROW_INCLUDE_DIR})

    add_library(Arrow::arrow STATIC IMPORTED)
    set_property(TARGET arrow::arrow PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow.a)
    add_dependencies(arrow::arrow Arrow-external)

    add_library(Arrow::flight STATIC IMPORTED)
    set_property(TARGET arrow::flight PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_flight.a)
    add_dependencies(arrow::flight Arrow-external)

    add_library(Arrow::bundled STATIC IMPORTED)
    set_property(TARGET arrow::bundled PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_bundled_dependencies.a)
    add_dependencies(arrow::bundled Arrow-external)

    add_library(Arrow::cuda STATIC IMPORTED)
    set_property(TARGET arrow::cuda PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_cuda.a)
    add_dependencies(arrow::cuda Arrow-external)
endif()