/**
 *  @file test/tools/arrow_ipc.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Unit tests of the Arrow IPC codec in <tt>tools/arrow_ipc.hpp</tt>: schemas and batches written and
 *      read back through IPC and the C Data interface, absolute alignment, legacy prefixes, refusals, truncation,
 *      footers, nested lists and structs, and the bytes pyarrow writes.
 *
 *  The golden bytes come from pyarrow 26, so the hand-numbered FlatBuffers slots are checked against Arrow's own writer
 *  with no Python at test time.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::int32_t`, `std::int64_t`, `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <algorithm>        // `std::copy`
#include <array>            // `std::array`
#include <initializer_list> // `std::initializer_list`
#include <span>             // `std::span`
#include <string_view>      // `std::string_view`
#include <vector>           // `std::vector`

#include "arrow_ipc.hpp"

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `report`

namespace unum::ustore::tools {

/** The enumerator's own spelling of @p status, so a failed check names it. */
constexpr std::string_view format_as(ipc_status_t status) noexcept {
    switch (status) {
    case ipc_status_t::success_k: return "success_k";
    case ipc_status_t::truncated_k: return "truncated_k";
    case ipc_status_t::malformed_k: return "malformed_k";
    case ipc_status_t::unsupported_k: return "unsupported_k";
    case ipc_status_t::too_small_k: return "too_small_k";
    }
    return "unknown";
}

} // namespace unum::ustore::tools

