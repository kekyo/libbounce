/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <string.h>

#include "shared/test_posix_freertos_runtime.h"

static TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID
test_freertos_runtime_current_executor(void) {
  return xTaskGetCurrentTaskHandle();
}

static bool test_freertos_runtime_executor_equals(
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID lhs,
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID rhs) {
  return lhs == rhs;
}

static TickType_t test_freertos_runtime_timeout_ticks(unsigned int timeout_ms) {
  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

  if (timeout_ticks == 0u) {
    timeout_ticks = 1u;
  }
  return timeout_ticks;
}

static void test_freertos_runtime_clear_signal(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  if (signal->semaphore == NULL) {
    return;
  }

  while (xSemaphoreTake(signal->semaphore, 0u) == pdTRUE) {
  }
}

static unsigned int test_freertos_runtime_atomic_load_uint(
  const volatile unsigned int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static int test_freertos_runtime_atomic_load_int(
  const volatile int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

bool test_posix_freertos_runtime_signal_init(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  memset(signal, 0, sizeof *signal);
  signal->semaphore = xSemaphoreCreateBinaryStatic(&signal->semaphore_buffer);
  if (signal->semaphore == NULL) {
    return false;
  }
  test_freertos_runtime_clear_signal(signal);
  return true;
}

void test_posix_freertos_runtime_signal_destroy(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  if (signal->semaphore != NULL) {
    vSemaphoreDelete(signal->semaphore);
    signal->semaphore = NULL;
  }
}

void test_posix_freertos_runtime_signal_reset(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  test_freertos_runtime_clear_signal(signal);
}

void test_posix_freertos_runtime_signal_notify(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  (void)__atomic_add_fetch(&signal->sequence, 1u, __ATOMIC_RELEASE);
  if (signal->semaphore != NULL) {
    (void)xSemaphoreGive(signal->semaphore);
  }
}

unsigned int test_posix_freertos_runtime_signal_checkpoint(
  const TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  return test_freertos_runtime_atomic_load_uint(&signal->sequence);
}

bool test_posix_freertos_runtime_signal_wait_for_change(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal,
  unsigned int checkpoint,
  unsigned int timeout_ms) {
  if (test_freertos_runtime_atomic_load_uint(&signal->sequence) != checkpoint) {
    return true;
  }
  return (signal->semaphore != NULL) &&
         (xSemaphoreTake(signal->semaphore, test_freertos_runtime_timeout_ticks(timeout_ms)) ==
          pdTRUE);
}

bool test_posix_freertos_runtime_signal_wait(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal,
  unsigned int timeout_ms) {
  return test_posix_freertos_runtime_signal_wait_for_change(
    signal,
    test_posix_freertos_runtime_signal_checkpoint(signal),
    timeout_ms);
}

bool test_posix_freertos_runtime_completion_context_init(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  volatile unsigned int *next_order) {
  memset(context, 0, sizeof *context);
  if (!test_posix_freertos_runtime_signal_init(&context->signal)) {
    return false;
  }
  context->result = -1;
  context->next_order = next_order;
  return true;
}

void test_posix_freertos_runtime_completion_context_destroy(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  test_posix_freertos_runtime_signal_destroy(&context->signal);
}

void test_posix_freertos_runtime_record_completion(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  const TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID executor =
    test_freertos_runtime_current_executor();

  if (context->next_order != NULL) {
    const unsigned int order = __atomic_add_fetch(context->next_order, 1u, __ATOMIC_ACQ_REL);

    __atomic_store_n(&context->order, order, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&context->result, (int)result, __ATOMIC_RELEASE);
  context->callback_executor = executor;
  __atomic_store_n(&context->callback_executor_set, 1u, __ATOMIC_RELEASE);
  (void)__atomic_add_fetch(&context->call_count, 1u, __ATOMIC_RELEASE);
  test_posix_freertos_runtime_signal_notify(&context->signal);
}

bool test_posix_freertos_runtime_wait_completion_count(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  while (test_freertos_runtime_atomic_load_uint(&context->call_count) < expected_call_count) {
    if (!test_posix_freertos_runtime_signal_wait(&context->signal, timeout_ms)) {
      return false;
    }
  }
  return true;
}

bool test_posix_freertos_runtime_wait_no_additional_completion(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  if (test_freertos_runtime_atomic_load_uint(&context->call_count) != expected_call_count) {
    return false;
  }
  test_posix_freertos_runtime_signal_reset(&context->signal);
  if (test_freertos_runtime_atomic_load_uint(&context->call_count) != expected_call_count) {
    return false;
  }
  if (test_posix_freertos_runtime_signal_wait(&context->signal, timeout_ms)) {
    return false;
  }
  return test_freertos_runtime_atomic_load_uint(&context->call_count) == expected_call_count;
}

unsigned int test_posix_freertos_runtime_completion_call_count(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_freertos_runtime_atomic_load_uint(&context->call_count);
}

int test_posix_freertos_runtime_completion_result(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_freertos_runtime_atomic_load_int(&context->result);
}

unsigned int test_posix_freertos_runtime_completion_order(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_freertos_runtime_atomic_load_uint(&context->order);
}

bool test_posix_freertos_runtime_completion_has_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_freertos_runtime_atomic_load_uint(&context->callback_executor_set) != 0u;
}

static TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID
test_freertos_runtime_completion_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return context->callback_executor;
}

bool test_posix_freertos_runtime_completion_ran_on_parker(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker) {
  return test_posix_freertos_runtime_completion_has_executor(context) &&
         test_freertos_runtime_executor_equals(
           test_freertos_runtime_completion_executor(context),
           parker->parker_executor);
}

bool test_posix_freertos_runtime_completion_ran_on_current_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_has_executor(context) &&
         test_freertos_runtime_executor_equals(
           test_freertos_runtime_completion_executor(context),
           test_freertos_runtime_current_executor());
}

