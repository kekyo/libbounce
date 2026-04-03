/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_POSIX_GLIB_H
#define _LIBBOUNCE_POSIX_GLIB_H

#if defined(BOUNCE_POSIX_GLIB)

#include <glib.h>
#include <pthread.h>
#include <time.h>

#include "bounce.h"
#include "utils.h"

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief GLib POSIX backend overview.
 * @remarks This backend is tied to a single `GMainContext`-driving parker.
 * Backend-local producers enqueue ready completion items, and the parker drives
 * a GLib main context that dispatches the bounce ready source. fd readiness is
 * represented through `GSource`, not through raw waiter threads.
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
 * @brief Maximum number of parker wakeups emitted while a ready burst is building.
 * @remarks GLib uses `g_main_context_wakeup()` rather than multi-parker wake
 * fan-out, but this limit still bounds repeated wake requests while a burst is
 * building.
 */
#if !defined(BOUNCE_MAX_READY_WAKE_FANOUT)
#define BOUNCE_MAX_READY_WAKE_FANOUT 2
#endif

/**
 * @brief Maximum number of GLib fd watches owned by one bounce instance.
 */
#if !defined(BOUNCE_MAX_POSIX_GLIB_WATCHES)
#define BOUNCE_MAX_POSIX_GLIB_WATCHES BOUNCE_MAX_STATIC_COMPLETION_ITEMS
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

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
  int fd;
  GIOCondition condition;
  BOUNCE_CANCELLATION *cancellation;
  BOUNCE_CANCELLATION_REGISTRATION *registration_owner;
  GSource *source;
} __BOUNCE_COMPLETION_ITEM;

/**
 * @brief One-shot cancellation.
 * @remarks This source is caller-owned. Once cancelled, it stays cancelled for
 * the lifetime of the object.
 */
struct BOUNCE_CANCELLATION {
  pthread_mutex_t lock;
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
 * @brief GLib timer helper backed by a one-shot timeout `GSource`.
 * @remarks Only one timeout may be active at a time. The object stays
 * caller-owned and can be deinitialized from its completion callback.
 */
struct BOUNCE_TIMER {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  GSource *source;
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  volatile int deinitialized;
  bool active;
  bool settling;
  bool abort_requested;
  bool registration_active;
};

/**
 * @brief Bounce core storage.
 * @remarks The GLib backend owns a custom ready `GSource` attached to the main
 * context. `bounce_park()` drives that context and only one parker is valid at
 * a time.
 */
struct BOUNCE_CORE {
  pthread_mutex_t lock;
  volatile int shutting_down;
  unsigned int active_watch_count;
  GMainContext *main_context;
  GSource *ready_source;
  BOUNCE_QUEUE ready_queue;
  BOUNCE_STACK free_items;
  BOUNCE_DYNAMIC_BLOCK_LIST dynamic_completion_blocks;
  __BOUNCE_COMPLETION_ITEM
    static_completion_items[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
};

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize the GLib backend with an explicit `GMainContext`.
 * @param r BOUNCE_CORE structure space provided by the caller.
 * @param main_context GLib main context to drive from `bounce_park()`, or
 * `NULL` to create a private context like `bounce_init()`.
 * @remarks When a non-NULL context is provided, libbounce keeps a reference to
 * it for the lifetime of the core and releases that reference during
 * `bounce_deinit()`.
 */
extern void bounce_init_with_main_context(
  BOUNCE_CORE *r,
  GMainContext *main_context);

/**
 * @brief Await GLib-integrated file-descriptor readiness through `GSource`.
 * @param r Initialized BOUNCE_CORE.
 * @param fd File descriptor watched by GLib main-context polling.
 * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks This backend-local extension maps fd readiness onto a one-shot
 * `GSource`. If registration fails, completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`. The caller must re-register if it needs a new
 * wait after completion.
 */
extern void bounce_await_posix_glib_fd(
  BOUNCE_CORE *r,
  int fd,
  GIOCondition condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

class bounce_ref : public bounce_ref_base<BOUNCE_CORE> {
private:
  friend class bounce;

  explicit inline bounce_ref(BOUNCE_CORE *core) noexcept
    : bounce_ref_base(core) {
  }

public:
  /**
   * @brief Await GLib-integrated file-descriptor readiness and continue on a
   * parked thread.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    GIOCondition condition,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_glib_fd(
      this->get_core(),
      fd,
      condition,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await GLib-integrated file-descriptor readiness with a C++
   * callable completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    int fd,
    GIOCondition condition,
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

    ::bounce_await_posix_glib_fd(
      this->get_core(),
      fd,
      condition,
      &bounce_base<BOUNCE_CORE>::template callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await GLib-integrated file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    GIOCondition condition,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
};

class bounce : public bounce_base<BOUNCE_CORE> {
public:
  /**
   * @brief Initialize the bounce.
   */
  inline bounce(): bounce_base() {
  }

  /**
   * @brief Initialize the bounce on a caller-provided GLib main context.
   * @param main_context GLib main context to drive from `park()`, or `NULL` to
   * create a private context like the default constructor.
   */
  explicit inline bounce(GMainContext *main_context) noexcept
    : bounce_base(
        [main_context](BOUNCE_CORE *core) noexcept {
          ::bounce_init_with_main_context(core, main_context);
        }) {
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

  /**
   * @brief Await GLib-integrated file-descriptor readiness and continue on a
   * parked thread.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    GIOCondition condition,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_glib_fd(
      this->get_core(),
      fd,
      condition,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await GLib-integrated file-descriptor readiness with a C++
   * callable completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    int fd,
    GIOCondition condition,
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

    ::bounce_await_posix_glib_fd(
      this->get_core(),
      fd,
      condition,
      &callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await GLib-integrated file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local `GSource`.
   * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    GIOCondition condition,
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

//////////////////////////////////////////////////////////////////////////////////

#endif

#endif
