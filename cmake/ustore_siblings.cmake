# The sibling helper, the switches forwarded to the siblings, and the siblings themselves.

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

# The siblings build for the floor and the build kind UStore builds for.
foreach (sibling_ IN ITEMS FORKUNION STRINGZILLA SMASHTABLE)
    set(${sibling_}_MARCH "${USTORE_MARCH}")
    set(${sibling_}_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
endforeach ()
# StringZilla's CUDA engines match the values of filtered scans on NVIDIA executors, for the codes UStore builds, over
# the static runtime CMake's own `CMAKE_CUDA_RUNTIME_LIBRARY` names, which the shared library keeps local, so it still
# names no GPU runtime as needed. CUDA 13 keeps CCCL under `include/cccl`, so the toolkit's include path never shadows
# the pinned one. Its ROCm tier stays off, as it would link the HIP runtime.
if (USTORE_CUDA_ARCH)
    # A toolkit installed outside the PATH, which is where the distribution packages put it
    if (NOT DEFINED CMAKE_CUDA_COMPILER)
        find_program(USTORE_NVCC nvcc HINTS /usr/local/cuda/bin /opt/cuda/bin)
        if (USTORE_NVCC)
            set(CMAKE_CUDA_COMPILER "${USTORE_NVCC}")
        endif ()
    endif ()
    include(CheckLanguage)
    check_language(CUDA)
    if (CMAKE_CUDA_COMPILER)
        set(STRINGZILLA_CUDA_ARCH "${USTORE_CUDA_ARCH}")
        set(CMAKE_CUDA_RUNTIME_LIBRARY Static)
    endif ()
endif ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    53b2c33f7fd2f06c5c6e36de8104ec7a19a66696
)
# Every UStore thread runs inside ForkUnion's compiled pool, whose barriers ThreadSanitizer sees only once that archive
# is instrumented as well
if (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    target_link_libraries(forkunion_static PRIVATE ustore_sanitizers)
endif ()

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    3ad4d089bfd8d3e5700e7bd81108d2183cead4dd
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    54ddcf0bc679b79226a6373859ff6181a1f064c2
)
