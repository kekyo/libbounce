/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_PROMISE_H
#define _LIBBOUNCE_PROMISE_H

#if !defined(__cplusplus)
#error "libbounce/promise.h requires C++ compilation"
#endif

#if (__cplusplus < 202002L) && !defined(__cpp_impl_coroutine)
#error "libbounce/promise.h requires C++20 or later"
#endif

#include <atomic>
#include <coroutine>
#include <exception>
#include <new>

#include "bounce.h"

namespace libbounce {

/**
 * @brief Result state observed from a coroutine await on libbounce work.
 */
enum class await_status : unsigned int {
  completed = 0u,
  canceled = 1u,
  aborted = 2u,
  start_failed = 3u
};

/**
 * @brief Result returned from coroutine await operations.
 */
struct await_result {
  await_status status = await_status::aborted;

  /**
   * @brief Check whether the awaited operation completed normally.
   * @return True when the awaited operation completed normally.
   */
  inline bool completed() const noexcept {
    return status == await_status::completed;
  }

  /**
   * @brief Check whether the awaited operation completed by cancellation.
   * @return True when the awaited operation completed by cancellation.
   */
  inline bool canceled() const noexcept {
    return status == await_status::canceled;
  }

  /**
   * @brief Check whether the awaited operation completed as aborted.
   * @return True when the awaited operation completed as aborted.
   */
  inline bool aborted() const noexcept {
    return status == await_status::aborted;
  }

  /**
   * @brief Check whether the awaited operation failed before registration started.
   * @return True when local setup failed before asynchronous waiting began.
   */
  inline bool start_failed() const noexcept {
    return status == await_status::start_failed;
  }
};

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB) || defined(_WIN32)
/**
 * @brief Result returned from coroutine file I/O helpers.
 * @remarks The await state reports helper completion. The syscall-level result
 * is stored in @ref result and @ref error_code so native errors can be reported
 * without treating them as libbounce infrastructure aborts.
 */
struct file_io_result {
  await_result await {};
  int64_t result = -1;
  int error_code = 0;

  /**
   * @brief Create a default aborted file I/O result.
   */
  inline file_io_result() noexcept = default;

  /**
   * @brief Create a file I/O result.
   * @param await_result_ Await completion state.
   * @param result_ Bytes, seek offset, flush result, or -1.
   * @param error_code_ Backend native error code, or zero.
   */
  inline file_io_result(
    await_result await_result_,
    int64_t result_,
    int error_code_) noexcept
    : await(await_result_),
      result(result_),
      error_code(error_code_) {
  }

  /**
   * @brief Check whether the helper completed normally.
   * @return True when the helper completed normally.
   */
  inline bool completed() const noexcept {
    return await.completed();
  }

  /**
   * @brief Check whether the helper was canceled.
   * @return True when the helper observed cancellation.
   */
  inline bool canceled() const noexcept {
    return await.canceled();
  }

  /**
   * @brief Check whether the helper aborted.
   * @return True when the helper aborted.
   */
  inline bool aborted() const noexcept {
    return await.aborted();
  }

  /**
   * @brief Check whether helper setup failed before registration.
   * @return True when local setup failed before asynchronous waiting began.
   */
  inline bool start_failed() const noexcept {
    return await.start_failed();
  }

  /**
   * @brief Check whether the file operation itself succeeded.
   * @return True when the await completed and the syscall-level result is not
   * an error.
   */
  inline bool succeeded() const noexcept {
    return completed() && (error_code == 0) && (result >= 0);
  }
};
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
/**
 * @brief Result returned from coroutine socket I/O helpers.
 * @remarks The await state reports helper completion. The syscall-level result
 * is stored in @ref result and @ref error_code so POSIX socket errors can be
 * reported without treating them as libbounce infrastructure aborts.
 */
struct socket_io_result {
  await_result await {};
  int64_t result = -1;
  int error_code = 0;

  /**
   * @brief Create a default aborted socket I/O result.
   */
  inline socket_io_result() noexcept = default;

  /**
   * @brief Create a socket I/O result.
   * @param await_result_ Await completion state.
   * @param result_ Bytes sent/received, EOF zero, zero-length result, or -1.
   * @param error_code_ POSIX errno value, or zero.
   */
  inline socket_io_result(
    await_result await_result_,
    int64_t result_,
    int error_code_) noexcept
    : await(await_result_),
      result(result_),
      error_code(error_code_) {
  }

  /**
   * @brief Check whether the helper completed normally.
   * @return True when the helper completed normally.
   */
  inline bool completed() const noexcept {
    return await.completed();
  }

  /**
   * @brief Check whether the helper was canceled.
   * @return True when the helper observed cancellation.
   */
  inline bool canceled() const noexcept {
    return await.canceled();
  }

  /**
   * @brief Check whether the helper aborted.
   * @return True when the helper aborted.
   */
  inline bool aborted() const noexcept {
    return await.aborted();
  }

  /**
   * @brief Check whether helper setup failed before registration.
   * @return True when local setup failed before asynchronous waiting began.
   */
  inline bool start_failed() const noexcept {
    return await.start_failed();
  }

  /**
   * @brief Check whether the socket operation itself succeeded.
   * @return True when the await completed and the syscall-level result is not
   * an error.
   */
  inline bool succeeded() const noexcept {
    return completed() && (error_code == 0) && (result >= 0);
  }
};
#endif

/**
 * @brief Promise result produced by callback-backed helpers.
 * @tparam T Mapped callback payload type, or `void`.
 * @remarks The helper-level completion state is reported through @ref await.
 * The mapped value is engaged only after a normal completion.
 */
template<typename T>
struct callback_promise_result {
  await_result await {};
  std::optional<T> value {};

  /**
   * @brief Check whether the callback helper completed normally.
   * @return True when the callback fired and the helper completed normally.
   */
  inline bool completed() const noexcept {
    return await.completed();
  }

  /**
   * @brief Check whether the callback helper completed by cancellation.
   * @return True when the helper observed cancellation.
   */
  inline bool canceled() const noexcept {
    return await.canceled();
  }

  /**
   * @brief Check whether the callback helper completed as aborted.
   * @return True when the helper aborted.
   */
  inline bool aborted() const noexcept {
    return await.aborted();
  }

  /**
   * @brief Check whether the callback helper failed before registration started.
   * @return True when setup failed before the callback registration began.
   */
  inline bool start_failed() const noexcept {
    return await.start_failed();
  }
};

/**
 * @brief Promise result produced by callback-backed helpers for `void` payloads.
 */
template<>
struct callback_promise_result<void> {
  await_result await {};

  /**
   * @brief Check whether the callback helper completed normally.
   * @return True when the callback fired and the helper completed normally.
   */
  inline bool completed() const noexcept {
    return await.completed();
  }

