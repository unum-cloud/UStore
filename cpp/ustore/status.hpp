/**
 *  @file cpp/ustore/status.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Statuses with static messages and the expected wrapper that every fallible C++ call returns, re-exported from
 *      `ustore/utilities/types.hpp`, and the deferred call that a call submitted to an executor answers.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <new>     // `std::nothrow`
#include <tuple>   // `std::apply`, `std::tuple`
#include <utility> // `std::exchange`, `std::move`, `std::swap`

#include "ustore/types.h"             // `ustore_outcome_t`, `ustore_outcome_wait`
#include "ustore/utilities/types.hpp" // `expected`, `status_code_t`, `status_t`, re-exported into `unum::ustore`

namespace unum::ustore {

/**
 *  @brief A call submitted to an executor, owning the one heap block that holds its outcome, every plain input it
 *      reads and every output it writes through, which never moves until the call is final.
 *
 *  Submission copies the C arguments, so they live only on the stack of @c submit, while everything they point to lives
 *  in the block. An arena is a handle the call borrows, since freeing it waits for the call. @c status polls without
 *  blocking, and @c wait blocks until the call is final and hands over what it produced. The destructor waits too, so
 *  nothing the call reads goes away under it, and a block that could not be allocated reads as a call that finished
 *  with @c status_code_t::out_of_memory_k.
 *
 *  @tparam verb_type_ What one verb keeps beside its inputs: the outputs the call writes through, its C
 *      @c arguments_t, the static @c submit that hands them to the library, and @c finish, which turns the outputs of
 *      a successful call into its @c answer_t.
 */
template <typename verb_type_>
class [[nodiscard]] deferred {
  public:
    using verb_t = verb_type_;
    using arguments_t = typename verb_type_::arguments_t;
    using answer_t = typename verb_type_::answer_t;

  private:
    struct block_t {
        ustore_outcome_t outcome {};
        verb_type_ verb {};
        void (*destroy)(block_t*) noexcept = nullptr;
    };

    template <typename... inputs_types_>
    struct owning_block : block_t {
        std::tuple<inputs_types_...> inputs;
        explicit owning_block(inputs_types_&&... owned) noexcept : inputs(std::move(owned)...) {}
    };

    block_t* block_ = nullptr;

    explicit deferred(block_t* block) noexcept : block_(block) {}

    void release_() noexcept {
        if (!block_) return;
        ustore_outcome_wait(&block_->outcome);
        block_->destroy(block_);
        block_ = nullptr;
    }

  public:
    deferred(deferred&& other) noexcept : block_(std::exchange(other.block_, nullptr)) {}
    deferred& operator=(deferred&& other) noexcept {
        std::swap(block_, other.block_);
        return *this;
    }
    deferred(deferred const&) = delete;
    deferred& operator=(deferred const&) = delete;
    ~deferred() noexcept { release_(); }

    /**
     *  @brief Moves @p inputs into a new block, lets @p prepare fill the C arguments from them, and submits a copy.
     *  @param[in] prepare Fills the C arguments from the verb and the owned inputs, pointing them only into the block,
     *      and answers a status that becomes the final one, with nothing submitted, when it fails.
     *  @param[in] inputs Everything the call reads, which the block owns until the call is final.
     */
    template <typename prepare_type_, typename... inputs_types_>
    static deferred submit(prepare_type_&& prepare, inputs_types_... inputs) noexcept {
        using owning_block_t = owning_block<inputs_types_...>;
        owning_block_t* const block = new (std::nothrow) owning_block_t(std::move(inputs)...);
        if (!block) return deferred {nullptr};
        block->destroy = [](block_t* erased) noexcept { delete static_cast<owning_block_t*>(erased); };
        arguments_t arguments {};
        status_t const prepared = std::apply(
            [&](inputs_types_&... owned) noexcept { return prepare(block->verb, arguments, owned...); }, block->inputs);
        if (prepared.failed()) block->outcome = prepared;
        else {
            arguments.outcome = &block->outcome;
            arguments.completion = ustore_completion_deferred_k;
            verb_type_::submit(arguments);
        }
        return deferred {block};
    }

    /** The status of the call, polled through @c ustore_outcome_status: @c status_code_t::deferred_k until final. */
    status_code_t status() const noexcept {
        if (!block_) return status_code_t::out_of_memory_k;
        return static_cast<status_code_t>(ustore_outcome_status(&block_->outcome));
    }

    /** Blocks until the call is final, then answers what it produced, or the status explaining why it did not. */
    answer_t wait() && noexcept {
        if (!block_) return status_t {status_code_t::out_of_memory_k, "Failed to allocate a deferred call"};
        auto const code = static_cast<status_code_t>(ustore_outcome_wait(&block_->outcome));
        status_t const status {code, block_->outcome.message};
        answer_t answer = status.failed() ? answer_t {status} : std::move(block_->verb).finish();
        release_();
        return answer;
    }
};

} // namespace unum::ustore
