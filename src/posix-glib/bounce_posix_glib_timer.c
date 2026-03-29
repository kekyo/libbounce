/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_POSIX_GLIB)

#include <errno.h>
#include <string.h>

#include <glib.h>
#include <pthread.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

// The timer should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

static inline int bounce_posix_glib_timer_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : EINVAL;
}

static inline int bounce_posix_glib_timer_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : EINVAL;
}

static void bounce_posix_glib_timer_release_source(GSource *source) {
  if (source != NULL) {
    g_source_destroy(source);
    g_source_unref(source);
  }
}

static void bounce_posix_glib_timer_complete(
  BOUNCE_TIMER *timer,
  BOUNCE_COMPLETION_RESULT result) {
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state = NULL;
  BOUNCE_COMPLETION_RESULT final_result = result;
  GSource *source = NULL;

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  if (timer->active) {
    if ((result != BOUNCE_COMPLETION_ABORTED) &&
        timer->abort_requested) {
      final_result = BOUNCE_COMPLETION_ABORTED;
    }

    source = timer->source;
    timer->source = NULL;
    completion = timer->completion;
    completion_state = timer->completion_state;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->bounce = NULL;
    timer->active = false;
    timer->settling = false;
    timer->abort_requested = false;
    timer->registration_active = false;
    (void)pthread_cond_broadcast(&timer->condition);
  }
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  bounce_posix_glib_timer_release_source(source);
  if (completion != NULL) {
    completion(final_result, completion_state);
  }
}

static void bounce_posix_glib_timer_post_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  bounce_posix_glib_timer_complete((BOUNCE_TIMER *)completion_state, result);
}

static void bounce_posix_glib_timer_cancellation_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  bounce_posix_glib_timer_complete((BOUNCE_TIMER *)completion_state, result);
}

static void bounce_posix_glib_timer_fail_start(BOUNCE_TIMER *timer) {
  GSource *source = NULL;

  (void)bounce_unregister_canceled(&timer->registration);

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  if (timer->active) {
    source = timer->source;
    timer->source = NULL;
    timer->bounce = NULL;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->active = false;
    timer->settling = false;
    timer->abort_requested = false;
    timer->registration_active = false;
    (void)pthread_cond_broadcast(&timer->condition);
  }
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  bounce_posix_glib_timer_release_source(source);
}

static gboolean bounce_posix_glib_timer_source_fired(gpointer parameter) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)parameter;
  BOUNCE_CORE *bounce = NULL;
  bool registration_active = false;
  bool can_complete = true;

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  if (!timer->active ||
      timer->settling) {
    (void)bounce_posix_glib_timer_unlock(&timer->lock);
    return G_SOURCE_REMOVE;
  }

  timer->settling = true;
  bounce = timer->bounce;
  registration_active = timer->registration_active;
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  if (registration_active) {
    can_complete = bounce_unregister_canceled(&timer->registration);
    if (can_complete) {
      (void)bounce_posix_glib_timer_lock(&timer->lock);
      timer->registration_active = false;
      (void)bounce_posix_glib_timer_unlock(&timer->lock);
    }
  }

  if (!can_complete) {
    return G_SOURCE_REMOVE;
  }
  if ((bounce != NULL) &&
      bounce_post(
        bounce,
        bounce_posix_glib_timer_post_completion,
        timer)) {
    return G_SOURCE_REMOVE;
  }

  bounce_posix_glib_timer_complete(timer, BOUNCE_COMPLETION_ABORTED);
  return G_SOURCE_REMOVE;
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
  GSource *source;
  bool active;

  if ((r == NULL) ||
      (timer == NULL) ||
      (completion == NULL)) {
    return false;
  }
  if (r->main_context == NULL) {
    return false;
  }

  source = g_timeout_source_new(duration_msec);
  if (source == NULL) {
    return false;
  }
  g_source_set_callback(
    source,
    bounce_posix_glib_timer_source_fired,
    timer,
    NULL);
  g_source_ref(source);

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  if ((timer->deinitialized != 0) ||
      timer->active) {
    (void)bounce_posix_glib_timer_unlock(&timer->lock);
    g_source_unref(source);
    g_source_unref(source);
    return false;
  }

  timer->source = source;
  timer->bounce = r;
  timer->completion = completion;
  timer->completion_state = completion_state;
  timer->active = true;
  timer->settling = false;
  timer->abort_requested = false;
  timer->registration_active = false;
  bounce_cancellation_registration_init(&timer->registration);
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  if (cancellation != NULL) {
    if (!bounce_register_canceled(
          r,
          cancellation,
          &timer->registration,
          bounce_posix_glib_timer_cancellation_completion,
          timer)) {
      bounce_posix_glib_timer_fail_start(timer);
      g_source_unref(source);
      return false;
    }

    (void)bounce_posix_glib_timer_lock(&timer->lock);
    timer->registration_active = (timer->registration.item != NULL);
    if (!timer->registration_active) {
      timer->settling = true;
    }
    (void)bounce_posix_glib_timer_unlock(&timer->lock);
  }

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  active = timer->active && !timer->settling;
  (void)bounce_posix_glib_timer_unlock(&timer->lock);
  if (!active) {
    g_source_unref(source);
    return true;
  }

  if (g_source_attach(source, r->main_context) == 0u) {
    bounce_posix_glib_timer_fail_start(timer);
    g_source_unref(source);
    return false;
  }

  g_source_unref(source);
  return true;
}

/**
 * @brief Deinitialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_deinit(BOUNCE_TIMER *timer) {
  BOUNCE_CORE *bounce = NULL;
  bool need_abort_completion = false;
  GSource *source = NULL;

  if (timer == NULL) {
    return;
  }

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  timer->deinitialized = 1;
  if (timer->active) {
    bounce = timer->bounce;
    timer->abort_requested = true;
    if (!timer->settling) {
      timer->settling = true;
      source = timer->source;
      timer->source = NULL;
      need_abort_completion = true;
    }
  }
  (void)pthread_cond_broadcast(&timer->condition);
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  if (need_abort_completion) {
    (void)bounce_unregister_canceled(&timer->registration);
  }
  bounce_posix_glib_timer_release_source(source);

  if (need_abort_completion) {
    if ((bounce != NULL) &&
        bounce_post(
          bounce,
          bounce_posix_glib_timer_post_completion,
          timer)) {
      (void)bounce_posix_glib_timer_lock(&timer->lock);
      (void)pthread_cond_broadcast(&timer->condition);
      (void)bounce_posix_glib_timer_unlock(&timer->lock);
    } else {
      bounce_posix_glib_timer_complete(timer, BOUNCE_COMPLETION_ABORTED);
    }
  }

  (void)bounce_posix_glib_timer_lock(&timer->lock);
  while (timer->active) {
    (void)pthread_cond_wait(&timer->condition, &timer->lock);
  }
  (void)bounce_posix_glib_timer_unlock(&timer->lock);

  bounce_cancellation_registration_deinit(&timer->registration);
  (void)pthread_cond_destroy(&timer->condition);
  (void)pthread_mutex_destroy(&timer->lock);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
