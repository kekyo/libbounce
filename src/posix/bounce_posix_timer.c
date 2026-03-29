/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_POSIX)

#include <errno.h>
#include <string.h>
#include <time.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

// The timer should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

static inline int bounce_posix_timer_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : EINVAL;
}

static inline int bounce_posix_timer_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : EINVAL;
}

static bool bounce_posix_timer_deadline_after_ms(
  struct timespec *deadline,
  uint32_t duration_msec) {
  if ((deadline == NULL) ||
      (clock_gettime(CLOCK_REALTIME, deadline) != 0)) {
    return false;
  }

  deadline->tv_sec += (time_t)(duration_msec / 1000u);
  deadline->tv_nsec += (long)((duration_msec % 1000u) * 1000000u);
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec += 1;
    deadline->tv_nsec -= 1000000000L;
  }
  return true;
}

static void *bounce_posix_timer_thread_proc(void *parameter) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)parameter;

  (void)bounce_posix_timer_lock(&timer->lock);
  for (;;) {
    while (!timer->active &&
           (timer->deinitialized == 0)) {
      (void)pthread_cond_wait(&timer->condition, &timer->lock);
    }
    if (!timer->active &&
        (timer->deinitialized != 0)) {
      (void)bounce_posix_timer_unlock(&timer->lock);
      return NULL;
    }

    while (timer->active) {
      int wait_result;

      if (timer->settling) {
        (void)pthread_cond_wait(&timer->condition, &timer->lock);
        continue;
      }

      wait_result = pthread_cond_timedwait(
        &timer->condition,
        &timer->lock,
        &timer->deadline);
      if (!timer->active) {
        continue;
      }
      if (timer->settling) {
        continue;
      }
      if (wait_result == ETIMEDOUT) {
        BOUNCE_CORE *bounce = timer->bounce;

        timer->settling = true;
        (void)bounce_posix_timer_unlock(&timer->lock);
        bounce_posix_condition_raise(
          bounce,
          &timer->fired_condition);
        (void)bounce_posix_timer_lock(&timer->lock);
      }
    }
  }
}

static void bounce_posix_timer_wait_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)completion_state;
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state_ = NULL;
  BOUNCE_COMPLETION_RESULT final_result = result;

  (void)bounce_posix_timer_lock(&timer->lock);
  if (timer->active) {
    if (timer->abort_requested) {
      final_result = BOUNCE_COMPLETION_ABORTED;
    }

    completion = timer->completion;
    completion_state_ = timer->completion_state;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->bounce = NULL;
    timer->active = false;
    timer->settling = false;
    timer->abort_requested = false;
    (void)pthread_cond_broadcast(&timer->condition);
  }
  (void)bounce_posix_timer_unlock(&timer->lock);

  if (completion != NULL) {
    completion(final_result, completion_state_);
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_init(BOUNCE_TIMER *timer) {
  if (timer == NULL) {
    return;
  }

  memset(timer, 0, sizeof *timer);
  (void)pthread_mutex_init(&timer->lock, NULL);
  (void)pthread_cond_init(&timer->condition, NULL);
  bounce_posix_condition_init(&timer->fired_condition);

  if (pthread_create(&timer->thread, NULL, bounce_posix_timer_thread_proc, timer) != 0) {
    timer->deinitialized = 1;
    return;
  }

  timer->thread_created = true;
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
  struct timespec deadline;

  if ((r == NULL) ||
      (timer == NULL) ||
      (completion == NULL)) {
    return false;
  }
  if (!bounce_posix_timer_deadline_after_ms(&deadline, duration_msec)) {
    return false;
  }

  (void)bounce_posix_timer_lock(&timer->lock);
  if ((timer->deinitialized != 0) ||
      !timer->thread_created ||
      timer->active) {
    (void)bounce_posix_timer_unlock(&timer->lock);
    return false;
  }

  timer->deadline = deadline;
  timer->bounce = r;
  timer->completion = completion;
  timer->completion_state = completion_state;
  timer->active = true;
  timer->settling = false;
  timer->abort_requested = false;
  (void)bounce_posix_timer_unlock(&timer->lock);

  bounce_await_posix_condition(
    r,
    &timer->fired_condition,
    bounce_posix_timer_wait_completion,
    timer,
    cancellation);

  (void)bounce_posix_timer_lock(&timer->lock);
  if (timer->active) {
    (void)pthread_cond_broadcast(&timer->condition);
  }
  (void)bounce_posix_timer_unlock(&timer->lock);
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

  (void)bounce_posix_timer_lock(&timer->lock);
  timer->deinitialized = 1;
  if (timer->active) {
    bounce = timer->bounce;
    timer->abort_requested = true;
    if (!timer->settling) {
      timer->settling = true;
      raise_condition = true;
    }
  }
  (void)pthread_cond_broadcast(&timer->condition);
  (void)bounce_posix_timer_unlock(&timer->lock);

  if (raise_condition && (bounce != NULL)) {
    bounce_posix_condition_raise(
      bounce,
      &timer->fired_condition);
  }

  (void)bounce_posix_timer_lock(&timer->lock);
  while (timer->active) {
    (void)pthread_cond_wait(&timer->condition, &timer->lock);
  }
  (void)bounce_posix_timer_unlock(&timer->lock);

  if (timer->thread_created) {
    (void)pthread_join(timer->thread, NULL);
    timer->thread_created = false;
  }

  (void)pthread_mutex_destroy(&timer->fired_condition.lock);
  (void)pthread_cond_destroy(&timer->condition);
  (void)pthread_mutex_destroy(&timer->lock);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
