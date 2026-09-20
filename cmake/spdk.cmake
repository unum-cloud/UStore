# SPDK's initiator library for the user-space NVMe transport. `USTORE_SPDK_ROOT` names an SPDK source tree with a
# finished `make`; the `pkgconfig` files that build writes name the exact library set, so nothing is listed by hand.
set(USTORE_SPDK_ROOT
    "$ENV{USTORE_SPDK_ROOT}"
    CACHE PATH "SPDK source tree with a finished build"
)
if (NOT USTORE_SPDK_ROOT)
    message(FATAL_ERROR "USTORE_USE_SPDK needs USTORE_SPDK_ROOT, an SPDK tree with a finished build")
endif ()
if (NOT EXISTS "${USTORE_SPDK_ROOT}/include/spdk/nvme.h")
    message(FATAL_ERROR "USTORE_SPDK_ROOT=${USTORE_SPDK_ROOT} carries no include/spdk/nvme.h")
endif ()

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH}
    "${USTORE_SPDK_ROOT}/build/lib/pkgconfig:${USTORE_SPDK_ROOT}/dpdk/build/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}"
)
pkg_check_modules(USTORE_SPDK_INITIATOR REQUIRED spdk_nvme spdk_env_dpdk)
pkg_check_modules(USTORE_SPDK_SYSTEM REQUIRED spdk_syslibs)

# SPDK and DPDK register their transports, buses and mempool operations from constructors, which the linker drops from
# an archive unless the whole archive is kept; `-Bstatic` picks DPDK's archives over the shared objects beside them.
add_library(ustore_spdk INTERFACE)
target_include_directories(ustore_spdk SYSTEM INTERFACE ${USTORE_SPDK_INITIATOR_INCLUDE_DIRS})
target_compile_definitions(ustore_spdk INTERFACE USTORE_USE_SPDK=1)
target_link_libraries(
    ustore_spdk INTERFACE -Wl,--whole-archive -Wl,-Bstatic ${USTORE_SPDK_INITIATOR_LDFLAGS} -Wl,-Bdynamic
                          -Wl,--no-whole-archive ${USTORE_SPDK_SYSTEM_STATIC_LDFLAGS}
)
message(STATUS "UStore: SPDK initiator from ${USTORE_SPDK_ROOT}")
