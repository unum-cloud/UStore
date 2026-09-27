cmake_minimum_required(VERSION 3.25)

file(READ "VERSION" USTORE_VERSION)
string(STRIP ${USTORE_VERSION} USTORE_VERSION)
# CMake versions are numeric, so the pre-release suffix of VERSION stays in USTORE_VERSION only.
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" USTORE_VERSION_NUMBER ${USTORE_VERSION})
project(
    UStore
    LANGUAGES C CXX
    VERSION ${USTORE_VERSION_NUMBER}
    HOMEPAGE_URL "https://github.com/unum-cloud/UStore"
    DESCRIPTION "GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices"
)

# Determine if UStore is built as a subproject (using `add_subdirectory`) or if it is the main project
set(USTORE_IS_MAIN_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(USTORE_IS_MAIN_PROJECT ON)
endif ()

# Git does not run hooks shipped inside a clone, so configuring the build points `core.hooksPath` at `.githooks`.
if (USTORE_IS_MAIN_PROJECT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND git config core.hooksPath .githooks
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE ustore_hooks_status
        OUTPUT_QUIET ERROR_QUIET
    )
    if (ustore_hooks_status EQUAL 0)
        message(STATUS "UStore: Git hooks enabled (core.hooksPath -> .githooks)")
    endif ()
endif ()

# Options
option(USTORE_INSTALL "Install CMake targets" OFF)

option(USTORE_WITH_ROCKSDB "Use RocksDB for a persistent engine" OFF)
option(USTORE_WITH_LEVELDB "Use LevelDB for a persistent engine" OFF)
option(USTORE_WITH_SPDK "Use SPDK for user-space NVMe access in NVMe" OFF)

option(USTORE_BUILD_TEST "Compile the C++ suites in test/" ${USTORE_IS_MAIN_PROJECT})
option(USTORE_BUILD_BENCH "Compile the C++ benchmarks in bench/" ${USTORE_IS_MAIN_PROJECT})
option(USTORE_BUILD_TOOLS "Compile the CLI and the dataset tools in tools/" OFF)
option(USTORE_BUILD_LIB_C "Compile a native library for the C interface, one per engine" OFF)
option(USTORE_BUILD_LIB_PYTHON "Compile the CPython extension; `scikit-build-core` switches it on" OFF)
option(USTORE_BUILD_NVME "Compile the in-tree NVMe engine" OFF)
option(USTORE_BUILD_CUDA "Compile NVMe's GPU compaction" OFF)
option(USTORE_WERROR "Treat compiler warnings as errors" OFF)
# CUB is all the compaction kernels call, so one plain code per generation covers Ampere through Blackwell.
set(USTORE_CUDA_ARCHITECTURES
    "80;90;100;120"
    CACHE STRING "GPU architectures NVMe's compaction compiles for"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
set(CMAKE_CACHEFILE_DIR "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/build/lib"
    CACHE PATH "Path to static libs"
)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/build/lib"
    CACHE PATH "Path to shared libs"
)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/bin")

message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# No instruction-set flags: artifacts run on any CPU of the target architecture.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Warnings apply to UStore's own targets only, so fetched dependencies build with their own flags.
add_library(ustore_warnings INTERFACE)
if (MSVC)
    target_compile_options(ustore_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE /WX)
    endif ()
else ()
    # The C ABI takes named arguments as partially designated structs, leaving the rest zeroed.
    target_compile_options(
        ustore_warnings INTERFACE -Wall -Wextra -Wpedantic -Wno-unknown-pragmas -Wno-missing-field-initializers
    )
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE -Werror)
    endif ()
endif ()

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Install 3rd Party Packages
include(FetchContent)
include_directories(include/)
include_directories(tools)
include_directories(src)
include_directories(src/helpers)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    set(FETCHCONTENT_QUIET OFF)
else ()
    set(FETCHCONTENT_QUIET ON)
endif ()

