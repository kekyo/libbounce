/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIB_BOUNCE_H
#define _LIB_BOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BOUNCE_CORE BOUNCE_CORE;
typedef struct BOUNCE_CANCELLATION BOUNCE_CANCELLATION;
typedef struct BOUNCE_CANCELLATION_REGISTRATION BOUNCE_CANCELLATION_REGISTRATION;
typedef struct BOUNCE_TIMER BOUNCE_TIMER;

/**
 * @brief State of a completion item.
 */
typedef enum BOUNCE_COMPLETION_ITEM_STATE {
  BOUNCE_COMPLETION_ITEM_STATE_FREE = 0,
  BOUNCE_COMPLETION_ITEM_STATE_READY = 1,
  BOUNCE_COMPLETION_ITEM_STATE_WAITING = 2,
  BOUNCE_COMPLETION_ITEM_STATE_CLAIMED = 3,
  BOUNCE_COMPLETION_ITEM_STATE_ABORTED = 4
} BOUNCE_COMPLETION_ITEM_STATE;

typedef enum BOUNCE_COMPLETION_RESULT {
  BOUNCE_COMPLETION_COMPLETED,  // Normally completed.
  BOUNCE_COMPLETION_CANCELED,   // Request canceled by cancellation.
  BOUNCE_COMPLETION_ABORTED     // Request aborted by fatal state or bounce is shutted down.
} BOUNCE_COMPLETION_RESULT;

/**
 * @brief A type for completion callback.
 * @param result Completion result.
 * @param completion_state Completion callback state, user provided.
 */
typedef void (*BOUNCE_COMPLETION)(BOUNCE_COMPLETION_RESULT result, void *completion_state);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
extern void bounce_init(BOUNCE_CORE *r);

/**
 * @brief Post completion, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @return True when succeeded.
 */
extern bool bounce_post(BOUNCE_CORE *r,
  BOUNCE_COMPLETION completion,
  void *completion_state);

/**
 * @brief Park current thread and run continuation repeatedly with inline nesting control.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 * @remarks The thread will block inside. Release when `shutdown()` called.
 * A zero value disables inline nested execution and preserves the
 * traditional ready-queue-only behavior.
 */
extern bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth);

/**
 * @brief Pump current thread once without waiting for new completion work.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 * @remarks This executes completion work that is already immediately
 * dispatchable and then returns without blocking for future work. A zero value
 * disables inline nested execution and preserves the traditional
 * ready-queue-only behavior.
 */
extern bool bounce_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth);

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 * @param wait_for_idle When true, parked threads or tasks keep running until
 * all already-pending wait operations settle. When false, they may leave even
 * while wait operations are still pending.
 */
extern void bounce_shutdown(BOUNCE_CORE *r, bool wait_for_idle);

/**
 * @brief Deinitialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
extern void bounce_deinit(BOUNCE_CORE *r);

/**
 * @brief Set the current thread/task-local bounce core pointer.
 * @param b Bounce core to publish from `bounce_get_core()`, or `NULL` to clear.
 * @remarks Parked threads or tasks may call this explicitly before entering
 * third-party code that needs to discover the current bounce. The runtime does
 * not attach this pointer automatically when `bounce_park()` runs. When this
 * pointer is `NULL`, `bounce_get_core()` may still return the process-wide
 * fallback core when one is configured.
 */
extern void bounce_set_core(BOUNCE_CORE *b);

/**
 * @brief Set the process-wide fallback bounce core pointer.
 * @param b Bounce core to return from `bounce_get_core()` when the current
 * thread or task has no attached core, or `NULL` to clear the fallback.
 * @remarks This fallback is shared by the library instance and is not stored
 * in thread-local storage.
 */
extern void bounce_set_fallback_core(BOUNCE_CORE *b);

/**
 * @brief Get the current thread/task-local bounce core pointer.
 * @return The current bounce core pointer, or the process-wide fallback core
 * when no bounce is attached to the current thread or task. Returns `NULL`
 * when neither is available.
 */
extern BOUNCE_CORE *bounce_get_core(void);

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a cancellation.
 * @param cancellation Cancellation storage provided by the caller.
 */
