/**
 *  @file src/nvme/sorted_run_index.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Layout of a sorted run's sorted run index, the B-tree whose cells hold the sorted run's key slots.
 */
#pragma once

#include <cstdint> // `std::uint64_t`

#include <ustore/types.hpp> // `divide_round_up`

namespace unum::ustore::nvme {

/** Levels a sorted run index layout describes, far beyond any sorted run index a sorted run file holds. */
inline constexpr std::uint64_t sorted_run_index_levels_capacity_k = 32;

/**
 *  @brief Maps between a sorted run index's cells and the sorted order of the key slots they hold.
 *
 *  Cells form rows of @c branching stored breadth-first, row @c p parenting rows @c p*(branching+1)+1 through
 *  @c p*(branching+1)+branching+1, and an in-order walk visits the cells in ascending order. Only sorted positions
 *  below @c key_slots_count hold key slots; the positions past them up to @c cells_count are padding.
 */
struct sorted_run_index_layout_t {

    /** Key slots in the sorted run index. */
    std::uint64_t key_slots_count = 0;

    /** Cells in one row. */
    std::uint64_t branching = 0;

    /** Rows of cells, enough for every key slot. */
    std::uint64_t rows_count = 0;

    /** Levels of rows, the root counting as one, and zero for a layout of no key slots. */
    std::uint64_t height = 0;

    /** Cells in the deepest level. */
    std::uint64_t leaf_cells_count = 0;

    /** Rows in the deepest level. */
    std::uint64_t leaf_rows_count = 0;

    /** At zero the key slots count, and at @c index above zero the cells of the top @c height minus @c index levels. */
    std::uint64_t cells_above[sorted_run_index_levels_capacity_k] = {};

    /** At @c index above zero the cells of level @c index minus one in a complete sorted run index, the root being
     *  level zero. */
    std::uint64_t level_cells[sorted_run_index_levels_capacity_k] = {};

    /** Sorted positions between neighbouring cells of a level in a complete sorted run index. */
    std::uint64_t level_stride[sorted_run_index_levels_capacity_k] = {};

    constexpr sorted_run_index_layout_t() noexcept = default;

    /** Lays out @p key_slots key slots in rows of @p cells_per_row, which must be two or more. */
    constexpr sorted_run_index_layout_t(std::uint64_t key_slots, std::uint64_t cells_per_row) noexcept
        : key_slots_count(key_slots), branching(cells_per_row), rows_count(divide_round_up(key_slots, cells_per_row)) {
        if (key_slots == 0) return;
        std::uint64_t levels = 1;
        for (std::uint64_t reach = branching + 1; reach <= key_slots && levels + 1 < sorted_run_index_levels_capacity_k;
             reach *= branching + 1)
            ++levels;
        height = levels;

        std::uint64_t const upper_cells = power_(branching + 1, height - 1) - 1;
        leaf_cells_count = rows_count * branching - upper_cells;
        leaf_rows_count = rows_count - upper_cells / branching;

        cells_above[0] = key_slots;
        for (std::uint64_t index = 1; index <= height; ++index)
            cells_above[index] = power_(branching + 1, height - index) - 1;

        level_cells[1] = cells_above[height - 1];
        level_stride[height - 1] = 1;
        for (std::uint64_t index = 2; index <= height; ++index) {
            level_cells[index] = cells_above[height - index] - cells_above[height - index + 1];
            level_stride[index - 2] = cells_above[index - 1] + 1;
        }
    }

    /** Cells in the sorted run index, padding included. */
    constexpr std::uint64_t cells_count() const noexcept { return rows_count * branching; }

    /** Level of the row holding @p cell, the root being level zero. */
    constexpr std::uint64_t level_of(std::uint64_t cell) const noexcept {
        for (std::uint64_t index = 1; index < height; ++index)
            if (cells_above[index] <= cell) return height - index;
        return 0;
    }

    /** Whether the layout describes any key slot, which decides whether its two mappings mean anything. */
    constexpr bool holds_key_slots() const noexcept { return height != 0; }

    /** Sorted position of @p cell, at or past @c key_slots_count for padding and for an empty layout. */
    constexpr std::uint64_t cell_to_sorted(std::uint64_t cell) const noexcept {
        if (!holds_key_slots()) return key_slots_count;
        std::uint64_t const level = level_of(cell);
        std::uint64_t const offset_in_level = cell - cells_above[height - level];
        std::uint64_t const slots = offset_in_level + offset_in_level / branching + 1;
        std::uint64_t const leaf_cells_before = level_cells[height - level - 1] * slots;
        std::uint64_t const missing_leaf_cells = leaf_cells_before < leaf_cells_count
                                                     ? 0
                                                     : leaf_cells_before - leaf_cells_count;
        return slots * level_stride[level] - 1 - missing_leaf_cells;
    }

    /** Cell holding sorted position @p sorted, which is @c cells_count for an empty layout that holds none. */
    constexpr std::uint64_t sorted_to_cell(std::uint64_t sorted) const noexcept {
        if (!holds_key_slots()) return cells_count();
        std::uint64_t const leaf_row = sorted / (branching + 1);
        std::uint64_t leaf_cells_before = leaf_rows_count * branching;
        if (leaf_row < leaf_rows_count) {
            std::uint64_t const offset_in_row = sorted - leaf_row * (branching + 1);
            if (offset_in_row != branching) return cells_above[1] + leaf_row * branching + offset_in_row;
            leaf_cells_before = (leaf_row + 1) * branching;
        }

        // Without the leaves the sorted run index is complete, so the trailing zeros of the position in base
        // branching+1 name its level
        std::uint64_t level_from_bottom = 0;
        std::uint64_t rows_before = sorted - leaf_cells_before + 1;
        std::uint64_t offset_in_row = 0;
        while (true) {
            std::uint64_t const previous = rows_before;
            rows_before /= branching + 1;
            offset_in_row = previous - rows_before * (branching + 1);
            if (offset_in_row != 0) break;
            ++level_from_bottom;
        }
        return cells_above[level_from_bottom + 2] + rows_before * branching + offset_in_row - 1;
    }

  private:
    static constexpr std::uint64_t power_(std::uint64_t base, std::uint64_t exponent) noexcept {
        std::uint64_t result = 1;
        for (std::uint64_t step = 0; step != exponent; ++step) result *= base;
        return result;
    }
};

} // namespace unum::ustore::nvme