namespace unum::ustore::test {

using namespace unum::ustore::tools;

#pragma region Golden Bytes

/** pyarrow's <tt>ipc.new_file</tt> of a @c key <tt>pa.uuid()</tt> and a @c value <tt>pa.binary()</tt> column, both
 *  non-nullable, with schema metadata <tt>ustore.modality=blobs</tt>, holding batches of keys 1 and 2 with values
 *  @c alpha and empty, then key 3 with @c omega. */
constexpr std::uint8_t pyarrow_file_k[] {
    0x41, 0x52, 0x52, 0x4f, 0x57, 0x31, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x78, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x0e, 0x00, 0x06, 0x00, 0x05, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x04, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x10, 0xff, 0xff, 0xff, 0x14, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x62, 0x6c, 0x6f,
    0x62, 0x73, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x75, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x2e, 0x6d, 0x6f, 0x64,
    0x61, 0x6c, 0x69, 0x74, 0x79, 0x00, 0x02, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x14, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x00, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00, 0x00, 0x04, 0x00, 0x06, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x18, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x14, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x14, 0x00, 0x00, 0x00, 0x9c, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x6b, 0x65, 0x79,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xc8, 0xff, 0xff, 0xff, 0x18, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x61, 0x72, 0x72, 0x6f, 0x77, 0x2e, 0x75, 0x75, 0x69,
    0x64, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73,
    0x69, 0x6f, 0x6e, 0x3a, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08,
    0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69,
    0x6f, 0x6e, 0x3a, 0x6d, 0x65, 0x74, 0x61, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xc8, 0x00, 0x00,
    0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x16, 0x00, 0x06, 0x00, 0x05, 0x00, 0x08, 0x00,
    0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x6c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xc8, 0x00, 0x00, 0x00, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x16, 0x00, 0x06, 0x00, 0x05, 0x00, 0x08, 0x00, 0x0c, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x6c, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x6f, 0x6d, 0x65, 0x67, 0x61,
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x06, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x10, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x54, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x88, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0c, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0xff, 0xff, 0xff, 0x14, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x62, 0x6c, 0x6f, 0x62, 0x73, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x75, 0x73, 0x74, 0x6f,
    0x72, 0x65, 0x2e, 0x6d, 0x6f, 0x64, 0x61, 0x6c, 0x69, 0x74, 0x79, 0x00, 0x02, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00,
    0x00, 0x14, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x06, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x18, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07,
    0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x14, 0x00,
    0x00, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x6b, 0x65, 0x79, 0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0xc8, 0xff, 0xff, 0xff, 0x18, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x61, 0x72, 0x72,
    0x6f, 0x77, 0x2e, 0x75, 0x75, 0x69, 0x64, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a,
    0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x3a, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a, 0x65,
    0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x3a, 0x6d, 0x65, 0x74, 0x61, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00,
    0x06, 0x00, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x41,
    0x52, 0x52, 0x4f, 0x57, 0x31,
};

/** The record batch message of a pyarrow stream of the same schema written with <tt>compression="lz4"</tt>, its prefix
 *  and metadata only. */
constexpr std::uint8_t pyarrow_compressed_batch_k[] {
    0xff, 0xff, 0xff, 0xff, 0xd8, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x18,
    0x00, 0x06, 0x00, 0x05, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x1c, 0x00,
    0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x1c, 0x00, 0x10,
    0x00, 0x04, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** pyarrow's stream of one @c vector column of <tt>FixedSizeList&lt;float32, 4&gt;</tt>, its child tagged
 *  <tt>numkong.f32</tt>, holding <tt>[1, 2, 3, 4]</tt> and <tt>[-0.5, 0.25, 8, 16]</tt>. */
constexpr std::uint8_t pyarrow_vectors_k[] {
    0xff, 0xff, 0xff, 0xff, 0x10, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0c, 0x00, 0x06,
    0x00, 0x05, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x14,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x14, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72,
    0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x12, 0x00, 0x18, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00,
    0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x14, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x69, 0x74, 0x65, 0x6d, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x6e, 0x75, 0x6d, 0x6b,
    0x6f, 0x6e, 0x67, 0x2e, 0x66, 0x33, 0x32, 0x00, 0x14, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a, 0x65,
    0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x3a, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00,
    0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xa8,
    0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x16, 0x00, 0x06, 0x00, 0x05, 0x00,
    0x08, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0x00,
    0xbf, 0x00, 0x00, 0x80, 0x3e, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x80, 0x41, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00,
};

/** pyarrow's stream of one @c results column of <tt>LargeList&lt;Struct&lt;key: arrow.uuid, distance:
 *  float64&gt;&gt;</tt> holding <tt>[(1, 0.5), (2, 1.5)]</tt> and <tt>[(3, 2.5)]</tt>. */
constexpr std::uint8_t pyarrow_results_k[] {
    0xff, 0xff, 0xff, 0xff, 0x98, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0c, 0x00, 0x06,
    0x00, 0x05, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0xa0, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x15, 0x14, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x72, 0x65, 0x73,
    0x75, 0x6c, 0x74, 0x73, 0x00, 0xd0, 0xff, 0xff, 0xff, 0xcc, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x0d, 0x18, 0x00,
    0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x28,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x69, 0x74, 0x65, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x69, 0x73, 0x74, 0x61, 0x6e, 0x63, 0x65, 0x00,
    0x00, 0x06, 0x00, 0x06, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x12, 0x00, 0x18, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x14, 0x00, 0x00, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x6b, 0x65, 0x79, 0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0xc8, 0xff, 0xff, 0xff, 0x18, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x61, 0x72, 0x72, 0x6f, 0x77, 0x2e, 0x75, 0x75, 0x69, 0x64, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52,
    0x4f, 0x57, 0x3a, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x3a, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x41, 0x52, 0x52, 0x4f,
    0x57, 0x3a, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f, 0x6e, 0x3a, 0x6d, 0x65, 0x74, 0x61, 0x64, 0x61, 0x74,
    0x61, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xff, 0xff,
    0xff, 0xff, 0x08, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x16, 0x00, 0x06,
    0x00, 0x05, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08,
    0x00, 0x0a, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x40, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
};

/** pyarrow's stream of one @c shape column of <tt>LargeList&lt;FixedSizeList&lt;float32, 3&gt;&gt;</tt> holding three
 *  points and one. */
constexpr std::uint8_t pyarrow_shapes_k[] {
    0xff, 0xff, 0xff, 0xff, 0xd8, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0c, 0x00, 0x06,
    0x00, 0x05, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x9c, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x15, 0x14, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x73, 0x68, 0x61,
    0x70, 0x65, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0xcc, 0xff, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x10, 0x14, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2c,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x69, 0x74, 0x65, 0x6d, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x14, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00, 0x0c,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x00, 0x00, 0x1c, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x69, 0x74, 0x65, 0x6d, 0x00,
    0x00, 0x06, 0x00, 0x08, 0x00, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xd8, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x16, 0x00, 0x06, 0x00, 0x05,
    0x00, 0x08, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00, 0x48, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
};

#pragma endregion Golden Bytes

#pragma region Harness

std::span<std::byte const> bytes_of_text(std::string_view text) noexcept {
    return {reinterpret_cast<std::byte const*>(text.data()), text.size()};
}

std::string_view text_of(std::span<std::byte const> bytes) noexcept {
    return {reinterpret_cast<char const*>(bytes.data()), bytes.size()};
}

/** The 16 bytes of the UUID spelling @p integer, big-endian in its last 8 bytes as <tt>uuid.UUID(int=n).bytes</tt>. */
std::array<std::byte, 16> uuid_bytes(std::uint64_t integer) noexcept {
    std::array<std::byte, 16> bytes {};
    for (std::size_t index = 0; index != 8; ++index) bytes[15 - index] = std::byte((integer >> (8 * index)) & 0xFF);
    return bytes;
}

/** A column of byte strings with offsets of @p offset_type_, owned by the test. */
template <typename offset_type_>
struct strings_column {
    std::vector<offset_type_> offsets {0};
    std::vector<std::byte> data;

    void push_back(std::string_view text) {
        for (char const character : text) data.push_back(std::byte(character));
        offsets.push_back(static_cast<offset_type_>(data.size()));
    }
    std::span<std::byte const> offsets_bytes() const noexcept { return std::as_bytes(std::span(offsets)); }
    std::span<std::byte const> data_bytes() const noexcept { return data; }
};

/** Texts cycled through the rows, with an empty one and an inner zero byte. */
constexpr std::string_view row_texts_k[] {"alpha", "", std::string_view {"zero\0byte", 9}, "omega", "\xCF\x80"};

/** Fields of every type the codec handles, the key tagged as <tt>arrow.uuid</tt>. */
struct schema_fixture_t {
    std::array<ipc_key_value_t, 1> key_metadata {{{ipc_extension_name_key_k, ipc_extension_uuid_k}}};
    std::array<ipc_key_value_t, 2> schema_metadata {{{"ustore.modality", "blobs"}, {"empty", ""}}};
    std::array<ipc_field_t, 7> fields {{
        {.name = "key", .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = key_metadata},
        {.name = "value", .type = ipc_type_t::binary_k},
        {.name = "text", .type = ipc_type_t::utf8_k, .nullability = ipc_nullability_t::nullable_k},
        {.name = "large_value", .type = ipc_type_t::large_binary_k},
        {.name = "large_text", .type = ipc_type_t::large_utf8_k},
        {.name = "count", .type = ipc_type_t::int_k, .width = 64},
        {.name = "delta", .type = ipc_type_t::int_k, .width = 64, .signedness = ipc_signedness_t::signed_k},
    }};
    ipc_schema_t schema {fields, schema_metadata};

    schema_fixture_t() = default;
    schema_fixture_t(schema_fixture_t const&) = delete;
    schema_fixture_t& operator=(schema_fixture_t const&) = delete;
};

/** A batch of @p rows over @c schema_fixture_t, starting at key @p first_key, whose buffers the test owns. */
struct batch_fixture_t {
    std::vector<std::byte> keys;
    strings_column<std::int32_t> values, texts;
    strings_column<std::int64_t> large_values, large_texts;
    std::vector<std::uint64_t> counts;
    std::vector<std::int64_t> deltas;
    std::array<ipc_node_t, 7> nodes {};
    std::array<std::span<std::byte const>, 18> buffers {};
    ipc_batch_t batch {};

    batch_fixture_t(std::uint64_t first_key, std::size_t rows) {
        for (std::size_t row = 0; row != rows; ++row) {
            std::array<std::byte, 16> const key = uuid_bytes(first_key + row);
            keys.insert(keys.end(), key.begin(), key.end());
            std::string_view const text = row_texts_k[(first_key + row) % std::size(row_texts_k)];
            values.push_back(text), texts.push_back(text), large_values.push_back(text), large_texts.push_back(text);
            counts.push_back(first_key + row);
            deltas.push_back(-static_cast<std::int64_t>(row));
        }
        for (ipc_node_t& node : nodes) node = {static_cast<std::int64_t>(rows), 0};
        buffers = {{
            {},
            keys, //
            {},
            values.offsets_bytes(),
            values.data_bytes(), //
            {},
            texts.offsets_bytes(),
            texts.data_bytes(), //
            {},
            large_values.offsets_bytes(),
            large_values.data_bytes(), //
            {},
            large_texts.offsets_bytes(),
            large_texts.data_bytes(), //
            {},
            std::as_bytes(std::span(counts)), //
            {},
            std::as_bytes(std::span(deltas)), //
        }};
        batch = {static_cast<std::int64_t>(rows), nodes, buffers};
    }
    batch_fixture_t(batch_fixture_t const&) = delete;
    batch_fixture_t& operator=(batch_fixture_t const&) = delete;
};

/** A stream or file assembled in memory as a @c writev caller would, with the Block of every batch. */
struct assembled_t {
    std::vector<std::byte> bytes;
    std::vector<ipc_block_t> blocks;
    ipc_options_t options;
};

/** A file so far: the magic, ready for the stream. */
assembled_t file_start(ipc_options_t options = {}) {
    assembled_t file;
    file.options = options;
    std::span<std::byte const> const magic = bytes_of_text(ipc_file_magic_k);
    file.bytes.assign(magic.begin(), magic.end());
    return file;
}

/** Appends a schema message, measured first through an empty output. */
std::size_t append_schema(assembled_t& out, ipc_schema_t const& schema) {
    std::size_t failures = 0, needed = 0, written = 0;
    std::size_t const start = out.bytes.size();
    failures += expect_eq(ipc_write_schema(schema, out.options, start, {}, needed), ipc_status_t::too_small_k);
    out.bytes.resize(start + needed);
    failures += expect_eq(ipc_write_schema(schema, out.options, start, std::span(out.bytes).subspan(start), written),
                          ipc_status_t::success_k);
    failures += expect_eq(written, needed);
    failures += expect_eq(out.bytes.size() % out.options.alignment, 0u);
    return failures;
}

/** Appends a batch message and its body gathered with zero padding, checking the gathers alias the batch's buffers. */
std::size_t append_batch(assembled_t& out, ipc_schema_t const& schema, ipc_batch_t const& batch) {
    std::size_t failures = 0, needed = 0, written = 0;
    std::uint64_t body_bytes = 0;
    std::array<ipc_gather_t, ipc_buffers_max_k> gathers {};
    std::size_t const start = out.bytes.size();
    failures += expect_eq(ipc_write_batch(schema, batch, out.options, start, {}, needed, gathers, body_bytes),
                          ipc_status_t::too_small_k);
    failures += expect_eq(ipc_write_batch(schema, batch, out.options, start, {}, needed, {}, body_bytes),
                          ipc_status_t::too_small_k);
    out.bytes.resize(start + needed);
    failures += expect_eq(ipc_write_batch(schema, batch, out.options, start, std::span(out.bytes).subspan(start),
                                          written, gathers, body_bytes),
                          ipc_status_t::success_k);
    failures += expect_eq(written, needed);
    std::uint64_t gathered = 0;
    for (std::size_t index = 0; index != batch.buffers.size(); ++index) {
        ipc_gather_t const& gather = gathers[index];
        failures += expect(gather.bytes.data() == batch.buffers[index].data());
        failures += expect_eq(gather.bytes.size(), batch.buffers[index].size());
        out.bytes.insert(out.bytes.end(), gather.bytes.begin(), gather.bytes.end());
        out.bytes.resize(out.bytes.size() + gather.padding);
        gathered += gather.bytes.size() + gather.padding;
    }
    failures += expect_eq(gathered, body_bytes);
    out.blocks.push_back(
        {static_cast<std::int64_t>(start), static_cast<std::int32_t>(written), static_cast<std::int64_t>(body_bytes)});
    return failures;
}

/** Appends the end-of-stream marker. */
std::size_t append_end_of_stream(assembled_t& out) {
    std::size_t written = 0;
    std::size_t const start = out.bytes.size();
    out.bytes.resize(start + 8);
    return expect_eq(ipc_write_end_of_stream(std::span(out.bytes).subspan(start), written), ipc_status_t::success_k) +
           expect_eq(written, 8u);
}

/** Appends the footer of a file, measured first through an empty output. */
std::size_t append_footer(assembled_t& out, ipc_schema_t const& schema) {
    std::size_t failures = 0, needed = 0, written = 0;
    failures += expect_eq(ipc_write_footer(schema, out.blocks, {}, needed), ipc_status_t::too_small_k);
    std::size_t const start = out.bytes.size();
    out.bytes.resize(start + needed);
    failures += expect_eq(ipc_write_footer(schema, out.blocks, std::span(out.bytes).subspan(start), written),
                          ipc_status_t::success_k);
    failures += expect_eq(written, needed);
    return failures;
}

/** What a schema reads into. */
struct schema_storage_t {
    std::array<ipc_field_t, ipc_fields_max_k> fields {};
    std::array<ipc_key_value_t, 32> metadata {};
    ipc_schema_t schema {};
};

/** The first @p count bytes of @p bytes past @p start, or fewer where they end, as a reader of a cut input gets. */
std::span<std::byte const> clipped(std::span<std::byte const> bytes, std::size_t start, std::size_t count) noexcept {
    if (start >= bytes.size()) return {};
    return bytes.subspan(start, count < bytes.size() - start ? count : bytes.size() - start);
}

/** Reads a stream from the start of @p bytes as a pipe reader would, taking exactly the bytes each step names, and
 *  hands every validated batch with its prefix width to @p visit; answers the first status that is not a success. */
template <typename visit_type_>
ipc_status_t read_stream(std::span<std::byte const> bytes, schema_storage_t& storage, visit_type_&& visit) {
    std::size_t cursor = 0;
    for (;;) {
        std::uint32_t metadata_bytes = 0;
        std::size_t prefix_bytes = 0;
        ipc_status_t status = ipc_read_prefix(clipped(bytes, cursor, 4), metadata_bytes, prefix_bytes);
        if (status == ipc_status_t::truncated_k && prefix_bytes == 8)
            status = ipc_read_prefix(clipped(bytes, cursor, 8), metadata_bytes, prefix_bytes);
        if (status != ipc_status_t::success_k) return status;
        if (metadata_bytes == 0) return ipc_status_t::success_k;
        cursor += prefix_bytes;
        ipc_message_t message;
        status = ipc_read_message(clipped(bytes, cursor, metadata_bytes), message);
        cursor += metadata_bytes;
        if (status != ipc_status_t::success_k) return status;
        if (message.kind == ipc_message_kind_t::schema_k) {
            status = ipc_read_schema(message, storage.fields, storage.metadata, storage.schema);
            if (status != ipc_status_t::success_k) return status;
            continue;
        }
        std::array<ipc_node_t, ipc_fields_max_k> nodes {};
        std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
        ipc_batch_t batch;
        std::size_t const body_bytes = static_cast<std::size_t>(message.body_bytes);
        status = ipc_read_batch(message, clipped(bytes, cursor, body_bytes), storage.schema, nodes, buffers, batch);
        cursor += body_bytes;
        if (status != ipc_status_t::success_k) return status;
        status = ipc_validate_batch(storage.schema, batch);
        if (status != ipc_status_t::success_k) return status;
        visit(batch, prefix_bytes);
    }
}

/** Counts the pairs of metadata that differ. */
std::size_t compare_metadata(std::span<ipc_key_value_t const> actual, std::span<ipc_key_value_t const> wanted) {
    std::size_t failures = expect_eq(actual.size(), wanted.size());
    for (std::size_t pair = 0; pair != wanted.size() && pair != actual.size(); ++pair) {
        failures += expect_eq(actual[pair].key, wanted[pair].key);
        failures += expect_eq(actual[pair].value, wanted[pair].value);
    }
    return failures;
}

/** Counts the fields that differ, their metadata and children included. */
std::size_t compare_fields(std::span<ipc_field_t const> actual, std::span<ipc_field_t const> wanted) {
    std::size_t failures = expect_eq(actual.size(), wanted.size());
    for (std::size_t index = 0; index != wanted.size() && index != actual.size(); ++index) {
        ipc_field_t const &got = actual[index], &field = wanted[index];
        failures += expect_eq(got.name, field.name);
        failures += expect(got.type == field.type);
        failures += expect_eq(got.width, field.width);
        failures += expect(got.signedness == field.signedness);
        failures += expect(got.nullability == field.nullability);
        failures += compare_metadata(got.metadata, field.metadata);
        failures += compare_fields(got.children, field.children);
    }
    return failures;
}

/** Counts the fields of two schemas that differ, metadata included. */
std::size_t compare_schemas(ipc_schema_t const& actual, ipc_schema_t const& wanted) {
    return compare_metadata(actual.metadata, wanted.metadata) + compare_fields(actual.fields, wanted.fields);
}

/** Counts the buffers and nodes of two batches whose bytes differ. */
std::size_t compare_batches(ipc_batch_t const& actual, ipc_batch_t const& wanted) {
    std::size_t failures = expect_eq(actual.length, wanted.length);
    failures += expect_eq(actual.nodes.size(), wanted.nodes.size());
    failures += expect_eq(actual.buffers.size(), wanted.buffers.size());
    if (failures) return failures;
    for (std::size_t index = 0; index != wanted.nodes.size(); ++index) {
        failures += expect_eq(actual.nodes[index].length, wanted.nodes[index].length);
        failures += expect_eq(actual.nodes[index].null_count, wanted.nodes[index].null_count);
    }
    for (std::size_t index = 0; index != wanted.buffers.size(); ++index)
        failures += expect_eq(text_of(actual.buffers[index]), text_of(wanted.buffers[index]));
    return failures;
}

#pragma endregion Harness

#pragma region Cases

/** A schema of every type survives IPC, then the C Data interface, and its release reaches every child. */
std::size_t schema_round_trip() {
    std::size_t failures = 0;
    schema_fixture_t const fixture;
    assembled_t stream;
    failures += append_schema(stream, fixture.schema);
    failures += append_end_of_stream(stream);

    schema_storage_t read;
    failures += expect_eq(read_stream(stream.bytes, read, [](ipc_batch_t const&, std::size_t) {}),
                          ipc_status_t::success_k);
    failures += compare_schemas(read.schema, fixture.schema);

    std::array<char, 512> text {};
    ipc_arrow_storage_t storage {};
    ArrowSchema arrow {};
    failures += expect_eq(ipc_schema_to_arrow(read.schema, storage, arrow), ipc_status_t::too_small_k);
    storage.text = text;
    failures += expect_eq(ipc_schema_to_arrow(read.schema, storage, arrow), ipc_status_t::success_k);
    failures += expect_eq(std::string_view(arrow.format), "+s");
    constexpr std::string_view formats[] {"w:16", "z", "u", "Z", "U", "L", "l"};
    for (std::size_t index = 0; index != std::size(formats); ++index)
        failures += expect_eq(std::string_view(arrow.children[index]->format), formats[index]);
    failures += expect_eq(arrow.children[2]->flags, ARROW_FLAG_NULLABLE);

    schema_storage_t imported;
    failures += expect_eq(ipc_schema_from_arrow(arrow, imported.fields, imported.metadata, imported.schema),
                          ipc_status_t::success_k);
    failures += compare_schemas(imported.schema, fixture.schema);
    arrow.release(&arrow);
    failures += expect(arrow.release == nullptr && storage.schema_children[6].release == nullptr);
    return failures;
}

/** A batch of every type survives IPC with its body never copied, then the C Data interface back to the same metadata
 *  bytes. */
std::size_t batch_round_trip() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const fixture(1, 5);
    assembled_t stream;
    failures += append_schema(stream, schema.schema);
    failures += append_batch(stream, schema.schema, fixture.batch);
    failures += append_end_of_stream(stream);

    schema_storage_t read;
    std::size_t batches = 0;
    failures += expect_eq(read_stream(stream.bytes, read,
                                      [&](ipc_batch_t const& batch, std::size_t) {
                                          ++batches;
                                          failures += compare_batches(batch, fixture.batch);

                                          ipc_arrow_storage_t storage {};
                                          ArrowArray arrow {};
                                          failures += expect_eq(ipc_batch_to_arrow(read.schema, batch, storage, arrow),
                                                                ipc_status_t::success_k);
                                          failures += expect_eq(arrow.length, 5);
                                          failures += expect_eq(arrow.children[1]->n_buffers, 3);
                                          failures += expect(arrow.children[1]->buffers[1] == batch.buffers[3].data());

                                          std::array<ipc_node_t, ipc_fields_max_k> nodes {};
                                          std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
                                          ipc_batch_t imported;
                                          failures += expect_eq(
                                              ipc_batch_from_arrow(read.schema, arrow, nodes, buffers, imported),
                                              ipc_status_t::success_k);
                                          failures += compare_batches(imported, fixture.batch);
                                          arrow.release(&arrow);
                                          failures += expect(storage.array_children[0].release == nullptr);
                                      }),
                          ipc_status_t::success_k);
    failures += expect_eq(batches, 1u);
    return failures;
}

/** Offsets that fall, offsets past their data and nulls are each refused. */
std::size_t validation_refuses() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const fixture(1, 3);
    failures += expect_eq(ipc_validate_batch(schema.schema, fixture.batch), ipc_status_t::success_k);

