/**
 *  @file include/ustore/strings/nvme_memory_run.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief One commit's records for one strings collection, keyed by whole strings on a tape and searched by their
 *      16-byte heads, sealed and stamped into an immutable run, and the cursor that lists a view's strings in order.
 *
 *  A strings run is the twin of @c blobs::memory_run with a key tape in place of the fixed-width keys column. Records
 *  ascend by string in unsigned byte order, the versions of one string newest first, and every record carries the
 *  @c head_of its string, so a search compares heads and reads the tape only between equal ones:
 *
 *  @code{.c}
 *  struct strings_run_columns_t {             // n records, ascending by string, versions newest first
 *      ustore_key_t heads[n];                 // 16 B: the leading 15 bytes, zero-padded, then min(length, 16)
 *      u32 key_offsets[n + 1];                //  4 B: record i's string is keys[key_offsets[i], key_offsets[i + 1])
 *      char keys[key_offsets[n]];             //  every string whole, back to back, zero bytes included
 *      u32 value_offsets[n + 1];              //  4 B: record i's bytes are values[value_offsets[i], ...[i + 1])
 *      st::immutable_packed_array<u64> stamps; //  bit_width(newest − oldest) bits, none for one commit
 *      st::immutable_packed_array<u8> kinds;  //  delta_kind_t, 0 to 2 bits
 *      byte_t values[value_offsets[n]];       //  back to back in string order
 *  };
 *  @endcode
 *
 *  The keys and the values of a run together fit one sorted run file, which bounds both offsets columns. A view of
 *  strings runs is a @c blobs::collection_view, since its walks only compare the keys a run answers.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`, `std::sort`
#include <compare>     // `std::strong_ordering`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp>           // `st::basic_vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/shared.hpp>                 // `st::default_allocator`, `st::expected`
#include <stringzilla/stringzilla.hpp>           // `sz::string_view_t`

#include "ustore/blobs/nvme_memory_run.hpp" // `keyed_delta`, `stamped_delta`, `kept_versions`
#include "ustore/blobs/nvme_compaction.hpp" // `compaction_stamps_t`
#include "ustore/blobs/nvme_key_slot.hpp"   // `transaction_stamp_t`, `stored_value_offset_t`, `storage_limits_t`
#include "ustore/strings/string_keys.hpp"   // `head_of`, `tail_of`
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
inline std::strong_ordering order_strings(ustore_key_t const& first_head, std::string_view first,
                                          ustore_key_t const& second_head, std::string_view second) noexcept {
    std::strong_ordering const heads = first_head <=> second_head;
    if (heads != 0 || first_head.uuid.bytes[head_prefix_bytes_k] != long_head_length_k) return heads;
    return sz::string_view_t {tail_of(first)} <=> sz::string_view_t {tail_of(second)};
}

/**
 *  @brief The records one transaction wrote to one strings collection, sorted by string and copied into columns, not
 *      yet stamped.
 *
 *  Sealing runs on the committing thread before the commit is ordered, as @c blobs::sealed_writes does, so the sort
 *  and the copies of the strings and their values never sit on a path another commit waits on.
 */
template <typename allocator_type_ = st::default_allocator<byte_t>>
class strings_sealed_writes {
  public:
    using stored_key_t = std::string_view;
    using allocator_t = allocator_type_;
    using keyed_delta_t = keyed_delta<stored_key_t>;
    using stamped_delta_t = stamped_delta<stored_key_t>;

    /** The kind of every record, packed from the least kind present, so a run of writes alone spends no bits. */
    using kinds_t =
        st::immutable_packed_array<std::uint8_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    strings_sealed_writes() noexcept = default;
    strings_sealed_writes(strings_sealed_writes&&) noexcept = default;
    strings_sealed_writes& operator=(strings_sealed_writes&&) noexcept = default;
    strings_sealed_writes(strings_sealed_writes const&) = delete;
    strings_sealed_writes& operator=(strings_sealed_writes const&) = delete;

