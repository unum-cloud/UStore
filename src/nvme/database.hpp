/**
 *  @file src/nvme/database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The state, handles, accesses, collections, transactions and snapshots of the NVMe engine, which its calls of
 *      every modality build on.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`, `std::sort`
#include <array>       // `std::array`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <optional>    // `std::optional`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_reference_t`
#include <utility>     // `std::in_range`, `std::make_index_sequence`, `std::move`
#include <variant>     // `std::variant`, `std::visit`, `std::variant_size_v`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `strided`
#include "ustore/strings.h"

#include "ustore/blobs/nvme_database.hpp"              // `basic_database`, `lookup_result_t`
#include "ustore/blobs/nvme_transactions.hpp"          // `watch_schedule_t`
#include "ustore/blobs/nvme_tree.hpp"                  // `user_collection_config_t`, `validate`
#include "ustore/blobs/vram_nvidia_compaction.hpp"     // `cuda_compactor_t`
#include "ustore/docs/nvme_store.hpp"                  // `docs::nvme_store`
#include "ustore/graph/nvme_store.hpp"                 // `graph::nvme_store`
#include "ustore/graph/vram_nvidia_adjacency_fold.hpp" // `vram_nvidia_adjacency_fold_t`
#include "ustore/pages/nvme_store.hpp"                 // `pages::nvme_store`
#include "ustore/strings/nvme_store.hpp"               // `strings::nvme_store`, `nvme_transaction`, `strings_page_t`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`, `join_path`, `path_max_bytes_k`, `remove_file`
#include "ustore/utilities/nvme_mounts.hpp" // `resources_config_t`, `open_resources`, `mounted_storage`, `addresses_of`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `arena_copy`, `growing_strings`, `growth_status`

#include "engine.hpp"             // `engine_t`, `ustore_*_handle_t`, `state_of`, `view_t`, `nvme_placement_of`
#include "lib.hpp"                // `cpu_class_t`, `atomic_ref_for_cpu_class`, `waiter_of`, `strings_kit_of`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/outputs.hpp"  // `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::nvme {
using namespace unum::ustore::blobs;
using namespace unum::ustore::strings;
using namespace unum::ustore::utilities;

#pragma region Store

/** The key every collection of this engine stores: the 16 bytes of the C ABI's key, ordered like @c memcmp. */
using stored_key_t = st::key128_t;

/** The mount the volumes of a store of @p cpu_class_ are opened over, which lends it its router and channels. */
template <cpu_class_t cpu_class_>
using storage_mount_of =
    typename mounted_storage<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>::files_mount_t;

/** The cuFile disk a store of @p cpu_class_ reads its run sections into device memory through. */
template <cpu_class_t cpu_class_>
using sections_disk_of =
    linux_cufile_files_disk<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>;

/** What reads the sections of a store of @p cpu_class_'s runs into device memory, over a cuFile disk of its own. */
template <cpu_class_t cpu_class_>
using sections_reader_of = ring_sections_reader<sections_disk_of<cpu_class_>, waiter_of<cpu_class_>,
                                                typename storage_mount_of<cpu_class_>::volume_t>;

/** What composes the half-edge changes of a graph's merge on a device: the CUDA tower, which a build without a CUDA
 *  compiler never reaches, as it makes no device compactor. */
using graph_device_fold_t = graph::vram_nvidia_adjacency_fold_t;

/** The store of a blobs collection, and of the collections system collection, whose values follow
 *  @p value_layout_. */
template <cpu_class_t cpu_class_, value_layout_t value_layout_>
using blobs_store_of =
    blobs::nvme_store<key_slot<stored_key_t, value_layout_>, typename storage_mount_of<cpu_class_>::volume_t,
                      atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, last_write_fold_t,
                      blobs::no_device_fold_t>;

/** The store of a graph collection. */
template <cpu_class_t cpu_class_>
using graph_store_of =
    graph::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                      atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, graph_device_fold_t>;

/** The store of a docs collection. */
template <cpu_class_t cpu_class_>
using docs_store_of = docs::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                       atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                       blobs::no_device_fold_t>;

/** The store of a pages collection. */
template <cpu_class_t cpu_class_>
using pages_store_of = pages::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                         atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                         blobs::no_device_fold_t>;

/** The store of a strings collection, keyed by whole strings. */
template <cpu_class_t cpu_class_>
using strings_store_of = strings::nvme_store<typename storage_mount_of<cpu_class_>::volume_t,
                                             atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                             strings_kit_of<cpu_class_>>;

/** The tag a collection holding @p modality with values of @p layout persists, naming its store whatever order
 *  @c basic_database_of lists its stores in: the modality in the high byte and the value layout in
 *      the low one. */
constexpr std::uint16_t store_tag_of(ustore_modality_t modality, value_layout_t layout) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(modality) << 8 | static_cast<unsigned>(layout));
}

/** What the collections of the store @p store_tag names hold. */
constexpr ustore_modality_t modality_of(std::uint16_t store_tag) noexcept {
    return static_cast<ustore_modality_t>(store_tag >> 8);
}

/** The tag of every store of @c basic_database_of, in the order it lists them. */
inline constexpr std::uint16_t store_tags_k[] {
    store_tag_of(ustore_modality_strings_k, value_layout_t::uuid_k),
    store_tag_of(ustore_modality_graph_k, value_layout_t::variable_width_k),
    store_tag_of(ustore_modality_docs_k, value_layout_t::variable_width_k),
    store_tag_of(ustore_modality_pages_k, value_layout_t::variable_width_k),
    store_tag_of(ustore_modality_blobs_k, value_layout_t::inline_12_bytes_k),
    store_tag_of(ustore_modality_blobs_k, value_layout_t::inline_44_bytes_k),
    store_tag_of(ustore_modality_blobs_k, value_layout_t::fixed_width_k),
    store_tag_of(ustore_modality_blobs_k, value_layout_t::variable_width_k),
};

/** Where among the stores of @c basic_database_of each tag's store sits. */
struct store_tags_t {

    /** The index of the store @p store_tag names, or the count of the stores for a tag naming none. */
    static constexpr std::size_t index_of(std::uint16_t store_tag) noexcept {
        std::size_t index = 0;
        while (index != std::size(store_tags_k) && store_tags_k[index] != store_tag) ++index;
        return index;
    }
};

/** The database every call of @p cpu_class_ runs against, whose volumes live in files, owning its words through the
 *  class's atomic reference and waiting through its waiter, and whose collections live in the store their tag names,
 *  a pair of modality and value layout no store keeps being refused. */
template <cpu_class_t cpu_class_>
using basic_database_of =
    basic_database<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                   atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, store_tags_t,
                   strings_store_of<cpu_class_>, graph_store_of<cpu_class_>, docs_store_of<cpu_class_>,
                   pages_store_of<cpu_class_>, blobs_store_of<cpu_class_, value_layout_t::inline_12_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_44_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::fixed_width_k>,
                   blobs_store_of<cpu_class_, value_layout_t::variable_width_k>>;

static_assert(
    []<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
        using stores_t = typename basic_database_of<cpu_class_t::serial_k>::any_collection_t;
        return sizeof...(indices_) == std::variant_size_v<stores_t> &&
               ((static_cast<std::uint16_t>(
                     std::variant_alternative_t<indices_, stores_t>::key_slot_t::value_layout_k) ==
                 (store_tags_k[indices_] & 0xFF)) &&
                ...);
    }(std::make_index_sequence<std::size(store_tags_k)> {}),
    "Every store of the database has its tag, in the order it lists them");

/** One transaction over the collections of that database. */
template <cpu_class_t cpu_class_>
using database_transaction_of = typename basic_database_of<cpu_class_>::transaction_t;

/** The strings one transaction of that database writes and watches, joined to it as a share. */
template <cpu_class_t cpu_class_>
using strings_transaction_of = nvme_transaction<basic_database_of<cpu_class_>, strings_store_of<cpu_class_>>;

/** @p uuid as the collection ID the C ABI addresses a collection by. */
constexpr ustore_collection_id_t collection_id_of(uuid_t const& uuid) noexcept {
    ustore_collection_id_t collection {};
    for (std::size_t index = 0; index != uuid_t::size_bytes_k; ++index)
        collection.uuid.bytes[index] = uuid.bytes[index];
    return collection;
}

/** The collections system collection, keeping every collection's record and name, which no minted
 *  UUIDv7 spells, as its byte 6 is @c 0x7X. */
inline constexpr ustore_collection_id_t collections_collection_k = collection_id_of(collections_collection_uuid_k);

/** The names system collection, a strings collection mapping every collection's name onto its UUID, which no minted
 *  UUIDv7 spells either. */
inline constexpr ustore_collection_id_t names_collection_k = collection_id_of(names_collection_uuid_k);

/** Channels the threads outside an executor share, beyond one per thread of the executor opening the database. */
inline constexpr std::size_t engine_shared_io_channels_k = 4;

/** Bytes one caller's transaction buffers, which it takes eagerly when it opens. */
inline constexpr std::size_t engine_transaction_bytes_k = 4ull * 1024ull * 1024ull;

/** Deltas one caller's transaction buffers at once. */
inline constexpr std::size_t engine_transaction_deltas_k = 16ull * 1024ull;

/** Times a call retries a commit of its own whose watched reads conflicted. */
inline constexpr std::size_t commit_attempts_k = 64;

/** Times a read measures and copies a value that keeps changing its size in between. */
inline constexpr std::size_t read_attempts_k = 8;

/** Keys a clear rewrites per commit, so its transaction stays bounded however large the collection. */
inline constexpr std::size_t clear_window_keys_k = 4096;

/** Merges a compaction pass after a write runs at most, so a pass another thread holds never spins a writer. */
inline constexpr std::size_t compaction_passes_k = 8;

/** The bytes an empty present value points at, as a missing value alone points at none. */
inline constexpr byte_t empty_value_bytes_k[1] {};

/** The stored key holding the 16 bytes of @p uuid. */
inline stored_key_t stored_of(ustore_uuid_t const& uuid) noexcept {
    return stored_key_t::from_bytes(std::as_bytes(std::span<std::uint8_t const, 16> {uuid.bytes}));
}

/** The key of the C ABI whose 16 bytes @p stored holds. */
inline ustore_key_t key_of(stored_key_t const& stored) noexcept {
    ustore_key_t key;
    stored.to_bytes(std::as_writable_bytes(std::span<std::uint8_t, 16> {key.uuid.bytes}));
    return key;
}

/** The shape of a strings collection: @p shape, every value of which is one 16-byte key. */
inline collection_config_t strings_shape_of(collection_config_t shape) noexcept {
    sorted_run_config_t& values = shape.lsm_tree.lsm_level_0.sorted_run;
    values.value_size = sizeof(ustore_uuid_t);
    values.value_max_size = 0;
    return shape;
}

/** Whether @p collection is one the engine keeps for itself, which no call of the C ABI may address. */
inline bool reserved(ustore_collection_id_t const& collection) noexcept {
    return collection == collections_collection_k || collection == names_collection_k;
}

/** The shape every collection of a database takes unless its open names another. */
inline user_collection_config_t engine_collection_defaults() noexcept {
    user_collection_config_t config;
    config.cache_limit = engine_collection_bytes_k;
    config.memory_runs_max_deltas = engine_memory_runs_deltas_k;
    config.memory_runs_max_bytes = engine_collection_bytes_k;
    config.level_enlarge_factor = engine_level_enlarge_factor_k;
    config.value_max_size = engine_value_max_size_k;
    return config;
}

#pragma endregion Store

#pragma region Handles

/** The calls a handle of @p cpu_class_ is served by, and the open that hands such handles out; both are defined below,
 *  so only the class's own unit compiles them. */
template <cpu_class_t cpu_class_>
struct nvme_variant {

    /** The StringZilla kernels this class runs its string work through. */
    using strings_kit_t = strings_kit_of<cpu_class_>;

    /** The database verbs of this class. */
    static database_verbs_t const database_verbs_k;

    /** The unchecked whole-value reads, removals and key scans of this class, through each collection's own store,
     *  which its docs, graph and pages verbs reach after their resolver. */
    static blobs_verbs<home_t::host_k> const values_verbs_k;

    /** The verbs of this class's blobs collections. */
    static blobs_verbs<home_t::host_k> const blobs_verbs_k;

    /** The verbs of this class's strings collections. */
    static strings_verbs<home_t::host_k> const strings_verbs_k;

    /** The verbs of this class's documents collections. */
    static docs_verbs<home_t::host_k> const docs_verbs_k;

    /** The verbs of this class's graph collections. */
    static graph_verbs<home_t::host_k> const graph_verbs_k;

    /** The verbs of this class's pages collections. */
    static pages_verbs<home_t::host_k> const pages_verbs_k;

    /** The table every handle the open of this class hands out is stamped with. */
    static engine_t const engine_k;

    /** Implements @c ustore_database_open_nvme over a store of this class. */
    static status_t open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept;
};

/**
 *  @brief The device merges of a store of @p cpu_class_: an executor of their own on the device of the caller's, what
 *      reads run sections into device memory over a cuFile disk of its own, and the compactor over both.
 *
 *  Sections read by DMA where the executor's GPUDirect Storage mode allows it and cuFile reaches the device, since
 *  compat mode bounces through host pages, slower than uploading the indexes the runs already hold in memory. The
 *  members die in reverse, the compactor first, so it frees its scratch while the reader and the executor live.
 */
template <cpu_class_t cpu_class_>
class device_compaction {
  public:

    /** The volume the reader names run files through. */
    using volume_t = typename storage_mount_of<cpu_class_>::volume_t;

    device_compaction() noexcept = default;
    device_compaction(device_compaction const&) = delete;
    device_compaction& operator=(device_compaction const&) = delete;

