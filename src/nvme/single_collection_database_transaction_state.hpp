/**
 *  @file src/nvme/single_collection_database_transaction_state.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Uncommitted changes and watched keys of a single single-collection database transaction.
 */

#pragma once

#include "nvme/read_set.hpp"     // `noexcept`, `read_set`
#include "nvme/write_buffer.hpp" // `write_buffer`

namespace unum::ustore::nvme {

template <typename stored_key_>
struct single_collection_database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = write_buffer<stored_key_t>;
    using read_set_t = nvme::read_set<stored_key_t>;

    inline single_collection_database_transaction_state(write_buffer_t&& write_buffer,
                                                        resources_shared_ptr_t resources) noexcept
        : changes(std::forward<write_buffer_t>(write_buffer)), read_set(resources) {}

    inline bool empty() const noexcept { return changes.empty() & read_set.empty(); }

    inline void clear() {
        changes.unsafe_clear();
        read_set.clear();
    }

    write_buffer_t changes;
    read_set_t read_set;
};

} // namespace unum::ustore::nvme
