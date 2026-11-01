/**
 *  @file test/abi/pages.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Unit tests of the pages modality through the C++ wrappers, and the refusals of
 *      its C calls.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`

#include <random>      // `std::mt19937_64`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::underlying_type_t`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `create_collection`, `tape_written`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_eq`, `expect_status`
#include "pages/model.hpp" // `model_page_t`, `model_edit_t`, `node_spec_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::pages;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

#pragma region Pages Modality

/** The key of the C ABI spelling the same 16 bytes as @p element. */
ustore_key_t page_key_of(element_key_t const& element) noexcept {
    ustore_key_t key;
    std::memcpy(&key, element.bytes, sizeof(key));
    return key;
}

/** The element key spelling the same 16 bytes as @p key. */
element_key_t element_key_of(ustore_key_t const& key) noexcept {
    element_key_t element;
    std::memcpy(element.bytes, &key, sizeof(key));
    return element;
}

/** One task of a page call, each verb reading the fields it takes. */
struct page_task_t {
    element_key_t element {}, parent {}, anchor {};
    std::uint64_t start = 0, length = 0;
    std::string field, value;
};

/** What a read of a missing page or element answers in @c pages_client_t::read. */
constexpr std::string_view page_absent_k = "<absent>";

/** Whether an insertion's elements carry the keys the caller gave, or take the keys the call
 *  mints. */
enum class key_source_t : bool { given_k, minted_k };

/** Blocking page calls on one collection of a database, through the C ABI,
 *  joining @c transaction when set. */