    std::array<std::span<std::byte const>, 18> buffers = fixture.buffers;
    ipc_batch_t batch = fixture.batch;
    batch.buffers = buffers;
    std::int32_t const falling[] {0, 5, 3, 9};
    buffers[3] = std::as_bytes(std::span(falling));
    failures += expect_eq(ipc_validate_batch(schema.schema, batch), ipc_status_t::malformed_k);
    std::int32_t const overlong[] {0, 5, 5, 400};
    buffers[3] = std::as_bytes(std::span(overlong));
    failures += expect_eq(ipc_validate_batch(schema.schema, batch), ipc_status_t::malformed_k);
    buffers[3] = fixture.buffers[3];
    buffers[1] = buffers[1].first(16);
    failures += expect_eq(ipc_validate_batch(schema.schema, batch), ipc_status_t::malformed_k);

    std::array<ipc_node_t, 7> nodes = fixture.nodes;
    ipc_batch_t nullable = fixture.batch;
    nullable.nodes = nodes;
    nodes[2].null_count = 1;
    failures += expect_eq(ipc_validate_batch(schema.schema, nullable), ipc_status_t::unsupported_k);
    return failures;
}

/** With the direct alignment every body and buffer starts on a page at its absolute file offset. */
std::size_t direct_alignment_absolute() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const first(1, 7), second(8, 2);
    assembled_t file = file_start({ipc_alignment_direct_k});
    failures += append_schema(file, schema.schema);
    failures += append_batch(file, schema.schema, first.batch);
    failures += append_batch(file, schema.schema, second.batch);
    failures += append_end_of_stream(file);
    failures += append_footer(file, schema.schema);
    for (ipc_block_t const& block : file.blocks) {
        failures += expect_eq(block.offset % ipc_alignment_direct_k, 0);
        failures += expect_eq((block.offset + block.metadata_bytes) % ipc_alignment_direct_k, 0);
        failures += expect_eq(block.body_bytes % ipc_alignment_direct_k, 0);
    }

    std::span<std::byte const> const bytes = file.bytes;
    schema_storage_t read;
    std::size_t buffers = 0;
    failures += expect_eq(read_stream(bytes.subspan(ipc_file_magic_k.size()), read,
                                      [&](ipc_batch_t const& batch, std::size_t) {
                                          for (std::span<std::byte const> buffer : batch.buffers) {
                                              std::size_t const offset = std::size_t(buffer.data() - bytes.data());
                                              failures += expect_eq(offset % ipc_alignment_direct_k, 0u);
                                              ++buffers;
                                          }
                                      }),
                          ipc_status_t::success_k);
    failures += expect_eq(buffers, 2 * first.buffers.size());

    std::size_t needed = 0;
    std::uint64_t body_bytes = 0;
    failures += expect_eq(ipc_write_schema(schema.schema, {48}, 0, {}, needed), ipc_status_t::unsupported_k);
    failures += expect_eq(ipc_write_batch(schema.schema, first.batch, {}, 4, {}, needed, {}, body_bytes),
                          ipc_status_t::unsupported_k);
    return failures;
}

