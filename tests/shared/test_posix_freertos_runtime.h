#ifndef LIBBOUNCE_TEST_POSIX_FREERTOS_RUNTIME_H
#define LIBBOUNCE_TEST_POSIX_FREERTOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#if defined(BOUNCE_FREERTOS)
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#else
#include <pthread.h>
#endif

#include "libbounce/bounce.h"

#if defined(BOUNCE_FREERTOS)
/*
 * The inline-depth benchmark can recurse through completion execution multiple
 * levels on the parker task. Match the C++ runtime parker headroom so the C
 * runtime does not hit a host-side stack fault first.
 */
#define TEST_POSIX_FREERTOS_RUNTIME_PARKER_STACK_DEPTH ( configMINIMAL_STACK_SIZE * 8u )
#endif

#if defined(BOUNCE_FREERTOS)
typedef TaskHandle_t TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID;
#else
typedef pthread_t TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID;
#endif

typedef struct TEST_POSIX_FREERTOS_RUNTIME_SIGNAL {
  volatile unsigned int sequence;
#if defined(BOUNCE_FREERTOS)
  SemaphoreHandle_t semaphore;
  StaticSemaphore_t semaphore_buffer;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
#endif
} TEST_POSIX_FREERTOS_RUNTIME_SIGNAL;

typedef struct TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT {
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL signal;
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID callback_executor;
  volatile unsigned int callback_executor_set;
  volatile unsigned int call_count;
  volatile int result;
  volatile unsigned int order;
  volatile unsigned int *next_order;
} TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT;

typedef struct TEST_POSIX_FREERTOS_RUNTIME_PARKER {
  BOUNCE_CORE *bounce;
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL started_signal;
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL finished_signal;
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID parker_executor;
  unsigned int max_inline_depth;
  volatile unsigned int started;
  volatile unsigned int finished;
  volatile unsigned int park_result;
#if defined(BOUNCE_FREERTOS)
  TaskHandle_t task_handle;
  StaticTask_t task_tcb;
  StackType_t task_stack[ TEST_POSIX_FREERTOS_RUNTIME_PARKER_STACK_DEPTH ];
#else
  pthread_t thread;
#endif
} TEST_POSIX_FREERTOS_RUNTIME_PARKER;

extern bool test_posix_freertos_runtime_signal_init(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal);
extern void test_posix_freertos_runtime_signal_destroy(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal);
extern void test_posix_freertos_runtime_signal_reset(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal);
extern void test_posix_freertos_runtime_signal_notify(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal);
extern unsigned int test_posix_freertos_runtime_signal_checkpoint(
  const TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal);
extern bool test_posix_freertos_runtime_signal_wait_for_change(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal,
  unsigned int checkpoint,
  unsigned int timeout_ms);
extern bool test_posix_freertos_runtime_signal_wait(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal,
  unsigned int timeout_ms);

extern bool test_posix_freertos_runtime_completion_context_init(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  volatile unsigned int *next_order);
extern void test_posix_freertos_runtime_completion_context_destroy(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern void test_posix_freertos_runtime_record_completion(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result);
extern bool test_posix_freertos_runtime_wait_completion_count(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms);
extern bool test_posix_freertos_runtime_wait_no_additional_completion(
  TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms);
extern unsigned int test_posix_freertos_runtime_completion_call_count(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern int test_posix_freertos_runtime_completion_result(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern unsigned int test_posix_freertos_runtime_completion_order(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern bool test_posix_freertos_runtime_completion_has_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern bool test_posix_freertos_runtime_completion_ran_on_parker(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker);
extern bool test_posix_freertos_runtime_completion_ran_on_current_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context);
extern size_t test_posix_freertos_runtime_find_current_parker_index(
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count);

extern bool test_posix_freertos_runtime_start_parker(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms);
extern bool test_posix_freertos_runtime_start_parker_ex(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms,
  unsigned int max_inline_depth);
extern bool test_posix_freertos_runtime_stop_parker(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms);
extern bool test_posix_freertos_runtime_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count,
  unsigned int timeout_ms);

#endif
