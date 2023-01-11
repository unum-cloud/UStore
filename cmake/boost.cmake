

set(Boost_USE_DEBUG_LIBS OFF)
set(Boost_USE_RELEASE_LIBS ON)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)
set(Boost_USE_MULTITHREADED ON)

# Boost can be fetched by CMake, but when building from scratch,
# a separate build system is used - b2.
# https://cmake.org/cmake/help/latest/module/FindBoost.html
# We need to build some Boost libraries, just fetching headers won't be enough.
# https://stackoverflow.com/a/13604163/2766161
if(NOT ${UKV_REBUILD_BOOST})
    find_package(Boost 1.71.0)

else()
    include(ExternalProject)
    set(BOOST_BOOTSTRAP_COMMAND ./bootstrap.sh)
    set(BOOST_BUILD_TOOL ./b2)
    set(BOOST_LIBRARY_SUFFIX .a)

    if(${BUILD_SHARED_LIBS})
        set(BOOST_CXXFLAGS "cxxflags=-fPIC")
    endif()

    # FetchContent_Declare(
    # boost
    # URL https://boostorg.jfrog.io/artifactory/main/release/1.79.0/source/boost_1_79_0.tar.bz2
    # URL_HASH SHA256=475d589d51a7f8b3ba2ba4eda022b170e562ca3b760ee922c146b6c65856ef39
    # )
    # FetchContent_MakeAvailable(boost)
    # So let's do like Ethereum does:
    # https://cs.github.com/ethereum/cpp-dependencies/blob/e5c8316db8d3daa0abc3b5af8545ce330057608c/boost.cmake?q=externalproject_add+boost
    ExternalProject_Add(
        Boost-external
        URL https://boostorg.jfrog.io/artifactory/main/release/1.79.0/source/boost_1_79_0.tar.bz2
        URL_HASH SHA256=475d589d51a7f8b3ba2ba4eda022b170e562ca3b760ee922c146b6c65856ef39

        PREFIX "_deps"
        DOWNLOAD_DIR "_deps/boost-src"
        LOG_DIR "_deps/boost-log"
        STAMP_DIR "_deps/boost-stamp"
        TMP_DIR "_deps/boost-tmp"
        SOURCE_DIR "_deps/boost-src"
        INSTALL_DIR "_deps/boost-install"

        BUILD_IN_SOURCE 1
        CONFIGURE_COMMAND ${BOOST_BOOTSTRAP_COMMAND}
        BUILD_COMMAND ${BOOST_BUILD_TOOL} stage
        ${BOOST_CXXFLAGS}
        threading=multi
        link=static
        variant=release
        address-model=64
        -d0
        --with-chrono
        --with-date_time
        --with-filesystem
        --with-random
        --with-system
        --with-thread
        INSTALL_COMMAND ""
    )

    set(BOOST_ROOT _deps/boost-src)
    set(BOOST_INCLUDE_DIR _deps/boost-src)
    set(BOOST_LIB_DIR _deps/boost-src/stage/lib)
endif()