  /**
   * @brief Check whether the callback helper completed by cancellation.
   * @return True when the helper observed cancellation.
   */
  inline bool canceled() const noexcept {
    return await.canceled();
  }

  /**
   * @brief Check whether the callback helper completed as aborted.
   * @return True when the helper aborted.
   */
  inline bool aborted() const noexcept {
    return await.aborted();
  }

  /**
   * @brief Check whether the callback helper failed before registration started.
   * @return True when setup failed before the callback registration began.
   */
  inline bool start_failed() const noexcept {
    return await.start_failed();
  }
};

template<typename TBOUNCE_HANDLE, typename START_FN>
inline await_operation make_awaitable(
  TBOUNCE_HANDLE &bounce_handle,
  START_FN&& start,
  BOUNCE_CANCELLATION *cancellation = nullptr) noexcept;

namespace detail {

static inline await_result await_result_from_completion(
  BOUNCE_COMPLETION_RESULT result) noexcept {
  switch (result) {
    case BOUNCE_COMPLETION_COMPLETED:
      return await_result { await_status::completed };
    case BOUNCE_COMPLETION_CANCELED:
      return await_result { await_status::canceled };
    case BOUNCE_COMPLETION_ABORTED:
    default:
      return await_result { await_status::aborted };
  }
}

template<typename T>
class promise_storage {
private:
  std::optional<T> value_;

public:
  template<typename U>
  inline void emplace(U&& value) {
    value_.emplace(std::forward<U>(value));
  }

  inline T consume() {
    return std::move(*value_);
  }
};

template<>
class promise_storage<void> {
public:
  inline void emplace() noexcept {
  }

  inline void consume() noexcept {
  }
};

template<typename T>
class promise_shared_state {
public:
  std::atomic<unsigned int> suspend_state_ { 0u };
  std::atomic<unsigned int> start_returned_ { 0u };
  std::coroutine_handle<> continuation_;
  BOUNCE_CORE *continuation_bounce_ = nullptr;
  std::coroutine_handle<> handle_;
  std::exception_ptr exception_;
  std::atomic<bool> started_ { false };
  std::atomic<bool> completed_ { false };
  promise_storage<T> storage_{};

  inline void capture_exception() noexcept {
    exception_ = std::current_exception();
  }

  inline void rethrow_if_failed() {
    if (exception_) {
      std::exception_ptr captured = exception_;

      exception_ = nullptr;
      std::rethrow_exception(captured);
    }
  }
};

template<typename T>
static inline void destroy_promise_state(
  std::shared_ptr<promise_shared_state<T>> &state) noexcept {
  if (!state) {
    return;
  }

  const bool started = state->started_.load(std::memory_order_acquire);
  const bool completed = state->completed_.load(std::memory_order_acquire);

  if (((!started) || completed) && state->handle_) {
    std::coroutine_handle<> handle = state->handle_;

    state->handle_ = std::coroutine_handle<> {};
    handle.destroy();
  } else if (!completed && state->handle_) {
    std::terminate();
  }

  state.reset();
}

template<typename TPROMISE>
static inline void promise_resume_continuation(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) noexcept;

template<typename T>
static inline void promise_resume_shared_state_continuation(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) noexcept;

template<typename T>
static inline void promise_schedule_continuation(
  promise_shared_state<T> *state) noexcept;

template<typename TPROMISE>
struct promise_final_awaiter {
  inline bool await_ready() const noexcept {
    return false;
  }

  inline void await_suspend(std::coroutine_handle<TPROMISE> completed_handle) const noexcept {
    auto state = completed_handle.promise().state_;
    unsigned int previous = 0u;

    state->completed_.store(true, std::memory_order_release);
    if (!state->continuation_) {
      return;
    }

    if (state->start_returned_.load(std::memory_order_acquire) != 0u) {
      previous = state->suspend_state_.exchange(3u, std::memory_order_acq_rel);
      if ((previous != 0u) && (previous != 1u)) {
        return;
      }
    } else {
      (void)state->suspend_state_.exchange(2u, std::memory_order_acq_rel);
      return;
    }

    promise_schedule_continuation(state.get());
  }

  inline void await_resume() const noexcept {
  }
};

template<typename TPROMISE>
static inline void promise_resume_continuation(
  BOUNCE_COMPLETION_RESULT /*result*/,
  void *completion_state) noexcept {
  auto *state =
    static_cast<typename TPROMISE::state_type *>(completion_state);

  state->continuation_.resume();
}

template<typename T>
static inline void promise_resume_shared_state_continuation(
  BOUNCE_COMPLETION_RESULT /*result*/,
  void *completion_state) noexcept {
  auto *state = static_cast<promise_shared_state<T> *>(completion_state);

  state->continuation_.resume();
}

template<typename T>
static inline void promise_schedule_continuation(
  promise_shared_state<T> *state) noexcept {
  if ((state->continuation_bounce_ != nullptr) &&
      ::bounce_post(
        state->continuation_bounce_,
        &promise_resume_shared_state_continuation<T>,
        state)) {
    return;
  }

  state->continuation_.resume();
}

template<typename T>
class promise_awaiter {
private:
  std::shared_ptr<promise_shared_state<T>> state_;

public:
  explicit inline promise_awaiter(std::shared_ptr<promise_shared_state<T>> state) noexcept
    : state_(std::move(state)) {
  }

  promise_awaiter(const promise_awaiter&) = delete;
  promise_awaiter& operator=(const promise_awaiter&) = delete;

  inline promise_awaiter(promise_awaiter&& other) noexcept
    : state_(std::move(other.state_)) {
  }

  inline promise_awaiter& operator=(promise_awaiter&& other) noexcept {
    if (this != &other) {
      destroy_promise_state(state_);
      state_ = std::move(other.state_);
    }
    return *this;
  }

  inline ~promise_awaiter() noexcept {
    destroy_promise_state(state_);
  }

  inline bool await_ready() const noexcept {
    return !state_ || state_->completed_.load(std::memory_order_acquire);
  }

  inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    auto state = state_;
    unsigned int expected = 0u;

    if (!state) {
      return false;
    }

    state->continuation_ = continuation;
    state->continuation_bounce_ = ::bounce_get_core();
    if (state->completed_.load(std::memory_order_acquire)) {
      return false;
    }
    if (!state->suspend_state_.compare_exchange_strong(
      expected,
      1u,
      std::memory_order_acq_rel,
      std::memory_order_acquire)) {
      return expected == 3u;
    }
    state->start_returned_.store(1u, std::memory_order_release);
    if (!state->started_.exchange(true, std::memory_order_acq_rel)) {
      state->handle_.resume();
    }
    return true;
  }

  inline T await_resume() {
    if (!state_) {
      std::terminate();
    }

    state_->rethrow_if_failed();
    return state_->storage_.consume();
  }
};

