/**
 *  @file test/nvme/pages.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The pages store of the NVMe engine through the C ABI, checked against the in-memory engine: transactions
 *      editing one page side by side, and random edits read through merges, flushes, compactions and a reopen.
 *
 *  Edits of a pages collection append change deltas rather than rewrite the page, so two transactions editing one
 *  page both commit, and the deltas fold into the page wherever they lie. The in-memory engine rewrites the page on
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

#include "harness.hpp" // `expect_true`, `expect_equal`, `run_test_case`, `scratch_directory_t`

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

/** Bytes of memory runs every collection of the maintenance case holds before it flushes, small enough to flush many
 *  times. */
constexpr std::size_t small_memory_runs_bytes_k = 64 * 1024;

/** What a read of a missing page renders. */
constexpr std::string_view page_absent_k = "<absent>";

#pragma endregion Shape

#pragma region Pages Client

/** Opens the NVMe engine over a manifest in @p directory, every collection flushing past 64 KiB,
 *  every level twice the one above. */
expected<database_t> open_small_nvme(char const* directory) noexcept {
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {};
    open.database = &database;
    open.outcome = &outcome;
    std::string const manifest = fmt::format("{}/{}", directory, "pages.ustore.json");
    open.directory = {reinterpret_cast<ustore_byte_t const*>(manifest.c_str()), std::strlen(manifest.c_str())};
    open.collection_memory_runs_max_bytes = small_memory_runs_bytes_k;
    open.collection_value_max_size = small_memory_runs_bytes_k / 4;
    open.transaction_max_bytes = small_memory_runs_bytes_k / 2;
    open.collection_level_enlarge_factor = 2;
    ustore_database_open_nvme(&open);
    if (status_t const status = status_of<status_t, status_code_t>(outcome); status.failed()) return {{}, status};
    return {database_t {database}};
}

