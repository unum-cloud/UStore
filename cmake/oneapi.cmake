
if(${UKV_REBUILD_ONEAPI})
    include(FetchContent)
    FetchContent_Declare(
        TBB
        GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
        GIT_TAG v2021.7.0
    )

    FetchContent_GetProperties(TBB)

    if(NOT TBB_POPULATED)
        set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
        FetchContent_Populate(TBB)
        add_subdirectory(${TBB_SOURCE_DIR} ${TBB_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

    include_directories(${TBB_SOURCE_DIR}/include)

else()
    find_package(TBB REQUIRED)
    set(TBB_LIBRARIES tbb)
    include_directories("/opt/intel/oneapi/tbb/latest/")
    link_directories("/opt/intel/oneapi/tbb/latest/lib/intel64/gcc4.8/")
endif()