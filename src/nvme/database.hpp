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

#include <algorithm>   // `std::sort`
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
#include "ustore/strings.h"
#include "ustore/utilities/ranges.hpp" // `strided`

#include "ustore/blobs/nvme_database.hpp"          // `basic_database`, `lookup_result_t`
#include "ustore/blobs/nvme_transactions.hpp"      // `watch_schedule_t`
#include "ustore/blobs/nvme_tree.hpp"              // `user_collection_config_t`, `validate`
#include "ustore/blobs/vram_amd_compaction.hpp"    // `hip_compactor_t`
#include "ustore/blobs/vram_apple_compaction.hpp"  // `metal_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/docs/nvme_store.hpp"              // `docs::nvme_store`
#include "ustore/graph/nvme_store.hpp"             // `graph::nvme_store`
#include "ustore/graph/vram_adjacency_fold.hpp"    // `vram_adjacency_fold_t`
#include "ustore/pages/nvme_store.hpp"             // `pages::nvme_store`
#include "ustore/strings/nvme_store.hpp"           // `strings::nvme_store`, `nvme_transaction`, `strings_page_k`
#include "ustore/utilities/blocking_files.hpp"     // `directory_exists`, `path_max_bytes_k`
#include "ustore/utilities/nvme_mounts.hpp" // `resources_config_t`, `open_resources`, `mounted_storage`, `addresses_of`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `arena_copy`, `growing_strings`, `growth_status`

#include "engine.hpp"             // `engine_t`, `ustore_*_handle_t`, `state_of`, `view_t`, `nvme_queues_of`
#include "lib.hpp"                // `cpu_class_t`, `atomic_ref_for_cpu_class`, `waiter_of`, `strings_kit_of`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/outputs.hpp"  // `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

#include "nvme/compaction_variants.hpp" // the Apple compactor's instantiations, declared extern

namespace unum::ustore::nvme {
using namespace unum::ustore::blobs;
using namespace unum::ustore::strings;

#pragma region Store

/** The key every collection of this engine stores: the 16 bytes of the C ABI's key, ordered like @c memcmp. */
using stored_key_t = st::key128_t;

/** The mount the volumes of a store of @p cpu_class_ are opened over, which gives it its
 *  router and channels. */
template <cpu_class_t cpu_class_>
using storage_mount_of = typename util::mounted_storage<atomic_ref_for_cpu_class<cpu_class_>::template type,
                                                        waiter_of<cpu_class_>>::files_mount_t;

/** The cuFile disk a store of @p cpu_class_ reads its run sections into device memory through. */
template <cpu_class_t cpu_class_>
using sections_disk_of =
    util::linux_cufile_files_disk<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>;

/** What reads the sections of a store of @p cpu_class_'s runs into device memory, over a cuFile disk of its own. */
template <cpu_class_t cpu_class_>
using sections_reader_of = ring_sections_reader<sections_disk_of<cpu_class_>, waiter_of<cpu_class_>,
                                                typename storage_mount_of<cpu_class_>::volume_t>;

/** What composes the half-edge changes of a graph's merge on a device, whichever vendor's compactor
 *  runs it. */
using graph_device_fold_t = graph::vram_adjacency_fold_t;

/** The store of a blobs collection, and of the collections system collection, whose values follow
 *  @p value_layout_. */
template <cpu_class_t cpu_class_, value_layout_t value_layout_>
using blobs_store_of =
    blobs::nvme_store<key_slot<stored_key_t, value_layout_>, typename storage_mount_of<cpu_class_>::volume_t,
                      atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                      util::last_write_fold_t, blobs::no_device_fold_t, row_kit_of<cpu_class_>>;

/** The store of a graph collection. */
template <cpu_class_t cpu_class_>
using graph_store_of = graph::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                         atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                         graph_device_fold_t, row_kit_of<cpu_class_>>;

/** The store of a docs collection. */
template <cpu_class_t cpu_class_>
using docs_store_of = docs::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                       atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                       blobs::no_device_fold_t, row_kit_of<cpu_class_>>;

/** The store of a pages collection. */
template <cpu_class_t cpu_class_>
using pages_store_of = pages::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                         atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                         blobs::no_device_fold_t, row_kit_of<cpu_class_>>;

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
                   atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, row_kit_of<cpu_class_>,
                   store_tags_t, strings_store_of<cpu_class_>, graph_store_of<cpu_class_>, docs_store_of<cpu_class_>,
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

/** One collection of that database read at one state for a whole call. */
template <cpu_class_t cpu_class_>
using collection_reading_of = typename basic_database_of<cpu_class_>::collection_reading_t;

/** @p uuid as the collection ID the C ABI addresses a collection by. */
constexpr ustore_collection_id_t collection_id_of(util::uuid128_t const& uuid) noexcept {
    ustore_collection_id_t collection {};
    for (std::size_t index = 0; index != util::uuid128_t::size_bytes_k; ++index)
        collection.uuid.bytes[index] = uuid.bytes[index];
    return collection;
}

/** The collections system collection, keeping every collection's record and name, which no minted
 *  UUIDv7 spells, as its byte 6 is @c 0x7X. */
inline constexpr ustore_collection_id_t collections_collection_k = collection_id_of(
    util::collections_collection_uuid_k);

/** The names system collection, a strings collection mapping every collection's name onto its UUID, which no minted
 *  UUIDv7 spells either. */
inline constexpr ustore_collection_id_t names_collection_k = collection_id_of(util::names_collection_uuid_k);

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
inline constexpr util::byte_t empty_value_bytes_k[1] {};

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
    config.cache_limit = util::engine_collection_bytes_k;
    config.memory_runs_max_deltas = util::engine_memory_runs_deltas_k;
    config.memory_runs_max_bytes = util::engine_collection_bytes_k;
    config.level_enlarge_factor = util::engine_level_enlarge_factor_k;
    config.value_max_size = util::engine_value_max_size_k;
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
    static blobs_verbs<vendored_residency_t::host_k> const values_verbs_k;

    /** The verbs of this class's blobs collections. */
    static blobs_verbs<vendored_residency_t::host_k> const blobs_verbs_k;

    /** The verbs of this class's strings collections. */
    static strings_verbs<vendored_residency_t::host_k> const strings_verbs_k;

    /** The verbs of this class's documents collections. */
    static docs_verbs<vendored_residency_t::host_k> const docs_verbs_k;

    /** The verbs of this class's graph collections. */
    static graph_verbs<vendored_residency_t::host_k> const graph_verbs_k;

    /** The verbs of this class's pages collections. */
    static pages_verbs<vendored_residency_t::host_k> const pages_verbs_k;

    /** One compaction of a database of this class in flight. */
    struct compact_plan_t;

    /** The table every handle the open of this class hands out is stamped with. */
    static engine_t const engine_k;

    /** Implements @c ustore_database_open_nvme over a store of this class, and for @p access of
     *  @c access_t::read_only_k @c ustore_database_view_nvme, which writes to no data directory. */
    static util::status_t open(ustore_database_open_nvme_t const& arguments, access_t access,
                               view_t const& view) noexcept;
};

/**
 *  @brief The device merges of a store of @p cpu_class_: an executor of their own on the device of the caller's, what
 *      reads run sections into device memory over a cuFile disk of its own, and the compactor over both.
 *
 *  Sections read by DMA on an NVIDIA device where the executor's GPUDirect Storage mode allows it
 *  and cuFile reaches the device, since compat mode bounces through host pages, slower than
 *  uploading the indexes the runs already hold in memory; AMD and Apple devices upload them from
 *  memory alone. The members die in reverse, the compactor first, so it frees its scratch while the
 *  reader and the executor live.
 */
template <cpu_class_t cpu_class_>
class device_compaction {
  public:

    /** The volume the reader names run files through. */
    using volume_t = typename storage_mount_of<cpu_class_>::volume_t;

    device_compaction() noexcept = default;
    device_compaction(device_compaction const&) = delete;
    device_compaction& operator=(device_compaction const&) = delete;

    /** Spawns the executor on the device @p compactions computes on and binds the compactor over
     *  it, refusing with @c unsupported_k where the device loaded no bundle of the library's
     *  kernels; on an NVIDIA device the compactor reads run sections through the reader over
     *  @p volume, mounted over the data directories of @p resources, and on an AMD or an Apple one
     *  it uploads them from memory. */
    util::status_t open(ustore_executor_handle_t const& compactions, util::resources_config_t const& resources,
                        volume_t const& volume) noexcept {
        util::gpu_device_t twin {};
        std::size_t scratch_limit = 0;
        bool const on_device = std::visit(
            [&]<typename device_type_>(device_type_ const& device) noexcept {
                if constexpr (!util::gpu_executor_concept<device_type_>) return false;
                else {
                    twin = device.device();
                    scratch_limit = device.scratch_limit();
                    return true;
                }
            },
            compactions.device);
        if (!on_device)
            return {util::status_code_t::device_k, "The compactions need an executor of ustore_executor_for_vram"};
        util::cuda_executor_t const* const nvidia = std::get_if<util::cuda_executor_t>(&compactions.device);
        gpu_runtime_t& runtime = gpu_runtime();
        util::gpu_drivers_t const& drivers = runtime.drivers;
        if (util::status_t const spawned = runtime.spawn(twin, compactions.gpu_class, scratch_limit,
                                                         nvidia ? nvidia->gds() : ustore_gds_compatibility_k,
                                                         executor_);
            spawned.failed())
            return spawned;
        return std::visit([&](auto& executor) noexcept { return bind_(executor, drivers, resources, volume); },
                          executor_);
    }

    /** The compactor every merge runs on: the device one @c open bound, or the host's before. */
    compactor_ref_t compactor() noexcept {
        return std::visit(
            []<typename compactor_type_>(compactor_type_& compactor) noexcept -> compactor_ref_t {
                if constexpr (std::is_same_v<compactor_type_, std::monostate>) return host_compactor_t {};
                else return &compactor;
            },
            compactor_);
    }

  private:

    /** Refuses an executor slot no spawn filled, which a successful spawn never leaves. */
    util::status_t bind_(std::monostate&, util::gpu_drivers_t const&, util::resources_config_t const&,
                         volume_t const&) noexcept {
        return {util::status_code_t::internal_k, "The compactions spawned no executor"};
    }

    /** Binds the compactor over @p executor, reading run sections over @p volume where cuFile
     *  reaches the NVIDIA device. */
    util::status_t bind_(util::cuda_executor_t& executor, util::gpu_drivers_t const& drivers,
                         util::resources_config_t const& resources, volume_t const& volume) noexcept {
        if (util::status_t const opened = open_sections(drivers.cufile, executor.gds(), resources, volume);
            opened.failed())
            return opened;
        auto [compactor, status] = cuda_compactor_t::make(
            executor, sections_ ? device_sections_reader_t {&sections_.value()} : device_sections_reader_t {});
        if (status.failed()) return status;
        compactor_.template emplace<cuda_compactor_t>(std::move(compactor));
        return {};
    }

    /** Binds the compactor over @p executor, uploading run sections from memory, as no AMD device
     *  reads them by DMA. */
    util::status_t bind_(util::hip_executor_t& executor, util::gpu_drivers_t const&, util::resources_config_t const&,
                         volume_t const&) noexcept {
        auto [compactor, status] = hip_compactor_t::make(executor);
        if (status.failed()) return status;
        compactor_.template emplace<hip_compactor_t>(std::move(compactor));
        return {};
    }

    /** Binds the compactor over @p executor, uploading run sections from memory, as no Apple device
     *  reads them by DMA. */
    util::status_t bind_(util::metal_executor_t& executor, util::gpu_drivers_t const&, util::resources_config_t const&,
                         volume_t const&) noexcept {
        auto [compactor, status] = metal_compactor_t::make(executor);
        if (status.failed()) return status;
        compactor_.template emplace<metal_compactor_t>(std::move(compactor));
        return {};
    }

    /** Opens the reader over every raw medium of @p resources, or over the run files of @p volume
     *  on directories, where @p gds lets bytes move by DMA and cuFile reaches the device,
     *  refusing @c ustore_gds_direct_k where it does not. */
    util::status_t open_sections(util::cufile_api_t const& cufile, ustore_gds_t gds,
                                 util::resources_config_t const& resources, volume_t const& volume) noexcept {
        if (gds == ustore_gds_compatibility_k) return {};
        if (sections_disk_of<cpu_class_>::probe(cufile) != util::linux_cufile_probe_t::direct_k) {
            if (gds == ustore_gds_direct_k)
                return {util::status_code_t::device_k, "GPUDirect Storage lacks the nvidia_fs module"};
            return {};
        }
        auto [addresses, addresses_status] = util::addresses_of(resources);
        if (addresses_status.failed()) return addresses_status;
        util::linux_cufile_disk_config_t config;
        config.gds = ustore_gds_direct_k;
        if (addresses.data_directories[0].kind == util::data_address_kind_t::directory_k) {
            // Every run a merge reads is a file of its own, registered while that merge runs.
            config.media_limit = merged_run_files_k;
            auto [disk, disk_status] = sections_disk_of<cpu_class_>::make(cufile, config);
            if (disk_status.failed()) return disk_status;
            auto [reader, reader_status] = sections_reader_of<cpu_class_>::make(std::move(disk), volume);
            if (reader_status.failed()) return reader_status;
            sections_.emplace(std::move(reader));
            return {};
        }
        std::size_t const media_count = addresses.data_directories.size();
        if (media_count > config.media_limit) config.media_limit = static_cast<std::uint16_t>(media_count);
        auto [disk, disk_status] = sections_disk_of<cpu_class_>::make(cufile, config);
        if (disk_status.failed()) return disk_status;
        st::vector<std::array<char, util::path_max_bytes_k>> paths;
        st::vector<util::medium_options_t> media;
        if (paths.resize(media_count) != st::status_t::success_k ||
            media.resize(media_count) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "The compactions could not track their media"};
        // Raw media are read in data directory order, which is the index a run's extent names.
        for (std::size_t index = 0; index != media_count; ++index) {
            std::string_view const location = addresses.data_directories[index].location;
            if (location.size() >= util::path_max_bytes_k)
                return {util::status_code_t::invalid_argument_k, "A raw medium's path outgrew the platform's limit"};
            std::memcpy(paths[index].data(), location.data(), location.size());
            paths[index][location.size()] = '\0';
            media[index].name = paths[index].data();
            media[index].access = util::medium_access_t::read_only_k;
        }
        auto [reader, reader_status] = sections_reader_of<cpu_class_>::make(
            std::move(disk), std::span<util::medium_options_t const> {media});
        if (reader_status.failed()) return reader_status;
        sections_.emplace(std::move(reader));
        return {};
    }

