#include <vector>
#include <filesystem>

#include <gtest/gtest.h>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

#if defined(UKV_TEST_PATH)
constexpr char const* path_k = UKV_TEST_PATH;
#else
constexpr char const* path_k = "";
#endif

TEST(db, validation) {

    database_t db;
    EXPECT_TRUE(db.open(path_k));
    blobs_collection_t collection = db.main();
    blobs_collection_t named_collection = *db.find_or_create("col");
    transaction_t txn = *db.transact();
    std::vector<ukv_key_t> keys {34, 35, 36};
    std::vector<std::uint64_t> vals {34, 35, 36};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());
    constexpr ukv_length_t count = 3;

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = count,
    };

    using value_extractor_t = contents_arg_extractor_gt<std::remove_reference_t<contents_arg_t>>;
    auto contents = value_extractor_t {}.contents(values);
    auto offsets = value_extractor_t {}.offsets(values);
    auto lengths = value_extractor_t {}.lengths(values);

    status_t status;
    std::vector<ukv_options_t> options {ukv_options_default_k, ukv_option_write_flush_k};

    ukv_write_t write_options {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };

    for (auto& option : options) {
        write_options.options = option;
        ukv_write(&write_options);

        EXPECT_TRUE(status);
    }

    if (!ukv_supports_named_collections_k) {
        ukv_collection_t collections[count] = {1, 2, 3};
        ukv_write_t write {
            .db = db,
            .error = status.member_ptr(),
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = &collections[0],
            .collections_stride = sizeof(ukv_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ukv_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };

        ukv_write(&write);

        EXPECT_FALSE(status);
        status.release_error();

        ukv_collection_t collections_only_default[count] = {0, 0, 0};
        ukv_write_t write_rel {
            .db = db,
            .error = status.member_ptr(),
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = &collections_only_default[0],
            .collections_stride = sizeof(ukv_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ukv_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };

        ukv_write(&write_rel);

        EXPECT_TRUE(status);
    }

    ukv_collection_t* null_collection = nullptr;
    ukv_write_t write_null_coll {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = null_collection,
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ukv_write(&write_null_coll);

    EXPECT_TRUE(status);

    ukv_write_t write_named {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = named_collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    // Named Collection
    ukv_write(&write_named);

    if (ukv_supports_named_collections_k)
        EXPECT_TRUE(status);
    else {
        EXPECT_FALSE(status);
        status.release_error();
    }

    ukv_write_t write {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };

    ukv_write(&write);

    if (ukv_supports_transactions_k)
        EXPECT_TRUE(status);
    else {
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Transaction With Flush
    write.options = ukv_option_write_flush_k;
    ukv_write(&write);

    EXPECT_FALSE(status);
    status.release_error();

    // Count = 0, Keys!= nullptr
    write.transaction = nullptr;
    write.tasks_count = 0;
    write.collections_stride = 0;
    write.options = ukv_options_default_k;
    ukv_write(&write);

    EXPECT_FALSE(status);
    status.release_error();

    // Count > 0; Keys == nullptr
    ukv_write_t write_null_keys {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ukv_write(&write_null_keys);

    EXPECT_FALSE(status);
    status.release_error();

    // Wrong Write Options
    std::vector<ukv_options_t> wrong_write_options {
        ukv_option_transaction_dont_watch_k,
    };

    ukv_write_t write_wrong_options {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    for (auto& option : wrong_write_options) {
        write_wrong_options.options = option;
        ukv_write(&write_wrong_options);

        EXPECT_FALSE(status);
        status.release_error();
    }

    ukv_length_t* found_offsets = nullptr;
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;
    ukv_read_t read_no_txn {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };

    ukv_read(&read_no_txn);

    EXPECT_TRUE(status);

    ukv_read_t read {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .options = ukv_option_transaction_dont_watch_k,
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };

    ukv_read(&read);

    EXPECT_TRUE(status);

    // Wrong Read Options
    std::vector<ukv_options_t> wrong_read_options {
        ukv_option_write_flush_k,
        ukv_option_transaction_dont_watch_k,
    };

    for (auto& option : wrong_read_options) {
        read_no_txn.options = option;
        ukv_read(&read_no_txn);

        EXPECT_FALSE(status);
        status.release_error();
    }

    // Transaction

    ukv_transaction_t ukv_txn = nullptr;
    ukv_transaction_init_t txn_init {
        .db = db,
        .error = status.member_ptr(),
        .transaction = &ukv_txn,
    };

    ukv_transaction_init(&txn_init);
    EXPECT_TRUE(status);

    txn_init.transaction = nullptr;
    ukv_transaction_init(&txn_init);
    EXPECT_FALSE(status);
    status.release_error();

    // Wrong Transaction Begin Options
    std::vector<ukv_options_t> wrong_txn_begin_options {
        ukv_option_write_flush_k,
        ukv_option_dont_discard_memory_k,
    };

    txn_init.transaction = &ukv_txn;
    for (auto& option : wrong_txn_begin_options) {
        txn_init.options = option;
        ukv_transaction_init(&txn_init);
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Wrong Transaction Commit Options
    std::vector<ukv_options_t> wrong_txn_commit_options {
        ukv_option_dont_discard_memory_k,
    };

    ukv_transaction_commit_t txn_commit {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
    };

    for (auto& option : wrong_txn_commit_options) {
        txn_commit.options = option;
        ukv_transaction_commit(&txn_commit);
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Scans
    ukv_key_t* found_keys = nullptr;
    ukv_length_t* found_counts = nullptr;
    ukv_scan_t scan {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .start_keys = keys.data(),
        .count_limits = &count,
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };

    ukv_scan(&scan);

    EXPECT_TRUE(status);

    // Count > 0, Keys = nullptr
    ukv_scan_t scan_no_keys {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .count_limits = &count,
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };

    ukv_scan(&scan_no_keys);

    EXPECT_FALSE(status);
    status.release_error();

    // Limits == nullptr
    ukv_scan_t scan_no_limits {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .start_keys = keys.data(),
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };

    ukv_scan(&scan_no_limits);

    EXPECT_FALSE(status);
    status.release_error();
}

int main(int argc, char** argv) {
    std::filesystem::create_directory(path_k);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}