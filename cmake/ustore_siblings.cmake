# The sibling helper, the switches forwarded to the siblings, and the siblings themselves.

include(FetchContent)

# A sibling builds in place from its submodule when a recursive clone checked one out, or else from the commit that
# submodule pins, fetched, so a plain clone or a `FetchContent` of UStore still configures.
# The caller names the sibling's `FETCHCONTENT_SOURCE_DIR_<NAME>`, so nothing derives it.
# A non-empty one points the sibling at a local checkout, as in a workspace keeping the siblings side by side, and
# outranks the submodule.
# A parent project that already added the sibling keeps its copy.
# Every sibling is a system dependency that builds only what UStore's targets link, so its own warnings never fail a
# build under USTORE_WERROR.
function (ustore_sibling name_ override_ repository_ tag_)
    if (TARGET ${name_}::header)
        return()
    endif ()
    if (EXISTS "${PROJECT_SOURCE_DIR}/${name_}/CMakeLists.txt" AND NOT ${override_})
        add_subdirectory("${PROJECT_SOURCE_DIR}/${name_}" "${PROJECT_BINARY_DIR}/${name_}" EXCLUDE_FROM_ALL SYSTEM)
        return()
    endif ()
    message(STATUS "ustore: ${name_} from its local checkout, or else fetched at ${tag_}")
    FetchContent_Declare(
        ${name_}
        GIT_REPOSITORY ${repository_}
        GIT_TAG ${tag_}
        EXCLUDE_FROM_ALL SYSTEM
    )
    FetchContent_MakeAvailable(${name_})
endfunction ()

# The siblings build for the floor, the build kind and the sanitizer UStore builds for, each taking only the
# switches it reads. ThreadSanitizer then sees their barriers, and ForkUnion leaves out the instructions it cannot see.
set(NUMKONG_MARCH "${USTORE_MARCH}")
set(NUMKONG_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
set(NUMKONG_BUILD_SANITIZERS "${USTORE_BUILD_SANITIZERS}")
set(STRINGZILLA_MARCH "${USTORE_MARCH}")
set(STRINGZILLA_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
set(STRINGZILLA_BUILD_SANITIZERS "${USTORE_BUILD_SANITIZERS}")
set(FORKUNION_MARCH "${USTORE_MARCH}")
set(FORKUNION_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
set(FORKUNION_BUILD_SANITIZERS "${USTORE_BUILD_SANITIZERS}")
set(SMASHTABLE_BUILD_SANITIZERS "${USTORE_BUILD_SANITIZERS}")

# Everything below is this project's own: the siblings it builds on, at the commits their submodules pin.

# StringZilla compiles its Metal layer, the unified allocator and the registry the Apple executor binds through, for
# the families UStore serves.
set(STRINGZILLA_METAL_ARCH "${USTORE_METAL_ARCH}")

# A toolkit installed outside the PATH, which is where the distribution packages put it
if (USTORE_CUDA_ARCH)
    if (NOT DEFINED CMAKE_CUDA_COMPILER)
        find_program(USTORE_NVCC nvcc HINTS /usr/local/cuda/bin /opt/cuda/bin)
        if (USTORE_NVCC)
            set(CMAKE_CUDA_COMPILER "${USTORE_NVCC}")
        endif ()
    endif ()
    include(CheckLanguage)
    check_language(CUDA)
endif ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    2797304869337c41500e8e2d1ca9bbc278c351c5
)

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    36ec681da1874d63ef4b57114405df00e426728a
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::types link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    e0bf6b066bd7f966e6eb302458c4f2d863e77f2b
)
