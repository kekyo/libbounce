/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#if defined(BOUNCE_POSIX)
#include <sys/socket.h>
#endif
#include <time.h>
#include <unistd.h>

#include "test_harness.h"
#include "shared/test_posix_freertos_runtime.h"
#include "shared/test_posix_freertos_shared.h"
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
extern void test_cpp_wrapper_condition_await_runs(void);
extern void test_cpp_wrapper_lambda_condition_await_runs(void);
extern void test_cpp_wrapper_lambda_condition_await_aborts_on_deinit(void);
#if defined(BOUNCE_POSIX)
extern void test_cpp_wrapper_fd_await_runs(void);
extern void test_cpp_wrapper_lambda_fd_await_runs(void);
extern void test_cpp_wrapper_lambda_fd_await_aborts_on_deinit(void);
extern void test_cpp_wrapper_file_io_runs(void);
extern void test_cpp_wrapper_socket_io_runs(void);
#endif

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
extern void test_cpp_promise_condition_await_runs(void);
#if defined(BOUNCE_POSIX)
extern void test_cpp_promise_fd_await_runs(void);
extern void test_cpp_promise_fd_write_all_bytes_awaits_before_each_write_runs(void);
extern void test_cpp_promise_file_io_async_runs(void);
extern void test_cpp_promise_socket_io_async_runs(void);
#if defined(__linux__)
extern void test_cpp_io_uring_operation_init_accessors(void);
extern void test_cpp_promise_io_uring_await_runs(void);
extern void test_cpp_promise_io_uring_await_canceled_runs(void);
#endif
#endif
#endif

#define TEST_TIMEOUT_MS 5000u
#define TEST_NO_COMPLETION_TIMEOUT_MS 200u
#define TEST_STRESS_TIMEOUT_MS 60000u
#define TEST_STRESS_PARKER_COUNT 4u
#define TEST_STRESS_REQUEST_COUNT 10000u
#define TEST_STRESS_MAX_ACTIVE_WAITS 32u
#define TEST_TIMEOUT_REARM_REPEAT_COUNT 32u
#define TEST_INLINE_BENCH_ITERATIONS 100000u
#if defined(MSG_DONTWAIT)
#define TEST_SOCKET_DONTWAIT_FLAG MSG_DONTWAIT
#else
#define TEST_SOCKET_DONTWAIT_FLAG 0
#endif

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      abort();                                                                   \
    }                                                                            \
  } while (0)

typedef TEST_POSIX_FREERTOS_RUNTIME_PARKER TEST_PARK_THREAD_CONTEXT;
typedef TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT TEST_COMPLETION_CONTEXT;

typedef struct TEST_TIMER_DEINIT_CONTEXT {
  BOUNCE_TIMER *timer;
  TEST_COMPLETION_CONTEXT completion;
} TEST_TIMER_DEINIT_CONTEXT;

typedef struct TEST_CURRENT_CORE_CONTEXT {
  BOUNCE_CORE *expected_bounce;
  BOUNCE_CORE *observed_bounce;
  TEST_COMPLETION_CONTEXT completion;
} TEST_CURRENT_CORE_CONTEXT;

#if defined(BOUNCE_FREERTOS)
typedef BOUNCE_FREERTOS_CONDITION TEST_BACKEND_CONDITION;
#else
typedef BOUNCE_POSIX_CONDITION TEST_BACKEND_CONDITION;
#endif

typedef struct TEST_INLINE_POST_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
} TEST_INLINE_POST_CONTEXT;

typedef struct TEST_INLINE_AWAIT_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_BACKEND_CONDITION condition;
  BOUNCE_CANCELLATION cancellation;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
} TEST_INLINE_AWAIT_CONTEXT;

typedef struct TEST_INLINE_READY_ORDER_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT pending;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
} TEST_INLINE_READY_ORDER_CONTEXT;

typedef struct TEST_INLINE_BENCH_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL done_signal;
  volatile unsigned int remaining;
} TEST_INLINE_BENCH_CONTEXT;

struct TEST_STRESS_STATE;
typedef struct TEST_STRESS_STATE TEST_STRESS_STATE;

typedef struct TEST_STRESS_SLOT {
  TEST_STRESS_STATE *state;
  volatile unsigned int task_index;
} TEST_STRESS_SLOT;

struct TEST_STRESS_STATE {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_contexts[ TEST_STRESS_PARKER_COUNT ];
  volatile unsigned int per_thread_counts[ TEST_STRESS_PARKER_COUNT ];
  volatile unsigned char task_hits[ TEST_STRESS_REQUEST_COUNT ];
  TEST_STRESS_SLOT slots[ TEST_STRESS_MAX_ACTIVE_WAITS ];
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL batch_signal;
  volatile unsigned int batch_remaining;
  volatile unsigned int completed_count;
};

