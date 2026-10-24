/**
 *  @file test/nvme/pages.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The pages store of the NVMe engine through the C ABI, checked against the in-memory engine: transactions
 *      editing one page side by side, and random edits read through merges, flushes, compactions and a reopen.
 *
 *  Edits of a pages collection append change records rather than rewrite the page, so two transactions editing one
 *  page both commit, and the records fold into the page wherever they lie. The in-memory engine rewrites the page on
 *  every call, so the two engines meet only in what a read renders, which every case compares.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::strlen`

#include <array>       // `std::array`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "harness.hpp" // `expect`, `expect_eq`, `report`, `suite_exit_code`, `directory_t`, `random_t`

using namespace unum::ustore;
using namespace unum::ustore::test;

#pragma region Shape

/** Pages the maintenance case edits. */
constexpr std::size_t model_pages_k = 4;

/** Calls the maintenance case makes on each engine, each a small commit of its own. */
constexpr std::size_t model_calls_k = 3000;

/** Calls between two comparisons of every page. */
constexpr std::size_t checkpoint_calls_k = 500;

/** Calls between two compactions, each flushing the memory runs first. */
constexpr std::size_t flush_calls_k = 100;

/** Bytes every collection of the maintenance case buffers before it flushes, small enough to flush many times. */
constexpr std::size_t small_write_buffer_bytes_k = 64 * 1024;

/** What a read of a missing page renders. */
constexpr std::string_view page_absent_k = "<absent>";

#pragma endregion Shape

#pragma region Helpers

/** Opens the NVMe engine over @p directory, every collection flushing past 64 KiB, every level twice the one above. */
expected<database_t> open_small_nvme(char const* directory) noexcept {
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {};
    open.database = &database;
    open.outcome = &outcome;
    open.directory = {reinterpret_cast<ustore_byte_t const*>(directory), std::strlen(directory)};
    open.collection_write_buffer_max_bytes = small_write_buffer_bytes_k;
    open.collection_value_max_size = small_write_buffer_bytes_k / 4;
    open.transaction_max_bytes = small_write_buffer_bytes_k / 2;
    open.collection_level_enlarge_factor = 2;
    ustore_database_open_nvme(&open);
    if (status_t const status {outcome}; status.failed()) return status;
    return database_t {database};
}

/** Creates a collection named @p name holding pages in @p database. */
expected<ustore_collection_id_t> create_pages_collection(ustore_database_t database, std::string_view name) {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = database, create.outcome = &outcome, create.name = bytes_of(name);
    create.modality = ustore_modality_pages_k, create.collection = &created;
    ustore_collection_create(&create);
    if (status_t const status {outcome}; status.failed()) return status;
    return created;
}

/** The Nil key, which stands for the root as a parent and for the front as an anchor. */
constexpr ustore_key_t nil_k {};