struct pages_client_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection {};
    ustore_transaction_t transaction = nullptr;
    arena_t arena;

    /** Replaces the page keyed @p page with @p markup. */
    status_t import(element_key_t const& page, std::string_view markup,
                    ustore_page_encoding_t encoding = ustore_page_encoding_xml_k) {
        ustore_key_t const key = page_key_of(page);
        strided_strings<std::string_view> const values {{&markup, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_import_t import {};
        import.database = database, import.outcome = &outcome, import.transaction = transaction;
        import.count = 1, import.collections = {&collection, 0}, import.keys = {&key, 0};
        import.values = values.view(), import.encoding = encoding;
        ustore_pages_import(&import);
        return status_t {outcome};
    }

    /** Applies @p tasks of @p verb to the page keyed @p page in one call, the effective keys of an
     *  insertion landing in @p effective. */
    status_t edit(modification_t verb, element_key_t const& page, std::vector<page_task_t> const& tasks,
                  std::vector<element_key_t>* effective = nullptr, key_source_t key_source = key_source_t::given_k) {
        ustore_key_t const key = page_key_of(page);
        std::vector<ustore_key_t> elements, parents, anchors;
        std::vector<ustore_length_t> starts, lengths;
        std::vector<std::string_view> fields, values;
        for (page_task_t const& task : tasks) {
            elements.push_back(page_key_of(task.element)), parents.push_back(page_key_of(task.parent));
            anchors.push_back(page_key_of(task.anchor)), starts.push_back(task.start), lengths.push_back(task.length);
            fields.push_back(task.field), values.push_back(task.value);
        }
        strided_strings<std::string_view> const fields_strings {{fields.data(), sizeof(std::string_view)}};
        strided_strings<std::string_view> const values_strings {{values.data(), sizeof(std::string_view)}};
        ustore_keys_view_t const elements_view {elements.data(), sizeof(ustore_key_t)};
        ustore_keys_view_t const parents_view {parents.data(), sizeof(ustore_key_t)};
        ustore_keys_view_t const anchors_view {anchors.data(), sizeof(ustore_key_t)};
        ustore_outcome_t outcome {};
        auto fill = [&](auto& arguments) {
            arguments.database = database, arguments.outcome = &outcome, arguments.transaction = transaction;
            arguments.count = tasks.size(), arguments.collections = {&collection, 0}, arguments.keys = {&key, 0};
        };
        switch (verb) {
        case modification_t::insert_k: {
            ustore_key_t const* landed = nullptr;
            ustore_pages_insert_t insert {};
            fill(insert);
            insert.elements = key_source == key_source_t::minted_k ? ustore_keys_view_t {} : elements_view;
            insert.parents = parents_view, insert.anchors = anchors_view;
            insert.values = values_strings.view(), insert.encoding = ustore_page_encoding_xml_k;
            insert.effective_keys = &landed, insert.arena = arena;
            ustore_pages_insert(&insert);
            if (effective && outcome.status == ustore_status_success_k)
                for (std::size_t index = 0; index != tasks.size(); ++index)
                    effective->push_back(element_key_of(landed[index]));
            break;
        }
        case modification_t::remove_k: {
            ustore_pages_remove_t remove {};
            fill(remove);
            remove.elements = elements_view;
            ustore_pages_remove(&remove);
            break;
        }
        case modification_t::move_k: {
            ustore_pages_move_t move {};
            fill(move);
            move.elements = elements_view, move.parents = parents_view, move.anchors = anchors_view;
            ustore_pages_move(&move);
            break;
        }
        case modification_t::splice_k: {
            ustore_pages_splice_t splice {};
            fill(splice);
            splice.elements = elements_view, splice.values = values_strings.view();
            splice.text_starts = {starts.data(), sizeof(ustore_length_t)};
            splice.text_lengths = {lengths.data(), sizeof(ustore_length_t)};
            ustore_pages_splice(&splice);
            break;
        }
        case modification_t::set_attributes_k: {
            ustore_pages_set_attributes_t set_attributes {};
            fill(set_attributes);
            set_attributes.elements = elements_view, set_attributes.fields = fields_strings.view(),
            set_attributes.values = values_strings.view();
            ustore_pages_set_attributes(&set_attributes);
            break;
        }
        case modification_t::remove_attributes_k: {
            ustore_pages_remove_attributes_t remove {};
            fill(remove);
            remove.elements = elements_view, remove.fields = fields_strings.view();
            ustore_pages_remove_attributes(&remove);
            break;
        }
        default: return {status_code_t::invalid_argument_k, "Imports go through their own call"};
        }
        return status_t {outcome};
    }

    /** The rendering of @p element of the page keyed @p page, the whole page for
     *  Nil, @c page_absent_k where missing, or the status name of a failure. */
    std::string read(element_key_t const& page, element_key_t const& element = {},
                     ustore_page_encoding_t encoding = ustore_page_encoding_xml_k, ustore_view_t view = nullptr) {
        ustore_key_t const key = page_key_of(page);
        ustore_key_t const wanted = page_key_of(element);
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_pages_read_t read {};
        read.database = database, read.outcome = &outcome, read.view = view;
        read.count = 1, read.collections = {&collection, 0}, read.keys = {&key, 0}, read.elements = {&wanted, 0};
        read.presences = &presences, read.values = &values, read.arena = arena, read.outputs_encoding = encoding;
        ustore_pages_read(&read);
        if (outcome.status != ustore_status_success_k) return ustore_status_name(outcome.status);
        if (!(presences[0] & 1)) return std::string(page_absent_k);
        return std::string(string_of(tape_string(values, 0)));
    }
};

/** Whether the engine behind @p database serves the pages calls, as every engine
 *  with blobs does. */
bool serves_pages(ustore_database_t database) {
    pages_client_t probe {database};
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) return false;
    probe.arena = std::move(arena);
    return probe.read(mint_key(1, 1, 1)) != std::string_view {"unsupported"};
}

/** Creates a collection named @p name holding pages. */
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

/** What a random text is for: an element's content, or a comment body, which may hold no double
 *  dash. */
enum class text_use_t : bool { content_k, comment_k };

/** Which keys a random pick may land on: any live element, or elements alone, which
 *  parents must be. */
enum class key_filter_t : bool { any_kind_k, elements_only_k };

/** Whether a random node may be a text node, which a fragment's first node may not. */
enum class node_text_t : bool { forbidden_k, allowed_k };

/** Generates random page calls against a model, mostly aimed at elements the stream gave keys to,
 *  sometimes at removed or unknown ones, so the forgiving rules run too. Elements whose keys the
 *  engine mints get placeholder keys in the model, which no call ever names. */
class page_stream_t {
    std::mt19937_64& random_;
    std::uint64_t stream_;
    std::uint32_t given_ = 0, placeholders_ = 0;
    std::vector<element_key_t> given_keys_;

  public:
    element_key_t page;
    model_page_t model;

    page_stream_t(std::mt19937_64& random, std::uint64_t stream) noexcept
        : random_(random), stream_(stream), page(element_key_of(key_from_integer(1000 + stream))) {}