/** Streams with the bare 4-byte prefixes of writers before format 0.15 read the same. */
std::size_t legacy_prefix() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const fixture(3, 4);
    assembled_t stream;
    failures += append_schema(stream, schema.schema);
    failures += append_batch(stream, schema.schema, fixture.batch);
    failures += append_end_of_stream(stream);

    // Drop each marker, so every length stands bare and the stream ends with 4 zero bytes
    std::vector<std::byte> legacy;
    std::span<std::byte const> const bytes = stream.bytes;
    for (std::size_t cursor = 0; cursor < bytes.size();) {
        std::uint32_t const length = ipc_load_little<std::uint32_t>(bytes.data() + cursor + 4);
        ipc_message_t message;
        std::uint64_t body_bytes = 0;
        if (length && ipc_read_message(bytes.subspan(cursor + 8, length), message) == ipc_status_t::success_k)
            body_bytes = static_cast<std::uint64_t>(message.body_bytes);
        legacy.insert(legacy.end(), bytes.begin() + std::ptrdiff_t(cursor + 4),
                      bytes.begin() + std::ptrdiff_t(cursor + 8 + length + body_bytes));
        cursor += 8 + length + body_bytes;
    }
    failures += expect_eq(legacy.size(), stream.bytes.size() - 4 * 3);

    schema_storage_t read;
    std::size_t batches = 0;
    failures += expect_eq(read_stream(legacy, read,
                                      [&](ipc_batch_t const& batch, std::size_t prefix_bytes) {
                                          ++batches;
                                          failures += expect_eq(prefix_bytes, 4u);
                                          failures += compare_batches(batch, fixture.batch);
                                      }),
                          ipc_status_t::success_k);
    failures += expect_eq(batches, 1u);
    failures += compare_schemas(read.schema, schema.schema);
    return failures;
}

