#include <numeric>
#include <random>
#include <thread>
#include <mutex>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <unordered_map>

#include <gtest/gtest.h>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

static char const* path() {
    char* path = std::getenv("UKV_TEST_PATH");
    if (path)
        return path;

#if defined(UKV_FLIGHT_CLIENT)
    return nullptr;
#elif defined(UKV_TEST_PATH)
    return UKV_TEST_PATH;
#else
    return nullptr;
#endif
}

/**
 * @brief Tests the atomicity of transactions.
 *
 * T threads are created. Each tries to insert B identical values for B consecutive keys.
 * As all threads have their own way of selecting which value to write, we then test,
 * that after the ingestion, every consecutive set of B keys maps to the same values.
 *
 * @tparam threads_count_ak Number of competing threads.
 * @tparam batch_size_ak Number consecutive keys changes by each thread on each transaction.
 * @tparam deletes_periodicity_ak Every once in a while a thread removes a batch of keys instead of overwriting them.
 */
template <std::size_t threads_count_ak, std::size_t batch_size_ak, std::size_t deletes_periodicity_ak>
void insert_atomic_isolated(std::size_t count_batches) {
    database_t db;
    EXPECT_TRUE(db.open(path()));
    EXPECT_TRUE(db.clear());

    auto task = [&](size_t thread_idx) {
        std::random_device random_device;
        std::mt19937 random_generator(random_device());

        for (std::size_t idx_batch = 0; idx_batch != count_batches; ++idx_batch) {

            std::array<ukv_key_t, batch_size_ak> keys;
            ukv_key_t const first_key_in_batch = idx_batch * batch_size_ak;
            std::iota(keys.begin(), keys.end(), first_key_in_batch);

            bool const will_delete = deletes_periodicity_ak ? random_generator() % deletes_periodicity_ak == 0 : 0;
            std::uint64_t const num_value = idx_batch * threads_count_ak + thread_idx;
            value_view_t value((byte_t const*)&num_value, sizeof(num_value));

            while (true) {
                transaction_t txn = db.transact().throw_or_release();
                auto collection = txn.main();
                if (!will_delete)
                    collection[keys].assign(value);
                else
                    collection[keys].erase();
                status_t status = txn.commit();
                if (status)
                    break;
            }
        }
    };

    std::array<std::thread, threads_count_ak> threads;
    for (std::size_t i = 0; i < threads_count_ak; ++i)
        threads[i] = std::thread(task, i);
    for (std::size_t i = 0; i < threads_count_ak; ++i)
        threads[i].join();

    blobs_collection_t collection = db.main();

    for (std::size_t idx_batch = 0; idx_batch != count_batches; ++idx_batch) {
        std::array<ukv_key_t, batch_size_ak> keys;
        ukv_key_t const first_key_in_batch = idx_batch * batch_size_ak;
        std::iota(keys.begin(), keys.end(), first_key_in_batch);

        embedded_blobs_t retrieved = collection[keys].value().throw_or_release();
        for (std::size_t idx_in_batch = 1; idx_in_batch != batch_size_ak; ++idx_in_batch)
            EXPECT_EQ(retrieved[0], retrieved[idx_in_batch]);
    }

    EXPECT_TRUE(db.clear());
    db.close();
}

TEST(db, inserts) {
    insert_atomic_isolated<4, 100, 0>(1'000);
    insert_atomic_isolated<8, 100, 0>(1'000);
    insert_atomic_isolated<13, 150, 0>(1'000);
    insert_atomic_isolated<16, 10, 0>(10'000);
}

TEST(db, inserts_and_deletes) {
    insert_atomic_isolated<4, 100, 3>(1'000);
    insert_atomic_isolated<8, 100, 3>(1'000);
    insert_atomic_isolated<13, 150, 3>(1'000);
    insert_atomic_isolated<16, 10, 3>(10'000);
}

int main(int argc, char** argv) {

    if (!ukv_supports_transactions_k) {
        std::printf("Selected UKV Engine doesn't support ACID transactions\n");
        return 1;
    }

    if (path() && std::strlen(path())) {
        std::filesystem::remove_all(path());
        std::filesystem::create_directories(path());
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}