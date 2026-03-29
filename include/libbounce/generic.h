/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_GENERIC_H
#define _LIBBOUNCE_GENERIC_H

#if defined(BOUNCE_GENERIC)

#include "bounce.h"
#include "utils.h"

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Generic backend overview.
 * @remarks This backend does not depend on platform-specific wait objects.
 * It allows only a single parked thread and keeps idle waiting as a busy spin.
 * Producer paths are multi-thread safe through backend-local spin locks.
 */

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
 * @remarks The generic backend intentionally supports only one parker.
 */
#if !defined(BOUNCE_MAX_PARKERS)
#define BOUNCE_MAX_PARKERS 1
#elif (BOUNCE_MAX_PARKERS != 1)
#error "BOUNCE_MAX_PARKERS must be 1 for the generic backend"
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define BOUNCE_GENERIC_STATIC_ASSERT(expr, message) static_assert((expr), message)
#else
#define BOUNCE_GENERIC_STATIC_ASSERT(expr, message) _Static_assert((expr), message)
#endif

/**
 * @brief Backend-local spin lock.
 * @remarks The lock uses a busy loop and should only protect short critical
 * sections.
 */
typedef struct __BOUNCE_GENERIC_SPINLOCK {
  volatile unsigned int state;
} __BOUNCE_GENERIC_SPINLOCK;

BOUNCE_GENERIC_STATIC_ASSERT(
  __GCC_ATOMIC_INT_LOCK_FREE == 2,
  "generic backend requires always lock-free int-width atomics");
BOUNCE_GENERIC_STATIC_ASSERT(
  sizeof(unsigned int) == sizeof(int),
  "generic backend requires unsigned int to match int atomic width");
BOUNCE_GENERIC_STATIC_ASSERT(
  __GCC_ATOMIC_INT_LOCK_FREE == 2,
  "generic backend requires lock-free int atomics");
BOUNCE_GENERIC_STATIC_ASSERT(
  __GCC_ATOMIC_POINTER_LOCK_FREE == 2,
  "generic backend requires lock-free bounce core pointer atomics");

/**
 * @brief Completion item stored in static bounce pools.
 */
typedef struct __BOUNCE_COMPLETION_ITEM {
  BOUNCE_NODE_ITEM ready_or_free_link;
  BOUNCE_LIST_ITEM cancellation_link;
  volatile int state;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  BOUNCE_COMPLETION_RESULT completion_result;
  BOUNCE_CORE *bounce;
  BOUNCE_CANCELLATION *cancellation;
  BOUNCE_CANCELLATION_REGISTRATION *registration_owner;
} __BOUNCE_COMPLETION_ITEM;

/**
 * @brief One-shot cancellation.
 * @remarks This source is caller-owned. Once cancelled, it stays cancelled for
 * the lifetime of the object.
 */
