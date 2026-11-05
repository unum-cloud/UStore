/**
 *  @file src_oracles/rocksdb/changes.hpp
 *  @author Ash Vardanian
 *  @date November 5, 2026
 *  @brief The change feeds of the C ABI over the log of the RocksDB oracle's database.
 *
 *  A commit writes one batch into RocksDB's log, ending at the sequence its commit stamp reports,
 *  so a feed reads the batches after its stamp through @c GetUpdatesSince and keeps the records of
 *  its collection's column family. While any feed is open, file deletions stay disabled, which
 *  keeps every log file from the oldest open feed on, and every table file a compaction replaced,
 *  until the last feed frees.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstring> // `std::memcpy`

#include <algorithm>    // `std::stable_sort`
#include <memory>       // `std::unique_ptr`, the iterator type `GetUpdatesSince` takes
#include <new>          // `std::nothrow`
#include <shared_mutex> // `std::shared_lock`

#include <rocksdb/db.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/write_batch.h>

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/ram_buffers.hpp" // `arena_copy`, `growing_strings`

#include "database.hpp" // `database_t`, `column_of`, `refuse_collection`, `status_of`
#include "lib.hpp"      // `ustore_changes_cursor_handle_t`

namespace unum::ustore::oracles::rocksdb {

#pragma region Change Feeds

/** Where a feed resumes: the next pull's first stamp, and the count of its changes handed out. */
struct changes_feed_t {
    ustore_stamp_t resume_stamp = 0;
    std::size_t handed = 0;
};

/** One change a commit made to the fed collection, its value viewing the batch it was read from. */
struct logged_change_t {
    ustore_key_t key;
    ustore_change_kind_t kind;
    ::rocksdb::Slice value;
};

/** Lists a logged batch's changes to a column family; RocksDB hands batches to handlers only. */
class change_lister_t : public ::rocksdb::WriteBatch::Handler {
    std::uint32_t column_id_;
    st::vector<logged_change_t, arena_allocator<logged_change_t>>& changes_;

    ::rocksdb::Status list_(std::uint32_t column_id, ::rocksdb::Slice const& key, ustore_change_kind_t kind,
                            ::rocksdb::Slice const& value) {
        if (column_id != column_id_) return ::rocksdb::Status::OK();
        if (key.size() != sizeof(ustore_key_t)) return ::rocksdb::Status::Corruption("A logged key is not a UUID");
        logged_change_t change {{}, kind, value};
        std::memcpy(&change.key, key.data(), sizeof(ustore_key_t));
        if (st::failed(changes_.emplace_back(change))) return ::rocksdb::Status::MemoryLimit();
        return ::rocksdb::Status::OK();
    }

  public:
    change_lister_t(std::uint32_t column_id,
                    st::vector<logged_change_t, arena_allocator<logged_change_t>>& changes) noexcept
        : column_id_(column_id), changes_(changes) {}

    ::rocksdb::Status PutCF(std::uint32_t column_id, ::rocksdb::Slice const& key,
                            ::rocksdb::Slice const& value) override {
        return list_(column_id, key, ustore_change_write_k, value);
    }
    ::rocksdb::Status DeleteCF(std::uint32_t column_id, ::rocksdb::Slice const& key) override {
        return list_(column_id, key, ustore_change_removal_k, {});
    }
    ::rocksdb::Status SingleDeleteCF(std::uint32_t column_id, ::rocksdb::Slice const& key) override {
        return list_(column_id, key, ustore_change_removal_k, {});
    }

    ::rocksdb::Status MarkNoop(bool) override { return ::rocksdb::Status::OK(); }
};

/** Whether the log still holds every batch committed after @p since_stamp, which the first
 *  batch it holds from there tells, as every sequence of this engine is logged and log files
 *  go oldest first. */
expected<bool> log_holds_since(database_t& database, ustore_stamp_t since_stamp) {
    if (since_stamp >= database.native->GetLatestSequenceNumber()) return true;
    std::unique_ptr<::rocksdb::TransactionLogIterator> batches;
    native_status_t const status = database.native->GetUpdatesSince(since_stamp + 1, &batches);
    if (status.IsNotFound()) return false;
    if (!status.ok()) return status_of(status);
    return batches->Valid() && batches->GetBatch().sequence <= since_stamp + 1;
}

/** The status of a pull finding a gap in the log, which its feed's pin should have prevented. */
inline status_t log_lost_changes_() noexcept {
    return {status_code_t::internal_k, "The log lost changes the feed pinned"};
}

/** Opens a feed into @p cursor, refusing a stamp whose changes the log has dropped. */
status_t open_changes_feed(ustore_changes_cursor_init_t const& arguments, ustore_changes_cursor_t cursor,
                           view_t const&) {
    database_t& database = *state_of<database_t>(arguments.database);
    {
        std::shared_lock lock {database.columns_mutex};
        status_t const refused = refuse_collection(database, arguments.collection, entry_layout_t::uuids_to_bytes_k);
        if (refused.failed()) return refused;
    }
    // Deletions stop before the log is checked, so no file the check found goes till the feed frees
    if (native_status_t const status = database.native->DisableFileDeletions(); !status.ok()) return status_of(status);
    auto [held, held_status] = log_holds_since(database, arguments.since_stamp);
    changes_feed_t* feed = nullptr;
    if (held_status.succeeded() && held) feed = new (std::nothrow) changes_feed_t {arguments.since_stamp + 1, 0};
    if (!feed) {
        [[maybe_unused]] native_status_t const enabled = database.native->EnableFileDeletions();
        if (held_status.failed()) return held_status;
        if (!held) return {status_code_t::write_conflict_k, "The log no longer holds the changes after the stamp"};
        return {status_code_t::out_of_memory_k, "Failed to allocate the change feed"};
    }
    cursor->state = feed;
    return {};
}

