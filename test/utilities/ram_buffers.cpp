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

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`, `arena_allocator`, `growing_strings`
#include "ustore/utilities/types.hpp"       // `bytes_of`, `string_of`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `expect_status`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;

namespace unum::ustore::test {

/** An arena on the first memory domain, freed with every block it grew when the test ends. */
class scoped_arena_t {
    ustore_arena_t arena_ = new (std::nothrow) ustore_arena_handle_t {};

  public:
    scoped_arena_t() noexcept = default;
    scoped_arena_t(scoped_arena_t const&) = delete;
    scoped_arena_t& operator=(scoped_arena_t const&) = delete;
    ~scoped_arena_t() noexcept { arena_free(arena_); }

    operator ustore_arena_t() const noexcept { return arena_; }
    ustore_arena_handle_t* operator->() const noexcept { return arena_; }
};

/** How many blocks the chain of @p arena holds. */
std::size_t blocks_in(ustore_arena_t arena) noexcept {
    std::size_t count = 0;
    for (arena_block_t const* block = host_blocks_of(arena)->newest; block; block = block->previous) ++count;
    return count;
}

/** Every allocation lands aligned, the first block spans one page, and the empty and the arena-less requests stand
 *  apart. */
std::size_t arena_alignment() {
    std::size_t failures = 0;
    scoped_arena_t arena;

    auto [nothing, nothing_status] = arena_allocate<std::uint32_t>(arena, 0);
    failures += expect_status(nothing_status, status_code_t::success_k);
    failures += expect(nothing.empty());
    failures += expect_eq(blocks_in(arena), 0u);

    failures += expect_status(arena_allocate<std::uint32_t>(nullptr, 1), status_code_t::invalid_argument_k);

    for (std::size_t count : {1u, 3u, 17u, 100u}) {
        auto [run, status] = arena_allocate<std::uint32_t>(arena, count);
        failures += expect_status(status, status_code_t::success_k);
        failures += expect_eq(run.size(), count);
        failures += expect_eq(reinterpret_cast<std::uintptr_t>(run.data()) % arena_alignment_k, 0u);
    }
    failures += expect_eq(blocks_in(arena), 1u);
    failures += expect_eq(host_blocks_of(arena)->newest->capacity, arena_page_bytes_k);

    auto [bits, bits_status] = arena_allocate_bits(arena, 13);
    failures += expect_status(bits_status, status_code_t::success_k);
    failures += expect_eq(bits.size(), 2u);
    failures += expect(bits[0] == 0 && bits[1] == 0);
    return failures;
}

/** A request past the newest block opens one at least twice its size, and a rewind keeps only the newest block. */
std::size_t arena_growth_and_rewind() {
    std::size_t failures = 0;
    scoped_arena_t arena;

    auto [first, first_status] = arena_allocate<byte_t>(arena, 100);
    failures += expect_status(first_status, status_code_t::success_k);
    std::size_t const first_capacity = host_blocks_of(arena)->newest->capacity;

    auto [large, large_status] = arena_allocate<byte_t>(arena, 3 * arena_page_bytes_k);
    failures += expect_status(large_status, status_code_t::success_k);
    failures += expect_eq(large.size(), 3 * arena_page_bytes_k);
    failures += expect_eq(blocks_in(arena), 2u);
    failures += expect(host_blocks_of(arena)->newest->capacity >= 2 * first_capacity);
    failures += expect_eq(host_blocks_of(arena)->newest->capacity % arena_page_bytes_k, 0u);

    std::size_t const newest_capacity = host_blocks_of(arena)->newest->capacity;
    arena_rewind(arena);
    failures += expect_eq(blocks_in(arena), 1u);
    failures += expect_eq(host_blocks_of(arena)->newest->capacity, newest_capacity);
    failures += expect_eq(host_blocks_of(arena)->newest->used, sizeof(arena_block_t));
    return failures;
}

/** Growing the newest allocation extends it in place, while growing an older one copies it to the end. */
std::size_t arena_grow_in_place() {
    std::size_t failures = 0;
    scoped_arena_t arena;

    auto [older, older_status] = arena_allocate<std::uint32_t>(arena, 4);
    failures += expect_status(older_status, status_code_t::success_k);
    for (std::size_t index = 0; index != older.size(); ++index) older[index] = static_cast<std::uint32_t>(index);

    auto [newest, newest_status] = arena_allocate<std::uint32_t>(arena, 4);
    failures += expect_status(newest_status, status_code_t::success_k);
    auto [extended, extended_status] = arena_grow(arena, newest, 4);
    failures += expect_status(extended_status, status_code_t::success_k);
    failures += expect(extended.data() == newest.data());
    failures += expect_eq(extended.size(), 8u);

    auto [moved, moved_status] = arena_grow(arena, older, 4);
    failures += expect_status(moved_status, status_code_t::success_k);
    failures += expect(moved.data() != older.data());
    failures += expect_eq(moved.size(), 8u);
    for (std::size_t index = 0; index != older.size(); ++index)
        failures += expect_eq(moved[index], static_cast<std::uint32_t>(index));

    std::array<std::uint32_t const, 3> const copied_values {7, 8, 9};
    auto [copied, copied_status] = arena_copy<std::uint32_t>(arena, copied_values);
    failures += expect_status(copied_status, status_code_t::success_k);
    failures += expect(std::memcmp(copied.data(), copied_values.data(), sizeof(copied_values)) == 0);
    return failures;
}

/** A SmashTable vector grows inside an arena through @c arena_allocator, and a null arena refuses to grow it. */
std::size_t arena_allocator_vector() {
    std::size_t failures = 0;
    scoped_arena_t arena;

    st::basic_vector<std::uint32_t, arena_allocator<std::uint32_t>> numbers {arena_allocator<std::uint32_t> {arena}};
    for (std::uint32_t number = 0; number != 5000; ++number)
        failures += expect_status(growth_status(numbers.emplace_back(number)), status_code_t::success_k);
    failures += expect_eq(numbers.size(), 5000u);
    for (std::uint32_t number = 0; number != 5000; ++number) failures += expect_eq(numbers[number], number);
    failures += expect(blocks_in(arena) > 1);

    st::basic_vector<std::uint32_t, arena_allocator<std::uint32_t>> refused {arena_allocator<std::uint32_t> {}};
    failures += expect_status(growth_status(refused.emplace_back(1u)), status_code_t::out_of_memory_k);
    return failures;
}

/** A column of strings keeps empty strings and embedded zeros, and exports into the arena as a 32-bit tape. */
std::size_t growing_strings_export() {
    std::size_t failures = 0;
    scoped_arena_t scratch;
    scoped_arena_t outputs;

    std::array<std::string_view, 4> const texts {"alpha", "", std::string_view {"zero\0byte", 9}, "omega"};
    growing_strings<arena_allocator<byte_t>> column {arena_allocator<byte_t> {scratch}};
    failures += expect_eq(column.size(), 0u);
    for (std::string_view text : texts)
        failures += expect_status(column.push_back(bytes_of(text)), status_code_t::success_k);
    failures += expect_eq(column.size(), texts.size());
    for (std::size_t index = 0; index != texts.size(); ++index)
        failures += expect(string_of(column[index]) == texts[index]);

    ustore_strings_view_t const own = column.tape();
    failures += expect(own.layout == ustore_strings_u64_tape_k);
    failures += expect_eq(own.data.u64_tape.offsets[texts.size()], 19u);

    ustore_strings_view_t exported {};
    failures += expect_status(column.export_into(outputs, exported), status_code_t::success_k);
    failures += expect(exported.layout == ustore_strings_u32_tape_k);
    std::uint32_t const* const offsets = exported.data.u32_tape.offsets;
    for (std::size_t index = 0; index != texts.size(); ++index) {
        std::string_view const read {reinterpret_cast<char const*>(exported.data.u32_tape.data) + offsets[index],
                                     offsets[index + 1] - offsets[index]};
        failures += expect(read == texts[index]);
    }

    ustore_strings_view_t untouched {};
    growing_strings<arena_allocator<byte_t>> empty {arena_allocator<byte_t> {scratch}};
    failures += expect_status(empty.export_into(outputs, untouched), status_code_t::success_k);
    failures += expect(untouched.data.u32_tape.offsets == nullptr);
    return failures;
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "arena.alignment", arena_alignment);
    report(totals, "arena.growth_and_rewind", arena_growth_and_rewind);
    report(totals, "arena.grow_in_place", arena_grow_in_place);
    report(totals, "arena.allocator_vector", arena_allocator_vector);
    report(totals, "growing_strings.export", growing_strings_export);

    return suite_exit_code(totals);
}
