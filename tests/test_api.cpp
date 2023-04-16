#include <vector>
#include <filesystem>

#include <gtest/gtest.h>
#include <fmt/format.h>

#include "ustore/ustore.hpp"

using namespace unum::ustore;
using namespace unum;

static char const* path() {
    char* path = std::getenv("USTORE_TEST_PATH");
    if (path)
        return std::strlen(path) ? path : nullptr;

#if defined(USTORE_FLIGHT_CLIENT)
    return nullptr;
#elif defined(USTORE_TEST_PATH)
    return USTORE_TEST_PATH;
#else
    return nullptr;
#endif
}

static std::string config() {
    auto dir = path();
    if (!dir)
        return {};
    return fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir);
}

TEST(db, validation) {

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    blobs_collection_t collection = db.main();
    blobs_collection_t named_collection = *db.find_or_create("col");
    transaction_t txn = *db.transact();
    std::vector<ustore_key_t> keys {34, 35, 36};
    std::vector<std::uint64_t> vals {34, 35, 36};
    ustore_length_t val_len = sizeof(std::uint64_t);
    std::vector<ustore_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(vals.data());
    constexpr ustore_length_t count = 3;

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ustore_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = count,
    };

    using value_extractor_t = contents_arg_extractor_gt<std::remove_reference_t<contents_arg_t>>;
    auto contents = value_extractor_t {}.contents(values);
    auto offsets = value_extractor_t {}.offsets(values);
    auto lengths = value_extractor_t {}.lengths(values);

    status_t status;
    std::vector<ustore_options_t> options {ustore_options_default_k, ustore_option_write_flush_k};

    ustore_write_t write_options {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };

    for (auto& option : options) {
        write_options.options = option;
        ustore_write(&write_options);

        EXPECT_TRUE(status);
    }

    if (!ustore_supports_named_collections_k) {
        ustore_collection_t collections[count] = {1, 2, 3};
        ustore_write_t write {
            .db = db,
            .error = status.member_ptr(),
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = &collections[0],
            .collections_stride = sizeof(ustore_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ustore_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };

        ustore_write(&write);

        EXPECT_FALSE(status);
        status.release_error();

        ustore_collection_t collections_only_default[count] = {0, 0, 0};
        ustore_write_t write_rel {
            .db = db,
            .error = status.member_ptr(),
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = &collections_only_default[0],
            .collections_stride = sizeof(ustore_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ustore_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };

        ustore_write(&write_rel);

        EXPECT_TRUE(status);
    }

    ustore_collection_t* null_collection = nullptr;
    ustore_write_t write_null_coll {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = null_collection,
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ustore_write(&write_null_coll);

    EXPECT_TRUE(status);

    ustore_write_t write_named {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = named_collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    // Named Collection
    ustore_write(&write_named);

    if (ustore_supports_named_collections_k)
        EXPECT_TRUE(status);
    else {
        EXPECT_FALSE(status);
        status.release_error();
    }

    ustore_write_t write {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };

    ustore_write(&write);

    if (ustore_supports_transactions_k)
        EXPECT_TRUE(status);
    else {
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Transaction With Flush
    write.options = ustore_option_write_flush_k;
    ustore_write(&write);

    EXPECT_FALSE(status);
    status.release_error();

    // Count = 0, Keys!= nullptr
    write.transaction = nullptr;
    write.tasks_count = 0;
    write.collections_stride = 0;
    write.options = ustore_options_default_k;
    ustore_write(&write);

    EXPECT_FALSE(status);
    status.release_error();

    // Count > 0; Keys == nullptr
    ustore_write_t write_null_keys {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ustore_write(&write_null_keys);

    EXPECT_FALSE(status);
    status.release_error();

    // Wrong Write Options
    std::vector<ustore_options_t> wrong_write_options {
        ustore_option_transaction_dont_watch_k,
    };

    ustore_write_t write_wrong_options {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    for (auto& option : wrong_write_options) {
        write_wrong_options.options = option;
        ustore_write(&write_wrong_options);

        EXPECT_FALSE(status);
        status.release_error();
    }

    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;
    ustore_read_t read_no_txn {
        .db = db,
        .error = status.member_ptr(),
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };

    ustore_read(&read_no_txn);

    EXPECT_TRUE(status);

    ustore_read_t read {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
        .arena = collection.member_arena(),
        .options = ustore_option_transaction_dont_watch_k,
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };

    ustore_read(&read);

    EXPECT_TRUE(status);

    // Wrong Read Options
    std::vector<ustore_options_t> wrong_read_options {
        ustore_option_write_flush_k,
        ustore_option_transaction_dont_watch_k,
    };

    for (auto& option : wrong_read_options) {
        read_no_txn.options = option;
        ustore_read(&read_no_txn);

        EXPECT_FALSE(status);
        status.release_error();
    }

    // Transaction

    ustore_transaction_t ustore_txn = nullptr;
    ustore_transaction_init_t txn_init {
        .db = db,
        .error = status.member_ptr(),
        .transaction = &ustore_txn,
    };

    ustore_transaction_init(&txn_init);
    EXPECT_TRUE(status);

    txn_init.transaction = nullptr;
    ustore_transaction_init(&txn_init);
    EXPECT_FALSE(status);
    status.release_error();

    // Wrong Transaction Begin Options
    std::vector<ustore_options_t> wrong_txn_begin_options {
        ustore_option_write_flush_k,
        ustore_option_dont_discard_memory_k,
    };

    txn_init.transaction = &ustore_txn;
    for (auto& option : wrong_txn_begin_options) {
        txn_init.options = option;
        ustore_transaction_init(&txn_init);
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Wrong Transaction Commit Options
    std::vector<ustore_options_t> wrong_txn_commit_options {
        ustore_option_dont_discard_memory_k,
    };

    ustore_transaction_commit_t txn_commit {
        .db = db,
        .error = status.member_ptr(),
        .transaction = txn,
    };

    for (auto& option : wrong_txn_commit_options) {
        txn_commit.options = option;
        ustore_transaction_commit(&txn_commit);
        EXPECT_FALSE(status);
        status.release_error();
    }

    // Scans
    ustore_key_t* found_keys = nullptr;
    ustore_length_t* found_counts = nullptr;
    ustore_scan_t scan {
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

    ustore_scan(&scan);

    EXPECT_TRUE(status);

    // Count > 0, Keys = nullptr
    ustore_scan_t scan_no_keys {
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

    ustore_scan(&scan_no_keys);

    EXPECT_FALSE(status);
    status.release_error();

    // Limits == nullptr
    ustore_scan_t scan_no_limits {
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

    ustore_scan(&scan_no_limits);

    EXPECT_FALSE(status);
    status.release_error();
}

int main(int argc, char** argv) {
    std::filesystem::create_directory(path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}