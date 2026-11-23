# The header-only C++ SDK over the C interface: `cpp/ustore/*.hpp` include only the C headers `ustore::abi` carries
# and the standard library, which the interface target enforces, so a consumer linking `ustore::cpp` gets the archive
# and sees no engine header.
add_library(ustore_cpp INTERFACE)
add_library(ustore::cpp ALIAS ustore_cpp)
set_target_properties(ustore_cpp PROPERTIES EXPORT_NAME cpp)
target_include_directories(
    ustore_cpp INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/cpp> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(ustore_cpp INTERFACE ustore::abi ustore::static)
target_compile_features(ustore_cpp INTERFACE cxx_std_20)

# The smoke test against the engine, linked as the C one is.
if (USTORE_BUILD_TEST)
    add_executable(ustore_test_cpp test.cpp)
    target_link_libraries(ustore_test_cpp PRIVATE ustore::cpp ustore_static fmt::fmt ustore_warnings ustore_sanitizers)
    add_test(NAME ustore_test_cpp COMMAND ustore_test_cpp)
endif ()

# The same test against the stand-in, which builds without the engine, so an SDK header reaching past `c/` fails to
# compile here. It takes the SDK's headers rather than `ustore::cpp`, which would link the engine's archive.
if (USTORE_BUILD_TEST AND USTORE_BUILD_STUB)
    add_executable(ustore_test_cpp_stub test.cpp)
    target_include_directories(ustore_test_cpp_stub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(ustore_test_cpp_stub PRIVATE cxx_std_20)
    target_link_libraries(
        ustore_test_cpp_stub PRIVATE ustore::abi ustore_static_stub fmt::fmt ustore_warnings ustore_sanitizers
    )
    add_test(NAME ustore_test_cpp_stub COMMAND ustore_test_cpp_stub)
endif ()
