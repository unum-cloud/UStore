# LibBSON is not maintained any more.
# Estead the MongoDB C driver has to be used.
include(ExternalProject)
ExternalProject_Add(
    mongo_c_driver
    GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
    GIT_TAG 1.23.0
    GIT_SHALLOW 1
    GIT_PROGRESS 0

    PREFIX "_deps"
    DOWNLOAD_DIR "_deps/mongo_c_driver-src"
    LOG_DIR "_deps/mongo_c_driver-log"
    STAMP_DIR "_deps/mongo_c_driver-stamp"
    TMP_DIR "_deps/mongo_c_driver-tmp"
    SOURCE_DIR "_deps/mongo_c_driver-src"
    INSTALL_DIR "_deps/mongo_c_driver-install"
    BINARY_DIR "_deps/mongo_c_driver-build"

    BUILD_ALWAYS 0
    UPDATE_COMMAND ""

    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/_deps/mongo_c_driver-install
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_INSTALL_RPATH:PATH=<INSTALL_DIR>/lib
    -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
    -DENABLE_STATIC:STRING=ON
    -DENABLE_BSON:BOOL=ON
    -DENABLE_SSL:BOOL=OFF
    -DENABLE_SASL:BOOL=OFF
    -DENABLE_TESTS:BOOL=OFF
    -DENABLE_EXAMPLES:BOOL=OFF
    -DENABLE_SRV:BOOL=OFF
    -DENABLE_TRACING:BOOL=OFF
    -DENABLE_COVERAGE:BOOL=OFF
    -DENABLE_SHM_COUNTERS:BOOL=OFF
    -DENABLE_MONGOC:BOOL=OFF
    -DENABLE_SNAPPY:BOOL=OFF
    -DENABLE_ZLIB:BOOL=OFF
    -DENABLE_ZSTD:BOOL=OFF
    -DENABLE_MAN_PAGES:BOOL=OFF
    -DENABLE_HTML_DOCS:BOOL=OFF
)

set(bson_INCLUDE_DIR ${CMAKE_BINARY_DIR}/_deps/mongo_c_driver-install/include/libbson-1.0)

set(bson_LIBRARY_PATH ${CMAKE_BINARY_DIR}/_deps/mongo_c_driver-install/lib/libbson-static-1.0.a)

file(MAKE_DIRECTORY ${bson_INCLUDE_DIR})
add_library(bson STATIC IMPORTED)

set_property(TARGET bson PROPERTY IMPORTED_LOCATION ${bson_LIBRARY_PATH})
set_property(TARGET bson APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${bson_INCLUDE_DIR})

# Dependencies
add_dependencies(bson mongo_c_driver)
include_directories(${bson_INCLUDE_DIR})