struct BOUNCE_CANCELLATION {
  __BOUNCE_GENERIC_SPINLOCK lock;
  volatile int canceled;
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
 * @brief Generic timeout callback invoked by the external timeout sink.
 * @param callback_state User-provided callback state.
 */
typedef void (*BOUNCE_GENERIC_TIMEOUT_CALLBACK)(void *callback_state);

/**
 * @brief Create a dormant one-shot timeout handle for the generic backend.
 * @param timeout_msec Timeout duration in milliseconds.
 * @param callback Callback invoked when the timeout elapses.
 * @param callback_state User-provided callback state.
 * @param timeout_handle Receives the opaque timeout handle on success.
 * @return True when the timeout handle was created successfully.
 * @remarks The sink must not invoke `callback` until the created handle is
 * armed through `start_timeout`. The returned handle is later passed to
 * `start_timeout` and `cancel_timeout`.
 */
typedef bool (*BOUNCE_GENERIC_TIMEOUT_CREATE)(
  uint32_t timeout_msec,
  BOUNCE_GENERIC_TIMEOUT_CALLBACK callback,
  void *callback_state,
  void **timeout_handle);

/**
 * @brief Arm a timeout handle created by the generic timeout sink.
 * @param timeout_handle Opaque timeout handle returned by `create_timeout`.
 * @remarks The sink may invoke the timeout callback asynchronously after this
 * function returns.
 */
typedef void (*BOUNCE_GENERIC_TIMEOUT_START)(void *timeout_handle);

/**
 * @brief Cancel and release a timeout handle created by the generic timeout sink.
 * @param timeout_handle Opaque timeout handle returned by `create_timeout`.
 * @remarks After this function returns, the timeout callback must not run
 * again, and the sink must release any resources associated with the handle.
 * Calling this after the callback already fired is allowed.
 */
typedef void (*BOUNCE_GENERIC_TIMEOUT_CANCEL)(void *timeout_handle);

BOUNCE_GENERIC_STATIC_ASSERT(
  sizeof(BOUNCE_GENERIC_TIMEOUT_CREATE) == sizeof(void *),
  "generic backend requires timeout create function pointers to match pointer width");
BOUNCE_GENERIC_STATIC_ASSERT(
  sizeof(BOUNCE_GENERIC_TIMEOUT_START) == sizeof(void *),
  "generic backend requires timeout start function pointers to match pointer width");
BOUNCE_GENERIC_STATIC_ASSERT(
  sizeof(BOUNCE_GENERIC_TIMEOUT_CANCEL) == sizeof(void *),
  "generic backend requires timeout cancel function pointers to match pointer width");
BOUNCE_GENERIC_STATIC_ASSERT(
  __GCC_ATOMIC_POINTER_LOCK_FREE == 2,
  "generic backend requires always lock-free pointer-width atomics");

/**
 * @brief Install the external timeout sink used by the generic timer backend.
 * @param create_timeout Creates a dormant one-shot timeout handle.
 * @param start_timeout Arms a created timeout handle.
 * @param cancel_timeout Cancels and releases a timeout handle.
 * @remarks The generic backend does not provide its own time source. Timer
 * waits return false until a sink is installed. Passing `NULL` for all
 * arguments clears the current sink.
 */
extern void bounce_generic_set_timeout_sink(
  BOUNCE_GENERIC_TIMEOUT_CREATE create_timeout,
  BOUNCE_GENERIC_TIMEOUT_START start_timeout,
  BOUNCE_GENERIC_TIMEOUT_CANCEL cancel_timeout);

/**
 * @brief Generic timer helper driven by an external timeout sink.
 * @remarks Only one timeout may be active at a time. Timeout readiness is
 * supplied by the installed timeout sink, and completion publication uses only
 * the public bounce core API.
 */
struct BOUNCE_TIMER {
  __BOUNCE_GENERIC_SPINLOCK lock;
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  void *timeout_handle;
  volatile int deinitialized;
  bool active;
  bool registration_active;
};

/**
 * @brief Bounce core storage.
 * @remarks Producers enqueue ready completion items into the shared ready
 * queue, and the single parked thread drains that queue while busy-spinning.
 * Timer expiry is injected through the external timeout sink and enters the
 * core through the public completion APIs.
 */
struct BOUNCE_CORE {
  __BOUNCE_GENERIC_SPINLOCK lock;
  volatile int shutting_down;
  volatile int parker_active;
  BOUNCE_QUEUE ready_queue;
  BOUNCE_STACK free_items;
  BOUNCE_DYNAMIC_BLOCK_LIST dynamic_completion_blocks;
  __BOUNCE_COMPLETION_ITEM
    static_completion_items[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
};

#ifdef __cplusplus
}
#endif

#undef BOUNCE_GENERIC_STATIC_ASSERT

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

class bounce_ref : public bounce_ref_base<BOUNCE_CORE> {
private:
  friend class bounce;

  explicit inline bounce_ref(BOUNCE_CORE *core) noexcept
    : bounce_ref_base(core) {
  }
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
   * @brief Get a non-owning bounce reference from the current attachment or fallback core.
   * @return Bounce reference when present.
   */
  static inline std::optional<bounce_ref> current() noexcept {
    BOUNCE_CORE *core = bounce::get_current_core();

    return (core != nullptr) ?
             std::optional<bounce_ref>(bounce_ref(core)) :
             std::nullopt;
  }
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

//////////////////////////////////////////////////////////////////////////////////

#endif

#endif
