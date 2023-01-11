# TurboPFor for 64-bit integers
# https://github.com/powturbo/TurboPFor-Integer-Compression
# https://github.com/topics/integer-compression
# include("${CMAKE_SOURCE_DIR}/cmake/turbopfor.cmake")

# FastPFor Integer Compression for 32-bit integers
# FetchContent_Declare(
# fpfor
# GIT_REPOSITORY https://github.com/lemire/FastPFor.git
# GIT_TAG v0.1.8
# GIT_SHALLOW TRUE
# )
# FetchContent_MakeAvailable(fpfor)

include(ExternalProject)

if(${UKV_REBUILD_TURBOPFOR})
    include(ExternalProject)
    find_package(Git REQUIRED)
    find_program(MAKE_EXE NAMES gmake nmake make)

    # Get turbopfor
    ExternalProject_Add(
        turbopfor_src
        PREFIX "_deps/turbopfor"
        GIT_REPOSITORY "https://github.com/powturbo/TurboPFor-Integer-Compression.git"
        TIMEOUT 10
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE TRUE
        BUILD_COMMAND make libic.a
        UPDATE_COMMAND ""
        INSTALL_COMMAND ""
    )

    # Prepare turbopfor
    ExternalProject_Get_Property(turbopfor_src source_dir)
    set(turbopfor_INCLUDE_DIR ${source_dir})
    set(turbopfor_LIBRARY_PATH ${source_dir}/libic.a)
    file(MAKE_DIRECTORY ${turbopfor_INCLUDE_DIR})
    add_library(turbopfor STATIC IMPORTED)

    set_property(TARGET turbopfor PROPERTY IMPORTED_LOCATION ${turbopfor_LIBRARY_PATH})
    set_property(TARGET turbopfor APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${turbopfor_INCLUDE_DIR})

    # Dependencies
    add_dependencies(turbopfor turbopfor_src)
endif()