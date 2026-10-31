# The CPython extension `ustore.lib`, which `scikit-build-core` drives from `pyproject.toml`.

find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module)

# Module glue first, then the shared vocabulary, then one file per header of the C ABI.
python_add_library(
    ustore_python
    MODULE
    WITH_SOABI
    lib.c
    types.c
    db.c
    blobs.c
    docs.c
    graph.c
    pages.c
    strings.c
    collection.c
    arrow.c
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

target_include_directories(ustore_python PRIVATE ${PROJECT_SOURCE_DIR}/c ${CMAKE_CURRENT_SOURCE_DIR})

# The one library carries every engine the build enables, with the `USTORE_WITH_*` switches `engines_compiled` reads,
# and documents convert to and from Python objects through the yyjson it already carries.
target_link_libraries(ustore_python PRIVATE ustore::static yyjson::yyjson ustore_warnings)
# The module's one export is its init function; the archives linked inside, UStore's own and its siblings', fmt and
# yyjson among them, keep their symbols to themselves.
set_target_properties(ustore_python PROPERTIES C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(
    ustore_python PRIVATE $<$<PLATFORM_ID:Linux,Android,FreeBSD>:LINKER:--exclude-libs,ALL>
    $<$<PLATFORM_ID:Darwin>:LINKER:-exported_symbol,_PyInit_lib>
)

# Named, or every dependency's own install rules land in the wheel beside the extension.
install(TARGETS ustore_python LIBRARY DESTINATION ustore COMPONENT ustore_python)