size_t test_posix_freertos_runtime_find_current_parker_index(
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count) {
  const TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID current_executor =
    test_freertos_runtime_current_executor();

  for (size_t index = 0u; index < parker_count; index++) {
    if (test_freertos_runtime_executor_equals(
          parkers[index].parker_executor,
          current_executor)) {
      return index;
    }
  }
  return parker_count;
}

static void test_freertos_runtime_mark_started(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker) {
  parker->parker_executor = test_freertos_runtime_current_executor();
  __atomic_store_n(&parker->started, 1u, __ATOMIC_RELEASE);
  test_posix_freertos_runtime_signal_notify(&parker->started_signal);
}

static void test_freertos_runtime_mark_finished(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  bool park_result) {
  __atomic_store_n(&parker->park_result, park_result ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&parker->finished, 1u, __ATOMIC_RELEASE);
  test_posix_freertos_runtime_signal_notify(&parker->finished_signal);
}

static void test_freertos_runtime_park_task(void *parameter) {
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker = parameter;
  BOUNCE_CORE *previous_core = bounce_get_core();

  bounce_set_core(parker->bounce);
  test_freertos_runtime_mark_started(parker);
  test_freertos_runtime_mark_finished(
    parker,
    bounce_park(parker->bounce, parker->max_inline_depth));
  bounce_set_core(previous_core);
  vTaskDelete(NULL);

  for (;;) {
  }
}

static bool test_freertos_runtime_wait_for_started(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  while (__atomic_load_n(&parker->started, __ATOMIC_ACQUIRE) == 0u) {
    if (!test_posix_freertos_runtime_signal_wait(&parker->started_signal, timeout_ms)) {
      return false;
    }
  }
  return true;
}

static bool test_freertos_runtime_wait_for_finished(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  while (__atomic_load_n(&parker->finished, __ATOMIC_ACQUIRE) == 0u) {
    if (!test_posix_freertos_runtime_signal_wait(&parker->finished_signal, timeout_ms)) {
      return false;
    }
  }
  return true;
}

static bool test_freertos_runtime_wait_for_task_deleted(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
#if (INCLUDE_eTaskGetState == 1)
  const TickType_t timeout_ticks = test_freertos_runtime_timeout_ticks(timeout_ms);
  const TickType_t started = xTaskGetTickCount();

  while ((parker->task_handle != NULL) && (eTaskGetState(parker->task_handle) != eDeleted)) {
    if ((xTaskGetTickCount() - started) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1u);
  }
#else
  (void)parker;
  (void)timeout_ms;
#endif
  return true;
}

