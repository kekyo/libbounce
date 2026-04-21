/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_harness.h"
#include "libbounce/bounce.h"

extern void test_cpp_wrapper_post_runs(void);
extern void test_cpp_wrapper_lambda_post_runs(void);
extern void test_cpp_wrapper_lambda_post_aborts_on_deinit(void);
extern void test_cpp_wrapper_timer_await_runs(void);
extern void test_cpp_wrapper_lambda_timer_await_runs(void);
extern void test_cpp_wrapper_lambda_timer_await_aborts_on_deinit(void);
extern void test_cpp_wrapper_cancellation_cancel_timeout_runs(void);
extern void test_cpp_wrapper_cancellation_precanceled_timeout_runs(void);
extern void test_cpp_wrapper_registration_completes_canceled(void);
extern void test_cpp_wrapper_registration_lambda_completes_canceled(void);
extern void test_cpp_wrapper_registration_unregister_prevents_callback(void);
extern void test_cpp_wrapper_registration_precanceled_completes_canceled(void);
extern void test_cpp_wrapper_shutdown_wait_for_idle_keeps_pending_registration_alive(void);
extern void test_cpp_wrapper_set_default_timeout_await_runs(void);
extern void test_cpp_wrapper_set_default_overrides_fallback_view(void);
extern void test_cpp_wrapper_nested_post_inlines_with_park_ex(void);
extern void test_cpp_wrapper_current_post_runs_on_defaulted_parker(void);
extern void test_cpp_wrapper_nested_post_falls_back_at_depth_limit(void);

#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)
extern void test_cpp_promise_resume_on_runs(void);
extern void test_cpp_promise_make_awaitable_runs(void);
extern void test_cpp_promise_make_awaitable_start_failed(void);
extern void test_cpp_promise_make_awaitable_aborted(void);
extern void test_cpp_promise_make_callback_promise_runs(void);
extern void test_cpp_promise_make_callback_promise_void_runs(void);
extern void test_cpp_promise_make_callback_promise_start_failed(void);
extern void test_cpp_promise_nested_value_runs(void);
extern void test_cpp_promise_exception_propagates(void);
extern void test_cpp_promise_await_canceled_runs(void);
extern void test_cpp_promise_fire_and_forget_void_runs(void);
extern void test_cpp_promise_fire_and_forget_value_runs(void);
extern void test_cpp_promise_fire_and_forget_empty_fails(void);
#endif

#define TEST_TIMEOUT_MS 5000u
#define TEST_STRESS_PRODUCER_COUNT 4u
#define TEST_STRESS_POSTS_PER_PRODUCER 250u

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      abort();                                                                   \
    }                                                                            \
  } while (0)

typedef struct TEST_PARK_THREAD_CONTEXT {
  BOUNCE_CORE *bounce;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned int max_inline_depth;
  bool started;
  bool finished;
  bool park_result;
} TEST_PARK_THREAD_CONTEXT;

typedef struct TEST_COMPLETION_CONTEXT {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned int call_count;
  unsigned int order;
  int result;
  pthread_t callback_thread;
  volatile unsigned int *next_order;
  bool callback_thread_set;
  BOUNCE_CORE *observed_bounce;
} TEST_COMPLETION_CONTEXT;

typedef struct TEST_STRESS_CONTEXT {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned int completed_count;
  pthread_t callback_thread;
  bool callback_thread_set;
} TEST_STRESS_CONTEXT;

typedef struct TEST_STRESS_PRODUCER_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_STRESS_CONTEXT *stress;
  unsigned int remaining_posts;
} TEST_STRESS_PRODUCER_CONTEXT;

typedef struct TEST_INLINE_POST_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
} TEST_INLINE_POST_CONTEXT;

typedef struct TEST_GENERIC_TIMEOUT_HANDLE {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  bool thread_created;
  bool started;
  bool canceled;
  bool finished;
  bool release_on_exit;
  uint32_t duration_msec;
  BOUNCE_GENERIC_TIMEOUT_CALLBACK callback;
  void *callback_state;
} TEST_GENERIC_TIMEOUT_HANDLE;

