/**
 *  @file include/ustore/pages/page_fold.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The pages fold: a dense base and chains of edits, applied in commit order into the page's current shape,
 *      and composed without a base where that is sound.
 *
 *  Edits apply forgivingly, because concurrent transactions that watch nothing may commit edits whose targets another
 *  one removed, and a fold that refused them would leave the page unreadable. An edit of a missing element, under a
 *  parent that is missing or no element, or moving an element into its own subtree does nothing. An anchor that is Nil,
 *  missing or under another parent lands the element at the front. An insertion whose fragment holds a live key does
 *  nothing, a splice clamps its start and length to the text, and attributes are only set on and removed from
 *  elements. Removing the root empties it, and removing Nil removes the page until the next import.
 *
 *  Composition without a base keeps the chain, minus what is provably dead or mergeable for every base. The newest
 *  import or page removal drops everything older, and an import folds its followers into itself. A removal of an
 *  element drops the older splices and attribute edits of it. A newer removal of an attribute drops its older edits,
 *  and a newer value merges into an older set, keeping the attribute's position. A splice staying inside the bytes an
 *  older splice at byte zero inserted merges into it. Other splices never merge, since clamping makes where they land
 *  depend on the text's length, which only a base knows.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span> // `std::span`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/pages/element_tree.hpp" // `element_tree`, `page_view_t`
#include "ustore/pages/page_edits.hpp"   // `edit_t`, `for_each_edit`, `encode_edits`
#include "ustore/utilities/folds.hpp"    // `delta_kind_t`, `delta_view_t`, `fold_concept`

namespace unum::ustore::pages {

// A write carries a dense tree, a removal nothing, and a change a chain of edits
using utilities::delta_kind_t;
using utilities::delta_view_t;

/** A page being folded: its tree and whether it exists. */
template <typename allocator_type_>
class page_state {
  public:
    using allocator_t = allocator_type_;

    /** The tree, meaningful while @c exists. */
    element_tree<allocator_t> tree;

    /** Whether the page exists, false before the first import and after a removal of the page. */
    bool exists = false;

    explicit page_state(allocator_t allocator = {}) noexcept : tree(allocator) {}

    /** Starts from the dense @p base, where empty bytes stand for no page. */
    status_t load(std::span<byte_t const> base) noexcept {
        exists = !base.empty();
        if (!exists) return {};
        return tree.decode(base);
    }

    /** Applies @p edit with the forgiving rules this header names. */
    status_t apply(edit_t const& edit) noexcept {
        if (edit.modification == modification_t::import_k) {
            if (status_t status = tree.decode(edit.value); status.failed()) return status;
            exists = true;
            return {};
        }
        if (edit.modification == modification_t::remove_k && edit.element.is_nil()) {
            exists = false;
            return {};
        }
        if (!exists) return {};
        element_index_t const element = tree.find(edit.element);
        switch (edit.modification) {
        case modification_t::remove_k:
            if (element != no_element_k) tree.remove(element);
            return {};
        case modification_t::insert_k: {
            element_index_t const parent = edit.parent.is_nil() ? tree.root() : tree.find(edit.parent);
            if (parent == no_element_k || tree[parent].kind != element_kind_t::element_k) return {};
            element_tree<allocator_t> fragment(tree.allocator());
            if (status_t status = fragment.decode(edit.value); status.failed()) return status;
            if (tree.collides(fragment)) return {};
            return tree.graft(fragment, parent, tree.landing(parent, edit.anchor, edit.placement), edit.placement);
        }
        case modification_t::move_k: {
            if (element == no_element_k || element == tree.root() || edit.anchor == edit.element) return {};
            element_index_t const parent = edit.parent.is_nil() ? tree.root() : tree.find(edit.parent);
            if (parent == no_element_k || tree[parent].kind != element_kind_t::element_k ||
                tree.contains(element, parent))
                return {};
            tree.unlink(element);
            tree.relink(element, parent, tree.landing(parent, edit.anchor, edit.placement), edit.placement);
            return {};
        }
        case modification_t::splice_k:
            if (element == no_element_k) return {};
            return tree.splice(element, edit.text_start, edit.text_length, as_chars(edit.value));
        case modification_t::set_attributes_k:
            if (element == no_element_k || tree[element].kind != element_kind_t::element_k) return {};
            return tree.set_attribute(element, edit.field, as_chars(edit.value));
        case modification_t::remove_attributes_k:
            if (element == no_element_k || tree[element].kind != element_kind_t::element_k) return {};
            tree.remove_attribute(element, edit.field);
            return {};
        default: return {status_code_t::malformed_k, "An edit has an unknown modification"};
        }
    }

