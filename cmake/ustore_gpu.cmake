# The GPU host compiler, the CUDA and ROCm codes, the Metal families, the bundles kernels embed through, and the CUDA
# toolkit.

# Clang hosts every GPU build: NVCC's device passes take it as their host compiler, where GCC trips over the device
# copies of libstdc++, and AMD's device passes are its own HIP mode.
if ((USTORE_CUDA_ARCH
     OR USTORE_ROCM_ARCH
     OR USTORE_METAL_ARCH)
    AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
)
    message(
        FATAL_ERROR
            "Every GPU build compiles its host code with Clang, found ${CMAKE_CXX_COMPILER_ID} - set CMAKE_CXX_COMPILER to clang++, and to ROCm's own clang++ for USTORE_ROCM_ARCH"
    )
endif ()

# The AMD processors a bundle embeds a code object for, as the `--offload-arch` flags of its device-only pass: `all` is
# gfx942 for CDNA3, gfx950 for CDNA4 and gfx1250 and gfx1251 for CDNA5, `native` the processors `rocm_agent_enumerator`
# lists, and anything else a list of processors.
set(ustore_rocm_architectures_ ${USTORE_ROCM_ARCH})
if (USTORE_ROCM_ARCH STREQUAL "all")
    set(ustore_rocm_architectures_ gfx942 gfx950 gfx1250 gfx1251)
elseif (USTORE_ROCM_ARCH STREQUAL "native")
    execute_process(
        COMMAND rocm_agent_enumerator
        OUTPUT_VARIABLE ustore_rocm_architectures_
        ERROR_QUIET
    )
    separate_arguments(ustore_rocm_architectures_ UNIX_COMMAND "${ustore_rocm_architectures_}")
