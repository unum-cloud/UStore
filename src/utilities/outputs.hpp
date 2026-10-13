/**
 *  @file src/utilities/outputs.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief The output columns every engine writes into the arena of a call, each named for the verb whose outputs it
 *      holds, growing in scratch where their count is known only at the end.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/ranges.hpp" // `bits_span_t`, `bytes_of`
#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/types.hpp"  // `value_view_t`, `string_of`, `round_up_to_multiple`

#include "utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `growing_strings`, `export_strings`

namespace unum::ustore::utilities {

/** The columns of a batched read in its arena: presences and lengths sized by the batch, then the values, copied in
 *  once all are found and their length is known, so the arena holds exactly the outputs. A value appended must stay
 *  where it lies until @c finish copies it. */
class read_outputs_t {
    ustore_arena_t arena_ = nullptr;
    bits_span_t presences_ {};
    ustore_length_t* lengths_ = nullptr;
    value_view_t* found_ = nullptr;
    std::size_t index_ = 0;
    std::size_t length_ = 0;

  public:

    /** Opens the presences and lengths @p arguments ask for in their arena, and in @p scratch a view of every value
     *  when they ask for the values. */
    template <typename arguments_type_>
    static expected<read_outputs_t> make(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
        std::size_t const count = arguments.count;
        auto [presences, presences_status] = arena_allocate_bits(arguments.arena, arguments.presences ? count : 0);
        if (presences_status.failed()) return presences_status;
        auto [lengths, lengths_status] = arena_allocate<ustore_length_t>(arguments.arena,
                                                                         arguments.lengths ? count : 0);
        if (lengths_status.failed()) return lengths_status;
        auto [found, found_status] = arena_allocate<value_view_t>(scratch, arguments.values ? count : 0);
        if (found_status.failed()) return found_status;
        read_outputs_t outputs;
        outputs.arena_ = arguments.arena;
        outputs.presences_ = presences.data();
        outputs.lengths_ = lengths.data();
        outputs.found_ = found.data();
        return outputs;
    }

    /** Appends a found value, which stays where it lies until @c finish copies it. */
    void append(value_view_t value) noexcept {
        if (presences_) presences_[index_] = true;
        if (lengths_) lengths_[index_] = static_cast<ustore_length_t>(value.size());
        if (found_) found_[index_] = value;
        length_ += value.size();
        ++index_;
    }

    /** Appends a missing value, which takes no bytes. */
    void append_missing() noexcept {
        if (lengths_) lengths_[index_] = 0;
        if (found_) found_[index_] = {};
        ++index_;
    }

    /** Copies every value into the arena at once, behind offsets as narrow as their bytes allow, and exports every
     *  column into the outputs of @p arguments. */
    template <typename arguments_type_>
    status_t finish(arguments_type_ const& arguments) noexcept {
        if (arguments.values) {
            auto const values = [&](std::size_t index) noexcept { return found_[index].bytes(); };
            if (status_t const status = export_strings(arena_, index_, length_, values, *arguments.values);
                status.failed())
                return status;
        }
        if (arguments.presences) *arguments.presences = presences_.get();
        if (arguments.lengths) *arguments.lengths = lengths_;
        return {};
    }
};

/** The columns of batched scans and samples: an offset per task in their arena, then the keys, which grow in scratch
 *  and are copied in once their count is known. */
class scan_outputs_t {
    std::span<ustore_length_t> offsets_ {};
    st::basic_vector<ustore_key_t, arena_allocator<ustore_key_t>> keys_ {};

  public:

    /** Opens the offsets of every task of @p arguments in their arena, and the keys in @p scratch. */
    template <typename arguments_type_>
    static expected<scan_outputs_t> make(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
        auto [offsets, status] = arena_allocate<ustore_length_t>(arguments.arena, arguments.count + 1);
        if (status.failed()) return status;
        scan_outputs_t outputs;
        outputs.offsets_ = offsets;
        outputs.keys_ = st::basic_vector<ustore_key_t, arena_allocator<ustore_key_t>> {{scratch}};
        return outputs;
    }

    /** How many keys the outputs hold. */
    std::size_t size() const noexcept { return keys_.size(); }

    /** The key at @p index. */
    ustore_key_t& operator[](std::size_t index) noexcept { return keys_[index]; }

