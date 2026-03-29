/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_TIMER_H
#define _LIBBOUNCE_TIMER_H

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
extern void bounce_timer_init(BOUNCE_TIMER *timer);

/**
 * @brief Set timeout.
 * @param duration_msec Timeout when reached this duration later.
 * @param timer Initialized timer. Only one timeout may be active per timer.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when succeeded.
 * @remarks Local timer setup failures are returned directly as false. Backend
 * registration failures are reported asynchronously through completion with
 * `BOUNCE_COMPLETION_ABORTED`.
 */
extern bool bounce_await_timeout(BOUNCE_CORE *r,
  BOUNCE_TIMER *timer,
  uint32_t duration_msec,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Deinitialize a timer.
 * @param timer Timer storage provided by the caller.
 * @remarks If a timeout is still active, this call blocks until that request is
 * settled. Calling this from the timer completion is allowed.
 */
extern void bounce_timer_deinit(BOUNCE_TIMER *timer);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

template <typename TBOUNCE_CORE, typename TBOUNCE_TIMER> class timer_base {
private:
  TBOUNCE_TIMER timer_;
  timer_base(const timer_base&) = delete;
  timer_base(timer_base&&) = delete;
  timer_base& operator=(const timer_base&) = delete;
  timer_base& operator=(timer_base&&) = delete;

protected:
  template<typename COMPLETION_TYPE>
  static inline void callable_completion(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    bounce_base<TBOUNCE_CORE>::template callable_completion<COMPLETION_TYPE>(
      result,
      completion_state);
  }

  inline timer_base() noexcept {
    ::bounce_timer_init(&timer_);
  }

  inline ~timer_base() noexcept {
    ::bounce_timer_deinit(&timer_);
  }

public:
  /**
   * @brief Get the underlying timer storage.
   * @return Backend timer storage pointer.
   */
  inline TBOUNCE_TIMER *get_timer() noexcept {
    return &timer_;
  }

  /**
   * @brief Await a timeout through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param duration_msec Timeout duration in milliseconds.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local timer setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool wait(
    TBOUNCE_HANDLE &bounce_handle,
    uint32_t duration_msec,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_timeout(
               core,
               &timer_,
               duration_msec,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a timeout with a C++ callable completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param duration_msec Timeout duration in milliseconds.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup and timer registration succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool wait(
    TBOUNCE_HANDLE &bounce_handle,
    uint32_t duration_msec,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type TIMER_COMPLETION_TYPE;
    std::unique_ptr<TIMER_COMPLETION_TYPE> completion_state;
    TBOUNCE_CORE *core = bounce_handle.get_core();

    if (core == nullptr) {
      return false;
    }

    try {
      completion_state.reset(
        new TIMER_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!::bounce_await_timeout(
          core,
          &timer_,
          duration_msec,
          &callable_completion<TIMER_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }
};

}
#endif

#endif
