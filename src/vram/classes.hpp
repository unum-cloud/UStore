/**
 *  @file src/vram/classes.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The GPU classes and how the router picks one per device: their ladders, the bundle each
 *      embeds, whether a device's driver loads it, and the per-class tables of loaded kernels.
 *
 *  Engines under @c include/ take the kernel table and kits a class names, never the class, so all
 *  that is keyed by class lives here, beside the bundles unit defining the two functions it names.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`

#include <span> // `std::span`

#include "ustore/utilities/vram_device.hpp" // `gpu_drivers_t`, `kernel_table`

namespace unum::ustore {

using util::byte_t;
using util::gpu_device_t;
using util::gpu_devices_capacity_k;
using util::gpu_drivers_t;
using util::gpu_vendor_t;
using util::kernel_table;

/** The GPU generations UStore tells apart, the device twin of its CPU classes: a class implies its
 *  vendor, and a device takes its vendor's newest class whose code it runs. */
enum class gpu_class_t : std::uint32_t {

    /** No class, which a device reports where no compiled class runs on it. */
    unknown_k = ustore_gpu_class_unknown_k,

    /** Ampere-class NVIDIA - A100, A10 & RTX 30, and every later GPU that compiles its PTX. */
    nvidia_ampere_k = ustore_gpu_class_nvidia_ampere_k,

    /** Ada-class NVIDIA - L4, L40S & RTX 40. */
    nvidia_ada_k = ustore_gpu_class_nvidia_ada_k,

    /** Hopper-class NVIDIA - H100 & H200: thread-block clusters and their shared memory. */
    nvidia_hopper_k = ustore_gpu_class_nvidia_hopper_k,

    /** Blackwell-class NVIDIA - B200, B300 & GB300: cluster launch control. */
    nvidia_blackwell_k = ustore_gpu_class_nvidia_blackwell_k,

    /** Consumer Blackwell NVIDIA - RTX 50, RTX PRO 6000 & DGX Spark. */
    nvidia_blackwellrtx_k = ustore_gpu_class_nvidia_blackwellrtx_k,

    /** CDNA3 AMD - MI300X & MI325X: gfx942, 64-wide waves. */
    amd_cdna3_k = ustore_gpu_class_amd_cdna3_k,

    /** CDNA4 AMD - MI350X & MI355X: gfx950. */
    amd_cdna4_k = ustore_gpu_class_amd_cdna4_k,

    /** CDNA5 AMD - MI400: gfx1250, 32-wide waves. */
    amd_cdna5_k = ustore_gpu_class_amd_cdna5_k,

    /** Apple7-family GPUs and every Apple GPU without a class of its own - M1 & M2. */
    apple_metal7_k = ustore_gpu_class_apple_metal7_k,

    /** Apple9-family GPUs - M3 & M4. */
    apple_metal9_k = ustore_gpu_class_apple_metal9_k,

    /** Apple10-family GPUs - M5. */
    apple_metal10_k = ustore_gpu_class_apple_metal10_k,
};

/** Values a GPU class takes, @c unknown_k included, so a table keyed by class is an array. */
inline constexpr std::size_t gpu_classes_count_k = static_cast<std::size_t>(gpu_class_t::apple_metal10_k) + 1;

/** The bundle of kernels the library embeds for @p gpu_class, empty for a class left out. */
std::span<byte_t const> gpu_class_bundle(gpu_class_t gpu_class) noexcept;

/** Bytes of the walk record StringZilla's MSL regex walks read after an Apple kernel's parameters,
 *  which the one unit filling it checks against StringZilla's own record. */
inline constexpr std::size_t metal_regex_walk_bytes_k = 888;

/** The walk record an Apple regex kernel reads after the parameters its CUDA twin takes, held as
 *  bytes, since StringZilla declares it only in a header one unit may include. */
struct metal_regex_walk_t {
    alignas(8) byte_t bytes[metal_regex_walk_bytes_k];
};

/** The walk record of @p engine: its start states, acceptance bound and row stride, as
 *  StringZilla's own Metal launches fill them, and zeros in a build without the Metal layer. */
metal_regex_walk_t metal_regex_walk(sz_regex_engine_t const& engine) noexcept;

