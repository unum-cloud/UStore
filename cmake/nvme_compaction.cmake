# UStoreNVMe's compaction: the CUDA runtime library, and one suite built twice to check the GPU against the host
set(ustore_compaction_test_source_ "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme/compaction.cu")

# The suite as plain C++, so the host path builds and runs without a CUDA compiler
set(ustore_compaction_host_source_ "${CMAKE_CURRENT_BINARY_DIR}/test/nvme/compaction_host.cpp")
configure_file("${ustore_compaction_test_source_}" "${ustore_compaction_host_source_}" COPYONLY)
add_executable(ustore_test_nvme_compaction_host "${ustore_compaction_host_source_}")
target_include_directories(ustore_test_nvme_compaction_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_compile_features(ustore_test_nvme_compaction_host PRIVATE cxx_std_20)
# A fold's scratch arena allocates on ForkUnion's memory domains, which the host compactor reaches
target_link_libraries(
    ustore_test_nvme_compaction_host PRIVATE ustore_header smashtable::header forkunion::static stringzilla::header
                                             fmt::fmt ustore_warnings
)
if (USTORE_BUILD_TEST)
    add_test(NAME ustore_test_nvme_compaction_host COMMAND ustore_test_nvme_compaction_host)
endif ()

# The database suites run as plain C++ everywhere, and merge on the device through the compactor linked further down
# once the build carries one
set(ustore_nvme_device_suites_ database_transaction single_collection_database_transaction
                               single_collection_database_stress single_collection_database_transaction_stress
)
if (USTORE_BUILD_NVME AND USTORE_BUILD_TEST)
    foreach (suite_name_ IN ITEMS ${ustore_nvme_device_suites_})
        add_executable(ustore_test_nvme_${suite_name_} "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme/${suite_name_}.cpp")
        target_include_directories(ustore_test_nvme_${suite_name_} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
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
        add_test(NAME ustore_test_nvme_${suite_name_} COMMAND ustore_test_nvme_${suite_name_} --directory
                                                              ${CMAKE_BINARY_DIR}/test_data
        )
    endforeach ()
endif ()

# The GPU half is opt-in, so a plain test build takes the host suite and leaves the toolkit alone
if (NOT USTORE_BUILD_CUDA)
    return()
endif ()

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
        WARNING "UStore: no CUDA compiler found, so only the host compaction suite builds - install CUDA 13 or newer"
    )
    return()
endif ()

# The same codes on every machine, rather than whatever GPU the building one carries
set(CMAKE_CUDA_ARCHITECTURES ${USTORE_CUDA_ARCHITECTURES})
enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/cccl.cmake")

add_library(ustore_nvme_compaction STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/nvme/compaction.cu")
target_link_libraries(
    ustore_nvme_compaction
    PUBLIC CCCL::CUB CUDA::cudart ustore_header smashtable::header forkunion::header
    PRIVATE stringzilla::static
)
set_target_properties(ustore_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)
# The host pass reads nvcc's preprocessed output, where every header is the main file and every macro is expanded, so
# constant operands from capability macros and unused statics of the C headers would warn there and nowhere else
target_compile_options(
    ustore_nvme_compaction
    PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wno-constant-logical-operand,-Wno-unneeded-internal-declaration>
)
# A constexpr function is callable from a kernel, so the shared headers carry no host-device qualifier
target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr>)
# StringZilla's serial kernels, reached through the engine's key slots, carry GCC pragmas nvcc's front end does not know
target_compile_options(ustore_nvme_compaction PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--diag-suppress=1675>)
# Every target linking the library merges on the device, and every other one takes the host branch of the merger
target_compile_definitions(ustore_nvme_compaction PUBLIC USTORE_WITH_CUDA=1)

# The device half of the suite is the same source as host C++ over the compiled compactor, since nvcc's front end
# crashes instantiating StringZilla's string, which the harness reaches; one CUDA TU instantiates the suite's key slots
add_executable(
    ustore_test_nvme_compaction "${ustore_compaction_host_source_}"
                                "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme/compaction_device.cu"
)
target_include_directories(ustore_test_nvme_compaction PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_compile_features(ustore_test_nvme_compaction PRIVATE cxx_std_20)
set_target_properties(ustore_test_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
target_link_libraries(
    ustore_test_nvme_compaction PRIVATE ustore_nvme_compaction forkunion::static fmt::fmt ustore_warnings
)
if (USTORE_BUILD_TEST)
    add_test(NAME ustore_test_nvme_compaction COMMAND ustore_test_nvme_compaction)
endif ()

# The database suites merge on the device where it exists, through the same compactor the engine links
if (USTORE_BUILD_NVME AND USTORE_BUILD_TEST)
    foreach (suite_name_ IN ITEMS ${ustore_nvme_device_suites_})
        target_link_libraries(ustore_test_nvme_${suite_name_} PRIVATE ustore_nvme_compaction)
    endforeach ()
endif ()