template<>
class promise_awaiter<void> {
private:
  std::shared_ptr<promise_shared_state<void>> state_;

public:
  explicit inline promise_awaiter(std::shared_ptr<promise_shared_state<void>> state) noexcept
    : state_(std::move(state)) {
  }

  promise_awaiter(const promise_awaiter&) = delete;
  promise_awaiter& operator=(const promise_awaiter&) = delete;

  inline promise_awaiter(promise_awaiter&& other) noexcept
    : state_(std::move(other.state_)) {
  }

  inline promise_awaiter& operator=(promise_awaiter&& other) noexcept {
    if (this != &other) {
      destroy_promise_state(state_);
      state_ = std::move(other.state_);
    }
    return *this;
  }

  inline ~promise_awaiter() noexcept {
    destroy_promise_state(state_);
  }

  inline bool await_ready() const noexcept {
    return !state_ || state_->completed_.load(std::memory_order_acquire);
  }

  inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    auto state = state_;
    unsigned int expected = 0u;

    if (!state) {
      return false;
    }

    state->continuation_ = continuation;
    state->continuation_bounce_ = ::bounce_get_core();
    if (state->completed_.load(std::memory_order_acquire)) {
      return false;
    }
    if (!state->suspend_state_.compare_exchange_strong(
      expected,
      1u,
      std::memory_order_acq_rel,
      std::memory_order_acquire)) {
      return expected == 3u;
    }
    state->start_returned_.store(1u, std::memory_order_release);
    if (!state->started_.exchange(true, std::memory_order_acq_rel)) {
      state->handle_.resume();
    }
    return true;
  }

  inline void await_resume() {
    if (!state_) {
      std::terminate();
    }

    state_->rethrow_if_failed();
    state_->storage_.consume();
  }
};

class cancellation_wait_start {
private:
  BOUNCE_CORE *core_;
  BOUNCE_CANCELLATION *cancellation_;
  std::unique_ptr<cancellation_registration> registration_;

public:
  inline cancellation_wait_start(
    BOUNCE_CORE *core,
    BOUNCE_CANCELLATION *cancellation)
    : core_(core),
      cancellation_(cancellation),
      registration_(new cancellation_registration()) {
  }

  cancellation_wait_start(const cancellation_wait_start&) = delete;
  cancellation_wait_start& operator=(const cancellation_wait_start&) = delete;

  inline cancellation_wait_start(cancellation_wait_start&& other) noexcept
    : core_(other.core_),
      cancellation_(other.cancellation_),
      registration_(std::move(other.registration_)) {
    other.core_ = nullptr;
    other.cancellation_ = nullptr;
  }

  inline cancellation_wait_start& operator=(cancellation_wait_start&& other) noexcept {
    if (this != &other) {
      core_ = other.core_;
      cancellation_ = other.cancellation_;
      registration_ = std::move(other.registration_);
      other.core_ = nullptr;
      other.cancellation_ = nullptr;
    }
    return *this;
  }

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION * /*unused*/) noexcept {
    return (core_ != nullptr) && (cancellation_ != nullptr) && (registration_ != nullptr) ?
             ::bounce_register_canceled(
               core_,
               cancellation_,
               registration_->get_registration(),
               completion,
               completion_state) :
             false;
  }
};

}  // namespace detail

/**
 * @brief Awaitable operation object that bridges libbounce callback APIs into
 * `co_await`.
 */
class await_operation {
private:
  struct state_base {
    std::atomic<unsigned int> suspend_state_;
    std::atomic<unsigned int> start_returned_;
    std::coroutine_handle<> continuation_;
    BOUNCE_CANCELLATION *cancellation_;
    await_result result_;

    explicit inline state_base(BOUNCE_CANCELLATION *cancellation) noexcept
      : suspend_state_(0u),
        start_returned_(0u),
        continuation_(),
        cancellation_(cancellation),
        result_ { await_status::aborted } {
    }

    virtual ~state_base() = default;

    virtual bool start() noexcept = 0;

    inline bool suspend(std::coroutine_handle<> continuation) noexcept {
      unsigned int expected = 0u;

      continuation_ = continuation;
      if (!suspend_state_.compare_exchange_strong(
        expected,
        1u,
        std::memory_order_acq_rel,
        std::memory_order_acquire)) {
        return expected == 3u;
      }
      start_returned_.store(1u, std::memory_order_release);
      if (!start()) {
        result_ = await_result { await_status::start_failed };
        (void)suspend_state_.exchange(3u, std::memory_order_acq_rel);
        return false;
      }

      return true;
    }

    inline await_result consume_result() noexcept {
      (void)suspend_state_.load(std::memory_order_acquire);
      return result_;
    }

    static inline void completion_callback(
      BOUNCE_COMPLETION_RESULT result,
      void *completion_state) noexcept {
      state_base *state = static_cast<state_base *>(completion_state);

      state->result_ = detail::await_result_from_completion(result);
      if (state->start_returned_.load(std::memory_order_acquire) != 0u) {
        const unsigned int previous =
          state->suspend_state_.exchange(3u, std::memory_order_acq_rel);

        if (previous == 1u) {
          state->continuation_.resume();
        }
        return;
      }

      (void)state->suspend_state_.exchange(2u, std::memory_order_acq_rel);
    }

  };

  template<typename START_FN>
  class start_state final : public state_base {
  private:
    using starter_type = typename std::decay<START_FN>::type;
    starter_type starter_;

  public:
    inline start_state(START_FN&& starter, BOUNCE_CANCELLATION *cancellation)
      : state_base(cancellation),
        starter_(std::forward<START_FN>(starter)) {
    }

    inline bool start() noexcept override {
      static_assert(
        std::is_invocable_v<
          starter_type&,
          BOUNCE_COMPLETION,
          void *,
          BOUNCE_CANCELLATION *>,
        "start must be invocable with (BOUNCE_COMPLETION, void*, BOUNCE_CANCELLATION*)");

      using start_result = std::invoke_result_t<
        starter_type&,
        BOUNCE_COMPLETION,
        void *,
        BOUNCE_CANCELLATION *>;

      static_assert(
        std::is_same_v<start_result, bool> ||
        std::is_same_v<start_result, void>,
        "start must return bool or void");

      try {
        if constexpr (std::is_same_v<start_result, bool>) {
          return starter_(
            &state_base::completion_callback,
            this,
            this->cancellation_);
        } else {
          starter_(
            &state_base::completion_callback,
            this,
            this->cancellation_);
          return true;
        }
      } catch (...) {
        return false;
      }
    }
  };

  std::shared_ptr<state_base> state_;
  await_result immediate_result_;

  explicit inline await_operation(await_result immediate_result) noexcept
    : state_(nullptr),
      immediate_result_(immediate_result) {
  }

  explicit inline await_operation(std::shared_ptr<state_base> state) noexcept
    : state_(std::move(state)),
      immediate_result_ { await_status::aborted } {
  }

public:
  inline await_operation() noexcept
    : state_(nullptr),
      immediate_result_ { await_status::start_failed } {
  }