/** Whether @p gpu_class, a class of @p device 's vendor, runs there, as its driver answers by
 *  loading the class's bundle: an NVIDIA device's into the primary context @p drivers retains, an
 *  AMD device's by its ordinal, while an Apple device runs a class of its family or an older one,
 *  as Metal compiles the bundle's source on the device. No device loads the empty bundle of a
 *  class the build left out. */
bool gpu_class_serves(gpu_drivers_t& drivers, gpu_class_t gpu_class, gpu_device_t const& device) noexcept;

/** The class @p gpu_class degrades to, whose code its devices also run, or @c unknown_k for its
 *  vendor's oldest class. */
constexpr gpu_class_t parent_class_of(gpu_class_t gpu_class) noexcept {
    switch (gpu_class) {
    case gpu_class_t::nvidia_blackwell_k:
    case gpu_class_t::nvidia_blackwellrtx_k:
    case gpu_class_t::nvidia_hopper_k: return gpu_class_t::nvidia_ada_k;
    case gpu_class_t::nvidia_ada_k: return gpu_class_t::nvidia_ampere_k;
    case gpu_class_t::amd_cdna5_k: return gpu_class_t::amd_cdna4_k;
    case gpu_class_t::amd_cdna4_k: return gpu_class_t::amd_cdna3_k;
    case gpu_class_t::apple_metal10_k: return gpu_class_t::apple_metal9_k;
    case gpu_class_t::apple_metal9_k: return gpu_class_t::apple_metal7_k;
    default: return gpu_class_t::unknown_k;
    }
}

/** Whether @p gpu_class is the @p gpu_class_ceiling or a class it degrades to, any under none. */
constexpr bool gpu_class_under_ceiling(gpu_class_t gpu_class, gpu_class_t gpu_class_ceiling) noexcept {
    if (gpu_class_ceiling == gpu_class_t::unknown_k) return true;
    for (gpu_class_t rung = gpu_class_ceiling; rung != gpu_class_t::unknown_k; rung = parent_class_of(rung))
        if (rung == gpu_class) return true;
    return false;
}

/** GPU classes of one vendor newest first, the order its ladder tries them in. */
template <gpu_class_t... classes_>
struct gpu_ladder {};

/** NVIDIA classes newest first. */
using nvidia_ladder_t =
    gpu_ladder<gpu_class_t::nvidia_blackwellrtx_k, gpu_class_t::nvidia_blackwell_k, gpu_class_t::nvidia_hopper_k,
               gpu_class_t::nvidia_ada_k, gpu_class_t::nvidia_ampere_k>;

/** AMD classes newest first. */
using amd_ladder_t = gpu_ladder<gpu_class_t::amd_cdna5_k, gpu_class_t::amd_cdna4_k, gpu_class_t::amd_cdna3_k>;

/** Apple classes newest first. */
using apple_ladder_t =
    gpu_ladder<gpu_class_t::apple_metal10_k, gpu_class_t::apple_metal9_k, gpu_class_t::apple_metal7_k>;

/** A set of GPU classes, one bit per class value. */
using gpu_class_mask_t = std::uint32_t;

/** The bit of @p gpu_class in a @c gpu_class_mask_t. */
constexpr gpu_class_mask_t gpu_class_bit(gpu_class_t gpu_class) noexcept {
    return gpu_class_mask_t {1} << static_cast<std::uint32_t>(gpu_class);
}

/** The classes whose bundles @p device loads through @p drivers, each probed alone, as a parent
 *  class may fail to load where a newer one does. */
inline gpu_class_mask_t loaded_gpu_classes(gpu_drivers_t& drivers, gpu_device_t const& device) noexcept {
    gpu_class_mask_t loaded = 0;
    for (std::size_t value = 1; value != gpu_classes_count_k; ++value)
        if (gpu_class_serves(drivers, static_cast<gpu_class_t>(value), device))
            loaded |= gpu_class_bit(static_cast<gpu_class_t>(value));
    return loaded;
}

/** The newest class of @p ladder at or under @p gpu_class_ceiling among @p loaded, or @c unknown_k
 *  where none of them is. */
template <gpu_class_t... classes_>
gpu_class_t pick_gpu_class(gpu_ladder<classes_...>, gpu_class_mask_t loaded, gpu_class_t gpu_class_ceiling) noexcept {
    for (gpu_class_t const gpu_class : {classes_...})
        if (gpu_class_under_ceiling(gpu_class, gpu_class_ceiling) && (loaded & gpu_class_bit(gpu_class)))
            return gpu_class;
    return gpu_class_t::unknown_k;
}

