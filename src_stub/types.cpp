/**
 *  @file src_stub/types.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The vocabulary verbs that belong to no database: outcomes, the spelling of every closed
 *      set, the version, executors and arenas.
 *
 *  An executor starts no threads, so every call runs on its caller, a deferred one at submission,
 *  final when it returns. A call meeting a running one on its executor finishes contended. No GPU
 *  is ever listed, so a VRAM executor is refused for the ordinal no device carries. The one CPU
 *  class is serial, so any known CPU class ceiling picks it, and the stub refuses only a value
 *  that no class takes.
 */
#include <sys/stat.h> // `stat` - the memory domains the system lists

#include <limits> // `std::numeric_limits`
#include <string> // `std::to_string`

#include "lib.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

/** One enumerator and its canonical spelling. */
template <typename enum_type_>
struct spelling {
    enum_type_ value;
    std::string_view name;
};

/** The spelling of @p value in @p names, or "unrecognized" for a value no enumerator holds. */
template <typename enum_type_, std::size_t count_>
char const* name_in(spelling<enum_type_> const (&names)[count_], enum_type_ value) noexcept {
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read
    // in place instead
    using bits_t = std::underlying_type_t<enum_type_>;
    for (spelling<enum_type_> const& spelling : names)
        if (std::bit_cast<bits_t>(spelling.value) == std::bit_cast<bits_t>(value)) return spelling.name.data();
    return "unrecognized";
}

/** Writes to @p value the enumerator @p names spells as @p name, or refuses a name none holds. */
template <typename enum_type_, std::size_t count_>
ustore_status_t value_in(spelling<enum_type_> const (&names)[count_], ustore_bytes_view_t name,
                         enum_type_* value) noexcept {
    if (!value) return ustore_status_invalid_argument_k;
    std::string_view const text = text_of(name);
    for (spelling<enum_type_> const& spelling : names)
        if (spelling.name == text) return *value = spelling.value, ustore_status_success_k;
    return ustore_status_invalid_argument_k;
}

constexpr spelling<ustore_status_t> status_names_k[] {
    {ustore_status_success_k, "success"},
    {ustore_status_deferred_k, "deferred"},
    {ustore_status_out_of_memory_k, "out-of-memory"},
    {ustore_status_read_only_k, "read-only"},
    {ustore_status_io_k, "io"},
    {ustore_status_device_k, "device"},
    {ustore_status_malformed_k, "malformed"},
    {ustore_status_internal_k, "internal"},
    {ustore_status_overflow_k, "overflow"},
    {ustore_status_uninitialized_k, "uninitialized"},
    {ustore_status_config_mismatch_k, "config-mismatch"},
    {ustore_status_contended_k, "contended"},
    {ustore_status_cancelled_k, "cancelled"},
    {ustore_status_unsupported_k, "unsupported"},
    {ustore_status_invalid_argument_k, "invalid-argument"},
    {ustore_status_not_found_k, "not-found"},
    {ustore_status_already_exists_k, "already-exists"},
    {ustore_status_write_conflict_k, "write-conflict"},
    {ustore_status_read_conflict_k, "read-conflict"},
    {ustore_status_phantom_conflict_k, "phantom-conflict"},
    {ustore_status_capacity_exhausted_k, "capacity-exhausted"},
};

constexpr spelling<ustore_phase_t> phase_names_k[] {
    {ustore_phase_none_k, "none"},       {ustore_phase_flush_k, "flush"}, {ustore_phase_merge_k, "merge"},
    {ustore_phase_reclaim_k, "reclaim"}, {ustore_phase_done_k, "done"},
};

constexpr spelling<ustore_mood_t> mood_names_k[] {
    {ustore_mood_parked_k, "parked"},
    {ustore_mood_spinning_k, "spinning"},
};

constexpr spelling<ustore_gds_t> gds_names_k[] {
    {ustore_gds_automatic_k, "automatic"},
    {ustore_gds_direct_k, "direct"},
    {ustore_gds_compatibility_k, "compatibility"},
};

constexpr spelling<ustore_host_access_t> host_access_names_k[] {
    {ustore_host_access_refused_k, "refused"},
    {ustore_host_access_readable_k, "readable"},
};