static unsigned int test_atomic_load_uint(const volatile unsigned int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void test_completion_context_init(
  TEST_COMPLETION_CONTEXT *context,
  bool waitable,
  volatile unsigned int *next_order) {
  (void)waitable;
  ASSERT_TRUE(
    test_posix_freertos_runtime_completion_context_init(
      context,
      next_order));
}

static void test_completion_context_destroy(TEST_COMPLETION_CONTEXT *context) {
  test_posix_freertos_runtime_completion_context_destroy(context);
}

static void test_completion_callback(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  test_posix_freertos_runtime_record_completion(completion_state, result);
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

static void test_wait_completion_count(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  ASSERT_TRUE(
    test_posix_freertos_runtime_wait_completion_count(
      context,
      expected_call_count,
      TEST_TIMEOUT_MS));
}

static void test_wait_no_additional_completion(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  ASSERT_TRUE(
    test_posix_freertos_runtime_wait_no_additional_completion(
      context,
      expected_call_count,
      TEST_NO_COMPLETION_TIMEOUT_MS));
}

static double test_monotonic_now_ms(void) {
  struct timespec now;

  ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return ((double)now.tv_sec * 1000.0) + ((double)now.tv_nsec / 1000000.0);
}

static void test_stress_state_init(TEST_STRESS_STATE *state) {
  ASSERT_TRUE(test_posix_freertos_runtime_signal_init(&state->batch_signal));
}

static void test_stress_state_destroy(TEST_STRESS_STATE *state) {
  test_posix_freertos_runtime_signal_destroy(&state->batch_signal);
}

static void test_wait_stress_batch(TEST_STRESS_STATE *state) {
  unsigned int checkpoint = test_posix_freertos_runtime_signal_checkpoint(&state->batch_signal);

  while (test_atomic_load_uint(&state->batch_remaining) != 0u) {
    ASSERT_TRUE(
      test_posix_freertos_runtime_signal_wait_for_change(
        &state->batch_signal,
        checkpoint,
        TEST_STRESS_TIMEOUT_MS));
    checkpoint = test_posix_freertos_runtime_signal_checkpoint(&state->batch_signal);
  }
}

static void test_start_parker(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context) {
  ASSERT_TRUE(test_posix_freertos_runtime_start_parker(bounce, context, TEST_TIMEOUT_MS));
}

static void test_start_parker_with_inline_depth(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth) {
  ASSERT_TRUE(
    test_posix_freertos_runtime_start_parker_ex(
      bounce,
      context,
      TEST_TIMEOUT_MS,
      max_inline_depth));
}

static void test_stop_parker(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context) {
  ASSERT_TRUE(test_posix_freertos_runtime_stop_parker(bounce, context, TEST_TIMEOUT_MS));
  ASSERT_TRUE(test_atomic_load_uint(&context->park_result) != 0u);
}

static void test_wait_parker_finished(TEST_PARK_THREAD_CONTEXT *context) {
  while (test_atomic_load_uint(&context->finished) == 0u) {
    ASSERT_TRUE(
      test_posix_freertos_runtime_signal_wait(
        &context->finished_signal,
        TEST_TIMEOUT_MS));
  }
}

static void test_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *contexts,
  unsigned int parker_count) {
  ASSERT_TRUE(
    test_posix_freertos_runtime_stop_parkers(
      bounce,
      contexts,
      parker_count,
      TEST_TIMEOUT_MS));
  for (unsigned int index = 0; index < parker_count; index++) {
    ASSERT_TRUE(test_atomic_load_uint(&contexts[index].park_result) != 0u);
  }
}

static unsigned int test_completion_call_count(const TEST_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_call_count(context);
}

static int test_completion_result(const TEST_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_result(context);
}

static unsigned int test_completion_order(const TEST_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_order(context);
}

static void test_assert_completion_on_current_executor(
  const TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(test_completion_call_count(context) == 1u);
  ASSERT_TRUE(test_completion_result(context) == (int)result);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_has_executor(context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_current_executor(context));
}

static void test_yield_park_once_poll(void) {
#if defined(BOUNCE_FREERTOS)
  vTaskDelay(1u);
#endif
}

// For testing purpose.
extern bool bounce_dangerous_unsafe_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth);

static void test_poll_park_once_until_completion(
  BOUNCE_CORE *bounce,
  TEST_COMPLETION_CONTEXT *context,
  unsigned int max_inline_depth,
  unsigned int expected_call_count) {
  const double started_ms = test_monotonic_now_ms();

  while (test_completion_call_count(context) < expected_call_count) {
    ASSERT_TRUE(bounce_dangerous_unsafe_park_once(bounce, max_inline_depth));
    ASSERT_TRUE((test_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
    if (test_completion_call_count(context) < expected_call_count) {
      test_yield_park_once_poll();
    }
  }
}

static unsigned int test_stress_active_wait_count(void) {
  unsigned int active_wait_count = TEST_STRESS_MAX_ACTIVE_WAITS;

  if (BOUNCE_MAX_STATIC_COMPLETION_ITEMS < active_wait_count) {
    active_wait_count = BOUNCE_MAX_STATIC_COMPLETION_ITEMS;
  }
  return active_wait_count;
}

static bool test_completion_has_executor(const TEST_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_has_executor(context);
}

static void test_backend_condition_init(TEST_BACKEND_CONDITION *condition) {
#if defined(BOUNCE_FREERTOS)
  bounce_freertos_condition_init(condition);
#else
  bounce_posix_condition_init(condition);
#endif
}

static void test_backend_condition_await(
  BOUNCE_CORE *bounce,
  TEST_BACKEND_CONDITION *condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
#if defined(BOUNCE_FREERTOS)
  bounce_await_freertos_condition(
    bounce,
    condition,
    completion,
    completion_state,
    cancellation);
#else
  bounce_await_posix_condition(
    bounce,
    condition,
    completion,
    completion_state,
    cancellation);
#endif
}

static void test_backend_condition_raise(
  BOUNCE_CORE *bounce,
  TEST_BACKEND_CONDITION *condition) {
#if defined(BOUNCE_FREERTOS)
  bounce_freertos_condition_raise(bounce, condition);
#else
  bounce_posix_condition_raise(bounce, condition);
#endif
}

static void test_stress_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_STRESS_SLOT *slot = completion_state;
  TEST_STRESS_STATE *state = slot->state;
  unsigned int task_index;
  unsigned int parker_index;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);

  task_index = __atomic_load_n(&slot->task_index, __ATOMIC_ACQUIRE);
  ASSERT_TRUE(task_index < TEST_STRESS_REQUEST_COUNT);

  parker_index = (unsigned int)test_posix_freertos_runtime_find_current_parker_index(
    &state->park_contexts[0],
    TEST_STRESS_PARKER_COUNT);
  ASSERT_TRUE(parker_index < TEST_STRESS_PARKER_COUNT);

  ASSERT_TRUE(__atomic_fetch_add(&state->task_hits[task_index], 1u, __ATOMIC_ACQ_REL) == 0u);
  __atomic_fetch_add(&state->per_thread_counts[parker_index], 1u, __ATOMIC_ACQ_REL);
  (void)__atomic_add_fetch(&state->completed_count, 1u, __ATOMIC_ACQ_REL);
  {
    const unsigned int previous_remaining =
      __atomic_fetch_sub(&state->batch_remaining, 1u, __ATOMIC_ACQ_REL);

    ASSERT_TRUE(previous_remaining > 0u);
    if (previous_remaining == 1u) {
      test_posix_freertos_runtime_signal_notify(&state->batch_signal);
    }
  }
}

