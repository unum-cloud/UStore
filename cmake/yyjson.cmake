include(FetchContent)
FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.6.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(yyjson)
set_property(TARGET yyjson PROPERTY POSITION_INDEPENDENT_CODE ON)
include_directories(${yyjson_SOURCE_DIR}/include)
