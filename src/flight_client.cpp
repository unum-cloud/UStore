/**
 * @file flight_client.cpp
 * @author Ashot Vardanian
 *
 * @brief Client library for Apache Arrow RPC server.
 * Converts native UStore operations into Arrows classical `DoPut`, `DoExchange`...
 * Understanding the costs of remote communication, might keep a cache.
 */

#include <thread>      // `std::this_thread`
#include <mutex>       // `std::mutex`
#include <string_view> // `std::string_view`

#include <fmt/core.h> // `fmt::format_to`
#include <arrow/c/abi.h>
#include <arrow/flight/client.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>

#include "ustore/db.h"
#include "ustore/arrow.h"
#include "ustore/cpp/types.hpp" // `ustore_doc_field()`
#include "helpers/arrow.hpp"

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

ustore_collection_t const ustore_collection_main_k = 0;
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = std::numeric_limits<ustore_key_t>::max();
bool const ustore_supports_transactions_k = true;
bool const ustore_supports_named_collections_k = true;
bool const ustore_supports_snapshots_k = true;

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ustore;
using namespace unum;

struct rpc_client_t {
    std::unique_ptr<arf::FlightClient> flight;
    std::vector<std::unique_ptr<arf::FlightStreamReader>> readers;
    linked_memory_t arena;
    std::mutex arena_lock;
};

arf::FlightCallOptions arrow_call_options(arrow_mem_pool_t& pool) {
    arf::FlightCallOptions options;
    options.read_options = arrow_read_options(pool);
    options.write_options = arrow_write_options(pool);
    options.memory_manager = ar::CPUDevice::memory_manager(&pool);
    return options;
}

void export_options(ustore_options_t options, std::string& cmd) {
    if (options & ustore_option_read_shared_memory_k)
        fmt::format_to(std::back_inserter(cmd), "{}&", kParamFlagSharedMemRead);
    if (options & ustore_option_transaction_dont_watch_k)
        fmt::format_to(std::back_inserter(cmd), "{}&", kParamFlagDontWatch);

    // This flag shouldn't be forwarded to the server.
    // In standalone builds it only applies to the client.
    // if (options & ustore_option_dont_discard_memory_k)
    //     fmt::format_to(std::back_inserter(cmd), "{}&", kParamFlagDontDiscard);
}

/*********************************************************/
/*****************	    C Interface 	  ****************/
/*********************************************************/

void ustore_database_init(ustore_database_init_t* c_ptr) {

    ustore_database_init_t& c = *c_ptr;

    safe_section("Starting client", c.error, [&] {
        if (!c.config || !std::strlen(c.config))
            c.config = "grpc://0.0.0.0:38709";

        auto db_ptr = new rpc_client_t {};
        auto maybe_location = arf::Location::Parse(c.config);
        return_error_if_m(maybe_location.ok(), c.error, args_wrong_k, "Server URI");

        auto maybe_flight_ptr = arf::FlightClient::Connect(*maybe_location);
        return_error_if_m(maybe_flight_ptr.ok(), c.error, network_k, "Flight Client Connection");

        linked_memory(reinterpret_cast<ustore_arena_t*>(&db_ptr->arena), ustore_option_dont_discard_memory_k, c.error);
        return_error_if_m(maybe_location.ok(), c.error, args_wrong_k, "Failed to allocate default arena.");
        db_ptr->flight = maybe_flight_ptr.MoveValueUnsafe();
        *c.db = db_ptr;
    });
}

void ustore_read(ustore_read_t* c_ptr) {

    ustore_read_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places {collections, keys, {}, c.tasks_count};

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const request_only_presences = c.presences && !c.lengths && !c.values;
    bool const request_only_lengths = c.lengths && !c.values;
    char const* partial_mode = request_only_presences //
                                   ? kParamReadPartPresences.c_str()
                                   : request_only_lengths //
                                         ? kParamReadPartLengths.c_str()
                                         : nullptr;

    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightRead);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamSnapshotID, c.snapshot);
    if (same_named_collection)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    if (partial_mode)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamReadPart, partial_mode);
    export_options(c.options, descriptor.cmd);

    bool const has_collections_column = collections && !same_collection;
    constexpr bool has_keys_column = true;

    // If all requests map to the same collection, we can avoid passing its ID
    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.count, c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.count, continuous.begin());
        collections = {continuous.begin(), sizeof(ustore_collection_t)};
    }

    // When exporting keys, make sure they are properly strided
    if (has_keys_column && !keys.is_continuous()) {
        auto continuous = arena.alloc<ustore_key_t>(places.count, c.error);
        return_if_error_m(c.error);
        transform_n(keys, places.count, continuous.begin());
        keys = {continuous.begin(), sizeof(ustore_key_t)};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c, output_array_c;
    ArrowSchema input_schema_c, output_schema_c;
    auto count_collections = has_collections_column + has_keys_column;
    ustore_to_arrow_schema(places.count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_keys_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            "keys",
            ustore_doc_field<ustore_key_t>(),
            nullptr,
            nullptr,
            keys.get(),
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    if (batch_ptr->num_rows() == 0)
        return;
    ar::Result<arf::FlightClient::DoExchangeResult> result = db.flight->DoExchange(options, descriptor);
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    ar_status = result->writer->Begin(batch_ptr->schema());
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto input_table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*input_table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // Requesting `ToTable` might be more efficient than concatenating and
    // reallocating directly from our arena, as the underlying Arrow implementation
    // may know the length of the entire dataset.
    auto maybe_table = result->reader->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();
    return_error_if_m(table->num_columns() == 1, c.error, error_unknown_k, "Expecting one column");

    if (request_only_presences) {
        auto array = std::static_pointer_cast<ar::NumericArray<ar::UInt8Type>>(table->column(0)->chunk(0));
        *c.presences = (ustore_octet_t*)array->raw_values();
    }
    else if (request_only_lengths) {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(0)->chunk(0));
        auto presences_ptr = (ustore_octet_t*)array->null_bitmap_data();
        auto lens_ptr = (ustore_length_t*)array->value_offsets()->data();
        if (c.lengths)
            *c.lengths =
                presences_ptr //
                    ? arrow_replace_missing_scalars(presences_ptr, lens_ptr, table->num_rows(), ustore_length_missing_k)
                    : lens_ptr;
        if (c.presences)
            *c.presences = presences_ptr;
    }
    else {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(0)->chunk(0));
        auto presences_ptr = (ustore_octet_t*)array->null_bitmap_data();
        auto offs_ptr = (ustore_length_t*)array->value_offsets()->data();
        auto data_ptr = (ustore_bytes_ptr_t)array->value_data()->data();

        if (c.presences)
            *c.presences = presences_ptr;
        if (c.offsets)
            *c.offsets = offs_ptr;
        if (c.values)
            *c.values = data_ptr;

        if (c.lengths) {
            auto lens = *c.lengths = arena.alloc<ustore_length_t>(places.count, c.error).begin();
            return_if_error_m(c.error);
            if (presences_ptr) {
                auto presences = bits_view_t(presences_ptr);
                for (std::size_t i = 0; i != places.count; ++i)
                    lens[i] = presences[i] ? (offs_ptr[i + 1] - offs_ptr[i]) : ustore_length_missing_k;
            }
            else {
                for (std::size_t i = 0; i != places.count; ++i)
                    lens[i] = offs_ptr[i + 1] - offs_ptr[i];
            }
        }
    }

    db.readers.push_back(std::move(result->reader));
}

