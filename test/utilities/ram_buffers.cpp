/**
 *  @file test/utilities/ram_buffers.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of the arenas in `ustore/utilities/ram_buffers.hpp`: block growth, alignment, rewinding, the
 *      allocator SmashTable's containers take, and the column of strings exported as a tape.
 *
 *  The suite links no engine, only the header target and ForkUnion's allocator, so it checks the templates apart from
 *  any call routed through the C ABI.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`
#include <cstring> // `std::memcmp`

#include <array>       // `std::array`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`, `arena_allocator`, `growing_strings`
#include "ustore/utilities/types.hpp"       // `bytes_of`, `chars_of`
#include "ustore/utilities/tapes.hpp"       // `assemble_tape`, `tape_source_t`

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_equal`, `expect_status`

using namespace unum::ustore;
using namespace unum::ustore::test;

namespace unum::ustore::test {

/** An arena on the first memory domain, freed with every block it grew when the test ends. */
class scoped_arena_t {
    ustore_arena_t arena_ = new (std::nothrow) ustore_arena_handle_t {};

  public:
    scoped_arena_t() noexcept = default;
    scoped_arena_t(scoped_arena_t const&) = delete;
    scoped_arena_t& operator=(scoped_arena_t const&) = delete;
    ~scoped_arena_t() noexcept { util::arena_free(arena_); }

    operator ustore_arena_t() const noexcept { return arena_; }
    ustore_arena_handle_t* operator->() const noexcept { return arena_; }
};

/** How many blocks the chain of @p arena holds. */
std::size_t blocks_in(ustore_arena_t arena) noexcept {
    std::size_t count = 0;
    for (util::arena_block_t const* block = util::host_blocks_of(arena)->newest; block; block = block->previous)
        ++count;
    return count;
}

/** Every allocation lands aligned, the first block spans one page, and the empty and the arena-less requests stand
 *  apart. */
void test_arena_alignment() {
    scoped_arena_t arena;

    auto [nothing, nothing_status] = util::arena_allocate<std::uint32_t>(arena, 0);
    expect_status(nothing_status, util::status_code_t::success_k);
    expect_true(nothing.empty());
    expect_equal(blocks_in(arena), 0u);

    expect_status(util::arena_allocate<std::uint32_t>(nullptr, 1), util::status_code_t::invalid_argument_k);

    for (std::size_t count : {1u, 3u, 17u, 100u}) {
        auto [run, status] = util::arena_allocate<std::uint32_t>(arena, count);
        expect_status(status, util::status_code_t::success_k);
        expect_equal(run.size(), count);
        expect_equal(reinterpret_cast<std::uintptr_t>(run.data()) % util::arena_alignment_k, 0u);
    }
    expect_equal(blocks_in(arena), 1u);
    expect_equal(util::host_blocks_of(arena)->newest->capacity, util::arena_page_bytes_k);

    auto [bits, bits_status] = util::arena_allocate_bits(arena, 13);
    expect_status(bits_status, util::status_code_t::success_k);
    expect_equal(bits.size(), 2u);
    expect_true(bits[0] == 0 && bits[1] == 0);
}

/** A request past the newest block opens one at least twice its size, and a rewind keeps only the newest block. */
void test_arena_growth_and_rewind() {
    scoped_arena_t arena;

    auto [first, first_status] = util::arena_allocate<util::byte_t>(arena, 100);
    expect_status(first_status, util::status_code_t::success_k);
    std::size_t const first_capacity = util::host_blocks_of(arena)->newest->capacity;

    auto [large, large_status] = util::arena_allocate<util::byte_t>(arena, 3 * util::arena_page_bytes_k);
    expect_status(large_status, util::status_code_t::success_k);
    expect_equal(large.size(), 3 * util::arena_page_bytes_k);
    expect_equal(blocks_in(arena), 2u);
    expect_true(util::host_blocks_of(arena)->newest->capacity >= 2 * first_capacity);
    expect_equal(util::host_blocks_of(arena)->newest->capacity % util::arena_page_bytes_k, 0u);

    std::size_t const newest_capacity = util::host_blocks_of(arena)->newest->capacity;
    util::arena_rewind(arena);
    expect_equal(blocks_in(arena), 1u);
    expect_equal(util::host_blocks_of(arena)->newest->capacity, newest_capacity);
    expect_equal(util::host_blocks_of(arena)->newest->used, sizeof(util::arena_block_t));
}

