/**
 *  @file include/ustore/pages/ram_store.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The RAM store of a pages collection: every key mapped onto the dense element tree of its
 *      page, edited in place.
 */

#pragma once

#include <smashtable/shared.hpp> // `st::mapping`

#include "ustore/blobs/ram_store.hpp" // `ordered_store`, `page_tree_bytes_t`, `isolation_t`
#include "ustore/utilities/types.hpp" // `uuid_order_t`, `uuid_hash_t`

namespace unum::ustore::pages {

/** A page filed under its key. */
using page_pair_t = st::mapping<ustore_key_t, blobs::page_tree_bytes_t>;

/** A pages collection: every key mapped onto the dense element tree of its page,
 *  edited in place. */
template <blobs::isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using pages_by_key_store = blobs::ordered_store<page_pair_t, utilities::uuid_order_t, utilities::uuid_hash_t,
                                                isolation_, atomic_reference_, waiter_type_>;

} // namespace unum::ustore::pages
