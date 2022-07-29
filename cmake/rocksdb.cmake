# RocksDB:
# https://github.com/facebook/rocksdb/blob/main/CMakeLists.txt

FetchContent_Declare(
    rocksdb
    GIT_REPOSITORY https://github.com/facebook/rocksdb.git
    GIT_TAG v7.3.1
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(rocksdb)

if(NOT rocksdb_POPULATED)
    # Fetch the content using previously declared details
    set(WITH_LIBURING OFF CACHE INTERNAL "")
    set(FAIL_ON_WARNINGS OFF CACHE INTERNAL "")
    set(WITH_BENCHMARK_TOOLS OFF CACHE INTERNAL "")
    set(WITH_SNAPPY OFF CACHE INTERNAL "")
    set(WITH_LZ4 OFF CACHE INTERNAL "")
    set(WITH_GFLAGS OFF CACHE INTERNAL "")
    set(WITH_JEMALLOC OFF CACHE INTERNAL "")
    set(USE_RTTI 1 CACHE INTERNAL "")
    set(PORTABLE ON CACHE INTERNAL "")

    set(FORCE_SSE42 ON CACHE INTERNAL "")
    set(BUILD_SHARED OFF CACHE INTERNAL "")
    set(WITH_TESTS OFF CACHE INTERNAL "")
    set(WITH_TOOLS OFF CACHE INTERNAL "")
    set(CMAKE_ENABLE_SHARED OFF CACHE INTERNAL "")
    FetchContent_Populate(rocksdb)
    add_subdirectory(${rocksdb_SOURCE_DIR} ${rocksdb_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

include_directories(${rocksdb_SOURCE_DIR}/include)