/** Growing the newest allocation extends it in place, while growing an older one copies it to the end. */
void test_arena_grow_in_place() {
    scoped_arena_t arena;

    auto [older, older_status] = util::arena_allocate<std::uint32_t>(arena, 4);
    expect_status(older_status, util::status_code_t::success_k);
    for (std::size_t index = 0; index != older.size(); ++index) older[index] = static_cast<std::uint32_t>(index);

    auto [newest, newest_status] = util::arena_allocate<std::uint32_t>(arena, 4);
    expect_status(newest_status, util::status_code_t::success_k);
    auto [extended, extended_status] = util::arena_grow(arena, newest, 4);
    expect_status(extended_status, util::status_code_t::success_k);
    expect_true(extended.data() == newest.data());
    expect_equal(extended.size(), 8u);

    auto [moved, moved_status] = util::arena_grow(arena, older, 4);
    expect_status(moved_status, util::status_code_t::success_k);
    expect_true(moved.data() != older.data());
    expect_equal(moved.size(), 8u);
    for (std::size_t index = 0; index != older.size(); ++index)
        expect_equal(moved[index], static_cast<std::uint32_t>(index));

    std::array<std::uint32_t const, 3> const copied_values {7, 8, 9};
    auto [copied, copied_status] = util::arena_copy<std::uint32_t>(arena, copied_values);
    expect_status(copied_status, util::status_code_t::success_k);
    expect_true(std::memcmp(copied.data(), copied_values.data(), sizeof(copied_values)) == 0);
}

/** A SmashTable vector grows inside an arena through @c arena_allocator, and a null arena refuses to grow it. */
void test_arena_allocator_vector() {
    scoped_arena_t arena;

    st::vector<std::uint32_t, util::arena_allocator<std::uint32_t>> numbers {
        util::arena_allocator<std::uint32_t> {arena}};
    for (std::uint32_t number = 0; number != 5000; ++number)
        expect_status(util::growth_status(numbers.emplace_back(number)), util::status_code_t::success_k);
    expect_equal(numbers.size(), 5000u);
    for (std::uint32_t number = 0; number != 5000; ++number) expect_equal(numbers[number], number);
    expect_true(blocks_in(arena) > 1);

    st::vector<std::uint32_t, util::arena_allocator<std::uint32_t>> refused {util::arena_allocator<std::uint32_t> {}};
    expect_status(util::growth_status(refused.emplace_back(1u)), util::status_code_t::out_of_memory_k);
}

/** A column of strings keeps empty strings and embedded zeros, and exports into the arena as a 32-bit tape. */
void test_growing_strings_export() {
    scoped_arena_t scratch;
    scoped_arena_t outputs;

    std::array<std::string_view, 4> const texts {"alpha", "", std::string_view {"zero\0byte", 9}, "omega"};
    util::growing_strings<util::arena_allocator<util::byte_t>> column {util::arena_allocator<util::byte_t> {scratch}};
    expect_equal(column.size(), 0u);
    for (std::string_view text : texts)
        expect_status(column.push_back(util::bytes_of(text)), util::status_code_t::success_k);
    expect_equal(column.size(), texts.size());
    for (std::size_t index = 0; index != texts.size(); ++index)
        expect_true(util::chars_of(column[index]) == texts[index]);

    ustore_strings_view_t const tape = column.tape();
    expect_true(tape.layout == ustore_strings_u64_tape_k);
    expect_equal(tape.data.u64_tape.offsets[texts.size()], 19u);

    ustore_strings_view_t exported {};
    expect_status(column.export_into(outputs, exported), util::status_code_t::success_k);
    expect_true(exported.layout == ustore_strings_u32_tape_k);
    std::uint32_t const* const offsets = exported.data.u32_tape.offsets;
    for (std::size_t index = 0; index != texts.size(); ++index) {
        std::string_view const read {reinterpret_cast<char const*>(exported.data.u32_tape.data) + offsets[index],
                                     offsets[index + 1] - offsets[index]};
        expect_true(read == texts[index]);
    }

    ustore_strings_view_t untouched {};
    util::growing_strings<util::arena_allocator<util::byte_t>> empty {util::arena_allocator<util::byte_t> {scratch}};
    expect_status(empty.export_into(outputs, untouched), util::status_code_t::success_k);
    expect_true(untouched.data.u32_tape.offsets == nullptr);
}