    std::string text(std::size_t longest, text_use_t use = text_use_t::content_k) {
        static constexpr std::string_view alphabet = "abcxyz &<>\"'";
        std::string result(random_() % (longest + 1), ' ');
        for (char& character : result) character = alphabet[random_() % alphabet.size()];
        // A comment body may hold neither `--` nor `-->`, and this alphabet spells neither
        if (use == text_use_t::comment_k)
            for (char& character : result)
                if (character == '&') character = 'q';
        return result;
    }

    element_key_t fresh_given() {
        element_key_t const key = element_key_of(key_from_integer(1'000'000 * (stream_ + 1) + ++given_));
        given_keys_.push_back(key);
        return key;
    }

    element_key_t placeholder() { return mint_key(~std::uint64_t(0), std::uint32_t(stream_), ++placeholders_); }

    /** A key the stream gave out, mostly a live one, sometimes a retired one, the page key or
     *  an unknown one. */
    element_key_t any_key(key_filter_t filter) {
        std::vector<element_key_t> live;
        for (element_key_t const& key : given_keys_)
            if (model.live(key) &&
                (filter == key_filter_t::any_kind_k || model.elements.at(key).kind == element_kind_t::element_k))
                live.push_back(key);
        std::uint64_t const roll = random_() % 10;
        if (roll == 0 && !given_keys_.empty()) return given_keys_[random_() % given_keys_.size()];
        if (roll == 1) return element_key_of(key_from_integer(77));
        if (roll == 2 || live.empty()) return page;
        return live[random_() % live.size()];
    }

    std::string attribute_name() {
        static constexpr std::string_view names[] = {"id", "class", "href", "x"};
        return std::string(names[random_() % 4]);
    }

    /** One random node of an insertion keyed @p key, spelled as XML in @p markup. */
    node_spec_t node(element_key_t const& key, std::string& markup, node_text_t text_nodes = node_text_t::allowed_k) {
        node_spec_t spec;
        spec.key = key;
        std::uint64_t const roll = random_() % 6;
        if (roll == 0) {
            spec.kind = element_kind_t::comment_k, spec.text = text(4, text_use_t::comment_k);
            markup += "<!--" + spec.text + "-->";
            return spec;
        }
        if (roll == 1 && text_nodes == node_text_t::allowed_k) {
            spec.kind = element_kind_t::text_k, spec.text = "t" + text(4);
            markup += model_page_t::escaped(spec.text);
            return spec;
        }
        spec.name = random_() % 2 ? "p" : "row";
        for (std::size_t attribute = random_() % 3; attribute; --attribute) {
            std::string name = attribute_name();
            bool taken = false;
            for (auto const& existing : spec.attributes) taken |= existing.first == name;
            if (!taken) spec.attributes.emplace_back(name, text(3));
        }
        spec.text = text(5);
        model_page_t single;
        single.exists = true, single.root = page, single.elements[page] = {};
        single.graft(page, 0, {spec});
        markup += single.render_xml(key);
        return spec;
    }

    /** A random nested fragment of elements with leading text to import, spelled as XML
     *  in @p markup. */
    std::vector<node_spec_t> import_fragment(std::string& markup) {
        std::vector<node_spec_t> nodes;
        std::size_t const count = random_() % 7;
        std::size_t depth_limit = 1;
        for (std::size_t index = 0; index != count; ++index) {
            node_spec_t spec;
            spec.key = placeholder();
            spec.depth = random_() % depth_limit + 1;
            spec.name = random_() % 2 ? "p" : "s:t";
            if (random_() % 2) spec.attributes.emplace_back(attribute_name(), text(3));
            spec.text = text(4);
            depth_limit = spec.depth + 1;
            nodes.push_back(spec);
        }
        model_page_t staged;
        staged.exists = true, staged.root = page, staged.elements[page] = {};
        staged.graft(page, 0, nodes);
        markup = staged.render_xml(page);
        return nodes;
    }
};

/** Random streams of every page call agree with the reference model of <tt>test/pages/model.hpp</tt>
 *  after every call, read whole, as text and by subtree, and a snapshot keeps reading a page
 *  as it was. */
