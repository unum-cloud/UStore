/**
 *  @file src/nvme/single_collection_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One collection with transactions, snapshots and a schema of its own, over one volume.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection, the
 *  registry that orders the commits landing in it, and two blobs on the volume - the schema a reopen
 *  checks itself against, and whatever bytes the owner keeps beside the data. A graph is two of these.
 *
 *  The volume and the lanes are borrowed rather than reached for. Reads take the caller's lane, so any
 *  number of threads read at once; writes take the two lanes the database was opened with, because the
 *  registry commits through calls that carry no lane. A lease belongs to one thread at a time, so the
 *  writer that wins the write path adopts both lanes before it writes through them.
 *
 *  That write path is the one claim here, and it is not lock-free. The collection under it is a
 *  single-writer structure - a staged write mutates a hash map, an arena and a value cache that
 *  readers probe - so a writer must exclude readers or every commit is a data race. It is one word,
 *  held as a reader count or as a single writer, and it is the only exclusion in this file.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <atomic>      // `std::memory_order`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <thread>      // `std::this_thread::yield`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::forward`, `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/buffer_pool.hpp"                            // `buffer_lease_t`
#include "nvme/catalog.hpp"                                // `blob_kind_t`, `blob_name_t`
#include "nvme/collection.hpp"                             // `collection`
#include "nvme/collection_scanner.hpp"                     // `collection_scanner`
#include "nvme/collection_snapshot.hpp"                    // `select_result_t`
#include "nvme/config.hpp"                                 // `single_collection_database_config_t`
#include "nvme/extent.hpp"                                 // `extent_t`, `rounded_up_to`, `portable_alignment_k`
#include "nvme/io_lane.hpp"                                // `io_lane_t`
#include "nvme/io_pipeline.hpp"                            // `basic_read_pipeline`, `basic_write_pipeline`
#include "nvme/lsm_tree.hpp"                               // `lsm_tree_run_record_t`, `lsm_tree_publication_hook_t`
#include "nvme/read_set.hpp"                               // `transaction_stamp_t`
#include "nvme/resources.hpp"                              // `resources_shared_ptr_t`
#include "nvme/single_collection_database_snapshot.hpp"    // `single_collection_database_snapshot`
#include "nvme/single_collection_database_transaction.hpp" // `single_collection_database_transaction`
#include "nvme/stored_value.hpp"                           // `stored_value_size_t`
#include "nvme/types.hpp"                                  // `value_layout_t`, `is_inline`, `is_fixed`
#include "nvme/volume.hpp"                                 // `blob_t`
#include "nvme/write_buffer.hpp"                           // `write_buffer`, `tombstone_offset_k`

namespace unum::ustore::nvme {

#pragma region Schema

/** The eight bytes that tell the schema blob of this layer from every other blob of a volume. */
inline constexpr std::uint64_t single_collection_database_magic_k = 0x5343444231303031ull;

/** The layout of the schema record, of which a mount reads exactly one version. */
inline constexpr std::uint32_t single_collection_database_format_version_k = 2;

/**
 *  @brief What a single-collection database writes into its schema blob, and a reopen checks against.
 *
 *  Fixed width and trivially copyable, so the record is the bytes: the volume already carries the
 *  database identity in its own head, and what is left is the shape a reopen must not contradict.
 *  The run records the blob carries after this one are what a reopen puts back into the tree.
 */
struct single_collection_database_schema_record_t {

    /** The magic every schema blob of this layer opens with. */
    std::uint64_t magic = single_collection_database_magic_k;

    /** The layout version this record follows. */
    std::uint32_t format_version = single_collection_database_format_version_k;

    /** The @c value_layout_t the collection was created under. */
    std::uint16_t value_layout = 0;

    /** Written as zero, so the record is the same bytes on every compiler that packs it. */
    std::uint16_t padding = 0;

    /** Bytes the largest value the collection admits takes. */
    std::uint64_t value_max_size = 0;

    /** Runs the blob names after this record, in the order their levels hold them. */
    std::uint64_t run_count = 0;
};

