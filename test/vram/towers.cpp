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

#include <cstddef> // `std::size_t`, `std::byte`, `std::to_integer`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`
#include <cstring> // `std::memcmp`

#include <algorithm>       // `std::find_if`
#include <array>           // `std::array`
#include <span>            // `std::span`, `std::as_bytes`, `std::as_writable_bytes`
#include <string>          // `std::string`, `std::to_string`
#include <string_view>     // `std::string_view`
#include <source_location> // `std::source_location`
#include <variant>         // `std::variant`, `std::visit`
#include <vector>          // `std::vector`

#include <fmt/format.h>

#include <forkunion/topology.hpp> // `fu::allowed_cores_count`

#include "ustore/ustore.h"
#include "ustore/utilities/blocking_files.hpp" // `shared_library_t`
#include "ustore/utilities/vram_device.hpp"    // `gpu_drivers_t`, `assemble_tape`

#include "harness.hpp"      // `run_test_case`, `expect_true`, `expect_success`, `scratch_directory_t`
#include "vram/classes.hpp" // `gpu_class_bundle`

using namespace unum::ustore;
using namespace unum::ustore::test;

namespace unum::ustore::test {

/** Entries every collection holds, enough for a tape and a bitmap to span several bytes and blocks. */
constexpr std::size_t entries_k = 96;

/** The reason a case counting what a call costs reports without the HIP stand-in. */
constexpr char const* needs_hip_stand_in_k = "needs the HIP stand-in";

/** The reason a device of a vendor this build compiled no class for reports. */
constexpr char const* needs_gpu_class_k = "needs a GPU class this build compiled for the device";

/** A tape of strings the suite owns, written as the C ABI reads one. */
struct tape_t {
    std::string bytes;
    std::vector<std::uint32_t> offsets {0};

    void push(std::string_view text) {
        bytes.append(text);
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
    }
    std::size_t size() const noexcept { return offsets.size() - 1; }
    ustore_strings_view_t view() const noexcept { return util::u32_tape(bytes.data(), offsets.data()); }
};

/** What the HIP stand-in counted, read through the library the runtime opened. */
struct fake_hip_counters_t {
    std::size_t allocations, frees, managed_allocations, fills, uploads, downloads, registrations, unregistrations,
        waits, pointer_queries, stream_creations, stream_destructions, callbacks;
};

/** The stand-in's counters, or null when the real runtime or none is loaded. */
fake_hip_counters_t const* fake_hip_counters() noexcept {
    fake_hip_counters_t const* counters = nullptr;
    util::shared_library_t::open("libamdhip64.so").resolve("fake_hip_counters", counters);
    return counters;
}

/** Hands @p body the one device of the executor a mover holds, of any vendor, and nothing for
 *  none. */
template <typename body_type_>
struct device_visitor {
    body_type_& body;
    void operator()(std::monostate) const noexcept {}
    void operator()(util::cuda_executor_t& executor) const noexcept { body(executor.stream(0)); }
    void operator()(util::hip_executor_t& executor) const noexcept { body(executor.stream(0)); }
    void operator()(util::metal_executor_t& executor) const noexcept { body(executor.stream(0)); }
};

/** The suite's own executor on one listed device, which places inputs there and fetches outputs back. */
class mover_t {
  public:
    util::status_t open(ustore_gpu_device_t const& listed) noexcept {
        util::gpu_device_t device;
        device.vendor = static_cast<util::gpu_vendor_t>(listed.vendor);
        device.vendor_ordinal = listed.vendor_ordinal;
        // The mover only moves bytes, so its streams launch no kernels
        auto no_kernels = []<typename module_type_>(std::type_identity<module_type_>, int) noexcept {
            return static_cast<util::kernel_table<module_type_> const*>(nullptr);
        };
        std::size_t no_ceiling = 0;
        auto [spawned, spawn_status] = util::spawn_gpu_executor(drivers_, {&device, 1}, {}, no_kernels, no_ceiling);
        if (spawn_status.succeeded()) executor_ = std::move(spawned);
        return spawn_status;
    }

    ~mover_t() noexcept {
        visit([&](auto& executor) noexcept {
            if (executor.bind().failed()) return;
            for (std::uintptr_t address : placed_) executor.free_async(address);
            [[maybe_unused]] util::status_t const waited = executor.wait();
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
            auto [allocated, allocate_status] = executor.allocate_async(host.size_bytes());
            if (allocate_status.failed()) return allocate_status;
            address = allocated;
            placed_.push_back(address);
            return executor.upload_async(address, std::as_bytes(host));
        });
        return reinterpret_cast<element_type_*>(address);
    }

    /** Names @p arena, which the calls under test write into, and the UStore @p ordinal of the
     *  device it must describe, as every output check reads it. */
    void lands_in(ustore_arena_t arena, std::size_t ordinal) noexcept {
        outputs_arena_ = arena;
        outputs_device_ = static_cast<std::int32_t>(ordinal);
    }

    /** Whether the arena the calls write into describes as the expected device's memory. */
    bool outputs_on_device() noexcept {
        ustore_arena_description_t description {};
        ustore_outcome_t outcome {};
        ustore_arena_describe_t describe {.arena = outputs_arena_, .outcome = &outcome, .description = &description};
        ustore_arena_describe(&describe);
        return outcome.status == ustore_status_success_k && description.residency == ustore_residency_vram_k &&
               description.device == outputs_device_;
    }

    /** The first failure of a copy, sticky until the mover dies. */
    util::status_t status() const noexcept { return status_; }

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
        visit([&](auto& executor) noexcept { loaded = executor.kernels != nullptr; });
        return loaded;
    }

  private:
    template <typename body_type_>
    void visit(body_type_&& body) noexcept {
        std::visit(device_visitor<body_type_> {body}, executor_);
    }

    util::gpu_drivers_t drivers_;
    ustore_arena_t outputs_arena_ = nullptr;
    std::int32_t outputs_device_ = -1;
    util::gpu_executor_t executor_;
    std::vector<std::uintptr_t> placed_;
    util::status_t status_;
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

/** A VRAM executor on the UStore @p ordinals and an arena on the first, freed together. */
struct device_call_t {
    ustore_executor_t executor = nullptr;
    ustore_arena_t arena = nullptr;
    util::status_t status;

