/**
 *  @file test/vram/towers.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Every read verb on a VRAM executor of each listed GPU against the same verb on the host:
 *      listings, device arenas, input staging with its cross-vendor refusal, the upload tower's
 *      copies, and tapes the device assembles.
 *
 *  The suite keeps an executor of its own on every device, through the same driver tables the library uses, to place
 *  inputs on the device and fetch outputs back. With the HIP stand-in on @c LD_LIBRARY_PATH it also counts what one
 *  call costs: copies, fills, waits and allocations, cold and warm.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`
#include <cstring> // `std::memcmp`

#include <dlfcn.h> // `dlopen`, `dlsym`

#include <array>       // `std::array`
#include <span>        // `std::span`, `std::as_writable_bytes`
#include <string>      // `std::string`, `std::to_string`
#include <string_view> // `std::string_view`
#include <variant>     // `std::variant`, `std::visit`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ustore.h"
#include "ustore/utilities/vram_device.hpp" // `gpu_drivers_t`, `spawn_gpu_executor`, `kernel_tables`, `assemble_tape`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_success`, `report`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;

namespace unum::ustore::test {

/** Entries every collection holds, enough for a tape and a bitmap to span several bytes and blocks. */
constexpr std::size_t entries_k = 96;

/** A tape of strings the suite owns, written as the C ABI reads one. */
struct tape_t {
    std::string bytes;
    std::vector<std::uint32_t> offsets {0};

    void push(std::string_view text) {
        bytes.append(text);
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
    }
    std::size_t size() const noexcept { return offsets.size() - 1; }
    ustore_strings_view_t view() const noexcept { return u32_tape(bytes.data(), offsets.data()); }
};

/** What the HIP stand-in counted, read through the library the runtime opened. */
struct fake_hip_counters_t {
    std::size_t allocations, frees, managed_allocations, fills, uploads, downloads, registrations, unregistrations,
        waits, pointer_queries;
};

/** The stand-in's counters, or null when the real runtime or none is loaded. */
fake_hip_counters_t const* fake_hip_counters() noexcept {
    void* const library = dlopen("libamdhip64.so", RTLD_NOW | RTLD_NOLOAD);
    return library ? static_cast<fake_hip_counters_t const*>(dlsym(library, "fake_hip_counters")) : nullptr;
}

/** Hands @p body the executor a mover holds, of either vendor, and nothing for none. */
template <typename body_type_>
struct device_visitor {
    body_type_& body;
    void operator()(std::monostate) const noexcept {}
    void operator()(cuda_executor_t& executor) const noexcept { body(executor); }
    void operator()(hip_executor_t& executor) const noexcept { body(executor); }
};

/** The suite's own executor on one listed device, which places inputs there and fetches outputs back. */
class mover_t {
  public:
    status_t open(ustore_gpu_device_t const& listed) noexcept {
        gpu_device_t device;
        device.vendor = static_cast<gpu_vendor_t>(listed.vendor);
        device.vendor_ordinal = listed.vendor_ordinal;
        if (status_t const opened = open_gpu_device(drivers_, device, cuda_kernels_, hip_kernels_); opened.failed())
            return opened;
        return spawn_gpu_executor(drivers_, cuda_kernels_, hip_kernels_, device,
                                  static_cast<gpu_class_t>(listed.gpu_class), nullptr, 0, ustore_gds_automatic_k,
                                  executor_);
    }

    ~mover_t() noexcept {
        visit([&](auto& executor) noexcept {
            if (executor.bind().failed()) return;
            for (std::uintptr_t address : placed_) executor.free_async(address);
            [[maybe_unused]] status_t const waited = executor.wait();
        });
    }

    /** Copies @p count elements at device @p device back to the host. */
    template <typename element_type_>
    std::vector<element_type_> fetch(element_type_ const* device, std::size_t count) noexcept {
        std::vector<element_type_> host(count);
        if (!count || !device) return host;
        copy([&](auto& executor) noexcept {
            std::span<element_type_> const target {host};
            return executor.download_async(std::as_writable_bytes(target), reinterpret_cast<std::uintptr_t>(device));
        });
        return host;
    }

    /** Copies @p host onto the device, freed with the mover. */
    template <typename element_type_>
    element_type_* place(std::span<element_type_ const> host) noexcept {
        std::uintptr_t address = 0;
        copy([&](auto& executor) noexcept {
            auto [allocated, allocated_status] = executor.allocate_async(host.size_bytes());
            if (allocated_status.failed()) return allocated_status;
            address = allocated;
            placed_.push_back(address);
            return executor.upload_async(address, std::as_bytes(host));
        });
        return reinterpret_cast<element_type_*>(address);
    }

    /** Whether @p pointer lies on this mover's device. */
    bool holds(void const* pointer) noexcept {
        bool held = false;
        visit([&](auto& executor) noexcept { held = executor.device_of(pointer) == executor.device(); });
        return held;
    }

    /** The first failure of a copy, sticky until the mover dies. */
    status_t status() const noexcept { return status_; }

    /** Runs @p body's work on the device and waits for it, keeping the first failure. */
    template <typename body_type_>
    void copy(body_type_&& body) noexcept {
        visit([&](auto& executor) noexcept {
            if (status_.failed() || (status_ = executor.bind()).failed() || (status_ = body(executor)).failed()) return;
            status_ = executor.wait();
        });
    }

    /** Whether the device loaded the library's kernels. */
    bool launches() noexcept {
        bool loaded = false;
        visit([&](auto& executor) noexcept { loaded = executor.kernels() != nullptr; });
        return loaded;
    }

  private:
    template <typename body_type_>
    void visit(body_type_&& body) noexcept {
        std::visit(device_visitor<body_type_> {body}, executor_);
    }

    gpu_drivers_t drivers_;
    kernel_tables<cuda_module_t> cuda_kernels_;
    kernel_tables<hip_module_t> hip_kernels_;
    std::variant<std::monostate, cuda_executor_t, hip_executor_t> executor_;
    std::vector<std::uintptr_t> placed_;
    status_t status_;
};