static_assert(sizeof(single_collection_database_schema_record_t) == 32, "The schema record is its own bytes");

#pragma endregion Schema

#pragma region Database

/**
 *  @brief One collection, its transactions, its snapshots and its two blobs, over one volume.
 *
 *  Pinned rather than movable: the registry and every live transaction point back here, so a database
 *  is opened into a @c std::unique_ptr and stays where it was opened.
 */
template <typename key_slot_type_, typename volume_type_>
class basic_single_collection_database {
  public:

#pragma region Types

    /** The key slot every run of this database holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run and every blob of this database lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The collection every write of this database lands in. */
    using collection_t = collection<key_slot_t, volume_t>;

    /** An ordered walk over that collection, staged writes merged in front of every run. */
    using scanner_t = collection_scanner<key_slot_t, volume_t>;

    /** One moment of this database, which a reader holds and a publication waits behind. */
    using snapshot_t = single_collection_database_snapshot<key_slot_t, volume_t>;

    /** One optimistic transaction over this database. */
    using transaction_t = single_collection_database_transaction<stored_key_t, basic_single_collection_database>;

    /** The order every transaction of this database is placed in, and its conflicts. */
    using registry_t = single_collection_database_transaction_registry<stored_key_t, basic_single_collection_database>;

    /** The uncommitted changes of one transaction, as the registry hands them over. */
    using write_buffer_t = write_buffer<stored_key_t>;

    /** A database, which is pinned and therefore always owned through a pointer. */
    using database_unique_ptr_t = std::unique_ptr<basic_single_collection_database>;

    /** A transaction, which is owned by whoever created it and retires when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** How the values of this database are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

#pragma endregion Types

#pragma region Construction

    basic_single_collection_database(basic_single_collection_database const&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database const&) = delete;
    basic_single_collection_database(basic_single_collection_database&&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database&&) = delete;

    /** Writes whatever is still staged onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        write_path_guard_t const held {*this};
        [[maybe_unused]] status_t const flushed = collection_.flush(writing_lane_);
        // A destroyed database dropped its schema, and a close must not write one back over an empty volume.
        if (!volume_.names(schema_blob_name_())) return;
        [[maybe_unused]] status_t const saved = save_schema_();
    }

    /**
     *  @brief Opens a database over @p volume, creating its schema blob when the volume holds none.
     *  @param[inout] volume The volume every run and blob lands on, borrowed and outliving the database.
     *  @param[inout] writing_lane The lane every write, flush and commit runs on, borrowed for the database's life.
     *  @param[inout] merging_lane The second lane a compaction writes its new runs on, which is not @p writing_lane.
     *  @param[in] config The collection's bounds and the transaction defaults.
     *  @param[in] resources The budget the conflict record, the roster and every transaction's writes are charged to.
     *  @return The database, or @c malformed_k for a volume whose schema names another value layout.
     */
    static expected<database_unique_ptr_t> open(volume_t& volume, io_lane_t& writing_lane, io_lane_t& merging_lane,
                                                single_collection_database_config_t const& config,
                                                resources_shared_ptr_t resources) noexcept {
        if (!resources) return status_t {status_code_t::invalid_argument_k, "A database is charged to some budget"};
        if (&writing_lane == &merging_lane)
            return status_t {status_code_t::invalid_argument_k, "A compaction reads and writes on two lanes"};

        expected<collection_t> mounted = collection_t::mount(volume, config.collection, resources->memory_constraint);
        if (!mounted) return mounted.status();

        database_unique_ptr_t opened {new (std::nothrow) basic_single_collection_database {
            volume, writing_lane, merging_lane, config, std::move(resources), std::move(*mounted)}};
        if (!opened) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        if (status_t const agreed = opened->agree_on_schema_(); agreed.failed()) return agreed;
        return opened;
    }

