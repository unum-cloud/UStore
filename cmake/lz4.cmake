# LZ4 Compression
# https://stackoverflow.com/questions/67537111/how-do-i-decide-between-lz4-and-snappy-compression
# include("${CMAKE_SOURCE_DIR}/cmake/lz4.cmake")

include(ExternalProject)
find_package(Git REQUIRED)
find_program(MAKE_EXE NAMES gmake nmake make)

# Get lz4
ExternalProject_Add(
    lz4_src
    PREFIX "_deps/lz4"
    GIT_REPOSITORY "https://github.com/lz4/lz4.git"
    GIT_TAG v1.9.3
    TIMEOUT 10
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND make
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
)

# Prepare lz4
ExternalProject_Get_Property(lz4_src source_dir)
set(lz4_INCLUDE_DIR ${source_dir}/lib)
set(lz4_LIBRARY_PATH ${source_dir}/lib/liblz4.a)
file(MAKE_DIRECTORY ${lz4_INCLUDE_DIR})
add_library(lz4 STATIC IMPORTED)

set_property(TARGET lz4 PROPERTY IMPORTED_LOCATION ${lz4_LIBRARY_PATH})
set_property(TARGET lz4 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${lz4_INCLUDE_DIR})

# Dependencies
add_dependencies(lz4 lz4_src)