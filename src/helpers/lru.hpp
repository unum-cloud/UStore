/**
 * @file lru.hpp
 * @author Ashot Vardanian
 *
 * @brief Least-Recently Used cache
 */
#pragma once
#include "helpers.hpp"

namespace unum::ukv {

/**
 * @brief Extension of Boosts LRU cache.
 *
 * Changes:
 * - Adds support for non-copyable values.
 * - Exposes eviction function.
 * - Allows popping key-value pairs.
 * - Uses `unordered_map` for faster lookups and preallocation.
 *
 * https://www.boost.org/doc/libs/1_67_0/boost/compute/detail/lru_cache.hpp
 */
template <typename key_at, typename value_at>
class lru_cache_gt {

  public:
    using key_type = key_at;
    using value_type = value_at;
    using list_type = std::list<key_type>;
    using map_type = std::unordered_map<key_type, std::pair<value_type, typename list_type::iterator>>;

  private:
    map_type map_;
    list_type list_;
    size_t capacity_;

  public:
    lru_cache_gt(size_t capacity) : capacity_(capacity) { map_.reserve(capacity_); }
    ~lru_cache_gt() {}

    size_t size() const { return map_.size(); }
    size_t capacity() const { return capacity_; }
    bool empty() const { return map_.empty(); }
    bool contains(key_type const& key) { return map_.find(key) != map_.end(); }

    void insert(key_type const& key, value_type&& value) {
        auto i = map_.find(key);
        if (i != map_.end())
            return;
        if (size() >= capacity_)
            evict();
        list_.push_front(key);
        map_[key] = std::make_pair(std::move(value), list_.begin());
    }

    value_type const* get_ptr(key_type const& key) {
        auto i = map_.find(key);
        if (i == map_.end())
            return std::nullopt;

        auto j = i->second.second;
        if (j != list_.begin()) {
            list_.erase(j);
            list_.push_front(key);

            // update iterator in map
            j = list_.begin();
            value_type const& value = i->second.first;
            map_[key] = std::make_pair(value, j);
            return &value;
        }

        // the item is already at the front of the most recently
        // used list so just return it
        return &i->second.first;
    }

    std::optional<value_type> pop(key_type const& key) {
        auto i = map_.find(key);
        if (i == map_.end())
            return std::nullopt;

        value_type result;
        auto j = i->second.second;
        if (j != list_.begin()) {
            std::swap(result, i->second.first);
            list_.erase(j);
        }
        else {
            std::swap(result, list_.front());
            list_.pop_front();
        }
        map_.erase(i);
        return result;
    }

    void clear() {
        map_.clear();
        list_.clear();
    }

    void evict() {
        auto i = --list_.end();
        map_.erase(*i);
        list_.erase(i);
    }

    std::optional<std::pair<key_type, std::reference_wrapper<value_type>>> oldset() {
        auto i = --list_.end();
        map_.erase(*i);
        list_.erase(i);
    }
};

} // namespace unum::ukv
