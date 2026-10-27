/**
 *  @file include/ustore/strings/nvme_memory_run.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief One commit's deltas for one strings collection, keyed by whole strings on a tape and searched by their
 *      16-byte heads, sealed and stamped into an immutable run, and the cursor that lists a view's strings in order.
 *
 *  A strings run is the twin of @c blobs::memory_run with a key tape in place of the fixed-width keys column. Deltas
 *  ascend by string in unsigned byte order, the versions of one string newest first, and every delta carries the
 *  @c head_of its string, so a search compares heads and reads the tape only between equal ones:
 *
 *  @code{.c}
 *  struct strings_run_columns_t {             // n deltas, ascending by string, versions newest first
 *      key128_t heads[n];                     // 16 B: the leading 15 bytes, zero-padded, then min(length, 16)
 *      u32 key_offsets[n + 1];                //  4 B: delta i's string is keys[key_offsets[i], key_offsets[i + 1])
 *      char keys[key_offsets[n]];             //  every string whole, back to back, zero bytes included
 *      ustore_key_t values[n];                // 16 B: the key delta i's string maps onto, zero for a removal
 *      st::immutable_packed_array<u64> stamps; //  bit_width(newest − oldest) bits, none for one commit
 *      st::immutable_packed_array<u8> kinds;  //  delta_kind_t, 0 to 2 bits
 *  };
 *  @endcode
 *
 *  The keys and the values of a run together fit one sorted run file, which bounds the key offsets. A view of strings
 *  runs is a @c blobs::collection_view, since its walks only compare the keys a run answers.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`
#include <compare>     // `std::strong_ordering`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`, `std::as_bytes`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp>           // `st::basic_vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/row_layout.hpp>             // `st::key128_t`
#include <smashtable/shared.hpp>                 // `st::default_allocator`, `st::expected`
#include <stringzilla/sort.h>                    // `sz_kernel_sort_order_t`
#include <stringzilla/stringzilla.hpp>           // `sz::string_view_t`

#include "ustore/blobs/nvme_memory_run.hpp" // `keyed_delta`, `stamped_delta`, `kept_versions`
#include "ustore/blobs/nvme_compaction.hpp" // `compaction_stamps_t`
#include "ustore/blobs/nvme_key_slot.hpp"   // `transaction_stamp_t`, `stored_value_offset_t`, `storage_limits_t`
#include "ustore/strings/string_keys.hpp"   // `head_of`, `head_length_of`, `tail_of`
#include "ustore/utilities/folds.hpp"       // `delta_kind_t`, `delta_view_t`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `status_t`

namespace unum::ustore::strings {

using blobs::compaction_stamps_t;
using blobs::kept_versions;
using blobs::keyed_delta;
using blobs::stamped_delta;
using blobs::storage_limits_t;
using blobs::stored_value_offset_t;
using blobs::transaction_stamp_t;
using blobs::unwritten_stamp_k;
using utilities::byte_t;
using utilities::delta_kind_t;
using utilities::delta_view_t;

/** Orders the string under @p first_head against the one under @p second_head, reading the tails only when two
 *  heads of longer strings tie, which unsigned byte order of the whole strings agrees with. */
inline std::strong_ordering order_strings(st::key128_t const& first_head, std::string_view first,
                                          st::key128_t const& second_head, std::string_view second) noexcept {
    std::strong_ordering const heads = first_head <=> second_head;
    if (heads != 0 || head_length_of(first_head) != long_head_length_k) return heads;
    return sz::string_view_t {tail_of(first)} <=> sz::string_view_t {tail_of(second)};
}