constexpr spelling<ustore_gpu_vendor_t> gpu_vendor_names_k[] {
    {ustore_gpu_vendor_unknown_k, "unknown"},
    {ustore_gpu_vendor_nvidia_k, "nvidia"},
    {ustore_gpu_vendor_amd_k, "amd"},
    {ustore_gpu_vendor_apple_k, "apple"},
};

constexpr spelling<ustore_gpu_class_t> gpu_class_names_k[] {
    {ustore_gpu_class_unknown_k, "unknown"},
    {ustore_gpu_class_nvidia_ampere_k, "nvidia-ampere"},
    {ustore_gpu_class_nvidia_ada_k, "nvidia-ada"},
    {ustore_gpu_class_nvidia_hopper_k, "nvidia-hopper"},
    {ustore_gpu_class_nvidia_blackwell_k, "nvidia-blackwell"},
    {ustore_gpu_class_nvidia_blackwellrtx_k, "nvidia-blackwellrtx"},
    {ustore_gpu_class_amd_cdna3_k, "amd-cdna3"},
    {ustore_gpu_class_amd_cdna4_k, "amd-cdna4"},
    {ustore_gpu_class_amd_cdna5_k, "amd-cdna5"},
    {ustore_gpu_class_apple_metal7_k, "apple-metal7"},
    {ustore_gpu_class_apple_metal9_k, "apple-metal9"},
    {ustore_gpu_class_apple_metal10_k, "apple-metal10"},
};

constexpr spelling<ustore_pattern_kind_t> pattern_kind_names_k[] {
    {ustore_pattern_regex_k, "regex"},
    {ustore_pattern_substrings_k, "substrings"},
};

constexpr spelling<ustore_case_sensitivity_t> case_sensitivity_names_k[] {
    {ustore_cased_k, "cased"},
    {ustore_uncased_k, "uncased"},
};

constexpr spelling<ustore_predicate_cost_t> predicate_cost_names_k[] {
    {ustore_predicate_cost_cheap_k, "cheap"},
    {ustore_predicate_cost_expensive_k, "expensive"},
};

constexpr spelling<ustore_cpu_class_t> cpu_class_names_k[] {
    {ustore_cpu_class_unknown_k, "unknown"},
    {ustore_cpu_class_serial_k, "serial"},
    {ustore_cpu_class_monoversioned_k, "monoversioned"},
    {ustore_cpu_class_intel_haswell_k, "intel-haswell"},
    {ustore_cpu_class_intel_sierra_k, "intel-sierra"},
    {ustore_cpu_class_intel_skylake_k, "intel-skylake"},
    {ustore_cpu_class_intel_icelake_k, "intel-icelake"},
    {ustore_cpu_class_aws_g2_k, "aws-g2"},
    {ustore_cpu_class_apple_m2_k, "apple-m2"},
    {ustore_cpu_class_aws_g5_k, "aws-g5"},
    {ustore_cpu_class_alibaba_c930_k, "alibaba-c930"},
    {ustore_cpu_class_ibm_power10_k, "ibm-power10"},
};

constexpr spelling<ustore_transport_t> transport_names_k[] {
    {ustore_transport_automatic_k, "automatic"},     {ustore_transport_blocking_k, "blocking"},
    {ustore_transport_linux_uring_k, "linux-uring"}, {ustore_transport_linux_aio_k, "linux-aio"},
    {ustore_transport_linux_spdk_k, "linux-spdk"},
};

constexpr spelling<ustore_durability_t> durability_names_k[] {
    {ustore_durability_buffered_k, "buffered"},
    {ustore_durability_flushed_k, "flushed"},
};

constexpr spelling<ustore_watch_t> watch_names_k[] {
    {ustore_watch_keys_k, "keys"},
    {ustore_watch_none_k, "none"},
};

constexpr spelling<ustore_scan_mode_t> scan_mode_names_k[] {
    {ustore_scan_ordered_k, "ordered"},
    {ustore_scan_unordered_k, "unordered"},
};

constexpr spelling<ustore_change_kind_t> change_kind_names_k[] {
    {ustore_change_write_k, "write"},
    {ustore_change_removal_k, "removal"},
    {ustore_change_change_k, "change"},
};

constexpr spelling<ustore_scan_direction_t> scan_direction_names_k[] {
    {ustore_scan_ascending_k, "ascending"},
    {ustore_scan_descending_k, "descending"},
};

