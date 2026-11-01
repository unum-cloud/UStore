# The GPU host compiler, the CUDA and ROCm codes, the bundles kernels embed through, and the CUDA toolkit.

# Clang hosts every GPU build: NVCC's device passes take it as their host compiler, where GCC trips over the device
# copies of libstdc++, and AMD's device passes are its own HIP mode.
if ((USTORE_CUDA_ARCH OR USTORE_ROCM_ARCH) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(
        FATAL_ERROR
            "Every GPU build compiles its host code with Clang, found ${CMAKE_CXX_COMPILER_ID} - set CMAKE_CXX_COMPILER to clang++, and to ROCm's own clang++ for USTORE_ROCM_ARCH"
    )
endif ()

# The AMD processors a bundle embeds a code object for, as the `--offload-arch` flags of its device-only pass: `all` is
# gfx942 for CDNA3, gfx950 for CDNA4 and gfx1250 for CDNA5, `native` the processors `rocm_agent_enumerator` lists, and
# anything else a list of processors.
set(ustore_rocm_architectures_ ${USTORE_ROCM_ARCH})
if (USTORE_ROCM_ARCH STREQUAL "all")
    set(ustore_rocm_architectures_ gfx942 gfx950 gfx1250)
elseif (USTORE_ROCM_ARCH STREQUAL "native")
    execute_process(
        COMMAND rocm_agent_enumerator
        OUTPUT_VARIABLE ustore_rocm_architectures_
        ERROR_QUIET
    )
    separate_arguments(ustore_rocm_architectures_ UNIX_COMMAND "${ustore_rocm_architectures_}")
endif ()
set(ustore_rocm_codes_)
foreach (architecture_ IN LISTS ustore_rocm_architectures_)
    # `rocm_agent_enumerator` lists the host CPU too, as gfx000, and a processor once per GPU carrying it
    if (NOT architecture_ STREQUAL "gfx000" AND NOT "--offload-arch=${architecture_}" IN_LIST ustore_rocm_codes_)
        list(APPEND ustore_rocm_codes_ --offload-arch=${architecture_})
    endif ()
endforeach ()
if (USTORE_ROCM_ARCH STREQUAL "native" AND NOT ustore_rocm_codes_)
    message(FATAL_ERROR "USTORE_ROCM_ARCH=native found no GPU through rocm_agent_enumerator; name its processor")
endif ()

# Every NVIDIA and AMD bundle links into its target through an assembler stub.
if (USTORE_CUDA_ARCH OR USTORE_ROCM_ARCH)
    enable_language(ASM)
endif ()

# Every NVIDIA or AMD unit with kernels compiles once, device-only, NVCC for NVIDIA and Clang for AMD, into a bundle of
# the codes the remaining arguments name, a CUDA fatbin or a HIP offload bundle, with @p target_'s includes and
# definitions. No target compiles its host half, so nothing registers with or links a GPU runtime, which opens at first
# use. The bundle links into @p target_ page-aligned in a read-only section through an assembler stub, under
# `<name>_bundle_start` and `<name>_bundle_end`, so it travels into archives too. With no codes the bundle is empty, and
# its devices refuse to load it.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS ustore_gpu_device_pass=4)
function (ustore_gpu_bundle target_ vendor_ name_ source_)
    set(bundle_ ${CMAKE_CURRENT_BINARY_DIR}/bundles/${target_}/${name_}.bundle)
    if (vendor_ STREQUAL "nvidia")
        set(host_compiler_ ${CMAKE_CXX_COMPILER})
        if (CMAKE_CUDA_HOST_COMPILER)
            set(host_compiler_ ${CMAKE_CUDA_HOST_COMPILER})
        endif ()
        # NVCC's default leaves Turing and Ampere cubins uncompressed, ten times their compressed size.
        set(device_only_ "${CUDAToolkit_NVCC_EXECUTABLE};-x;cu;--fatbin;--compress-mode=size;-ccbin;${host_compiler_}"
                         --expt-relaxed-constexpr
        )
        set(optimization_ "$<$<CONFIG:Debug>:-O0$<SEMICOLON>-Xcicc$<SEMICOLON>-O0$<SEMICOLON>-Xptxas$<SEMICOLON>-O0>"
                          "$<$<NOT:$<CONFIG:Debug>>:-O3>"
        )
        target_include_directories(${target_} SYSTEM PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    else ()
        set(device_only_ "${CMAKE_CXX_COMPILER};-x;hip;--cuda-device-only;-c")
        set(optimization_ "$<$<CONFIG:Debug>:-O0>" "$<$<NOT:$<CONFIG:Debug>>:-O3>")
    endif ()
    if (ARGN)
        add_custom_command(
            OUTPUT ${bundle_}
            COMMAND
                ${device_only_} -std=c++20 ${optimization_} ${ARGN}
                "-I$<JOIN:$<TARGET_PROPERTY:${target_},INCLUDE_DIRECTORIES>,;-I>"
                "$<$<BOOL:$<TARGET_PROPERTY:${target_},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${target_},COMPILE_DEFINITIONS>,;-D>>"
                -MD -MF ${bundle_}.d ${source_} -o ${bundle_}
            DEPENDS ${source_}
            DEPFILE ${bundle_}.d
            COMMENT "Compiling the ${name_} kernels"
            JOB_POOL ustore_gpu_device_pass
            COMMAND_EXPAND_LISTS VERBATIM
        )
    else ()
        file(WRITE ${bundle_} "")
    endif ()
    set(stub_ ${CMAKE_CURRENT_BINARY_DIR}/bundles/${target_}/${name_}_bundle.S)
    file(
        WRITE ${stub_}
        ".section .rodata\n.balign 4096\n.globl ${name_}_bundle_start\n${name_}_bundle_start:\n"
        ".incbin \"${bundle_}\"\n.globl ${name_}_bundle_end\n${name_}_bundle_end:\n"
        ".section .note.GNU-stack,\"\",@progbits\n"
    )
    set_source_files_properties(${stub_} PROPERTIES LANGUAGE ASM OBJECT_DEPENDS ${bundle_})
    target_sources(${target_} PRIVATE ${stub_})
endfunction ()

# The CUDA language, its codes and its toolkit, for the NVIDIA bundles and StringZilla's CUDA engines. The GPU half is
# opt-in, so a build without it leaves the toolkit alone. The siblings probed for nvcc, outside the PATH too, before
# StringZilla enabled CUDA, and that probe's cached answer is the one this module reads.
if (USTORE_CUDA_ARCH AND NOT CMAKE_CUDA_COMPILER)
    message(
        WARNING "UStore: no CUDA compiler found, so only the host compaction suite builds - install CUDA 13 or newer"
    )
endif ()
if (USTORE_CUDA_ARCH AND CMAKE_CUDA_COMPILER)
    # The library's kernels need only plain codes, which cover Ampere through Blackwell: `all` carries a cubin per
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
    # The same codes as NVCC's flags, for the device-only passes of `ustore_gpu_bundle`, where CUDA is no language
    set(ustore_cuda_codes_)
    foreach (architecture_ IN LISTS CMAKE_CUDA_ARCHITECTURES)
        if (architecture_ STREQUAL "native")
            list(APPEND ustore_cuda_codes_ -arch=native)
        elseif (architecture_ MATCHES "^([0-9]+[af]?)-real$")
            list(APPEND ustore_cuda_codes_ --generate-code=arch=compute_${CMAKE_MATCH_1},code=sm_${CMAKE_MATCH_1})
        elseif (architecture_ MATCHES "^([0-9]+[af]?)-virtual$")
            list(APPEND ustore_cuda_codes_ --generate-code=arch=compute_${CMAKE_MATCH_1},code=compute_${CMAKE_MATCH_1})
        endif ()
    endforeach ()
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
endif ()