    /**
     *  @brief Sorts @p deltas by string in place and copies them, strings and bytes alike, into columns on
     *      @p allocator.
     *  @retval invalid_argument_k No records, two records of one string, or a removal carrying bytes.
     *  @retval capacity_exhausted_k More string and value bytes than one sorted run file holds.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<strings_sealed_writes> seal(std::span<keyed_delta_t> deltas, allocator_t allocator = {}) noexcept {
        if (deltas.empty())
            return status_t {status_code_t::invalid_argument_k, "A write set seals at least one record"};
        // A caller's empty string may view no bytes at all, which only the standard views compare safely.
        std::sort(deltas.begin(), deltas.end(), [](keyed_delta_t const& first, keyed_delta_t const& second) noexcept {
            return first.stored_key < second.stored_key;
        });
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            if (index != 0 && deltas[index - 1].stored_key == deltas[index].stored_key)
                return status_t {status_code_t::invalid_argument_k, "A write set holds one record per string"};
            if (deltas[index].delta.kind == delta_kind_t::remove_k && !deltas[index].delta.bytes.empty())
                return status_t {status_code_t::invalid_argument_k, "A removal carries no bytes"};
        }
        return columns_of_<keyed_delta_t>(deltas, allocator);
    }

    /**
     *  @brief Copies @p versions into columns on @p allocator, in the order given, which a merge of runs hands over
     *      in string order with the versions of one string newest first.
     *  @retval capacity_exhausted_k More string and value bytes than one sorted run file holds.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<strings_sealed_writes> merged(std::span<stamped_delta_t const> versions,
                                                  allocator_t allocator = {}) noexcept {
        return columns_of_(versions, allocator);
    }

    /** Records sealed. */
    [[nodiscard]] std::size_t size() const noexcept { return heads_.size(); }

    /** Bytes every record's string takes together. */
    [[nodiscard]] std::size_t keys_bytes() const noexcept { return keys_.size(); }

    /** Bytes every record's value takes together. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return values_.size(); }

    /** The string at sorted @p position, which may hold zero bytes. */
    [[nodiscard]] std::string_view stored_key(std::size_t position) const noexcept {
        std::size_t const begin = key_offsets_[position];
        return std::string_view(keys_.data() + begin, key_offsets_[position + 1] - begin);
    }

    /** The head of the string at sorted @p position. */
    [[nodiscard]] ustore_key_t const& head(std::size_t position) const noexcept { return heads_[position]; }