/** Creates a collection named @p name holding pages in @p database. */
expected<ustore_collection_id_t> create_pages_collection(ustore_database_t database, std::string_view name) {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = database, create.outcome = &outcome, create.name = bytes_of(name);
    create.modality = ustore_modality_pages_k, create.collection = &created;
    ustore_collection_create(&create);
    if (status_t const status = status_of<status_t, status_code_t>(outcome); status.failed()) return {{}, status};
    return {std::move(created)};
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
        return status_of<status_t, status_code_t>(outcome);
    }

    status_t insert(ustore_key_t page, ustore_key_t element, ustore_key_t parent, ustore_key_t anchor,
                    std::string_view fragment) {
        strided_strings<std::string_view> const values {{&fragment, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_insert_t insert {};
        address(insert, outcome, page);
        insert.elements = {&element, 0}, insert.parents = {&parent, 0}, insert.anchors = {&anchor, 0};
        insert.values = values.view(), insert.encoding = ustore_page_encoding_xml_k, insert.arena = arena.handle();
        ustore_pages_insert(&insert);
        return status_of<status_t, status_code_t>(outcome);
    }

    status_t remove(ustore_key_t page, ustore_key_t element) {
        ustore_outcome_t outcome {};
        ustore_pages_remove_t remove {};
        address(remove, outcome, page);
        remove.elements = {&element, 0};
        ustore_pages_remove(&remove);
        return status_of<status_t, status_code_t>(outcome);
    }

    status_t move(ustore_key_t page, ustore_key_t element, ustore_key_t parent, ustore_key_t anchor) {
        ustore_outcome_t outcome {};
        ustore_pages_move_t move {};
        address(move, outcome, page);
        move.elements = {&element, 0}, move.parents = {&parent, 0}, move.anchors = {&anchor, 0};
        ustore_pages_move(&move);
        return status_of<status_t, status_code_t>(outcome);
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
        return status_of<status_t, status_code_t>(outcome);
    }

    status_t set_attribute(ustore_key_t page, ustore_key_t element, std::string_view name, std::string_view value) {
        strided_strings<std::string_view> const fields {{&name, 0}};
        strided_strings<std::string_view> const values {{&value, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_set_attributes_t set_attributes {};
        address(set_attributes, outcome, page);
        set_attributes.elements = {&element, 0}, set_attributes.fields = fields.view(),
        set_attributes.values = values.view();
        ustore_pages_set_attributes(&set_attributes);
        return status_of<status_t, status_code_t>(outcome);
    }

    status_t remove_attribute(ustore_key_t page, ustore_key_t element, std::string_view name) {
        strided_strings<std::string_view> const fields {{&name, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_remove_attributes_t remove {};
        address(remove, outcome, page);
        remove.elements = {&element, 0}, remove.fields = fields.view();
        ustore_pages_remove_attributes(&remove);
        return status_of<status_t, status_code_t>(outcome);
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
        read.presences = &presences, read.values = &values, read.arena = arena.handle();
        read.outputs_encoding = ustore_page_encoding_xml_k;
        ustore_pages_read(&read);
        if (outcome.status != ustore_status_success_k) return ustore_status_name(outcome.status);
        if (!(presences[0] & 1)) return std::string(page_absent_k);
        return std::string(strings_view_t {values, 1}[0]);
    }
};

/** A client of a fresh pages collection named @p name in @p database, or a failure. */
expected<pages_client_t> pages_client(ustore_database_t database, std::string_view name) {
    auto [collection, create_status] = create_pages_collection(database, name);
    if (create_status.failed()) return {{}, create_status};
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) return {{}, arena_status};
    return {pages_client_t {database, collection, nullptr, std::move(arena)}};
}

#pragma endregion Pages Client

#pragma region Cases

/** Two transactions editing one page both commit, each reading its own edit before it does, and the page lands as
 *  the in-memory engine leaves it after the two edits in commit order. */
case_result_t test_concurrent_edits_commit(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::string const home = fmt::format("{}/{}", directory, "concurrent");
    if (!expect_true(scratch_directory_t::recreate(home.c_str()))) return {};
    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    auto [drive, drive_status] = open_small_nvme(home.c_str());
    if (!expect_true(!memory_status.failed() && !drive_status.failed())) return {};
    auto [expected_pages, expected_status] = pages_client(memory.handle(), "concurrent");
    auto [stored_pages, stored_status] = pages_client(drive.handle(), "concurrent");
    if (!expect_true(!expected_status.failed() && !stored_status.failed())) return {};

    ustore_key_t const page = key_from_integer(7);
    ustore_key_t const inserted = key_from_integer(70);
    std::string_view const markup = "<doc><p>one</p></doc>";
    expect_true(!expected_pages.import(page, markup).failed() && !stored_pages.import(page, markup).failed());
    expect_true(!expected_pages.insert(page, inserted, nil_k, nil_k, "<p>two</p>").failed());
    std::string const after_insert = expected_pages.read(page);
    expect_true(!expected_pages.set_attribute(page, page, "class", "x").failed());
    std::string const after_both = expected_pages.read(page);

    auto [first, first_status] = drive.transaction();
    auto [second, second_status] = drive.transaction();
    if (!expect_true(!first_status.failed() && !second_status.failed())) return {};
    pages_client_t first_pages {drive.handle(), stored_pages.collection, first.handle(), std::move(stored_pages.arena)};
    auto [second_arena, second_arena_status] = arena_t::make();
    if (!expect_true(!second_arena_status.failed())) return {};
    pages_client_t second_pages {drive.handle(), first_pages.collection, second.handle(), std::move(second_arena)};
    pages_client_t head_pages {drive.handle(), first_pages.collection, nullptr, std::move(expected_pages.arena)};

    expect_true(!first_pages.insert(page, inserted, nil_k, nil_k, "<p>two</p>").failed());
    expect_true(!second_pages.set_attribute(page, page, "class", "x").failed());
    expect_equal(first_pages.read(page), after_insert);
    expect_equal(head_pages.read(page), std::string(markup));
    expect_true(!first.commit().status.failed());
    expect_true(!second.commit().status.failed());
    expect_equal(head_pages.read(page), after_both);
    return {};
}

/** Random edits of a few pages, many small commits with page removals and imports among them, flushed every hundred
 *  calls into small levels that merge often, read against the in-memory engine as the records reach flushed runs, after
 *  a compaction folds them to the bottom, and after a reopen. */
case_result_t test_fold_survives_maintenance(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    split_mix_generator_t random {case_stream_key()};
    std::string const home = fmt::format("{}/{}", directory, "maintenance");
    if (!expect_true(scratch_directory_t::recreate(home.c_str()))) return {};
    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    if (!expect_true(!memory_status.failed())) return {};
    auto [expected_pages, expected_status] = pages_client(memory.handle(), "maintenance");
    if (!expect_true(!expected_status.failed())) return {};

    std::array<ustore_key_t, model_pages_k> pages;
    std::array<std::vector<ustore_key_t>, model_pages_k> given;
    for (std::size_t index = 0; index != model_pages_k; ++index) pages[index] = key_from_integer(1000 + index);
    std::uint64_t next_given = 1'000'000;
    std::size_t landed = 0;
    auto const text = [](split_mix_generator_t& draws, std::size_t longest) {
        static constexpr std::string_view alphabet = "abcxyz &<>";
        std::string result(draws.below(longest + 1), ' ');
        for (char& character : result) character = alphabet[draws.below(alphabet.size())];
        return result;
    };
    auto const escaped = [](std::string const& text) {
        std::string result;
        for (char const character : text)
            if (character == '&') result += "&amp;";
            else if (character == '<') result += "&lt;";
            else if (character == '>') result += "&gt;";
            else result += character;
        return result;
    };
    // A key the page gave out, live or retired, the page key itself, or Nil
    auto const any_key = [&](split_mix_generator_t& draws, std::size_t index) {
        std::uint64_t const roll = draws.below(10);
        if (roll == 0 || given[index].empty()) return pages[index];
        if (roll == 1) return nil_k;
        return given[index][draws.below(given[index].size())];
    };
    auto const compare_all = [&](pages_client_t& stored, std::string_view context) {
        bool agreed = true;
        for (ustore_key_t const& page : pages) {
            std::string const wanted = expected_pages.read(page);
            std::string const found = stored.read(page);
            agreed = expect_true(found == wanted, "{}: wanted `{}`, got `{}`", context, wanted, found) && agreed;
        }
        return agreed;
    };

    ustore_collection_id_t stored_collection {};
    {
        auto [drive, drive_status] = open_small_nvme(home.c_str());
        if (!expect_true(!drive_status.failed())) return {};
        auto [stored_pages, stored_status] = pages_client(drive.handle(), "maintenance");
        if (!expect_true(!stored_status.failed())) return {};
        stored_collection = stored_pages.collection;

        for (std::size_t call = 0; call != model_calls_k; ++call) {
            std::size_t const index = random.below(model_pages_k);
            ustore_key_t const page = pages[index];
            std::uint64_t const roll = random.below(100);
            bool const missing = expected_pages.read(page) == page_absent_k;
            // Both engines see the same call, as each replays the draws of one seed
            auto const apply = [&](pages_client_t& client, std::uint64_t draw) -> status_t {
                split_mix_generator_t replay {draw};
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
            if (!expect_true(answered.code == wanted.code, "call {} of roll {}: wanted {}, got {}", call, roll,
                             status_name(wanted.code), status_name(answered.code)))
                break;
            landed += !wanted.failed();
            if (roll >= 6 && roll < 40 && !missing) given[index].push_back(key_from_integer(next_given++));
            // A compaction flushes the memory runs first, so the records land in runs over bases in older ones
            if (call % flush_calls_k == 0 && !expect_true(!drive.compact().failed())) break;
            if (call % checkpoint_calls_k == 0 && !compare_all(stored_pages, "after a checkpoint")) break;
        }
        expect_true(landed * 10 > model_calls_k * 9, "{} of {} calls landed", landed, model_calls_k);
        compare_all(stored_pages, "after every call");
        expect_true(!drive.compact().failed());
        compare_all(stored_pages, "after a compaction");
    }

    auto [reopened, reopen_status] = open_small_nvme(home.c_str());
    if (!expect_true(!reopen_status.failed())) return {};
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return {};
    pages_client_t stored_pages {reopened.handle(), stored_collection, nullptr, std::move(arena)};
    compare_all(stored_pages, "after a reopen");
    return {};
}

#pragma endregion Cases

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    char const* const home = directory.path();
    run_test_case("test_concurrent_edits_commit", test_concurrent_edits_commit, home);
    run_test_case("test_fold_survives_maintenance", test_fold_survives_maintenance, home);
    return finish_test_run();
}
