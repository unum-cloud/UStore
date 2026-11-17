/**
 *  @file test/pages/page_fold.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of `ustore/pages/page_edits.hpp` and `ustore/pages/page_fold.hpp` against a naive reference
 *      model: edit codecs, random edit streams, composition without a base, and the convergence of concurrent
 *      insertions.
 *
 *  The model of `test/pages/model.hpp` keeps a page as a map from keys to elements with vectors of children, and
 *  applies every edit by the forgiving rules `page_fold.hpp` names, with nothing shared with the templates beyond the
 *  key and kind types.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm>   // `std::find`, `std::next_permutation`
#include <memory>      // `std::unique_ptr`, `std::align`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include "ustore/pages/element_tree.hpp" // `element_tree`, `mint_key`
#include "ustore/pages/page_edits.hpp"   // `edit_t`, `encode_edits`
#include "ustore/pages/page_fold.hpp"    // `page_fold_t`, `page_state`

#include "harness.hpp" // `expect_true`, `expect_equal`, `expect_status`, `run_test_case`
#include "model.hpp"   // `model_page_t`, `model_edit_t`, `node_spec_t`, `hex`

namespace unum::ustore::test {

using namespace unum::ustore::test::pages;

using namespace unum::ustore::pages;
using namespace unum::ustore::test::pages;

#pragma region Arenas

/** A bump arena handing out blocks that live until it is destroyed, as engines' arenas do. */
struct test_arena_t {
    std::vector<std::unique_ptr<std::byte[]>> blocks;
};

template <typename value_type_>
struct test_allocator {
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    test_arena_t* arena = nullptr;

    test_allocator(test_arena_t* owner = nullptr) noexcept : arena(owner) {}
    template <typename other_type_>
    test_allocator(test_allocator<other_type_> const& other) noexcept : arena(other.arena) {}

    value_type_* allocate(std::size_t count) noexcept {
        std::size_t const bytes = count * sizeof(value_type_) + alignof(std::max_align_t);
        auto block = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[bytes]);
        if (!block) return nullptr;
        void* pointer = block.get();
        std::size_t space = bytes;
        pointer = std::align(alignof(value_type_), count * sizeof(value_type_), pointer, space);
        arena->blocks.push_back(std::move(block));
        return static_cast<value_type_*>(pointer);
    }
    void deallocate(value_type_*, std::size_t) noexcept {}

    template <typename other_type_>
    bool operator==(test_allocator<other_type_> const& other) const noexcept {
        return arena == other.arena;
    }
};

using bytes_allocator_t = test_allocator<byte_t>;
using tree_t = element_tree<bytes_allocator_t>;
using bytes_t = std::vector<byte_t>;

#pragma endregion Arenas

#pragma region Reference Model

/** Every live element of @p tree in preorder, keys included, placements left out as the model keeps none. */
std::string dump_tree(tree_t const& tree) {
    std::string result;
    element_index_t const root = tree.root();
    for (element_index_t at = root; at != no_element_k; at = tree.next_in_preorder(at, root)) {
        std::size_t depth = 0;
        for (element_index_t up = at; up != root; up = tree[up].parent) ++depth;
        result += std::string(depth * 2, ' ') + hex(tree[at].key) + " " + std::to_string(int(tree[at].kind)) + " <" +
                  std::string(tree.name(at)) + "> [" + std::string(tree.text(at)) + "]";
        for (element_index_t attribute = tree[at].first_attribute; attribute != no_element_k;
             attribute = tree.attribute(attribute).next)
            result += " " + std::string(tree.string(tree.attribute(attribute).name)) + "=" +
                      std::string(tree.string(tree.attribute(attribute).value));
        result += "\n";
    }
    return result;
}

/** The dump of a dense page, or "absent" for a missing one. */
std::string dump_page(expected<std::span<byte_t const>> const& page, test_arena_t& arena) {
    if (page.code() == status_code_t::not_found_k) return "absent\n";
    if (!page) return "failed " + std::to_string(int(page.code())) + "\n";
    tree_t tree {bytes_allocator_t(&arena)};
    if (tree.decode(*page).failed()) return "malformed\n";
    return dump_tree(tree);
}