    /** Run files one merge on a directory volume reads at most. */
    static constexpr std::uint16_t merged_run_files_k = 1024;

    std::variant<std::monostate, util::cuda_executor_t, util::hip_executor_t, util::metal_executor_t> executor_;
    std::optional<sections_reader_of<cpu_class_>> sections_;
    std::variant<std::monostate, cuda_compactor_t, hip_compactor_t, metal_compactor_t> compactor_;
};

/** Everything one open database holds, behind its database handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct database_state {

    /** The budgets and the mount the store views its router and channels from. */
    util::resources_shared_ptr<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>> resources;

    /** What every device merge of the store runs on, empty for host merges, outliving the store. */
    std::optional<device_compaction<cpu_class_>> compaction;

    /** The store itself, unmounted before the mount underneath it is torn down. */
    std::unique_ptr<basic_database_of<cpu_class_>> store;

    /** The shape every collection this database creates takes. */
    collection_config_t collection_shape;

    /** The buffer every caller's transaction takes. */
    transaction_config_t transaction_shape;

    /** Held by whoever changes both system collections, so a name is checked and taken in one step. */
    std::int32_t collections_claim = 0;

    /** Snapshots and transactions not yet freed, or the mark of a collection drop in progress,
     *  through the class's atomic reference. */
    std::size_t open_readers = 0;

    database_state() noexcept = default;
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;

    /** Lets the store's collections go without flushing them, then unmounts the storage under
     *  it, before letting both go, since a collection or buffer may keep the resources alive
     *  past the database. */
    ~database_state() noexcept {
        store.reset();
        if (resources) { [[maybe_unused]] util::status_t const released = resources->storage.unmount(); }
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

/** Holds @c open_readers of one database at the dropping mark for the life of the guard, when no
 *  reader is open, so no snapshot or transaction opens while a drop returns runs to the volume. */
template <cpu_class_t cpu_class_>
class dropping_guard {
    std::size_t* open_readers_ = nullptr;

  public:
    /** The high bit, which no count of readers reaches. */
    static constexpr std::size_t dropping_k = std::size_t {1} << (sizeof(std::size_t) * 8 - 1);

    /** Holds no mark, for a call that drops nothing. */
    dropping_guard() noexcept = default;

    explicit dropping_guard(database_state<cpu_class_>& state) noexcept {
        std::size_t expected_open = 0;
        if (atomic_ref_of<cpu_class_, std::size_t> {state.open_readers}.compare_exchange_strong(
                expected_open, dropping_k, std::memory_order_acquire, std::memory_order_relaxed))
            open_readers_ = &state.open_readers;
    }
    dropping_guard(dropping_guard&& other) noexcept : open_readers_(std::exchange(other.open_readers_, nullptr)) {}
    dropping_guard(dropping_guard const&) = delete;
    dropping_guard& operator=(dropping_guard const&) = delete;
    dropping_guard& operator=(dropping_guard&&) = delete;

    /** Clears the mark, keeping the counts of readers that found it and are backing out. */
    ~dropping_guard() noexcept {
        if (open_readers_)
            atomic_ref_of<cpu_class_, std::size_t> {*open_readers_}.fetch_sub(dropping_k, std::memory_order_release);
    }

    /** Whether the guard holds the mark, so the drop may proceed. */
    explicit operator bool() const noexcept { return open_readers_ != nullptr; }
};

/** Counts a reader of @p state open before its stamp is pinned, backing out and answering false
 *  while a collection drop holds the mark. */
template <cpu_class_t cpu_class_>
[[nodiscard]] bool open_reader(database_state<cpu_class_>& state) noexcept {
    atomic_ref_of<cpu_class_, std::size_t> const readers {state.open_readers};
    if ((readers.fetch_add(1, std::memory_order_acq_rel) & dropping_guard<cpu_class_>::dropping_k) == 0) return true;
    readers.fetch_sub(1, std::memory_order_release);
    return false;
}

/** The state of the database a call addresses. */
template <cpu_class_t cpu_class_, typename arguments_type_>
database_state<cpu_class_>& database_state_of(arguments_type_ const& arguments) noexcept {
    return *state_of<database_state<cpu_class_>>(database_of(arguments));
}

/** Opens a transaction of @p state whose buffer @p shape bounds. */
template <cpu_class_t cpu_class_>
util::expected<typename basic_database_of<cpu_class_>::transaction_unique_ptr_t> begin_transaction(
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

    /** The queue set of the call's executor and the thread running the call, which reads on
     *  that thread's rings and hints its epoch slot, or no queue set for a call whose
     *  executor holds none. */
    nvme_queues_ref_t queues;

    /** The strings the transaction writes and watches, null with no transaction. */
    strings_transaction_of<cpu_class_>* strings = nullptr;

    /** The one collection a call outside a transaction reads through one view for the whole
     *  call, or null for every read to load the view its own stamp reads. */
    collection_reading_of<cpu_class_> const* reading = nullptr;

    /** The pin holding @c stamp for a call reading several collections at the head, which
     *  every read of the call shares, released when the call returns. */
    typename basic_database_of<cpu_class_>::pin_t pin;
};

/** The schedule the watches of a call asking for @p watch take. */
constexpr watch_schedule_t schedule_of(ustore_watch_t watch) noexcept {
    return watch == ustore_watch_keys_k ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k;
}

/** One set of blocking queues of @p threads threads for a call naming no NVMe executor, freed when
 *  the call returns, at the 1 MiB shape of @c blocking_queues_config_k, which reach a database on
 *  any files transport and never open a ring, an AIO context or an SPDK queue pair. */
inline util::expected<util::nvme_queues_t> make_call_queues(std::size_t threads) noexcept {
    st::vector<fu_memory_domain_id_t> memory_domains;
    if (memory_domains.resize(threads, fu_memory_domain_id_t {-1}) != st::status_t::success_k)
        return util::status_t {util::status_code_t::out_of_memory_k, "A call could not size its queues"};
    return util::nvme_queues_t::make(util::io_transport_t::blocking_k, util::blocking_queues_config_k, memory_domains);
}

/** The queue set a call running on @p view's thread reads and writes on: its NVMe executor's, at
 *  its thread, else the queues of the router's frame, built here on this first ask, one thread per
 *  slot of the call, which refuse every transfer if they do not fit. */
inline nvme_queues_ref_t nvme_queues_of(view_t const& view) noexcept {
    if (util::nvme_queues_t* const own = nvme_queues_of(view.executor)) return {own, view.thread_index};
    if (view.queues && view.queues->threads() == 0) {
        std::size_t const slots = view.executor ? view.executor->scratches.size() : 1;
        auto [made, make_status] = make_call_queues(slots);
        if (make_status.succeeded()) *view.queues = std::move(made);
    }
    return {view.queues, view.thread_index};
}

/** The access of a call reading through @p view: its transaction, its snapshot's stamp, the
 *  stamp or the reading the router took, or the head, which @c read_at_one_state holds. */
template <cpu_class_t cpu_class_>
call_access<cpu_class_> access_of(database_state<cpu_class_>& state, view_t const& view) noexcept {
    nvme_queues_ref_t const queues = nvme_queues_of(view);
    if (view.snapshot)
        return {state,
                nullptr,
                watch_schedule_t::never_k,
                view.scratch,
                state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp(),
                queues};
    if (!view.transaction)
        return {state,
                nullptr,
                watch_schedule_t::never_k,
                view.scratch,
                view.stamp ? static_cast<transaction_stamp_t>(view.stamp) : head_stamp_k,
                queues,
                nullptr,
                static_cast<collection_reading_of<cpu_class_> const*>(view.reading)};
    transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
    return {state,  transaction.held.get(), schedule_of(view.watch), view.scratch, head_stamp_k,
            queues, &transaction.strings};
}

/** The stamp a read through @p access answers at: its transaction's start, its snapshot's, or
 *  the head's. */
template <cpu_class_t cpu_class_>
transaction_stamp_t read_stamp_of(call_access<cpu_class_> const& access) noexcept {
    return access.transaction ? access.transaction->start_stamp() : access.stamp;
}

/** Where the tasks on the collection of task @p first end: the next task naming another one. */
inline std::size_t run_end(util::strided_iterator<ustore_collection_id_t const> collections, std::size_t first,
                           std::size_t count) noexcept {
    std::size_t end = first + 1;
    while (end != count && collections[end] == collections[first]) ++end;
    return end;
}

/**
 *  @brief Holds one state for every read of a call through @p access over the @p count
 *      collections @p collections names, outside a transaction and a pinned stamp.
 *
 *  One collection is read through its view, loaded once into @p held, and several at a stamp
 *  pinned for the call, as a merge keeps the versions at a stamp only for a pinned reader.
 */
template <cpu_class_t cpu_class_>
util::status_t read_at_one_state(call_access<cpu_class_>& access,
                                 util::strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                                 collection_reading_of<cpu_class_>& held) noexcept {
    if (access.transaction || access.stamp != head_stamp_k || count == 0) return {};
    bool const one = run_end(collections, 0, count) == count;
    if (one && access.reading && access.reading->id() == collections[0]) return {};
    access.reading = nullptr;
    if (!one) {
        access.stamp = access.state.store->ledger().pin_reader(access.pin);
        return {};
    }
    auto [taken, taken_status] = access.state.store->read_collection(collections[0], head_stamp_k,
                                                                     access.queues.thread_index);
    if (taken_status.failed()) return taken_status;
    held = std::move(taken);
    access.reading = &held;
    return {};
}

/** Calls @p reads with a reading of @p collection at the state @p access reads, on executor thread
 *  @p thread_index: the one the call holds, or one loaded now at its stamp; answers as it does. */
template <cpu_class_t cpu_class_, typename reads_type_>
util::status_t with_reading(call_access<cpu_class_> const& access, collection_id_t collection, std::size_t thread_index,
                            reads_type_&& reads) noexcept {
    if (access.reading && access.reading->id() == collection) return reads(*access.reading);
    auto [taken, taken_status] = access.state.store->read_collection(collection, read_stamp_of(access), thread_index);
    if (taken_status.failed()) return taken_status;
    return reads(taken);
}

/** Watches @p keys of @p collection through the transaction of @p access, when it watches at all. */
template <cpu_class_t cpu_class_>
util::status_t watch_keys(call_access<cpu_class_> const& access, collection_id_t collection,
                          std::span<stored_key_t const> keys) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k || keys.empty()) return {};
    return access.transaction->template watch_more<watch_schedule_t::on_commit_k>(collection, keys);
}

/** Watches every key of @p collection from @p lower to @p upper through the transaction of @p access, when it watches
 *  at all, so a commit landing a key the walk over them did not find conflicts too. */
template <cpu_class_t cpu_class_>
util::status_t watch_range(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                           stored_key_t upper) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k) return {};
    return access.transaction->template watch_range<watch_schedule_t::on_commit_k>(collection, lower, upper);
}

/** Copies @p size bytes into the scratch of @p access through @p copy, answering the present value they hold. */
template <cpu_class_t cpu_class_, typename copy_type_>
util::expected<util::value_view_t> copied_value(call_access<cpu_class_> const& access, stored_value_size_t size,
                                                copy_type_&& copy) noexcept {
    if (size == 0) {
        if (util::status_t const copied = copy(std::span<util::byte_t> {}); copied.failed()) return copied;
        return util::value_view_t {empty_value_bytes_k, std::size_t {0}};
    }
    auto [bytes, allocate_status] = util::arena_allocate<util::byte_t>(access.scratch, size);
    if (allocate_status.failed()) return allocate_status;
    if (util::status_t const copied = copy(bytes); copied.failed()) return copied;
    return util::value_view_t {bytes};
}

/** The value a transaction's change @p own_delta of a key in @p collection folds @p committed
 *  into, through the fold of the store holding it, copied into the scratch of @p access, or a
 *  missing value where the fold leaves none. */
template <cpu_class_t cpu_class_>
util::expected<util::value_view_t> fold_own_change(call_access<cpu_class_> const& access, collection_id_t collection,
                                                   util::value_view_t committed,
                                                   util::delta_view_t const& own_delta) noexcept {
    // A present value of no bytes still views a byte, so a fold tells it from a missing one
    std::span<util::byte_t const> base;
    if (committed)
        base = committed.size() ? std::span<util::byte_t const> {committed.begin(), committed.size()}
                                : std::span<util::byte_t const> {empty_value_bytes_k, 0};
    util::value_view_t folded {};
    util::status_t const visited = access.state.store->visit_store(
        collection,
        [&]<typename store_type_>(store_type_&) noexcept -> status_t {
            util::arena_allocator<util::byte_t> arena {access.scratch};
            auto [applied, apply_status] = typename store_type_::fold_t {}.apply(
                base, std::span<util::delta_view_t const> {&own_delta, 1}, arena);
            if (apply_status == util::status_code_t::not_found_k) return {};
            if (apply_status.failed()) return apply_status;
            auto [copied, copy_status] = copied_value(access, static_cast<stored_value_size_t>(applied.size()),
                                                      [&](std::span<util::byte_t> into) noexcept {
                                                          if (!into.empty())
                                                              std::memcpy(into.data(), applied.data(), into.size());
                                                          return util::status_t {};
                                                      });
            folded = copied;
            return copy_status;
        },
        access.queues.thread_index);
    if (visited.failed()) return visited;
    return folded;
}

/** The value of @p key in @p collection as @p access sees it outside a transaction, measured and
 *  copied through one reading into its scratch, or a missing value. */