static void test_stress_dump_and_assert_balance(const TEST_STRESS_STATE *state) {
  unsigned int min_count = state->per_thread_counts[0];
  unsigned int max_count = state->per_thread_counts[0];
  unsigned int threads_with_work = 0u;

  for (unsigned int index = 0; index < TEST_STRESS_PARKER_COUNT; index++) {
    const unsigned int count = state->per_thread_counts[index];

    printf("stress parker[%u] count=%u\n", index, count);
    if (count > 0u) {
      threads_with_work += 1u;
    }
    if (count < min_count) {
      min_count = count;
    }
    if (count > max_count) {
      max_count = count;
    }
  }

  printf("stress count spread=%u\n", max_count - min_count);
  ASSERT_TRUE(threads_with_work == TEST_STRESS_PARKER_COUNT);
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

static void test_nested_ready_order_outer_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_READY_ORDER_CONTEXT *context = completion_state;

  ASSERT_TRUE(bounce_post(context->bounce, test_nested_post_inner_completion, &context->nested));
  test_completion_callback(result, &context->outer);
}

static void test_nested_precanceled_condition_outer_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_AWAIT_CONTEXT *context = completion_state;

  test_backend_condition_await(
    context->bounce,
    &context->condition,
    test_completion_callback,
    &context->nested,
    &context->cancellation);
  test_completion_callback(result, &context->outer);
}

static void test_inline_benchmark_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_BENCH_CONTEXT *context = completion_state;
  unsigned int remaining;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  remaining = __atomic_sub_fetch(&context->remaining, 1u, __ATOMIC_ACQ_REL);
  if (remaining == 0u) {
    test_posix_freertos_runtime_signal_notify(&context->done_signal);
    return;
  }

  ASSERT_TRUE(bounce_post(context->bounce, test_inline_benchmark_completion, context));
}

static double test_run_inline_depth_benchmark(unsigned int max_inline_depth) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_BENCH_CONTEXT context;
  unsigned int checkpoint;
  double started_ms;
  double finished_ms;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  context.remaining = TEST_INLINE_BENCH_ITERATIONS;
  ASSERT_TRUE(test_posix_freertos_runtime_signal_init(&context.done_signal));
  test_start_parker_with_inline_depth(&bounce, &park_context, max_inline_depth);

  checkpoint = test_posix_freertos_runtime_signal_checkpoint(&context.done_signal);
  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_post(&bounce, test_inline_benchmark_completion, &context));
  ASSERT_TRUE(
    test_posix_freertos_runtime_signal_wait_for_change(
      &context.done_signal,
      checkpoint,
      TEST_STRESS_TIMEOUT_MS));
  finished_ms = test_monotonic_now_ms();

  test_stop_parker(&bounce, &park_context);
  test_posix_freertos_runtime_signal_destroy(&context.done_signal);
  bounce_deinit(&bounce);
  return finished_ms - started_ms;
}

static void test_single_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion_context));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
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
  test_completion_context_init(&context.completion, true, NULL);
  test_start_parker(&bounce, &park_context);

  ASSERT_TRUE(bounce_post(&bounce, test_current_core_completion, &context));
  test_wait_completion_count(&context.completion, 1u);

  ASSERT_TRUE(test_completion_call_count(&context.completion) == 1u);
  ASSERT_TRUE(test_completion_result(&context.completion) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.observed_bounce == context.expected_bounce);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &context.completion,
    &park_context));

  test_completion_context_destroy(&context.completion);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_park_once_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  test_completion_context_init(&completion_context, true, NULL);

  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &completion_context));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 0u));
  test_assert_completion_on_current_executor(
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
  test_completion_context_init(&completion_context, true, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    200u,
    test_completion_callback,
    &completion_context,
    NULL));
  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 0u));
  first_return_ms = test_monotonic_now_ms();

  ASSERT_TRUE((first_return_ms - started_ms) < 100.0);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 0u);

  test_poll_park_once_until_completion(&bounce, &completion_context, 0u, 1u);
  test_assert_completion_on_current_executor(
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
  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 2u));

  test_assert_completion_on_current_executor(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_executor(
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
  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_dangerous_unsafe_park_once(&bounce, 1u));

  test_assert_completion_on_current_executor(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_executor(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&context.outer) == 1u);
  ASSERT_TRUE(test_completion_order(&context.nested) == 2u);

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  bounce_deinit(&bounce);
}