/** A batch pyarrow compressed with LZ4 is refused, with no body needed to tell. */
std::size_t compression_unsupported() {
    std::size_t failures = 0;
    std::span<std::byte const> const golden = std::as_bytes(std::span(pyarrow_compressed_batch_k));
    std::span<std::byte const> const file = std::as_bytes(std::span(pyarrow_file_k));
    schema_storage_t read;
    std::array<ipc_block_t, 4> blocks {};
    std::size_t blocks_count = 0;
    failures += expect_eq(ipc_read_footer(file, read.fields, read.metadata, read.schema, blocks, blocks_count),
                          ipc_status_t::success_k);

    std::uint32_t metadata_bytes = 0;
    std::size_t prefix_bytes = 0;
    failures += expect_eq(ipc_read_prefix(golden, metadata_bytes, prefix_bytes), ipc_status_t::success_k);
    failures += expect_eq(prefix_bytes + metadata_bytes, golden.size());
    ipc_message_t message;
    failures += expect_eq(ipc_read_message(golden.subspan(prefix_bytes), message), ipc_status_t::success_k);
    failures += expect(message.kind == ipc_message_kind_t::record_batch_k);
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_batch_t batch;
    failures += expect_eq(ipc_read_batch(message, {}, read.schema, nodes, buffers, batch), ipc_status_t::unsupported_k);
    return failures;
}

/** A schema that declares big-endian buffers is refused. */
std::size_t big_endian_unsupported() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    assembled_t stream;
    failures += append_schema(stream, schema.schema);
    std::span<std::byte> const metadata = std::span(stream.bytes).subspan(8);

    ipc_message_t message;
    failures += expect_eq(ipc_read_message(metadata, message), ipc_status_t::success_k);
    ipc_status_t status = ipc_status_t::success_k;
    std::size_t const endianness = message.header.field(ipc_schema_endianness_k, 2, status);
    failures += expect_eq(status, ipc_status_t::success_k);
    failures += expect(endianness != 0);
    metadata[endianness] = std::byte {1};

    schema_storage_t read;
    failures += expect_eq(ipc_read_message(metadata, message), ipc_status_t::success_k);
    failures += expect_eq(ipc_read_schema(message, read.fields, read.metadata, read.schema),
                          ipc_status_t::unsupported_k);
    return failures;
}