static void test_generic_timeout_handle_destroy(TEST_GENERIC_TIMEOUT_HANDLE *handle) {
  ASSERT_TRUE(pthread_cond_destroy(&handle->cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&handle->mutex) == 0);
  free(handle);
}

static struct timespec test_generic_timeout_deadline_after_ms(uint32_t timeout_ms) {
  struct timespec timeout;

  ASSERT_TRUE(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
  timeout.tv_sec += (time_t)(timeout_ms / 1000u);
  timeout.tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
  if (timeout.tv_nsec >= 1000000000L) {
    timeout.tv_sec += 1;
    timeout.tv_nsec -= 1000000000L;
  }
  return timeout;
}

static void *test_generic_timeout_thread(void *parameter) {
  TEST_GENERIC_TIMEOUT_HANDLE *handle = parameter;
  bool should_fire = false;
  bool release_on_exit;

  ASSERT_TRUE(pthread_mutex_lock(&handle->mutex) == 0);
  while (!handle->started && !handle->canceled) {
    ASSERT_TRUE(pthread_cond_wait(&handle->cond, &handle->mutex) == 0);
  }

  if (!handle->canceled) {
    struct timespec deadline =
      test_generic_timeout_deadline_after_ms(handle->duration_msec);

    while (!handle->canceled) {
      int wait_result =
        pthread_cond_timedwait(&handle->cond, &handle->mutex, &deadline);

      ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
      if (wait_result == ETIMEDOUT) {
        should_fire = true;
        break;
      }
    }
  }

  ASSERT_TRUE(pthread_mutex_unlock(&handle->mutex) == 0);
  if (should_fire) {
    handle->callback(handle->callback_state);
  }

  ASSERT_TRUE(pthread_mutex_lock(&handle->mutex) == 0);
  handle->finished = true;
  release_on_exit = handle->release_on_exit;
  ASSERT_TRUE(pthread_cond_broadcast(&handle->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&handle->mutex) == 0);

  if (release_on_exit) {
    test_generic_timeout_handle_destroy(handle);
  }
  return NULL;
}

static bool test_generic_create_timeout(
  uint32_t timeout_msec,
  BOUNCE_GENERIC_TIMEOUT_CALLBACK callback,
  void *callback_state,
  void **timeout_handle) {
  TEST_GENERIC_TIMEOUT_HANDLE *handle;

  if ((callback == NULL) || (timeout_handle == NULL)) {
    return false;
  }

  handle = calloc(1u, sizeof *handle);
  if (handle == NULL) {
    return false;
  }

  ASSERT_TRUE(pthread_mutex_init(&handle->mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&handle->cond, NULL) == 0);
  handle->duration_msec = timeout_msec;
  handle->callback = callback;
  handle->callback_state = callback_state;
  if (pthread_create(&handle->thread, NULL, test_generic_timeout_thread, handle) != 0) {
    test_generic_timeout_handle_destroy(handle);
    return false;
  }

  handle->thread_created = true;
  *timeout_handle = handle;
  return true;
}

static void test_generic_start_timeout(void *timeout_handle) {
  TEST_GENERIC_TIMEOUT_HANDLE *handle = timeout_handle;

  ASSERT_TRUE(handle != NULL);
  ASSERT_TRUE(pthread_mutex_lock(&handle->mutex) == 0);
  handle->started = true;
  ASSERT_TRUE(pthread_cond_broadcast(&handle->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&handle->mutex) == 0);
}

static void test_generic_cancel_timeout(void *timeout_handle) {
  TEST_GENERIC_TIMEOUT_HANDLE *handle = timeout_handle;
  bool release_on_exit;

  ASSERT_TRUE(handle != NULL);
  ASSERT_TRUE(pthread_mutex_lock(&handle->mutex) == 0);
  handle->canceled = true;
  release_on_exit =
    handle->thread_created &&
    pthread_equal(pthread_self(), handle->thread);
  handle->release_on_exit = release_on_exit;
  ASSERT_TRUE(pthread_cond_broadcast(&handle->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&handle->mutex) == 0);

  if (handle->thread_created && !release_on_exit) {
    ASSERT_TRUE(pthread_join(handle->thread, NULL) == 0);
  }
  if (!release_on_exit) {
    test_generic_timeout_handle_destroy(handle);
  }
}

static struct timespec test_deadline_after_ms(unsigned int timeout_ms) {
  struct timespec timeout;