static void test_nested_post_inlines_with_park_ex(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_start_parker_with_inline_depth(&bounce, &park_context, 2u);

  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);

  ASSERT_TRUE(test_completion_result(&context.outer) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_result(&context.nested) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&context.nested) == 1u);
  ASSERT_TRUE(test_completion_order(&context.outer) == 2u);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.outer, &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.nested, &park_context));

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_nested_post_falls_back_at_depth_limit(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_start_parker_with_inline_depth(&bounce, &park_context, 1u);

  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);

  ASSERT_TRUE(test_completion_result(&context.outer) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_result(&context.nested) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&context.outer) == 1u);
  ASSERT_TRUE(test_completion_order(&context.nested) == 2u);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.outer, &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.nested, &park_context));

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_nested_precanceled_condition_await_inlines(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_AWAIT_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  test_backend_condition_init(&context.condition);
  bounce_cancellation_init(&context.cancellation);
  context.bounce = &bounce;
  bounce_cancel(&bounce, &context.cancellation);
  test_start_parker_with_inline_depth(&bounce, &park_context, 2u);

  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_precanceled_condition_outer_completion, &context));
  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);

  ASSERT_TRUE(test_completion_result(&context.outer) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_result(&context.nested) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_completion_order(&context.nested) == 1u);
  ASSERT_TRUE(test_completion_order(&context.outer) == 2u);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.outer, &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.nested, &park_context));

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.outer);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&context.cancellation);
  bounce_deinit(&bounce);
}

static void test_nested_post_does_not_overtake_existing_ready_item(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_INLINE_READY_ORDER_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;

  test_completion_context_init(&context.outer, true, &context.next_order);
  test_completion_context_init(&context.pending, true, &context.next_order);
  test_completion_context_init(&context.nested, true, &context.next_order);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_ready_order_outer_completion, &context));
  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &context.pending));

  test_start_parker_with_inline_depth(&bounce, &park_context, 2u);
  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.pending, 1u);
  test_wait_completion_count(&context.nested, 1u);

  ASSERT_TRUE(test_completion_order(&context.outer) == 1u);
  ASSERT_TRUE(test_completion_order(&context.pending) == 2u);
  ASSERT_TRUE(test_completion_order(&context.nested) == 3u);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.outer, &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.pending, &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(&context.nested, &park_context));

  test_completion_context_destroy(&context.nested);
  test_completion_context_destroy(&context.pending);
  test_completion_context_destroy(&context.outer);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
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

static void test_single_condition_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    NULL);
  test_backend_condition_raise(&bounce, &condition);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_post_then_condition_order(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT post_context;
  TEST_COMPLETION_CONTEXT await_context;
  volatile unsigned int next_order = 0u;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&post_context, true, &next_order);
  test_completion_context_init(&await_context, true, &next_order);

  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &await_context,
    NULL);
  ASSERT_TRUE(bounce_post(&bounce, test_completion_callback, &post_context));
  test_wait_completion_count(&post_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&await_context) == 0u);

  test_backend_condition_raise(&bounce, &condition);
  test_wait_completion_count(&await_context, 1u);

  ASSERT_TRUE(test_completion_result(&post_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_result(&await_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_completion_order(&post_context) == 1u);
  ASSERT_TRUE(test_completion_order(&await_context) == 2u);

  test_completion_context_destroy(&post_context);
  test_completion_context_destroy(&await_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
}

static void test_condition_overflow_stays_pending_until_deinit(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_COMPLETION_CONTEXT waiting_contexts[ BOUNCE_MAX_STATIC_COMPLETION_ITEMS ];
  TEST_COMPLETION_CONTEXT overflow_context;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);

  for (unsigned int index = 0; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    test_completion_context_init(&waiting_contexts[index], false, NULL);
    test_backend_condition_await(
      &bounce,
      &condition,
      test_completion_callback,
      &waiting_contexts[index],
      NULL);
  }

  test_completion_context_init(&overflow_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &overflow_context,
    NULL);
  test_wait_no_additional_completion(&overflow_context, 0u);

  ASSERT_TRUE(test_completion_call_count(&overflow_context) == 0u);

  bounce_deinit(&bounce);

  for (unsigned int index = 0; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    ASSERT_TRUE(test_completion_call_count(&waiting_contexts[index]) == 1u);
    ASSERT_TRUE(test_completion_result(&waiting_contexts[index]) == BOUNCE_COMPLETION_ABORTED);
    ASSERT_TRUE(
      test_posix_freertos_runtime_completion_ran_on_current_executor(
        &waiting_contexts[index]));
    test_completion_context_destroy(&waiting_contexts[index]);
  }

  ASSERT_TRUE(test_completion_call_count(&overflow_context) == 1u);
  ASSERT_TRUE(test_completion_result(&overflow_context) == BOUNCE_COMPLETION_ABORTED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_current_executor(&overflow_context));
  test_completion_context_destroy(&overflow_context);
}

#if !defined(BOUNCE_FREERTOS)
static void test_single_fd_read_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  unsigned char byte = 1u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[0],
    POLLIN,
    test_completion_callback,
    &completion_context,
    NULL);
  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_single_fd_write_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[1],
    POLLOUT,
    test_completion_callback,
    &completion_context,
    NULL);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

#if defined(BOUNCE_POSIX)
static int test_open_temporary_file(void) {
  char path[] = "/tmp/libbounce_file_io_XXXXXX";
  const int fd = mkstemp(path);

  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(unlink(path) == 0);
  return fd;
}

