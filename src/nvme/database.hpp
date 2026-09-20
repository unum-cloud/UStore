/**
 *  @file src/nvme/database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Named collections sharing one set of transactions, one catalog and one set of volumes.
 *
 *  A database is a catalog and nothing else: it resolves a name to a collection, hands that collection
 *  a channel, and persists the mapping as one named blob of a volume so a reopen finds the same names.
 *  Every byte of data belongs to the collection below it, and every conflict belongs to the registry
 *  beside it; what is left here is placement, naming and the order the two are changed in.
 *
 *  One claim guards every call that reaches a collection. The collection layer is single-threaded by
 *  construction - its write buffer, its arena and its value cache are plain containers that a read
 *  mutates as much as a write does - so no two calls may be inside one collection at once, and a
 *  transaction has to write several collections with nothing reading between them. That makes the
 *  exclusion the database's rather than each collection's.
 *
 *  A call that reaches no collection takes no claim. The entries are pinned on the heap, so a create
 *  and a drop replace only the array naming them: the catalog is published into one of two views, a
 *  lookup counts itself into whichever is live, and a publication waits until the view it retired has
 *  no reader left before the dropped entry is destroyed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::move`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/buffer_pool.hpp"       // `pooled_buffer_t`
#include "nvme/catalog.hpp"           // `blob_kind_t`, `blob_name_t`
#include "nvme/collection.hpp"        // `collection`, `select_result_t`
#include "nvme/config.hpp"            // `collection_config_t`, `database_config_t`
#include "nvme/database_snapshot.hpp" // `database_snapshot`
#include "nvme/extent.hpp"            // `rounded_up_to`, `portable_alignment_k`
#include "nvme/files_disk.hpp"        // `any_files_disk_t`
#include "nvme/io_channel.hpp"        // `io_channel_t`, `basic_io_channel_pool`
#include "nvme/io_pipeline.hpp"       // `basic_read_pipeline`, `basic_write_pipeline`, `require_completed_in_full`
#include "nvme/key_slot.hpp"          // `key_slot`
#include "nvme/lsm_tree.hpp"          // `lsm_tree_run_record_t`, `lsm_tree_publication_hook_t`
#include "nvme/resources.hpp"         // `resources_shared_ptr_t`
#include "nvme/stored_value.hpp"      // `stored_value_size_t`, `stored_value_view_t`
#include "nvme/types.hpp"             // `collection_numeric_id_t`, `value_layout_t`, `resolve_value_layout`
#include "nvme/volume.hpp"            // `basic_extent_volume`, `blob_t`, `volume_id_t`
#include "nvme/volume_router.hpp"     // `basic_volume_router`

#include "nvme/database_transaction_registry.hpp" // `database_transaction_registry`
#include "nvme/database_transaction.hpp"          // `database_transaction`, which the registry no longer includes

namespace unum::ustore::nvme {

#pragma region Schema

/** The layout the catalog blob follows, which a database of another version refuses to open. */
inline constexpr std::uint64_t database_schema_version_k = 2;

/** @p text as the bytes a volume names a blob by, without its terminator. */
template <std::size_t length_>
[[nodiscard]] inline blob_name_t blob_name_of(char const (&text)[length_]) noexcept {
    return blob_name_t {reinterpret_cast<byte_t const*>(text), length_ - 1};
}

/** The name the catalog of a database is stored under, on whichever volume placement gave it. */
[[nodiscard]] inline blob_name_t database_schema_name() noexcept { return blob_name_of("ustore.database.schema"); }

/** The name the caller's own bytes are stored under, which the engine never reads. */
[[nodiscard]] inline blob_name_t database_user_data_name() noexcept { return blob_name_of("ustore.database.user"); }

/** Bytes the longest collection name takes, which a record holds inline rather than pointing at. */
inline constexpr std::size_t collection_name_max_bytes_k = 64;

/**
 *  @brief One collection as the catalog blob names it, which is all a mount needs to open it again.
 *
 *  Every field is stored as it lies, so the image is the object representation and no field of it is
 *  ever read through a pointer the layout did not align.
 */
struct collection_record_t {

    /** The number the catalog and every commit address the collection by. */
    collection_numeric_id_t numeric_id = collection_invalid_numeric_id_k;

    /** Bytes level zero admits before a compaction is due. */
    std::uint64_t first_level_max_bytes = 0;

    /** How fast the bytes a level admits grow with its number. */
    std::uint64_t level_enlarge_factor = 0;

    /** Bytes the value cache of the collection holds. */
    std::uint64_t cache_limit = 0;

    /** Bytes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_bytes = 0;

    /** Writes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_elements = 0;

    /** Runs of this collection in the image's run list, which follow those of the records before it. */
    std::uint64_t run_count = 0;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    std::uint32_t value_size = 0;

    /** Bytes the largest value takes, zero for a fixed-width layout. */
    std::uint32_t value_max_size = 0;

    /** The volume the collection's runs live on, which a reopen binds it to again. */
    std::uint16_t volume = 0;

    /** Bytes of @c name_storage the name occupies. */
    std::uint16_t name_bytes = 0;

    /** The name a caller reaches the collection by, held inline and never terminated. */
    std::array<char, collection_name_max_bytes_k> name_storage {};

    /** The name this record carries, as a view into its own bytes. */
    [[nodiscard]] string_view_t name() const noexcept { return string_view_t {name_storage.data(), name_bytes}; }

