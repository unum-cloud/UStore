# UStoreNVMe's compaction: the CUDA runtime library, and one suite built twice to check the GPU against the host
set(USTORE_COMPACTION_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme_compaction.cu")

# The suite as plain C++, so the host path builds and runs without a CUDA compiler
set(USTORE_COMPACTION_HOST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/test/nvme_compaction_host.cpp")
configure_file("${USTORE_COMPACTION_TEST_SOURCE}" "${USTORE_COMPACTION_HOST_SOURCE}" COPYONLY)
add_executable(ustore_test_nvme_compaction_host "${USTORE_COMPACTION_HOST_SOURCE}")
target_include_directories(ustore_test_nvme_compaction_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_compile_features(ustore_test_nvme_compaction_host PRIVATE cxx_std_20)
target_link_libraries(
    ustore_test_nvme_compaction_host PRIVATE smashtable::header forkunion::header fmt::fmt ustore_warnings
)
if (USTORE_BUILD_TEST)
    add_test(NAME ustore_test_nvme_compaction_host COMMAND ustore_test_nvme_compaction_host)
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
    PUBLIC CCCL::CUB CUDA::cudart smashtable::header forkunion::header
    PRIVATE stringzilla::static
)
set_target_properties(ustore_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)
# A constexpr function is callable from a kernel, so the shared headers carry no host-device qualifier
target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr>)
# StringZilla's serial kernels, reached through the engine's key slots, carry GCC pragmas nvcc's front end does not know
target_compile_options(ustore_nvme_compaction PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--diag-suppress=1675>)
# Every target linking the library merges on the device, and every other one takes the host branch of the merger
target_compile_definitions(ustore_nvme_compaction PUBLIC USTORE_WITH_CUDA=1)

add_executable(ustore_test_nvme_compaction "${USTORE_COMPACTION_TEST_SOURCE}")
target_include_directories(ustore_test_nvme_compaction PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_link_libraries(ustore_test_nvme_compaction PRIVATE ustore_nvme_compaction fmt::fmt)
set_target_properties(ustore_test_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
if (USTORE_BUILD_TEST)
    add_test(NAME ustore_test_nvme_compaction COMMAND ustore_test_nvme_compaction)
endif ()
