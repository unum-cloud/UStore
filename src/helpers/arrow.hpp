/**
 * @file helpers/arrow.hpp
 * @author Ashot Vardanian
 *
 * @brief Helper functions for Apache Arrow interoperability.
 */
#pragma once
#include <string>
#include <string_view>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#include <arrow/type.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/buffer.h>
#include <arrow/table.h>
#include <arrow/memory_pool.h>
#include <arrow/c/bridge.h>
#pragma GCC diagnostic pop

#include "linked_memory.hpp"       // `linked_memory_lock_t`
#include "ukv/cpp/ranges_args.hpp" // `contents_arg_t`

namespace unum::ukv {

constexpr std::size_t arrow_extra_offsets_k = 1;
constexpr std::size_t arrow_bytes_alignment_k = 64;

namespace arf = arrow::flight;
namespace ar = arrow;

inline static std::string const kFlightListCols = "list_collections";   /// `DoGet`
inline static std::string const kFlightColCreate = "create_collection"; /// `DoAction`
inline static std::string const kFlightColDrop = "remove_collection";   /// `DoAction`

inline static std::string const kFlightTxnBegin = "begin_transaction";   /// `DoAction`
inline static std::string const kFlightTxnCommit = "commit_transaction"; /// `DoAction`

inline static std::string const kFlightWrite = "write";          /// `DoPut`
inline static std::string const kFlightRead = "read";            /// `DoExchange`
inline static std::string const kFlightWritePath = "write_path"; /// `DoPut`
inline static std::string const kFlightMatchPath = "match_path"; /// `DoExchange`
inline static std::string const kFlightReadPath = "read_path";   /// `DoExchange`
inline static std::string const kFlightScan = "scan";            /// `DoExchange`
inline static std::string const kFlightMeasure = "measure";      /// `DoExchange`

inline static std::string const kArgCols = "collections";
inline static std::string const kArgKeys = "keys";
inline static std::string const kArgVals = "values";
inline static std::string const kArgFields = "fields";
inline static std::string const kArgScanStarts = "start_keys";
inline static std::string const kArgCountLimits = "count_limits";
inline static std::string const kArgPresences = "fields";
inline static std::string const kArgLengths = "lengths";
inline static std::string const kArgNames = "names";
inline static std::string const kArgPaths = "paths";
inline static std::string const kArgPatterns = "patterns";
inline static std::string const kArgPrevPatterns = "prev_patterns";

inline static std::string const kParamCollectionID = "collection_id";
inline static std::string const kParamCollectionName = "collection_name";
inline static std::string const kParamTransactionID = "transaction_id";
inline static std::string const kParamReadPart = "part";
inline static std::string const kParamDropMode = "mode";
inline static std::string const kParamFlagFlushWrite = "flush";
inline static std::string const kParamFlagDontWatch = "dont_watch";
inline static std::string const kParamFlagDontDiscard = "";
inline static std::string const kParamFlagSharedMemRead = "shared";

inline static std::string const kParamReadPartLengths = "lengths";
inline static std::string const kParamReadPartPresences = "presences";

inline static std::string const kParamDropModeValues = "values";
inline static std::string const kParamDropModeContents = "contents";
inline static std::string const kParamDropModeCollection = "collection";

class arrow_mem_pool_t final : public ar::MemoryPool {
    linked_memory_t resource_;
    int64_t bytes_allocated_ = 0;
    size_t alignment_ = 64;

  public:
    arrow_mem_pool_t(linked_memory_t& arena) : resource_(arena) {}
    arrow_mem_pool_t(linked_memory_lock_t& arena) : resource_(arena.memory) {}
    ~arrow_mem_pool_t() {}