constexpr spelling<ustore_ingest_order_t> ingest_order_names_k[] {
    {ustore_ingest_sorted_k, "sorted"},
    {ustore_ingest_unsorted_k, "unsorted"},
};

constexpr spelling<ustore_drop_mode_t> drop_mode_names_k[] {
    {ustore_drop_collection_k, "collection"},
    {ustore_drop_pairs_k, "pairs"},
    {ustore_drop_values_k, "values"},
};

constexpr spelling<ustore_isolation_t> isolation_names_k[] {
    {ustore_isolation_strict_serializable_k, "strict-serializable"},
    {ustore_isolation_snapshot_k, "snapshot"},
};

constexpr spelling<ustore_engine_t> engine_names_k[] {
    {ustore_engine_ram_k, "ram"},
    {ustore_engine_rocksdb_k, "rocksdb"},
    {ustore_engine_leveldb_k, "leveldb"},
    {ustore_engine_nvme_k, "nvme"},
};

constexpr spelling<ustore_modality_t> modality_names_k[] {
    {ustore_modality_blobs_k, "blobs"},     {ustore_modality_docs_k, "docs"},   {ustore_modality_graph_k, "graph"},
    {ustore_modality_strings_k, "strings"}, {ustore_modality_pages_k, "pages"},
};

constexpr spelling<ustore_vertex_role_t> vertex_role_names_k[] {
    {ustore_vertex_role_unknown_k, "unknown"},
    {ustore_vertex_role_source_k, "source"},
    {ustore_vertex_role_target_k, "target"},
    {ustore_vertex_role_any_k, "any"},
};

constexpr spelling<ustore_docs_field_type_t> docs_field_type_names_k[] {
    {ustore_docs_field_type_json_k, "json"},     {ustore_docs_field_type_null_k, "null"},
    {ustore_docs_field_type_bool_k, "bool"},     {ustore_docs_field_type_uuid_k, "uuid"},
    {ustore_docs_field_type_f64_k, "f64"},       {ustore_docs_field_type_f32_k, "f32"},
    {ustore_docs_field_type_f16_k, "f16"},       {ustore_docs_field_type_i64_k, "i64"},
    {ustore_docs_field_type_i32_k, "i32"},       {ustore_docs_field_type_i16_k, "i16"},
    {ustore_docs_field_type_i8_k, "i8"},         {ustore_docs_field_type_u64_k, "u64"},
    {ustore_docs_field_type_u32_k, "u32"},       {ustore_docs_field_type_u16_k, "u16"},
    {ustore_docs_field_type_u8_k, "u8"},         {ustore_docs_field_type_binary_k, "binary"},
    {ustore_docs_field_type_string_k, "string"},
};

constexpr spelling<ustore_docs_modification_t> docs_modification_names_k[] {
    {ustore_docs_modify_upsert_k, "upsert"}, {ustore_docs_modify_update_k, "update"},
    {ustore_docs_modify_insert_k, "insert"}, {ustore_docs_modify_patch_k, "patch"},
    {ustore_docs_modify_merge_k, "merge"},
};

constexpr spelling<ustore_page_encoding_t> page_encoding_names_k[] {
    {ustore_page_encoding_html_k, "html"},         {ustore_page_encoding_xml_k, "xml"},
    {ustore_page_encoding_markdown_k, "markdown"}, {ustore_page_encoding_text_k, "text"},
    {ustore_page_encoding_json_k, "json"},
};

/** The memory domains the system lists, numbered from zero, or one where it lists none. */
std::size_t memory_domains_count() noexcept {
    std::size_t count = 0;
    struct stat info {};
    while (count < 4096 && ::stat(("/sys/devices/system/node/node" + std::to_string(count)).c_str(), &info) == 0)
        ++count;
    return count ? count : 1;
}

/** Refuses memory domains no domain of this machine holds. */
bool refuse_memory_domains(call_t& call, ustore_ram_options_t const& options) noexcept {
    if (!options.memory_domains || !options.memory_domains_count) return false;
    std::size_t const available = memory_domains_count();
    for (std::size_t index = 0; index != options.memory_domains_count; ++index)
        if (options.memory_domains[index] < 0 || static_cast<std::size_t>(options.memory_domains[index]) >= available)
            return call.stop(ustore_status_invalid_argument_k, "No memory domain holds the identifier");
    return false;
}