    explicit device_call_t(std::span<std::int32_t const> ordinals) noexcept {
        ustore_outcome_t outcome {};
        ustore_executor_for_vram_t spawn {.executor = &executor,
                                          .outcome = &outcome,
                                          .vram = {.devices = ordinals.data(), .devices_count = ordinals.size()}};
        ustore_executor_for_vram(&spawn);
        if ((status = status_of(outcome)).failed()) return;
        ustore_arena_init_t init {.arena = &arena, .executor = executor, .outcome = &outcome};
        ustore_arena_init(&init);
        status = status_of(outcome);
    }
    explicit device_call_t(std::size_t ordinal) noexcept
        : device_call_t(std::array {static_cast<std::int32_t>(ordinal)}) {}
    ~device_call_t() noexcept {
        ustore_arena_free(arena);
        expect_true(ustore_executor_free(executor) == ustore_status_success_k);
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

    /** Whether the database opened and every write landed, so a case may read what it holds. */
    bool filled = true;

    /** Fills a fresh database of @p config's engine, persisted under @p name inside its directory when it has one. */
    fixture_t(config_t const& config, char const* name) noexcept {
        std::string const subdirectory = fmt::format("{}/{}", config.directory() ? config.directory() : "", name);
        config_t const fresh {config.options(), config.directory() ? subdirectory.c_str() : nullptr};
        if (config.directory()) filled = expect_true(scratch_directory_t::recreate(subdirectory.c_str()));
        if (!filled) return;
        auto [opened, open_status] = open_raw(fresh);
        filled = expect_true(open_status.succeeded());
        if (!filled) return;
        database = opened;
        for (std::size_t index = 0; index != entries_k; ++index) {
            keys.push_back(util::key_from_integer(index + 1));
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
        filled = expect_success(outcome) && filled;
        ustore_strings_write_t strings_write {.database = database, .outcome = &outcome, .count = entries_k};
        strings_write.collections = {&strings, 0}, strings_write.strings = texts.view(), strings_write.values = strided;
        ustore_strings_write(&strings_write);
        filled = expect_success(outcome) && filled;
        ustore_docs_write_t docs_write {.database = database, .outcome = &outcome, .count = entries_k};
        docs_write.collections = {&docs, 0}, docs_write.keys = strided, docs_write.values = documents.view();
        ustore_docs_write(&docs_write);
        filled = expect_success(outcome) && filled;
        // Edge `i` joins vertex `i` to vertex `i + 1`, so every vertex but the ends has degree two
        std::vector<ustore_key_t> targets(keys.begin() + 1, keys.end());
        targets.push_back(keys.front());
        ustore_graph_write_edges_t edges_write {.database = database, .outcome = &outcome, .count = entries_k};
        edges_write.collections = {&graph, 0}, edges_write.edges = strided, edges_write.sources = strided;
        edges_write.targets = {targets.data(), sizeof(ustore_key_t)};
        ustore_graph_write_edges(&edges_write);
        filled = expect_success(outcome) && filled;
        ustore_pages_import_t pages_import {.database = database, .outcome = &outcome, .count = entries_k};
        pages_import.collections = {&pages, 0}, pages_import.keys = strided, pages_import.values = renderings.view();
        pages_import.encoding = ustore_page_encoding_xml_k;
        ustore_pages_import(&pages_import);
        filled = expect_success(outcome) && filled;
    }
    ~fixture_t() noexcept { ustore_database_free(database); }

    ustore_collection_id_t create(std::string_view name, ustore_modality_t modality) noexcept {
        ustore_collection_id_t created {};
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {.database = database,
                                           .outcome = &outcome,
                                           .name = util::bytes_of(name),
                                           .modality = modality,
                                           .collection = &created};
        ustore_collection_create(&create);
        filled = expect_success(outcome) && filled;
        return created;
    }
};

/** Holds when the @p count elements at device @p device equal those at host @p host, and the arena
 *  @p mover names describes as its device; otherwise fails a check at the caller's line. */
template <typename element_type_>
bool expect_column(mover_t& mover, element_type_ const* host, element_type_ const* device, std::size_t count,
                   std::source_location const where = std::source_location::current()) {
    if (!count || (!host && !device)) return true;
    located_format<> one_sided {"one side has no column"};
    one_sided.location = where; // a consteval constructor cannot take the caller's runtime location
    if (!expect_true(host && device, one_sided)) return false;
    std::vector<element_type_> const fetched = mover.fetch(device, count);
    std::span<std::byte const> const fetched_bytes = std::as_bytes(std::span<element_type_ const> {fetched});
    std::span<std::byte const> const host_bytes = std::as_bytes(std::span<element_type_ const> {host, count});
    std::size_t differing = 0;
    while (differing != host_bytes.size() && fetched_bytes[differing] == host_bytes[differing]) ++differing;
    bool const same = differing == host_bytes.size();
    unsigned const device_byte = same ? 0 : std::to_integer<unsigned>(fetched_bytes[differing]);
    unsigned const host_byte = same ? 0 : std::to_integer<unsigned>(host_bytes[differing]);
    bool const on_device = expect_true(mover.outputs_on_device(), where);
    bool const fetched_whole = expect_true(mover.status().succeeded(), where);
    located_format<std::size_t, std::size_t, unsigned, unsigned> unequal {
        "byte {} of {} is {:#x} on the device, {:#x} on the host"};
    unequal.location = where;
    bool const equal = expect_true(same, unequal, differing, host_bytes.size(), device_byte, host_byte);
    return on_device && fetched_whole && equal;
}

/** Holds when the device tape of @p count strings equals the host one; otherwise fails a check. */
bool expect_tape(mover_t& mover, ustore_strings_view_t const& host, ustore_strings_view_t const& device,
                 std::size_t count) {
    if (!count) return true;
    if (!expect_true(host.layout == device.layout)) return false;
    if (host.layout == ustore_strings_u64_tape_k) {
        bool const offsets = expect_column(mover, host.data.u64_tape.offsets, device.data.u64_tape.offsets, count + 1);
        std::size_t const bytes = host.data.u64_tape.offsets ? host.data.u64_tape.offsets[count] : 0;
        return expect_column(mover, host.data.u64_tape.data, device.data.u64_tape.data, bytes) && offsets;
    }
    bool const offsets = expect_column(mover, host.data.u32_tape.offsets, device.data.u32_tape.offsets, count + 1);
    std::size_t const bytes = host.data.u32_tape.offsets ? host.data.u32_tape.offsets[count] : 0;
    return expect_column(mover, host.data.u32_tape.data, device.data.u32_tape.data, bytes) && offsets;
}

#pragma region Cases

/** What a listing verb answers: a scan from the first key, or a sample whose draw is random. */
enum class listing_kind_t : bool { scan_k, sample_k };

/** Whether the device serves a call, or refuses it as unsupported. */
enum class device_service_t : bool { unsupported_k, served_k };

void test_vram_listing(ustore_gpu_device_t listed) {
    expect_true(listed.vendor != ustore_gpu_vendor_unknown_k);
    expect_true(listed.gpu_class != ustore_gpu_class_unknown_k);
    if (listed.vendor == ustore_gpu_vendor_amd_k && fake_hip_counters())
        expect_true(listed.gpu_class == ustore_gpu_class_amd_cdna3_k);
}

/** The GPU class ceiling caps the class the device loads: its own class serves, one of the other
 *  vendor's ladder leaves it nothing to load and is unsupported, and a value no class takes is
 *  refused as an invalid argument before either. */
void test_vram_class_ceiling(std::size_t ordinal, ustore_gpu_device_t listed) {
    auto spawned_under = [ordinal](ustore_gpu_class_t gpu_class_ceiling) noexcept {
        ustore_outcome_t outcome {};
        ustore_executor_t executor = nullptr;
        std::int32_t const device = static_cast<std::int32_t>(ordinal);
        ustore_executor_for_vram_t spawn {
            .executor = &executor,
            .outcome = &outcome,
            .vram = {.devices = &device, .devices_count = 1, .gpu_class_ceiling = gpu_class_ceiling}};
        ustore_executor_for_vram(&spawn);
        expect_true(ustore_executor_free(executor) == ustore_status_success_k);
        return outcome.status;
    };
    expect_true(spawned_under(listed.gpu_class) == ustore_status_success_k);
    ustore_gpu_class_t const foreign = listed.vendor == ustore_gpu_vendor_amd_k ? ustore_gpu_class_nvidia_blackwell_k
                                                                                : ustore_gpu_class_amd_cdna5_k;
    expect_true(spawned_under(foreign) == ustore_status_unsupported_k);
    expect_true(spawned_under(static_cast<ustore_gpu_class_t>(6)) == ustore_status_invalid_argument_k);
}

void test_vram_arenas(std::size_t ordinal, config_t config) {
    // A VRAM executor runs on its creator, so neither making nor freeing it pins that thread
    std::size_t const cpus = fu::allowed_cores_count();
    {
        device_call_t const made {ordinal};
        expect_true(made.status.succeeded() && fu::allowed_cores_count() == cpus);
    }
    expect_true(fu::allowed_cores_count() == cpus);
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return;
    fixture_t fixture {config, "arenas"};
    if (!fixture.filled) return;
    ustore_key_t const key = fixture.keys.front();
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {.database = fixture.database, .outcome = &outcome, .count = 1};
    read.collections = {&fixture.blobs, 0}, read.keys = {&key, 0}, read.values = &values;
    // A host arena on a VRAM executor and a device arena on the calling thread are both refused
    host_arena_t const host;
    read.executor = call.executor, read.arena = host.arena;
    ustore_blobs_read(&read);
    expect_true(outcome.status == ustore_status_device_k);
    outcome = {};
    read.executor = nullptr, read.arena = call.arena;
    ustore_blobs_read(&read);
    expect_true(outcome.status == ustore_status_device_k);
    outcome = {};
    read.executor = call.executor;
    ustore_blobs_read(&read);
    expect_success(outcome);
}

/** A read's outputs in a device arena export as Arrow columns living on that device, in place and
 *  with their offsets converted to the other width, counting the same bytes. */
void test_vram_exports(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return;
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return;
    fixture_t fixture {config, "exports"};
    if (!fixture.filled) return;
    ustore_key_t const key = fixture.keys.front();
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {.database = fixture.database, .executor = call.executor, .outcome = &outcome};
    read.count = 1, read.collections = {&fixture.blobs, 0}, read.keys = {&key, 0}, read.values = &values;
    read.arena = call.arena;
    ustore_blobs_read(&read);
    if (!expect_success(outcome)) return;

    ArrowSchema schema {};
    ArrowDeviceArray array {};
    expect_true(ustore_strings_view_to_arrow(values, 1, values.layout, call.arena, &schema, &array) ==
                ustore_status_success_k);
    ArrowDeviceType const vendor = listed.vendor == ustore_gpu_vendor_amd_k     ? ARROW_DEVICE_ROCM
                                   : listed.vendor == ustore_gpu_vendor_apple_k ? ARROW_DEVICE_METAL
                                                                                : ARROW_DEVICE_CUDA;
    expect_true(array.device_type == vendor);
    expect_true(array.device_id == static_cast<std::int64_t>(listed.vendor_ordinal));
    expect_true(array.sync_event == nullptr);
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
    expect_true(ustore_strings_view_to_arrow(values, 1, other, call.arena, &schema, &array) == ustore_status_success_k);
    expect_true(array.device_type == vendor);
    std::vector<std::uint64_t> const converted = offsets_of(array, other);
    expect_true(mover.status().succeeded());
    expect_true(converted[0] == 0 && converted[1] == in_place[1] - in_place[0]);
    if (array.array.release) array.array.release(&array.array);
    if (schema.release) schema.release(&schema);
}

/** A cursor opened on host threads walks there alone, so pulling it on a device is refused with
 *  @c ustore_status_device_k. */
void test_vram_cursors(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return;
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return;
    mover.lands_in(call.arena, ordinal);
    fixture_t fixture {config, "cursors"};
    if (!fixture.filled) return;
    ustore_outcome_t outcome {};
    ustore_blobs_cursor_t cursor = nullptr;
    ustore_blobs_cursor_init_t opened {.cursor = &cursor, .database = fixture.database, .outcome = &outcome};
    opened.collection = fixture.blobs, opened.start_key = fixture.keys.front();
    ustore_blobs_cursor_init(&opened);
    if (!expect_success(outcome)) return;
    ustore_key_t const* found = nullptr;
    ustore_size_t written = 0;
    ustore_blobs_cursor_next_t pull {.database = fixture.database, .executor = call.executor, .outcome = &outcome};
    pull.cursor = cursor, pull.keys = &found, pull.written = &written, pull.arena = call.arena, pull.wanted = 1;
    ustore_blobs_cursor_next(&pull);
    expect_true(outcome.status == ustore_status_device_k);

    // A device cursor walks there, its keys and values landing in the device arena as a host pull's
    ustore_blobs_cursor_t on_device = nullptr;
    ustore_blobs_cursor_init_t device_opened = opened;
    device_opened.cursor = &on_device, device_opened.executor = call.executor;
    ustore_blobs_cursor_init(&device_opened);
    if (!expect_success(outcome)) return;
    ustore_key_t const *host_keys = nullptr, *device_keys = nullptr;
    ustore_strings_view_t host_values {}, device_values {};
    ustore_size_t host_written = 0, device_written = 0;
    host_arena_t const reference;
    ustore_blobs_cursor_next_t host_pull {.database = fixture.database, .outcome = &outcome};
    host_pull.cursor = cursor, host_pull.keys = &host_keys, host_pull.values = &host_values;
    host_pull.written = &host_written, host_pull.arena = reference.arena, host_pull.wanted = entries_k / 2;
    ustore_blobs_cursor_next(&host_pull);
    expect_success(outcome);
    ustore_blobs_cursor_next_t device_pull = host_pull;
    device_pull.executor = call.executor, device_pull.cursor = on_device, device_pull.keys = &device_keys;
    device_pull.values = &device_values, device_pull.written = &device_written, device_pull.arena = call.arena;
    ustore_blobs_cursor_next(&device_pull);
    expect_success(outcome);
    expect_equal(device_written, host_written);
    expect_column(mover, host_keys, device_keys, host_written);
    expect_tape(mover, host_values, device_values, host_written);
    ustore_blobs_cursor_free(on_device);
    ustore_blobs_cursor_free(cursor);
}

void test_vram_reads(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return;
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return;
    mover.lands_in(call.arena, ordinal);
    fixture_t fixture {config, "reads"};
    if (!fixture.filled) return;
    std::size_t const count = entries_k + 4;
    std::vector<ustore_key_t> asked = fixture.keys;
    for (std::size_t extra = 0; extra != 4; ++extra) asked.push_back(util::key_from_integer(1000 + extra));
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_presences, device_presences, (count + 7) / 8);
        expect_column(mover, host_lengths, device_lengths, count);
        expect_tape(mover, host_values, device_values, count);
        // Every carve is padded to 64 bytes with its tail zeroed, as an Arrow validity bitmap reads it whole
        std::vector<ustore_octet_t> const padded = mover.fetch(device_presences, 64);
        bool tail_zeroed = true;
        for (std::size_t index = (count + 7) / 8; index != padded.size(); ++index) tail_zeroed &= padded[index] == 0;
        expect_true(tail_zeroed);
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_offsets, device_offsets, 2);
        if (kind == listing_kind_t::scan_k && host_offsets)
            expect_column(mover, host_keys, device_keys, host_offsets[1]);
    };
    listing(&ustore_blobs_scan, fixture.blobs, listing_kind_t::scan_k);
    listing(&ustore_blobs_sample, fixture.blobs, listing_kind_t::sample_k);
    listing(&ustore_docs_scan, fixture.docs, listing_kind_t::scan_k);
    listing(&ustore_docs_sample, fixture.docs, listing_kind_t::sample_k);
    listing(&ustore_graph_scan_vertices, fixture.graph, listing_kind_t::scan_k);
    listing(&ustore_pages_scan, fixture.pages, listing_kind_t::scan_k);

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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_presences, device_presences, (entries_k + 7) / 8);
        expect_column(mover, host_values, device_values, entries_k);
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_offsets, device_offsets, 2);
        if (host_offsets) expect_tape(mover, host_strings, device_strings, host_offsets[1]);
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_presences, device_presences, (count + 7) / 8);
        expect_column(mover, host_lengths, device_lengths, count);
        expect_tape(mover, host_values, device_values, count);
    };
    tape_t names;
    for (std::size_t index = 0; index != count; ++index) names.push("name");
    rendering(&ustore_docs_read, fixture.docs, {});
    rendering(&ustore_docs_read, fixture.docs, names.view());
    rendering(&ustore_pages_read, fixture.pages, {});
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        if (host_columns && expect_true(device_columns && mover.outputs_on_device())) {
            std::vector<ustore_docs_column_view_t> const columns = mover.fetch(device_columns, 3);
            for (std::size_t field = 0; field != 3; ++field) {
                ustore_docs_column_view_t const &on_host = host_columns[field], &on_device = columns[field];
                std::size_t const bitmap = (count + 7) / 8;
                expect_column(mover, on_host.validities, on_device.validities, bitmap);
                expect_column(mover, on_host.conversions, on_device.conversions, bitmap);
                expect_column(mover, on_host.collisions, on_device.collisions, bitmap);
                expect_column(mover, on_host.scalars, on_device.scalars, count * widths[field]);
                expect_tape(mover, on_host.strings, on_device.strings, widths[field] ? 0 : count);
            }
        }
        else expect_true(!host_columns);
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_degrees, device_degrees, count);
        expect_column(mover, host_offsets, device_offsets, count + 1);
        if (host_offsets) expect_column(mover, host_edges, device_edges, 3 * host_offsets[count]);
    }
}