    /** Appends @p key, failing when the scratch refuses. */
    status_t push(ustore_key_t const& key) noexcept { return growth_status(keys_.emplace_back(key)); }

    /** Records that task @p task starts where the keys end. */
    void open_task(std::size_t task) noexcept { offsets_[task] = static_cast<ustore_length_t>(size()); }

    /** Closes the offsets, copies the keys into the arena of @p arguments and exports both. */
    template <typename arguments_type_>
    status_t finish(arguments_type_ const& arguments) noexcept {
        auto [keys, status] = arena_copy<ustore_key_t>(arguments.arena, keys_);
        if (status.failed()) return status;
        offsets_.back() = static_cast<ustore_length_t>(size());
        *arguments.offsets = offsets_.data();
        *arguments.keys = keys.data();
        return {};
    }
};

/** The columns of batched strings scans: an offset per task in their arena, then the found strings, which grow in
 *  scratch and are copied in once, as narrow as their bytes allow. */
class strings_scan_outputs_t {
    std::span<ustore_length_t> tasks_offsets_ {};
    growing_strings<arena_allocator<byte_t>> strings_ {};

  public:

    /** Opens the offsets of every task of @p arguments in their arena, and the strings in @p scratch. */
    static expected<strings_scan_outputs_t> make(ustore_strings_scan_t const& arguments,
                                                 ustore_arena_t scratch) noexcept {
        auto [tasks_offsets, status] = arena_allocate<ustore_length_t>(arguments.arena, arguments.count + 1);
        if (status.failed()) return status;
        strings_scan_outputs_t outputs;
        outputs.tasks_offsets_ = tasks_offsets;
        outputs.strings_ = growing_strings<arena_allocator<byte_t>> {scratch};
        return outputs;
    }

    /** How many strings the outputs hold. */
    std::size_t size() const noexcept { return strings_.size(); }

    /** The string at @p index. */
    std::string_view operator[](std::size_t index) const noexcept { return string_of(strings_[index]); }

    /** Appends @p text, failing when the scratch refuses. */
    status_t push(std::string_view text) noexcept { return strings_.push_back(bytes_of(text)); }

    /** Records that task @p task starts where the strings end. */
    void open_task(std::size_t task) noexcept { tasks_offsets_[task] = static_cast<ustore_length_t>(size()); }

    /** Closes the offsets of the tasks, copies the strings into the arena of @p arguments as a tape and exports both
     *  columns. */
    status_t finish(ustore_strings_scan_t const& arguments) noexcept {
        if (status_t const status = strings_.export_into(arguments.arena, *arguments.strings); status.failed())
            return status;
        tasks_offsets_.back() = static_cast<ustore_length_t>(size());
        *arguments.offsets = tasks_offsets_.data();
        return {};
    }
};

/** The columns of a collection listing, which grow in scratch while the catalog is walked and are copied into the
 *  arena of the call once: identifiers, modalities, and the names end to end after their offsets. */
class collection_list_outputs_t {
    st::basic_vector<ustore_collection_id_t, arena_allocator<ustore_collection_id_t>> ids_;
    st::basic_vector<ustore_modality_t, arena_allocator<ustore_modality_t>> modalities_;
    growing_strings<arena_allocator<byte_t>> names_;

  public:
    explicit collection_list_outputs_t(ustore_arena_t scratch) noexcept
        : ids_ {{scratch}}, modalities_ {{scratch}}, names_ {scratch} {}

    /** Appends collection @p id holding @p modality under @p name, empty for a collection without one. */
    status_t push(ustore_collection_id_t id, ustore_modality_t modality, value_view_t name) noexcept {
        if (status_t const status = growth_status(ids_.emplace_back(id)); status.failed()) return status;
        if (status_t const status = growth_status(modalities_.emplace_back(modality)); status.failed()) return status;
        return names_.push_back(name.bytes());
    }

