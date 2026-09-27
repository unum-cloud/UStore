# liburing: https://github.com/axboe/liburing, built here as a static position-independent archive, so RocksDB finds a
# copy compiled with this build's flags rather than a system `liburing.a` that most distributions leave out.

include(FetchContent)
FetchContent_Declare(
    liburing
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG liburing-2.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(liburing)

# `configure` writes `config-host.h` into the root and `compat.h` and `io_uring_version.h` into `src/include/liburing/`.
# It runs once, so a reconfigure leaves the headers RocksDB's I/O units include untouched. `--use-libc` keeps the
# freestanding flags of the libc-free build away from sanitizers and the Python extension.
if (NOT EXISTS "${liburing_SOURCE_DIR}/config-host.h")
    execute_process(
        COMMAND sh ./configure --use-libc --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
        WORKING_DIRECTORY "${liburing_SOURCE_DIR}"
        RESULT_VARIABLE liburing_configured
        OUTPUT_QUIET
    )
    if (NOT liburing_configured EQUAL 0)
        message(FATAL_ERROR "liburing's configure failed, see ${liburing_SOURCE_DIR}/config.log")
    endif ()
endif ()

add_library(
    uring STATIC
    "${liburing_SOURCE_DIR}/src/setup.c" "${liburing_SOURCE_DIR}/src/queue.c" "${liburing_SOURCE_DIR}/src/register.c"
    "${liburing_SOURCE_DIR}/src/syscall.c" "${liburing_SOURCE_DIR}/src/version.c"
)
add_library(uring::uring ALIAS uring)
target_include_directories(uring PUBLIC $<BUILD_INTERFACE:${liburing_SOURCE_DIR}/src/include>)
target_compile_definitions(uring PRIVATE _GNU_SOURCE _LARGEFILE_SOURCE _FILE_OFFSET_BITS=64 LIBURING_INTERNAL)
target_compile_options(uring PRIVATE -include "${liburing_SOURCE_DIR}/config-host.h")
set_target_properties(uring PROPERTIES POSITION_INDEPENDENT_CODE ON)

# RocksDB's `Finduring` takes these as found and leaves `uring::uring` alone, since the target already exists.
set(uring_INCLUDE_DIR
    "${liburing_SOURCE_DIR}/src/include"
    CACHE INTERNAL ""
)
set(uring_LIBRARIES
    uring::uring
    CACHE INTERNAL ""
)