#pragma endregion Reference Model

#pragma region Random Streams

/** The dense form of @p fragment under a root keyed @p root_key, every element placed at @p placement. */
bytes_t encode_fragment(element_key_t const& root_key, std::vector<node_spec_t> const& fragment,
                        placement_t placement) {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    tree_t tree(allocator);
    if (tree.reset(root_key, placement).failed()) return {};
    std::vector<element_index_t> open {tree.root()};
    for (node_spec_t const& spec : fragment) {
        open.resize(spec.depth);
        auto added = tree.append(open.back(), spec.key, spec.kind, spec.name, spec.text, placement);
        if (!added) return {};
        for (auto const& [name, value] : spec.attributes)
            if (tree.set_attribute(*added, name, value).failed()) return {};
        open.push_back(*added);
    }
    auto blob = tree.encode(allocator);
    if (!blob) return {};
    return bytes_t(blob->begin(), blob->end());
}

/** One random commit of a stream: the model's edits, and their serialized chain. */
struct commit_t {
    std::uint64_t stamp = 0;
    std::vector<model_edit_t> edits;
    bytes_t chain;
};

/** Which keys a random pick may land on: any live element, or elements alone, which
 *  parents must be. */
enum class key_filter_t : bool { any_kind_k, elements_only_k };

/** Generates commits against a model that tracks what is live, aiming mostly at live elements and sometimes at
 *  removed or unknown ones, so the forgiving rules run too. */
class stream_generator_t {
    split_mix_generator_t& random_;
    model_page_t model_;
    std::vector<element_key_t> retired_;
    std::uint64_t stamp_ = 0;
    element_key_t page_ = mint_key(0, 0, 1);

    std::string text(std::size_t longest) {
        static constexpr std::string_view alphabet = "abcxyz &<";
        std::string result(random_.below(longest + 1), ' ');
        for (char& character : result) character = alphabet[random_.below(alphabet.size())];
        return result;
    }
    std::string attribute_name() {
        static constexpr std::string_view names[] = {"id", "class", "href", "x"};
        return std::string(names[random_.below(4)]);
    }
    element_key_t any_key(key_filter_t filter) {
        std::vector<element_key_t> candidates;
        for (auto const& [key, element] : model_.elements)
            if (filter == key_filter_t::any_kind_k || element.kind == element_kind_t::element_k)
                candidates.push_back(key);
        std::uint64_t const roll = random_.below(10);
        if (roll == 0 && !retired_.empty()) return retired_[random_.below(retired_.size())];
        if (roll == 1) return mint_key(999, 0, std::uint32_t(random_.below(4)));
        if (candidates.empty()) return nil_key_k;
        return candidates[random_.below(candidates.size())];
    }
    std::vector<node_spec_t> fragment(std::uint64_t stamp, std::uint32_t sequence, std::size_t largest) {
        std::vector<node_spec_t> nodes;
        std::size_t const count = random_.below(largest) + 1;
        std::size_t depth_limit = 1;
        for (std::size_t index = 0; index != count; ++index) {
            node_spec_t spec;
            spec.key = mint_key(stamp, sequence, std::uint32_t(index + 1));
            if (index == 0 && !retired_.empty() && random_.below(8) == 0)
                spec.key = retired_[random_.below(retired_.size())];
            spec.depth = random_.below(depth_limit) + 1;
            spec.kind = element_kind_t(random_.below(4) == 0 ? random_.below(3) : 0);
            if (spec.kind == element_kind_t::element_k) {
                spec.name = random_.below(2) ? "p" : "row";
                for (std::size_t attribute = random_.below(3); attribute; --attribute) {
                    std::string name = attribute_name();
                    bool taken = false;
                    for (auto const& existing : spec.attributes) taken |= existing.first == name;
                    if (!taken) spec.attributes.emplace_back(name, text(3));
                }
            }
            spec.text = text(5);
            depth_limit = spec.kind == element_kind_t::element_k ? spec.depth + 1 : spec.depth;
            nodes.push_back(spec);
        }
        return nodes;
    }

