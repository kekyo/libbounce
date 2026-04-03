/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_WIN32_H
#define _LIBBOUNCE_WIN32_H

#if defined(_WIN32)

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

#include "bounce.h"
#include "utils.h"

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Maximum number of static completion items owned by a bounce instance.
 */
#if !defined(BOUNCE_MAX_STATIC_COMPLETION_ITEMS)
#define BOUNCE_MAX_STATIC_COMPLETION_ITEMS 64
#endif

/**
 * @brief Number of completion items added per dynamic overflow block.
 */
#if !defined(BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP)
#define BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP BOUNCE_MAX_STATIC_COMPLETION_ITEMS
#endif

/**
 * @brief Maximum number of concurrent parked threads.
 */
#if !defined(BOUNCE_MAX_PARKERS)
#define BOUNCE_MAX_PARKERS 8
#endif

/**
 * @brief Maximum number of parker wakeups emitted while a ready burst is building.
 * @remarks This keeps wake fan-out bounded while still allowing parallel drain
 * under large bursts.
 */
#if !defined(BOUNCE_MAX_READY_WAKE_FANOUT)
#define BOUNCE_MAX_READY_WAKE_FANOUT (BOUNCE_MAX_PARKERS * 2)
#endif

/**
 * @brief Maximum number of backend-local Win32 waiter shards.
 */
#if !defined(BOUNCE_MAX_WIN32_WAITERS)
#define BOUNCE_MAX_WIN32_WAITERS 2
#endif

/**
 * @brief Maximum number of external waits assigned to one Win32 waiter shard.
 */
#if !defined(BOUNCE_MAX_WIN32_WAITS_PER_WAITER)
#define BOUNCE_MAX_WIN32_WAITS_PER_WAITER (MAXIMUM_WAIT_OBJECTS - 2)
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __BOUNCE_WIN32_WAITER __BOUNCE_WIN32_WAITER;

/**
 * @brief Completion item stored in static bounce pools.
 */
typedef struct __BOUNCE_COMPLETION_ITEM {
  BOUNCE_NODE_ITEM ready_or_free_link;
  BOUNCE_LIST_ITEM cancellation_link;
  volatile LONG state;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  BOUNCE_COMPLETION_RESULT completion_result;
  BOUNCE_CORE *bounce;
  HANDLE wait_handle;
  BOUNCE_CANCELLATION *cancellation;
  BOUNCE_CANCELLATION_REGISTRATION *registration_owner;
  __BOUNCE_WIN32_WAITER *waiter;
  DWORD waiter_slot;
} __BOUNCE_COMPLETION_ITEM;

/**
 * @brief Backend-local Win32 waiter shard state.
 */
struct __BOUNCE_WIN32_WAITER {
  CRITICAL_SECTION lock;
  HANDLE refresh_event;
  HANDLE thread_handle;
  BOUNCE_CORE *bounce;
  DWORD active_count;
  __BOUNCE_COMPLETION_ITEM *slots[BOUNCE_MAX_WIN32_WAITS_PER_WAITER];
};

/**
 * @brief Bounce core storage.
 */
