include(FetchContent)

# https://json.nlohmann.me/integration/cmake/#fetchcontent
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.10.5/json.tar.xz
)
FetchContent_MakeAvailable(json)
include_directories(${json_SOURCE_DIR}/include)