/** Every listed GPU. */
std::vector<ustore_gpu_device_t> listed_devices() noexcept {
    ustore_outcome_t outcome {};
    ustore_size_t count = 0;
    std::vector<ustore_gpu_device_t> devices(64);
    ustore_gpu_devices_t listing {.outcome = &outcome,
                                  .devices = devices.data(),
                                  .devices_capacity = devices.size(),
                                  .devices_count = &count};
    ustore_gpu_devices(&listing);
    devices.resize(outcome.status == ustore_status_success_k ? static_cast<std::size_t>(count) : 0);
    return devices;
}

/** A VRAM executor on UStore ordinal @p ordinal and an arena on its device, freed together. */
struct device_call_t {
    ustore_executor_t executor = nullptr;
    ustore_arena_t arena = nullptr;
    status_t status;

    explicit device_call_t(std::size_t ordinal) noexcept {
        ustore_outcome_t outcome {};
        ustore_executor_for_vram_t spawn {.executor = &executor,
                                          .outcome = &outcome,
                                          .device = static_cast<std::int32_t>(ordinal)};
        ustore_executor_for_vram(&spawn);
        if ((status = status_t {outcome}).failed()) return;
        ustore_arena_init_t init {.arena = &arena, .executor = executor, .outcome = &outcome};
        ustore_arena_init(&init);
        status = status_t {outcome};
    }
    ~device_call_t() noexcept {
        ustore_arena_free(arena);
        ustore_executor_free(executor);
    }
};

/** A host arena for the reference call. */
struct host_arena_t {
    ustore_arena_t arena = nullptr;
    host_arena_t() noexcept {
        ustore_outcome_t outcome {};
        ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
        ustore_arena_init(&init);
    }
    ~host_arena_t() noexcept { ustore_arena_free(arena); }
};

/** One collection of every modality, filled with @c entries_k entries each. */
struct fixture_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t blobs {}, strings {}, docs {}, graph {}, pages {};
    std::vector<ustore_key_t> keys;
    tape_t values, texts, documents, renderings;
    std::size_t failures = 0;

    /** Fills a fresh database of @p config's engine, persisted under @p name inside its directory when it has one. */
    fixture_t(config_t const& config, char const* name) noexcept {
        path_t const subdirectory {config.directory() ? config.directory() : "", name};
        config_t const fresh {config.options(), config.directory() ? subdirectory.c_str() : nullptr};
        if (config.directory() && (failures += expect(recreate_directory(subdirectory.c_str())))) return;
        auto [opened, open_status] = open_raw(fresh);
        if ((failures += expect(open_status.succeeded()))) return;
        database = opened;
        for (std::size_t index = 0; index != entries_k; ++index) {
            keys.push_back(key_from_integer(index + 1));
            std::string const number = std::to_string(index + 1);
            values.push(std::string(index % 7, 'v') + number);
            texts.push("text:" + number);
            documents.push(fmt::format(R"({{"id":{},"name":"n{}","score":{}.5}})", index + 1, number, index));
            renderings.push("<p><b>" + number + "</b></p>");
        }
        ustore_keys_view_t const strided {keys.data(), sizeof(ustore_key_t)};
        blobs = create("blobs", ustore_modality_blobs_k);
        strings = create("strings", ustore_modality_strings_k);
        docs = create("docs", ustore_modality_docs_k);
        graph = create("graph", ustore_modality_graph_k);
        pages = create("pages", ustore_modality_pages_k);

        ustore_outcome_t outcome {};
        ustore_blobs_write_t blobs_write {.database = database, .outcome = &outcome, .count = entries_k};
        blobs_write.collections = {&blobs, 0}, blobs_write.keys = strided, blobs_write.values = values.view();
        ustore_blobs_write(&blobs_write);
        failures += expect_success(outcome);
        ustore_strings_write_t strings_write {.database = database, .outcome = &outcome, .count = entries_k};
        strings_write.collections = {&strings, 0}, strings_write.strings = texts.view(), strings_write.values = strided;
        ustore_strings_write(&strings_write);
        failures += expect_success(outcome);
        ustore_docs_write_t docs_write {.database = database, .outcome = &outcome, .count = entries_k};
        docs_write.collections = {&docs, 0}, docs_write.keys = strided, docs_write.values = documents.view();
        ustore_docs_write(&docs_write);
        failures += expect_success(outcome);
        // Edge `i` joins vertex `i` to vertex `i + 1`, so every vertex but the ends has degree two
        std::vector<ustore_key_t> targets(keys.begin() + 1, keys.end());
        targets.push_back(keys.front());
        ustore_graph_write_edges_t edges_write {.database = database, .outcome = &outcome, .count = entries_k};
        edges_write.collections = {&graph, 0}, edges_write.edges = strided, edges_write.sources = strided;
        edges_write.targets = {targets.data(), sizeof(ustore_key_t)};
        ustore_graph_write_edges(&edges_write);
        failures += expect_success(outcome);
        ustore_pages_import_t pages_import {.database = database, .outcome = &outcome, .count = entries_k};
        pages_import.collections = {&pages, 0}, pages_import.keys = strided, pages_import.values = renderings.view();
        pages_import.encoding = ustore_page_encoding_xml_k;
        ustore_pages_import(&pages_import);
        failures += expect_success(outcome);
    }
    ~fixture_t() noexcept { ustore_database_free(database); }

    ustore_collection_id_t create(std::string_view name, ustore_modality_t modality) noexcept {
        ustore_collection_id_t created {};
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {.database = database,
                                           .outcome = &outcome,
                                           .name = bytes_of(name),
                                           .modality = modality,
                                           .collection = &created};
        ustore_collection_create(&create);
        failures += expect_success(outcome);
        return created;
    }
};

/** Counts a failure unless the @p count elements at device @p device equal those at host @p host, and live on the
 *  device of @p mover. */
template <typename element_type_>
std::size_t expect_column(mover_t& mover, element_type_ const* host, element_type_ const* device, std::size_t count) {
    if (!count || (!host && !device)) return 0;
    if (expect(host && device)) return 1;
    std::vector<element_type_> const fetched = mover.fetch(device, count);
    return expect(mover.holds(device)) + expect(mover.status().succeeded()) +
           expect(std::memcmp(fetched.data(), host, count * sizeof(element_type_)) == 0);
}

