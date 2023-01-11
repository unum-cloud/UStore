# Consistent-Sets are also in active development we may want to reuse the local version
if(EXISTS ${CMAKE_SOURCE_DIR}/../ucset)
    include_directories(${CMAKE_SOURCE_DIR}/../ucset/include/)
else()
    include(FetchContent)
    FetchContent_Declare(
        ucset
        GIT_REPOSITORY https://github.com/unum-cloud/ucset
        GIT_TAG main
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
    )
    FetchContent_MakeAvailable(ucset)
    include_directories(${ucset_SOURCE_DIR}/include/)
endif()
