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

template<typename TPROMISE>
struct promise_final_awaiter {
  inline bool await_ready() const noexcept {
    return false;
  }

  inline void await_suspend(std::coroutine_handle<TPROMISE> completed_handle) const noexcept {
    auto state = completed_handle.promise().state_;

    state->completed_.store(true, std::memory_order_release);
    if (!state->continuation_) {
      return;
    }

    if ((state->continuation_bounce_ != nullptr) &&
        ::bounce_post(
          state->continuation_bounce_,
          &promise_resume_continuation<TPROMISE>,
          state.get())) {
      return;
    }

    state->continuation_.resume();
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
    if (!state_) {
      return false;
    }

    state_->continuation_ = continuation;
    state_->continuation_bounce_ = ::bounce_get_core();
    if (!state_->started_.exchange(true, std::memory_order_acq_rel)) {
      state_->handle_.resume();
    }
    return !state_->completed_.load(std::memory_order_acquire);
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
    if (!state_) {
      return false;
    }

    state_->continuation_ = continuation;
    state_->continuation_bounce_ = ::bounce_get_core();
    if (!state_->started_.exchange(true, std::memory_order_acq_rel)) {
      state_->handle_.resume();
    }
    return !state_->completed_.load(std::memory_order_acquire);
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
      if (!start()) {
        result_ = await_result { await_status::start_failed };
        return false;
      }

      start_returned_.store(1u, std::memory_order_release);
      if (suspend_state_.compare_exchange_strong(
        expected,
        1u,
        std::memory_order_acq_rel,
        std::memory_order_acquire)) {
        return true;
      }
      return expected == 3u;
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

        if ((previous == 0u) || (previous == 1u)) {
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

  std::unique_ptr<state_base> state_;
  await_result immediate_result_;

  explicit inline await_operation(await_result immediate_result) noexcept
    : state_(nullptr),
      immediate_result_(immediate_result) {
  }

  explicit inline await_operation(std::unique_ptr<state_base>&& state) noexcept
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
    if (state_ == nullptr) {
      return false;
    }

    if (!state_->suspend(continuation)) {
      immediate_result_ = state_->consume_result();
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

    const await_result result = state_->consume_result();

    state_.reset();
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
        std::unique_ptr<state_base>(
          new state_type(
            std::forward<START_FN>(start),
            cancellation)));
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
  BOUNCE_CANCELLATION *cancellation = nullptr) noexcept {
  return (bounce_handle.get_core() != nullptr) ?
           await_operation::create(
             std::forward<START_FN>(start),
             cancellation) :
           await_operation::from_immediate(await_result { await_status::start_failed });
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
#endif

#if defined(BOUNCE_POSIX_GLIB)
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