/** Counts a failure unless the device tape of @p count strings equals the host one. */
std::size_t expect_tape(mover_t& mover, ustore_strings_view_t const& host, ustore_strings_view_t const& device,
                        std::size_t count) {
    if (!count) return 0;
    if (expect(host.layout == device.layout)) return 1;
    if (host.layout == ustore_strings_u64_tape_k) {
        std::size_t failures = expect_column(mover, host.data.u64_tape.offsets, device.data.u64_tape.offsets,
                                             count + 1);
        std::size_t const bytes = host.data.u64_tape.offsets ? host.data.u64_tape.offsets[count] : 0;
        return failures + expect_column(mover, host.data.u64_tape.data, device.data.u64_tape.data, bytes);
    }
    std::size_t failures = expect_column(mover, host.data.u32_tape.offsets, device.data.u32_tape.offsets, count + 1);
    std::size_t const bytes = host.data.u32_tape.offsets ? host.data.u32_tape.offsets[count] : 0;
    return failures + expect_column(mover, host.data.u32_tape.data, device.data.u32_tape.data, bytes);
}

#pragma region Cases

/** What a listing verb answers: a scan from the first key, or a sample whose draw is random. */
enum class listing_kind_t : bool { scan_k, sample_k };

/** Whether the device serves a call, or refuses it as unsupported. */
enum class device_service_t : bool { unsupported_k, served_k };

std::size_t vram_listing(ustore_gpu_device_t listed) {
    std::size_t failures = expect(listed.vendor != ustore_gpu_vendor_unknown_k);
    failures += expect(listed.gpu_class != ustore_gpu_class_unknown_k);
    if (listed.vendor == ustore_gpu_vendor_amd_k && fake_hip_counters())
        failures += expect(listed.gpu_class == ustore_gpu_class_amd_cdna3_k);
    return failures;
}

std::size_t vram_arenas(std::size_t ordinal, config_t config) {
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "arenas"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    ustore_key_t const key = fixture.keys.front();
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {.database = fixture.database, .outcome = &outcome, .count = 1};
    read.collections = {&fixture.blobs, 0}, read.keys = {&key, 0}, read.values = &values;
    // A host arena on a VRAM executor and a device arena on the calling thread are both refused
    host_arena_t const host;
    read.executor = call.executor, read.arena = host.arena;
    ustore_blobs_read(&read);
    failures += expect(outcome.status == ustore_status_device_k);
    outcome = {};
    read.executor = nullptr, read.arena = call.arena;
    ustore_blobs_read(&read);
    failures += expect(outcome.status == ustore_status_device_k);
    outcome = {};
    read.executor = call.executor;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    return failures;
}

/** A read's outputs in a device arena export as Arrow columns living on that device, in place and
 *  with their offsets converted to the other width, counting the same bytes. */