  public:
    explicit stream_generator_t(split_mix_generator_t& random) noexcept : random_(random) {}

    model_page_t const& model() const noexcept { return model_; }

    /** The next commit, with one to three tasks, applied to the generator's model. */
    commit_t next() {
        commit_t commit;
        commit.stamp = ++stamp_;
        std::size_t const tasks = random_.below(3) + 1;
        std::vector<edit_t> edits;
        std::vector<bytes_t> fragments;
        // The serialized edits view the strings of these, so neither may reallocate
        fragments.reserve(tasks);
        commit.edits.reserve(tasks);
        for (std::uint32_t sequence = 0; sequence != tasks; ++sequence) {
            model_edit_t edit;
            edit.stamp = commit.stamp, edit.sequence = sequence;
            std::uint64_t const roll = random_.below(100);
            if (!model_.exists || roll < 3) {
                edit.modification = modification_t::import_k;
                edit.element = page_;
                edit.fragment = fragment(commit.stamp, sequence, 6);
            }
            else if (roll < 5) edit.modification = modification_t::remove_k;
            else if (roll < 35) {
                edit.modification = modification_t::insert_k;
                edit.fragment = fragment(commit.stamp, sequence, 3);
                edit.element = edit.fragment.front().key;
                edit.parent = random_.below(4) == 0 ? nil_key_k : any_key(key_filter_t::elements_only_k);
                edit.anchor = random_.below(3) == 0 ? nil_key_k : any_key(key_filter_t::any_kind_k);
            }
            else if (roll < 45) {
                edit.modification = modification_t::remove_k;
                edit.element = any_key(key_filter_t::any_kind_k);
                if (edit.element.is_nil()) edit.element = page_;
            }
            else if (roll < 57) {
                edit.modification = modification_t::move_k;
                edit.element = any_key(key_filter_t::any_kind_k);
                edit.parent = random_.below(4) == 0 ? nil_key_k : any_key(key_filter_t::elements_only_k);
                edit.anchor = random_.below(3) == 0 ? nil_key_k : any_key(key_filter_t::any_kind_k);
            }
            else if (roll < 80) {
                edit.modification = modification_t::splice_k;
                edit.element = any_key(key_filter_t::any_kind_k);
                std::size_t const length = model_.live(edit.element) ? model_.elements.at(edit.element).text.size() : 4;
                edit.text_start = random_.below(3) == 0 ? 0 : random_.below(length + 3);
                edit.text_length = random_.below(4);
                edit.value = text(4);
                // Follow-up splices inside the run a splice at zero inserted, which composition merges
                if (!commit.edits.empty()) {
                    model_edit_t const& previous = commit.edits.back();
                    if (previous.modification == modification_t::splice_k && previous.text_start == 0 &&
                        random_.below(2) && !previous.value.empty()) {
                        edit.element = previous.element;
                        edit.text_start = random_.below(previous.value.size() + 1);
                        edit.text_length = random_.below(previous.value.size() - edit.text_start + 1);
                    }
                }
            }
            else if (roll < 93) {
                edit.modification = modification_t::set_attributes_k;
                edit.element = any_key(key_filter_t::any_kind_k);
                edit.field = attribute_name();
                edit.value = text(3);
            }
            else {
                edit.modification = modification_t::remove_attributes_k;
                edit.element = any_key(key_filter_t::any_kind_k);
                edit.field = attribute_name();
            }

            std::vector<element_key_t> const before = [&] {
                std::vector<element_key_t> keys;
                for (auto const& [key, element] : model_.elements) keys.push_back(key);
                return keys;
            }();
            model_.apply(edit);
            for (element_key_t const& key : before)
                if (!model_.live(key) && !(key == page_)) retired_.push_back(key);

            placement_t const placement {edit.stamp, edit.sequence};
            edit_t serialized;
            serialized.modification = edit.modification, serialized.placement = placement;
            serialized.element = edit.element, serialized.parent = edit.parent, serialized.anchor = edit.anchor;
            serialized.text_start = edit.text_start, serialized.text_length = edit.text_length;
            commit.edits.push_back(std::move(edit));
            model_edit_t const& kept = commit.edits.back();
            serialized.field = kept.field;
            if (kept.modification == modification_t::import_k || kept.modification == modification_t::insert_k) {
                fragments.push_back(encode_fragment(kept.modification == modification_t::import_k ? page_ : nil_key_k,
                                                    kept.fragment, placement));
                serialized.value = fragments.back();
            }
            else serialized.value = as_bytes(kept.value);
            edits.push_back(serialized);
        }
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        auto chain = encode_edits(std::span<edit_t const>(edits), allocator);
        if (chain) commit.chain.assign(chain->begin(), chain->end());
        return commit;
    }
};

