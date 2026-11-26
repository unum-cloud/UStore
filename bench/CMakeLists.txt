# Every benchmark reads the harness, which parses its settings through `tools/arguments.hpp` and reads the templates of
# `include/ustore/`, so every benchmark links `ustore_header`; those over the C++ SDK link `ustore::cpp`, which carries
# the library.
include_directories(${PROJECT_SOURCE_DIR}/tools)
link_libraries(ustore_header)

# Benchmarks, each picking its engine with `--engine`
add_executable(ustore_bench_twitter twitter.cpp)
target_link_libraries(ustore_bench_twitter PRIVATE ustore::static ustore::cpp fmt::fmt ustore_warnings)

add_executable(
    ustore_bench_tabular_graph tabular_graph.cpp ${PROJECT_SOURCE_DIR}/tools/dataset.cpp
                               ${PROJECT_SOURCE_DIR}/tools/dataset_arrow.cpp
)
target_link_libraries(ustore_bench_tabular_graph PRIVATE ustore::static ustore::cpp fmt::fmt ustore_warnings)

add_executable(ustore_bench_louvain louvain.cpp)
target_link_libraries(ustore_bench_louvain PRIVATE ustore::static ustore::cpp fmt::fmt ustore_warnings)

# The docs paths benchmark times the templates alone, so it links StringZilla's compiled library and no engine
add_executable(ustore_bench_docs_paths docs_paths.cpp)
target_link_libraries(
    ustore_bench_docs_paths
    PRIVATE ustore::header
            smashtable::header
            forkunion::cpp
            stringzilla::static
            yyjson
            fmt::fmt
            ustore_warnings
            ustore_sanitizers
)

# The single-collection database benchmark drives the NVMe templates over volumes of its own, with no engine, and
# links the compactor its merges reach, as the storage suites do
if (${USTORE_WITH_NVME})
    # The PubChem ingest opens the engine through the C ABI alone, so one source measures every layout shipped
    add_executable(ustore_bench_nvme_pubchem nvme_pubchem.cpp)
    target_link_libraries(ustore_bench_nvme_pubchem PRIVATE ustore::static ustore::cpp fmt::fmt ustore_warnings)

    add_executable(ustore_bench_compaction compaction.cpp)
    target_link_libraries(ustore_bench_compaction PRIVATE ustore::static ustore::cpp fmt::fmt ustore_warnings)

    add_executable(ustore_bench_nvme_single_collection_database nvme_single_collection_database.cpp)
    target_link_libraries(
        ustore_bench_nvme_single_collection_database
        PRIVATE ustore::header
                forkunion::static
                smashtable::header
                stringzilla::static
                yyjson
                fmt::fmt
                Threads::Threads
                ustore_nvme_compaction
                ustore_warnings
    )
endif ()
