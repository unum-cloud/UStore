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

#pragma region Fixtures

using linux_cufile_files_disk_t = util::linux_cufile_files_disk<atomic_reference, waiter_t>;

/** The cuFile entry points, loaded once, as the process's GPU drivers load them. */
util::cufile_api_t const& cufile() noexcept {
    static util::cufile_api_t const api = util::cufile_api_t::load();
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
        util::shared_library_t const library = util::shared_library_t::open("libcuda.so.1");
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
    [[nodiscard]] std::span<util::byte_t> bytes() const noexcept {
        return {reinterpret_cast<util::byte_t*>(static_cast<std::uintptr_t>(address_)), address_ ? bytes_ : 0};
    }

    /** Copies @p from into the start of the device bytes, answering whether the driver did. */
    [[nodiscard]] bool upload(std::span<util::byte_t const> from) const noexcept {
        return cuda_->copy_to_device(address_, from.data(), from.size()) == 0;
    }

    /** Copies the start of the device bytes into @p into, answering whether the driver did. */
    [[nodiscard]] bool download(std::span<util::byte_t> into) const noexcept {
        return cuda_->copy_to_host(into.data(), address_, into.size()) == 0;
    }
};

/** Submits @p requests in one batch and waits for every completion, checking that each
 *  completed in full. */
void move_in_full(linux_cufile_files_disk_t& disk, util::queue_pair_t const& queue_pair,
                  std::span<util::request_t const> requests) {
    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(disk.submit(queue_pair, requests) == requests.size());
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          requests.size()) == requests.size());
    for (std::size_t index = 0; index != requests.size(); ++index) expect_true(completions[index].completed_in_full());
}

#pragma endregion Fixtures

#pragma region Cases

/** Without the @c nvidia_fs module, a mount that demands DMA is refused as a device failure. */
case_result_t test_cufile_refuses_direct_without_nvidia_fs() {
    if (linux_cufile_files_disk_t::probe(cufile()) == util::linux_cufile_probe_t::direct_k)
        return case_result_t::skipped("needs cuFile without nvidia_fs");
    util::linux_cufile_disk_config_t config;
    config.gds = ustore_gds_direct_k;
    expect_status(linux_cufile_files_disk_t::make(cufile(), config), util::status_code_t::device_k);
    return {};
}

/** Writes painted host bytes to the medium, reads them into device memory, writes that device memory to a second
 *  extent, and reads the second extent back into host memory, which must hold the same bytes. */
case_result_t test_cufile_moves_device_memory(linux_cufile_files_disk_t& disk, util::medium_reference_t medium,
                                              test_cuda_t const& cuda) {
    if (!cuda) return case_result_t::skipped(no_gpu_device_k);
    device_buffer_t const device {cuda, device_bytes_k};
    test_buffer_t host = make_buffer(2 * device_bytes_k);
    if (!expect_true(!device.bytes().empty() && host.size() == 2 * device_bytes_k)) return {};
    std::span<util::byte_t> const written = whole(host).first(device_bytes_k);
    std::span<util::byte_t> const read = whole(host).subspan(device_bytes_k);
    paint(written, 41);

    auto [queue_pair, acquire_status] = disk.acquire_queue_pair(queue_pair_depth_k);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return {};
    auto [host_memory, register_host_status] = disk.register_memory(whole(host), util::memory_space_t::host_k);
    auto [device_memory, register_device_status] = disk.register_memory(device.bytes(), util::memory_space_t::gpu_k);
    expect_true(register_host_status.succeeded() && register_device_status.succeeded());
    if (register_host_status.succeeded() && register_device_status.succeeded()) {
        expect_true(device_memory.space() == util::memory_space_t::gpu_k);

        util::extent_t const first {0, device_bytes_k}, second {device_bytes_k, device_bytes_k};
        std::array<util::request_t, 1> const host_write {
            util::write_request(medium, first, host_memory.reference(0), queue_pair.token(0))};
        std::array<util::request_t, 1> const device_read {
            util::read_request(medium, first, device_memory.reference(0), queue_pair.token(1))};
        std::array<util::request_t, 1> const device_write {
            util::write_request(medium, second, device_memory.reference(0), queue_pair.token(2))};
        std::array<util::request_t, 1> const host_read {
            util::read_request(medium, second, host_memory.reference(device_bytes_k), queue_pair.token(3))};
        move_in_full(disk, queue_pair, host_write);
        move_in_full(disk, queue_pair, device_read);

        // The device must hold what was written before it writes it back, which a copy of its own checks
        test_buffer_t copied = make_buffer(device_bytes_k);
        expect_true(device.download(whole(copied)) && same_bytes(whole(copied), written));
        move_in_full(disk, queue_pair, device_write);
        move_in_full(disk, queue_pair, host_read);
        expect_true(same_bytes(read, written));

        expect_true(disk.unregister_memory(device_memory).succeeded());
        expect_true(disk.unregister_memory(host_memory).succeeded());
    }
    else {
        if (register_host_status.succeeded()) expect_true(disk.unregister_memory(host_memory).succeeded());
        if (register_device_status.succeeded()) expect_true(disk.unregister_memory(device_memory).succeeded());
    }
    expect_true(disk.release_queue_pair(queue_pair).succeeded());
    return {};
}