/** Every cut of a stream and of a file reads as truncated, never as a success or as malformed. */
std::size_t truncated_input() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const first(1, 3), second(4, 1);
    assembled_t file = file_start();
    failures += append_schema(file, schema.schema);
    failures += append_batch(file, schema.schema, first.batch);
    failures += append_batch(file, schema.schema, second.batch);
    failures += append_end_of_stream(file);
    std::size_t const stream_end = file.bytes.size();
    failures += append_footer(file, schema.schema);

    std::span<std::byte const> const stream = std::span<std::byte const>(file.bytes).subspan(8, stream_end - 8);
    schema_storage_t read;
    failures += expect_eq(read_stream(stream, read, [](ipc_batch_t const&, std::size_t) {}), ipc_status_t::success_k);
    for (std::size_t cut = 0; cut != stream.size(); ++cut)
        failures += expect_eq(read_stream(stream.first(cut), read, [](ipc_batch_t const&, std::size_t) {}),
                              ipc_status_t::truncated_k);

    std::array<ipc_block_t, 4> blocks {};
    std::size_t blocks_count = 0;
    for (std::size_t cut = 0; cut != file.bytes.size(); ++cut)
        failures += expect_eq(ipc_read_footer(std::span<std::byte const>(file.bytes).first(cut), read.fields,
                                              read.metadata, read.schema, blocks, blocks_count),
                              ipc_status_t::truncated_k);
    return failures;
}

/** The footer answers the Blocks the writer recorded, and each one reads its batch. */
std::size_t footer_blocks_match() {
    std::size_t failures = 0;
    schema_fixture_t const schema;
    batch_fixture_t const first(1, 3), second(4, 1), third(5, 0);
    batch_fixture_t const* const fixtures[] {&first, &second, &third};
    assembled_t file = file_start();
    failures += append_schema(file, schema.schema);
    for (batch_fixture_t const* fixture : fixtures) failures += append_batch(file, schema.schema, fixture->batch);
    failures += append_end_of_stream(file);
    failures += append_footer(file, schema.schema);

    std::span<std::byte const> const bytes = file.bytes;
    schema_storage_t read;
    std::array<ipc_block_t, 2> short_blocks {};
    std::array<ipc_block_t, 8> blocks {};
    std::size_t blocks_count = 0;
    failures += expect_eq(ipc_read_footer(bytes, read.fields, read.metadata, read.schema, short_blocks, blocks_count),
                          ipc_status_t::too_small_k);
    failures += expect_eq(blocks_count, 3u);
    failures += expect_eq(ipc_read_footer(bytes, read.fields, read.metadata, read.schema, blocks, blocks_count),
                          ipc_status_t::success_k);
    failures += compare_schemas(read.schema, schema.schema);
    failures += expect_eq(blocks_count, file.blocks.size());
    for (std::size_t index = 0; index != blocks_count && index != file.blocks.size(); ++index) {
        ipc_block_t const& block = blocks[index];
        failures += expect_eq(block.offset, file.blocks[index].offset);
        failures += expect_eq(block.metadata_bytes, file.blocks[index].metadata_bytes);
        failures += expect_eq(block.body_bytes, file.blocks[index].body_bytes);

        std::uint32_t metadata_bytes = 0;
        std::size_t prefix_bytes = 0;
        std::size_t const offset = static_cast<std::size_t>(block.offset);
        failures += expect_eq(ipc_read_prefix(bytes.subspan(offset), metadata_bytes, prefix_bytes),
                              ipc_status_t::success_k);
        ipc_message_t message;
        failures += expect_eq(ipc_read_message(bytes.subspan(offset + prefix_bytes, metadata_bytes), message),
                              ipc_status_t::success_k);
        std::array<ipc_node_t, ipc_fields_max_k> nodes {};
        std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
        ipc_batch_t batch;
        failures += expect_eq(ipc_read_batch(message,
                                             bytes.subspan(offset + static_cast<std::size_t>(block.metadata_bytes),
                                                           static_cast<std::size_t>(block.body_bytes)),
                                             read.schema, nodes, buffers, batch),
                              ipc_status_t::success_k);
        failures += expect_eq(ipc_validate_batch(read.schema, batch), ipc_status_t::success_k);
        failures += compare_batches(batch, fixtures[index]->batch);
    }
    return failures;
}

/** A file pyarrow wrote reads through its footer and as a stream, with the UUID extension tag and every value. */
std::size_t pyarrow_golden() {
    std::size_t failures = 0;
    std::span<std::byte const> const file = std::as_bytes(std::span(pyarrow_file_k));
    schema_storage_t read;
    std::array<ipc_block_t, 4> blocks {};
    std::size_t blocks_count = 0;
    failures += expect_eq(ipc_read_footer(file, read.fields, read.metadata, read.schema, blocks, blocks_count),
                          ipc_status_t::success_k);
    failures += expect_eq(blocks_count, 2u);
    failures += expect_eq(blocks[0].offset, 392);
    failures += expect_eq(blocks[0].metadata_bytes, 208);
    failures += expect_eq(blocks[0].body_bytes, 56);
    failures += expect_eq(blocks[1].offset, 656);

    std::array<ipc_key_value_t, 2> const key_metadata {
        {{"ARROW:extension:metadata", ""}, {ipc_extension_name_key_k, ipc_extension_uuid_k}}};
    std::array<ipc_key_value_t, 1> const schema_metadata {{{"ustore.modality", "blobs"}}};
    std::array<ipc_field_t, 2> const fields {{
        {.name = "key", .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = key_metadata},
        {.name = "value", .type = ipc_type_t::binary_k},
    }};
    failures += compare_schemas(read.schema, {fields, schema_metadata});

    std::array<std::uint64_t, 3> const keys {1, 2, 3};
    std::array<std::string_view, 3> const values {"alpha", "", "omega"};
    std::size_t row = 0;
    schema_storage_t streamed;
    failures += expect_eq(
        read_stream(file.subspan(ipc_file_magic_k.size()), streamed,
                    [&](ipc_batch_t const& batch, std::size_t prefix_bytes) {
                        failures += expect_eq(prefix_bytes, 8u);
                        std::span<std::byte const> const key_bytes = batch.buffers[1], offsets = batch.buffers[3];
                        for (std::int64_t index = 0; index != batch.length && row != keys.size(); ++index, ++row) {
                            std::array<std::byte, 16> const key = uuid_bytes(keys[row]);
                            failures += expect(std::memcmp(key_bytes.data() + 16 * index, key.data(), 16) == 0);
                            std::int32_t const begin = ipc_load_little<std::int32_t>(offsets.data() + 4 * index);
                            std::int32_t const end = ipc_load_little<std::int32_t>(offsets.data() + 4 * index + 4);
                            failures += expect_eq(
                                text_of(batch.buffers[4].subspan(std::size_t(begin), std::size_t(end - begin))),
                                values[row]);
                        }
                    }),
        ipc_status_t::success_k);
    failures += expect_eq(row, 3u);
    failures += compare_schemas(streamed.schema, read.schema);
    return failures;
}

