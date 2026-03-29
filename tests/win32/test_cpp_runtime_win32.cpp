/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(_WIN32) && !defined(BOUNCE_FREERTOS)

#include <limits.h>
#include <process.h>

#include "shared/test_cpp_runtime.h"

static DWORD test_cpp_runtime_start_tick(void) {
  return GetTickCount();
}

static DWORD test_cpp_runtime_remaining_ms(
  DWORD start_tick,
  unsigned int timeout_ms) {
  const DWORD elapsed_ms = GetTickCount() - start_tick;

  if (elapsed_ms >= timeout_ms) {
    return 0u;
  }

  return (DWORD)(timeout_ms - elapsed_ms);
}

static bool test_cpp_runtime_wait_handle_until(
  HANDLE handle,
  DWORD start_tick,
  unsigned int timeout_ms) {
  if (handle == NULL) {
    return false;
  }

  return WaitForSingleObject(
           handle,
           test_cpp_runtime_remaining_ms(start_tick, timeout_ms)) == WAIT_OBJECT_0;
}

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_current_executor(void) {
  return GetCurrentThreadId();
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
  EnterCriticalSection(&context->mutex);
  context->signal_sequence = 0u;
  context->call_count = 0u;
  context->result = BOUNCE_COMPLETION_ABORTED;
  context->callback_executor = 0u;
  context->callback_executor_set = false;
  ResetEvent(context->signal_event);
  LeaveCriticalSection(&context->mutex);
}

void test_cpp_runtime_record_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  EnterCriticalSection(&context->mutex);
  context->signal_sequence += 1u;
  context->call_count += 1u;
  context->result = result;
  context->callback_executor = GetCurrentThreadId();
  context->callback_executor_set = true;
  LeaveCriticalSection(&context->mutex);
  SetEvent(context->signal_event);
}

bool test_cpp_runtime_wait_completion_count(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  const DWORD start_tick = test_cpp_runtime_start_tick();

  for (;;) {
    EnterCriticalSection(&context->mutex);
    if (context->call_count >= expected_call_count) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    ResetEvent(context->signal_event);
    if (context->call_count >= expected_call_count) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    LeaveCriticalSection(&context->mutex);

    if (!test_cpp_runtime_wait_handle_until(
          context->signal_event,
          start_tick,
          timeout_ms)) {
      EnterCriticalSection(&context->mutex);
      const bool reached = context->call_count >= expected_call_count;
      LeaveCriticalSection(&context->mutex);
      return reached;
    }
  }
}

bool test_cpp_runtime_wait_no_additional_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  const DWORD start_tick = test_cpp_runtime_start_tick();

  EnterCriticalSection(&context->mutex);
  if (context->call_count != expected_call_count) {
    LeaveCriticalSection(&context->mutex);
    return false;
  }
  ResetEvent(context->signal_event);
  LeaveCriticalSection(&context->mutex);

  if (test_cpp_runtime_wait_handle_until(
        context->signal_event,
        start_tick,
        timeout_ms)) {
    return false;
  }

  EnterCriticalSection(&context->mutex);
  const bool unchanged = context->call_count == expected_call_count;
  LeaveCriticalSection(&context->mutex);
  return unchanged;
}

unsigned int test_cpp_runtime_completion_call_count(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  unsigned int call_count;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  call_count = context->call_count;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  return call_count;
}

BOUNCE_COMPLETION_RESULT test_cpp_runtime_completion_result(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  BOUNCE_COMPLETION_RESULT result;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  result = context->result;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  return result;
}

bool test_cpp_runtime_completion_has_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  bool callback_executor_set;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  callback_executor_set = context->callback_executor_set;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  return callback_executor_set;
}

void test_cpp_runtime_destruction_counter_init(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  EnterCriticalSection(&counter->mutex);
  counter->signal_sequence = 0u;
  counter->count = 0;
  ResetEvent(counter->signal_event);
  LeaveCriticalSection(&counter->mutex);
}

void test_cpp_runtime_destruction_counter_increment(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  EnterCriticalSection(&counter->mutex);
  counter->signal_sequence += 1u;
  counter->count += 1;
  LeaveCriticalSection(&counter->mutex);
  SetEvent(counter->signal_event);
}

bool test_cpp_runtime_wait_destruction_count(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter,
  int expected_destroyed_count,
  unsigned int timeout_ms) {
  const DWORD start_tick = test_cpp_runtime_start_tick();

  for (;;) {
    EnterCriticalSection(&counter->mutex);
    if (counter->count >= expected_destroyed_count) {
      LeaveCriticalSection(&counter->mutex);
      return true;
    }
    ResetEvent(counter->signal_event);
    if (counter->count >= expected_destroyed_count) {
      LeaveCriticalSection(&counter->mutex);
      return true;
    }
    LeaveCriticalSection(&counter->mutex);

    if (!test_cpp_runtime_wait_handle_until(
          counter->signal_event,
          start_tick,
          timeout_ms)) {
      EnterCriticalSection(&counter->mutex);
      const bool reached = counter->count >= expected_destroyed_count;
      LeaveCriticalSection(&counter->mutex);
      return reached;
    }
  }
}

int test_cpp_runtime_destruction_count(
  const TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  int count;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&counter->mutex));
  count = counter->count;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&counter->mutex));
  return count;
}

static TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_completion_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  TEST_CPP_RUNTIME_EXECUTOR_ID callback_executor;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  callback_executor = context->callback_executor;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  return callback_executor;
}

static TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_parker_executor(
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *parker) {
  TEST_CPP_RUNTIME_EXECUTOR_ID parker_executor;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&parker->mutex));
  parker_executor = parker->parker_executor;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&parker->mutex));
  return parker_executor;
}

bool test_cpp_runtime_completion_ran_on_parker(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *parker) {
  return test_cpp_runtime_completion_has_executor(context) &&
         test_cpp_runtime_executor_equals(
           test_cpp_runtime_completion_executor(context),
           test_cpp_runtime_parker_executor(parker));
}

bool test_cpp_runtime_completion_ran_on_current_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  return test_cpp_runtime_completion_has_executor(context) &&
         test_cpp_runtime_executor_equals(
           test_cpp_runtime_completion_executor(context),
           test_cpp_runtime_current_executor());
}

bool test_cpp_runtime_parker_started(
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *context) {
  bool started;

  EnterCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  started = context->started;
  LeaveCriticalSection(
    const_cast<LPCRITICAL_SECTION>(&context->mutex));
  return started;
}

static void test_cpp_runtime_mark_started(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context) {
  EnterCriticalSection(&context->mutex);
  context->started = true;
  context->parker_executor = GetCurrentThreadId();
  LeaveCriticalSection(&context->mutex);
  SetEvent(context->started_event);
}

static void test_cpp_runtime_mark_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  bool park_result) {
  EnterCriticalSection(&context->mutex);
  context->park_result = park_result;
  context->finished = true;
  LeaveCriticalSection(&context->mutex);
  SetEvent(context->finished_event);
}

static bool test_cpp_runtime_wait_parker_started(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const DWORD start_tick = test_cpp_runtime_start_tick();

  for (;;) {
    EnterCriticalSection(&context->mutex);
    if (context->started) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    ResetEvent(context->started_event);
    if (context->started) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    LeaveCriticalSection(&context->mutex);

    if (!test_cpp_runtime_wait_handle_until(
          context->started_event,
          start_tick,
          timeout_ms)) {
      EnterCriticalSection(&context->mutex);
      const bool started = context->started;
      LeaveCriticalSection(&context->mutex);
      return started;
    }
  }
}

static bool test_cpp_runtime_wait_parker_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  DWORD start_tick,
  unsigned int timeout_ms) {
  for (;;) {
    EnterCriticalSection(&context->mutex);
    if (context->finished) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    ResetEvent(context->finished_event);
    if (context->finished) {
      LeaveCriticalSection(&context->mutex);
      return true;
    }
    LeaveCriticalSection(&context->mutex);

    if (!test_cpp_runtime_wait_handle_until(
          context->finished_event,
          start_tick,
          timeout_ms)) {
      EnterCriticalSection(&context->mutex);
      const bool finished = context->finished;
      LeaveCriticalSection(&context->mutex);
      return finished;
    }
  }
}

static unsigned __stdcall test_cpp_runtime_parker_proc(
  void *parameter) {
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context =
    static_cast<TEST_CPP_RUNTIME_PARKER_CONTEXT *>(parameter);
  auto attachment = context->bounce->attach_current();

  (void)attachment;
  test_cpp_runtime_mark_started(context);
  test_cpp_runtime_mark_finished(
    context,
    context->bounce->park(context->max_inline_depth));
  return 0u;
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
  uintptr_t thread_handle;

  EnterCriticalSection(&context->mutex);
  context->bounce = bounce_instance;
  context->max_inline_depth = max_inline_depth;
  context->started = false;
  context->finished = false;
  context->park_result = false;
  context->parker_executor = 0u;
  ResetEvent(context->started_event);
  ResetEvent(context->finished_event);
  LeaveCriticalSection(&context->mutex);

  thread_handle = _beginthreadex(
    NULL,
    0u,
    test_cpp_runtime_parker_proc,
    context,
    0u,
    NULL);
  if (thread_handle == 0u) {
    return 0u;
  }

  EnterCriticalSection(&context->mutex);
  context->thread_handle = (HANDLE)thread_handle;
  LeaveCriticalSection(&context->mutex);

  return test_cpp_runtime_wait_parker_started(context, timeout_ms) ?
           test_cpp_runtime_parker_executor(context) :
           0u;
}

bool test_cpp_runtime_stop_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const DWORD start_tick = test_cpp_runtime_start_tick();
  HANDLE thread_handle;
  bool park_result;

  bounce_instance->shutdown();
  if (!test_cpp_runtime_wait_parker_finished(
        context,
        start_tick,
        timeout_ms)) {
    return false;
  }

  EnterCriticalSection(&context->mutex);
  thread_handle = context->thread_handle;
  LeaveCriticalSection(&context->mutex);

  if ((thread_handle != NULL) &&
      !test_cpp_runtime_wait_handle_until(
        thread_handle,
        start_tick,
        timeout_ms)) {
    return false;
  }

  if (thread_handle != NULL) {
    CloseHandle(thread_handle);
  }

  EnterCriticalSection(&context->mutex);
  context->thread_handle = NULL;
  park_result = context->park_result;
  LeaveCriticalSection(&context->mutex);
  return park_result;
}

#endif