    /** The collection this record describes, as the collection layer takes its bounds. */
    [[nodiscard]] collection_config_t config() const noexcept {
        collection_config_t config;
        config.cache_limit = static_cast<std::size_t>(cache_limit);
        config.write_buffer_max_bytes = static_cast<std::size_t>(write_buffer_max_bytes);
        config.write_buffer_max_elements = static_cast<std::size_t>(write_buffer_max_elements);
        config.lsm_tree.level_enlarge_factor = static_cast<std::size_t>(level_enlarge_factor);
        config.lsm_tree.lsm_level_0.max_bytes = static_cast<std::size_t>(first_level_max_bytes);
        config.lsm_tree.lsm_level_0.sorted_run.value_size = static_cast<stored_value_size_t>(value_size);
        config.lsm_tree.lsm_level_0.sorted_run.value_max_size = static_cast<stored_value_size_t>(value_max_size);
        return config;
    }
};

#pragma endregion Schema

#pragma region Database

/**
 *  @brief Named collections of one database, the catalog that resolves them and the transactions they share.
 *  @tparam stored_key_ The key every collection of this database is addressed by.
 *  @tparam volume_type_ The volume every run of every collection lives on.
 *
 *  The router and the channels are borrowed and must outlive the database, and so must the budget the
 *  collections charge their memory to. A database is pinned rather than movable, because its registry
 *  and every live transaction name it.
 */
template <typename stored_key_, typename volume_type_>
class basic_database {
  public:

#pragma region Types

    /** The key every collection of this database is addressed by. */
    using stored_key_t = stored_key_;

    /** The volume every run of every collection lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The volumes of this database and the placement that spreads collections over them. */
    using router_t = basic_volume_router<volume_t>;

    /** The queue pairs every call that moves bytes borrows one of. */
    using channel_pool_t = basic_io_channel_pool<disk_t>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** A collection named by its number or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_numeric_id_t, collection_alphanumeric_id_t>;

    /** The transactions of this database, the order they commit in and the conflicts that stop them. */
    using registry_t = database_transaction_registry<stored_key_t, basic_database>;

    /** One transaction over the collections of this database. */
    using transaction_t = database_transaction<stored_key_t, basic_database>;

    /** A live transaction, owned by whoever asked for it and retired when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** One level set per collection, held so the whole database answers at one moment. */
    using snapshot_t = database_snapshot<stored_key_t, volume_t>;

    /** A collection whose values follow @p value_layout_. */
    template <value_layout_t value_layout_>
    using collection_of = collection<key_slot<stored_key_t, value_layout_>, volume_t>;

    /** One collection, of whichever value layout it was created with. */
    using any_collection_t =
        std::variant<collection_of<value_layout_t::inline_1_byte_k>, collection_of<value_layout_t::inline_4_bytes_k>,
                     collection_of<value_layout_t::inline_8_bytes_k>, collection_of<value_layout_t::inline_16_bytes_k>,
                     collection_of<value_layout_t::inline_32_bytes_k>, collection_of<value_layout_t::inline_64_bytes_k>,
                     collection_of<value_layout_t::fixed_width_k>, collection_of<value_layout_t::variable_width_k>>;

    /** One entry of the catalog: what a collection is called, how it is shaped, and the collection itself. */
    struct collection_entry_t {

        /** The number the catalog and every commit address this collection by. */
        collection_numeric_id_t numeric_id = collection_invalid_numeric_id_k;

        /** The name a caller reaches this collection by. */
        string_t name;

        /** The bounds this collection was mounted with. */
        collection_config_t config {};

        /** The volume this collection's runs live on. */
        volume_id_t volume = volume_id_t::unmounted_k;

        /** The collection itself, pinned here so a snapshot may name it. */
        any_collection_t held;
    };

#pragma endregion Types

#pragma region Construction

    basic_database(basic_database const&) = delete;
    basic_database(basic_database&&) = delete;
    basic_database& operator=(basic_database const&) = delete;
    basic_database& operator=(basic_database&&) = delete;

    /** A database charging @p resources for every collection it opens, which @c mount then fills. */
    explicit basic_database(resources_shared_ptr_t resources) noexcept
        : resources_(std::move(resources)), transactions_(*this, resources_) {}

    ~basic_database() noexcept { [[maybe_unused]] status_t const closed = unmount(); }

    /**
     *  @brief Opens every collection @p router already names, or starts an empty catalog.
     *  @param[inout] router The volumes every run lands on, borrowed and outliving the database.
     *  @param[inout] channels The queue pairs every call borrows one of, borrowed and outliving the database.
     *  @param[in] config The write-buffer bounds a transaction takes when its caller names none.
     */
    status_t mount(router_t& router, channel_pool_t& channels, database_config_t const& config) noexcept {
        if (router_ != nullptr) return {status_code_t::already_exists_k, "This database has already mounted"};
        router_ = &router;
        channels_ = &channels;
        config_ = config;

        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (status_t const loaded = load_schema_(claimed->channel()); loaded.failed()) return loaded;
        return publish_catalog_();
    }

    /**
     *  @brief Flushes every collection, rewrites the catalog and lets every collection go.
     *
     *  @note The router and the channels are still needed here, so this runs before the mount underneath
     *      is torn down; the destructor calls it for a caller that did not.
     */
    status_t unmount() noexcept {
        if (router_ == nullptr) return {};
        status_t first;
        {
            expected<claimed_channel_t> claimed = claim_channel_();
            if (!claimed) first = claimed.status();
            else {
                for (std::size_t index = 0; index != collections_.size(); ++index)
                    if (status_t const flushed = std::visit(
                            [&](auto& held) noexcept { return held.flush(claimed->channel()); },
                            collections_[index]->held);
                        flushed.failed() && first.succeeded())
                        first = flushed;
                if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded())
                    first = saved;
            }
            // The entries outlive the publication that stops naming them, so no reader is left in one.
            st::basic_vector<entry_pointer_t> released = std::move(collections_);
            [[maybe_unused]] status_t const emptied = publish_catalog_();
        }
        router_ = nullptr;
        channels_ = nullptr;
        return first;
    }