endif ()
set(ustore_rocm_codes_)
set(ustore_amd_unit_codes_)
foreach (architecture_ IN LISTS ustore_rocm_architectures_)
    # `rocm_agent_enumerator` lists the host CPU too, as gfx000, and a processor once per GPU holding it
    if (NOT architecture_ STREQUAL "gfx000" AND NOT "--offload-arch=${architecture_}" IN_LIST ustore_rocm_codes_)
        list(APPEND ustore_rocm_codes_ --offload-arch=${architecture_})
        list(APPEND ustore_amd_unit_codes_ ${architecture_})
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
    set(includes_ "-I$<JOIN:$<TARGET_PROPERTY:${target_},INCLUDE_DIRECTORIES>,$<SEMICOLON>-I>")
    set(definitions_
        "$<$<BOOL:$<TARGET_PROPERTY:${target_},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${target_},COMPILE_DEFINITIONS>,$<SEMICOLON>-D>>"
    )
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
        set(dependencies_)
        set(depfile_flags_ -MD -MF ${bundle_}.d)
    else ()
        set(device_only_ "${CMAKE_CXX_COMPILER};-x;hip;--cuda-device-only;-c")
        set(optimization_ "$<$<CONFIG:Debug>:-O0>" "$<$<NOT:$<CONFIG:Debug>>:-O3>")
        # Clang writes no dependencies from a device-only pass, so a preprocessing pass of both sides writes them first
        set(preprocess_ "${CMAKE_CXX_COMPILER};-x;hip;-std=c++20;-M;-MT;${bundle_};-MF;${bundle_}.d")
        set(dependencies_ COMMAND ${preprocess_} ${ARGN} ${includes_} ${definitions_} ${source_})
        set(depfile_flags_)
    endif ()
    if (ARGN)
        add_custom_command(
            OUTPUT ${bundle_} ${dependencies_}
            COMMAND ${device_only_} -std=c++20 ${optimization_} ${ARGN} ${includes_} ${definitions_} ${depfile_flags_}
                    ${source_} -o ${bundle_}
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
    # The library's kernels need only plain codes, which cover Ampere through Blackwell: `all` holds a cubin per
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

# The GPU classes, each one generation of one vendor, whose bundle compiles that vendor's kernels for the configured
# codes naming its generation, so the classes partition the vendor's codes. A device runs a class where its driver loads
# the class's bundle. A code is an entry of `CMAKE_CUDA_ARCHITECTURES`, `native` read as the codes CMake found on this
# machine, or an AMD processor, whose name a target ID's features follow.
set(ustore_nvidia_unit_codes_)
if (USTORE_CUDA_ARCH AND CMAKE_CUDA_COMPILER)
    set(ustore_nvidia_unit_codes_ ${CMAKE_CUDA_ARCHITECTURES})
    if (USTORE_CUDA_ARCH STREQUAL "native")
        set(ustore_nvidia_unit_codes_ ${CMAKE_CUDA_ARCHITECTURES_NATIVE})
    endif ()
endif ()
set(ustore_nvidia_classes_)
set(ustore_amd_classes_)

# Declares the GPU class @p class_ of @p vendor_, gated as `USTORE_TARGET_<target_macro_>`, whose bundle compiles
# `src/vram/<source_>` for the configured codes of the architectures after `ARCHITECTURES`, with the library floor
# targets after `FLOOR_OFF`, which write the libraries' kernels out of it. Each vendor declares its classes oldest
# first.
function (ustore_gpu_class class_ target_macro_ vendor_ source_)
    cmake_parse_arguments(PARSE_ARGV 4 arg "" "" "FLOOR_OFF;ARCHITECTURES")
    set(codes_)
    foreach (code_ IN LISTS ustore_${vendor_}_unit_codes_)
        if (code_ MATCHES "^([^-:]+)")
            set(architecture_ ${CMAKE_MATCH_1})
        endif ()
        if (architecture_ IN_LIST arg_ARCHITECTURES AND NOT code_ IN_LIST codes_)
            list(APPEND codes_ "${code_}")
        endif ()
    endforeach ()
    set(ustore_class_${class_}_codes_
        "${codes_}"
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_macro_
        ${target_macro_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_vendor_
        ${vendor_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_source_
        ${source_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_floor_off_
        ${arg_FLOOR_OFF}
        PARENT_SCOPE
    )
    set(ustore_${vendor_}_classes_
        ${ustore_${vendor_}_classes_} ${class_}
        PARENT_SCOPE
    )
endfunction ()

# Settles the classes of @p vendor_: a configured code no class names stops the configure, as the oldest class's own PTX
# already holds NVIDIA's later GPUs. A class holding codes is kept, its codes turned into the flags of its bundle's
# device pass, but a monoversioned build keeps only the newest one, which holds every kernel. Each class gets its
# vendor's gates, 1 for itself and 0 for the others, which its unit reads its class from.
function (ustore_gpu_vendor vendor_)
    set(classes_ ${ustore_${vendor_}_classes_})
    set(named_)
    foreach (class_ IN LISTS classes_)
        list(APPEND named_ ${ustore_class_${class_}_codes_})
    endforeach ()
    foreach (code_ IN LISTS ustore_${vendor_}_unit_codes_)
        if (NOT code_ IN_LIST named_)
            message(FATAL_ERROR "UStore: no GPU class runs ${code_}; name an architecture a class claims")
        endif ()
    endforeach ()
    set(newest_)
    foreach (class_ IN LISTS classes_)
        if (ustore_class_${class_}_codes_)
            set(newest_ ${class_})
        endif ()
    endforeach ()
    foreach (class_ IN LISTS classes_)
        set(codes_ ${ustore_class_${class_}_codes_})
        if (NOT (USTORE_MULTIVERSIONED OR class_ STREQUAL newest_))
            set(codes_)
        endif ()
        set(flags_)
        foreach (code_ IN LISTS codes_)
            if (vendor_ STREQUAL "amd")
                list(APPEND flags_ --offload-arch=${code_})
            elseif (code_ MATCHES "^([0-9]+[af]?)-real$")
                list(APPEND flags_ --generate-code=arch=compute_${CMAKE_MATCH_1},code=sm_${CMAKE_MATCH_1})
            elseif (code_ MATCHES "^([0-9]+[af]?)-virtual$")
                list(APPEND flags_ --generate-code=arch=compute_${CMAKE_MATCH_1},code=compute_${CMAKE_MATCH_1})
            endif ()
        endforeach ()
        set(gates_)
        foreach (sibling_ IN LISTS classes_)
            set(own_ 0)
            if (sibling_ STREQUAL class_)
                set(own_ 1)
            endif ()
            list(APPEND gates_ "USTORE_TARGET_${ustore_class_${sibling_}_macro_}=${own_}")
        endforeach ()
        set(ustore_class_${class_}_gates_
            "${gates_}"
            PARENT_SCOPE
        )
        set(kept_ FALSE)
        if (codes_)
            set(kept_ TRUE)
        endif ()
        set(ustore_class_${class_}_kept_
            ${kept_}
            PARENT_SCOPE
        )
        set(ustore_class_${class_}_flags_
            "${flags_}"
            PARENT_SCOPE
        )
    endforeach ()
endfunction ()

ustore_gpu_class(
    nvidia_ampere NVIDIA_AMPERE nvidia vram_kernels_nvidia.cu
    FLOOR_OFF STRINGZILLA_TARGET_CUDA=0
    ARCHITECTURES 80 86
)
ustore_gpu_class(
    nvidia_ada NVIDIA_ADA nvidia vram_kernels_nvidia.cu
    FLOOR_OFF STRINGZILLA_TARGET_CUDA=0
    ARCHITECTURES 89
)
ustore_gpu_class(
    nvidia_hopper NVIDIA_HOPPER nvidia vram_kernels_nvidia.cu
    FLOOR_OFF STRINGZILLA_TARGET_CUDA=0
    ARCHITECTURES 90 90a
)
ustore_gpu_class(
    nvidia_blackwell NVIDIA_BLACKWELL nvidia vram_kernels_nvidia.cu
    FLOOR_OFF STRINGZILLA_TARGET_CUDA=0
    ARCHITECTURES 100 100a 100f 103 103a 103f
)
ustore_gpu_class(
    nvidia_blackwellrtx NVIDIA_BLACKWELLRTX nvidia vram_kernels_nvidia.cu
    FLOOR_OFF STRINGZILLA_TARGET_CUDA=0
    ARCHITECTURES 120 120a 120f 121 121a 121f
)
ustore_gpu_class(
    amd_cdna3 AMD_CDNA3 amd vram_kernels_amd.hip
    FLOOR_OFF STRINGZILLA_TARGET_ROCM=0
    ARCHITECTURES gfx942
)
ustore_gpu_class(
    amd_cdna4 AMD_CDNA4 amd vram_kernels_amd.hip
    FLOOR_OFF STRINGZILLA_TARGET_ROCM=0
    ARCHITECTURES gfx950
)
ustore_gpu_class(
    amd_cdna5 AMD_CDNA5 amd vram_kernels_amd.hip
    FLOOR_OFF STRINGZILLA_TARGET_ROCM=0
    ARCHITECTURES gfx1250 gfx1251
)
ustore_gpu_vendor(nvidia)
ustore_gpu_vendor(amd)

# Metal compiles its kernels on the device, so a family only names the Apple class it serves.
if (USTORE_METAL_ARCH STREQUAL "native")
    message(FATAL_ERROR "USTORE_METAL_ARCH takes all or one family like apple9, as Metal compiles on the device")
endif ()
if (USTORE_METAL_ARCH AND NOT USTORE_METAL_ARCH MATCHES "^(all|apple7|apple9|apple10)$")
    message(FATAL_ERROR "USTORE_METAL_ARCH takes all, apple7, apple9 or apple10, found ${USTORE_METAL_ARCH}")
endif ()

# The Apple classes, one per Metal generation, whose kernels travel as MSL source the class unit `src/vram/<source_>`
# embeds and a device compiles at the first spawn of the class, so no Metal toolchain builds them and a class holds
# no flags. A class is kept for `all` or its own @p family_, and a monoversioned build keeps only the newest kept one.
# Each class gets the Apple gates, 1 for itself and 0 for the others, which its unit reads its class from.
set(ustore_apple_classes_)
function (ustore_apple_class class_ target_macro_ source_ family_)
    set(kept_ FALSE)
    if (USTORE_METAL_ARCH STREQUAL "all" OR USTORE_METAL_ARCH STREQUAL family_)
        set(kept_ TRUE)
    endif ()
    set(ustore_class_${class_}_kept_
        ${kept_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_macro_
        ${target_macro_}
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_vendor_
        apple
        PARENT_SCOPE
    )
    set(ustore_class_${class_}_source_
        ${source_}
        PARENT_SCOPE
    )
    set(ustore_apple_classes_
        ${ustore_apple_classes_} ${class_}
        PARENT_SCOPE
    )
endfunction ()
ustore_apple_class(apple_metal7 APPLE_METAL7 vram_entries_apple.cpp apple7)
ustore_apple_class(apple_metal9 APPLE_METAL9 vram_entries_apple.cpp apple9)
ustore_apple_class(apple_metal10 APPLE_METAL10 vram_entries_apple.cpp apple10)
set(ustore_apple_newest_)
foreach (class_ IN LISTS ustore_apple_classes_)
    if (ustore_class_${class_}_kept_)
        set(ustore_apple_newest_ ${class_})
    endif ()
endforeach ()
foreach (class_ IN LISTS ustore_apple_classes_)
    if (NOT (USTORE_MULTIVERSIONED OR class_ STREQUAL ustore_apple_newest_))
        set(ustore_class_${class_}_kept_ FALSE)
    endif ()
    set(ustore_class_${class_}_gates_)
    foreach (sibling_ IN LISTS ustore_apple_classes_)
        set(own_ 0)
        if (sibling_ STREQUAL class_)
            set(own_ 1)
        endif ()
        list(APPEND ustore_class_${class_}_gates_ "USTORE_TARGET_${ustore_class_${sibling_}_macro_}=${own_}")
    endforeach ()
endforeach ()
