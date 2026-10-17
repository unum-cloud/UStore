/**
 *  @file test/nvme/catalog.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The free-space map and the catalog of a volume that has no filesystem under it.
 *
 *  Neither piece touches a disk, so every case here runs in memory: the allocator is arithmetic over
 *  a region, and the catalog is one image parsed and republished. The hostile cases matter most,
 *  since the catalog is the one structure a torn write or a stray medium hands to the mount before
 *  anything has had a chance to validate it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

#include "ustore/utilities/nvme_volume.hpp" // `catalog_t`, `crc32c`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

namespace unum::ustore::test {

using namespace unum::ustore::utilities;

#pragma region Fixtures

/** Bytes of the granularity every case allocates in, which is also the portable alignment. */
constexpr std::uint64_t block_bytes_k = 4096;

/** Bytes of the coarse alignment a large allocation takes. */
constexpr std::uint64_t coarse_bytes_k = large_allocation_alignment_k;

/** Bytes of the region the alignment and the mixed-run cases hand out. */
constexpr std::uint64_t wide_region_bytes_k = std::uint64_t {64} << 20;

/** Entries the round-trip case writes and reads back. */
constexpr std::size_t many_entries_k = 1000;

/** Allocate and release cycles the mixed-run case performs. */
constexpr std::size_t many_cycles_k = 1000;

/** Extents the mixed-run case keeps live at once. */
constexpr std::size_t live_extents_k = 16;

/** A generator each case owns, so no case depends on the state another one left. */
struct random_source_t {

    /** The xorshift state, advanced by every draw. */
    std::uint64_t state = 0x9E3779B97F4A7C15ull;

    /** The next draw, which is never zero for a state that started non-zero. */
    [[nodiscard]] std::uint64_t next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

/** A map of @p bytes with the usual two granularities, empty when it would not build. */
[[nodiscard]] extent_allocator_t make_allocator(std::uint64_t bytes) noexcept {
    expected<extent_allocator_t> built = extent_allocator_t::make(extent_t {0, bytes}, block_bytes_k, coarse_bytes_k);
    if (!built) return extent_allocator_t {};
    return std::move(*built);
}

/** A buffer of @p bytes, empty when the allocation refused. */
[[nodiscard]] st::basic_vector<byte_t> make_image(std::size_t bytes) noexcept {
    st::basic_vector<byte_t> image;
    [[maybe_unused]] st::status_t const resized = image.resize(bytes);
    return image;
}

/** The whole buffer as the span a save takes. */
[[nodiscard]] std::span<byte_t> whole(st::basic_vector<byte_t>& image) noexcept { return std::span<byte_t> {image}; }

/** The whole buffer as the span a load takes. */
[[nodiscard]] std::span<byte_t const> viewed(st::basic_vector<byte_t> const& image) noexcept {
    return std::span<byte_t const> {image};
}

/** Writes a name of @p bytes into @p storage that only @p seed produces, answering it. */
[[nodiscard]] blob_name_t paint_name(std::span<byte_t> storage, std::uint64_t seed, std::size_t bytes) noexcept {
    for (std::size_t index = 0; index != 8; ++index)
        storage[index] = static_cast<byte_t>((seed >> (index * 8)) & 0xFFu);
    for (std::size_t index = 8; index != bytes; ++index)
        storage[index] = static_cast<byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
    return blob_name_t {storage.data(), bytes};
}

/** Bytes the name of @p seed occupies, spread over the whole range the format allows. */
[[nodiscard]] std::size_t name_bytes_of(std::uint64_t seed) noexcept {
    return 8 + static_cast<std::size_t>(seed % (blob_name_max_bytes_k - 7));
}

/** What the entry of @p seed holds, so one loop writes them and another checks them. */
[[nodiscard]] blob_kind_t kind_of(std::uint64_t seed) noexcept { return static_cast<blob_kind_t>(seed % 4); }

/** The range the entry of @p seed claims. */
[[nodiscard]] extent_t extent_of(std::uint64_t seed) noexcept {
    return extent_t {seed * 8192, (seed % 7 + 1) * block_bytes_k};
}

/** Fills a catalog with @p count entries of mixed kinds, names and ranges. */
[[nodiscard]] std::size_t fill_catalog(catalog_t& catalog, std::size_t count) noexcept {
    std::size_t failures = 0;
    for (std::uint64_t seed = 0; seed != count; ++seed) {
        std::array<byte_t, blob_name_max_bytes_k> storage {};
        blob_name_t const name = paint_name(storage, seed, name_bytes_of(seed));
        failures += expect(
            catalog.insert(name, kind_of(seed), extent_of(seed), extent_of(seed).length - seed % 17).succeeded());
    }
    return failures;
}

/** Recomputes the slot checksum of @p image, so a patched image fails for the reason under test. */
void reseal(std::span<byte_t> image) noexcept {
    catalog_slot_header_t header {};
    std::memcpy(&header, image.data(), sizeof(header));
    header.checksum = 0;
    header.checksum = crc32c(std::span<byte_t const> {image.data() + sizeof(header), header.records_bytes},
                             crc32c(viewed_bytes(header)));
    std::memcpy(image.data(), &header, sizeof(header));
}

#pragma endregion Fixtures

#pragma region Allocator Cases

/** Of the ranges that fit, the one that leaves the least behind is the one handed out. */
std::size_t allocator_picks_the_smallest_fitting_range() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(256 * block_bytes_k);
    failures += expect_eq(allocator.capacity(), 256 * block_bytes_k);

