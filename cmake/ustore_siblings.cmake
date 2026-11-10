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

# The siblings build for the floor, the build kind and the sanitizer UStore builds for, so ThreadSanitizer sees their
# barriers and ForkUnion drops the atomics it cannot see. StringZilla's own GPU tiers stay off, as the library's bundles
# walk its values and they would link a GPU runtime.
foreach (sibling_ IN ITEMS FORKUNION STRINGZILLA SMASHTABLE)
    set(${sibling_}_MARCH "${USTORE_MARCH}")
    set(${sibling_}_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
    set(${sibling_}_BUILD_SANITIZERS "${USTORE_BUILD_SANITIZERS}")
endforeach ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    ce67be6511ce8cfc4b139b3e7266ec0e5edc372c
)

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    30e349dcd1e582896386d959d4a36badce061787
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    ff22abca2e1f271f37bd8cb10174bc9dd37ed4e7
)