    ar::Status Allocate(int64_t size, uint8_t** ptr) override {
        auto new_ptr = resource_.alloc(size, alignment_);
        if (!new_ptr)
            return ar::Status::OutOfMemory("");

        *ptr = reinterpret_cast<uint8_t*>(new_ptr);
        bytes_allocated_ += size;
        return ar::Status::OK();
    }
    ar::Status Reallocate(int64_t old_size, int64_t new_size, uint8_t** ptr) override {
        auto new_ptr = resource_.alloc(new_size, alignment_);
        if (!new_ptr)
            return ar::Status::OutOfMemory("");

        std::memcpy(new_ptr, *ptr, old_size);
        // resource_.deallocate(buffer, size); // deallocation is no-op
        *ptr = reinterpret_cast<uint8_t*>(new_ptr);
        bytes_allocated_ += new_size - old_size;
        return ar::Status::OK();
    }
    void Free(uint8_t* buffer, int64_t size) override {
        // resource_.deallocate(buffer, size); // deallocation is no-op
        bytes_allocated_ -= size;
    }
    void ReleaseUnused() override {}
    int64_t bytes_allocated() const override { return bytes_allocated_; }
    int64_t max_memory() const override { return INT64_MAX; }
    std::string backend_name() const override { return "ukv"; }
};

ar::ipc::IpcReadOptions arrow_read_options(arrow_mem_pool_t& pool) {
    ar::ipc::IpcReadOptions options;
    options.memory_pool = &pool;
    options.use_threads = false;
    options.max_recursion_depth = 2;
    return options;
}

ar::ipc::IpcWriteOptions arrow_write_options(arrow_mem_pool_t& pool) {
    ar::ipc::IpcWriteOptions options;
    options.memory_pool = &pool;
    options.use_threads = false;
    options.max_recursion_depth = 2;
    return options;
}

ar::Result<std::shared_ptr<ar::RecordBatch>> combined_batch(std::shared_ptr<ar::Table> table) {
    return table->num_rows() ? table->CombineChunksToBatch() : ar::RecordBatch::MakeEmpty(table->schema());
}

ar::Status unpack_table( //
    ar::Result<std::shared_ptr<ar::Table>> const& maybe_table,
    ArrowSchema& schema_c,
    ArrowArray& batch_c) {

    if (!maybe_table.ok())
        return maybe_table.status();

    std::shared_ptr<ar::Table> const& table = maybe_table.ValueUnsafe();
    ar::Status ar_status = ar::ExportSchema(*table->schema(), &schema_c);
    if (!ar_status.ok())
        return ar_status;

    // Join all the chunks to form a single table
    auto maybe_batch = combined_batch(table);
    if (!maybe_batch.ok())
        return maybe_batch.status();

    std::shared_ptr<ar::RecordBatch> const& batch_ptr = maybe_batch.ValueUnsafe();
    ar_status = ar::ExportRecordBatch(*batch_ptr, &batch_c, nullptr);
    return ar_status;
}

inline expected_gt<std::size_t> column_idx(ArrowSchema const& schema_c, std::string_view name) {
    auto begin = schema_c.children;
    auto end = begin + schema_c.n_children;
    auto it = std::find_if(begin, end, [=](ArrowSchema* column_schema) {
        return std::string_view {column_schema->name} == name;
    });
    if (it == end)
        return status_t::status_view("Column not found!");
    return static_cast<std::size_t>(it - begin);
}

/**
 * We have a different methodology of marking NULL entries, than Arrow.
 * We can reuse the `column_lengths` to put-in some NULL markers.
 * Bitmask would use 32x less memory.
 */
inline ukv_octet_t* convert_lengths_into_bitmap(ukv_length_t* lengths, ukv_size_t n) {
    size_t count_slots = (n + (CHAR_BIT - 1)) / CHAR_BIT;
    ukv_octet_t* slots = (ukv_octet_t*)lengths;
    for (size_t slot_idx = 0; slot_idx != count_slots; ++slot_idx) {
        ukv_octet_t slot_value = 0;
        size_t first_idx = slot_idx * CHAR_BIT;
        size_t remaining_count = count_slots - first_idx;
        size_t remaining_in_slot = remaining_count > CHAR_BIT ? CHAR_BIT : remaining_count;
        for (size_t bit_idx = 0; bit_idx != remaining_in_slot; ++bit_idx) {
            slot_value |= 1 << bit_idx;
        }
        slots[slot_idx] = slot_value;
    }
    // Cleanup the following memory
    std::memset(slots + count_slots + 1, 0, n * sizeof(ukv_length_t) - count_slots);
    return slots;
}

/**
 * @brief Replaces "lengths" with `::ukv_length_missing_k` if matching NULL indicator is set.
 */
template <typename scalar_at>
inline scalar_at* arrow_replace_missing_scalars(ukv_octet_t const* slots,
                                                scalar_at* scalars,
                                                ukv_size_t n,
                                                scalar_at missing) {
    size_t count_slots = (n + (CHAR_BIT - 1)) / CHAR_BIT;
    for (size_t slot_idx = 0; slot_idx != count_slots; ++slot_idx) {
        size_t first_idx = slot_idx * CHAR_BIT;
        size_t remaining_count = count_slots - first_idx;
        size_t remaining_in_slot = remaining_count > CHAR_BIT ? CHAR_BIT : remaining_count;
        for (size_t bit_idx = 0; bit_idx != remaining_in_slot; ++bit_idx) {
            if (slots[slot_idx] & (1 << bit_idx))
                scalars[first_idx + bit_idx] = missing;
        }
    }
    return scalars;
}

inline strided_iterator_gt<ukv_key_t> get_keys( //
    ArrowSchema const& schema_c,
    ArrowArray const& batch_c,
    std::string_view arg_name) {
    auto maybe_idx = column_idx(schema_c, arg_name);
    if (!maybe_idx)
        return {};

    ukv_key_t* begin = nullptr;
    auto& array = *batch_c.children[*maybe_idx];
    begin = (ukv_key_t*)array.buffers[1];
    // Make sure there are no NULL entries.
    return {begin, sizeof(ukv_key_t)};
}

inline strided_iterator_gt<ukv_collection_t> get_collections( //
    ArrowSchema const& schema_c,
    ArrowArray const& batch_c,
    std::string_view arg_name) {
    auto maybe_idx = column_idx(schema_c, arg_name);
    if (!maybe_idx)
        return {};

    ukv_collection_t* begin = nullptr;
    auto& array = *batch_c.children[*maybe_idx];
    auto bitmasks = (ukv_octet_t const*)array.buffers[0];
    begin = (ukv_collection_t*)array.buffers[1];
    if (bitmasks && array.null_count != 0)
        arrow_replace_missing_scalars(bitmasks, begin, array.length, ukv_collection_main_k);
    return {begin, sizeof(ukv_collection_t)};
}

inline strided_iterator_gt<ukv_length_t> get_lengths( //
    ArrowSchema const& schema_c,
    ArrowArray const& batch_c,
    std::string_view arg_name) {
    auto maybe_idx = column_idx(schema_c, arg_name);
    if (!maybe_idx)
        return {};

    ukv_length_t* begin = nullptr;
    auto& array = *batch_c.children[*maybe_idx];
    auto bitmasks = (ukv_octet_t const*)array.buffers[0];
    begin = (ukv_length_t*)array.buffers[1];
    if (bitmasks && array.null_count != 0)
        arrow_replace_missing_scalars(bitmasks, begin, array.length, ukv_length_missing_k);
    return {begin, sizeof(ukv_length_t)};
}

inline contents_arg_t get_contents( //
    ArrowSchema const& schema_c,
    ArrowArray const& batch_c,
    std::string_view arg_name) {

    auto maybe_idx = column_idx(schema_c, arg_name);
    if (!maybe_idx)
        return {};

    auto& array = *batch_c.children[*maybe_idx];
    contents_arg_t result;
    result.contents_begin = {(ukv_bytes_cptr_t const*)&array.buffers[2], 0};
    result.offsets_begin = {(ukv_length_t const*)array.buffers[1], sizeof(ukv_length_t)};
    if (array.buffers[0] && array.null_count != 0)
        result.presences_begin = {(ukv_octet_t const*)array.buffers[0]};
    result.count = static_cast<ukv_size_t>(batch_c.length);
    return result;
}

void ukv_to_continuous_bin( //
    contents_arg_t& contents,
    size_t places_count,
    size_t c_tasks_count,
    ukv_bytes_cptr_t* continuous_bin,
    ptr_range_gt<ukv_length_t> continuous_bin_offs,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) {

    // Check if the paths are continuous and are already in an Arrow-compatible form
    if (!contents.is_continuous()) {
        size_t total = transform_reduce_n(contents, places_count, 0ul, std::mem_fn(&value_view_t::size));
        auto joined_paths = arena.alloc<byte_t>(total, c_error);
        return_if_error_m(c_error);
        size_t slots_count = divide_round_up<std::size_t>(places_count, CHAR_BIT);

        // Exports into the Arrow-compatible form
        ukv_length_t exported_bytes = 0;
        for (std::size_t i = 0; i != c_tasks_count; ++i) {
            auto path = contents[i];
            continuous_bin_offs[i] = exported_bytes;
            std::memcpy(joined_paths.begin() + exported_bytes, path.begin(), path.size());
            exported_bytes += path.size();
        }
        continuous_bin_offs[places_count] = exported_bytes;

        *continuous_bin = (ukv_bytes_cptr_t)joined_paths.begin();
    }
    // It may be the case, that we only have `c_tasks_count` offsets instead of `c_tasks_count+1`,
    // which won't be enough for Arrow.
    else if (!contents.is_arrow()) {
        size_t slots_count = divide_round_up<std::size_t>(places_count, CHAR_BIT);

        // Exports into the Arrow-compatible form
        ukv_length_t exported_bytes = 0;
        for (std::size_t i = 0; i != c_tasks_count; ++i) {
            auto path = contents[i];
            continuous_bin_offs[i] = exported_bytes;
            exported_bytes += path.size();
        }
        continuous_bin_offs[places_count] = exported_bytes;
    }
}
} // namespace unum::ukv