  await_operation(const await_operation&) = delete;
  await_operation& operator=(const await_operation&) = delete;

  inline await_operation(await_operation&& other) noexcept = default;
  inline await_operation& operator=(await_operation&& other) noexcept = default;

  /**
   * @brief Check whether this operation is immediately ready.
   * @return True when no suspension is needed.
   */
  inline bool await_ready() const noexcept {
    return state_ == nullptr;
  }

  /**
   * @brief Start the underlying callback registration and suspend if needed.
   * @param continuation Continuation coroutine handle.
   * @return True when the coroutine should suspend.
   */
  inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    auto state = state_;

    if (state == nullptr) {
      return false;
    }

    if (!state->suspend(continuation)) {
      immediate_result_ = state->consume_result();
      state_.reset();
      return false;
    }
    return true;
  }

  /**
   * @brief Consume the final await result.
   * @return Final await result.
   */
  inline await_result await_resume() noexcept {
    if (state_ == nullptr) {
      return immediate_result_;
    }

    auto state = std::move(state_);
    const await_result result = state->consume_result();

    return result;
  }

  /**
   * @brief Create an operation that is already completed.
   * @param immediate_result Result returned without suspension.
   * @return Immediate operation object.
   */
  static inline await_operation from_immediate(await_result immediate_result) noexcept {
    return await_operation(immediate_result);
  }

  template<typename START_FN>
  static inline await_operation create(
    START_FN&& start,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    using state_type = start_state<START_FN>;

    try {
      return await_operation(
        std::make_shared<state_type>(
          std::forward<START_FN>(start),
          cancellation));
    } catch (...) {
      return from_immediate(await_result { await_status::start_failed });
    }
  }
};

/**
 * @brief Coroutine return object integrated with the libbounce await helpers.
 * @tparam T Returned value type, or `void`.
 * @remarks Destroying a started but not-yet-completed promise is a programming
 * error and terminates the process.
 */
template<typename T>
class promise {
public:
  struct promise_type;

private:
  using state_type = detail::promise_shared_state<T>;
  std::shared_ptr<state_type> state_;

public:
  /**
   * @brief Promise storage used by the compiler-generated coroutine frame.
   */
  struct promise_type {
    using value_type = T;
    using state_type = detail::promise_shared_state<T>;
    std::shared_ptr<state_type> state_;

    inline promise_type()
      : state_(std::make_shared<state_type>()) {
    }

    /**
     * @brief Build the public coroutine object.
     * @return Public coroutine object.
     */
    inline promise get_return_object() noexcept {
      auto handle = std::coroutine_handle<promise_type>::from_promise(*this);

      state_->handle_ = handle;
      return promise(state_);
    }

    /**
     * @brief Start lazily so the caller controls first resume.
     * @return Initial suspension marker.
     */
    inline std::suspend_always initial_suspend() const noexcept {
      return std::suspend_always {};
    }

    /**
     * @brief Resume the awaiting coroutine when this coroutine finishes.
     * @return Final suspension awaiter.
     */
    inline detail::promise_final_awaiter<promise_type> final_suspend() const noexcept {
      return detail::promise_final_awaiter<promise_type> {};
    }

    /**
     * @brief Store the returned value.
     * @param value Returned value.
     */
    template<typename U>
    inline void return_value(U&& value) {
      state_->storage_.emplace(std::forward<U>(value));
    }

    inline void unhandled_exception() noexcept {
      state_->capture_exception();
    }
  };

  /**
   * @brief Create an empty promise.
   */
  inline promise() noexcept
    : state_() {
  }

  explicit inline promise(std::shared_ptr<state_type> state) noexcept
    : state_(std::move(state)) {
  }

  promise(const promise&) = delete;
  promise& operator=(const promise&) = delete;

  inline promise(promise&& other) noexcept
    : state_(std::move(other.state_)) {
  }

  inline promise& operator=(promise&& other) noexcept {
    if (this != &other) {
      detail::destroy_promise_state(state_);
      state_ = std::move(other.state_);
    }
    return *this;
  }

  inline ~promise() noexcept {
    detail::destroy_promise_state(state_);
  }

  /**
   * @brief Check whether this promise currently owns a coroutine frame.
   * @return True when this promise currently owns a coroutine frame.
   */
  inline explicit operator bool() const noexcept {
    return static_cast<bool>(state_);
  }

  /**
   * @brief Check whether the coroutine has already finished.
   * @return True when the coroutine has already finished.
   */
  inline bool done() const noexcept {
    return !state_ || state_->completed_.load(std::memory_order_acquire);
  }

  /**
   * @brief Start the coroutine until its next suspension point.
   * @return True when this promise owns a coroutine frame.
   */
  inline bool start() noexcept {
    if (!state_) {
      return false;
    }

    if (!state_->started_.exchange(true, std::memory_order_acq_rel)) {
      state_->handle_.resume();
    }
    return true;
  }

  /**
   * @brief Consume this promise as an awaitable object.
   * @return Awaiter that transfers ownership of the coroutine frame.
   */
  inline auto operator co_await() & noexcept {
    return detail::promise_awaiter<T>(std::move(state_));
  }

  /**
   * @brief Consume this promise as an awaitable object.
   * @return Awaiter that transfers ownership of the coroutine frame.
   */
  inline auto operator co_await() && noexcept {
    return detail::promise_awaiter<T>(std::move(state_));
  }
};

/**
 * @brief Coroutine return object integrated with the libbounce await helpers.
 * @remarks Destroying a started but not-yet-completed promise is a programming
 * error and terminates the process.
 */
template<>
class promise<void> {
public:
  struct promise_type;

private:
  using state_type = detail::promise_shared_state<void>;
  std::shared_ptr<state_type> state_;

public:
  /**
   * @brief Promise storage used by the compiler-generated coroutine frame.
   */
  struct promise_type {
    using value_type = void;
    using state_type = detail::promise_shared_state<void>;
    std::shared_ptr<state_type> state_;

    inline promise_type()
      : state_(std::make_shared<state_type>()) {
    }

    /**
     * @brief Build the public coroutine object.
     * @return Public coroutine object.
     */
    inline promise get_return_object() noexcept {
      auto handle = std::coroutine_handle<promise_type>::from_promise(*this);

      state_->handle_ = handle;
      return promise(state_);
    }

    /**
     * @brief Start lazily so the caller controls first resume.
     * @return Initial suspension marker.
     */
    inline std::suspend_always initial_suspend() const noexcept {
      return std::suspend_always {};
    }

    /**
     * @brief Resume the awaiting coroutine when this coroutine finishes.
     * @return Final suspension awaiter.
     */
    inline detail::promise_final_awaiter<promise_type> final_suspend() const noexcept {
      return detail::promise_final_awaiter<promise_type> {};
    }

