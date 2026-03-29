/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_FREERTOS) || defined(FREERTOS) || defined(ESP_PLATFORM)

#include <string.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

// The timer should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

#if defined(ESP_PLATFORM)
static portMUX_TYPE bounce_freertos_timer_lock_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

static inline void bounce_freertos_timer_lock(void) {
#if defined(ESP_PLATFORM)
  taskENTER_CRITICAL(&bounce_freertos_timer_lock_mux);
#else
  taskENTER_CRITICAL();
#endif
}

static inline void bounce_freertos_timer_unlock(void) {
#if defined(ESP_PLATFORM)
  taskEXIT_CRITICAL(&bounce_freertos_timer_lock_mux);
#else
  taskEXIT_CRITICAL();
#endif
}

static inline TickType_t bounce_freertos_timer_duration_ticks(
  uint32_t duration_msec) {
  TickType_t duration_ticks = pdMS_TO_TICKS(duration_msec);

  if (duration_ticks == 0u) {
    duration_ticks = 1u;
  }
  return duration_ticks;
}

static void bounce_freertos_timer_clear_settled_signal(BOUNCE_TIMER *timer) {
  if ((timer == NULL) ||
      (timer->settled_semaphore == NULL)) {
    return;
  }

  while (xSemaphoreTake(timer->settled_semaphore, 0u) == pdTRUE) {
  }
}

static void bounce_freertos_timer_notify_settled(
  void *parameter1,
  uint32_t parameter2) {
  SemaphoreHandle_t semaphore = (SemaphoreHandle_t)parameter1;

  (void)parameter2;
  if (semaphore != NULL) {
    (void)xSemaphoreGive(semaphore);
  }
}

static void bounce_freertos_timer_wait_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)completion_state;
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state_ = NULL;
  BOUNCE_COMPLETION_RESULT final_result = result;

  if (timer == NULL) {
    return;
  }

  if ((result != BOUNCE_COMPLETION_COMPLETED) &&
      (timer->timer_handle != NULL)) {
    (void)xTimerStop(timer->timer_handle, 0u);
  }

  bounce_freertos_timer_lock();
  if (timer->active != 0) {
    if (timer->abort_requested != 0) {
      final_result = BOUNCE_COMPLETION_ABORTED;
    }

    completion = timer->completion;
    completion_state_ = timer->completion_state;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->bounce = NULL;
    timer->active = 0;
    timer->settling = 0;
    timer->abort_requested = 0;
  }
  bounce_freertos_timer_unlock();

  if (timer->settled_semaphore != NULL) {
    (void)xSemaphoreGive(timer->settled_semaphore);
  }
  if (completion != NULL) {
    completion(final_result, completion_state_);
  }
}

static void bounce_freertos_timer_expired(TimerHandle_t timer_handle) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)pvTimerGetTimerID(timer_handle);
  BOUNCE_CORE *bounce = NULL;

  if (timer == NULL) {
    return;
  }

  bounce_freertos_timer_lock();
  if ((timer->active != 0) &&
      (timer->settling == 0)) {
    timer->settling = 1;
    bounce = timer->bounce;
  }
  bounce_freertos_timer_unlock();

  if (bounce != NULL) {
    bounce_freertos_condition_raise(
      bounce,
      &timer->fired_condition);
  }
}

/**
 * @brief Initialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_init(BOUNCE_TIMER *timer) {
  if (timer == NULL) {
    return;
  }

  memset(timer, 0, sizeof *timer);
  bounce_freertos_condition_init(&timer->fired_condition);
  timer->settled_semaphore =
    xSemaphoreCreateBinaryStatic(&timer->settled_semaphore_buffer);
  if (timer->settled_semaphore == NULL) {
    timer->deinitialized = 1;
    return;
  }
  bounce_freertos_timer_clear_settled_signal(timer);

  timer->timer_handle = xTimerCreateStatic(
    "bounce-timer",
    1u,
    pdFALSE,
    timer,
    bounce_freertos_timer_expired,
    &timer->timer_buffer);
  if (timer->timer_handle == NULL) {
    timer->deinitialized = 1;
  }
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
  const TickType_t duration_ticks =
    bounce_freertos_timer_duration_ticks(duration_msec);
  BOUNCE_CORE *bounce_for_abort = NULL;
  bool should_raise_abort = false;

  if ((r == NULL) ||
      (timer == NULL) ||
      (completion == NULL) ||
      (timer->timer_handle == NULL)) {
    return false;
  }

  bounce_freertos_timer_clear_settled_signal(timer);
  bounce_freertos_timer_lock();
  if ((timer->deinitialized != 0) ||
      (timer->active != 0)) {
    bounce_freertos_timer_unlock();
    return false;
  }

  timer->bounce = r;
  timer->completion = completion;
  timer->completion_state = completion_state;
  timer->active = 1;
  timer->settling = 0;
  timer->abort_requested = 0;
  bounce_freertos_timer_unlock();

  bounce_await_freertos_condition(
    r,
    &timer->fired_condition,
    bounce_freertos_timer_wait_completion,
    timer,
    cancellation);

  bounce_freertos_timer_lock();
  if (timer->active != 0) {
    if (xTimerChangePeriod(timer->timer_handle, duration_ticks, 0u) != pdPASS) {
      if (timer->settling == 0) {
        timer->abort_requested = 1;
        timer->settling = 1;
        bounce_for_abort = timer->bounce;
        should_raise_abort = bounce_for_abort != NULL;
      }
    }
  }
  bounce_freertos_timer_unlock();

  if (should_raise_abort) {
    bounce_freertos_condition_raise(
      bounce_for_abort,
      &timer->fired_condition);
  }
  return true;
}

/**
 * @brief Deinitialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_deinit(BOUNCE_TIMER *timer) {
  BOUNCE_CORE *bounce = NULL;
  bool raise_condition = false;

  if (timer == NULL) {
    return;
  }

  bounce_freertos_timer_lock();
  timer->deinitialized = 1;
  if (timer->active != 0) {
    bounce = timer->bounce;
    timer->abort_requested = 1;
    if (timer->settling == 0) {
      timer->settling = 1;
      raise_condition = bounce != NULL;
    }
  }
  bounce_freertos_timer_unlock();

  if (timer->timer_handle != NULL) {
    (void)xTimerStop(timer->timer_handle, 0u);
  }
  if (raise_condition) {
    bounce_freertos_condition_raise(
      bounce,
      &timer->fired_condition);
  }

  for (;;) {
    int active;

    bounce_freertos_timer_lock();
    active = timer->active;
    bounce_freertos_timer_unlock();
    if (active == 0) {
      break;
    }
    if (timer->settled_semaphore == NULL) {
      break;
    }
    (void)xSemaphoreTake(timer->settled_semaphore, portMAX_DELAY);
  }

  if (timer->timer_handle != NULL) {
    bounce_freertos_timer_clear_settled_signal(timer);
    if (xTimerDelete(timer->timer_handle, portMAX_DELAY) == pdPASS) {
      if (xTimerPendFunctionCall(
            bounce_freertos_timer_notify_settled,
            timer->settled_semaphore,
            0u,
            portMAX_DELAY) == pdPASS) {
        (void)xSemaphoreTake(timer->settled_semaphore, portMAX_DELAY);
      }
    }
    timer->timer_handle = NULL;
  }
}

//////////////////////////////////////////////////////////////////////////////////

#endif
