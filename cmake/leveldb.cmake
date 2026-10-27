include(FetchContent)

# LevelDB: https://github.com/google/leveldb/blob/main/CMakeLists.txt
FetchContent_Declare(
    leveldb
    GIT_REPOSITORY https://github.com/google/leveldb.git
    GIT_TAG 1.23
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(leveldb)

if (NOT leveldb_POPULATED)
    # Fetch the content using previously declared details
    set(LEVELDB_BUILD_TESTS
        OFF
        CACHE BOOL "Build LevelDB's unit tests"
    )
    set(LEVELDB_BUILD_BENCHMARKS
        OFF
        CACHE BOOL "Build LevelDB's benchmarks"
    )
    set(HAVE_SNAPPY
        OFF
        CACHE BOOL "Build with snappy compression library"
    )
    # Probed libraries would make the archive depend on whatever the build machine has installed.
    set(HAVE_CRC32C
        OFF
        CACHE BOOL "Build with the crc32c library"
    )
    set(HAVE_TCMALLOC
        OFF
        CACHE BOOL "Build with tcmalloc"
    )

    # Should not be set globally, but just for this target!
    FetchContent_Populate(leveldb)
    add_subdirectory(${leveldb_SOURCE_DIR} ${leveldb_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
    # LevelDB's own sources keep -Werror -Wthread-safety, which it would otherwise impose on every consumer.
    set_property(TARGET leveldb PROPERTY INTERFACE_COMPILE_OPTIONS "")
    add_library(leveldb::leveldb ALIAS leveldb)
endif ()

include_directories(SYSTEM ${leveldb_SOURCE_DIR}/include)
