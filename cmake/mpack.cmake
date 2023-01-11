include(FetchContent)
FetchContent_Declare(
    mpack
    GIT_REPOSITORY https://github.com/unum-cloud/mpack.git
    GIT_TAG origin/develop
)
FetchContent_MakeAvailable(mpack)
include_directories(${mpack_SOURCE_DIR}/src/mpack)