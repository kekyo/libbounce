/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_POSIX_H
#define _LIBBOUNCE_POSIX_H

#if defined(BOUNCE_POSIX)

#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "bounce.h"
#include "utils.h"

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief POSIX backend overview.
 * @remarks Backend-local producer paths publish ready completion items into the
 * shared ready queue, and parked threads only drain that queue and execute
 * completions. Linux integrates fd waits and io_uring completion into the park
 * path itself. Non-Linux POSIX keeps fd waiting backend-local through fixed
 * `poll()` waiter shards.
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
 * @brief Maximum number of backend-local POSIX waiter shards.
 */
#if !defined(BOUNCE_MAX_POSIX_WAITERS)
#define BOUNCE_MAX_POSIX_WAITERS 2
#endif

/**
 * @brief Maximum number of fd waits assigned to one POSIX waiter shard.
 */
#if !defined(BOUNCE_MAX_POSIX_WAITS_PER_WAITER)
#define BOUNCE_MAX_POSIX_WAITS_PER_WAITER 64
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
struct __BOUNCE_POSIX_IO_URING_WAIT;
struct io_uring;
struct io_uring_sqe;

typedef void (*BOUNCE_POSIX_IO_URING_PREPARE)(
  struct io_uring_sqe *sqe,
  void *prepare_state);

typedef struct BOUNCE_POSIX_IO_URING_OP {
  BOUNCE_POSIX_IO_URING_PREPARE prepare;
  void *prepare_state;
  int result;
  unsigned int cqe_flags;
  volatile int active;
} BOUNCE_POSIX_IO_URING_OP;
#endif

typedef struct BOUNCE_POSIX_CONDITION BOUNCE_POSIX_CONDITION;
typedef struct __BOUNCE_POSIX_WAITER __BOUNCE_POSIX_WAITER;

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
  int fd;
  short events;
  BOUNCE_CANCELLATION *cancellation;
  BOUNCE_CANCELLATION_REGISTRATION *registration_owner;
  BOUNCE_POSIX_CONDITION *condition;
  __BOUNCE_POSIX_WAITER *waiter;
  unsigned int waiter_slot;
#if defined(__linux__)
  struct __BOUNCE_POSIX_IO_URING_WAIT *io_uring_wait;
#endif
} __BOUNCE_COMPLETION_ITEM;

/**
 * @brief Backend-local one-shot condition object for POSIX awaiting.
 */
struct BOUNCE_POSIX_CONDITION {
  pthread_mutex_t lock;
  BOUNCE_LIST waiters;
};

/**
 * @brief Backend-local POSIX waiter shard state.
 */
struct __BOUNCE_POSIX_WAITER {
  pthread_mutex_t lock;
  pthread_t thread;
  BOUNCE_CORE *bounce;
  int control_pipe_fds[2];
  bool thread_created;
  unsigned int active_count;
  __BOUNCE_COMPLETION_ITEM *slots[BOUNCE_MAX_POSIX_WAITS_PER_WAITER];
};

/**
 * @brief POSIX timer helper backed by a timer worker thread and a backend-local condition.
 */
struct BOUNCE_TIMER {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  pthread_t thread;
  BOUNCE_POSIX_CONDITION fired_condition;
  struct timespec deadline;
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  volatile int deinitialized;
  bool thread_created;
  bool active;
  bool settling;
  bool abort_requested;
};

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
 * @brief Caller-owned file I/O helper operation.
 * @remarks This helper is independent from the core wait-item pools. Linux
 * read/write/flush operations reuse the backend's one-shot io_uring operation
 * support when available; otherwise POSIX readiness/post helpers dispatch the
 * final syscall on a parked thread.
 */
struct BOUNCE_FILE_IO {
  pthread_mutex_t lock;
  BOUNCE_CANCELLATION_REGISTRATION cancellation_registration;
#if defined(__linux__)
  BOUNCE_POSIX_IO_URING_OP io_uring_operation;
#endif
  BOUNCE_CORE *bounce;
  BOUNCE_COMPLETION completion;
  void *completion_state;
  int fd;
  void *buffer;
  const void *const_buffer;
  int64_t offset;
  size_t length;
  int whence;
  BOUNCE_FILE_FLUSH_MODE flush_mode;
  int operation;
  int64_t result;
  int error_code;
  bool active;
  bool cancellation_registration_active;
};