#pragma endregion Construction

#pragma region Catalog

    /** Collections this database names right now. */
    [[nodiscard]] std::size_t size() const noexcept {
        catalog_guard_t const published {*this};
        return published.view().size();
    }

    /** Whether @p id names a collection of this database. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) const noexcept {
        catalog_guard_t const published {*this};
        return entry_at_in_(published.view(), id) != nullptr;
    }

    /** The number @p id resolves to, or @c collection_invalid_numeric_id_k when it names nothing. */
    [[nodiscard]] collection_numeric_id_t collection_numeric_id(any_collection_id_t const& id) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_at_in_(published.view(), id);
        return found ? found->numeric_id : collection_invalid_numeric_id_k;
    }

    /** The name @p numeric_id answers to, which is empty when it names nothing. */
    [[nodiscard]] string_t collection_name(collection_numeric_id_t numeric_id) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_numbered_in_(published.view(), numeric_id);
        string_t name;
        if (found) { [[maybe_unused]] bool const copied = name.try_assign(found->name.view()); }
        return name;
    }

    /** The bounds @p numeric_id was created with, which a transaction checks every write against. */
    status_t collection_config(collection_numeric_id_t numeric_id, collection_config_t& config) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_numbered_in_(published.view(), numeric_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /**
     *  @brief The number @p id answers to and the bounds it was created with, read in one lookup.
     *  @param[in] id The collection to resolve, named by its number or by its name.
     *  @param[out] numeric_id The number it answers to, invalid when the database names none.
     *  @param[out] config The bounds it was created with, untouched when the database names none.
     */
    status_t resolve_collection(any_collection_id_t const& id, collection_numeric_id_t& numeric_id,
                                collection_config_t& config) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_at_in_(published.view(), id);
        numeric_id = found ? found->numeric_id : collection_invalid_numeric_id_k;
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** How many times the catalog has been published, which a memo of a lookup is checked against. */
    [[nodiscard]] std::uint64_t catalog_version() const noexcept {
        return fu::preferred_atomic_ref<std::uint64_t const> {catalog_version_}.load(std::memory_order_acquire);
    }

    /** The name and the number of every collection, which is short of the full list only when memory runs out. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() const noexcept {
        catalog_guard_t const published {*this};
        catalog_view_t const& view = published.view();
        st::basic_vector<collection_ids_t> listed;
        if (listed.reserve(view.size()) != st::status_t::success_k) return listed;
        for (std::size_t index = 0; index != view.size(); ++index) {
            collection_ids_t named;
            if (!named.name.try_assign(view[index]->name.view())) break;
            named.numeric_id = view[index]->numeric_id;
            if (listed.push_back(std::move(named)) != st::status_t::success_k) break;
        }
        return listed;
    }

    /**
     *  @brief Creates the collection @p name, shaped by @p config, and records it in the catalog.
     *  @param[in] name The name callers reach the collection by, which no other collection may hold.
     *  @param[in] config The write-buffer bounds, the cache bound and the shape of the tree below.
     *  @return The number the collection is addressed by, which outlives its name.
     *
     *  @note The value layout follows from @c config's value size, so a width of eight bytes is stored
     *      inside the key slots and a width of none is a collection of values that differ.
     */
    expected<collection_numeric_id_t> create_collection(string_view_t name,
                                                        collection_config_t const& config) noexcept {
        if (name.empty()) return status_t {status_code_t::invalid_argument_k, "A collection is named by a byte"};
        if (name.size() > collection_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A collection name is shorter than that"};
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (entry_named_in_(claimed_catalog_(), name) != nullptr)
            return status_t {status_code_t::already_exists_k, "The database already names that collection"};

        expected<volume_id_t> const placed = router_->optimal_volume(1);
        if (!placed) return placed.status();
        value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
        expected<any_collection_t> mounted = mount_collection_(router_->volume(*placed), config, layout);
        if (!mounted) return mounted.status();

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->numeric_id = unused_numeric_id_();
        if (!entry->name.try_assign(name))
            return status_t {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->config = config;
        entry->volume = *placed;
        entry->held = std::move(*mounted);

        collection_numeric_id_t const numeric_id = entry->numeric_id;
        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            collections_.erase(collections_.size() - 1);
            return saved;
        }
        if (status_t const publication = publish_catalog_(); publication.failed()) {
            collections_.erase(collections_.size() - 1);
            return publication;
        }
        return numeric_id;
    }

    /** Returns every run of @p id to its volume and forgets the name, which destroys the data. */
    status_t drop_collection(any_collection_id_t const& id) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::size_t const position = position_at_(id);
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};

        // The catalog stops naming the runs before a single one of them is handed back.
        entry_pointer_t doomed = std::move(collections_[position]);
        collections_.erase(position);
        if (status_t const publication = publish_catalog_(); publication.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            return publication;
        }
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            [[maybe_unused]] status_t const restored = publish_catalog_();
            return saved;
        }
        return std::visit([&](auto& held) noexcept { return held.destroy(claimed->channel()); }, doomed->held);
    }

#pragma endregion Catalog