    std::array<extent_t, 12> blocks {};
    for (std::size_t index = 0; index != blocks.size(); ++index) {
        expected<extent_t> const taken = allocator.allocate(block_bytes_k);
        failures += expect(taken.has_value());
        if (!taken) return failures;
        failures += expect_eq(taken->offset, index * block_bytes_k);
        blocks[index] = *taken;
    }

    // The holes descend in size as the offsets ascend, so first fit answers a different range.
    failures += expect(allocator.release(blocks[1]).succeeded());
    failures += expect(allocator.release(blocks[2]).succeeded());
    failures += expect(allocator.release(blocks[3]).succeeded());
    failures += expect(allocator.release(blocks[5]).succeeded());
    failures += expect(allocator.release(blocks[7]).succeeded());
    failures += expect(allocator.release(blocks[8]).succeeded());

    expected<extent_t> const one = allocator.allocate(block_bytes_k);
    failures += expect(one.has_value());
    if (one) failures += expect_eq(one->offset, 5 * block_bytes_k);
    expected<extent_t> const two = allocator.allocate(2 * block_bytes_k);
    failures += expect(two.has_value());
    if (two) failures += expect_eq(two->offset, 7 * block_bytes_k);
    expected<extent_t> const three = allocator.allocate(3 * block_bytes_k);
    failures += expect(three.has_value());
    if (three) failures += expect_eq(three->offset, 1 * block_bytes_k);
    return failures;
}

/** A released range merges with the range before it, the one after it, and both at once. */
std::size_t allocator_coalesces_with_both_neighbours() {
    std::size_t failures = 0;
    constexpr std::uint64_t region_bytes_k = 6 * block_bytes_k;
    extent_allocator_t allocator = make_allocator(region_bytes_k);

    std::array<extent_t, 4> blocks {};
    for (std::size_t index = 0; index != blocks.size(); ++index) {
        expected<extent_t> const taken = allocator.allocate(block_bytes_k);
        failures += expect(taken.has_value());
        if (!taken) return failures;
        blocks[index] = *taken;
    }
    failures += expect_eq(allocator.free_bytes(), 2 * block_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), 2 * block_bytes_k);

    failures += expect(allocator.release(blocks[0]).succeeded());
    failures += expect_eq(allocator.free_bytes(), 3 * block_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), 2 * block_bytes_k);

    failures += expect(allocator.release(blocks[3]).succeeded());
    failures += expect_eq(allocator.free_bytes(), 4 * block_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), 3 * block_bytes_k);

    failures += expect(allocator.release(blocks[1]).succeeded());
    failures += expect_eq(allocator.free_bytes(), 5 * block_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), 3 * block_bytes_k);

    failures += expect(allocator.release(blocks[2]).succeeded());
    failures += expect_eq(allocator.free_bytes(), region_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), region_bytes_k);
    return failures;
}

/** A replay whose entries claim the same bytes twice is a catalog nobody should mount. */
std::size_t allocator_refuses_overlapping_reservations() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(16 * block_bytes_k);

    failures += expect(allocator.reserve(extent_t {0, 2 * block_bytes_k}).succeeded());
    failures += expect(allocator.reserve(extent_t {4 * block_bytes_k, block_bytes_k}).succeeded());
    failures += expect_status(allocator.reserve(extent_t {block_bytes_k, 2 * block_bytes_k}),
                              status_code_t::malformed_k);
    failures += expect_status(allocator.reserve(extent_t {16 * block_bytes_k, block_bytes_k}),
                              status_code_t::malformed_k);
    failures += expect_eq(allocator.free_bytes(), 13 * block_bytes_k);
    return failures;
}

