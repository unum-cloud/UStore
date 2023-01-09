#include <numeric>
#include <random>
#include <thread>
#include <mutex>
#include <chrono>
#include <charconv>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <condition_variable>

#include <gtest/gtest.h>
#include <fmt/format.h>

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

enum class operation_code_t : std::uint8_t {
    insert_k,
    remove_k,
    select_k,
};

using payload_t = std::size_t;
struct operation_t {
    ukv_key_t key;
    payload_t value;
    ukv_sequence_number_t sequence;
    operation_code_t code;
    bool commited;

    value_view_t value_view() const noexcept {
        auto value_ptr = reinterpret_cast<byte_t const*>(&value);
        return value_view_t {value_ptr, sizeof(payload_t)};
    }
};

class barrier_t {

    mutable std::mutex mutex_;
    std::condition_variable conditional_;
    std::size_t size_;
    std::ptrdiff_t remaining_;
    std::ptrdiff_t phase_ = 0;
    std::function<void()> completion_;

  public:
    barrier_t(
        std::size_t s, std::function<void()> f = []() noexcept {})
        : size_(s), remaining_(s), completion_(std::move(f)) {}
    barrier_t(barrier_t const& barrier) = delete;
    barrier_t(barrier_t&& barrier) = delete;
    barrier_t& operator=(barrier_t const& barrier) = delete;
    barrier_t& operator=(barrier_t&& barrier) = delete;

    void arrive_and_wait() {
        auto l = std::unique_lock(mutex_);
        --remaining_;
        if (remaining_ != 0) {
            auto next_phase = phase_ + 1;
            conditional_.wait(l, [&] { return next_phase - phase_ <= 0; });
        }
        else {
            completion_();
            remaining_ = size_;
            ++phase_;
            conditional_.notify_all();
        }
    }

    void arrive_and_drop() {
        auto l = std::unique_lock(mutex_);
        --size_;
        --remaining_;
        if (remaining_ == 0) {
            completion_();
            remaining_ = size_;
            ++phase_;
            conditional_.notify_all();
        }
    }
};

std::string log_operations(std::vector<operation_t> const& ops) {
    std::string logs;
    for (operation_t const& op : ops) {
        auto mark = op.commited ? "✅" : "❌";
        auto back = std::back_inserter(logs);
        switch (op.code) {
        case operation_code_t::insert_k:
            fmt::format_to(back, "{} {}. main[{}].assign(\"{}\");\n", op.sequence, mark, op.key, op.value);
            break;
        case operation_code_t::remove_k: //
            fmt::format_to(back, "{} {}. main[{}].erase();\n", op.sequence, mark, op.key);
            break;
        default: break;
        }
    }
    return logs;
}

template <typename associative_container_at>
std::string log_contents(associative_container_at&& elements) {
    std::string logs;
    for (auto element : elements) {
        std::string payload;
        using second_t = std::remove_all_extents_t<decltype(element.second)>;
        if constexpr (std::is_same_v<second_t, payload_t>)
            payload = std::to_string(element.second);
        else
            payload = element.second.size() //
                          ? std::to_string(*reinterpret_cast<payload_t const*>(element.second.data()))
                          : "";
        fmt::format_to(std::back_inserter(logs), "{}={}\n", element.first, payload);
    }
    return logs;
}

template <typename expected_container_at, typename received_container_at>
std::string log_comparison(std::vector<operation_t> const& ops,
                           expected_container_at&& expected,
                           received_container_at&& received) {
    std::stringstream logs;
    logs                          //
        << "Operations:\n"        //
        << log_operations(ops)    //
        << "Expected contents:\n" //
        << log_contents(expected) //
        << "Received contents:\n" //
        << log_contents(received);
    return logs.str();
}

/**
 * @brief
 *
 * On every thread runs random write operations: insertions and removals.
 * After ::transactions_between_checkpoints it reaches a checkpoint, where all threads stop.
 *
 * @tparam part_inserts_ak
 * @tparam part_removes_ak
 * @tparam part_selects_ak
 * @param transactions_between_checkpoints
 * @param concurrent_threads
 */
