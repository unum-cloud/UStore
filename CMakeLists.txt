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
option(USTORE_BUILD_CUDA "Compile NVMe's GPU compaction" OFF)
option(USTORE_WERROR "Treat compiler warnings as errors" OFF)
option(USTORE_BUNDLE_SIBLINGS "Copy the ForkUnion and StringZilla objects into ustore_static; Cargo links their own" ON)
set(USTORE_BUILD_SANITIZERS
    "off"
    CACHE STRING "Sanitizers for UStore's own targets: off, address (with undefined) or thread"
)
set_property(CACHE USTORE_BUILD_SANITIZERS PROPERTY STRINGS off address thread)
# Seeded from the environment at the first configure, so `cargo` and `scikit-build-core` honour
# `USTORE_TARGET_ARCH=native`.
set(USTORE_TARGET_ARCH
    "$ENV{USTORE_TARGET_ARCH}"
    CACHE STRING
          "CPU the own units and the siblings are tuned for: empty for the toolchain default, native, or a CPU name"
)
set(USTORE_TARGET_CLASSES
    ""
    CACHE STRING "CPU classes compiled beside serial: empty for every class, a list like apple_m2, or native"
)
if (USTORE_TARGET_CLASSES STREQUAL "native" AND NOT USTORE_TARGET_ARCH STREQUAL "native")
    message(FATAL_ERROR "USTORE_TARGET_CLASSES=native reads what this host runs, which the siblings probe only under "
                        "USTORE_TARGET_ARCH=native"
    )
endif ()
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

# Warnings apply to UStore's own targets only, so fetched dependencies build with their own flags.
add_library(ustore_warnings INTERFACE)
if (MSVC)
    target_compile_options(ustore_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE /WX)
    endif ()
else ()
    # The C ABI takes named arguments as partially designated structs, leaving the rest zeroed. CUDA sources take the
    # compactor's own flags instead, since nvcc's host pass reads line markers that pedantic mode rejects.
    target_compile_options(
        ustore_warnings
        INTERFACE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic;-Wno-unknown-pragmas;-Wno-missing-field-initializers>"
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wdocumentation>"
    )
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE -Werror)
    endif ()
endif ()

# USTORE_TARGET_ARCH tunes UStore's own units, which all link `ustore_warnings`, for one CPU: `native` for this host, or
# a CPU name like `graniterapids`, also when cross-compiling. A WebAssembly module takes its one SIMD capability
# instead: serial, v128 or v128relaxed. Fetched dependencies keep their own flags.
if (USTORE_TARGET_ARCH STREQUAL "native" AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "USTORE_TARGET_ARCH=native needs the host as the target; name a CPU instead")
elseif (EMSCRIPTEN OR CMAKE_SYSTEM_PROCESSOR MATCHES "wasm")
    if (USTORE_TARGET_ARCH STREQUAL "v128relaxed")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128;-mrelaxed-simd>")
    elseif (USTORE_TARGET_ARCH STREQUAL "v128")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128>")
    elseif (USTORE_TARGET_ARCH AND NOT USTORE_TARGET_ARCH STREQUAL "serial")
        message(
            FATAL_ERROR "USTORE_TARGET_ARCH on WebAssembly: serial, v128 or v128relaxed, not `${USTORE_TARGET_ARCH}`"
        )
    endif ()
elseif (
    USTORE_TARGET_ARCH
    AND MSVC
    AND NOT USTORE_TARGET_ARCH STREQUAL "native"
)
    message(FATAL_ERROR "MSVC takes USTORE_TARGET_ARCH=native only; pass an /arch: value such as AVX2 or AVX512 "
                        "through CMAKE_CXX_FLAGS instead of `${USTORE_TARGET_ARCH}`"
    )
elseif (USTORE_TARGET_ARCH AND MSVC)
    # MSVC has no `/arch:native`, and no UStore unit needs instruction-set flags, so the toolchain default stays.