/** An allocation larger than anything left is refused rather than trimmed to what fits. */
std::size_t allocator_refuses_what_does_not_fit() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(4 * block_bytes_k);

    failures += expect_status(allocator.allocate(5 * block_bytes_k), status_code_t::capacity_exhausted_k);
    expected<extent_t> const whole_region = allocator.allocate(4 * block_bytes_k);
    failures += expect(whole_region.has_value());
    failures += expect_eq(allocator.free_bytes(), std::uint64_t {0});
    failures += expect_status(allocator.allocate(1), status_code_t::capacity_exhausted_k);
    failures += expect_status(allocator.allocate(0), status_code_t::invalid_argument_k);
    return failures;
}

/** Releasing bytes that are free already is a defect the map refuses to absorb. */
std::size_t allocator_refuses_a_double_release() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(16 * block_bytes_k);

    expected<extent_t> const taken = allocator.allocate(block_bytes_k);
    failures += expect(taken.has_value());
    if (!taken) return failures;
    failures += expect(allocator.release(*taken).succeeded());
    failures += expect_status(allocator.release(*taken), status_code_t::invalid_argument_k);
    failures += expect(allocator.release(extent_t {8 * block_bytes_k, block_bytes_k}) ==
                       status_code_t::invalid_argument_k);
    failures += expect_eq(allocator.free_bytes(), 16 * block_bytes_k);
    return failures;
}

/** A large allocation begins on the coarse alignment, and a small one takes whatever is next. */
std::size_t allocator_aligns_a_large_allocation() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(wide_region_bytes_k);

    expected<extent_t> const first_small = allocator.allocate(block_bytes_k);
    failures += expect(first_small.has_value());
    if (first_small) failures += expect_eq(first_small->offset, std::uint64_t {0});
    expected<extent_t> const second_small = allocator.allocate(100);
    failures += expect(second_small.has_value());
    if (!second_small) return failures;
    failures += expect_eq(second_small->offset, block_bytes_k);
    failures += expect_eq(second_small->length, block_bytes_k);
    failures += expect(second_small->offset % coarse_bytes_k != 0);

    expected<extent_t> const large = allocator.allocate(large_allocation_bytes_k);
    failures += expect(large.has_value());
    if (!large) return failures;
    failures += expect_eq(large->offset, coarse_bytes_k);
    failures += expect_eq(large->offset % coarse_bytes_k, std::uint64_t {0});
    failures += expect_eq(allocator.free_bytes(), wide_region_bytes_k - 2 * block_bytes_k - large_allocation_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(),
                          wide_region_bytes_k - coarse_bytes_k - large_allocation_bytes_k);
    return failures;
}

/** A thousand mixed cycles give every byte back, which only complete coalescing achieves. */
std::size_t allocator_returns_every_byte_of_a_mixed_run() {
    std::size_t failures = 0;
    extent_allocator_t allocator = make_allocator(wide_region_bytes_k);
    random_source_t source;
    st::basic_vector<extent_t> live;

    for (std::size_t cycle = 0; cycle != many_cycles_k; ++cycle) {
        expected<extent_t> const taken = allocator.allocate(1 + source.next() % 65536);
        failures += expect(taken.has_value());
        if (!taken) return failures;
        if (live.push_back(extent_t {*taken}) != st::status_t::success_k) return failures + 1;
        if (live.size() < live_extents_k) continue;
        std::size_t const position = static_cast<std::size_t>(source.next() % live.size());
        failures += expect(allocator.release(live[position]).succeeded());
        live[position] = live[live.size() - 1];
        live.pop_back();
    }
    for (std::size_t index = 0; index != live.size(); ++index)
        failures += expect(allocator.release(live[index]).succeeded());

    failures += expect_eq(allocator.free_bytes(), wide_region_bytes_k);
    failures += expect_eq(allocator.largest_free_bytes(), wide_region_bytes_k);
    return failures;
}

#pragma endregion Allocator Cases

#pragma region Catalog Cases