  ASSERT_TRUE(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
  timeout.tv_sec += (time_t)(timeout_ms / 1000u);
  timeout.tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
  if (timeout.tv_nsec >= 1000000000L) {
    timeout.tv_sec += 1;
    timeout.tv_nsec -= 1000000000L;
  }
  return timeout;
}

static double test_monotonic_now_ms(void) {
  struct timespec now;

  ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return ((double)now.tv_sec * 1000.0) + ((double)now.tv_nsec / 1000000.0);
}

static void test_yield_park_once_poll(void) {
  struct timespec delay;

  delay.tv_sec = 0;
  delay.tv_nsec = 1000000L;
  while (nanosleep(&delay, &delay) != 0) {
    ASSERT_TRUE(errno == EINTR);
  }
}

static void test_completion_callback(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_COMPLETION_CONTEXT *context = completion_state;

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->call_count += 1u;
  if (context->next_order != NULL) {
    context->order =
      __atomic_add_fetch(context->next_order, 1u, __ATOMIC_ACQ_REL);
  }
  context->result = (int)result;
  context->callback_thread = pthread_self();
  context->callback_thread_set = true;
  context->observed_bounce = bounce_get_core();
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_completion_context_init(TEST_COMPLETION_CONTEXT *context) {
  memset(context, 0, sizeof *context);
  ASSERT_TRUE(pthread_mutex_init(&context->mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&context->cond, NULL) == 0);
  context->result = -1;
}

static void test_completion_context_init_ordered(
  TEST_COMPLETION_CONTEXT *context,
  volatile unsigned int *next_order) {
  test_completion_context_init(context);
  context->next_order = next_order;
}

static void test_completion_context_destroy(TEST_COMPLETION_CONTEXT *context) {
  ASSERT_TRUE(pthread_cond_destroy(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&context->mutex) == 0);
}

static unsigned int test_completion_call_count(TEST_COMPLETION_CONTEXT *context) {
  unsigned int call_count;

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  call_count = context->call_count;
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
  return call_count;
}

static void test_wait_completion_count(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  struct timespec timeout = test_deadline_after_ms(TEST_TIMEOUT_MS);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  while (context->call_count < expected_call_count) {
    int wait_result = pthread_cond_timedwait(&context->cond, &context->mutex, &timeout);

    ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
    ASSERT_TRUE(wait_result != ETIMEDOUT);
  }
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_assert_completion(
  const TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(context->call_count == 1u);
  ASSERT_TRUE(context->result == (int)result);
  ASSERT_TRUE(context->callback_thread_set);
}

static unsigned int test_completion_order(const TEST_COMPLETION_CONTEXT *context) {
  return context->order;
}

static void test_assert_completion_on_current_thread(
  const TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  test_assert_completion(context, result);
  ASSERT_TRUE(pthread_equal(context->callback_thread, pthread_self()));
}

// For testing purpose.
extern bool bounce_dangerous_unsafe_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth);

static void test_poll_park_once_until_completion(
  BOUNCE_CORE *bounce,
  TEST_COMPLETION_CONTEXT *completion,
  unsigned int max_inline_depth,
  unsigned int expected_call_count) {
  double started_ms = test_monotonic_now_ms();

  while (test_completion_call_count(completion) < expected_call_count) {
    ASSERT_TRUE(bounce_dangerous_unsafe_park_once(bounce, max_inline_depth));
    ASSERT_TRUE((test_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
    if (test_completion_call_count(completion) < expected_call_count) {
      test_yield_park_once_poll();
    }
  }
}

static void test_park_context_init(
  TEST_PARK_THREAD_CONTEXT *context,
  BOUNCE_CORE *bounce) {
  memset(context, 0, sizeof *context);
  context->bounce = bounce;
  ASSERT_TRUE(pthread_mutex_init(&context->mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&context->cond, NULL) == 0);
}

static void test_park_context_destroy(TEST_PARK_THREAD_CONTEXT *context) {
  ASSERT_TRUE(pthread_cond_destroy(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&context->mutex) == 0);
}

static void *test_park_thread(void *parameter) {
  TEST_PARK_THREAD_CONTEXT *context = parameter;
  BOUNCE_CORE *previous_core = bounce_get_core();

  bounce_set_core(context->bounce);
  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->started = true;
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);

  context->park_result = bounce_park(context->bounce, context->max_inline_depth);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->finished = true;
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);

  bounce_set_core(previous_core);
  return NULL;
}

static void test_nested_post_inner_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  test_completion_callback(result, completion_state);
}

static void test_nested_post_outer_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_POST_CONTEXT *context = completion_state;

