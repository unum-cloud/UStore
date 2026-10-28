# The CUDA compiler, its codes and toolkit, and NVMe's compaction library on the device.

# NVMe's compaction on the device, the CUDA runtime library every engine and suite links once the build carries it. The
# GPU half is opt-in, so a build without it leaves the toolkit alone.
if (USTORE_CUDA_ARCH)
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
if (USTORE_CUDA_ARCH AND CMAKE_CUDA_COMPILER)
    # CUB is all the compaction kernels call, so plain codes cover Ampere through Blackwell: `all` carries a cubin per
    # generation and the floor's PTX for every later GPU, `native` the code of each visible GPU, and a named code its
    # cubin alone.
    if (USTORE_CUDA_ARCH STREQUAL "all")
        set(CMAKE_CUDA_ARCHITECTURES 80-real 90-real 100-real 120-real 80-virtual)
    elseif (USTORE_CUDA_ARCH STREQUAL "native")
        set(CMAKE_CUDA_ARCHITECTURES native)
    elseif (USTORE_CUDA_ARCH MATCHES "^[0-9]+[af]?$")
        set(CMAKE_CUDA_ARCHITECTURES ${USTORE_CUDA_ARCH}-real)
    else ()
        message(FATAL_ERROR "USTORE_CUDA_ARCH takes all, native or one code like 100f, not `${USTORE_CUDA_ARCH}`")
    endif ()
    if (CMAKE_VERSION VERSION_LESS 3.31 AND CMAKE_CUDA_ARCHITECTURES MATCHES "1[0-9][0-9][af]")
        message(FATAL_ERROR "Three-digit \"a\" and \"f\" CUDA codes need CMake 3.31, found ${CMAKE_VERSION}")
    endif ()
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