case_result_t database_pages_random_streams(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);
    bool const snapshots = database.supports_snapshots();

    std::mt19937_64 random(42);
    for (std::uint64_t stream_index = 0; stream_index != 8; ++stream_index) {
        pages_client_t client {database};
        auto [arena, arena_status] = arena_t::make();
        failures += expect(arena_status.succeeded());
        if (failures) return failures;
        client.arena = std::move(arena);
        auto [created, create_status] = create_pages_collection(database, fmt::format("pages-{}", stream_index));
        failures += expect(create_status.succeeded());
        if (failures) return failures;
        client.collection = created;

        page_stream_t stream(random, stream_index);
        ustore_snapshot_t snapshot = nullptr;
        std::string snapshot_rendering;
        for (std::size_t call = 0; call != 120 && !failures; ++call) {
            if (snapshots && call == 60) {
                ustore_outcome_t outcome {};
                ustore_snapshot_init_t init {};
                init.snapshot = &snapshot, init.database = database, init.outcome = &outcome;
                ustore_snapshot_init(&init);
                failures += expect_success(outcome);
                snapshot_rendering = client.read(stream.page);
            }

            std::uint64_t const roll = random() % 100;
            std::vector<model_edit_t> edits;
            std::vector<page_task_t> tasks;
            modification_t verb = modification_t::import_k;
            std::size_t const count = random() % 3 + 1;
            status_t status;
            key_source_t key_source = key_source_t::given_k;
            std::vector<element_key_t> effective;

            if (!stream.model.exists || roll < 4) {
                model_edit_t edit;
                edit.modification = modification_t::import_k, edit.element = stream.page;
                std::string markup;
                if (random() % 4 == 0) {
                    markup = stream.text(6);
                    if (!markup.empty()) {
                        node_spec_t spec;
                        spec.key = stream.placeholder(), spec.kind = element_kind_t::text_k, spec.text = markup;
                        edit.fragment.push_back(spec);
                    }
                    status = client.import(stream.page, markup, ustore_page_encoding_text_k);
                }
                else {
                    edit.fragment = stream.import_fragment(markup);
                    status = client.import(stream.page, markup);
                }
                edits.push_back(edit);
            }
            else if (roll < 6) {
                verb = modification_t::remove_k;
                model_edit_t edit;
                edit.modification = modification_t::remove_k;
                edits.push_back(edit);
                tasks.push_back({});
                status = client.edit(verb, stream.page, tasks);
            }
            else {
                key_source = roll < 40 && random() % 8 == 0 ? key_source_t::minted_k : key_source_t::given_k;
                verb = roll < 40   ? modification_t::insert_k
                       : roll < 50 ? modification_t::remove_k
                       : roll < 62 ? modification_t::move_k
                       : roll < 80 ? modification_t::splice_k
                       : roll < 93 ? modification_t::set_attributes_k
                                   : modification_t::remove_attributes_k;
                for (std::size_t task = 0; task != count; ++task) {
                    model_edit_t edit;
                    page_task_t call_task;
                    edit.modification = verb;
                    switch (verb) {
                    case modification_t::insert_k: {
                        element_key_t const first = key_source == key_source_t::minted_k ? stream.placeholder()
                                                    : random() % 10 == 0 ? stream.any_key(key_filter_t::any_kind_k)
                                                                         : stream.fresh_given();
                        edit.fragment.push_back(stream.node(first, call_task.value));
                        if (random() % 4 == 0)
                            edit.fragment.push_back(
                                stream.node(stream.placeholder(), call_task.value, node_text_t::forbidden_k));
                        call_task.element = first;
                        call_task.parent = edit.parent = random() % 4 == 0
                                                             ? nil_key_k
                                                             : stream.any_key(key_filter_t::elements_only_k);
                        call_task.anchor = edit.anchor = random() % 3 == 0 ? nil_key_k
                                                                           : stream.any_key(key_filter_t::any_kind_k);
                        break;
                    }
                    case modification_t::remove_k:
                        call_task.element = edit.element = stream.any_key(key_filter_t::any_kind_k);
                        break;
                    case modification_t::move_k:
                        call_task.element = edit.element = stream.any_key(key_filter_t::any_kind_k);
                        call_task.parent = edit.parent = random() % 4 == 0
                                                             ? nil_key_k
                                                             : stream.any_key(key_filter_t::elements_only_k);
                        call_task.anchor = edit.anchor = random() % 3 == 0 ? nil_key_k
                                                                           : stream.any_key(key_filter_t::any_kind_k);
                        break;
                    case modification_t::splice_k: {
                        call_task.element = edit.element = stream.any_key(key_filter_t::any_kind_k);
                        std::size_t const length = stream.model.live(edit.element)
                                                       ? stream.model.elements.at(edit.element).text.size()
                                                       : 4;
                        call_task.start = edit.text_start = random() % (length + 3);
                        call_task.length = edit.text_length = random() % 4;
                        call_task.value = edit.value = stream.text(4);
                        break;
                    }
                    case modification_t::set_attributes_k:
                        call_task.element = edit.element = stream.any_key(key_filter_t::any_kind_k);
                        call_task.field = edit.field = stream.attribute_name();
                        call_task.value = edit.value = stream.text(3);
                        break;
                    default:
                        call_task.element = edit.element = stream.any_key(key_filter_t::any_kind_k);
                        call_task.field = edit.field = stream.attribute_name();
                        break;
                    }
                    edits.push_back(std::move(edit));
                    tasks.push_back(std::move(call_task));
                }
                status = client.edit(verb, stream.page, tasks, &effective, key_source);
            }

            failures += expect_status(status, status_code_t::success_k);
            if (status.failed()) break;
            for (std::size_t index = 0; index != effective.size(); ++index)
                if (key_source == key_source_t::given_k) failures += expect(effective[index] == tasks[index].element);
            for (model_edit_t const& edit : edits) stream.model.apply(edit);

            std::string const wanted = stream.model.exists ? stream.model.render_xml(stream.page)
                                                           : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page), wanted);
            std::string const wanted_text = stream.model.exists ? stream.model.render_text(stream.page)
                                                                : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page, {}, ustore_page_encoding_text_k), wanted_text);
            element_key_t const probed = stream.any_key(key_filter_t::any_kind_k);
            std::string const wanted_subtree = stream.model.exists && stream.model.live(probed)
                                                   ? stream.model.render_xml(probed)
                                                   : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page, probed), wanted_subtree);
        }

        // Editing a removed page answers that it is missing
        if (!stream.model.exists && !failures) {
            failures += expect_status(client.edit(modification_t::splice_k, stream.page, {{}}),
                                      status_code_t::not_found_k);
        }

        // A snapshot keeps reading the page as it was when it was taken
        if (snapshot) {
            failures += expect_eq(
                client.read(stream.page, {}, ustore_page_encoding_xml_k, ustore_snapshot_view(snapshot)),
                snapshot_rendering);
            ustore_snapshot_free(snapshot);
        }
        if (failures) {
            fmt::println(stderr, "  pages stream {} diverged", stream_index);
            return failures;
        }
    }
    return failures;
}

