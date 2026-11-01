/**
 *  @file test/nvme/linux_cufile_disk.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The ring contract on cuFile, and what only a transport reaching device memory can get wrong.
 *
 *  The transport-independent cases of @c ring_cases.hpp run on host memory registered
 *  with cuFile. What is added here is device memory: a read into it and a write out
 *  of it, and the refusal of DMA where the @c nvidia_fs module is absent. Device
 *  memory comes from a @c libcuda this suite opens itself, and the suite skips
 *  cleanly where either library is absent. It needs @c --directory, off tmpfs, as
 *  every unbuffered transport.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include "ustore/utilities/blocking_files.hpp"         // `shared_library_t`
#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `linux_cufile_files_disk`, `linux_cufile_disk_config_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, every transport-independent result

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Fixtures

using linux_cufile_files_disk_t = linux_cufile_files_disk<atomic_reference, waiter_t>;

/** The cuFile entry points, loaded once, as the process's GPU drivers load them. */
cufile_api_t const& cufile() noexcept {
    static cufile_api_t const api = cufile_api_t::load();
    return api;
}

/** Bytes of device memory the device cases move, sixteen blocks. */
constexpr std::size_t device_bytes_k = 16 * block_bytes_k;

/** The few @c libcuda entry points the suite allocates and copies device memory with, on the primary context of
 *  device zero, which the cuFile registrations then run under. */
struct test_cuda_t {
    int (*init)(unsigned) = nullptr;
    int (*get_device)(int*, int) = nullptr;
    int (*retain_primary_context)(void**, int) = nullptr;
    int (*set_current_context)(void*) = nullptr;
    int (*allocate)(unsigned long long*, std::size_t) = nullptr;
    int (*free)(unsigned long long) = nullptr;
    int (*copy_to_host)(void*, unsigned long long, std::size_t) = nullptr;
    int (*copy_to_device)(unsigned long long, void const*, std::size_t) = nullptr;

    /** Whether the driver loaded and made device zero's primary context current on this thread. */
    explicit operator bool() const noexcept { return copy_to_device != nullptr; }

    /** Opens the driver and makes device zero current, or answers the empty table. */
    static test_cuda_t load() noexcept {
        shared_library_t const library = shared_library_t::open("libcuda.so.1");
        test_cuda_t cuda;
        if (!library || !library.resolve("cuInit", cuda.init) || !library.resolve("cuDeviceGet", cuda.get_device) ||
            !library.resolve("cuDevicePrimaryCtxRetain", cuda.retain_primary_context) ||
            !library.resolve("cuCtxSetCurrent", cuda.set_current_context) ||
            !library.resolve("cuMemAlloc_v2", cuda.allocate) || !library.resolve("cuMemFree_v2", cuda.free) ||
            !library.resolve("cuMemcpyDtoH_v2", cuda.copy_to_host) ||
            !library.resolve("cuMemcpyHtoD_v2", cuda.copy_to_device))
            return {};
        int device = 0;
        void* context = nullptr;
        if (cuda.init(0) != 0 || cuda.get_device(&device, 0) != 0 ||
            cuda.retain_primary_context(&context, device) != 0 || cuda.set_current_context(context) != 0)
            return {};
        return cuda;
    }
};

/** Device memory of @p bytes, freed with the scope, viewed as the span a registered memory takes. */
class device_buffer_t {
    test_cuda_t const* cuda_ = nullptr;
    unsigned long long address_ = 0;
    std::size_t bytes_ = 0;

  public:
    device_buffer_t(test_cuda_t const& cuda, std::size_t bytes) noexcept : cuda_(&cuda), bytes_(bytes) {
        if (cuda.allocate(&address_, bytes) != 0) address_ = 0;
    }
    device_buffer_t(device_buffer_t const&) = delete;
    device_buffer_t& operator=(device_buffer_t const&) = delete;
    ~device_buffer_t() noexcept {
        if (address_) cuda_->free(address_);
    }

    /** The device bytes, which only cuFile and the copies below touch. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept {
        return {reinterpret_cast<byte_t*>(static_cast<std::uintptr_t>(address_)), address_ ? bytes_ : 0};
    }

    /** Copies @p from into the start of the device bytes, answering whether the driver did. */
    [[nodiscard]] bool upload(std::span<byte_t const> from) const noexcept {
        return cuda_->copy_to_device(address_, from.data(), from.size()) == 0;
    }

    /** Copies the start of the device bytes into @p into, answering whether the driver did. */
    [[nodiscard]] bool download(std::span<byte_t> into) const noexcept {
        return cuda_->copy_to_host(into.data(), address_, into.size()) == 0;
    }
};

/** Submits @p requests in one batch and waits for every completion, counting each that did not complete in full. */
std::size_t move_in_full(linux_cufile_files_disk_t& disk, queue_pair_t const& queue_pair,
                         std::span<request_t const> requests) {
    std::array<completion_t, polling_room_k> completions {};
    std::size_t failures = expect(disk.submit(queue_pair, requests) == requests.size());
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           requests.size()) == requests.size());
    for (std::size_t index = 0; index != requests.size(); ++index)
        failures += expect(completions[index].completed_in_full());
    return failures;
}

#pragma endregion Fixtures

#pragma region Cases

/** Without the @c nvidia_fs module, a mount that demands DMA is refused as a device failure. */
case_result_t cufile_refuses_direct_without_nvidia_fs() {
    if (linux_cufile_files_disk_t::probe(cufile()) == linux_cufile_probe_t::direct_k)
        return case_result_t::skipped("nvidia_fs is loaded, so GPUDirect Storage is direct");
    linux_cufile_disk_config_t config;
    config.gds = ustore_gds_direct_k;
    return expect_status(linux_cufile_files_disk_t::make(cufile(), config), status_code_t::device_k);
}