/** Blocking page calls of one task each on one collection, joining @c transaction when set. */
struct pages_client_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection {};
    ustore_transaction_t transaction = nullptr;
    arena_t arena;

    /** Fills the arguments every call shares, addressing @p page. */
    template <typename arguments_type_>
    void address(arguments_type_& arguments, ustore_outcome_t& outcome, ustore_key_t const& page) const noexcept {
        arguments.database = database, arguments.outcome = &outcome, arguments.transaction = transaction;
        arguments.count = 1, arguments.collections = {&collection, 0}, arguments.keys = {&page, 0};
    }

    status_t import(ustore_key_t page, std::string_view markup) {
        strided_strings<std::string_view> const values {{&markup, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_import_t import {};
        address(import, outcome, page);
        import.values = values.view(), import.encoding = ustore_page_encoding_xml_k;
        ustore_pages_import(&import);
        return status_t {outcome};
    }

    status_t insert(ustore_key_t page, ustore_key_t element, ustore_key_t parent, ustore_key_t anchor,
                    std::string_view fragment) {
        strided_strings<std::string_view> const values {{&fragment, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_insert_t insert {};
        address(insert, outcome, page);
        insert.elements = {&element, 0}, insert.parents = {&parent, 0}, insert.anchors = {&anchor, 0};
        insert.values = values.view(), insert.encoding = ustore_page_encoding_xml_k, insert.arena = arena;
        ustore_pages_insert(&insert);
        return status_t {outcome};
    }

    status_t remove(ustore_key_t page, ustore_key_t element) {
        ustore_outcome_t outcome {};
        ustore_pages_remove_t remove {};
        address(remove, outcome, page);
        remove.elements = {&element, 0};
        ustore_pages_remove(&remove);
        return status_t {outcome};
    }

    status_t move(ustore_key_t page, ustore_key_t element, ustore_key_t parent, ustore_key_t anchor) {
        ustore_outcome_t outcome {};
        ustore_pages_move_t move {};
        address(move, outcome, page);
        move.elements = {&element, 0}, move.parents = {&parent, 0}, move.anchors = {&anchor, 0};
        ustore_pages_move(&move);
        return status_t {outcome};
    }

    status_t splice(ustore_key_t page, ustore_key_t element, ustore_length_t start, ustore_length_t length,
                    std::string_view text) {
        strided_strings<std::string_view> const values {{&text, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_splice_t splice {};
        address(splice, outcome, page);
        splice.elements = {&element, 0}, splice.values = values.view();
        splice.text_starts = {&start, 0}, splice.text_lengths = {&length, 0};
        ustore_pages_splice(&splice);
        return status_t {outcome};
    }

    status_t set_attribute(ustore_key_t page, ustore_key_t element, std::string_view name, std::string_view value) {
        strided_strings<std::string_view> const fields {{&name, 0}};
        strided_strings<std::string_view> const values {{&value, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_set_attributes_t set {};
        address(set, outcome, page);
        set.elements = {&element, 0}, set.fields = fields.view(), set.values = values.view();
        ustore_pages_set_attributes(&set);
        return status_t {outcome};
    }

    status_t remove_attribute(ustore_key_t page, ustore_key_t element, std::string_view name) {
        strided_strings<std::string_view> const fields {{&name, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_remove_attributes_t remove {};
        address(remove, outcome, page);
        remove.elements = {&element, 0}, remove.fields = fields.view();
        ustore_pages_remove_attributes(&remove);
        return status_t {outcome};
    }

    /** The page keyed @p page rendered as XML, through the transaction when set, @c page_absent_k where missing, or
     *  the status name of a failure. */
    std::string read(ustore_key_t page) {
        ustore_key_t const root = nil_k;
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_pages_read_t read {};
        read.database = database, read.outcome = &outcome;
        read.view = transaction ? ustore_transaction_view(transaction, ustore_watch_none_k) : nullptr;
        read.count = 1, read.collections = {&collection, 0}, read.keys = {&page, 0}, read.elements = {&root, 0};
        read.presences = &presences, read.values = &values, read.arena = arena;
        read.outputs_encoding = ustore_page_encoding_xml_k;
        ustore_pages_read(&read);
        if (outcome.status != ustore_status_success_k) return ustore_status_name(outcome.status);
        if (!(presences[0] & 1)) return std::string(page_absent_k);
        return std::string(string_of(tape_string(values, 0)));
    }
};

/** A client of a fresh pages collection named @p name in @p database, or a failure. */
expected<pages_client_t> pages_client(ustore_database_t database, std::string_view name) {
    auto [collection, create_status] = create_pages_collection(database, name);
    if (create_status.failed()) return create_status;
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) return arena_status;
    return pages_client_t {database, collection, nullptr, std::move(arena)};
}

#pragma endregion Helpers

#pragma region Cases

/** Two transactions editing one page both commit, each reading its own edit before it does, and the page lands as
 *  the in-memory engine leaves it after the two edits in commit order. */
case_result_t concurrent_edits_commit(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "concurrent"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    auto [drive, drive_status] = open_small_nvme(home.c_str());
    failures += expect(memory_status.succeeded() && drive_status.succeeded());
    if (failures) return failures;
    auto [expected_pages, expected_status] = pages_client(memory, "concurrent");
    auto [stored_pages, stored_status] = pages_client(drive, "concurrent");
    failures += expect(expected_status.succeeded() && stored_status.succeeded());
    if (failures) return failures;

    ustore_key_t const page = key_from_integer(7);
    ustore_key_t const inserted = key_from_integer(70);
    std::string_view const markup = "<doc><p>one</p></doc>";
    failures += expect(expected_pages.import(page, markup).succeeded() &&
                       stored_pages.import(page, markup).succeeded());
    failures += expect(expected_pages.insert(page, inserted, nil_k, nil_k, "<p>two</p>").succeeded());
    std::string const after_insert = expected_pages.read(page);
    failures += expect(expected_pages.set_attribute(page, page, "class", "x").succeeded());
    std::string const after_both = expected_pages.read(page);

    auto [first, first_status] = drive.transaction();
    auto [second, second_status] = drive.transaction();
    failures += expect(first_status.succeeded() && second_status.succeeded());
    if (failures) return failures;
    pages_client_t first_pages {drive, stored_pages.collection, first.transaction(), std::move(stored_pages.arena)};
    auto [second_arena, second_arena_status] = arena_t::make();
    failures += expect(second_arena_status.succeeded());
    if (failures) return failures;
    pages_client_t second_pages {drive, first_pages.collection, second.transaction(), std::move(second_arena)};
    pages_client_t head_pages {drive, first_pages.collection, nullptr, std::move(expected_pages.arena)};

    failures += expect(first_pages.insert(page, inserted, nil_k, nil_k, "<p>two</p>").succeeded());
    failures += expect(second_pages.set_attribute(page, page, "class", "x").succeeded());
    failures += expect_eq(first_pages.read(page), after_insert);
    failures += expect_eq(head_pages.read(page), std::string(markup));
    failures += expect(first.commit().has_value());
    failures += expect(second.commit().has_value());
    failures += expect_eq(head_pages.read(page), after_both);
    return failures;
}

/** Random edits of a few pages, many small commits with page removals and imports among them, flushed every hundred
 *  calls into small levels that merge often, read against the in-memory engine as the records reach flushed runs, after
 *  a compaction folds them to the bottom, and after a reopen. */
case_result_t fold_survives_maintenance(char const* directory, random_t random) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "maintenance"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    failures += expect(memory_status.succeeded());
    if (failures) return failures;
    auto [expected_pages, expected_status] = pages_client(memory, "maintenance");
    failures += expect(expected_status.succeeded());
    if (failures) return failures;

    std::array<ustore_key_t, model_pages_k> pages;
    std::array<std::vector<ustore_key_t>, model_pages_k> given;
    for (std::size_t index = 0; index != model_pages_k; ++index) pages[index] = key_from_integer(1000 + index);
    std::uint64_t next_given = 1'000'000;
    std::size_t landed = 0;
    auto const text = [](random_t& draws, std::size_t longest) {
        static constexpr std::string_view alphabet = "abcxyz &<>";
        std::string result(draws.below(longest + 1), ' ');
        for (char& character : result) character = alphabet[draws.below(alphabet.size())];
        return result;
    };
    auto const escaped = [](std::string const& raw) {
        std::string result;
        for (char const character : raw)
            if (character == '&') result += "&amp;";
            else if (character == '<') result += "&lt;";
            else if (character == '>') result += "&gt;";
            else result += character;
        return result;
    };
    // A key the page gave out, live or retired, the page key itself, or Nil
    auto const any_key = [&](random_t& draws, std::size_t index) {
        std::uint64_t const roll = draws.below(10);
        if (roll == 0 || given[index].empty()) return pages[index];
        if (roll == 1) return nil_k;
        return given[index][draws.below(given[index].size())];
    };
    auto const compare_all = [&](pages_client_t& stored, std::string_view context) {
        std::size_t differing = 0;
        for (ustore_key_t const& page : pages) {
            std::string const wanted = expected_pages.read(page);
            std::string const found = stored.read(page);
            if (found == wanted) continue;
            fmt::println(stderr, "  {}: wanted `{}`, got `{}`", context, wanted, found);
            ++differing;
        }
        return differing;
    };

    ustore_collection_id_t stored_collection {};
    {
        auto [drive, drive_status] = open_small_nvme(home.c_str());
        failures += expect(drive_status.succeeded());
        if (failures) return failures;
        auto [stored_pages, stored_status] = pages_client(drive, "maintenance");
        failures += expect(stored_status.succeeded());
        if (failures) return failures;
        stored_collection = stored_pages.collection;

        for (std::size_t call = 0; call != model_calls_k && failures == 0; ++call) {
            std::size_t const index = random.below(model_pages_k);
            ustore_key_t const page = pages[index];
            std::uint64_t const roll = random.below(100);
            bool const missing = expected_pages.read(page) == page_absent_k;
            // Both engines see the same call, as each replays the draws of one seed
            auto const apply = [&](pages_client_t& client, std::uint64_t draw) -> status_t {
                random_t replay {draw};
                auto const pick = [&](std::vector<ustore_key_t> const& keys) {
                    return keys.empty() ? nil_k : keys[replay.below(keys.size())];
                };
                if (missing || roll < 4)
                    return client.import(page, "<doc><p>" + escaped(text(replay, 6)) + "</p><p/></doc>");
                if (roll < 6) return client.remove(page, nil_k);
                if (roll < 40)
                    return client.insert(page, key_from_integer(next_given), pick(given[index]),
                                         replay.below(3) ? pick(given[index]) : nil_k,
                                         "<p class=\"c\">" + escaped(text(replay, 5)) + "</p>");
                ustore_key_t const element = any_key(replay, index);
                if (roll < 50) return client.remove(page, element);
                if (roll < 62) return client.move(page, element, pick(given[index]), pick(given[index]));
                if (roll < 80) return client.splice(page, element, replay.below(6), replay.below(3), text(replay, 4));
                if (roll < 93)
                    return client.set_attribute(page, element, replay.below(2) ? "id" : "class", text(replay, 3));
                return client.remove_attribute(page, element, replay.below(2) ? "id" : "class");
            };
            std::uint64_t const draw = random.below(~std::uint64_t(0));
            status_t const wanted = apply(expected_pages, draw);
            status_t const answered = apply(stored_pages, draw);
            if (answered.code != wanted.code) {
                fmt::println(stderr, "  call {} of roll {}: wanted {}, got {}", call, roll, describe(wanted.code),
                             describe(answered.code));
                ++failures;
            }
            landed += wanted.succeeded();
            if (roll >= 6 && roll < 40 && !missing) given[index].push_back(key_from_integer(next_given++));
            // A compaction flushes the memory runs first, so the records land in runs over bases in older ones
            if (call % flush_calls_k == 0) failures += expect(drive.compact().succeeded());
            if (call % checkpoint_calls_k == 0) failures += compare_all(stored_pages, "after a checkpoint");
        }
        fmt::println("  {} of {} calls landed", landed, model_calls_k);
        failures += expect(landed * 10 > model_calls_k * 9);
        failures += compare_all(stored_pages, "after every call");
        failures += expect(drive.compact().succeeded());
        failures += compare_all(stored_pages, "after a compaction");
    }

    auto [reopened, reopen_status] = open_small_nvme(home.c_str());
    failures += expect(reopen_status.succeeded());
    if (failures) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures) return failures;
    pages_client_t stored_pages {reopened, stored_collection, nullptr, std::move(arena)};
    failures += compare_all(stored_pages, "after a reopen");
    return failures;
}

#pragma endregion Cases

int main(int argc, char const* argv[]) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    char const* const home = directory->path();
    report(totals, "pages.concurrent_edits_commit", concurrent_edits_commit, home);
    report(totals, "pages.fold_survives_maintenance", fold_survives_maintenance, home, options.random());
    return suite_exit_code(totals);
}
