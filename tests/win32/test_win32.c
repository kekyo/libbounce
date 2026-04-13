/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
extern void test_cpp_wrapper_set_default_timeout_await_runs(void);
extern void test_cpp_wrapper_set_default_overrides_fallback_view(void);
extern void test_cpp_wrapper_park_once_returns_before_timeout_completion(void);
extern void test_cpp_wrapper_park_once_nested_post_inlines(void);
extern void test_cpp_wrapper_park_once_nested_post_falls_back_at_depth_limit(void);
extern void test_cpp_wrapper_current_post_runs_on_defaulted_parker(void);
extern void test_cpp_wrapper_await_runs(void);
extern void test_cpp_wrapper_lambda_await_runs(void);
extern void test_cpp_wrapper_lambda_await_aborts_on_deinit(void);
extern void test_win32_example_button_click_writes_sample_file(void);

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
extern void test_cpp_promise_handle_await_runs(void);
#endif

#if defined(_WIN64)
#define TEST_WIN32_SUITE_NAME "win32_x86_64"
#else
#define TEST_WIN32_SUITE_NAME "win32_i686"
#endif

#define TEST_TIMEOUT_MS 5000u
#define TEST_NO_CONDITION_TIMEOUT_MS 100u
#define TEST_STRESS_TIMEOUT_MS 60000u
#define TEST_STRESS_PARKER_COUNT 4u
#define TEST_STRESS_REQUEST_COUNT 10000u
#define TEST_STRESS_MAX_ACTIVE_WAITS 32u
#define TEST_INLINE_BENCH_ITERATIONS 100000u
#define TEST_MESSAGE_PUMP_WINDOW_CLASS_NAME "libbounce-test-message-pump"
#define TEST_MESSAGE_PUMP_WINDOW_MESSAGE (WM_APP + 1u)

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      ExitProcess(1);                                                            \
    }                                                                            \
  } while (0)

struct TEST_STRESS_STATE;
typedef struct TEST_STRESS_STATE TEST_STRESS_STATE;

typedef struct TEST_PARK_THREAD_CONTEXT {
  BOUNCE_CORE *bounce;
  HANDLE started_event;
  DWORD thread_id;
  unsigned int max_inline_depth;
  bool park_result;
} TEST_PARK_THREAD_CONTEXT;

typedef struct TEST_COMPLETION_CONTEXT {
  HANDLE done_event;
  volatile LONG call_count;
  volatile LONG result;
  volatile LONG order;
  DWORD callback_thread_id;
} TEST_COMPLETION_CONTEXT;

typedef struct TEST_ORDER_CONTEXT {
  TEST_COMPLETION_CONTEXT post;
  TEST_COMPLETION_CONTEXT await;
  volatile LONG next_order;
} TEST_ORDER_CONTEXT;

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
  HANDLE done_event;
  volatile LONG remaining;
} TEST_INLINE_BENCH_CONTEXT;

typedef struct TEST_INLINE_POST_CONTEXT {
  BOUNCE_CORE *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile LONG next_order;
} TEST_INLINE_POST_CONTEXT;

typedef struct TEST_MESSAGE_PUMP_CONTEXT {
  BOUNCE_CORE *bounce;
  HANDLE window_ready_event;
  HANDLE message_handled_event;
  HWND window_handle;
  bool park_result;
} TEST_MESSAGE_PUMP_CONTEXT;

typedef struct TEST_STRESS_SLOT {
  TEST_STRESS_STATE *state;
  HANDLE event_handle;
  volatile LONG task_index;
} TEST_STRESS_SLOT;

struct TEST_STRESS_STATE {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_threads[TEST_STRESS_PARKER_COUNT];
  HANDLE park_thread_handles[TEST_STRESS_PARKER_COUNT];
  CRITICAL_SECTION lock;
  HANDLE done_event;
  DWORD parker_thread_ids[TEST_STRESS_PARKER_COUNT];
  LONG per_thread_counts[TEST_STRESS_PARKER_COUNT];
  unsigned char task_hits[TEST_STRESS_REQUEST_COUNT];
  TEST_STRESS_SLOT slots[TEST_STRESS_MAX_ACTIVE_WAITS];
  volatile LONG batch_remaining;
  volatile LONG completed_count;
  DWORD active_wait_count;
};

static void test_completion_common(
  TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result,
  volatile LONG *next_order) {
  InterlockedIncrement(&context->call_count);
  context->result = (LONG)result;
  context->callback_thread_id = GetCurrentThreadId();
  if (next_order != NULL) {
    context->order = InterlockedIncrement(next_order);
  }
  ASSERT_TRUE(SetEvent(context->done_event) != 0);
}

static void test_post_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  test_completion_common(completion_state, result, NULL);
}

static void test_await_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  test_completion_common(completion_state, result, NULL);
}

static void test_post_order_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_ORDER_CONTEXT *context = completion_state;
  test_completion_common(&context->post, result, &context->next_order);
}

static void test_await_order_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_ORDER_CONTEXT *context = completion_state;
  test_completion_common(&context->await, result, &context->next_order);
}

static void test_timeout_deinit_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_TIMER_DEINIT_CONTEXT *context = completion_state;

  bounce_timer_deinit(context->timer);
  test_completion_common(&context->completion, result, NULL);
}