    /**
     * @brief Store a void return.
     */
    inline void return_void() noexcept {
      state_->storage_.emplace();
    }

    inline void unhandled_exception() noexcept {
      state_->capture_exception();
    }
  };

  /**
   * @brief Create an empty promise.
   */
  inline promise() noexcept
    : state_() {
  }

  explicit inline promise(std::shared_ptr<state_type> state) noexcept
    : state_(std::move(state)) {
  }

  promise(const promise&) = delete;
  promise& operator=(const promise&) = delete;

  inline promise(promise&& other) noexcept
    : state_(std::move(other.state_)) {
  }

  inline promise& operator=(promise&& other) noexcept {
    if (this != &other) {
      detail::destroy_promise_state(state_);
      state_ = std::move(other.state_);
    }
    return *this;
  }

  inline ~promise() noexcept {
    detail::destroy_promise_state(state_);
  }

  /**
   * @brief Check whether this promise currently owns a coroutine frame.
   * @return True when this promise currently owns a coroutine frame.
   */
  inline explicit operator bool() const noexcept {
    return static_cast<bool>(state_);
  }

  /**
   * @brief Check whether the coroutine has already finished.
   * @return True when the coroutine has already finished.
   */
  inline bool done() const noexcept {
    return !state_ || state_->completed_.load(std::memory_order_acquire);
  }

  /**
   * @brief Start the coroutine until its next suspension point.
   * @return True when this promise owns a coroutine frame.
   */
  inline bool start() noexcept {
    if (!state_) {
      return false;
    }

    if (!state_->started_.exchange(true, std::memory_order_acq_rel)) {
      state_->handle_.resume();
    }
    return true;
  }

  /**
   * @brief Consume this promise as an awaitable object.
   * @return Awaiter that transfers ownership of the coroutine frame.
   */
  inline auto operator co_await() & noexcept {
    return detail::promise_awaiter<void>(std::move(state_));
  }

  /**
   * @brief Consume this promise as an awaitable object.
   * @return Awaiter that transfers ownership of the coroutine frame.
   */
  inline auto operator co_await() && noexcept {
    return detail::promise_awaiter<void>(std::move(state_));
  }
};

namespace detail {

class detached_promise_runner {
public:
  struct promise_type;

private:
  std::coroutine_handle<promise_type> handle_;

  explicit inline detached_promise_runner(
    std::coroutine_handle<promise_type> handle) noexcept
    : handle_(handle) {
  }

public:
  struct promise_type {
    struct final_awaiter {
      inline bool await_ready() const noexcept {
        return false;
      }

      inline void await_suspend(
        std::coroutine_handle<promise_type> completed_handle) const noexcept {
        completed_handle.destroy();
      }

      inline void await_resume() const noexcept {
      }
    };

    inline detached_promise_runner get_return_object() noexcept {
      return detached_promise_runner(
        std::coroutine_handle<promise_type>::from_promise(*this));
    }

    inline std::suspend_always initial_suspend() const noexcept {
      return std::suspend_always {};
    }

    inline final_awaiter final_suspend() const noexcept {
      return final_awaiter {};
    }

    inline void return_void() const noexcept {
    }

    inline void unhandled_exception() const noexcept {
      std::terminate();
    }
  };

  detached_promise_runner(const detached_promise_runner&) = delete;
  detached_promise_runner& operator=(const detached_promise_runner&) = delete;

  inline detached_promise_runner(detached_promise_runner&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = std::coroutine_handle<promise_type> {};
  }

  inline detached_promise_runner& operator=(detached_promise_runner&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = other.handle_;
      other.handle_ = std::coroutine_handle<promise_type> {};
    }
    return *this;
  }

  inline ~detached_promise_runner() noexcept {
    if (handle_) {
      handle_.destroy();
    }
  }

  inline bool start() noexcept {
    if (!handle_) {
      return false;
    }

    std::coroutine_handle<promise_type> handle = handle_;

    handle_ = std::coroutine_handle<promise_type> {};
    handle.resume();
    return true;
  }
};

template<typename T>
static inline detached_promise_runner fire_and_forget_impl(promise<T> operation) {
  if constexpr (std::is_void_v<T>) {
    co_await operation;
  } else {
    (void)co_await operation;
  }
}

template<typename TResult, typename TMAPPER, typename... TCALLBACK_ARGS>
class callback_promise_context {
private:
  using mapper_type = typename std::decay<TMAPPER>::type;

  BOUNCE_CORE *bounce_;
  BOUNCE_COMPLETION completion_ = nullptr;
  void *completion_state_ = nullptr;
  mapper_type mapper_;
  promise_storage<TResult> storage_{};
  std::exception_ptr exception_{};

  static inline void complete_on_bounce(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    auto *context =
      static_cast<callback_promise_context *>(completion_state);

    if (context->completion_ != nullptr) {
      context->completion_(result, context->completion_state_);
    }
  }

public:
  template<typename TMAPPER_ARG>
  explicit inline callback_promise_context(
    BOUNCE_CORE *bounce,
    TMAPPER_ARG&& mapper)
    : bounce_(bounce),
      mapper_(std::forward<TMAPPER_ARG>(mapper)) {
  }

  callback_promise_context(const callback_promise_context&) = delete;
  callback_promise_context& operator=(const callback_promise_context&) = delete;

  callback_promise_context(callback_promise_context&&) = delete;
  callback_promise_context& operator=(callback_promise_context&&) = delete;

  template<typename TSTART_FN>
  inline bool start(
    TSTART_FN &start,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    using callback_type = void (*)(void *, TCALLBACK_ARGS...);

    completion_ = completion;
    completion_state_ = completion_state;

    try {
      if constexpr (
        std::is_invocable_v<
          TSTART_FN&,
          callback_type,
          void *,
          BOUNCE_CANCELLATION *>) {
        using start_result = std::invoke_result_t<
          TSTART_FN&,
          callback_type,
          void *,
          BOUNCE_CANCELLATION *>;

        static_assert(
          std::is_same_v<start_result, bool> ||
          std::is_same_v<start_result, void>,
          "start must return bool or void");

        if constexpr (std::is_same_v<start_result, bool>) {
          return start(
            &callback_promise_context::callback,
            this,
            cancellation);
        } else {
          start(
            &callback_promise_context::callback,
            this,
            cancellation);
          return true;
        }
      } else {
        static_assert(
          std::is_invocable_v<TSTART_FN&, callback_type, void *>,
          "start must be invocable with (callback, void*) or (callback, void*, BOUNCE_CANCELLATION*)");

        using start_result = std::invoke_result_t<
          TSTART_FN&,
          callback_type,
          void *>;

        static_assert(
          std::is_same_v<start_result, bool> ||
          std::is_same_v<start_result, void>,
          "start must return bool or void");

        if constexpr (std::is_same_v<start_result, bool>) {
          return start(
            &callback_promise_context::callback,
            this);
        } else {
          start(
            &callback_promise_context::callback,
            this);
          return true;
        }
      }
    } catch (...) {
      exception_ = std::current_exception();
      return false;
    }
  }

