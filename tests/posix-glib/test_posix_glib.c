/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <errno.h>
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
extern void test_cpp_wrapper_park_once_post_runs(void);
extern void test_cpp_wrapper_attach_current_timeout_await_runs(void);
extern void test_cpp_wrapper_attach_current_restores_fallback_view(void);
extern void test_cpp_wrapper_park_once_returns_before_timeout_completion(void);
extern void test_cpp_wrapper_park_once_nested_post_inlines(void);
extern void test_cpp_wrapper_park_once_nested_post_falls_back_at_depth_limit(void);
extern void test_cpp_wrapper_current_post_runs_on_attached_parker(void);
extern void test_cpp_wrapper_fd_await_runs(void);
extern void test_cpp_wrapper_lambda_fd_await_runs(void);
extern void test_cpp_wrapper_lambda_fd_await_aborts_on_deinit(void);
extern void test_posix_glib_gtk3_example_button_click_writes_sample_file(void);

#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)
extern void test_cpp_promise_resume_on_runs(void);
extern void test_cpp_promise_make_awaitable_runs(void);
extern void test_cpp_promise_make_awaitable_start_failed(void);
extern void test_cpp_promise_make_awaitable_aborted(void);
extern void test_cpp_promise_nested_value_runs(void);
extern void test_cpp_promise_exception_propagates(void);
extern void test_cpp_promise_await_canceled_runs(void);
extern void test_cpp_promise_fd_await_runs(void);
#endif

#define TEST_TIMEOUT_MS 5000u
#define TEST_NO_COMPLETION_TIMEOUT_MS 200u
#define TEST_INLINE_BENCH_ITERATIONS 100000u

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
  int result;
  unsigned int order;
  pthread_t callback_thread;
  bool callback_thread_set;
  volatile unsigned int *next_order;
} TEST_COMPLETION_CONTEXT;

typedef struct TEST_TIMER_DEINIT_CONTEXT {
  BOUNCE_TIMER *timer;
  TEST_COMPLETION_CONTEXT completion;
} TEST_TIMER_DEINIT_CONTEXT;

typedef struct TEST_CURRENT_CORE_CONTEXT {
  BOUNCE_CORE *expected_bounce;
  BOUNCE_CORE *observed_bounce;
  TEST_COMPLETION_CONTEXT completion;
} TEST_CURRENT_CORE_CONTEXT;

typedef struct TEST_INLINE_BENCH_CONTEXT {
  BOUNCE_CORE *bounce;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  volatile unsigned int remaining;
  bool done;
} TEST_INLINE_BENCH_CONTEXT;

typedef struct TEST_INLINE_POST_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
} TEST_INLINE_POST_CONTEXT;

typedef struct TEST_EXTERNAL_CONTEXT_STATE {
  BOUNCE_CORE *bounce;
  bool fired;
} TEST_EXTERNAL_CONTEXT_STATE;

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

static void test_completion_callback(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_COMPLETION_CONTEXT *context = completion_state;

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->call_count += 1u;
  context->result = (int)result;
  context->callback_thread = pthread_self();
  context->callback_thread_set = true;
  if (context->next_order != NULL) {
    context->order = __atomic_add_fetch(context->next_order, 1u, __ATOMIC_RELAXED);
  }
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_timeout_deinit_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_TIMER_DEINIT_CONTEXT *context = completion_state;

  bounce_timer_deinit(context->timer);
  test_completion_callback(result, &context->completion);
}

static void test_current_core_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_CURRENT_CORE_CONTEXT *context = completion_state;

  context->observed_bounce = bounce_get_core();
  test_completion_callback(result, &context->completion);
}

static void test_completion_context_init(
  TEST_COMPLETION_CONTEXT *context,
  volatile unsigned int *next_order) {
  memset(context, 0, sizeof *context);
  ASSERT_TRUE(pthread_mutex_init(&context->mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&context->cond, NULL) == 0);
  context->result = -1;
  context->next_order = next_order;
}

