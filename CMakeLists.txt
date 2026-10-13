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

# C++20 is a floor each UStore target declares through `cxx_std_20`, so fetched dependencies build at the standard they
# pick for themselves.
set(CMAKE_CXX_EXTENSIONS OFF)
# No target uses C++20 modules, and Homebrew's clang-scan-deps cannot find the macOS SDK on its own.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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

# Windows headers define `min`, `max` and, through the RPC headers, `small` as macros unless told not to
if (WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()

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
include_directories(c cpp)
include_directories(tools)
include_directories(src)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    set(FETCHCONTENT_QUIET OFF)
else ()
    set(FETCHCONTENT_QUIET ON)
endif ()

# Engines:
if (${USTORE_WITH_LEVELDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/leveldb.cmake")
endif ()

if (${USTORE_WITH_ROCKSDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rocksdb.cmake")
endif ()

# The user-space NVMe transport, an `ustore_spdk` interface target over the archives of an SPDK build
if (${USTORE_WITH_SPDK})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/spdk.cmake")
endif ()

# Modalities:
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fmt.cmake")
# Its objects go inside the UStore libraries, so an install publishes no yyjson header, archive or config of its own
set(YYJSON_INSTALL
    OFF
    CACHE BOOL "" FORCE
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/yyjson.cmake")

# A sibling builds from its nested checkout, or from the directory `FETCHCONTENT_SOURCE_DIR_<NAME>` names when the
# submodule is not initialized, as in a workspace that keeps the siblings side by side
function (ustore_sibling NAME)
    string(TOUPPER ${NAME} UPPER_NAME)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}/CMakeLists.txt")
        add_subdirectory(${NAME} EXCLUDE_FROM_ALL)
    elseif (DEFINED FETCHCONTENT_SOURCE_DIR_${UPPER_NAME})
        add_subdirectory(
            "${FETCHCONTENT_SOURCE_DIR_${UPPER_NAME}}" "${CMAKE_CURRENT_BINARY_DIR}/${NAME}" EXCLUDE_FROM_ALL
        )
    else ()
        message(FATAL_ERROR "ustore: initialize the ${NAME} submodule or set FETCHCONTENT_SOURCE_DIR_${UPPER_NAME}")
    endif ()
endfunction ()

# ForkUnion's C ABI, linked statically into every engine and suite
set(FORKUNION_BUILD_TEST OFF)
set(FORKUNION_INSTALL OFF)
ustore_sibling(forkunion)

# SmashTable's containers, header-only
set(SMASHTABLE_BUILD_TEST OFF)
set(SMASHTABLE_BUILD_CUDA OFF)
set(SMASHTABLE_INSTALL OFF)
ustore_sibling(smashtable)

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
ustore_sibling(stringzilla)

# The host half of the compaction suite checks the comparator, the survival rule, the sorted-run split and the placement
# without a device, so a plain test build registers it too.
if (USTORE_BUILD_CUDA OR USTORE_BUILD_TEST)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nvme_compaction.cmake")
endif ()

# Without `USTORE_BUILD_LIB_C`, the libraries build only for the targets that link them
if (NOT USTORE_BUILD_LIB_C AND NOT USTORE_INSTALL)
    set(USTORE_LIB_C_EXCLUSION EXCLUDE_FROM_ALL)
endif ()

# One library holds the C ABI and every engine the build enables, each opened through its own `ustore_database_*` calls;
# every engine is its own object of the archive, so a program links only those it opens. The archive bundles the small
# siblings every build needs, as USearch's does, while the storage engines it links stay archives of their own,
# installed beside it, so a change to UStore never rewrites RocksDB.
set(USTORE_ENGINE_NAMES "ram")
set(USTORE_SOURCES src/lib.cpp src/docs.cpp src/graph.cpp src/ram/engine.cpp)
set(USTORE_DEPENDENCIES forkunion_static stringzilla_static yyjson fmt)
set(USTORE_ENGINE_ARCHIVES)
set(USTORE_ENGINE_DEFINITIONS)

# The NVMe engine mounts its volumes on files, so it builds everywhere the siblings do; the io_uring transport beside it
# compiles on every platform and only opens a ring on Linux.
if (${USTORE_BUILD_NVME})
    list(APPEND USTORE_SOURCES src/nvme/engine.cpp src/nvme/strings.cpp)
    list(APPEND USTORE_ENGINE_DEFINITIONS USTORE_WITH_NVME=1)
    list(APPEND USTORE_ENGINE_NAMES "nvme")
endif ()

if (${USTORE_WITH_ROCKSDB})
    list(APPEND USTORE_SOURCES src_oracles/rocksdb/engine.cpp)
    list(APPEND USTORE_ENGINE_ARCHIVES rocksdb)
    if (TARGET uring)
        list(APPEND USTORE_ENGINE_ARCHIVES uring)
    endif ()
    list(APPEND USTORE_ENGINE_DEFINITIONS USTORE_WITH_ROCKSDB=1)
    list(APPEND USTORE_ENGINE_NAMES "rocksdb")
endif ()

if (${USTORE_WITH_LEVELDB})
    list(APPEND USTORE_SOURCES src_oracles/leveldb/engine.cpp)
    list(APPEND USTORE_ENGINE_ARCHIVES leveldb)
    list(APPEND USTORE_ENGINE_DEFINITIONS USTORE_WITH_LEVELDB=1)
    if (NOT MSVC)
        set_source_files_properties(src_oracles/leveldb/engine.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
    endif ()
    list(APPEND USTORE_ENGINE_NAMES "leveldb")
endif ()

# An archive links nothing, so it takes the objects of every library it builds on, while the shared library links them
# and exports only what `USTORE_EXPORT` marks, which its consumers import on Windows. Inside this tree the archive also
# links them, since the suites and tools reach the siblings through it.
set(USTORE_DEPENDENCY_OBJECTS)
set(USTORE_DEPENDENCY_LINKS)
foreach (dependency IN LISTS USTORE_DEPENDENCIES)
    list(APPEND USTORE_DEPENDENCY_OBJECTS $<TARGET_OBJECTS:${dependency}>)
    list(APPEND USTORE_DEPENDENCY_LINKS $<BUILD_INTERFACE:${dependency}>)
endforeach ()
add_library(ustore_static STATIC ${USTORE_LIB_C_EXCLUSION} ${USTORE_SOURCES} ${USTORE_DEPENDENCY_OBJECTS})
add_library(ustore::static ALIAS ustore_static)
add_library(ustore::ustore ALIAS ustore_static)
add_library(ustore_shared SHARED ${USTORE_LIB_C_EXCLUSION} ${USTORE_SOURCES})
add_library(ustore::shared ALIAS ustore_shared)
set_target_properties(ustore_static PROPERTIES EXPORT_NAME static)
set_target_properties(ustore_shared PROPERTIES EXPORT_NAME shared)
target_compile_definitions(
    ustore_shared
    PRIVATE USTORE_EXPORTING
    INTERFACE USTORE_SHARED
)
target_link_libraries(
    ustore_static PUBLIC Threads::Threads $<BUILD_INTERFACE:smashtable::header> ${USTORE_DEPENDENCY_LINKS}
                         ${USTORE_ENGINE_ARCHIVES}
)
target_link_libraries(
    ustore_shared PRIVATE Threads::Threads smashtable::header ${USTORE_DEPENDENCIES} ${USTORE_ENGINE_ARCHIVES}
)
# The archives linked inside keep their exports to themselves, so the shared library exports the C ABI alone
target_link_options(
    ustore_shared PRIVATE $<$<PLATFORM_ID:Linux>:LINKER:--exclude-libs,ALL>
    $<$<PLATFORM_ID:Darwin>:LINKER:-exported_symbol,_ustore_*>
)

foreach (library IN ITEMS ustore_static ustore_shared)
    target_compile_features(${library} PUBLIC cxx_std_20)
    target_compile_definitions(
        ${library}
        PUBLIC ${USTORE_ENGINE_DEFINITIONS}
        INTERFACE USTORE_VERSION="${USTORE_VERSION}"
    )
    # Collection identifiers draw their entropy from `BCryptGenRandom` on Windows
    target_link_libraries(${library} PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt> $<BUILD_INTERFACE:ustore_warnings>)
    if (TARGET ustore_nvme_compaction)
        target_link_libraries(${library} PUBLIC ustore_nvme_compaction)
    endif ()
    # The `include_directories` above reach this project's targets only, so a consumer linking a library inherits the
    # public headers from the target instead.
    target_include_directories(
        ${library}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endforeach ()

# Both libraries install with a CMake package, `find_package(ustore)` naming them `ustore::static` and `ustore::shared`;
# the engine archives install beside them as `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`, which
# `ustore::static` links
if (USTORE_INSTALL)
    include(CMakePackageConfigHelpers)
    set(USTORE_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ustore")
    install(
        TARGETS ustore_static ustore_shared ${USTORE_ENGINE_ARCHIVES}
        EXPORT ustore_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        EXPORT ustore_targets
        NAMESPACE ustore::
        DESTINATION ${USTORE_CONFIG_INSTALL_DIR}
    )
    file(
        WRITE "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
        [[include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/ustore_targets.cmake")
]]
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
                  "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake" DESTINATION ${USTORE_CONFIG_INSTALL_DIR}
    )
    install(DIRECTORY c/ustore cpp/ustore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (${USTORE_BUILD_TOOLS})
    add_executable(ustore_cli tools/cli.cpp tools/dataset.cpp)
    target_link_libraries(ustore_cli PRIVATE ustore::static fmt::fmt ustore_warnings)
endif ()

if (${USTORE_BUILD_TEST})
    enable_testing()
    set(USTORE_TEST_NAMES "api" "compilation" "units" "strings" "strings_collection")

    # The storage suites build with the NVMe engine they test. The storage ring stands outside the per-engine loop: it
    # links no engine, only the siblings it is written against.
    if (${USTORE_BUILD_NVME})
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
        )

        # The io_uring, Linux AIO and SPDK transports are Linux-only and take no option of their own; each suite skips
        # itself where the kernel or the build refuses its transport.
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND USTORE_STORAGE_TEST_NAMES "linux_uring" "linux_aio" "linux_spdk")
        endif ()

        # The SPDK suite starts and stops its own target through the rig script, so the script's path is compiled in and
        # `--spdk-root` names the build the script runs; without the option the suite prints its skip line instead.
        foreach (storage_name IN ITEMS ${USTORE_STORAGE_TEST_NAMES})
            set(storage_exe ustore_test_nvme_${storage_name})
            add_executable(${storage_exe} test/nvme_${storage_name}.cpp)
            target_compile_features(${storage_exe} PRIVATE cxx_std_20)
            target_link_libraries(
                ${storage_exe}
                PRIVATE forkunion::static
                        smashtable::header
                        stringzilla::static
                        fmt::fmt
                        Threads::Threads
                        $<$<PLATFORM_ID:Windows>:bcrypt>
                        ustore_warnings
            )
            set(storage_arguments --directory ${CMAKE_BINARY_DIR}/test_data)
            # The LSM suite merges on the device where the build carries one, so on the transports that need no hardware
            # option its device case may no longer skip
            if (storage_name STREQUAL "lsm" AND TARGET ustore_nvme_compaction)
                target_link_libraries(${storage_exe} PRIVATE ustore_nvme_compaction)
                list(APPEND storage_arguments --no-skips --transports memory,blocking,linux-uring)
            endif ()
            if (storage_name STREQUAL "linux_spdk")
                target_compile_definitions(
                    ${storage_exe} PRIVATE USTORE_SPDK_RIG="${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigs/spdk_target.sh"
                )
                if (${USTORE_WITH_SPDK})
                    target_link_libraries(${storage_exe} PRIVATE ustore_spdk)
                    list(APPEND storage_arguments --spdk-root ${USTORE_SPDK_ROOT})
                endif ()
            endif ()
            add_test(NAME ${storage_exe} COMMAND ${storage_exe} ${storage_arguments})
        endforeach ()
    endif ()

    set(USTORE_STRESS_NAMES "atomicity" "linearizability")

    # Every suite builds once and runs once per engine, picking it with `--engine`
    foreach (test_name IN ITEMS ${USTORE_TEST_NAMES})
        add_executable(ustore_test_${test_name} test/${test_name}.cpp)
        target_link_libraries(ustore_test_${test_name} PRIVATE ustore::static fmt::fmt ustore_warnings)
        foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
            add_test(NAME ustore_test_${test_name}_${engine_name}
                     COMMAND ustore_test_${test_name} --engine ${engine_name} --directory ${CMAKE_BINARY_DIR}/test_data
            )
        endforeach ()
    endforeach ()

    # The C ABI compiles as plain C11, so a header that only a C++ compiler accepts fails to build. It links against
    # both libraries, and the shared one exports only what `USTORE_EXPORT` marks.
    add_executable(ustore_test_c c/test.c)
    add_executable(ustore_test_c_shared c/test.c)
    target_link_libraries(ustore_test_c PRIVATE ustore::static)
    target_link_libraries(ustore_test_c_shared PRIVATE ustore::shared)
    foreach (c_test IN ITEMS ustore_test_c ustore_test_c_shared)
        set_target_properties(${c_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        target_compile_options(
            ${c_test}
            PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Werror;-Werror=incompatible-pointer-types>"
        )
        add_test(NAME ${c_test} COMMAND ${c_test})
    endforeach ()

    # RAM stages half-edges and watches its reads only under snapshot isolation, so two suites run there as well
    set(snapshot_arguments --engine ram --isolation snapshot --directory ${CMAKE_BINARY_DIR}/test_data)
    foreach (test_name IN ITEMS "api" "units")
        add_test(NAME ustore_test_${test_name}_ram_snapshot COMMAND ustore_test_${test_name} ${snapshot_arguments})
    endforeach ()

    foreach (stress_name IN ITEMS ${USTORE_STRESS_NAMES})
        add_executable(ustore_stress_${stress_name} test/stress_${stress_name}.cpp)
        target_link_libraries(ustore_stress_${stress_name} PRIVATE ustore::static fmt::fmt ustore_warnings)
        foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
            add_test(NAME ustore_stress_${stress_name}_${engine_name}
                     COMMAND ustore_stress_${stress_name} --engine ${engine_name} --directory
                             ${CMAKE_BINARY_DIR}/test_data
            )
        endforeach ()
    endforeach ()

    # The tools suite drives the CLI on the same engine
    if (${USTORE_BUILD_TOOLS})
        add_executable(ustore_test_tools test/tools.cpp tools/dataset.cpp)
        target_link_libraries(ustore_test_tools PRIVATE ustore::static fmt::fmt ustore_warnings)
        add_dependencies(ustore_test_tools ustore_cli)
        foreach (engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
            add_test(NAME ustore_test_tools_${engine_name}
                     COMMAND ustore_test_tools --engine ${engine_name} --directory ${CMAKE_BINARY_DIR}/test_data --cli
                             $<TARGET_FILE:ustore_cli>
            )
        endforeach ()
    endif ()
endif ()

# Benchmarks, each picking its engine with `--engine`
if (${USTORE_BUILD_BENCH})
    add_executable(ustore_bench_twitter bench/twitter.cpp)
    target_link_libraries(ustore_bench_twitter PRIVATE ustore::static fmt::fmt ustore_warnings)

    add_executable(ustore_bench_tabular_graph bench/tabular_graph.cpp tools/dataset.cpp)
    target_link_libraries(ustore_bench_tabular_graph PRIVATE ustore::static fmt::fmt ustore_warnings)

    add_executable(ustore_bench_louvain bench/louvain.cpp)
    target_link_libraries(ustore_bench_louvain PRIVATE ustore::static fmt::fmt ustore_warnings)
endif ()

if (${USTORE_BUILD_LIB_PYTHON})
    add_subdirectory(python)
endif ()