/** Reads a float of @p scalar_type_ at @p index of @p bytes, whatever its alignment. */
template <typename scalar_type_>
scalar_type_ read_value(std::span<std::byte const> bytes, std::size_t index) noexcept {
    scalar_type_ value {};
    std::memcpy(&value, bytes.data() + index * sizeof(scalar_type_), sizeof(scalar_type_));
    return value;
}

/** Writes @p batch of @p schema as a stream with the codec and reads it back, then passes it through the C Data
 *  interface and back, counting every field, node and buffer that changed on either way. */
std::size_t nested_round_trips(ipc_schema_t const& schema, ipc_batch_t const& batch) {
    std::size_t failures = 0;
    assembled_t stream;
    failures += append_schema(stream, schema);
    failures += append_batch(stream, schema, batch);
    failures += append_end_of_stream(stream);
    schema_storage_t read;
    std::size_t batches = 0;
    failures += expect_eq(read_stream(stream.bytes, read,
                                      [&](ipc_batch_t const& written, std::size_t) {
                                          ++batches;
                                          failures += compare_batches(written, batch);
                                      }),
                          ipc_status_t::success_k);
    failures += expect_eq(batches, 1u);
    failures += compare_schemas(read.schema, schema);

    std::array<char, 1024> text {};
    ipc_arrow_storage_t storage {};
    storage.text = text;
    ArrowSchema arrow_schema {};
    ArrowArray arrow_array {};
    failures += expect_eq(ipc_schema_to_arrow(schema, storage, arrow_schema), ipc_status_t::success_k);
    failures += expect_eq(ipc_batch_to_arrow(schema, batch, storage, arrow_array), ipc_status_t::success_k);
    schema_storage_t imported;
    failures += expect_eq(ipc_schema_from_arrow(arrow_schema, imported.fields, imported.metadata, imported.schema),
                          ipc_status_t::success_k);
    failures += compare_schemas(imported.schema, schema);
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_batch_t imported_batch;
    failures += expect_eq(ipc_batch_from_arrow(imported.schema, arrow_array, nodes, buffers, imported_batch),
                          ipc_status_t::success_k);
    failures += compare_batches(imported_batch, batch);
    arrow_array.release(&arrow_array);
    arrow_schema.release(&arrow_schema);
    return failures;
}

/** Reads the first batch of the golden stream @p golden with the codec into @p read, @p nodes and @p buffers. */
std::size_t read_golden(std::span<std::byte const> golden, schema_storage_t& read,
                        std::array<ipc_node_t, ipc_fields_max_k>& nodes,
                        std::array<std::span<std::byte const>, ipc_buffers_max_k>& buffers, ipc_batch_t& batch) {
    std::size_t batches = 0;
    std::size_t failures = expect_eq(read_stream(golden, read,
                                                 [&](ipc_batch_t const& found, std::size_t) {
                                                     ++batches;
                                                     std::copy(found.nodes.begin(), found.nodes.end(), nodes.begin());
                                                     std::copy(found.buffers.begin(), found.buffers.end(),
                                                               buffers.begin());
                                                     batch = {found.length, std::span(nodes).first(found.nodes.size()),
                                                              std::span(buffers).first(found.buffers.size())};
                                                 }),
                                     ipc_status_t::success_k);
    return failures + expect_eq(batches, 1u);
}

/** pyarrow's <tt>FixedSizeList&lt;float32, 4&gt;</tt> vector column, its child tagged <tt>numkong.f32</tt>, reads with
 *  its child's metadata and round-trips. */
std::size_t nested_vectors_golden() {
    std::size_t failures = 0;
    schema_storage_t read;
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_batch_t batch;
    failures += read_golden(std::as_bytes(std::span(pyarrow_vectors_k)), read, nodes, buffers, batch);
    if (failures) return failures;
    constexpr ipc_key_value_t item_metadata[] {{ipc_extension_name_key_k, "numkong.f32"}};
    ipc_field_t const item {.name = "item",
                            .type = ipc_type_t::floating_point_k,
                            .width = 32,
                            .metadata = item_metadata};
    ipc_field_t const vector {.name = "vector",
                              .type = ipc_type_t::fixed_size_list_k,
                              .width = 4,
                              .children = {&item, 1}};
    failures += compare_schemas(read.schema, {{&vector, 1}, {}});
    failures += expect_eq(batch.length, 2);
    failures += expect_eq(batch.nodes.size(), 2u);
    failures += expect_eq(batch.nodes[1].length, 8);
    constexpr float wanted[] {1.0f, 2.0f, 3.0f, 4.0f, -0.5f, 0.25f, 8.0f, 16.0f};
    for (std::size_t index = 0; index != std::size(wanted); ++index)
        failures += expect_eq(read_value<float>(batch.buffers[2], index), wanted[index]);
    return failures + nested_round_trips(read.schema, batch);
}

/** pyarrow's <tt>LargeList&lt;Struct&lt;key: arrow.uuid, distance: float64&gt;&gt;</tt> search results read row by row
 *  and round-trip. */
std::size_t nested_results_golden() {
    std::size_t failures = 0;
    schema_storage_t read;
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_batch_t batch;
    failures += read_golden(std::as_bytes(std::span(pyarrow_results_k)), read, nodes, buffers, batch);
    if (failures) return failures;
    constexpr ipc_key_value_t key_metadata[] {{"ARROW:extension:metadata", ""},
                                              {ipc_extension_name_key_k, ipc_extension_uuid_k}};
    ipc_field_t const hit[] {
        {.name = "key", .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = key_metadata},
        {.name = "distance", .type = ipc_type_t::floating_point_k, .width = 64},
    };
    ipc_field_t const item {.name = "item", .type = ipc_type_t::struct_k, .children = hit};
    ipc_field_t const results {.name = "results", .type = ipc_type_t::large_list_k, .children = {&item, 1}};
    failures += compare_schemas(read.schema, {{&results, 1}, {}});
    // Depth-first: the list, its struct, then the key and the distance, each with its validity bitmap first
    failures += expect_eq(batch.nodes.size(), 4u);
    failures += expect_eq(batch.buffers.size(), 2u + 1u + 2u + 2u);
    failures += expect_eq(batch.nodes[1].length, 3);
    constexpr std::int64_t offsets[] {0, 2, 3};
    constexpr double distances[] {0.5, 1.5, 2.5};
    for (std::size_t index = 0; index != std::size(offsets); ++index)
        failures += expect_eq(read_value<std::int64_t>(batch.buffers[1], index), offsets[index]);
    for (std::size_t index = 0; index != std::size(distances); ++index) {
        std::array<std::byte, 16> const key = uuid_bytes(index + 1);
        failures += expect(std::memcmp(batch.buffers[4].data() + 16 * index, key.data(), 16) == 0);
        failures += expect_eq(read_value<double>(batch.buffers[6], index), distances[index]);
    }
    return failures + nested_round_trips(read.schema, batch);
}