/** Edits of one page in a transaction stay invisible until it commits, and a concurrent transaction
 *  that edited the same page either fails to commit, where every edit watches the page it rewrites,
 *  or lands beside the first, where the engine keeps a page as its edits. */
case_result_t database_pages_transactions(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);

    auto [pages, pages_status] = create_pages_collection(database, "pages");
    pages_client_t head {database, pages};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(pages_status.succeeded() && arena_status.succeeded());
    if (failures) return failures;
    head.arena = std::move(arena);
    element_key_t const page = element_key_of(key_from_integer(7));
    failures += expect(head.import(page, "<doc><p>one</p></doc>").succeeded());

    auto [first, first_status] = database.transaction();
    auto [second, second_status] = database.transaction();
    failures += expect(first_status.succeeded() && second_status.succeeded());
    if (failures) return failures;
    pages_client_t first_client {database, pages, first.transaction()};
    pages_client_t second_client {database, pages, second.transaction()};
    auto [first_arena, first_arena_status] = arena_t::make();
    auto [second_arena, second_arena_status] = arena_t::make();
    failures += expect(first_arena_status.succeeded() && second_arena_status.succeeded());
    if (failures) return failures;
    first_client.arena = std::move(first_arena), second_client.arena = std::move(second_arena);

    page_task_t inserted;
    inserted.value = "<p>two</p>";
    std::vector<element_key_t> effective;
    failures += expect(
        first_client.edit(modification_t::insert_k, page, {inserted}, &effective, key_source_t::minted_k).succeeded());
    failures += expect_eq(effective.size(), 1u);
    failures += expect(!effective.empty() && !effective[0].is_nil());
    page_task_t spliced;
    spliced.element = page, spliced.value = "lead";
    failures += expect(second_client.edit(modification_t::splice_k, page, {spliced}).succeeded());

    failures += expect_eq(head.read(page), std::string("<doc><p>one</p></doc>"));
    failures += expect_eq(first_client.read(page, {}, ustore_page_encoding_xml_k,
                                            ustore_transaction_view(first.transaction(), ustore_watch_none_k)),
                          std::string("<p>two</p><doc><p>one</p></doc>"));
    failures += expect(first.commit().has_value());
    failures += expect_eq(head.read(page), std::string("<p>two</p><doc><p>one</p></doc>"));

    // A page rewritten whole conflicts, while a page kept as its edits lands both, folded
    // in commit order
    bool const both_landed = second.commit().has_value();
    failures += expect_eq(head.read(page), std::string(both_landed ? "lead<p>two</p><doc><p>one</p></doc>"
                                                                   : "<p>two</p><doc><p>one</p></doc>"));
    return failures;
}