/** Refuses a CPU class ceiling no constant names, while a known one picks serial, the one class. */
bool refuse_cpu_class_ceiling(call_t& call, ustore_cpu_class_t cpu_class_ceiling) noexcept {
    if (std::string_view(name_in(cpu_class_names_k, cpu_class_ceiling)) == "unrecognized")
        return call.stop(ustore_status_invalid_argument_k, "Unknown CPU class ceiling");
    return false;
}

} // namespace unum::ustore::stub

ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome) {
    ustore_status_t status = load_status(outcome);
    while (status == ustore_status_deferred_k) std::this_thread::yield(), status = load_status(outcome);
    return status;
}

ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome) { return load_status(outcome); }

/** Spells a host access, which a caller is only ever answered, so no name reads it back. */
char const* ustore_host_access_name(ustore_host_access_t value) { return name_in(host_access_names_k, value); }

#define spell_names_(type_, prefix_, table_)                                               \
    char const* ustore_##prefix_##_name(type_ value) { return name_in(table_, value); }    \
    ustore_status_t ustore_##prefix_##_from_name(ustore_bytes_view_t name, type_* value) { \
        return value_in(table_, name, value);                                              \
    }

spell_names_(ustore_status_t, status, status_names_k) spell_names_(ustore_phase_t, phase, phase_names_k)
    spell_names_(ustore_mood_t, mood, mood_names_k) spell_names_(ustore_cpu_class_t, cpu_class, cpu_class_names_k)
        spell_names_(ustore_pattern_kind_t, pattern_kind, pattern_kind_names_k) spell_names_(ustore_case_sensitivity_t,
                                                                                             case_sensitivity,
                                                                                             case_sensitivity_names_k)
            spell_names_(ustore_predicate_cost_t, predicate_cost, predicate_cost_names_k) spell_names_(ustore_gds_t,
                                                                                                       gds, gds_names_k)
                spell_names_(ustore_gpu_vendor_t, gpu_vendor,
                             gpu_vendor_names_k) spell_names_(ustore_gpu_class_t, gpu_class, gpu_class_names_k)
                    spell_names_(ustore_transport_t, transport,
                                 transport_names_k) spell_names_(ustore_durability_t, durability, durability_names_k)
                        spell_names_(ustore_watch_t, watch, watch_names_k) spell_names_(ustore_scan_mode_t, scan_mode,
                                                                                        scan_mode_names_k)
                            spell_names_(ustore_change_kind_t, change_kind, change_kind_names_k)
                                spell_names_(ustore_scan_direction_t, scan_direction, scan_direction_names_k)
                                    spell_names_(ustore_ingest_order_t, ingest_order, ingest_order_names_k)
                                        spell_names_(ustore_drop_mode_t, drop_mode, drop_mode_names_k)
                                            spell_names_(ustore_isolation_t, isolation, isolation_names_k)
                                                spell_names_(ustore_engine_t, engine,
                                                             engine_names_k) spell_names_(ustore_modality_t, modality,
                                                                                          modality_names_k)
                                                    spell_names_(ustore_vertex_role_t, vertex_role, vertex_role_names_k)
                                                        spell_names_(ustore_docs_field_type_t, docs_field_type,
                                                                     docs_field_type_names_k)
                                                            spell_names_(ustore_docs_modification_t, docs_modification,
                                                                         docs_modification_names_k)
                                                                spell_names_(ustore_page_encoding_t, page_encoding,
                                                                             page_encoding_names_k)

#undef spell_names_

                                                                    void ustore_version(int* major, int* minor,
                                                                                        int* patch) {
    *major = USTORE_VERSION_MAJOR;
    *minor = USTORE_VERSION_MINOR;
    *patch = USTORE_VERSION_PATCH;
}

#if !defined(USTORE_STUB_VERSION)
#define USTORE_STUB_VERSION "1.0.0"
#endif

char const* ustore_version_name(void) { return USTORE_STUB_VERSION; }

#pragma region Executors

