include(FetchContent)

# https://json.nlohmann.me/integration/cmake/#fetchcontent
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)
include_directories(${json_SOURCE_DIR}/include)
