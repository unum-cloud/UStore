/**
 *  @file tools/dataset.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief What the dataset units share: the format a path names, the arena a task brought or made,
 *      the placement of one field into a JSON document, the progress report between batches, the
 *      temporary an export writes before it lands, and the checks every step of a task makes
 *      before it runs.
 */

#pragma once

#include <cstring>     // `std::memcpy`, `std::strlen`
#include <string_view> // `std::string_view`

#include <yyjson.h> // `yyjson_mut_doc`, `yyjson_mut_val`, `yyjson_val`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/db.h"                         // `ustore_arena_t`, `ustore_outcome_t`, `ustore_arena_init`
#include "ustore/utilities/blocking_files.hpp" // `temporary_path_of`, `rename_file`, `sync_directory`

#include "dataset.h" // `ustore_dataset_format_t`

namespace unum::ustore::tools {

/** The format @p path names through its extension, or @p asked when it names one, staying automatic
 *  for a path naming none, like the standard input of @c -. */
[[nodiscard]] ustore_dataset_format_t format_of(std::string_view path, ustore_dataset_format_t asked) noexcept;

/** Whether @p outcome already holds a failure, which every later step of a task skips past. */
[[nodiscard]] bool failed(ustore_outcome_t const* outcome) noexcept;

/** The C outcome of @p status: its note as the message, else the code's name, none on success. */
[[nodiscard]] inline ustore_outcome_t outcome_of(util::status_t status) noexcept {
    ustore_status_t const code = static_cast<ustore_status_t>(status.code);
    if (status.succeeded()) return {code, nullptr};
    return {code, status.note ? status.note : ustore_status_name(code)};
}

/** Reports @p completed of @p total to the progress callback @p task carries, answering whether the
 *  task goes on; a cancel writes @c ustore_status_cancelled_k into the task's outcome. */
template <typename task_type_>
[[nodiscard]] bool advance(task_type_ const& task, ustore_size_t completed, ustore_size_t total) noexcept {
    if (!task.progress || task.progress(completed, total, task.progress_state) == ustore_continue_k) return true;
    *task.outcome = {ustore_status_cancelled_k, "The progress callback cancelled the call"};
    return false;
}

/** Copies @p path into @p copy, which ends in the null the file calls need, refusing
 *  an empty path. */
void copy_path(ustore_bytes_view_t path, sz::string_t& copy, ustore_outcome_t* outcome) noexcept;

/** Copies @p value into @p root at the path @p field names, creating the objects between; a
 *  placed path stays. */
void place(yyjson_mut_doc* builder, yyjson_mut_val* root, std::string_view field, yyjson_val* value) noexcept;

/**
 *  @brief The file an export targets and the sibling temporary its bytes go to first, landed over
 *      the target only once whole, as @c replace_file_durably lands a file.
 *
 *  A failure or a cancel removes the temporary alone, so a file already at the target stays as it
 *  was, byte for byte. The temporary is the target's name with @c .tmp appended, opened without
 *  following a link on POSIX systems, so two exports to one target must not run at once.
 */
class staged_output_t {
    char target_[util::path_max_bytes_k] {};
    char temporary_[util::path_max_bytes_k] {};
    char directory_[util::path_max_bytes_k] {};
    bool staged_ = false;

  public:

    /** Names the temporary and the directory of @p target, reporting a path past the limit into
     *  @p outcome. */
    void stage(char const* target, ustore_outcome_t* outcome) noexcept {
        std::size_t const length = std::strlen(target);
        util::status_t status;
        if (length >= sizeof(target_)) status = {util::status_code_t::invalid_argument_k, "A path outgrew the limit"};
        if (status.succeeded()) status = util::temporary_path_of(target, temporary_);
        if (status.succeeded()) status = util::parent_directory_of(target, directory_);
        if (status.failed()) {
            *outcome = outcome_of(status);
            return;
        }
        std::memcpy(target_, target, length + 1);
        staged_ = true;
    }

    /** The temporary the export writes, null-terminated. */
    [[nodiscard]] char const* temporary() const noexcept { return temporary_; }

    /** Renames the flushed and closed temporary over the target and syncs their directory while
     *  @p outcome holds success, and removes the temporary otherwise. */
    void land(ustore_outcome_t* outcome) noexcept {
        if (!staged_) return;
        staged_ = false;
        util::status_t landed;
        if (outcome->status == ustore_status_success_k) landed = util::rename_file(temporary_, target_);
        if (outcome->status != ustore_status_success_k || landed.failed()) {
            [[maybe_unused]] util::status_t const removed = util::remove_file(temporary_);
            if (landed.failed()) *outcome = outcome_of(landed);
            return;
        }
        if (util::status_t const synced = util::sync_directory(directory_); synced.failed())
            *outcome = outcome_of(synced);
    }
};

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