#pragma region Shape

    /** Keys staged plus key slots every level of every collection holds. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            counted += std::visit([](auto const& collection) noexcept { return collection.population_count(); },
                                  collections_[index]->held);
        return counted;
    }

    /** Bytes every run of every collection takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            counted += std::visit([](auto const& collection) noexcept { return collection.size_bytes(); },
                                  collections_[index]->held);
        return counted;
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return 0;
        return std::visit([](auto const& collection) noexcept { return collection.value_max_size(); }, found->held);
    }

#pragma endregion Shape

#pragma region Lookups

    /** Whether @p id carries @p stored_key with a value rather than a tombstone. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return false;
        stored_value_size_t size = 0;
        return std::visit([&](auto const& collection) noexcept { return collection.find(stored_key, size); },
                          found->held) == select_result_t::success_k;
    }

    /** Bytes the value of @p stored_key takes in @p id, or @c not_found_k for a miss and an erasure. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key,
                               stored_value_size_t& size) const noexcept {
        size = 0;
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        select_result_t const answered = std::visit(
            [&](auto const& collection) noexcept { return collection.find(stored_key, size); }, found->held);
        if (answered != select_result_t::success_k) return {status_code_t::not_found_k, "That collection misses it"};
        return {};
    }

    /**
     *  @brief Copies the value @p id holds for @p stored_key into @p into, which needs only its bytes.
     *  @param[in] id The collection to read from, named by its number or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.select(claimed->channel(), stored_key, into, size);
            },
            found->held);
    }

    /**
     *  @brief Reads the values of @p stored_keys out of @p id, refilling the channel as completions retire.
     *  @param[in] id The collection to read from, named by its number or by its name.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.select(claimed->channel(), stored_keys, results, sink);
            },
            found->held);
    }

#pragma endregion Lookups

#pragma region Writes

    /** Stages @p stored_value under @p stored_key of @p id, flushing first when the buffer is full. */
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto& collection) noexcept { return collection.insert(claimed->channel(), stored_key, stored_value); },
            found->held);
    }

    /** Stages a tombstone under @p stored_key of @p id, flushing first when the buffer is full. */
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& collection) noexcept { return collection.erase(claimed->channel(), stored_key); },
                          found->held);
    }

    /**
     *  @brief Stages one value per key into @p id, flushing first when they do not all fit.
     *  @param[in] id The collection to write to, named by its number or by its name.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto& collection) noexcept {
                return collection.insert(claimed->channel(), stored_keys, stored_values, sizes);
            },
            found->held);
    }

    /** Stages a tombstone under every key of @p stored_keys in @p id. */
    status_t remove(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& collection) noexcept { return collection.erase(claimed->channel(), stored_keys); },
                          found->held);
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write of every collection as one run of its level zero. */
    status_t flush() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const flushed = std::visit([&](auto& held) noexcept { return held.flush(claimed->channel()); },
                                                    collections_[index]->held);
                flushed.failed() && first.succeeded())
                first = flushed;
        // Every run the flushes wrote is sealed by now, so the catalog may name them.
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded()) first = saved;
        return first;
    }

    /** Merges one overfull level of every collection that has one, which needs two channels at once. */
    status_t compact() noexcept {
        expected<claimed_channel_t> reading = claim_channel_();
        if (!reading) return reading.status();
        expected<channel_guard_t> writing = channels_->acquire();
        if (!writing) return writing.status();

        schema_write_t recording {this, &reading->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = &recording;

        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            expected<compaction_outcome_t> const merged = std::visit(
                [&](auto& held) noexcept {
                    return held.tree().compact(reading->channel(), writing->channel(),
                                               compaction_placement_t::automatic_k, nullptr, published);
                },
                collections_[index]->held);
            if (!merged && first.succeeded()) first = merged.status();
        }
        return first;
    }

    /**
     *  @brief Flushes every collection, then holds the level set each flush published.
     *
     *  @note A hold is what a publication waits on, so a flush, a compaction or another snapshot
     *      taken on the same thread while one is still alive never returns.
     */
    expected<snapshot_t> take_snapshot() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        snapshot_t taken;
        if (status_t const room = taken.reserve(collections_.size()); room.failed()) return room;

        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_entry_t& entry = *collections_[index];
            status_t const held = std::visit(
                [&](auto& collection) noexcept -> status_t {
                    auto levels = collection.take_snapshot(claimed->channel());
                    if (!levels) return levels.status();
                    return taken.hold(entry.numeric_id, entry.name.view(),
                                      typename snapshot_t::any_collection_snapshot_t {std::move(*levels)});
                },
                entry.held);
            if (held.failed()) return held;
        }
        return taken;
    }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction of @p id over every collection, with the write-buffer bounds of @p config. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id,
                                                          transaction_config_t const& config) noexcept {
        return transactions_.create_transaction(id, config);
    }

    /** A transaction of @p id with the write-buffer bounds this database was mounted with. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id) noexcept {
        return create_transaction(id, config_.default_transaction);
    }

    /** Whether @p id names a live transaction of this database. */
    [[nodiscard]] bool has_transaction(transaction_id_t id) const noexcept { return transactions_.has_transaction(id); }

#pragma endregion Transactions

