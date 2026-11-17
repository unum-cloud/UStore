/**
 *  @file test/utilities/algorithm.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of the 128-bit sorted merges in `ustore/utilities/algorithm.hpp`: the bytewise order, the
 *      galloping cursor, the leapfrog intersection and the union walk, against `std::set` algorithms on random keys.
 *
 *  Keys are drawn so they share long prefixes and differ in either half, which a comparison of one
 *  64-bit word would get wrong.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm> // `std::sort`, `std::unique`, `std::set_intersection`, `std::set_union`
#include <iterator>  // `std::back_inserter`
#include <vector>    // `std::vector`

#include "ustore/utilities/algorithm.hpp" // `compare_128`, `sorted_128_cursor_t`, `intersect_sorted_128`

#include "harness.hpp" // `expect_true`, `run_test_case`, `split_mix_generator_t`

namespace unum::ustore::test {

using namespace unum::ustore::utilities;

#pragma region Keys

/** Orders keys through their bytes, apart from the helper under test. */
static bool key_less(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return std::memcmp(first.uuid.bytes, second.uuid.bytes, 16) < 0;
}

static bool key_equal(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return std::memcmp(first.uuid.bytes, second.uuid.bytes, 16) == 0;
}

/** A key from a small alphabet of bytes in either half, so random keys collide and share prefixes. */
static ustore_key_t random_key(split_mix_generator_t& generator) noexcept {
    ustore_key_t key {};
    std::uint64_t const bits = generator();
    key.uuid.bytes[3] = std::uint8_t(bits & 3);
    key.uuid.bytes[7] = std::uint8_t((bits >> 2) & 1) * 0x80;
    key.uuid.bytes[8] = std::uint8_t((bits >> 3) & 3) * 0x40;
    key.uuid.bytes[15] = std::uint8_t((bits >> 5) & 7);
    return key;
}

/** Sorted keys with repeats, @p count of them. */
static std::vector<ustore_key_t> random_sorted(split_mix_generator_t& generator, std::size_t count) {
    std::vector<ustore_key_t> keys;
    for (std::size_t index = 0; index != count; ++index) keys.push_back(random_key(generator));
    std::sort(keys.begin(), keys.end(), key_less);
    return keys;
}

/** The distinct keys of a sorted run. */
static std::vector<ustore_key_t> distinct(std::vector<ustore_key_t> keys) {
    keys.erase(std::unique(keys.begin(), keys.end(), key_equal), keys.end());
    return keys;
}

#pragma endregion Keys

#pragma region Cases

/** The order is bytewise, both halves count, and the high half dominates. */
void test_order_is_bytewise() {
    split_mix_generator_t generator {case_stream_key()};
    for (std::size_t round = 0; round != 20000; ++round) {
        ustore_key_t const first = random_key(generator), second = random_key(generator);
        int const wanted = std::memcmp(first.uuid.bytes, second.uuid.bytes, 16);
        int const actual = compare_128(first, second);
        expect_true((wanted < 0) == (actual < 0) && (wanted == 0) == (actual == 0));
        expect_true((actual < 0) == (first < second));
    }
    ustore_key_t low {}, high {};
    low.uuid.bytes[15] = 0xFF, high.uuid.bytes[0] = 0x01;
    expect_true(compare_128(low, high) < 0 && compare_128(high, low) > 0 && compare_128(low, low) == 0);
}

/** The cursor steps over repeats and every seek lands on the lower bound, never moving back. */
void test_cursor_seeks() {
    split_mix_generator_t generator {case_stream_key()};
    for (std::size_t round = 0; round != 500; ++round) {
        std::vector<ustore_key_t> const keys = random_sorted(generator, generator.below(200));
        std::vector<ustore_key_t> const unique = distinct(keys);
        sorted_128_cursor_t walk(keys);
        for (ustore_key_t const& key : unique) {
            expect_true(walk.valid() && key_equal(walk.key(), key));
            expect_true(walk.next().succeeded());
        }
        expect_true(!walk.valid());

        sorted_128_cursor_t seeker(keys);
        std::size_t lowest = 0;
        for (std::size_t probe = 0; probe != 40; ++probe) {
            ustore_key_t const wanted = random_key(generator);
            expect_true(seeker.seek(wanted).succeeded());
            std::size_t bound = std::size_t(std::lower_bound(keys.begin(), keys.end(), wanted, key_less) -
                                            keys.begin());
            bound = fu::max_of(bound, lowest);
            expect_true(seeker.valid() == (bound < keys.size()));
            if (bound < keys.size() && seeker.valid()) expect_true(key_equal(seeker.key(), keys[bound]));
            lowest = seeker.valid() ? std::size_t(std::lower_bound(keys.begin(), keys.end(), seeker.key(), key_less) -
                                                  keys.begin())
                                    : keys.size();
        }
    }
}