  ASSERT_TRUE(bounce_post(context->bounce, test_nested_post_inner_completion, &context->nested));
  test_completion_callback(result, &context->outer);
}

static void test_start_parker(
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth) {
  struct timespec timeout = test_deadline_after_ms(TEST_TIMEOUT_MS);

  context->max_inline_depth = max_inline_depth;
  ASSERT_TRUE(pthread_create(&context->thread, NULL, test_park_thread, context) == 0);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  while (!context->started) {
    int wait_result = pthread_cond_timedwait(&context->cond, &context->mutex, &timeout);

    ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
    ASSERT_TRUE(wait_result != ETIMEDOUT);
  }
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_stop_parker(TEST_PARK_THREAD_CONTEXT *context) {
  bounce_shutdown(context->bounce, false);
  ASSERT_TRUE(pthread_join(context->thread, NULL) == 0);
  ASSERT_TRUE(context->finished);
  ASSERT_TRUE(context->park_result);
}

static void test_tls_current_core_roundtrip(void) {
  BOUNCE_CORE bounce;

  bounce_init(&bounce);
  bounce_set_fallback_core(NULL);
  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);
  bounce_set_core(&bounce);
  ASSERT_TRUE(bounce_get_core() == &bounce);
  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);
  bounce_deinit(&bounce);
}

static void test_tls_current_core_uses_fallback_when_unattached(void) {
  BOUNCE_CORE bounce;
  BOUNCE_CORE other;

  bounce_init(&bounce);
  bounce_init(&other);
  bounce_set_core(NULL);
  bounce_set_fallback_core(&bounce);
  ASSERT_TRUE(bounce_get_core() == &bounce);
  bounce_set_core(&other);
  ASSERT_TRUE(bounce_get_core() == &other);
  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == &bounce);
  bounce_set_fallback_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);
  bounce_deinit(&other);
  bounce_deinit(&bounce);
}

static void test_tls_current_core_visible_on_attached_parker(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion));
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.observed_bounce == &bounce);

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_deinit(&bounce);
}

static void test_single_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion));
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(pthread_equal(completion.callback_thread, park_context.thread));

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_deinit(&bounce);
}

static void test_park_once_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  test_completion_context_init(&completion);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 0u));
  test_assert_completion_on_current_thread(
    &completion,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_destroy(&completion);
  bounce_deinit(&bounce);
}

static void test_park_once_returns_before_timeout_completion(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_COMPLETION_CONTEXT completion;
  double started_ms;
  double first_return_ms;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_completion_context_init(&completion);

  ASSERT_TRUE(
    bounce_await_timeout(
      &bounce,
      &timer,
      200u,
      test_completion_callback,
      &completion,
      NULL));
  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 0u));
  first_return_ms = test_monotonic_now_ms();

  ASSERT_TRUE((first_return_ms - started_ms) < 100.0);
  ASSERT_TRUE(test_completion_call_count(&completion) == 0u);

  test_poll_park_once_until_completion(&bounce, &completion, 0u, 1u);
  test_assert_completion_on_current_thread(
    &completion,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_destroy(&completion);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_park_once_nested_post_inlines(void) {
  BOUNCE_CORE bounce;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_completion_context_init_ordered(&context.outer, &context.next_order);
  test_completion_context_init_ordered(&context.nested, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 2u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&context.nested) == 1u);
  ASSERT_TRUE(test_completion_order(&context.outer) == 2u);

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  bounce_deinit(&bounce);
}