    /** Drops both blobs and every run, which destroys the data and leaves the volume empty of it. */
    status_t destroy() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        status_t first = drop_blob_(schema_blob_name_());
        if (status_t const dropped = drop_blob_(user_data_blob_name_()); dropped.failed() && first.succeeded())
            first = dropped;
        if (status_t const emptied = collection_.destroy(writing_lane_); emptied.failed() && first.succeeded())
            first = emptied;
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this database was opened with. */
    [[nodiscard]] single_collection_database_config_t const& config() const noexcept { return config_; }

    /** The collection every write lands in, which a scan walks and a benchmark measures. */
    [[nodiscard]] collection_t const& records() const noexcept { return collection_; }

    /** Bytes the largest value this database admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return collection_.value_max_size(); }

    /** Whether a value of @p size bytes is one this database's layout can hold. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == collection_.value_max_size();
        else if constexpr (is_inline(value_layout_k)) return size != 0 && size <= collection_.value_max_size();
        else return size <= collection_.value_max_size();
    }

    /** Keys staged plus key slots every level holds, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        read_path_guard_t const held {*this};
        return collection_.population_count();
    }

    /** Bytes every run of every level takes on the volume, answered without excluding a writer. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return collection_.size_bytes(); }

    /** Writes staged and not yet flushed. */
    [[nodiscard]] std::size_t staged_count() const noexcept {
        read_path_guard_t const held {*this};
        return collection_.staged_count();
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] lane The lane the read is submitted and reaped on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_lane_t& lane, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.select(lane, stored_key, into, size);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the lane as completions retire.
     *  @param[inout] lane The lane the reads are submitted and reaped on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_lane_t& lane, std::span<stored_key_t const> stored_keys, std::span<select_result_t> results,
                    sink_type_&& sink) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.select(lane, stored_keys, results, std::forward<sink_type_>(sink));
    }

#pragma endregion Lookups

#pragma region Writes

    /** Stages @p stored_value under @p stored_key on the writing lane, flushing when the buffer is full. */
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.insert(writing_lane_, stored_key, stored_value);
    }

    /** Stages a tombstone under @p stored_key on the writing lane. */
    status_t erase(stored_key_t stored_key) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.erase(writing_lane_, stored_key);
    }

    /**
     *  @brief Stages one value per key on the writing lane, flushing when they do not all fit.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                    std::span<stored_value_size_t const> sizes) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.insert(writing_lane_, stored_keys, stored_values, sizes);
    }

    /** Stages a tombstone under every key of @p stored_keys on the writing lane. */
    status_t erase(std::span<stored_key_t const> stored_keys) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.erase(writing_lane_, stored_keys);
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write as one run of level zero, leaving the write buffer empty. */
    status_t flush() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (status_t const flushed = collection_.flush(writing_lane_); flushed.failed()) return flushed;
        // The run the flush wrote is sealed by now, so the schema may name it.
        return save_schema_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one lane and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.tree().compact(writing_lane_, merging_lane_, compaction_placement_t::automatic_k, nullptr,
                                          published);
    }

    /** Flushes, then holds the level set the flush published together with the stamp it holds it at. */
    expected<snapshot_t> take_snapshot() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        expected<typename collection_t::snapshot_t> levels = collection_.take_snapshot(writing_lane_);
        if (!levels) return levels.status();
        return snapshot_t::of(std::move(*levels), registry_.current_stamp_());
    }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction of @p id over this database, with the write-buffer bounds @p config names. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id,
                                                          transaction_config_t const& config) noexcept {
        if (config.write_buffer_max_bytes > config_.collection.write_buffer_max_bytes ||
            config.write_buffer_max_elements > config_.collection.write_buffer_max_elements)
            return status_t {status_code_t::invalid_argument_k,
                             "A transaction may not outgrow the write buffer it commits into"};

        // The write buffer takes exactly one of the two widths, and which one is the layout's own answer.
        std::size_t const exact = is_fixed(value_layout_k) ? collection_.value_max_size() : 0;
        std::size_t const bounded = is_fixed(value_layout_k) ? 0 : collection_.value_max_size();
        return registry_.create_transaction(id, config, exact, bounded);
    }

    /** A transaction of @p id with the write-buffer bounds this database was opened with. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id) noexcept {
        return create_transaction(id, config_.default_transaction);
    }

    /** Whether @p id names a transaction this database holds. */
    [[nodiscard]] bool has_transaction(transaction_id_t id) const noexcept { return registry_.has_transaction(id); }