elseif (USTORE_TARGET_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm64|ARM64")
    # AArch64 names CPUs through `-mcpu`, like `neoverse-v2`, as `-march` takes only architecture versions.
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${USTORE_TARGET_ARCH}>")
elseif (USTORE_TARGET_ARCH)
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=${USTORE_TARGET_ARCH}>")
endif ()

# Sanitizers instrument UStore's own targets and the sibling headers they compile, while RocksDB, LevelDB, liburing,
# fmt, yyjson and the sibling archives keep their release flags. -O1 keeps the instrumented suites tolerable outside
# Debug.
add_library(ustore_sanitizers INTERFACE)
if (USTORE_BUILD_SANITIZERS STREQUAL "address")
    if (MSVC)
        set(ustore_sanitizer_flags_ /fsanitize=address)
    else ()
        set(ustore_sanitizer_flags_ -fsanitize=address,undefined)
    endif ()
elseif (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    if (MSVC)
        message(FATAL_ERROR "MSVC has no ThreadSanitizer, so USTORE_BUILD_SANITIZERS=thread needs Clang or GCC")
    endif ()
    set(ustore_sanitizer_flags_ -fsanitize=thread)
elseif (NOT USTORE_BUILD_SANITIZERS STREQUAL "off")
    message(FATAL_ERROR "USTORE_BUILD_SANITIZERS is off, address or thread, not '${USTORE_BUILD_SANITIZERS}'")
endif ()
if (ustore_sanitizer_flags_ AND NOT MSVC)
    target_compile_options(
        ustore_sanitizers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_};-fno-omit-frame-pointer>"
                                    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-O1>"
    )
    target_link_options(ustore_sanitizers INTERFACE "$<$<LINK_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_}>")
elseif (ustore_sanitizer_flags_)
    target_compile_options(ustore_sanitizers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_}>")
endif ()

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

# A sibling builds from its nested checkout, or else from the commit its submodule pins, fetched unless
# `FETCHCONTENT_SOURCE_DIR_<NAME>` names a local checkout, as in a workspace that keeps the siblings side by side. The
# caller passes that override variable, so nothing derives its name.
function (ustore_sibling name_ override_ repository_ tag_)
    if (TARGET ${name_}::header)
        return()
    endif ()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name_}/CMakeLists.txt" AND NOT DEFINED ${override_})
        add_subdirectory(${name_} EXCLUDE_FROM_ALL SYSTEM)
    else ()
        message(STATUS "ustore: ${name_} submodule absent or overridden, fetching ${tag_}")
        FetchContent_Declare(
            ${name_}
            GIT_REPOSITORY ${repository_}
            GIT_TAG ${tag_}
            EXCLUDE_FROM_ALL SYSTEM
        )
        FetchContent_MakeAvailable(${name_})
    endif ()
endfunction ()

# The siblings tune their own units for the same CPU, and probe what this host runs under `native`.
if (USTORE_TARGET_ARCH)
    set(FORKUNION_TARGET_ARCH ${USTORE_TARGET_ARCH})
    set(STRINGZILLA_TARGET_ARCH ${USTORE_TARGET_ARCH})
endif ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    12a0a2706534452830868b7e33a4697549c898d5
)
# Every UStore thread runs inside ForkUnion's compiled pool, whose barriers ThreadSanitizer sees only once that archive
# is instrumented as well
if (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    target_link_libraries(forkunion_static PRIVATE ustore_sanitizers)
endif ()

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    e0db741da4e7b9cf6bc13aa493eff264aff3cfb8
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    a4e9f5b0ac91481a5c30957e05d2310f3928c1d8
)

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

# NVMe's compaction on the device, the CUDA runtime library every engine and suite links once the build carries it. The
# GPU half is opt-in, so a build without it leaves the toolkit alone.
if (USTORE_BUILD_CUDA)
    include(CheckLanguage)

    # A toolkit installed outside the PATH, which is where the distribution packages put it
    if (NOT DEFINED CMAKE_CUDA_COMPILER)
        find_program(USTORE_NVCC nvcc HINTS /usr/local/cuda/bin /opt/cuda/bin)
        if (USTORE_NVCC)
            set(CMAKE_CUDA_COMPILER "${USTORE_NVCC}")
        endif ()
    endif ()
    check_language(CUDA)
    if (NOT CMAKE_CUDA_COMPILER)
        message(
            WARNING
                "UStore: no CUDA compiler found, so only the host compaction suite builds - install CUDA 13 or newer"
        )
    endif ()
endif ()
if (USTORE_BUILD_CUDA AND CMAKE_CUDA_COMPILER)
    if (CMAKE_VERSION VERSION_LESS 3.31 AND USTORE_CUDA_ARCHITECTURES MATCHES "1[0-9][0-9][af]")
        message(FATAL_ERROR "Three-digit \"a\" and \"f\" CUDA codes need CMake 3.31, found ${CMAKE_VERSION}")
    endif ()
    # The same codes on every machine, rather than whatever GPU the building one carries
    set(CMAKE_CUDA_ARCHITECTURES ${USTORE_CUDA_ARCHITECTURES})
    enable_language(CUDA)
    if (CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 13)
        message(FATAL_ERROR "UStore needs CUDA 13 or newer, found ${CMAKE_CUDA_COMPILER_VERSION}")
    endif ()
    find_package(CUDAToolkit REQUIRED)
    # NVCC's default leaves Turing and Ampere cubins uncompressed, ten times their compressed size.
    add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:--compress-mode=size>)
    include(cmake/cccl.cmake)

    add_library(ustore_nvme_compaction STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/nvme/compaction.cu")
    target_link_libraries(
        ustore_nvme_compaction
        PUBLIC CCCL::CUB CUDA::cudart ustore_header smashtable::header forkunion::header
        PRIVATE stringzilla::static
    )
    set_target_properties(ustore_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
    target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)
    # The host pass reads nvcc's preprocessed output, where every header is the main file and every macro is expanded,
    # so constant operands from capability macros and unused statics of the C headers would warn there and nowhere else
    target_compile_options(
        ustore_nvme_compaction
        PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wno-constant-logical-operand,-Wno-unneeded-internal-declaration>
    )
    # A constexpr function is callable from a kernel, so the shared headers carry no host-device qualifier
    target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr>)
    # StringZilla's serial kernels, reached through the engine's key slots, carry GCC pragmas nvcc's front end does not
    # know
    target_compile_options(ustore_nvme_compaction PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--diag-suppress=1675>)
    # Every target linking the library merges on the device, and every other one takes the host branch of the merger
    target_compile_definitions(ustore_nvme_compaction PUBLIC USTORE_WITH_CUDA=1)