/** A thousand entries of mixed kinds and name lengths survive a save and a load unchanged. */
std::size_t catalog_round_trips_every_field() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, many_entries_k);
    failures += expect_eq(catalog.size(), many_entries_k);

    st::basic_vector<byte_t> image = make_image(static_cast<std::size_t>(catalog.image_bytes()));
    failures += expect_eq(image.size(), static_cast<std::size_t>(catalog.image_bytes()));
    if (image.empty()) return failures;
    expected<std::uint64_t> const saved = catalog.save(whole(image), 7);
    failures += expect(saved == catalog.image_bytes());

    catalog_t reloaded;
    failures += expect(reloaded.load(viewed(image)).succeeded());
    failures += expect_eq(reloaded.size(), many_entries_k);
    for (std::uint64_t seed = 0; seed != many_entries_k; ++seed) {
        std::array<byte_t, blob_name_max_bytes_k> storage {};
        blob_name_t const name = paint_name(storage, seed, name_bytes_of(seed));
        catalog_entry_t const* const found = reloaded.find(name);
        failures += expect(found != nullptr);
        if (!found) continue;
        failures += expect(found->kind == kind_of(seed));
        failures += expect(found->extent == extent_of(seed));
        failures += expect_eq(found->content_length, extent_of(seed).length - seed % 17);
        failures += expect_eq(found->sequence_number, seed + 1);
        failures += expect_eq(found->name().size(), name_bytes_of(seed));
    }
    return failures;
}

/** One flipped byte anywhere in the image is a catalog the mount refuses whole. */
std::size_t catalog_refuses_a_flipped_byte() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 8);

    st::basic_vector<byte_t> image = make_image(static_cast<std::size_t>(catalog.image_bytes()));
    if (image.empty()) return failures + 1;
    failures += expect(catalog.save(whole(image), 3).has_value());

    image[sizeof(catalog_slot_header_t) + 3] ^= static_cast<byte_t>(0x01u);
    catalog_t reloaded;
    failures += expect_status(reloaded.load(viewed(image)), status_code_t::malformed_k);
    failures += expect_eq(reloaded.size(), std::size_t {0});
    return failures;
}

/** A slot whose records reach past the bytes that were read is refused before any of it is parsed. */
std::size_t catalog_refuses_records_past_the_image() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 8);

    st::basic_vector<byte_t> image = make_image(static_cast<std::size_t>(catalog.image_bytes()));
    if (image.empty()) return failures + 1;
    failures += expect(catalog.save(whole(image), 3).has_value());

    catalog_t reloaded;
    failures += expect_status(reloaded.load(viewed(image).first(image.size() - 1)), status_code_t::malformed_k);
    failures += expect(reloaded.load(viewed(image).first(sizeof(catalog_slot_header_t) - 1)) ==
                       status_code_t::malformed_k);
    return failures;
}

/** One record claiming more bytes than the slot holds, or more content than its extent, is refused, checksum or no
 *  checksum. */
std::size_t catalog_refuses_a_record_past_the_records() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 4);

    st::basic_vector<byte_t> image = make_image(static_cast<std::size_t>(catalog.image_bytes()));
    if (image.empty()) return failures + 1;
    failures += expect(catalog.save(whole(image), 3).has_value());

    catalog_slot_header_t header {};
    std::memcpy(&header, image.data(), sizeof(header));
    catalog_record_t record {};
    std::memcpy(&record, image.data() + sizeof(header), sizeof(record));
    catalog_record_t const original = record;
    record.record_bytes = static_cast<std::uint16_t>(header.records_bytes + record_alignment_k);
    std::memcpy(image.data() + sizeof(header), &record, sizeof(record));
    reseal(whole(image));

    catalog_t reloaded;
    failures += expect_status(reloaded.load(viewed(image)), status_code_t::malformed_k);

    record.record_bytes = static_cast<std::uint16_t>(sizeof(record) + record.name_bytes - 1);
    std::memcpy(image.data() + sizeof(header), &record, sizeof(record));
    reseal(whole(image));
    failures += expect_status(reloaded.load(viewed(image)), status_code_t::malformed_k);

    record = original;
    record.content_length = record.extent_length + 1;
    std::memcpy(image.data() + sizeof(header), &record, sizeof(record));
    reseal(whole(image));
    failures += expect_status(reloaded.load(viewed(image)), status_code_t::malformed_k);
    return failures;
}

