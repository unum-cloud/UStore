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

# The siblings build for the floor and the build kind UStore builds for. StringZilla's GPU backends stay off, as the
# compaction calls none and their toolkit headers would shadow the pinned CCCL in every unit linking StringZilla.
foreach (sibling_ IN ITEMS FORKUNION STRINGZILLA SMASHTABLE)
    set(${sibling_}_MARCH "${USTORE_MARCH}")
    set(${sibling_}_MULTIVERSIONED "${USTORE_MULTIVERSIONED}")
endforeach ()

# ForkUnion's C ABI, linked statically into every engine and suite
ustore_sibling(
    forkunion FETCHCONTENT_SOURCE_DIR_FORKUNION https://github.com/ashvardanian/ForkUnion.git
    5fac4b88fcd2f611db41055da2b20e1e765449a2
)
# Every UStore thread runs inside ForkUnion's compiled pool, whose barriers ThreadSanitizer sees only once that archive
# is instrumented as well
if (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    target_link_libraries(forkunion_static PRIVATE ustore_sanitizers)
endif ()

# SmashTable's containers, header-only
ustore_sibling(
    smashtable FETCHCONTENT_SOURCE_DIR_SMASHTABLE https://github.com/ashvardanian/SmashTable.git
    fe3d32edb4685b925e62a427ef6ebdf74b311af9
)

# StringZilla's compiled core, so a portable build without instruction-set flags still dispatches to the fastest kernel
# the running CPU supports. NumKong is unused today; wiring it in later is the same add_subdirectory plus a
# numkong::header link, once something includes its headers.
ustore_sibling(
    stringzilla FETCHCONTENT_SOURCE_DIR_STRINGZILLA https://github.com/ashvardanian/StringZilla.git
    a43a398cecd159f6c383843756540b898a45bee7
)