static void test_completion_context_destroy(TEST_COMPLETION_CONTEXT *context) {
  ASSERT_TRUE(pthread_cond_destroy(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&context->mutex) == 0);
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

static void test_wait_no_additional_completion(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  struct timespec timeout = test_deadline_after_ms(TEST_NO_COMPLETION_TIMEOUT_MS);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  while (context->call_count == expected_call_count) {
    int wait_result = pthread_cond_timedwait(&context->cond, &context->mutex, &timeout);

    ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
    if (wait_result == ETIMEDOUT) {
      break;
    }
  }
  ASSERT_TRUE(context->call_count == expected_call_count);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
}

static void test_assert_completion_on_current_thread(
  const TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(context->call_count == 1u);
  ASSERT_TRUE(context->result == (int)result);
  ASSERT_TRUE(context->callback_thread_set);
  ASSERT_TRUE(pthread_equal(context->callback_thread, pthread_self()));
}

static void test_poll_park_once_until_completion(
  BOUNCE_CORE *bounce,
  TEST_COMPLETION_CONTEXT *context,
  unsigned int max_inline_depth,
  unsigned int expected_call_count) {
  const double started_ms = test_monotonic_now_ms();

  while (context->call_count < expected_call_count) {
    ASSERT_TRUE(bounce_park_once(bounce, max_inline_depth));
    ASSERT_TRUE((test_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
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
  bounce_set_core(previous_core);

  ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
  context->finished = true;
  ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
  ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
  return NULL;
}

static void test_start_parker_with_inline_depth(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth);

static void test_start_parker(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context) {
  test_start_parker_with_inline_depth(bounce, context, 0u);
}

static void test_start_parker_with_inline_depth(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth) {
  struct timespec timeout = test_deadline_after_ms(TEST_TIMEOUT_MS);

  test_park_context_init(context, bounce);
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

static void test_stop_parker(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context) {
  bounce_shutdown(bounce, false);
  ASSERT_TRUE(pthread_join(context->thread, NULL) == 0);
  ASSERT_TRUE(context->park_result);
  test_park_context_destroy(context);
}

static void test_tls_current_core_roundtrip(void) {
  BOUNCE_CORE bounce;
  BOUNCE_CORE *previous_core = bounce_get_core();

  bounce_init(&bounce);

  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);
  bounce_set_core(&bounce);
  ASSERT_TRUE(bounce_get_core() == &bounce);

  bounce_set_core(previous_core);
  bounce_deinit(&bounce);
}

static void test_tls_current_core_uses_fallback_when_unattached(void) {
  BOUNCE_CORE bounce;
  BOUNCE_CORE fallback_bounce;
  BOUNCE_CORE *previous_core;

  bounce_set_fallback_core(NULL);
  previous_core = bounce_get_core();

  bounce_init(&bounce);
  bounce_init(&fallback_bounce);

  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);

  bounce_set_fallback_core(&fallback_bounce);
  ASSERT_TRUE(bounce_get_core() == &fallback_bounce);

  bounce_set_core(&bounce);
  ASSERT_TRUE(bounce_get_core() == &bounce);

  bounce_set_core(NULL);
  ASSERT_TRUE(bounce_get_core() == &fallback_bounce);

  bounce_set_fallback_core(NULL);
  ASSERT_TRUE(bounce_get_core() == NULL);

  bounce_set_core(previous_core);
  bounce_deinit(&fallback_bounce);
  bounce_deinit(&bounce);
}

static void test_tls_current_core_visible_on_attached_parker(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_CURRENT_CORE_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.expected_bounce = &bounce;
  test_completion_context_init(&context.completion, NULL);
  test_start_parker(&bounce, &park_context);

  ASSERT_TRUE(bounce_post(&bounce, test_current_core_completion, &context));
  test_wait_completion_count(&context.completion, 1u);

  ASSERT_TRUE(context.completion.call_count == 1u);
  ASSERT_TRUE(context.completion.result == (int)BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.observed_bounce == context.expected_bounce);
  ASSERT_TRUE(context.completion.callback_thread_set);
  ASSERT_TRUE(pthread_equal(context.completion.callback_thread, park_context.thread));

  test_completion_context_destroy(&context.completion);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_inline_benchmark_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_BENCH_CONTEXT *context = completion_state;
  unsigned int remaining;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  remaining = __atomic_sub_fetch(&context->remaining, 1u, __ATOMIC_ACQ_REL);
  if (remaining == 0u) {
    ASSERT_TRUE(pthread_mutex_lock(&context->mutex) == 0);
    context->done = true;
    ASSERT_TRUE(pthread_cond_broadcast(&context->cond) == 0);
    ASSERT_TRUE(pthread_mutex_unlock(&context->mutex) == 0);
    return;
  }

  ASSERT_TRUE(bounce_post(context->bounce, test_inline_benchmark_completion, context));
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

static void test_timeout_nested_post_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_POST_CONTEXT *context = completion_state;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_post(context->bounce, test_nested_post_inner_completion, &context->nested));
  test_completion_callback(result, &context->outer);
}

static gboolean test_external_context_source_fired(gpointer parameter) {
  TEST_EXTERNAL_CONTEXT_STATE *state = parameter;

  state->fired = true;
  bounce_shutdown(state->bounce, false);
  return G_SOURCE_REMOVE;
}

static double test_run_inline_depth_benchmark(unsigned int max_inline_depth) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_BENCH_CONTEXT context;
  struct timespec timeout = test_deadline_after_ms(TEST_TIMEOUT_MS);
  double started_ms;
  double finished_ms;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  context.remaining = TEST_INLINE_BENCH_ITERATIONS;
  ASSERT_TRUE(pthread_mutex_init(&context.mutex, NULL) == 0);
  ASSERT_TRUE(pthread_cond_init(&context.cond, NULL) == 0);
  test_start_parker_with_inline_depth(&bounce, &park_context, max_inline_depth);

  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_post(&bounce, test_inline_benchmark_completion, &context));
  ASSERT_TRUE(pthread_mutex_lock(&context.mutex) == 0);
  while (!context.done) {
    int wait_result = pthread_cond_timedwait(&context.cond, &context.mutex, &timeout);

    ASSERT_TRUE((wait_result == 0) || (wait_result == ETIMEDOUT));
    ASSERT_TRUE(wait_result != ETIMEDOUT);
  }
  ASSERT_TRUE(pthread_mutex_unlock(&context.mutex) == 0);
  finished_ms = test_monotonic_now_ms();

  test_stop_parker(&bounce, &park_context);
  ASSERT_TRUE(pthread_cond_destroy(&context.cond) == 0);
  ASSERT_TRUE(pthread_mutex_destroy(&context.mutex) == 0);
  bounce_deinit(&bounce);
  return finished_ms - started_ms;
}

