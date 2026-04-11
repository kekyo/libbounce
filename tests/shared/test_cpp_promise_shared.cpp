/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(LIBBOUNCE_ENABLE_COROUTINE_TESTS)

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#if !defined(BOUNCE_FREERTOS)
#include <thread>
#endif

#if defined(BOUNCE_POSIX)
#include <poll.h>
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__linux__) && (defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB))
#include <liburing.h>
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

static void test_set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);

  ASSERT_TRUE(flags >= 0);
  ASSERT_TRUE(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static size_t test_fill_pipe_until_would_block(int fd) {
  std::array<unsigned char, 4096u> buffer {};
  size_t total = 0u;

  while (true) {
    const ssize_t result = write(fd, buffer.data(), buffer.size());

    if (result > 0) {
      total += (size_t)result;
      continue;
    }

    ASSERT_TRUE(result < 0);
    ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));
    return total;
  }
}

static void test_read_exact_bytes(int fd, size_t length) {
  std::array<unsigned char, 4096u> buffer {};
  size_t read_total = 0u;

  while (read_total < length) {
    const size_t chunk =
      (length - read_total) < buffer.size() ? (length - read_total) : buffer.size();
    const ssize_t result = read(fd, buffer.data(), chunk);

    ASSERT_TRUE(result > 0);
    read_total += (size_t)result;
  }
}

struct TEST_FD_WRITE_ALL_BYTES_CONTEXT {
  std::atomic<unsigned int> await_count { 0u };
  std::atomic<unsigned int> write_attempt_count { 0u };
  std::atomic<unsigned int> successful_write_count { 0u };
  std::atomic<size_t> bytes_written { 0u };
};

#if defined(__linux__) && (defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB))
static void test_dummy_io_uring_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state) {
  (void)sqe;
  (void)prepare_state;
}

struct TEST_IO_URING_TIMEOUT_CONTEXT {
  __kernel_timespec timespec {};
};

static void test_io_uring_nop_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state) {
  (void)prepare_state;
  io_uring_prep_nop(sqe);
}

static void test_io_uring_timeout_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state) {
  TEST_IO_URING_TIMEOUT_CONTEXT *context =
    static_cast<TEST_IO_URING_TIMEOUT_CONTEXT *>(prepare_state);

  io_uring_prep_timeout(sqe, &context->timespec, 0u, 0u);
}
#endif
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

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
static libbounce::promise<void> test_fd_write_all_bytes_coroutine(
  libbounce::bounce &bounce_instance,
  int fd,
  const char *buffer,
  size_t length,
  TEST_FD_WRITE_ALL_BYTES_CONTEXT *write_context,
  TEST_COMPLETION_CONTEXT *completion_context) {
  size_t written = 0u;

  while (written < length) {
    write_context->await_count.fetch_add(1u, std::memory_order_relaxed);
#if defined(BOUNCE_POSIX)
    const libbounce::await_result await_result =
      co_await bounce_instance.await(fd, POLLOUT, NULL);
#else
    const libbounce::await_result await_result =
      co_await bounce_instance.await(fd, G_IO_OUT, NULL);
#endif
    const ssize_t result = write(fd, buffer + written, length - written);

    ASSERT_TRUE(await_result.completed());
    write_context->write_attempt_count.fetch_add(1u, std::memory_order_relaxed);

    if (result < 0) {
      ASSERT_TRUE(
        (errno == EINTR) ||
        (errno == EAGAIN) ||
        (errno == EWOULDBLOCK));
      continue;
    }

    ASSERT_TRUE(result > 0);
    write_context->successful_write_count.fetch_add(1u, std::memory_order_relaxed);
    write_context->bytes_written.fetch_add((size_t)result, std::memory_order_relaxed);
    written += (size_t)result;
  }

  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}
#endif

#if defined(__linux__) && (defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB))
static libbounce::promise<void> test_io_uring_await_completed_coroutine(
  libbounce::bounce &bounce_instance,
  TEST_COMPLETION_CONTEXT *completion_context) {
  int prepare_state = 0;
  libbounce::io_uring_operation operation(
    &test_io_uring_nop_prepare,
    &prepare_state);
  const libbounce::await_result result =
    co_await bounce_instance.await(*operation.get_operation(), NULL);

  ASSERT_TRUE(result.completed());
  ASSERT_TRUE(operation.result() == 0);
  test_record_completion(completion_context, BOUNCE_COMPLETION_COMPLETED);
}