status_t changes_cursor_init(ustore_changes_cursor_init_t const& arguments, ustore_changes_cursor_t cursor,
                             view_t const& view) noexcept {
    return guarded<&open_changes_feed>(arguments, cursor, view);
}

status_t changes_cursor_next(ustore_changes_cursor_next_t const& arguments, view_t const& view) {
    ustore_changes_cursor_handle_t& cursor = *arguments.cursor;
    changes_feed_t& feed = *static_cast<changes_feed_t*>(cursor.state);
    database_t& database = *state_of<database_t>(cursor.cursor.database);
    std::size_t const wanted = arguments.wanted;

    st::vector<ustore_key_t, arena_allocator<ustore_key_t>> keys {{view.scratch}};
    st::vector<ustore_stamp_t, arena_allocator<ustore_stamp_t>> stamps {{view.scratch}};
    st::vector<ustore_change_kind_t, arena_allocator<ustore_change_kind_t>> kinds {{view.scratch}};
    growing_strings<arena_allocator<byte_t>> values {view.scratch};
    st::vector<logged_change_t, arena_allocator<logged_change_t>> changes {{view.scratch}};
    changes_feed_t resumed = feed;

    std::shared_lock lock {database.columns_mutex};
    column_t* const column = column_of(database, cursor.cursor.collection);
    if (!column) return {status_code_t::not_found_k, "The collection was dropped"};

    std::unique_ptr<::rocksdb::TransactionLogIterator> batches;
    if (wanted && resumed.resume_stamp <= database.native->GetLatestSequenceNumber())
        if (native_status_t const status = database.native->GetUpdatesSince(resumed.resume_stamp, &batches);
            !status.ok())
            return status_of(status);

    for (std::size_t read = 0; batches && batches->Valid() && keys.size() != wanted; batches->Next(), ++read) {
        ::rocksdb::BatchResult const batch = batches->GetBatch();
        if (read == 0 && batch.sequence > resumed.resume_stamp) return log_lost_changes_();
        if (batch.sequence < resumed.resume_stamp) continue;

        // A commit's changes come by key, and a key written twice in one batch keeps its last write
        changes.clear();
        change_lister_t lister {column->GetID(), changes};
        if (native_status_t const status = batch.writeBatchPtr->Iterate(&lister); !status.ok())
            return status_of(status);
        std::stable_sort(
            changes.begin(), changes.end(),
            [](logged_change_t const& first, logged_change_t const& second) { return first.key < second.key; });
        std::size_t distinct = 0;
        for (std::size_t index = 0; index != changes.size(); ++index) {
            if (distinct && changes[distinct - 1].key == changes[index].key) changes[distinct - 1] = changes[index];
            else changes[distinct++] = changes[index];
        }

        // A commit's stamp is the last sequence it wrote, which a snapshot taken right after it
        // reads at, and an empty batch consumes none, so the next batch may start at its sequence
        ustore_stamp_t const next_sequence = batch.sequence + batch.writeBatchPtr->Count();
        ustore_stamp_t const stamp = next_sequence - 1;
        std::size_t const skipped = batch.sequence == resumed.resume_stamp ? resumed.handed : 0;
        std::size_t index = skipped;
        for (; index < distinct && keys.size() != wanted; ++index) {
            logged_change_t const& change = changes[index];
            if (status_t const status = growth_status(keys.emplace_back(change.key)); status.failed()) return status;
            if (status_t const status = growth_status(stamps.emplace_back(stamp)); status.failed()) return status;
            if (status_t const status = growth_status(kinds.emplace_back(change.kind)); status.failed()) return status;
            if (arguments.values)
                if (status_t const status = values.push_back(bytes_of(change.value.ToStringView())); status.failed())
                    return status;
        }
        resumed = index < distinct ? changes_feed_t {batch.sequence, index} : changes_feed_t {next_sequence, 0};
    }
    if (batches && !batches->Valid() && batches->status().IsNotFound()) return log_lost_changes_();
    if (batches && !batches->Valid() && !batches->status().ok() && !batches->status().IsTryAgain())
        return status_of(batches->status());

    auto [exported_keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, keys);
    if (keys_status.failed()) return keys_status;
    auto [exported_stamps, stamps_status] = arena_copy<ustore_stamp_t>(arguments.arena, stamps);
    if (stamps_status.failed()) return stamps_status;
    auto [exported_kinds, kinds_status] = arena_copy<ustore_change_kind_t>(arguments.arena, kinds);
    if (kinds_status.failed()) return kinds_status;
    if (arguments.values)
        if (status_t const status = values.export_into(arguments.arena, *arguments.values); status.failed())
            return status;
    *arguments.keys = exported_keys.data();
    *arguments.stamps = exported_stamps.data();
    *arguments.kinds = exported_kinds.data();
    *arguments.count = static_cast<ustore_size_t>(keys.size());
    feed = resumed;
    return {};
}

void changes_cursor_free(ustore_changes_cursor_t cursor) noexcept {
    database_t& database = *state_of<database_t>(cursor->cursor.database);
    [[maybe_unused]] native_status_t const enabled = database.native->EnableFileDeletions();
    delete static_cast<changes_feed_t*>(cursor->state);
    cursor->state = nullptr;
}

#pragma endregion Change Feeds

} // namespace unum::ustore::oracles::rocksdb
