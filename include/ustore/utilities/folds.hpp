/**
 *  @file include/ustore/utilities/folds.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The fold that turns a stored base and the newer deltas of one key into the value a read
 *      sees, and @c last_write_fold_t, where the newest delta wins.
 *
 *  A collection keeps a base per key and appends deltas newer than it. A read folds the base with the deltas, oldest
 *  first, and a compaction folds a chain of deltas without its base into one, so the fold of a modality decides what a
 *  delta means while the stores stay agnostic of it.
 */

#pragma once

#include <cstdint> // `std::uint8_t`

#include <concepts> // `std::same_as`
#include <span>     // `std::span`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `status_t`

namespace unum::ustore::utilities {

/** What one delta does to the value of its key. */
enum class delta_kind_t : std::uint8_t {

    /** Replaces the whole value, closing every older delta. */
    write_k = 0,

    /** Removes the key, closing every older delta. */
    remove_k = 1,

    /** Changes part of the value, which only the modality's fold reads. */
    change_k = 2,
};

/** One delta of a key: its kind and the bytes it carries. */
struct delta_view_t {

    /** What the delta does to the value. */
    delta_kind_t kind = delta_kind_t::write_k;

    /** The bytes the delta carries, empty for a removal. */
    std::span<byte_t const> bytes;
};

/**
 *  @brief Folds the base of a key and the deltas newer than it into the value a read sees.
 *
 *  Deltas arrive oldest first, so the newest is last. @c apply folds a base with its deltas, @c compose folds a chain
 *  of deltas without a base into one equivalent delta, as compaction does, and @c closes tells a delta after which no
 *  older one matters, such as a removal or a full rewrite. A result may view the inputs, or bytes the fold wrote into
 *  the arena it is given.
 */
template <typename fold_type_>
concept fold_concept = requires(fold_type_ const& fold, std::span<byte_t const> base,
                                std::span<delta_view_t const> deltas, arena_allocator<byte_t>& arena) {
    { fold.apply(base, deltas, arena) } -> std::same_as<expected<std::span<byte_t const>>>;
    { fold.compose(deltas, arena) } -> std::same_as<expected<std::span<byte_t const>>>;
    { fold.closes(deltas.front()) } -> std::same_as<bool>;
};

/** The fold of blobs and strings, where every delta is a whole value or a removal and the newest one wins. */
struct last_write_fold_t {

    /** The newest delta, or @p base without any, refusing with @c status_code_t::not_found_k after a removal. */
    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            arena_allocator<byte_t>&) const noexcept {
        if (deltas.empty()) return base;
        delta_view_t const& newest = deltas.back();
        if (newest.kind == delta_kind_t::remove_k) return status_t {status_code_t::not_found_k, "The key was removed"};
        return newest.bytes;
    }

    /** The bytes of the newest delta, which stands for the whole chain, refusing an empty chain. */
    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              arena_allocator<byte_t>&) const noexcept {
        if (deltas.empty()) return status_t {status_code_t::invalid_argument_k, "No deltas to compose"};
        return deltas.back().bytes;
    }

    /** Every delta replaces or removes the whole value, so each one closes the chain behind it. */
    bool closes(delta_view_t const&) const noexcept { return true; }
};

static_assert(fold_concept<last_write_fold_t>);

/**
 *  @brief The one delta that stands for change @p newer over @p older, which a merge keeps where no reader tells the
 *      two apart: a write of @p newer applied to a write or a removal below it, or one change composed of both.
 *  @return The delta, whose bytes may sit in @p arena, or the status @p fold refused it with.
 */
template <fold_concept fold_type_>
expected<delta_view_t> fold_delta_pair(fold_type_ const& fold, delta_view_t const& newer, delta_view_t const& older,
                                       arena_allocator<byte_t>& arena) noexcept {
    if (older.kind == delta_kind_t::change_k) {
        delta_view_t const chain[2] {older, newer};
        auto [composed, compose_status] = fold.compose(std::span<delta_view_t const> {chain}, arena);
        if (compose_status.failed()) return compose_status;
        return delta_view_t {delta_kind_t::change_k, composed};
    }
    std::span<byte_t const> const base = older.kind == delta_kind_t::write_k ? older.bytes : std::span<byte_t const> {};
    auto [applied, apply_status] = fold.apply(base, std::span<delta_view_t const> {&newer, 1}, arena);
    if (apply_status.failed()) return apply_status;
    return delta_view_t {delta_kind_t::write_k, applied};
}

} // namespace unum::ustore::utilities