/** pyarrow's <tt>LargeList&lt;FixedSizeList&lt;float32, 3&gt;&gt;</tt> shapes read and round-trip. */
std::size_t nested_shapes_golden() {
    std::size_t failures = 0;
    schema_storage_t read;
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_batch_t batch;
    failures += read_golden(std::as_bytes(std::span(pyarrow_shapes_k)), read, nodes, buffers, batch);
    if (failures) return failures;
    ipc_field_t const coordinate {.name = "item", .type = ipc_type_t::floating_point_k, .width = 32};
    ipc_field_t const point {.name = "item",
                             .type = ipc_type_t::fixed_size_list_k,
                             .width = 3,
                             .children = {&coordinate, 1}};
    ipc_field_t const shape {.name = "shape", .type = ipc_type_t::large_list_k, .children = {&point, 1}};
    failures += compare_schemas(read.schema, {{&shape, 1}, {}});
    failures += expect_eq(batch.nodes.size(), 3u);
    failures += expect_eq(batch.nodes[1].length, 4);
    failures += expect_eq(batch.nodes[2].length, 12);
    failures += expect_eq(read_value<std::int64_t>(batch.buffers[1], 2), 4);
    failures += expect_eq(read_value<float>(batch.buffers[4], 3), 1.0f);
    failures += expect_eq(read_value<float>(batch.buffers[4], 11), 2.0f);
    return failures + nested_round_trips(read.schema, batch);
}

/** Integers and floats of every width, nested as a struct in a list beside a <tt>numkong.bf16</tt> vector of u16,
 *  round-trip, and a list offset past its child, a fixed-size list child too short and a list without a child are
 *  refused. */
std::size_t nested_types_round_trip() {
    std::size_t failures = 0;
    constexpr ipc_key_value_t bf16_metadata[] {{ipc_extension_name_key_k, "numkong.bf16"}};
    ipc_field_t const lane {.name = "lane", .type = ipc_type_t::int_k, .width = 16, .metadata = bf16_metadata};
    ipc_field_t const numbers[] {
        {.name = "i8", .type = ipc_type_t::int_k, .width = 8, .signedness = ipc_signedness_t::signed_k},
        {.name = "u32", .type = ipc_type_t::int_k, .width = 32},
        {.name = "f16", .type = ipc_type_t::floating_point_k, .width = 16},
        {.name = "f64", .type = ipc_type_t::floating_point_k, .width = 64},
    };
    ipc_field_t const row {.name = "row", .type = ipc_type_t::struct_k, .children = numbers};
    ipc_field_t const fields[] {
        {.name = "vector", .type = ipc_type_t::fixed_size_list_k, .width = 2, .children = {&lane, 1}},
        {.name = "rows", .type = ipc_type_t::list_k, .children = {&row, 1}},
    };
    ipc_schema_t const schema {fields, {}};
    failures += expect_eq(ipc_fields_of(schema.fields), 8u);
    failures += expect_eq(ipc_buffers_of(schema.fields), 1u + 2u + 2u + 1u + 4u * 2u);

    std::uint16_t const lanes[] {0x3F80, 0x4000, 0x4040, 0x4080};
    std::int32_t const offsets[] {0, 1, 3};
    std::int8_t const i8[] {-1, 2, -3};
    std::uint32_t const u32[] {1, 2, 3};
    std::uint16_t const f16[] {0x3C00, 0x4000, 0x4200};
    double const f64[] {0.5, 1.5, 2.5};
    ipc_node_t nodes[] {{2, 0}, {4, 0}, {2, 0}, {3, 0}, {3, 0}, {3, 0}, {3, 0}, {3, 0}};
    std::span<std::byte const> buffers[] {
        {},
        {},
        std::as_bytes(std::span(lanes)),
        {},
        std::as_bytes(std::span(offsets)),
        {},
        {},
        std::as_bytes(std::span(i8)),
        {},
        std::as_bytes(std::span(u32)),
        {},
        std::as_bytes(std::span(f16)),
        {},
        std::as_bytes(std::span(f64)),
    };
    ipc_batch_t const batch {2, nodes, buffers};
    failures += expect_eq(ipc_validate_batch(schema, batch), ipc_status_t::success_k);
    failures += nested_round_trips(schema, batch);

    std::int32_t const overlong[] {0, 1, 4};
    buffers[4] = std::as_bytes(std::span(overlong));
    failures += expect_eq(ipc_validate_batch(schema, batch), ipc_status_t::malformed_k);
    buffers[4] = std::as_bytes(std::span(offsets));
    nodes[1].length = 3;
    failures += expect_eq(ipc_validate_batch(schema, batch), ipc_status_t::malformed_k);
    nodes[1].length = 4;

    ipc_field_t const childless {.name = "rows", .type = ipc_type_t::list_k};
    std::size_t needed = 0;
    failures += expect_eq(ipc_write_schema({{&childless, 1}, {}}, {}, 0, {}, needed), ipc_status_t::malformed_k);
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "arrow_ipc.schema_round_trip", schema_round_trip);
    report(totals, "arrow_ipc.batch_round_trip", batch_round_trip);
    report(totals, "arrow_ipc.validation_refuses", validation_refuses);
    report(totals, "arrow_ipc.direct_alignment_absolute", direct_alignment_absolute);
    report(totals, "arrow_ipc.legacy_prefix", legacy_prefix);
    report(totals, "arrow_ipc.compression_unsupported", compression_unsupported);
    report(totals, "arrow_ipc.big_endian_unsupported", big_endian_unsupported);
    report(totals, "arrow_ipc.truncated_input", truncated_input);
    report(totals, "arrow_ipc.footer_blocks_match", footer_blocks_match);
    report(totals, "arrow_ipc.pyarrow_golden", pyarrow_golden);
    report(totals, "arrow_ipc.nested_vectors_golden", nested_vectors_golden);
    report(totals, "arrow_ipc.nested_results_golden", nested_results_golden);
    report(totals, "arrow_ipc.nested_shapes_golden", nested_shapes_golden);
    report(totals, "arrow_ipc.nested_types_round_trip", nested_types_round_trip);

    return suite_exit_code(totals);
}