template <cpu_class_t cpu_class_>
util::expected<util::value_view_t> read_committed_value(call_access<cpu_class_> const& access,
                                                        collection_id_t collection, stored_key_t key) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    util::value_view_t read {};
    util::status_t const visited = with_reading(
        access, collection, access.queues.thread_index,
        [&](collection_reading_of<cpu_class_> const& reading) noexcept -> status_t {
            stored_value_size_t size = 0;
            util::status_t const measured = store.stored_value_size(reading, key, size, access.queues);
            if (measured == util::status_code_t::not_found_k) return {};
            if (measured.failed()) return measured;
            auto [copied, copy_status] = copied_value(access, size, [&](std::span<util::byte_t> into) noexcept {
                stored_value_size_t copied_size = 0;
                return store.select(reading, key, into, copied_size, access.queues);
            });
            read = copied;
            return copy_status;
        });
    if (visited.failed()) return visited;
    return read;
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
util::expected<util::value_view_t> read_value(call_access<cpu_class_> const& access, collection_id_t collection,
                                              stored_key_t key) noexcept {
    database_transaction_of<cpu_class_>* const transaction = access.transaction;
    if (!transaction) return read_committed_value(access, collection, key);
    util::delta_view_t own_delta;
    bool const changed = transaction->uncommitted_delta(collection, key, own_delta) &&
                         own_delta.kind == util::delta_kind_t::change_k;
    lookup_result_t const own_status = transaction->lookup_uncommitted(collection, key);
    if (own_status == lookup_result_t::removed_k) return util::value_view_t {};
    if (own_status == lookup_result_t::written_k && !changed) {
        stored_value_size_t size = 0;
        if (util::status_t const measured = transaction->stored_value_size_uncommitted(collection, key, size);
            measured.failed())
            return measured;
        return copied_value(access, size, [&](std::span<util::byte_t> into) noexcept {
            stored_value_size_t copied = 0;
            return transaction->select_uncommitted(collection, key, into, copied);
        });
    }

    // A value may change its size between the measurement and the copy, which the copy refuses rather than truncates
    for (std::size_t attempt = 0; attempt != read_attempts_k; ++attempt) {
        stored_value_size_t size = 0;
        util::status_t const measured = transaction->stored_value_size_committed(collection, key, size, access.queues);
        bool const missing = measured == util::status_code_t::not_found_k;
        if (measured.failed() && !missing) return measured;
        if (!missing) {
            auto const [value, copy_status] = copied_value(access, size, [&](std::span<util::byte_t> into) noexcept {
                stored_value_size_t copied = 0;
                if (access.schedule == watch_schedule_t::never_k)
                    return transaction->template select_committed<watch_schedule_t::never_k>(collection, key, into,
                                                                                             copied, access.queues);
                return transaction->template select_committed<watch_schedule_t::on_commit_k>(collection, key, into,
                                                                                             copied, access.queues);
            });
            if (copy_status.succeeded() && changed) return fold_own_change(access, collection, value, own_delta);
            if (copy_status.succeeded()) return value;
            if (copy_status == util::status_code_t::invalid_argument_k) continue;
            if (copy_status != util::status_code_t::not_found_k) return copy_status;
        }
        stored_key_t const absent[1] {key};
        if (util::status_t const watched = watch_keys(access, collection, absent); watched.failed()) return watched;
        if (changed) return fold_own_change(access, collection, util::value_view_t {}, own_delta);
        return util::value_view_t {};
    }
    return util::status_t {util::status_code_t::contended_k, "A value changed size while it was read"};
}

/** Writes @p value under @p key of @p collection, a missing value removing it: through the transaction of @p access,
 *  or straight into the store for both system collections, which no transaction reaches. */
