/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <errno.h>
#include <string.h>
#include <time.h>

#include "shared/test_posix_freertos_runtime.h"

static struct timespec test_posix_runtime_deadline_after_ms(unsigned int timeout_ms) {
  struct timespec timeout;

  if (clock_gettime(CLOCK_REALTIME, &timeout) != 0) {
    memset(&timeout, 0, sizeof timeout);
    return timeout;
  }

  timeout.tv_sec += (time_t)(timeout_ms / 1000u);
  timeout.tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
  if (timeout.tv_nsec >= 1000000000L) {
    timeout.tv_sec += 1;
    timeout.tv_nsec -= 1000000000L;
  }
  return timeout;
}

static TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID
test_posix_runtime_current_executor(void) {
  return pthread_self();
}

static bool test_posix_runtime_executor_equals(
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID lhs,
  TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID rhs) {
  return pthread_equal(lhs, rhs) != 0;
}

static unsigned int test_posix_runtime_atomic_load_uint(
  const volatile unsigned int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static int test_posix_runtime_atomic_load_int(
  const volatile int *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

bool test_posix_freertos_runtime_signal_init(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  memset(signal, 0, sizeof *signal);
  if (pthread_mutex_init(&signal->mutex, NULL) != 0) {
    return false;
  }
  if (pthread_cond_init(&signal->cond, NULL) != 0) {
    (void)pthread_mutex_destroy(&signal->mutex);
    return false;
  }
  return true;
}

void test_posix_freertos_runtime_signal_destroy(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  (void)pthread_cond_destroy(&signal->cond);
  (void)pthread_mutex_destroy(&signal->mutex);
}

void test_posix_freertos_runtime_signal_reset(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  (void)signal;
}

void test_posix_freertos_runtime_signal_notify(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  (void)pthread_mutex_lock(&signal->mutex);
  (void)__atomic_add_fetch(&signal->sequence, 1u, __ATOMIC_RELEASE);
  (void)pthread_cond_broadcast(&signal->cond);
  (void)pthread_mutex_unlock(&signal->mutex);
}

unsigned int test_posix_freertos_runtime_signal_checkpoint(
  const TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal) {
  return test_posix_runtime_atomic_load_uint(&signal->sequence);
}

bool test_posix_freertos_runtime_signal_wait_for_change(
  TEST_POSIX_FREERTOS_RUNTIME_SIGNAL *signal,
  unsigned int checkpoint,
  unsigned int timeout_ms) {
  const struct timespec timeout = test_posix_runtime_deadline_after_ms(timeout_ms);

  if (pthread_mutex_lock(&signal->mutex) != 0) {
    return false;
  }
  while (test_posix_runtime_atomic_load_uint(&signal->sequence) == checkpoint) {
    const int wait_result = pthread_cond_timedwait(&signal->cond, &signal->mutex, &timeout);

    if ((wait_result != 0) && (wait_result != ETIMEDOUT)) {
      (void)pthread_mutex_unlock(&signal->mutex);
      return false;
    }
    if (wait_result == ETIMEDOUT) {
      (void)pthread_mutex_unlock(&signal->mutex);
      return false;
    }
  }
  return pthread_mutex_unlock(&signal->mutex) == 0;
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
    test_posix_runtime_current_executor();

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
  while (test_posix_runtime_atomic_load_uint(&context->call_count) < expected_call_count) {
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
  if (test_posix_runtime_atomic_load_uint(&context->call_count) != expected_call_count) {
    return false;
  }
  if (test_posix_freertos_runtime_signal_wait(&context->signal, timeout_ms)) {
    return false;
  }
  return test_posix_runtime_atomic_load_uint(&context->call_count) == expected_call_count;
}

unsigned int test_posix_freertos_runtime_completion_call_count(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_runtime_atomic_load_uint(&context->call_count);
}

int test_posix_freertos_runtime_completion_result(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_runtime_atomic_load_int(&context->result);
}

unsigned int test_posix_freertos_runtime_completion_order(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_runtime_atomic_load_uint(&context->order);
}

bool test_posix_freertos_runtime_completion_has_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_runtime_atomic_load_uint(&context->callback_executor_set) != 0u;
}

static TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID
test_posix_runtime_completion_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return context->callback_executor;
}

bool test_posix_freertos_runtime_completion_ran_on_parker(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker) {
  return test_posix_freertos_runtime_completion_has_executor(context) &&
         test_posix_runtime_executor_equals(
           test_posix_runtime_completion_executor(context),
           parker->parker_executor);
}

bool test_posix_freertos_runtime_completion_ran_on_current_executor(
  const TEST_POSIX_FREERTOS_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_posix_freertos_runtime_completion_has_executor(context) &&
         test_posix_runtime_executor_equals(
           test_posix_runtime_completion_executor(context),
           test_posix_runtime_current_executor());
}

size_t test_posix_freertos_runtime_find_current_parker_index(
  const TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count) {
  const TEST_POSIX_FREERTOS_RUNTIME_EXECUTOR_ID current_executor =
    test_posix_runtime_current_executor();

  for (size_t index = 0u; index < parker_count; index++) {
    if (test_posix_runtime_executor_equals(
          parkers[index].parker_executor,
          current_executor)) {
      return index;
    }
  }
  return parker_count;
}

static void test_posix_runtime_mark_started(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker) {
  parker->parker_executor = test_posix_runtime_current_executor();
  __atomic_store_n(&parker->started, 1u, __ATOMIC_RELEASE);
  test_posix_freertos_runtime_signal_notify(&parker->started_signal);
}

static void test_posix_runtime_mark_finished(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  bool park_result) {
  __atomic_store_n(&parker->park_result, park_result ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&parker->finished, 1u, __ATOMIC_RELEASE);
  test_posix_freertos_runtime_signal_notify(&parker->finished_signal);
}

static void *test_posix_runtime_park_thread(void *parameter) {
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker = parameter;
  BOUNCE_CORE *previous_core = bounce_get_core();

  bounce_set_core(parker->bounce);
  test_posix_runtime_mark_started(parker);
  test_posix_runtime_mark_finished(
    parker,
    bounce_park(parker->bounce, parker->max_inline_depth));
  bounce_set_core(previous_core);
  return NULL;
}

static bool test_posix_runtime_wait_for_started(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  while (__atomic_load_n(&parker->started, __ATOMIC_ACQUIRE) == 0u) {
    if (!test_posix_freertos_runtime_signal_wait(&parker->started_signal, timeout_ms)) {
      return false;
    }
  }
  return true;
}

static bool test_posix_runtime_wait_for_finished(
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  while (__atomic_load_n(&parker->finished, __ATOMIC_ACQUIRE) == 0u) {
    if (!test_posix_freertos_runtime_signal_wait(&parker->finished_signal, timeout_ms)) {
      return false;
    }
  }
  return true;
}

static void test_posix_runtime_destroy_parker(
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
  if (pthread_create(&parker->thread, NULL, test_posix_runtime_park_thread, parker) != 0) {
    test_posix_runtime_destroy_parker(parker);
    return false;
  }
  if (!test_posix_runtime_wait_for_started(parker, timeout_ms)) {
    bounce_shutdown(bounce, false);
    (void)pthread_join(parker->thread, NULL);
    test_posix_runtime_destroy_parker(parker);
    return false;
  }
  return true;
}

bool test_posix_freertos_runtime_stop_parker(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parker,
  unsigned int timeout_ms) {
  bounce_shutdown(bounce, false);
  if (!test_posix_runtime_wait_for_finished(parker, timeout_ms)) {
    return false;
  }
  if (pthread_join(parker->thread, NULL) != 0) {
    return false;
  }
  test_posix_runtime_destroy_parker(parker);
  return true;
}

bool test_posix_freertos_runtime_stop_parkers(
  BOUNCE_CORE *bounce,
  TEST_POSIX_FREERTOS_RUNTIME_PARKER *parkers,
  size_t parker_count,
  unsigned int timeout_ms) {
  bounce_shutdown(bounce, false);

  for (size_t index = 0u; index < parker_count; index++) {
    if (!test_posix_runtime_wait_for_finished(&parkers[index], timeout_ms)) {
      return false;
    }
  }
  for (size_t index = 0u; index < parker_count; index++) {
    if (pthread_join(parkers[index].thread, NULL) != 0) {
      return false;
    }
    test_posix_runtime_destroy_parker(&parkers[index]);
  }
  return true;
}
