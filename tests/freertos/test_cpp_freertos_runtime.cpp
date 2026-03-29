/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_FREERTOS)

#include <string.h>

#include "shared/test_cpp_runtime.h"

static TickType_t test_cpp_runtime_timeout_ticks(unsigned int timeout_ms) {
  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

  if (timeout_ticks == 0u) {
    timeout_ticks = 1u;
  }
  return timeout_ticks;
}

static void test_cpp_runtime_clear_semaphore(SemaphoreHandle_t semaphore) {
  if (semaphore == NULL) {
    return;
  }

  while (xSemaphoreTake(semaphore, 0u) == pdTRUE) {
  }
}

static unsigned int test_cpp_runtime_atomic_load_uint(
  const volatile unsigned int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static int test_cpp_runtime_atomic_load_int(
  const volatile int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_atomic_load_executor(
  const TEST_CPP_RUNTIME_EXECUTOR_ID *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_current_executor(void) {
  return xTaskGetCurrentTaskHandle();
}

bool test_cpp_runtime_executor_equals(
  TEST_CPP_RUNTIME_EXECUTOR_ID lhs,
  TEST_CPP_RUNTIME_EXECUTOR_ID rhs) {
  return lhs == rhs;
}

void test_cpp_runtime_completion_context_init(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  bool waitable) {
  (void)waitable;
  if (context->signal_semaphore == NULL) {
    context->signal_semaphore = xSemaphoreCreateBinaryStatic(&context->signal_semaphore_buffer);
  }
  context->signal_sequence = 0u;
  context->call_count = 0u;
  context->result = (int)BOUNCE_COMPLETION_ABORTED;
  context->callback_executor = TEST_CPP_RUNTIME_EXECUTOR_ID();
  context->callback_executor_set = 0u;
  test_cpp_runtime_clear_semaphore(context->signal_semaphore);
}

void test_cpp_runtime_record_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  __atomic_add_fetch(&context->signal_sequence, 1u, __ATOMIC_ACQ_REL);
  __atomic_store_n(&context->result, (int)result, __ATOMIC_RELEASE);
  __atomic_store_n(
    &context->callback_executor,
    xTaskGetCurrentTaskHandle(),
    __ATOMIC_RELEASE);
  __atomic_store_n(&context->callback_executor_set, 1u, __ATOMIC_RELEASE);
  (void)__atomic_add_fetch(&context->call_count, 1u, __ATOMIC_ACQ_REL);
  (void)xSemaphoreGive(context->signal_semaphore);
}

bool test_cpp_runtime_wait_completion_count(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);

  while (test_cpp_runtime_atomic_load_uint(&context->call_count) < expected_call_count) {
    if (xSemaphoreTake(context->signal_semaphore, timeout_ticks) != pdTRUE) {
      return false;
    }
  }
  test_cpp_runtime_clear_semaphore(context->signal_semaphore);
  return true;
}

bool test_cpp_runtime_wait_no_additional_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  const unsigned int checkpoint =
    test_cpp_runtime_atomic_load_uint(&context->signal_sequence);

  if (test_cpp_runtime_atomic_load_uint(&context->call_count) != expected_call_count) {
    return false;
  }
  test_cpp_runtime_clear_semaphore(context->signal_semaphore);
  if (test_cpp_runtime_atomic_load_uint(&context->call_count) != expected_call_count) {
    return false;
  }
  if (test_cpp_runtime_atomic_load_uint(&context->signal_sequence) != checkpoint) {
    return false;
  }
  if (xSemaphoreTake(context->signal_semaphore, test_cpp_runtime_timeout_ticks(timeout_ms)) != pdFALSE) {
    return false;
  }
  return test_cpp_runtime_atomic_load_uint(&context->call_count) == expected_call_count;
}

unsigned int test_cpp_runtime_completion_call_count(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_cpp_runtime_atomic_load_uint(&context->call_count);
}

BOUNCE_COMPLETION_RESULT test_cpp_runtime_completion_result(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  return (BOUNCE_COMPLETION_RESULT)test_cpp_runtime_atomic_load_int(&context->result);
}

bool test_cpp_runtime_completion_has_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_cpp_runtime_atomic_load_uint(&context->callback_executor_set) != 0u;
}

void test_cpp_runtime_destruction_counter_init(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  if (counter->signal_semaphore == NULL) {
    counter->signal_semaphore = xSemaphoreCreateBinaryStatic(&counter->signal_semaphore_buffer);
  }
  counter->signal_sequence = 0u;
  counter->count = 0;
  test_cpp_runtime_clear_semaphore(counter->signal_semaphore);
}

void test_cpp_runtime_destruction_counter_increment(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  __atomic_add_fetch(&counter->signal_sequence, 1u, __ATOMIC_ACQ_REL);
  (void)__atomic_add_fetch(&counter->count, 1, __ATOMIC_ACQ_REL);
  (void)xSemaphoreGive(counter->signal_semaphore);
}

bool test_cpp_runtime_wait_destruction_count(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter,
  int expected_destroyed_count,
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);

  while (test_cpp_runtime_atomic_load_int(&counter->count) < expected_destroyed_count) {
    if (xSemaphoreTake(counter->signal_semaphore, timeout_ticks) != pdTRUE) {
      return false;
    }
  }
  test_cpp_runtime_clear_semaphore(counter->signal_semaphore);
  return true;
}

int test_cpp_runtime_destruction_count(
  const TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  return test_cpp_runtime_atomic_load_int(&counter->count);
}