template <cpu_class_t cpu_class_>
util::status_t write_value(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key,
                           util::value_view_t value) noexcept {
    stored_value_view_t const bytes {value};
    if (!access.transaction) {
        basic_database_of<cpu_class_>& store = *access.state.store;
        return value ? store.insert(collection, key, bytes, access.queues)
                     : store.remove(collection, key, access.queues);
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
util::status_t remove_key_range(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t start,
                                stored_key_t end) noexcept {
    if (!access.transaction)
        return {util::status_code_t::invalid_argument_k, "A range removal lands through a transaction"};
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
    util::delta_kind_t kind = util::delta_kind_t::write_k;
};

/**
 *  @brief Hands @p visitor every present key of @p collection from @p from up, or down for
 *      @p direction descending, as @p access sees it, in that order, as <tt>visitor(key, size)</tt>
 *      answering whether the walk continues.
 *
 *  The store's walk holds the view it loaded, which a flush waits for, so @p visitor collects
 *  keys and writes nothing.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
util::status_t walk(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t from,
                    visitor_type_&& visitor, scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    if (!access.transaction)
        return with_reading(access, collection, access.queues.thread_index,
                            [&](collection_reading_of<cpu_class_> const& reading) noexcept {
                                return store.scan(
                                    reading, access.scratch, from,
                                    [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
                                        return state != lookup_result_t::written_k || visitor(key, size);
                                    },
                                    access.queues, direction);
                            });

    bool const descending = direction == scan_direction_t::descending_k;
    auto const before = [descending](stored_key_t const& first, stored_key_t const& second) noexcept {
        return descending ? second < first : first < second;
    };

    st::vector<own_write_t, util::arena_allocator<own_write_t>> own_writes {{access.scratch}};
    util::status_t listed;
    access.transaction->for_each_uncommitted(
        collection, [&](stored_key_t key, util::delta_kind_t kind, stored_value_size_t size) noexcept {
            if (before(key, from) || listed.failed()) return;
            listed = util::growth_status(own_writes.push_back(own_write_t {key, size, kind}));
        });
    if (listed.failed()) return listed;
    // A change folds over the committed value, which tells its size and whether any value is left
    for (own_write_t& write : own_writes) {
        if (write.kind != util::delta_kind_t::change_k) continue;
        auto [folded, fold_status] = read_value(access, collection, write.key);
        if (fold_status.failed()) return fold_status;
        write.kind = folded ? util::delta_kind_t::write_k : util::delta_kind_t::remove_k;
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
    bool keeps_going = true;
    auto const emit_own = [&](own_write_t const& write) noexcept {
        if (write.kind != util::delta_kind_t::remove_k) keeps_going = visitor(write.key, write.size);
        return keeps_going;
    };
    auto const removed_in_range = [&](stored_key_t const& key) noexcept {
        for (range_tombstone<stored_key_t> const& range : own_ranges)
            if (range.covers(key)) return true;
        return false;
    };
    // The store's side is what the transaction started at, which its reads of single keys answer too
    util::status_t const walked = store.scan(
        collection, access.scratch, from,
        [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
            for (; next != own_writes.size() && before(own_writes[next].key, key); ++next)
                if (!emit_own(own_writes[next])) return false;
            if (next != own_writes.size() && own_writes[next].key == key) return emit_own(own_writes[next++]);
            if (state != lookup_result_t::written_k || removed_in_range(key)) return true;
            keeps_going = visitor(key, size);
            return keeps_going;
        },
        access.transaction->start_stamp(), access.queues, direction);
    if (walked.failed()) return walked;
    for (; keeps_going && next != own_writes.size(); ++next) emit_own(own_writes[next]);
    return {};
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p lower up to @p upper
 *  inclusive, as @p access sees them. */
template <cpu_class_t cpu_class_, typename keys_type_>
util::status_t collect_keys(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                            stored_key_t upper, std::size_t limit, keys_type_& keys) noexcept {
    util::status_t room;
    util::status_t const walked = walk(access, collection, lower, [&](stored_key_t key, stored_value_size_t) noexcept {
        if (upper < key) return false;
        room = util::growth_status(keys.push_back(stored_key_t {key}));
        return room.succeeded() && keys.size() < limit;
    });
    return walked.failed() ? walked : room;
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p upper down to
 *  @p lower inclusive, as @p access sees them, largest first, walking the runs downwards, so a page
 *  reads about its own keys per run. */
template <cpu_class_t cpu_class_, typename keys_type_>
util::status_t collect_keys_descending(call_access<cpu_class_> const& access, collection_id_t collection,
                                       stored_key_t lower, stored_key_t upper, std::size_t limit,
                                       keys_type_& keys) noexcept {
    if (limit == 0) return {};
    util::status_t room;
    util::status_t const walked = walk(
        access, collection, upper,
        [&](stored_key_t key, stored_value_size_t) noexcept {
            if (key < lower) return false;
            room = util::growth_status(keys.push_back(stored_key_t {key}));
            return room.succeeded() && keys.size() < limit;
        },
        scan_direction_t::descending_k);
    return walked.failed() ? walked : room;
}

/** Collects into @p keys up to @p limit keys of @p collection from @p lower up to @p upper
 *  inclusive, walking each run of one reading on its own with no merge across them, as an unordered
 *  scan may list a key another run removed or repeats. */
template <cpu_class_t cpu_class_, typename keys_type_>
util::status_t collect_keys_unordered(call_access<cpu_class_> const& access, collection_id_t collection,
                                      stored_key_t lower, stored_key_t upper, std::size_t limit,
                                      keys_type_& keys) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    return with_reading(access, collection, access.queues.thread_index,
                        [&](collection_reading_of<cpu_class_> const& reading) noexcept -> status_t {
                            auto [counted, counted_status] = store.runs_count(reading);
                            if (counted_status.failed()) return counted_status;
                            util::status_t room;
                            for (std::uint64_t run = 0; run != counted.count && keys.size() < limit; ++run) {
                                util::status_t const walked = store.scan_run(
                                    reading, counted, run, lower,
                                    [&](stored_key_t key, lookup_result_t state, stored_value_size_t) noexcept {
                                        if (upper < key) return false;
                                        if (state == lookup_result_t::removed_k) return true;
                                        room = util::growth_status(keys.push_back(stored_key_t {key}));
                                        return room.succeeded() && keys.size() < limit;
                                    });
                                if (walked.failed()) return walked;
                                if (room.failed()) return room;
                            }
                            return {};
                        });
}

/** The key after @p key, or answers false for the largest key, which has none. */
inline bool advance_key(stored_key_t& key) noexcept {
    if (key == limit<stored_key_t>::max_k) return false;
    key = stored_of(util::key_after(key_of(key)).uuid);
    return true;
}

#pragma endregion Access

#pragma region Maintenance

template <cpu_class_t cpu_class_>
util::status_t compact_due(database_state<cpu_class_>& state, view_t const& view) noexcept;

/** Makes landed writes durable when @p durability asks, the only failure a landed write can still
 *  report, as the commit's own maintenance already compacts what it left due. */
template <cpu_class_t cpu_class_>
util::status_t settle(database_state<cpu_class_>& state, ustore_durability_t durability,
                      nvme_queues_ref_t queues) noexcept {
    if (durability != ustore_durability_flushed_k) return {};
    return state.store->flush(queues);
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
util::status_t commit_one_collection(database_state<cpu_class_>& state, ustore_arena_t scratch,
                                     nvme_queues_ref_t queues, std::size_t count, std::size_t bytes,
                                     watch_schedule_t schedule, writes_type_&& writes) noexcept {
    transaction_config_t shape;
    shape.max_deltas = fu::max_of<std::size_t>(count, 1);
    shape.max_bytes = fu::max_of<std::size_t>(bytes, 1);
    for (std::size_t attempt = 0; attempt != commit_attempts_k; ++attempt) {
        // Declared first, so the transaction it joins dies before it does.
        strings_transaction_of<cpu_class_> strings {*state.store};
        auto [held, begin_status] = begin_transaction(state, shape);
        if (begin_status.failed()) return begin_status;
        strings.join(*held);
        call_access<cpu_class_> const access {state, held.get(), schedule, scratch, head_stamp_k, queues, &strings};
        util::status_t const written = writes(access);
        // Only a refusal for room retries larger; a format's ceiling stands
        transaction_config_t const wanted = held->wanted_shape();
        bool const outgrew = wanted.max_bytes > shape.max_bytes || wanted.max_deltas > shape.max_deltas;
        if (written == util::status_code_t::capacity_exhausted_k && outgrew &&
            wanted.max_bytes <= storage_limits_t::transaction_max_bytes_k &&
            wanted.max_deltas <= storage_limits_t::transaction_max_deltas_k) {
            shape.max_bytes = fu::min_of(fu::max_of(shape.max_bytes * 2, wanted.max_bytes),
                                         storage_limits_t::transaction_max_bytes_k);
            shape.max_deltas = fu::min_of(fu::max_of(shape.max_deltas * 2, wanted.max_deltas),
                                          storage_limits_t::transaction_max_deltas_k);
            continue;
        }
        if (written.failed()) return written;
        util::status_t const committed = strings.commit(*held, queues);
        if (committed == util::status_code_t::write_conflict_k && schedule != watch_schedule_t::never_k) continue;
        return committed;
    }
    return {util::status_code_t::write_conflict_k, "Concurrent commits kept conflicting with the call"};
}

/** Writes @p writes into the transaction @p view holds, or commits them as a transaction of their
 *  own, flushed when @p durability asks, for every call that writes. */
template <cpu_class_t cpu_class_, typename writes_type_>
util::status_t serve_write(database_state<cpu_class_>& state, view_t const& view, ustore_durability_t durability,
                           std::size_t count, std::size_t bytes, watch_schedule_t schedule,
                           writes_type_&& writes) noexcept {
    if (view.transaction) {
        transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
        if (transaction.staged)
            return {util::status_code_t::invalid_argument_k, "A staged transaction takes no writes"};
        call_access<cpu_class_> access = access_of(state, view);
        if (schedule != watch_schedule_t::never_k) access.schedule = schedule;
        return writes(access);
    }
    if (util::status_t const committed = commit_one_collection(state, view.scratch, nvme_queues_of(view), count, bytes,
                                                               schedule, writes);
        committed.failed())
        return committed;
    return settle(state, durability, nvme_queues_of(view));
}

#pragma endregion Maintenance

#pragma region Collections

/** What @p collection of the store of @p state holds, which the store its tag names tells. */
template <cpu_class_t cpu_class_>
util::status_t collection_modality(database_state<cpu_class_>& state, collection_id_t const& collection,
                                   ustore_modality_t& modality) noexcept {
    std::uint16_t store_tag = 0;
    if (util::status_t const held = state.store->collection_store_tag(collection, store_tag); held.failed())
        return held;
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
util::status_t create_collection(database_state<cpu_class_>& state, nvme_queues_ref_t queues,
                                 collection_id_t const& collection, std::string_view name, ustore_modality_t modality,
                                 collection_config_t const& shape,
                                 collection_id_t flushed_with = collection_invalid_id_k) noexcept {
    std::uint16_t const store_tag = store_tag_for(modality, shape);
    if (store_tags_t::index_of(store_tag) == std::size(store_tags_k))
        return {util::status_code_t::unsupported_k, "No store holds that modality and layout"};
    return state.store->create_collection(collection, util::string_view_t {name}, store_tag, shape, flushed_with,
                                          queues);
}

/** Lays the record of a collection of @p store_tag shaped as @p shape and named @p name out
 *  into @p into. */
inline util::status_t lay_out_collections_record(std::uint16_t store_tag, collection_config_t const& shape,
                                                 std::string_view name, st::vector<util::byte_t>& into) noexcept {
    util::collections_record_t record;
    record.store_tag = store_tag;
    record.value_size = static_cast<std::uint32_t>(shape.lsm_tree.lsm_level_0.sorted_run.value_size);
    record.value_max_size = static_cast<std::uint32_t>(shape.lsm_tree.lsm_level_0.sorted_run.value_max_size);
    record.level_enlarge_factor = shape.lsm_tree.level_enlarge_factor;
    record.cache_limit = shape.cache_limit;
    record.memory_runs_max_bytes = shape.memory_runs_max_bytes;
    record.memory_runs_max_deltas = shape.memory_runs_max_deltas;
    if (into.resize(sizeof(record) + name.size()) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "No room for a collections record"};
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

/** The entry @p held, a listing of the collections system collection, holds. */
template <cpu_class_t cpu_class_>
util::expected<collections_entry_t> read_collections_entry(database_state<cpu_class_>&, ustore_collection_id_t const&,
                                                           util::value_view_t held) noexcept {
    std::string_view const bytes {held};
    if (bytes.size() < sizeof(util::collections_record_t))
        return util::status_t {util::status_code_t::malformed_k, "A collections record is shorter than its fields"};
    util::collections_record_t record;
    std::memcpy(&record, bytes.data(), sizeof(record));
    return collections_entry_t {modality_of(record.store_tag), bytes.substr(sizeof(record))};
}

/** What the collections system collection records of @p collection at @p stamp, refusing one it does not list, the
 *  reserved ones included. */
template <cpu_class_t cpu_class_>
util::expected<collections_entry_t> look_up(database_state<cpu_class_>& state, ustore_arena_t scratch,
                                            nvme_queues_ref_t queues, ustore_collection_id_t const& collection,
                                            transaction_stamp_t stamp = head_stamp_k) noexcept {
    call_access<cpu_class_> const listed {state, nullptr, watch_schedule_t::never_k, scratch, stamp, queues};
    auto [held, read_status] = read_value(listed, collections_collection_k, stored_of(collection.uuid));
    if (read_status.failed()) return read_status;
    if (!held) return util::status_t {util::status_code_t::not_found_k, "No such collection"};
    return read_collections_entry(state, collection, held);
}

/** Refuses a batch addressing a reserved collection, one the database does not list at the stamp
 *  of @p view's snapshot or at the head, or one @c admit_modality refuses for @p modality, looking
 *  each run of tasks on one collection up once in the entries the database holds in memory. */
template <cpu_class_t cpu_class_>
util::status_t resolve_runs(database_state<cpu_class_>& state, view_t const& view,
                            util::strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                            ustore_modality_t modality) noexcept {
    transaction_stamp_t const stamp = view.snapshot ? state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp()
                                                    : head_stamp_k;
    std::size_t const thread_index = view.thread_index;
    for (std::size_t index = 0; index != count; ++index) {
        ustore_collection_id_t const& collection = collections[index];
        if (index != 0 && collection == collections[index - 1]) continue;
        if (reserved(collection)) return {util::status_code_t::not_found_k, "No such collection"};
        std::uint16_t store_tag = 0;
        if (util::status_t const listed = state.store->listed_store_tag(collection, stamp, store_tag, thread_index);
            listed.failed())
            return listed;
        if (util::status_t const admitted = admit_modality(modality_of(store_tag), modality); admitted.failed())
            return admitted;
    }
    return {};
}

/** The resolver of this engine, which its docs and pages verbs run first. */
template <cpu_class_t cpu_class_>
util::status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                                ustore_modality_t modality, view_t const& view) noexcept {
    return resolve_runs(*state_of<database_state<cpu_class_>>(database), view, util::strided(collections), count,
                        modality);
}

#pragma endregion Collections

#pragma region Strings

/** One string a batch writes or removes, which the last task naming it wins. */
struct string_task_t {
    ustore_collection_id_t collection;
    std::string_view text;

    /** Whether the task writes @c value or removes the string. */
    util::delta_kind_t kind = util::delta_kind_t::remove_k;

    /** The 16 bytes of the key the string maps onto, nil for a removal. */
    ustore_uuid_t value {};
};

/** Calls @p visitor as <tt>visitor(store, reading)</tt> with the strings store of @p collection and
 *  its reading at the state @p access reads, inside the database's epochs, answering what it
 *  answers, or refuses a collection another store keeps. */
template <cpu_class_t cpu_class_, typename visitor_type_>
util::status_t visit_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                             visitor_type_&& visitor) noexcept {
    return with_reading(
        access, collection, access.queues.thread_index, [&](collection_reading_of<cpu_class_> const& read) noexcept {
            return access.state.store->visit_reading(
                read,
                [&]<typename store_type_>(store_type_& store, typename store_type_::reading_t const& reading) noexcept {
                    if constexpr (std::is_same_v<store_type_, strings_store_of<cpu_class_>>)
                        return util::status_t {visitor(store, reading)};
                    else return collection_holds_another_modality();
                });
        });
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
util::status_t stage_strings(call_access<cpu_class_> const& access, std::span<string_task_t const> tasks) noexcept {
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    for (string_task_t const& task : tasks) {
        util::delta_view_t const delta = task.kind == util::delta_kind_t::write_k
                                             ? util::delta_view_t {util::delta_kind_t::write_k,
                                                                   std::as_bytes(std::span {task.value.bytes})}
                                             : util::delta_view_t {util::delta_kind_t::remove_k, {}};
        if (util::status_t const written = access.strings->write(task.collection, task.text, delta, watch_stamp);
            written.failed())
            return written;
    }
    return {};
}

/**
 *  @brief The key @p text maps onto in @p collection as @p access sees it, and whether any
 *      write holds it.
 *
 *  A transaction's own write outranks the store, and its own removal hides the store's key. A read through a watching
 *  transaction watches the string, its absence included, so a commit landing on it in between conflicts.
 */
template <cpu_class_t cpu_class_>
util::status_t read_string(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view text,
                           ustore_uuid_t& value, bool& present) noexcept {
    std::span<util::byte_t, sizeof(ustore_uuid_t)> const bytes = std::as_writable_bytes(std::span {value.bytes});
    util::delta_view_t own_delta;
    if (access.strings && access.strings->uncommitted(collection, text, own_delta)) {
        present = own_delta.kind != util::delta_kind_t::remove_k;
        if (present) std::memcpy(bytes.data(), own_delta.bytes.data(), bytes.size());
        return {};
    }
    if (transaction_stamp_t const watch_stamp = watch_stamp_of(access); watch_stamp != invalid_transaction_stamp_k)
        if (util::status_t const watched = access.strings->watch(collection, text, watch_stamp); watched.failed())
            return watched;
    return visit_strings(access, collection, [&](auto const& store, auto const& reading) noexcept -> status_t {
        present = store.find(reading, text, bytes) == lookup_result_t::written_k;
        return {};
    });
}

/**
 *  @brief Hands @p visitor every string of @p collection starting with @p prefix and not below
 *      @p start, in order, as @p access reads them, each with what its newest version holds.
 *  @param[in] visitor Called as <tt>visitor(text, state, value)</tt> inside the epochs, answering
 *      whether the walk continues, with the key the string maps onto, so it writes nothing; the
 *      text lives until it returns.
 *
 *  Every page reads through one reading of the collection, so the walk sees one state with no pin.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
util::status_t walk_strings(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view prefix,
                            std::string_view start, visitor_type_&& visitor) noexcept {
    typename strings_store_of<cpu_class_>::page_t page {access.scratch};
    st::vector<char, util::arena_allocator<char>> from {{access.scratch}};
    std::string_view const first = fu::max_of(start, prefix);
    if (from.resize(first.size()) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
    if (!first.empty()) std::memcpy(from.data(), first.data(), first.size());
    return visit_strings(access, collection, [&](auto const& store, auto const& reading) noexcept -> status_t {
        for (;;) {
            if (util::status_t const read = store.read_page(reading, prefix,
                                                            std::string_view {from.begin(), from.end()}, page);
                read.failed())
                return read;
            for (std::size_t index = 0; index != page.size(); ++index)
                if (!visitor(page.text(index), page.listed[index].state, page.listed[index].value)) return {};
            if (page.size() != strings_page_k) return {};
            // The smallest string past the last one listed is that string and one zero byte more.
            std::string_view const last = page.text(page.size() - 1);
            if (from.resize(last.size() + 1) != st::status_t::success_k)
                return {util::status_code_t::out_of_memory_k, "A walk could not hold where it resumes"};
            if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
            from[last.size()] = '\0';
        }
    });
}

/** Hands @p visitor every string of @p collection starting with @p prefix and below @p below, or
 *  all of them for an empty @p below, largest first, as @c walk_strings does upwards, reading one
 *  page per run at a time. */
template <cpu_class_t cpu_class_, typename visitor_type_>
util::status_t walk_strings_down(call_access<cpu_class_> const& access, collection_id_t collection,
                                 std::string_view prefix, std::string_view below, visitor_type_&& visitor) noexcept {
    typename strings_store_of<cpu_class_>::page_t page {access.scratch};
    st::vector<char, util::arena_allocator<char>> until {{access.scratch}};
    if (until.resize(below.size()) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
    if (!below.empty()) std::memcpy(until.data(), below.data(), below.size());
    return visit_strings(access, collection, [&](auto const& store, auto const& reading) noexcept -> status_t {
        for (;;) {
            if (util::status_t const read = store.read_page_down(reading, prefix,
                                                                 std::string_view {until.begin(), until.end()}, page);
                read.failed())
                return read;
            for (std::size_t index = 0; index != page.size(); ++index)
                if (!visitor(page.text(index), page.listed[index].state, page.listed[index].value)) return {};
            if (page.size() != strings_page_k) return {};
            // No string sits below the empty one, and an empty bound would mean none at all.
            std::string_view const last = page.text(page.size() - 1);
            if (last.empty()) return {};
            if (until.resize(last.size()) != st::status_t::success_k)
                return {util::status_code_t::out_of_memory_k, "A walk could not hold where it resumes"};
            std::memcpy(until.data(), last.data(), last.size());
        }
    });
}

/**
 *  @brief Hands @p visitor every present string of @p window in @p collection, smallest first, or
 *      largest first in @p order descending, as @p access sees them: a transaction's own writes
 *      merged in, its removals hiding the store's strings.
 *  @param[in] visitor Called as @c visitor(text) answering whether the walk continues; the text
 *      lives until it returns.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
util::status_t walk_window_strings(call_access<cpu_class_> const& access, collection_id_t collection,
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
    st::vector<own_string_t, util::arena_allocator<own_string_t>> own_strings {{access.scratch}};
    util::status_t listed;
    if (access.strings)
        access.strings->for_each_uncommitted(collection, [&](std::string_view text, util::delta_view_t delta) noexcept {
            if (listed.failed() || text < lower || !text.starts_with(window.prefix) || window.past(text) ||
                window.skips(text))
                return;
            listed = util::growth_status(
                own_strings.push_back(own_string_t {text, delta.kind != util::delta_kind_t::remove_k}));
        });
    if (listed.failed()) return listed;
    std::sort(own_strings.begin(), own_strings.end(),
              [&](own_string_t const& first, own_string_t const& second) noexcept {
                  return before(first.text, second.text);
              });
    // The ranges the transaction removes hide the store's strings in them, bar its later writes.
    st::vector<range_tombstone<std::string_view>, util::arena_allocator<range_tombstone<std::string_view>>> own_ranges {
        {access.scratch}};
    if (access.strings)
        access.strings->for_each_removed_range(
            collection, [&](range_tombstone<std::string_view> const& range) noexcept {
                if (listed.succeeded())
                    listed = util::growth_status(own_ranges.push_back(range_tombstone<std::string_view> {range}));
            });
    if (listed.failed()) return listed;
    auto const removed_in_range = [&](std::string_view text) noexcept {
        for (range_tombstone<std::string_view> const& range : own_ranges)
            if (range.covers(text)) return true;
        return false;
    };

    std::size_t next = 0;
    bool keeps_going = true;
    auto const emit_own = [&](own_string_t const& written) noexcept {
        if (written.present) keeps_going = visitor(written.text);
        return keeps_going;
    };
    auto const merged = [&](std::string_view text, lookup_result_t state, ustore_key_t const&) noexcept {
        // A descending walk ends below the window's least string, which an ascending one starts at.
        if (descending ? text < lower : window.past(text)) return false;
        for (; next != own_strings.size() && before(own_strings[next].text, text); ++next)
            if (!emit_own(own_strings[next])) return false;
        if (next != own_strings.size() && own_strings[next].text == text) return emit_own(own_strings[next++]);
        if (state != lookup_result_t::written_k || window.skips(text) || removed_in_range(text)) return true;
        keeps_going = visitor(text);
        return keeps_going;
    };
    util::status_t walked;
    if (!descending) walked = walk_strings(access, collection, window.prefix, lower, merged);
    else {
        // The window's start is inclusive, so the walk starts below the least string past it.
        st::vector<char> below;
        if (!window.upper().empty()) {
            if (below.resize(window.upper().size() + 1) != st::status_t::success_k)
                return {util::status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
            std::memcpy(below.data(), window.upper().data(), window.upper().size());
            below[window.upper().size()] = '\0';
        }
        walked = walk_strings_down(access, collection, window.prefix, std::string_view {below.begin(), below.end()},
                                   merged);
    }
    if (walked.failed()) return walked;
    for (; keeps_going && next != own_strings.size(); ++next) emit_own(own_strings[next]);
    return {};
}

/** Whether a walk listed as many strings as its page holds, so more may follow what it listed. */
enum class page_fill_t : bool { short_k, full_k };

/** Watches what a walk of @p window covered through a watching transaction of @p access: from the
 *  window's start in its direction to @p smallest or @p largest listed for a full page, and the
 *  whole window otherwise, so a commit landing a string inside it conflicts. */
template <cpu_class_t cpu_class_>
util::status_t watch_window_strings(call_access<cpu_class_> const& access, collection_id_t collection,
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

/** Appends to @p outputs up to @p limit strings of @p window in @p collection, in its direction, as
 *  @p access sees them, and watches what the walk covered. */
template <cpu_class_t cpu_class_>
util::status_t scan_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                            strings_window_t const& window, std::size_t limit,
                            util::strings_scan_outputs_t& outputs) noexcept {
    if (limit == 0) return {};
    std::size_t const first = outputs.size();
    util::status_t pushed;
    scan_direction_t const order = window.direction == ustore_scan_descending_k ? scan_direction_t::descending_k
                                                                                : scan_direction_t::ascending_k;
    util::status_t const walked = walk_window_strings(
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
util::status_t remove_string_range(call_access<cpu_class_> const& access, collection_id_t collection,
                                   std::string_view start, std::string_view end) noexcept {
    if (!end.empty() && !(start < end)) return {};
    if (util::status_t const removed = access.strings->remove_range(collection, start, end); removed.failed())
        return removed;
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    if (watch_stamp == invalid_transaction_stamp_k) return {};
    if (end.empty()) return access.strings->watch_prefix(collection, {}, start, watch_stamp);
    return access.strings->watch_range(collection, start, end, watch_stamp);
}

/** Commits @p tasks as a transaction of their own, which watches nothing, so the last writer wins. */
template <cpu_class_t cpu_class_>
util::status_t write_strings_alone(database_state<cpu_class_>& state, ustore_arena_t scratch, nvme_queues_ref_t queues,
                                   std::span<string_task_t const> tasks) noexcept {
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + sizeof(task.value);
    return commit_one_collection(
        state, scratch, queues, tasks.size(), bytes, watch_schedule_t::never_k,
        [&](call_access<cpu_class_> const& access) noexcept { return stage_strings(access, tasks); });
}

#pragma endregion Strings

#pragma region Names

/** The collection @p name maps onto as @p access reads the names, and whether any collection holds
 *  the name. */
template <cpu_class_t cpu_class_>
util::status_t look_up_name(call_access<cpu_class_> const& access, std::string_view name,
                            ustore_collection_id_t& collection, bool& present) noexcept {
    return read_string(access, names_collection_k, name, collection.uuid, present);
}

/** Commits @p name mapped onto @p collection, or its removal when @p collection is null, as a write of its own. */
template <cpu_class_t cpu_class_>
util::status_t write_name(database_state<cpu_class_>& state, ustore_arena_t scratch, nvme_queues_ref_t queues,
                          std::string_view name, ustore_collection_id_t const* collection) noexcept {
    string_task_t const task = collection ? string_task_t {names_collection_k, name, util::delta_kind_t::write_k,
                                                           collection->uuid}
                                          : string_task_t {names_collection_k, name};
    return write_strings_alone(state, scratch, queues, std::span<string_task_t const> {&task, 1});
}

#pragma endregion Names

#pragma region Clearing

/** Empties the strings collection @p collection for @c ustore_drop_pairs_k, removing every string in windows committed
 *  one after another, so the transaction of each stays bounded, and refuses @c ustore_drop_values_k, as a string always
 *  maps onto a key. */
template <cpu_class_t cpu_class_>
util::status_t empty_strings(database_state<cpu_class_>& state, ustore_arena_t scratch, nvme_queues_ref_t queues,
                             collection_id_t collection, ustore_drop_mode_t mode) noexcept {
    if (mode == ustore_drop_values_k)
        return {util::status_code_t::invalid_argument_k, "A string's value never empties"};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, head_stamp_k, queues};
    st::vector<char> from;
    st::vector<char> texts;
    st::vector<std::size_t> ends;
    st::vector<string_task_t> tasks;
    for (;;) {
        texts.clear();
        ends.clear();
        tasks.clear();
        util::status_t room;
        util::status_t const walked = walk_strings(
            head, collection, {}, std::string_view {from.begin(), from.end()},
            [&](std::string_view text, lookup_result_t found, ustore_key_t const&) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = util::growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = util::growth_status(ends.push_back(offset + text.size()));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                return ends.size() != clear_window_keys_k;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (ends.empty()) return {};
        if (util::status_t const reserved = util::growth_status(tasks.reserve(ends.size())); reserved.failed())
            return reserved;
        for (std::size_t index = 0, begin = 0; index != ends.size(); begin = ends[index++]) [[maybe_unused]]
            st::status_t const placed = tasks.push_back(
                string_task_t {collection, std::string_view {texts.data() + begin, ends[index] - begin}});
        if (util::status_t const written = write_strings_alone(state, scratch, queues,
                                                               std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (ends.size() < clear_window_keys_k) return {};
        // The next window starts past the last string of this one, past the removals the walk would list again.
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "A clear could not hold where it resumes"};
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
util::status_t empty_collection(database_state<cpu_class_>& state, ustore_arena_t scratch, nvme_queues_ref_t queues,
                                collection_id_t collection, ustore_modality_t modality,
                                ustore_drop_mode_t mode) noexcept {
    if (entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k)
        return empty_strings(state, scratch, queues, collection, mode);
    if (modality == ustore_modality_graph_k && mode == ustore_drop_values_k) return {};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, head_stamp_k, queues};
    st::vector<stored_key_t, util::arena_allocator<stored_key_t>> keys {{scratch}};
    stored_key_t from = limit<stored_key_t>::min_k;
    util::value_view_t const emptied = mode == ustore_drop_values_k ? util::value_view_t::make_empty()
                                                                    : util::value_view_t {};
    for (;;) {
        keys.clear();
        if (util::status_t const collected = collect_keys(head, collection, from, limit<stored_key_t>::max_k,
                                                          clear_window_keys_k, keys);
            collected.failed())
            return collected;
        if (keys.empty()) return {};
        util::status_t const committed = commit_one_collection(
            state, scratch, queues, keys.size(), keys.size(), watch_schedule_t::never_k,
            [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                for (std::size_t index = 0; index != keys.size(); ++index)
                    if (util::status_t const written = write_value(access, collection, keys[index], emptied);
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
util::status_t list_collections(database_state<cpu_class_>& state, ustore_arena_t scratch, nvme_queues_ref_t queues,
                                listed_type_& listed, transaction_stamp_t stamp = head_stamp_k) noexcept {
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, stamp, queues};
    st::vector<stored_key_t, util::arena_allocator<stored_key_t>> keys {{scratch}};
    if (util::status_t const collected = collect_keys(head, collections_collection_k, limit<stored_key_t>::min_k,
                                                      limit<stored_key_t>::max_k,
                                                      (std::numeric_limits<std::size_t>::max)(), keys);
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
        if (util::status_t const pushed = util::growth_status(listed.push_back(listed_collection_t {id, entry}));
            pushed.failed())
            return pushed;
    }
    return {};
}

/**
 *  @brief Forgets @p collection in both system collections, then returns its runs to the volume,
 *      under the mark @p dropping holds, so no snapshot or transaction reads them.
 *
 *  The collections system collection changes under its own claim and straight into the store,
 *  outside the registry, since no caller's transaction reads it, so its change never shows as a
 *  commit a transaction's watches might collide with. The name goes first, so a crash in between
 *  leaves a listed collection without a name, never a dangling name, and a crash with the runs
 *  still kept leaves an unlisted collection, which the next open drops.
 */
template <cpu_class_t cpu_class_>
util::status_t drop_collection(database_state<cpu_class_>& state, dropping_guard<cpu_class_> const&,
                               ustore_arena_t scratch, nvme_queues_ref_t queues,
                               ustore_collection_id_t const& collection) noexcept {
    collections_guard<cpu_class_> const held {state};
    {
        call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, head_stamp_k, queues};
        stored_key_t const listed = stored_of(collection.uuid);
        auto [listing, read_status] = read_value(head, collections_collection_k, listed);
        if (read_status.failed()) return read_status;
        if (!listing) return {util::status_code_t::not_found_k, "No collection has such identifier"};
        std::string_view const record {listing};
        std::string_view const name = record.size() > sizeof(util::collections_record_t)
                                          ? record.substr(sizeof(util::collections_record_t))
                                          : std::string_view {};
        if (!name.empty())
            if (util::status_t const unnamed = write_name(state, scratch, queues, name, nullptr); unnamed.failed())
                return unnamed;
        if (util::status_t const unlisted = write_value(head, collections_collection_k, listed, {}); unlisted.failed())
            return unlisted;
        if (util::status_t const unlisted = state.store->unlist_collection(collection); unlisted.failed())
            return unlisted;
    }
    // The unlisting reaches a run in the same manifest that stops naming the collection
    return state.store->drop_collection(collection, collections_collection_k, queues);
}

/** Counts one reader of @p state closed. */
template <cpu_class_t cpu_class_>
void close_reader(database_state<cpu_class_>& state) noexcept {
    [[maybe_unused]] bool const last = count_reader_closed<cpu_class_>(state.open_readers);
}

#pragma endregion Clearing

#pragma region Database Calls

/** Implements @c ustore_database_describe with the store's estimates, which count every version of a key that the
 *  runs still hold. */
template <cpu_class_t cpu_class_>
util::status_t database_describe(ustore_database_describe_t const& arguments, view_t const& view) noexcept {
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    util::nvme_queues_t const* const queues = nvme_queues_of(view.executor);
    return util::assign_describe(
        arguments,
        {
            .capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k |
                                                  ustore_capabilities_saves_snapshots_k),
            .engine = ustore_engine_nvme_k,
            .isolation = ustore_isolation_snapshot_k,
            .cpu_class = static_cast<ustore_cpu_class_t>(cpu_class_),
            .keys = static_cast<ustore_size_t>(state.store->population_count()),
            .memory_bytes = static_cast<ustore_size_t>(state.resources->memory_constraint.used()),
            .disk_bytes = static_cast<ustore_size_t>(state.store->size_bytes()),
            .unpinned_staging_bytes = static_cast<ustore_size_t>(queues ? queues->unpinned_staging_bytes() : 0),
            .requests_across_registered_slots = static_cast<ustore_size_t>(
                queues ? queues->requests_across_registered_slots() : 0),
        });
}

/** Implements @c ustore_database_clear: removing every collection, or emptying every collection's pairs or values. */
template <cpu_class_t cpu_class_>
util::status_t database_clear(ustore_database_clear_t const& arguments, view_t const& view) noexcept {
    ustore_drop_mode_t const mode = arguments.mode;
    if (util::status_t const status = util::validate_drop_mode(mode); status.failed()) return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    st::vector<listed_collection_t, util::arena_allocator<listed_collection_t>> listed {{view.scratch}};
    nvme_queues_ref_t const queues = nvme_queues_of(view);
    // No reader opens while the mark is held, so none can pin a stamp that still lists
    // a dropped collection
    dropping_guard<cpu_class_> const dropping = mode == ustore_drop_collection_k ? dropping_guard<cpu_class_> {state}
                                                                                 : dropping_guard<cpu_class_> {};
    if (mode == ustore_drop_collection_k && !dropping)
        return {util::status_code_t::contended_k, "A reader holds the collections' runs"};
    if (util::status_t const collected = list_collections(state, view.scratch, queues, listed); collected.failed())
        return collected;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        // A string always maps onto a key, so emptying every value leaves the strings collections as they are
        if (mode == ustore_drop_values_k &&
            entry_layout_of(listed[index].entry.modality) == entry_layout_t::strings_to_uuids_k)
            continue;
        util::status_t const cleared = mode == ustore_drop_collection_k
                                           ? drop_collection(state, dropping, view.scratch, queues, listed[index].id)
                                           : empty_collection(state, view.scratch, queues, listed[index].id,
                                                              listed[index].entry.modality, mode);
        if (cleared.failed()) return cleared;
    }
    if (util::status_t const flushed = state.store->flush(queues); flushed.failed()) return flushed;
    return compact_due(state, view);
}

/**
 *  @brief Fans the slices of one merge out over the threads of the executor a call holds, one
 *      slice per thread.
 *
 *  The call holds its executor, eager or delegated, so this dispatch is the pool's only one, and
 *  each slice reads and writes on the two rings of its own thread of the executor's queue set.
 */
template <cpu_class_t cpu_class_>
class compaction_fan_out {
  public:
    using disk_t = typename storage_mount_of<cpu_class_>::disk_t;

    compaction_fan_out(util::pool_view_t pool, util::nvme_queues_t& queues, disk_t& disk, view_t const& caller) noexcept
        : pool_(pool), queues_(queues), disk_(disk), scratch_(caller.scratch) {}

    /** One slice per thread of the pool. */
    [[nodiscard]] std::size_t slices() const noexcept { return pool_.threads_count(); }

    /** Runs @p slice once per index on the pool's threads, answering the first failure. */
    util::status_t run(compaction_fan_out_t::slice_call_t const& slice) noexcept {
        st::vector<util::status_t, util::arena_allocator<util::status_t>> statuses {{scratch_}};
        if (statuses.resize(slices()) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "A merge could not track its slices"};
        auto run_slice = [&](std::size_t task, std::size_t thread) noexcept {
            auto [channels, media_status] = queues_.both_channels_with_disk_media(thread, disk_);
            if (media_status.failed()) {
                statuses[task] = media_status;
                return;
            }
            statuses[task] = slice(task, *channels.reading, *channels.writing);
            [[maybe_unused]] util::status_t const read_drained = disk_.drain(channels.reading->queue_pair());
            [[maybe_unused]] util::status_t const write_drained = disk_.drain(channels.writing->queue_pair());
        };
        if (st::failed(pool_.for_n_dynamic(statuses.size(), run_slice)))
            return {util::status_code_t::internal_k, "The executor's idle pool refused a fork"};
        for (util::status_t const& status : statuses)
            if (status.failed()) return status;
        return {};
    }

  private:
    /** The executor's pool, idle but for this call's forks. */
    util::pool_view_t pool_;

    /** The queue set of the executor the call holds, whose thread @c t rings serve slice @c t. */
    util::nvme_queues_t& queues_;

    /** The disk every slice reads and writes. */
    disk_t& disk_;

    /** The call's scratch, which holds every slice's status. */
    ustore_arena_t scratch_;
};

/** The fan-out a merge takes inside a call on @p view: one slice per thread of the executor's
 *  queue set, or the calling thread alone for a call without an executor or whose executor holds
 *  no queue set. */
template <cpu_class_t cpu_class_>
class queues_fan_out {
  public:
    queues_fan_out(database_state<cpu_class_>& state, view_t const& view) noexcept {
        if (!view.executor) return;
        util::nvme_queues_t* const queues = nvme_queues_of(view).queues;
        if (!queues) return;
        auto fan_out_on = [&]<typename mount_type_>(mount_type_& mount) noexcept {
            if constexpr (std::is_same_v<mount_type_, storage_mount_of<cpu_class_>>)
                fan_out_ = compaction_fan_out_t {
                    threads_.emplace(view.executor->pool.view(), *queues, mount.disk(), view)};
            return util::status_t {};
        };
        [[maybe_unused]] util::status_t const visited = state.resources->storage.visit(fan_out_on);
    }

    queues_fan_out(queues_fan_out const&) = delete;
    queues_fan_out& operator=(queues_fan_out const&) = delete;

    /** The fan-out the store's merge takes. */
    [[nodiscard]] compaction_fan_out_t const& fan_out() const noexcept { return fan_out_; }

  private:
    /** The slices over the queue set's threads, empty for a call without one. */
    std::optional<compaction_fan_out<cpu_class_>> threads_;

    /** What the merge calls, the calling thread alone until @c threads_ is set. */
    compaction_fan_out_t fan_out_;
};

/** Merges every collection due for a compaction, a bounded number of passes, fanned out on the
 *  queue set of the call on @p view, whatever queue set that is. */
template <cpu_class_t cpu_class_>
util::status_t compact_due(database_state<cpu_class_>& state, view_t const& view) noexcept {
    for (std::size_t pass = 0; pass != compaction_passes_k && state.store->needs_compaction(); ++pass) {
        queues_fan_out<cpu_class_> const fanned {state, view};
        if (util::status_t const compacted = state.store->compact(fanned.fan_out(), nvme_queues_of(view));
            compacted.failed())
            return compacted;
    }
    return {};
}

/**
 *  @brief One compaction of an NVMe database in flight: a flush writing every collection's memory
 *      runs, then up to @c compaction_passes_k merge passes, each merging the levels due over their
 *      bounds, one task each, as the store flushes and merges every collection under its one claim.
 *
 *  A merge task fans its own merge out over the threads of the executor that opened the database
 *  whenever the call holds it, so the driver runs it on the call's thread. A flush or a merge run
 *  again writes or merges only what is left, and each weighs one. The plan keeps no list, so
 *  neither @ref run, @ref advance nor @ref finish allocates.
 */
template <cpu_class_t cpu_class_>
struct nvme_variant<cpu_class_>::compact_plan_t {

    /** A phase's one task, read from the phase alone. */
    struct task_t {};

    /** The database the plan compacts, which outlives the plan. */
    database_state<cpu_class_>* database = nullptr;

    compact_plan_t() noexcept = default;
    compact_plan_t(compact_plan_t const&) = delete;
    compact_plan_t& operator=(compact_plan_t const&) = delete;

    /** Task @p position of the current phase, an empty one past the end. */
    task_t operator[](std::size_t) const noexcept { return {}; }

    /** The current phase's tasks, positionally; none once the plan is over. */
    util::indexed_range<compact_plan_t> tasks() const noexcept { return {this, phase_ == phase_t::done_k ? 0u : 1u}; }

    /** Runs the current phase's task on the calling thread, which @p context names. */
    util::status_t run(task_t const&, view_t const& context) noexcept {
        if (phase_ == phase_t::flush_k) return database->store->flush(nvme_queues_of(context));
        if (phase_ != phase_t::merge_k || !database->store->needs_compaction()) return {};
        queues_fan_out<cpu_class_> const fanned {*database, context};
        return database->store->compact(fanned.fan_out(), nvme_queues_of(context));
    }

    /** The barrier between phases and passes, one caller after the task ran: picks the next pass or
     *  phase, and answers whether one follows. */
    bool advance() noexcept {
        if (phase_ == phase_t::flush_k) phase_ = phase_t::merge_k;
        else if (phase_ == phase_t::merge_k && ++merged_passes_ == compaction_passes_k) phase_ = phase_t::done_k;
        return phase_ != phase_t::done_k;
    }

    /** Skips what is left of the plan, keeping what its phases compacted. */
    void stop() noexcept { phase_ = phase_t::done_k; }

    /** Success, as no barrier fails; a failed task answers its own call. */
    util::status_t status() const noexcept { return {}; }

    phase_t phase() const noexcept { return phase_; }

    /** The position of the phase in hand, the flush first and each merge pass after, and the count
     *  of phases run once done. */
    std::size_t ordinal() const noexcept { return phase_ == phase_t::flush_k ? 0 : 1 + merged_passes_; }

    /** Units of the phase in hand done at its barrier: all, as each phase runs to its end. */
    std::size_t completed() const noexcept { return total(); }

    /** Units of the phase in hand: one for the flush or a merge pass. */
    std::size_t total() const noexcept { return tasks().size(); }

    /** Stops the plan where it stands, keeping what its phases compacted. */
    util::status_t finish() noexcept {
        stop();
        return status();
    }

  private:
    phase_t phase_ = phase_t::flush_k;

    /** Merge passes whose barrier the plan crossed. */
    std::size_t merged_passes_ = 0;
};

/** Opens the plan of @c ustore_database_compact, which keeps nothing but the database. */
template <cpu_class_t cpu_class_>
util::expected<void*> compact_plan_open(ustore_database_compact_t const& arguments, view_t const&) noexcept {
    using compact_plan_t = typename nvme_variant<cpu_class_>::compact_plan_t;
    compact_plan_t* const plan = new (std::nothrow) compact_plan_t {};
    if (!plan) return util::status_t {util::status_code_t::out_of_memory_k, "No room for the plan"};
    plan->database = &database_state_of<cpu_class_>(arguments);
    return static_cast<void*>(plan);
}

/** Implements @c ustore_database_flush, writing every collection's memory runs and publishing the
 *  manifest naming them. */
template <cpu_class_t cpu_class_>
util::status_t database_flush(ustore_database_flush_t const& arguments, view_t const& view) noexcept {
    return database_state_of<cpu_class_>(arguments).store->flush(nvme_queues_of(view));
}

#pragma endregion Database Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the collections system collection at the snapshot it names or at
 *  the head. */
template <cpu_class_t cpu_class_>
util::status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) noexcept {
    if (!arguments.written)
        return {util::status_code_t::invalid_argument_k, "Need an output for the collections written"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    st::vector<listed_collection_t, util::arena_allocator<listed_collection_t>> listed {{view.scratch}};
    transaction_stamp_t const stamp = view.snapshot ? state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp()
                                                    : head_stamp_k;
    if (util::status_t const collected = list_collections(state, view.scratch, nvme_queues_of(view), listed, stamp);
        collected.failed())
        return collected;
    util::collection_list_outputs_t outputs {view.scratch};
    for (std::size_t index = 0; index != listed.size(); ++index) {
        collections_entry_t const& entry = listed[index].entry;
        util::value_view_t const name = entry.name.empty() ? util::value_view_t {} : util::value_view_t {entry.name};
        if (util::status_t const pushed = outputs.push(listed[index].id, entry.modality, name); pushed.failed())
            return pushed;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_collection_create: the collections record and the name first, then the
 *  collection, whose manifest also names the run the record lands in, unlisting it again when the
 *  store refuses the collection. */
template <cpu_class_t cpu_class_>
util::status_t collection_create(ustore_collection_create_t const& arguments, view_t const& view) noexcept {
    auto [name, name_status] = util::validate_collection_create(arguments);
    if (name_status.failed()) return name_status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [created, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    // The engine's own name of a collection is its UUID's text, which never leaves the engine
    char text[util::uuid128_t::printed_length_k] = {};
    util::uuid128_t {std::span<std::uint8_t const, util::uuid128_t::size_bytes_k> {created.uuid.bytes}}.print(text);
    collection_config_t const shape = entry_layout_of(arguments.modality) == entry_layout_t::strings_to_uuids_k
                                          ? strings_shape_of(state.collection_shape)
                                          : state.collection_shape;
    std::uint16_t const store_tag = store_tag_for(arguments.modality, shape);
    st::vector<util::byte_t> record;
    if (util::status_t const laid = lay_out_collections_record(store_tag, shape, name, record); laid.failed())
        return laid;

    // The collections system collection changes under its own claim, outside the registry, and
    // lists the collection before the names system collection names it; the create's manifest then
    // names the collection and the run its listing lands in at once, so the two never disagree
    // after a crash
    nvme_queues_ref_t const queues = nvme_queues_of(view);
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, view.scratch, head_stamp_k, queues};
    util::status_t listed;
    bool taken = false;
    {
        collections_guard<cpu_class_> const held {state};
        ustore_collection_id_t holder;
        if (!name.empty()) listed = look_up_name(head, name, holder, taken);
        if (listed.succeeded() && taken)
            listed = {util::status_code_t::already_exists_k, "A collection already holds the name"};
        if (listed.succeeded())
            listed = write_value(head, collections_collection_k, stored_of(created.uuid),
                                 util::value_view_t {std::span<util::byte_t const> {record}});
        if (listed.succeeded() && !name.empty()) {
            listed = write_name(state, view.scratch, queues, name, &created);
            if (listed.failed()) [[maybe_unused]]
                util::status_t const unlisted = write_value(head, collections_collection_k, stored_of(created.uuid),
                                                            {});
        }
    }
    if (listed.failed()) return listed;
    util::status_t const made = create_collection(state, queues, created, std::string_view {text, sizeof(text)},
                                                  arguments.modality, shape, collections_collection_k);
    if (made.failed()) {
        collections_guard<cpu_class_> const held {state};
        if (!name.empty()) [[maybe_unused]]
            util::status_t const unnamed = write_name(state, view.scratch, queues, name, nullptr);
        [[maybe_unused]] util::status_t const unlisted = write_value(head, collections_collection_k,
                                                                     stored_of(created.uuid), {});
        return made;
    }
    if (!name.empty())
        if (util::status_t const flushed = state.store->flush(queues); flushed.failed()) return flushed;
    *arguments.collection = created;
    return {};
}

/** Implements @c ustore_collection_drop: forgetting the collection and its runs, or emptying its pairs or values. */
template <cpu_class_t cpu_class_>
util::status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const& view) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (util::status_t const status = util::validate_drop_mode(mode); status.failed()) return status;
    if (reserved(collection)) return {util::status_code_t::not_found_k, "No collection has such identifier"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    nvme_queues_ref_t const queues = nvme_queues_of(view);
    if (mode == ustore_drop_collection_k) {
        // No reader opens while the mark is held, so none can pin a stamp that still
        // lists the collection
        dropping_guard<cpu_class_> const dropping {state};
        if (!dropping) return {util::status_code_t::contended_k, "A reader holds the collection's runs"};
        if (util::status_t const dropped = drop_collection(state, dropping, view.scratch, queues, collection);
            dropped.failed())
            return dropped;
        return state.store->flush(queues);
    }
    auto [entry, look_up_status] = look_up(state, view.scratch, queues, collection);
    if (look_up_status == util::status_code_t::not_found_k)
        return {util::status_code_t::not_found_k, "No collection has such identifier"};
    if (look_up_status.failed()) return look_up_status;
    if (util::status_t const emptied = empty_collection(state, view.scratch, queues, collection, entry.modality, mode);
        emptied.failed())
        return emptied;
    return compact_due(state, view);
}

/** Implements @c ustore_collection_find, reading the names at the snapshot it names or at the head. */
template <cpu_class_t cpu_class_>
util::status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names,
                               view_t const& view) noexcept {
    if (!arguments.count) return {};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = validate_collection_find(arguments, names);
    if (outputs_status.failed()) return outputs_status;
    // Through a snapshot the names read as they stood at its stamp, and through anything else at the head
    call_access<cpu_class_> named {state, nullptr, watch_schedule_t::never_k, view.scratch};
    if (view.snapshot) {
        named.stamp = state_of<snapshot_state<cpu_class_>>(view.snapshot)->pin.stamp();
        named.queues = nvme_queues_of(view);
    }
    for (std::size_t index = 0; index != arguments.count; ++index) {
        bool present = false;
        ustore_collection_id_t found;
        if (util::status_t const looked = look_up_name(named, util::chars_of(names[index]), found, present);
            looked.failed())
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
util::status_t transaction_init(ustore_transaction_init_t const& arguments, view_t const& view) noexcept {
    if (util::status_t const status = util::validate_transaction_begin(arguments.transaction); status.failed())
        return status;
    if (view.transaction) {
        transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
        transaction.held->rollback();
        transaction.strings.clear();
        transaction.staged = false;
        return {};
    }
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    // Counted before its stamp is pinned, so no drop runs while it reads
    if (!open_reader(state)) return {util::status_code_t::contended_k, "A collection drop is in progress"};
    auto [opened, begin_status] = begin_transaction(state, state.transaction_shape);
    if (begin_status.failed()) {
        close_reader(state);
        return begin_status;
    }
    auto [made, status] = make_handle<ustore_transaction_handle_t, transaction_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k, state, std::move(opened));
    if (status.failed()) {
        close_reader(state);
        return status;
    }
    *arguments.transaction = made.release();
    return {};
}

/** Implements @c ustore_transaction_stage: checking every watch and that every write fits, then handing the
 *  ledger's leader word back, so the commit checks them again. */
template <cpu_class_t cpu_class_>
util::status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const& view) noexcept {
    auto [state, status] = transaction_state_of<transaction_state<cpu_class_>>(view.transaction);
    if (status.failed()) return status;
    transaction_state<cpu_class_>& transaction = *state;
    if (!transaction.staged) {
        if (util::status_t const staged = transaction.strings.stage(*transaction.held, nvme_queues_of(view));
            staged.failed())
            return staged;
        transaction.held->unstage();
        transaction.staged = true;
    }
    if (arguments.read_stamp) *arguments.read_stamp = transaction.held->start_stamp();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first, flushing when asked and compacting what it left due. */
template <cpu_class_t cpu_class_>
util::status_t transaction_commit(ustore_transaction_commit_t const& arguments, view_t const& view) noexcept {
    if (util::status_t const status = util::validate_transaction_commit(view.transaction, arguments.durability);
        status.failed())
        return status;
    transaction_state<cpu_class_>& transaction = *state_of<transaction_state<cpu_class_>>(view.transaction);
    if (util::status_t const committed = transaction.strings.commit(*transaction.held, nvme_queues_of(view));
        committed.failed())
        return committed;
    transaction.staged = false;
    if (arguments.commit_stamp) *arguments.commit_stamp = transaction.held->last_commit_stamp();
    return settle(transaction.state, arguments.durability, nvme_queues_of(view));
}

/** Implements @c ustore_transaction_rollback, withdrawing a stage and keeping the writes, which a transaction never
 *  staged has none of to withdraw. */
template <cpu_class_t cpu_class_>
util::status_t transaction_rollback(ustore_transaction_rollback_t const&, view_t const& view) noexcept {
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
inline util::status_t read_directories(ustore_database_open_nvme_t const& arguments,
                                       util::resources_config_t& resources) noexcept {
    if (sz::failed(resources.manifest.assign(util::string_view_t {util::chars_of(arguments.directory)})))
        return {util::status_code_t::out_of_memory_k, "No room for the manifest path"};
    if (arguments.data_directories_count && !arguments.data_directories)
        return {util::status_code_t::invalid_argument_k, "Counted data directories are null"};
    for (std::size_t index = 0; index != arguments.data_directories_count; ++index) {
        ustore_data_directory_t const& given = arguments.data_directories[index];
        if (!util::names_path(given.path))
            return {util::status_code_t::invalid_argument_k, "A data directory needs a path"};
        util::data_directory_config_t directory;
        if (sz::failed(directory.path.assign(util::string_view_t {util::chars_of(given.path)})))
            return {util::status_code_t::out_of_memory_k, "No room for a data directory"};
        directory.max_size = given.max_bytes;
        if (st::failed(resources.data_directories.push_back(std::move(directory))))
            return {util::status_code_t::out_of_memory_k, "No room to list the data directories"};
    }
    return {};
}

/** The directory the manifest of @p resources sits in, into @p into, or an empty path for
 *  a raw address. */
inline util::status_t manifest_directory_of(util::resources_config_t const& resources, std::span<char> into) noexcept {
    auto [addresses, addresses_status] = util::addresses_of(resources);
    if (addresses_status.failed()) return addresses_status;
    std::string_view const location = addresses.data_directories[0].location;
    bool const raw = addresses.data_directories[0].kind != util::data_address_kind_t::directory_k;
    if (raw || location.size() >= into.size()) {
        into[0] = '\0';
        return raw ? util::status_t {}
                   : util::status_t {util::status_code_t::invalid_argument_k, "A manifest path outgrew the limit"};
    }
    std::memcpy(into.data(), location.data(), location.size());
    into[location.size()] = '\0';
    return {};
}

/** Whether @p resources name the namespaces of an SPDK controller, whose queues no call builds for
 *  itself. */
inline bool opens_on_spdk(util::resources_config_t const& resources) noexcept {
    auto [addresses, addresses_status] = util::addresses_of(resources);
    return addresses_status.succeeded() && addresses.data_directories[0].kind == util::data_address_kind_t::spdk_k;
}

/** Reads the placement of the executor of @p arguments and their own sizes, keeping the engine's
 *  default for every zero among them. */
inline util::status_t read_options(ustore_database_open_nvme_t const& arguments, util::resources_config_t& resources,
                                   database_config_t& transactions, user_collection_config_t& collections) noexcept {
    // A database moves bytes as the queues of the executor that opens it
    if (util::nvme_queues_t const* const queues = nvme_queues_of(arguments.executor)) {
        resources.io_transport = queues->transport();
        resources.channels = queues->config();
    }

    if (arguments.collection_value_size) collections.value_max_size = 0;
    bool const fits = take_unless_zero(resources.memory_limit, arguments.memory_limit) &&
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
    if (!fits) return {util::status_code_t::invalid_argument_k, "An option of the engine exceeds its range"};
    auto ignore_issue = [](issue_kind_t, std::string_view) noexcept {};
    if (!validate(resources, transactions, ignore_issue) || !validate(resources, collections, ignore_issue))
        return {util::status_code_t::invalid_argument_k, "The configuration of the engine is invalid"};
    return {};
}

/** Creates the collection @p collection named @p name, holding @p modality, shaped as @p shape, unless the store holds
 *  it already. */
template <cpu_class_t cpu_class_>
util::status_t reserve_collection(database_state<cpu_class_>& state, nvme_queues_ref_t queues,
                                  collection_id_t const& collection, std::string_view name, ustore_modality_t modality,
                                  collection_config_t const& shape) noexcept {
    if (state.store->has_collection(collection)) return {};
    return create_collection(state, queues, collection, name, modality, shape);
}

/** Drops every collection of the store the collections system collection does not list, and
 *  unlists every listed one the store does not hold, which a crash between a create's or a drop's
 *  two steps leaves behind. */
template <cpu_class_t cpu_class_>
util::status_t drop_unlisted(database_state<cpu_class_>& state, ustore_arena_t scratch,
                             nvme_queues_ref_t queues) noexcept {
    st::vector<collection_ids_t> const held = state.store->list_collections();
    util::status_t first;
    for (std::size_t index = 0; index != held.size() && first.succeeded(); ++index) {
        ustore_collection_id_t const id = held[index].id;
        if (reserved(id)) continue;
        auto [entry, look_up_status] = look_up(state, scratch, queues, id);
        if (look_up_status == util::status_code_t::not_found_k)
            first = state.store->drop_collection(id, collection_invalid_id_k, queues);
        else if (look_up_status.failed()) first = look_up_status;
    }
    if (first.failed()) return first;

    st::vector<listed_collection_t, util::arena_allocator<listed_collection_t>> listed {{scratch}};
    if (util::status_t const collected = list_collections(state, scratch, queues, listed); collected.failed())
        return collected;
    collections_guard<cpu_class_> const guarded {state};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, head_stamp_k, queues};
    for (listed_collection_t const& collection : listed) {
        if (state.store->has_collection(collection.id)) continue;
        if (!collection.entry.name.empty())
            if (util::status_t const unnamed = write_name(state, scratch, queues, collection.entry.name, nullptr);
                unnamed.failed())
                return unnamed;
        if (util::status_t const unlisted = write_value(head, collections_collection_k, stored_of(collection.id.uuid),
                                                        {});
            unlisted.failed())
            return unlisted;
    }
    return {};
}

/** The compactor every merge of @p state runs on: the device compaction @p state owns over an executor of its own on
 *  the device of @p compactions, and the host for null. */
template <cpu_class_t cpu_class_>
util::expected<compactor_ref_t> compactor_of(database_state<cpu_class_>& state,
                                             util::resources_config_t const& resources,
                                             ustore_executor_handle_t const* compactions) noexcept {
    if (!compactions) return compactor_ref_t {};
    typename device_compaction<cpu_class_>::volume_t const* volume = nullptr;
    [[maybe_unused]] util::status_t const visited = state.resources->storage.visit(
        [&]<typename mount_type_>(mount_type_& mount) noexcept -> status_t {
            if constexpr (std::is_same_v<mount_type_, storage_mount_of<cpu_class_>>) volume = &mount.volume();
            return {};
        });
    if (!volume)
        return util::status_t {util::status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    device_compaction<cpu_class_>& compaction = state.compaction.emplace();
    if (util::status_t const opened = compaction.open(*compactions, resources, *volume); opened.failed()) return opened;
    return compaction.compactor();
}

/** Mounts the store of @p state over the storage its resources opened, which must be the one on files, merging on
 *  @p compactor. */
template <cpu_class_t cpu_class_>
util::status_t mount_store(database_state<cpu_class_>& state, database_config_t const& transactions,
                           compactor_ref_t compactor, nvme_queues_ref_t queues) noexcept {
    return state.resources->storage.visit([&]<typename mount_type_>(mount_type_& mount) noexcept -> status_t {
        if constexpr (std::is_same_v<mount_type_, storage_mount_of<cpu_class_>>)
            return state.store->mount(mount.volume(), queues, transactions, compactor,
                                      util::strings_kit_sha256<strings_kit_of<cpu_class_>> {});
        else return {util::status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    });
}

/** Opens the store @p resources place, every collection of it shaped as @p shape and merged
 *  on @p compactions, reserving both system collections on the first open, or for a read-only
 *  store refusing a database that lacks them. */
template <cpu_class_t cpu_class_>
util::status_t open_state(database_state<cpu_class_>& state, util::resources_config_t const& resources,
                          database_config_t const& transactions, collection_config_t const& shape,
                          ustore_arena_t scratch, nvme_queues_ref_t queues,
                          ustore_executor_handle_t const* compactions) noexcept {
    state.collection_shape = shape;
    state.transaction_shape = transactions.default_transaction;
    auto [opened, open_status] =
        util::open_resources<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>(resources,
                                                                                                         queues);
    if (open_status.failed()) return open_status;
    state.resources = std::move(opened);
    state.store.reset(new (std::nothrow) basic_database_of<cpu_class_> {state.resources});
    if (!state.store) return util::status_t {util::status_code_t::out_of_memory_k, "No room for the store"};
    auto [compactor, compactor_status] = compactor_of(state, resources, compactions);
    if (compactor_status.failed()) return compactor_status;
    if (util::status_t const mounted = mount_store(state, transactions, compactor, queues); mounted.failed())
        return mounted;
    if (transactions.access == util::medium_access_t::read_only_k) {
        if (!state.store->has_collection(collections_collection_k) || !state.store->has_collection(names_collection_k))
            return {util::status_code_t::malformed_k, "The database holds no system collections"};
        return {};
    }

    // Both system collections take the records a recovery rebuilds them with, whatever bounds
    // the open names
    collection_config_t records = config_of(
        util::default_reserved_record(util::collections_collection_uuid_k, util::engine_value_max_size_k));
    collection_config_t names = config_of(util::default_reserved_record(util::names_collection_uuid_k, 0));
    records.concurrency_limit = names.concurrency_limit = shape.concurrency_limit;
    if (util::status_t const reserved = reserve_collection(state, queues, collections_collection_k, "_collections",
                                                           ustore_modality_blobs_k, records);
        reserved.failed())
        return reserved;
    if (util::status_t const reserved = reserve_collection(state, queues, names_collection_k, "_names",
                                                           ustore_modality_strings_k, names);
        reserved.failed())
        return reserved;
    return drop_unlisted(state, scratch, queues);
}

/** Implements @c ustore_database_open_nvme, reserving both system collections on the first open. */
template <cpu_class_t cpu_class_>
util::status_t nvme_variant<cpu_class_>::open(ustore_database_open_nvme_t const& arguments, access_t access,
                                              view_t const& view) noexcept {
    if (!arguments.database) return {util::status_code_t::invalid_argument_k, "Need an output for the database"};
    if (!util::names_path(arguments.directory))
        return {util::status_code_t::invalid_argument_k, "A database needs a manifest"};

    util::resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = {engine_transaction_bytes_k, engine_transaction_deltas_k};
    user_collection_config_t collections = engine_collection_defaults();
    if (util::status_t const read = read_directories(arguments, resources); read.failed()) return read;
    char directory[util::path_max_bytes_k];
    if (util::status_t const located = manifest_directory_of(resources, directory); located.failed()) return located;
    if (!nvme_queues_of(arguments.executor) && opens_on_spdk(resources))
        return {util::status_code_t::invalid_argument_k, "An SPDK database opens on an NVMe executor"};
    if (directory[0] != '\0') {
        auto [exists, exists_status] = util::directory_exists(directory);
        if (exists_status.failed()) return exists_status;
        if (!exists) return {util::status_code_t::not_found_k, "The manifest's directory doesn't exist"};
    }
    if (util::status_t const read = read_options(arguments, resources, transactions, collections); read.failed())
        return read;
    if (access == access_t::read_only_k) resources.access = transactions.access = util::medium_access_t::read_only_k;
    auto [made, status] = make_handle<ustore_database_handle_t, database_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k);
    if (status.failed()) return status;
    if (util::status_t const opened = open_state<cpu_class_>(*made.state, resources, transactions,
                                                             create_collection_config(collections), view.scratch,
                                                             nvme_queues_of(view), arguments.compactions);
        opened.failed())
        return opened;
    made.handle->access = access;
    made.handle->admitted_executors = made.state->resources->storage.report().transport ==
                                              util::io_transport_t::linux_spdk_k
                                          ? executor_admission_t::nvme_executor_k
                                          : executor_admission_t::no_ram_executor_k;
    *arguments.database = made.release();
    return {};
}

/** Frees @p transaction, retiring it from the registry. */
template <cpu_class_t cpu_class_>
void transaction_free(ustore_transaction_t transaction) noexcept {
    auto* const freed = state_of<transaction_state<cpu_class_>>(transaction);
    database_state<cpu_class_>& state = freed->state;
    delete freed;
    close_reader(state);
}

/** Frees @p database without any I/O, which every transaction and snapshot of it must
 *  precede, refusing while one is open, as its pin would outlive the ledger; commits not yet
 *  flushed are lost. */
template <cpu_class_t cpu_class_>
void database_free(ustore_database_t database) noexcept {
    database_state<cpu_class_>* const state = state_of<database_state<cpu_class_>>(database);
    if (state && atomic_ref_of<cpu_class_, std::size_t> {state->open_readers}.load(std::memory_order_acquire) != 0)
        return;
    delete state;
}

#pragma endregion Lifetime

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init, pinning the newest published stamp in the store's ledger. */
template <cpu_class_t cpu_class_>
util::status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) noexcept {
    if (!arguments.snapshot) return {util::status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [made, status] = make_handle<ustore_snapshot_handle_t, snapshot_state<cpu_class_>>(
        nvme_variant<cpu_class_>::engine_k, state);
    if (status.failed()) return status;
    // Counted before its stamp is pinned, so no drop runs while it reads
    if (!open_reader(state)) return {util::status_code_t::contended_k, "A collection drop is in progress"};
    transaction_stamp_t const stamp = state.store->ledger().pin_reader(made.state->pin);
    if (arguments.read_stamp) *arguments.read_stamp = static_cast<ustore_stamp_t>(stamp);
    *arguments.snapshot = made.release();
    return {};
}

/** Copies every string of @p collection that @p access reads into @p target with its key, @p window strings per
 *  commit. */
template <cpu_class_t cpu_class_>
util::status_t save_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                            database_state<cpu_class_>& target, std::size_t window,
                            util::progress_t& progress) noexcept {
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
        util::status_t room;
        util::status_t const walked = walk_strings(
            access, collection, {}, std::string_view {from.begin(), from.end()},
            [&](std::string_view text, lookup_result_t found, ustore_key_t const& value) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = util::growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = util::growth_status(text_ends.push_back(offset + text.size()));
                if (room.succeeded()) room = util::growth_status(values.resize(values.size() + 1));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                values[values.size() - 1] = value.uuid;
                return text_ends.size() != window;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (text_ends.empty()) return {};
        if (util::status_t const reserved = util::growth_status(tasks.reserve(text_ends.size())); reserved.failed())
            return reserved;
        for (std::size_t index = 0, begin = 0; index != text_ends.size(); begin = text_ends[index++]) {
            std::string_view const text {texts.data() + begin, text_ends[index] - begin};
            [[maybe_unused]] st::status_t const placed = tasks.push_back(
                string_task_t {collection, text, util::delta_kind_t::write_k, values[index]});
            if (!progress.advance(text.size() + sizeof(ustore_uuid_t))) return util::progress_cancelled_k;
        }
        if (util::status_t const written = write_strings_alone(target, access.scratch, access.queues,
                                                               std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (text_ends.size() < window) return {};
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "A save could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/** Copies every pair of @p collection that @p access reads into @p target, under the same identifier and bounds. */
template <cpu_class_t cpu_class_>
util::status_t save_collection(call_access<cpu_class_> const& access, collection_ids_t const& collection,
                               database_state<cpu_class_>& target, util::progress_t& progress) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::uint16_t store_tag = 0;
    if (util::status_t const held = store.collection_store_tag(collection.id, store_tag); held.failed()) return held;
    if (!target.store->has_collection(collection.id)) {
        collection_config_t config;
        if (util::status_t const shaped = store.collection_config(collection.id, config); shaped.failed())
            return shaped;
        if (util::status_t const made = target.store->create_collection(
                collection.id, collection.name.view(), store_tag, config, collection_invalid_id_k, access.queues);
            made.failed())
            return made;
    }

    // One window of keys is one commit's memory run of the target, which it lands without a flush in between
    collection_config_t config;
    if (util::status_t const shaped = target.store->collection_config(collection.id, config); shaped.failed())
        return shaped;
    std::size_t const window = config.memory_runs_max_deltas;
    if (entry_layout_of(modality_of(store_tag)) == entry_layout_t::strings_to_uuids_k)
        return save_strings(access, collection.id, target, window, progress);
    st::vector<stored_key_t> keys;
    st::vector<util::byte_t> value;
    stored_key_t from = limit<stored_key_t>::min_k;
    for (;;) {
        keys.clear();
        if (util::status_t const collected = collect_keys(access, collection.id, from, limit<stored_key_t>::max_k,
                                                          window, keys);
            collected.failed())
            return collected;
        for (stored_key_t const& key : keys) {
            stored_value_size_t size = 0;
            if (util::status_t const measured = store.stored_value_size(collection.id, key, size, access.stamp,
                                                                        access.queues);
                measured.failed())
                return measured;
            if (value.resize(size) != st::status_t::success_k)
                return {util::status_code_t::out_of_memory_k, "No room for a value being saved"};
            stored_value_size_t copied = 0;
            if (util::status_t const read = store.select(collection.id, key, std::span<util::byte_t> {value}, copied,
                                                         access.stamp, access.queues);
                read.failed())
                return read;
            if (util::status_t const written = target.store->insert(
                    collection.id, key, stored_value_view_t {value.data(), copied}, access.queues);
                written.failed())
                return written;
            if (!progress.advance(sizeof(stored_key_t) + copied)) return util::progress_cancelled_k;
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
util::status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) noexcept {
    if (!view.snapshot) return {util::status_code_t::invalid_argument_k, "A save reads through a snapshot"};
    database_state<cpu_class_>& source = database_state_of<cpu_class_>(arguments);
    call_access<cpu_class_> const access = access_of(source, view);

    ustore_database_open_nvme_t saving {};
    saving.directory = arguments.path;
    if (!util::names_path(saving.directory))
        return {util::status_code_t::invalid_argument_k, "A save needs a manifest path"};
    util::resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = source.transaction_shape;
    user_collection_config_t collections = engine_collection_defaults();
    if (util::status_t const read = read_directories(saving, resources); read.failed()) return read;
    if (util::status_t const read = read_options(saving, resources, transactions, collections); read.failed())
        return read;
    char directory[util::path_max_bytes_k];
    if (util::status_t const located = manifest_directory_of(resources, directory); located.failed()) return located;
    // A raw target names no directory, so a failed save there leaves nothing to remove
    bool existed = true;
    if (directory[0] != '\0') {
        auto [found, exists_status] = util::directory_exists(directory);
        if (exists_status.failed()) return exists_status;
        existed = found;
    }

    auto [addresses, addresses_status] = util::addresses_of(resources);
    if (addresses_status.failed()) return addresses_status;
    util::volume_config_t target;
    target.database = addresses.database;
    target.data_directories = std::span<util::data_address_t const> {addresses.data_directories};
    auto report = [&](std::uint64_t placed, std::uint64_t total) noexcept {
        return !arguments.progress || arguments.progress(placed, total, arguments.progress_state) != ustore_cancel_k;
    };
    auto [archive, archive_status] = source.store->save(
        target, access.stamp, report, util::strings_kit_sha256<strings_kit_of<cpu_class_>> {}, access.queues);
    // A cancelled or failed save gave back every run it placed, and the directory it made
    // goes with them
    if (archive_status.failed() && !existed) [[maybe_unused]]
        util::status_t const emptied = util::remove_directory(directory);
    return archive_status;
}

/** Frees the snapshot behind @p handle, letting go of the stamp it pinned. */
template <cpu_class_t cpu_class_>
void snapshot_free(ustore_snapshot_t handle) noexcept {
    auto* const snapshot = state_of<snapshot_state<cpu_class_>>(handle);
    database_state<cpu_class_>& state = snapshot->state;
    delete snapshot;
    close_reader(state);
}

/** Takes a reading of @p collection into the room of @p pin for one call's verbs on @p view's
 *  thread, or pins the newest stamp of the store's ledger there for a call naming several. */
template <cpu_class_t cpu_class_>
void pin_call(ustore_database_t database, ustore_collection_id_t const* collection, view_t const& view,
              call_pin_t& pin) noexcept {
    using pin_t = typename basic_database_of<cpu_class_>::pin_t;
    using reading_t = collection_reading_of<cpu_class_>;
    static_assert(sizeof(pin_t) <= call_pin_bytes_k && alignof(pin_t) <= alignof(std::max_align_t),
                  "A ledger pin fits the room the router keeps for it");
    static_assert(sizeof(reading_t) <= call_pin_bytes_k && alignof(reading_t) <= alignof(std::max_align_t),
                  "A collection reading fits the room the router keeps for it");
    database_state<cpu_class_>& state = *state_of<database_state<cpu_class_>>(database);
    if (collection) {
        // A collection gone since the call resolved it is left to the verbs, which refuse it.
        auto [taken, taken_status] = state.store->read_collection(*collection, head_stamp_k, view.thread_index);
        if (taken_status.succeeded()) {
            pin.reading = new (pin.room) reading_t {std::move(taken)};
            pin.release = [](call_pin_t& pinned) noexcept {
                std::launder(reinterpret_cast<reading_t*>(pinned.room))->~reading_t();
            };
            return;
        }
    }
    pin_t* const held = new (pin.room) pin_t {};
    pin.stamp = static_cast<ustore_stamp_t>(state.store->ledger().pin_reader(*held));
    pin.release = [](call_pin_t& pinned) noexcept { std::launder(reinterpret_cast<pin_t*>(pinned.room))->~pin_t(); };
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
constexpr ustore_change_kind_t change_kind_of(util::delta_kind_t kind) noexcept {
    switch (kind) {
    case util::delta_kind_t::remove_k: return ustore_change_removal_k;
    case util::delta_kind_t::change_k: return ustore_change_change_k;
    default: return ustore_change_write_k;
    }
}

/** Implements @c ustore_changes_cursor_init, pinning the stamp in the store's ledger, for every
 *  collection but a strings one, whose runs keep no version of a string apart from the next. */
template <cpu_class_t cpu_class_>
util::status_t changes_cursor_init(ustore_changes_cursor_init_t const& arguments, ustore_changes_cursor_t cursor,
                                   view_t const& view) noexcept {
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [entry, look_up_status] = look_up(state, view.scratch, nvme_queues_of(view), arguments.collection);
    if (look_up_status.failed()) return look_up_status;
    if (entry_layout_of(entry.modality) == entry_layout_t::strings_to_uuids_k)
        return {util::status_code_t::unsupported_k, "A strings collection feeds no changes"};
    auto* const feed = new (std::nothrow) changes_state<cpu_class_> {};
    if (!feed) return {util::status_code_t::out_of_memory_k, "No room for the change feed"};
    feed->handed.stamp = static_cast<transaction_stamp_t>(arguments.since_stamp);
    feed->handed.stored_key = limit<stored_key_t>::max_k;
    util::status_t const pinned = state.store->pin_feed(arguments.collection, feed->pin, feed->handed.stamp);
    if (pinned.failed()) {
        delete feed;
        return pinned;
    }
    cursor->state = feed;
    return {};
}

/** Implements @c ustore_changes_cursor_next, advancing once every output reached the arena. */
template <cpu_class_t cpu_class_>
util::status_t changes_cursor_next(ustore_changes_cursor_next_t const& arguments, view_t const& view) noexcept {
    using stored_change_t = typename basic_database_of<cpu_class_>::stored_change_t;
    changes_state<cpu_class_>& feed = *static_cast<changes_state<cpu_class_>*>(arguments.cursor->state);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    ustore_collection_id_t const collection = arguments.cursor->cursor.collection;
    // A dropped collection lingers while a snapshot may read it, so the listing tells if it is gone
    auto [entry, look_up_status] = look_up(state, view.scratch, nvme_queues_of(view), collection);
    if (look_up_status.failed()) return look_up_status;

    st::vector<ustore_key_t, util::arena_allocator<ustore_key_t>> keys {{view.scratch}};
    st::vector<ustore_stamp_t, util::arena_allocator<ustore_stamp_t>> stamps {{view.scratch}};
    st::vector<ustore_change_kind_t, util::arena_allocator<ustore_change_kind_t>> kinds {{view.scratch}};
    util::growing_strings<util::arena_allocator<util::byte_t>> values {view.scratch};
    st::vector<util::byte_t, util::arena_allocator<util::byte_t>> value {{view.scratch}};
    stored_change_t handed = feed.handed;
    auto take = [&]<typename read_type_>(stored_change_t const& change, std::uint64_t value_bytes,
                                         read_type_&& read) noexcept -> status_t {
        if (util::status_t const grown = util::growth_status(keys.push_back(key_of(change.stored_key))); grown.failed())
            return grown;
        if (util::status_t const grown = util::growth_status(
                stamps.push_back(static_cast<ustore_stamp_t>(change.stamp)));
            grown.failed())
            return grown;
        if (util::status_t const grown = util::growth_status(kinds.push_back(change_kind_of(change.kind)));
            grown.failed())
            return grown;
        if (arguments.values) {
            if (util::status_t const grown = util::growth_status(value.resize(value_bytes)); grown.failed())
                return grown;
            if (util::status_t const copied = read(std::span<util::byte_t> {value}); copied.failed()) return copied;
            if (util::status_t const grown = values.push_back(
                    util::value_view_t {std::span<util::byte_t const> {value}}.bytes());
                grown.failed())
                return grown;
        }
        handed = change;
        return {};
    };
    if (util::status_t const pulled = state.store->changes_after(collection, feed.handed, arguments.wanted, take,
                                                                 nvme_queues_of(view));
        pulled.failed())
        return pulled;

    auto [exported_keys, keys_status] = util::arena_copy<ustore_key_t>(arguments.arena, keys);
    if (keys_status.failed()) return keys_status;
    auto [exported_stamps, stamps_status] = util::arena_copy<ustore_stamp_t>(arguments.arena, stamps);
    if (stamps_status.failed()) return stamps_status;
    auto [exported_kinds, kinds_status] = util::arena_copy<ustore_change_kind_t>(arguments.arena, kinds);
    if (kinds_status.failed()) return kinds_status;
    if (arguments.values)
        if (util::status_t const exported = values.export_into(arguments.arena, *arguments.values); exported.failed())
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