    /** Spawns the executor on the device @p compactions computes on, and on an NVIDIA one opens
     *  the reader over @p volume, mounted over the data directories of @p resources, and binds
     *  the compactor over both; an AMD device binds no compactor yet, so a merge placed on it
     *  answers @c unsupported_k. */
    status_t open(ustore_executor_handle_t const& compactions, resources_config_t const& resources,
                  volume_t const& volume) noexcept {
        hip_executor_t const* const amd = std::get_if<hip_executor_t>(&compactions.device);
        cuda_executor_t const* const caller = std::get_if<cuda_executor_t>(&compactions.device);
        if (!caller && !amd)
            return {status_code_t::device_k, "The compactions need an executor of ustore_executor_for_vram"};
        gpu_runtime_t& runtime = gpu_runtime();
        gpu_drivers_t const& drivers = runtime.drivers;
        gpu_device_t twin {};
        twin.vendor = caller ? cuda_executor_t::vendor_k : hip_executor_t::vendor_k;
        twin.vendor_ordinal = static_cast<std::uint32_t>(caller ? caller->device() : amd->device());
        if (status_t const spawned = runtime.spawn(twin, caller ? caller->gpu_class() : amd->gpu_class(), nullptr,
                                                   caller ? caller->scratch_limit() : amd->scratch_limit(),
                                                   caller ? caller->gds() : ustore_gds_compatibility_k, executor_);
            spawned.failed())
            return spawned;
        if (amd) return {};
        cuda_executor_t* const executor = &std::get<cuda_executor_t>(executor_);
        if (status_t const opened = open_sections(drivers.cufile, executor->gds(), resources, volume); opened.failed())
            return opened;
        expected<cuda_compactor_t> made = cuda_compactor_t::make(
            *executor, sections_ ? device_sections_reader_t {&sections_.value()} : device_sections_reader_t {});
        if (!made) return made.status();
        compactor_.emplace(std::move(*made));
        return {};
    }

    /** The compactor every merge runs on, once @c open bound it. */
    cuda_compactor_t* compactor() noexcept { return compactor_ ? &compactor_.value() : nullptr; }

  private:

    /** Opens the reader over every raw medium of @p resources, or over the run files of @p volume
     *  on directories, where @p gds lets bytes move by DMA and cuFile reaches the device,
     *  refusing @c ustore_gds_direct_k where it does not. */
    status_t open_sections(cufile_api_t const& cufile, ustore_gds_t gds, resources_config_t const& resources,
                           volume_t const& volume) noexcept {
        if (gds == ustore_gds_compatibility_k) return {};
        if (sections_disk_of<cpu_class_>::probe(cufile) != linux_cufile_probe_t::direct_k) {
            if (gds == ustore_gds_direct_k)
                return {status_code_t::device_k, "GPUDirect Storage needs the nvidia_fs module, which is absent"};
            return {};
        }
        auto [addresses, addresses_status] = addresses_of(resources);
        if (addresses_status.failed()) return addresses_status;
        linux_cufile_disk_config_t config;
        config.gds = ustore_gds_direct_k;
        if (addresses.data_directories[0].kind == data_address_kind_t::directory_k) {
            // Every run a merge reads is a file of its own, registered while that merge runs.
            config.media_limit = merged_run_files_k;
            auto [disk, disk_status] = sections_disk_of<cpu_class_>::make(cufile, config);
            if (disk_status.failed()) return disk_status;
            expected<sections_reader_of<cpu_class_>> reader = sections_reader_of<cpu_class_>::make(std::move(disk),
                                                                                                   volume);
            if (!reader) return reader.status();
            sections_.emplace(std::move(*reader));
            return {};
        }
        std::size_t const media_count = addresses.data_directories.size();
        if (media_count > config.media_limit) config.media_limit = static_cast<std::uint16_t>(media_count);
        auto [disk, disk_status] = sections_disk_of<cpu_class_>::make(cufile, config);
        if (disk_status.failed()) return disk_status;
        st::vector<std::array<char, path_max_bytes_k>> paths;
        st::vector<medium_options_t> media;
        if (paths.resize(media_count) != st::status_t::success_k ||
            media.resize(media_count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "The compactions could not track their media"};
        // Raw media are read in data directory order, which is the index a run's extent names.
        for (std::size_t index = 0; index != media_count; ++index) {
            std::string_view const location = addresses.data_directories[index].location;
            if (location.size() >= path_max_bytes_k)
                return {status_code_t::invalid_argument_k, "A raw medium's path outgrew the platform's limit"};
            std::memcpy(paths[index].data(), location.data(), location.size());
            paths[index][location.size()] = '\0';
            media[index].name = paths[index].data();
            media[index].access = medium_access_t::read_only_k;
        }
        expected<sections_reader_of<cpu_class_>> reader = sections_reader_of<cpu_class_>::make(
            std::move(disk), std::span<medium_options_t const> {media});
        if (!reader) return reader.status();
        sections_.emplace(std::move(*reader));
        return {};
    }

    /** Run files one merge on a directory volume reads at most. */
    static constexpr std::uint16_t merged_run_files_k = 1024;

    std::variant<std::monostate, cuda_executor_t, hip_executor_t> executor_;
    std::optional<sections_reader_of<cpu_class_>> sections_;
    std::optional<cuda_compactor_t> compactor_;
};

/** Everything one open database holds, behind its database handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct database_state {

    /** The budgets and the mount the store borrows its router and channels from. */
    resources_shared_ptr<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>> resources;

    /** What every device merge of the store runs on, empty for host merges, outliving the store. */
    std::optional<device_compaction<cpu_class_>> compaction;

    /** The store itself, unmounted before the mount underneath it is torn down. */
    std::unique_ptr<basic_database_of<cpu_class_>> store;

    /** The shape every collection this database creates takes. */
    collection_config_t collection_shape;

    /** The buffer every caller's transaction takes. */
    transaction_config_t transaction_shape;

    /** The executor that opened the database, whose threads read on a channel and an epoch slot of their own, or
     *  null; compared by address alone, as one made where a freed one was has threads as exclusive, and any index
     *  past the threads the database counted reads as an outside thread's would. */
    ustore_executor_handle_t const* executor = nullptr;

    /** Held by whoever changes both system collections, so a name is checked and taken in one step. */
    std::int32_t collections_claim = 0;

    /** Snapshots not yet freed, through the class's atomic reference. */
    std::size_t open_snapshots = 0;

    /** Collections the collections system collection no longer lists whose runs an open snapshot may still read,
     *  dropped once the last snapshot is freed; guarded by @c collections_claim. */
    st::vector<ustore_collection_id_t> retired_collections;

    database_state() noexcept = default;
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;

    /** Flushes and unmounts the store, then the storage under it, before letting both go, since
     *  a collection or buffer may keep the resources alive past the database. */
    ~database_state() noexcept {
        if (store) { [[maybe_unused]] status_t const unmounted = store->unmount(); }
        store.reset();
        if (resources) { [[maybe_unused]] status_t const released = resources->storage.unmount(); }
        resources.reset();
    }
};

/** What a transaction holds, behind its transaction handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct transaction_state {

    /** The database the transaction runs against. */
    database_state<cpu_class_>& state;

    /** The strings the transaction writes and watches, joined to it, and outliving it. */
    strings_transaction_of<cpu_class_> strings;

    /** The transaction itself, retired when the state dies. */
    typename basic_database_of<cpu_class_>::transaction_unique_ptr_t held;

    /** Whether the caller staged it, which a rollback withdraws and a commit consumes, refusing writes meanwhile. */
    bool staged = false;

    transaction_state(database_state<cpu_class_>& owner,
                      typename basic_database_of<cpu_class_>::transaction_unique_ptr_t&& opened) noexcept
        : state(owner), strings(*owner.store), held(std::move(opened)) {
        strings.join(*held);
    }
};

/** What a snapshot holds, behind its snapshot handle of the C ABI, pinning the stamp it reads at in the store's ledger
 *  until freed. */
template <cpu_class_t cpu_class_>
struct snapshot_state {

    /** The database the snapshot reads. */
    database_state<cpu_class_>& state;

    /** The pin, which keeps every version a read at its stamp needs out of the reach of a flush and a merge. */
    typename basic_database_of<cpu_class_>::pin_t pin;

    explicit snapshot_state(database_state<cpu_class_>& owner) noexcept : state(owner) {}
};

/** Holds both system collections of one database for the life of the guard, waiting while another change holds
 *  them. */
template <cpu_class_t cpu_class_>
class collections_guard {
    std::int32_t& claim_;

  public:
    explicit collections_guard(database_state<cpu_class_>& state) noexcept : claim_(state.collections_claim) {
        atomic_ref_of<cpu_class_, std::int32_t> const word {claim_};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            waiter_of<cpu_class_> {}(&claim_, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }
    collections_guard(collections_guard const&) = delete;
    collections_guard& operator=(collections_guard const&) = delete;
    ~collections_guard() noexcept {
        atomic_ref_of<cpu_class_, std::int32_t> {claim_}.store(0, std::memory_order_release);
    }
};

/** The state of the database a call addresses. */
template <cpu_class_t cpu_class_, typename arguments_type_>
database_state<cpu_class_>& database_state_of(arguments_type_ const& arguments) noexcept {
    return *state_of<database_state<cpu_class_>>(database_of(arguments));
}

/** Opens a transaction of @p state whose buffer @p shape bounds. */
template <cpu_class_t cpu_class_>
expected<typename basic_database_of<cpu_class_>::transaction_unique_ptr_t> begin_transaction(
    database_state<cpu_class_>& state, transaction_config_t const& shape) noexcept {
    return state.store->create_transaction(shape);
}

#pragma endregion Handles

#pragma region Access

/** Where one call reads and writes: through a transaction, the caller's or its own, or at the head for reads. */
template <cpu_class_t cpu_class_>
struct call_access {

    /** The database the call runs against. */
    database_state<cpu_class_>& state;

    /** The transaction it reads and writes through, or null to read at the head. */
    database_transaction_of<cpu_class_>* transaction = nullptr;

    /** Whether the transaction watches what the call reads and writes. */
    watch_schedule_t schedule = watch_schedule_t::never_k;

    /** Where the call keeps the values it read until it exports them. */
    ustore_arena_t scratch = nullptr;

    /** The stamp a call outside a transaction reads at: a snapshot's, or the head's. */
    transaction_stamp_t stamp = head_stamp_k;

    /** The thread of the executor that opened the database running the call, which reads on its own channel and
     *  epoch slot, or @c outside_executor_k for any other thread. */
    std::size_t thread_index = outside_executor_k;

    /** The strings the transaction writes and watches, null with no transaction. */
    strings_transaction_of<cpu_class_>* strings = nullptr;
};

/** The schedule the watches of a call asking for @p watch take. */
constexpr watch_schedule_t schedule_of(ustore_watch_t watch) noexcept {
    return watch == ustore_watch_keys_k ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k;
}

/** The thread index a call running on @p view's thread reads @p state with. */
template <cpu_class_t cpu_class_>
std::size_t thread_index_of(database_state<cpu_class_> const& state, view_t const& view) noexcept {
    return view.executor && view.executor == state.executor ? view.thread_index : outside_executor_k;
}

/** The access of a call reading through @p view: its transaction, its snapshot's stamp, or the head. */
template <cpu_class_t cpu_class_>
call_access<cpu_class_> access_of(database_state<cpu_class_>& state, view_t const& view) noexcept {
    std::size_t const thread_index = thread_index_of(state, view);
    if (view.snapshot)
        return {state,
                nullptr,
                watch_schedule_t::never_k,
                view.scratch,
                state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp(),
                thread_index};
    if (!view.transaction) return {state, nullptr, watch_schedule_t::never_k, view.scratch, head_stamp_k, thread_index};
    transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
    return {state,        transaction.held.get(), schedule_of(view.watch), view.scratch,
            head_stamp_k, thread_index,           &transaction.strings};
}

/** Watches @p keys of @p collection through the transaction of @p access, when it watches at all. */
template <cpu_class_t cpu_class_>
status_t watch_keys(call_access<cpu_class_> const& access, collection_id_t collection,
                    std::span<stored_key_t const> keys) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k || keys.empty()) return {};
    return access.transaction->template watch_more<watch_schedule_t::on_commit_k>(collection, keys);
}

/** Watches every key of @p collection from @p lower to @p upper through the transaction of @p access, when it watches
 *  at all, so a commit landing a key the walk over them did not find conflicts too. */
template <cpu_class_t cpu_class_>
status_t watch_range(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                     stored_key_t upper) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k) return {};
    return access.transaction->template watch_range<watch_schedule_t::on_commit_k>(collection, lower, upper);
}

/** Copies @p size bytes into the scratch of @p access through @p copy, answering the present value they hold. */
template <cpu_class_t cpu_class_, typename copy_type_>
expected<value_view_t> copied_value(call_access<cpu_class_> const& access, stored_value_size_t size,
                                    copy_type_&& copy) noexcept {
    if (size == 0) {
        if (status_t const copied = copy(std::span<byte_t> {}); copied.failed()) return copied;
        return value_view_t {empty_value_bytes_k, std::size_t {0}};
    }
    auto [bytes, allocate_status] = arena_allocate<byte_t>(access.scratch, size);
    if (allocate_status.failed()) return allocate_status;
    if (status_t const copied = copy(bytes); copied.failed()) return copied;
    return value_view_t {bytes};
}

/** The value a transaction's change @p own_delta of a key in @p collection folds @p committed
 *  into, through the fold of the store holding it, copied into the scratch of @p access, or a
 *  missing value where the fold leaves none. */
template <cpu_class_t cpu_class_>
expected<value_view_t> fold_own_change(call_access<cpu_class_> const& access, collection_id_t collection,
                                       value_view_t committed, delta_view_t const& own_delta) noexcept {
    // A present value of no bytes still views a byte, so a fold tells it from a missing one
    std::span<byte_t const> base;
    if (committed)
        base = committed.size() ? std::span<byte_t const> {committed.begin(), committed.size()}
                                : std::span<byte_t const> {empty_value_bytes_k, 0};
    expected<value_view_t> folded = value_view_t {};
    status_t const visited = access.state.store->visit_store(
        collection,
        [&]<typename store_type_>(store_type_&) noexcept -> status_t {
            arena_allocator<byte_t> arena {access.scratch};
            auto [applied, apply_status] = typename store_type_::fold_t {}.apply(
                base, std::span<delta_view_t const> {&own_delta, 1}, arena);
            if (apply_status == status_code_t::not_found_k) return {};
            if (apply_status.failed()) return apply_status;
            folded = copied_value(access, static_cast<stored_value_size_t>(applied.size()),
                                  [&](std::span<byte_t> into) noexcept {
                                      if (!into.empty()) std::memcpy(into.data(), applied.data(), into.size());
                                      return status_t {};
                                  });
            return {};
        },
        access.thread_index);
    if (visited.failed()) return visited;
    return folded;
}

