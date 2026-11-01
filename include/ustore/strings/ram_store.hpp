/**
 *  @file include/ustore/strings/ram_store.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The RAM store of a strings collection and of the names system collection: every
 *      string mapped onto the 16-byte key it names, held inline and partitioned by the hash of
 *      a strings kit.
 */

#pragma once

#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/shared.hpp> // `st::mapping`, `st::expected`

#include "ustore/blobs/ram_store.hpp"       // `ordered_store`, `copy_blob`, `isolation_t`
#include "ustore/strings/string_keys.hpp"   // `string_key_t`, `string_key_order_t`, `string_key_hash`
#include "ustore/utilities/ram_buffers.hpp" // `strings_kit_concept`
#include "ustore/utilities/types.hpp"       // `value_view_t`

namespace unum::ustore::strings {

/** A string and the 16-byte key it maps onto, held inline. */
using string_pair_t = st::mapping<string_key_t, ustore_key_t>;

/** A strings collection, and the names system collection, which maps names onto collection IDs,
 *  partitioned by the hash of @p strings_kit_type_. */
template <blobs::isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          utilities::strings_kit_concept strings_kit_type_>
using keys_by_string_store = blobs::ordered_store<string_pair_t, string_key_order_t, string_key_hash<strings_kit_type_>,
                                                  isolation_, atomic_reference_, waiter_type_>;

/** The key @p text is stored under, owning a copy of its bytes. */
inline st::expected<string_key_t> string_key_from(std::string_view text) noexcept {
    auto [copied, copy_blob_status] = blobs::copy_blob(utilities::value_view_t {text});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return string_key_t {std::move(copied)};
}

} // namespace unum::ustore::strings
