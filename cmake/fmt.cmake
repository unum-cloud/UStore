# The pin for {fmt}, which the headers format with; its objects land inside the project's own libraries, so it
# always builds from this source.
# {fmt} turns itself into a C++20 module under Ninja with recent Clang or MSVC, which no unit here imports.
include(FetchContent)
set(FMT_MODULE OFF)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 12.2.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL SYSTEM
)
FetchContent_MakeAvailable(fmt)
