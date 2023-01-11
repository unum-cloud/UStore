#pragma once

namespace unum::ukv {

/**
 * @brief TODO: A hybrid `shared_mutex` with upgrade and downgrade ability.
 *
 * ## Other Implementations
 *
 * https://github.com/ssteinberg/shared_futex
 * https://github.com/AlexeyAB/object_threadsafe
 * https://github.com/yohhoy/yamc
 *
 */
struct shared_mutex_t {};

} // namespace unum::ukv