    /** Copies the columns @p arguments ask for into their arena and exports them with the count written. */
    status_t finish(ustore_collection_list_t const& arguments) noexcept {
        std::size_t const count = ids_.size();
        auto [ids, ids_status] = arena_copy<ustore_collection_id_t>(arguments.arena,
                                                                    {ids_.data(), arguments.collections ? count : 0});
        if (ids_status.failed()) return ids_status;
        auto [modalities, modalities_status] = arena_copy<ustore_modality_t>(
            arguments.arena, {modalities_.data(), arguments.modalities ? count : 0});
        if (modalities_status.failed()) return modalities_status;
        if (arguments.names)
            if (status_t const status = names_.export_into(arguments.arena, *arguments.names); status.failed())
                return status;
        *arguments.written = static_cast<ustore_size_t>(count);
        if (arguments.collections) *arguments.collections = ids.data();
        if (arguments.modalities) *arguments.modalities = modalities.data();
        return {};
    }
};

/** The six bounds of a batched measurement in its arena, every column holding one bound per range. */
struct measure_outputs_t {
    ustore_size_t* min_cardinalities = nullptr;
    ustore_size_t* max_cardinalities = nullptr;
    ustore_size_t* min_value_bytes = nullptr;
    ustore_size_t* max_value_bytes = nullptr;
    ustore_size_t* min_space_usages = nullptr;
    ustore_size_t* max_space_usages = nullptr;

    /** Opens the six columns of @p arguments in their arena as one block, column after column, each starting aligned
     *  as every output does. */
    static expected<measure_outputs_t> make(ustore_blobs_measure_t const& arguments) noexcept {
        std::size_t const stride = round_up_to_multiple(static_cast<std::size_t>(arguments.count),
                                                        arena_alignment_k / sizeof(ustore_size_t));
        auto [bounds, status] = arena_allocate<ustore_size_t>(arguments.arena, 6 * stride);
        if (status.failed()) return status;
        ustore_size_t* const first = bounds.data();
        return measure_outputs_t {
            first, first + stride, first + 2 * stride, first + 3 * stride, first + 4 * stride, first + 5 * stride};
    }

    /** Exports every column @p arguments ask for. */
    void finish(ustore_blobs_measure_t const& arguments) const noexcept {
        if (arguments.min_cardinalities) *arguments.min_cardinalities = min_cardinalities;
        if (arguments.max_cardinalities) *arguments.max_cardinalities = max_cardinalities;
        if (arguments.min_value_bytes) *arguments.min_value_bytes = min_value_bytes;
        if (arguments.max_value_bytes) *arguments.max_value_bytes = max_value_bytes;
        if (arguments.min_space_usages) *arguments.min_space_usages = min_space_usages;
        if (arguments.max_space_usages) *arguments.max_space_usages = max_space_usages;
    }
};

/** The columns of a batch of collection names resolved in its arena: presences, identifiers and modalities. */
struct collection_find_outputs_t {
    bits_span_t presences {};
    ustore_collection_id_t* ids = nullptr;
    ustore_modality_t* modalities = nullptr;

    /** Opens the columns @p arguments ask for in their arena, where the identifiers are never optional. */
    static expected<collection_find_outputs_t> make(ustore_collection_find_t const& arguments) noexcept {
        std::size_t const count = arguments.count;
        auto [presences, presences_status] = arena_allocate_bits(arguments.arena, arguments.presences ? count : 0);
        if (presences_status.failed()) return presences_status;
        auto [ids, ids_status] = arena_allocate<ustore_collection_id_t>(arguments.arena, count);
        if (ids_status.failed()) return ids_status;
        auto [modalities, modalities_status] = arena_allocate<ustore_modality_t>(arguments.arena,
                                                                                 arguments.modalities ? count : 0);
        if (modalities_status.failed()) return modalities_status;
        return collection_find_outputs_t {presences.data(), ids.data(), modalities.data()};
    }

    /** Records that name @p index addresses @p id holding @p modality, which a missing name answers as the main one. */
    void assign(std::size_t index, ustore_collection_id_t id, ustore_modality_t modality, bool present) noexcept {
        ids[index] = id;
        if (modalities) modalities[index] = modality;
        if (presences) presences[index] = present;
    }

    /** Exports every column @p arguments ask for. */
    void finish(ustore_collection_find_t const& arguments) const noexcept {
        *arguments.collections = ids;
        if (arguments.presences) *arguments.presences = presences.get();
        if (arguments.modalities) *arguments.modalities = modalities;
    }
};

} // namespace unum::ustore::utilities