/** A failing task leaves every page of its call untouched, and a page is one value, its current
 *  shape, which a removal of the page takes away. */
case_result_t database_pages_all_or_nothing(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);

    auto [pages, pages_status] = create_pages_collection(database, "pages");
    pages_client_t client {database, pages};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(pages_status.succeeded() && arena_status.succeeded());
    if (failures) return failures;
    client.arena = std::move(arena);
    auto stored_keys = [&] {
        std::vector<ustore_key_t> keys;
        auto [stream, stream_status] = keys_stream_t::make(database.receiver(), pages, smallest_key_k, 256,
                                                           ustore_modality_pages_k);
        failures += expect(stream_status.succeeded());
        for (; stream_status.succeeded() && !stream.is_end(); ++stream) keys.push_back(stream.key());
        return keys;
    };
    element_key_t const page = element_key_of(key_from_integer(8));
    failures += expect(client.import(page, "<a>x</a>").succeeded());
    std::vector<page_task_t> tasks(3);
    tasks[0].value = "<b/>", tasks[1].value = "<c>", tasks[2].value = "<d/>";
    failures += expect_status(client.edit(modification_t::insert_k, page, tasks, nullptr, key_source_t::minted_k),
                              status_code_t::malformed_k);
    failures += expect_eq(client.read(page), std::string("<a>x</a>"));
    failures += expect(stored_keys() == std::vector<ustore_key_t> {key_from_integer(8)});

    // Removing the page by a Nil element empties it until the next import, and the page key empties
    // only its root
    failures += expect(client.edit(modification_t::remove_k, page, {{}}).succeeded());
    failures += expect_eq(client.read(page), std::string(page_absent_k));
    failures += expect(stored_keys().empty());
    failures += expect(client.import(page, "<a>x</a><b/>").succeeded());
    page_task_t root;
    root.element = page;
    failures += expect(client.edit(modification_t::remove_k, page, {root}).succeeded());
    failures += expect_eq(client.read(page), std::string(""));
    failures += expect(stored_keys() == std::vector<ustore_key_t> {key_from_integer(8)});
    return failures;
}

#pragma endregion Pages Modality

#pragma region Pages Refusals

/** Counts a failure unless @p outcome carries @p wanted, then resets it for the next call. */
std::size_t refused(ustore_outcome_t& outcome, status_code_t wanted) {
    std::size_t const failures = expect_status(status_t {outcome}, wanted);
    outcome = {};
    return failures;
}

/** The pages calls refuse what their header refuses, writing no outputs: missing keys and elements,
 *  unknown encodings, HTML, Markdown and JSON as unsupported, markup that is not well-formed XML,
 *  and edits of a missing page, while a read asking only for presences takes any encoding. */