void test_vram_staging(std::size_t ordinal, ustore_gpu_device_t listed, std::vector<ustore_gpu_device_t> others,
                       config_t config) {
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return;
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return;
    mover.lands_in(call.arena, ordinal);
    fixture_t fixture {config, "staging"};
    if (!fixture.filled) return;
    host_arena_t const kept;

    // Keys and a tape of strings on the executor's own device are downloaded once and read as the host's
    ustore_key_t const* const placed_keys = mover.place(std::span<ustore_key_t const> {fixture.keys});
    ustore_byte_t const* const placed_bytes = mover.place(std::span<ustore_byte_t const> {
        reinterpret_cast<ustore_byte_t const*>(fixture.texts.bytes.data()), fixture.texts.bytes.size()});
    std::uint32_t const* const placed_offsets = mover.place(std::span<std::uint32_t const> {fixture.texts.offsets});
    if (!expect_true(mover.status().succeeded())) return;
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
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_tape(mover, host_values, device_values, entries_k);
    }
    {
        ustore_key_t const *host_values = nullptr, *device_values = nullptr;
        ustore_strings_read_t host {.database = fixture.database, .count = entries_k}, device = host;
        host.collections = device.collections = {&fixture.strings, 0};
        host.strings = fixture.texts.view();
        device.strings = util::u32_tape(placed_bytes, placed_offsets);
        host.values = &host_values, device.values = &device_values;
        ustore_outcome_t host_outcome {}, device_outcome {};
        host_arena_t const reference;
        host.outcome = &host_outcome, host.arena = reference.arena;
        device.outcome = &device_outcome, device.executor = call.executor, device.arena = call.arena;
        ustore_strings_read(&host), ustore_strings_read(&device);
        expect_success(host_outcome);
        expect_success(device_outcome);
        expect_column(mover, host_values, device_values, entries_k);
    }
    // Keys on another vendor's device are refused, never read as host memory
    for (ustore_gpu_device_t const& other : others) {
        if (other.vendor == listed.vendor) continue;
        mover_t foreign;
        if (!expect_true(foreign.open(other).succeeded())) return;
        ustore_key_t const* const foreign_keys = foreign.place(std::span<ustore_key_t const> {fixture.keys});
        if (!expect_true(foreign.status().succeeded())) return;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read {.database = fixture.database,
                                  .executor = call.executor,
                                  .outcome = &outcome,
                                  .count = entries_k};
        read.collections = {&fixture.blobs, 0}, read.keys = {foreign_keys, sizeof(ustore_key_t)};
        read.values = &values, read.arena = call.arena;
        ustore_blobs_read(&read);
        expect_true(outcome.status == ustore_status_device_k);
        expect_true(values.data.u32_tape.offsets == nullptr);
    }
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
            after.pointer_queries - before.pointer_queries,
            after.stream_creations - before.stream_creations,
            after.stream_destructions - before.stream_destructions,
            after.callbacks - before.callbacks};
}

