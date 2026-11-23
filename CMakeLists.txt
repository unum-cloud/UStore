cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ustore_version_name_ LIMIT_COUNT 1)
# `project()` takes the numeric core alone, so a candidate's suffix stays in the name.
if (NOT ustore_version_name_ MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "VERSION must start with MAJOR.MINOR.PATCH, not '${ustore_version_name_}'")
endif ()
project(
    ustore
    VERSION ${CMAKE_MATCH_1}
    LANGUAGES C CXX
    HOMEPAGE_URL "https://www.unum.cloud/docs/ustore"
    DESCRIPTION "GPU-Accelerated Multi-Modal Transactional Database Engine Built around Kernel Bypass"
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
option(USTORE_WITH_LINUX_SPDK "Use SPDK for user-space NVMe access in NVMe" OFF)
option(USTORE_WITH_NVME "Compile the in-tree NVMe engine" OFF)

option(USTORE_BUILD_TEST "Compile the C++ suites in test/" OFF)
option(USTORE_BUILD_BENCH "Compile the C++ benchmarks in bench/" OFF)
option(USTORE_BUILD_TOOLS "Compile the CLI and the dataset tools in tools/" OFF)
option(USTORE_BUILD_SHARED "Compile the static and shared C libraries in the default build" OFF)
option(USTORE_BUILD_STUB "Compile a brute-force stand-in for the C 99 interface" OFF)
option(USTORE_BUILD_PYTHON "Compile the CPython extension; `scikit-build-core` switches it on" OFF)
set(USTORE_CUDA_ARCH
    ""
    CACHE STRING "NVIDIA code NVMe's GPU compaction builds for: empty, all, native or one like 100f"
)
set(USTORE_ROCM_ARCH
    ""
    CACHE STRING "AMD processors the device bundles embed code objects for: empty, all, native or a list like gfx942"
)
set(USTORE_METAL_ARCH
    ""
    CACHE STRING "Apple GPU family the Metal class units serve: empty, all or one like apple9"
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

message(STATUS "UStore: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}")

get_property(ustore_multi_config_ GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT ustore_multi_config_ AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# No instruction-set flags: artifacts run on any CPU of the target architecture.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(cmake/ustore_flags.cmake)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Install 3rd Party Packages
include(FetchContent)

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
if (${USTORE_WITH_LINUX_SPDK})
    include(cmake/spdk.cmake)
endif ()

include(cmake/ustore_siblings.cmake)
# The floor lands after the siblings, so MSVC's `native` reads which capabilities StringZilla found this host runs.
set(ustore_native_probe_library_ stringzilla)
set(ustore_native_probe_capabilities_ icelake skylake haswell neon)
include(cmake/ustore_platform.cmake)

# Modalities:
include(cmake/fmt.cmake)
include(cmake/yyjson.cmake)

# The C ABI of `c/ustore/`, which every other layer starts from.
add_library(ustore_abi INTERFACE)
add_library(ustore::abi ALIAS ustore_abi)
set_target_properties(ustore_abi PROPERTIES EXPORT_NAME abi)
target_include_directories(
    ustore_abi INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c>
                         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The C++ SDK of `cpp/ustore/`, over the C ABI and the archive.
add_subdirectory(cpp)

# The reusable templates of `include/ustore/`, header-only, with the include paths of the siblings and the third-party
# headers they reach into. The package installs UStore's own headers alone, so a consumer brings each sibling's.
add_library(ustore_templates INTERFACE)
add_library(ustore::templates ALIAS ustore_templates)
set_target_properties(ustore_templates PROPERTIES EXPORT_NAME templates)
# MSVC reads BOM-less UTF-8 in the system code page, so consumers of these Unicode-commented headers need `/utf-8`.
target_compile_options(
    ustore_templates INTERFACE "$<$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/utf-8>"
)
target_compile_features(ustore_templates INTERFACE cxx_std_20)
target_include_directories(
    ustore_templates
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(
    ustore_templates SYSTEM INTERFACE $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
                                      $<BUILD_INTERFACE:${fmt_SOURCE_DIR}/include>
)
target_link_libraries(
    ustore_templates INTERFACE $<BUILD_INTERFACE:forkunion::header> $<BUILD_INTERFACE:smashtable::header>
                               $<BUILD_INTERFACE:stringzilla::header> $<BUILD_INTERFACE:ustore_sanitizers>
)

include(cmake/ustore_gpu.cmake)

# The RAM and NVMe engines compile one variant per CPU class, in `src/<engine>/variants_<class>.cpp`, and their opens
# pick the newest one the running CPU admits. A class is its parent plus the ForkUnion capabilities its waiter and
# atomic reference add beyond the spin hint every core of an architecture carries and the StringZilla capabilities its
# strings kit adds, restated here row for row from `parent_class_of`, `waiter_for_cpu_class`, `atomic_ref_for_cpu_class`
# and `stringzilla_capabilities_of` of `src/lib.hpp`. It exists when its parent does and every one of those capabilities
# is enabled. A multiversioned build keeps every class that exists. A monoversioned build keeps none, the `serial` floor
# included, and compiles the one `monoversioned` class instead, with the waiter and atomic reference ForkUnion picks for
# `USTORE_MARCH` and what a preset vouches for. Every UStore unit reads the kept classes as `USTORE_TARGET_<CLASS>` set
# to 0 or 1, collected in `ustore_cpu_gates_`. ForkUnion spells each instruction in the assembly itself and
# StringZilla's kernels are symbols of its archive, so a class needs no flags of its own.
set(ustore_class_serial_exists_ TRUE)
set(ustore_class_serial_kept_ ${USTORE_MULTIVERSIONED})
set(ustore_class_serial_macro_ SERIAL)
set(ustore_cpu_rows_ serial)

# Declares the CPU class @p class_, gated as `USTORE_TARGET_<target_macro_>`, over the class named after `OVER` and the
# ForkUnion or StringZilla capabilities after it.
function (ustore_cpu_class class_ target_macro_)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "OVER" "")
    set(exists_ ${ustore_class_${arg_OVER}_exists_})
    foreach (capability_ IN LISTS arg_UNPARSED_ARGUMENTS)
        if (NOT forkunion_target_${capability_}_compiled AND NOT stringzilla_target_${capability_}_compiled)
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

ustore_cpu_class(intel_haswell INTEL_HASWELL OVER serial haswell westmere)
ustore_cpu_class(intel_skylake INTEL_SKYLAKE OVER intel_haswell skylake)
ustore_cpu_class(intel_icelake INTEL_ICELAKE OVER intel_skylake icelake goldmont)
ustore_cpu_class(
    intel_sierra
    INTEL_SIERRA
    OVER
    intel_haswell
    goldmont
    x86_tpause
    x86_cmpccxadd
)
ustore_cpu_class(
    aws_g2
    AWS_G2
    OVER
    serial
    neon
    neonaes
    neonsha
)
ustore_cpu_class(apple_m2 APPLE_M2 OVER aws_g2 arm64_lse arm64_rcpc)
ustore_cpu_class(aws_g5 AWS_G5 OVER apple_m2)
ustore_cpu_class(
    alibaba_c930
    ALIBABA_C930
    OVER
    serial
    riscv64_wrs
    riscv64_zacas
    rvv
)
ustore_cpu_class(
    ibm_power10
    IBM_POWER10
    OVER
    serial
    ppc64_pause
    ppc64_amo
    powervsx
)

# A monoversioned build compiles one class: every capability StringZilla and ForkUnion enabled, which is what
# `USTORE_MARCH` implies plus what a preset vouches for.
set(ustore_class_monoversioned_macro_ MONOVERSIONED)
set(ustore_monoversioned_capabilities_)
if (NOT USTORE_MULTIVERSIONED)
    foreach (library_ IN ITEMS stringzilla forkunion)
        get_property(enabled_ GLOBAL PROPERTY ${library_}_capabilities_compiled)
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
    src/arrow.cpp
    src/ram/engine.cpp
    src/vram/bundles.cpp
)
set(ustore_sibling_archives_ forkunion_static stringzilla_static)
set(ustore_dependencies_ ${ustore_sibling_archives_} yyjson fmt)
set(ustore_engine_archives_)
# Every engine switch reaches the units and their consumers as 0 or 1, so a header never reads an undefined name
set(ustore_engine_definitions_
    "USTORE_WITH_NVME=$<BOOL:${USTORE_WITH_NVME}>" "USTORE_WITH_ROCKSDB=$<BOOL:${USTORE_WITH_ROCKSDB}>"
    "USTORE_WITH_LEVELDB=$<BOOL:${USTORE_WITH_LEVELDB}>"
)
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
set(ustore_device_compaction_ OFF)
if (${USTORE_WITH_NVME})
    list(APPEND ustore_sources_ src/nvme/engine.cpp)
    foreach (class_ IN LISTS ustore_cpu_classes_)
        list(APPEND ustore_nvme_variants_ src/nvme/variants_${class_}.cpp)
    endforeach ()
    # Both libraries link one copy of every CPU class's engine, compiled position-independent once
    add_library(ustore_nvme_variants OBJECT ${ustore_library_exclusion_} ${ustore_nvme_variants_})
    set_target_properties(ustore_nvme_variants PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # SPDK's archives exist only in a build configured with `USTORE_WITH_LINUX_SPDK`
    target_link_libraries(
        ustore_nvme_variants PRIVATE Threads::Threads smashtable::header ${ustore_dependencies_}
                                     $<TARGET_NAME_IF_EXISTS:ustore_spdk>
    )
    list(APPEND ustore_sources_ $<TARGET_OBJECTS:ustore_nvme_variants>)
    list(APPEND ustore_compiled_objects_ ustore_nvme_variants)
    list(APPEND ustore_engine_names_ "nvme")

    # The engine's compaction on the device, whose objects both libraries take: host C++ launching the bundles' kernels
    # through the driver, so it builds everywhere and a device whose bundle carries no kernels makes no compactor
    add_library(ustore_nvme_compaction OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/nvme/compaction.cpp")
    target_link_libraries(
        ustore_nvme_compaction
        PUBLIC $<BUILD_INTERFACE:smashtable::header> $<BUILD_INTERFACE:forkunion::header>
        PRIVATE $<BUILD_INTERFACE:ustore_templates>
    )
    set_target_properties(ustore_nvme_compaction PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (ustore_cuda_codes_ OR ustore_rocm_codes_)
        set(ustore_device_compaction_ ON)
    endif ()
    list(APPEND ustore_sources_ $<TARGET_OBJECTS:ustore_nvme_compaction>)
endif ()

if (${USTORE_WITH_ROCKSDB})
    list(APPEND ustore_sources_ src_oracles/rocksdb/engine.cpp)
    list(APPEND ustore_engine_archives_ rocksdb)
    if (TARGET uring)
        list(APPEND ustore_engine_archives_ uring)
    endif ()
    list(APPEND ustore_engine_names_ "rocksdb")
endif ()

if (${USTORE_WITH_LEVELDB})
    list(APPEND ustore_sources_ src_oracles/leveldb/engine.cpp)
    list(APPEND ustore_engine_archives_ leveldb)
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
# The kernels a device loads at its first executor's spawn of its class, embedded in both libraries as one device-only
# bundle per kept GPU class, so neither links a GPU runtime. Each class's bundle is a target of its own, since a device
# pass reads its includes and definitions from its target and only the NVIDIA ones may see the CUDA toolkit's headers.
# All take the siblings' include directories without their definitions, so device code shares the host's layouts. Every
# unit reads the kept classes as `USTORE_TARGET_<CLASS>` set to 0 or 1, collected in `ustore_gpu_gates_`. A class unit
# learns its class from its vendor's gates, 1 for itself and 0 for the others, and writes StringZilla's GPU floor off,
# so its bundle holds UStore's kernels alone over the building blocks StringZilla opens wherever the vendor compiles
set(ustore_gpu_gates_)
set(ustore_vram_objects_)
foreach (class_ IN LISTS ustore_nvidia_classes_ ustore_amd_classes_ ustore_apple_classes_)
    set(macro_ ${ustore_class_${class_}_macro_})
    list(APPEND ustore_gpu_gates_ "USTORE_TARGET_${macro_}=$<BOOL:${ustore_class_${class_}_kept_}>")
    if (NOT ustore_class_${class_}_kept_)
        continue()
    endif ()
    # An Apple class unit is host C++ embedding its MSL source, which the device compiles at the class's first spawn
    if (ustore_class_${class_}_vendor_ STREQUAL "apple")
        add_library(ustore_vram_${class_} OBJECT src/vram/${ustore_class_${class_}_source_})
        target_compile_features(ustore_vram_${class_} PRIVATE cxx_std_20)
        target_include_directories(
            ustore_vram_${class_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/c ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_compile_definitions(ustore_vram_${class_} PRIVATE ${ustore_class_${class_}_gates_})
        target_compile_options(
            ustore_vram_${class_}
            PRIVATE "--embed-dir=${CMAKE_CURRENT_SOURCE_DIR}/include" "--embed-dir=${CMAKE_CURRENT_SOURCE_DIR}/src"
                    "--embed-dir=${stringzilla_SOURCE_DIR}/include"
        )
        list(APPEND ustore_vram_objects_ $<TARGET_OBJECTS:ustore_vram_${class_}>)
        continue()
    endif ()
    add_library(ustore_vram_${class_} OBJECT ${ustore_library_exclusion_})
    target_include_directories(
        ustore_vram_${class_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/c ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(
        ustore_vram_${class_} SYSTEM
        PRIVATE $<TARGET_PROPERTY:smashtable::header,INTERFACE_INCLUDE_DIRECTORIES>
                $<TARGET_PROPERTY:forkunion::header,INTERFACE_INCLUDE_DIRECTORIES>
                $<TARGET_PROPERTY:stringzilla::header,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(
        ustore_vram_${class_} PRIVATE ${ustore_class_${class_}_gates_} ${ustore_class_${class_}_floor_off_}
    )
    ustore_gpu_bundle(
        ustore_vram_${class_} ${ustore_class_${class_}_vendor_} vram_${class_}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vram/${ustore_class_${class_}_source_}" ${ustore_class_${class_}_flags_}
    )
    list(APPEND ustore_vram_objects_ $<TARGET_OBJECTS:ustore_vram_${class_}>)
endforeach ()
set_source_files_properties(src/vram/bundles.cpp PROPERTIES COMPILE_DEFINITIONS "${ustore_gpu_gates_}")
add_library(
    ustore_static STATIC ${ustore_library_exclusion_} ${ustore_sources_} ${ustore_vram_objects_}
                         ${ustore_dependency_objects_}
)
add_library(ustore::static ALIAS ustore_static)
add_library(ustore_shared SHARED ${ustore_library_exclusion_} ${ustore_sources_} ${ustore_vram_objects_})
add_library(ustore::shared ALIAS ustore_shared)
target_compile_options(
    ustore_static INTERFACE "$<$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/utf-8>"
)
target_compile_options(
    ustore_shared INTERFACE "$<$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/utf-8>"
)
set_target_properties(ustore_static PROPERTIES EXPORT_NAME static)
set_target_properties(ustore_shared PROPERTIES EXPORT_NAME shared)
# `SOVERSION` carries the major alone, the only break the C ABI admits
set_target_properties(ustore_shared PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_definitions(
    ustore_shared
    PRIVATE USTORE_EXPORTING=1
    INTERFACE USTORE_SHARED=1
)
target_link_libraries(
    ustore_static PUBLIC Threads::Threads $<BUILD_INTERFACE:smashtable::header> ${ustore_dependency_links_}
                         ${ustore_engine_archives_} $<BUILD_INTERFACE:$<TARGET_NAME_IF_EXISTS:ustore_spdk>>
)
target_link_libraries(
    ustore_shared PRIVATE Threads::Threads smashtable::header ${ustore_dependencies_} ${ustore_engine_archives_}
                          $<TARGET_NAME_IF_EXISTS:ustore_spdk>
)
# Every GPU runtime and cuFile open through `dlopen` at first use, which glibc before 2.34 keeps in `libdl`
target_link_libraries(ustore_static PUBLIC ${CMAKE_DL_LIBS})
# On Apple platforms the Metal layer calls Metal and the Objective-C runtime directly, which every Mac carries
if (USTORE_METAL_ARCH AND APPLE)
    target_link_libraries(ustore_static PUBLIC "-framework Metal" "-framework Foundation" objc)
    target_link_libraries(ustore_shared PRIVATE "-framework Metal" "-framework Foundation" objc)
endif ()
target_link_libraries(ustore_shared PRIVATE ${CMAKE_DL_LIBS})
# The shared library exports the C ABI alone. On ELF a version script does it, since the standard library's inline
# definitions, like `std::bad_variant_access`, keep default visibility under any preset, as do the archives linked
# inside
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ustore_shared.map" "{ global: ustore_*; local: *; };\n")
target_link_options(
    ustore_shared PRIVATE
    $<$<PLATFORM_ID:Linux,Android,FreeBSD>:LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/ustore_shared.map>
    $<$<PLATFORM_ID:Darwin>:LINKER:-exported_symbol,_ustore_*>
)

foreach (library_ IN ITEMS ustore_static ustore_shared ${ustore_compiled_objects_})
    target_compile_features(${library_} PUBLIC cxx_std_20)
    target_compile_definitions(
        ${library_}
        PUBLIC ${ustore_engine_definitions_}
        PUBLIC USTORE_VERSION="${ustore_version_name_}"
    )
    # Collection identifiers draw their entropy from `BCryptGenRandom` on Windows
    target_link_libraries(${library_} PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt> $<BUILD_INTERFACE:ustore_warnings>)
    # Every unit reads which CPU and GPU classes compiled and which ForkUnion and StringZilla capabilities the toolchain
    # builds, so the units instantiating a class and the open picking one name the same rungs and strings kits
    target_compile_definitions(${library_} PRIVATE ${ustore_cpu_gates_} ${ustore_gpu_gates_})
    # The Apple executor allocates, binds and waits through StringZilla's Metal layer
    if (USTORE_METAL_ARCH)
        target_compile_definitions(${library_} PRIVATE STRINGZILLA_ARCH_METAL_=1)
        target_compile_options(${library_} PRIVATE "--embed-dir=${stringzilla_SOURCE_DIR}/include")
    endif ()
    target_link_libraries(
        ${library_} PRIVATE $<BUILD_INTERFACE:forkunion::cpu_capabilities_compiled>
                            $<BUILD_INTERFACE:stringzilla::cpu_capabilities_compiled>
    )
    target_link_libraries(${library_} PUBLIC $<BUILD_INTERFACE:ustore_sanitizers>)
    # The engine sees the C ABI and its templates; a consumer of the C++ SDK links `ustore::cpp`, which carries it.
    target_include_directories(
        ${library_}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endforeach ()

if (${USTORE_BUILD_TOOLS})
    add_executable(ustore_cli tools/cli.cpp tools/dataset.cpp tools/dataset_arrow.cpp)
    target_link_libraries(ustore_cli PRIVATE ustore::static fmt::fmt ustore_warnings)
endif ()

if (${USTORE_BUILD_TEST})
    enable_testing()
    add_subdirectory(test)
endif ()

if (${USTORE_BUILD_BENCH})
    add_subdirectory(bench)
endif ()

if (USTORE_BUILD_STUB)
    add_subdirectory(src_stub)
endif ()

if (${USTORE_BUILD_PYTHON})
    add_subdirectory(python)
endif ()

# `find_package(ustore)` names the C headers `ustore::abi`, the C++ SDK carrying them and the archive `ustore::cpp`, the
# header-only templates `ustore::templates`, and the libraries `ustore::shared`, which links every engine inside, and
# `ustore::static`, beside the engine archives it links, `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`. The
# archive holds the device merger, whose consumers also link the static CUDA runtime it calls where it has one.
set(ustore_install_sdk_targets_ ustore_abi ustore_cpp ustore_templates ustore_static ustore_shared
                                ${ustore_engine_archives_}
)
set(ustore_static_dependencies_)
if (ustore_device_compaction_ AND ustore_cuda_codes_)
    set(ustore_static_dependencies_ "find_dependency(CUDAToolkit)")
endif ()
include(cmake/ustore_install.cmake)
# The header-only templates install as their own `templates` component.
if (USTORE_INSTALL)
    install(
        DIRECTORY include/ustore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT templates
    )
endif ()