    /** Applies @p delta: loads a write, drops the page for a removal, and applies every edit of a change in order. */
    status_t apply(delta_view_t const& delta) noexcept {
        switch (delta.kind) {
        case delta_kind_t::write_k: return load(delta.bytes);
        case delta_kind_t::remove_k: exists = false; return {};
        case delta_kind_t::change_k:
            return for_each_edit(delta.bytes, [&](edit_t const& edit) noexcept -> status_t { return apply(edit); });
        }
        return {status_code_t::malformed_k, "A delta has an unknown kind"};
    }
};

/** The fold of pages, fitting the fold concept of <tt>ustore/utilities/folds.hpp</tt>: apply, compose and closes. */
struct page_fold_t {

    /**
     *  @brief The dense page that @p base becomes once every edit of @p deltas applies, oldest first.
     *  @return The page in a block of @p allocator, @p base itself without deltas, or @c status_code_t::not_found_k
     *      when no page exists afterwards.
     */
    template <typename allocator_type_>
    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            allocator_type_& allocator) const noexcept {
        if (deltas.empty()) {
            if (base.empty()) return status_t {status_code_t::not_found_k, "The page does not exist"};
            return base;
        }
        page_state<allocator_type_> state(allocator);
        if (status_t status = state.load(base); status.failed()) return status;
        for (delta_view_t const& delta : deltas)
            if (status_t status = state.apply(delta); status.failed()) return status;
        if (!state.exists) return status_t {status_code_t::not_found_k, "The page does not exist"};
        return state.tree.encode(allocator);
    }

    /** Whether @p delta makes every older delta irrelevant: a write, a removal, or a chain holding an import or a
     *  page removal. */
    bool closes(delta_view_t const& delta) const noexcept {
        if (delta.kind != delta_kind_t::change_k) return true;
        bool closing = false;
        status_t const status = for_each_edit(delta.bytes, [&](edit_t const& edit) noexcept -> status_t {
            closing = closing || pages::closes(edit);
            return {};
        });
        return status.succeeded() && closing;
    }

    /**
     *  @brief One chain of edits equivalent to @p deltas on every base, as compaction stores it.
     *  @return The chain in a block of @p allocator, which the caller frees with its size.
     */
    template <typename allocator_type_>
    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              allocator_type_& allocator) const noexcept {
        using edits_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<edit_t>;
        using flags_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint8_t>;
        using blocks_allocator_t =
            typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::span<byte_t>>;
        st::basic_vector<edit_t, edits_allocator_t> edits {edits_allocator_t(allocator)};
        st::basic_vector<std::uint8_t, flags_allocator_t> keep {flags_allocator_t(allocator)};
        st::basic_vector<std::span<byte_t>, blocks_allocator_t> blocks {blocks_allocator_t(allocator)};
        auto release = [&]() noexcept {
            for (std::span<byte_t> block : blocks) allocator.deallocate(block.data(), block.size());
        };

        for (delta_view_t const& delta : deltas) {
            status_t status;
            if (delta.kind == delta_kind_t::write_k) {
                auto view = page_view_t::make(delta.bytes);
                if (!view) return view.status();
                status = growth_status(edits.push_back(import_edit({}, view->root_key(), delta.bytes)));
            }
            else if (delta.kind == delta_kind_t::remove_k)
                status = growth_status(edits.push_back(remove_edit({}, nil_key_k)));
            else
                status = for_each_edit(delta.bytes, [&](edit_t const& edit) noexcept -> status_t {
                    return growth_status(edits.push_back(edit_t(edit)));
                });
            if (status.failed()) return status;
        }
        if (edits.empty()) return status_t {status_code_t::invalid_argument_k, "No deltas to compose"};

        std::size_t closing = edits.size();
        for (std::size_t index = edits.size(); index-- && closing == edits.size();)
            if (pages::closes(edits[index])) closing = index;

        if (closing != edits.size() && edits[closing].modification == modification_t::remove_k)
            return encode_edits(std::span<edit_t const>(&edits[closing], 1), allocator);

        if (closing != edits.size()) {
            page_state<allocator_type_> state(allocator);
            for (std::size_t index = closing; index != edits.size(); ++index)
                if (status_t status = state.apply(edits[index]); status.failed()) return status;
            auto tree = state.tree.encode(allocator);
            if (!tree) return tree.status();
            edit_t const folded = import_edit(edits.back().placement, state.tree[state.tree.root()].key, *tree);
            auto chain = encode_edits(std::span<edit_t const>(&folded, 1), allocator);
            allocator.deallocate(const_cast<byte_t*>(tree->data()), tree->size());
            return chain;
        }

        if (status_t status = growth_status(keep.resize(edits.size(), 1)); status.failed()) return status;
        auto targets_content = [](edit_t const& edit) noexcept {
            return edit.modification == modification_t::splice_k || carries_field(edit.modification);
        };
        auto removes = [](edit_t const& edit, element_key_t const& element) noexcept {
            return edit.modification == modification_t::remove_k && edit.element == element;
        };
        // An insertion landing the key again starts a new element that older edits never touched
        auto reincarnates = [](edit_t const& edit, element_key_t const& element) noexcept {
            if (edit.modification != modification_t::insert_k) return false;
            auto view = page_view_t::make(edit.value);
            return !view || view->find(element) != no_element_k;
        };

        for (std::size_t newer = 0; newer != edits.size(); ++newer)
            if (edits[newer].modification == modification_t::remove_k)
                for (std::size_t older = 0; older != newer; ++older)
                    if (targets_content(edits[older]) && edits[older].element == edits[newer].element) keep[older] = 0;

        for (std::size_t older = 0; older != edits.size(); ++older) {
            if (!keep[older] || !carries_field(edits[older].modification)) continue;
            element_key_t const element = edits[older].element;
            for (std::size_t newer = older + 1; newer != edits.size(); ++newer) {
                edit_t const& candidate = edits[newer];
                if (!keep[newer]) continue;
                if (reincarnates(candidate, element) || removes(candidate, element)) break;
                if (!carries_field(candidate.modification) || !(candidate.element == element) ||
                    candidate.field != edits[older].field)
                    continue;
                if (candidate.modification == modification_t::remove_attributes_k) {
                    keep[older] = 0;
                    break;
                }
                if (edits[older].modification == modification_t::remove_attributes_k) break;
                edits[older].value = candidate.value, edits[older].placement = candidate.placement;
                keep[newer] = 0;
            }
        }

        for (std::size_t older = 0; older != edits.size(); ++older) {
            if (!keep[older] || edits[older].modification != modification_t::splice_k || edits[older].text_start)
                continue;
            element_key_t const element = edits[older].element;
            for (std::size_t newer = older + 1; newer != edits.size(); ++newer) {
                edit_t const& candidate = edits[newer];
                if (!keep[newer]) continue;
                if (reincarnates(candidate, element) || removes(candidate, element)) break;
                if (candidate.modification != modification_t::splice_k || !(candidate.element == element)) continue;
                std::span<byte_t const> const inserted = edits[older].value;
                if (candidate.text_start > inserted.size() ||
                    candidate.text_length > inserted.size() - candidate.text_start)
                    break;
                std::size_t const start = std::size_t(candidate.text_start);
                std::size_t const length = std::size_t(candidate.text_length);
                std::size_t const size = inserted.size() - length + candidate.value.size();
                byte_t* const block = size ? allocator.allocate(size) : nullptr;
                if (size && !block) return release(), status_t {status_code_t::out_of_memory_k, "Failed to merge"};
                if (status_t status = growth_status(blocks.push_back(std::span<byte_t>(block, size)));
                    status.failed()) {
                    if (block) allocator.deallocate(block, size);
                    return release(), status;
                }
                if (start) std::memcpy(block, inserted.data(), start);
                if (!candidate.value.empty())
                    std::memcpy(block + start, candidate.value.data(), candidate.value.size());
                if (inserted.size() - start - length)
                    std::memcpy(block + start + candidate.value.size(), inserted.data() + start + length,
                                inserted.size() - start - length);
                edits[older].value = {block, size}, edits[older].placement = candidate.placement;
                keep[newer] = 0;
            }
        }

        std::size_t kept = 0;
        for (std::size_t index = 0; index != edits.size(); ++index)
            if (keep[index]) edits[kept++] = edits[index];
        auto chain = encode_edits(std::span<edit_t const>(edits.data(), kept), allocator);
        release();
        return chain;
    }
};

static_assert(utilities::fold_concept<page_fold_t>);

} // namespace unum::ustore::pages
