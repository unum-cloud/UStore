/**
 *  @file include/ustore/docs/ram_store.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The RAM store of a documents collection: every key mapped onto the JSON text of its
 *      document, edited in place.
 */

#pragma once

#include <smashtable/shared.hpp> // `st::mapping`

#include "ustore/blobs/ram_store.hpp" // `ordered_store`, `document_text_t`, `isolation_t`
#include "ustore/utilities/types.hpp" // `uuid_order_t`, `uuid_hash_t`

namespace unum::ustore::docs {

/** A document filed under its key. */
using document_pair_t = st::mapping<ustore_key_t, blobs::document_text_t>;

/** A documents collection: every key mapped onto the JSON text of its document, edited in place. */
template <blobs::isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using documents_by_key_store = blobs::ordered_store<document_pair_t, utilities::uuid_order_t, utilities::uuid_hash_t,
                                                    isolation_, atomic_reference_, waiter_type_>;

} // namespace unum::ustore::docs