void print_counted(char const* label, fake_hip_counters_t const& counted) {
    fmt::println("  {}: {} uploads, {} downloads, {} fills, {} waits, {} allocations, {} frees, {} pins, {} queries",
                 label, counted.uploads, counted.downloads, counted.fills, counted.waits, counted.allocations,
                 counted.frees, counted.registrations, counted.pointer_queries);
}

case_result_t test_vram_warm_calls(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    // Without the stand-in the calls still run, one cold and three warm, for a driver trace to count
    fake_hip_counters_t const* const counters = listed.vendor == ustore_gpu_vendor_amd_k ? fake_hip_counters()
                                                                                         : nullptr;
    fake_hip_counters_t const unused {};
    fake_hip_counters_t const& counted = counters ? *counters : unused;
    device_call_t call {ordinal};
    if (!expect_true(call.status.succeeded())) return {};
    fixture_t fixture {config, "warm_calls"};
    if (!fixture.filled) return {};
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
    expect_success(outcome);
    fake_hip_counters_t const cold = counted_since(before_cold, counted);
    for (std::size_t repeat = 0; repeat != 3; ++repeat) {
        fake_hip_counters_t const before_warm = counted;
        ustore_blobs_read(&read);
        expect_success(outcome);
        fake_hip_counters_t const warm = counted_since(before_warm, counted);
        if (!counters) continue;
        if (repeat == 0) print_counted("cold blobs.read", cold), print_counted("warm blobs.read", warm);
        expect_true(warm.uploads == 4);
        // The stream ends the call through its callback, so the host waits on no stream
        expect_true(warm.waits == 0 && warm.callbacks == 1);
        expect_true(warm.allocations == 0);
        expect_true(warm.registrations == 0);
        expect_true(warm.frees == 0);
        expect_true(warm.downloads == 0);
    }
    if (!counters) return case_result_t::skipped(needs_hip_stand_in_k);
    expect_true(cold.uploads == 4);
    expect_true(cold.waits == 0 && cold.callbacks == 1);
    expect_true(cold.allocations >= 1);
    return {};
}