template <std::size_t part_inserts_ak, std::size_t part_removes_ak, std::size_t part_selects_ak = 0>
void linear_writes( //
    database_t& db,
    std::size_t transactions_between_checkpoints,
    std::size_t concurrent_threads,
    std::size_t max_checkpoints = 1'000) {

    std::unordered_map<ukv_key_t, payload_t> sequential;

    barrier_t sync_point(concurrent_threads);

    constexpr std::size_t parts_total_k = part_inserts_ak + part_removes_ak;
    constexpr std::size_t mean_key_frequency_k = 4;
    ukv_key_t max_key = parts_total_k * transactions_between_checkpoints * concurrent_threads / mean_key_frequency_k;
    std::uniform_int_distribution<ukv_key_t> dist_keys(1, max_key);

    std::size_t operations_per_thread = transactions_between_checkpoints * parts_total_k;
    std::vector<operation_t> operations_across_threads {concurrent_threads * operations_per_thread};

    auto thread_logic = [&](std::size_t thread_idx) {
        std::random_device random_device;
        std::mt19937 random_generator(random_device());
        operation_t* operations = operations_across_threads.data() + operations_per_thread * thread_idx;
        bool produced_error = false;
        std::size_t passed_checkpoints = 0;

        transaction_t txn = db.transact().throw_or_release();

        while (!produced_error && passed_checkpoints < max_checkpoints) {
            // Make a few transactions in a row.
            // They will be of identical size, but with different keys.
            for (std::size_t iteration = 0; iteration != transactions_between_checkpoints; ++iteration) {
                txn.reset().throw_unhandled();
                for (std::size_t part = 0; part != parts_total_k; ++part) {
                    operation_t& op = operations[iteration * parts_total_k + part];
                    op.code = (random_generator() % parts_total_k) > part_inserts_ak //
                                  ? operation_code_t::insert_k
                                  : operation_code_t::remove_k;
                    op.key = dist_keys(random_generator);
                    op.value = random_generator();
                    switch (op.code) {
                    case operation_code_t::insert_k: txn[op.key].assign(op.value_view()).throw_unhandled(); break;
                    case operation_code_t::remove_k: txn[op.key].erase().throw_unhandled(); break;
                    default: break;
                    }
                }
                auto maybe_sequence = txn.sequenced_commit();
                auto commited = bool(maybe_sequence);
                auto sequence =
                    commited ? maybe_sequence.throw_or_ref() : std::numeric_limits<ukv_sequence_number_t>::max();
                for (std::size_t part = 0; part != parts_total_k; ++part) {
                    operation_t& op = operations[iteration * parts_total_k + part];
                    op.commited = commited;
                    op.sequence = sequence;
                }
            }

            sync_point.arrive_and_wait();

            // Only the main thread will perform validation
            if (thread_idx == 0) {
                // Sort the operations across the threads according to their evaluation order.
                // The sort must be stable, as the same key may be inserted and deleted within the same transaction.
                // Those operations will have sequence number, but their relative order must be preserved.
                std::stable_sort(operations_across_threads.begin(),
                                 operations_across_threads.end(),
                                 [](operation_t const& a, operation_t const& b) { return a.sequence < b.sequence; });

                // Now repeat everything on top of a simple non-concurrent container
                sequential.reserve(operations_across_threads.size());
                for (operation_t& op : operations_across_threads) {
                    if (!op.commited)
                        continue;
                    switch (op.code) {
                    case operation_code_t::insert_k: sequential[op.key] = op.value; break;
                    case operation_code_t::remove_k: sequential.erase(op.key); break;
                    default: break;
                    }
                }

                // Now check that the contents of both collections are identical
                blobs_collection_t concurrent = db.main();
                EXPECT_EQ(sequential.size(), concurrent.items().size())
                    << log_comparison(operations_across_threads, sequential, concurrent.items());

                for (auto const& kv : sequential) {
                    payload_t expected = kv.second;
                    auto maybe_retrieved_str = concurrent[kv.first].value();
                    EXPECT_TRUE(maybe_retrieved_str)                                                 //
                        << log_comparison(operations_across_threads, sequential, concurrent.items()) //
                        << "Failed to retrieve: " << kv.first;
                    value_view_t retrieved_str = maybe_retrieved_str.throw_or_ref();
                    EXPECT_TRUE(retrieved_str)                                                       //
                        << log_comparison(operations_across_threads, sequential, concurrent.items()) //
                        << "Missing key: " << kv.first;
                    payload_t retrieved = *reinterpret_cast<payload_t const*>(retrieved_str.data());
                    EXPECT_EQ(expected, retrieved)                                                   //
                        << log_comparison(operations_across_threads, sequential, concurrent.items()) //
                        << "Received wrong value for: " << kv.first;
                }
                EXPECT_TRUE(concurrent.clear());
                EXPECT_TRUE(db.clear());
                sequential.clear();
            }

            // Continue into
            sync_point.arrive_and_wait();
            ++passed_checkpoints;
        }
    };

    std::vector<std::thread> threads;
    for (std::size_t thread_idx = 0; thread_idx != concurrent_threads; ++thread_idx)
        threads.push_back(std::thread(thread_logic, thread_idx));
    for (auto& thread : threads)
        thread.join();

    EXPECT_TRUE(db.clear());
}