case_result_t pages_refusals(config_t const& config) {
    std::size_t failures = 0;
    auto [owner, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(owner.clear(ustore_drop_collection_k).succeeded());
    ustore_database_t const database = owner;
    auto [pages, pages_status] = create_collection(database, "pages", ustore_modality_pages_k);
    failures += expect(pages_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const page = key_from_integer(901);
    ustore_key_t const missing = key_from_integer(902);
    // C++ may not form an enum value no enumerator names, even by `std::bit_cast`, so the integer
    // lands as bytes
    using encoding_bits_t = std::underlying_type_t<ustore_page_encoding_t>;
    auto import = [&](std::string_view markup, encoding_bits_t encoding, ustore_key_t const* key) {
        strided_strings<std::string_view> const values {{&markup, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_import_t import {};
        import.database = database, import.outcome = &outcome, import.count = 1, import.collections = {&pages, 0};
        import.keys = {key, 0}, import.values = values.view();
        std::memcpy(reinterpret_cast<std::byte*>(&import.encoding), &encoding, sizeof(encoding));
        ustore_pages_import(&import);
        return status_t {outcome};
    };
    // A probe through an encoding every engine serving pages takes tells apart an
    // engine serving none
    if (import("<a/>", ustore_page_encoding_xml_k, &page).code == status_code_t::unsupported_k)
        return case_result_t::skipped("the engine serves no pages yet", failures);

    failures += expect_status(import("<a/>", ustore_page_encoding_html_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("# a", ustore_page_encoding_markdown_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("{}", ustore_page_encoding_json_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("<a/>", 42, &page), status_code_t::invalid_argument_k);
    failures += expect_status(import("<a/>", ustore_page_encoding_xml_k, nullptr), status_code_t::invalid_argument_k);
    for (std::string_view malformed : {"<a>", "<a>&nbsp;</a>", "<a b=\"<\"/>", "<a>&#0;</a>", "a & b"})
        failures += expect_status(import(malformed, ustore_page_encoding_xml_k, &page), status_code_t::malformed_k);
    failures += expect_status(import("a & b", ustore_page_encoding_text_k, &page), status_code_t::success_k);

    // An insertion refused anywhere leaves its effective keys unwritten
    std::string_view const fragment = "<b/>";
    strided_strings<std::string_view> const fragments {{&fragment, 0}};
    ustore_key_t const* effective = nullptr;
    ustore_outcome_t outcome {};
    ustore_pages_insert_t insert {};
    insert.database = database, insert.outcome = &outcome, insert.count = 1, insert.collections = {&pages, 0};
    insert.keys = {&missing, 0};
    insert.values = fragments.view(), insert.encoding = ustore_page_encoding_xml_k;
    insert.effective_keys = &effective, insert.arena = arena;
    ustore_pages_insert(&insert);
    failures += refused(outcome, status_code_t::not_found_k);
    failures += expect(effective == nullptr);
    insert.keys = {&page, 0};
    insert.values = {};
    ustore_pages_insert(&insert);
    failures += refused(outcome, status_code_t::invalid_argument_k);
    insert.values = fragments.view();
    ustore_pages_insert(&insert);
    failures += expect_success(outcome);
    failures += expect(effective != nullptr);

    ustore_pages_remove_t remove {};
    remove.database = database, remove.outcome = &outcome, remove.count = 1, remove.collections = {&pages, 0};
    remove.keys = {&page, 0};
    ustore_pages_remove(&remove);
    failures += refused(outcome, status_code_t::invalid_argument_k);

    ustore_key_t const element = page;
    ustore_pages_splice_t splice {};
    splice.database = database, splice.outcome = &outcome, splice.count = 1, splice.collections = {&pages, 0};
    splice.keys = {&missing, 0};
    splice.elements = {&element, 0};
    ustore_pages_splice(&splice);
    failures += refused(outcome, status_code_t::not_found_k);

    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};
    ustore_pages_read_t read {};
    read.database = database, read.outcome = &outcome, read.count = 1, read.collections = {&pages, 0};
    read.keys = {&page, 0}, read.presences = &presences, read.values = &values, read.arena = arena;
    read.outputs_encoding = ustore_page_encoding_html_k;
    ustore_pages_read(&read);
    failures += refused(outcome, status_code_t::unsupported_k);
    failures += expect(presences == nullptr && !tape_written(values));
    read.values = nullptr;
    ustore_pages_read(&read);
    failures += expect_success(outcome);
    failures += expect(presences != nullptr && (presences[0] & 1));
    return failures;
}

#pragma endregion Pages Refusals

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    config_t const config {options.engine, maybe_directory->path()};

    report(totals, "database.pages_random_streams", database_pages_random_streams, config);
    report(totals, "database.pages_transactions", database_pages_transactions, config);
    report(totals, "database.pages_all_or_nothing", database_pages_all_or_nothing, config);
    report(totals, "pages.refusals", pages_refusals, config);
    return suite_exit_code(totals);
}
