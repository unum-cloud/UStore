# UStoreNVMe's compaction: the CUDA runtime library, and one suite built twice to check the GPU against the host
set(USTORE_COMPACTION_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test/nvme_compaction.cu")

# The suite as plain C++, so the host path builds and runs without a CUDA compiler
set(USTORE_COMPACTION_HOST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/test/nvme_compaction_host.cpp")
configure_file("${USTORE_COMPACTION_TEST_SOURCE}" "${USTORE_COMPACTION_HOST_SOURCE}" COPYONLY)
add_executable(ustore_test_nvme_compaction_host "${USTORE_COMPACTION_HOST_SOURCE}")
target_include_directories(ustore_test_nvme_compaction_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_link_libraries(ustore_test_nvme_compaction_host ${LIB_FMT} ustore_warnings smashtable::header forkunion::header)
if (USTORE_BUILD_TEST_CPP)
    add_test(NAME ustore_test_nvme_compaction_host COMMAND ustore_test_nvme_compaction_host)
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

# Hopper, Blackwell data center and Blackwell workstation GPUs, plus the GPUs of the building machine
if (NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 90 100 120)
    find_program(USTORE_NVIDIA_SMI nvidia-smi HINTS /usr/lib/wsl/lib)
    if (USTORE_NVIDIA_SMI)
        execute_process(
            COMMAND "${USTORE_NVIDIA_SMI}" --query-gpu=compute_cap --format=csv,noheader
            OUTPUT_VARIABLE ustore_compute_capabilities
            RESULT_VARIABLE ustore_nvidia_smi_status
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        )
        if (ustore_nvidia_smi_status EQUAL 0)
            string(REPLACE "." "" ustore_compute_capabilities "${ustore_compute_capabilities}")
            string(REPLACE "\n" ";" ustore_compute_capabilities "${ustore_compute_capabilities}")
            list(APPEND CMAKE_CUDA_ARCHITECTURES ${ustore_compute_capabilities})
            list(REMOVE_DUPLICATES CMAKE_CUDA_ARCHITECTURES)
        endif ()
    endif ()
endif ()
enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/cccl.cmake")

add_library(ustore_nvme_compaction STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/nvme/compaction.cu")
target_link_libraries(ustore_nvme_compaction PUBLIC CCCL::CUB CUDA::cudart smashtable::header forkunion::header)
set_target_properties(ustore_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
target_compile_options(ustore_nvme_compaction PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)

add_executable(ustore_test_nvme_compaction "${USTORE_COMPACTION_TEST_SOURCE}")
target_include_directories(ustore_test_nvme_compaction PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
target_link_libraries(ustore_test_nvme_compaction ${LIB_FMT} ustore_nvme_compaction)
set_target_properties(ustore_test_nvme_compaction PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
if (USTORE_BUILD_TEST_CPP)
    add_test(NAME ustore_test_nvme_compaction COMMAND ustore_test_nvme_compaction)
endif ()