/** Writes painted host bytes to the medium, reads them into device memory, writes that device memory to a second
 *  extent, and reads the second extent back into host memory, which must hold the same bytes. */
case_result_t cufile_moves_device_memory(linux_cufile_files_disk_t& disk, medium_reference_t medium,
                                         test_cuda_t const& cuda) {
    if (!cuda) return case_result_t::skipped("needs libcuda and a GPU for device memory");
    std::size_t failures = 0;
    device_buffer_t const device {cuda, device_bytes_k};
    test_buffer_t host = make_buffer(2 * device_bytes_k);
    failures += expect(!device.bytes().empty() && host.size() == 2 * device_bytes_k);
    if (failures) return failures;
    std::span<byte_t> const written = whole(host).first(device_bytes_k);
    std::span<byte_t> const read = whole(host).subspan(device_bytes_k);
    paint(written, 41);

    expected<queue_pair_t> leased = disk.acquire_queue_pair(queue_pair_depth_k);
    failures += expect(leased.has_value());
    if (!leased) return failures;
    queue_pair_t queue_pair = std::move(*leased);
    expected<registered_memory_t> host_registered = disk.register_memory(whole(host), memory_space_t::host_k);
    expected<registered_memory_t> device_registered = disk.register_memory(device.bytes(), memory_space_t::gpu_k);
    failures += expect(host_registered.has_value() && device_registered.has_value());
    if (host_registered && device_registered) {
        registered_memory_t host_memory = std::move(*host_registered);
        registered_memory_t device_memory = std::move(*device_registered);
        failures += expect(device_memory.space() == memory_space_t::gpu_k);

        extent_t const first {0, device_bytes_k}, second {device_bytes_k, device_bytes_k};
        std::array<request_t, 1> const host_write {
            write_request(medium, first, host_memory.reference(0), queue_pair.token(0))};
        std::array<request_t, 1> const device_read {
            read_request(medium, first, device_memory.reference(0), queue_pair.token(1))};
        std::array<request_t, 1> const device_write {
            write_request(medium, second, device_memory.reference(0), queue_pair.token(2))};
        std::array<request_t, 1> const host_read {
            read_request(medium, second, host_memory.reference(device_bytes_k), queue_pair.token(3))};
        failures += move_in_full(disk, queue_pair, host_write);
        failures += move_in_full(disk, queue_pair, device_read);

        // The device must hold what was written before it writes it back, which a copy of its own checks
        test_buffer_t copied = make_buffer(device_bytes_k);
        failures += expect(device.download(whole(copied)) && same_bytes(whole(copied), written));
        failures += move_in_full(disk, queue_pair, device_write);
        failures += move_in_full(disk, queue_pair, host_read);
        failures += expect(same_bytes(read, written));

        failures += expect(disk.unregister_memory(device_memory).succeeded());
        failures += expect(disk.unregister_memory(host_memory).succeeded());
    }
    else {
        if (host_registered) failures += expect(disk.unregister_memory(*host_registered).succeeded());
        if (device_registered) failures += expect(disk.unregister_memory(*device_registered).succeeded());
    }
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

#pragma endregion Cases

#pragma region Driver

/** Mounts cuFile in compat mode on unbuffered files and runs the suite on it. */
void cufile_transport(suite_totals_t& totals, char const* directory) {
    report(totals, "cufile.direct_without_nvidia_fs", cufile_refuses_direct_without_nvidia_fs);

    linux_cufile_disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = 64;
    config.registered_memory_limit = 8;
    config.media_limit = 4;
    config.gds = ustore_gds_compatibility_k;
    expected<linux_cufile_files_disk_t> disk = linux_cufile_files_disk_t::make(cufile(), config);
    if (report(totals, "cufile.mount", expect_status(disk, status_code_t::success_k)) != 0) return;
    report(totals, "cufile.reaches_gpu_memory", expect(disk->capabilities().reaches_gpu_memory));

    test_cuda_t const cuda = test_cuda_t::load();
    path_t const path {directory, "volume.bin"};
    on_medium(totals, "cufile.volume", *disk, volume_request(path.c_str()), [&](medium_reference_t medium) {
        ring_cases(totals, "cufile", *disk, medium);
        report(totals, "cufile.moves_device_memory", cufile_moves_device_memory, *disk, medium, cuda);
    });

    path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(totals, "cufile.medium_in_flight", ring_holds_a_medium_in_flight<linux_cufile_files_disk_t>, *disk,
           volume_request(borrowed_path.c_str()));
    path_t const durable_path {directory, "volume_durable.bin"};
    report(totals, "cufile.keeps_what_it_flushed", ring_keeps_what_it_flushed<linux_cufile_files_disk_t>, *disk,
           volume_request(durable_path.c_str()));
}

/** Why this machine cannot run the suite, or null when it can. */
[[nodiscard]] char const* cufile_refusal(test_environment_t const& environment) noexcept {
    switch (linux_cufile_files_disk_t::probe(cufile())) {
    case linux_cufile_probe_t::absent_k: return "needs libcufile";
    case linux_cufile_probe_t::refused_k: return "cuFile has neither nvidia_fs nor compat mode";
    case linux_cufile_probe_t::direct_k:
    case linux_cufile_probe_t::compat_k: break;
    }
    if (!environment.directory) return needs_directory_k;
    if (on_memory_filesystem(environment.directory)) return "needs a filesystem that enforces O_DIRECT, not tmpfs";
    return nullptr;
}

#pragma endregion Driver

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    if (char const* const reason = cufile_refusal(environment))
        report(totals, "cufile", case_result_t::skipped(reason));
    else cufile_transport(totals, environment.directory);
    return suite_exit_code(totals);
}
