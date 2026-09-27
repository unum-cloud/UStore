include(FetchContent)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 12.2.0
    GIT_SHALLOW TRUE
)
# fmt turns itself into a C++20 module under Ninja and Clang 16 or newer, which UStore neither needs nor scans for.
set(FMT_MODULE OFF)
FetchContent_MakeAvailable(fmt)
set(fmt_INCLUDE_DIR ${fmt_SOURCE_DIR}/include)
include_directories(${fmt_INCLUDE_DIR})