void ustore_write(ustore_write_t* c_ptr) {

    ustore_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_key_t const> keys {c.keys, c.keys_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> vals {c.values, c.values_stride};
    strided_iterator_gt<ustore_length_t const> offs {c.offsets, c.offsets_stride};
    strided_iterator_gt<ustore_length_t const> lens {c.lengths, c.lengths_stride};
    bits_view_t presences {c.presences};

    places_arg_t places {collections, keys, {}, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const write_flush = c.options & ustore_option_write_flush_k;

    bool const has_collections_column = collections && !same_collection;
    constexpr bool has_keys_column = true;
    bool const has_contents_column = vals != nullptr;

    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.size(), continuous.begin());
        collections = {continuous.begin(), places.size()};
    }

    if (has_keys_column && !keys.is_continuous()) {
        auto continuous = arena.alloc<ustore_key_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(keys, places.size(), continuous.begin());
        keys = {continuous.begin(), places.size()};
    }

    // Check if the input is continuous and is already in an Arrow-compatible form
    ustore_bytes_cptr_t joined_vals_begin = vals ? vals[0] : nullptr;
    if (has_contents_column && !contents.is_continuous()) {
        size_t total = transform_reduce_n(contents, places.size(), 0ul, std::mem_fn(&value_view_t::size));
        auto joined_vals = arena.alloc<byte_t>(total, c.error);
        return_if_error_m(c.error);
        auto joined_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        size_t slots_count = divide_round_up<std::size_t>(places.size(), CHAR_BIT);
        auto slots_presences = arena.alloc<ustore_octet_t>(slots_count, c.error);
        return_if_error_m(c.error);
        std::memset(slots_presences.begin(), 0, slots_count);
        auto joined_presences = bits_span_t(slots_presences.begin());

        // Exports into the Arrow-compatible form
        ustore_length_t exported_bytes = 0;
        for (std::size_t i = 0; i != c.tasks_count; ++i) {
            auto value = contents[i];
            joined_presences[i] = value;
            joined_offs[i] = exported_bytes;
            std::memcpy(joined_vals.begin() + exported_bytes, value.begin(), value.size());
            exported_bytes += value.size();
        }
        joined_offs[places.size()] = exported_bytes;

        joined_vals_begin = (ustore_bytes_cptr_t)joined_vals.begin();
        vals = {&joined_vals_begin, 0};
        offs = {joined_offs.begin(), sizeof(ustore_key_t)};
        presences = {slots_presences.begin()};
    }
    // It may be the case, that we only have `c.tasks_count` offsets instead of `c.tasks_count+1`,
    // which won't be enough for Arrow.
    else if (has_contents_column && !contents.is_arrow()) {
        auto joined_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        size_t slots_count = divide_round_up<std::size_t>(places.size(), CHAR_BIT);
        auto slots_presences = arena.alloc<ustore_octet_t>(slots_count, c.error);
        return_if_error_m(c.error);
        std::memset(slots_presences.begin(), 0, slots_count);
        auto joined_presences = bits_span_t(slots_presences.begin());

        // Exports into the Arrow-compatible form
        ustore_length_t exported_bytes = 0;
        for (std::size_t i = 0; i != c.tasks_count; ++i) {
            auto value = contents[i];
            joined_presences[i] = value;
            joined_offs[i] = exported_bytes;
            exported_bytes += value.size();
        }
        joined_offs[places.size()] = exported_bytes;

        vals = {&joined_vals_begin, 0};
        offs = {joined_offs.begin(), sizeof(ustore_key_t)};
        presences = {slots_presences.begin()};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c;
    ArrowSchema input_schema_c;
    auto count_collections = has_collections_column + has_keys_column + has_contents_column;
    ustore_to_arrow_schema(c.tasks_count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_keys_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            "keys",
            ustore_doc_field<ustore_key_t>(),
            nullptr,
            nullptr,
            keys.get(),
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    if (has_contents_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgVals.c_str(),
            ustore_doc_field<value_view_t>(),
            presences.get(),
            offs.get(),
            joined_vals_begin,
            input_schema_c.children[has_collections_column + has_keys_column],
            input_array_c.children[has_collections_column + has_keys_column],
            c.error);
    return_if_error_m(c.error);

    // Send everything over the network and wait for the response
    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightWrite);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    if (!has_collections_column && collections)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    if (write_flush)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}&", kParamFlagFlushWrite);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    ar::Result<arf::FlightClient::DoPutResult> result = db.flight->DoPut(options, descriptor, batch_ptr->schema());
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    // This writer has already been started!
    // ar_status = result->writer->Begin(batch_ptr->schema());
    // return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // std::shared_ptr<ar::Buffer> response;
    // ar_status = result->reader->ReadMetadata(&response);
    // return_error_if_m(ar_status.ok(), c.error, network_k, "No response");
}

