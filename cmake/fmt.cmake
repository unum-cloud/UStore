include(FetchContent)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 8.1.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)
set(fmt_INCLUDE_DIR ${fmt_SOURCE_DIR}/include)
include_directories(${fmt_INCLUDE_DIR})