/** A deferred read served through the host verb is still deferred once submitted, while the
 *  stream holds the callback ending it, and final once the stream drains: failed with its outputs
 *  null when the queued work fails, and otherwise holding the values, after which its executor is
 *  freed at once, as nothing of the call touches it past its end. The stand-in's device memory is
 *  the host's, so the outputs read in place. */
case_result_t test_vram_streams_end_deferred_reads(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    using hold_t = void (*)(int);
    using drain_t = void (*)(int);
    hold_t hold = nullptr;
    drain_t drain = nullptr;
    util::shared_library_t const runtime = util::shared_library_t::open("libamdhip64.so");
    if (listed.vendor != ustore_gpu_vendor_amd_k || !runtime.resolve("fake_hip_hold_callbacks", hold) ||
        !runtime.resolve("fake_hip_drain", drain))
        return case_result_t::skipped(needs_hip_stand_in_k);
    fixture_t fixture {config, "streams_end_deferred_reads"};
    if (!fixture.filled) return {};
    std::array<std::int32_t, 1> const named {static_cast<std::int32_t>(ordinal)};
    ustore_executor_t executor = nullptr;
    ustore_arena_t arena = nullptr;
    ustore_outcome_t outcome {};
    ustore_executor_for_vram_t spawn {.executor = &executor,
                                      .outcome = &outcome,
                                      .vram = {.devices = named.data(), .devices_count = named.size()}};
    ustore_executor_for_vram(&spawn);
    if (!expect_success(outcome)) return {};
    ustore_arena_init_t init {.arena = &arena, .executor = executor, .outcome = &outcome};
    ustore_arena_init(&init);
    if (!expect_success(outcome)) return ustore_executor_free(executor), case_result_t {};
    ustore_octet_t const* presences = nullptr;
    ustore_length_t const* lengths = nullptr;
    ustore_strings_view_t values {};
    ustore_blobs_read_t read {.database = fixture.database,
                              .executor = executor,
                              .outcome = &outcome,
                              .completion = ustore_completion_deferred_k,
                              .count = entries_k};
    read.collections = {&fixture.blobs, 0}, read.keys = {fixture.keys.data(), sizeof(ustore_key_t)};
    read.presences = &presences, read.lengths = &lengths, read.values = &values, read.arena = arena;
    constexpr int failed_k = 1;
    hold(1);
    ustore_blobs_read(&read);
    expect_true(ustore_outcome_status(&outcome) == ustore_status_deferred_k);
    drain(failed_k);
    expect_true(ustore_outcome_status(&outcome) == ustore_status_device_k);
    expect_true(presences == nullptr && lengths == nullptr && values.data.u32_tape.offsets == nullptr);

    ustore_blobs_read(&read);
    expect_true(ustore_outcome_status(&outcome) == ustore_status_deferred_k);
    drain(0);
    hold(0);
    if (expect_success(outcome) && expect_true(presences && lengths))
        for (std::size_t index = 0; index != entries_k; ++index) {
            std::string const expected = std::string(index % 7, 'v') + std::to_string(index + 1);
            bool const wide = values.layout == ustore_strings_u64_tape_k;
            std::uint64_t const offset = wide ? values.data.u64_tape.offsets[index]
                                              : values.data.u32_tape.offsets[index];
            ustore_byte_t const* const data = wide ? values.data.u64_tape.data : values.data.u32_tape.data;
            expect_true((presences[index / 8] >> (index % 8)) & 1);
            expect_true(lengths[index] == expected.size());
            expect_true(std::string_view {reinterpret_cast<char const*>(data + offset), expected.size()} == expected);
        }
    expect_true(ustore_arena_free(arena) == ustore_status_success_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

case_result_t test_vram_frees_everything(std::size_t ordinal, ustore_gpu_device_t listed, config_t config) {
    if (listed.vendor != ustore_gpu_vendor_amd_k || !fake_hip_counters())
        return case_result_t::skipped(needs_hip_stand_in_k);
    using live_ranges_t = std::size_t (*)();
    live_ranges_t live = nullptr;
    util::shared_library_t::open("libamdhip64.so").resolve("fake_hip_live_ranges", live);
    std::size_t const before = live();
    {
        device_call_t call {ordinal};
        if (!expect_true(call.status.succeeded())) return {};
        fixture_t fixture {config, "frees_everything"};
        if (!fixture.filled) return {};
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
            if (!expect_success(outcome)) return {};
        }
    }
    expect_true(live() == before);
    return {};
}

/** An executor naming no device drives every listed device of the first vendor with a known class,
 *  @p ordinals here, and its arenas live on the first of them. */
case_result_t test_vram_every_device(std::vector<std::int32_t> ordinals) {
    ustore_outcome_t outcome {};
    ustore_executor_t executor = nullptr;
    ustore_executor_for_vram_t spawn {.executor = &executor, .outcome = &outcome};
    ustore_executor_for_vram(&spawn);
    if (!expect_success(outcome)) return {};
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t init {.arena = &arena, .executor = executor, .outcome = &outcome};
    ustore_arena_init(&init);
    ustore_arena_description_t description {};
    ustore_arena_describe_t describe {.arena = arena, .outcome = &outcome, .description = &description};
    if (expect_success(outcome)) ustore_arena_describe(&describe);
    expect_success(outcome);
    expect_true(description.residency == ustore_residency_vram_k && description.device == ordinals.front());
    // Only Apple's shared runs are host-readable; NVIDIA and AMD runs are the device's own memory
    expect_true(description.vendor != ustore_gpu_vendor_unknown_k && description.vendor_ordinal >= 0);
    expect_true(description.host_access == (description.vendor == ustore_gpu_vendor_apple_k
                                                ? ustore_host_access_readable_k
                                                : ustore_host_access_refused_k));
    ustore_arena_free(arena);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

/** The executor adopts a HIP stream per named device and destroys it when freed, and refuses a
 *  null or default stream, a device named twice beside streams, and streams beside no named device
 *  as malformed, and a stream on another device than its entry names as mismatched, each leaving
 *  the streams the caller's. */
case_result_t test_vram_adopted_streams(std::vector<std::int32_t> ordinals, std::vector<ustore_gpu_device_t> devices) {
    using set_device_t = int (*)(int);
    using create_stream_t = int (*)(void**, unsigned);
    using destroy_stream_t = int (*)(void*);
    util::shared_library_t const runtime = util::shared_library_t::open("libamdhip64.so");
    set_device_t set_device = nullptr;
    create_stream_t create_stream = nullptr;
    destroy_stream_t destroy_stream = nullptr;
    if (!expect_true(runtime.resolve("hipSetDevice", set_device) &&
                     runtime.resolve("hipStreamCreateWithFlags", create_stream) &&
                     runtime.resolve("hipStreamDestroy", destroy_stream)))
        return {};
    auto const stream_on = [&](std::int32_t ordinal) noexcept {
        void* stream = nullptr;
        if (set_device(static_cast<int>(devices[static_cast<std::size_t>(ordinal)].vendor_ordinal)) != 0 ||
            create_stream(&stream, 1) != 0)
            return static_cast<void*>(nullptr);
        return stream;
    };
    auto const spawned = [](std::span<std::int32_t const> named, std::span<void* const> streams) noexcept {
        ustore_outcome_t outcome {};
        ustore_executor_t executor = nullptr;
        ustore_executor_for_vram_t spawn {.executor = &executor,
                                          .outcome = &outcome,
                                          .vram = {.devices = named.data(), .devices_count = named.size()},
                                          .streams = streams.empty() ? nullptr : streams.data()};
        ustore_executor_for_vram(&spawn);
        expect_true(ustore_executor_free(executor) == ustore_status_success_k);
        return outcome.status;
    };
    std::int32_t const first = ordinals[0], second = ordinals[1];
    std::array<std::int32_t, 1> const alone {first};
    std::array<std::int32_t, 2> const both {first, second}, twice {first, first};

    // The stand-in counts streams, so each adopted one is seen destroyed exactly once, by the
    // executor's free, and each refused one never by the library
    fake_hip_counters_t const* const counters = fake_hip_counters();
    auto const destroyed = [counters]() noexcept { return counters ? counters->stream_destructions : 0; };
    std::size_t const created_before = counters ? counters->stream_creations : 0;
    std::size_t const destroyed_before = destroyed();

    // Adopted streams are the executor's from then on, destroyed when it is freed
    std::array<void*, 1> const adopted {stream_on(first)};
    expect_true(spawned(alone, adopted) == ustore_status_success_k);
    std::array<void*, 2> const adopted_both {stream_on(first), stream_on(second)};
    expect_true(spawned(both, adopted_both) == ustore_status_success_k);
    if (counters) expect_equal(destroyed() - destroyed_before, std::size_t {3});

    // Default streams no executor can destroy
    for (std::uintptr_t const handle : {std::uintptr_t {0}, std::uintptr_t {1}, std::uintptr_t {2}}) {
        std::array<void*, 1> const default_stream {reinterpret_cast<void*>(handle)};
        expect_true(spawned(alone, default_stream) == ustore_status_invalid_argument_k);
    }
    // A refused stream stays the caller's, which destroys it
    std::array<void*, 1> const elsewhere {stream_on(second)};
    expect_true(spawned(alone, elsewhere) == ustore_status_config_mismatch_k);
    std::array<void*, 2> const repeated {stream_on(first), stream_on(first)};
    expect_true(spawned(twice, repeated) == ustore_status_invalid_argument_k);
    std::array<void*, 1> const unnamed {stream_on(first)};
    expect_true(spawned({}, unnamed) == ustore_status_invalid_argument_k);
    if (counters) expect_equal(destroyed() - destroyed_before, std::size_t {3});
    for (void* const stream : {elsewhere[0], repeated[0], repeated[1], unnamed[0]})
        expect_true(destroy_stream(stream) == 0);
    // Every stream the case made is gone, the caller's refused ones by the caller alone
    if (counters) expect_equal(counters->stream_creations - created_before, destroyed() - destroyed_before);
    return {};
}

/** The executor names its HIP stream, accepts it back and refuses one on another device, and
 *  queues waits on a foreign stream and a foreign event, refusing a wait naming neither or both
 *  and a position past its streams. */
case_result_t test_vram_names_streams(std::vector<std::int32_t> ordinals, std::vector<ustore_gpu_device_t> devices) {
    using set_device_t = int (*)(int);
    using create_t = int (*)(void**, unsigned);
    using destroy_t = int (*)(void*);
    util::shared_library_t const runtime = util::shared_library_t::open("libamdhip64.so");
    set_device_t set_device = nullptr;
    create_t create_stream = nullptr, create_event = nullptr;
    destroy_t destroy_stream = nullptr, destroy_event = nullptr;
    if (!expect_true(runtime.resolve("hipSetDevice", set_device) &&
                     runtime.resolve("hipStreamCreateWithFlags", create_stream) &&
                     runtime.resolve("hipStreamDestroy", destroy_stream) &&
                     runtime.resolve("hipEventCreateWithFlags", create_event) &&
                     runtime.resolve("hipEventDestroy", destroy_event)))
        return {};
    auto const stream_on = [&](std::int32_t ordinal) noexcept {
        void* stream = nullptr;
        if (set_device(static_cast<int>(devices[static_cast<std::size_t>(ordinal)].vendor_ordinal)) != 0 ||
            create_stream(&stream, 1) != 0)
            return static_cast<void*>(nullptr);
        return stream;
    };
    device_call_t call {static_cast<std::size_t>(ordinals[0])};
    if (!expect_true(call.status.succeeded())) return {};
    ustore_outcome_t outcome {};

    // The executor's own stream is named, and sits on its own device
    void* own = nullptr;
    ustore_executor_stream_t named {.executor = call.executor, .outcome = &outcome, .stream = &own};
    ustore_executor_stream(&named);
    expect_success(outcome);
    expect_true(own != nullptr);
    void* past = &outcome;
    named.position = 1, named.stream = &past;
    ustore_executor_stream(&named);
    expect_true(outcome.status == ustore_status_invalid_argument_k && past == nullptr);
    ustore_executor_accepts_t accepts {.executor = call.executor, .stream = own, .outcome = &outcome};
    ustore_executor_accepts(&accepts);
    expect_success(outcome);
    void* const elsewhere = stream_on(ordinals[1]);
    accepts.stream = elsewhere;
    ustore_executor_accepts(&accepts);
    expect_true(outcome.status == ustore_status_config_mismatch_k);
    // The null and per-thread handles name the current device's default streams, meaning this one's
    for (std::uintptr_t const special : {std::uintptr_t {0}, std::uintptr_t {2}}) {
        accepts.stream = reinterpret_cast<void*>(special);
        ustore_executor_accepts(&accepts);
        expect_success(outcome);
    }

    // A foreign stream or event on the same device is followed without the host waiting
    void* const foreign = stream_on(ordinals[0]);
    void* event = nullptr;
    expect_true(create_event(&event, 0) == 0);
    ustore_executor_follow_t follow {.executor = call.executor, .stream = foreign, .outcome = &outcome};
    ustore_executor_follow(&follow);
    expect_success(outcome);
    // The event is passed as Arrow's `sync_event` holds it, through a pointer, where a null one
    // leaves nothing to wait for
    follow.stream = nullptr, follow.event = &event;
    ustore_executor_follow(&follow);
    expect_success(outcome);
    void* no_event = nullptr;
    follow.event = &no_event;
    ustore_executor_follow(&follow);
    expect_success(outcome);
    follow.event = &event, follow.stream = foreign;
    ustore_executor_follow(&follow);
    expect_true(outcome.status == ustore_status_invalid_argument_k);
    // A stream on another device can't take this stream's mark, though its events can be followed
    follow.event = nullptr, follow.stream = elsewhere;
    ustore_executor_follow(&follow);
    expect_true(outcome.status == ustore_status_config_mismatch_k);
    follow.stream = nullptr, follow.event = &event, follow.position = 1;
    ustore_executor_follow(&follow);
    expect_true(outcome.status == ustore_status_invalid_argument_k);
    for (void* const stream : {elsewhere, foreign}) expect_true(destroy_stream(stream) == 0);
    expect_true(destroy_event(event) == 0);
    return {};
}

/** Admits the keys spelling even integers, at either predicate cost. */
void admit_even_keys(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count, void*) {
    for (std::size_t index = 0; index != count; ++index)
        admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
}

/** Whether the library embeds kernels for the class of @p listed. */
bool embeds_kernels(ustore_gpu_device_t const& listed) {
    return !gpu_class_bundle(static_cast<gpu_class_t>(listed.gpu_class)).empty();
}

/** Filtered scans on an executor over the UStore @p ordinals, @p listed the first, keep what the
 *  host keeps, however many devices the pages are dealt across. */
case_result_t test_vram_matching(std::vector<std::int32_t> ordinals, ustore_gpu_device_t listed, config_t config) {
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return {};
    device_call_t call {ordinals};
    if (!expect_true(call.status.succeeded())) return {};
    mover.lands_in(call.arena, static_cast<std::size_t>(ordinals.front()));
    fixture_t fixture {config, "matching"};
    if (!fixture.filled) return {};
    // Every device matches values through the library's kernels, but the stand-in HIP driver
    // loads no bundle
    bool const loads_bundle = embeds_kernels(listed) &&
                              (listed.vendor == ustore_gpu_vendor_nvidia_k || !fake_hip_counters());
    device_service_t const matches_values = loads_bundle ? device_service_t::served_k : device_service_t::unsupported_k;
    ustore_predicate_t const even_cheap {.callback = &admit_even_keys, .cost = ustore_predicate_cost_cheap_k};
    ustore_predicate_t const even_expensive {.callback = &admit_even_keys, .cost = ustore_predicate_cost_expensive_k};

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
        if (!expect_success(host_outcome) || !expect_true(host_offsets != nullptr)) return;
        expect_true(host_offsets[1] >= least && host_offsets[1] <= most);
        if (service == device_service_t::unsupported_k) {
            expect_true(device_outcome.status == ustore_status_unsupported_k);
            return;
        }
        expect_success(device_outcome);
        expect_column(mover, host_offsets, device_offsets, 2);
        expect_column(mover, host_keys, device_keys, host_offsets[1]);
        expect_column(mover, host_scores, device_scores, host_offsets[1]);
    };
    std::array<std::uint32_t, 2> pattern_offsets {};
    auto const patterns_of = [&](std::string_view text, ustore_pattern_kind_t kind) {
        pattern_offsets = {0, static_cast<std::uint32_t>(text.size())};
        return ustore_patterns_t {.patterns = util::u32_tape(text.data(), pattern_offsets.data()),
                                  .count = 1,
                                  .kind = kind};
    };
    std::size_t const wanted = entries_k / 4;
    float const weight = 2.0f;

    // Values of three `v` before their number, one in seven
    filtered(
        &ustore_blobs_scan, fixture.blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of("^vvv[0-9]", ustore_pattern_regex_k); },
        matches_values, 1, wanted - 1);
    // Three or more `v` from a start key on the device, which staging moves into the bounce every page rewinds
    ustore_key_t const* const placed_start = mover.place(
        std::span<ustore_key_t const> {&fixture.keys[entries_k / 2], 1});
    if (!expect_true(mover.status().succeeded())) return {};
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
        expect_success(host_outcome);
        expect_true(host_offsets && host_offsets[1] > 0);
        if (matches_values == device_service_t::unsupported_k)
            expect_true(device_outcome.status == ustore_status_unsupported_k);
        else if (host_offsets) {
            expect_success(device_outcome);
            expect_column(mover, host_offsets, device_offsets, 2);
            expect_column(mover, host_keys, device_keys, host_offsets[1]);
        }
    }
    // Five or six `v`, scored, and only even keys among them, asked before the values are read
    filtered(
        &ustore_blobs_scan, fixture.blobs,
        [&](ustore_blobs_scan_t& scan) {
            scan.patterns = patterns_of("vvvvv", ustore_pattern_substrings_k);
            scan.patterns.weights = &weight;
            scan.predicate = even_cheap;
        },
        matches_values, 1, wanted - 1);
    // A predicate alone reads no values, so every vendor serves it
    filtered(
        &ustore_blobs_scan, fixture.blobs, [&](ustore_blobs_scan_t& scan) { scan.predicate = even_expensive; },
        device_service_t::served_k, wanted, wanted);
    filtered(
        &ustore_graph_scan_vertices, fixture.graph,
        [&](ustore_graph_scan_vertices_t& scan) { scan.predicate = even_cheap; }, device_service_t::served_k, wanted,
        wanted);
    // Documents with a member past 40, their even keys asked after the values matched, and a path none holds
    filtered(
        &ustore_docs_scan, fixture.docs,
        [&](ustore_docs_scan_t& scan) {
            scan.filter = util::bytes_of("$[?@ > 40]");
            scan.predicate = even_expensive;
        },
        matches_values, wanted, wanted);
    filtered(
        &ustore_docs_scan, fixture.docs, [&](ustore_docs_scan_t& scan) { scan.filter = util::bytes_of("$.missing"); },
        matches_values, 0, 0);
    // A filter the kernels do not walk matches on the host, so every device serving
    // matches serves it
    filtered(
        &ustore_docs_scan, fixture.docs,
        [&](ustore_docs_scan_t& scan) { scan.filter = util::bytes_of("?@.score > 40"); }, matches_values, wanted,
        wanted);
    // Every page holds a bold element, and none an italic one
    filtered(
        &ustore_pages_scan, fixture.pages, [&](ustore_pages_scan_t& scan) { scan.filter = util::bytes_of("//b"); },
        matches_values, wanted, wanted);
    filtered(
        &ustore_pages_scan, fixture.pages, [&](ustore_pages_scan_t& scan) { scan.filter = util::bytes_of("//i"); },
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
        expect_success(host_outcome);
        expect_true(host_offsets && host_offsets[1] > 0);
        if (matches_values == device_service_t::unsupported_k)
            expect_true(device_outcome.status == ustore_status_unsupported_k);
        else if (host_offsets) {
            expect_success(device_outcome);
            expect_column(mover, host_offsets, device_offsets, 2);
            expect_tape(mover, host_strings, device_strings, host_offsets[1]);
        }
    }
    return {};
}