struct BOUNCE_CORE {
  CRITICAL_SECTION lock;
  HANDLE shutdown_event;
  HANDLE wake_semaphore;
  volatile LONG shutting_down;
  volatile LONG shutdown_wait_for_idle;
  BOUNCE_QUEUE ready_queue;
  BOUNCE_STACK free_items;
  BOUNCE_DYNAMIC_BLOCK_LIST dynamic_completion_blocks;
  __BOUNCE_COMPLETION_ITEM
    static_completion_items[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
  __BOUNCE_WIN32_WAITER waiters[BOUNCE_MAX_WIN32_WAITERS];
};

/**
 * @brief One-shot cancellation.
 * @remarks This source is caller-owned. Once cancelled, it stays cancelled for
 * the lifetime of the object.
 */
struct BOUNCE_CANCELLATION {
  CRITICAL_SECTION lock;
  volatile LONG canceled;
  BOUNCE_LIST listeners;
};

/**
 * @brief Caller-owned cancellation callback registration.
 * @remarks Active registrations consume one completion item until they are
 * unregistered or settled.
 */
struct BOUNCE_CANCELLATION_REGISTRATION {
  __BOUNCE_COMPLETION_ITEM *item;
};

/**
 * @brief Win32 timer helper backed by a waitable timer handle.
 * @remarks Only one timeout may be active at a time. The object stays
 * caller-owned and can be deinitialized from its completion callback.
 */
struct BOUNCE_TIMER {
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE settled_cv;
  HANDLE timer_handle;
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  volatile LONG deinitialized;
  volatile LONG active;
  volatile LONG abort_requested;
};

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Await raising kernel object, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param handle Win32 kernel object handle (HANDLE).
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks When the wait cannot be registered because completion-item or
 * backend resources are exhausted, completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`.
 */
extern void bounce_await_win32_handle(BOUNCE_CORE *r,
  HANDLE handle,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

class bounce_ref : public bounce_base_ref<BOUNCE_CORE> {
private:
  friend class bounce;

  explicit inline bounce_ref(BOUNCE_CORE *core) noexcept
    : bounce_base_ref(core) {
  }

public:
  /**
   * @brief Build a backend-specific bounce reference from a common
   * non-owning bounce reference.
   * @param reference Common bounce reference.
   */
  explicit inline bounce_ref(const bounce_base_ref<BOUNCE_CORE>& reference) noexcept
    : bounce_base_ref(reference.get_core()) {
  }

  /**
   * @brief Await a Win32 handle and continue completion on a parked thread.
   * @param handle Win32 kernel object handle.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    HANDLE handle,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_win32_handle(
      this->get_core(),
      handle,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a Win32 handle with a C++ callable completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param handle Win32 kernel object handle.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    HANDLE handle,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type AWAIT_COMPLETION_TYPE;
    std::unique_ptr<AWAIT_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new AWAIT_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    ::bounce_await_win32_handle(
      this->get_core(),
      handle,
      &bounce_base<BOUNCE_CORE>::template callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await a Win32 handle inside a coroutine.
   * @param handle Win32 kernel object handle.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    HANDLE handle,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
};

class bounce : public bounce_base<BOUNCE_CORE> {
public:
  /**
   * @brief Initialize the bounce.
   */
  inline bounce() noexcept: bounce_base() {
  }

  /**
   * @brief Deinitialize the bounce.
   */
  ~bounce() = default;

  /**
   * @brief Get the current thread/task-local or fallback bounce as a
   * backend-specific non-owning reference.
   * @return Backend-specific bounce reference. The returned reference is
   * unbound when neither a current attachment nor a fallback core is
   * available.
   */
  static inline bounce_ref get_current() noexcept {
    return bounce_ref(bounce_base<BOUNCE_CORE>::get_current());
  }

  /**
   * @brief Get a non-owning backend-specific bounce reference from the current
   * attachment or fallback core.
   * @return Bounce reference when present.
   */
  static inline std::optional<bounce_ref> current() noexcept {
    bounce_ref current = bounce::get_current();

    return current ?
             std::optional<bounce_ref>(current) :
             std::nullopt;
  }

  /**
   * @brief Await a Win32 handle and continue completion on a parked thread.
   * @param handle Win32 kernel object handle.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    HANDLE handle,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_win32_handle(
      this->get_core(),
      handle,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a Win32 handle with a C++ callable completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param handle Win32 kernel object handle.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    HANDLE handle,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type AWAIT_COMPLETION_TYPE;
    std::unique_ptr<AWAIT_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new AWAIT_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    ::bounce_await_win32_handle(
      this->get_core(),
      handle,
      &callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await a Win32 handle inside a coroutine.
   * @param handle Win32 kernel object handle.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    HANDLE handle,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
};

/**
 * @brief Caller-owned backend-local timer storage for the C++ helper API.
 */
class timer : public timer_base<BOUNCE_CORE, BOUNCE_TIMER> {
public:
  /**
   * @brief Initialize the timer.
   */
  inline timer() noexcept: timer_base() {
  }

  /**
   * @brief Deinitialize the timer.
   */
  ~timer() = default;
};

class cancellation : public cancellation_base<BOUNCE_CORE, BOUNCE_CANCELLATION> {
public:
  /**
   * @brief Initialize the cancellation.
   */
  inline cancellation() noexcept: cancellation_base() {
  }

  /**
   * @brief Deinitialize the cancellation.
   */
  ~cancellation() = default;
};

class cancellation_registration :
  public cancellation_registration_base<
    BOUNCE_CORE,
    BOUNCE_CANCELLATION,
    BOUNCE_CANCELLATION_REGISTRATION> {
public:
  /**
   * @brief Initialize the cancellation registration.
   */
  inline cancellation_registration() noexcept: cancellation_registration_base() {
  }

  /**
   * @brief Deinitialize the cancellation registration.
   */
  ~cancellation_registration() = default;
};

}
#endif

#endif

#endif