/** Change deltas viewing the chains of @p commits from @p first to @p last. */
std::vector<delta_view_t> deltas_of(std::vector<commit_t> const& commits, std::size_t first, std::size_t last) {
    std::vector<delta_view_t> deltas;
    for (std::size_t index = first; index != last; ++index)
        deltas.push_back(delta_view_t {delta_kind_t::change_k, commits[index].chain});
    return deltas;
}

#pragma endregion Random Streams

#pragma region Cases

/** Every kind of edit survives its serialized form, a chain reads back in order, and truncations are refused. */
void test_edit_codec() {
    split_mix_generator_t random {case_stream_key()};
    stream_generator_t generator(random);
    for (std::size_t round = 0; round != 300; ++round) {
        commit_t const commit = generator.next();
        std::size_t index = 0;
        std::vector<std::size_t> boundaries {0};
        status_t const status = for_each_edit(commit.chain, [&](edit_t const& edit) noexcept -> status_t {
            boundaries.push_back(boundaries.back() + encoded_size(edit));
            model_edit_t const& wanted = commit.edits[index++];
            expect_true(edit.modification == wanted.modification);
            expect_true(edit.placement == placement_t {wanted.stamp, wanted.sequence});
            expect_true(edit.element == wanted.element);
            if (carries_position(edit.modification))
                expect_true(edit.parent == wanted.parent && edit.anchor == wanted.anchor);
            if (edit.modification == modification_t::splice_k)
                expect_true(edit.text_start == wanted.text_start && edit.text_length == wanted.text_length);
            if (carries_field(edit.modification)) expect_true(edit.field == wanted.field);
            if (edit.modification == modification_t::splice_k || edit.modification == modification_t::set_attributes_k)
                expect_true(string_of(edit.value) == wanted.value);
            if (edit.modification == modification_t::insert_k || edit.modification == modification_t::import_k) {
                std::size_t minted = 0;
                expect_true(for_each_minted_key(edit, [&](element_key_t const& key) {
                                expect_true(minted < wanted.fragment.size() && key == wanted.fragment[minted].key);
                                ++minted;
                            }).succeeded());
                expect_true(minted == wanted.fragment.size());
            }
            return {};
        });
        expect_true(status.succeeded() && index == commit.edits.size());
        expect_true(boundaries.back() == commit.chain.size());
        for (std::size_t prefix_length = 1; prefix_length < commit.chain.size();
             prefix_length += 1 + commit.chain.size() / 7) {
            bool const whole = std::find(boundaries.begin(), boundaries.end(), prefix_length) != boundaries.end();
            expect_true(for_each_edit({commit.chain.data(), prefix_length}, [](edit_t const&) noexcept -> status_t {
                            return {};
                        }).code == (whole ? status_code_t::success_k : status_code_t::malformed_k));
        }
    }
    bytes_t unknown(edit_header_bytes_k, byte_t(0));
    unknown[0] = byte_t(7);
    expect_true(for_each_edit(unknown, [](edit_t const&) noexcept -> status_t { return {}; }).code ==
                status_code_t::malformed_k);
}