static void test_file_read_write_seek_flush_await_runs(void) {
  static const char payload[] = "libbounce file helper payload";
  BOUNCE_CORE bounce;
  BOUNCE_FILE_IO operation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  char buffer[sizeof payload];
  const int fd = test_open_temporary_file();

  memset(&buffer[0], 0, sizeof buffer);
  bounce_init(&bounce);
  bounce_file_io_init(&operation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_file_write(
    &bounce,
    &operation,
    fd,
    &payload[0],
    0,
    sizeof payload,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_file_io_result(&operation) == (int64_t)sizeof payload);
  ASSERT_TRUE(bounce_file_io_error(&operation) == 0);
  test_completion_context_destroy(&completion_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_file_flush(
    &bounce,
    &operation,
    fd,
    BOUNCE_FILE_FLUSH_FULL,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_file_io_result(&operation) == 0);
  ASSERT_TRUE(bounce_file_io_error(&operation) == 0);
  test_completion_context_destroy(&completion_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_file_seek(
    &bounce,
    &operation,
    fd,
    0,
    SEEK_SET,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_file_io_result(&operation) == 0);
  ASSERT_TRUE(bounce_file_io_error(&operation) == 0);
  test_completion_context_destroy(&completion_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_file_read(
    &bounce,
    &operation,
    fd,
    &buffer[0],
    BOUNCE_FILE_OFFSET_CURRENT,
    sizeof buffer,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_file_io_result(&operation) == (int64_t)sizeof payload);
  ASSERT_TRUE(bounce_file_io_error(&operation) == 0);
  ASSERT_TRUE(memcmp(&buffer[0], &payload[0], sizeof payload) == 0);
  test_completion_context_destroy(&completion_context);

  test_stop_parker(&bounce, &park_context);
  bounce_file_io_deinit(&operation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(fd) == 0);
}

static void test_file_read_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_FILE_IO operation;
  BOUNCE_CANCELLATION cancellation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  unsigned char byte = 0u;

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_file_io_init(&operation);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_file_read(
    &bounce,
    &operation,
    pipe_fds[0],
    &byte,
    BOUNCE_FILE_OFFSET_CURRENT,
    sizeof byte,
    test_completion_callback,
    &completion_context,
    &cancellation));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(bounce_file_io_result(&operation) == -1);
  ASSERT_TRUE(bounce_file_io_error(&operation) == ECANCELED);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_file_io_deinit(&operation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_socket_send_recv_await_runs(void) {
  static const char payload[] = "libbounce socket helper payload";
  BOUNCE_CORE bounce;
  BOUNCE_SOCKET_IO operation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int socket_fds[2];
  char buffer[sizeof payload];

  memset(&buffer[0], 0, sizeof buffer);
  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, &socket_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_socket_io_init(&operation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_socket_send(
    &bounce,
    &operation,
    socket_fds[0],
    &payload[0],
    sizeof payload,
    0,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_socket_io_result(&operation) == (int64_t)sizeof payload);
  ASSERT_TRUE(bounce_socket_io_error(&operation) == 0);
  test_completion_context_destroy(&completion_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_socket_recv(
    &bounce,
    &operation,
    socket_fds[1],
    &buffer[0],
    sizeof buffer,
    0,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_socket_io_result(&operation) == (int64_t)sizeof payload);
  ASSERT_TRUE(bounce_socket_io_error(&operation) == 0);
  ASSERT_TRUE(memcmp(&buffer[0], &payload[0], sizeof payload) == 0);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_socket_io_deinit(&operation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(socket_fds[0]) == 0);
  ASSERT_TRUE(close(socket_fds[1]) == 0);
}

static void test_socket_recvfrom_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_SOCKET_IO operation;
  BOUNCE_CANCELLATION cancellation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int socket_fds[2];
  unsigned char byte = 0u;

  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, &socket_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_socket_io_init(&operation);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_socket_recvfrom(
    &bounce,
    &operation,
    socket_fds[1],
    &byte,
    sizeof byte,
    0,
    NULL,
    NULL,
    test_completion_callback,
    &completion_context,
    &cancellation));
  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(bounce_socket_io_result(&operation) == -1);
  ASSERT_TRUE(bounce_socket_io_error(&operation) == ECANCELED);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_socket_io_deinit(&operation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(socket_fds[0]) == 0);
  ASSERT_TRUE(close(socket_fds[1]) == 0);
}

static void test_socket_send_await_reports_syscall_error(void) {
  static const char payload[] = "x";
  BOUNCE_CORE bounce;
  BOUNCE_SOCKET_IO operation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_socket_io_init(&operation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_socket_send(
    &bounce,
    &operation,
    pipe_fds[1],
    &payload[0],
    sizeof payload,
    TEST_SOCKET_DONTWAIT_FLAG,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(bounce_socket_io_result(&operation) == -1);
  ASSERT_TRUE(bounce_socket_io_error(&operation) == ENOTSOCK);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_socket_io_deinit(&operation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}
#endif
#endif

static void test_single_timeout_runs(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_completion_callback,
    &completion_context,
    NULL));
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
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

  test_completion_context_init(&completion_contexts[0], true, NULL);
  test_completion_context_init(&completion_contexts[1], true, NULL);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_completion_callback,
    &completion_contexts[0],
    NULL));
  test_wait_completion_count(&completion_contexts[0], 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_contexts[0]) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_contexts[0]) == BOUNCE_COMPLETION_COMPLETED);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_completion_callback,
    &completion_contexts[1],
    NULL));
  test_wait_completion_count(&completion_contexts[1], 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_contexts[1]) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_contexts[1]) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_contexts[0],
    &park_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_contexts[1],
    &park_context));

  test_completion_context_destroy(&completion_contexts[1]);
  test_completion_context_destroy(&completion_contexts[0]);
  test_stop_parker(&bounce, &park_context);
  bounce_timer_deinit(&timer);
  bounce_deinit(&bounce);
}