static libbounce::promise<void> test_io_uring_await_canceled_coroutine(
  libbounce::bounce &bounce_instance,
  libbounce::cancellation &cancellation_source,
  TEST_COMPLETION_CONTEXT *completion_context) {
  TEST_IO_URING_TIMEOUT_CONTEXT timeout_context {};

  timeout_context.timespec.tv_sec = 10;
  libbounce::io_uring_operation operation(
    &test_io_uring_timeout_prepare,
    &timeout_context);
  const libbounce::await_result result =
    co_await bounce_instance.await(
      *operation.get_operation(),
      cancellation_source.get_cancellation());

  ASSERT_TRUE(result.canceled());
  ASSERT_TRUE(!operation.active());
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

extern "C" void test_cpp_promise_fd_write_all_bytes_awaits_before_each_write_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_FD_WRITE_ALL_BYTES_CONTEXT write_context;
  constexpr size_t drain_chunk_length = 4096u;
  const std::string payload(drain_chunk_length * 3u, 'x');
  int pipe_fds[2];
  double started_ms;
  size_t filled_bytes;

  test_completion_context_init(&completion_context);
  test_open_pipe(pipe_fds);
  test_set_nonblocking(pipe_fds[1]);
  filled_bytes = test_fill_pipe_until_would_block(pipe_fds[1]);
  ASSERT_TRUE(filled_bytes >= drain_chunk_length);

  auto coroutine = test_fd_write_all_bytes_coroutine(
    bounce_instance,
    pipe_fds[1],
    payload.data(),
    payload.size(),
    &write_context,
    &completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(coroutine.start());

  started_ms = test_cpp_monotonic_now_ms();
  while (test_cpp_runtime_completion_call_count(&completion_context) == 0u) {
    const unsigned int previous_write_count =
      write_context.successful_write_count.load(std::memory_order_acquire);

    test_read_exact_bytes(pipe_fds[0], drain_chunk_length);
    while (test_cpp_runtime_completion_call_count(&completion_context) == 0u &&
           write_context.successful_write_count.load(std::memory_order_acquire) ==
             previous_write_count) {
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

  test_wait_completion_count(&completion_context, 1u);
  test_wait_promise_done(&coroutine);
  test_assert_completion_result(&completion_context, BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(
    write_context.bytes_written.load(std::memory_order_acquire) == payload.size());
  ASSERT_TRUE(
    write_context.successful_write_count.load(std::memory_order_acquire) > 1u);
  ASSERT_TRUE(
    write_context.await_count.load(std::memory_order_acquire) ==
    write_context.write_attempt_count.load(std::memory_order_acquire));
  ASSERT_TRUE(
    write_context.write_attempt_count.load(std::memory_order_acquire) >=
    write_context.successful_write_count.load(std::memory_order_acquire));

  test_stop_parker(&bounce_instance, &park_context);
  test_close_pipe(pipe_fds);
}

#if defined(__linux__) && (defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB))
extern "C" void test_cpp_io_uring_operation_init_accessors(void) {
  int prepare_state = 7;
  libbounce::io_uring_operation operation(
    &test_dummy_io_uring_prepare,
    &prepare_state);

  ASSERT_TRUE(operation.get_operation()->prepare == &test_dummy_io_uring_prepare);
  ASSERT_TRUE(operation.get_operation()->prepare_state == &prepare_state);
  ASSERT_TRUE(operation.result() == 0);
  ASSERT_TRUE(operation.cqe_flags() == 0u);
  ASSERT_TRUE(!operation.active());
}

extern "C" void test_cpp_promise_io_uring_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_io_uring_await_completed_coroutine(
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

extern "C" void test_cpp_promise_io_uring_await_canceled_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation_source;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  auto coroutine = test_io_uring_await_canceled_coroutine(
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
#endif
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