void ustore_paths_write(ustore_paths_write_t* c_ptr) {

    ustore_paths_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_length_t const> path_offs {c.paths_offsets, c.paths_offsets_stride};
    strided_iterator_gt<ustore_length_t const> path_lens {c.paths_lengths, c.paths_lengths_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> paths {reinterpret_cast<ustore_bytes_cptr_t const*>(c.paths),
                                                          c.paths_stride};

    strided_iterator_gt<ustore_bytes_cptr_t const> vals {c.values_bytes, c.values_bytes_stride};
    strided_iterator_gt<ustore_length_t const> offs {c.values_offsets, c.values_offsets_stride};
    strided_iterator_gt<ustore_length_t const> lens {c.values_lengths, c.values_lengths_stride};
    bits_view_t presences {c.values_presences};

    places_arg_t places {collections, {}, {}, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};
    contents_arg_t path_contents {nullptr, path_offs, path_lens, paths, c.tasks_count, c.path_separator};

    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const write_flush = c.options & ustore_option_write_flush_k;

    bool const has_collections_column = collections && !same_collection;
    constexpr bool has_paths_column = true;
    bool const has_contents_column = vals != nullptr;

    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.size(), continuous.begin());
        collections = {continuous.begin(), places.size()};
    }

    ustore_bytes_cptr_t joined_vals_begin = vals ? vals[0] : nullptr;
    if (has_contents_column) {
        auto joined_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        ustore_to_continuous_bin(contents,
                                 places.size(),
                                 c.tasks_count,
                                 &joined_vals_begin,
                                 joined_offs,
                                 arena,
                                 c.error);
        offs = {joined_offs.begin(), sizeof(ustore_length_t)};
    }

    ustore_bytes_cptr_t joined_paths_begin = paths[0];
    if (has_paths_column) {
        auto joined_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        ustore_to_continuous_bin(path_contents,
                                 places.size(),
                                 c.tasks_count,
                                 &joined_paths_begin,
                                 joined_offs,
                                 arena,
                                 c.error);
        path_offs = {joined_offs.begin(), sizeof(ustore_length_t)};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c;
    ArrowSchema input_schema_c;
    auto count_collections = has_collections_column + has_paths_column + has_contents_column;
    ustore_to_arrow_schema(c.tasks_count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_paths_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgPaths.c_str(),
            ustore_doc_field<ustore_str_view_t>(),
            nullptr,
            path_offs.get(),
            joined_paths_begin,
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    if (has_contents_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgVals.c_str(),
            ustore_doc_field<value_view_t>(),
            presences.get(),
            offs.get(),
            joined_vals_begin,
            input_schema_c.children[has_collections_column + has_paths_column],
            input_array_c.children[has_collections_column + has_paths_column],
            c.error);
    return_if_error_m(c.error);

    // Send everything over the network and wait for the response
    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightWritePath);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    if (!has_collections_column && collections)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    if (write_flush)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}&", kParamFlagFlushWrite);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    ar::Result<arf::FlightClient::DoPutResult> result = db.flight->DoPut(options, descriptor, batch_ptr->schema());
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    // This writer has already been started!
    // ar_status = result->writer->Begin(batch_ptr->schema());
    // return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // std::shared_ptr<ar::Buffer> response;
    // ar_status = result->reader->ReadMetadata(&response);
    // return_error_if_m(ar_status.ok(), c.error, network_k, "No response");
}

