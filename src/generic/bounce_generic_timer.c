/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_GENERIC)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

// The timer should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

static BOUNCE_GENERIC_TIMEOUT_CREATE bounce_generic_timeout_create_sink = NULL;
static BOUNCE_GENERIC_TIMEOUT_START bounce_generic_timeout_start_sink = NULL;
static BOUNCE_GENERIC_TIMEOUT_CANCEL bounce_generic_timeout_cancel_sink = NULL;

static inline void bounce_generic_timer_lock(__BOUNCE_GENERIC_SPINLOCK *lock) {
  for (;;) {
    unsigned int expected = 0u;

    if (__atomic_compare_exchange_n(
          &lock->state,
          &expected,
          1u,
          false,
          __ATOMIC_ACQUIRE,
          __ATOMIC_RELAXED)) {
      return;
    }

    while (__atomic_load_n(&lock->state, __ATOMIC_RELAXED) != 0u) {
    }
  }
}

static inline void bounce_generic_timer_unlock(__BOUNCE_GENERIC_SPINLOCK *lock) {
  __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

static inline BOUNCE_GENERIC_TIMEOUT_CREATE bounce_generic_get_timeout_create(void) {
  return __atomic_load_n(&bounce_generic_timeout_create_sink, __ATOMIC_ACQUIRE);
}

static inline BOUNCE_GENERIC_TIMEOUT_START bounce_generic_get_timeout_start(void) {
  return __atomic_load_n(&bounce_generic_timeout_start_sink, __ATOMIC_ACQUIRE);
}

static inline BOUNCE_GENERIC_TIMEOUT_CANCEL bounce_generic_get_timeout_cancel(void) {
  return __atomic_load_n(&bounce_generic_timeout_cancel_sink, __ATOMIC_ACQUIRE);
}

static void bounce_generic_timer_reset_start_state(BOUNCE_TIMER *timer) {
  bounce_generic_timer_lock(&timer->lock);
  if (timer->active) {
    timer->timeout_handle = NULL;
    timer->bounce = NULL;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->active = false;
    timer->registration_active = false;
  }
  bounce_generic_timer_unlock(&timer->lock);
}

static void bounce_generic_timer_complete(
  BOUNCE_TIMER *timer,
  BOUNCE_COMPLETION_RESULT result,
  bool release_timeout_handle) {
  BOUNCE_GENERIC_TIMEOUT_CANCEL cancel_timeout = bounce_generic_get_timeout_cancel();
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state = NULL;
  void *timeout_handle = NULL;

  if (timer == NULL) {
    return;
  }

  bounce_generic_timer_lock(&timer->lock);
  if (timer->active) {
    if (release_timeout_handle) {
      timeout_handle = timer->timeout_handle;
    }
    timer->timeout_handle = NULL;
    completion = timer->completion;
    completion_state = timer->completion_state;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->bounce = NULL;
    timer->active = false;
    timer->registration_active = false;
  }
  bounce_generic_timer_unlock(&timer->lock);

  if ((timeout_handle != NULL) && (cancel_timeout != NULL)) {
    cancel_timeout(timeout_handle);
  }
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static void bounce_generic_timer_post_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  bounce_generic_timer_complete(
    (BOUNCE_TIMER *)completion_state,
    result,
    true);
}

static void bounce_generic_timer_cancellation_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  bounce_generic_timer_complete(
    (BOUNCE_TIMER *)completion_state,
    result,
    true);
}

static void bounce_generic_timer_sink_fired(void *completion_state) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)completion_state;
  BOUNCE_CORE *bounce = NULL;
  bool registration_active = false;

  if (timer == NULL) {
    return;
  }

  bounce_generic_timer_lock(&timer->lock);
  if (timer->active) {
    bounce = timer->bounce;
    registration_active = timer->registration_active;
  }
  bounce_generic_timer_unlock(&timer->lock);

  if (bounce == NULL) {
    return;
  }

  if (registration_active) {
    if (!bounce_unregister_canceled(&timer->registration)) {
      return;
    }

    bounce_generic_timer_lock(&timer->lock);
    if (timer->active) {
      timer->registration_active = false;
    }
    bounce_generic_timer_unlock(&timer->lock);
  }

  if (bounce_post(
        bounce,
        bounce_generic_timer_post_completion,
        timer)) {
    return;
  }

  bounce_generic_timer_complete(
    timer,
    BOUNCE_COMPLETION_ABORTED,
    false);
}

void bounce_generic_set_timeout_sink(
  BOUNCE_GENERIC_TIMEOUT_CREATE create_timeout,
  BOUNCE_GENERIC_TIMEOUT_START start_timeout,
  BOUNCE_GENERIC_TIMEOUT_CANCEL cancel_timeout) {
  __atomic_store_n(&bounce_generic_timeout_cancel_sink, cancel_timeout, __ATOMIC_RELEASE);
  __atomic_store_n(&bounce_generic_timeout_start_sink, start_timeout, __ATOMIC_RELEASE);
  __atomic_store_n(&bounce_generic_timeout_create_sink, create_timeout, __ATOMIC_RELEASE);
}

