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
# freestanding flags of the libc-free build away from sanitizers and the Python extension, and a cross build names its
# target through the compilers, since `configure` itself knows none.
if (NOT EXISTS "${liburing_SOURCE_DIR}/config-host.h")
    set(ustore_liburing_cc_ "${CMAKE_C_COMPILER}")
    set(ustore_liburing_cxx_ "${CMAKE_CXX_COMPILER}")
    if (CMAKE_C_COMPILER_TARGET)
        string(APPEND ustore_liburing_cc_ " --target=${CMAKE_C_COMPILER_TARGET}")
        string(APPEND ustore_liburing_cxx_ " --target=${CMAKE_CXX_COMPILER_TARGET}")
    endif ()
    execute_process(
        COMMAND sh ./configure --use-libc "--cc=${ustore_liburing_cc_}" "--cxx=${ustore_liburing_cxx_}"
        WORKING_DIRECTORY "${liburing_SOURCE_DIR}"
        RESULT_VARIABLE ustore_liburing_configured_
        OUTPUT_QUIET
    )
    if (NOT ustore_liburing_configured_ EQUAL 0)
        message(FATAL_ERROR "liburing's configure failed, see ${liburing_SOURCE_DIR}/config.log")
    endif ()
endif ()

add_library(
    uring STATIC
    "${liburing_SOURCE_DIR}/src/setup.c" "${liburing_SOURCE_DIR}/src/queue.c" "${liburing_SOURCE_DIR}/src/register.c"
    "${liburing_SOURCE_DIR}/src/syscall.c" "${liburing_SOURCE_DIR}/src/version.c"
)
add_library(uring::uring ALIAS uring)
target_include_directories(uring SYSTEM PUBLIC $<BUILD_INTERFACE:${liburing_SOURCE_DIR}/src/include>)
target_compile_definitions(uring PRIVATE _GNU_SOURCE _LARGEFILE_SOURCE _FILE_OFFSET_BITS=64 LIBURING_INTERNAL)
target_compile_options(uring PRIVATE -include "${liburing_SOURCE_DIR}/config-host.h")
set_target_properties(uring PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

# RocksDB's `Finduring` takes these as found and leaves `uring::uring` alone, since the target already exists.
set(uring_INCLUDE_DIR
    "${liburing_SOURCE_DIR}/src/include"
    CACHE INTERNAL ""
)
set(uring_LIBRARIES
    uring::uring
    CACHE INTERNAL ""
)