void ustore_paths_match(ustore_paths_match_t* c_ptr) {

    ustore_paths_match_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_length_t const> count_limits {c.match_counts_limits, c.match_counts_limits_stride};

    strided_iterator_gt<ustore_length_t const> pattern_offs {c.patterns_offsets, c.patterns_offsets_stride};
    strided_iterator_gt<ustore_length_t const> pattern_lens {c.patterns_lengths, c.patterns_lengths_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> patterns {reinterpret_cast<ustore_bytes_cptr_t const*>(c.patterns),
                                                             c.patterns_stride};

    strided_iterator_gt<ustore_length_t const> previous_offs {c.previous_offsets, c.previous_offsets_stride};
    strided_iterator_gt<ustore_length_t const> previous_lens {c.previous_lengths, c.previous_lengths_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> previous {reinterpret_cast<ustore_bytes_cptr_t const*>(c.previous),
                                                             c.previous_stride};

    places_arg_t places {collections, {}, {}, c.tasks_count};
    contents_arg_t pattern_contents {nullptr, pattern_offs, pattern_lens, patterns, c.tasks_count, c.path_separator};
    contents_arg_t previous_contents {nullptr, previous_offs, previous_lens, previous, c.tasks_count, c.path_separator};

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const request_only_counts = c.match_counts && !c.paths_strings;
    char const* partial_mode = request_only_counts //
                                   ? kParamReadPartPresences.c_str()
                                   : nullptr;

    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightMatchPath);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    if (same_named_collection)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    if (partial_mode)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamReadPart, partial_mode);
    export_options(c.options, descriptor.cmd);

    bool const has_collections_column = collections && !same_collection;
    bool const has_previous_column = previous != nullptr;
    bool const has_limits_column = count_limits != nullptr;

    // If all requests map to the same collection, we can avoid passing its ID
    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.count, c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.count, continuous.begin());
        collections = {continuous.begin(), sizeof(ustore_collection_t)};
    }

    if (has_limits_column && !count_limits.is_continuous()) {
        auto continuous = arena.alloc<ustore_length_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(count_limits, places.size(), continuous.begin());
        count_limits = {continuous.begin(), places.size()};
    }

    ustore_bytes_cptr_t joined_patrns_begin = patterns[0];
    auto joined_patrns_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
    return_if_error_m(c.error);
    ustore_to_continuous_bin(pattern_contents,
                             places.size(),
                             c.tasks_count,
                             &joined_patrns_begin,
                             joined_patrns_offs,
                             arena,
                             c.error);
    pattern_offs = {joined_patrns_offs.begin(), sizeof(ustore_length_t)};

    ustore_bytes_cptr_t joined_prevs_begin;
    if (has_previous_column) {
        joined_prevs_begin = previous[0];
        auto joined_prevs_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        ustore_to_continuous_bin(previous_contents,
                                 places.size(),
                                 c.tasks_count,
                                 &joined_prevs_begin,
                                 joined_prevs_offs,
                                 arena,
                                 c.error);
        previous_offs = {joined_prevs_offs.begin(), sizeof(ustore_length_t)};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c, output_array_c;
    ArrowSchema input_schema_c, output_schema_c;
    auto count_collections = has_collections_column + has_limits_column + has_previous_column + 1;
    ustore_to_arrow_schema(places.count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_limits_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCountLimits.c_str(),
            ustore_doc_field<ustore_length_t>(),
            nullptr,
            nullptr,
            count_limits.get(),
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    if (has_previous_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgPrevPatterns.c_str(),
            ustore_doc_field<ustore_str_view_t>(),
            nullptr,
            previous_offs.get(),
            joined_prevs_begin,
            input_schema_c.children[has_collections_column + has_limits_column],
            input_array_c.children[has_collections_column + has_limits_column],
            c.error);
    return_if_error_m(c.error);

    ustore_to_arrow_column( //
        c.tasks_count,
        kArgPatterns.c_str(),
        ustore_doc_field<ustore_str_view_t>(),
        nullptr,
        pattern_offs.get(),
        joined_patrns_begin,
        input_schema_c.children[has_collections_column + has_limits_column + has_previous_column],
        input_array_c.children[has_collections_column + has_limits_column + has_previous_column],
        c.error);
    return_if_error_m(c.error);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    if (batch_ptr->num_rows() == 0)
        return;
    ar::Result<arf::FlightClient::DoExchangeResult> result = db.flight->DoExchange(options, descriptor);
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    ar_status = result->writer->Begin(batch_ptr->schema());
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto input_table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*input_table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // Requesting `ToTable` might be more efficient than concatenating and
    // reallocating directly from our arena, as the underlying Arrow implementation
    // may know the length of the entire dataset.
    auto maybe_table = result->reader->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();
    return_error_if_m(table->num_columns() >= 1, c.error, error_unknown_k, "Expecting one or two columns");

    auto array = std::static_pointer_cast<ar::NumericArray<ar::UInt32Type>>(table->column(0)->chunk(0));
    *c.match_counts = (ustore_length_t*)array->raw_values();
    if (!request_only_counts) {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(1)->chunk(0));
        auto presences_ptr = (ustore_octet_t*)array->null_bitmap_data();
        auto offs_ptr = (ustore_length_t*)array->value_offsets()->data();
        auto data_ptr = (ustore_bytes_ptr_t)array->value_data()->data();

        if (c.paths_offsets)
            *c.paths_offsets = offs_ptr;
        if (c.paths_strings)
            *c.paths_strings = reinterpret_cast<ustore_char_t*>(data_ptr);
    }

    db.readers.push_back(std::move(result->reader));
}