endif ()

# The RAM and NVMe engines compile one variant per CPU class, in `src/<engine>/variants_<class>.cpp`, and their opens
# pick the newest one the running CPU admits. A class is its parent plus the ForkUnion capabilities its waiter and
# atomic reference add beyond the spin hint every core of an architecture carries, restated here row for row from
# `parent_class_of`, `waiter_for_cpu_class` and `atomic_ref_for_cpu_class` of `src/lib.hpp`. It exists when its parent
# does and every one of those capabilities is enabled, and compiles when it also passes `USTORE_TARGET_CLASSES`, which
# every UStore unit reads as `USTORE_TARGET_<CLASS>` set to 0 or 1, collected in `ustore_cpu_gates_`. ForkUnion spells
# each instruction in the assembly itself, so a class needs no flags of its own, and the `serial` class is the floor
# every CPU runs.
set(ustore_class_serial_exists_ TRUE)
set(ustore_cpu_classes_ serial)
set(ustore_cpu_gates_)

# Declares the CPU class @p class_, gated as `USTORE_TARGET_<target_macro_>`, over the class named after `OVER` and the
# ForkUnion capabilities after it. A targeted build keeps serial and the named classes, or under `native` the classes
# this host runs in full.
function (ustore_cpu_class class_ target_macro_)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "OVER" "")
    set(exists_ ${ustore_class_${arg_OVER}_exists_})
    set(kept_ ${exists_})
    foreach (capability_ IN LISTS arg_UNPARSED_ARGUMENTS)
        if (NOT forkunion_target_${capability_}_enabled)
            set(exists_ FALSE)
            set(kept_ FALSE)
        elseif (USTORE_TARGET_CLASSES STREQUAL "native" AND NOT forkunion_target_${capability_}_runs)
            set(kept_ FALSE)
        endif ()
    endforeach ()
    if (USTORE_TARGET_CLASSES
        AND NOT USTORE_TARGET_CLASSES STREQUAL "native"
        AND NOT class_ IN_LIST USTORE_TARGET_CLASSES
    )
        set(kept_ FALSE)
    endif ()
    set(ustore_class_${class_}_exists_
        ${exists_}
        PARENT_SCOPE
    )
    if (kept_)
        set(ustore_cpu_classes_
            ${ustore_cpu_classes_} ${class_}
            PARENT_SCOPE
        )
        set(ustore_cpu_gates_
            ${ustore_cpu_gates_} "USTORE_TARGET_${target_macro_}=1"
            PARENT_SCOPE
        )
    else ()
        set(ustore_cpu_gates_
            ${ustore_cpu_gates_} "USTORE_TARGET_${target_macro_}=0"
            PARENT_SCOPE
        )
    endif ()
endfunction ()

ustore_cpu_class(intel_sierra INTEL_SIERRA OVER serial x86_tpause x86_cmpccxadd)
ustore_cpu_class(apple_m2 APPLE_M2 OVER serial arm64_lse arm64_rcpc)
ustore_cpu_class(aws_g5 AWS_G5 OVER apple_m2 arm64_wfet)

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

# Both libraries install with a CMake package, `find_package(ustore)` naming them `ustore::static` and `ustore::shared`;
# the engine archives install beside them as `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`, which
# `ustore::static` links
if (USTORE_INSTALL)
    include(CMakePackageConfigHelpers)
    set(ustore_config_install_dir_ "${CMAKE_INSTALL_LIBDIR}/cmake/ustore")
    install(
        TARGETS ustore_header ustore_static ustore_shared ${ustore_engine_archives_}
        EXPORT ustoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        EXPORT ustoreTargets
        NAMESPACE ustore::
        DESTINATION ${ustore_config_install_dir_}
    )
    file(
        WRITE "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
        [[include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/ustoreTargets.cmake")
]]
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
                  "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake" DESTINATION ${ustore_config_install_dir_}
    )
    install(DIRECTORY c/ustore cpp/ustore include/ustore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

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