#pragma endregion Transactions

#pragma region User Data

    /** Bytes the owner's blob takes beside the data, which is zero when the volume holds none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() const noexcept {
        expected<blob_t> const found = volume_.find(user_data_blob_name_());
        return found ? found->length : 0;
    }

    /** Replaces the owner's blob with @p bytes, which the volume keeps beside the runs. */
    status_t save_user_data(std::span<byte_t const> bytes) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (bytes.empty()) return drop_blob_(user_data_blob_name_());
        return write_blob_(user_data_blob_name_(), blob_kind_t::user_data_k, bytes);
    }

    /** Copies the owner's blob into @p into, which must be at least as long as the blob. */
    status_t load_user_data(io_lane_t& lane, std::span<byte_t> into) const noexcept {
        expected<blob_t> const found = volume_.find(user_data_blob_name_());
        if (!found) return found.status();
        if (into.size() < found->length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        return read_blob_(lane, *found, into.first(static_cast<std::size_t>(found->length)));
    }

    /** Forgets the owner's blob, which a volume holding none reports as @c not_found_k. */
    status_t clear_user_data() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return drop_blob_(user_data_blob_name_());
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /**
     *  @brief Takes the collection for one writer, waiting while another writer or a reader holds it.
     *
     *  Held from the moment a transaction stages until it commits or rolls back, so every reader sees
     *  all of a commit's writes or none of them.
     */
    void acquire_write_path() { acquire_write_path_(); }

    /** Hands the collection back to the readers and the next writer. */
    void release_write_path() { release_write_path_(); }

    /**
     *  @brief Whether the collection could stage @p changes without flushing first.
     *
     *  Counted in keys alone, since the collection flushes on its own when the bytes do not fit, so a
     *  wrong answer here costs a flush and never correctness.
     */
    [[nodiscard]] bool collection_fits(write_buffer_t const& changes) const noexcept {
        return collection_.staged_count() + changes.count() <= config_.collection.write_buffer_max_elements;
    }

    /** Writes every staged write as one run, called by the registry under the write path it took. */
    status_t flush_collection() {
        if (write_path_adoption_.failed()) return write_path_adoption_;
        return collection_.flush(writing_lane_);
    }

    /** Stages every change of @p changes, called by the registry under the write path it took. */
    status_t insert_write_buffer(write_buffer_t const& changes) {
        if (write_path_adoption_.failed()) return write_path_adoption_;
        status_t first;
        [[maybe_unused]] st::status_t const walked = changes.stored_values.for_each([&](auto const& entry) noexcept {
            auto const& place = entry.value();
            status_t const written = place.offset == tombstone_offset_k
                                         ? collection_.erase(writing_lane_, entry.key())
                                         : collection_.insert(writing_lane_, entry.key(),
                                                              changed_bytes_(changes, place));
            if (written.failed() && first.succeeded()) first = written;
            return st::walk_control_t::resume_k;
        });
        return first;
    }

#pragma endregion Transaction Store Interface

  private:

#pragma region Private Types

    /** One reader's hold on the collection, which a writer waits behind and which never nests. */
    class read_path_guard_t {
      public:

        explicit read_path_guard_t(basic_single_collection_database const& owner) noexcept : owner_(&owner) {
            owner_->acquire_read_path_();
        }
        read_path_guard_t(read_path_guard_t const&) = delete;
        read_path_guard_t& operator=(read_path_guard_t const&) = delete;
        ~read_path_guard_t() noexcept { owner_->release_read_path_(); }

      private:

        /** The database whose collection this guard holds, borrowed for the guard's life. */
        basic_single_collection_database const* owner_;
    };

    /** One writer's hold on the collection, which every reader and every other writer waits behind. */
    class write_path_guard_t {
      public:

        explicit write_path_guard_t(basic_single_collection_database& owner) noexcept : owner_(&owner) {
            owner_->acquire_write_path_();
        }
        write_path_guard_t(write_path_guard_t const&) = delete;
        write_path_guard_t& operator=(write_path_guard_t const&) = delete;
        ~write_path_guard_t() noexcept { owner_->release_write_path_(); }

        /** Whether this thread took the lanes over, which every write through them needs. */
        status_t adopted() const noexcept { return owner_->write_path_adoption_; }

      private:

        /** The database whose collection this guard holds, borrowed for the guard's life. */
        basic_single_collection_database* owner_;
    };

#pragma endregion Private Types

#pragma region Private Construction

    /** Takes what @c open assembled, and points the registry at the database it will commit into. */
    basic_single_collection_database(volume_t& volume, io_lane_t& writing_lane, io_lane_t& merging_lane,
                                     single_collection_database_config_t const& config,
                                     resources_shared_ptr_t resources, collection_t&& mounted) noexcept
        : volume_(volume), writing_lane_(writing_lane), merging_lane_(merging_lane), config_(config),
          collection_(std::move(mounted)), registry_(*this, std::move(resources)) {}

#pragma endregion Private Construction

#pragma region Write Path

    /** Takes the collection for one reader, waiting while a writer holds it. */
    void acquire_read_path_() const noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {write_path_claim_};
        for (;;) {
            std::int32_t readers = claim.load(std::memory_order_relaxed);
            if (readers >= 0 && claim.compare_exchange_strong(readers, readers + 1, std::memory_order_acquire,
                                                              std::memory_order_relaxed))
                return;
            std::this_thread::yield();
        }
    }

    /** Counts one reader back out of the collection. */
    void release_read_path_() const noexcept {
        [[maybe_unused]] std::int32_t const before =
            fu::preferred_atomic_ref<std::int32_t> {write_path_claim_}.fetch_sub(1, std::memory_order_release);
    }

    /**
     *  @brief Takes the collection for one writer, waiting while any reader or writer holds it.
     *
     *  A lease belongs to the thread that announced itself on it, so the winner adopts both lanes
     *  before it writes through them; the claim is what makes that handoff a handoff and not a share.
     */
    void acquire_write_path_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {write_path_claim_};
        std::int32_t expected_idle = 0;
        while (
            !claim.compare_exchange_strong(expected_idle, -1, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected_idle = 0;
            std::this_thread::yield();
        }
        write_path_adoption_ = volume_.disk().adopt(writing_lane_.owner());
        if (write_path_adoption_.succeeded()) write_path_adoption_ = volume_.disk().adopt(merging_lane_.owner());
    }

    /** Hands the collection back to the readers and the next writer. */
    void release_write_path_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> {write_path_claim_}.store(0, std::memory_order_release);
    }

