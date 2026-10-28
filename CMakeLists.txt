cmake_minimum_required(VERSION 3.28)

file(READ "VERSION" ustore_version_)
string(STRIP ${ustore_version_} ustore_version_)
# CMake versions are numeric, so the pre-release suffix of VERSION stays in ustore_version_ only.
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" ustore_version_number_ ${ustore_version_})
project(
    UStore
    LANGUAGES C CXX
    VERSION ${ustore_version_number_}
    HOMEPAGE_URL "https://github.com/unum-cloud/UStore"
    DESCRIPTION "GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices"
)

# Git does not run hooks shipped inside a clone, so configuring the build points `core.hooksPath` at `.githooks`.
if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND git config core.hooksPath .githooks
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE ustore_hooks_status_
        OUTPUT_QUIET ERROR_QUIET
    )
    if (ustore_hooks_status_ EQUAL 0)
        message(STATUS "UStore: Git hooks enabled (core.hooksPath -> .githooks)")
    endif ()
endif ()

# Options
option(USTORE_INSTALL "Install CMake targets" OFF)

option(USTORE_WITH_ROCKSDB "Use RocksDB for a persistent engine" OFF)
option(USTORE_WITH_LEVELDB "Use LevelDB for a persistent engine" OFF)
option(USTORE_WITH_SPDK "Use SPDK for user-space NVMe access in NVMe" OFF)
option(USTORE_WITH_NVME "Compile the in-tree NVMe engine" OFF)

option(USTORE_BUILD_TEST "Compile the C++ suites in test/" OFF)
option(USTORE_BUILD_BENCH "Compile the C++ benchmarks in bench/" OFF)
option(USTORE_BUILD_TOOLS "Compile the CLI and the dataset tools in tools/" OFF)
option(USTORE_BUILD_SHARED "Compile the static and shared C libraries in the default build" OFF)
option(USTORE_BUILD_PYTHON "Compile the CPython extension; `scikit-build-core` switches it on" OFF)
set(USTORE_CUDA_ARCH
    ""
    CACHE STRING "NVIDIA code NVMe's GPU compaction builds for: empty, all, native or one like 100f"
)
option(USTORE_WERROR "Treat compiler warnings as errors" OFF)
option(USTORE_BUNDLE_SIBLINGS "Copy the ForkUnion and StringZilla objects into ustore_static; Cargo links their own" ON)
set(USTORE_BUILD_SANITIZERS
    "off"
    CACHE STRING "Sanitizers for UStore's own targets: off, address (with undefined) or thread"
)
set_property(CACHE USTORE_BUILD_SANITIZERS PROPERTY STRINGS off address thread)
# Seeded from the environment at the first configure, so `cargo` and `scikit-build-core` honour `USTORE_MARCH=native`
# and `USTORE_MULTIVERSIONED=OFF`.
set(USTORE_MARCH
    "$ENV{USTORE_MARCH}"
    CACHE STRING
          "The `-march` floor: empty for the toolchain default, an ISA level, native, or a CPU like graniterapids"
)
set(ustore_multiversioned_ ON)
if (DEFINED ENV{USTORE_MULTIVERSIONED})
    set(ustore_multiversioned_ "$ENV{USTORE_MULTIVERSIONED}")
endif ()
option(USTORE_MULTIVERSIONED "Compile every CPU class above the floor, or one monoversioned class for USTORE_MARCH"
       ${ustore_multiversioned_}
)

# C++20 is a floor each UStore target declares through `cxx_std_20`, so fetched dependencies build at the standard they
# pick for themselves.
set(CMAKE_CXX_EXTENSIONS OFF)
# No target uses C++20 modules, and Homebrew's clang-scan-deps cannot find the macOS SDK on its own.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib")
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

include(cmake/ustore_flags.cmake)

include(cmake/ustore_platform.cmake)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Install 3rd Party Packages
include(FetchContent)
include_directories(c cpp include)
include_directories(tools)
include_directories(src)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    set(FETCHCONTENT_QUIET OFF)
else ()
    set(FETCHCONTENT_QUIET ON)
endif ()

