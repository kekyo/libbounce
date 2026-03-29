/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "test_harness.h"
#include "shared/test_posix_freertos_runtime.h"
#include "shared/test_posix_freertos_shared.h"
#include "libbounce/bounce.h"
#include "libbounce/freertos.h"

#define TEST_MAIN_TASK_STACK_DEPTH ( configMINIMAL_STACK_SIZE * 64u )
#define TEST_TIMEOUT_MS 5000u

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      abort();                                                                   \
    }                                                                            \
  } while (0)

typedef TEST_POSIX_FREERTOS_RUNTIME_PARKER TEST_PARK_THREAD_CONTEXT;
typedef TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT TEST_COMPLETION_CONTEXT;

extern void test_cpp_wrapper_condition_raise_from_isr_runs(void);
extern void test_cpp_wrapper_timer_await_runs(void);
extern void test_cpp_wrapper_attach_current_timeout_await_runs(void);

static StaticTask_t g_main_task_tcb;
static StackType_t g_main_task_stack[ TEST_MAIN_TASK_STACK_DEPTH ];
static volatile int g_exit_code = 1;
static const TEST_POSIX_FREERTOS_SHARED_CONFIG g_shared_test_config = {
  "freertos",
  true,
  false,
  true,
  true,
  false
};

static unsigned int test_atomic_load_uint(const volatile unsigned int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void test_completion_context_init(TEST_COMPLETION_CONTEXT *context) {
  ASSERT_TRUE(test_posix_freertos_runtime_completion_context_init(context, NULL));
}

static void test_completion_context_destroy(TEST_COMPLETION_CONTEXT *context) {
  test_posix_freertos_runtime_completion_context_destroy(context);
}

static void test_completion_callback(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  test_posix_freertos_runtime_record_completion(completion_state, result);
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

static void test_completion_context_array_destroy(
  TEST_COMPLETION_CONTEXT *contexts,
  unsigned int context_count) {
  for (unsigned int index = 0u; index < context_count; index++) {
    test_completion_context_destroy(&contexts[index]);
  }
}

static void test_condition_wait_burst_completes(
  bool raise_from_isr,
  unsigned int wait_count) {
  BOUNCE_CORE bounce;
  BOUNCE_FREERTOS_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT
    completion_contexts[ BOUNCE_MAX_STATIC_COMPLETION_ITEMS + 1u ];

  ASSERT_TRUE(wait_count <= (BOUNCE_MAX_STATIC_COMPLETION_ITEMS + 1u));

  bounce_init(&bounce);
  bounce_freertos_condition_init(&condition);
  ASSERT_TRUE(test_posix_freertos_runtime_start_parker(&bounce, &park_context, TEST_TIMEOUT_MS));

  for (unsigned int index = 0u; index < wait_count; index++) {
    test_completion_context_init(&completion_contexts[index]);
    bounce_await_freertos_condition(
      &bounce,
      &condition,
      test_completion_callback,
      &completion_contexts[index],
      NULL);
  }

  if (raise_from_isr) {
    bounce_freertos_condition_raise_from_isr(&bounce, &condition);
  } else {
    bounce_freertos_condition_raise(&bounce, &condition);
  }

  for (unsigned int index = 0u; index < wait_count; index++) {
    test_wait_completion_count(&completion_contexts[index], 1u);
    ASSERT_TRUE(
      test_posix_freertos_runtime_completion_call_count(&completion_contexts[index]) == 1u);
    ASSERT_TRUE(
      test_posix_freertos_runtime_completion_result(&completion_contexts[index]) ==
      BOUNCE_COMPLETION_COMPLETED);
    ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
      &completion_contexts[index],
      &park_context));
  }

  test_completion_context_array_destroy(&completion_contexts[0], wait_count);
  ASSERT_TRUE(test_posix_freertos_runtime_stop_parker(&bounce, &park_context, TEST_TIMEOUT_MS));
  ASSERT_TRUE(test_atomic_load_uint(&park_context.park_result) != 0u);
  bounce_deinit(&bounce);
}