static void test_current_core_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_CURRENT_CORE_CONTEXT *context = completion_state;

  context->observed_bounce = bounce_get_core();
  test_completion_common(&context->completion, result, NULL);
}

static LRESULT CALLBACK test_message_pump_window_proc(
  HWND window_handle,
  UINT message,
  WPARAM wparam,
  LPARAM lparam) {
  TEST_MESSAGE_PUMP_CONTEXT *context =
    (TEST_MESSAGE_PUMP_CONTEXT *)GetWindowLongPtr(
      window_handle,
      GWLP_USERDATA);

  (void)wparam;
  (void)lparam;

  if (message == TEST_MESSAGE_PUMP_WINDOW_MESSAGE) {
    ASSERT_TRUE(context != NULL);
    ASSERT_TRUE(SetEvent(context->message_handled_event) != 0);
    return 0;
  }

  return DefWindowProc(window_handle, message, wparam, lparam);
}

static bool test_register_message_pump_window_class(void) {
  WNDCLASSA window_class;

  memset(&window_class, 0, sizeof window_class);
  window_class.lpfnWndProc = test_message_pump_window_proc;
  window_class.hInstance = GetModuleHandleA(NULL);
  window_class.lpszClassName = TEST_MESSAGE_PUMP_WINDOW_CLASS_NAME;

  if (RegisterClassA(&window_class) != 0) {
    return true;
  }
  return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static DWORD WINAPI test_message_pump_park_thread_proc(LPVOID parameter) {
  TEST_MESSAGE_PUMP_CONTEXT *context = parameter;
  BOUNCE_CORE *previous_core = bounce_get_core();
  HWND window_handle;

  bounce_set_core(context->bounce);
  ASSERT_TRUE(test_register_message_pump_window_class());
  window_handle = CreateWindowExA(
    0u,
    TEST_MESSAGE_PUMP_WINDOW_CLASS_NAME,
    "libbounce-test-message-pump",
    0u,
    0,
    0,
    0,
    0,
    HWND_MESSAGE,
    NULL,
    GetModuleHandleA(NULL),
    NULL);
  ASSERT_TRUE(window_handle != NULL);
  SetLastError(0u);
  ASSERT_TRUE(
    (SetWindowLongPtr(
       window_handle,
       GWLP_USERDATA,
       (LONG_PTR)context) != 0) ||
    (GetLastError() == 0u));
  context->window_handle = window_handle;
  ASSERT_TRUE(SetEvent(context->window_ready_event) != 0);
  context->park_result = bounce_park(context->bounce, 0u);
  ASSERT_TRUE(DestroyWindow(window_handle) != 0);
  context->window_handle = NULL;
  bounce_set_core(previous_core);
  return context->park_result ? 0u : 1u;
}

static DWORD test_find_thread_index(
  const DWORD *thread_ids,
  DWORD thread_count,
  DWORD thread_id) {
  for (DWORD index = 0; index < thread_count; index++) {
    if (thread_ids[index] == thread_id) {
      return index;
    }
  }
  return MAXDWORD;
}

static DWORD WINAPI test_park_thread_proc(LPVOID parameter) {
  TEST_PARK_THREAD_CONTEXT *context = parameter;
  BOUNCE_CORE *previous_core = bounce_get_core();

  bounce_set_core(context->bounce);
  context->thread_id = GetCurrentThreadId();
  ASSERT_TRUE(SetEvent(context->started_event) != 0);
  context->park_result = bounce_park(context->bounce, context->max_inline_depth);
  bounce_set_core(previous_core);
  return context->park_result ? 0u : 1u;
}

static void test_completion_context_init(TEST_COMPLETION_CONTEXT *context) {
  context->done_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(context->done_event != NULL);
  context->call_count = 0;
  context->result = -1;
  context->order = 0;
  context->callback_thread_id = 0;
}

static void test_completion_context_deinit(TEST_COMPLETION_CONTEXT *context) {
  if (context->done_event != NULL) {
    CloseHandle(context->done_event);
    context->done_event = NULL;
  }
}

static void test_assert_completion_on_current_thread(
  const TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(context->call_count == 1);
  ASSERT_TRUE(context->result == (LONG)result);
  ASSERT_TRUE(context->callback_thread_id == GetCurrentThreadId());
}

static HANDLE test_start_parker(BOUNCE_CORE *bounce, TEST_PARK_THREAD_CONTEXT *context);
static void test_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *contexts,
  HANDLE *thread_handles,
  DWORD thread_count);

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
  TEST_PARK_THREAD_CONTEXT park_thread;
  HANDLE thread_handle;
  TEST_CURRENT_CORE_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.expected_bounce = &bounce;
  test_completion_context_init(&context.completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_post(&bounce, test_current_core_completion, &context));
  ASSERT_TRUE(WaitForSingleObject(context.completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(context.completion.call_count == 1);
  ASSERT_TRUE(context.completion.result == (LONG)BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.observed_bounce == context.expected_bounce);
  ASSERT_TRUE(context.completion.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  test_completion_context_deinit(&context.completion);
  bounce_deinit(&bounce);
}

static void test_park_pumps_window_messages(void) {
  BOUNCE_CORE bounce;
  TEST_MESSAGE_PUMP_CONTEXT context;
  HANDLE thread_handle;
  DWORD exit_code;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  context.window_ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  context.message_handled_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(context.window_ready_event != NULL);
  ASSERT_TRUE(context.message_handled_event != NULL);

  thread_handle = CreateThread(
    NULL,
    0u,
    test_message_pump_park_thread_proc,
    &context,
    0u,
    NULL);
  ASSERT_TRUE(thread_handle != NULL);
  ASSERT_TRUE(
    WaitForSingleObject(context.window_ready_event, TEST_TIMEOUT_MS) ==
    WAIT_OBJECT_0);
  ASSERT_TRUE(context.window_handle != NULL);
  ASSERT_TRUE(
    PostMessage(
      context.window_handle,
      TEST_MESSAGE_PUMP_WINDOW_MESSAGE,
      0u,
      0u) != 0);
  ASSERT_TRUE(
    WaitForSingleObject(context.message_handled_event, TEST_TIMEOUT_MS) ==
    WAIT_OBJECT_0);

  bounce_shutdown(&bounce, false);
  ASSERT_TRUE(WaitForSingleObject(thread_handle, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
  ASSERT_TRUE(GetExitCodeThread(thread_handle, &exit_code) != 0);
  ASSERT_TRUE(exit_code == 0u);
  ASSERT_TRUE(context.park_result);

  CloseHandle(thread_handle);
  CloseHandle(context.message_handled_event);
  CloseHandle(context.window_ready_event);
  bounce_deinit(&bounce);
}

static HANDLE test_start_parker_with_inline_depth(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth);

static HANDLE test_start_parker(BOUNCE_CORE *bounce, TEST_PARK_THREAD_CONTEXT *context) {
  return test_start_parker_with_inline_depth(bounce, context, 0u);
}

static HANDLE test_start_parker_with_inline_depth(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth) {
  HANDLE thread_handle;

  context->bounce = bounce;
  context->started_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(context->started_event != NULL);
  context->thread_id = 0;
  context->max_inline_depth = max_inline_depth;
  context->park_result = false;

  thread_handle = CreateThread(NULL, 0, test_park_thread_proc, context, 0, NULL);
  ASSERT_TRUE(thread_handle != NULL);
  ASSERT_TRUE(WaitForSingleObject(context->started_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
  CloseHandle(context->started_event);
  context->started_event = NULL;
  return thread_handle;
}

static double test_monotonic_now_ms(void) {
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;

  ASSERT_TRUE(QueryPerformanceCounter(&counter) != 0);
  ASSERT_TRUE(QueryPerformanceFrequency(&frequency) != 0);
  ASSERT_TRUE(frequency.QuadPart != 0);
  return ((double)counter.QuadPart * 1000.0) / (double)frequency.QuadPart;
}

static void test_poll_park_once_until_completion(
  BOUNCE_CORE *bounce,
  TEST_COMPLETION_CONTEXT *context,
  unsigned int max_inline_depth,
  LONG expected_call_count) {
  const double started_ms = test_monotonic_now_ms();

  while (context->call_count < expected_call_count) {
    ASSERT_TRUE(bounce_park_once(bounce, max_inline_depth));
    ASSERT_TRUE((test_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
  }
}

static void test_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *contexts,
  HANDLE *thread_handles,
  DWORD thread_count);

static void test_inline_benchmark_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_BENCH_CONTEXT *context = completion_state;
  LONG remaining;

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  remaining = InterlockedDecrement(&context->remaining);
  if (remaining == 0) {
    ASSERT_TRUE(SetEvent(context->done_event) != 0);
    return;
  }

  ASSERT_TRUE(bounce_post(context->bounce, test_inline_benchmark_completion, context));
}

static void test_nested_post_inner_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_POST_CONTEXT *context = completion_state;

  test_completion_common(&context->nested, result, &context->next_order);
}

static void test_nested_post_outer_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_INLINE_POST_CONTEXT *context = completion_state;

  ASSERT_TRUE(bounce_post(context->bounce, test_nested_post_inner_completion, context));
  test_completion_common(&context->outer, result, &context->next_order);
}

static double test_run_inline_depth_benchmark(unsigned int max_inline_depth) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_context;
  HANDLE thread_handle;
  TEST_INLINE_BENCH_CONTEXT context;
  double started_ms;
  double finished_ms;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  context.done_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(context.done_event != NULL);
  context.remaining = (LONG)TEST_INLINE_BENCH_ITERATIONS;
  thread_handle = test_start_parker_with_inline_depth(&bounce, &park_context, max_inline_depth);

  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_post(&bounce, test_inline_benchmark_completion, &context));
  ASSERT_TRUE(WaitForSingleObject(context.done_event, TEST_STRESS_TIMEOUT_MS) == WAIT_OBJECT_0);
  finished_ms = test_monotonic_now_ms();

  test_stop_parkers(&bounce, &park_context, &thread_handle, 1u);
  CloseHandle(context.done_event);
  bounce_deinit(&bounce);
  return finished_ms - started_ms;
}

static void test_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_PARK_THREAD_CONTEXT *contexts,
  HANDLE *thread_handles,
  DWORD thread_count) {
  bounce_shutdown(bounce, false);
  for (DWORD index = 0; index < thread_count; index++) {
    DWORD exit_code;

    ASSERT_TRUE(WaitForSingleObject(thread_handles[index], TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
    ASSERT_TRUE(GetExitCodeThread(thread_handles[index], &exit_code) != 0);
    ASSERT_TRUE(exit_code == 0u);
    ASSERT_TRUE(contexts[index].park_result);
    CloseHandle(thread_handles[index]);
    thread_handles[index] = NULL;
  }
}

static DWORD test_stress_active_wait_count(void) {
  DWORD active_wait_count = TEST_STRESS_MAX_ACTIVE_WAITS;
  DWORD completion_limit = 0u;

  if (BOUNCE_MAX_STATIC_COMPLETION_ITEMS > TEST_STRESS_PARKER_COUNT) {
    completion_limit = BOUNCE_MAX_STATIC_COMPLETION_ITEMS - TEST_STRESS_PARKER_COUNT;
  }
  if (completion_limit < active_wait_count) {
    active_wait_count = completion_limit;
  }
  if ((MAXIMUM_WAIT_OBJECTS - 3u) < active_wait_count) {
    active_wait_count = MAXIMUM_WAIT_OBJECTS - 3u;
  }
  return active_wait_count;
}

static void test_stress_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_STRESS_SLOT *slot = completion_state;
  TEST_STRESS_STATE *state = slot->state;
  const DWORD thread_index = test_find_thread_index(
    state->parker_thread_ids,
    TEST_STRESS_PARKER_COUNT,
    GetCurrentThreadId());
  const LONG task_index = InterlockedCompareExchange(&slot->task_index, 0, 0);

  ASSERT_TRUE(result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(thread_index < TEST_STRESS_PARKER_COUNT);
  ASSERT_TRUE(task_index >= 0);
  ASSERT_TRUE((DWORD)task_index < TEST_STRESS_REQUEST_COUNT);

  EnterCriticalSection(&state->lock);
  ASSERT_TRUE(state->task_hits[task_index] == 0u);
  state->task_hits[task_index] = 1u;
  state->per_thread_counts[thread_index] += 1;
  state->completed_count += 1;
  LeaveCriticalSection(&state->lock);

  if (InterlockedDecrement(&state->batch_remaining) == 0) {
    ASSERT_TRUE(SetEvent(state->done_event) != 0);
  }
}

static void test_stress_dump_and_assert_balance(const TEST_STRESS_STATE *state) {
  LONG min_count = state->per_thread_counts[0];
  LONG max_count = state->per_thread_counts[0];
  DWORD threads_with_work = 0u;

  for (DWORD index = 0; index < TEST_STRESS_PARKER_COUNT; index++) {
    const LONG count = state->per_thread_counts[index];

    fprintf(stdout, "stress parker[%lu] count=%ld\n", (unsigned long)index, count);
    if (count > 0) {
      threads_with_work += 1u;
    }
    if (count < min_count) {
      min_count = count;
    }
    if (count > max_count) {
      max_count = count;
    }
  }

  fprintf(
    stdout,
    "stress count spread=%ld\n",
    max_count - min_count);

  ASSERT_TRUE(threads_with_work == TEST_STRESS_PARKER_COUNT);
}

static void test_single_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  HANDLE thread_handle;

  bounce_init(&bounce);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_post(&bounce, test_post_completion, &completion));
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_park_once_post_runs(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion;

  bounce_init(&bounce);
  test_completion_context_init(&completion);

  ASSERT_TRUE(bounce_post(&bounce, test_post_completion, &completion));
  ASSERT_TRUE(bounce_park_once(&bounce, 0u));
  test_assert_completion_on_current_thread(
    &completion,
    BOUNCE_COMPLETION_COMPLETED);

  test_completion_context_deinit(&completion);
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

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    200u,
    test_await_completion,
    &completion,
    NULL));
  started_ms = test_monotonic_now_ms();
  ASSERT_TRUE(bounce_park_once(&bounce, 0u));
  first_return_ms = test_monotonic_now_ms();

  ASSERT_TRUE((first_return_ms - started_ms) < 100.0);
  ASSERT_TRUE(completion.call_count == 0);

  test_poll_park_once_until_completion(&bounce, &completion, 0u, 1);
  test_assert_completion_on_current_thread(
    &completion,
    BOUNCE_COMPLETION_COMPLETED);

  bounce_timer_deinit(&timer);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_park_once_nested_post_inlines(void) {
  BOUNCE_CORE bounce;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_park_once(&bounce, 2u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.nested.order == 1);
  ASSERT_TRUE(context.outer.order == 2);

  test_completion_context_deinit(&context.nested);
  test_completion_context_deinit(&context.outer);
  bounce_deinit(&bounce);
}

static void test_park_once_nested_post_falls_back_at_depth_limit(void) {
  BOUNCE_CORE bounce;
  TEST_INLINE_POST_CONTEXT context;

  memset(&context, 0, sizeof context);
  bounce_init(&bounce);
  context.bounce = &bounce;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);

  ASSERT_TRUE(bounce_post(&bounce, test_nested_post_outer_completion, &context));
  ASSERT_TRUE(bounce_park_once(&bounce, 1u));

  test_assert_completion_on_current_thread(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_thread(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(context.outer.order == 1);
  ASSERT_TRUE(context.nested.order == 2);

  test_completion_context_deinit(&context.nested);
  test_completion_context_deinit(&context.outer);
  bounce_deinit(&bounce);
}

static void test_single_await_runs(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  test_completion_context_init(&completion);
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_await_win32_handle(&bounce, event_handle, test_await_completion, &completion, NULL);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_single_timeout_runs(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_await_completion,
    &completion,
    NULL));
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_timer_deinit(&timer);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_post_then_await_order(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_ORDER_CONTEXT order_context;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  test_completion_context_init(&order_context.post);
  test_completion_context_init(&order_context.await);
  order_context.next_order = 0;
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_await_win32_handle(
    &bounce,
    event_handle,
    test_await_order_completion,
    &order_context,
    NULL);
  ASSERT_TRUE(bounce_post(&bounce, test_post_order_completion, &order_context));
  ASSERT_TRUE(WaitForSingleObject(order_context.post.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(order_context.post.call_count == 1);
  ASSERT_TRUE(order_context.post.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(order_context.post.order == 1);
  ASSERT_TRUE(order_context.await.call_count == 0);

  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(order_context.await.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(order_context.await.call_count == 1);
  ASSERT_TRUE(order_context.await.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(order_context.await.order == 2);
  ASSERT_TRUE(order_context.post.callback_thread_id == park_thread.thread_id);
  ASSERT_TRUE(order_context.await.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  test_completion_context_deinit(&order_context.await);
  test_completion_context_deinit(&order_context.post);
  bounce_deinit(&bounce);
}

static void test_nested_post_inline_depth_benchmark(void) {
  static const unsigned int depths[] = { 0u, 1u, 2u, 4u, 8u, 16u };
  double baseline_ms = 0.0;

  fprintf(stdout, "inline benchmark iterations=%u\n", TEST_INLINE_BENCH_ITERATIONS);
  for (size_t index = 0u; index < (sizeof depths / sizeof depths[0]); index++) {
    double duration_ms = test_run_inline_depth_benchmark(depths[index]);
    double speedup = 1.0;

    if (index == 0u) {
      baseline_ms = duration_ms;
    } else if (duration_ms > 0.0) {
      speedup = baseline_ms / duration_ms;
    }

    fprintf(
      stdout,
      "inline benchmark max_inline_depth=%u duration_ms=%.3f speedup_vs_depth0=%.3fx\n",
      depths[index],
      duration_ms,
      speedup);
  }
}

static void test_timeout_can_rearm_after_completion(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completions[2];
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  test_completion_context_init(&completions[0]);
  test_completion_context_init(&completions[1]);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_await_completion,
    &completions[0],
    NULL));
  ASSERT_TRUE(WaitForSingleObject(completions[0].done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
  ASSERT_TRUE(completions[0].call_count == 1);
  ASSERT_TRUE(completions[0].result == BOUNCE_COMPLETION_COMPLETED);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    30u,
    test_await_completion,
    &completions[1],
    NULL));
  ASSERT_TRUE(WaitForSingleObject(completions[1].done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
  ASSERT_TRUE(completions[1].call_count == 1);
  ASSERT_TRUE(completions[1].result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completions[0].callback_thread_id == park_thread.thread_id);
  ASSERT_TRUE(completions[1].callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_timer_deinit(&timer);
  test_completion_context_deinit(&completions[1]);
  test_completion_context_deinit(&completions[0]);
  bounce_deinit(&bounce);
}

static void test_timeout_rejects_second_active_wait(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  TEST_COMPLETION_CONTEXT rejected_completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  test_completion_context_init(&rejected_completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1000u,
    test_await_completion,
    &completion,
    &cancellation));
  ASSERT_TRUE(!bounce_await_timeout(
    &bounce,
    &timer,
    10u,
    test_await_completion,
    &rejected_completion,
    NULL));

  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(rejected_completion.call_count == 0);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_deinit(&cancellation);
  bounce_timer_deinit(&timer);
  test_completion_context_deinit(&rejected_completion);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_timeout_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    1000u,
    test_await_completion,
    &completion,
    &cancellation));
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_deinit(&cancellation);
  bounce_timer_deinit(&timer);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_timeout_completion_can_deinit_timer(void) {
  BOUNCE_CORE bounce;
  BOUNCE_TIMER timer;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_TIMER_DEINIT_CONTEXT completion;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_timer_init(&timer);
  completion.timer = &timer;
  test_completion_context_init(&completion.completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_await_timeout(
    &bounce,
    &timer,
    50u,
    test_timeout_deinit_completion,
    &completion,
    NULL));
  ASSERT_TRUE(WaitForSingleObject(completion.completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.completion.call_count == 1);
  ASSERT_TRUE(completion.completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.completion.callback_thread_id == park_thread.thread_id);
  ASSERT_TRUE(!bounce_await_timeout(
    &bounce,
    &timer,
    10u,
    test_await_completion,
    &completion.completion,
    NULL));

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  test_completion_context_deinit(&completion.completion);
  bounce_deinit(&bounce);
}

static void test_await_overflow_completes_after_dynamic_growth(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  HANDLE thread_handle;
  TEST_COMPLETION_CONTEXT waiting_contexts[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
  HANDLE waiting_events[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
  HANDLE overflow_event;
  TEST_COMPLETION_CONTEXT overflow_context;

  memset(waiting_events, 0, sizeof waiting_events);
  bounce_init(&bounce);
  test_completion_context_init(&overflow_context);
  overflow_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(overflow_event != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  for (DWORD index = 0; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    waiting_events[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_TRUE(waiting_events[index] != NULL);
    test_completion_context_init(&waiting_contexts[index]);
    bounce_await_win32_handle(
      &bounce,
      waiting_events[index],
      test_await_completion,
      &waiting_contexts[index],
      NULL);
  }

  bounce_await_win32_handle(
    &bounce,
    overflow_event,
    test_await_completion,
    &overflow_context,
    NULL);
  ASSERT_TRUE(WaitForSingleObject(overflow_context.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  SetEvent(overflow_event);
  ASSERT_TRUE(WaitForSingleObject(overflow_context.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
  ASSERT_TRUE(overflow_context.call_count == 1);
  ASSERT_TRUE(overflow_context.result == BOUNCE_COMPLETION_COMPLETED);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_deinit(&bounce);

  for (DWORD index = 0; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    ASSERT_TRUE(WaitForSingleObject(waiting_contexts[index].done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
    ASSERT_TRUE(waiting_contexts[index].call_count == 1);
    ASSERT_TRUE(waiting_contexts[index].result == BOUNCE_COMPLETION_ABORTED);
    test_completion_context_deinit(&waiting_contexts[index]);
    CloseHandle(waiting_events[index]);
    waiting_events[index] = NULL;
  }

  CloseHandle(overflow_event);
  test_completion_context_deinit(&overflow_context);
}

static void test_waiter_sharding_completes_over_single_wait_set(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion_contexts[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
  HANDLE wait_events[BOUNCE_MAX_STATIC_COMPLETION_ITEMS];
  HANDLE thread_handle;
  DWORD wait_count = BOUNCE_MAX_WIN32_WAITS_PER_WAITER + 2u;

  ASSERT_TRUE(BOUNCE_MAX_WIN32_WAITERS >= 2);
  ASSERT_TRUE(BOUNCE_MAX_STATIC_COMPLETION_ITEMS > BOUNCE_MAX_WIN32_WAITS_PER_WAITER);
  ASSERT_TRUE(wait_count <= BOUNCE_MAX_STATIC_COMPLETION_ITEMS);

  memset(wait_events, 0, sizeof wait_events);
  bounce_init(&bounce);
  thread_handle = test_start_parker(&bounce, &park_thread);

  for (DWORD index = 0; index < wait_count; index++) {
    wait_events[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_TRUE(wait_events[index] != NULL);
    test_completion_context_init(&completion_contexts[index]);
    bounce_await_win32_handle(
      &bounce,
      wait_events[index],
      test_await_completion,
      &completion_contexts[index],
      NULL);
  }

  for (DWORD index = 0; index < wait_count; index++) {
    ASSERT_TRUE(SetEvent(wait_events[index]) != 0);
  }

  for (DWORD index = 0; index < wait_count; index++) {
    ASSERT_TRUE(WaitForSingleObject(completion_contexts[index].done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
    ASSERT_TRUE(completion_contexts[index].call_count == 1);
    ASSERT_TRUE(completion_contexts[index].result == BOUNCE_COMPLETION_COMPLETED);
  }

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);

  for (DWORD index = 0; index < wait_count; index++) {
    test_completion_context_deinit(&completion_contexts[index]);
    CloseHandle(wait_events[index]);
    wait_events[index] = NULL;
  }
  bounce_deinit(&bounce);
}

static void test_multi_parker_await_stress(void) {
  TEST_STRESS_STATE state;

  memset(&state, 0, sizeof state);
  state.active_wait_count = test_stress_active_wait_count();
  ASSERT_TRUE(state.active_wait_count >= TEST_STRESS_PARKER_COUNT);

  bounce_init(&state.bounce);
  InitializeCriticalSection(&state.lock);
  state.done_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(state.done_event != NULL);

  for (DWORD index = 0; index < state.active_wait_count; index++) {
    state.slots[index].state = &state;
    state.slots[index].task_index = -1;
    state.slots[index].event_handle = NULL;
  }

  for (DWORD index = 0; index < TEST_STRESS_PARKER_COUNT; index++) {
    state.park_thread_handles[index] = test_start_parker(
      &state.bounce,
      &state.park_threads[index]);
    state.parker_thread_ids[index] = state.park_threads[index].thread_id;
  }

  Sleep(50u);

  for (DWORD base = 0; base < TEST_STRESS_REQUEST_COUNT; base += state.active_wait_count) {
    DWORD batch_count = TEST_STRESS_REQUEST_COUNT - base;

    if (batch_count > state.active_wait_count) {
      batch_count = state.active_wait_count;
    }

    ASSERT_TRUE(ResetEvent(state.done_event) != 0);
    InterlockedExchange(&state.batch_remaining, (LONG)batch_count);

    for (DWORD index = 0; index < batch_count; index++) {
      TEST_STRESS_SLOT *slot = &state.slots[index];

      slot->event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
      ASSERT_TRUE(slot->event_handle != NULL);
      InterlockedExchange(&slot->task_index, (LONG)(base + index));
      bounce_await_win32_handle(
        &state.bounce,
        slot->event_handle,
        test_stress_completion,
        slot,
        NULL);
    }

    for (DWORD index = 0; index < batch_count; index++) {
      ASSERT_TRUE(SetEvent(state.slots[index].event_handle) != 0);
    }

    ASSERT_TRUE(WaitForSingleObject(state.done_event, TEST_STRESS_TIMEOUT_MS) == WAIT_OBJECT_0);

    for (DWORD index = 0; index < batch_count; index++) {
      CloseHandle(state.slots[index].event_handle);
      state.slots[index].event_handle = NULL;
    }
  }

  ASSERT_TRUE((DWORD)state.completed_count == TEST_STRESS_REQUEST_COUNT);

  for (DWORD index = 0; index < TEST_STRESS_REQUEST_COUNT; index++) {
    ASSERT_TRUE(state.task_hits[index] == 1u);
  }
  test_stress_dump_and_assert_balance(&state);

  test_stop_parkers(
    &state.bounce,
    state.park_threads,
    state.park_thread_handles,
    TEST_STRESS_PARKER_COUNT);
  CloseHandle(state.done_event);
  state.done_event = NULL;
  DeleteCriticalSection(&state.lock);
  bounce_deinit(&state.bounce);
}

static void test_await_cancel_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_await_win32_handle(
    &bounce,
    event_handle,
    test_await_completion,
    &completion,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_register_canceled_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_await_completion,
    &completion));
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_register_canceled_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_await_completion,
    &completion));
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_unregister_canceled_prevents_callback(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;
  HANDLE thread_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion);
  thread_handle = test_start_parker(&bounce, &park_thread);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_await_completion,
    &completion));
  ASSERT_TRUE(bounce_unregister_canceled(&registration));
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));
  bounce_cancel(&bounce, &cancellation);

  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 0);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_registration_deinit_aborts_pending_callback(void) {
  BOUNCE_CORE bounce;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  BOUNCE_CANCELLATION_REGISTRATION registration;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  bounce_cancellation_registration_init(&registration);
  test_completion_context_init(&completion);

  ASSERT_TRUE(bounce_register_canceled(
    &bounce,
    &cancellation,
    &registration,
    test_await_completion,
    &completion));
  bounce_deinit(&bounce);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_ABORTED);
  ASSERT_TRUE(!bounce_unregister_canceled(&registration));

  bounce_cancellation_registration_deinit(&registration);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
}

static void test_await_with_already_canceled_source_completes_canceled(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_cancel(&bounce, &cancellation);
  bounce_await_win32_handle(
    &bounce,
    event_handle,
    test_await_completion,
    &completion,
    &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_await_completion_wins_over_later_cancel(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_await_win32_handle(
    &bounce,
    event_handle,
    test_await_completion,
    &completion,
    &cancellation);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

static void test_shared_cancellation_cancels_all_waits(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completions[4];
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;
  HANDLE event_handles[4];

  memset(event_handles, 0, sizeof event_handles);
  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  thread_handle = test_start_parker(&bounce, &park_thread);

  for (DWORD index = 0; index < 4u; index++) {
    test_completion_context_init(&completions[index]);
    event_handles[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_TRUE(event_handles[index] != NULL);
    bounce_await_win32_handle(
      &bounce,
      event_handles[index],
      test_await_completion,
      &completions[index],
      &cancellation);
  }

  bounce_cancel(&bounce, &cancellation);

  for (DWORD index = 0; index < 4u; index++) {
    ASSERT_TRUE(WaitForSingleObject(completions[index].done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);
    ASSERT_TRUE(completions[index].call_count == 1);
    ASSERT_TRUE(completions[index].result == BOUNCE_COMPLETION_CANCELED);
    ASSERT_TRUE(completions[index].callback_thread_id == park_thread.thread_id);
  }

  for (DWORD index = 0; index < 4u; index++) {
    ASSERT_TRUE(ResetEvent(completions[index].done_event) != 0);
    ASSERT_TRUE(SetEvent(event_handles[index]) != 0);
    ASSERT_TRUE(WaitForSingleObject(completions[index].done_event, TEST_NO_CONDITION_TIMEOUT_MS) ==
      WAIT_TIMEOUT);
    ASSERT_TRUE(completions[index].call_count == 1);
  }

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  bounce_cancellation_deinit(&cancellation);
  for (DWORD index = 0; index < 4u; index++) {
    CloseHandle(event_handles[index]);
    event_handles[index] = NULL;
    test_completion_context_deinit(&completions[index]);
  }
  bounce_deinit(&bounce);
}

static void test_repeated_cancel_is_harmless(void) {
  BOUNCE_CORE bounce;
  TEST_PARK_THREAD_CONTEXT park_thread;
  TEST_COMPLETION_CONTEXT completion;
  BOUNCE_CANCELLATION cancellation;
  HANDLE thread_handle;
  HANDLE event_handle;

  bounce_init(&bounce);
  bounce_cancellation_init(&cancellation);
  test_completion_context_init(&completion);
  event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ASSERT_TRUE(event_handle != NULL);
  thread_handle = test_start_parker(&bounce, &park_thread);

  bounce_await_win32_handle(
    &bounce,
    event_handle,
    test_await_completion,
    &completion,
    &cancellation);
  bounce_cancel(&bounce, &cancellation);
  bounce_cancel(&bounce, &cancellation);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_TIMEOUT_MS) == WAIT_OBJECT_0);

  ASSERT_TRUE(completion.call_count == 1);
  ASSERT_TRUE(completion.result == BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(completion.callback_thread_id == park_thread.thread_id);

  ASSERT_TRUE(ResetEvent(completion.done_event) != 0);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  ASSERT_TRUE(WaitForSingleObject(completion.done_event, TEST_NO_CONDITION_TIMEOUT_MS) == WAIT_TIMEOUT);
  ASSERT_TRUE(completion.call_count == 1);

  test_stop_parkers(&bounce, &park_thread, &thread_handle, 1u);
  CloseHandle(event_handle);
  bounce_cancellation_deinit(&cancellation);
  test_completion_context_deinit(&completion);
  bounce_deinit(&bounce);
}

int main(void) {
  static const TEST_CASE cases[] = {
    TEST_CASE_ENTRY(test_tls_current_core_roundtrip),
    TEST_CASE_ENTRY(test_tls_current_core_uses_fallback_when_unattached),
    TEST_CASE_ENTRY(test_tls_current_core_visible_on_attached_parker),
    TEST_CASE_ENTRY(test_park_pumps_window_messages),
    TEST_CASE_ENTRY(test_single_post_runs),
    TEST_CASE_ENTRY(test_park_once_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_park_once_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_post_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_post_aborts_on_deinit),
    TEST_CASE_ENTRY(test_cpp_wrapper_timer_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_set_default_timeout_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_set_default_overrides_fallback_view),
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
    TEST_CASE_ENTRY(test_cpp_wrapper_current_post_runs_on_defaulted_parker),
    TEST_CASE_ENTRY(test_cpp_wrapper_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_await_runs),
    TEST_CASE_ENTRY(test_cpp_wrapper_lambda_await_aborts_on_deinit),
    TEST_CASE_ENTRY(test_win32_example_button_click_writes_sample_file),
#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)
    TEST_CASE_ENTRY(test_cpp_promise_resume_on_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_start_failed),
    TEST_CASE_ENTRY(test_cpp_promise_make_awaitable_aborted),
    TEST_CASE_ENTRY(test_cpp_promise_make_callback_promise_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_callback_promise_void_runs),
    TEST_CASE_ENTRY(test_cpp_promise_make_callback_promise_start_failed),
    TEST_CASE_ENTRY(test_cpp_promise_nested_value_runs),
    TEST_CASE_ENTRY(test_cpp_promise_exception_propagates),
    TEST_CASE_ENTRY(test_cpp_promise_await_canceled_runs),
    TEST_CASE_ENTRY(test_cpp_promise_handle_await_runs),
#endif
    TEST_CASE_ENTRY(test_park_once_returns_before_timeout_completion),
    TEST_CASE_ENTRY(test_park_once_nested_post_inlines),
    TEST_CASE_ENTRY(test_park_once_nested_post_falls_back_at_depth_limit),
    TEST_CASE_ENTRY(test_single_await_runs),
    TEST_CASE_ENTRY(test_single_timeout_runs),
    TEST_CASE_ENTRY(test_post_then_await_order),
    TEST_CASE_ENTRY(test_nested_post_inline_depth_benchmark),
    TEST_CASE_ENTRY(test_timeout_can_rearm_after_completion),
    TEST_CASE_ENTRY(test_timeout_rejects_second_active_wait),
    TEST_CASE_ENTRY(test_timeout_cancel_completes_canceled),
    TEST_CASE_ENTRY(test_timeout_completion_can_deinit_timer),
    TEST_CASE_ENTRY(test_await_overflow_completes_after_dynamic_growth),
    TEST_CASE_ENTRY(test_waiter_sharding_completes_over_single_wait_set),
    TEST_CASE_ENTRY(test_multi_parker_await_stress),
    TEST_CASE_ENTRY(test_register_canceled_completes_canceled),
    TEST_CASE_ENTRY(test_register_canceled_with_already_canceled_source_completes_canceled),
    TEST_CASE_ENTRY(test_unregister_canceled_prevents_callback),
    TEST_CASE_ENTRY(test_registration_deinit_aborts_pending_callback),
    TEST_CASE_ENTRY(test_await_cancel_completes_canceled),
    TEST_CASE_ENTRY(test_await_with_already_canceled_source_completes_canceled),
    TEST_CASE_ENTRY(test_await_completion_wins_over_later_cancel),
    TEST_CASE_ENTRY(test_shared_cancellation_cancels_all_waits),
    TEST_CASE_ENTRY(test_repeated_cancel_is_harmless)
  };

  return test_run_suite(
    TEST_WIN32_SUITE_NAME,
    cases,
    sizeof cases / sizeof cases[0]);
}
