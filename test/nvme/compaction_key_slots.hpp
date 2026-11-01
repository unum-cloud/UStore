/**
 *  @file test/nvme/compaction_key_slots.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The key slots the compaction suite merges, the engine's own, shared by its host
 *      translation unit and its device one, so its 16-byte cases run on the kernels the
 *      library's bundle carries.
 */
#pragma once

#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`, `limit`, `value_layout_t`

namespace unum::ustore::test::nvme {

using blobs::limit;

/** Stored values of up to twelve bytes inside the key slot, the engine's inline
 *  12-byte key slots. */
template <typename stored_key_type_>
using inline_key_slot = blobs::key_slot<stored_key_type_, blobs::value_layout_t::inline_12_bytes_k>;

/** Stored values of one width in the stored values section, found by stride, the engine's
 *  fixed-width key slots. */
template <typename stored_key_type_>
using fixed_width_key_slot = blobs::key_slot<stored_key_type_, blobs::value_layout_t::fixed_width_k>;

/** Stored values of any width in the stored values section, found through value offsets, the
 *  engine's variable-width key slots. */
template <typename stored_key_type_>
using variable_width_key_slot = blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>;

} // namespace unum::ustore::test::nvme