static bool test_freertos_runtime_wait_for_idle_reclaim(
  unsigned int timeout_ms) {
  const TickType_t timeout_ticks = test_freertos_runtime_timeout_ticks(timeout_ms);
  const TickType_t started = xTaskGetTickCount();

  /*
   * The POSIX port stores pthread bookkeeping in the caller-provided task stack
   * buffer and reclaims it from the idle task via portCLEAN_UP_TCB(). Give the
   * idle task time to run before the test reuses the same static parker
   * storage, otherwise the next benchmark iteration can race with deferred
   * cleanup and crash.
   */
  for (unsigned int index = 0u; index < 2u; index++) {
    if ((xTaskGetTickCount() - started) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1u);
  }

  return true;
}

static void test_freertos_runtime_destroy_parker(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker) {
  test_posix_freertos_runtime_signal_destroy(&parker->finished_signal);
  test_posix_freertos_runtime_signal_destroy(&parker->started_signal);
}

bool test_posix_freertos_runtime_start_parker_ex(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms,
  unsigned int max_inline_depth);

bool test_posix_freertos_runtime_start_parker(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  return test_posix_freertos_runtime_start_parker_ex(bounce, parker, timeout_ms, 0u);
}

bool test_posix_freertos_runtime_start_parker_ex(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms,
  unsigned int max_inline_depth) {
  memset(parker, 0, sizeof *parker);
  parker->bounce = bounce;
  parker->max_inline_depth = max_inline_depth;
  if (!test_posix_freertos_runtime_signal_init(&parker->started_signal)) {
    return false;
  }
  if (!test_posix_freertos_runtime_signal_init(&parker->finished_signal)) {
    test_posix_freertos_runtime_signal_destroy(&parker->started_signal);
    return false;
  }

  parker->task_handle = xTaskCreateStatic(
    test_freertos_runtime_park_task,
    "park",
    TEST_POSIX_FREERTOS_RUNTIME_PARKER_STACK_DEPTH,
    parker,
    (configMAX_PRIORITIES - 2u),
    &parker->task_stack[0],
    &parker->task_tcb);
  if (parker->task_handle == NULL) {
    test_freertos_runtime_destroy_parker(parker);
    return false;
  }
  if (!test_freertos_runtime_wait_for_started(parker, timeout_ms)) {
    bounce_shutdown(bounce, false);
    (void)test_freertos_runtime_wait_for_finished(parker, timeout_ms);
    (void)test_freertos_runtime_wait_for_task_deleted(parker, timeout_ms);
    test_freertos_runtime_destroy_parker(parker);
    return false;
  }
  return true;
}

bool test_posix_freertos_runtime_stop_parker(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  bounce_shutdown(bounce, false);
  if (!test_freertos_runtime_wait_for_finished(parker, timeout_ms)) {
    return false;
  }
  if (!test_freertos_runtime_wait_for_task_deleted(parker, timeout_ms)) {
    return false;
  }
  if (!test_freertos_runtime_wait_for_idle_reclaim(timeout_ms)) {
    return false;
  }
  test_freertos_runtime_destroy_parker(parker);
  return test_freertos_runtime_atomic_load_uint(&parker->park_result) != 0u;
}

bool test_posix_freertos_runtime_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count,
  unsigned int timeout_ms) {
  bounce_shutdown(bounce, false);

  for (size_t index = 0u; index < parker_count; index++) {
    if (!test_freertos_runtime_wait_for_finished(&parkers[index], timeout_ms)) {
      return false;
    }
  }
  for (size_t index = 0u; index < parker_count; index++) {
    if (!test_freertos_runtime_wait_for_task_deleted(&parkers[index], timeout_ms)) {
      return false;
    }
  }
  if (!test_freertos_runtime_wait_for_idle_reclaim(timeout_ms)) {
    return false;
  }
  for (size_t index = 0u; index < parker_count; index++) {
    test_freertos_runtime_destroy_parker(&parkers[index]);
  }
  return true;
}