/** Both intersections match `std::set_intersection` over the distinct keys. */
void test_intersections_match() {
    split_mix_generator_t generator {case_stream_key()};
    for (std::size_t round = 0; round != 2000; ++round) {
        std::vector<ustore_key_t> const first = random_sorted(generator, generator.below(64));
        // Lopsided sizes exercise the galloping
        std::vector<ustore_key_t> const second = random_sorted(generator, round % 3 ? generator.below(64) : 500);
        std::vector<ustore_key_t> wanted;
        std::vector<ustore_key_t> const unique_first = distinct(first), unique_second = distinct(second);
        std::set_intersection(unique_first.begin(), unique_first.end(), unique_second.begin(), unique_second.end(),
                              std::back_inserter(wanted), key_less);

        std::vector<ustore_key_t> output(fu::min_of(first.size(), second.size()) + 1);
        std::size_t const count = intersect_sorted_128(first, second, output.data());
        expect_true(count == wanted.size());
        for (std::size_t index = 0; index != fu::min_of(count, wanted.size()); ++index)
            expect_true(key_equal(output[index], wanted[index]));

        sorted_128_cursor_t first_cursor(first), second_cursor(second);
        std::size_t seen = 0;
        status_t const status = intersect_sorted_128(first_cursor, second_cursor, [&](ustore_key_t const& key) {
            if (seen < wanted.size()) expect_true(key_equal(key, wanted[seen]));
            ++seen;
            return status_t {};
        });
        expect_true(status.succeeded() && seen == wanted.size());
    }

    // A failure from the callback stops the walk and comes back
    std::vector<ustore_key_t> const shared = distinct(random_sorted(generator, 100));
    sorted_128_cursor_t first_cursor(shared), second_cursor(shared);
    std::size_t calls = 0;
    status_t const stopped = intersect_sorted_128(first_cursor, second_cursor, [&](ustore_key_t const&) {
        return ++calls == 3 ? status_t {status_code_t::invalid_argument_k, "stop"} : status_t {};
    });
    expect_true(stopped == status_code_t::invalid_argument_k && calls == 3);
}

/** The union walk reports every distinct key once, with the sides that hold it. */
void test_merges_match() {
    split_mix_generator_t generator {case_stream_key()};
    for (std::size_t round = 0; round != 2000; ++round) {
        std::vector<ustore_key_t> const first = random_sorted(generator, generator.below(64));
        std::vector<ustore_key_t> const second = random_sorted(generator, generator.below(64));
        std::vector<ustore_key_t> const unique_first = distinct(first), unique_second = distinct(second);
        std::vector<ustore_key_t> wanted;
        std::set_union(unique_first.begin(), unique_first.end(), unique_second.begin(), unique_second.end(),
                       std::back_inserter(wanted), key_less);
        std::size_t seen = 0;
        status_t const status = merge_sorted_128(
            first, second, [&](ustore_key_t const& key, bool in_first, bool in_second) {
                if (seen < wanted.size()) expect_true(key_equal(key, wanted[seen]));
                expect_true(in_first == std::binary_search(first.begin(), first.end(), key, key_less));
                expect_true(in_second == std::binary_search(second.begin(), second.end(), key, key_less));
                ++seen;
                return status_t {};
            });
        expect_true(status.succeeded() && seen == wanted.size());
    }
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    run_test_case("test_order_is_bytewise", test_order_is_bytewise);
    run_test_case("test_cursor_seeks", test_cursor_seeks);
    run_test_case("test_intersections_match", test_intersections_match);
    run_test_case("test_merges_match", test_merges_match);
    return finish_test_run();
}
