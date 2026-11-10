/**
 *  @file src/utilities/outputs.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief The output columns every engine writes into the arena of a call, each named for the verb whose outputs it
 *      holds, growing in scratch where their count is known only at the end.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::type_identity`, `std::is_same_v`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/ranges.hpp" // `bits_span_t`, `bytes_of`
#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/types.hpp"  // `value_view_t`, `string_of`, `round_up_to_multiple`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `growing_strings`, `export_strings`

namespace unum::ustore::utilities {

/** The bytes of a value one task of a read asks for: from an offset, at most a limit. */
struct task_window_t {
    std::size_t offset = 0;
    std::size_t length = 0;
};

/** The columns of a batched read in its arena: presences and lengths sized by the batch, then the
 *  values, copied in once all are found and their length is known, so the arena holds exactly the
 *  outputs. A value appended must stay where it lies until @c finish copies it, and reads as the
 *  byte range a blobs read names for it. An engine that reads every window straight into one room
 *  of the arena, end to end in task order, has @c finish adopt that room and copy nothing. */
class read_outputs_t {
    ustore_arena_t arena_ = nullptr;
    bits_span_t presences_ {};
    ustore_length_t* lengths_ = nullptr;
    value_view_t* found_ = nullptr;
    strided_iterator<ustore_length_t const> offsets_ {};
    strided_iterator<ustore_length_t const> limits_ {};
    std::span<byte_t> adopted_ {};
    std::size_t index_ = 0;
    std::size_t length_ = 0;

    /** The bytes of @p value that task @p task reads: from its offset, at most its limit. */
    value_view_t ranged_(std::size_t task, value_view_t value) const noexcept {
        task_window_t const ranged = window(task, value.size());
        return value_view_t {value.data() + ranged.offset, ranged.length};
    }

