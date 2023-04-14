include(FetchContent)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 9.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)
set(fmt_INCLUDE_DIR ${fmt_SOURCE_DIR}/include)
include_directories(${fmt_INCLUDE_DIR})
