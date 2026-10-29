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
    # RocksDB keeps both paths in one build: io_uring where a ring can be created, `pread` where the kernel or a seccomp
    # profile refuses one.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include("${CMAKE_CURRENT_LIST_DIR}/liburing.cmake")
        set(WITH_LIBURING
            ON
            CACHE INTERNAL ""
        )
    else ()
        set(WITH_LIBURING
            OFF
            CACHE INTERNAL ""
        )
    endif ()
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
    # RocksDB moves a default install prefix to `/usr` on Linux, which every install rule of this project would follow
    set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OFF)
    FetchContent_Populate(rocksdb)
    add_subdirectory(${rocksdb_SOURCE_DIR} ${rocksdb_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
    add_library(rocksdb::rocksdb ALIAS rocksdb)
    # RocksDB loads fixed-width integers from unaligned addresses on purpose, and exempts those loads from the
    # undefined-behavior sanitizer only when told a sanitized build is running
    target_compile_definitions(rocksdb PRIVATE ROCKSDB_UBSAN_RUN)
    # RocksDB installs its static archive through an export set, which must also carry the archives it links
    if (TARGET uring)
        install(
            TARGETS uring
            EXPORT RocksDBTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif ()
endif ()

include_directories(SYSTEM ${rocksdb_SOURCE_DIR}/include)