static void test_shutdown_stops_parker(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;

  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_init_with_main_context_uses_provided_context(void) {
  BOUNCE_CORE bounce;
  GMainContext *main_context = g_main_context_new();
  GSource *source;
  TEST_EXTERNAL_CONTEXT_STATE state;

  ASSERT_TRUE(main_context != NULL);
  memset(&state, 0, sizeof state);
  state.bounce = &bounce;

  bounce_init_with_main_context(&bounce, main_context);
  ASSERT_TRUE(bounce.main_context == main_context);

  source = g_idle_source_new();
  ASSERT_TRUE(source != NULL);
  g_source_set_callback(source, test_external_context_source_fired, &state, NULL);
  ASSERT_TRUE(g_source_attach(source, main_context) != 0u);
  g_source_unref(source);

  ASSERT_TRUE(bounce_park(&bounce, 0u));
  ASSERT_TRUE(state.fired);

  bounce_deinit(&bounce);
  g_main_context_unref(main_context);
}

static void test_single_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, NULL);
  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion_context));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_park_once_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion_context));
  ASSERT_TRUE(bounce_park_once(&bounce, 0u));
  test_assert_completion_on_current_thread(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_destroy(&completion_context);
  bounce_deinit(&bounce);
}

static void test_park_once_returns_before_timeout_completion(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_COMPLETION_CONTEXT completion_context;
  double started_ms;
  double first_return_ms;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    200u,
    test_completion_callback,
    &completion_context,
    NULL));
  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_park_once(&bounce, 0u));
  first_return_ms = test_monotonic_now_ms();

  ASSERT_TRUE((first_return_ms - started_ms) < 100.0);
  ASSERT_TRUE(completion_context.call_count == 0u);

  test_poll_park_once_until_completion(&bounce, &completion_context, 0u, 1u);
  test_assert_completion_on_current_thread(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_destroy(&completion_context);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_park_once_nested_post_inlines(void) {
  BOUNCE_CORE bounce;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_completion_context_init(&context.outer, &context.next_order);
  test_completion_context_init(&context.nested, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_park_once(&bounce, 2u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.nested.order == 1u);
  ASSERT_TRUE(context.outer.order == 2u);

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
  test_completion_context_init(&context.outer, &context.next_order);
  test_completion_context_init(&context.nested, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_park_once(&bounce, 1u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.outer.order == 1u);
  ASSERT_TRUE(context.nested.order == 2u);

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  bounce_deinit(&bounce);
}

static void test_single_fd_read_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, NULL);
  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    NULL);
  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_post_then_fd_order(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT post_context;
  TEST_COMPLETION_CONTEXT fd_context;
  volatile unsigned int next_order = 0u;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&post_context, &next_order);
  test_completion_context_init(&fd_context, &next_order);

  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &fd_context,
    NULL);
  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &post_context));
  test_wait_completion_count(&post_context, 1u);
  ASSERT_TRUE(fd_context.call_count == 0u);

  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_completion_count(&fd_context, 1u);

  ASSERT_TRUE(post_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(fd_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(post_context.order == 1u);
  ASSERT_TRUE(fd_context.order == 2u);

  test_completion_context_destroy(&post_context);
  test_completion_context_destroy(&fd_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_nested_post_inline_depth_benchmark(void) {
  static const unsigned int depths[] = { 0u, 1u, 2u, 4u, 8u, 16u };
  double baseline_ms = 0.0;

  printf("inline benchmark iterations=%u\n", TEST_INLINE_BENCH_ITERATIONS);
  for (size_t index = 0u; index < (sizeof depths / sizeof depths[0]); index++) {
    double duration_ms = test_run_inline_depth_benchmark(depths[index]);
    double speedup = 1.0;

    if (index == 0u) {
      baseline_ms = duration_ms;
    } else if (duration_ms > 0.0) {
      speedup = baseline_ms / duration_ms;
    }

    printf(
      "inline benchmark max_inline_depth=%u duration_ms=%.3f speedup_vs_depth0=%.3fx\n",
      depths[index],
      duration_ms,
      speedup);
  }
}

static void test_single_timeout_runs(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_completion_nested_post_inlines(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  bounce_timer_init(&timer);
  context.bounce = &bounce;
  test_completion_context_init(&context.outer, &context.next_order);
  test_completion_context_init(&context.nested, &context.next_order);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1u,
    test_timeout_nested_post_completion,
    &context,
    NULL));
  test_poll_park_once_until_completion(&bounce, &context.outer, 2u, 1u);

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.nested.order == 1u);
  ASSERT_TRUE(context.outer.order == 2u);

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_can_rearm_after_completion(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_contexts[2];

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_contexts[0], NULL);
  test_completion_context_init(&completion_contexts[1], NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_completion_callback,
    &completion_contexts[0],
    NULL));
  test_wait_completion_count(&completion_contexts[0], 1u);

  ASSERT_TRUE(completion_contexts[0].call_count == 1u);
  ASSERT_TRUE(completion_contexts[0].result == BOUNCE_COMPLETION_COMPLETED);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_completion_callback,
    &completion_contexts[1],
    NULL));
  test_wait_completion_count(&completion_contexts[1], 1u);

  ASSERT_TRUE(completion_contexts[1].call_count == 1u);
  ASSERT_TRUE(completion_contexts[1].result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_contexts[0].callback_thread_set);
  ASSERT_TRUE(completion_contexts[1].callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_contexts[0].callback_thread, park_context.thread));
  ASSERT_TRUE(pthread_equal(completion_contexts[1].callback_thread, park_context.thread));

  test_completion_context_destroy(&completion_contexts[1]);
  test_completion_context_destroy(&completion_contexts[0]);
  test_stop_parker(&bounce, &park_context);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_rejects_second_active_wait(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_COMPLETION_CONTEXT rejected_completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);
  test_completion_context_init(&rejected_completion_context, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1000u,
    test_completion_callback,
    &completion_context,
    &cancellation));
  ASSERT_TRUE(!bounce_await_timeout(
    &bounce,
    &timer,
    10u,
    test_completion_callback,
    &rejected_completion_context,
    NULL));

  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);
  test_wait_no_additional_completion(&rejected_completion_context, 0u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(rejected_completion_context.call_count == 0u);

  test_completion_context_destroy(&rejected_completion_context);
  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1000u,
    test_completion_callback,
    &completion_context,
    &cancellation));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_completion_can_deinit_timer(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_TIMER_DEINIT_CONTEXT completion_context;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_start_parker(&bounce, &park_context);
  completion_context.timer = &timer;
  test_completion_context_init(&completion_context.completion, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_timeout_deinit_completion,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context.completion, 1u);

  ASSERT_TRUE(completion_context.completion.call_count == 1u);
  ASSERT_TRUE(completion_context.completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_context.completion.callback_thread_set);
  ASSERT_TRUE(pthread_equal(
    completion_context.completion.callback_thread,
    park_context.thread));
  ASSERT_TRUE(!bounce_await_timeout(
    &bounce,
    &timer,
    10u,
    test_completion_callback,
    &completion_context.completion,
    NULL));

  test_completion_context_destroy(&completion_context.completion);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_park_once_fd_read_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  test_completion_context_init(&completion_context, NULL);

  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    NULL);
  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  ASSERT_TRUE(bounce_park_once(&bounce, 0u));

  test_assert_completion_on_current_thread(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_destroy(&completion_context);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_fd_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_register_canceled_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_register_canceled_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_unregister_canceled_prevents_callback(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  ASSERT_TRUE(bounce_unregister_canceled(&registration));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));
  bounce_cancel(&bounce, &cancellation);
  test_wait_no_additional_completion(&completion_context, 0u);

  ASSERT_TRUE(completion_context.call_count == 0u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_registration_deinit_aborts_pending_callback(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion_context, NULL);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  bounce_deinit(&bounce);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_ABORTED);
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  test_completion_context_destroy(&completion_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
}