/**
 *  @brief The value of @p key in @p collection as @p access sees it, copied into its scratch, or
 *      a missing value.
 *
 *  A transaction's own write outranks the store, its own removal hides the store's value, and its
 *  own change folds over the store's value. A read through a watching transaction watches the key,
 *  its absence included, so a commit landing on it in between conflicts.
 */
template <cpu_class_t cpu_class_>
expected<value_view_t> read_value(call_access<cpu_class_> const& access, collection_id_t collection,
                                  stored_key_t key) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    database_transaction_of<cpu_class_>* const transaction = access.transaction;
    delta_view_t own_delta;
    bool const changed = transaction && transaction->uncommitted_delta(collection, key, own_delta) &&
                         own_delta.kind == delta_kind_t::change_k;
    if (transaction) {
        lookup_result_t const own_status = transaction->lookup_uncommitted(collection, key);
        if (own_status == lookup_result_t::removed_k) return value_view_t {};
        if (own_status == lookup_result_t::written_k && !changed) {
            stored_value_size_t size = 0;
            if (status_t const measured = transaction->stored_value_size_uncommitted(collection, key, size);
                measured.failed())
                return measured;
            return copied_value(access, size, [&](std::span<byte_t> into) noexcept {
                stored_value_size_t copied = 0;
                return transaction->select_uncommitted(collection, key, into, copied);
            });
        }
    }

    // A value may change its size between the measurement and the copy, which the copy refuses rather than truncates
    for (std::size_t attempt = 0; attempt != read_attempts_k; ++attempt) {
        stored_value_size_t size = 0;
        std::size_t const thread_index = access.thread_index;
        status_t const measured = transaction
                                      ? transaction->stored_value_size_committed(collection, key, size, thread_index)
                                      : store.stored_value_size(collection, key, size, access.stamp, thread_index);
        bool const missing = measured == status_code_t::not_found_k;
        if (measured.failed() && !missing) return measured;
        if (!missing) {
            expected<value_view_t> read = copied_value(access, size, [&](std::span<byte_t> into) noexcept {
                stored_value_size_t copied = 0;
                if (!transaction) return store.select(collection, key, into, copied, access.stamp, thread_index);
                if (access.schedule == watch_schedule_t::never_k)
                    return transaction->template select_committed<watch_schedule_t::never_k>(collection, key, into,
                                                                                             copied, thread_index);
                return transaction->template select_committed<watch_schedule_t::on_commit_k>(collection, key, into,
                                                                                             copied, thread_index);
            });
            if (read && changed) return fold_own_change(access, collection, *read, own_delta);
            if (read) return read;
            if (read.status() == status_code_t::invalid_argument_k) continue;
            if (read.status() != status_code_t::not_found_k) return read.status();
        }
        stored_key_t const absent[1] {key};
        if (status_t const watched = watch_keys(access, collection, absent); watched.failed()) return watched;
        if (changed) return fold_own_change(access, collection, value_view_t {}, own_delta);
        return value_view_t {};
    }
    return status_t {status_code_t::contended_k, "A value kept changing its size while it was read"};
}

/**
 *  @brief The values of @p keys in @p collection at the stamp of @p access, outside a transaction,
 *      as one batched read of the store with as many reads in flight as its channel takes, each
 *      copied into the scratch of @p access, a missing key reading as a missing value.
 *
 *  Sizes are measured first, from the index in memory, so every value lands in a room of its own;
 *  a value whose size changed before its bytes arrived is read again alone, as @c read_value does.
 */
template <cpu_class_t cpu_class_>
status_t read_values_batched(call_access<cpu_class_> const& access, collection_id_t collection,
                             std::span<stored_key_t const> keys, std::span<value_view_t> values) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::size_t const count = keys.size();
    auto [sizes, sizes_status] = arena_allocate<stored_value_size_t>(access.scratch, count);
    if (sizes_status.failed()) return sizes_status;
    auto [delivered, delivered_status] = arena_allocate<std::uint64_t>(access.scratch, count);
    if (delivered_status.failed()) return delivered_status;
    auto [results, results_status] = arena_allocate<lookup_result_t>(access.scratch, count);
    if (results_status.failed()) return results_status;
    auto [present, present_status] = arena_allocate<bool>(access.scratch, count);
    if (present_status.failed()) return present_status;
    std::uint64_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        status_t const measured = store.stored_value_size(collection, keys[index], sizes[index], access.stamp,
                                                          access.thread_index);
        if (measured.failed() && measured != status_code_t::not_found_k) return measured;
        present[index] = measured.succeeded();
        if (!present[index]) sizes[index] = 0;
        delivered[index] = 0;
        total += sizes[index];
    }
    auto [room, room_status] = arena_allocate<byte_t>(access.scratch, static_cast<std::size_t>(total));
    if (room_status.failed()) return room_status;
    auto [rooms, rooms_status] = arena_allocate<byte_t*>(access.scratch, count);
    if (rooms_status.failed()) return rooms_status;
    for (std::size_t index = 0, at = 0; index != count; at += sizes[index], ++index) rooms[index] = room.data() + at;

    // Bytes past a value's measured size mark it as changed meanwhile, never written past its room.
    constexpr std::uint64_t changed_k = std::numeric_limits<std::uint64_t>::max();
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        if (delivered[request] == changed_k || offset + bytes.size() > sizes[request]) {
            delivered[request] = changed_k;
            return;
        }
        if (!bytes.empty()) std::memcpy(rooms[request] + offset, bytes.data(), bytes.size());
        delivered[request] += bytes.size();
    };
    if (status_t const read = store.select(collection, keys, std::span<lookup_result_t> {results}, sink, access.stamp,
                                           access.thread_index);
        read.failed())
        return read;

    for (std::size_t index = 0; index != count; ++index) {
        bool const written = results[index] == lookup_result_t::written_k;
        if (written && present[index] && delivered[index] == sizes[index])
            values[index] = sizes[index] ? value_view_t {std::span<byte_t const> {rooms[index], sizes[index]}}
                                         : value_view_t {empty_value_bytes_k, std::size_t {0}};
        else if (!written && !present[index]) values[index] = value_view_t {};
        else {
            auto [value, read_status] = read_value(access, collection, keys[index]);
            if (read_status.failed()) return read_status;
            values[index] = value;
        }
    }
    return {};
}

/** Writes @p value under @p key of @p collection, a missing value removing it: through the transaction of @p access,
 *  or straight into the store for both system collections, which no transaction reaches. */
template <cpu_class_t cpu_class_>
status_t write_value(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key,
                     value_view_t value) noexcept {
    stored_value_view_t const bytes {reinterpret_cast<byte_t const*>(value.data()), value.size()};
    if (!access.transaction) {
        basic_database_of<cpu_class_>& store = *access.state.store;
        return value ? store.insert(collection, key, bytes) : store.remove(collection, key);
    }
    database_transaction_of<cpu_class_>& transaction = *access.transaction;
    bool const watched = access.schedule != watch_schedule_t::never_k;
    if (!value)
        return watched ? transaction.template remove<watch_schedule_t::on_commit_k>(collection, key)
                       : transaction.template remove<watch_schedule_t::never_k>(collection, key);
    return watched ? transaction.template insert<watch_schedule_t::on_commit_k>(collection, key, bytes)
                   : transaction.template insert<watch_schedule_t::never_k>(collection, key, bytes);
}

/** Removes every key of @p collection from @p start up to @p end, exclusive, as one range tombstone
 *  the transaction of @p access commits, watching the range when it watches; the system
 *  collections, which no transaction reaches, take no range. */
template <cpu_class_t cpu_class_>
status_t remove_key_range(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t start,
                          stored_key_t end) noexcept {
    if (!access.transaction) return {status_code_t::invalid_argument_k, "A range removal lands through a transaction"};
    if (!(start < end)) return {};
    database_transaction_of<cpu_class_>& transaction = *access.transaction;
    return access.schedule != watch_schedule_t::never_k
               ? transaction.template remove_range<watch_schedule_t::on_commit_k>(collection, start, end)
               : transaction.template remove_range<watch_schedule_t::never_k>(collection, start, end);
}

/** One write of a transaction to its own key, which a walk merges with the store. */
struct own_write_t {
    stored_key_t key;
    stored_value_size_t size = 0;
    delta_kind_t kind = delta_kind_t::write_k;
};

/**
 *  @brief Hands @p visitor every present key of @p collection from @p from up, or down for
 *      @p direction descending, as @p access sees it, in that order, as <tt>visitor(key, size)</tt>
 *      answering whether the walk carries on.
 *
 *  The store's walk holds the view it loaded, which a flush waits for, so @p visitor collects and calls nothing of the
 *  database.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t from,
              visitor_type_&& visitor, scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    if (!access.transaction)
        return store.scan(
            collection, from,
            [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
                return state != lookup_result_t::written_k || visitor(key, size);
            },
            access.stamp, access.thread_index, direction);

    bool const descending = direction == scan_direction_t::descending_k;
    auto const before = [descending](stored_key_t const& first, stored_key_t const& second) noexcept {
        return descending ? second < first : first < second;
    };

    st::vector<own_write_t, arena_allocator<own_write_t>> own_writes {{access.scratch}};
    status_t listed;
    access.transaction->for_each_uncommitted(
        collection, [&](stored_key_t key, delta_kind_t kind, stored_value_size_t size) noexcept {
            if (before(key, from) || listed.failed()) return;
            listed = growth_status(own_writes.push_back(own_write_t {key, size, kind}));
        });
    if (listed.failed()) return listed;
    // A change folds over the committed value, which tells its size and whether any value is left
    for (own_write_t& write : own_writes) {
        if (write.kind != delta_kind_t::change_k) continue;
        auto [folded, fold_status] = read_value(access, collection, write.key);
        if (fold_status.failed()) return fold_status;
        write.kind = folded ? delta_kind_t::write_k : delta_kind_t::remove_k;
        write.size = static_cast<stored_value_size_t>(folded.size());
    }
    std::sort(own_writes.begin(), own_writes.end(), [&](own_write_t const& first, own_write_t const& second) noexcept {
        return before(first.key, second.key);
    });

    // The transaction's writes outrank the store's version of the same key, and its removals, alone
    // or of a range, hide it
    std::span<range_tombstone<stored_key_t> const> const own_ranges = access.transaction->uncommitted_ranges(
        collection);
    std::size_t next = 0;
    bool carries_on = true;
    auto const emit_own = [&](own_write_t const& write) noexcept {
        if (write.kind != delta_kind_t::remove_k) carries_on = visitor(write.key, write.size);
        return carries_on;
    };
    auto const removed_in_range = [&](stored_key_t const& key) noexcept {
        for (range_tombstone<stored_key_t> const& range : own_ranges)
            if (range.covers(key)) return true;
        return false;
    };
    // The store's side is what the transaction started at, which its reads of single keys answer too
    status_t const walked = store.scan(
        collection, from,
        [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
            for (; next != own_writes.size() && before(own_writes[next].key, key); ++next)
                if (!emit_own(own_writes[next])) return false;
            if (next != own_writes.size() && own_writes[next].key == key) return emit_own(own_writes[next++]);
            if (state != lookup_result_t::written_k || removed_in_range(key)) return true;
            carries_on = visitor(key, size);
            return carries_on;
        },
        access.transaction->start_stamp(), access.thread_index, direction);
    if (walked.failed()) return walked;
    for (; carries_on && next != own_writes.size(); ++next) emit_own(own_writes[next]);
    return {};
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p lower up to @p upper
 *  inclusive, as @p access sees them. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t collect_keys(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                      stored_key_t upper, std::size_t limit, keys_type_& keys) noexcept {
    status_t room;
    status_t const walked = walk(access, collection, lower, [&](stored_key_t key, stored_value_size_t) noexcept {
        if (upper < key) return false;
        room = growth_status(keys.push_back(stored_key_t {key}));
        return room.succeeded() && keys.size() < limit;
    });
    return walked.failed() ? walked : room;
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p upper down to
 *  @p lower inclusive, as @p access sees them, largest first, walking the runs downwards, so a page
 *  reads about its own keys per run. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t collect_keys_descending(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                                 stored_key_t upper, std::size_t limit, keys_type_& keys) noexcept {
    if (limit == 0) return {};
    status_t room;
    status_t const walked = walk(
        access, collection, upper,
        [&](stored_key_t key, stored_value_size_t) noexcept {
            if (key < lower) return false;
            room = growth_status(keys.push_back(stored_key_t {key}));
            return room.succeeded() && keys.size() < limit;
        },
        scan_direction_t::descending_k);
    return walked.failed() ? walked : room;
}

/** Collects into @p keys up to @p limit keys of @p collection from @p lower up to @p upper
 *  inclusive, walking each run on its own with no merge across them, as an unordered scan may list
 *  a key another run removed or repeats. Answers @c contended_k when a flush, a compaction or a
 *  merge rearranged the runs meanwhile, so the caller walks in order instead. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t collect_keys_unordered(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                                stored_key_t upper, std::size_t limit, keys_type_& keys) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    typename basic_database_of<cpu_class_>::pin_t pin;
    transaction_stamp_t const stamp = access.stamp == head_stamp_k ? store.ledger().pin_reader(pin) : access.stamp;
    auto [counted, counted_status] = store.runs_count(collection, stamp, access.thread_index);
    if (counted_status.failed()) return counted_status;
    status_t room;
    for (std::uint64_t run = 0; run != counted.count && keys.size() < limit; ++run) {
        status_t const walked = store.scan_run(
            collection, counted, run, lower,
            [&](stored_key_t key, lookup_result_t state, stored_value_size_t) noexcept {
                if (upper < key) return false;
                if (state == lookup_result_t::removed_k) return true;
                room = growth_status(keys.push_back(stored_key_t {key}));
                return room.succeeded() && keys.size() < limit;
            },
            stamp, access.thread_index);
        if (walked.failed()) return walked;
        if (room.failed()) return room;
    }
    return {};
}

/** The key after @p key, or answers false for the largest key, which has none. */
inline bool advance_key(stored_key_t& key) noexcept {
    if (key == limit<stored_key_t>::max()) return false;
    key = stored_of(key_after(key_of(key)).uuid);
    return true;
}