/** The strings @p deltas write, as StringZilla's kernels read a collection, viewing them where they lie. */
template <typename delta_type_>
sz_strings_view_t strings_of(std::span<delta_type_ const> deltas) noexcept {
    sz_strings_view_t strings;
    strings.layout = sz_strings_accessor_k;
    strings.count = deltas.size();
    strings.data.accessor.handle = deltas.data();
    strings.data.accessor.get = [](void const* handle, sz_size_t const* indices, sz_size_t count,
                                   sz_string_view_t* views) {
        delta_type_ const* const all = static_cast<delta_type_ const*>(handle);
        for (sz_size_t index = 0; index != count; ++index) {
            std::string_view const text = all[indices[index]].stored_key;
            // A caller's empty string may view no bytes at all, which a kernel's compare must still read safely.
            views[index] = sz_string_view_t {text.empty() ? "" : text.data(), text.size()};
        }
    };
    return strings;
}

/** Which of several deltas to one string a deduplicating sort keeps. */
enum class keep_t : std::uint8_t {

    /** The one given first, as a seal of distinct strings expects. */
    first_k = 0,

    /** The one given last, as a batch naming a string twice means. */
    last_k = 1,
};

/**
 *  @brief Orders @p deltas by their strings through @p sort_order and keeps one delta of every string, the first or
 *      the last given as @p keep says, packed at the front in string order.
 *  @param[in] sort_order The StringZilla sort kernel src/ picked for the CPU, which orders bytes like @c memcmp.
 *  @return How many deltas were kept, or the status a kernel or the room for the order refused.
 */
