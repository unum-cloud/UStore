

include(ExternalProject)
ExternalProject_Add(
    pcre2_external
    GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
    GIT_TAG "pcre2-10.42"
    GIT_SHALLOW 1
    GIT_PROGRESS 0

    PREFIX "_deps"
    DOWNLOAD_DIR "_deps/pcre2-src"
    LOG_DIR "_deps/pcre2-log"
    STAMP_DIR "_deps/pcre2-stamp"
    TMP_DIR "_deps/pcre2-tmp"
    SOURCE_DIR "_deps/pcre2-src"
    INSTALL_DIR "_deps/pcre2-install"
    BINARY_DIR "_deps/pcre2-build"

    BUILD_ALWAYS 0
    UPDATE_COMMAND ""

    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/_deps/pcre2-install
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_INSTALL_RPATH:PATH=<INSTALL_DIR>/lib
    -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
    -DPCRE2_STATIC_PIC:BOOL=ON
    -DPCRE2_SUPPORT_JIT:BOOL=ON
    -DPCRE2GREP_SUPPORT_JIT:BOOL=OFF
    -DPCRE2_BUILD_PCRE2GREP:BOOL=OFF
    -DPCRE2_BUILD_TESTS:BOOL=OFF
)

set(pcre2_INCLUDE_DIR ${CMAKE_BINARY_DIR}/_deps/pcre2-install/include/)
set(pcre2_LIBRARY_PATH ${CMAKE_BINARY_DIR}/_deps/pcre2-install/lib/libpcre2-8.a)

file(MAKE_DIRECTORY ${pcre2_INCLUDE_DIR})
add_library(pcre2 STATIC IMPORTED)

set_property(TARGET pcre2 PROPERTY IMPORTED_LOCATION ${pcre2_LIBRARY_PATH})
set_property(TARGET pcre2 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${pcre2_INCLUDE_DIR})

# Dependencies
add_dependencies(pcre2 pcre2_external)
include_directories(${pcre2_INCLUDE_DIR})