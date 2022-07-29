# LevelDB:
# https://github.com/google/leveldb/blob/main/CMakeLists.txt

FetchContent_Declare(
    leveldb
    GIT_REPOSITORY https://github.com/google/leveldb.git
    GIT_TAG 1.23
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(leveldb)

if(NOT leveldb_POPULATED)
    # Fetch the content using previously declared details
    set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "Build LevelDB's unit tests")
    set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "Build LevelDB's benchmarks")
    set(HAVE_SNAPPY OFF CACHE BOOL "Build with snappy compression library")
    set(RTTI ON CACHE BOOL "Build with RTTI")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

    FetchContent_Populate(leveldb)
    add_subdirectory(${leveldb_SOURCE_DIR} ${leveldb_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

include_directories(${leveldb_SOURCE_DIR}/include)
