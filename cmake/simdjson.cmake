include(FetchContent)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson
    GIT_TAG v3.0.0
)
FetchContent_MakeAvailable(simdjson)
include_directories(${simdjson_SOURCE_DIR}/include)