void ustore_paths_read(ustore_paths_read_t* c_ptr) {

    ustore_paths_read_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_length_t const> path_offs {c.paths_offsets, c.paths_offsets_stride};
    strided_iterator_gt<ustore_length_t const> path_lens {c.paths_lengths, c.paths_lengths_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> paths {reinterpret_cast<ustore_bytes_cptr_t const*>(c.paths),
                                                          c.paths_stride};

    places_arg_t places {collections, {}, {}, c.tasks_count};
    contents_arg_t path_contents {nullptr, path_offs, path_lens, paths, c.tasks_count, c.path_separator};

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const request_only_presences = c.presences && !c.lengths && !c.values;
    bool const request_only_lengths = c.lengths && !c.values;
    char const* partial_mode = request_only_presences //
                                   ? kParamReadPartPresences.c_str()
                                   : request_only_lengths //
                                         ? kParamReadPartLengths.c_str()
                                         : nullptr;

    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightReadPath);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamSnapshotID, c.snapshot);
    if (same_named_collection)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    if (partial_mode)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamReadPart, partial_mode);
    export_options(c.options, descriptor.cmd);

    bool const has_collections_column = collections && !same_collection;
    constexpr bool has_paths_column = true;

    // If all requests map to the same collection, we can avoid passing its ID
    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.count, c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.count, continuous.begin());
        collections = {continuous.begin(), sizeof(ustore_collection_t)};
    }

    // Check if the paths are continuous and are already in an Arrow-compatible form
    ustore_bytes_cptr_t joined_paths_begin = paths[0];
    if (has_paths_column) {
        auto joined_offs = arena.alloc<ustore_length_t>(places.size() + 1, c.error);
        return_if_error_m(c.error);
        ustore_to_continuous_bin(path_contents,
                                 places.size(),
                                 c.tasks_count,
                                 &joined_paths_begin,
                                 joined_offs,
                                 arena,
                                 c.error);
        path_offs = {joined_offs.begin(), sizeof(ustore_length_t)};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c, output_array_c;
    ArrowSchema input_schema_c, output_schema_c;
    auto count_collections = has_collections_column + has_paths_column;
    ustore_to_arrow_schema(places.count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_paths_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgPaths.c_str(),
            ustore_doc_field<ustore_str_view_t>(),
            nullptr,
            path_offs.get(),
            joined_paths_begin,
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    if (batch_ptr->num_rows() == 0)
        return;
    ar::Result<arf::FlightClient::DoExchangeResult> result = db.flight->DoExchange(options, descriptor);
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    ar_status = result->writer->Begin(batch_ptr->schema());
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto input_table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*input_table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // Requesting `ToTable` might be more efficient than concatenating and
    // reallocating directly from our arena, as the underlying Arrow implementation
    // may know the length of the entire dataset.
    auto maybe_table = result->reader->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();

    if (request_only_presences) {
        auto array = std::static_pointer_cast<ar::NumericArray<ar::UInt8Type>>(table->column(0)->chunk(0));
        *c.presences = (ustore_octet_t*)array->raw_values();
    }
    else if (request_only_lengths) {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(0)->chunk(0));
        auto presences_ptr = (ustore_octet_t*)array->null_bitmap_data();
        auto lens_ptr = (ustore_length_t*)array->value_offsets()->data();

        if (c.lengths)
            *c.lengths =
                presences_ptr //
                    ? arrow_replace_missing_scalars(presences_ptr, lens_ptr, table->num_rows(), ustore_length_missing_k)
                    : lens_ptr;
        if (c.presences)
            *c.presences = presences_ptr;
    }
    else {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(0)->chunk(0));
        auto presences_ptr = (ustore_octet_t*)array->null_bitmap_data();
        auto offs_ptr = (ustore_length_t*)array->value_offsets()->data();
        auto data_ptr = (ustore_bytes_ptr_t)array->value_data()->data();

        if (c.presences)
            *c.presences = presences_ptr;
        if (c.offsets)
            *c.offsets = offs_ptr;
        if (c.values)
            *c.values = data_ptr;

        if (c.lengths) {
            auto lens = *c.lengths = arena.alloc<ustore_length_t>(places.count, c.error).begin();
            return_if_error_m(c.error);
            if (presences_ptr) {
                auto presences = bits_view_t(presences_ptr);
                for (std::size_t i = 0; i != places.count; ++i)
                    lens[i] = presences[i] ? (offs_ptr[i + 1] - offs_ptr[i]) : ustore_length_missing_k;
            }
            else {
                for (std::size_t i = 0; i != places.count; ++i)
                    lens[i] = offs_ptr[i + 1] - offs_ptr[i];
            }
        }
    }

    db.readers.push_back(std::move(result->reader));
}

void ustore_scan(ustore_scan_t* c_ptr) {

    ustore_scan_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ustore_length_t const> limits {c.count_limits, c.count_limits_stride};
    scans_arg_t scans {collections, start_keys, limits, c.tasks_count};
    places_arg_t places {collections, start_keys, {}, c.tasks_count};

    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);
    bool const write_flush = c.options & ustore_option_write_flush_k;

    bool const has_collections_column = !same_collection;
    constexpr bool has_start_keys_column = true;
    constexpr bool has_lens_column = true;

    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.size(), continuous.begin());
        collections = {continuous.begin(), places.size()};
    }

    if (has_start_keys_column && !start_keys.is_continuous()) {
        auto continuous = arena.alloc<ustore_key_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(start_keys, places.size(), continuous.begin());
        start_keys = {continuous.begin(), places.size()};
    }

    if (has_lens_column && !limits.is_continuous()) {
        auto continuous = arena.alloc<ustore_length_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(limits, places.size(), continuous.begin());
        limits = {continuous.begin(), places.size()};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c, output_array_c;
    ArrowSchema input_schema_c, output_schema_c;
    auto count_collections = has_collections_column + has_start_keys_column + has_lens_column;
    ustore_to_arrow_schema(c.tasks_count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_start_keys_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgScanStarts.c_str(),
            ustore_doc_field<ustore_key_t>(),
            nullptr,
            nullptr,
            start_keys.get(),
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    if (has_lens_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCountLimits.c_str(),
            ustore_doc_field<ustore_length_t>(),
            nullptr,
            nullptr,
            limits.get(),
            input_schema_c.children[has_collections_column + has_start_keys_column],
            input_array_c.children[has_collections_column + has_start_keys_column],
            c.error);
    return_if_error_m(c.error);

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightScan);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamSnapshotID, c.snapshot);
    if (same_named_collection)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    export_options(c.options, descriptor.cmd);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    if (batch_ptr->num_rows() == 0)
        return;
    ar::Result<arf::FlightClient::DoExchangeResult> result = db.flight->DoExchange(options, descriptor);
    return_error_if_m(result.ok(), c.error, network_k, "Failed to exchange with Arrow server");

    ar_status = result->writer->Begin(batch_ptr->schema());
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto input_table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*input_table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // Requesting `ToTable` might be more efficient than concatenating and
    // reallocating directly from our arena, as the underlying Arrow implementation
    // may know the length of the entire dataset.
    auto maybe_table = result->reader->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();

    auto keys_array = std::static_pointer_cast<ar::NumericArray<ar::Int32Type>>(table->column(0)->chunk(0));
    auto offs_array = std::static_pointer_cast<ar::NumericArray<ar::UInt32Type>>(table->column(1)->chunk(0));
    auto data_ptr = (ustore_key_t*)keys_array->raw_values();
    auto offs_ptr = (ustore_length_t*)offs_array->raw_values();

    if (c.offsets)
        *c.offsets = offs_ptr;
    if (c.keys)
        *c.keys = data_ptr;
    if (c.counts) {
        auto lens = *c.counts = arena.alloc<ustore_length_t>(places.count, c.error).begin();
        return_if_error_m(c.error);
        for (std::size_t i = 0; i != places.count; ++i)
            lens[i] = offs_ptr ? offs_ptr[i + 1] - offs_ptr[i] : 0;
    }

    db.readers.push_back(std::move(result->reader));
}