/** A tape the device assembles from strings it holds lays out byte for byte as the
 *  host's @c assemble_tape lays it, across several blocks of the offsets scan and
 *  segments of the gather, empty strings among them. */
case_result_t test_vram_tapes(ustore_gpu_device_t listed) {
    mover_t mover;
    if (!expect_true(mover.open(listed).succeeded())) return {};
    if (!mover.launches()) return case_result_t::skipped("needs library kernels for the device");
    std::vector<std::string> strings;
    for (std::size_t index = 0; index != 3 * util::tape_block_threads_k + 5; ++index)
        strings.push_back(std::string(index % 11, static_cast<char>('a' + index % 26)));
    std::string joined;
    for (std::string const& string : strings) joined += string;
    char const* const placed = mover.place(std::span<char const> {joined});
    std::vector<util::tape_source_t> host_sources, device_sources;
    std::size_t offset = 0;
    for (std::string const& string : strings) {
        host_sources.push_back({reinterpret_cast<std::byte const*>(string.data()), string.size()});
        device_sources.push_back({reinterpret_cast<std::byte const*>(placed + offset), string.size()});
        offset += string.size();
    }

    std::vector<std::uint32_t> host_offsets(strings.size() + 1);
    std::vector<std::byte> host_bytes(joined.size());
    util::assemble_tape<std::uint32_t>(
        strings.size(), [&](std::size_t index) noexcept { return host_sources[index]; },
        std::span<std::uint32_t> {host_offsets}, std::span<std::byte> {host_bytes});

    // The outputs start poisoned, so every byte the device leaves unwritten shows
    std::vector<std::uint32_t> const poisoned_offsets(host_offsets.size(), ~std::uint32_t {0});
    std::vector<std::byte> const poisoned_bytes(host_bytes.size(), std::byte {0xFF});
    util::tape_source_t const* const sources = mover.place(std::span<util::tape_source_t const> {device_sources});
    std::uint32_t* const offsets = mover.place(std::span {poisoned_offsets});
    std::byte* const bytes = mover.place(std::span {poisoned_bytes});
    mover.copy([&](auto& executor) noexcept {
        return util::assemble_tape<std::uint32_t>(
            executor, std::span<util::tape_source_t const> {sources, strings.size()},
            std::span<std::uint32_t> {offsets, host_offsets.size()}, std::span<std::byte> {bytes, host_bytes.size()});
    });
    expect_true(mover.status().succeeded());
    expect_true(mover.fetch(offsets, host_offsets.size()) == host_offsets);
    expect_true(mover.fetch(bytes, host_bytes.size()) == host_bytes);
    return {};
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);
    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    std::vector<ustore_gpu_device_t> const devices = listed_devices();
    if (devices.empty())
        run_test_case("test_vram_devices_listed", [] { return case_result_t::skipped(no_gpu_device_k); });
    for (std::size_t ordinal = 0; ordinal != devices.size(); ++ordinal) {
        ustore_gpu_device_t const& listed = devices[ordinal];
        fmt::println("device {}: {}, vendor ordinal {}", ordinal, listed.name, listed.vendor_ordinal);
        std::string const prefix = fmt::format("test_vram_{}{}_", ustore_gpu_vendor_name(listed.vendor), ordinal);
        // A device of a class this build left out still lists, and every executor on it is refused
        if (listed.gpu_class == ustore_gpu_class_unknown_k) {
            run_test_case((prefix + "classes").c_str(), [] { return case_result_t::skipped(needs_gpu_class_k); });
            continue;
        }
        run_test_case((prefix + "listing").c_str(), test_vram_listing, listed);
        run_test_case((prefix + "class_ceiling").c_str(), test_vram_class_ceiling, ordinal, listed);
        run_test_case((prefix + "arenas").c_str(), test_vram_arenas, ordinal, config);
        run_test_case((prefix + "cursors").c_str(), test_vram_cursors, ordinal, listed, config);
        run_test_case((prefix + "exports").c_str(), test_vram_exports, ordinal, listed, config);
        run_test_case((prefix + "reads").c_str(), test_vram_reads, ordinal, listed, config);
        run_test_case((prefix + "staging").c_str(), test_vram_staging, ordinal, listed, devices, config);
        run_test_case((prefix + "warm_calls").c_str(), test_vram_warm_calls, ordinal, listed, config);
        run_test_case((prefix + "streams_end_deferred_reads").c_str(), test_vram_streams_end_deferred_reads, ordinal,
                      listed, config);
        run_test_case((prefix + "frees_everything").c_str(), test_vram_frees_everything, ordinal, listed, config);
        run_test_case((prefix + "matching").c_str(), test_vram_matching,
                      std::vector<std::int32_t> {static_cast<std::int32_t>(ordinal)}, listed, config);
        // Tapes launch kernels, which neither a build embedding none nor the HIP stand-in can run
        if (embeds_kernels(listed) && (listed.vendor != ustore_gpu_vendor_amd_k || !fake_hip_counters()))
            run_test_case((prefix + "tapes").c_str(), test_vram_tapes, listed);
    }
    // Every vendor listing several devices of a known class has one executor drive them all
    std::size_t const first_known = static_cast<std::size_t>(
        std::find_if(devices.begin(), devices.end(),
                     [](ustore_gpu_device_t const& listed) { return listed.gpu_class != ustore_gpu_class_unknown_k; }) -
        devices.begin());
    for (ustore_gpu_vendor_t const vendor :
         {ustore_gpu_vendor_nvidia_k, ustore_gpu_vendor_amd_k, ustore_gpu_vendor_apple_k}) {
        std::vector<std::int32_t> ordinals;
        for (std::size_t ordinal = 0; ordinal != devices.size(); ++ordinal)
            if (devices[ordinal].vendor == vendor && devices[ordinal].gpu_class != ustore_gpu_class_unknown_k)
                ordinals.push_back(static_cast<std::int32_t>(ordinal));
        if (ordinals.size() < 2) continue;
        std::string const prefix = fmt::format("test_vram_{}_", ustore_gpu_vendor_name(vendor));
        ustore_gpu_device_t const& first = devices[static_cast<std::size_t>(ordinals.front())];
        if (static_cast<std::size_t>(ordinals.front()) == first_known)
            run_test_case((prefix + "every_device").c_str(), test_vram_every_device, ordinals);
        if (vendor == ustore_gpu_vendor_amd_k) {
            run_test_case((prefix + "adopted_streams").c_str(), test_vram_adopted_streams, ordinals, devices);
            run_test_case((prefix + "names_streams").c_str(), test_vram_names_streams, ordinals, devices);
        }
        run_test_case((prefix + "matching_across_devices").c_str(), test_vram_matching, ordinals, first, config);
    }
    return finish_test_run();
}
