/**
 *  @file tools/dataset.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief What the dataset units share: the format a path names, the arena a task brought or made,
 *      the placement of one field into a JSON document, and the checks every step of a task makes
 *      before it runs.
 */

#pragma once

#include <string_view> // `std::string_view`

#include <yyjson.h> // `yyjson_mut_doc`, `yyjson_mut_val`, `yyjson_val`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/db.h" // `ustore_arena_t`, `ustore_outcome_t`, `ustore_arena_init`, `ustore_arena_free`

#include "dataset.h" // `ustore_dataset_format_t`

namespace unum::ustore::tools {

/** The format @p path names through its extension, or @p asked when it names one, staying automatic
 *  for a path naming none, like the standard input of @c -. */
[[nodiscard]] ustore_dataset_format_t format_of(std::string_view path, ustore_dataset_format_t asked) noexcept;

/** Whether @p outcome already holds a failure, which every later step of a task skips past. */
[[nodiscard]] bool failed(ustore_outcome_t const* outcome) noexcept;

/** Copies @p path into @p copy, which ends in the null the file calls need, refusing
 *  an empty path. */
void copy_path(ustore_bytes_view_t path, sz::string_t& copy, ustore_outcome_t* outcome) noexcept;

/** Copies @p value into @p root at the path @p field names, creating the objects between; a
 *  placed path stays. */
void place(yyjson_mut_doc* builder, yyjson_mut_val* root, std::string_view field, yyjson_val* value) noexcept;

/** Takes the arena a task brought, or makes a private one on host pages when it brought none, which
 *  it frees on destruction. */
class task_arena_t {
    ustore_arena_t private_arena_ = nullptr;
    ustore_arena_t chosen_ = nullptr;

  public:

    /** Takes @p given, or makes a private arena, reporting a failure to make one
     *  into @p outcome. */
    task_arena_t(ustore_arena_t given, ustore_outcome_t* outcome) noexcept : chosen_(given) {
        if (given) return;
        ustore_arena_init_t init {.arena = &private_arena_, .executor = nullptr, .outcome = outcome};
        ustore_arena_init(&init);
        chosen_ = private_arena_;
    }
    task_arena_t(task_arena_t const&) = delete;
    task_arena_t& operator=(task_arena_t const&) = delete;
    ~task_arena_t() noexcept { ustore_arena_free(private_arena_); }

    [[nodiscard]] ustore_arena_t get() const noexcept { return chosen_; }
};

} // namespace unum::ustore::tools