void test_writes(database_t& db, std::size_t thread_count, std::size_t checkpoint_frequency) {
    // Just Writes
    linear_writes<1, 0>(db, checkpoint_frequency, thread_count);
    linear_writes<2, 0>(db, checkpoint_frequency, thread_count);
    linear_writes<3, 0>(db, checkpoint_frequency, thread_count);
    linear_writes<4, 0>(db, checkpoint_frequency, thread_count);
    linear_writes<10, 0>(db, checkpoint_frequency, thread_count);

    // Mixing
    linear_writes<1, 1>(db, checkpoint_frequency, thread_count);
    linear_writes<2, 1>(db, checkpoint_frequency, thread_count);
    linear_writes<3, 1>(db, checkpoint_frequency, thread_count);
    linear_writes<4, 1>(db, checkpoint_frequency, thread_count);
    linear_writes<10, 1>(db, checkpoint_frequency, thread_count);

    // Larger Batches
    linear_writes<10, 5>(db, checkpoint_frequency, thread_count);
    linear_writes<30, 3>(db, checkpoint_frequency, thread_count);
}

class test_one_config_t : public testing::Test {
    database_t db;
    std::size_t thread_count;
    std::size_t checkpoint_frequency;

  public:
    test_one_config_t(std::size_t thread_count, std::size_t checkpoint_frequency)
        : thread_count(thread_count), checkpoint_frequency(checkpoint_frequency) {}

    static void SetUpTestSuite() {}
    static void TearDownTestSuite() {}
    void SetUp() override { EXPECT_TRUE(db.open(path())); }
    void TearDown() override { EXPECT_TRUE(db.clear()); }
    void TestBody() override { test_writes(db, thread_count, checkpoint_frequency); }
};

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

    std::vector<std::size_t> thread_counts {2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<std::size_t> checkpoint_frequencies {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 50, 100};

    for (auto thread_count : thread_counts)
        for (auto checkpoint_frequency : checkpoint_frequencies)
            testing::RegisterTest(
                "linear_writes",
                fmt::format("{} threads, {} transactions between checks", thread_count, checkpoint_frequency).c_str(),
                nullptr,
                nullptr,
                __FILE__,
                __LINE__,
                [=]() -> test_one_config_t* { return new test_one_config_t(thread_count, checkpoint_frequency); });

    return RUN_ALL_TESTS();
}