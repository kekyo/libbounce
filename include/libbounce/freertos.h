/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_FREERTOS_H
#define _LIBBOUNCE_FREERTOS_H

#if defined(BOUNCE_FREERTOS) || defined(FREERTOS) || defined(ESP_PLATFORM)

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#else
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"
#endif

#include "bounce.h"
#include "utils.h"

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief FreeRTOS backend overview.
 * @remarks Backend-local producers publish ready completion items into the
 * shared queue, and parked tasks only drain that queue and execute
 * completions. The baseline backend keeps the first completion-item tranche
 * inline and expands with dynamic overflow blocks on demand without requiring
 * internal waiter tasks. Optional ESP-IDF fd waiting is compiled under an
 * explicit feature gate and remains outside the `post()` fast path.
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
 * @brief Number of free completion items reserved for ISR producers.
 * @remarks Task-context producers do not consume below this watermark and
 * instead trigger dynamic growth when available.
 */
#if !defined(BOUNCE_FREERTOS_ISR_RESERVED_COMPLETION_ITEMS)
#define BOUNCE_FREERTOS_ISR_RESERVED_COMPLETION_ITEMS BOUNCE_MAX_READY_WAKE_FANOUT
#endif

/**
 * @brief Remaining free-item watermark that proactively triggers task-context growth.
 */
#if !defined(BOUNCE_FREERTOS_GROW_LOW_WATERMARK)
#define BOUNCE_FREERTOS_GROW_LOW_WATERMARK BOUNCE_FREERTOS_ISR_RESERVED_COMPLETION_ITEMS
#endif

/**
 * @brief Maximum number of concurrent parked tasks.
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
 * @brief Default stack depth for the FreeRTOS timer worker task.
 */
#if !defined(BOUNCE_FREERTOS_TIMER_TASK_STACK_DEPTH)
#define BOUNCE_FREERTOS_TIMER_TASK_STACK_DEPTH ( configMINIMAL_STACK_SIZE * 4 )
#endif

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)

/**
 * @brief Default stack depth for the optional ESP-IDF fd waiter task.
 */
#if !defined(BOUNCE_FREERTOS_FD_WAITER_STACK_DEPTH)
#define BOUNCE_FREERTOS_FD_WAITER_STACK_DEPTH ( configMINIMAL_STACK_SIZE * 4 )
#endif

/**
 * @brief Maximum number of eventfds reserved for libbounce's ESP-IDF backend registration.
 * @remarks The eventfd VFS registration is process-global in ESP-IDF. libbounce
 * keeps one control eventfd per active bounce instance that uses fd awaiting.
 */
#if !defined(BOUNCE_FREERTOS_EVENTFD_MAX_FDS)
#define BOUNCE_FREERTOS_EVENTFD_MAX_FDS BOUNCE_MAX_PARKERS
#endif

/**
 * @brief Read readiness for `bounce_await_freertos_fd()`.
 */
#define BOUNCE_FREERTOS_FD_EVENT_READ 0x0001u

/**
 * @brief Write readiness for `bounce_await_freertos_fd()`.
 */
#define BOUNCE_FREERTOS_FD_EVENT_WRITE 0x0002u

/**
 * @brief Exceptional readiness for `bounce_await_freertos_fd()`.
 */
#define BOUNCE_FREERTOS_FD_EVENT_EXCEPT 0x0004u

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
  BOUNCE_LIST_ITEM backend_link;
  BOUNCE_LIST_ITEM cancellation_link;
  volatile int state;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  BOUNCE_COMPLETION_RESULT completion_result;
  BOUNCE_CORE *bounce;
  BOUNCE_CANCELLATION *cancellation;
  BOUNCE_CANCELLATION_REGISTRATION *registration_owner;
  struct BOUNCE_FREERTOS_CONDITION *condition;
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  struct __BOUNCE_FREERTOS_FD_BACKEND *fd_backend;
  int fd;
  unsigned int events;
#endif
} __BOUNCE_COMPLETION_ITEM;

/**
 * @brief One-shot cancellation.
 * @remarks This source is caller-owned. Once cancelled, it stays cancelled for
 * the lifetime of the object.
 */