  inline callback_promise_result<TResult> consume(await_result await) {
    if (exception_) {
      std::exception_ptr captured = exception_;

      exception_ = nullptr;
      std::rethrow_exception(captured);
    }

    callback_promise_result<TResult> result {};
    result.await = await;
    if constexpr (!std::is_void_v<TResult>) {
      if (await.completed()) {
        result.value.emplace(storage_.consume());
      }
    }
    return result;
  }

  static inline void callback(
    void *opaque,
    TCALLBACK_ARGS... args) noexcept {
    auto *context = static_cast<callback_promise_context *>(opaque);
    BOUNCE_COMPLETION_RESULT completion_result =
      BOUNCE_COMPLETION_COMPLETED;

    try {
      if constexpr (std::is_void_v<TResult>) {
        context->mapper_(args...);
        context->storage_.emplace();
      } else {
        context->storage_.emplace(context->mapper_(args...));
      }
    } catch (...) {
      context->exception_ = std::current_exception();
      completion_result = BOUNCE_COMPLETION_ABORTED;
    }

    if ((context->bounce_ != nullptr) &&
        ::bounce_post(
          context->bounce_,
          &callback_promise_context::complete_on_bounce,
          context)) {
      return;
    }

    complete_on_bounce(completion_result, context);
  }
};

template<typename TResult, typename... TCALLBACK_ARGS, typename TBOUNCE_HANDLE, typename TSTART_FN, typename TMAPPER>
static inline promise<callback_promise_result<TResult>>
make_callback_promise_impl(
  TBOUNCE_HANDLE &bounce_handle,
  TSTART_FN&& start,
  TMAPPER&& mapper,
  BOUNCE_CANCELLATION *cancellation) {
  using context_type =
    callback_promise_context<TResult, TMAPPER, TCALLBACK_ARGS...>;
  using start_type = typename std::decay<TSTART_FN>::type;

  start_type starter(std::forward<TSTART_FN>(start));
  context_type context(
    bounce_handle.get_core(),
    std::forward<TMAPPER>(mapper));
  const await_result result =
    co_await make_awaitable(
      bounce_handle,
      [&context, &starter](
        BOUNCE_COMPLETION completion,
        void *completion_state,
        BOUNCE_CANCELLATION *operation_cancellation) noexcept -> bool {
        return context.start(
          starter,
          completion,
          completion_state,
          operation_cancellation);
      },
      cancellation);

  co_return context.consume(result);
}

}  // namespace detail

template<typename TBOUNCE_HANDLE, typename START_FN>
/**
 * @brief Build a coroutine awaitable from a callback-based libbounce starter.
 * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
 * @tparam START_FN Callback-based starter type.
 * @param bounce_handle Bounce handle used to bind the operation.
 * @param start Starter that accepts `(BOUNCE_COMPLETION, void*, BOUNCE_CANCELLATION*)`.
 * @param cancellation Cancellation when provided.
 * @return Coroutine awaitable operation.
 */
inline await_operation make_awaitable(
  TBOUNCE_HANDLE &bounce_handle,
  START_FN&& start,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  return (bounce_handle.get_core() != nullptr) ?
           await_operation::create(
             std::forward<START_FN>(start),
             cancellation) :
           await_operation::from_immediate(await_result { await_status::start_failed });
}

template<typename TResult, typename... TCALLBACK_ARGS, typename TBOUNCE_HANDLE, typename TSTART_FN, typename TMAPPER>
/**
 * @brief Build a coroutine promise from a single-shot C callback registration.
 * @tparam TResult Mapped callback payload type, or `void`.
 * @tparam TCALLBACK_ARGS Callback payload argument types that follow the opaque
 * state pointer.
 * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
 * @tparam TSTART_FN Callback registration starter type.
 * @tparam TMAPPER Mapper that converts callback payload arguments into
 * @p TResult.
 * @param bounce_handle Bounce handle used to marshal continuation resumption.
 * @param start Starter that accepts `(callback, void*)` or
 * `(callback, void*, BOUNCE_CANCELLATION*)`, where `callback` is a
 * single-shot `void (*)(void*, TCALLBACK_ARGS...)`.
 * @param mapper Mapper invoked from the callback to convert payload arguments.
 * @param cancellation Optional cancellation forwarded to @p start.
 * @return Promise that resolves to @ref callback_promise_result once the
 * callback fires or startup fails.
 * @remarks The callback convention handled here requires the opaque state
 * pointer as the first callback argument.
 */
inline promise<callback_promise_result<TResult>> make_callback_promise(
  TBOUNCE_HANDLE &bounce_handle,
  TSTART_FN&& start,
  TMAPPER&& mapper,
  BOUNCE_CANCELLATION *cancellation = nullptr) {
  return detail::make_callback_promise_impl<
    TResult,
    TCALLBACK_ARGS...>(
      bounce_handle,
      std::forward<TSTART_FN>(start),
      std::forward<TMAPPER>(mapper),
      cancellation);
}

template<typename T>
/**
 * @brief Start a libbounce coroutine and detach it until completion.
 * @tparam T Promise result type. The result value is discarded.
 * @param operation Promise whose coroutine frame is transferred to the
 * detached runner.
 * @return True when the promise was non-empty and the detached runner started.
 * @remarks The caller must pass the promise with `std::move()`. Any exception
 * escaping from the detached coroutine is treated as unhandled and terminates
 * the process.
 */
inline bool fire_and_forget(promise<T>&& operation) noexcept {
  if (!operation) {
    return false;
  }

  try {
    return detail::fire_and_forget_impl<T>(std::move(operation)).start();
  } catch (...) {
    return false;
  }
}

template<typename TBOUNCE_HANDLE>
/**
 * @brief Hop the current coroutine onto a bounce parker through `post()`.
 * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
 * @param bounce_handle Bounce handle used to publish the resume callback.
 * @return Coroutine awaitable operation.
 */
inline await_operation resume_on(TBOUNCE_HANDLE &bounce_handle) noexcept {
  BOUNCE_CORE *core = bounce_handle.get_core();

  return (core != nullptr) ?
           await_operation::create(
             [core](
               BOUNCE_COMPLETION completion,
               void *completion_state,
               BOUNCE_CANCELLATION * /*cancellation*/) noexcept -> bool {
               return ::bounce_post(core, completion, completion_state);
             },
             nullptr) :
           await_operation::from_immediate(await_result { await_status::start_failed });
}