void ustore_sample(ustore_sample_t* c_ptr) {

    ustore_sample_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_length_t const> limits {c.count_limits, c.count_limits_stride};

    places_arg_t places {collections, {}, {}, c.tasks_count};

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    // Configure the `cmd` descriptor
    bool const same_collection = places.same_collection();
    bool const same_named_collection = same_collection && same_collections_are_named(places.collections_begin);

    arf::FlightDescriptor descriptor;
    descriptor.type = arf::FlightDescriptor::UNKNOWN;
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}?", kFlightSample);
    if (c.transaction)
        fmt::format_to(std::back_inserter(descriptor.cmd),
                       "{}=0x{:0>16x}&",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));
    fmt::format_to(std::back_inserter(descriptor.cmd), "{}={}&", kParamSnapshotID, c.snapshot);
    if (same_named_collection)
        fmt::format_to(std::back_inserter(descriptor.cmd), "{}=0x{:0>16x}&", kParamCollectionID, collections[0]);
    export_options(c.options, descriptor.cmd);

    bool const has_collections_column = collections && !same_collection;
    bool const has_limits_column = true;

    // If all requests map to the same collection, we can avoid passing its ID
    if (has_collections_column && !collections.is_continuous()) {
        auto continuous = arena.alloc<ustore_collection_t>(places.count, c.error);
        return_if_error_m(c.error);
        transform_n(collections, places.count, continuous.begin());
        collections = {continuous.begin(), sizeof(ustore_collection_t)};
    }

    if (has_limits_column && !limits.is_continuous()) {
        auto continuous = arena.alloc<ustore_length_t>(places.size(), c.error);
        return_if_error_m(c.error);
        transform_n(limits, places.size(), continuous.begin());
        limits = {continuous.begin(), places.size()};
    }

    // Now build-up the Arrow representation
    ArrowArray input_array_c, output_array_c;
    ArrowSchema input_schema_c, output_schema_c;
    auto count_collections = has_collections_column + has_limits_column;
    ustore_to_arrow_schema(places.count, count_collections, &input_schema_c, &input_array_c, c.error);
    return_if_error_m(c.error);

    if (has_collections_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCols.c_str(),
            ustore_doc_field<ustore_collection_t>(),
            nullptr,
            nullptr,
            collections.get(),
            input_schema_c.children[0],
            input_array_c.children[0],
            c.error);
    return_if_error_m(c.error);

    if (has_limits_column)
        ustore_to_arrow_column( //
            c.tasks_count,
            kArgCountLimits.c_str(),
            ustore_doc_field<ustore_length_t>(),
            nullptr,
            nullptr,
            limits.get(),
            input_schema_c.children[has_collections_column],
            input_array_c.children[has_collections_column],
            c.error);
    return_if_error_m(c.error);

    // Send the request to server
    ar::Result<std::shared_ptr<ar::RecordBatch>> maybe_batch = ar::ImportRecordBatch(&input_array_c, &input_schema_c);
    return_error_if_m(maybe_batch.ok(), c.error, error_unknown_k, "Can't pack RecordBatch");

    std::shared_ptr<ar::RecordBatch> batch_ptr = maybe_batch.ValueUnsafe();
    if (batch_ptr->num_rows() == 0)
        return;
    ar::Result<arf::FlightClient::DoExchangeResult> result = db.flight->DoExchange(options, descriptor);
    return_error_if_m(result.ok(), c.error, network_k, "Failed to Get with Arrow server");

    ar_status = result->writer->Begin(batch_ptr->schema());
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing schema");

    auto input_table = ar::Table::Make(batch_ptr->schema(), batch_ptr->columns(), static_cast<int64_t>(places.size()));
    ar_status = result->writer->WriteTable(*input_table);
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Serializing request");

    ar_status = result->writer->DoneWriting();
    return_error_if_m(ar_status.ok(), c.error, error_unknown_k, "Submitting request");

    // Fetch the responses
    // Requesting `ToTable` might be more efficient than concatenating and
    // reallocating directly from our arena, as the underlying Arrow implementation
    // may know the length of the entire dataset.
    auto maybe_table = result->reader->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();

    auto keys_array = std::static_pointer_cast<ar::NumericArray<ar::Int32Type>>(table->column(0)->chunk(0));
    auto offs_array = std::static_pointer_cast<ar::NumericArray<ar::UInt32Type>>(table->column(1)->chunk(0));
    auto data_ptr = (ustore_key_t*)keys_array->raw_values();
    auto offs_ptr = (ustore_length_t*)offs_array->raw_values();

    if (c.offsets)
        *c.offsets = offs_ptr;
    if (c.keys)
        *c.keys = data_ptr;
    if (c.counts) {
        auto lens = *c.counts = arena.alloc<ustore_length_t>(places.count, c.error).begin();
        return_if_error_m(c.error);
        for (std::size_t i = 0; i != places.count; ++i)
            lens[i] = offs_ptr ? offs_ptr[i + 1] - offs_ptr[i] : 0;
    }

    db.readers.push_back(std::move(result->reader));
}

