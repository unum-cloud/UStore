# Installation scripts can be take from here:
# https://arrow.apache.org/install/
include(ExternalProject)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(ARROW_DEPENDENCY Arrow-external)
ExternalProject_Add(
    Arrow-external
    GIT_REPOSITORY https://github.com/apache/arrow.git
    GIT_TAG "apache-arrow-10.0.1"
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

    -DARROW_DEPENDENCY_SOURCE=AUTO
    -DARROW_BUILD_STATIC=ON
    -DARROW_BUILD_SHARED=OFF
    -DARROW_SIMD_LEVEL=AVX2
    -DARROW_DEPENDENCY_USE_SHARED=OFF

    -DARROW_BUILD_TESTS=OFF
    -DARROW_ENABLE_TIMING_TESTS=OFF
    -DARROW_BUILD_EXAMPLES=OFF
    -DARROW_BUILD_BENCHMARKS=OFF
    -DARROW_BUILD_INTEGRATION=OFF
    -DARROW_EXTRA_ERROR_CONTEXT=OFF

    -DARROW_DATASET=ON
    -DARROW_PARQUET=ON
    -DARROW_WITH_RE2=ON
    -DARROW_COMPUTE=ON
    -DARROW_FLIGHT=ON
    -DARROW_WITH_UTF8PROC=ON

    -DPARQUET_REQUIRE_ENCRYPTION=OFF
    -DARROW_CUDA=OFF
    -DARROW_JEMALLOC=OFF
    -DARROW_IPC=OFF
    -DARROW_JSON=OFF
    -DARROW_CSV=OFF
    -DARROW_FLIGHT_SQL=OFF
    -DARROW_WITH_UCX=OFF
    -DARROW_BUILD_UTILITIES=OFF
    -DARROW_GANDIVA=OFF
    -DARROW_S3=OFF

    -DABS_VENDORED=ON

    # Which components should be bundled:
    # https://arrow.apache.org/docs/developers/cpp/building.html#build-dependency-management
    -DARROW_DEPENDENCY_SOURCE=BUNDLED
    -Dc-ares_SOURCE=BUNDLED
    -Dre2_SOURCE=BUNDLED
    -Dabsl_SOURCE=BUNDLED
    -DProtobuf_SOURCE=BUNDLED
    -DgRPC_SOURCE=BUNDLED
    -DZLIB_SOURCE=BUNDLED
    -DThrift_SOURCE=BUNDLED
    -Dutf8proc_SOURCE=BUNDLED
)

ExternalProject_Get_Property(Arrow-external SOURCE_DIR)
ExternalProject_Get_Property(Arrow-external BINARY_DIR)
ExternalProject_Get_Property(Arrow-external INSTALL_DIR)

set(ARROW_INCLUDE_DIR "${BINARY_DIR}/stage/include")
set(ARROW_INCLUDE_GEN_DIR "${BINARY_DIR}/stage/src")
include_directories(${ARROW_INCLUDE_DIR} ${ARROW_INCLUDE_GEN_DIR})
message("ARROW_INCLUDE_DIR: " ${ARROW_INCLUDE_DIR})

add_library(arrow::arrow STATIC IMPORTED)
set_property(TARGET arrow::arrow PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow.a)
add_dependencies(arrow::arrow Arrow-external)

add_library(arrow::flight STATIC IMPORTED)
set_property(TARGET arrow::flight PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_flight.a)
add_dependencies(arrow::flight Arrow-external)

add_library(arrow::cuda STATIC IMPORTED)
set_property(TARGET arrow::cuda PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_cuda.a)
add_dependencies(arrow::cuda Arrow-external)

add_library(arrow::dataset STATIC IMPORTED)
set_property(TARGET arrow::dataset PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_dataset.a)
add_dependencies(arrow::dataset Arrow-external)

add_library(arrow::parquet STATIC IMPORTED)
set_property(TARGET arrow::parquet PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libparquet.a)
add_dependencies(arrow::parquet Arrow-external)

add_library(arrow::bundled STATIC IMPORTED)
set_property(TARGET arrow::bundled PROPERTY IMPORTED_LOCATION ${BINARY_DIR}/release/libarrow_bundled_dependencies.a)
add_dependencies(arrow::bundled Arrow-external)