    /** Exports the offsets of the values lying end to end in @c adopted_, as narrow as their bytes
     *  allow. */
    status_t export_adopted_(ustore_strings_view_t& tape) const noexcept {
        auto const export_as = [&]<typename offset_type_>(std::type_identity<offset_type_>) noexcept {
            auto [offsets, status] = arena_allocate<offset_type_>(arena_, index_ + 1);
            if (status.failed()) return status;
            offsets[0] = 0;
            for (std::size_t index = 0; index != index_; ++index)
                offsets[index + 1] = static_cast<offset_type_>(offsets[index] + found_[index].size());
            if constexpr (std::is_same_v<offset_type_, std::uint32_t>) tape = u32_tape(adopted_.data(), offsets.data());
            else tape = u64_tape(adopted_.data(), offsets.data());
            return status_t {};
        };
        if (length_ <= (std::numeric_limits<std::uint32_t>::max)())
            return export_as(std::type_identity<std::uint32_t> {});
        return export_as(std::type_identity<std::uint64_t> {});
    }

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
        if constexpr (requires { arguments.limits; }) {
            outputs.offsets_ = strided(arguments.offsets);
            outputs.limits_ = strided(arguments.limits);
        }
        return outputs;
    }

    /** The arena the outputs land in. */
    ustore_arena_t arena() const noexcept { return arena_; }

    /** The bytes of a value of @p size that task @p task reads: from its offset, at most its
     *  limit. */
    task_window_t window(std::size_t task, std::size_t size) const noexcept {
        auto const signed_task = static_cast<std::ptrdiff_t>(task);
        std::size_t offset = offsets_ ? static_cast<std::size_t>(offsets_[signed_task]) : 0;
        if (offset > size) offset = size;
        std::size_t const rest = size - offset;
        std::size_t const length = limits_ && limits_[signed_task] < rest
                                       ? static_cast<std::size_t>(limits_[signed_task])
                                       : rest;
        return {offset, length};
    }

    /** Takes @p room of the arena, where the windows appended next through @c append_adopted lie
     *  end to end, as the values @c finish exports without copying them. */
    void adopt(std::span<byte_t> room) noexcept { adopted_ = room; }

    /** Appends a found value whose window of @p length bytes lies next in the adopted room. */
    void append_adopted(std::size_t length) noexcept {
        if (presences_) presences_[index_] = true;
        if (lengths_) lengths_[index_] = static_cast<ustore_length_t>(length);
        if (found_) found_[index_] = value_view_t {adopted_.data() + length_, length};
        length_ += length;
        ++index_;
    }

    /** Appends a found value, which stays where it lies until @c finish copies it. */
    void append(value_view_t value) noexcept {
        if (offsets_ || limits_) value = ranged_(index_, value);
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

    /** Places the value found for task @p task, as @c append does, for slices of a split batch. */
    void assign(std::size_t task, value_view_t value) noexcept {
        if (offsets_ || limits_) value = ranged_(task, value);
        if (presences_) presences_[task] = true;
        if (lengths_) lengths_[task] = static_cast<ustore_length_t>(value.size());
        if (found_) found_[task] = value;
    }

    /** Places a missing value for task @p task, as @c append_missing does. */
    void assign_missing(std::size_t task) noexcept {
        if (lengths_) lengths_[task] = 0;
        if (found_) found_[task] = {};
    }

    /** Counts the first @p count tasks placed, once every slice placing them returned. */
    void assigned(std::size_t count) noexcept {
        index_ = count;
        length_ = 0;
        for (std::size_t task = 0; found_ && task != count; ++task) length_ += found_[task].size();
    }

    /** Copies every value into the arena at once, unless it lies there already, behind offsets as
     *  narrow as their bytes allow, and exports every column into the outputs of @p arguments. */
    template <typename arguments_type_>
    status_t finish(arguments_type_ const& arguments) noexcept {
        if (arguments.values && adopted_.data()) {
            if (status_t const status = export_adopted_(*arguments.values); status.failed()) return status;
        }
        else if (arguments.values) {
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

/** The columns of a batched strings read in its arena: presences and the key every string maps onto, both sized by
 *  the batch, so every key lands in place as it is found and nothing waits for @c finish. */
class strings_read_outputs_t {
    bits_span_t presences_ {};
    ustore_key_t* keys_ = nullptr;

  public:

    /** Opens the presences and keys @p arguments ask for in their arena. */
    static expected<strings_read_outputs_t> make(ustore_strings_read_t const& arguments) noexcept {
        std::size_t const count = arguments.count;
        auto [presences, presences_status] = arena_allocate_bits(arguments.arena, arguments.presences ? count : 0);
        if (presences_status.failed()) return presences_status;
        auto [keys, keys_status] = arena_allocate<ustore_key_t>(arguments.arena, arguments.values ? count : 0);
        if (keys_status.failed()) return keys_status;
        strings_read_outputs_t outputs;
        outputs.presences_ = presences.data();
        outputs.keys_ = keys.data();
        return outputs;
    }

    /** Places the key the present string of task @p task maps onto. */
    void assign_present(std::size_t task, ustore_key_t key) noexcept {
        if (presences_) presences_[task] = true;
        if (keys_) keys_[task] = key;
    }

    /** Places the missing string of task @p task, which reads as the nil UUID. */
    void assign_missing(std::size_t task) noexcept {
        if (keys_) keys_[task] = ustore_key_t {};
    }

    /** Exports every column into the outputs of @p arguments. */
    void finish(ustore_strings_read_t const& arguments) const noexcept {
        if (arguments.presences) *arguments.presences = presences_.get();
        if (arguments.values) *arguments.values = keys_;
    }
};

struct scan_slice_t;

/** The columns of batched scans and samples: an offset per task in their arena, then the keys, which grow in scratch
 *  and are copied in once their count is known. */
class scan_outputs_t {
    std::span<ustore_length_t> offsets_ {};
    st::vector<ustore_key_t, arena_allocator<ustore_key_t>> keys_ {};

  public:

    /** Opens the offsets of every task of @p arguments in their arena, and the keys in @p scratch. */
    template <typename arguments_type_>
    static expected<scan_outputs_t> make(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
        auto [offsets, status] = arena_allocate<ustore_length_t>(arguments.arena, arguments.count + 1);
        if (status.failed()) return status;
        scan_outputs_t outputs;
        outputs.offsets_ = offsets;
        outputs.keys_ = st::vector<ustore_key_t, arena_allocator<ustore_key_t>> {{scratch}};
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

    /** The outputs one slice of a split scan fills: offsets of its tasks land in this column, local
     *  to the slice until @c finish rebases them, and its keys grow in @p scratch. */
    scan_outputs_t slice_in(ustore_arena_t scratch) const noexcept {
        scan_outputs_t slice;
        slice.offsets_ = offsets_;
        slice.keys_ = st::vector<ustore_key_t, arena_allocator<ustore_key_t>> {{scratch}};
        return slice;
    }

    /** Closes the offsets of a scan split into @p slices, rebasing each run of tasks past the keys
     *  of the runs before it, and copies every key into the arena of @p arguments once. */
    template <typename arguments_type_>
    status_t finish(arguments_type_ const& arguments, std::span<scan_slice_t const> slices) noexcept;
};

/** One slice of a split scan: the run of tasks it filled, and its outputs. */
struct scan_slice_t {
    std::size_t first = 0;
    std::size_t length = 0;
    scan_outputs_t outputs;
};

template <typename arguments_type_>
status_t scan_outputs_t::finish(arguments_type_ const& arguments, std::span<scan_slice_t const> slices) noexcept {
    std::size_t total = 0;
    for (scan_slice_t const& slice : slices) total += slice.outputs.size();
    auto [keys, status] = arena_allocate<ustore_key_t>(arguments.arena, total);
    if (status.failed()) return status;
    std::size_t base = 0;
    for (scan_slice_t const& slice : slices) {
        for (std::size_t task = slice.first; task != slice.first + slice.length; ++task)
            offsets_[task] += static_cast<ustore_length_t>(base);
        if (slice.outputs.size())
            std::memcpy(keys.data() + base, slice.outputs.keys_.data(), slice.outputs.size() * sizeof(ustore_key_t));
        base += slice.outputs.size();
    }
    offsets_.back() = static_cast<ustore_length_t>(total);
    *arguments.offsets = offsets_.data();
    *arguments.keys = keys.data();
    return {};
}

/** The columns of batched strings scans: an offset per task in their arena, then the found strings, which grow in
 *  scratch and are copied in once, as narrow as their bytes allow. */
class strings_scan_outputs_t {
    std::span<ustore_length_t> tasks_offsets_ {};
    growing_strings<arena_allocator<byte_t>> strings_ {};

  public:

    /** Opens the offsets of every task of @p arguments in their arena, and the strings in @p scratch. */
    template <typename arguments_type_>
    static expected<strings_scan_outputs_t> make(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
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
    template <typename arguments_type_>
    status_t finish(arguments_type_ const& arguments) noexcept {
        if (status_t const status = strings_.export_into(arguments.arena, *arguments.strings); status.failed())
            return status;
        tasks_offsets_.back() = static_cast<ustore_length_t>(size());
        *arguments.offsets = tasks_offsets_.data();
        return {};
    }

    /** The outputs one slice of a split scan fills: offsets of its tasks land in this column, local
     *  to the slice until @c adopt rebases them, and its strings grow in @p scratch. */
    strings_scan_outputs_t slice_in(ustore_arena_t scratch) const noexcept {
        strings_scan_outputs_t slice;
        slice.tasks_offsets_ = tasks_offsets_;
        slice.strings_ = growing_strings<arena_allocator<byte_t>> {scratch};
        return slice;
    }

    /** Appends the strings of @p slice, which filled tasks [ @p first, @p first + @p length ),
     *  rebasing their offsets past the strings already here, as slices merge in task order. */
    status_t adopt(strings_scan_outputs_t const& slice, std::size_t first, std::size_t length) noexcept {
        std::size_t const base = size();
        for (std::size_t task = first; task != first + length; ++task)
            tasks_offsets_[task] += static_cast<ustore_length_t>(base);
        for (std::size_t index = 0; index != slice.size(); ++index)
            if (status_t const pushed = push(slice[index]); pushed.failed()) return pushed;
        return {};
    }
};

/** One slice of a split strings scan: the run of tasks it filled, and its outputs. */
struct strings_scan_slice_t {
    std::size_t first = 0;
    std::size_t length = 0;
    strings_scan_outputs_t outputs;
};

/** The columns of a collection listing, which grow in scratch while the collections system collection is walked and
 *  are copied into the arena of the call once: identifiers, modalities, and names end to end after their offsets. */
class collection_list_outputs_t {
    st::vector<ustore_collection_id_t, arena_allocator<ustore_collection_id_t>> ids_;
    st::vector<ustore_modality_t, arena_allocator<ustore_modality_t>> modalities_;
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
    template <typename arguments_type_>
    static expected<measure_outputs_t> make(arguments_type_ const& arguments) noexcept {
        std::size_t const stride = round_up_to_multiple(static_cast<std::size_t>(arguments.count),
                                                        arena_alignment_k / sizeof(ustore_size_t));
        auto [bounds, status] = arena_allocate<ustore_size_t>(arguments.arena, 6 * stride);
        if (status.failed()) return status;
        ustore_size_t* const first = bounds.data();
        return measure_outputs_t {
            first, first + stride, first + 2 * stride, first + 3 * stride, first + 4 * stride, first + 5 * stride};
    }

    /** Records the exact measure of range @p task: @p cardinality entries holding @p value_bytes in
     *  @p space_bytes of the store, which only a compaction could lower. */
    void assign(std::size_t task, ustore_size_t cardinality, ustore_size_t value_bytes,
                ustore_size_t space_bytes) noexcept {
        min_cardinalities[task] = cardinality;
        max_cardinalities[task] = cardinality;
        min_value_bytes[task] = value_bytes;
        max_value_bytes[task] = value_bytes;
        min_space_usages[task] = space_bytes;
        max_space_usages[task] = (std::numeric_limits<ustore_size_t>::max)();
    }

    /** Exports every column @p arguments ask for. */
    template <typename arguments_type_>
    void finish(arguments_type_ const& arguments) const noexcept {
        if (arguments.min_cardinalities) *arguments.min_cardinalities = min_cardinalities;
        if (arguments.max_cardinalities) *arguments.max_cardinalities = max_cardinalities;
        if (arguments.min_value_bytes) *arguments.min_value_bytes = min_value_bytes;
        if (arguments.max_value_bytes) *arguments.max_value_bytes = max_value_bytes;
        if (arguments.min_space_usages) *arguments.min_space_usages = min_space_usages;
        if (arguments.max_space_usages) *arguments.max_space_usages = max_space_usages;
    }
};

/** Fills the description @p arguments ask for with @p description, refusing a call that
 *  asks for none. */
inline status_t assign_describe(ustore_database_describe_t const& arguments,
                                ustore_database_description_t const& description) noexcept {
    if (!arguments.description) return {status_code_t::invalid_argument_k, "Need an output for the description"};
    *arguments.description = description;
    return {};
}

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