/** A tape laid out of many strings, every third empty, holds every string at its offset, at either offset width, and
 *  a tape of no strings is its zero offset alone. */
void test_assemble_tape_lays_out_both_widths() {
    constexpr std::size_t count_k = 1000;
    std::array<util::byte_t, 64> pattern {};
    for (std::size_t index = 0; index != pattern.size(); ++index)
        pattern[index] = static_cast<util::byte_t>(index * 7 + 1);
    auto const length_of = [](std::size_t index) noexcept -> std::uint64_t { return index % 3 ? index % 61 + 1 : 0; };
    auto const sources = [&](std::size_t index) noexcept {
        return util::tape_source_t {length_of(index) ? pattern.data() + index % 3 : nullptr, length_of(index)};
    };
    std::uint64_t total = 0;
    for (std::size_t index = 0; index != count_k; ++index) total += length_of(index);

    st::vector<util::byte_t> bytes;
    st::vector<std::uint32_t> narrow;
    st::vector<std::uint64_t> wide;
    expect_true(bytes.resize(static_cast<std::size_t>(total)) == st::status_t::success_k);
    expect_true(narrow.resize(count_k + 1) == st::status_t::success_k);
    if (!expect_true(wide.resize(count_k + 1) == st::status_t::success_k)) return;

    util::assemble_tape(count_k, sources, std::span<std::uint64_t> {wide}, std::span<util::byte_t> {bytes});
    std::size_t misplaced = 0;
    std::uint64_t expected_offset = 0;
    for (std::size_t index = 0; index != count_k; ++index) {
        misplaced += wide[index] != expected_offset;
        misplaced += std::memcmp(bytes.data() + wide[index], pattern.data() + index % 3,
                                 static_cast<std::size_t>(length_of(index))) != 0;
        expected_offset += length_of(index);
    }
    expect_equal(misplaced, std::size_t {0});
    expect_equal(wide[count_k], total);

    st::vector<util::byte_t> narrow_bytes;
    if (!expect_true(narrow_bytes.resize(static_cast<std::size_t>(total)) == st::status_t::success_k)) return;
    util::assemble_tape(count_k, sources, std::span<std::uint32_t> {narrow}, std::span<util::byte_t> {narrow_bytes});
    std::size_t differing = 0;
    for (std::size_t index = 0; index <= count_k; ++index) differing += narrow[index] != wide[index];
    expect_equal(differing, std::size_t {0});
    expect_true(std::memcmp(narrow_bytes.data(), bytes.data(), bytes.size()) == 0);

    std::array<std::uint32_t, 1> none {7};
    util::assemble_tape(0, sources, std::span<std::uint32_t> {none}, std::span<util::byte_t> {});
    expect_equal(none[0], 0u);
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    run_test_case("test_arena_alignment", test_arena_alignment);
    run_test_case("test_arena_growth_and_rewind", test_arena_growth_and_rewind);
    run_test_case("test_arena_grow_in_place", test_arena_grow_in_place);
    run_test_case("test_arena_allocator_vector", test_arena_allocator_vector);
    run_test_case("test_growing_strings_export", test_growing_strings_export);
    run_test_case("test_assemble_tape_lays_out_both_widths", test_assemble_tape_lays_out_both_widths);

    return finish_test_run();
}