# Engines:
if (${USTORE_WITH_LEVELDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/leveldb.cmake")
    set(LIB_LEVELDB leveldb)
endif ()

if (${USTORE_WITH_ROCKSDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rocksdb.cmake")
    set(LIB_ROCKSDB rocksdb)
endif ()

# The user-space NVMe transport, an `ustore_spdk` interface target over the archives of an SPDK build
if (${USTORE_WITH_SPDK})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/spdk.cmake")
endif ()

# Modalities:
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fmt.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/yyjson.cmake")

# ForkUnion's C ABI, linked statically into every engine and suite
set(FORKUNION_BUILD_TEST OFF)
set(FORKUNION_INSTALL OFF)
add_subdirectory(forkunion EXCLUDE_FROM_ALL)

# SmashTable's containers, header-only
set(SMASHTABLE_BUILD_TEST OFF)
set(SMASHTABLE_BUILD_CUDA OFF)
set(SMASHTABLE_INSTALL OFF)
add_subdirectory(smashtable EXCLUDE_FROM_ALL)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
set(STRINGZILLA_BUILD_TEST OFF)
set(STRINGZILLA_BUILD_BENCH OFF)
set(STRINGZILLA_BUILD_SHARED OFF)
set(STRINGZILLA_BUILD_STATIC ON)
set(STRINGZILLA_BUILD_CUDA OFF)
set(STRINGZILLA_INSTALL OFF)
# StringZilla arms sanitizers in Debug by default, which leaves its archive needing a runtime the consumers' link lines
# do not carry.
set(STRINGZILLA_USE_SANITIZERS OFF)
add_subdirectory(stringzilla EXCLUDE_FROM_ALL)

add_library(ustore_stringzilla_dispatch INTERFACE)
target_link_libraries(ustore_stringzilla_dispatch INTERFACE stringzilla::static)
target_compile_definitions(ustore_stringzilla_dispatch INTERFACE STRINGZILLA_RUNTIME_DISPATCH=1)

if (${USTORE_BUILD_BENCH})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gbench.cmake")
endif ()

set(LIB_FMT fmt::fmt)

# The host half of the compaction suite checks the comparator, the survival rule, the sorted-run split and the placement
# without a device, so a plain test build registers it too.
if (USTORE_BUILD_CUDA OR USTORE_BUILD_TEST)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nvme_compaction.cmake")
endif ()

# Without `USTORE_BUILD_LIB_C`, an engine library builds only for the targets that link it
if (NOT USTORE_BUILD_LIB_C AND NOT USTORE_INSTALL)
    set(USTORE_LIB_C_EXCLUSION EXCLUDE_FROM_ALL)
endif ()

set(USTORE_ENGINE_NAMES "ram")
add_library(
    ustore_embedded_ram STATIC ${USTORE_LIB_C_EXCLUSION} src/ram/engine.cpp c/docs.cpp c/graph.cpp c/strings.cpp
)
target_link_libraries(
    ustore_embedded_ram Threads::Threads yyjson forkunion::static smashtable::header ustore_stringzilla_dispatch
)
target_compile_definitions(ustore_embedded_ram INTERFACE USTORE_VERSION="${USTORE_VERSION}")

# The NVMe engine mounts its volumes on files, so it builds everywhere the siblings do; the io_uring transport beside it
# compiles on every platform and only opens a ring on Linux.
if (${USTORE_BUILD_NVME})
    add_library(
        ustore_embedded_nvme STATIC ${USTORE_LIB_C_EXCLUSION} src/nvme/engine.cpp c/docs.cpp c/graph.cpp c/strings.cpp
    )
    target_link_libraries(
        ustore_embedded_nvme Threads::Threads yyjson forkunion::static smashtable::header ustore_stringzilla_dispatch
    )
    if (TARGET ustore_nvme_compaction)
        target_link_libraries(ustore_embedded_nvme ustore_nvme_compaction)
        set(USTORE_COMPACTION_LIB ustore_nvme_compaction)
    endif ()
    target_compile_definitions(ustore_embedded_nvme INTERFACE USTORE_VERSION="${USTORE_VERSION}")
    list(APPEND USTORE_ENGINE_NAMES "nvme")
endif ()

if (${USTORE_WITH_ROCKSDB})
    add_library(
        ustore_embedded_rocksdb STATIC ${USTORE_LIB_C_EXCLUSION} src_oracles/rocksdb/engine.cpp c/docs.cpp c/graph.cpp
                                       c/strings.cpp
    )
    target_link_libraries(ustore_embedded_rocksdb ${LIB_ROCKSDB} Threads::Threads yyjson)
    target_compile_definitions(ustore_embedded_rocksdb INTERFACE USTORE_VERSION="${USTORE_VERSION}")
    list(APPEND USTORE_ENGINE_NAMES "rocksdb")
endif ()

if (${USTORE_WITH_LEVELDB})
    add_library(
        ustore_embedded_leveldb STATIC ${USTORE_LIB_C_EXCLUSION} src_oracles/leveldb/engine.cpp c/docs.cpp c/graph.cpp
                                       c/strings.cpp
    )
    target_link_libraries(ustore_embedded_leveldb ${LIB_LEVELDB} Threads::Threads yyjson)
    if (NOT MSVC)
        set_source_files_properties(src_oracles/leveldb/engine.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
    endif ()
    target_compile_definitions(ustore_embedded_leveldb INTERFACE USTORE_VERSION="${USTORE_VERSION}")
    list(APPEND USTORE_ENGINE_NAMES "leveldb")
endif ()

foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
    target_link_libraries(
        ustore_embedded_${engine_name} ustore_warnings ${LIB_FMT} smashtable::header forkunion::static
    )
    # The `include_directories` above reach this project's targets only, so a consumer linking an engine inherits the
    # public headers from the target instead.
    target_include_directories(
        ustore_embedded_${engine_name}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/helpers
    )
endforeach ()

# A static engine library links only beside the archives it depends on, so they install together, apart from system
# copies.
if (USTORE_INSTALL)
    list(TRANSFORM USTORE_ENGINE_NAMES PREPEND "ustore_embedded_" OUTPUT_VARIABLE USTORE_ENGINE_LIBS)
    install(
        TARGETS ${USTORE_ENGINE_LIBS}
                ${USTORE_COMPACTION_LIB}
                ${LIB_ROCKSDB}
                ${LIB_LEVELDB}
                fmt
                yyjson
                forkunion_static
                stringzilla_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/ustore
    )
    install(DIRECTORY include/ustore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (${USTORE_BUILD_TOOLS})
    foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
        add_executable(ustore_cli_${engine_name} tools/cli.cpp tools/dataset.cpp)
        target_link_libraries(
            ustore_cli_${engine_name} ${LIB_FMT} ustore_embedded_${engine_name} ustore_stringzilla_dispatch
        )
    endforeach ()
endif ()

if (${USTORE_BUILD_TEST})
    enable_testing()
    set(USTORE_TEST_NAMES "api" "compilation" "units" "strings" "strings_collection")

    # The storage ring stands outside the per-engine loop: it links no engine, only the siblings it is written against.
    set(USTORE_STORAGE_TEST_NAMES
        "ring"
        "channels"
        "catalog"
        "volume"
        "sorted_run"
        "lsm"
        "collection"
        "transactions"
        "database"
        "single_collection_database"
        "linux_spdk"
    )

    # The io_uring and Linux AIO transports are Linux-only and take no option of their own; each suite skips itself
    # where the kernel refuses its transport.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND USTORE_STORAGE_TEST_NAMES "linux_uring" "linux_aio")
    endif ()

    # The SPDK suite starts and stops its own target through the rig script, so the script's path is compiled in and
    # `--spdk-root` names the build the script runs; without the option the suite prints its skip line instead.
    foreach (storage_name IN ITEMS ${USTORE_STORAGE_TEST_NAMES})
        set(storage_exe ustore_test_nvme_${storage_name})
        add_executable(${storage_exe} test/nvme_${storage_name}.cpp)
        target_link_libraries(
            ${storage_exe} ${LIB_FMT} forkunion::static smashtable::header ustore_stringzilla_dispatch Threads::Threads
        )
        set(storage_arguments --directory ${CMAKE_BINARY_DIR}/test_data)
        # The LSM suite merges on the device where the build carries one, so on the transports that need no hardware
        # option its device case may no longer skip
        if (storage_name STREQUAL "lsm" AND TARGET ustore_nvme_compaction)
            target_link_libraries(${storage_exe} ustore_nvme_compaction)
            list(APPEND storage_arguments --no-skips --transports memory,blocking,linux-uring)
        endif ()
        if (storage_name STREQUAL "linux_spdk")
            target_compile_definitions(
                ${storage_exe} PRIVATE USTORE_SPDK_RIG="${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigs/spdk_target.sh"
            )
            if (${USTORE_WITH_SPDK})
                target_link_libraries(${storage_exe} ustore_spdk)
                list(APPEND storage_arguments --spdk-root ${USTORE_SPDK_ROOT})
            endif ()
        endif ()
        add_test(NAME ${storage_exe} COMMAND ${storage_exe} ${storage_arguments})
    endforeach ()

    set(USTORE_STRESS_NAMES "atomicity" "linearizability")

    foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
        set(client_lib ustore_embedded_${engine_name})

        foreach (test_name IN ITEMS ${USTORE_TEST_NAMES})
            set(test_exe ustore_test_${test_name}_${engine_name})
            add_executable(${test_exe} test/${test_name}.cpp)
            target_link_libraries(${test_exe} ${LIB_FMT} ${client_lib})
            add_test(NAME ${test_exe} COMMAND ${test_exe} --directory ${CMAKE_BINARY_DIR}/test_data)
        endforeach ()

        foreach (stress_name IN ITEMS ${USTORE_STRESS_NAMES})
            set(test_exe ustore_stress_${stress_name}_${engine_name})
            add_executable(${test_exe} test/stress_${stress_name}.cpp)
            target_link_libraries(${test_exe} ${LIB_FMT} ${client_lib})
            add_test(NAME ${test_exe} COMMAND ${test_exe} --directory ${CMAKE_BINARY_DIR}/test_data)
        endforeach ()

        # The tools suite drives the CLI of the same engine
        if (${USTORE_BUILD_TOOLS})
            set(test_exe ustore_test_tools_${engine_name})
            add_executable(${test_exe} test/tools.cpp tools/dataset.cpp)
            target_link_libraries(${test_exe} ${LIB_FMT} ${client_lib} ustore_stringzilla_dispatch)
            add_dependencies(${test_exe} ustore_cli_${engine_name})
            add_test(NAME ${test_exe} COMMAND ${test_exe} --directory ${CMAKE_BINARY_DIR}/test_data --cli
                                              $<TARGET_FILE:ustore_cli_${engine_name}>
            )
        endif ()
    endforeach ()
endif ()

# Benchmarks, one executable per engine
if (${USTORE_BUILD_BENCH})
    foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
        set(client_lib ustore_embedded_${engine_name})

        set(bench_name ustore_bench_twitter_${engine_name})
        add_executable(${bench_name} bench/twitter.cpp)
        target_link_libraries(${bench_name} benchmark ${LIB_FMT} ${client_lib} ustore_stringzilla_dispatch)

        set(bench_name ustore_bench_tabular_graph_${engine_name})
        add_executable(${bench_name} bench/tabular_graph.cpp tools/dataset.cpp)
        target_link_libraries(${bench_name} benchmark ${LIB_FMT} ${client_lib} ustore_stringzilla_dispatch)

        set(bench_name ustore_bench_louvain_${engine_name})
        add_executable(${bench_name} bench/louvain.cpp)
        target_link_libraries(${bench_name} benchmark ${LIB_FMT} ${client_lib} ustore_stringzilla_dispatch)
    endforeach ()
endif ()

if (${USTORE_BUILD_LIB_PYTHON})
    add_subdirectory(python)
endif ()