/**
 * @brief Initialize a timer.
 * @param timer Timer storage provided by the caller.
 */
void bounce_timer_init(BOUNCE_TIMER *timer) {
  if (timer == NULL) {
    return;
  }

  memset(timer, 0, sizeof *timer);
  bounce_cancellation_registration_init(&timer->registration);
}

/**
 * @brief Set timeout.
 * @param duration_msec Timeout when reached this duration later.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when succeeded.
 */
bool bounce_await_timeout(
  BOUNCE_CORE *r,
  BOUNCE_TIMER *timer,
  uint32_t duration_msec,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  BOUNCE_GENERIC_TIMEOUT_CREATE create_timeout =
    bounce_generic_get_timeout_create();
  BOUNCE_GENERIC_TIMEOUT_START start_timeout =
    bounce_generic_get_timeout_start();
  BOUNCE_GENERIC_TIMEOUT_CANCEL cancel_timeout =
    bounce_generic_get_timeout_cancel();
  void *timeout_handle = NULL;

  if ((r == NULL) ||
      (timer == NULL) ||
      (completion == NULL) ||
      (create_timeout == NULL) ||
      (start_timeout == NULL) ||
      (cancel_timeout == NULL)) {
    return false;
  }

  bounce_generic_timer_lock(&timer->lock);
  if ((timer->deinitialized != 0) ||
      timer->active) {
    bounce_generic_timer_unlock(&timer->lock);
    return false;
  }

  timer->bounce = r;
  timer->completion = completion;
  timer->completion_state = completion_state;
  timer->timeout_handle = NULL;
  timer->active = true;
  timer->registration_active = false;
  bounce_generic_timer_unlock(&timer->lock);

  if (cancellation != NULL) {
    if (!bounce_register_canceled(
          r,
          cancellation,
          &timer->registration,
          bounce_generic_timer_cancellation_completion,
          timer)) {
      bounce_generic_timer_reset_start_state(timer);
      return false;
    }

    bounce_generic_timer_lock(&timer->lock);
    if (timer->active) {
      timer->registration_active = true;
    }
    bounce_generic_timer_unlock(&timer->lock);

    bounce_generic_timer_lock(&timer->lock);
    if (!timer->active) {
      bounce_generic_timer_unlock(&timer->lock);
      return true;
    }
    bounce_generic_timer_unlock(&timer->lock);
  }

  if (!create_timeout(
        duration_msec,
        bounce_generic_timer_sink_fired,
        timer,
        &timeout_handle) ||
      (timeout_handle == NULL)) {
    bool completion_pending = false;

    if (cancellation != NULL) {
      completion_pending =
        !bounce_unregister_canceled(&timer->registration);
    }
    if (!completion_pending) {
      bounce_generic_timer_reset_start_state(timer);
    }
    return completion_pending;
  }

  bounce_generic_timer_lock(&timer->lock);
  if (!timer->active) {
    bounce_generic_timer_unlock(&timer->lock);
    cancel_timeout(timeout_handle);
    return true;
  }
  timer->timeout_handle = timeout_handle;
  bounce_generic_timer_unlock(&timer->lock);

  start_timeout(timeout_handle);
  return true;
}

/**
 * @brief Deinitialize a timer.
 * @param timer Timer storage provided by the caller.
 * @remarks If a timeout is still active, this call busy-spins until that
 * request is settled. Calling this from the timer completion is allowed.
 */
void bounce_timer_deinit(BOUNCE_TIMER *timer) {
  BOUNCE_GENERIC_TIMEOUT_CANCEL cancel_timeout = bounce_generic_get_timeout_cancel();
  void *timeout_handle = NULL;
  bool active;
  bool registration_active;

  if (timer == NULL) {
    return;
  }

  __atomic_store_n(&timer->deinitialized, 1, __ATOMIC_RELEASE);

  bounce_generic_timer_lock(&timer->lock);
  active = timer->active;
  registration_active = timer->registration_active;
  timeout_handle = timer->timeout_handle;
  timer->timeout_handle = NULL;
  bounce_generic_timer_unlock(&timer->lock);

  if (active) {
    if (registration_active) {
      (void)bounce_unregister_canceled(&timer->registration);
    }
    if ((timeout_handle != NULL) && (cancel_timeout != NULL)) {
      cancel_timeout(timeout_handle);
    }
    bounce_generic_timer_complete(
      timer,
      BOUNCE_COMPLETION_ABORTED,
      false);
  }

  bounce_cancellation_registration_deinit(&timer->registration);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