extern void bounce_cancellation_init(BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Cancel a cancellation.
 * @param r Initialized BOUNCE_CORE.
 * @param cancellation Initialized cancellation.
 * @remarks The first successful call wins. Later calls are ignored.
 */
extern void bounce_cancel(BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Deinitialize a cancellation.
 * @param cancellation Cancellation storage provided by the caller.
 */
extern void bounce_cancellation_deinit(BOUNCE_CANCELLATION *cancellation);

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a cancellation registration.
 * @param registration Cancellation registration storage provided by the caller.
 */
extern void bounce_cancellation_registration_init(
  BOUNCE_CANCELLATION_REGISTRATION *registration);

/**
 * @brief Register a completion that is queued when a cancellation is canceled.
 * @param r Initialized BOUNCE_CORE.
 * @param cancellation Initialized cancellation to observe.
 * @param registration Caller-owned registration storage.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @return True when the registration was created or when an already-canceled
 * source queued the completion immediately. False when local setup fails or
 * when the registration is already active.
 * @remarks The completion executes on a parked thread like other bounce
 * completions. The caller must not call `bounce_unregister_canceled()` until
 * this function returns.
 */
extern bool bounce_register_canceled(
  BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation,
  BOUNCE_CANCELLATION_REGISTRATION *registration,
  BOUNCE_COMPLETION completion,
  void *completion_state);

/**
 * @brief Unregister a previously registered cancellation completion.
 * @param registration Initialized cancellation registration.
 * @return True when unregister wins and the completion will not run after this
 * call returns. False when the registration is inactive or when cancellation
 * or shutdown has already won.
 */
extern bool bounce_unregister_canceled(
  BOUNCE_CANCELLATION_REGISTRATION *registration);

/**
 * @brief Deinitialize a cancellation registration.
 * @param registration Cancellation registration storage provided by the caller.
 * @remarks The registration must already be inactive.
 */
extern void bounce_cancellation_registration_deinit(
  BOUNCE_CANCELLATION_REGISTRATION *registration);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

#if (__cplusplus >= 202002L) || defined(__cpp_impl_coroutine)
#define LIBBOUNCE_HAS_COROUTINE_SUPPORT 1
#else
#define LIBBOUNCE_HAS_COROUTINE_SUPPORT 0
#endif

enum class await_status : unsigned int;
struct await_result;
class await_operation;
template<typename T = void> class promise;

template <typename TBOUNCE_CORE> class bounce_base;
template <typename TBOUNCE_CORE, typename TBOUNCE_TIMER> class timer_base;
template <typename TBOUNCE_CORE> class bounce_base_ref;

/**
 * @brief Non-owning bounce reference shared by backend-specific handles.
 * @tparam TBOUNCE_CORE Backend bounce core storage type.
 */
template <typename TBOUNCE_CORE> class bounce_base_ref {
  template<typename> friend class bounce_base;

private:
  TBOUNCE_CORE *bounce_;

protected:
  explicit inline bounce_base_ref(TBOUNCE_CORE *bounce) noexcept
    : bounce_(bounce) {
  }

public:
  /**
   * @brief Check whether this reference currently holds a bounce core.
   * @return True when the reference is bound to a bounce core.
   */
  inline explicit operator bool() const noexcept {
    return bounce_ != nullptr;
  }

  /**
   * @brief Get the underlying bounce core pointer.
   * @return Backend bounce core pointer, or `NULL` when unbound.
   */
  inline TBOUNCE_CORE *get_core() const noexcept {
    return bounce_;
  }

  /**
   * @brief Publish this bounce reference as the current thread/task-local core.
   * @remarks Passing an unbound reference clears the current attachment. This
   * is equivalent to calling `bounce_set_core(get_core())`.
   */
  inline void set_default() const noexcept {
    ::bounce_set_core(bounce_);
  }

  /**
   * @brief Post completion, will continue completion on the parked thread.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @return True when succeeded.
   */
  inline bool post(BOUNCE_COMPLETION completion, void *completion_state) noexcept {
    return (bounce_ != nullptr) ?
             ::bounce_post(bounce_, completion, completion_state) :
             false;
  }

  /**
   * @brief Post a C++ callable completion to run on a parked thread.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param completion Callable completion entry point.
   * @return True when succeeded.
   * @remarks The callable is heap-allocated until it runs or until bounce
   * deinitialization aborts it. Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool post(COMPLETION_TYPE&& completion) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type POST_COMPLETION_TYPE;
    std::unique_ptr<POST_COMPLETION_TYPE> completion_state;

    if (bounce_ == nullptr) {
      return false;
    }

    try {
      completion_state.reset(
        new POST_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!::bounce_post(
          bounce_,
          &bounce_base<TBOUNCE_CORE>::template callable_completion<POST_COMPLETION_TYPE>,
          completion_state.get())) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Park current thread and run continuation repeatedly.
   * @return True when succeeded continuation pumps.
   * @remarks The thread will block inside. Release when `shutdown()` called.
   * Call `set_default()` first when callbacks or coroutine helpers on this
   * thread need `get_current()`.
   */
  inline bool park() noexcept {
    return (bounce_ != nullptr) ?
             ::bounce_park(bounce_, 0u) :
             false;
  }

  /**
   * @brief Park current thread and run continuation repeatedly with inline nesting control.
   * @param max_inline_depth Maximum number of inline nested completion executions.
   * @return True when succeeded continuation pumps.
   * @remarks A zero value disables inline nested execution and preserves the
   * traditional ready-queue-only behavior. Call `set_default()` first when
   * callbacks or coroutine helpers on this thread need `get_current()`.
   */
  inline bool park(unsigned int max_inline_depth) noexcept {
    return (bounce_ != nullptr) ?
             ::bounce_park(bounce_, max_inline_depth) :
             false;
  }

  /**
   * @brief Pump current thread once without waiting for new completion work.
   * @return True when succeeded continuation pumps.
   * @remarks This executes completion work that is already immediately
   * dispatchable and then returns without blocking for future work. Call
   * `set_default()` first when callbacks or coroutine helpers on this thread
   * need `get_current()`.
   */
  inline bool park_once() noexcept {
    return (bounce_ != nullptr) ?
             ::bounce_park_once(bounce_, 0u) :
             false;
  }

  /**
   * @brief Pump current thread once without waiting for new completion work.
   * @param max_inline_depth Maximum number of inline nested completion executions.
   * @return True when succeeded continuation pumps.
   * @remarks A zero value disables inline nested execution and preserves the
   * traditional ready-queue-only behavior. Call `set_default()` first when
   * callbacks or coroutine helpers on this thread need `get_current()`.
   */
  inline bool park_once(unsigned int max_inline_depth) noexcept {
    return (bounce_ != nullptr) ?
             ::bounce_park_once(bounce_, max_inline_depth) :
             false;
  }

  /**
   * @brief Shutdown parking threads.
   * @param wait_for_idle When true, keep parking until already-pending wait
   * operations settle. Defaults to true.
   */
  inline void shutdown(bool wait_for_idle = true) noexcept {
    if (bounce_ != nullptr) {
      ::bounce_shutdown(bounce_, wait_for_idle);
    }
  }
};

template <typename TBOUNCE_CORE> class bounce_base {
private:
  template<typename, typename> friend class cancellation_base;
  template<typename, typename> friend class timer_base;
  template<typename, typename, typename> friend class cancellation_registration_base;

  TBOUNCE_CORE bounce_;
  bounce_base(const bounce_base&) = delete;
  bounce_base(bounce_base&&) = delete;
  bounce_base& operator=(const bounce_base&) = delete;
  bounce_base& operator=(bounce_base&&) = delete;

public:
  /**
   * @brief Get the underlying bounce core storage.
   * @return Backend bounce core pointer.
   */
  inline TBOUNCE_CORE *get_core() noexcept {
    return &bounce_;
  }

  /**
   * @brief Publish this bounce as the current thread/task-local core.
   * @remarks This is equivalent to calling `bounce_set_core(get_core())`.
   */
  inline void set_default() noexcept {
    ::bounce_set_core(&bounce_);
  }

  /**
   * @brief Get the current thread/task-local or fallback bounce core pointer.
   * @return Backend bounce core pointer, or `NULL` when neither a current
   * attachment nor a fallback core is available.
   */
  static inline TBOUNCE_CORE *get_current_core() noexcept {
    return static_cast<TBOUNCE_CORE *>(::bounce_get_core());
  }

  /**
   * @brief Get the current thread/task-local or fallback bounce as a
   * non-owning reference.
   * @return Non-owning reference to the current or fallback bounce core. The
   * returned reference is unbound when neither is available.
   */
  static inline bounce_base_ref<TBOUNCE_CORE> get_current() noexcept {
    return bounce_base_ref<TBOUNCE_CORE>(get_current_core());
  }

  template<typename COMPLETION_TYPE>
  static inline void callable_completion(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    std::unique_ptr<COMPLETION_TYPE> completion(
      static_cast<COMPLETION_TYPE *>(completion_state));

    if constexpr (std::is_invocable_v<COMPLETION_TYPE&, BOUNCE_COMPLETION_RESULT>) {
      (*completion)(result);
    } else {
      static_assert(
        std::is_invocable_v<COMPLETION_TYPE&>,
        "completion must be invocable with no arguments or BOUNCE_COMPLETION_RESULT");
      (*completion)();
    }
  }

protected:
  template<typename INIT_FN>
  explicit inline bounce_base(INIT_FN&& init) noexcept {
    init(&bounce_);
  }

  inline bounce_base() noexcept {
    ::bounce_init(&bounce_);
  }

  inline ~bounce_base() noexcept {
    ::bounce_deinit(&bounce_);
  }

public:
  /**
   * @brief Post completion, will continue completion on the parked thread.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @return True when succeeded.
   */
  inline bool post(BOUNCE_COMPLETION completion, void *completion_state) noexcept {
    return ::bounce_post(&bounce_, completion, completion_state);
  }

  /**
   * @brief Post a C++ callable completion to run on a parked thread.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param completion Callable completion entry point.
   * @return True when succeeded.
   * @remarks The callable is heap-allocated until it runs or until bounce
   * deinitialization aborts it. Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool post(COMPLETION_TYPE&& completion) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type POST_COMPLETION_TYPE;
    std::unique_ptr<POST_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new POST_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!::bounce_post(
          &bounce_,
          &callable_completion<POST_COMPLETION_TYPE>,
          completion_state.get())) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Park current thread and run continuation repeatedly.
   * @return True when succeeded continuation pumps.
   * @remarks The thread will block inside. Release when `shutdown()` called.
   * Call `set_default()` first when callbacks or coroutine helpers on this
   * thread need `get_current()`.
   */
  inline bool park() noexcept {
    return ::bounce_park(&bounce_, 0u);
  }

  /**
   * @brief Park current thread and run continuation repeatedly with inline nesting control.
   * @param max_inline_depth Maximum number of inline nested completion executions.
   * @return True when succeeded continuation pumps.
   * @remarks A zero value disables inline nested execution and preserves the
   * traditional ready-queue-only behavior. Call `set_default()` first when
   * callbacks or coroutine helpers on this thread need `get_current()`.
   */
  inline bool park(unsigned int max_inline_depth) noexcept {
    return ::bounce_park(&bounce_, max_inline_depth);
  }

  /**
   * @brief Pump current thread once without waiting for new completion work.
   * @return True when succeeded continuation pumps.
   * @remarks This executes completion work that is already immediately
   * dispatchable and then returns without blocking for future work. Call
   * `set_default()` first when callbacks or coroutine helpers on this thread
   * need `get_current()`.
   */
  inline bool park_once() noexcept {
    return ::bounce_park_once(&bounce_, 0u);
  }

  /**
   * @brief Pump current thread once without waiting for new completion work.
   * @param max_inline_depth Maximum number of inline nested completion executions.
   * @return True when succeeded continuation pumps.
   * @remarks A zero value disables inline nested execution and preserves the
   * traditional ready-queue-only behavior. Call `set_default()` first when
   * callbacks or coroutine helpers on this thread need `get_current()`.
   */
  inline bool park_once(unsigned int max_inline_depth) noexcept {
    return ::bounce_park_once(&bounce_, max_inline_depth);
  }

  /**
   * @brief Shutdown parking threads.
   * @param wait_for_idle When true, keep parking until already-pending wait
   * operations settle. Defaults to true.
   */
  inline void shutdown(bool wait_for_idle = true) noexcept {
    ::bounce_shutdown(&bounce_, wait_for_idle);
  }
};

template <typename TBOUNCE_CORE, typename TBOUNCE_CANCELLATION> class cancellation_base {
private:
  template<typename, typename, typename> friend class cancellation_registration_base;

  TBOUNCE_CANCELLATION cancellation_;
  cancellation_base(const cancellation_base&) = delete;
  cancellation_base(cancellation_base&&) = delete;
  cancellation_base& operator=(const cancellation_base&) = delete;
  cancellation_base& operator=(cancellation_base&&) = delete;

protected:
  inline cancellation_base() noexcept {
    ::bounce_cancellation_init(&cancellation_);
  }

  inline ~cancellation_base() noexcept {
    ::bounce_cancellation_deinit(&cancellation_);
  }

public:
  /**
   * @brief Get the underlying cancellation storage.
   * @return Backend cancellation storage pointer.
   */
  inline TBOUNCE_CANCELLATION *get_cancellation() noexcept {
    return &cancellation_;
  }

  /**
   * @brief Cancel the source through a bounce handle.
   * @param bounce_handle Bounce handle used to publish cancellation completions.
   */
  template<typename TBOUNCE_HANDLE>
  inline void cancel(TBOUNCE_HANDLE &bounce_handle) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    if (core != nullptr) {
      ::bounce_cancel(core, &cancellation_);
    }
  }
};

template<
  typename TBOUNCE_CORE,
  typename TBOUNCE_CANCELLATION,
  typename TBOUNCE_CANCELLATION_REGISTRATION>
class cancellation_registration_base {
private:
  TBOUNCE_CANCELLATION_REGISTRATION registration_;
  cancellation_registration_base(const cancellation_registration_base&) = delete;
  cancellation_registration_base(cancellation_registration_base&&) = delete;
  cancellation_registration_base& operator=(const cancellation_registration_base&) = delete;
  cancellation_registration_base& operator=(cancellation_registration_base&&) = delete;

protected:
  template<typename COMPLETION_TYPE>
  static inline void callable_completion(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    bounce_base<TBOUNCE_CORE>::template callable_completion<COMPLETION_TYPE>(
      result,
      completion_state);
  }

  inline cancellation_registration_base() noexcept {
    ::bounce_cancellation_registration_init(&registration_);
  }

  inline ~cancellation_registration_base() noexcept {
    ::bounce_cancellation_registration_deinit(&registration_);
  }

public:
  /**
   * @brief Get the underlying cancellation registration storage.
   * @return Backend cancellation registration storage pointer.
   */
  inline TBOUNCE_CANCELLATION_REGISTRATION *get_registration() noexcept {
    return &registration_;
  }

  /**
   * @brief Register a completion that runs when a cancellation is canceled.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param cancellation Cancellation to observe.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @return True when the registration was created.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool register_canceled(
    TBOUNCE_HANDLE &bounce_handle,
    cancellation_base<TBOUNCE_CORE, TBOUNCE_CANCELLATION> &cancellation,
    BOUNCE_COMPLETION completion,
    void *completion_state) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_register_canceled(
               core,
               cancellation.get_cancellation(),
               &registration_,
               completion,
               completion_state) :
             false;
  }

  /**
   * @brief Register a C++ callable that runs when a cancellation is canceled.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param cancellation Cancellation to observe.
   * @param completion Callable completion entry point.
   * @return True when local setup and registration succeeded.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool register_canceled(
    TBOUNCE_HANDLE &bounce_handle,
    cancellation_base<TBOUNCE_CORE, TBOUNCE_CANCELLATION> &cancellation,
    COMPLETION_TYPE&& completion) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type REGISTRATION_COMPLETION_TYPE;
    std::unique_ptr<REGISTRATION_COMPLETION_TYPE> completion_state;
    TBOUNCE_CORE *core = bounce_handle.get_core();

    if (core == nullptr) {
      return false;
    }

    try {
      completion_state.reset(
        new REGISTRATION_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!::bounce_register_canceled(
          core,
          cancellation.get_cancellation(),
          &registration_,
          &callable_completion<REGISTRATION_COMPLETION_TYPE>,
          completion_state.get())) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Unregister a previously registered cancellation completion.
   * @return True when unregister wins.
   */
  inline bool unregister() noexcept {
    return ::bounce_unregister_canceled(&registration_);
  }
};

}
#endif

//////////////////////////////////////////////////////////////////////////////////

#include "timer.h"

#if defined(BOUNCE_GENERIC)
#include "generic.h"
#endif

#if defined(BOUNCE_POSIX)
#include "posix.h"
#endif

#if defined(BOUNCE_POSIX_GLIB)
#include "posix_glib.h"
#endif

#if defined(BOUNCE_FREERTOS) || defined(FREERTOS) || defined(ESP_PLATFORM)
#include "freertos.h"
#endif

#if defined(_WIN32)
#include "win32.h"
#endif

//////////////////////////////////////////////////////////////////////////////////

#endif