/**
 * @brief Bounce core storage.
 * @remarks The first `BOUNCE_MAX_STATIC_COMPLETION_ITEMS` items stay inline,
 * and overflow expands through append-only dynamic blocks on demand.
 */
struct BOUNCE_CORE {
  pthread_mutex_t lock;
  pthread_cond_t parkers_cond;
  volatile int shutting_down;
  volatile int shutdown_wait_for_idle;
  BOUNCE_QUEUE ready_queue;
  BOUNCE_STACK free_items;
  BOUNCE_DYNAMIC_BLOCK_LIST dynamic_completion_blocks;
  __BOUNCE_COMPLETION_ITEM
    static_completion_items[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
#if defined(__linux__)
  int wake_pipe_fds[2];
  int io_uring_event_fd;
  bool linux_unified_wait_enabled;
  struct io_uring *io_uring_ring;
  struct __BOUNCE_POSIX_IO_URING_WAIT *io_uring_waits;
#endif
  __BOUNCE_POSIX_WAITER waiters[BOUNCE_MAX_POSIX_WAITERS];
};

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a backend-local POSIX condition object.
 * @param condition Condition storage provided by the caller.
 */
extern void bounce_posix_condition_init(BOUNCE_POSIX_CONDITION *condition);

/**
 * @brief Await a backend-local POSIX condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks If capacity is exhausted, completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`.
 */
extern void bounce_await_posix_condition(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_CONDITION *condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Raise a backend-local POSIX condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 */
extern void bounce_posix_condition_raise(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_CONDITION *condition);

/**
 * @brief Await POSIX file-descriptor readiness.
 * @param r Initialized BOUNCE_CORE.
 * @param fd File descriptor watched by the backend.
 * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @remarks Linux integrates these waits into the park-thread sleep set.
 * Other POSIX targets use backend-local waiter threads. If registration fails,
 * completion is forced with
 * `BOUNCE_COMPLETION_ABORTED`. Registration is one-shot and must be renewed by
 * the completion path if the caller wants to wait again.
 */
extern void bounce_await_posix_fd(
  BOUNCE_CORE *r,
  int fd,
  short events,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

#if defined(__linux__)
extern void bounce_posix_io_uring_op_init(
  BOUNCE_POSIX_IO_URING_OP *op,
  BOUNCE_POSIX_IO_URING_PREPARE prepare,
  void *prepare_state);

extern void bounce_posix_io_uring_op_deinit(BOUNCE_POSIX_IO_URING_OP *op);

extern int bounce_posix_io_uring_op_result(
  const BOUNCE_POSIX_IO_URING_OP *op);

extern unsigned int bounce_posix_io_uring_op_cqe_flags(
  const BOUNCE_POSIX_IO_URING_OP *op);

extern void bounce_await_posix_io_uring_op(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_IO_URING_OP *op,
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
 * @brief Caller-owned backend-local POSIX condition storage for the C++
 * helper API.
 */
class condition {
private:
  BOUNCE_POSIX_CONDITION condition_;
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
    ::bounce_posix_condition_init(&condition_);
  }

  /**
   * @brief Deinitialize the backend-local condition storage.
   */
  ~condition() = default;

  /**
   * @brief Get the underlying condition storage.
   * @return Backend condition storage pointer.
   */
  inline BOUNCE_POSIX_CONDITION *get_condition() noexcept {
    return &condition_;
  }
};

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
   * @brief Await a backend-local POSIX condition and continue on a parked
   * thread.
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
    ::bounce_await_posix_condition(
      this->get_core(),
      condition_instance.get_condition(),
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a backend-local POSIX condition with a C++ callable
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

    ::bounce_await_posix_condition(
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
   * @brief Await a backend-local POSIX condition inside a coroutine.
   * @param condition_instance Initialized backend-local condition.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    condition &condition_instance,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

  /**
   * @brief Raise a backend-local POSIX condition.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise(condition &condition_instance) noexcept {
    ::bounce_posix_condition_raise(
      this->get_core(),
      condition_instance.get_condition());
  }

  /**
   * @brief Await POSIX file-descriptor readiness and continue on a parked
   * thread.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    short events,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_fd(
      this->get_core(),
      fd,
      events,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await POSIX file-descriptor readiness with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
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
    short events,
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

    ::bounce_await_posix_fd(
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
   * @brief Await POSIX file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    short events,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

#if defined(__linux__)
  inline void wait(
    BOUNCE_POSIX_IO_URING_OP &operation,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_io_uring_op(
      this->get_core(),
      &operation,
      completion,
      completion_state,
      cancellation);
  }

  template<typename COMPLETION_TYPE>
  inline bool wait(
    BOUNCE_POSIX_IO_URING_OP &operation,
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

    ::bounce_await_posix_io_uring_op(
      this->get_core(),
      &operation,
      &bounce_base<BOUNCE_CORE>::template callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  await_operation await(
    BOUNCE_POSIX_IO_URING_OP &operation,
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
   * @brief Await a backend-local POSIX condition and continue on a parked
   * thread.
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
    ::bounce_await_posix_condition(
      this->get_core(),
      condition_instance.get_condition(),
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await a backend-local POSIX condition with a C++ callable
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

    ::bounce_await_posix_condition(
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
   * @brief Await a backend-local POSIX condition inside a coroutine.
   * @param condition_instance Initialized backend-local condition.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    condition &condition_instance,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

  /**
   * @brief Raise a backend-local POSIX condition.
   * @param condition_instance Initialized backend-local condition.
   */
  inline void raise(condition &condition_instance) noexcept {
    ::bounce_posix_condition_raise(
      this->get_core(),
      condition_instance.get_condition());
  }

  /**
   * @brief Await POSIX file-descriptor readiness and continue on a parked
   * thread.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   */
  inline void wait(
    int fd,
    short events,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_fd(
      this->get_core(),
      fd,
      events,
      completion,
      completion_state,
      cancellation);
  }

  /**
   * @brief Await POSIX file-descriptor readiness with a C++ callable
   * completion.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
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
    short events,
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

    ::bounce_await_posix_fd(
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
   * @brief Await POSIX file-descriptor readiness inside a coroutine.
   * @param fd File descriptor watched by the backend-local waiter.
   * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
   * @param cancellation Cancellation when provided.
   * @return Coroutine awaitable operation.
   */
  await_operation await(
    int fd,
    short events,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif

#if defined(__linux__)
  inline void wait(
    BOUNCE_POSIX_IO_URING_OP &operation,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    ::bounce_await_posix_io_uring_op(
      this->get_core(),
      &operation,
      completion,
      completion_state,
      cancellation);
  }

  template<typename COMPLETION_TYPE>
  inline bool wait(
    BOUNCE_POSIX_IO_URING_OP &operation,
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

    ::bounce_await_posix_io_uring_op(
      this->get_core(),
      &operation,
      &callable_completion<AWAIT_COMPLETION_TYPE>,
      completion_state.get(),
      cancellation);

    (void)completion_state.release();
    return true;
  }

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
  await_operation await(
    BOUNCE_POSIX_IO_URING_OP &operation,
    BOUNCE_CANCELLATION *cancellation) noexcept;
#endif
#endif
};

#if defined(__linux__)
class io_uring_operation {
private:
  BOUNCE_POSIX_IO_URING_OP operation_;
  io_uring_operation(const io_uring_operation&) = delete;
  io_uring_operation(io_uring_operation&&) = delete;
  io_uring_operation& operator=(const io_uring_operation&) = delete;
  io_uring_operation& operator=(io_uring_operation&&) = delete;

public:
  inline io_uring_operation(
    BOUNCE_POSIX_IO_URING_PREPARE prepare,
    void *prepare_state) noexcept {
    ::bounce_posix_io_uring_op_init(&operation_, prepare, prepare_state);
  }

  ~io_uring_operation() {
    ::bounce_posix_io_uring_op_deinit(&operation_);
  }

  inline BOUNCE_POSIX_IO_URING_OP *get_operation() noexcept {
    return &operation_;
  }

  inline int result() const noexcept {
    return ::bounce_posix_io_uring_op_result(&operation_);
  }

  inline unsigned int cqe_flags() const noexcept {
    return ::bounce_posix_io_uring_op_cqe_flags(&operation_);
  }

  inline bool active() const noexcept {
    return operation_.active != 0;
  }
};
#endif

/**
 * @brief Caller-owned file I/O operation storage for the C++ helper API.
 */
class file_io : public file_io_base<BOUNCE_CORE, BOUNCE_FILE_IO> {
public:
  /**
   * @brief Initialize the file I/O operation.
   */
  inline file_io() noexcept: file_io_base() {
  }

  /**
   * @brief Deinitialize the file I/O operation.
   */
  ~file_io() = default;
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
