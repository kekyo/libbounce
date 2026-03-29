/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if !defined(BOUNCE_FREERTOS) && !defined(_WIN32)

#include <chrono>

#include "shared/test_cpp_runtime.h"

static std::chrono::steady_clock::time_point test_cpp_runtime_deadline_after_ms(
  unsigned int timeout_ms) {
  return std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeout_ms);
}

TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_current_executor(void) {
  return std::this_thread::get_id();
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
  context->signal_sequence = 0u;
  context->call_count = 0u;
  context->result = BOUNCE_COMPLETION_ABORTED;
  context->callback_executor = TEST_CPP_RUNTIME_EXECUTOR_ID{};
  context->callback_executor_set = false;
}

void test_cpp_runtime_record_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  std::lock_guard<std::mutex> lock(context->mutex);

  context->signal_sequence += 1u;
  context->call_count += 1u;
  context->result = result;
  context->callback_executor = std::this_thread::get_id();
  context->callback_executor_set = true;
  context->cond.notify_all();
}

bool test_cpp_runtime_wait_completion_count(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
    test_cpp_runtime_deadline_after_ms(timeout_ms);
  std::unique_lock<std::mutex> lock(context->mutex);

  return context->cond.wait_until(
    lock,
    deadline,
    [context, expected_call_count]() {
      return context->call_count >= expected_call_count;
    });
}

bool test_cpp_runtime_wait_no_additional_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms) {
  std::unique_lock<std::mutex> lock(context->mutex);

  return !context->cond.wait_for(
    lock,
    std::chrono::milliseconds(timeout_ms),
    [context, expected_call_count]() {
      return context->call_count != expected_call_count;
    });
}

unsigned int test_cpp_runtime_completion_call_count(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  std::lock_guard<std::mutex> lock(context->mutex);

  return context->call_count;
}

BOUNCE_COMPLETION_RESULT test_cpp_runtime_completion_result(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  std::lock_guard<std::mutex> lock(context->mutex);

  return context->result;
}

bool test_cpp_runtime_completion_has_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  std::lock_guard<std::mutex> lock(context->mutex);

  return context->callback_executor_set;
}

void test_cpp_runtime_destruction_counter_init(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  counter->signal_sequence = 0u;
  counter->count = 0;
}

void test_cpp_runtime_destruction_counter_increment(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  {
    std::lock_guard<std::mutex> lock(counter->mutex);
    counter->signal_sequence += 1u;
    counter->count += 1;
  }
  counter->cond.notify_all();
}

bool test_cpp_runtime_wait_destruction_count(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter,
  int expected_destroyed_count,
  unsigned int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
    test_cpp_runtime_deadline_after_ms(timeout_ms);
  std::unique_lock<std::mutex> lock(counter->mutex);

  return counter->cond.wait_until(
    lock,
    deadline,
    [counter, expected_destroyed_count]() {
      return counter->count >= expected_destroyed_count;
    });
}

int test_cpp_runtime_destruction_count(
  const TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter) {
  std::lock_guard<std::mutex> lock(counter->mutex);

  return counter->count;
}

static TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_completion_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context) {
  std::lock_guard<std::mutex> lock(context->mutex);

  return context->callback_executor;
}

static TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_parker_executor(
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *parker) {
  std::lock_guard<std::mutex> lock(parker->mutex);

  return parker->parker_executor;
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
  std::lock_guard<std::mutex> lock(context->mutex);

  return context->started;
}

static void test_cpp_runtime_mark_started(TEST_CPP_RUNTIME_PARKER_CONTEXT *context) {
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->started = true;
    context->parker_executor = std::this_thread::get_id();
  }
  context->cond.notify_all();
}

static void test_cpp_runtime_mark_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  bool park_result) {
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->park_result = park_result;
    context->finished = true;
  }
  context->cond.notify_all();
}

static bool test_cpp_runtime_wait_parker_started(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
    test_cpp_runtime_deadline_after_ms(timeout_ms);
  std::unique_lock<std::mutex> lock(context->mutex);

  return context->cond.wait_until(
    lock,
    deadline,
    [context]() {
      return context->started;
    });
}

static bool test_cpp_runtime_wait_parker_finished(
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
    test_cpp_runtime_deadline_after_ms(timeout_ms);
  std::unique_lock<std::mutex> lock(context->mutex);

  return context->cond.wait_until(
    lock,
    deadline,
    [context]() {
      return context->finished;
    });
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
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->bounce = bounce_instance;
    context->max_inline_depth = max_inline_depth;
    context->started = false;
    context->finished = false;
    context->park_result = false;
    context->parker_executor = TEST_CPP_RUNTIME_EXECUTOR_ID{};
  }
  context->thread = std::thread([bounce_instance, context]() {
    auto attachment = bounce_instance->attach_current();

    (void)attachment;
    test_cpp_runtime_mark_started(context);
    test_cpp_runtime_mark_finished(
      context,
      bounce_instance->park(context->max_inline_depth));
  });

  return test_cpp_runtime_wait_parker_started(context, timeout_ms) ?
           context->parker_executor :
           TEST_CPP_RUNTIME_EXECUTOR_ID{};
}

bool test_cpp_runtime_stop_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms) {
  bool park_result;

  bounce_instance->shutdown();
  if (!test_cpp_runtime_wait_parker_finished(context, timeout_ms)) {
    return false;
  }
  context->thread.join();
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    park_result = context->park_result;
  }
  return park_result;
}

#endif