static void test_timeout_rearm_stays_stable_under_repeated_reuse(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_context;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_start_parker(&bounce, &park_context);

  for (unsigned int index = 0; index < TEST_TIMEOUT_REARM_REPEAT_COUNT; index++) {
    TEST_COMPLETION_CONTEXT completion_context;

    test_completion_context_init(&completion_context, true, NULL);
    ASSERT_TRUE(bounce_await_timeout(
      &bounce,
      &timer,
      20u,
      test_completion_callback,
      &completion_context,
      NULL));
    test_wait_completion_count(&completion_context, 1u);
    ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
    ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
    ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
      &completion_context,
      &park_context));
    test_completion_context_destroy(&completion_context);
  }

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

  test_completion_context_init(&completion_context, true, NULL);
  test_completion_context_init(&rejected_completion_context, true, NULL);

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
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  test_wait_no_additional_completion(&rejected_completion_context, 0u);

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

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1000u,
    test_completion_callback,
    &completion_context,
    &cancellation));
  bounce_cancel(&bounce, &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

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
  test_completion_context_init(&completion_context.completion, true, NULL);
  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_timeout_deinit_completion,
    &completion_context,
    NULL));

  test_wait_completion_count(&completion_context.completion, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context.completion) == 1u);
  ASSERT_TRUE(
    test_completion_result(&completion_context.completion) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context.completion,
    &park_context));
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

static void test_multi_parker_condition_stress(void) {
  static TEST_STRESS_STATE state;
  const unsigned int active_wait_count = test_stress_active_wait_count();

  memset(&state, 0, sizeof state);
  ASSERT_TRUE(active_wait_count > 0u);

  bounce_init(&state.bounce);
  test_backend_condition_init(&state.condition);
  test_stress_state_init(&state);

  for (unsigned int index = 0; index < TEST_STRESS_PARKER_COUNT; index++) {
    test_start_parker(
      &state.bounce,
      &state.park_contexts[index]);
  }

  for (unsigned int base = 0; base < TEST_STRESS_REQUEST_COUNT; base += active_wait_count) {
    unsigned int batch_size = TEST_STRESS_REQUEST_COUNT - base;

    if (batch_size > active_wait_count) {
      batch_size = active_wait_count;
    }

    test_posix_freertos_runtime_signal_reset(&state.batch_signal);
    __atomic_store_n(&state.batch_remaining, batch_size, __ATOMIC_RELEASE);

    for (unsigned int index = 0; index < batch_size; index++) {
      state.slots[index].state = &state;
      __atomic_store_n(&state.slots[index].task_index, base + index, __ATOMIC_RELEASE);
      test_backend_condition_await(
        &state.bounce,
        &state.condition,
        test_stress_completion,
        &state.slots[index],
        NULL);
    }

    test_backend_condition_raise(&state.bounce, &state.condition);
    test_wait_stress_batch(&state);
  }

  ASSERT_TRUE(test_atomic_load_uint(&state.completed_count) == TEST_STRESS_REQUEST_COUNT);
  for (unsigned int index = 0; index < TEST_STRESS_REQUEST_COUNT; index++) {
    ASSERT_TRUE(state.task_hits[index] == 1u);
  }
  test_stress_dump_and_assert_balance(&state);

  test_stop_parkers(&state.bounce, &state.park_contexts[0], TEST_STRESS_PARKER_COUNT);
  bounce_deinit(&state.bounce);
  test_stress_state_destroy(&state);
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

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  bounce_cancel(&bounce, &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));
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

  test_completion_context_init(&completion_context, true, NULL);
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_shutdown_without_wait_for_idle_returns_before_registration_settles(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));

  bounce_shutdown(&bounce, false);
  test_wait_parker_finished(&park_context);
  ASSERT_TRUE(test_atomic_load_uint(&park_context.finished) != 0u);
  ASSERT_TRUE(test_atomic_load_uint(&park_context.park_result) != 0u);
  test_wait_no_additional_completion(&completion_context, 0u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 0u);

  ASSERT_TRUE(test_posix_freertos_runtime_stop_parker(&bounce, &park_context, TEST_TIMEOUT_MS));
  bounce_deinit(&bounce);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_ABORTED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_current_executor(&completion_context));

  test_completion_context_destroy(&completion_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
}

static void test_shutdown_wait_for_idle_waits_for_registration_settle(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));

  bounce_shutdown(&bounce, true);
  ASSERT_TRUE(
    !test_posix_freertos_runtime_signal_wait(
      &park_context.finished_signal,
      TEST_NO_COMPLETION_TIMEOUT_MS));
  ASSERT_TRUE(test_atomic_load_uint(&park_context.finished) == 0u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 0u);

  bounce_cancel(&bounce, &cancellation);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

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

  test_completion_context_init(&completion_context, true, NULL);
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
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 0u);

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
  test_completion_context_init(&completion_context, false, NULL);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_completion_callback,
    &completion_context));
  bounce_deinit(&bounce);

  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_ABORTED);
  ASSERT_TRUE(test_completion_has_executor(&completion_context));
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_current_executor(
    &completion_context));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  test_completion_context_destroy(&completion_context);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
}

