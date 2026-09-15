/**
 *  @file src/helpers/full_scan.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Reservoir sampling of keys over the iterators of ordered engines.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <chrono> // `std::chrono::steady_clock`
#include <random> // `std::minstd_rand`, `std::uniform_int_distribution`

#include <ustore/ranges.hpp> // `ptr_range`

namespace unum::ustore {

/**
 *  Fills @p sampled_keys with keys drawn uniformly over a RocksDB or LevelDB iterator.
 *  @param[inout] iterator Engine iterator, rewound to the first key before sampling.
 *  @param[out] sampled_keys Destination whose size is the size of the sample.
 *  @param[out] error Receives a message when the collection holds fewer keys than the sample needs.
 *  @see https://en.wikipedia.org/wiki/Reservoir_sampling
 */
template <typename iterator_>
void reservoir_sample_iterator(iterator_&& iterator, ptr_range<ustore_key_t> sampled_keys, ustore_error_t* error) {

    auto const seed = static_cast<std::minstd_rand::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::minstd_rand generator(seed);

    std::size_t visited = 0;
    for (iterator->SeekToFirst(); visited < sampled_keys.size(); ++visited, iterator->Next()) {
        if (!iterator->Valid()) {
            *error = "Sample Failure!";
            return;
        }
        std::memcpy(&sampled_keys[visited], iterator->key().data(), sizeof(ustore_key_t));
    }

    for (; iterator->Valid(); ++visited, iterator->Next()) {
        std::size_t const slot = std::uniform_int_distribution<std::size_t>(0, visited)(generator);
        if (slot < sampled_keys.size()) std::memcpy(&sampled_keys[slot], iterator->key().data(), sizeof(ustore_key_t));
    }
}

} // namespace unum::ustore