# Engines:
if (${USTORE_WITH_LEVELDB})
    include(cmake/leveldb.cmake)
endif ()

if (${USTORE_WITH_ROCKSDB})
    include(cmake/rocksdb.cmake)
endif ()

# The user-space NVMe transport, an `ustore_spdk` interface target over the archives of an SPDK build
if (${USTORE_WITH_SPDK})
    include(cmake/spdk.cmake)
endif ()

# Modalities:
include(cmake/fmt.cmake)
# Its objects go inside the UStore libraries, so an install publishes no yyjson header, archive or config of its own
set(YYJSON_INSTALL OFF)
include(cmake/yyjson.cmake)

include(cmake/ustore_siblings.cmake)

# The reusable templates of `include/ustore/`, header-only, with the include paths of the siblings and the third-party
# headers they reach into. The package installs UStore's own headers alone, so a consumer brings each sibling's.
add_library(ustore_header INTERFACE)
add_library(ustore::header ALIAS ustore_header)
set_target_properties(ustore_header PROPERTIES EXPORT_NAME header)
target_compile_features(ustore_header INTERFACE cxx_std_20)
target_include_directories(
    ustore_header
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(
    ustore_header SYSTEM INTERFACE $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
                                   $<BUILD_INTERFACE:${ustore_fmt_include_dir_}>
)
target_link_libraries(
    ustore_header INTERFACE $<BUILD_INTERFACE:forkunion::header> $<BUILD_INTERFACE:smashtable::header>
                            $<BUILD_INTERFACE:stringzilla::header> $<BUILD_INTERFACE:ustore_sanitizers>
)

include(cmake/ustore_gpu.cmake)

# The RAM and NVMe engines compile one variant per CPU class, in `src/<engine>/variants_<class>.cpp`, and their opens
# pick the newest one the running CPU admits. A class is its parent plus the ForkUnion capabilities its waiter and
# atomic reference add beyond the spin hint every core of an architecture carries, restated here row for row from
# `parent_class_of`, `waiter_for_cpu_class` and `atomic_ref_for_cpu_class` of `src/lib.hpp`. It exists when its parent
# does and every one of those capabilities is enabled. A multiversioned build keeps every class that exists. A
# monoversioned build keeps none, the `serial` floor included, and compiles the one `monoversioned` class instead, with
# the waiter and atomic reference ForkUnion picks for `USTORE_MARCH` and what a preset vouches for. Every UStore unit
# reads the kept classes as `USTORE_TARGET_<CLASS>` set to 0 or 1, collected in `ustore_cpu_gates_`. ForkUnion spells
# each instruction in the assembly itself, so a class needs no flags of its own.
set(ustore_class_serial_exists_ TRUE)
set(ustore_class_serial_kept_ ${USTORE_MULTIVERSIONED})
set(ustore_class_serial_macro_ SERIAL)
set(ustore_cpu_rows_ serial)

# Declares the CPU class @p class_, gated as `USTORE_TARGET_<target_macro_>`, over the class named after `OVER` and the
# ForkUnion capabilities after it.
function (ustore_cpu_class class_ target_macro_)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "OVER" "")
    set(exists_ ${ustore_class_${arg_OVER}_exists_})
    foreach (capability_ IN LISTS arg_UNPARSED_ARGUMENTS)
        if (NOT forkunion_target_${capability_}_enabled)
            set(exists_ FALSE)
        endif ()
    endforeach ()
    set(kept_ FALSE)
    if (USTORE_MULTIVERSIONED)
        set(kept_ ${exists_})
    endif ()
    set(ustore_class_${class_}_exists_
        ${exists_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_kept_
        ${kept_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_macro_
        ${target_macro_}
        PARENT_SCOPE
    )
    set(ustore_cpu_rows_
        ${ustore_cpu_rows_} ${class_}
        PARENT_SCOPE
    )
endfunction ()

ustore_cpu_class(intel_sierra INTEL_SIERRA OVER serial x86_tpause x86_cmpccxadd)
ustore_cpu_class(apple_m2 APPLE_M2 OVER serial arm64_lse arm64_rcpc)

# A monoversioned build compiles one class: every capability StringZilla and ForkUnion enabled, which is what
# `USTORE_MARCH` implies plus what a preset vouches for.
set(ustore_class_monoversioned_macro_ MONOVERSIONED)
set(ustore_monoversioned_capabilities_)
if (NOT USTORE_MULTIVERSIONED)
    foreach (library_ IN ITEMS stringzilla forkunion)
        get_property(enabled_ GLOBAL PROPERTY ${library_}_capabilities_enabled)
        foreach (capability_ IN LISTS enabled_)
            if (NOT DEFINED ${library_}_target_${capability_}_architectures
                AND NOT "${ustore_monoversioned_capabilities_} " MATCHES " ${capability_} "
            )
                string(APPEND ustore_monoversioned_capabilities_ " ${capability_}")
            endif ()
        endforeach ()
    endforeach ()
    set(ustore_class_monoversioned_kept_ TRUE)
endif ()
list(APPEND ustore_cpu_rows_ monoversioned)

set(ustore_cpu_classes_)
set(ustore_cpu_gates_)
foreach (class_ IN LISTS ustore_cpu_rows_)
    if (ustore_class_${class_}_kept_)
        list(APPEND ustore_cpu_classes_ ${class_})
        list(APPEND ustore_cpu_gates_ "USTORE_TARGET_${ustore_class_${class_}_macro_}=1")
    else ()
        list(APPEND ustore_cpu_gates_ "USTORE_TARGET_${ustore_class_${class_}_macro_}=0")
    endif ()
endforeach ()
if (USTORE_MARCH)
    set(ustore_floor_ "${USTORE_MARCH}")
else ()
    set(ustore_floor_ "the portable floor")
endif ()
if (USTORE_MULTIVERSIONED)
    message(STATUS "UStore: multiversioned above ${ustore_floor_}")
else ()
    message(STATUS "UStore: monoversioned for ${ustore_floor_}:${ustore_monoversioned_capabilities_}")
endif ()

# Without `USTORE_BUILD_SHARED`, the libraries build only for the targets that link them
if (NOT USTORE_BUILD_SHARED AND NOT USTORE_INSTALL)
    set(ustore_library_exclusion_ EXCLUDE_FROM_ALL)
endif ()

# One library holds the C ABI and every engine the build enables, each opened through its own `ustore_database_*` calls;
# every engine is its own object of the archive, so a program links only those it opens. The archive bundles the small
# siblings every build needs, as USearch's does, while the storage engines it links stay archives of their own,
# installed beside it, so a change to UStore never rewrites RocksDB.
set(ustore_engine_names_ "ram")
set(ustore_sources_
    src/types.cpp
    src/db.cpp
    src/blobs.cpp
    src/strings.cpp
    src/docs.cpp
    src/graph.cpp
    src/pages.cpp
    src/ram/engine.cpp
)
set(ustore_sibling_archives_ forkunion_static stringzilla_static)
set(ustore_dependencies_ ${ustore_sibling_archives_} yyjson fmt)
set(ustore_engine_archives_)
set(ustore_engine_definitions_)
set(ustore_compiled_objects_)

foreach (class_ IN LISTS ustore_cpu_classes_)
    list(APPEND ustore_ram_variants_ src/ram/variants_${class_}.cpp)
endforeach ()
# Both libraries link one copy of every CPU class's engine, compiled position-independent once
add_library(ustore_ram_variants OBJECT ${ustore_library_exclusion_} ${ustore_ram_variants_})
set_target_properties(ustore_ram_variants PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ustore_ram_variants PRIVATE Threads::Threads smashtable::header ${ustore_dependencies_})
list(APPEND ustore_sources_ $<TARGET_OBJECTS:ustore_ram_variants>)
list(APPEND ustore_compiled_objects_ ustore_ram_variants)

# The NVMe engine mounts its volumes on files, so it builds everywhere the siblings do; the io_uring transport beside it
# compiles on every platform and only opens a ring on Linux.
if (${USTORE_WITH_NVME})
    list(APPEND ustore_sources_ src/nvme/engine.cpp)
    foreach (class_ IN LISTS ustore_cpu_classes_)
        list(APPEND ustore_nvme_variants_ src/nvme/variants_${class_}.cpp)
    endforeach ()
    # Both libraries link one copy of every CPU class's engine, compiled position-independent once
    add_library(ustore_nvme_variants OBJECT ${ustore_library_exclusion_} ${ustore_nvme_variants_})
    set_target_properties(ustore_nvme_variants PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(ustore_nvme_variants PRIVATE Threads::Threads smashtable::header ${ustore_dependencies_})
    list(APPEND ustore_sources_ $<TARGET_OBJECTS:ustore_nvme_variants>)
    list(APPEND ustore_compiled_objects_ ustore_nvme_variants)
    list(APPEND ustore_engine_definitions_ USTORE_WITH_NVME=1)
    list(APPEND ustore_engine_names_ "nvme")
endif ()

if (${USTORE_WITH_ROCKSDB})
    list(APPEND ustore_sources_ src_oracles/rocksdb/engine.cpp)
    list(APPEND ustore_engine_archives_ rocksdb)
    if (TARGET uring)
        list(APPEND ustore_engine_archives_ uring)
    endif ()
    list(APPEND ustore_engine_definitions_ USTORE_WITH_ROCKSDB=1)
    list(APPEND ustore_engine_names_ "rocksdb")
endif ()

if (${USTORE_WITH_LEVELDB})
    list(APPEND ustore_sources_ src_oracles/leveldb/engine.cpp)
    list(APPEND ustore_engine_archives_ leveldb)
    list(APPEND ustore_engine_definitions_ USTORE_WITH_LEVELDB=1)
    if (NOT MSVC)
        set_source_files_properties(src_oracles/leveldb/engine.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
    endif ()
    list(APPEND ustore_engine_names_ "leveldb")
endif ()

# An archive links nothing, so it takes the objects of every library it builds on, while the shared library links them
# and exports only what `USTORE_EXPORT` marks, which its consumers import on Windows. Inside this tree the archive also
# links them, since the suites and tools reach the siblings through it.
set(ustore_dependency_objects_)
set(ustore_dependency_links_)
foreach (dependency_ IN LISTS ustore_dependencies_)
    if (USTORE_BUNDLE_SIBLINGS OR NOT dependency_ IN_LIST ustore_sibling_archives_)
        list(APPEND ustore_dependency_objects_ $<TARGET_OBJECTS:${dependency_}>)
    endif ()
    list(APPEND ustore_dependency_links_ $<BUILD_INTERFACE:${dependency_}>)
endforeach ()
add_library(ustore_static STATIC ${ustore_library_exclusion_} ${ustore_sources_} ${ustore_dependency_objects_})
add_library(ustore::static ALIAS ustore_static)
add_library(ustore::ustore ALIAS ustore_static)
add_library(ustore_shared SHARED ${ustore_library_exclusion_} ${ustore_sources_})
add_library(ustore::shared ALIAS ustore_shared)
set_target_properties(ustore_static PROPERTIES EXPORT_NAME static)
set_target_properties(ustore_shared PROPERTIES EXPORT_NAME shared)
target_compile_definitions(
    ustore_shared
    PRIVATE USTORE_EXPORTING
    INTERFACE USTORE_SHARED
)
target_link_libraries(
    ustore_static PUBLIC Threads::Threads $<BUILD_INTERFACE:smashtable::header> ${ustore_dependency_links_}
                         ${ustore_engine_archives_}
)
target_link_libraries(
    ustore_shared PRIVATE Threads::Threads smashtable::header ${ustore_dependencies_} ${ustore_engine_archives_}
)
# The archives linked inside keep their exports to themselves, so the shared library exports the C ABI alone
target_link_options(
    ustore_shared PRIVATE $<$<PLATFORM_ID:Linux,Android,FreeBSD>:LINKER:--exclude-libs,ALL>
    $<$<PLATFORM_ID:Darwin>:LINKER:-exported_symbol,_ustore_*>
)

foreach (library_ IN ITEMS ustore_static ustore_shared ${ustore_compiled_objects_})
    target_compile_features(${library_} PUBLIC cxx_std_20)
    target_compile_definitions(
        ${library_}
        PUBLIC ${ustore_engine_definitions_}
        INTERFACE USTORE_VERSION="${ustore_version_}"
    )
    # Collection identifiers draw their entropy from `BCryptGenRandom` on Windows
    target_link_libraries(${library_} PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt> $<BUILD_INTERFACE:ustore_warnings>)
    # Every unit reads which CPU classes compiled and which ForkUnion capabilities the toolchain builds, so the units
    # instantiating a class and the open picking one name the same rungs
    target_compile_definitions(${library_} PRIVATE ${ustore_cpu_gates_})
    target_link_libraries(${library_} PRIVATE $<BUILD_INTERFACE:forkunion::cpu_capabilities_compiled>)
    target_link_libraries(${library_} PUBLIC $<BUILD_INTERFACE:ustore_sanitizers>)
    if (TARGET ustore_nvme_compaction)
        target_link_libraries(${library_} PUBLIC ustore_nvme_compaction)
    endif ()
    # The `include_directories` above reach this project's targets only, so a consumer linking a library inherits the
    # public headers from the target instead.
    target_include_directories(
        ${library_}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endforeach ()

if (${USTORE_BUILD_TOOLS})
    add_executable(ustore_cli tools/cli.cpp tools/dataset.cpp)
    target_link_libraries(ustore_cli PRIVATE ustore::static fmt::fmt ustore_warnings)
endif ()

if (${USTORE_BUILD_TEST})
    enable_testing()
    set(ustore_test_names_ "api" "compilation" "units" "strings" "strings_collection")

    # The storage suites build with the NVMe engine they test. The storage ring stands outside the per-engine loop: it
    # links no engine, only the siblings it is written against.
    if (${USTORE_WITH_NVME})
        set(ustore_storage_test_names_
            "ring"
            "channels"
            "catalog"
            "volume"
            "sorted_run"
            "tree"
            "collection"
            "transaction_registry"
            "database"
            "single_collection_database"
        )

        # The io_uring, Linux AIO and SPDK transports are Linux-only and take no option of their own; each suite skips
        # itself where the kernel or the build refuses its transport.
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND ustore_storage_test_names_ "linux_uring_disk" "linux_aio_disk" "linux_spdk_disk")
        endif ()

        # The SPDK suite starts and stops its own target through the rig script, so the script's path is compiled in and
        # `--spdk-root` names the build the script runs; without the option the suite prints its skip line instead.
        foreach (storage_name_ IN ITEMS ${ustore_storage_test_names_})
            set(storage_exe_ ustore_test_nvme_${storage_name_})
            add_executable(${storage_exe_} test/nvme/${storage_name_}.cpp)
            target_compile_features(${storage_exe_} PRIVATE cxx_std_20)
            target_include_directories(${storage_exe_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
            target_link_libraries(
                ${storage_exe_}
                PRIVATE ustore::header
                        forkunion::static
                        smashtable::header
                        stringzilla::static
                        fmt::fmt
                        Threads::Threads
                        $<$<PLATFORM_ID:Windows>:bcrypt>
                        ustore_warnings
            )
            set(storage_arguments_ --directory ${CMAKE_BINARY_DIR}/test_data)
            # The LSM suite merges on the device where the build carries one, so on the transports that need no hardware
            # option its device case may no longer skip
            if (storage_name_ STREQUAL "tree" AND TARGET ustore_nvme_compaction)
                target_link_libraries(${storage_exe_} PRIVATE ustore_nvme_compaction)
                list(APPEND storage_arguments_ --no-skips --transports memory,blocking,linux-uring)
            endif ()
            if (storage_name_ STREQUAL "linux_spdk_disk")
                target_compile_definitions(
                    ${storage_exe_} PRIVATE USTORE_SPDK_RIG="${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigs/spdk_target.sh"
                )
                if (${USTORE_WITH_SPDK})
                    target_link_libraries(${storage_exe_} PRIVATE ustore_spdk)
                    list(APPEND storage_arguments_ --spdk-root ${USTORE_SPDK_ROOT})
                endif ()
            endif ()
            add_test(NAME ${storage_exe_} COMMAND ${storage_exe_} ${storage_arguments_})
        endforeach ()

        # The database suites run as plain C++ everywhere, and merge on the device through the compactor once the build
        # carries one
        set(ustore_nvme_device_suites_ database_transaction single_collection_database_transaction
                                       single_collection_database_stress single_collection_database_transaction_stress
        )
        foreach (suite_name_ IN ITEMS ${ustore_nvme_device_suites_})
            add_executable(ustore_test_nvme_${suite_name_} test/nvme/${suite_name_}.cpp)
            target_include_directories(ustore_test_nvme_${suite_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
            target_compile_features(ustore_test_nvme_${suite_name_} PRIVATE cxx_std_20)
            target_link_libraries(
                ustore_test_nvme_${suite_name_}
                PRIVATE ustore_header
                        forkunion::static
                        smashtable::header
                        stringzilla::static
                        fmt::fmt
                        Threads::Threads
                        $<$<PLATFORM_ID:Windows>:bcrypt>
                        ustore_warnings
            )
            if (TARGET ustore_nvme_compaction)
                target_link_libraries(ustore_test_nvme_${suite_name_} PRIVATE ustore_nvme_compaction)
            endif ()
            add_test(NAME ustore_test_nvme_${suite_name_} COMMAND ustore_test_nvme_${suite_name_} --directory
                                                                  ${CMAKE_BINARY_DIR}/test_data
            )
        endforeach ()

        # The graph store lives in the engine's database, so its suite drives it through the C ABI of the engine
        add_executable(ustore_test_nvme_graph test/nvme/graph.cpp)
        target_include_directories(ustore_test_nvme_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_link_libraries(ustore_test_nvme_graph PRIVATE ustore::static fmt::fmt Threads::Threads ustore_warnings)
        add_test(NAME ustore_test_nvme_graph COMMAND ustore_test_nvme_graph --directory ${CMAKE_BINARY_DIR}/test_data)

        # The pages store too, checked against the in-memory engine
        add_executable(ustore_test_nvme_pages test/nvme/pages.cpp)
        target_include_directories(ustore_test_nvme_pages PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_link_libraries(ustore_test_nvme_pages PRIVATE ustore::static fmt::fmt ustore_warnings)
        add_test(NAME ustore_test_nvme_pages COMMAND ustore_test_nvme_pages --directory ${CMAKE_BINARY_DIR}/test_data)
    endif ()

    # The compaction suite as plain C++ checks the comparator, the survival rule, the sorted-run split and the placement
    # without a device, so it builds without a CUDA compiler
    set(ustore_compaction_host_source_ "${CMAKE_CURRENT_BINARY_DIR}/test/nvme/compaction_host.cpp")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test/nvme/compaction.cu" "${ustore_compaction_host_source_}" COPYONLY)
    add_executable(ustore_test_nvme_compaction_host "${ustore_compaction_host_source_}")
    target_include_directories(ustore_test_nvme_compaction_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_compile_features(ustore_test_nvme_compaction_host PRIVATE cxx_std_20)
    # A fold's scratch arena allocates on ForkUnion's memory domains, which the host compactor reaches
    target_link_libraries(
        ustore_test_nvme_compaction_host PRIVATE ustore_header smashtable::header forkunion::static stringzilla::header
                                                 fmt::fmt ustore_warnings
    )
    add_test(NAME ustore_test_nvme_compaction_host COMMAND ustore_test_nvme_compaction_host)

    # The device half of the suite is the same source as host C++ over the compiled compactor, since nvcc's front end
    # crashes instantiating StringZilla's string, which the harness reaches; one CUDA TU instantiates its key slots
    if (TARGET ustore_nvme_compaction)
        add_executable(
            ustore_test_nvme_compaction "${ustore_compaction_host_source_}"
                                        "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme/compaction_device.cu"
        )
        target_include_directories(ustore_test_nvme_compaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_compile_features(ustore_test_nvme_compaction PRIVATE cxx_std_20)
        set_target_properties(ustore_test_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
        target_link_libraries(
            ustore_test_nvme_compaction PRIVATE ustore_nvme_compaction forkunion::static fmt::fmt ustore_warnings
        )
        add_test(NAME ustore_test_nvme_compaction COMMAND ustore_test_nvme_compaction)
    endif ()

    set(ustore_stress_names_ "atomicity" "linearizability")

    # Every suite builds once and runs once per engine, picking it with `--engine`
    foreach (test_name_ IN ITEMS ${ustore_test_names_})
        add_executable(ustore_test_${test_name_} test/${test_name_}.cpp)
        target_link_libraries(ustore_test_${test_name_} PRIVATE ustore::static fmt::fmt ustore_warnings)
        foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
            add_test(NAME ustore_test_${test_name_}_${engine_name_}
                     COMMAND ustore_test_${test_name_} --engine ${engine_name_} --directory
                             ${CMAKE_BINARY_DIR}/test_data
            )
        endforeach ()
    endforeach ()

    # The templates of `include/ustore/` build against the header target alone, with no engine linked
    set(ustore_utilities_test_names_ "ram_buffers" "algorithm")
    foreach (test_name_ IN ITEMS ${ustore_utilities_test_names_})
        add_executable(ustore_test_utilities_${test_name_} test/utilities/${test_name_}.cpp)
        target_include_directories(ustore_test_utilities_${test_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_link_libraries(
            ustore_test_utilities_${test_name_} PRIVATE ustore::header forkunion::static fmt::fmt ustore_warnings
        )
        add_test(NAME ustore_test_utilities_${test_name_} COMMAND ustore_test_utilities_${test_name_})
    endforeach ()

    # The pages and docs templates call StringZilla's dispatched kernels, which a header-only StringZilla answers with
    # `sz_missing_library_k`, so these suites link its compiled library instead of `ustore::header`
    set(ustore_templates_test_paths_ "pages/element_tree" "pages/page_fold" "docs/json_paths")
    foreach (test_path_ IN ITEMS ${ustore_templates_test_paths_})
        string(REPLACE "/" "_" test_name_ ${test_path_})
        add_executable(ustore_test_${test_name_} test/${test_path_}.cpp)
        target_include_directories(ustore_test_${test_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_link_libraries(
            ustore_test_${test_name_}
            PRIVATE smashtable::header
                    forkunion::static
                    stringzilla::static
                    yyjson
                    fmt::fmt
                    ustore_warnings
                    ustore_sanitizers
        )
        add_test(NAME ustore_test_${test_name_} COMMAND ustore_test_${test_name_})
    endforeach ()

    # The graph fold calls no StringZilla kernel, so its suite builds against the header target alone
    add_executable(ustore_test_graph_adjacency_fold test/graph/adjacency_fold.cpp)
    target_link_libraries(ustore_test_graph_adjacency_fold PRIVATE ustore::header fmt::fmt ustore_warnings)
    add_test(NAME ustore_test_graph_adjacency_fold COMMAND ustore_test_graph_adjacency_fold)

    # The runs of strings build against the header target alone
    add_executable(ustore_test_strings_nvme_memory_run test/strings/nvme_memory_run.cpp)
    target_link_libraries(
        ustore_test_strings_nvme_memory_run PRIVATE ustore::header forkunion::static fmt::fmt ustore_warnings
    )
    add_test(NAME ustore_test_strings_nvme_memory_run COMMAND ustore_test_strings_nvme_memory_run)

    # Memory runs and their views call no StringZilla kernel either, and need only SmashTable's headers
    add_executable(ustore_test_blobs_nvme_memory_run test/blobs/nvme_memory_run.cpp)
    target_link_libraries(ustore_test_blobs_nvme_memory_run PRIVATE ustore::header fmt::fmt ustore_warnings)
    add_test(NAME ustore_test_blobs_nvme_memory_run COMMAND ustore_test_blobs_nvme_memory_run)

    # The docs fold is checked against `ustore_docs_write` on every engine, so its suite links them all
    add_executable(ustore_test_docs_json_fold test/docs/json_fold.cpp)
    target_include_directories(ustore_test_docs_json_fold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_link_libraries(ustore_test_docs_json_fold PRIVATE ustore::static yyjson fmt::fmt ustore_warnings)
    foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
        add_test(NAME ustore_test_docs_json_fold_${engine_name_}
                 COMMAND ustore_test_docs_json_fold --engine ${engine_name_} --directory ${CMAKE_BINARY_DIR}/test_data
        )
    endforeach ()

    # The C ABI compiles as plain C11, so a header that only a C++ compiler accepts fails to build. It links against
    # both libraries, and the shared one exports only what `USTORE_EXPORT` marks.
    add_executable(ustore_test_c c/test.c)
    add_executable(ustore_test_c_shared c/test.c)
    target_link_libraries(ustore_test_c PRIVATE ustore::static)
    target_link_libraries(ustore_test_c_shared PRIVATE ustore::shared)
    foreach (c_test_ IN ITEMS ustore_test_c ustore_test_c_shared)
        set_target_properties(${c_test_} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        # The shared library's C++ reaches the sanitizers' guard and allocation interceptors only when the program
        # exports them, which the C++ driver alone does
        if (NOT USTORE_BUILD_SANITIZERS STREQUAL "off")
            set_target_properties(${c_test_} PROPERTIES LINKER_LANGUAGE CXX)
        endif ()
        target_compile_options(
            ${c_test_}
            PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Werror;-Werror=incompatible-pointer-types>"
        )
        add_test(NAME ${c_test_} COMMAND ${c_test_})
    endforeach ()

    # RAM stages half-edges and watches its reads only under snapshot isolation, so two suites run there as well
    set(ustore_snapshot_arguments_ --engine ram --isolation snapshot --directory ${CMAKE_BINARY_DIR}/test_data)
    foreach (test_name_ IN ITEMS "api" "units")
        add_test(NAME ustore_test_${test_name_}_ram_snapshot COMMAND ustore_test_${test_name_}
                                                                     ${ustore_snapshot_arguments_}
        )
    endforeach ()

    foreach (stress_name_ IN ITEMS ${ustore_stress_names_})
        add_executable(ustore_stress_${stress_name_} test/stress_${stress_name_}.cpp)
        target_link_libraries(ustore_stress_${stress_name_} PRIVATE ustore::static fmt::fmt ustore_warnings)
        foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
            add_test(NAME ustore_stress_${stress_name_}_${engine_name_}
                     COMMAND ustore_stress_${stress_name_} --engine ${engine_name_} --directory
                             ${CMAKE_BINARY_DIR}/test_data
            )
        endforeach ()
    endforeach ()

    # The tools suite drives the CLI on the same engine
    if (${USTORE_BUILD_TOOLS})
        add_executable(ustore_test_tools test/tools.cpp tools/dataset.cpp)
        target_link_libraries(ustore_test_tools PRIVATE ustore::static fmt::fmt ustore_warnings)
        add_dependencies(ustore_test_tools ustore_cli)
        foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
            add_test(NAME ustore_test_tools_${engine_name_}
                     COMMAND ustore_test_tools --engine ${engine_name_} --directory ${CMAKE_BINARY_DIR}/test_data --cli
                             $<TARGET_FILE:ustore_cli>
            )
        endforeach ()
    endif ()

    # RocksDB builds uninstrumented, so ThreadSanitizer cannot see the barriers of its own threads
    if (USTORE_BUILD_SANITIZERS STREQUAL "thread")
        get_property(
            ustore_registered_tests_
            DIRECTORY
            PROPERTY TESTS
        )
        set_tests_properties(
            ${ustore_registered_tests_}
            PROPERTIES
                ENVIRONMENT_MODIFICATION
                "TSAN_OPTIONS=string_prepend:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/test/thread_sanitizer.supp:"
        )
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

    # The docs paths benchmark times the templates alone, so it links StringZilla's compiled library and no engine
    add_executable(ustore_bench_docs_paths bench/docs_paths.cpp)
    target_link_libraries(
        ustore_bench_docs_paths
        PRIVATE smashtable::header
                forkunion::static
                stringzilla::static
                yyjson
                fmt::fmt
                ustore_warnings
                ustore_sanitizers
    )
endif ()

if (${USTORE_BUILD_PYTHON})
    add_subdirectory(python)
endif ()

include(cmake/ustore_install.cmake)