/** Random streams folded edit by edit, commit by commit, and from random intermediate bases all match the model. */
void test_random_streams() {
    for (std::uint64_t seed = 1; seed != 41; ++seed) {
        split_mix_generator_t random {case_stream_key(seed)};
        stream_generator_t generator(random);
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        page_state<bytes_allocator_t> state(allocator);
        std::vector<commit_t> commits;
        std::vector<std::string> snapshots;
        for (std::size_t round = 0; round != 120; ++round) {
            commits.push_back(generator.next());
            expect_true(state.apply(delta_view_t {delta_kind_t::change_k, commits.back().chain}).succeeded());
            snapshots.push_back(generator.model().dump());
            std::string const folded = state.exists ? dump_tree(state.tree) : "absent\n";
            if (!expect_equal(folded, snapshots.back())) return;
        }

        page_fold_t const fold;
        test_arena_t scratch;
        bytes_allocator_t scratch_allocator(&scratch);
        auto const chain = deltas_of(commits, 0, commits.size());
        expect_equal(dump_page(fold.apply({}, chain, scratch_allocator), scratch), snapshots.back());

        for (std::size_t trial = 0; trial != 8; ++trial) {
            std::size_t const split = random.below(commits.size());
            auto const head = deltas_of(commits, 0, split + 1);
            auto base = fold.apply({}, head, scratch_allocator);
            std::span<byte_t const> base_bytes;
            if (base) base_bytes = *base;
            else expect_status(base.code(), status_code_t::not_found_k);
            auto const tail = deltas_of(commits, split + 1, commits.size());
            expect_equal(dump_page(fold.apply(base_bytes, tail, scratch_allocator), scratch), snapshots.back());
        }
    }
}

/** Composing any run of a stream without its base folds to the same page as the run itself, on the base it follows,
 *  and never grows the chain. */
void test_compose_is_sound() {
    std::size_t raw_edits = 0, composed_edits = 0;
    for (std::uint64_t seed = 100; seed != 140; ++seed) {
        split_mix_generator_t random {case_stream_key(seed)};
        stream_generator_t generator(random);
        std::vector<commit_t> commits;
        for (std::size_t round = 0; round != 80; ++round) commits.push_back(generator.next());
        page_fold_t const fold;
        for (std::size_t trial = 0; trial != 30; ++trial) {
            test_arena_t arena;
            bytes_allocator_t allocator(&arena);
            std::size_t const first = random.below(commits.size());
            std::size_t const last = first + 1 + random.below(fu::min_of<std::size_t>(12, commits.size() - first));
            auto const head = deltas_of(commits, 0, first);
            auto base = fold.apply({}, head, allocator);
            std::span<byte_t const> base_bytes;
            if (base) base_bytes = *base;
            auto const run = deltas_of(commits, first, last);
            auto composed = fold.compose(run, allocator);
            expect_true(bool(composed));
            if (!composed) continue;
            delta_view_t const single {delta_kind_t::change_k, *composed};
            auto const sequential = fold.apply(base_bytes, run, allocator);
            auto const folded = fold.apply(base_bytes, std::span<delta_view_t const>(&single, 1), allocator);
            expect_true(sequential.code() == folded.code());
            if (sequential && folded)
                expect_true(sequential->size() == folded->size() &&
                            std::memcmp(sequential->data(), folded->data(), folded->size()) == 0);
            expect_equal(dump_page(folded, arena), dump_page(sequential, arena));
            for (delta_view_t const& delta : run)
                expect_true(for_each_edit(delta.bytes, [&](edit_t const&) noexcept -> status_t {
                                ++raw_edits;
                                return {};
                            }).succeeded());
            std::size_t count = 0;
            expect_true(for_each_edit(*composed, [&](edit_t const&) noexcept -> status_t {
                            ++count;
                            return {};
                        }).succeeded());
            std::size_t raw_count = 0;
            for (delta_view_t const& delta : run)
                (void)for_each_edit(delta.bytes, [&](edit_t const&) noexcept -> status_t {
                    ++raw_count;
                    return {};
                });
            expect_true(count <= raw_count);
            composed_edits += count;
        }
    }
    fmt::println("    composed {} edits into {}", raw_edits, composed_edits);
}