    /** What the record at sorted @p position does and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept {
        std::size_t const begin = value_offsets_[position];
        std::size_t const end = value_offsets_[position + 1];
        return delta_view_t {static_cast<delta_kind_t>(kinds_[position]),
                             std::span<byte_t const>(values_.data() + begin, end - begin)};
    }

    /** The first sorted position whose string is not below @p text, or @c size when every string is. */
    [[nodiscard]] std::size_t lower_bound(std::string_view text) const noexcept {
        ustore_key_t const wanted = head_of(text);
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
            ustore_key_t const& head = heads_[middle];
            std::string_view const head_bytes(reinterpret_cast<char const*>(head.uuid.bytes), leading.size());
            std::strong_ordering const order = leading.empty()
                                                   ? std::strong_ordering::equal
                                                   : sz::string_view_t {head_bytes} <=> sz::string_view_t {leading};
            // Only a string longer than its head can reach past the leading bytes of a longer prefix.
            bool const within_or_below =
                order < 0 ||
                (order == 0 && (trailing.empty() || head.uuid.bytes[head_prefix_bytes_k] != long_head_length_k ||
                                sz::string_view_t {tail_of(stored_key(middle)).substr(0, trailing.size())} <=
                                    sz::string_view_t {trailing}));
            if (within_or_below) low = middle + 1;
            else high = middle;
        }
        return low;
    }

  private:
    template <typename value_type_>
    using column_t =
        st::basic_vector<value_type_, typename std::allocator_traits<allocator_t>::template rebind_alloc<value_type_>>;

    explicit strings_sealed_writes(allocator_t const& allocator) noexcept
        : heads_(typename column_t<ustore_key_t>::allocator_t(allocator)),
          key_offsets_(typename column_t<stored_value_offset_t>::allocator_t(allocator)),
          keys_(typename column_t<char>::allocator_t(allocator)),
          value_offsets_(typename column_t<stored_value_offset_t>::allocator_t(allocator)),
          values_(typename column_t<byte_t>::allocator_t(allocator)) {}

    /** Copies the strings, the heads, the bytes and the kinds of @p records, in their order, into the columns. */
    template <typename record_type_>
    static expected<strings_sealed_writes> columns_of_(std::span<record_type_ const> records,
                                                       allocator_t const& allocator) noexcept {
        std::uint64_t keys_bytes = 0;
        std::uint64_t values_bytes = 0;
        std::uint8_t least_kind = records.empty() ? 0 : static_cast<std::uint8_t>(records[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (record_type_ const& record : records) {
            keys_bytes += record.stored_key.size();
            values_bytes += record.delta.bytes.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(record.delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(record.delta.kind));
        }
        // A run flushes into one sorted run file, whose length bounds every offset of both tapes.
        if (keys_bytes + values_bytes > storage_limits_t::sorted_run_file_max_size_k)
            return status_t {status_code_t::capacity_exhausted_k, "A strings run outgrows one run file"};

        strings_sealed_writes sealed(allocator);
        if (sealed.heads_.resize(records.size()) != st::status_t::success_k ||
            sealed.key_offsets_.resize(records.size() + 1) != st::status_t::success_k ||
            sealed.keys_.resize(static_cast<std::size_t>(keys_bytes)) != st::status_t::success_k ||
            sealed.value_offsets_.resize(records.size() + 1) != st::status_t::success_k ||
            sealed.values_.resize(static_cast<std::size_t>(values_bytes)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to seal a strings write set"};
        stored_value_offset_t key_offset = 0;
        stored_value_offset_t value_offset = 0;
        for (std::size_t index = 0; index != records.size(); ++index) {
            std::string_view const text = records[index].stored_key;
            std::span<byte_t const> const bytes = records[index].delta.bytes;
            sealed.heads_[index] = head_of(text);
            sealed.key_offsets_[index] = key_offset;
            sealed.value_offsets_[index] = value_offset;
            if (!text.empty()) std::memcpy(sealed.keys_.data() + key_offset, text.data(), text.size());
            if (!bytes.empty()) std::memcpy(sealed.values_.data() + value_offset, bytes.data(), bytes.size());
            key_offset += static_cast<stored_value_offset_t>(text.size());
            value_offset += static_cast<stored_value_offset_t>(bytes.size());
        }
        sealed.key_offsets_[records.size()] = key_offset;
        sealed.value_offsets_[records.size()] = value_offset;

        std::uint8_t pulled = 0;
        auto fill = [&](std::size_t position) noexcept {
            pulled = static_cast<std::uint8_t>(records[position].delta.kind);
            return st::expected<std::span<std::uint8_t const>>(std::span<std::uint8_t const>(&pulled, 1),
                                                               st::status_t::success_k);
        };
        st::expected<kinds_t> kinds = kinds_t::make(records.size(), least_kind, greatest_kind, fill,
                                                    typename kinds_t::allocator_t(allocator));
        if (!kinds) return status_t {status_code_t::out_of_memory_k, "No room to pack a strings write set's kinds"};
        sealed.kinds_ = std::move(*kinds);
        return sealed;
    }

    /** The head of every string, in sorted order. */
    column_t<ustore_key_t> heads_;

    /** One offset per record and one past the last, so a record's string runs from its offset to the next one. */
    column_t<stored_value_offset_t> key_offsets_;

    /** Every record's string whole, back to back, in sorted order. */
    column_t<char> keys_;

    /** One offset per record and one past the last, so a record's bytes run from its offset to the next one. */
    column_t<stored_value_offset_t> value_offsets_;

    /** Every record's bytes back to back, in sorted order. */
    column_t<byte_t> values_;

    /** Every record's @c delta_kind_t, packed. */
    kinds_t kinds_;
};

/**
 *  @brief One commit's records for one strings collection, stamped, ordered by string, and never changed once
 *      published.
 *
 *  Versions of one string sit newest first, which a merge of several runs relies on; a run sealed from one commit
 *  holds one version per string. The stamps column is based at the oldest stamp, so a run of one commit spends no
 *  bits on it.
 */
template <typename allocator_type_ = st::default_allocator<byte_t>>
class strings_memory_run {
  public:
    using stored_key_t = std::string_view;
    using allocator_t = allocator_type_;
    using sealed_writes_t = strings_sealed_writes<allocator_t>;
    using stamped_delta_t = stamped_delta<stored_key_t>;
    using stamps_t =
        st::immutable_packed_array<std::uint64_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    strings_memory_run() noexcept = default;
    strings_memory_run(strings_memory_run&&) noexcept = default;
    strings_memory_run& operator=(strings_memory_run&&) noexcept = default;
    strings_memory_run(strings_memory_run const&) = delete;
    strings_memory_run& operator=(strings_memory_run const&) = delete;

    /**
     *  @brief Stamps every record of @p writes with the commit's @p stamp, which only the leader ordering it draws.
     *  @retval invalid_argument_k An unwritten stamp, which no commit carries.
     */
    static expected<strings_memory_run> stamped(sealed_writes_t&& writes, transaction_stamp_t stamp,
                                                allocator_t allocator = {}) noexcept {
        if (stamp == unwritten_stamp_k)
            return status_t {status_code_t::invalid_argument_k, "No commit carries the unwritten stamp"};
        st::expected<stamps_t> stamps = stamps_t::make(writes.size(), stamp, typename stamps_t::allocator_t(allocator));
        if (!stamps) return status_t {status_code_t::out_of_memory_k, "No room to stamp a strings write set"};
        strings_memory_run run;
        run.writes_ = std::move(writes);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = stamp;
        return run;
    }

    /**
     *  @brief The run one merge makes of @p versions, which come in string order with the versions of a string newest
     *      first, for runs of one generation it replaces.
     *  @param[in] oldest_stamp The oldest stamp of the runs it replaces, which the run keeps covering where that
     *      version did not survive, so neighbouring runs still never overlap.
     *  @param[in] generation The generation of the runs it replaces, which the run is one past.
     */
    static expected<strings_memory_run> merged(std::span<stamped_delta_t const> versions,
                                               transaction_stamp_t oldest_stamp, std::uint32_t generation,
                                               allocator_t allocator = {}) noexcept {
        transaction_stamp_t newest_stamp = oldest_stamp;
        for (stamped_delta_t const& version : versions) newest_stamp = std::max(newest_stamp, version.stamp);
        auto [writes, merged_status] = sealed_writes_t::merged(versions, allocator);
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
        strings_memory_run run;
        run.writes_ = std::move(writes);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = newest_stamp;
        run.generation_ = generation + 1;
        return run;
    }

    /** How many merges built the run: zero for one commit's, and one past the runs a merge replaced. */
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

    /** Records the run holds, every version counted. */
    [[nodiscard]] std::size_t size() const noexcept { return writes_.size(); }

    /** Bytes every record's string takes together. */
    [[nodiscard]] std::size_t keys_bytes() const noexcept { return writes_.keys_bytes(); }

    /** Bytes every record's value takes together. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return writes_.values_bytes(); }

    /** The string at sorted @p position. */
    [[nodiscard]] std::string_view stored_key(std::size_t position) const noexcept {
        return writes_.stored_key(position);
    }

    /** The head of the string at sorted @p position. */
    [[nodiscard]] ustore_key_t const& head(std::size_t position) const noexcept { return writes_.head(position); }

    /** What the record at sorted @p position does and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept { return writes_.delta(position); }

    /** The stamp of the commit that wrote the record at sorted @p position. */
    [[nodiscard]] transaction_stamp_t stamp(std::size_t position) const noexcept { return stamps_[position]; }

    /** The first sorted position whose string is not below @p text, or @c size when every string is. */
    [[nodiscard]] std::size_t lower_bound(std::string_view text) const noexcept { return writes_.lower_bound(text); }

    /** The first sorted position past every string starting with @p prefix. */
    [[nodiscard]] std::size_t prefix_end(std::string_view prefix) const noexcept { return writes_.prefix_end(prefix); }

    /** The newest stamp any record carries. */
    [[nodiscard]] transaction_stamp_t newest_stamp() const noexcept { return newest_stamp_; }

    /** The oldest stamp any record carries, which the stamps column is based at. */
    [[nodiscard]] transaction_stamp_t oldest_stamp() const noexcept { return stamps_.base(); }

  private:

    /** The sorted heads, strings, offsets, bytes and kinds. */
    sealed_writes_t writes_;

    /** The stamp of every record in sorted order. */
    stamps_t stamps_;

    /** The newest stamp in @c stamps_, which a check of what changed since a stamp skips the run by. */
    transaction_stamp_t newest_stamp_ = unwritten_stamp_k;

    /** How many merges built the run. */
    std::uint32_t generation_ = 0;
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
class strings_cursor {
  public:
    using view_t = view_type_;
    using run_t = typename view_t::run_t;
    using allocator_t = allocator_type_;

    strings_cursor() noexcept = default;
    strings_cursor(strings_cursor&&) noexcept = default;
    strings_cursor& operator=(strings_cursor&&) noexcept = default;
    strings_cursor(strings_cursor const&) = delete;
    strings_cursor& operator=(strings_cursor const&) = delete;

    /**
     *  @brief A cursor over @p view at stamp @p at, listing the strings starting with @p prefix that are not below
     *      @p start, where an empty prefix admits every string.
     *  @retval out_of_memory_k The positions in each run could not be allocated.
     */
    static expected<strings_cursor> over(view_t const& view, std::string_view prefix, std::string_view start,
                                         transaction_stamp_t at, allocator_t allocator = {}) noexcept {
        strings_cursor cursor {view, at, allocator};
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

            ustore_key_t const head = runs[smallest]->head(positions_[smallest]);
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

    strings_cursor(view_t const& view, transaction_stamp_t at, allocator_t const& allocator) noexcept
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