/** The class a device of @p vendor whose bundles @p loaded names takes under @p gpu_class_ceiling,
 *  @c unknown_k setting none, walking its own vendor's ladder; @c unknown_k where none at or under
 *  the ceiling loaded. */
inline gpu_class_t pick_gpu_class(gpu_vendor_t vendor, gpu_class_mask_t loaded,
                                  gpu_class_t gpu_class_ceiling) noexcept {
    switch (vendor) {
    case gpu_vendor_t::nvidia_k: return pick_gpu_class(nvidia_ladder_t {}, loaded, gpu_class_ceiling);
    case gpu_vendor_t::amd_k: return pick_gpu_class(amd_ladder_t {}, loaded, gpu_class_ceiling);
    case gpu_vendor_t::apple_k: return pick_gpu_class(apple_ladder_t {}, loaded, gpu_class_ceiling);
    default: return gpu_class_t::unknown_k;
    }
}

/** The bundles the devices of one vendor loaded, one table per device by its vendor's ordinal and
 *  per GPU class. The first spawn of a class on a device loads its table, holding that device's
 *  @c opening claim so no other spawn loads onto it, and every later one only reads it. */
template <typename module_type_>
class kernel_tables {
  public:

    /**
     *  @brief Loads the bundle of @p gpu_class onto @p device through @p api and resolves @p names,
     *      unless its table holds it already.
     *  @param[in] api The vendor's driver table.
     *  @param[in] place Where the module loads: an NVIDIA or AMD device's ordinal, or an Apple
     *      device's @c id<MTLDevice>.
     *  @param[in] device The vendor's ordinal of the device.
     *  @param[in] gpu_class The class whose bundle loads.
     *  @param[in] names The C names of the kernels to resolve, which outlive the tables.
     *  @param[in] options What else the vendor's module loader takes, like a Metal language.
     *
     *  @par Statuses:
     *  - @c status_code_t::device_k The ordinal passes the devices UStore lists, the device refused
     *    the bundle, an empty one included, or the bundle exports no kernel of a name.
     */
    template <typename api_type_, typename place_type_, typename... options_types_>
    util::status_t load(api_type_ const& api, place_type_ place, int device, gpu_class_t gpu_class,
                        std::span<char const* const> names, options_types_... options) noexcept {
        if (device < 0 || device >= static_cast<int>(gpu_devices_capacity_k))
            return {util::status_code_t::device_k, "The ordinal passes the devices UStore lists"};
        if (table_of(device, gpu_class)) return {};
        std::span<byte_t const> const bundle = gpu_class_bundle(gpu_class);
        auto [bundle_module, load_status] = module_type_::load(api, place, bundle, options...);
        if (load_status.failed()) return load_status;
        kernel_table<module_type_>& table = tables_[device][static_cast<std::size_t>(gpu_class)];
        if (st::failed(table.functions.resize(names.size())))
            return {util::status_code_t::out_of_memory_k, "No room for the bundle kernel table"};
        for (std::size_t index = 0; index != names.size(); ++index)
            if (!(table.functions[index] = bundle_module.function(names[index])))
                return {util::status_code_t::device_k, "The bundle exports no kernel of a name"};
        table.device = device, table.module = std::move(bundle_module), table.names = names;
        table.bundle = bundle.data();
        return {};
    }

    /** The table of the bundle of @p gpu_class on @p device, or null where it never loaded. */
    kernel_table<module_type_> const* table_of(int device, gpu_class_t gpu_class) const noexcept {
        if (device < 0 || device >= static_cast<int>(gpu_devices_capacity_k) ||
            static_cast<std::size_t>(gpu_class) >= gpu_classes_count_k)
            return nullptr;
        kernel_table<module_type_> const& table = tables_[device][static_cast<std::size_t>(gpu_class)];
        return table.bundle ? &table : nullptr;
    }

    /** Held by the spawn loading a bundle onto the device of the vendor's ordinal @p device, while
     *  no other spawn loads onto it. */
    claim_t& opening(int device) noexcept { return opening_[device]; }

  private:
    kernel_table<module_type_> tables_[gpu_devices_capacity_k][gpu_classes_count_k];
    claim_t opening_[gpu_devices_capacity_k] = {};
};

} // namespace unum::ustore