void ustore_measure(ustore_measure_t* c_ptr) {

    ustore_measure_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);
}

/*********************************************************/
/*****************	Collections Management	****************/
/*********************************************************/

void ustore_collection_create(ustore_collection_create_t* c_ptr) {

    ustore_collection_create_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    auto name_len = c.name ? std::strlen(c.name) : 0;
    return_error_if_m(name_len, c.error, args_wrong_k, "Default collection is always present");

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    fmt::format_to(std::back_inserter(action.type), "{}?{}={}", kFlightColCreate, kParamCollectionName, c.name);
    if (c.config)
        action.body = std::make_shared<ar::Buffer>(std::string_view {c.config});

    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream;
    {
        std::lock_guard<std::mutex> lk(db.arena_lock);
        arrow_mem_pool_t pool(db.arena);
        arf::FlightCallOptions options = arrow_call_options(pool);
        maybe_stream = db.flight->DoAction(options, action);
    }
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
    auto& stream_ptr = maybe_stream.ValueUnsafe();
    ar::Result<std::unique_ptr<arf::Result>> maybe_id = stream_ptr->Next();
    return_error_if_m(maybe_id.ok(), c.error, network_k, "No response received");

    auto& id_ptr = maybe_id.ValueUnsafe();
    return_error_if_m(id_ptr->body->size() == sizeof(ustore_collection_t),
                      c.error,
                      error_unknown_k,
                      "Inadequate response");
    std::memcpy(c.id, id_ptr->body->data(), sizeof(ustore_collection_t));
}

void ustore_collection_drop(ustore_collection_drop_t* c_ptr) {

    ustore_collection_drop_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    std::string_view mode;
    switch (c.mode) {
    case ustore_drop_vals_k: mode = kParamDropModeValues; break;
    case ustore_drop_keys_vals_k: mode = kParamDropModeContents; break;
    case ustore_drop_keys_vals_handle_k: mode = kParamDropModeCollection; break;
    }

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    fmt::format_to(std::back_inserter(action.type),
                   "{}?{}=0x{:0>16x}&{}={}",
                   kFlightColDrop,
                   kParamCollectionID,
                   c.id,
                   kParamDropMode,
                   mode);

    std::lock_guard<std::mutex> lk(db.arena_lock);
    arrow_mem_pool_t pool(db.arena);
    arf::FlightCallOptions options = arrow_call_options(pool);
    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream = db.flight->DoAction(options, action);
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
}

void ustore_collection_list(ustore_collection_list_t* c_ptr) {

    ustore_collection_list_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);
    if (!(c.options & ustore_option_dont_discard_memory_k))
        db.readers.clear();

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    arf::Ticket ticket {kFlightListCols};
    if (c.transaction)
        fmt::format_to(std::back_inserter(ticket.ticket),
                       "?{}=0x{:0>16x}",
                       kParamTransactionID,
                       std::uintptr_t(c.transaction));

    auto maybe_stream = db.flight->DoGet(options, ticket);
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
    auto& stream_ptr = maybe_stream.ValueUnsafe();

    auto maybe_table = stream_ptr->ToTable();
    return_error_if_m(maybe_table.ok(), c.error, error_unknown_k, "Failed to create table");
    auto table = maybe_table.ValueUnsafe();

    if (c.count)
        *c.count = static_cast<ustore_size_t>(table->num_rows());
    if (c.names) {
        auto array = std::static_pointer_cast<ar::BinaryArray>(table->column(1)->chunk(0));
        return_error_if_m(table->column(1)->num_chunks() == 1, c.error, network_k, "Expected one chunk");
        *c.names = (ustore_str_span_t)array->value_data()->data();
        if (c.offsets)
            *c.offsets = (ustore_length_t*)array->value_offsets()->data();
    }
    if (c.ids) {
        auto array = std::static_pointer_cast<ar::NumericArray<ar::Int64Type>>(table->column(0)->chunk(0));
        return_error_if_m(table->column(0)->num_chunks() == 1, c.error, network_k, "Expected one chunk");
        *c.ids = (ustore_collection_t*)array->raw_values();
    }

    db.readers.push_back(std::move(stream_ptr));
}

void ustore_database_control(ustore_database_control_t* c_ptr) {

    ustore_database_control_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.request, c.error, uninitialized_state_k, "Request is uninitialized");

    *c.response = NULL;
    log_error_m(c.error, missing_feature_k, "Controls aren't supported in this implementation!");
}

