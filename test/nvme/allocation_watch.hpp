/**
 *  @file test/nvme/allocation_watch.hpp
 *  @author Ash Vardanian
 *  @date September 20, 2026
 *  @brief The watch over the global allocation functions, which allocation-counting suites link.
 */
#pragma once

#include <cstddef> // `std::size_t`

namespace unum::ustore::test::nvme {

/** The allocation sizes one case watches for, how often a request of each size arrived, and how
 *  many arrived in all. */
struct allocation_watch_t {

    /** Most sizes one case watches at once. */
    static constexpr std::size_t capacity_k = 4;

    /** The byte sizes watched, which a request matches exactly or not at all. */
    std::size_t sizes[capacity_k] = {};

    /** Requests seen of each watched size. */
    std::size_t hits[capacity_k] = {};

    /** Sizes watched right now, zero between cases. */
    std::size_t count = 0;

    /** Requests of every size since the watch was set. */
    std::size_t calls = 0;
};

/** The one watch the replaced allocation functions write to, since a global @c operator @c new
 *  has no other context. */
extern allocation_watch_t allocation_watch_g;

} // namespace unum::ustore::test::nvme