bool test_cpp_runtime_completion_ran_on_parker(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *parker) {
  return test_cpp_runtime_completion_has_executor(context) &&
         test_cpp_runtime_executor_equals(
           test_cpp_runtime_atomic_load_executor(&context->callback_executor),
           test_cpp_runtime_atomic_load_executor(&parker->parker_executor));
}

bool test_cpp_runtime_completion_ran_on_current_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_cpp_runtime_completion_has_executor(context) &&
         test_cpp_runtime_executor_equals(
           test_cpp_runtime_atomic_load_executor(&context->callback_executor),
           test_cpp_runtime_current_executor());
}

bool test_cpp_runtime_parker_started(
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *context) {
  return test_cpp_runtime_atomic_load_uint(&context->started) != 0u;
}

static void test_cpp_runtime_mark_started(TEST_CPP_RUNTIME_PARKER_CONTEXT *context) {
  __atomic_store_n(&context->parker_executor, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
  __atomic_store_n(&context->started, 1u, __ATOMIC_RELEASE);
  if (context->started_semaphore != NULL) {
    (void)xSemaphoreGive(context->started_semaphore);
  }
}

static void test_cpp_runtime_mark_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  bool park_result) {
  __atomic_store_n(&context->park_result, park_result ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&context->finished, 1u, __ATOMIC_RELEASE);
  if (context->finished_semaphore != NULL) {
    (void)xSemaphoreGive(context->finished_semaphore);
  }
}

static void test_cpp_runtime_park_task(void *parameter) {
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context =
    static_cast<TEST_CPP_RUNTIME_PARKER_CONTEXT *>(parameter);
  auto attachment = context->bounce->attach_current();

  (void)attachment;
  test_cpp_runtime_mark_started(context);
  test_cpp_runtime_mark_finished(
    context,
    context->bounce->park(context->max_inline_depth));
  vTaskDelete(NULL);

  for (;;) {
  }
}

static bool test_cpp_runtime_wait_parker_started(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);

  while (test_cpp_runtime_atomic_load_uint(&context->started) == 0u) {
    if (xSemaphoreTake(context->started_semaphore, timeout_ticks) != pdTRUE) {
      return false;
    }
  }
  return true;
}

static bool test_cpp_runtime_wait_parker_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);

  while (test_cpp_runtime_atomic_load_uint(&context->finished) == 0u) {
    if (xSemaphoreTake(context->finished_semaphore, timeout_ticks) != pdTRUE) {
      return false;
    }
  }
  return true;
}

static bool test_cpp_runtime_wait_task_deleted(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
#if (INCLUDE_eTaskGetState == 1)
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);
  const TickType_t started = xTaskGetTickCount();

  while ((context->task_handle != NULL) && (eTaskGetState(context->task_handle) != eDeleted)) {
    if ((xTaskGetTickCount() - started) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1u);
  }
#else
  (void)context;
  (void)timeout_ms;
#endif
  return true;
}

static bool test_cpp_runtime_wait_for_idle_reclaim(
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_cpp_runtime_timeout_ticks(timeout_ms);
  const TickType_t started = xTaskGetTickCount();

  /*
   * The POSIX port stores pthread bookkeeping in the caller-provided task stack
   * buffer and reclaims it from the idle task via portCLEAN_UP_TCB(). Give the
   * idle task time to run before the test reuses the same static parker
   * storage, otherwise later iterations can race with deferred cleanup and
   * crash.
   */
  for (unsigned int index = 0u; index < 2u; index++) {
    if ((xTaskGetTickCount() - started) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1u);
  }

  return true;
}

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_start_parker_ex(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms,
  unsigned int max_inline_depth);

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_start_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  return test_cpp_runtime_start_parker_ex(
    bounce_instance,
    context,
    timeout_ms,
    0u);
}

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_start_parker_ex(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms,
  unsigned int max_inline_depth) {
  context->bounce = bounce_instance;
  context->max_inline_depth = max_inline_depth;
  context->started = 0u;
  context->finished = 0u;
  context->park_result = 0u;
  context->parker_executor = TEST_CPP_RUNTIME_EXECUTOR_ID();
  if (context->started_semaphore == NULL) {
    context->started_semaphore = xSemaphoreCreateBinaryStatic(&context->started_semaphore_buffer);
  }
  if (context->finished_semaphore == NULL) {
    context->finished_semaphore = xSemaphoreCreateBinaryStatic(&context->finished_semaphore_buffer);
  }
  test_cpp_runtime_clear_semaphore(context->started_semaphore);
  test_cpp_runtime_clear_semaphore(context->finished_semaphore);
  context->task_handle = xTaskCreateStatic(
    test_cpp_runtime_park_task,
    "cpppark",
    configMINIMAL_STACK_SIZE * 8u,
    context,
    (configMAX_PRIORITIES - 2u),
    &context->task_stack[0],
    &context->task_tcb);
  if ((context->task_handle == NULL) ||
      !test_cpp_runtime_wait_parker_started(context, timeout_ms)) {
    return TEST_CPP_RUNTIME_EXECUTOR_ID();
  }
  return test_cpp_runtime_atomic_load_executor(&context->parker_executor);
}

bool test_cpp_runtime_stop_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  bounce_instance->shutdown();
  if (!test_cpp_runtime_wait_parker_finished(context, timeout_ms)) {
    return false;
  }
  if (!test_cpp_runtime_wait_task_deleted(context, timeout_ms)) {
    return false;
  }
  if (!test_cpp_runtime_wait_for_idle_reclaim(timeout_ms)) {
    return false;
  }
  return test_cpp_runtime_atomic_load_uint(&context->park_result) != 0u;
}

#endif