template <typename delta_type_, typename allocator_type_>
expected<std::size_t> sort_unique_strings(std::span<delta_type_> deltas, sz_kernel_sort_order_t sort_order, keep_t keep,
                                          allocator_type_ const& allocator) noexcept {
    using order_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<sz_size_t>;
    using deltas_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<delta_type_>;
    if (sort_order == nullptr) return status_t {status_code_t::invalid_argument_k, "No kernel sorts the strings"};
    st::basic_vector<sz_size_t, order_allocator_t> order {order_allocator_t {allocator}};
    if (order.resize(deltas.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room to order the strings"};
    sz_strings_view_t const strings = strings_of(std::span<delta_type_ const> {deltas});
    sz_status_t const sorted = sort_order(&strings, 0, sz_false_k, nullptr, order.data(), nullptr);
    if (sorted != sz_success_k)
        return status_t {sorted == sz_bad_alloc_k ? status_code_t::out_of_memory_k : status_code_t::internal_k,
                         sz_status_name(sorted)};

    // Choosing among equal strings by their given positions keeps the answer whatever the sort's stability.
    std::size_t kept = 0;
    for (std::size_t index = 0; index != order.size(); ++index) {
        sz_size_t const given = order[index];
        if (kept == 0 ||
            std::string_view {deltas[order[kept - 1]].stored_key} != std::string_view {deltas[given].stored_key}) {
            order[kept++] = given;
            continue;
        }
        sz_size_t& survivor = order[kept - 1];
        survivor = keep == keep_t::first_k ? std::min(survivor, given) : std::max(survivor, given);
    }
    st::basic_vector<delta_type_, deltas_allocator_t> survivors {deltas_allocator_t {allocator}};
    if (survivors.resize(kept) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room to gather the kept deltas"};
    for (std::size_t index = 0; index != kept; ++index) survivors[index] = deltas[order[index]];
    for (std::size_t index = 0; index != kept; ++index) deltas[index] = survivors[index];
    return kept;
}

/**
 *  @brief The deltas one transaction wrote to one strings collection, sorted by string and copied into columns, not
 *      yet stamped.
 *
 *  Sealing runs on the committing thread before the commit is ordered, as @c blobs::sealed_deltas does, so the sort
 *  and the copies of the strings and their values never sit on a path another commit waits on. Every write carries
 *  one 16-byte value, the key its string maps onto.
 */
template <typename allocator_type_ = st::default_allocator<byte_t>>
class sealed_deltas {
  public:
    using stored_key_t = std::string_view;
    using allocator_t = allocator_type_;
    using keyed_delta_t = keyed_delta<stored_key_t>;
    using stamped_delta_t = stamped_delta<stored_key_t>;

    /** The kind of every delta, packed from the least kind present, so a run of writes alone spends no bits. */
    using kinds_t =
        st::immutable_packed_array<std::uint8_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    sealed_deltas() noexcept = default;
    sealed_deltas(sealed_deltas&&) noexcept = default;
    sealed_deltas& operator=(sealed_deltas&&) noexcept = default;
    sealed_deltas(sealed_deltas const&) = delete;
    sealed_deltas& operator=(sealed_deltas const&) = delete;

    /**
     *  @brief Sorts @p deltas by string in place through @p sort_order and copies them, strings and bytes alike, into
     *      columns on @p allocator.
     *  @param[in] sort_order The StringZilla sort kernel src/ picked for the CPU.
     *  @retval invalid_argument_k No deltas, two deltas of one string, a removal carrying bytes, or a write carrying
     *      anything but one 16-byte value.
     *  @retval capacity_exhausted_k More string and value bytes than one sorted run file holds.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> seal(std::span<keyed_delta_t> deltas, sz_kernel_sort_order_t sort_order,
                                        allocator_t allocator = {}) noexcept {
        if (deltas.empty()) return status_t {status_code_t::invalid_argument_k, "A seal takes at least one delta"};
        auto [kept, sort_status] = sort_unique_strings(deltas, sort_order, keep_t::first_k, allocator);
        if (sort_status.failed()) return sort_status;
        if (kept != deltas.size())
            return status_t {status_code_t::invalid_argument_k, "A seal takes one delta per string"};
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            std::size_t const carried = deltas[index].delta.kind == delta_kind_t::remove_k ? 0 : value_bytes_k;
            if (deltas[index].delta.bytes.size() != carried)
                return status_t {status_code_t::invalid_argument_k, "A write carries one key and a removal none"};
        }
        return columns_of_<keyed_delta_t>(deltas, allocator);
    }

    /**
     *  @brief Copies @p versions into columns on @p allocator, in the order given, which a merge of runs hands over
     *      in string order with the versions of one string newest first.
     *  @retval capacity_exhausted_k More string and value bytes than one sorted run file holds.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> merged(std::span<stamped_delta_t const> versions,
                                          allocator_t allocator = {}) noexcept {
        return columns_of_(versions, allocator);
    }

    /** Deltas sealed. */
    [[nodiscard]] std::size_t size() const noexcept { return heads_.size(); }

    /** Bytes every delta's string takes together. */
    [[nodiscard]] std::size_t keys_bytes() const noexcept { return keys_.size(); }

    /** Bytes every delta's value takes together, 16 per delta. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return values_.size() * value_bytes_k; }

    /** The string at sorted @p position, which may hold zero bytes. */
    [[nodiscard]] std::string_view stored_key(std::size_t position) const noexcept {
        std::size_t const begin = key_offsets_[position];
        return std::string_view(keys_.data() + begin, key_offsets_[position + 1] - begin);
    }

    /** The head of the string at sorted @p position. */
    [[nodiscard]] st::key128_t const& head(std::size_t position) const noexcept { return heads_[position]; }

    /** The delta at sorted @p position: its kind and the 16 bytes of its value, none for a removal. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept {
        delta_kind_t const kind = static_cast<delta_kind_t>(kinds_[position]);
        if (kind == delta_kind_t::remove_k) return delta_view_t {kind, {}};
        return delta_view_t {kind, std::as_bytes(std::span<ustore_key_t const, 1> {&values_[position], 1})};
    }

    /** The first sorted position whose string is not below @p text, or @c size when every string is. */
    [[nodiscard]] std::size_t lower_bound(std::string_view text) const noexcept {
        st::key128_t const wanted = head_of(text);
        std::size_t low = 0;
        std::size_t high = heads_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            if (order_strings(heads_[middle], stored_key(middle), wanted, text) < 0) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** The first sorted position past every string starting with @p prefix, or @c size when none is past them. */
    [[nodiscard]] std::size_t prefix_end(std::string_view prefix) const noexcept {
        // A head's padding zeros never sort above a prefix's bytes, so a string shorter than the prefix that ties
        // with it in the head is one of its prefixes and sorts below it, as every string below the window does.
        std::string_view const leading = prefix.substr(0, std::min(prefix.size(), head_prefix_bytes_k));
        std::string_view const trailing = tail_of(prefix);
        std::size_t low = 0;
        std::size_t high = heads_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            st::key128_t const& head = heads_[middle];
            std::byte head_bytes[sizeof(st::key128_t)];
            head.to_bytes(head_bytes);
            std::string_view const head_leading(reinterpret_cast<char const*>(head_bytes), leading.size());
            std::strong_ordering const order = leading.empty()
                                                   ? std::strong_ordering::equal
                                                   : sz::string_view_t {head_leading} <=> sz::string_view_t {leading};
            // Only a string longer than its head can reach past the leading bytes of a longer prefix.
            bool const within_or_below = order < 0 ||
                                         (order == 0 &&
                                          (trailing.empty() || head_length_of(head) != long_head_length_k ||
                                           sz::string_view_t {tail_of(stored_key(middle)).substr(0, trailing.size())} <=
                                               sz::string_view_t {trailing}));
            if (within_or_below) low = middle + 1;
            else high = middle;
        }
        return low;
    }

  private:

    /** Bytes one value takes: the key a string maps onto. */
    static constexpr std::size_t value_bytes_k = sizeof(ustore_key_t);

    template <typename value_type_>
    using column =
        st::basic_vector<value_type_, typename std::allocator_traits<allocator_t>::template rebind_alloc<value_type_>>;

    explicit sealed_deltas(allocator_t const& allocator) noexcept
        : heads_(typename column<st::key128_t>::allocator_t(allocator)),
          key_offsets_(typename column<stored_value_offset_t>::allocator_t(allocator)),
          keys_(typename column<char>::allocator_t(allocator)),
          values_(typename column<ustore_key_t>::allocator_t(allocator)) {}

    /** Copies the strings, the heads, the values and the kinds of @p deltas, in their order, into the columns. */
    template <typename delta_type_>
    static expected<sealed_deltas> columns_of_(std::span<delta_type_ const> deltas,
                                               allocator_t const& allocator) noexcept {
        std::uint64_t keys_bytes = 0;
        std::uint8_t least_kind = deltas.empty() ? 0 : static_cast<std::uint8_t>(deltas[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (delta_type_ const& each : deltas) {
            keys_bytes += each.stored_key.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(each.delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(each.delta.kind));
        }
        // A run flushes into one sorted run file, whose length bounds every offset of the key tape.
        if (keys_bytes + deltas.size() * value_bytes_k > storage_limits_t::sorted_run_file_max_size_k)
            return status_t {status_code_t::capacity_exhausted_k, "A strings run outgrows one run file"};

        sealed_deltas sealed(allocator);
        if (sealed.heads_.resize(deltas.size()) != st::status_t::success_k ||
            sealed.key_offsets_.resize(deltas.size() + 1) != st::status_t::success_k ||
            sealed.keys_.resize(static_cast<std::size_t>(keys_bytes)) != st::status_t::success_k ||
            sealed.values_.resize(deltas.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to seal the deltas of strings"};
        stored_value_offset_t key_offset = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            std::string_view const text = deltas[index].stored_key;
            std::span<byte_t const> const bytes = deltas[index].delta.bytes;
            sealed.heads_[index] = head_of(text);
            sealed.key_offsets_[index] = key_offset;
            sealed.values_[index] = ustore_key_t {};
            if (!text.empty()) std::memcpy(sealed.keys_.data() + key_offset, text.data(), text.size());
            if (!bytes.empty()) std::memcpy(&sealed.values_[index], bytes.data(), value_bytes_k);
            key_offset += static_cast<stored_value_offset_t>(text.size());
        }
        sealed.key_offsets_[deltas.size()] = key_offset;

        std::uint8_t pulled = 0;
        auto fill = [&](std::size_t position) noexcept {
            pulled = static_cast<std::uint8_t>(deltas[position].delta.kind);
            return st::expected<std::span<std::uint8_t const>>(std::span<std::uint8_t const>(&pulled, 1),
                                                               st::status_t::success_k);
        };
        st::expected<kinds_t> kinds = kinds_t::make(deltas.size(), least_kind, greatest_kind, fill,
                                                    typename kinds_t::allocator_t(allocator));
        if (!kinds) return status_t {status_code_t::out_of_memory_k, "No room to pack the kinds of strings deltas"};
        sealed.kinds_ = std::move(*kinds);
        return sealed;
    }

    /** The head of every string, in sorted order. */
    column<st::key128_t> heads_;

    /** One offset per delta and one past the last, so a delta's string runs from its offset to the next one. */
    column<stored_value_offset_t> key_offsets_;

    /** Every delta's string whole, back to back, in sorted order. */
    column<char> keys_;

    /** Every delta's value as its 16 bytes came, zero for a removal, in sorted order. */
    column<ustore_key_t> values_;

    /** Every delta's @c delta_kind_t, packed. */
    kinds_t kinds_;
};

/**
 *  @brief One commit's deltas for one strings collection, stamped, ordered by string, and never changed once
 *      published.
 *
 *  Versions of one string sit newest first, which a merge of several runs relies on; a run sealed from one commit
 *  holds one version per string. The stamps column is based at the oldest stamp, so a run of one commit spends no
 *  bits on it.
 */
template <typename allocator_type_ = st::default_allocator<byte_t>>
class memory_run {
  public:
    using stored_key_t = std::string_view;
    using allocator_t = allocator_type_;
    using sealed_deltas_t = sealed_deltas<allocator_t>;
    using stamped_delta_t = stamped_delta<stored_key_t>;
    using stamps_t =
        st::immutable_packed_array<std::uint64_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    memory_run() noexcept = default;
    memory_run(memory_run&&) noexcept = default;
    memory_run& operator=(memory_run&&) noexcept = default;
    memory_run(memory_run const&) = delete;
    memory_run& operator=(memory_run const&) = delete;

    /**
     *  @brief Stamps every delta of @p deltas with the commit's @p stamp, which only the leader ordering it draws.
     *  @retval invalid_argument_k An unwritten stamp, which no commit carries.
     */
    static expected<memory_run> stamped(sealed_deltas_t&& deltas, transaction_stamp_t stamp,
                                        allocator_t allocator = {}) noexcept {
        if (stamp == unwritten_stamp_k)
            return status_t {status_code_t::invalid_argument_k, "No commit carries the unwritten stamp"};
        st::expected<stamps_t> stamps = stamps_t::make(deltas.size(), stamp, typename stamps_t::allocator_t(allocator));
        if (!stamps) return status_t {status_code_t::out_of_memory_k, "No room to stamp the deltas of strings"};
        memory_run run;
        run.deltas_ = std::move(deltas);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = stamp;
        return run;
    }

    /**
     *  @brief The run one merge makes of @p versions, which come in string order with the versions of a string newest
     *      first, for runs of one tier it replaces.
     *  @param[in] oldest_stamp The oldest stamp of the runs it replaces, which the run keeps covering where that
     *      version did not survive, so neighbouring runs still never overlap.
     *  @param[in] tier The tier of the runs it replaces, which the run is one above.
     */
    static expected<memory_run> merged(std::span<stamped_delta_t const> versions, transaction_stamp_t oldest_stamp,
                                       std::uint32_t tier, allocator_t allocator = {}) noexcept {
        transaction_stamp_t newest_stamp = oldest_stamp;
        for (stamped_delta_t const& version : versions) newest_stamp = std::max(newest_stamp, version.stamp);
        auto [deltas, merged_status] = sealed_deltas_t::merged(versions, allocator);
        if (merged_status.failed()) return merged_status;
        transaction_stamp_t pulled = unwritten_stamp_k;
        auto fill = [&](std::size_t position) noexcept {
            pulled = versions[position].stamp;
            return st::expected<std::span<transaction_stamp_t const>>(std::span<transaction_stamp_t const>(&pulled, 1),
                                                                      st::status_t::success_k);
        };
        st::expected<stamps_t> stamps = stamps_t::make(versions.size(), oldest_stamp, newest_stamp, fill,
                                                       typename stamps_t::allocator_t(allocator));
        if (!stamps) return status_t {status_code_t::out_of_memory_k, "No room to stamp a merged strings run"};
        memory_run run;
        run.deltas_ = std::move(deltas);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = newest_stamp;
        run.tier_ = tier + 1;
        return run;
    }

    /** How many merges built the run: tier zero for one commit's, and one above the runs a merge replaced. */
    [[nodiscard]] std::uint32_t tier() const noexcept { return tier_; }

    /** Deltas the run holds, every version counted. */
    [[nodiscard]] std::size_t size() const noexcept { return deltas_.size(); }

    /** Bytes every delta's string takes together. */
    [[nodiscard]] std::size_t keys_bytes() const noexcept { return deltas_.keys_bytes(); }

    /** Bytes every delta's value takes together, 16 per delta. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return deltas_.values_bytes(); }

    /** Bytes the run charges against its store's memory bounds: its strings and its values. */
    [[nodiscard]] std::uint64_t memory_bytes() const noexcept { return keys_bytes() + values_bytes(); }

    /** The string at sorted @p position. */
    [[nodiscard]] std::string_view stored_key(std::size_t position) const noexcept {
        return deltas_.stored_key(position);
    }

    /** The head of the string at sorted @p position. */
    [[nodiscard]] st::key128_t const& head(std::size_t position) const noexcept { return deltas_.head(position); }

    /** The delta at sorted @p position: its kind and the 16 bytes of its value, none for a removal. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept { return deltas_.delta(position); }

    /** The stamp of the commit that wrote the delta at sorted @p position. */
    [[nodiscard]] transaction_stamp_t stamp(std::size_t position) const noexcept { return stamps_[position]; }

    /** The first sorted position whose string is not below @p text, or @c size when every string is. */
    [[nodiscard]] std::size_t lower_bound(std::string_view text) const noexcept { return deltas_.lower_bound(text); }

    /** The first sorted position past every string starting with @p prefix. */
    [[nodiscard]] std::size_t prefix_end(std::string_view prefix) const noexcept { return deltas_.prefix_end(prefix); }

    /** The newest stamp any delta carries. */
    [[nodiscard]] transaction_stamp_t newest_stamp() const noexcept { return newest_stamp_; }

    /** The oldest stamp any delta carries, which the stamps column is based at. */
    [[nodiscard]] transaction_stamp_t oldest_stamp() const noexcept { return stamps_.base(); }

  private:

    /** The sorted heads, strings, offsets, values and kinds. */
    sealed_deltas_t deltas_;

    /** The stamp of every delta in sorted order. */
    stamps_t stamps_;

    /** The newest stamp in @c stamps_, which a check of what changed since a stamp skips the run by. */
    transaction_stamp_t newest_stamp_ = unwritten_stamp_k;

    /** How many merges built the run. */
    std::uint32_t tier_ = 0;
};

/** One string a cursor lists, with its newest version at or below the cursor's stamp, a removal included. */
struct string_version_t {

    /** The string, viewing the run that holds it. */
    std::string_view string;

    /** What the version does and its bytes, viewing the run that holds it. */
    delta_view_t delta;

    /** The stamp of the commit that wrote the version. */
    transaction_stamp_t stamp = unwritten_stamp_k;
};

/**
 *  @brief Lists the strings of one view in order, from a start string on and within one prefix, each with its newest
 *      version at or below a stamp.
 *
 *  Each run's window is cut by binary searches over its heads, so a prefix shared by every string of a run costs two
 *  searches, and the walk compares heads across runs, reading tails only where two heads tie. A removal is listed as
 *  any version is, since it hides older versions the levels below may hold. A cursor borrows its view, so a scan
 *  takes a page inside one epoch and resumes the next from a later start string.
 */
template <typename view_type_, typename allocator_type_ = st::default_allocator<byte_t>>
class memory_run_cursor {
  public:
    using view_t = view_type_;
    using run_t = typename view_t::run_t;
    using allocator_t = allocator_type_;

    memory_run_cursor() noexcept = default;
    memory_run_cursor(memory_run_cursor&&) noexcept = default;
    memory_run_cursor& operator=(memory_run_cursor&&) noexcept = default;
    memory_run_cursor(memory_run_cursor const&) = delete;
    memory_run_cursor& operator=(memory_run_cursor const&) = delete;

    /**
     *  @brief A cursor over @p view at stamp @p at, listing the strings starting with @p prefix that are not below
     *      @p start, where an empty prefix admits every string.
     *  @retval out_of_memory_k The positions in each run could not be allocated.
     */
    static expected<memory_run_cursor> over(view_t const& view, std::string_view prefix, std::string_view start,
                                            transaction_stamp_t at, allocator_t allocator = {}) noexcept {
        memory_run_cursor cursor {view, at, allocator};
        std::span<run_t const* const> const runs = view.runs();
        if (cursor.positions_.resize(runs.size()) != st::status_t::success_k ||
            cursor.ends_.resize(runs.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A cursor could not hold a position per run"};
        std::string_view const from = std::max(start, prefix);
        for (std::size_t index = 0; index != runs.size(); ++index) {
            cursor.ends_[index] = runs[index]->prefix_end(prefix);
            cursor.positions_[index] = std::min(runs[index]->lower_bound(from), cursor.ends_[index]);
        }
        return cursor;
    }

    /** Moves past the next string with a version at or below the cursor's stamp, answering it in @p listed, and
     *  answers whether one was left. */
    bool next(string_version_t& listed) noexcept {
        std::span<run_t const* const> const runs = view_->runs();
        for (;;) {
            std::size_t smallest = runs.size();
            for (std::size_t index = 0; index != runs.size(); ++index) {
                if (positions_[index] == ends_[index]) continue;
                if (smallest != runs.size() &&
                    order_strings(runs[index]->head(positions_[index]), runs[index]->stored_key(positions_[index]),
                                  runs[smallest]->head(positions_[smallest]),
                                  runs[smallest]->stored_key(positions_[smallest])) >= 0)
                    continue;
                smallest = index;
            }
            if (smallest == runs.size()) return false;

            st::key128_t const head = runs[smallest]->head(positions_[smallest]);
            std::string_view const string = runs[smallest]->stored_key(positions_[smallest]);
            // Runs sit newest first with disjoint stamps and a run's versions of a string newest first, so the first
            // version at or below the stamp is the newest, and every cursor passes all the string's versions at once.
            bool taken = false;
            for (std::size_t index = 0; index != runs.size(); ++index) {
                run_t const& run = *runs[index];
                std::size_t& position = positions_[index];
                for (; position != ends_[index] &&
                       order_strings(run.head(position), run.stored_key(position), head, string) == 0;
                     ++position) {
                    if (taken || run.stamp(position) > at_) continue;
                    listed = string_version_t {string, run.delta(position), run.stamp(position)};
                    taken = true;
                }
            }
            if (taken) return true;
        }
    }

  private:
    using positions_t =
        st::basic_vector<std::size_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<std::size_t>>;

    memory_run_cursor(view_t const& view, transaction_stamp_t at, allocator_t const& allocator) noexcept
        : view_(&view), at_(at), positions_(typename positions_t::allocator_t(allocator)),
          ends_(typename positions_t::allocator_t(allocator)) {}

    /** The view walked, borrowed while the cursor lives. */
    view_t const* view_ = nullptr;

    /** The stamp the cursor reads at. */
    transaction_stamp_t at_ = unwritten_stamp_k;

    /** The next sorted position in every run, in the order the view names the runs. */
    positions_t positions_;

    /** The first sorted position past the prefix in every run. */
    positions_t ends_;
};

} // namespace unum::ustore::strings