/** The compositions this header promises, one by one. */
void test_compose_shapes() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    element_key_t const page = mint_key(1, 0, 0), paragraph = mint_key(1, 0, 1);
    bytes_t const tree = encode_fragment(
        page, {node_spec_t {paragraph, element_kind_t::element_k, "p", "hello", {}, 1}}, {1, 0});
    page_fold_t const fold;
    auto count_of = [&](std::span<byte_t const> chain) {
        std::size_t count = 0;
        (void)for_each_edit(chain, [&](edit_t const&) noexcept -> status_t {
            ++count;
            return {};
        });
        return count;
    };
    auto compose = [&](std::vector<edit_t> const& edits) {
        auto chain = encode_edits(std::span<edit_t const>(edits), allocator);
        delta_view_t const delta {delta_kind_t::change_k, *chain};
        auto composed = fold.compose(std::span<delta_view_t const>(&delta, 1), allocator);
        return composed ? *composed : std::span<byte_t const> {};
    };

    // Setting one attribute three times keeps one edit with the newest value
    auto attributes = compose(
        {set_attribute_edit({2, 0}, paragraph, "id", "a"), set_attribute_edit({3, 0}, paragraph, "x", "1"),
         set_attribute_edit({4, 0}, paragraph, "id", "b"), set_attribute_edit({5, 0}, paragraph, "id", "c")});
    expect_true(count_of(attributes) == 2);
    std::vector<std::string> values;
    (void)for_each_edit(attributes, [&](edit_t const& edit) noexcept -> status_t {
        values.push_back(std::string(edit.field) + "=" + std::string(string_of(edit.value)));
        return {};
    });
    expect_true(values == std::vector<std::string> {"id=c", "x=1"});

    // A removal of an attribute drops every older edit of it, but a set after a removal keeps both
    expect_true(count_of(compose({set_attribute_edit({2, 0}, paragraph, "id", "a"),
                                  remove_attribute_edit({3, 0}, paragraph, "id")})) == 1);
    expect_true(count_of(compose({remove_attribute_edit({2, 0}, paragraph, "id"),
                                  set_attribute_edit({3, 0}, paragraph, "id", "a")})) == 2);

    // Splices inside the run a splice at zero inserted merge into it
    auto typed = compose({splice_edit({2, 0}, paragraph, 0, 2, "Hey"), splice_edit({3, 0}, paragraph, 3, 0, "!"),
                          splice_edit({4, 0}, paragraph, 1, 1, "a")});
    expect_true(count_of(typed) == 1);
    (void)for_each_edit(typed, [&](edit_t const& edit) noexcept -> status_t {
        expect_true(string_of(edit.value) == "Hay!" && edit.text_start == 0 && edit.text_length == 2);
        expect_true(edit.placement == placement_t {4, 0});
        return {};
    });
    // Splices elsewhere never merge, as the text's length decides where they land
    expect_true(
        count_of(compose({splice_edit({2, 0}, paragraph, 1, 0, "a"), splice_edit({3, 0}, paragraph, 2, 0, "b")})) == 2);

    // A removal drops the older content edits of its element
    expect_true(count_of(compose({splice_edit({2, 0}, paragraph, 1, 0, "a"),
                                  set_attribute_edit({3, 0}, paragraph, "id", "a"), remove_edit({4, 0}, paragraph)})) ==
                1);

    // An import folds every newer edit into itself, and a page removal stands alone
    auto imported = compose({splice_edit({1, 0}, paragraph, 0, 0, "lost"), import_edit({2, 0}, page, tree),
                             splice_edit({3, 0}, paragraph, 5, 0, "!"),
                             set_attribute_edit({4, 0}, paragraph, "id", "q")});
    expect_true(count_of(imported) == 1);
    delta_view_t const imported_delta {delta_kind_t::change_k, imported};
    expect_true(fold.closes(imported_delta));
    auto page_after = fold.apply({}, std::span<delta_view_t const>(&imported_delta, 1), allocator);
    expect_equal(dump_page(page_after, arena), hex(page) + " 0 <> []\n  " + hex(paragraph) + " 0 <p> [hello!] id=q\n");
    auto removed = compose(
        {import_edit({2, 0}, page, tree), remove_edit({3, 0}, nil_key_k), splice_edit({4, 0}, paragraph, 0, 0, "x")});
    expect_true(count_of(removed) == 1);
    delta_view_t const removed_delta {delta_kind_t::change_k, removed};
    expect_status(fold.apply(tree, std::span<delta_view_t const>(&removed_delta, 1), allocator).code(),
                  status_code_t::not_found_k);

    // Whole-page records compose too: a write becomes an import, and a removal a page removal
    delta_view_t const records[] = {{delta_kind_t::write_k, tree},
                                    {delta_kind_t::change_k, compose({splice_edit({5, 0}, paragraph, 0, 1, "j")})}};
    auto written = fold.compose(records, allocator);
    expect_true(bool(written) && count_of(*written) == 1);
    delta_view_t const removal {delta_kind_t::remove_k, {}};
    expect_true(fold.closes(removal) && fold.closes(records[0]) && !fold.closes(records[1]));
    expect_true(fold.apply({}, {}, allocator).code() == status_code_t::not_found_k);
    expect_true(fold.compose({}, allocator).code() == status_code_t::invalid_argument_k);
}

