/**
 * @file context.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/db.h".
 */

#pragma once
#include "ustore/db.h"

namespace unum::ustore {

/**
 * Abstraction over @c ustore_database_t, @c ustore_transaction_t and @c ustore_arena_t,
 * needed for default-construction.
 */
class null_context_t {
  public:
};

/**
 * @brief Allows transactional operations on memory managed by someone else.
 * Won't allow adding or removing collections or bulk-removing all entries.
 */
class borrowed_transactional_context_t {
    ustore_database_t db_ {nullptr};
    ustore_transaction_t txn_ {nullptr};
    ustore_arena_t* arena_ {nullptr};

  public:
    ustore_database_t db() noexcept { return db_; }
    ustore_transaction_t txn() noexcept { return txn_; }
    ustore_arena_t* arena() noexcept { return arena_; }

    borrowed_transactional_context_t(ustore_database_t db, ustore_transaction_t txn, ustore_arena_t* arena) noexcept
        : db_(db), txn_(txn), arena_(arena) {}
};

/**
 * @brief Allows transactional operations on the memory owned by this context.
 * Won't allow adding or removing collections or bulk-removing all entries.
 */
class owned_transactional_context_t {
    ustore_database_t db_ {nullptr};
    ustore_transaction_t txn_ {nullptr};
    ustore_arena_t arena_ {nullptr};

  public:
    ustore_database_t db() noexcept { return db_; }
    ustore_transaction_t txn() noexcept { return txn_; }
    ustore_arena_t* arena() noexcept { return &arena_; }
};

/**
 * @brief Allows any operations without any "transactional compositing"
 * on the memory owned by this context. Mostly used for testing, addition
 * and removal of collections and bulk entry removals.
 */
class owned_context_t {
    ustore_database_t db_ = nullptr;
    ustore_arena_t arena_ = nullptr;

  public:
    ustore_database_t db() noexcept { return db_; }
    ustore_transaction_t txn() noexcept { return nullptr; }
    ustore_arena_t* arena() noexcept { return &arena_; }
};

} // namespace unum::ustore
