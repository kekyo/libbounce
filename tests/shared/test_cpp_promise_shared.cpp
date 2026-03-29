/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#if !defined(BOUNCE_FREERTOS)
#include <thread>
#endif

#if defined(BOUNCE_POSIX)
#include <poll.h>
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include "shared/test_cpp_runtime.h"
#include "libbounce/bounce.h"
#include "libbounce/promise.h"

#define TEST_TIMEOUT_MS 5000u

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using TEST_COMPLETION_CONTEXT = TEST_CPP_RUNTIME_COMPLETION_CONTEXT;
using TEST_PARK_THREAD_CONTEXT = TEST_CPP_RUNTIME_PARKER_CONTEXT;

static double test_cpp_monotonic_now_ms(void) {
  return std::chrono::duration<double, std::milli>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

static void test_completion_context_init(TEST_COMPLETION_CONTEXT *context) {
  test_cpp_runtime_completion_context_init(context, true);
}

static void test_record_completion(
  TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  test_cpp_runtime_record_completion(context, result);
}

static void test_wait_completion_count(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  ASSERT_TRUE(
    test_cpp_runtime_wait_completion_count(
      context,
      expected_call_count,
      TEST_TIMEOUT_MS));
}

template<typename TPROMISE>
static void test_wait_promise_done(TPROMISE *promise_instance) {
  const double started_ms = test_cpp_monotonic_now_ms();

  while (!promise_instance->done()) {
#if defined(BOUNCE_FREERTOS)
    vTaskDelay(1u);
#elif defined(_WIN32)
    Sleep(1u);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    ASSERT_TRUE((test_cpp_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
  }
}

static void test_assert_completion_result(
  TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(test_cpp_runtime_completion_call_count(context) == 1u);
  ASSERT_TRUE(test_cpp_runtime_completion_result(context) == result);
  ASSERT_TRUE(test_cpp_runtime_completion_has_executor(context));
}

static void test_assert_completion_on_current_executor(
  TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(test_cpp_runtime_completion_call_count(context) == 1u);
  ASSERT_TRUE(test_cpp_runtime_completion_result(context) == result);
  ASSERT_TRUE(test_cpp_runtime_completion_has_executor(context));
  ASSERT_TRUE(test_cpp_runtime_completion_ran_on_current_executor(context));
}

static void test_start_parker(
  libbounce::bounce *bounce_instance,
  TEST_PARK_THREAD_CONTEXT *context) {
  (void)test_cpp_runtime_start_parker(
    bounce_instance,
    context,
    TEST_TIMEOUT_MS);

  ASSERT_TRUE(test_cpp_runtime_parker_started(context));
}

static void test_stop_parker(
  libbounce::bounce *bounce_instance,
  TEST_PARK_THREAD_CONTEXT *context) {
  ASSERT_TRUE(
    test_cpp_runtime_stop_parker(
      bounce_instance,
      context,
      TEST_TIMEOUT_MS));
}

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
static void test_open_pipe(int *pipe_fds) {
  ASSERT_TRUE(pipe(pipe_fds) == 0);
}

static void test_close_pipe(const int *pipe_fds) {
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_signal_pipe_readable(const int *pipe_fds) {
  const unsigned char byte = 1u;

  ASSERT_TRUE(write(pipe_fds[1], &byte, sizeof byte) == (ssize_t)sizeof byte);
}
#endif

struct TEST_PROMISE_EXCEPTION {
};

static libbounce::promise<void> test_resume_on_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result = co_await libbounce::resume_on(bounce_instance);

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_make_awaitable_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result = co_await libbounce::make_awaitable(
    bounce_instance,
    [&bounce_instance](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION * /*cancellation*/) noexcept -> bool {
      return bounce_instance.post(completion, completion_state);
    });

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_make_awaitable_start_failed_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result = co_await libbounce::make_awaitable(
    bounce_instance,
    [](
      BOUNCE_COMPLETION /*completion*/,
      void * /*completion_state*/,
      BOUNCE_CANCELLATION * /*cancellation*/) noexcept -> bool {
      return false;
    });

  ASSERT_TRUE(result.start_failed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_make_awaitable_aborted_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result = co_await libbounce::make_awaitable(
    bounce_instance,
    [](
      BOUNCE_COMPLETION completion,
      void *completion_state,
      BOUNCE_CANCELLATION * /*cancellation*/) noexcept {
      completion(BOUNCE_COMPLETION_ABORTED, completion_state);
    });

  ASSERT_TRUE(result.aborted());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<int> test_nested_value_child_coroutine(
  libbounce::bounce &bounce_instance) {
  const libbounce::await_result result = co_await libbounce::resume_on(bounce_instance);

  ASSERT_TRUE(result.completed());
  co_return 42;
}

static libbounce::promise<void> test_nested_value_root_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  auto child = test_nested_value_child_coroutine(bounce_instance);
  ASSERT_TRUE(child.start());
  const int value = co_await child;

  ASSERT_TRUE(value == 42);
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_exception_child_coroutine(
  libbounce::bounce &bounce_instance) {
  const libbounce::await_result result = co_await libbounce::resume_on(bounce_instance);

  ASSERT_TRUE(result.completed());
  throw TEST_PROMISE_EXCEPTION {};
}

static libbounce::promise<void> test_exception_root_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  bool caught = false;
  auto child = test_exception_child_coroutine(bounce_instance);

  ASSERT_TRUE(child.start());

  try {
    co_await child;
  } catch (const TEST_PROMISE_EXCEPTION&) {
    caught = true;
  }

  ASSERT_TRUE(caught);
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_await_canceled_coroutine(
  libbounce::bounce &bounce_instance,
  libbounce::cancellation &cancellation_source,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result =
    co_await libbounce::await_canceled(bounce_instance, cancellation_source);

  ASSERT_TRUE(result.canceled());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

#if defined(BOUNCE_POSIX) || defined(BOUNCE_FREERTOS)
static libbounce::promise<void> test_condition_await_coroutine(
  libbounce::bounce &bounce_instance,
  libbounce::condition &condition_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result =
    co_await bounce_instance.await(condition_instance, NULL);

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}
#endif

#if defined(BOUNCE_POSIX)
static libbounce::promise<void> test_fd_await_coroutine(
  libbounce::bounce &bounce_instance,
  int fd,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result =
    co_await bounce_instance.await(fd, POLLIN, NULL);

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}
#endif

#if defined(BOUNCE_POSIX_GLIB)
static libbounce::promise<void> test_fd_await_coroutine(
  libbounce::bounce &bounce_instance,
  int fd,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result =
    co_await bounce_instance.await(fd, G_IO_IN, NULL);

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}
#endif

#if defined(_WIN32)
static libbounce::promise<void> test_handle_await_coroutine(
  libbounce::bounce &bounce_instance,
  HANDLE event_handle,
  TEST_COMPLETION_CONTEXT *completion_context) {
  const libbounce::await_result result =
    co_await bounce_instance.await(event_handle, NULL);

  ASSERT_TRUE(result.completed());
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}
#endif

extern "C" void test_cpp_promise_resume_on_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_resume_on_coroutine(bounce_instance, &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_promise_make_awaitable_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_make_awaitable_coroutine(bounce_instance, &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_promise_make_awaitable_start_failed(void) {
  libbounce::bounce bounce_instance;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_make_awaitable_start_failed_coroutine(
    bounce_instance,
    &completion_context);

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);
}

extern "C" void test_cpp_promise_make_awaitable_aborted(void) {
  libbounce::bounce bounce_instance;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_make_awaitable_aborted_coroutine(
    bounce_instance,
    &completion_context);

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);
}

extern "C" void test_cpp_promise_nested_value_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_nested_value_root_coroutine(
    bounce_instance,
    &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_promise_exception_propagates(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_exception_root_coroutine(
    bounce_instance,
    &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_promise_await_canceled_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation_source;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_await_canceled_coroutine(
    bounce_instance,
    cancellation_source,
    &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  cancellation_source.cancel(bounce_instance);
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}

#if defined(BOUNCE_POSIX) || defined(BOUNCE_FREERTOS)
extern "C" void test_cpp_promise_condition_await_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::condition condition_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_condition_await_coroutine(
    bounce_instance,
    condition_instance,
    &completion_context);

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  bounce_instance.raise(condition_instance);
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
}
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
extern "C" void test_cpp_promise_fd_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];

  test_completion_context_init(&completion_context);
  test_open_pipe(pipe_fds);
  auto coroutine = test_fd_await_coroutine(
    bounce_instance,
    pipe_fds[0],
    &completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  test_signal_pipe_readable(pipe_fds);
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
  test_close_pipe(pipe_fds);
}
#endif

#if defined(_WIN32)
extern "C" void test_cpp_promise_handle_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  HANDLE event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
  auto coroutine = test_handle_await_coroutine(
    bounce_instance,
    event_handle,
    &completion_context);

  ASSERT_TRUE(event_handle != NULL);
  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  test_stop_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(CloseHandle(event_handle) != 0);
}
#endif

#endif
