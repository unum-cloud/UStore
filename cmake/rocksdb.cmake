# RocksDB: https://github.com/facebook/rocksdb/blob/main/CMakeLists.txt

include(FetchContent)
FetchContent_Declare(
    rocksdb
    GIT_REPOSITORY https://github.com/facebook/rocksdb
    GIT_TAG v11.8.1
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(rocksdb)

if (NOT rocksdb_POPULATED)
    # Fetch the content using previously declared details
    set(WITH_LIBURING
        OFF
        CACHE INTERNAL ""
    )
    set(FAIL_ON_WARNINGS
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_BENCHMARK_TOOLS
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_SNAPPY
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_LZ4
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_GFLAGS
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_JEMALLOC
        OFF
        CACHE INTERNAL ""
    )
    set(USE_RTTI
        1
        CACHE INTERNAL ""
    )
    # The baseline instruction set of each architecture, so artifacts run on any CPU of it.
    set(PORTABLE
        ON
        CACHE INTERNAL ""
    )

    set(ROCKSDB_BUILD_SHARED
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_TESTS
        OFF
        CACHE INTERNAL ""
    )
    set(WITH_TOOLS
        OFF
        CACHE INTERNAL ""
    )
    FetchContent_Populate(rocksdb)
    add_subdirectory(${rocksdb_SOURCE_DIR} ${rocksdb_BINARY_DIR} EXCLUDE_FROM_ALL)
    add_library(rocksdb::rocksdb ALIAS rocksdb)
    # RocksDB loads fixed-width integers from unaligned addresses on purpose, and exempts those loads from the
    # undefined-behavior sanitizer only when told a sanitized build is running
    target_compile_definitions(rocksdb PRIVATE ROCKSDB_UBSAN_RUN)
endif ()

include_directories(${rocksdb_SOURCE_DIR}/include)