static void test_condition_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_backend_condition_raise(&bounce, &condition);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_condition_await_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  bounce_cancel(&bounce, &cancellation);
  test_completion_context_init(&completion_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_backend_condition_raise(&bounce, &condition);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

static void test_condition_completion_wins_over_later_cancel(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    &cancellation);
  test_backend_condition_raise(&bounce, &condition);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  bounce_cancel(&bounce, &cancellation);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

#if !defined(BOUNCE_FREERTOS)
static void test_fd_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[0],
    POLLIN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_fd_await_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2];

  ASSERT_TRUE(pipe(&pipe_fds[0]) == 0);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  bounce_cancel(&bounce, &cancellation);
  test_completion_context_init(&completion_context, true, NULL);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[0],
    POLLIN,
    test_completion_callback,
    &completion_context,
    &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

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

  test_completion_context_init(&completion_context, true, NULL);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[0],
    POLLIN,
    test_completion_callback,
    &completion_context,
    &cancellation);
  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  bounce_cancel(&bounce, &cancellation);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_shared_cancellation_cancels_mixed_condition_and_fd_waits(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completions[4];
  BOUNCE_CANCELLATION cancellation;
  int pipe_fds[2][2];
  unsigned char byte = 1u;

  memset(&pipe_fds[0][0], 0, sizeof pipe_fds);
  for (unsigned int index = 0; index < 2u; index++) {
    ASSERT_TRUE(pipe(&pipe_fds[index][0]) == 0);
  }
  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  for (unsigned int index = 0; index < 4u; index++) {
    test_completion_context_init(&completions[index], true, NULL);
  }

  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completions[0],
    &cancellation);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completions[1],
    &cancellation);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[0][0],
    POLLIN,
    test_completion_callback,
    &completions[2],
    &cancellation);
  bounce_await_posix_fd(
    &bounce,
    pipe_fds[1][0],
    POLLIN,
    test_completion_callback,
    &completions[3],
    &cancellation);

  bounce_cancel(&bounce, &cancellation);

  for (unsigned int index = 0; index < 4u; index++) {
    test_wait_completion_count(&completions[index], 1u);
    ASSERT_TRUE(test_completion_call_count(&completions[index]) == 1u);
    ASSERT_TRUE(test_completion_result(&completions[index]) == BOUNCE_COMPLETION_CANCELED);
    ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
      &completions[index],
      &park_context));
  }

  test_backend_condition_raise(&bounce, &condition);
  for (unsigned int index = 0; index < 2u; index++) {
    ASSERT_TRUE(write(pipe_fds[index][1], &byte, sizeof byte) == (ssize_t)sizeof byte);
  }
  for (unsigned int index = 0; index < 4u; index++) {
    test_wait_no_additional_completion(&completions[index], 1u);
    test_completion_context_destroy(&completions[index]);
  }

  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
  for (unsigned int index = 0; index < 2u; index++) {
    ASSERT_TRUE(close(pipe_fds[index][0]) == 0);
    ASSERT_TRUE(close(pipe_fds[index][1]) == 0);
  }
}
#else
static void test_shared_cancellation_cancels_all_condition_waits(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_contexts[4];
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  for (unsigned int index = 0; index < 4u; index++) {
    test_completion_context_init(&completion_contexts[index], true, NULL);
    test_backend_condition_await(
      &bounce,
      &condition,
      test_completion_callback,
      &completion_contexts[index],
      &cancellation);
  }

  bounce_cancel(&bounce, &cancellation);

  for (unsigned int index = 0; index < 4u; index++) {
    test_wait_completion_count(&completion_contexts[index], 1u);
    ASSERT_TRUE(test_completion_call_count(&completion_contexts[index]) == 1u);
    ASSERT_TRUE(
      test_completion_result(&completion_contexts[index]) == BOUNCE_COMPLETION_CANCELED);
    ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
      &completion_contexts[index],
      &park_context));
  }

  test_backend_condition_raise(&bounce, &condition);
  test_wait_no_additional_completion(&completion_contexts[0], 1u);
  for (unsigned int index = 0; index < 4u; index++) {
    ASSERT_TRUE(test_completion_call_count(&completion_contexts[index]) == 1u);
    test_completion_context_destroy(&completion_contexts[index]);
  }

  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}
#endif