void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->executor) *arguments->executor = nullptr;
    if (!arguments->executor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the executor");
    if (refuse_memory_domains(call, arguments->ram) || refuse_cpu_class_ceiling(call, arguments->cpu_class_ceiling))
        return;
    auto* const executor = new (std::nothrow) ustore_executor_handle_t {};
    if (!executor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the executor");
    executor->threads = arguments->threads ? arguments->threads : 1;
    *arguments->executor = executor;
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->executor) *arguments->executor = nullptr;
    if (!arguments->executor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the executor");
    if (std::string_view(ustore_transport_name(arguments->nvme.transport)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "Unknown transport");
    if (arguments->nvme.paths_count && !arguments->nvme.paths)
        return call.refuse(ustore_status_invalid_argument_k, "Paths with a count need their views");
    if (refuse_memory_domains(call, arguments->ram) || refuse_cpu_class_ceiling(call, arguments->cpu_class_ceiling))
        return;
    auto* const executor = new (std::nothrow) ustore_executor_handle_t {};
    if (!executor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the executor");
    executor->for_nvme = true;
    executor->nvme = arguments->nvme;
    executor->threads = arguments->threads ? arguments->threads : 1;
    *arguments->executor = executor;
}

void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->executor) *arguments->executor = nullptr;
    if (!arguments->executor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the executor");
    if (std::string_view(ustore_gds_name(arguments->gds)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "Unknown GPUDirect Storage mode");
    if (arguments->vram.gpu_class_ceiling != ustore_gpu_class_unknown_k &&
        std::string_view(ustore_gpu_class_name(arguments->vram.gpu_class_ceiling)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "Unknown GPU class ceiling");
    // The stand-in drives no GPU, so it lists none and no ordinal names a device
    call.refuse(ustore_status_device_k, "No GPU holds the ordinal");
}

void ustore_gpu_devices(ustore_gpu_devices_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->devices_count) *arguments->devices_count = 0;
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    call_t call(arguments->outcome);
    if (std::string_view(ustore_mood_name(arguments->mood)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "The mood is neither parked nor spinning");
    ustore_executor_handle_t* const executor = arguments->executor;
    if (!executor) return;
    if (executor->claim.held() && executor->body_thread.load() == std::this_thread::get_id())
        return call.refuse(ustore_status_contended_k, "A callback cannot drain the executor running it");
    while (executor->claim.held()) std::this_thread::yield();
}

void ustore_executor_free(ustore_executor_t executor) {
    if (!executor) return;
    while (executor->claim.held()) std::this_thread::yield();
    delete executor;
}

#pragma endregion Executors

#pragma region Arenas

void ustore_arena_init(ustore_arena_init_t* arguments) {
    call_t call(arguments->outcome);
    if (!arguments->arena)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the arena");
    *arguments->arena = nullptr;
    auto* const arena = new (std::nothrow) ustore_arena_handle_t {};
    if (!arena) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the arena");
    *arguments->arena = arena;
}

ustore_status_t ustore_arena_free(ustore_arena_t arena) {
    if (!arena) return ustore_status_success_k;
    wait_named(arena->named_calls);
    if (arena->exports.load(std::memory_order_acquire) != 0) return ustore_status_contended_k;
    while (arena->claim.held()) std::this_thread::yield();
    delete arena;
    return ustore_status_success_k;
}

ustore_status_t ustore_arena_export_open(ustore_arena_t arena) {
    if (!arena) return ustore_status_invalid_argument_k;
    std::uint32_t seen = arena->exports.load(std::memory_order_relaxed);
    do
        if (seen == (std::numeric_limits<std::uint32_t>::max)()) return ustore_status_overflow_k;
    while (!arena->exports.compare_exchange_weak(seen, seen + 1, std::memory_order_seq_cst));
    if (arena->claim.held()) return ustore_arena_export_close(arena), ustore_status_contended_k;
    return ustore_status_success_k;
}

void ustore_arena_export_close(ustore_arena_t arena) {
    if (!arena) return;
    std::uint32_t seen = arena->exports.load(std::memory_order_relaxed);
    while (seen != 0 && !arena->exports.compare_exchange_weak(seen, seen - 1, std::memory_order_release)) {}
}

void ustore_arena_describe(ustore_arena_describe_t* arguments) {
    call_t call(arguments->outcome);
    if (!arguments->arena || !arguments->description)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena and a description output");
    *arguments->description = {ustore_residency_ram_k, -1, ustore_gpu_vendor_unknown_k, -1,
                               ustore_host_access_readable_k};
}

#pragma endregion Arenas
