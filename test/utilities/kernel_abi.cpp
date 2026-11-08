/**
 *  @file test/utilities/kernel_abi.cpp
 *  @author Ash Vardanian
 *  @date November 8, 2026
 *  @brief Compile-time checks of the Linux ABI mirrors in <tt>ustore/utilities/</tt> against the
 *      kernel's own headers.
 *
 *  The library spells the io_uring and statx structures and flags itself, so it builds against any
 *  kernel headers. This suite compares every namespace-scope mirror to the kernel's spelling, where
 *  the installed headers have it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `offsetof`

#include <linux/io_uring.h> // `IORING_SETUP_*`, `IOSQE_*`, `struct io_uring_sqe`
#include <linux/stat.h>     // `struct statx`
#include <linux/version.h>  // `LINUX_VERSION_CODE`

#include <fmt/format.h> // `fmt::println`

#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_sqe_t`, `linux_statx_t`

namespace unum::ustore::test {

using namespace unum::ustore::utilities;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static_assert(linux_uring_setup_r_disabled_k == IORING_SETUP_R_DISABLED);
static_assert(linux_uring_setup_coop_taskrun_k == IORING_SETUP_COOP_TASKRUN);
static_assert(linux_uring_setup_taskrun_flag_k == IORING_SETUP_TASKRUN_FLAG);
static_assert(linux_uring_setup_no_sqarray_k == IORING_SETUP_NO_SQARRAY);
static_assert(linux_uring_entry_io_link_k == IOSQE_IO_LINK);
static_assert(sizeof(linux_uring_sqe_t) == sizeof(struct io_uring_sqe));
static_assert(offsetof(linux_uring_sqe_t, off) == offsetof(struct io_uring_sqe, off));
static_assert(offsetof(linux_uring_sqe_t, len) == offsetof(struct io_uring_sqe, len));
static_assert(offsetof(linux_uring_sqe_t, fsync_flags) == offsetof(struct io_uring_sqe, fsync_flags));
static_assert(offsetof(linux_uring_sqe_t, user_data) == offsetof(struct io_uring_sqe, user_data));
static_assert(offsetof(linux_uring_sqe_t, buf_index) == offsetof(struct io_uring_sqe, buf_index));
static_assert(sizeof(linux_uring_cqe_t) == sizeof(struct io_uring_cqe));
static_assert(offsetof(linux_uring_cqe_t, res) == offsetof(struct io_uring_cqe, res));
static_assert(sizeof(linux_uring_submission_offsets_t) == sizeof(struct io_sqring_offsets));
static_assert(offsetof(linux_uring_submission_offsets_t, array_offset) == offsetof(struct io_sqring_offsets, array));
static_assert(sizeof(linux_uring_completion_offsets_t) == sizeof(struct io_cqring_offsets));
static_assert(offsetof(linux_uring_completion_offsets_t, cqes_offset) == offsetof(struct io_cqring_offsets, cqes));
static_assert(sizeof(linux_uring_params_t) == sizeof(struct io_uring_params));
static_assert(offsetof(linux_uring_params_t, features) == offsetof(struct io_uring_params, features));
static_assert(offsetof(linux_uring_params_t, cq_off) == offsetof(struct io_uring_params, cq_off));
static_assert(sizeof(linux_uring_rsrc_register_t) == sizeof(struct io_uring_rsrc_register));
static_assert(offsetof(linux_uring_rsrc_register_t, data) == offsetof(struct io_uring_rsrc_register, data));
static_assert(sizeof(linux_uring_rsrc_update_t) == sizeof(struct io_uring_rsrc_update));
static_assert(offsetof(linux_uring_rsrc_update_t, data) == offsetof(struct io_uring_rsrc_update, data));
static_assert(sizeof(linux_uring_rsrc_update2_t) == sizeof(struct io_uring_rsrc_update2));
static_assert(offsetof(linux_uring_rsrc_update2_t, nr) == offsetof(struct io_uring_rsrc_update2, nr));
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static_assert(sizeof(linux_statx_t) == sizeof(struct statx));
static_assert(offsetof(linux_statx_t, mask) == offsetof(struct statx, stx_mask));
static_assert(offsetof(linux_statx_t, dio_mem_align) == offsetof(struct statx, stx_dio_mem_align));
static_assert(offsetof(linux_statx_t, dio_offset_align) == offsetof(struct statx, stx_dio_offset_align));
#endif

} // namespace unum::ustore::test

int main() {
    fmt::println("kernel_abi: the Linux mirrors match the kernel headers, checked at compile time");
    return 0;
}