/*********************************************************/
/*****************		Snapshots	  ****************/
/*********************************************************/
void ustore_snapshot_list(ustore_snapshot_list_t* c_ptr) {
    ustore_snapshot_list_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    ar::Status ar_status;
    arrow_mem_pool_t pool(arena);
    arf::FlightCallOptions options = arrow_call_options(pool);

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Ticket ticket {kFlightListSnap};
    ar::Result<std::unique_ptr<arf::FlightStreamReader>> maybe_stream = db.flight->DoGet(options, ticket);
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");

    auto& stream_ptr = maybe_stream.ValueUnsafe();
    ar::Result<std::shared_ptr<ar::Table>> maybe_table = stream_ptr->ToTable();

    ArrowSchema schema_c;
    ArrowArray batch_c;
    ar_status = unpack_table(maybe_table, schema_c, batch_c, &pool);
    return_error_if_m(ar_status.ok(), c.error, args_combo_k, "Failed to unpack list of snapshots");

    auto ids_column_idx = column_idx(schema_c, kArgSnaps);
    return_error_if_m(ids_column_idx, c.error, args_combo_k, "Expecting one column");

    if (c.count)
        *c.count = static_cast<ustore_size_t>(batch_c.length);
    if (c.ids)
        *c.ids = (ustore_collection_t*)batch_c.children[*ids_column_idx]->buffers[1];
}

void ustore_snapshot_create(ustore_snapshot_create_t* c_ptr) {
    ustore_snapshot_create_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    fmt::format_to(std::back_inserter(action.type), "{}", kFlightSnapCreate);

    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream;
    {
        std::lock_guard<std::mutex> lk(db.arena_lock);
        arrow_mem_pool_t pool(db.arena);
        arf::FlightCallOptions options = arrow_call_options(pool);
        maybe_stream = db.flight->DoAction(options, action);
    }
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
    auto& stream_ptr = maybe_stream.ValueUnsafe();
    ar::Result<std::unique_ptr<arf::Result>> maybe_id = stream_ptr->Next();
    return_error_if_m(maybe_id.ok(), c.error, network_k, "No response received");

    auto& id_ptr = maybe_id.ValueUnsafe();
    return_error_if_m(id_ptr->body->size() == sizeof(ustore_snapshot_t),
                      c.error,
                      error_unknown_k,
                      "Inadequate response");
    std::memcpy(c.id, id_ptr->body->data(), sizeof(ustore_snapshot_t));
}

void ustore_snapshot_drop(ustore_snapshot_drop_t* c_ptr) {
    ustore_snapshot_drop_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    fmt::format_to(std::back_inserter(action.type), "{}?{}={}", kFlightSnapCreate, kParamSnapshotID, c.id);

    std::lock_guard<std::mutex> lk(db.arena_lock);
    arrow_mem_pool_t pool(db.arena);
    arf::FlightCallOptions options = arrow_call_options(pool);
    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream = db.flight->DoAction(options, action);
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
}

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

void ustore_transaction_init(ustore_transaction_init_t* c_ptr) {

    ustore_transaction_init_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.transaction, c.error, uninitialized_state_k, "Transaction is uninitialized");

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    ustore_size_t txn_id = *reinterpret_cast<ustore_size_t*>(c.transaction);
    fmt::format_to(std::back_inserter(action.type), "{}?", kFlightTxnBegin);
    if (txn_id != 0)
        fmt::format_to(std::back_inserter(action.type), "{}=0x{:0>16x}&", kParamTransactionID, txn_id);
    if (c.options & ustore_option_transaction_dont_watch_k)
        fmt::format_to(std::back_inserter(action.type), "{}&", kParamFlagDontWatch);

    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream;
    {
        std::lock_guard<std::mutex> lk(db.arena_lock);
        arrow_mem_pool_t pool(db.arena);
        arf::FlightCallOptions options = arrow_call_options(pool);
        maybe_stream = db.flight->DoAction(options, action);
    }
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");

    auto& stream_ptr = maybe_stream.ValueUnsafe();
    ar::Result<std::unique_ptr<arf::Result>> maybe_id = stream_ptr->Next();
    return_error_if_m(maybe_id.ok(), c.error, network_k, "No response received");

    auto& id_ptr = maybe_id.ValueUnsafe();
    return_error_if_m(id_ptr->body->size() == sizeof(ustore_transaction_t),
                      c.error,
                      error_unknown_k,
                      "Inadequate response");
    std::memcpy(c.transaction, id_ptr->body->data(), sizeof(ustore_transaction_t));
}

void ustore_transaction_commit(ustore_transaction_commit_t* c_ptr) {

    ustore_transaction_commit_t& c = *c_ptr;
    return_error_if_m(c.transaction, c.error, uninitialized_state_k, "Transaction is uninitialized");

    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c.db);

    arf::Action action;
    fmt::format_to(std::back_inserter(action.type),
                   "{}?{}=0x{:0>16x}&",
                   kFlightTxnCommit,
                   kParamTransactionID,
                   std::uintptr_t(c.transaction));
    if (c.options & ustore_option_write_flush_k)
        fmt::format_to(std::back_inserter(action.type), "{}&", kParamFlagFlushWrite);

    std::lock_guard<std::mutex> lk(db.arena_lock);
    arrow_mem_pool_t pool(db.arena);
    arf::FlightCallOptions options = arrow_call_options(pool);
    ar::Result<std::unique_ptr<arf::ResultStream>> maybe_stream = db.flight->DoAction(options, action);
    return_error_if_m(maybe_stream.ok(), c.error, network_k, "Failed to act on Arrow server");
}

/*********************************************************/
/*****************	  Memory Management   ****************/
/*********************************************************/

void ustore_arena_free(ustore_arena_t c_arena) {
    clear_linked_memory(c_arena);
}

void ustore_transaction_free(ustore_transaction_t const c_transaction) {
    if (!c_transaction)
        return;
}

void ustore_database_free(ustore_database_t c_db) {
    if (!c_db)
        return;
    rpc_client_t& db = *reinterpret_cast<rpc_client_t*>(c_db);
    db.arena.release_all();
    delete &db;
}

void ustore_error_free(ustore_error_t) {
}