std::size_t vram_exports(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    fixture_t fixture {config, "exports"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    ustore_key_t const key = fixture.keys.front();
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {.database = fixture.database, .executor = call.executor, .outcome = &outcome};
    read.count = 1, read.collections = {&fixture.blobs, 0}, read.keys = {&key, 0}, read.values = &values;
    read.arena = call.arena;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    if (failures) return failures;

    ArrowSchema schema {};
    ArrowDeviceArray array {};
    failures += expect(ustore_strings_view_to_arrow(values, 1, values.layout, call.arena, &schema, &array) ==
                       ustore_status_success_k);
    ArrowDeviceType const vendor = listed.vendor == ustore_gpu_vendor_amd_k ? ARROW_DEVICE_ROCM : ARROW_DEVICE_CUDA;
    failures += expect(array.device_type == vendor);
    failures += expect(array.device_id == static_cast<std::int64_t>(listed.vendor_ordinal));
    failures += expect(array.sync_event == nullptr);
    // The two offsets of the one value, of either width, fetched from the device
    auto const offsets_of = [&](ArrowDeviceArray const& exported, ustore_strings_layout_t layout) noexcept {
        if (layout == ustore_strings_u64_tape_k)
            return mover.fetch(static_cast<std::uint64_t const*>(exported.array.buffers[1]), 2);
        std::vector<std::uint32_t> const narrow = mover.fetch(
            static_cast<std::uint32_t const*>(exported.array.buffers[1]), 2);
        return std::vector<std::uint64_t> {narrow.begin(), narrow.end()};
    };
    std::vector<std::uint64_t> const in_place = offsets_of(array, values.layout);
    if (array.array.release) array.array.release(&array.array);
    if (schema.release) schema.release(&schema);

    ustore_strings_layout_t const other = values.layout == ustore_strings_u64_tape_k ? ustore_strings_u32_tape_k
                                                                                     : ustore_strings_u64_tape_k;
    failures += expect(ustore_strings_view_to_arrow(values, 1, other, call.arena, &schema, &array) ==
                       ustore_status_success_k);
    failures += expect(array.device_type == vendor);
    std::vector<std::uint64_t> const converted = offsets_of(array, other);
    failures += expect(mover.status().succeeded());
    failures += expect(converted[0] == 0 && converted[1] == in_place[1] - in_place[0]);
    if (array.array.release) array.array.release(&array.array);
    if (schema.release) schema.release(&schema);
    return failures;
}

/** A cursor opened on host threads walks there alone, so pulling it on a device is refused with
 *  @c ustore_status_device_k. */
std::size_t vram_cursors(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "cursors"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    ustore_blobs_cursor_t cursor = nullptr;
    ustore_blobs_cursor_init_t opened {.cursor = &cursor, .database = fixture.database, .outcome = &outcome};
    opened.collection = fixture.blobs, opened.start_key = fixture.keys.front();
    ustore_blobs_cursor_init(&opened);
    failures += expect_success(outcome);
    if (failures) return failures;
    ustore_key_t const* found = nullptr;
    ustore_size_t written = 0;
    ustore_blobs_cursor_next_t pull {.database = fixture.database, .executor = call.executor, .outcome = &outcome};
    pull.cursor = cursor, pull.keys = &found, pull.written = &written, pull.arena = call.arena, pull.wanted = 1;
    ustore_blobs_cursor_next(&pull);
    failures += expect(outcome.status == ustore_status_device_k);

    // A device cursor walks there, its keys and values landing in the device arena as a host pull's
    ustore_blobs_cursor_t on_device = nullptr;
    ustore_blobs_cursor_init_t device_opened = opened;
    device_opened.cursor = &on_device, device_opened.executor = call.executor;
    ustore_blobs_cursor_init(&device_opened);
    failures += expect_success(outcome);
    if (failures) return failures;
    ustore_key_t const *host_keys = nullptr, *device_keys = nullptr;
    ustore_strings_view_t host_values {}, device_values {};
    ustore_size_t host_written = 0, device_written = 0;
    host_arena_t const reference;
    ustore_blobs_cursor_next_t host_pull {.database = fixture.database, .outcome = &outcome};
    host_pull.cursor = cursor, host_pull.keys = &host_keys, host_pull.values = &host_values;
    host_pull.written = &host_written, host_pull.arena = reference.arena, host_pull.wanted = entries_k / 2;
    ustore_blobs_cursor_next(&host_pull);
    failures += expect_success(outcome);
    ustore_blobs_cursor_next_t device_pull = host_pull;
    device_pull.executor = call.executor, device_pull.cursor = on_device, device_pull.keys = &device_keys;
    device_pull.values = &device_values, device_pull.written = &device_written, device_pull.arena = call.arena;
    ustore_blobs_cursor_next(&device_pull);
    failures += expect_success(outcome);
    failures += expect_eq(device_written, host_written);
    failures += expect_column(mover, host_keys, device_keys, host_written);
    failures += expect_tape(mover, host_values, device_values, host_written);
    ustore_blobs_cursor_free(on_device);
    ustore_blobs_cursor_free(cursor);
    return failures;
}

std::size_t vram_reads(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "reads"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    std::size_t const count = entries_k + 4;
    std::vector<ustore_key_t> asked = fixture.keys;
    for (std::size_t extra = 0; extra != 4; ++extra) asked.push_back(key_from_integer(1000 + extra));
    ustore_keys_view_t const keys {asked.data(), sizeof(ustore_key_t)};
    host_arena_t const kept;

    {
        ustore_octet_t const *host_presences = nullptr, *device_presences = nullptr;
        ustore_length_t const *host_lengths = nullptr, *device_lengths = nullptr;
        ustore_strings_view_t host_values {}, device_values {};
        ustore_blobs_read_t arguments {.database = fixture.database, .count = count};
        arguments.collections = {&fixture.blobs, 0}, arguments.keys = keys;
        ustore_blobs_read_t host = arguments, device = arguments;
        host.presences = &host_presences, host.lengths = &host_lengths, host.values = &host_values;
        device.presences = &device_presences, device.lengths = &device_lengths, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host.outcome = &host_outcome, host.arena = kept.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_blobs_read(&host), ustore_blobs_read(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_column(mover, host_presences, device_presences, (count + 7) / 8);
        failures += expect_column(mover, host_lengths, device_lengths, count);
        failures += expect_tape(mover, host_values, device_values, count);
        // Every carve is padded to 64 bytes with its tail zeroed, as an Arrow validity bitmap reads it whole
        std::vector<ustore_octet_t> const padded = mover.fetch(device_presences, 64);
        bool tail_zeroed = true;
        for (std::size_t index = (count + 7) / 8; index != padded.size(); ++index) tail_zeroed &= padded[index] == 0;
        failures += expect(tail_zeroed);
    }
    // Every listing verb: one scan from the first key, and a sample, whose draw is random, compared by its offsets
    auto const listing = [&]<typename arguments_type_>(void (*verb)(arguments_type_*), ustore_collection_id_t& from,
                                                       listing_kind_t kind) {
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_key_t const *host_keys = nullptr, *device_keys = nullptr;
        arguments_type_ host {.database = fixture.database, .count = 1}, device = host;
        host.collections = device.collections = {&from, 0};
        host.wanted = device.wanted = entries_k / 2;
        host.offsets = &host_offsets, host.keys = &host_keys;
        device.offsets = &device_offsets, device.keys = &device_keys;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        verb(&host), verb(&device);
        std::size_t listed_failures = expect_success(host_outcome) + expect_success(device_outcome);
        listed_failures += expect_column(mover, host_offsets, device_offsets, 2);
        if (kind == listing_kind_t::scan_k && host_offsets)
            listed_failures += expect_column(mover, host_keys, device_keys, host_offsets[1]);
        return listed_failures;
    };
    failures += listing(&ustore_blobs_scan, fixture.blobs, listing_kind_t::scan_k);
    failures += listing(&ustore_blobs_sample, fixture.blobs, listing_kind_t::sample_k);
    failures += listing(&ustore_docs_scan, fixture.docs, listing_kind_t::scan_k);
    failures += listing(&ustore_docs_sample, fixture.docs, listing_kind_t::sample_k);
    failures += listing(&ustore_graph_scan_vertices, fixture.graph, listing_kind_t::scan_k);
    failures += listing(&ustore_pages_scan, fixture.pages, listing_kind_t::scan_k);

    {
        ustore_octet_t const *host_presences = nullptr, *device_presences = nullptr;
        ustore_key_t const *host_values = nullptr, *device_values = nullptr;
        ustore_strings_read_t host {.database = fixture.database, .count = entries_k}, device = host;
        host.collections = device.collections = {&fixture.strings, 0};
        host.strings = device.strings = fixture.texts.view();
        host.presences = &host_presences, host.values = &host_values;
        device.presences = &device_presences, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host.outcome = &host_outcome, host.arena = kept.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_strings_read(&host), ustore_strings_read(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_column(mover, host_presences, device_presences, (entries_k + 7) / 8);
        failures += expect_column(mover, host_values, device_values, entries_k);
    }
    {
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_strings_view_t host_strings {}, device_strings {};
        std::string_view const prefix = "text:1";
        tape_t prefixes;
        prefixes.push(prefix);
        ustore_strings_scan_t host {.database = fixture.database, .count = 1}, device = host;
        host.collections = device.collections = {&fixture.strings, 0};
        host.prefixes = device.prefixes = prefixes.view();
        host.wanted = device.wanted = entries_k;
        host.offsets = &host_offsets, host.strings = &host_strings;
        device.offsets = &device_offsets, device.strings = &device_strings;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_strings_scan(&host), ustore_strings_scan(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_column(mover, host_offsets, device_offsets, 2);
        if (host_offsets) failures += expect_tape(mover, host_strings, device_strings, host_offsets[1]);
    }
    // Whole documents and pages, then one field of every document
    auto const rendering = [&]<typename arguments_type_>(void (*verb)(arguments_type_*), ustore_collection_id_t& from,
                                                         ustore_strings_view_t fields) {
        ustore_octet_t const *host_presences = nullptr, *device_presences = nullptr;
        ustore_length_t const *host_lengths = nullptr, *device_lengths = nullptr;
        ustore_strings_view_t host_values {}, device_values {};
        arguments_type_ host {.database = fixture.database, .count = count}, device = host;
        host.collections = device.collections = {&from, 0};
        host.keys = device.keys = keys;
        if constexpr (requires { host.fields; }) host.fields = device.fields = fields;
        if constexpr (requires { host.outputs_encoding; })
            host.outputs_encoding = device.outputs_encoding = ustore_page_encoding_xml_k;
        host.presences = &host_presences, host.lengths = &host_lengths, host.values = &host_values;
        device.presences = &device_presences, device.lengths = &device_lengths, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        verb(&host), verb(&device);
        std::size_t rendered_failures = expect_success(host_outcome) + expect_success(device_outcome);
        rendered_failures += expect_column(mover, host_presences, device_presences, (count + 7) / 8);
        rendered_failures += expect_column(mover, host_lengths, device_lengths, count);
        return rendered_failures + expect_tape(mover, host_values, device_values, count);
    };
    tape_t names;
    for (std::size_t index = 0; index != count; ++index) names.push("name");
    failures += rendering(&ustore_docs_read, fixture.docs, {});
    failures += rendering(&ustore_docs_read, fixture.docs, names.view());
    failures += rendering(&ustore_pages_read, fixture.pages, {});
    {
        tape_t fields;
        fields.push("name"), fields.push("score"), fields.push("id");
        ustore_docs_field_type_t const types[3] = {ustore_docs_field_type_string_k, ustore_docs_field_type_f64_k,
                                                   ustore_docs_field_type_i64_k};
        std::size_t const widths[3] = {0, sizeof(double), sizeof(std::int64_t)};
        ustore_docs_column_view_t const *host_columns = nullptr, *device_columns = nullptr;
        ustore_docs_gather_t host {.database = fixture.database, .count = count}, device = host;
        host.collections = device.collections = {&fixture.docs, 0};
        host.keys = device.keys = keys;
        host.fields_count = device.fields_count = 3;
        host.fields = device.fields = fields.view();
        host.outputs_types = device.outputs_types = types;
        host.columns = &host_columns, device.columns = &device_columns;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host.outcome = &host_outcome, host.arena = kept.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_docs_gather(&host), ustore_docs_gather(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        if (host_columns && !expect(device_columns && mover.holds(device_columns))) {
            std::vector<ustore_docs_column_view_t> const columns = mover.fetch(device_columns, 3);
            for (std::size_t field = 0; field != 3; ++field) {
                ustore_docs_column_view_t const &on_host = host_columns[field], &on_device = columns[field];
                std::size_t const bitmap = (count + 7) / 8;
                failures += expect_column(mover, on_host.validities, on_device.validities, bitmap);
                failures += expect_column(mover, on_host.conversions, on_device.conversions, bitmap);
                failures += expect_column(mover, on_host.collisions, on_device.collisions, bitmap);
                failures += expect_column(mover, on_host.scalars, on_device.scalars, count * widths[field]);
                failures += expect_tape(mover, on_host.strings, on_device.strings, widths[field] ? 0 : count);
            }
        }
        else failures += expect(!host_columns);
    }
    {
        ustore_vertex_degree_t const *host_degrees = nullptr, *device_degrees = nullptr;
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_key_t const *host_edges = nullptr, *device_edges = nullptr;
        ustore_graph_read_edges_t host {.database = fixture.database, .count = count}, device = host;
        host.collections = device.collections = {&fixture.graph, 0};
        host.vertices = device.vertices = keys;
        host.degrees_per_vertex = &host_degrees, host.edges_offsets = &host_offsets,
        host.edges_per_vertex = &host_edges;
        device.degrees_per_vertex = &device_degrees, device.edges_offsets = &device_offsets;
        device.edges_per_vertex = &device_edges;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host.outcome = &host_outcome, host.arena = kept.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_graph_read_edges(&host), ustore_graph_read_edges(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_column(mover, host_degrees, device_degrees, count);
        failures += expect_column(mover, host_offsets, device_offsets, count + 1);
        if (host_offsets) failures += expect_column(mover, host_edges, device_edges, 3 * host_offsets[count]);
    }
    return failures;
}

std::size_t vram_staging(std::size_t ordinal, ustore_gpu_device_t listed, std::vector<ustore_gpu_device_t> others,
                         config_t config) {
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "staging"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    host_arena_t const kept;

    // Keys and a tape of strings on the executor's own device are downloaded once and read as the host's
    ustore_key_t const* const placed_keys = mover.place(std::span<ustore_key_t const> {fixture.keys});
    ustore_byte_t const* const placed_bytes = mover.place(std::span<ustore_byte_t const> {
        reinterpret_cast<ustore_byte_t const*>(fixture.texts.bytes.data()), fixture.texts.bytes.size()});
    std::uint32_t const* const placed_offsets = mover.place(std::span<std::uint32_t const> {fixture.texts.offsets});
    if (expect(mover.status().succeeded())) return failures + 1;
    {
        ustore_strings_view_t host_values {}, device_values {};
        ustore_blobs_read_t host {.database = fixture.database, .count = entries_k}, device = host;
        host.collections = device.collections = {&fixture.blobs, 0};
        host.keys = {fixture.keys.data(), sizeof(ustore_key_t)};
        device.keys = {placed_keys, sizeof(ustore_key_t)};
        host.values = &host_values, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host.outcome = &host_outcome, host.arena = kept.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_blobs_read(&host), ustore_blobs_read(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_tape(mover, host_values, device_values, entries_k);
    }
    {
        ustore_key_t const *host_values = nullptr, *device_values = nullptr;
        ustore_strings_read_t host {.database = fixture.database, .count = entries_k}, device = host;
        host.collections = device.collections = {&fixture.strings, 0};
        host.strings = fixture.texts.view();
        device.strings = u32_tape(placed_bytes, placed_offsets);
        host.values = &host_values, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_strings_read(&host), ustore_strings_read(&device);
        failures += expect_success(host_outcome) + expect_success(device_outcome);
        failures += expect_column(mover, host_values, device_values, entries_k);
    }
    // Keys on another vendor's device are refused, never read as host memory
    for (ustore_gpu_device_t const& other : others) {
        if (other.vendor == listed.vendor) continue;
        mover_t foreign;
        if (expect(foreign.open(other).succeeded())) return failures + 1;
        ustore_key_t const* const foreign_keys = foreign.place(std::span<ustore_key_t const> {fixture.keys});
        if (expect(foreign.status().succeeded())) return failures + 1;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read {.database = fixture.database,
                                  .executor = call.executor,
                                  .outcome = &outcome,
                                  .count = entries_k};
        read.collections = {&fixture.blobs, 0}, read.keys = {foreign_keys, sizeof(ustore_key_t)};
        read.values = &values, read.arena = call.arena;
        ustore_blobs_read(&read);
        failures += expect(outcome.status == ustore_status_device_k);
        failures += expect(values.data.u32_tape.offsets == nullptr);
    }
    return failures;
}

/** What the HIP stand-in counted over one call. */
fake_hip_counters_t counted_since(fake_hip_counters_t const& before, fake_hip_counters_t const& after) noexcept {
    return {after.allocations - before.allocations,
            after.frees - before.frees,
            after.managed_allocations - before.managed_allocations,
            after.fills - before.fills,
            after.uploads - before.uploads,
            after.downloads - before.downloads,
            after.registrations - before.registrations,
            after.unregistrations - before.unregistrations,
            after.waits - before.waits,
            after.pointer_queries - before.pointer_queries};
}

void print_counted(char const* label, fake_hip_counters_t const& counted) {
    fmt::println("  {}: {} uploads, {} downloads, {} fills, {} waits, {} allocations, {} frees, {} pins, {} queries",
                 label, counted.uploads, counted.downloads, counted.fills, counted.waits, counted.allocations,
                 counted.frees, counted.registrations, counted.pointer_queries);
}

case_result_t vram_warm_calls(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    // Without the stand-in the calls still run, one cold and three warm, for a driver trace to count
    fake_hip_counters_t const* const counters = listed.vendor == ustore_gpu_vendor_amd_k ? fake_hip_counters()
                                                                                         : nullptr;
    fake_hip_counters_t const unused {};
    fake_hip_counters_t const& counted = counters ? *counters : unused;
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "warm_calls"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    ustore_octet_t const* presences = nullptr;
    ustore_length_t const* lengths = nullptr;
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {.database = fixture.database,
                              .executor = call.executor,
                              .outcome = &outcome,
                              .count = entries_k};
    read.collections = {&fixture.blobs, 0}, read.keys = {fixture.keys.data(), sizeof(ustore_key_t)};
    read.presences = &presences, read.lengths = &lengths, read.values = &values, read.arena = call.arena;
    fake_hip_counters_t const before_cold = counted;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    fake_hip_counters_t const cold = counted_since(before_cold, counted);
    for (std::size_t repeat = 0; repeat != 3; ++repeat) {
        fake_hip_counters_t const before_warm = counted;
        ustore_blobs_read(&read);
        failures += expect_success(outcome);
        fake_hip_counters_t const warm = counted_since(before_warm, counted);
        if (!counters) continue;
        if (repeat == 0) print_counted("cold blobs.read", cold), print_counted("warm blobs.read", warm);
        failures += expect(warm.uploads == 4) + expect(warm.waits == 1) + expect(warm.allocations == 0);
        failures += expect(warm.registrations == 0) + expect(warm.frees == 0) + expect(warm.downloads == 0);
    }
    if (!counters) return case_result_t::skipped("counting needs the HIP stand-in or a driver trace", failures);
    failures += expect(cold.uploads == 4) + expect(cold.waits == 1) + expect(cold.allocations >= 1);
    return failures;
}

case_result_t vram_frees_everything(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    if (listed.vendor != ustore_gpu_vendor_amd_k || !fake_hip_counters())
        return case_result_t::skipped("counting live ranges needs the HIP stand-in");
    using live_ranges_t = std::size_t (*)();
    live_ranges_t const live = reinterpret_cast<live_ranges_t>(
        dlsym(dlopen("libamdhip64.so", RTLD_NOW | RTLD_NOLOAD), "fake_hip_live_ranges"));
    std::size_t const before = live();
    {
        device_call_t call {ordinal};
        if (expect(call.status.succeeded())) return 1;
        fixture_t fixture {config, "frees_everything"};
        if (fixture.failures) return fixture.failures;
        // A request past the first run grows the arena, and the next call frees the older run in stream order
        for (std::size_t width : {std::size_t {1}, entries_k}) {
            ustore_strings_view_t values {};
            ustore_outcome_t outcome {};
            ustore_blobs_read_t read {.database = fixture.database,
                                      .executor = call.executor,
                                      .outcome = &outcome,
                                      .count = width};
            read.collections = {&fixture.blobs, 0}, read.keys = {fixture.keys.data(), sizeof(ustore_key_t)};
            read.values = &values, read.arena = call.arena;
            ustore_blobs_read(&read);
            if (expect_success(outcome)) return 1;
        }
    }
    return expect(live() == before);
}

/** Admits the keys spelling even integers, at either predicate cost. */
void admit_even_keys(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count, void*) {
    for (std::size_t index = 0; index != count; ++index)
        admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
}

case_result_t vram_matching(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    device_call_t call {ordinal};
    if (expect(call.status.succeeded())) return 1;
    fixture_t fixture {config, "matching"};
    if (fixture.failures) return fixture.failures;
    std::size_t failures = 0;
    // Every device matches values through the library's kernels, but the stand-in HIP driver, which
    // loads no bundle
    device_service_t const matches_values = listed.vendor == ustore_gpu_vendor_nvidia_k || !fake_hip_counters()
                                                ? device_service_t::served_k
                                                : device_service_t::unsupported_k;
    ustore_predicate_t const even_cheap {.callback = &admit_even_keys, .cost = ustore_predicate_cheap_k};
    ustore_predicate_t const even_expensive {.callback = &admit_even_keys, .cost = ustore_predicate_expensive_k};

    // Every filtered listing wants a quarter of the collection, so a sparse filter pages through it several times
    auto const filtered = [&]<typename arguments_type_, typename narrow_type_>(
                              void (*verb)(arguments_type_*), ustore_collection_id_t& from, narrow_type_ const& narrow,
                              device_service_t service, std::size_t least, std::size_t most) {
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_key_t const *host_keys = nullptr, *device_keys = nullptr;
        float const *host_scores = nullptr, *device_scores = nullptr;
        arguments_type_ host {.database = fixture.database, .count = 1};
        narrow(host);
        arguments_type_ device = host;
        host.collections = device.collections = {&from, 0};
        host.wanted = device.wanted = entries_k / 4;
        host.offsets = &host_offsets, host.keys = &host_keys;
        device.offsets = &device_offsets, device.keys = &device_keys;
        if constexpr (requires { host.scores; })
            if (host.patterns.weights) host.scores = &host_scores, device.scores = &device_scores;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        verb(&host), verb(&device);
        std::size_t matched = expect_success(host_outcome) + expect(host_offsets != nullptr);
        if (matched) return matched;
        matched += expect(host_offsets[1] >= least && host_offsets[1] <= most);
        if (service == device_service_t::unsupported_k)
            return matched + expect(device_outcome.status == ustore_status_unsupported_k);
        matched += expect_success(device_outcome) + expect_column(mover, host_offsets, device_offsets, 2);
        matched += expect_column(mover, host_keys, device_keys, host_offsets[1]);
        return matched + expect_column(mover, host_scores, device_scores, host_offsets[1]);
    };
    std::array<std::uint32_t, 2> pattern_offsets {};
    auto const patterns_of = [&](std::string_view text, ustore_pattern_kind_t kind) {
        pattern_offsets = {0, static_cast<std::uint32_t>(text.size())};
        return ustore_patterns_t {.patterns = u32_tape(text.data(), pattern_offsets.data()), .count = 1, .kind = kind};
    };
    std::size_t const wanted = entries_k / 4;
    float const weight = 2.0f;

    // Values of three `v` before their number, one in seven
    failures += filtered(
        &ustore_blobs_scan, fixture.blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of("^vvv[0-9]", ustore_pattern_regex_k); },
        matches_values, 1, wanted - 1);
    // Three or more `v` from a start key on the device, which staging moves into the bounce every page rewinds
    ustore_key_t const* const placed_start = mover.place(
        std::span<ustore_key_t const> {&fixture.keys[entries_k / 2], 1});
    if (expect(mover.status().succeeded())) return failures + 1;
    {
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_key_t const *host_keys = nullptr, *device_keys = nullptr;
        ustore_blobs_scan_t host {.database = fixture.database, .count = 1};
        host.patterns = patterns_of("vvv", ustore_pattern_substrings_k);
        ustore_blobs_scan_t device = host;
        host.collections = device.collections = {&fixture.blobs, 0};
        host.start_keys = {&fixture.keys[entries_k / 2], 0}, device.start_keys = {placed_start, 0};
        host.wanted = device.wanted = entries_k / 4;
        host.offsets = &host_offsets, host.keys = &host_keys;
        device.offsets = &device_offsets, device.keys = &device_keys;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_blobs_scan(&host), ustore_blobs_scan(&device);
        failures += expect_success(host_outcome) + expect(host_offsets && host_offsets[1] > 0);
        if (matches_values == device_service_t::unsupported_k)
            failures += expect(device_outcome.status == ustore_status_unsupported_k);
        else if (host_offsets) {
            failures += expect_success(device_outcome) + expect_column(mover, host_offsets, device_offsets, 2);
            failures += expect_column(mover, host_keys, device_keys, host_offsets[1]);
        }
    }
    // Five or six `v`, scored, and only even keys among them, asked before the values are read
    failures += filtered(
        &ustore_blobs_scan, fixture.blobs,
        [&](ustore_blobs_scan_t& scan) {
            scan.patterns = patterns_of("vvvvv", ustore_pattern_substrings_k);
            scan.patterns.weights = &weight;
            scan.predicate = even_cheap;
        },
        matches_values, 1, wanted - 1);
    // A predicate alone reads no values, so every vendor serves it
    failures += filtered(
        &ustore_blobs_scan, fixture.blobs, [&](ustore_blobs_scan_t& scan) { scan.predicate = even_expensive; },
        device_service_t::served_k, wanted, wanted);
    failures += filtered(
        &ustore_graph_scan_vertices, fixture.graph,
        [&](ustore_graph_scan_vertices_t& scan) { scan.predicate = even_cheap; }, device_service_t::served_k, wanted,
        wanted);
    // Documents with a member past 40, their even keys asked after the values matched, and a path none holds
    failures += filtered(
        &ustore_docs_scan, fixture.docs,
        [&](ustore_docs_scan_t& scan) {
            scan.filter = bytes_of("$[?@ > 40]");
            scan.predicate = even_expensive;
        },
        matches_values, wanted, wanted);
    failures += filtered(
        &ustore_docs_scan, fixture.docs, [&](ustore_docs_scan_t& scan) { scan.filter = bytes_of("$.missing"); },
        matches_values, 0, 0);
    // A filter the kernels do not walk matches on the host, so every device serving
    // matches serves it
    failures += filtered(
        &ustore_docs_scan, fixture.docs, [&](ustore_docs_scan_t& scan) { scan.filter = bytes_of("?@.score > 40"); },
        matches_values, wanted, wanted);
    // Every page holds a bold element, and none an italic one
    failures += filtered(
        &ustore_pages_scan, fixture.pages, [&](ustore_pages_scan_t& scan) { scan.filter = bytes_of("//b"); },
        matches_values, wanted, wanted);
    failures += filtered(
        &ustore_pages_scan, fixture.pages, [&](ustore_pages_scan_t& scan) { scan.filter = bytes_of("//i"); },
        matches_values, 0, 0);

    // Strings holding a 7, whose keys the predicate sees
    {
        ustore_length_t const *host_offsets = nullptr, *device_offsets = nullptr;
        ustore_strings_view_t host_strings {}, device_strings {};
        ustore_strings_scan_t host {.database = fixture.database, .count = 1}, device = host;
        host.collections = device.collections = {&fixture.strings, 0};
        host.patterns = device.patterns = patterns_of("7", ustore_pattern_substrings_k);
        host.predicate = device.predicate = even_expensive;
        host.wanted = device.wanted = entries_k;
        host.offsets = &host_offsets, host.strings = &host_strings;
        device.offsets = &device_offsets, device.strings = &device_strings;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_strings_scan(&host), ustore_strings_scan(&device);
        failures += expect_success(host_outcome) + expect(host_offsets && host_offsets[1] > 0);
        if (matches_values == device_service_t::unsupported_k)
            failures += expect(device_outcome.status == ustore_status_unsupported_k);
        else if (host_offsets) {
            failures += expect_success(device_outcome) + expect_column(mover, host_offsets, device_offsets, 2);
            failures += expect_tape(mover, host_strings, device_strings, host_offsets[1]);
        }
    }
    return failures;
}

/** A tape the device assembles from strings it holds lays out byte for byte as the
 *  host's @c assemble_tape lays it, across several blocks of the offsets scan and
 *  segments of the gather, empty strings among them. */
case_result_t vram_tapes(ustore_gpu_device_t listed) {
    mover_t mover;
    if (expect(mover.open(listed).succeeded())) return 1;
    if (!mover.launches()) return case_result_t::skipped("the library embeds no kernels for the device");
    std::size_t failures = 0;
    std::vector<std::string> strings;
    for (std::size_t index = 0; index != 3 * tape_block_threads_k + 5; ++index)
        strings.push_back(std::string(index % 11, static_cast<char>('a' + index % 26)));
    std::string joined;
    for (std::string const& string : strings) joined += string;
    char const* const placed = mover.place(std::span<char const> {joined});
    std::vector<tape_source_t> host_sources, device_sources;
    std::size_t offset = 0;
    for (std::string const& string : strings) {
        host_sources.push_back({reinterpret_cast<std::byte const*>(string.data()), string.size()});
        device_sources.push_back({reinterpret_cast<std::byte const*>(placed + offset), string.size()});
        offset += string.size();
    }

    std::vector<std::uint32_t> host_offsets(strings.size() + 1);
    std::vector<std::byte> host_bytes(joined.size());
    assemble_tape<std::uint32_t>(
        strings.size(), [&](std::size_t index) noexcept { return host_sources[index]; },
        std::span<std::uint32_t> {host_offsets}, std::span<std::byte> {host_bytes});

    // The outputs start poisoned, so every byte the device leaves unwritten shows
    std::vector<std::uint32_t> const poisoned_offsets(host_offsets.size(), ~std::uint32_t {0});
    std::vector<std::byte> const poisoned_bytes(host_bytes.size(), std::byte {0xFF});
    tape_source_t const* const sources = mover.place(std::span<tape_source_t const> {device_sources});
    std::uint32_t* const offsets = mover.place(std::span {poisoned_offsets});
    std::byte* const bytes = mover.place(std::span {poisoned_bytes});
    mover.copy([&](auto& executor) noexcept {
        return assemble_tape<std::uint32_t>(executor, std::span<tape_source_t const> {sources, strings.size()},
                                            std::span<std::uint32_t> {offsets, host_offsets.size()},
                                            std::span<std::byte> {bytes, host_bytes.size()});
    });
    failures += expect(mover.status().succeeded());
    failures += expect(mover.fetch(offsets, host_offsets.size()) == host_offsets);
    failures += expect(mover.fetch(bytes, host_bytes.size()) == host_bytes);
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    auto const maybe_test_directory = directory_t::make(options.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    config_t const config {options.engine, maybe_test_directory->path()};

    std::vector<ustore_gpu_device_t> const devices = listed_devices();
    if (devices.empty()) report(totals, "vram.devices", case_result_t::skipped("no GPU is listed"));
    for (std::size_t ordinal = 0; ordinal != devices.size(); ++ordinal) {
        ustore_gpu_device_t const& listed = devices[ordinal];
        fmt::println("device {}: {}, vendor ordinal {}", ordinal, listed.name, listed.vendor_ordinal);
        std::string const prefix = fmt::format("vram.{}{}.", ustore_gpu_vendor_name(listed.vendor), ordinal);
        report(totals, (prefix + "listing").c_str(), vram_listing, listed);
        report(totals, (prefix + "arenas").c_str(), vram_arenas, ordinal, config);
        report(totals, (prefix + "cursors").c_str(), vram_cursors, ordinal, listed, config);
        report(totals, (prefix + "exports").c_str(), vram_exports, ordinal, listed, config);
        report(totals, (prefix + "reads").c_str(), vram_reads, ordinal, listed, config);
        report(totals, (prefix + "staging").c_str(), vram_staging, ordinal, listed, devices, config);
        report(totals, (prefix + "warm_calls").c_str(), vram_warm_calls, ordinal, listed, config);
        report(totals, (prefix + "frees_everything").c_str(), vram_frees_everything, ordinal, listed, config);
        report(totals, (prefix + "matching").c_str(), vram_matching, ordinal, listed, config);
        // A build embedding no kernels for the vendor, like the one the HIP stand-in runs under,
        // lists no tape case
        bool const embeds = listed.vendor == ustore_gpu_vendor_nvidia_k ? !vram_nvidia_bundle().empty()
                                                                        : !vram_amd_bundle().empty();
        if (embeds) report(totals, (prefix + "tapes").c_str(), vram_tapes, listed);
    }
    return suite_exit_code(totals);
}