#pragma endregion Cases

#pragma region Driver

/** Mounts cuFile in compat mode on unbuffered files and runs the suite on it. */
void cufile_transport(char const* directory) {
    run_test_case("test_cufile_refuses_direct_without_nvidia_fs", test_cufile_refuses_direct_without_nvidia_fs);

    util::linux_cufile_disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = 64;
    config.registered_memory_limit = 8;
    config.media_limit = 4;
    config.gds = ustore_gds_compatibility_k;
    auto [disk, status] = linux_cufile_files_disk_t::make(cufile(), config);
    run_test_case("linux_cufile_files_disk_t::make", [&] { expect_status(status, util::status_code_t::success_k); });
    if (status.failed()) return;
    run_test_case("linux_cufile_files_disk_t::capabilities",
                  [&] { expect_true(disk.capabilities().reaches_gpu_memory); });

    test_cuda_t const cuda = test_cuda_t::load();
    std::string const path = fmt::format("{}/{}", directory, "volume.bin");
    on_medium("linux_cufile_files_disk_t::register_medium", disk, volume_request(path.c_str()),
              [&](util::medium_reference_t medium) {
                  ring_cases("linux_cufile_files_disk_t", disk, medium);
                  run_test_case("test_cufile_moves_device_memory", test_cufile_moves_device_memory, disk, medium, cuda);
              });

    std::string const borrowed_path = fmt::format("{}/{}", directory, "volume_borrowed.bin");
    run_test_case("test_ring_holds_a_medium_in_flight<linux_cufile_files_disk_t>",
                  test_ring_holds_a_medium_in_flight<linux_cufile_files_disk_t>, disk,
                  volume_request(borrowed_path.c_str()));
    std::string const durable_path = fmt::format("{}/{}", directory, "volume_durable.bin");
    run_test_case("test_ring_keeps_what_it_flushed<linux_cufile_files_disk_t>",
                  test_ring_keeps_what_it_flushed<linux_cufile_files_disk_t>, disk,
                  volume_request(durable_path.c_str()));
}

/** Why this machine cannot run the suite, or null when it can. */
[[nodiscard]] char const* cufile_refusal(char const* directory) noexcept {
    switch (linux_cufile_files_disk_t::probe(cufile())) {
    case util::linux_cufile_probe_t::absent_k: return "needs libcufile";
    case util::linux_cufile_probe_t::refused_k: return "cuFile is refused on this machine";
    case util::linux_cufile_probe_t::direct_k:
    case util::linux_cufile_probe_t::compat_k: break;
    }
    if (!directory) return needs_directory_k;
    if (on_memory_filesystem(directory)) return needs_direct_filesystem_k;
    return nullptr;
}

#pragma endregion Driver

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    if (char const* const reason = cufile_refusal(directory.path()))
        run_test_case("linux_cufile_files_disk_t::make", [reason] { return case_result_t::skipped(reason); });
    else cufile_transport(directory.path());
    return finish_test_run();
}
