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

# The siblings tune their own units for the same CPU, and probe what this host runs under `native`.
if (USTORE_TARGET_ARCH)
    set(FORKUNION_TARGET_ARCH ${USTORE_TARGET_ARCH})
    set(STRINGZILLA_TARGET_ARCH ${USTORE_TARGET_ARCH})
endif ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    0b9296ebe0dc3886f4f850f0dc97d6ce382ac909
)
# Every UStore thread runs inside ForkUnion's compiled pool, whose barriers ThreadSanitizer sees only once that archive
# is instrumented as well
if (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    target_link_libraries(forkunion_static PRIVATE ustore_sanitizers)
endif ()

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    e45080c7ea61a5a9c7fc90ac8b9595b49b341574
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    53fb6a67b188561ffcab506ee3d03901716b1b49
)