#pragma region Write Path

    /**
     *  @brief Takes the database's claim and a channel, which a staged transaction holds until it commits.
     *
     *  @note The claim is what makes a commit atomic to a reader: no @c select of any collection runs
     *      between the first write a commit applies and the last.
     */
    void acquire_write_path() noexcept {
        write_claim_ = claim_guard_t {claim_};
        write_channel_ = channel_guard_t {};
        expected<channel_guard_t> guard = channels_->acquire();
        if (guard) write_channel_ = std::move(*guard);
    }

    /** Hands the channel and the claim back, which a commit and a rollback both do. */
    void release_write_path() noexcept {
        write_channel_ = channel_guard_t {};
        write_claim_ = claim_guard_t {};
    }

    /** Whether @p numeric_id still names a collection, which a commit checks before it writes one. */
    [[nodiscard]] bool contains_collection(collection_numeric_id_t numeric_id) const noexcept {
        return entry_numbered_in_(claimed_catalog_(), numeric_id) != nullptr;
    }

    /**
     *  @brief Whether @p count writes of @p bytes fit in the write buffer of @p numeric_id as it stands.
     *
     *  @note A refusal only asks the caller to flush first; a collection whose buffer fills anyway
     *      flushes itself rather than losing the write.
     */
    [[nodiscard]] bool collection_fits(collection_numeric_id_t numeric_id, std::size_t count,
                                       std::size_t bytes) const noexcept {
        collection_entry_t const* const found = entry_numbered_in_(claimed_catalog_(), numeric_id);
        if (!found) return false;
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.staged_count() + count <= collection.config().write_buffer_max_elements &&
                       bytes <= collection.config().write_buffer_max_bytes;
            },
            found->held);
    }

    /** Writes the staged writes of @p numeric_id out, on the channel the write path holds. */
    status_t flush_collection(collection_numeric_id_t numeric_id) noexcept {
        collection_entry_t* const found = entry_numbered_in_(claimed_catalog_(), numeric_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit([&](auto& collection) noexcept { return collection.flush(write_channel_.channel()); },
                          found->held);
    }

    /** Stages @p stored_value under @p stored_key of @p numeric_id, on the channel the write path holds. */
    status_t insert_in_collection(collection_numeric_id_t numeric_id, stored_key_t stored_key,
                                  stored_value_view_t stored_value) noexcept {
        collection_entry_t* const found = entry_numbered_in_(claimed_catalog_(), numeric_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit(
            [&](auto& collection) noexcept {
                return collection.insert(write_channel_.channel(), stored_key, stored_value);
            },
            found->held);
    }

    /** Stages a tombstone under @p stored_key of @p numeric_id, on the channel the write path holds. */
    status_t remove_from_collection(collection_numeric_id_t numeric_id, stored_key_t stored_key) noexcept {
        collection_entry_t* const found = entry_numbered_in_(claimed_catalog_(), numeric_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit(
            [&](auto& collection) noexcept { return collection.erase(write_channel_.channel(), stored_key); },
            found->held);
    }

#pragma endregion Write Path

#pragma region User Data

    /** Bytes the caller's own blob takes, which is zero when it has saved none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        expected<blob_t> const found = router_->find(database_user_data_name());
        return found ? found->length : 0;
    }

    /** Replaces the caller's own blob with @p bytes, which the engine never reads. */
    status_t save_user_data(bytes_view_t bytes) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return write_blob_(claimed->channel(), database_user_data_name(), blob_kind_t::user_data_k, bytes);
    }

    /** Reads the caller's own blob into @p into, which must hold @c user_data_size_bytes of it. */
    status_t load_user_data(bytes_span_t into) const noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return read_blob_(claimed->channel(), database_user_data_name(), into);
    }

    /** Forgets the caller's own blob and returns its bytes to the volume that held it. */
    status_t clear_user_data() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return router_->drop(claimed->channel(), database_user_data_name());
    }

#pragma endregion User Data

  private:

