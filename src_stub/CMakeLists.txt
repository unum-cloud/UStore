# The brute-force stand-in for the C interface: a separate library defining the same symbols a different way, so any
# consumer - a binding suite, a downstream SDK, a bisect - runs in seconds against the ABI instead of waiting on a build
# of the engine. It needs the C headers and the C++ standard library, and nothing else - no SmashTable, no StringZilla,
# no yyjson, no engine sources - so it skips the engine's target setup deliberately and sets by hand the few properties
# consumers depend on. The one runtime it does resolve is the platform's threads, since the stand-ins keep the headers'
# concurrency promises with a `std::shared_mutex` rather than a comment.
#
# The file names match what the root produces - `libustore_shared` and `libustore_static` - and sit in a directory of
# their own, so a suite chooses an implementation with one `-L` and the two libraries can never shadow one another.
set(ustore_stub_sources_
    types.cpp
    db.cpp
    blobs.cpp
    strings.cpp
    docs.cpp
    graph.cpp
    pages.cpp
    arrow.cpp
)

find_package(Threads REQUIRED)

add_library(ustore_shared_stub SHARED ${ustore_stub_sources_})
add_library(ustore_static_stub STATIC ${ustore_stub_sources_})
add_library(${PROJECT_NAME}::shared_stub ALIAS ustore_shared_stub)
add_library(${PROJECT_NAME}::static_stub ALIAS ustore_static_stub)

foreach (target_ ustore_shared_stub ustore_static_stub)
    target_include_directories(
        ${target_}
        PUBLIC ${PROJECT_SOURCE_DIR}/c
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${target_} PUBLIC Threads::Threads)
    # The stand-in answers the full spelling of `VERSION`, a candidate's suffix included, as the root reads it, and
    # hands its consumers the same `USTORE_VERSION` the engine's library does
    target_compile_definitions(
        ${target_}
        PRIVATE USTORE_EXPORTING=1 USTORE_STUB_VERSION="${ustore_version_name_}"
        INTERFACE USTORE_VERSION="${ustore_version_name_}"
    )
    set_target_properties(
        ${target_}
        PROPERTIES PREFIX "lib"
                   POSITION_INDEPENDENT_CODE ON
                   CXX_STANDARD 20
                   CXX_STANDARD_REQUIRED ON
                   CXX_VISIBILITY_PRESET hidden
                   LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
                   ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
                   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
    )
    # The root pins every configuration's directory too, and those outrank the plain ones above.
    foreach (configuration_ DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(
            ${target_}
            PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${configuration_} "${CMAKE_BINARY_DIR}/stub"
                       ARCHIVE_OUTPUT_DIRECTORY_${configuration_} "${CMAKE_BINARY_DIR}/stub"
                       RUNTIME_OUTPUT_DIRECTORY_${configuration_} "${CMAKE_BINARY_DIR}/stub"
        )
    endforeach ()
endforeach ()

set_target_properties(ustore_shared_stub PROPERTIES OUTPUT_NAME "ustore_shared")
target_compile_definitions(ustore_shared_stub INTERFACE USTORE_SHARED=1)
set_target_properties(ustore_static_stub PROPERTIES OUTPUT_NAME "ustore_static")

# The C suites linked against the static stand-in: the one the engine's library runs too, and the one checking every
# refusal and the compaction plan through the stand-in alone. They are written in C, but the library they link is
# C++, so the C++ driver links them and brings the standard library along.
if (USTORE_BUILD_TEST)
    add_executable(ustore_test_c_stub ${PROJECT_SOURCE_DIR}/c/test.c)
    add_executable(ustore_test_stub ${CMAKE_CURRENT_SOURCE_DIR}/test.c)
    foreach (stub_test_ ustore_test_c_stub ustore_test_stub)
        target_link_libraries(${stub_test_} PRIVATE ustore_static_stub)
        target_compile_definitions(${stub_test_} PRIVATE USTORE_TEST_STUB=1)
        set_target_properties(
            ${stub_test_}
            PROPERTIES C_STANDARD 11
                       C_STANDARD_REQUIRED ON
                       LINKER_LANGUAGE CXX
                       RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
        )
        add_test(NAME ${stub_test_} COMMAND ${stub_test_})
    endforeach ()
endif ()

# A stand-in is not part of the published package, so it stays out of the export set and installs under a subdirectory
# of its own, where it cannot shadow the engine's library.
if (USTORE_INSTALL)
    install(
        TARGETS ustore_shared_stub ustore_static_stub
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ustore-stub
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/ustore-stub
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/ustore-stub
    )
endif ()