/** Concurrent insertions after one anchor order newest first, whichever order they apply in. */
void test_concurrent_inserts_converge() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    element_key_t const page = mint_key(1, 0, 0), anchor = mint_key(1, 0, 1), tail = mint_key(1, 0, 2);
    bytes_t const base = encode_fragment(page,
                                         {node_spec_t {anchor, element_kind_t::element_k, "a", "", {}, 1},
                                          node_spec_t {tail, element_kind_t::element_k, "z", "", {}, 1}},
                                         {1, 0});
    std::vector<bytes_t> fragments, chains;
    std::vector<element_key_t> keys;
    for (std::uint32_t author = 0; author != 4; ++author) {
        element_key_t const key = mint_key(10 + author, 0, 1);
        keys.push_back(key);
        fragments.push_back(encode_fragment(nil_key_k, {node_spec_t {key, element_kind_t::element_k, "c", "", {}, 1}},
                                            {10 + author, 0}));
    }
    for (std::uint32_t author = 0; author != 4; ++author) {
        edit_t const edit = insert_edit({10 + author, 0}, keys[author], nil_key_k, anchor, fragments[author]);
        auto chain = encode_edits(std::span<edit_t const>(&edit, 1), allocator);
        chains.emplace_back(chain->begin(), chain->end());
    }
    std::vector<std::size_t> order {0, 1, 2, 3};
    std::string wanted;
    page_fold_t const fold;
    do {
        std::vector<delta_view_t> deltas;
        for (std::size_t author : order) deltas.push_back(delta_view_t {delta_kind_t::change_k, chains[author]});
        std::string const folded = dump_page(fold.apply(base, deltas, allocator), arena);
        if (wanted.empty()) wanted = folded;
        expect_equal(folded, wanted);
    } while (std::next_permutation(order.begin(), order.end()));
    std::string expected_order = hex(page) + " 0 <> []\n  " + hex(anchor) + " 0 <a> []\n";
    for (std::size_t author = 4; author-- > 0;) expected_order += "  " + hex(keys[author]) + " 0 <c> []\n";
    expected_order += "  " + hex(tail) + " 0 <z> []\n";
    expect_equal(wanted, expected_order);
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    run_test_case("test_edit_codec", test_edit_codec);
    run_test_case("test_random_streams", test_random_streams);
    run_test_case("test_compose_is_sound", test_compose_is_sound);
    run_test_case("test_compose_shapes", test_compose_shapes);
    run_test_case("test_concurrent_inserts_converge", test_concurrent_inserts_converge);
    return finish_test_run();
}