#pragma endregion Write Path

#pragma region Private Methods

    /** The bytes @p place names inside @p changes, empty for an empty value. */
    [[nodiscard]] static std::span<byte_t const> changed_bytes_(write_buffer_t const& changes,
                                                                auto const& place) noexcept {
        if (place.size == 0) return {};
        return std::span<byte_t const> {changes.stored_values_buffer.data() + place.offset, place.size};
    }

    /** The name the schema blob answers to on the volume. */
    [[nodiscard]] static blob_name_t schema_blob_name_() noexcept {
        static constexpr char text[] = "single-collection-database.schema";
        return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
    }

    /** The name the owner's blob answers to on the volume. */
    [[nodiscard]] static blob_name_t user_data_blob_name_() noexcept {
        static constexpr char text[] = "single-collection-database.user";
        return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
    }

    /** The shape this database would write into its schema blob, less the runs that follow it there. */
    [[nodiscard]] single_collection_database_schema_record_t wanted_schema_() const noexcept {
        single_collection_database_schema_record_t wanted;
        wanted.value_layout = static_cast<std::uint16_t>(value_layout_k);
        wanted.value_max_size = collection_.value_max_size();
        return wanted;
    }

    /** Reads the schema blob and takes its runs back, or writes the one this database would have written. */
    status_t agree_on_schema_() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        single_collection_database_schema_record_t const wanted = wanted_schema_();

        expected<blob_t> const found = volume_.find(schema_blob_name_());
        if (!found) return write_schema_(std::span<lsm_tree_run_record_t const> {});
        if (found->length < sizeof(wanted))
            return {status_code_t::malformed_k, "The volume's schema blob is shorter than one record"};

        st::basic_vector<byte_t> image;
        if (image.resize(static_cast<std::size_t>(found->length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the schema into"};
        if (status_t const read = read_blob_(writing_lane_, *found, std::span<byte_t> {image}); read.failed())
            return read;

        single_collection_database_schema_record_t carried;
        std::memcpy(&carried, image.data(), sizeof(carried));
        if (carried.magic != single_collection_database_magic_k)
            return {status_code_t::malformed_k, "The volume's schema blob carries another magic"};
        if (carried.format_version != single_collection_database_format_version_k)
            return {status_code_t::unsupported_k, "The volume's schema follows another format version"};
        if (carried.value_layout != wanted.value_layout || carried.value_max_size != wanted.value_max_size)
            return {status_code_t::invalid_argument_k, "The volume holds a collection of another value layout"};
        if (image.size() - sizeof(carried) != carried.run_count * sizeof(lsm_tree_run_record_t))
            return {status_code_t::malformed_k, "The volume's schema names another number of runs"};

        st::basic_vector<lsm_tree_run_record_t> runs;
        if (runs.resize(static_cast<std::size_t>(carried.run_count)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the runs the schema names"};
        if (!runs.empty())
            std::memcpy(runs.data(), image.data() + sizeof(carried), runs.size() * sizeof(lsm_tree_run_record_t));
        return collection_.tree().restore(writing_lane_, std::span<lsm_tree_run_record_t const> {runs});
    }

    /** Names every run the tree holds in the schema blob, which every publication of a run does. */
    status_t save_schema_() noexcept {
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (status_t const named = record_lsm_tree_runs(collection_.tree(), runs); named.failed()) return named;
        return write_schema_(std::span<lsm_tree_run_record_t const> {runs});
    }

    /** Lays the schema record and @p runs out back to back and replaces the schema blob with them. */
    status_t write_schema_(std::span<lsm_tree_run_record_t const> runs) noexcept {
        single_collection_database_schema_record_t record = wanted_schema_();
        record.run_count = runs.size();
        st::basic_vector<byte_t> image;
        if (image.resize(sizeof(record) + runs.size() * sizeof(lsm_tree_run_record_t)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the schema out"};
        std::memcpy(image.data(), &record, sizeof(record));
        if (!runs.empty())
            std::memcpy(image.data() + sizeof(record), runs.data(), runs.size() * sizeof(lsm_tree_run_record_t));
        return write_blob_(schema_blob_name_(), blob_kind_t::schema_k, std::span<byte_t const> {image});
    }

    /** Writes the schema out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        return static_cast<basic_single_collection_database*>(context)->save_schema_();
    }

    /** Forgets @p name, which a volume holding no such blob reports as @c not_found_k. */
    status_t drop_blob_(blob_name_t name) noexcept { return volume_.drop(writing_lane_, name); }

    /** Writes @p bytes as the blob @p name of @p kind on the writing lane, replacing any blob of that name. */
    status_t write_blob_(blob_name_t name, blob_kind_t kind, std::span<byte_t const> bytes) noexcept {
        if (volume_.names(name))
            if (status_t const dropped = volume_.drop(writing_lane_, name); dropped.failed()) return dropped;

        expected<blob_t> const reserved = volume_.create(name, kind, bytes.size());
        if (!reserved) return reserved.status();
        if (status_t const written = fill_blob_(*reserved, bytes); written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_lane_, name);
            return written;
        }
        if (status_t const sealed = volume_.seal(writing_lane_, *reserved); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_lane_, name);
            return sealed;
        }
        return {};
    }

    /** Moves @p bytes into the range @p blob reserved, one block at a time and padded to a page. */
    status_t fill_blob_(blob_t const& blob, std::span<byte_t const> bytes) noexcept {
        basic_write_pipeline<disk_t> writer {volume_.disk(), writing_lane_, volume_.target()};
        std::uint64_t at = 0;
        while (at != bytes.size()) {
            expected<buffer_lease_t> block = writer.stage_or_harvest();
            if (!block) return block.status();
            std::uint64_t const room = rounded_down_to(block->size(), portable_alignment_k);
            if (room == 0) return {status_code_t::capacity_exhausted_k, "A lane's block is shorter than one page"};

            std::uint64_t const carried = room < bytes.size() - at ? room : bytes.size() - at;
            std::uint64_t const padded = rounded_up_to(carried, portable_alignment_k);
            std::span<byte_t> const room_bytes = block->bytes();
            std::memcpy(room_bytes.data(), bytes.data() + at, static_cast<std::size_t>(carried));
            std::memset(room_bytes.data() + carried, 0, static_cast<std::size_t>(padded - carried));
            if (status_t const queued = writer.commit(std::move(*block), blob.slice(at, padded)); queued.failed())
                return queued;
            at += carried;
        }
        return writer.make_durable();
    }

    /** Copies the first @c into.size() bytes of @p blob into @p into, one block at a time. */
    status_t read_blob_(io_lane_t& lane, blob_t const& blob, std::span<byte_t> into) const noexcept {
        std::uint64_t const chunk = rounded_down_to(lane.requests().block_bytes(), portable_alignment_k);
        if (chunk == 0) return {status_code_t::capacity_exhausted_k, "A lane's block is shorter than one page"};
        std::uint64_t const wanted = rounded_up_to(into.size(), portable_alignment_k);

        status_t first;
        auto sink = [&](harvested_block_t const& reaped) noexcept {
            if (status_t const whole = require_whole(reaped); whole.failed()) {
                if (first.succeeded()) first = whole;
                return;
            }
            std::uint64_t const at = static_cast<std::uint64_t>(reaped.tag) * chunk;
            if (at >= into.size()) return;
            std::uint64_t const room = into.size() - at;
            std::uint64_t const carried = reaped.bytes.size() < room ? reaped.bytes.size() : room;
            std::memcpy(into.data() + at, reaped.bytes.data(), static_cast<std::size_t>(carried));
        };

        basic_read_pipeline<disk_t> reader {volume_.disk(), lane, volume_.target()};
        std::uint64_t at = 0;
        std::uint32_t tag = 0;
        while (at != wanted) {
            while (at != wanted && reader.has_room()) {
                std::uint64_t const length = chunk < wanted - at ? chunk : wanted - at;
                if (status_t const offered = reader.offer(blob.slice(at, length), tag); offered.failed())
                    return offered;
                at += length;
                ++tag;
            }
            // The offers sit staged until the lane submits them, and a harvest waits only on what it did.
            if (status_t const flushed = reader.flush(); flushed.failed()) return flushed;
            if (status_t const harvested = reader.harvest_all(sink); harvested.failed()) return harvested;
        }
        return first;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run and every blob lives on, borrowed and outliving the database. */
    volume_t& volume_;

    /** The lane every write, flush and commit runs on, borrowed and outliving the database. */
    io_lane_t& writing_lane_;

    /** The second lane a compaction writes its new runs on, borrowed and outliving the database. */
    io_lane_t& merging_lane_;

    /** The bounds this database was opened with. */
    single_collection_database_config_t config_ {};

    /** The collection every write lands in. */
    collection_t collection_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    registry_t registry_;

    /** Readers inside the collection, or minus one while a writer holds it. */
    std::int32_t mutable write_path_claim_ = 0;

    /** Whether the writer holding the path took both lanes over, written only under the claim. */
    status_t write_path_adoption_;

#pragma endregion Private Members
};

#pragma endregion Database

} // namespace unum::ustore::nvme