#pragma endregion Access

#pragma region Maintenance

/** Merges every collection due for a compaction, a bounded number of passes, on the calling thread. */
template <cpu_class_t cpu_class_>
status_t compact_due(database_state<cpu_class_>& state) noexcept {
    for (std::size_t pass = 0; pass != compaction_passes_k && state.store->needs_compaction(); ++pass)
        if (status_t const compacted = state.store->compact(); compacted.failed()) return compacted;
    return {};
}

/** Flushes @p state when @p durability asks for it, then compacts what the write left due. */
template <cpu_class_t cpu_class_>
status_t settle(database_state<cpu_class_>& state, ustore_durability_t durability) noexcept {
    if (durability == ustore_durability_flushed_k)
        if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/**
 *  @brief Commits what @p writes writes through a transaction of its own, retrying conflicts of its watched reads.
 *  @param[in] count The writes @p writes makes at most, sizing the transaction's buffer.
 *  @param[in] bytes The bytes those writes take, a hint the buffer doubles past when @p writes outgrows it.
 *  @param[in] schedule Whether @p writes watches what it reads, which a read-modify-write needs.
 *
 *  @p writes is invoked as <tt>writes(access)</tt> on every attempt, and a failure it answers ends the call.
 */
template <cpu_class_t cpu_class_, typename writes_type_>
status_t commit_alone(database_state<cpu_class_>& state, ustore_arena_t scratch, std::size_t count, std::size_t bytes,
                      watch_schedule_t schedule, writes_type_&& writes) noexcept {
    transaction_config_t shape;
    shape.max_deltas = std::max<std::size_t>(count, 1);
    shape.max_bytes = std::max<std::size_t>(bytes, 1);
    for (std::size_t attempt = 0; attempt != commit_attempts_k; ++attempt) {
        // Declared first, so the transaction it joins dies before it does.
        strings_transaction_of<cpu_class_> strings {*state.store};
        auto [held, begin_status] = begin_transaction(state, shape);
        if (begin_status.failed()) return begin_status;
        strings.join(*held);
        call_access<cpu_class_> const access {state,        held.get(),         schedule, scratch,
                                              head_stamp_k, outside_executor_k, &strings};
        status_t const written = writes(access);
        if (written == status_code_t::capacity_exhausted_k &&
            shape.max_bytes < storage_limits_t::transaction_max_bytes_k) {
            shape.max_bytes *= 2;
            shape.max_deltas = std::min(shape.max_deltas * 2, storage_limits_t::transaction_max_deltas_k);
            continue;
        }
        if (written.failed()) return written;
        status_t const committed = strings.commit(*held);
        if (committed == status_code_t::write_conflict_k && schedule != watch_schedule_t::never_k) continue;
        return committed;
    }
    return {status_code_t::write_conflict_k, "Concurrent commits kept conflicting with the call"};
}

/** Writes @p writes into the transaction @p view carries, or commits them as a transaction of their own, flushed when
 *  @p durability asks, for every call that writes. */
template <cpu_class_t cpu_class_, typename writes_type_>
status_t serve_write(database_state<cpu_class_>& state, view_t const& view, ustore_durability_t durability,
                     std::size_t count, std::size_t bytes, watch_schedule_t schedule, writes_type_&& writes) noexcept {
    if (view.transaction) {
        transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
        if (transaction.staged)
            return {status_code_t::invalid_argument_k, "A staged transaction takes no writes until rolled back"};
        call_access<cpu_class_> access = access_of(state, view);
        if (schedule != watch_schedule_t::never_k) access.schedule = schedule;
        return writes(access);
    }
    if (status_t const committed = commit_alone(state, view.scratch, count, bytes, schedule, writes);
        committed.failed())
        return committed;
    return settle(state, durability);
}

#pragma endregion Maintenance

#pragma region Collections

/** What @p collection of the store of @p state holds, which the store its tag names tells. */
template <cpu_class_t cpu_class_>
status_t collection_modality(database_state<cpu_class_>& state, collection_id_t const& collection,
                             ustore_modality_t& modality) noexcept {
    std::uint16_t store_tag = 0;
    if (status_t const held = state.store->collection_store_tag(collection, store_tag); held.failed()) return held;
    modality = modality_of(store_tag);
    return {};
}

/** The tag of the store keeping a collection of @p modality shaped as @p shape, @c store_tags_k's
 *  size for none. */
inline std::uint16_t store_tag_for(ustore_modality_t modality, collection_config_t const& shape) noexcept {
    value_layout_t const layout = entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k
                                      ? value_layout_t::uuid_k
                                      : resolve_value_layout(shape.lsm_tree.lsm_level_0.sorted_run.value_size);
    return store_tag_of(modality, layout);
}

/** Creates the collection @p collection named @p name, holding @p modality, shaped as @p shape, in
 *  the store tagged for values of one 16-byte key for strings and of the layout of their width
 *  otherwise; the manifest naming it also names the memory runs of @p flushed_with, which the
 *  caller wrote its listing into. */
template <cpu_class_t cpu_class_>
status_t create_collection(database_state<cpu_class_>& state, collection_id_t const& collection, std::string_view name,
                           ustore_modality_t modality, collection_config_t const& shape,
                           collection_id_t flushed_with = collection_invalid_id_k) noexcept {
    std::uint16_t const store_tag = store_tag_for(modality, shape);
    if (store_tags_t::index_of(store_tag) == std::size(store_tags_k))
        return {status_code_t::unsupported_k, "No store keeps a collection of that modality and layout"};
    return state.store->create_collection(collection, string_view_t {name}, store_tag, shape, flushed_with);
}

/** Lays the record of a collection of @p store_tag shaped as @p shape and named @p name out
 *  into @p into. */
inline status_t lay_out_collections_record(std::uint16_t store_tag, collection_config_t const& shape,
                                           std::string_view name, st::vector<byte_t>& into) noexcept {
    collections_record_t record;
    record.store_tag = store_tag;
    record.value_size = static_cast<std::uint32_t>(shape.lsm_tree.lsm_level_0.sorted_run.value_size);
    record.value_max_size = static_cast<std::uint32_t>(shape.lsm_tree.lsm_level_0.sorted_run.value_max_size);
    record.level_enlarge_factor = shape.lsm_tree.level_enlarge_factor;
    record.cache_limit = shape.cache_limit;
    record.memory_runs_max_bytes = shape.memory_runs_max_bytes;
    record.memory_runs_max_deltas = shape.memory_runs_max_deltas;
    if (into.resize(sizeof(record) + name.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room for a collections record"};
    std::memcpy(into.data(), &record, sizeof(record));
    if (!name.empty()) std::memcpy(into.data() + sizeof(record), name.data(), name.size());
    return {};
}

/** What the engine knows of a listed collection: its modality and its name, empty for
 *  an anonymous one. */
struct collections_entry_t {
    ustore_modality_t modality {};
    std::string_view name;
};

/** The entry @p held, a listing of the collections system collection, carries. */
template <cpu_class_t cpu_class_>
expected<collections_entry_t> read_collections_entry(database_state<cpu_class_>&, ustore_collection_id_t const&,
                                                     value_view_t held) noexcept {
    std::string_view const bytes {held};
    if (bytes.size() < sizeof(collections_record_t))
        return status_t {status_code_t::malformed_k, "A collections record is shorter than its fields"};
    collections_record_t record;
    std::memcpy(&record, bytes.data(), sizeof(record));
    return collections_entry_t {modality_of(record.store_tag), bytes.substr(sizeof(record))};
}

/** What the collections system collection records of @p collection at @p stamp, refusing one it does not list, the
 *  reserved ones included. */
template <cpu_class_t cpu_class_>
expected<collections_entry_t> look_up(database_state<cpu_class_>& state, ustore_arena_t scratch,
                                      ustore_collection_id_t const& collection,
                                      transaction_stamp_t stamp = head_stamp_k,
                                      std::size_t thread_index = outside_executor_k) noexcept {
    call_access<cpu_class_> const listed {state, nullptr, watch_schedule_t::never_k, scratch, stamp, thread_index};
    auto [held, read_status] = read_value(listed, collections_collection_k, stored_of(collection.uuid));
    if (read_status.failed()) return read_status;
    if (!held) return status_t {status_code_t::not_found_k, "No such collection"};
    return read_collections_entry(state, collection, held);
}

/** Refuses a batch addressing a collection the collections system collection does not list, as @p view reads it, or
 *  one @c admit_modality refuses for @p modality, looking each run of tasks on one collection up once. */
template <cpu_class_t cpu_class_>
status_t resolve_runs(database_state<cpu_class_>& state, view_t const& view,
                      strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                      ustore_modality_t modality) noexcept {
    transaction_stamp_t const stamp = view.snapshot ? state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp()
                                                    : head_stamp_k;
    for (std::size_t index = 0; index != count; ++index) {
        if (index != 0 && collections[index] == collections[index - 1]) continue;
        auto [entry, look_up_status] = look_up(state, view.scratch, collections[index], stamp,
                                               thread_index_of(state, view));
        if (look_up_status.failed()) return look_up_status;
        if (status_t const admitted = admit_modality(entry.modality, modality); admitted.failed()) return admitted;
    }
    return {};
}

/** The resolver of this engine, which its docs and pages verbs run first. */
template <cpu_class_t cpu_class_>
status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                          ustore_modality_t modality, view_t const& view) noexcept {
    return resolve_runs(*state_of<database_state<cpu_class_>>(database), view, strided(collections), count, modality);
}

#pragma endregion Collections

#pragma region Strings

/** One string a batch writes or removes, which the last task naming it wins. */
struct string_task_t {
    ustore_collection_id_t collection;
    std::string_view text;

    /** Whether the task writes @c value or removes the string. */
    delta_kind_t kind = delta_kind_t::remove_k;

    /** The 16 bytes of the key the string maps onto, nil for a removal. */
    ustore_uuid_t value {};
};

/** Calls @p visitor with the strings store of @p collection inside the database's epochs, which keep it alive until
 *  @p visitor returns, answering what it answers, or refuses a collection another store keeps. */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t visit_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                       visitor_type_&& visitor) noexcept {
    return access.state.store->visit_store(
        collection,
        [&]<typename store_type_>(store_type_& store) noexcept -> status_t {
            if constexpr (std::is_same_v<store_type_, strings_store_of<cpu_class_>>) return visitor(store);
            else return collection_holds_another_modality();
        },
        access.thread_index);
}

/** The stamp a read through @p access answers at: its transaction's start, its snapshot's, or the head's. */
template <cpu_class_t cpu_class_>
transaction_stamp_t read_stamp_of(call_access<cpu_class_> const& access) noexcept {
    return access.transaction ? access.transaction->start_stamp() : access.stamp;
}

/** The stamp a commit after which conflicts with what @p access reads and writes, or none when it watches nothing. */
template <cpu_class_t cpu_class_>
transaction_stamp_t watch_stamp_of(call_access<cpu_class_> const& access) noexcept {
    bool const watched = access.transaction && access.schedule != watch_schedule_t::never_k;
    return watched ? access.transaction->start_stamp() : invalid_transaction_stamp_k;
}

/** Writes or removes the string of every task of @p tasks through the strings of @p access's transaction, in batch
 *  order, so the last task naming a string wins, each watched on its own as @p access watches. */
template <cpu_class_t cpu_class_>
status_t stage_strings(call_access<cpu_class_> const& access, std::span<string_task_t const> tasks) noexcept {
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    for (string_task_t const& task : tasks) {
        delta_view_t const delta = task.kind == delta_kind_t::write_k
                                       ? delta_view_t {delta_kind_t::write_k,
                                                       std::as_bytes(std::span {task.value.bytes})}
                                       : delta_view_t {delta_kind_t::remove_k, {}};
        if (status_t const written = access.strings->write(task.collection, task.text, delta, watch_stamp);
            written.failed())
            return written;
    }
    return {};
}

/**
 *  @brief The key @p text maps onto in @p collection as @p access sees it, and whether any write carries it.
 *
 *  A transaction's own write outranks the store, and its own removal hides the store's key. A read through a watching
 *  transaction watches the string, its absence included, so a commit landing on it in between conflicts.
 */
template <cpu_class_t cpu_class_>
status_t read_string(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view text,
                     ustore_uuid_t& value, bool& present) noexcept {
    std::span<byte_t, sizeof(ustore_uuid_t)> const bytes = std::as_writable_bytes(std::span {value.bytes});
    delta_view_t own_delta;
    if (access.strings && access.strings->uncommitted(collection, text, own_delta)) {
        present = own_delta.kind != delta_kind_t::remove_k;
        if (present) std::memcpy(bytes.data(), own_delta.bytes.data(), bytes.size());
        return {};
    }
    transaction_stamp_t const stamp = read_stamp_of(access);
    if (transaction_stamp_t const watch_stamp = watch_stamp_of(access); watch_stamp != invalid_transaction_stamp_k)
        if (status_t const watched = access.strings->watch(collection, text, watch_stamp); watched.failed())
            return watched;
    return visit_strings(access, collection, [&](auto const& store) noexcept -> status_t {
        present = store.find(text, bytes, stamp, access.thread_index) == lookup_result_t::written_k;
        return {};
    });
}

/**
 *  @brief Hands @p visitor every string of @p collection starting with @p prefix and not below @p start, in order, as
 *      the store holds them at @p stamp, each with what its newest version carries, a removal included.
 *  @param[in] visitor Called as @c visitor(text, state, value) outside the epochs, answering whether the walk carries
 *      on, with the key the string maps onto; the text lives until it returns.
 *
 *  A page is read inside the epochs at a time, finding the collection again for each, so a commit waits for one page
 *  at most, and a head walk pins the stamp it starts at, so every page reads the same commits.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk_strings(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view prefix,
                      std::string_view start, transaction_stamp_t stamp, visitor_type_&& visitor) noexcept {
    typename basic_database_of<cpu_class_>::pin_t pin;
    if (stamp == head_stamp_k) stamp = access.state.store->ledger().pin_reader(pin);
    strings_page_t page;
    st::vector<char> from;
    std::string_view const first = std::max(start, prefix);
    if (from.resize(first.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
    if (!first.empty()) std::memcpy(from.data(), first.data(), first.size());
    for (;;) {
        std::string_view const resumed {from.begin(), from.end()};
        if (status_t const read = visit_strings(access, collection,
                                                [&](auto const& store) noexcept {
                                                    return store.read_page(prefix, resumed, stamp, access.thread_index,
                                                                           page);
                                                });
            read.failed())
            return read;
        for (std::size_t index = 0; index != page.size(); ++index)
            if (!visitor(page.text(index), page.listed[index].state, page.listed[index].value)) return {};
        if (page.size() != strings_page_k) return {};
        // The smallest string past the last one listed is that string and one zero byte more.
        std::string_view const last = page.text(page.size() - 1);
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/** Hands @p visitor every string of @p collection starting with @p prefix and below @p below, or
 *  all of them for an empty @p below, largest first, as @c walk_strings does upwards, reading one
 *  page per run at a time. */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk_strings_down(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view prefix,
                           std::string_view below, transaction_stamp_t stamp, visitor_type_&& visitor) noexcept {
    typename basic_database_of<cpu_class_>::pin_t pin;
    if (stamp == head_stamp_k) stamp = access.state.store->ledger().pin_reader(pin);
    strings_page_t page;
    st::vector<char> until;
    if (until.resize(below.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
    if (!below.empty()) std::memcpy(until.data(), below.data(), below.size());
    for (;;) {
        std::string_view const resumed {until.begin(), until.end()};
        if (status_t const read = visit_strings(access, collection,
                                                [&](auto const& store) noexcept {
                                                    return store.read_page_down(prefix, resumed, stamp,
                                                                                access.thread_index, page);
                                                });
            read.failed())
            return read;
        for (std::size_t index = 0; index != page.size(); ++index)
            if (!visitor(page.text(index), page.listed[index].state, page.listed[index].value)) return {};
        if (page.size() != strings_page_k) return {};
        // No string sits below the empty one, and an empty bound would mean none at all.
        std::string_view const last = page.text(page.size() - 1);
        if (last.empty()) return {};
        if (until.resize(last.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold where it resumes"};
        std::memcpy(until.data(), last.data(), last.size());
    }
}

/** The strings one task of a strings call names: those starting with @c prefix, from @c start,
 *  inclusive, towards @c end, exclusive, in @c direction, where an empty start begins at the first
 *  string in the direction and an empty end bounds nothing. */
struct strings_window_t {
    std::string_view prefix;
    std::string_view start;
    std::string_view end;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;

    /** The least string of the window, or of the prefix where the window names none. */
    [[nodiscard]] std::string_view lower() const noexcept {
        std::string_view const bound = direction == ustore_scan_descending_k ? end : start;
        return std::max(bound, prefix);
    }

    /** The string the window stops at, exclusive for an ascending window and inclusive for a
     *  descending one, or empty for none. */
    [[nodiscard]] std::string_view upper() const noexcept {
        return direction == ustore_scan_descending_k ? start : end;
    }

    /** Whether @p text, prefixed and not below @c lower, lies past the window's upper end. */
    [[nodiscard]] bool past(std::string_view text) const noexcept {
        std::string_view const bound = upper();
        if (bound.empty()) return false;
        return direction == ustore_scan_descending_k ? bound < text : !(text < bound);
    }

    /** Whether @p text is the exclusive end a descending window starts above. */
    [[nodiscard]] bool skips(std::string_view text) const noexcept {
        return direction == ustore_scan_descending_k && !end.empty() && text == end;
    }
};

/**
 *  @brief Hands @p visitor every present string of @p window in @p collection, smallest first, or
 *      largest first in @p order descending, as @p access sees them: a transaction's own writes
 *      merged in, its removals hiding the store's strings.
 *  @param[in] visitor Called as @c visitor(text) answering whether the walk carries on; the text
 *      lives until it returns.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk_window_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                             strings_window_t const& window, visitor_type_&& visitor,
                             scan_direction_t order = scan_direction_t::ascending_k) noexcept {
    std::string_view const lower = window.lower();
    bool const descending = order == scan_direction_t::descending_k;
    auto const before = [descending](std::string_view first, std::string_view second) noexcept {
        return descending ? second < first : first < second;
    };

    // The transaction's own strings inside the window, in order, which outrank the store's versions of them
    struct own_string_t {
        std::string_view text;
        bool present = false;
    };
    st::vector<own_string_t, arena_allocator<own_string_t>> own_strings {{access.scratch}};
    status_t listed;
    if (access.strings)
        access.strings->for_each_uncommitted(collection, [&](std::string_view text, delta_view_t delta) noexcept {
            if (listed.failed() || text < lower || !text.starts_with(window.prefix) || window.past(text) ||
                window.skips(text))
                return;
            listed = growth_status(own_strings.push_back(own_string_t {text, delta.kind != delta_kind_t::remove_k}));
        });
    if (listed.failed()) return listed;
    std::sort(own_strings.begin(), own_strings.end(),
              [&](own_string_t const& first, own_string_t const& second) noexcept {
                  return before(first.text, second.text);
              });
    // The ranges the transaction removes hide the store's strings in them, bar its later writes.
    st::vector<range_tombstone<std::string_view>, arena_allocator<range_tombstone<std::string_view>>> own_ranges {
        {access.scratch}};
    if (access.strings)
        access.strings->for_each_removed_range(
            collection, [&](range_tombstone<std::string_view> const& range) noexcept {
                if (listed.succeeded())
                    listed = growth_status(own_ranges.push_back(range_tombstone<std::string_view> {range}));
            });
    if (listed.failed()) return listed;
    auto const removed_in_range = [&](std::string_view text) noexcept {
        for (range_tombstone<std::string_view> const& range : own_ranges)
            if (range.covers(text)) return true;
        return false;
    };

    std::size_t next = 0;
    bool carries_on = true;
    auto const emit_own = [&](own_string_t const& written) noexcept {
        if (written.present) carries_on = visitor(written.text);
        return carries_on;
    };
    auto const merged = [&](std::string_view text, lookup_result_t state, ustore_key_t const&) noexcept {
        // A descending walk ends below the window's least string, which an ascending one starts at.
        if (descending ? text < lower : window.past(text)) return false;
        for (; next != own_strings.size() && before(own_strings[next].text, text); ++next)
            if (!emit_own(own_strings[next])) return false;
        if (next != own_strings.size() && own_strings[next].text == text) return emit_own(own_strings[next++]);
        if (state != lookup_result_t::written_k || window.skips(text) || removed_in_range(text)) return true;
        carries_on = visitor(text);
        return carries_on;
    };
    status_t walked;
    if (!descending) walked = walk_strings(access, collection, window.prefix, lower, read_stamp_of(access), merged);
    else {
        // The window's start is inclusive, so the walk starts below the least string past it.
        st::vector<char> below;
        if (!window.upper().empty()) {
            if (below.resize(window.upper().size() + 1) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
            std::memcpy(below.data(), window.upper().data(), window.upper().size());
            below[window.upper().size()] = '\0';
        }
        walked = walk_strings_down(access, collection, window.prefix, std::string_view {below.begin(), below.end()},
                                   read_stamp_of(access), merged);
    }
    if (walked.failed()) return walked;
    for (; carries_on && next != own_strings.size(); ++next) emit_own(own_strings[next]);
    return {};
}

/** Whether a walk listed as many strings as its page holds, so more may follow what it listed. */
enum class page_fill_t : bool { short_k, full_k };

/** Watches what a walk of @p window covered through a watching transaction of @p access: from the
 *  window's start in its direction to @p smallest or @p largest listed for a full page, and the
 *  whole window otherwise, so a commit landing a string inside it conflicts. */
template <cpu_class_t cpu_class_>
status_t watch_window_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                              strings_window_t const& window, std::string_view smallest, std::string_view largest,
                              page_fill_t fill) noexcept {
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    if (watch_stamp == invalid_transaction_stamp_k) return {};
    bool const descending = window.direction == ustore_scan_descending_k;
    bool const full = fill == page_fill_t::full_k;
    std::string_view const lower = full && descending ? smallest : window.lower();
    if (full && !descending) return access.strings->watch_range(collection, lower, largest, watch_stamp);
    if (!window.upper().empty()) return access.strings->watch_range(collection, lower, window.upper(), watch_stamp);
    return access.strings->watch_prefix(collection, window.prefix, lower, watch_stamp);
}

/** One string a sample keeps past the visitor that saw it, in a room of its own. */
using kept_string_t = st::vector<char>;

/** Copies @p text into @p kept, reusing its room. */
inline status_t keep_string(kept_string_t& kept, std::string_view text) noexcept {
    if (kept.resize(text.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to keep a string"};
    if (!text.empty()) std::memcpy(kept.data(), text.data(), text.size());
    return {};
}

/** The text @p kept holds. */
inline std::string_view kept_text(kept_string_t const& kept) noexcept { return {kept.begin(), kept.end()}; }

/** Appends to @p outputs up to @p limit strings of @p window in @p collection, in its direction, as
 *  @p access sees them, and watches what the walk covered. */
template <cpu_class_t cpu_class_>
status_t scan_strings(call_access<cpu_class_> const& access, collection_id_t collection, strings_window_t const& window,
                      std::size_t limit, strings_scan_outputs_t& outputs) noexcept {
    if (limit == 0) return {};
    std::size_t const first = outputs.size();
    status_t pushed;
    scan_direction_t const order = window.direction == ustore_scan_descending_k ? scan_direction_t::descending_k
                                                                                : scan_direction_t::ascending_k;
    status_t const walked = walk_window_strings(
        access, collection, window,
        [&](std::string_view text) noexcept {
            pushed = outputs.push(text);
            return pushed.succeeded() && outputs.size() - first != limit;
        },
        order);
    if (walked.failed()) return walked;
    if (pushed.failed()) return pushed;
    std::size_t const found = outputs.size() - first;
    if (found == 0) return watch_window_strings(access, collection, window, {}, {}, page_fill_t::short_k);
    std::string_view const head = outputs[first];
    std::string_view const tail = outputs[outputs.size() - 1];
    bool const descending = window.direction == ustore_scan_descending_k;
    return watch_window_strings(access, collection, window, descending ? tail : head, descending ? head : tail,
                                found == limit ? page_fill_t::full_k : page_fill_t::short_k);
}

/** Removes every string of @p collection from @p start up to @p end, exclusive, or to the last
 *  string for an empty @p end, as one range tombstone the strings of @p access's transaction land,
 *  watching the range when it watches. */
template <cpu_class_t cpu_class_>
status_t remove_string_range(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view start,
                             std::string_view end) noexcept {
    if (!end.empty() && !(start < end)) return {};
    if (status_t const removed = access.strings->remove_range(collection, start, end); removed.failed()) return removed;
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    if (watch_stamp == invalid_transaction_stamp_k) return {};
    if (end.empty()) return access.strings->watch_prefix(collection, {}, start, watch_stamp);
    return access.strings->watch_range(collection, start, end, watch_stamp);
}

/** Commits @p tasks as a transaction of their own, which watches nothing, so the last writer wins. */
template <cpu_class_t cpu_class_>
status_t write_strings_alone(database_state<cpu_class_>& state, ustore_arena_t scratch,
                             std::span<string_task_t const> tasks) noexcept {
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + sizeof(task.value);
    return commit_alone(state, scratch, tasks.size(), bytes, watch_schedule_t::never_k,
                        [&](call_access<cpu_class_> const& access) noexcept { return stage_strings(access, tasks); });
}

#pragma endregion Strings

#pragma region Names

/** The collection @p name maps onto as @p access reads the names, and whether any collection carries the name. */
template <cpu_class_t cpu_class_>
status_t look_up_name(call_access<cpu_class_> const& access, std::string_view name, ustore_collection_id_t& collection,
                      bool& present) noexcept {
    return read_string(access, names_collection_k, name, collection.uuid, present);
}

/** Commits @p name mapped onto @p collection, or its removal when @p collection is null, as a write of its own. */
template <cpu_class_t cpu_class_>
status_t write_name(database_state<cpu_class_>& state, ustore_arena_t scratch, std::string_view name,
                    ustore_collection_id_t const* collection) noexcept {
    string_task_t const task = collection
                                   ? string_task_t {names_collection_k, name, delta_kind_t::write_k, collection->uuid}
                                   : string_task_t {names_collection_k, name};
    return write_strings_alone(state, scratch, std::span<string_task_t const> {&task, 1});
}

#pragma endregion Names

#pragma region Clearing

/** Empties the strings collection @p collection for @c ustore_drop_pairs_k, removing every string in windows committed
 *  one after another, so the transaction of each stays bounded, and refuses @c ustore_drop_values_k, as a string always
 *  maps onto a key. */
template <cpu_class_t cpu_class_>
status_t empty_strings(database_state<cpu_class_>& state, ustore_arena_t scratch, collection_id_t collection,
                       ustore_drop_mode_t mode) noexcept {
    if (mode == ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "A string always maps onto a key, so its value never empties"};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
    st::vector<char> from;
    st::vector<char> texts;
    st::vector<std::size_t> ends;
    st::vector<string_task_t> tasks;
    for (;;) {
        texts.clear();
        ends.clear();
        tasks.clear();
        status_t room;
        status_t const walked = walk_strings(
            head, collection, {}, std::string_view {from.begin(), from.end()}, head_stamp_k,
            [&](std::string_view text, lookup_result_t found, ustore_key_t const&) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = growth_status(ends.push_back(offset + text.size()));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                return ends.size() != clear_window_keys_k;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (ends.empty()) return {};
        if (status_t const reserved = growth_status(tasks.reserve(ends.size())); reserved.failed()) return reserved;
        for (std::size_t index = 0, begin = 0; index != ends.size(); begin = ends[index++]) [[maybe_unused]]
            st::status_t const placed = tasks.push_back(
                string_task_t {collection, std::string_view {texts.data() + begin, ends[index] - begin}});
        if (status_t const written = write_strings_alone(state, scratch, std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (ends.size() < clear_window_keys_k) return {};
        // The next window starts past the last string of this one, past the removals the walk would list again.
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A clear could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/**
 *  @brief Empties @p collection holding @p modality: removing every key for @c ustore_drop_pairs_k, or emptying every
 *      value for @c ustore_drop_values_k, which a strings collection refuses and a graph, whose vertices hold no
 *      values but their edges, answers by keeping everything, as on the in-memory engine.
 *
 *  The keys are rewritten in windows committed one after another, so the transaction of each stays bounded.
 */
template <cpu_class_t cpu_class_>
status_t empty_collection(database_state<cpu_class_>& state, ustore_arena_t scratch, collection_id_t collection,
                          ustore_modality_t modality, ustore_drop_mode_t mode) noexcept {
    if (entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k)
        return empty_strings(state, scratch, collection, mode);
    if (modality == ustore_modality_graph_k && mode == ustore_drop_values_k) return {};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
    st::vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    stored_key_t from = limit<stored_key_t>::min();
    value_view_t const emptied = mode == ustore_drop_values_k ? value_view_t::make_empty() : value_view_t {};
    for (;;) {
        keys.clear();
        if (status_t const collected = collect_keys(head, collection, from, limit<stored_key_t>::max(),
                                                    clear_window_keys_k, keys);
            collected.failed())
            return collected;
        if (keys.empty()) return {};
        status_t const committed = commit_alone(
            state, scratch, keys.size(), keys.size(), watch_schedule_t::never_k,
            [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                for (std::size_t index = 0; index != keys.size(); ++index)
                    if (status_t const written = write_value(access, collection, keys[index], emptied);
                        written.failed())
                        return written;
                return {};
            });
        if (committed.failed()) return committed;
        if (keys.size() < clear_window_keys_k) return {};
        from = keys[keys.size() - 1];
        if (!advance_key(from)) return {};
    }
}

/** Every collection the collections system collection lists at the head, with its entry. */
struct listed_collection_t {
    ustore_collection_id_t id;
    collections_entry_t entry;
};

/** Collects every collection the collections system collection lists at @p stamp into @p listed. */
template <cpu_class_t cpu_class_, typename listed_type_>
status_t list_collections(database_state<cpu_class_>& state, ustore_arena_t scratch, listed_type_& listed,
                          transaction_stamp_t stamp = head_stamp_k) noexcept {
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, stamp};
    st::vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    if (status_t const collected = collect_keys(head, collections_collection_k, limit<stored_key_t>::min(),
                                                limit<stored_key_t>::max(), std::numeric_limits<std::size_t>::max(),
                                                keys);
        collected.failed())
        return collected;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        auto [held, read_status] = read_value(head, collections_collection_k, keys[index]);
        if (read_status.failed()) return read_status;
        if (!held) continue;
        ustore_collection_id_t id;
        keys[index].to_bytes(std::as_writable_bytes(std::span<std::uint8_t, 16> {id.uuid.bytes}));
        auto [entry, entry_status] = read_collections_entry(state, id, held);
        if (entry_status.failed()) return entry_status;
        if (status_t const pushed = growth_status(listed.push_back(listed_collection_t {id, entry})); pushed.failed())
            return pushed;
    }
    return {};
}

/**
 *  @brief Forgets @p collection in both system collections, then returns its runs to the volume, or leaves them to
 *      the snapshots open, which still read them.
 *
 *  The collections system collection changes under its own claim and straight into the store,
 *  outside the registry, since no caller's transaction reads it, so its change never shows as a
 *  commit a transaction's watches might collide with. The name goes first, so a crash in between
 *  leaves a listed collection without a name, never a dangling name, and a crash with the runs
 *  still kept leaves an unlisted collection, which the next open drops.
 */
template <cpu_class_t cpu_class_>
status_t drop_collection(database_state<cpu_class_>& state, ustore_arena_t scratch,
                         ustore_collection_id_t const& collection) noexcept {
    {
        collections_guard<cpu_class_> const held {state};
        call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
        stored_key_t const listed = stored_of(collection.uuid);
        auto [listing, read_status] = read_value(head, collections_collection_k, listed);
        if (read_status.failed()) return read_status;
        if (!listing) return {status_code_t::not_found_k, "No collection has such identifier"};
        std::string_view const record {listing};
        std::string_view const name = record.size() > sizeof(collections_record_t)
                                          ? record.substr(sizeof(collections_record_t))
                                          : std::string_view {};
        if (!name.empty())
            if (status_t const unnamed = write_name(state, scratch, name, nullptr); unnamed.failed()) return unnamed;
        if (status_t const unlisted = write_value(head, collections_collection_k, listed, {}); unlisted.failed())
            return unlisted;
        // A snapshot taken before the unlisting still reads the runs, which wait for the last snapshot to go
        if (atomic_ref_of<cpu_class_, std::size_t> {state.open_snapshots}.load(std::memory_order_acquire) != 0 &&
            state.retired_collections.push_back(ustore_collection_id_t {collection}) == st::status_t::success_k)
            return {};
    }
    // The unlisting reaches a run in the same manifest that stops naming the collection
    return state.store->drop_collection(collection, collections_collection_k);
}

/** Drops every collection a drop left for the snapshots, which the last snapshot freed no longer reads. */
template <cpu_class_t cpu_class_>
void drop_retired(database_state<cpu_class_>& state) noexcept {
    collections_guard<cpu_class_> const held {state};
    for (ustore_collection_id_t const& collection : state.retired_collections) [[maybe_unused]]
        status_t const dropped = state.store->drop_collection(collection);
    state.retired_collections.clear();
}

#pragma endregion Clearing

#pragma region Database Calls

/** Implements @c ustore_database_describe with the store's estimates, which count every version of a key that the
 *  runs still hold. */
template <cpu_class_t cpu_class_>
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) noexcept {
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    return assign_describe(
        arguments,
        {
            .capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k),
            .engine = ustore_engine_nvme_k,
            .isolation = ustore_isolation_snapshot_k,
            .keys = static_cast<ustore_size_t>(state.store->population_count()),
            .memory_bytes = static_cast<ustore_size_t>(state.resources->memory_constraint.used()),
            .disk_bytes = static_cast<ustore_size_t>(state.store->size_bytes()),
        });
}

/** Implements @c ustore_database_clear: removing every collection, or emptying every collection's pairs or values. */
template <cpu_class_t cpu_class_>
status_t database_clear(ustore_database_clear_t const& arguments, view_t const& view) noexcept {
    ustore_drop_mode_t const mode = arguments.mode;
    if (status_t const status = validate_drop_mode(mode); status.failed()) return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    st::vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    if (status_t const collected = list_collections(state, view.scratch, listed); collected.failed()) return collected;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        // A string always maps onto a key, so emptying every value leaves the strings collections as they are
        if (mode == ustore_drop_values_k &&
            entry_layout_of(listed[index].entry.modality) == entry_layout_t::strings_to_uuids_k)
            continue;
        status_t const cleared = mode == ustore_drop_collection_k
                                     ? drop_collection(state, view.scratch, listed[index].id)
                                     : empty_collection(state, view.scratch, listed[index].id,
                                                        listed[index].entry.modality, mode);
        if (cleared.failed()) return cleared;
    }
    if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/** Implements @c ustore_database_compact: flushing every collection, then merging every level over its bounds,
 *  reporting one unit per pass, as a pass merges every collection at once. */
template <cpu_class_t cpu_class_>
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) noexcept {
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    progress_t progress {arguments.progress, arguments.progress_state, compaction_passes_k};
    for (std::size_t pass = 0; pass != compaction_passes_k; ++pass) {
        if (state.store->needs_compaction())
            if (status_t const compacted = state.store->compact(); compacted.failed()) return compacted;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return {};
}

#pragma endregion Database Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the collections system collection at the snapshot it names or at
 *  the head. */
template <cpu_class_t cpu_class_>
status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) noexcept {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    st::vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    transaction_stamp_t const stamp = view.snapshot ? state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp()
                                                    : head_stamp_k;
    if (status_t const collected = list_collections(state, view.scratch, listed, stamp); collected.failed())
        return collected;
    collection_list_outputs_t outputs {view.scratch};
    for (std::size_t index = 0; index != listed.size(); ++index) {
        collections_entry_t const& entry = listed[index].entry;
        value_view_t const name = entry.name.empty() ? value_view_t {} : value_view_t {entry.name};
        if (status_t const pushed = outputs.push(listed[index].id, entry.modality, name); pushed.failed())
            return pushed;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_collection_create: the collections record and the name first, then the
 *  collection, whose manifest also names the run the record lands in, unlisting it again when the
 *  store refuses the collection. */
template <cpu_class_t cpu_class_>
status_t collection_create(ustore_collection_create_t const& arguments, view_t const& view) noexcept {
    auto [name, name_status] = validate_collection_create(arguments);
    if (name_status.failed()) return name_status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [created, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    // The engine's own name of a collection is its UUID's text, which never leaves the engine
    char text[uuid_t::printed_length_k] = {};
    uuid_t {std::span<std::uint8_t const, uuid_t::size_bytes_k> {created.uuid.bytes}}.print(text);
    collection_config_t const shape = entry_layout_of(arguments.modality) == entry_layout_t::strings_to_uuids_k
                                          ? strings_shape_of(state.collection_shape)
                                          : state.collection_shape;
    std::uint16_t const store_tag = store_tag_for(arguments.modality, shape);
    st::vector<byte_t> record;
    if (status_t const laid = lay_out_collections_record(store_tag, shape, name, record); laid.failed()) return laid;

    // The collections system collection changes under its own claim, outside the registry, and
    // lists the collection before the names system collection names it; the create's manifest then
    // names the collection and the run its listing lands in at once, so the two never disagree
    // after a crash
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, view.scratch};
    status_t listed;
    bool taken = false;
    {
        collections_guard<cpu_class_> const held {state};
        ustore_collection_id_t holder;
        if (!name.empty()) listed = look_up_name(head, name, holder, taken);
        if (listed.succeeded() && taken)
            listed = {status_code_t::already_exists_k, "A collection already carries the name"};
        if (listed.succeeded())
            listed = write_value(head, collections_collection_k, stored_of(created.uuid),
                                 value_view_t {std::span<byte_t const> {record}});
        if (listed.succeeded() && !name.empty()) {
            listed = write_name(state, view.scratch, name, &created);
            if (listed.failed()) [[maybe_unused]]
                status_t const unlisted = write_value(head, collections_collection_k, stored_of(created.uuid), {});
        }
    }
    if (listed.failed()) return listed;
    status_t const made = create_collection(state, created, std::string_view {text, sizeof(text)}, arguments.modality,
                                            shape, collections_collection_k);
    if (made.failed()) {
        collections_guard<cpu_class_> const held {state};
        if (!name.empty()) [[maybe_unused]]
            status_t const unnamed = write_name(state, view.scratch, name, nullptr);
        [[maybe_unused]] status_t const unlisted = write_value(head, collections_collection_k, stored_of(created.uuid),
                                                               {});
        return made;
    }
    if (!name.empty())
        if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    *arguments.collection = created;
    return {};
}

/** Implements @c ustore_collection_drop: forgetting the collection and its runs, or emptying its pairs or values. */
template <cpu_class_t cpu_class_>
status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const& view) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (status_t const status = validate_drop_mode(mode); status.failed()) return status;
    if (reserved(collection)) return {status_code_t::not_found_k, "No collection has such identifier"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (mode == ustore_drop_collection_k) {
        if (status_t const dropped = drop_collection(state, view.scratch, collection); dropped.failed()) return dropped;
        return state.store->flush();
    }
    auto [entry, look_up_status] = look_up(state, view.scratch, collection);
    if (look_up_status == status_code_t::not_found_k)
        return {status_code_t::not_found_k, "No collection has such identifier"};
    if (look_up_status.failed()) return look_up_status;
    if (status_t const emptied = empty_collection(state, view.scratch, collection, entry.modality, mode);
        emptied.failed())
        return emptied;
    return compact_due(state);
}

/** Implements @c ustore_collection_find, reading the names at the snapshot it names or at the head. */
template <cpu_class_t cpu_class_>
status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names,
                         view_t const& view) noexcept {
    if (!arguments.count) return {};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = validate_collection_find(arguments, names);
    if (outputs_status.failed()) return outputs_status;
    // Through a snapshot the names read as they stood at its stamp, and through anything else at the head
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, view.scratch};
    call_access<cpu_class_> const named = view.snapshot ? access_of(state, view) : head;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        bool present = false;
        ustore_collection_id_t found;
        if (status_t const looked = look_up_name(named, string_of(names[index]), found, present); looked.failed())
            return looked;
        // A drop unnames a collection before the store lets it go, so a name whose store is gone is a drop in flight
        ustore_modality_t modality {};
        if (present && collection_modality(state, found, modality).failed()) present = false;
        outputs.assign(index, present ? found : ustore_collection_id_t {}, modality, present);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Collection Calls

#pragma region Transaction Calls

/** Implements @c ustore_transaction_init, opening a transaction or restarting the one it reuses. */
template <cpu_class_t cpu_class_>
status_t transaction_init(ustore_transaction_init_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;
    if (view.transaction) {
        transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
        transaction.held->rollback();
        transaction.strings.clear();
        transaction.staged = false;
        return {};
    }
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [opened, begin_status] = begin_transaction(state, state.transaction_shape);
    if (begin_status.failed()) return begin_status;
    auto [made, status] = make_handle<ustore_transaction_handle_t, transaction_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k, state, std::move(opened));
    if (status.failed()) return status;
    *arguments.transaction = made.release();
    return {};
}

/** Implements @c ustore_transaction_stage: checking every watch and that every write fits, then handing the
 *  ledger's leader word back, so the commit checks them again. */
template <cpu_class_t cpu_class_>
status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const& view) noexcept {
    auto [state, status] = transaction_state_of<transaction_state<cpu_class_>>(view.transaction);
    if (status.failed()) return status;
    transaction_state<cpu_class_>& transaction = *state;
    if (!transaction.staged) {
        if (status_t const staged = transaction.strings.stage(*transaction.held); staged.failed()) return staged;
        transaction.held->unstage();
        transaction.staged = true;
    }
    if (arguments.read_stamp) *arguments.read_stamp = transaction.held->start_stamp();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first, flushing when asked and compacting what it left due. */
template <cpu_class_t cpu_class_>
status_t transaction_commit(ustore_transaction_commit_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_commit(view.transaction, arguments.durability); status.failed())
        return status;
    transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
    if (status_t const committed = transaction.strings.commit(*transaction.held); committed.failed()) return committed;
    transaction.staged = false;
    if (arguments.commit_stamp) *arguments.commit_stamp = transaction.held->last_commit_stamp();
    return settle(transaction.state, arguments.durability);
}

/** Implements @c ustore_transaction_rollback, withdrawing a stage and keeping the writes, which a transaction never
 *  staged has none of to withdraw. */
template <cpu_class_t cpu_class_>
status_t transaction_rollback(ustore_transaction_rollback_t const&, view_t const& view) noexcept {
    auto [state, status] = transaction_state_of<transaction_state<cpu_class_>>(view.transaction);
    if (status.failed()) return status;
    state->staged = false;
    return {};
}

#pragma endregion Transaction Calls

#pragma region Lifetime

/** Fills @p resources with the manifest and the data directories @p arguments name; data
 *  directory 0 is the manifest's own directory, or the raw address it names, and a quota of zero
 *  takes the whole medium. */
inline status_t read_directories(ustore_database_open_nvme_t const& arguments, resources_config_t& resources) noexcept {
    if (sz::failed(resources.manifest.assign(string_view_t {string_of(arguments.directory)})))
        return {status_code_t::out_of_memory_k, "Failed to copy the manifest path"};
    if (arguments.data_directories_count && !arguments.data_directories)
        return {status_code_t::invalid_argument_k, "Counted data directories are null"};
    for (std::size_t index = 0; index != arguments.data_directories_count; ++index) {
        ustore_data_directory_t const& given = arguments.data_directories[index];
        if (!given.path.length) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        data_directory_config_t directory;
        if (sz::failed(directory.path.assign(string_view_t {string_of(given.path)})))
            return {status_code_t::out_of_memory_k, "Failed to copy a data directory"};
        directory.max_size = given.max_bytes;
        if (st::failed(resources.data_directories.push_back(std::move(directory))))
            return {status_code_t::out_of_memory_k, "Failed to list the data directories"};
    }
    return {};
}

/** The directory the manifest of @p resources sits in, into @p into, or an empty path for
 *  a raw address. */
inline status_t manifest_directory_of(resources_config_t const& resources, std::span<char> into) noexcept {
    auto [addresses, addresses_status] = addresses_of(resources);
    if (addresses_status.failed()) return addresses_status;
    std::string_view const location = addresses.data_directories[0].location;
    bool const raw = addresses.data_directories[0].kind != data_address_kind_t::directory_k;
    if (raw || location.size() >= into.size()) {
        into[0] = '\0';
        return raw ? status_t {} : status_t {status_code_t::invalid_argument_k, "A manifest path outgrew the limit"};
    }
    std::memcpy(into.data(), location.data(), location.size());
    into[location.size()] = '\0';
    return {};
}

/** Replaces @p value with @p requested unless it is zero, answering whether @p requested fits @p value. */
template <typename value_type_>
bool take_unless_zero(value_type_& value, ustore_size_t requested) noexcept {
    if (requested == 0) return true;
    if (!std::in_range<value_type_>(requested)) return false;
    value = static_cast<value_type_>(requested);
    return true;
}

/** Reads the placement of the executor of @p arguments, the executor of their merges into @p compactions, null for
 *  host merges, and their own sizes, keeping the engine's default for every zero among them. */
inline status_t read_options(ustore_database_open_nvme_t const& arguments, resources_config_t& resources,
                             database_config_t& transactions, user_collection_config_t& collections,
                             ustore_executor_handle_t const*& compactions) noexcept {
    nvme_placement_t const placement = nvme_placement_of(arguments.executor);
    switch (placement.nvme.transport) {
    case ustore_transport_automatic_k: resources.io_transport = io_transport_t::automatic_k; break;
    case ustore_transport_blocking_k: resources.io_transport = io_transport_t::blocking_k; break;
    case ustore_transport_linux_uring_k: resources.io_transport = io_transport_t::linux_uring_k; break;
    case ustore_transport_linux_aio_k: resources.io_transport = io_transport_t::linux_aio_k; break;
    case ustore_transport_linux_spdk_k: resources.io_transport = io_transport_t::linux_spdk_k; break;
    default: return {status_code_t::invalid_argument_k, "Unknown transport"};
    }
    compactions = arguments.compactions;

    // Every thread of the executor, its blocking caller included, keeps a channel, and every other thread shares the
    // rest
    resources.threads = placement.threads + 1;
    resources.shared_io_channels = engine_shared_io_channels_k;
    ustore_nvme_options_t const& moving = placement.nvme;
    if (arguments.collection_value_size) collections.value_max_size = 0;
    bool const fits = take_unless_zero(resources.memory_limit, arguments.memory_limit) &&
                      take_unless_zero(resources.shared_io_channels, moving.shared_io_channels) &&
                      take_unless_zero(resources.channels.depth, moving.queue_depth) &&
                      take_unless_zero(resources.channels.request_buffer_bytes, moving.request_buffer_bytes) &&
                      take_unless_zero(resources.channels.request_buffers_count, moving.request_buffers_count) &&
                      take_unless_zero(resources.channels.stream_buffer_bytes, moving.stream_buffer_bytes) &&
                      take_unless_zero(resources.channels.stream_buffers_count, moving.stream_buffers_count) &&
                      take_unless_zero(transactions.default_transaction.max_bytes, arguments.transaction_max_bytes) &&
                      take_unless_zero(transactions.default_transaction.max_deltas, arguments.transaction_max_deltas) &&
                      take_unless_zero(collections.concurrency_limit, arguments.collection_concurrency_limit) &&
                      take_unless_zero(collections.cache_limit, arguments.collection_cache_limit) &&
                      take_unless_zero(collections.memory_runs_max_bytes, arguments.collection_memory_runs_max_bytes) &&
                      take_unless_zero(collections.memory_runs_max_deltas,
                                       arguments.collection_memory_runs_max_deltas) &&
                      take_unless_zero(collections.level_enlarge_factor, arguments.collection_level_enlarge_factor) &&
                      take_unless_zero(collections.value_size, arguments.collection_value_size) &&
                      take_unless_zero(collections.value_max_size, arguments.collection_value_max_size);
    if (!fits) return {status_code_t::invalid_argument_k, "An option of the engine exceeds its range"};
    auto ignore_issue = [](issue_kind_t, std::string_view) noexcept {};
    if (!validate(resources, transactions, ignore_issue) || !validate(resources, collections, ignore_issue))
        return {status_code_t::invalid_argument_k, "The configuration of the engine is invalid"};
    return {};
}

/** Creates the collection @p collection named @p name, holding @p modality, shaped as @p shape, unless the store holds
 *  it already. */
template <cpu_class_t cpu_class_>
status_t reserve_collection(database_state<cpu_class_>& state, collection_id_t const& collection, std::string_view name,
                            ustore_modality_t modality, collection_config_t const& shape) noexcept {
    if (state.store->has_collection(collection)) return {};
    return create_collection(state, collection, name, modality, shape);
}

/** Drops every collection of the store the collections system collection does not list, and
 *  unlists every listed one the store does not hold, which a crash between a create's or a drop's
 *  two steps leaves behind. */
template <cpu_class_t cpu_class_>
status_t drop_unlisted(database_state<cpu_class_>& state, ustore_arena_t scratch) noexcept {
    st::vector<collection_ids_t> const held = state.store->list_collections();
    status_t first;
    for (std::size_t index = 0; index != held.size() && first.succeeded(); ++index) {
        ustore_collection_id_t const id = held[index].id;
        if (reserved(id)) continue;
        auto [entry, look_up_status] = look_up(state, scratch, id);
        if (look_up_status == status_code_t::not_found_k) first = state.store->drop_collection(id);
        else if (look_up_status.failed()) first = look_up_status;
    }
    if (first.failed()) return first;

    st::vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{scratch}};
    if (status_t const collected = list_collections(state, scratch, listed); collected.failed()) return collected;
    collections_guard<cpu_class_> const guarded {state};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
    for (listed_collection_t const& collection : listed) {
        if (state.store->has_collection(collection.id)) continue;
        if (!collection.entry.name.empty())
            if (status_t const unnamed = write_name(state, scratch, collection.entry.name, nullptr); unnamed.failed())
                return unnamed;
        if (status_t const unlisted = write_value(head, collections_collection_k, stored_of(collection.id.uuid), {});
            unlisted.failed())
            return unlisted;
    }
    return {};
}

/** The compactor every merge of @p state runs on: the device compaction @p state owns over an executor of its own on
 *  the device of @p compactions, and the host for null. */
template <cpu_class_t cpu_class_>
expected<compactor_ref_t> compactor_of(database_state<cpu_class_>& state, resources_config_t const& resources,
                                       ustore_executor_handle_t const* compactions) noexcept {
    if (!compactions) return compactor_ref_t {};
    typename device_compaction<cpu_class_>::volume_t const* volume = nullptr;
    [[maybe_unused]] status_t const visited = state.resources->storage.visit([&](auto& mount) noexcept -> status_t {
        if constexpr (std::is_same_v<std::remove_reference_t<decltype(mount)>, storage_mount_of<cpu_class_>>)
            volume = &mount.volume();
        return {};
    });
    if (!volume) return status_t {status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    device_compaction<cpu_class_>& compaction = state.compaction.emplace();
    if (status_t const opened = compaction.open(*compactions, resources, *volume); opened.failed()) return opened;
    if (cuda_compactor_t* const compactor = compaction.compactor()) return compactor_ref_t {compactor};
    return compactor_ref_t {};
}

/** Mounts the store of @p state over the storage its resources opened, which must be the one on files, merging on
 *  @p compactor. */
template <cpu_class_t cpu_class_>
status_t mount_store(database_state<cpu_class_>& state, database_config_t const& transactions,
                     compactor_ref_t compactor) noexcept {
    return state.resources->storage.visit([&](auto& mount) noexcept -> status_t {
        using mount_t = std::remove_reference_t<decltype(mount)>;
        if constexpr (std::is_same_v<mount_t, storage_mount_of<cpu_class_>>)
            return state.store->mount(mount.volume(), mount.channels(), transactions, compactor,
                                      strings_kit_sha256<strings_kit_of<cpu_class_>> {});
        else return {status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    });
}

/** Opens the store @p resources place, every collection of it shaped as @p shape and merged
 *  on @p compactions, reserving both system collections on the first open. */
template <cpu_class_t cpu_class_>
status_t open_state(database_state<cpu_class_>& state, resources_config_t const& resources,
                    database_config_t const& transactions, collection_config_t const& shape, ustore_arena_t scratch,
                    ustore_executor_handle_t const* compactions) noexcept {
    state.collection_shape = shape;
    state.transaction_shape = transactions.default_transaction;
    auto [opened, open_status] =
        open_resources<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>(resources);
    if (open_status.failed()) return open_status;
    state.resources = std::move(opened);
    state.store.reset(new (std::nothrow) basic_database_of<cpu_class_> {state.resources});
    if (!state.store) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the store"};
    auto [compactor, compactor_status] = compactor_of(state, resources, compactions);
    if (compactor_status.failed()) return compactor_status;
    if (status_t const mounted = mount_store(state, transactions, compactor); mounted.failed()) return mounted;

    // Both system collections take the records a recovery rebuilds them with, whatever bounds
    // the open names
    collection_config_t records = config_of(
        default_reserved_record(collections_collection_uuid_k, engine_value_max_size_k));
    collection_config_t names = config_of(default_reserved_record(names_collection_uuid_k, 0));
    records.concurrency_limit = names.concurrency_limit = shape.concurrency_limit;
    if (status_t const reserved = reserve_collection(state, collections_collection_k, "_collections",
                                                     ustore_modality_blobs_k, records);
        reserved.failed())
        return reserved;
    if (status_t const reserved = reserve_collection(state, names_collection_k, "_names", ustore_modality_strings_k,
                                                     names);
        reserved.failed())
        return reserved;
    return drop_unlisted(state, scratch);
}

/** Implements @c ustore_database_open_nvme, reserving both system collections on the first open. */
template <cpu_class_t cpu_class_>
status_t nvme_variant<cpu_class_>::open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a manifest"};

    resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = {engine_transaction_bytes_k, engine_transaction_deltas_k};
    user_collection_config_t collections = engine_collection_defaults();
    if (status_t const read = read_directories(arguments, resources); read.failed()) return read;
    char directory[path_max_bytes_k];
    if (status_t const located = manifest_directory_of(resources, directory); located.failed()) return located;
    if (directory[0] != '\0') {
        auto [exists, exists_status] = directory_exists(directory);
        if (exists_status.failed()) return exists_status;
        if (!exists) return {status_code_t::not_found_k, "The manifest's directory doesn't exist"};
    }
    ustore_executor_handle_t const* compactions = nullptr;
    if (status_t const read = read_options(arguments, resources, transactions, collections, compactions); read.failed())
        return read;
    auto [made, status] = make_handle<ustore_database_handle_t, database_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k);
    if (status.failed()) return status;
    if (status_t const opened = open_state<cpu_class_>(
            *made.state, resources, transactions, create_collection_config(collections), view.scratch, compactions);
        opened.failed())
        return opened;
    made.state->executor = nvme_placement_of(arguments.executor).executor;
    *arguments.database = made.release();
    return {};
}

/** Frees @p transaction, retiring it from the registry. */
template <cpu_class_t cpu_class_>
void transaction_free(ustore_transaction_t transaction) noexcept {
    delete state_of<transaction_state<cpu_class_>>(transaction);
}

/** Flushes and unmounts @p database ahead of freeing it, which every transaction of
 *  it must precede. */
template <cpu_class_t cpu_class_>
void database_free(ustore_database_t database) noexcept {
    delete state_of<database_state<cpu_class_>>(database);
}

#pragma endregion Lifetime

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init, pinning the newest published stamp in the store's ledger. */
template <cpu_class_t cpu_class_>
status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) noexcept {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [made, status] = make_handle<ustore_snapshot_handle_t, snapshot_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k, state);
    if (status.failed()) return status;
    transaction_stamp_t const stamp = state.store->ledger().pin_reader(made.state->pin);
    count_snapshot_opened<cpu_class_>(state.open_snapshots);
    if (arguments.read_stamp) *arguments.read_stamp = static_cast<ustore_stamp_t>(stamp);
    *arguments.snapshot = made.release();
    return {};
}

/** Copies every string of @p collection that @p access reads into @p target with its key, @p window strings per
 *  commit. */
template <cpu_class_t cpu_class_>
status_t save_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                      database_state<cpu_class_>& target, std::size_t window, progress_t& progress) noexcept {
    st::vector<char> from;
    st::vector<char> texts;
    st::vector<std::size_t> text_ends;
    st::vector<ustore_uuid_t> values;
    st::vector<string_task_t> tasks;
    for (;;) {
        texts.clear();
        text_ends.clear();
        values.clear();
        tasks.clear();
        status_t room;
        status_t const walked = walk_strings(
            access, collection, {}, std::string_view {from.begin(), from.end()}, access.stamp,
            [&](std::string_view text, lookup_result_t found, ustore_key_t const& value) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = growth_status(text_ends.push_back(offset + text.size()));
                if (room.succeeded()) room = growth_status(values.resize(values.size() + 1));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                values[values.size() - 1] = value.uuid;
                return text_ends.size() != window;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (text_ends.empty()) return {};
        if (status_t const reserved = growth_status(tasks.reserve(text_ends.size())); reserved.failed())
            return reserved;
        for (std::size_t index = 0, begin = 0; index != text_ends.size(); begin = text_ends[index++]) {
            std::string_view const text {texts.data() + begin, text_ends[index] - begin};
            [[maybe_unused]] st::status_t const placed = tasks.push_back(
                string_task_t {collection, text, delta_kind_t::write_k, values[index]});
            if (!progress.advance(text.size() + sizeof(ustore_uuid_t))) return progress_cancelled_k;
        }
        if (status_t const written = write_strings_alone(target, access.scratch,
                                                         std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (text_ends.size() < window) return {};
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A save could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/** Copies every pair of @p collection that @p access reads into @p target, under the same identifier and bounds. */
template <cpu_class_t cpu_class_>
status_t save_collection(call_access<cpu_class_> const& access, collection_ids_t const& collection,
                         database_state<cpu_class_>& target, progress_t& progress) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::uint16_t store_tag = 0;
    if (status_t const held = store.collection_store_tag(collection.id, store_tag); held.failed()) return held;
    if (!target.store->has_collection(collection.id)) {
        collection_config_t config;
        if (status_t const shaped = store.collection_config(collection.id, config); shaped.failed()) return shaped;
        if (status_t const made = target.store->create_collection(collection.id, collection.name.view(), store_tag,
                                                                  config);
            made.failed())
            return made;
    }

    // One window of keys is one commit's memory run of the target, which it lands without a flush in between
    collection_config_t config;
    if (status_t const shaped = target.store->collection_config(collection.id, config); shaped.failed()) return shaped;
    std::size_t const window = config.memory_runs_max_deltas;
    if (entry_layout_of(modality_of(store_tag)) == entry_layout_t::strings_to_uuids_k)
        return save_strings(access, collection.id, target, window, progress);
    st::vector<stored_key_t> keys;
    st::vector<byte_t> value;
    stored_key_t from = limit<stored_key_t>::min();
    for (;;) {
        keys.clear();
        if (status_t const collected = collect_keys(access, collection.id, from, limit<stored_key_t>::max(), window,
                                                    keys);
            collected.failed())
            return collected;
        for (stored_key_t const& key : keys) {
            stored_value_size_t size = 0;
            if (status_t const measured = store.stored_value_size(collection.id, key, size, access.stamp,
                                                                  access.thread_index);
                measured.failed())
                return measured;
            if (value.resize(size) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "Failed to hold a value the save copies"};
            stored_value_size_t copied = 0;
            if (status_t const read = store.select(collection.id, key, std::span<byte_t> {value}, copied, access.stamp,
                                                   access.thread_index);
                read.failed())
                return read;
            if (status_t const written = target.store->insert(collection.id, key,
                                                              stored_value_view_t {value.data(), copied});
                written.failed())
                return written;
            if (!progress.advance(sizeof(stored_key_t) + copied)) return progress_cancelled_k;
        }
        if (keys.size() < window) return {};
        from = keys[keys.size() - 1];
        if (!advance_key(from)) return {};
    }
}

/**
 *  @brief Implements @c ustore_snapshot_save as a database of this engine at the manifest path or
 *      the raw address it names, holding every collection as the snapshot reads it.
 *
 *  A save its progress cancels, or one that fails, removes the run files and the manifest it wrote
 *  and the directory it made, so a retry at the same path saves afresh.
 */
template <cpu_class_t cpu_class_>
status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) noexcept {
    if (!view.snapshot) return {status_code_t::invalid_argument_k, "A save reads through a snapshot"};
    database_state<cpu_class_>& source = database_state_of<cpu_class_>(arguments);
    call_access<cpu_class_> const access = access_of(source, view);

    ustore_database_open_nvme_t saving {};
    saving.directory = arguments.path;
    if (!saving.directory.length) return {status_code_t::invalid_argument_k, "A save needs a manifest path"};
    resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = source.transaction_shape;
    user_collection_config_t collections = engine_collection_defaults();
    if (status_t const read = read_directories(saving, resources); read.failed()) return read;
    // The saved copy merges on host threads, whatever device the source merges on
    ustore_executor_handle_t const* host_merges = nullptr;
    if (status_t const read = read_options(saving, resources, transactions, collections, host_merges); read.failed())
        return read;
    char directory[path_max_bytes_k];
    if (status_t const located = manifest_directory_of(resources, directory); located.failed()) return located;
    // A raw target names no directory, so a failed save there leaves nothing to remove
    bool existed = true;
    if (directory[0] != '\0') {
        auto [found, exists_status] = directory_exists(directory);
        if (exists_status.failed()) return exists_status;
        existed = found;
    }

    auto [addresses, addresses_status] = addresses_of(resources);
    if (addresses_status.failed()) return addresses_status;
    volume_config_t target;
    target.database = addresses.database;
    target.data_directories = std::span<data_address_t const> {addresses.data_directories};
    auto report = [&](std::uint64_t placed, std::uint64_t total) noexcept {
        return !arguments.progress || arguments.progress(placed, total, arguments.progress_state) != ustore_cancel_k;
    };
    status_t const saved =
        source.store->save(target, access.stamp, report, strings_kit_sha256<strings_kit_of<cpu_class_>> {}).status();
    // A cancelled or failed save gave back every run it placed, and the directory it made
    // goes with them
    if (saved.failed() && !existed) [[maybe_unused]]
        status_t const emptied = remove_directory(directory);
    return saved;
}

/** Frees the snapshot behind @p handle, letting go of the stamp it pinned, and with the last one the runs of the
 *  collections dropped meanwhile. */
template <cpu_class_t cpu_class_>
void snapshot_free(ustore_snapshot_t handle) noexcept {
    auto* const snapshot = state_of<snapshot_state<cpu_class_>>(handle);
    database_state<cpu_class_>& state = snapshot->state;
    delete snapshot;
    if (count_snapshot_closed<cpu_class_>(state.open_snapshots)) drop_retired(state);
}

#pragma endregion Snapshot Calls

#pragma region Change Feed Calls

/** What a change feed holds, behind its cursor handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct changes_state {

    /** The pin keeping versions above the feed's stamp from merges until the feed is freed. */
    typename basic_database_of<cpu_class_>::pin_t pin;

    /** The last change the feed handed out, or one at its stamp past all keys before a pull. */
    typename basic_database_of<cpu_class_>::stored_change_t handed;
};

/** What the C ABI calls a change of @p kind. */
constexpr ustore_change_kind_t change_kind_of(delta_kind_t kind) noexcept {
    switch (kind) {
    case delta_kind_t::remove_k: return ustore_change_removal_k;
    case delta_kind_t::change_k: return ustore_change_change_k;
    default: return ustore_change_write_k;
    }
}

/** Implements @c ustore_changes_cursor_init, pinning the stamp in the store's ledger, for every
 *  collection but a strings one, whose runs keep no version of a string apart from the next. */
template <cpu_class_t cpu_class_>
status_t changes_cursor_init(ustore_changes_cursor_init_t const& arguments, ustore_changes_cursor_t cursor,
                             view_t const& view) noexcept {
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [entry, look_up_status] = look_up(state, view.scratch, arguments.collection);
    if (look_up_status.failed()) return look_up_status;
    if (entry_layout_of(entry.modality) == entry_layout_t::strings_to_uuids_k)
        return {status_code_t::unsupported_k, "A strings collection feeds no changes"};
    auto* const feed = new (std::nothrow) changes_state<cpu_class_> {};
    if (!feed) return {status_code_t::out_of_memory_k, "Failed to allocate the change feed"};
    feed->handed.stamp = static_cast<transaction_stamp_t>(arguments.since_stamp);
    feed->handed.stored_key = limit<stored_key_t>::max();
    status_t const pinned = state.store->pin_feed(arguments.collection, feed->pin, feed->handed.stamp);
    if (pinned.failed()) {
        delete feed;
        return pinned;
    }
    cursor->state = feed;
    return {};
}

/** Implements @c ustore_changes_cursor_next, advancing once every output reached the arena. */
template <cpu_class_t cpu_class_>
status_t changes_cursor_next(ustore_changes_cursor_next_t const& arguments, view_t const& view) noexcept {
    using stored_change_t = typename basic_database_of<cpu_class_>::stored_change_t;
    changes_state<cpu_class_>& feed = *static_cast<changes_state<cpu_class_>*>(arguments.cursor->state);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    ustore_collection_id_t const collection = arguments.cursor->cursor.collection;
    // A dropped collection lingers while a snapshot may read it, so the listing tells if it is gone
    if (status_t const listed = look_up(state, view.scratch, collection).status(); listed.failed()) return listed;

    st::vector<ustore_key_t, arena_allocator<ustore_key_t>> keys {{view.scratch}};
    st::vector<ustore_stamp_t, arena_allocator<ustore_stamp_t>> stamps {{view.scratch}};
    st::vector<ustore_change_kind_t, arena_allocator<ustore_change_kind_t>> kinds {{view.scratch}};
    growing_strings<arena_allocator<byte_t>> values {view.scratch};
    st::vector<byte_t, arena_allocator<byte_t>> value {{view.scratch}};
    stored_change_t handed = feed.handed;
    auto take = [&]<typename read_type_>(stored_change_t const& change, std::uint64_t value_bytes,
                                         read_type_&& read) noexcept -> status_t {
        if (status_t const grown = growth_status(keys.push_back(key_of(change.stored_key))); grown.failed())
            return grown;
        if (status_t const grown = growth_status(stamps.push_back(static_cast<ustore_stamp_t>(change.stamp)));
            grown.failed())
            return grown;
        if (status_t const grown = growth_status(kinds.push_back(change_kind_of(change.kind))); grown.failed())
            return grown;
        if (arguments.values) {
            if (status_t const grown = growth_status(value.resize(value_bytes)); grown.failed()) return grown;
            if (status_t const copied = read(std::span<byte_t> {value}); copied.failed()) return copied;
            if (status_t const grown = values.push_back(value_view_t {std::span<byte_t const> {value}}.bytes());
                grown.failed())
                return grown;
        }
        handed = change;
        return {};
    };
    if (status_t const pulled = state.store->changes_after(collection, feed.handed, arguments.wanted, take,
                                                           thread_index_of(state, view));
        pulled.failed())
        return pulled;

    auto [exported_keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, keys);
    if (keys_status.failed()) return keys_status;
    auto [exported_stamps, stamps_status] = arena_copy<ustore_stamp_t>(arguments.arena, stamps);
    if (stamps_status.failed()) return stamps_status;
    auto [exported_kinds, kinds_status] = arena_copy<ustore_change_kind_t>(arguments.arena, kinds);
    if (kinds_status.failed()) return kinds_status;
    if (arguments.values)
        if (status_t const exported = values.export_into(arguments.arena, *arguments.values); exported.failed())
            return exported;
    *arguments.keys = exported_keys.data();
    *arguments.stamps = exported_stamps.data();
    *arguments.kinds = exported_kinds.data();
    *arguments.count = static_cast<ustore_size_t>(keys.size());
    feed.handed = handed;
    return {};
}

/** Frees the feed behind @p cursor, letting go of the stamp it pinned. */
template <cpu_class_t cpu_class_>
void changes_cursor_free(ustore_changes_cursor_t cursor) noexcept {
    delete static_cast<changes_state<cpu_class_>*>(cursor->state);
    cursor->state = nullptr;
}

#pragma endregion Change Feed Calls

} // namespace unum::ustore::nvme