template<typename TBOUNCE_HANDLE>
/**
 * @brief Await cancellation notification through a coroutine.
 * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
 * @param bounce_handle Bounce handle used to publish the cancellation completion.
 * @param cancellation_source Cancellation source to observe.
 * @return Coroutine awaitable operation.
 */
inline await_operation await_canceled(
  TBOUNCE_HANDLE &bounce_handle,
  cancellation &cancellation_source) noexcept {
  BOUNCE_CORE *core = bounce_handle.get_core();

  if (core == nullptr) {
    return await_operation::from_immediate(await_result { await_status::start_failed });
  }

  try {
    return await_operation::create(
      detail::cancellation_wait_start(
        core,
        cancellation_source.get_cancellation()),
      nullptr);
  } catch (...) {
    return await_operation::from_immediate(await_result { await_status::start_failed });
  }
}

#if defined(BOUNCE_POSIX)
inline await_operation bounce_ref::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_POSIX_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce_ref::await(
  int fd,
  short events,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, events](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_fd(
        core,
        fd,
        events,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_POSIX_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  int fd,
  short events,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, events](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_fd(
        core,
        fd,
        events,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

#if defined(__linux__)
inline await_operation bounce_ref::await(
  BOUNCE_POSIX_IO_URING_OP &operation,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, &operation](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_io_uring_op(
        core,
        &operation,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  BOUNCE_POSIX_IO_URING_OP &operation,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, &operation](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_io_uring_op(
        core,
        &operation,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}
#endif

#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB) || defined(_WIN32)
namespace detail {

template<typename START_FN>
static inline await_operation make_file_awaitable_core(
  BOUNCE_CORE *core,
  START_FN&& start,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  return (core != nullptr) ?
           await_operation::create(
             std::forward<START_FN>(start),
             cancellation) :
           await_operation::from_immediate(
             await_result { await_status::start_failed });
}

struct file_read_start {
  BOUNCE_CORE *core;
  BOUNCE_FILE_IO *operation;
  BOUNCE_FILE_HANDLE handle;
  void *buffer;
  int64_t offset;
  size_t length;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_file_read(
      core,
      operation,
      handle,
      buffer,
      offset,
      length,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct file_write_start {
  BOUNCE_CORE *core;
  BOUNCE_FILE_IO *operation;
  BOUNCE_FILE_HANDLE handle;
  const void *buffer;
  int64_t offset;
  size_t length;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_file_write(
      core,
      operation,
      handle,
      buffer,
      offset,
      length,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct file_seek_start {
  BOUNCE_CORE *core;
  BOUNCE_FILE_IO *operation;
  BOUNCE_FILE_HANDLE handle;
  int64_t offset;
  int whence;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_file_seek(
      core,
      operation,
      handle,
      offset,
      whence,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct file_flush_start {
  BOUNCE_CORE *core;
  BOUNCE_FILE_IO *operation;
  BOUNCE_FILE_HANDLE handle;
  BOUNCE_FILE_FLUSH_MODE mode;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_file_flush(
      core,
      operation,
      handle,
      mode,
      completion,
      completion_state,
      operation_cancellation);
  }
};

static inline promise<file_io_result> file_read_async_core(
  BOUNCE_CORE *core,
  BOUNCE_FILE_HANDLE handle,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  file_io operation;
  const await_result result =
    co_await make_file_awaitable_core(
      core,
      file_read_start {
        core,
        operation.get_file_io(),
        handle,
        buffer,
        offset,
        length },
      cancellation);

  co_return file_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<file_io_result> file_write_async_core(
  BOUNCE_CORE *core,
  BOUNCE_FILE_HANDLE handle,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  file_io operation;
  const await_result result =
    co_await make_file_awaitable_core(
      core,
      file_write_start {
        core,
        operation.get_file_io(),
        handle,
        buffer,
        offset,
        length },
      cancellation);

  co_return file_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<file_io_result> file_seek_async_core(
  BOUNCE_CORE *core,
  BOUNCE_FILE_HANDLE handle,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation) {
  file_io operation;
  const await_result result =
    co_await make_file_awaitable_core(
      core,
      file_seek_start {
        core,
        operation.get_file_io(),
        handle,
        offset,
        whence },
      cancellation);

  co_return file_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<file_io_result> file_flush_async_core(
  BOUNCE_CORE *core,
  BOUNCE_FILE_HANDLE handle,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_CANCELLATION *cancellation) {
  file_io operation;
  const await_result result =
    co_await make_file_awaitable_core(
      core,
      file_flush_start {
        core,
        operation.get_file_io(),
        handle,
        mode },
      cancellation);

  co_return file_io_result(
    result,
    operation.result(),
    operation.error());
}

}  // namespace detail
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
namespace detail {

template<typename START_FN>
static inline await_operation make_socket_awaitable_core(
  BOUNCE_CORE *core,
  START_FN&& start,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  return (core != nullptr) ?
           await_operation::create(
             std::forward<START_FN>(start),
             cancellation) :
           await_operation::from_immediate(
             await_result { await_status::start_failed });
}

struct socket_recv_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  void *buffer;
  size_t length;
  int flags;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_recv(
      core,
      operation,
      fd,
      buffer,
      length,
      flags,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct socket_send_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  const void *buffer;
  size_t length;
  int flags;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_send(
      core,
      operation,
      fd,
      buffer,
      length,
      flags,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct socket_recv_from_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  void *buffer;
  size_t length;
  int flags;
  struct sockaddr *address;
  socklen_t *address_length;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_recvfrom(
      core,
      operation,
      fd,
      buffer,
      length,
      flags,
      address,
      address_length,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct socket_send_to_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  const void *buffer;
  size_t length;
  int flags;
  const struct sockaddr *address;
  socklen_t address_length;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_sendto(
      core,
      operation,
      fd,
      buffer,
      length,
      flags,
      address,
      address_length,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct socket_recv_msg_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  struct msghdr *message;
  int flags;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_recvmsg(
      core,
      operation,
      fd,
      message,
      flags,
      completion,
      completion_state,
      operation_cancellation);
  }
};

struct socket_send_msg_start {
  BOUNCE_CORE *core;
  BOUNCE_SOCKET_IO *operation;
  int fd;
  const struct msghdr *message;
  int flags;

  inline bool operator()(
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *operation_cancellation) const noexcept {
    return ::bounce_await_socket_sendmsg(
      core,
      operation,
      fd,
      message,
      flags,
      completion,
      completion_state,
      operation_cancellation);
  }
};

static inline promise<socket_io_result> socket_recv_async_core(
  BOUNCE_CORE *core,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_recv_start {
        core,
        operation.get_socket_io(),
        fd,
        buffer,
        length,
        flags },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<socket_io_result> socket_send_async_core(
  BOUNCE_CORE *core,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_send_start {
        core,
        operation.get_socket_io(),
        fd,
        buffer,
        length,
        flags },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<socket_io_result> socket_recv_from_async_core(
  BOUNCE_CORE *core,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_recv_from_start {
        core,
        operation.get_socket_io(),
        fd,
        buffer,
        length,
        flags,
        address,
        address_length },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<socket_io_result> socket_send_to_async_core(
  BOUNCE_CORE *core,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_send_to_start {
        core,
        operation.get_socket_io(),
        fd,
        buffer,
        length,
        flags,
        address,
        address_length },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<socket_io_result> socket_recv_msg_async_core(
  BOUNCE_CORE *core,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_recv_msg_start {
        core,
        operation.get_socket_io(),
        fd,
        message,
        flags },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

static inline promise<socket_io_result> socket_send_msg_async_core(
  BOUNCE_CORE *core,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  socket_io operation;
  const await_result result =
    co_await make_socket_awaitable_core(
      core,
      socket_send_msg_start {
        core,
        operation.get_socket_io(),
        fd,
        message,
        flags },
      cancellation);

  co_return socket_io_result(
    result,
    operation.result(),
    operation.error());
}

}  // namespace detail
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB) || defined(_WIN32)
inline promise<file_io_result> read_async(
  bounce &bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_read_async_core(
    bounce_handle.get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> read_async(
  bounce_ref bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_read_async_core(
    bounce_handle.get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> read_async(
  BOUNCE_FILE_HANDLE handle,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_read_async_core(
    ::bounce_get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> write_async(
  bounce &bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_write_async_core(
    bounce_handle.get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> write_async(
  bounce_ref bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_write_async_core(
    bounce_handle.get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> write_async(
  BOUNCE_FILE_HANDLE handle,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_write_async_core(
    ::bounce_get_core(),
    handle,
    buffer,
    offset,
    length,
    cancellation);
}

inline promise<file_io_result> seek_async(
  bounce &bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_seek_async_core(
    bounce_handle.get_core(),
    handle,
    offset,
    whence,
    cancellation);
}

inline promise<file_io_result> seek_async(
  bounce_ref bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_seek_async_core(
    bounce_handle.get_core(),
    handle,
    offset,
    whence,
    cancellation);
}

inline promise<file_io_result> seek_async(
  BOUNCE_FILE_HANDLE handle,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_seek_async_core(
    ::bounce_get_core(),
    handle,
    offset,
    whence,
    cancellation);
}

inline promise<file_io_result> flush_async(
  bounce &bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_flush_async_core(
    bounce_handle.get_core(),
    handle,
    mode,
    cancellation);
}

inline promise<file_io_result> flush_async(
  bounce_ref bounce_handle,
  BOUNCE_FILE_HANDLE handle,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_flush_async_core(
    bounce_handle.get_core(),
    handle,
    mode,
    cancellation);
}

inline promise<file_io_result> flush_async(
  BOUNCE_FILE_HANDLE handle,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::file_flush_async_core(
    ::bounce_get_core(),
    handle,
    mode,
    cancellation);
}
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
inline promise<socket_io_result> recv_async(
  bounce &bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> recv_async(
  bounce_ref bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> recv_async(
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_async_core(
    ::bounce_get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_async(
  bounce &bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_async(
  bounce_ref bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_async(
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_async_core(
    ::bounce_get_core(),
    fd,
    buffer,
    length,
    flags,
    cancellation);
}

inline promise<socket_io_result> recv_from_async(
  bounce &bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_from_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> recv_from_async(
  bounce_ref bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_from_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> recv_from_async(
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_from_async_core(
    ::bounce_get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> send_to_async(
  bounce &bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_to_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> send_to_async(
  bounce_ref bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_to_async_core(
    bounce_handle.get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> send_to_async(
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_to_async_core(
    ::bounce_get_core(),
    fd,
    buffer,
    length,
    flags,
    address,
    address_length,
    cancellation);
}

inline promise<socket_io_result> recv_msg_async(
  bounce &bounce_handle,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_msg_async_core(
    bounce_handle.get_core(),
    fd,
    message,
    flags,
    cancellation);
}

inline promise<socket_io_result> recv_msg_async(
  bounce_ref bounce_handle,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_msg_async_core(
    bounce_handle.get_core(),
    fd,
    message,
    flags,
    cancellation);
}

inline promise<socket_io_result> recv_msg_async(
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_recv_msg_async_core(
    ::bounce_get_core(),
    fd,
    message,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_msg_async(
  bounce &bounce_handle,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_msg_async_core(
    bounce_handle.get_core(),
    fd,
    message,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_msg_async(
  bounce_ref bounce_handle,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_msg_async_core(
    bounce_handle.get_core(),
    fd,
    message,
    flags,
    cancellation);
}

inline promise<socket_io_result> send_msg_async(
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation) {
  return detail::socket_send_msg_async_core(
    ::bounce_get_core(),
    fd,
    message,
    flags,
    cancellation);
}
#endif

#if defined(BOUNCE_POSIX_GLIB)
inline await_operation bounce_ref::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_POSIX_GLIB_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce_ref::await(
  int fd,
  GIOCondition condition,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, condition](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_fd(
        core,
        fd,
        condition,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_POSIX_GLIB_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  int fd,
  GIOCondition condition,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, condition](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_fd(
        core,
        fd,
        condition,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

#if defined(__linux__)
inline await_operation bounce_ref::await(
  BOUNCE_POSIX_IO_URING_OP &operation,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, &operation](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_io_uring_op(
        core,
        &operation,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  BOUNCE_POSIX_IO_URING_OP &operation,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, &operation](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_posix_glib_io_uring_op(
        core,
        &operation,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}
#endif
#endif

#if defined(BOUNCE_FREERTOS)
inline await_operation bounce_ref::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_FREERTOS_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_freertos_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  condition &condition_instance,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();
  BOUNCE_FREERTOS_CONDITION *condition_storage = condition_instance.get_condition();

  return make_awaitable(
    *this,
    [core, condition_storage](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_freertos_condition(
        core,
        condition_storage,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
inline await_operation bounce_ref::await(
  int fd,
  unsigned int events,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, events](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_freertos_fd(
        core,
        fd,
        events,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  int fd,
  unsigned int events,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, fd, events](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_freertos_fd(
        core,
        fd,
        events,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}
#endif
#endif

#if defined(_WIN32)
inline await_operation bounce_ref::await(
  HANDLE handle,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, handle](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_win32_handle(
        core,
        handle,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}

inline await_operation bounce::await(
  HANDLE handle,
  BOUNCE_CANCELLATION *cancellation) noexcept {
  BOUNCE_CORE *core = this->get_core();

  return make_awaitable(
    *this,
    [core, handle](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION *operation_cancellation) noexcept {
      ::bounce_await_win32_handle(
        core,
        handle,
        completion,
        completion_state,
        operation_cancellation);
    },
    cancellation);
}
#endif

}  // namespace libbounce

#endif