static void test_fd_await_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  bounce_cancel(&bounce, &cancellation);
  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_fd_completion_wins_over_later_cancel(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  bounce_cancel(&bounce, &cancellation);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_shared_cancellation_cancels_all_fd_waits(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completions[4];
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[4][2];
  unsigned char byte = 1u;

  memset(&pipe_fds[0][0], 0, sizeof pipe_fds);
  for (unsigned int index = 0; index < 4u; index++) {
    ASSERT_TRUE(pipe(&pipe_fds[index][0]) == 0);
  }

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  for (unsigned int index = 0; index < 4u; index++) {
    test_completion_context_init(&completions[index], NULL);
    bounce_await_posix_glib_fd(
      &bounce,
      pipe_fds[index][0],
      G_IO_IN,
      test_completion_callback,
      &completions[index],
      &cancellation);
  }

  bounce_cancel(&bounce, &cancellation);

  for (unsigned int index = 0; index < 4u; index++) {
    test_wait_completion_count(&completions[index], 1u);
    ASSERT_TRUE(completions[index].call_count == 1u);
    ASSERT_TRUE(completions[index].result == BOUNCE_COMPLETION_CANCELED);
    ASSERT_TRUE(completions[index].callback_thread_set);
    ASSERT_TRUE(pthread_equal(completions[index].callback_thread, park_context.thread));
  }

  for (unsigned int index = 0; index < 4u; index++) {
    ASSERT_TRUE(write(pipe_fds[index][1], &byte, sizeof byte) == (ssize_t)sizeof byte);
    test_wait_no_additional_completion(&completions[index], 1u);
  }

  for (unsigned int index = 0; index < 4u; index++) {
    test_completion_context_destroy(&completions[index]);
  }
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  for (unsigned int index = 0; index < 4u; index++) {
    ASSERT_TRUE(close(pipe_fds[index][0]) == 0);
    ASSERT_TRUE(close(pipe_fds[index][1]) == 0);
  }
}

static void test_repeated_cancel_is_harmless(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);
  test_completion_context_init(&completion_context, NULL);

  bounce_await_posix_glib_fd(
    &bounce,
    pipe_fds[0],
    G_IO_IN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(completion_context.call_count == 1u);
  ASSERT_TRUE(completion_context.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion_context.callback_thread_set);
  ASSERT_TRUE(pthread_equal(completion_context.callback_thread, park_context.thread));

  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

int main(void) {
  static const TEST_CASE cases[] = {
    TEST_CASE_ENTRY(test_tls_current_core_roundtrip),
    TEST_CASE_ENTRY(test_tls_current_core_uses_fallback_when_unattached),
    TEST_CASE_ENTRY(test_tls_current_core_visible_on_attached_parker),
    TEST_CASE_ENTRY(test_shutdown_stops_parker),
    TEST_CASE_ENTRY(test_init_with_main_context_uses_provided_context),
    TEST_CASE_ENTRY(test_single_post_runs),
    TEST_CASE_ENTRY(test_park_once_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_park_once_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_post_aborts_on_deinit),
    TEST_CASE_ENTRY(test_cpp_wrapper_timer_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_attach_current_timeout_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_attach_current_restores_fallback_view),
    TEST_CASE_ENTRY(test_cpp_wrapper_park_once_returns_before_timeout_completion),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_timer_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_timer_await_aborts_on_deinit),
    TEST_CASE_ENTRY(test_cpp_wrapper_cancellation_cancel_timeout_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_cancellation_precanceled_timeout_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_registration_completes_canceled),
    TEST_CASE_ENTRY(test_cpp_wrapper_registration_lambda_completes_canceled),
    TEST_CASE_ENTRY(test_cpp_wrapper_registration_unregister_prevents_callback),
    TEST_CASE_ENTRY(test_cpp_wrapper_registration_precanceled_completes_canceled),
    TEST_CASE_ENTRY(test_cpp_wrapper_shutdown_wait_for_idle_keeps_pending_registration_alive),
    TEST_CASE_ENTRY(test_cpp_wrapper_park_once_nested_post_inlines),
    TEST_CASE_ENTRY(test_cpp_wrapper_park_once_nested_post_falls_back_at_depth_limit),
    TEST_CASE_ENTRY(test_cpp_wrapper_current_post_runs_on_attached_parker),
    TEST_CASE_ENTRY(test_cpp_wrapper_fd_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_fd_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_fd_await_aborts_on_deinit),
    TEST_CASE_ENTRY(test_posix_glib_gtk3_example_button_click_writes_sample_file),
#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)
    TEST_CASE_ENTRY(test_cpp_promise_resume_on_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_start_failed),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_aborted),
    TEST_CASE_ENTRY(test_cpp_promise_nested_value_runs),
    TEST_CASE_ENTRY(test_cpp_promise_exception_propagates),
    TEST_CASE_ENTRY(test_cpp_promise_await_canceled_runs),
    TEST_CASE_ENTRY(test_cpp_promise_fd_await_runs),
#endif
    TEST_CASE_ENTRY(test_park_once_returns_before_timeout_completion),
    TEST_CASE_ENTRY(test_park_once_nested_post_inlines),
    TEST_CASE_ENTRY(test_park_once_nested_post_falls_back_at_depth_limit),
    TEST_CASE_ENTRY(test_park_once_fd_read_await_runs),
    TEST_CASE_ENTRY(test_single_fd_read_await_runs),
    TEST_CASE_ENTRY(test_post_then_fd_order),
    TEST_CASE_ENTRY(test_nested_post_inline_depth_benchmark),
    TEST_CASE_ENTRY(test_single_timeout_runs),
    TEST_CASE_ENTRY(test_timeout_completion_nested_post_inlines),
    TEST_CASE_ENTRY(test_timeout_can_rearm_after_completion),
    TEST_CASE_ENTRY(test_timeout_rejects_second_active_wait),
    TEST_CASE_ENTRY(test_timeout_cancel_completes_canceled),
    TEST_CASE_ENTRY(test_timeout_completion_can_deinit_timer),
    TEST_CASE_ENTRY(test_register_canceled_completes_canceled),
    TEST_CASE_ENTRY(test_register_canceled_with_already_canceled_source_completes_canceled),
    TEST_CASE_ENTRY(test_unregister_canceled_prevents_callback),
    TEST_CASE_ENTRY(test_registration_deinit_aborts_pending_callback),
    TEST_CASE_ENTRY(test_fd_await_cancel_completes_canceled),
    TEST_CASE_ENTRY(test_fd_await_with_already_canceled_source_completes_canceled),
    TEST_CASE_ENTRY(test_fd_completion_wins_over_later_cancel),
    TEST_CASE_ENTRY(test_shared_cancellation_cancels_all_fd_waits),
    TEST_CASE_ENTRY(test_repeated_cancel_is_harmless)
  };

  return test_run_suite("posix_glib", cases, sizeof cases / sizeof cases[0]);
}