/** A slot too small for the image is written nothing at all, rather than a prefix of one. */
std::size_t catalog_refuses_a_small_image() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 8);

    st::basic_vector<byte_t> image = make_image(static_cast<std::size_t>(catalog.image_bytes()));
    if (image.empty()) return failures + 1;
    expected<std::uint64_t> const saved = catalog.save(whole(image).first(image.size() - 1), 3);
    failures += expect_status(saved, status_code_t::capacity_exhausted_k);
    for (std::size_t index = 0; index != sizeof(catalog_slot_header_t); ++index)
        failures += expect(image[index] == byte_t {});
    return failures;
}

/** A name the catalog holds is never taken twice, and one the format cannot carry is never taken. */
std::size_t catalog_refuses_a_duplicate_name() {
    std::size_t failures = 0;
    catalog_t catalog;
    std::array<byte_t, blob_name_max_bytes_k + 1> storage {};
    blob_name_t const name = paint_name(std::span<byte_t> {storage}, 11, 32);

    failures += expect(catalog.insert(name, blob_kind_t::sorted_run_k, extent_t {0, block_bytes_k}, 100).succeeded());
    failures += expect(catalog.insert(name, blob_kind_t::schema_k, extent_t {block_bytes_k, block_bytes_k}, 100) ==
                       status_code_t::already_exists_k);
    failures += expect_status(
        catalog.insert(blob_name_t {storage}, blob_kind_t::user_data_k, extent_t {0, block_bytes_k}, 0),
        status_code_t::invalid_argument_k);
    failures += expect(catalog.insert(blob_name_t {}, blob_kind_t::user_data_k, extent_t {0, block_bytes_k}, 0) ==
                       status_code_t::invalid_argument_k);
    failures += expect_eq(catalog.size(), std::size_t {1});
    return failures;
}

/** An erased name answers the range it held once, and is absent from then on. */
std::size_t catalog_erases_by_name() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 3);

    std::array<byte_t, blob_name_max_bytes_k> storage {};
    blob_name_t const name = paint_name(storage, 1, name_bytes_of(1));
    expected<extent_t> const dropped = catalog.erase(name);
    failures += expect(dropped.has_value());
    failures += expect(dropped == extent_of(1));
    failures += expect(catalog.find(name) == nullptr);
    failures += expect_status(catalog.erase(name), status_code_t::not_found_k);
    failures += expect_eq(catalog.size(), std::size_t {2});

    std::array<byte_t, blob_name_max_bytes_k> other {};
    failures += expect(catalog.find(paint_name(other, 0, name_bytes_of(0))) != nullptr);
    failures += expect(catalog.find(paint_name(other, 2, name_bytes_of(2))) != nullptr);
    return failures;
}

/** A walk of one kind sees every entry of it and nothing of the other three. */
std::size_t catalog_visits_one_kind() {
    std::size_t failures = 0;
    catalog_t catalog;
    failures += fill_catalog(catalog, 64);

    for (std::uint64_t kind = 0; kind != 4; ++kind) {
        std::size_t visited = 0;
        std::size_t strangers = 0;
        catalog.for_each(kind_of(kind), [&](catalog_entry_t const& entry) noexcept {
            ++visited;
            if (entry.kind != kind_of(kind)) ++strangers;
        });
        failures += expect_eq(visited, std::size_t {16});
        failures += expect_eq(strangers, std::size_t {0});
    }
    return failures;
}

#pragma endregion Catalog Cases

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "allocator.best_fit", allocator_picks_the_smallest_fitting_range);
    report(totals, "allocator.coalesces", allocator_coalesces_with_both_neighbours);
    report(totals, "allocator.overlapping_reserve", allocator_refuses_overlapping_reservations);
    report(totals, "allocator.past_capacity", allocator_refuses_what_does_not_fit);
    report(totals, "allocator.double_release", allocator_refuses_a_double_release);
    report(totals, "allocator.large_alignment", allocator_aligns_a_large_allocation);
    report(totals, "allocator.mixed_run", allocator_returns_every_byte_of_a_mixed_run);
    report(totals, "catalog.round_trip", catalog_round_trips_every_field);
    report(totals, "catalog.flipped_byte", catalog_refuses_a_flipped_byte);
    report(totals, "catalog.records_past_image", catalog_refuses_records_past_the_image);
    report(totals, "catalog.record_past_records", catalog_refuses_a_record_past_the_records);
    report(totals, "catalog.small_image", catalog_refuses_a_small_image);
    report(totals, "catalog.duplicate_name", catalog_refuses_a_duplicate_name);
    report(totals, "catalog.erases_by_name", catalog_erases_by_name);
    report(totals, "catalog.visits_one_kind", catalog_visits_one_kind);

    return suite_exit_code(totals);
}
