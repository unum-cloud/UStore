
include(ExternalProject)

set(JEMALLOC_PREFIX_DIR ${CMAKE_BINARY_DIR}/_deps/jemalloc)
set(JEMALLOC_SRC_DIR ${JEMALLOC_PREFIX_DIR}/src/jemalloc)
set(JEMALLOC_INSTALL_DIR ${JEMALLOC_PREFIX_DIR}/install)

ExternalProject_Add(jemalloc
        GIT_REPOSITORY      https://github.com/jemalloc/jemalloc.git
        GIT_TAG             5.3.0
        PREFIX              ${JEMALLOC_PREFIX_DIR}
        CONFIGURE_COMMAND   echo Configuring jemalloc
                            && cd ${JEMALLOC_SRC_DIR}
                            && ./autogen.sh && ./configure
--prefix=${JEMALLOC_INSTALL_DIR} --enable-prof # --with-jemalloc-prefix=je_
        BUILD_COMMAND       echo Building jemalloc && cd ${JEMALLOC_SRC_DIR}
                            && make install_lib_static install_include
        INSTALL_COMMAND     ""
        UPDATE_COMMAND      ""
)

# Create libjemalloc and libjemalloc_pic targets to be used as
# dependencies
add_library(libjemalloc STATIC IMPORTED GLOBAL)
add_library(libjemalloc_pic STATIC IMPORTED GLOBAL)

set_property(TARGET libjemalloc 
    PROPERTY IMPORTED_LOCATION 
    ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a
)
set_property(TARGET libjemalloc_pic 
    PROPERTY IMPORTED_LOCATION 
    ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc_pic.a
)

link_libraries(-Wl,--no-as-needed)
link_libraries(dl ${libjemalloc_pic})

# Export the include directory path.
set(jemalloc_SOURCE_DIR ${JEMALLOC_INSTALL_DIR})
set(jemalloc_LIBRARIES ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc_pic.a ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a)