#pragma region Private Types

    /** One catalog entry, heap-pinned so a snapshot and a live transaction may name it. */
    using entry_pointer_t = std::unique_ptr<collection_entry_t>;

    /** One published catalog, naming every entry the database held when it was laid out. */
    using catalog_view_t = st::basic_vector<collection_entry_t*>;

    /**
     *  @brief Holds one published catalog against republication for the life of the guard.
     *
     *  A lookup that reads nothing but a name, a number or a shape needs no claim: every entry is
     *  pinned on the heap, so a create and a drop replace only the array naming them, and the
     *  publication that replaces it waits until the view it retired has no reader left inside.
     */
    class catalog_guard_t {
      public:

        /** Counts this thread into whichever view is live, retrying while one is being published. */
        explicit catalog_guard_t(basic_database const& database) noexcept : database_(&database) {
            for (;;) {
                slot_ = database.live_catalog_slot_();
                database.catalog_readers_reference_(slot_).fetch_add(1, std::memory_order_acquire);
                if (database.live_catalog_slot_() == slot_) return;
                database.catalog_readers_reference_(slot_).fetch_sub(1, std::memory_order_release);
            }
        }

        catalog_guard_t(catalog_guard_t const&) = delete;
        catalog_guard_t& operator=(catalog_guard_t const&) = delete;
        catalog_guard_t(catalog_guard_t&&) = delete;
        catalog_guard_t& operator=(catalog_guard_t&&) = delete;

        ~catalog_guard_t() noexcept {
            database_->catalog_readers_reference_(slot_).fetch_sub(1, std::memory_order_release);
        }

        /** The entries this view names, which no publication replaces while the guard lives. */
        [[nodiscard]] catalog_view_t const& view() const noexcept { return database_->catalogs_[slot_]; }

      private:

        /** The database whose catalog this guard holds, borrowed for the guard's life. */
        basic_database const* database_ = nullptr;

        /** Which of the two views this guard counted itself into. */
        std::size_t slot_ = 0;
    };

    /**
     *  @brief The database's one claim, held for the life of the guard.
     *
     *  This is the exclusion the file's brief names, and the only one: a collection admits one caller
     *  at a time, and a commit spans several collections, so the word is the database's.
     */
    class claim_guard_t {
      public:

        claim_guard_t() noexcept = default;
        claim_guard_t(claim_guard_t const&) = delete;
        claim_guard_t& operator=(claim_guard_t const&) = delete;

        /** Takes @p claim, waiting while another caller holds it. */
        explicit claim_guard_t(claim_word_t& claim) noexcept : claim_(&claim) {
            fu::preferred_atomic_ref<claim_word_t> const word {claim};
            claim_word_t expected_free = 0;
            while (
                !word.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                fu::preferred_yield_t {}(claim, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }

        claim_guard_t(claim_guard_t&& other) noexcept : claim_(std::exchange(other.claim_, nullptr)) {}

        claim_guard_t& operator=(claim_guard_t&& other) noexcept {
            if (this == &other) return *this;
            release_();
            claim_ = std::exchange(other.claim_, nullptr);
            return *this;
        }

        ~claim_guard_t() noexcept { release_(); }

      private:

        /** Hands the claim back, which both the destructor and a move assignment need. */
        void release_() noexcept {
            if (claim_) fu::preferred_atomic_ref<claim_word_t> {*claim_}.store(0, std::memory_order_release);
            claim_ = nullptr;
        }

        /** The word this guard holds, null once moved from. */
        claim_word_t* claim_ = nullptr;
    };

    /** The claim and the channel one call of this database runs under, released in that order. */
    class claimed_channel_t {
      public:

        claimed_channel_t(claim_guard_t&& claim, channel_guard_t&& channel) noexcept
            : claim_(std::move(claim)), channel_(std::move(channel)) {}

        claimed_channel_t(claimed_channel_t&&) noexcept = default;
        claimed_channel_t& operator=(claimed_channel_t&&) noexcept = default;

        /** The channel, which only the thread holding this is on. */
        [[nodiscard]] io_channel_t& channel() const noexcept { return channel_.channel(); }

      private:

        /** The database's claim, given back after the channel. */
        claim_guard_t claim_;

        /** The borrowed queue pair, drained and given back first. */
        channel_guard_t channel_;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Which of the two published catalogs a lookup reads right now. */
    [[nodiscard]] std::size_t live_catalog_slot_() const noexcept {
        return static_cast<std::size_t>(
            fu::preferred_atomic_ref<std::int32_t const> {live_catalog_}.load(std::memory_order_acquire));
    }

    /** The word counting the readers of published catalog @p slot. */
    [[nodiscard]] fu::preferred_atomic_ref<readers_count_t> catalog_readers_reference_(
        std::size_t slot) const noexcept {
        return fu::preferred_atomic_ref<readers_count_t> {catalog_readers_[slot]};
    }

    /**
     *  @brief The live catalog as a caller already holding the claim reads it, counting itself into none.
     *
     *  @note Only a claim holder publishes, and it publishes before it lets the claim go, so the view
     *      this answers is both live and in step with @c collections_ for as long as the claim is held.
     */
    [[nodiscard]] catalog_view_t const& claimed_catalog_() const noexcept { return catalogs_[live_catalog_slot_()]; }

    /**
     *  @brief Lays the catalog out into the idle view, makes it live, and waits its readers out.
     *
     *  @note Only a caller holding the claim publishes, so one thread alone writes the two views,
     *      and on return no reader is left inside the view this one retired - which is what lets
     *      a dropped collection be destroyed right after.
     */
    status_t publish_catalog_() noexcept {
        std::size_t const retired = live_catalog_slot_();
        std::size_t const staged = 1 - retired;
        catalog_view_t& building = catalogs_[staged];
        if (building.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to publish the catalog"};
        building.clear();
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            [[maybe_unused]] st::status_t const named = building.push_back(st::assume_reserved_t {},
                                                                           collections_[index].get());
        }
        fu::preferred_atomic_ref<std::uint64_t> const version {catalog_version_};
        version.store(version.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        fu::preferred_atomic_ref<std::int32_t> {live_catalog_}.store(static_cast<std::int32_t>(staged),
                                                                     std::memory_order_release);
        for (readers_count_t readers = catalog_readers_reference_(retired).load(std::memory_order_acquire);
             readers != 0; readers = catalog_readers_reference_(retired).load(std::memory_order_acquire))
            fu::preferred_yield_t {}(catalog_readers_[retired], readers, std::size_t {0});
        return {};
    }

    /** The entry @p numeric_id names in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_numbered_in_(catalog_view_t const& view,
                                                                collection_numeric_id_t numeric_id) noexcept {
        for (std::size_t index = 0; index != view.size(); ++index)
            if (view[index]->numeric_id == numeric_id) return view[index];
        return nullptr;
    }

    /** The entry @p name answers to in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_named_in_(catalog_view_t const& view, string_view_t name) noexcept {
        for (std::size_t index = 0; index != view.size(); ++index)
            if (view[index]->name.view() == name) return view[index];
        return nullptr;
    }

    /** The entry @p id names in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_at_in_(catalog_view_t const& view,
                                                          any_collection_id_t const& id) noexcept {
        if (collection_numeric_id_t const* const numbered = std::get_if<collection_numeric_id_t>(&id))
            return entry_numbered_in_(view, *numbered);
        return entry_named_in_(view, std::get<collection_alphanumeric_id_t>(id).view());
    }

    /** Takes the claim and then a shared channel, which every call that moves bytes runs under. */
    expected<claimed_channel_t> claim_channel_() const noexcept {
        claim_guard_t claim {claim_};
        expected<channel_guard_t> guard = channels_->acquire();
        if (!guard) return guard.status();
        return claimed_channel_t {std::move(claim), std::move(*guard)};
    }

    /** Where @p id sits in the catalog, or the catalog's size when it names nothing. */
    [[nodiscard]] std::size_t position_at_(any_collection_id_t const& id) const noexcept {
        if (collection_numeric_id_t const* const numbered = std::get_if<collection_numeric_id_t>(&id)) {
            for (std::size_t index = 0; index != collections_.size(); ++index)
                if (collections_[index]->numeric_id == *numbered) return index;
            return collections_.size();
        }
        collection_alphanumeric_id_t const& named = std::get<collection_alphanumeric_id_t>(id);
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (collections_[index]->name == named) return index;
        return collections_.size();
    }

    /** A number no live collection holds, which is one past the largest the catalog names. */
    [[nodiscard]] collection_numeric_id_t unused_numeric_id_() const noexcept {
        collection_numeric_id_t largest = 0;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_numeric_id_t const held = collections_[index]->numeric_id;
            if (held != collection_invalid_numeric_id_k && held > largest) largest = held;
        }
        return largest + 1;
    }

    /** Mounts a collection of @p value_layout_ over @p volume, as one alternative of the variant. */
    template <value_layout_t value_layout_>
    expected<any_collection_t> mounted_as_(volume_t& volume, collection_config_t const& config) const noexcept {
        expected<collection_of<value_layout_>> mounted = collection_of<value_layout_>::mount(
            volume, config, resources_->memory_constraint);
        if (!mounted) return mounted.status();
        return any_collection_t {std::move(*mounted)};
    }

    /** Mounts a collection of the runtime @p value_layout over @p volume. */
    expected<any_collection_t> mount_collection_(volume_t& volume, collection_config_t const& config,
                                                 value_layout_t value_layout) const noexcept {
        switch (value_layout) {
        case value_layout_t::inline_1_byte_k: return mounted_as_<value_layout_t::inline_1_byte_k>(volume, config);
        case value_layout_t::inline_4_bytes_k: return mounted_as_<value_layout_t::inline_4_bytes_k>(volume, config);
        case value_layout_t::inline_8_bytes_k: return mounted_as_<value_layout_t::inline_8_bytes_k>(volume, config);
        case value_layout_t::inline_16_bytes_k: return mounted_as_<value_layout_t::inline_16_bytes_k>(volume, config);
        case value_layout_t::inline_32_bytes_k: return mounted_as_<value_layout_t::inline_32_bytes_k>(volume, config);
        case value_layout_t::inline_64_bytes_k: return mounted_as_<value_layout_t::inline_64_bytes_k>(volume, config);
        case value_layout_t::fixed_width_k: return mounted_as_<value_layout_t::fixed_width_k>(volume, config);
        case value_layout_t::variable_width_k: return mounted_as_<value_layout_t::variable_width_k>(volume, config);
        default: break;
        }
        return status_t {status_code_t::invalid_argument_k, "A collection admits no such value layout"};
    }

    /**
     *  @brief Replaces the blob @p name with @p bytes, sealing it only once they are durable.
     *  @param[inout] channel The channel the bytes are staged and flushed on, held by the calling thread.
     *  @param[in] name The bytes the blob is named by, which no other blob of this database holds.
     *  @param[in] kind What the blob is, which is how a later mount enumerates it.
     *  @param[in] bytes The whole image, which must fit in one block of @p channel.
     */
    status_t write_blob_(io_channel_t& channel, blob_name_t name, blob_kind_t kind,
                         std::span<byte_t const> bytes) noexcept {
        if (bytes.empty()) return {status_code_t::invalid_argument_k, "A named blob carries at least one byte"};
        std::uint64_t const written = rounded_up_to(bytes.size(), portable_alignment_k);
        if (written > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        [[maybe_unused]] status_t const forgotten = router_->drop(channel, name);
        expected<blob_t> const reserved = router_->create(name, kind, bytes.size());
        if (!reserved) return reserved.status();
        if (reserved->extent.length < written)
            return {status_code_t::capacity_exhausted_k, "A volume reserved less than one page for that blob"};

        volume_t& volume = router_->volume(reserved->volume);
        basic_write_pipeline<disk_t> writer {volume.disk(), channel, reserved->medium};
        expected<pooled_buffer_t> block = writer.acquire_buffer();
        if (!block) return block.status();
        std::span<byte_t> const room = block->bytes();
        std::memset(room.data(), 0, static_cast<std::size_t>(written));
        std::memcpy(room.data(), bytes.data(), bytes.size());
        if (status_t const queued = writer.enqueue_write(std::move(*block), reserved->slice(0, written));
            queued.failed())
            return queued;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        return volume.seal(channel, *reserved);
    }

    /** Reads the whole blob @p name into @p into, which must hold its content length. */
    status_t read_blob_(io_channel_t& channel, blob_name_t name, std::span<byte_t> into) const noexcept {
        expected<blob_t> const found = router_->find(name);
        if (!found) return found.status();
        if (into.size() < found->length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        std::uint64_t const read = rounded_up_to(found->length, portable_alignment_k);
        if (read > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        volume_t& volume = router_->volume(found->volume);
        basic_read_pipeline<disk_t> reader {volume.disk(), channel, found->medium};
        if (status_t const offered = reader.enqueue_read(found->slice(0, read), 0); offered.failed()) return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            std::memcpy(into.data(), completed.bytes.data(), static_cast<std::size_t>(found->length));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        return carried;
    }

    /** Writes the catalog out as the database's schema blob, which every change to it does. */
    status_t save_schema_(io_channel_t& channel) noexcept {
        st::basic_vector<collection_record_t> records;
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (records.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_entry_t const& entry = *collections_[index];
            collection_record_t record;
            record.numeric_id = entry.numeric_id;
            std::size_t const named_before = runs.size();
            if (status_t const named = std::visit(
                    [&](auto const& held) noexcept { return record_lsm_tree_runs(held.tree(), runs); }, entry.held);
                named.failed())
                return named;
            record.run_count = runs.size() - named_before;
            if (entry.name.size() > collection_name_max_bytes_k)
                return {status_code_t::capacity_exhausted_k, "A collection name outgrew the catalog's record"};
            std::memcpy(record.name_storage.data(), entry.name.data(), entry.name.size());
            record.name_bytes = static_cast<std::uint16_t>(entry.name.size());
            record.volume = static_cast<std::uint16_t>(entry.volume);
            record.value_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_size;
            record.value_max_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_max_size;
            record.first_level_max_bytes = entry.config.lsm_tree.lsm_level_0.max_bytes;
            record.level_enlarge_factor = entry.config.lsm_tree.level_enlarge_factor;
            record.cache_limit = entry.config.cache_limit;
            record.write_buffer_max_bytes = entry.config.write_buffer_max_bytes;
            record.write_buffer_max_elements = entry.config.write_buffer_max_elements;
            if (records.push_back(std::move(record)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        }

        std::uint64_t const version = database_schema_version_k;
        st::basic_vector<byte_t> image;
        if (image.resize(fields_storage_requirements(version, records, runs)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        if (status_t const laid = save_fields(bytes_span_t {image}, version, records, runs); laid.failed()) return laid;
        return write_blob_(channel, database_schema_name(), blob_kind_t::schema_k, std::span<byte_t const> {image});
    }

    /** Opens every collection the schema blob names, which is what a mount of an older database does. */
    status_t load_schema_(io_channel_t& channel) noexcept {
        expected<blob_t> const found = router_->find(database_schema_name());
        if (!found) return found == status_code_t::not_found_k ? status_t {} : found.status();

        st::basic_vector<byte_t> image;
        if (image.resize(static_cast<std::size_t>(found->length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the catalog into"};
        if (status_t const read = read_blob_(channel, database_schema_name(), std::span<byte_t> {image}); read.failed())
            return read;

        std::uint64_t version = 0;
        st::basic_vector<collection_record_t> records;
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (status_t const parsed = load_fields(bytes_view_t {image}, version, records, runs); parsed.failed())
            return parsed;
        if (version != database_schema_version_k)
            return {status_code_t::unsupported_k, "The catalog follows another schema version"};

        if (collections_.reserve(records.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the collections the catalog names"};
        std::size_t named = 0;
        for (std::size_t index = 0; index != records.size(); ++index) {
            if (records[index].run_count > runs.size() - named)
                return {status_code_t::malformed_k, "The catalog names more runs than it carries"};
            std::size_t const count = static_cast<std::size_t>(records[index].run_count);
            if (status_t const opened = adopt_(channel, records[index],
                                               std::span<lsm_tree_run_record_t const> {runs.data() + named, count});
                opened.failed())
                return opened;
            named += count;
        }
        if (named != runs.size()) return {status_code_t::malformed_k, "The catalog carries runs no collection claims"};
        return {};
    }

    /** Opens the collection @p record describes over the runs @p runs names, and takes it into the catalog. */
    status_t adopt_(io_channel_t& channel, collection_record_t const& record,
                    std::span<lsm_tree_run_record_t const> runs) noexcept {
        if (static_cast<std::size_t>(record.volume) >= router_->size())
            return {status_code_t::malformed_k, "The catalog names a volume this mount did not open"};

        collection_config_t const config = record.config();
        value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
        expected<any_collection_t> mounted = mount_collection_(router_->volume(static_cast<volume_id_t>(record.volume)),
                                                               config, layout);
        if (!mounted) return mounted.status();

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->numeric_id = record.numeric_id;
        if (!entry->name.try_assign(record.name()))
            return {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->config = config;
        entry->volume = static_cast<volume_id_t>(record.volume);
        entry->held = std::move(*mounted);
        if (status_t const restored = std::visit(
                [&](auto& held) noexcept { return held.tree().restore(channel, runs); }, entry->held);
            restored.failed())
            return restored;
        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for another collection"};
        return {};
    }

    /** What a compaction's hook is handed, which is the database and the channel it records through. */
    struct schema_write_t {

        /** The database whose catalog the hook writes out. */
        basic_database* store = nullptr;

        /** The channel the schema blob is staged and sealed on, held by the compacting thread. */
        io_channel_t* channel = nullptr;
    };

    /** Writes the catalog out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        schema_write_t const& writing = *static_cast<schema_write_t const*>(context);
        return writing.store->save_schema_(*writing.channel);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The budget every collection and every transaction of this database charges. */
    resources_shared_ptr_t resources_;

    /** The transactions of this database, which names this store for as long as it lives. */
    registry_t transactions_;

    /** The volumes every run lands on, null until the database is mounted. */
    router_t* router_ = nullptr;

    /** The queue pairs every call that moves bytes takes one of, null until the database is mounted. */
    channel_pool_t* channels_ = nullptr;

    /** The write-buffer bounds a transaction takes when its caller names none. */
    database_config_t config_ {};

    /** Every collection the catalog names, pinned so a snapshot may point into one. */
    st::basic_vector<entry_pointer_t> collections_;

    /** The two published catalogs, of which one is live and the other is the next publication's room. */
    std::array<catalog_view_t, 2> catalogs_ {};

    /** Which of the two published catalogs is live, stored with release and read with acquire. */
    std::int32_t mutable live_catalog_ = 0;

    /** How many catalogs have been published, which is what lets a caller memoize a lookup. */
    std::uint64_t mutable catalog_version_ = 0;

    /** Readers counted into each published catalog, which a publication waits on before it retires one. */
    std::array<readers_count_t, 2> mutable catalog_readers_ {0, 0};

    /** Zero while nobody is inside a collection, one while a caller is. */
    claim_word_t mutable claim_ = 0;

    /** The claim a staged transaction holds from its stage until its commit. */
    claim_guard_t write_claim_;

    /** The channel that transaction writes on, which every applied key of its commit uses. */
    channel_guard_t write_channel_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma region Materialization

/**
 *  @brief The database of a host that keeps its volumes in files, which is the shape the C ABI names.
 *
 *  The disk is fixed here rather than templated because the layer above addresses a database by its
 *  key alone; that disk holds whichever file transport the mount probed, so one materialization
 *  serves the ring and the blocking floor alike, and a host on another medium instantiates
 *  @c basic_database over that volume itself.
 */
template <typename stored_key_>
class database final : public basic_database<stored_key_, basic_extent_volume<any_files_disk_t>> {
  public:

    /** The generic database this one materializes. */
    using base_t = basic_database<stored_key_, basic_extent_volume<any_files_disk_t>>;

    using base_t::base_t;
};

#pragma endregion Materialization

} // namespace unum::ustore::nvme