struct BOUNCE_CANCELLATION {
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
 * @brief Backend-local one-shot condition object for FreeRTOS awaiting.
 * @remarks One condition raise schedules all currently registered waiters. This
 * backend-local producer is the baseline await mechanism used by tests and by
 * simple backend glue.
 */
typedef struct BOUNCE_FREERTOS_CONDITION {
  BOUNCE_LIST waiters;
} BOUNCE_FREERTOS_CONDITION;

/**
 * @brief FreeRTOS timer helper backed by a backend-local worker task.
 * @remarks Only one timeout may be active at a time. The object stays
 * caller-owned and can be deinitialized from its completion callback.
 */
struct BOUNCE_TIMER {
  BOUNCE_FREERTOS_CONDITION fired_condition;
  StaticSemaphore_t settled_semaphore_buffer;
  SemaphoreHandle_t settled_semaphore;
  StaticTimer_t timer_buffer;
  TimerHandle_t timer_handle;
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  volatile int deinitialized;
  volatile int active;
  volatile int settling;
  volatile int abort_requested;
};

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)

/**
 * @brief Optional ESP-IDF specific fd waiter state.
 * @remarks This backend-local state remains outside the `post()` fast path.
 */
typedef struct __BOUNCE_FREERTOS_FD_BACKEND {
  SemaphoreHandle_t finished_semaphore;
  StaticSemaphore_t finished_semaphore_buffer;
  TaskHandle_t waiter_task;
  StaticTask_t waiter_task_tcb;
  StackType_t waiter_task_stack[BOUNCE_FREERTOS_FD_WAITER_STACK_DEPTH];
  BOUNCE_CORE *bounce;
  int control_event_fd;
} __BOUNCE_FREERTOS_FD_BACKEND;

#endif

/**
 * @brief Bounce core storage.
 * @remarks Parked tasks only drain ready work. Optional backend-specific
 * producers such as ESP-IDF fd waiting remain isolated from the `post()` hot
 * path. The first `BOUNCE_MAX_STATIC_COMPLETION_ITEMS` items stay inline, and
 * overflow expands through append-only dynamic blocks on demand. Task-context
 * producers preserve `BOUNCE_FREERTOS_ISR_RESERVED_COMPLETION_ITEMS` free items
 * for ISR producers whenever possible.
 */