static void test_park_once_nested_post_falls_back_at_depth_limit(void) {
  BOUNCE_CORE bounce;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_completion_context_init_ordered(&context.outer, &context.next_order);
  test_completion_context_init_ordered(&context.nested, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 1u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&context.outer) == 1u);
  ASSERT_TRUE(test_completion_order(&context.nested) == 2u);

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  bounce_deinit(&bounce);
}

static void test_single_timeout_runs(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(
    bounce_await_timeout(
      &bounce,
      &timer,
      20u,
      test_completion_callback,
      &completion,
      NULL));
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  BOUNCE_CANCELLATION cancellation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(
    bounce_await_timeout(
      &bounce,
      &timer,
      1000u,
      test_completion_callback,
      &completion,
      &cancellation));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_CANCELED);

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_cancellation_deinit(&cancellation);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_registration_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(
    bounce_register_canceled(
      &bounce,
      &cancellation,
      &registration,
      test_completion_callback,
      &completion));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_CANCELED);

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_second_parker_rejected(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  test_completion_context_init(&completion);
  test_park_context_init(&park_context, &bounce);
  test_start_parker(&park_context, 0u);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion));
  test_wait_completion_count(&completion, 1u);
  test_assert_completion(&completion, BOUNCE_COMPLETION_COMPLETED);

  ASSERT_TRUE(!bounce_dangerous_unsafe_park_once(&bounce, 0u));

  test_stop_parker(&park_context);
  test_park_context_destroy(&park_context);
  test_completion_context_destroy(&completion);
  bounce_deinit(&bounce);
}

static void test_stress_context_init(TEST_STRESS_CONTEXT *context) {
  memset(context, 0, sizeof *context);
  ASSERT_TRUE(pthread_mutex_init(&context->mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&context->cond, NULL) == 0);
}

static void test_stress_context_destroy(TEST_STRESS_CONTEXT *context) {
  ASSERT_TRUE(pthread_cond_destroy(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&context->mutex) == 0);
}

static void test_stress_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_STRESS_CONTEXT *context = completion_state;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->completed_count += 1u;
  if (!context->callback_thread_set) {
    context->callback_thread = pthread_self();
    context->callback_thread_set = true;
  } else {
    ASSERT_TRUE(pthread_equal(context->callback_thread, pthread_self()));
  }
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void *test_stress_producer_thread(void *parameter) {
  TEST_STRESS_PRODUCER_CONTEXT *context = parameter;

  for (unsigned int index = 0u; index < context->remaining_posts; index++) {
    ASSERT_TRUE(bounce_post(context->bounce, test_stress_completion, context->stress));
  }

  return NULL;
}

