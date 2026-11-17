# The suites, each over every engine this build compiles; `enable_testing()` stays in the root, before this directory.
set(ustore_abi_names_
    "database"
    "executors"
    "cursors"
    "blobs"
    "docs"
    "graph"
    "pages"
    "arrow"
    "strings"
)
list(TRANSFORM ustore_abi_names_ PREPEND "abi/" OUTPUT_VARIABLE ustore_abi_paths_)
set(ustore_test_paths_ ${ustore_abi_paths_} "compilation" "strings/collection")
# Every suite reaching the one GPU the machine shares, which CTest runs one at a time
set(ustore_gpu_tests_)

# The storage suites build with the NVMe engine they test. The storage ring stands outside the per-engine loop: it
# links no engine, only the siblings it is written against.
if (${USTORE_WITH_NVME})
    set(ustore_storage_test_names_
        "ring"
        "channels"
        "volume"
        "sorted_run"
        "tree"
        "collection"
        "transaction_registry"
        "database"
        "ingest"
        "single_collection_database"
    )

    # The io_uring, Linux AIO, SPDK and cuFile transports are Linux-only and take no option of their own; each suite
    # skips itself where the kernel or the build refuses its transport.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND ustore_storage_test_names_ "linux_uring_disk" "linux_aio_disk" "linux_spdk_disk"
             "linux_cufile_disk"
        )
    endif ()

    # The SPDK suite starts and stops its own target through the rig script, so the script's path is compiled in and
    # `--spdk-root` names the build the script runs; without the option the suite prints its skip line instead.
    foreach (storage_name_ IN ITEMS ${ustore_storage_test_names_})
        set(storage_exe_ ustore_test_nvme_${storage_name_})
        add_executable(${storage_exe_} nvme/${storage_name_}.cpp)
        target_compile_features(${storage_exe_} PRIVATE cxx_std_20)
        target_include_directories(${storage_exe_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(
            ${storage_exe_}
            PRIVATE ustore::header
                    forkunion::static
                    smashtable::header
                    stringzilla::static
                    yyjson
                    fmt::fmt
                    Threads::Threads
                    $<$<PLATFORM_ID:Windows>:bcrypt>
                    ustore_warnings
        )
        set(storage_arguments_ --directory ${CMAKE_BINARY_DIR}/test_data)
        # Every suite reaching the tree's merger links the compactor, which makes no lane without a CUDA compiler
        target_link_libraries(${storage_exe_} PRIVATE ustore_nvme_compaction)
        # The suites counting allocations share one replacement of the global allocation functions
        if (storage_name_ MATCHES "^(tree|database)$")
            target_sources(${storage_exe_} PRIVATE nvme/allocation_watch.cpp)
        endif ()
        # The LSM suite merges on the device where the build carries one, launching the kernels the library embeds,
        # so it links the bundles, and on the transports that need no hardware option its device case may no longer
        # skip
        if (storage_name_ STREQUAL "tree")
            target_sources(${storage_exe_} PRIVATE ${PROJECT_SOURCE_DIR}/src/vram/bundles.cpp ${ustore_vram_objects_})
        endif ()
        if (storage_name_ STREQUAL "tree" AND ustore_device_compaction_)
            list(APPEND storage_arguments_ --no-skips --transports memory,blocking,linux-uring)
        endif ()
        if (storage_name_ STREQUAL "linux_spdk_disk")
            target_compile_definitions(
                ${storage_exe_} PRIVATE USTORE_SPDK_RIG="${PROJECT_SOURCE_DIR}/scripts/rigs/spdk_target.sh"
            )
            if (${USTORE_WITH_LINUX_SPDK})
                target_link_libraries(${storage_exe_} PRIVATE ustore_spdk)
                list(APPEND storage_arguments_ --spdk-root ${USTORE_SPDK_ROOT})
            endif ()
        endif ()
        add_test(NAME ${storage_exe_} COMMAND ${storage_exe_} ${storage_arguments_})
        if (storage_name_ MATCHES "^(tree|database|linux_cufile_disk)$")
            list(APPEND ustore_gpu_tests_ ${storage_exe_})
        endif ()
    endforeach ()

    # The database suites run as plain C++ everywhere, and merge on the device through the compactor once the build
    # carries one
    set(ustore_nvme_device_suites_ database_transaction single_collection_database_transaction
                                   single_collection_database_stress single_collection_database_transaction_stress
    )
    foreach (suite_name_ IN ITEMS ${ustore_nvme_device_suites_})
        add_executable(ustore_test_nvme_${suite_name_} nvme/${suite_name_}.cpp)
        target_include_directories(ustore_test_nvme_${suite_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(ustore_test_nvme_${suite_name_} PRIVATE cxx_std_20)
        target_link_libraries(
            ustore_test_nvme_${suite_name_}
            PRIVATE ustore_header
                    forkunion::static
                    smashtable::header
                    stringzilla::static
                    yyjson
                    fmt::fmt
                    Threads::Threads
                    $<$<PLATFORM_ID:Windows>:bcrypt>
                    ustore_warnings
        )
        target_link_libraries(ustore_test_nvme_${suite_name_} PRIVATE ustore_nvme_compaction)
        add_test(NAME ustore_test_nvme_${suite_name_} COMMAND ustore_test_nvme_${suite_name_} --directory
                                                              ${CMAKE_BINARY_DIR}/test_data
        )
    endforeach ()

    # The graph store lives in the engine's database, so its suite drives it through the C ABI of the engine
    add_executable(ustore_test_nvme_graph nvme/graph.cpp)
    target_include_directories(ustore_test_nvme_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_nvme_graph PRIVATE ustore::static fmt::fmt Threads::Threads ustore_warnings)
    add_test(NAME ustore_test_nvme_graph COMMAND ustore_test_nvme_graph --directory ${CMAKE_BINARY_DIR}/test_data)

    # The pages store too, checked against the in-memory engine
    add_executable(ustore_test_nvme_pages nvme/pages.cpp)
    target_include_directories(ustore_test_nvme_pages PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_nvme_pages PRIVATE ustore::static fmt::fmt ustore_warnings)
    add_test(NAME ustore_test_nvme_pages COMMAND ustore_test_nvme_pages --directory ${CMAKE_BINARY_DIR}/test_data)
endif ()

# The compaction suite as plain C++ checks the comparator, the survival rule, the sorted-run split and the placement
# without a device, over a compactor that makes no lane, so it builds without a CUDA compiler
set(ustore_compaction_host_source_ "${CMAKE_CURRENT_BINARY_DIR}/nvme/compaction_host.cpp")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/nvme/compaction.cu" "${ustore_compaction_host_source_}" COPYONLY)
add_executable(
    ustore_test_nvme_compaction_host "${ustore_compaction_host_source_}"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/nvme/compaction_absent.cpp"
)
target_include_directories(ustore_test_nvme_compaction_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ustore_test_nvme_compaction_host PRIVATE cxx_std_20)
# A fold's scratch arena allocates on ForkUnion's memory domains, which the host compactor reaches
target_link_libraries(
    ustore_test_nvme_compaction_host PRIVATE ustore_header smashtable::header forkunion::static stringzilla::header
                                             fmt::fmt ustore_warnings
)
add_test(NAME ustore_test_nvme_compaction_host COMMAND ustore_test_nvme_compaction_host)

# The device half of the suite is the same source over the compiled compactor, whose kernels the library's bundle
# carries, so it links the library for them; one unit instantiates the compactor for its key slots
if (ustore_device_compaction_)
    add_executable(
        ustore_test_nvme_compaction "${ustore_compaction_host_source_}"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/nvme/compaction_device.cpp"
    )
    target_include_directories(ustore_test_nvme_compaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(ustore_test_nvme_compaction PRIVATE cxx_std_20)
    target_link_libraries(ustore_test_nvme_compaction PRIVATE ustore::static ustore_header fmt::fmt ustore_warnings)
    add_test(NAME ustore_test_nvme_compaction COMMAND ustore_test_nvme_compaction)
    list(APPEND ustore_gpu_tests_ ustore_test_nvme_compaction)
endif ()

# The shared library opens every GPU runtime at first use, so it names none as needed. It builds by default only
# with `USTORE_BUILD_SHARED` and CTest builds nothing, so the test requires its file, failing on a missing one
# instead of passing the `grep`
if (USTORE_BUILD_SHARED AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME ustore_test_links_no_gpu_runtime
             COMMAND sh -c "! readelf -d \"$0\" | grep -E 'NEEDED.*(libcudart|libcuda\\.|libamdhip64|libcufile)'"
                     $<TARGET_FILE:ustore_shared>
    )
    set_tests_properties(ustore_test_links_no_gpu_runtime PROPERTIES REQUIRED_FILES $<TARGET_FILE:ustore_shared>)
endif ()

# Every read verb on each listed GPU against the host, once per engine on the one GPU the device suites share
add_executable(ustore_test_vram vram/towers.cpp)
target_include_directories(ustore_test_vram PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ustore_test_vram PRIVATE ustore::static fmt::fmt ustore_warnings)
foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
    add_test(NAME ustore_test_vram_${engine_name_} COMMAND ustore_test_vram --engine ${engine_name_} --directory
                                                           ${CMAKE_BINARY_DIR}/test_data
    )
    list(APPEND ustore_gpu_tests_ ustore_test_vram_${engine_name_})
endforeach ()

# A stand-in HIP runtime reporting one gfx942 device drives the AMD routing on machines without AMD hardware,
# through a copy of the suite whose bundles unit gives the CDNA3 class a bundle the stand-in loads
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(ustore_fake_amdhip64 SHARED vram/fake_amdhip64.cpp)
    set_target_properties(
        ustore_fake_amdhip64 PROPERTIES OUTPUT_NAME amdhip64 LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fake_amdhip64
    )
    add_executable(ustore_test_vram_fake_amd vram/towers.cpp vram/fake_amd_bundles.cpp)
    set_source_files_properties(vram/fake_amd_bundles.cpp PROPERTIES COMPILE_DEFINITIONS "${ustore_gpu_gates_}")
    target_include_directories(ustore_test_vram_fake_amd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_vram_fake_amd PRIVATE ustore::static fmt::fmt ustore_warnings)
    add_dependencies(ustore_test_vram_fake_amd ustore_fake_amdhip64)
    add_test(NAME ustore_test_vram_fake_amd COMMAND ustore_test_vram_fake_amd --engine ram --no-skips --filter
                                                    "^vram\\.amd"
    )
    set_tests_properties(
        ustore_test_vram_fake_amd PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/fake_amdhip64"
    )
    list(APPEND ustore_gpu_tests_ ustore_test_vram_fake_amd)
endif ()
set_tests_properties(${ustore_gpu_tests_} PROPERTIES RESOURCE_LOCK gpu0)

# The class bundles hold UStore's kernels alone: no StringZilla kernel or device table rides along.
if ((USTORE_CUDA_ARCH AND CMAKE_CUDA_COMPILER) OR USTORE_ROCM_ARCH)
    if (USTORE_CUDA_ARCH AND CMAKE_CUDA_COMPILER)
        find_program(ustore_test_cuobjdump_ cuobjdump HINTS ${CUDAToolkit_BIN_DIR} REQUIRED)
    endif ()
    if (USTORE_ROCM_ARCH)
        find_program(ustore_test_offload_bundler_ clang-offload-bundler HINTS /opt/rocm/llvm/bin REQUIRED)
        find_program(ustore_test_readelf_ llvm-readelf HINTS /opt/rocm/llvm/bin REQUIRED)
    endif ()
    add_test(NAME ustore_test_gpu_bundles
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bundles.sh ${CMAKE_BINARY_DIR}/bundles "${ustore_test_cuobjdump_}"
                     "${ustore_test_offload_bundler_}" "${ustore_test_readelf_}" ${ustore_amd_unit_codes_}
    )
endif ()

set(ustore_stress_names_ "atomicity" "linearizability")

# Every suite builds once and runs once per engine, picking it with `--engine`
foreach (test_path_ IN ITEMS ${ustore_test_paths_})
    string(REPLACE "/" "_" test_name_ ${test_path_})
    add_executable(ustore_test_${test_name_} ${test_path_}.cpp)
    target_include_directories(ustore_test_${test_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_${test_name_} PRIVATE ustore::static fmt::fmt ustore_warnings)
    foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
        add_test(NAME ustore_test_${test_name_}_${engine_name_}
                 COMMAND ustore_test_${test_name_} --engine ${engine_name_} --directory ${CMAKE_BINARY_DIR}/test_data
        )
    endforeach ()
endforeach ()

# The kernel ABI suite compares the Linux mirrors to the kernel's own headers, which only Linux has
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ustore_utilities_test_names_ "ram_buffers" "algorithm" "kernel_abi")
else ()
    set(ustore_utilities_test_names_ "ram_buffers" "algorithm")
endif ()

# The templates of `include/ustore/` build against the header target alone, with no engine linked
foreach (test_name_ IN ITEMS ${ustore_utilities_test_names_})
    add_executable(ustore_test_utilities_${test_name_} utilities/${test_name_}.cpp)
    target_include_directories(ustore_test_utilities_${test_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
    add_executable(ustore_test_${test_name_} ${test_path_}.cpp)
    target_include_directories(ustore_test_${test_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
add_executable(ustore_test_graph_adjacency_fold graph/adjacency_fold.cpp)
target_link_libraries(ustore_test_graph_adjacency_fold PRIVATE ustore::header fmt::fmt ustore_warnings)
target_include_directories(ustore_test_graph_adjacency_fold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ustore_test_graph_adjacency_fold COMMAND ustore_test_graph_adjacency_fold)

# The runs of strings build against the header target alone
add_executable(ustore_test_strings_nvme_memory_run strings/nvme_memory_run.cpp)
target_link_libraries(
    ustore_test_strings_nvme_memory_run PRIVATE ustore::header forkunion::static fmt::fmt ustore_warnings
)
target_include_directories(ustore_test_strings_nvme_memory_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ustore_test_strings_nvme_memory_run COMMAND ustore_test_strings_nvme_memory_run)

# Memory runs and their views call no StringZilla kernel either, and need only SmashTable's headers
add_executable(ustore_test_blobs_nvme_memory_run blobs/nvme_memory_run.cpp)
target_link_libraries(ustore_test_blobs_nvme_memory_run PRIVATE ustore::header fmt::fmt ustore_warnings)
target_include_directories(ustore_test_blobs_nvme_memory_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ustore_test_blobs_nvme_memory_run COMMAND ustore_test_blobs_nvme_memory_run)

# The docs fold is checked against `ustore_docs_write` on every engine, so its suite links them all
add_executable(ustore_test_docs_json_fold docs/json_fold.cpp)
target_include_directories(ustore_test_docs_json_fold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ustore_test_docs_json_fold PRIVATE ustore::static yyjson fmt::fmt ustore_warnings)
foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
    add_test(NAME ustore_test_docs_json_fold_${engine_name_}
             COMMAND ustore_test_docs_json_fold --engine ${engine_name_} --directory ${CMAKE_BINARY_DIR}/test_data
    )
endforeach ()

# The C ABI compiles as plain C11, so a header that only a C++ compiler accepts fails to build. It links against
# both libraries, and the shared one exports only what `USTORE_EXPORT` marks.
add_executable(ustore_test_c ${PROJECT_SOURCE_DIR}/c/test.c)
add_executable(ustore_test_c_shared ${PROJECT_SOURCE_DIR}/c/test.c)
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
        ${c_test_} PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Werror;-Werror=incompatible-pointer-types>"
    )
    add_test(NAME ${c_test_} COMMAND ${c_test_})
endforeach ()

# RAM stages half-edges and watches its reads only under snapshot isolation, so the C ABI suites run there as well
set(ustore_snapshot_arguments_ --engine ram --isolation snapshot --directory ${CMAKE_BINARY_DIR}/test_data)
foreach (abi_name_ IN ITEMS ${ustore_abi_names_})
    add_test(NAME ustore_test_abi_${abi_name_}_ram_snapshot COMMAND ustore_test_abi_${abi_name_}
                                                                    ${ustore_snapshot_arguments_}
    )
endforeach ()

foreach (stress_name_ IN ITEMS ${ustore_stress_names_})
    add_executable(ustore_stress_${stress_name_} abi/stress_${stress_name_}.cpp)
    target_include_directories(ustore_stress_${stress_name_} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_stress_${stress_name_} PRIVATE ustore::static fmt::fmt ustore_warnings)
    foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
        add_test(NAME ustore_stress_${stress_name_}_${engine_name_}
                 COMMAND ustore_stress_${stress_name_} --engine ${engine_name_} --directory
                         ${CMAKE_BINARY_DIR}/test_data
        )
    endforeach ()
endforeach ()

# The tools suite drives the CLI on the same engine, and the codec of the tools builds against the headers alone
if (${USTORE_BUILD_TOOLS})
    add_executable(ustore_test_tools_arrow_ipc tools/arrow_ipc.cpp)
    target_include_directories(ustore_test_tools_arrow_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_tools_arrow_ipc PRIVATE ustore::header forkunion::static fmt::fmt ustore_warnings)
    add_test(NAME ustore_test_tools_arrow_ipc COMMAND ustore_test_tools_arrow_ipc)
    add_executable(
        ustore_test_tools tools/dataset.cpp ${PROJECT_SOURCE_DIR}/tools/dataset.cpp
                          ${PROJECT_SOURCE_DIR}/tools/dataset_arrow.cpp
    )
    target_include_directories(ustore_test_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ustore_test_tools PRIVATE ustore::static fmt::fmt ustore_warnings)
    add_dependencies(ustore_test_tools ustore_cli)
    foreach (engine_name_ IN ITEMS ${ustore_engine_names_})
        add_test(NAME ustore_test_tools_${engine_name_}
                 COMMAND ustore_test_tools --engine ${engine_name_} --directory ${CMAKE_BINARY_DIR}/test_data --cli
                         $<TARGET_FILE:ustore_cli>
        )
    endforeach ()
endif ()

# The CUDA driver and cuFile keep what they allocate at first use, which LeakSanitizer would charge to every suite
# probing the GPU tables
if (USTORE_BUILD_SANITIZERS STREQUAL "address")
    get_property(
        ustore_registered_tests_
        DIRECTORY
        PROPERTY TESTS
    )
    set_tests_properties(
        ${ustore_registered_tests_}
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "LSAN_OPTIONS=string_prepend:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/leak_sanitizer.supp:"
    )
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
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "TSAN_OPTIONS=string_prepend:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/thread_sanitizer.supp:"
    )
endif ()