struct BOUNCE_CORE {
  volatile int shutting_down;
  BOUNCE_QUEUE ready_queue;
  BOUNCE_STACK free_items;
  BOUNCE_DYNAMIC_BLOCK_LIST dynamic_item_blocks;
  unsigned int free_item_count;
  volatile unsigned int grow_in_progress;
  TaskHandle_t parkers[BOUNCE_MAX_PARKERS];
  __BOUNCE_COMPLETION_ITEM
    static_completion_items[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  __BOUNCE_FREERTOS_FD_BACKEND fd_backend;
#endif
};

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a backend-local FreeRTOS condition object.
 * @param condition Condition storage provided by the caller.
 */
extern void bounce_freertos_condition_init(BOUNCE_FREERTOS_CONDITION *condition);

/**
 * @brief Await a backend-local FreeRTOS condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks If capacity is exhausted, completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`.
 */
extern void bounce_await_freertos_condition(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Raise a backend-local FreeRTOS condition from task context.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 */
extern void bounce_freertos_condition_raise(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition);

/**
 * @brief Raise a backend-local FreeRTOS condition from ISR context.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 * @remarks This path schedules ready work without changing the parker contract.
 */
extern void bounce_freertos_condition_raise_from_isr(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition);

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)

/**
 * @brief Await ESP-IDF file-descriptor readiness through a backend-local waiter.
 * @param r Initialized BOUNCE_CORE.
 * @param fd File descriptor watched by the backend-local waiter task.
 * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks This optional extension is backend-local to ESP-IDF-style targets,
 * uses one-shot readiness semantics, and remains outside the `post()` fast
 * path. If registration fails, completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`.
 */
extern void bounce_await_freertos_fd(
  BOUNCE_CORE *r,
  int fd,
  unsigned int events,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

#endif

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

/**
 * @brief Caller-owned backend-local FreeRTOS condition storage for the C++
 * helper API.
 */
class condition {
private:
  BOUNCE_FREERTOS_CONDITION condition_;
  condition(const condition&) = delete;
  condition(condition&&) = delete;
  condition& operator=(const condition&) = delete;
  condition& operator=(condition&&) = delete;

public:
  /**
   * @brief Initialize the backend-local condition storage.
   * @remarks The condition must outlive any pending awaits registered against
   * it.
   */
  inline condition() noexcept {
    ::bounce_freertos_condition_init(&condition_);
  }

  /**
   * @brief Deinitialize the backend-local condition storage.
   */
  ~condition() = default;

  /**
   * @brief Get the underlying condition storage.
   * @return Backend condition storage pointer.
   */
  inline BOUNCE_FREERTOS_CONDITION *get_condition() noexcept {
    return &condition_;
  }
};

class bounce_ref : public bounce_ref_base<BOUNCE_CORE> {
private:
  friend class bounce;

  explicit inline bounce_ref(BOUNCE_CORE *core) noexcept
    : bounce_ref_base(core) {
  }

public:
  /**
   * @brief Await a backend-local FreeRTOS condition and continue on a parked
   * task.
   * @param condition_instance Initialized backend-local condition.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    condition &condition_instance,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_freertos_condition(
      this->get_core(),
      condition_instance.get_condition(),
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a backend-local FreeRTOS condition with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param condition_instance Initialized backend-local condition.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    condition &condition_instance,
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

    ::bounce_await_freertos_condition(
      this->get_core(),
      condition_instance.get_condition(),
      &bounce_base<BOUNCE_CORE>::template callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await a backend-local FreeRTOS condition inside a coroutine.
   * @param condition_instance Initialized backend-local condition.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    condition &condition_instance,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

  /**
   * @brief Raise a backend-local FreeRTOS condition from task context.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise(condition &condition_instance) noexcept {
    ::bounce_freertos_condition_raise(
      this->get_core(),
      condition_instance.get_condition());
  }

  /**
   * @brief Raise a backend-local FreeRTOS condition from ISR context.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise_from_isr(condition &condition_instance) noexcept {
    ::bounce_freertos_condition_raise_from_isr(
      this->get_core(),
      condition_instance.get_condition());
  }

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  /**
   * @brief Await ESP-IDF file-descriptor readiness and continue on a parked
   * task.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    unsigned int events,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_freertos_fd(
      this->get_core(),
      fd,
      events,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await ESP-IDF file-descriptor readiness with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
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
    unsigned int events,
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

    ::bounce_await_freertos_fd(
      this->get_core(),
      fd,
      events,
      &bounce_base<BOUNCE_CORE>::template callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await ESP-IDF file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    unsigned int events,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
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
   * @brief Await a backend-local FreeRTOS condition and continue on a parked
   * task.
   * @param condition_instance Initialized backend-local condition.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    condition &condition_instance,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_freertos_condition(
      this->get_core(),
      condition_instance.get_condition(),
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a backend-local FreeRTOS condition with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param condition_instance Initialized backend-local condition.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local callable setup succeeded.
   * @remarks Backend-local await registration failures are still reported
   * asynchronously through the completion with `BOUNCE_COMPLETION_ABORTED`.
   * Exceptions must not escape the callable.
   */
  template<typename COMPLETION_TYPE>
  inline bool wait(
    condition &condition_instance,
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

    ::bounce_await_freertos_condition(
      this->get_core(),
      condition_instance.get_condition(),
      &callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await a backend-local FreeRTOS condition inside a coroutine.
   * @param condition_instance Initialized backend-local condition.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    condition &condition_instance,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

  /**
   * @brief Raise a backend-local FreeRTOS condition from task context.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise(condition &condition_instance) noexcept {
    ::bounce_freertos_condition_raise(
      this->get_core(),
      condition_instance.get_condition());
  }

  /**
   * @brief Raise a backend-local FreeRTOS condition from ISR context.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise_from_isr(condition &condition_instance) noexcept {
    ::bounce_freertos_condition_raise_from_isr(
      this->get_core(),
      condition_instance.get_condition());
  }

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  /**
   * @brief Await ESP-IDF file-descriptor readiness and continue on a parked
   * task.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    unsigned int events,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_freertos_fd(
      this->get_core(),
      fd,
      events,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await ESP-IDF file-descriptor readiness with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
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
    unsigned int events,
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

    ::bounce_await_freertos_fd(
      this->get_core(),
      fd,
      events,
      &callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  /**
   * @brief Await ESP-IDF file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local waiter task.
   * @param events Bitmask of `BOUNCE_FREERTOS_FD_EVENT_*`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    unsigned int events,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
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