static void test_repeated_cancel_is_harmless(void) {
  BOUNCE_CORE bounce;
  TEST_BACKEND_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  BOUNCE_CANCELLATION cancellation;

  bounce_init(&bounce);
  test_backend_condition_init(&condition);
  bounce_cancellation_init(&cancellation);
  test_start_parker(&bounce, &park_context);

  test_completion_context_init(&completion_context, true, NULL);
  test_backend_condition_await(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);
  bounce_cancel(&bounce, &cancellation);

  test_wait_completion_count(&completion_context, 1u);
  ASSERT_TRUE(test_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(test_completion_result(&completion_context) == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_backend_condition_raise(&bounce, &condition);
  test_wait_no_additional_completion(&completion_context, 1u);

  test_completion_context_destroy(&completion_context);
  test_stop_parker(&bounce, &park_context);
  bounce_cancellation_deinit(&cancellation);
  bounce_deinit(&bounce);
}

int test_run_posix_freertos_shared_suite(
  const TEST_POSIX_FREERTOS_SHARED_CONFIG *config) {
  TEST_CASE cases[ 128u ];
  size_t case_count = 0u;

#define TEST_APPEND_CASE(function_name)                                          \
  do {                                                                           \
    cases[case_count] = (TEST_CASE)TEST_CASE_ENTRY(function_name);               \
    case_count += 1u;                                                            \
  } while (0)

  ASSERT_TRUE(config != NULL);
  ASSERT_TRUE(config->suite_name != NULL);

  TEST_APPEND_CASE(test_tls_current_core_roundtrip);
  TEST_APPEND_CASE(test_tls_current_core_uses_fallback_when_unattached);
  TEST_APPEND_CASE(test_tls_current_core_visible_on_attached_parker);
  TEST_APPEND_CASE(test_single_post_runs);
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
  if (config->supports_cpp_timeout_cancellation) {
    TEST_APPEND_CASE(test_cpp_wrapper_cancellation_cancel_timeout_runs);
    TEST_APPEND_CASE(test_cpp_wrapper_cancellation_precanceled_timeout_runs);
  }
  TEST_APPEND_CASE(test_cpp_wrapper_registration_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_lambda_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_unregister_prevents_callback);
  TEST_APPEND_CASE(test_cpp_wrapper_registration_precanceled_completes_canceled);
  TEST_APPEND_CASE(test_cpp_wrapper_shutdown_wait_for_idle_keeps_pending_registration_alive);
  TEST_APPEND_CASE(test_cpp_wrapper_condition_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_condition_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_condition_await_aborts_on_deinit);
#if defined(BOUNCE_POSIX)
  TEST_APPEND_CASE(test_cpp_wrapper_fd_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_fd_await_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_lambda_fd_await_aborts_on_deinit);
  TEST_APPEND_CASE(test_cpp_wrapper_file_io_runs);
  TEST_APPEND_CASE(test_cpp_wrapper_socket_io_runs);
#endif
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
  TEST_APPEND_CASE(test_cpp_promise_condition_await_runs);
#if defined(BOUNCE_POSIX)
  TEST_APPEND_CASE(test_cpp_promise_fd_await_runs);
  TEST_APPEND_CASE(test_cpp_promise_fd_write_all_bytes_awaits_before_each_write_runs);
  TEST_APPEND_CASE(test_cpp_promise_file_io_async_runs);
  TEST_APPEND_CASE(test_cpp_promise_socket_io_async_runs);
#if defined(__linux__)
  TEST_APPEND_CASE(test_cpp_io_uring_operation_init_accessors);
  TEST_APPEND_CASE(test_cpp_promise_io_uring_await_runs);
  TEST_APPEND_CASE(test_cpp_promise_io_uring_await_canceled_runs);
#endif
#endif
#endif
  TEST_APPEND_CASE(test_park_once_post_runs);
  TEST_APPEND_CASE(test_park_once_returns_before_timeout_completion);
  TEST_APPEND_CASE(test_park_once_nested_post_inlines);
  TEST_APPEND_CASE(test_park_once_nested_post_falls_back_at_depth_limit);
  TEST_APPEND_CASE(test_single_condition_await_runs);
  TEST_APPEND_CASE(test_nested_post_inlines_with_park_ex);
  TEST_APPEND_CASE(test_nested_post_falls_back_at_depth_limit);
  TEST_APPEND_CASE(test_nested_precanceled_condition_await_inlines);
  TEST_APPEND_CASE(test_nested_post_does_not_overtake_existing_ready_item);
  TEST_APPEND_CASE(test_nested_post_inline_depth_benchmark);
  TEST_APPEND_CASE(test_post_then_condition_order);
  TEST_APPEND_CASE(test_condition_overflow_stays_pending_until_deinit);
#if !defined(BOUNCE_FREERTOS)
  if (config->supports_fd_await) {
    TEST_APPEND_CASE(test_single_fd_read_await_runs);
    TEST_APPEND_CASE(test_single_fd_write_await_runs);
#if defined(BOUNCE_POSIX)
    TEST_APPEND_CASE(test_file_read_write_seek_flush_await_runs);
    TEST_APPEND_CASE(test_file_read_await_cancel_completes_canceled);
    TEST_APPEND_CASE(test_socket_send_recv_await_runs);
    TEST_APPEND_CASE(test_socket_recvfrom_await_cancel_completes_canceled);
    TEST_APPEND_CASE(test_socket_send_await_reports_syscall_error);
#endif
  }
#endif
  TEST_APPEND_CASE(test_single_timeout_runs);
  if (config->supports_timeout_rearm) {
    TEST_APPEND_CASE(test_timeout_can_rearm_after_completion);
    TEST_APPEND_CASE(test_timeout_rearm_stays_stable_under_repeated_reuse);
  }
  if (config->supports_timeout_cancellation) {
    TEST_APPEND_CASE(test_timeout_rejects_second_active_wait);
    TEST_APPEND_CASE(test_timeout_cancel_completes_canceled);
    TEST_APPEND_CASE(test_timeout_completion_can_deinit_timer);
  }
  TEST_APPEND_CASE(test_multi_parker_condition_stress);
  TEST_APPEND_CASE(test_register_canceled_completes_canceled);
  TEST_APPEND_CASE(test_register_canceled_with_already_canceled_source_completes_canceled);
  TEST_APPEND_CASE(test_shutdown_without_wait_for_idle_returns_before_registration_settles);
  TEST_APPEND_CASE(test_shutdown_wait_for_idle_waits_for_registration_settle);
  TEST_APPEND_CASE(test_unregister_canceled_prevents_callback);
  TEST_APPEND_CASE(test_registration_deinit_aborts_pending_callback);
  TEST_APPEND_CASE(test_condition_await_cancel_completes_canceled);
  TEST_APPEND_CASE(test_condition_await_with_already_canceled_source_completes_canceled);
  TEST_APPEND_CASE(test_condition_completion_wins_over_later_cancel);
#if !defined(BOUNCE_FREERTOS)
  if (config->supports_fd_await) {
    TEST_APPEND_CASE(test_fd_await_cancel_completes_canceled);
    TEST_APPEND_CASE(test_fd_await_with_already_canceled_source_completes_canceled);
    TEST_APPEND_CASE(test_fd_completion_wins_over_later_cancel);
  }
  if (config->supports_mixed_wait_cancellation) {
    TEST_APPEND_CASE(test_shared_cancellation_cancels_mixed_condition_and_fd_waits);
  }
#else
  if (!config->supports_mixed_wait_cancellation) {
    TEST_APPEND_CASE(test_shared_cancellation_cancels_all_condition_waits);
  }
#endif
  TEST_APPEND_CASE(test_repeated_cancel_is_harmless);

#undef TEST_APPEND_CASE

  return test_run_suite(config->suite_name, cases, case_count);
}