static void test_condition_raise_from_isr_completes_wait_burst(void) {
  test_condition_wait_burst_completes(
    true,
    BOUNCE_MAX_STATIC_COMPLETION_ITEMS);
}

static void test_task_context_waits_can_grow_beyond_static_capacity(void) {
#if (BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP == 0)
  return;
#else
  test_condition_wait_burst_completes(
    false,
    BOUNCE_MAX_STATIC_COMPLETION_ITEMS + 1u);
#endif
}

static void test_single_condition_await_runs_from_isr(void) {
  BOUNCE_CORE bounce;
  BOUNCE_FREERTOS_CONDITION condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  bounce_init(&bounce);
  bounce_freertos_condition_init(&condition);
  ASSERT_TRUE(test_posix_freertos_runtime_start_parker(&bounce, &park_context, TEST_TIMEOUT_MS));

  test_completion_context_init(&completion_context);
  bounce_await_freertos_condition(
    &bounce,
    &condition,
    test_completion_callback,
    &completion_context,
    NULL);
  bounce_freertos_condition_raise_from_isr(&bounce, &condition);
  test_wait_completion_count(&completion_context, 1u);

  ASSERT_TRUE(test_posix_freertos_runtime_completion_call_count(&completion_context) == 1u);
  ASSERT_TRUE(
    test_posix_freertos_runtime_completion_result(&completion_context) ==
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(test_posix_freertos_runtime_completion_ran_on_parker(
    &completion_context,
    &park_context));

  test_completion_context_destroy(&completion_context);
  ASSERT_TRUE(test_posix_freertos_runtime_stop_parker(&bounce, &park_context, TEST_TIMEOUT_MS));
  ASSERT_TRUE(test_atomic_load_uint(&park_context.park_result) != 0u);
  bounce_deinit(&bounce);
}

static void test_cpp_runtime_reclaims_stack_backed_tasks_before_reuse(void) {
  for (unsigned int iteration = 0u; iteration < 32u; iteration++) {
    test_cpp_wrapper_timer_await_runs();
    test_cpp_wrapper_attach_current_timeout_await_runs();
  }
}

static int test_run_freertos_local_suite(void) {
  static const TEST_CASE cases[] = {
    TEST_CASE_ENTRY(test_condition_raise_from_isr_completes_wait_burst),
    TEST_CASE_ENTRY(test_task_context_waits_can_grow_beyond_static_capacity),
    TEST_CASE_ENTRY(test_single_condition_await_runs_from_isr),
    TEST_CASE_ENTRY(test_cpp_runtime_reclaims_stack_backed_tasks_before_reuse),
    TEST_CASE_ENTRY(test_cpp_wrapper_condition_raise_from_isr_runs)
  };

  return test_run_suite(
    g_shared_test_config.suite_name,
    cases,
    sizeof cases / sizeof cases[0]);
}

static void test_main_task(void *parameter) {
  int exit_code;

  (void)parameter;

  exit_code = test_run_posix_freertos_shared_suite(&g_shared_test_config);
  if (exit_code == 0) {
    exit_code = test_run_freertos_local_suite();
  }
  g_exit_code = exit_code;
  vTaskEndScheduler();

  for (;;) {
  }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name) {
  fprintf(
    stderr,
    "FreeRTOS stack overflow detected in task %s (%p)\n",
    (task_name != NULL) ? task_name : "<unknown>",
    (void *)task);
  abort();
}

int main(void) {
  (void)xTaskCreateStatic(
    test_main_task,
    "freertos-main",
    TEST_MAIN_TASK_STACK_DEPTH,
    NULL,
    (configMAX_PRIORITIES - 1u),
    &g_main_task_stack[0],
    &g_main_task_tcb);

  vTaskStartScheduler();
  return g_exit_code;
}