static void test_wait_stress_count(
  TEST_STRESS_CONTEXT *context,
  unsigned int expected_count) {
  struct timespec timeout = test_deadline_after_ms(TEST_TIMEOUT_MS);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  while (context->completed_count < expected_count) {
    int wait_result = pthread_cond_timedwait(&context->cond, &context->mutex, &timeout);

    ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
    ASSERT_TRUE(wait_result != ETIMEDOUT);
  }
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_multi_producer_post_stress(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_STRESS_CONTEXT stress;
  TEST_STRESS_PRODUCER_CONTEXT producers[TEST_STRESS_PRODUCER_COUNT];
  pthread_t threads[TEST_STRESS_PRODUCER_COUNT];
  const unsigned int total_count =
    TEST_STRESS_PRODUCER_COUNT * TEST_STRESS_POSTS_PER_PRODUCER;

  bounce_init(&bounce);
  test_park_context_init(&park_context, &bounce);
  test_stress_context_init(&stress);
  test_start_parker(&park_context, 0u);

  for (unsigned int index = 0u; index < TEST_STRESS_PRODUCER_COUNT; index++) {
    producers[index].bounce = &bounce;
    producers[index].stress = &stress;
    producers[index].remaining_posts = TEST_STRESS_POSTS_PER_PRODUCER;
    ASSERT_TRUE(
      pthread_create(
        &threads[index],
        NULL,
        test_stress_producer_thread,
        &producers[index]) == 0);
  }

  for (unsigned int index = 0u; index < TEST_STRESS_PRODUCER_COUNT; index++) {
    ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
  }

  test_wait_stress_count(&stress, total_count);
  ASSERT_TRUE(stress.callback_thread_set);
  ASSERT_TRUE(pthread_equal(stress.callback_thread, park_context.thread));

  test_stop_parker(&park_context);
  test_stress_context_destroy(&stress);
  test_park_context_destroy(&park_context);
  bounce_deinit(&bounce);
}

int main(void) {
  TEST_CASE cases[64];
  size_t case_count = 0u;
  int result;

#define TEST_APPEND_CASE(function_name)                                          \
  do {                                                                           \
    cases[case_count] = (TEST_CASE)TEST_CASE_ENTRY(function_name);               \
    case_count += 1u;                                                            \
  } while (0)

  TEST_APPEND_CASE(test_tls_current_core_roundtrip);
  TEST_APPEND_CASE(test_tls_current_core_uses_fallback_when_unattached);
  TEST_APPEND_CASE(test_tls_current_core_visible_on_attached_parker);
  TEST_APPEND_CASE(test_single_post_runs);
  TEST_APPEND_CASE(test_park_once_post_runs);
  TEST_APPEND_CASE(test_park_once_returns_before_timeout_completion);
  TEST_APPEND_CASE(test_park_once_nested_post_inlines);
  TEST_APPEND_CASE(test_park_once_nested_post_falls_back_at_depth_limit);
  TEST_APPEND_CASE(test_single_timeout_runs);
  TEST_APPEND_CASE(test_timeout_cancel_completes_canceled);
  TEST_APPEND_CASE(test_registration_completes_canceled);
  TEST_APPEND_CASE(test_second_parker_rejected);
  TEST_APPEND_CASE(test_multi_producer_post_stress);
  TEST_APPEND_CASE(test_cpp_wrapper_post_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_post_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_post_aborts_on_deinit);
  TEST_APPEND_CASE(test_cpp_wrapper_set_default_timeout_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_set_default_overrides_fallback_view);
  TEST_APPEND_CASE(test_cpp_wrapper_nested_post_inlines_with_park_ex);
  TEST_APPEND_CASE(test_cpp_wrapper_current_post_runs_on_defaulted_parker);
  TEST_APPEND_CASE(test_cpp_wrapper_nested_post_falls_back_at_depth_limit);
  TEST_APPEND_CASE(test_cpp_wrapper_timer_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_timer_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_timer_await_aborts_on_deinit);
  TEST_APPEND_CASE(test_cpp_wrapper_cancellation_cancel_timeout_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_cancellation_precanceled_timeout_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_lambda_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_unregister_prevents_callback);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_precanceled_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_shutdown_wait_for_idle_keeps_pending_registration_alive);
#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)
  TEST_APPEND_CASE(test_cpp_promise_resume_on_runs);
  TEST_APPEND_CASE(test_cpp_promise_make_awaitable_runs);
  TEST_APPEND_CASE(test_cpp_promise_make_awaitable_start_failed);
  TEST_APPEND_CASE(test_cpp_promise_make_awaitable_aborted);
  TEST_APPEND_CASE(test_cpp_promise_make_callback_promise_runs);
  TEST_APPEND_CASE(test_cpp_promise_make_callback_promise_void_runs);
  TEST_APPEND_CASE(test_cpp_promise_make_callback_promise_start_failed);
  TEST_APPEND_CASE(test_cpp_promise_nested_value_runs);
  TEST_APPEND_CASE(test_cpp_promise_exception_propagates);
  TEST_APPEND_CASE(test_cpp_promise_await_canceled_runs);
  TEST_APPEND_CASE(test_cpp_promise_fire_and_forget_void_runs);
  TEST_APPEND_CASE(test_cpp_promise_fire_and_forget_value_runs);
  TEST_APPEND_CASE(test_cpp_promise_fire_and_forget_empty_fails);
#endif

#undef TEST_APPEND_CASE

  bounce_generic_set_timeout_sink(
    test_generic_create_timeout,
    test_generic_start_timeout,
    test_generic_cancel_timeout);
  result = test_run_suite("generic", cases, case_count);
  bounce_generic_set_timeout_sink(NULL, NULL, NULL);
  return result;
}
