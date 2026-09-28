# The CPython extension `ustore.lib`, which `scikit-build-core` drives from `pyproject.toml`.

find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module)

# Module glue first, then the shared vocabulary, then one file per header of the C ABI.
python_add_library(
    ustore_python
    MODULE
    WITH_SOABI
    lib.c
    types.c
    database.c
    blobs.c
    docs.c
    graph.c
    strings.c
)

# `ustore/__init__.py` imports it as `ustore.lib`, so the file name is part of the interface. The engines are C++, so
# the linker is told which driver pulls in the standard library they need.
set_target_properties(
    ustore_python
    PROPERTIES OUTPUT_NAME "lib"
               C_STANDARD 11
               C_STANDARD_REQUIRED ON
               C_EXTENSIONS OFF
               LINKER_LANGUAGE CXX
)

target_include_directories(ustore_python PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})

# A fast-call verb reaches its method table through a cast CPython's own headers prescribe, and no spelling of it
# satisfies the compiler, so the warning is silenced where the tables live.
if (NOT MSVC)
    target_compile_options(ustore_python PRIVATE -Wno-cast-function-type)
endif ()

# The one library carries every engine the build enables, with the `USTORE_WITH_*` switches `engines_compiled` reads.
target_link_libraries(ustore_python PRIVATE ustore ustore_warnings)

# Named, or every dependency's own install rules land in the wheel beside the extension.
install(TARGETS ustore_python LIBRARY DESTINATION ustore COMPONENT ustore_python)
