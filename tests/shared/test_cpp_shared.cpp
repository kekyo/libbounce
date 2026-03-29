/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <memory>
#if !defined(BOUNCE_FREERTOS)
#include <thread>
#endif
#include <type_traits>

#if defined(BOUNCE_POSIX)
#include <poll.h>
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
#include <unistd.h>
#endif

#include "shared/test_cpp_runtime.h"
#include "libbounce/bounce.h"

#define TEST_TIMEOUT_MS 5000u

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

static_assert(
  !std::is_copy_constructible<libbounce::bounce>::value,
  "libbounce::bounce must not be copy constructible");
static_assert(
  !std::is_copy_assignable<libbounce::bounce>::value,
  "libbounce::bounce must not be copy assignable");
static_assert(
  !std::is_move_constructible<libbounce::bounce>::value,
  "libbounce::bounce must not be move constructible");
static_assert(
  !std::is_move_assignable<libbounce::bounce>::value,
  "libbounce::bounce must not be move assignable");
static_assert(
  !std::is_copy_constructible<libbounce::cancellation>::value,
  "libbounce::cancellation must not be copy constructible");
static_assert(
  !std::is_copy_assignable<libbounce::cancellation>::value,
  "libbounce::cancellation must not be copy assignable");
static_assert(
  !std::is_move_constructible<libbounce::cancellation>::value,
  "libbounce::cancellation must not be move constructible");
static_assert(
  !std::is_move_assignable<libbounce::cancellation>::value,
  "libbounce::cancellation must not be move assignable");
static_assert(
  !std::is_copy_constructible<libbounce::cancellation_registration>::value,
  "libbounce::cancellation_registration must not be copy constructible");
static_assert(
  !std::is_copy_assignable<libbounce::cancellation_registration>::value,
  "libbounce::cancellation_registration must not be copy assignable");
static_assert(
  !std::is_move_constructible<libbounce::cancellation_registration>::value,
  "libbounce::cancellation_registration must not be move constructible");
static_assert(
  !std::is_move_assignable<libbounce::cancellation_registration>::value,
  "libbounce::cancellation_registration must not be move assignable");
static_assert(
  !std::is_copy_constructible<libbounce::timer>::value,
  "libbounce::timer must not be copy constructible");
static_assert(
  !std::is_copy_assignable<libbounce::timer>::value,
  "libbounce::timer must not be copy assignable");
static_assert(
  !std::is_move_constructible<libbounce::timer>::value,
  "libbounce::timer must not be move constructible");
static_assert(
  !std::is_move_assignable<libbounce::timer>::value,
  "libbounce::timer must not be move assignable");

#if defined(BOUNCE_POSIX) || defined(BOUNCE_FREERTOS)
static_assert(
  !std::is_copy_constructible<libbounce::condition>::value,
  "libbounce::condition must not be copy constructible");
static_assert(
  !std::is_copy_assignable<libbounce::condition>::value,
  "libbounce::condition must not be copy assignable");
static_assert(
  !std::is_move_constructible<libbounce::condition>::value,
  "libbounce::condition must not be move constructible");
static_assert(
  !std::is_move_assignable<libbounce::condition>::value,
  "libbounce::condition must not be move assignable");
#endif

using TEST_COMPLETION_CONTEXT = TEST_CPP_RUNTIME_COMPLETION_CONTEXT;
using TEST_PARK_THREAD_CONTEXT = TEST_CPP_RUNTIME_PARKER_CONTEXT;
using TEST_DESTRUCTION_COUNTER = TEST_CPP_RUNTIME_DESTRUCTION_COUNTER;

typedef struct TEST_DESTRUCTION_PROBE {
  TEST_DESTRUCTION_COUNTER *destroyed_count;

  explicit TEST_DESTRUCTION_PROBE(TEST_DESTRUCTION_COUNTER *destroyed_count_)
    : destroyed_count(destroyed_count_) {
  }

  TEST_DESTRUCTION_PROBE(const TEST_DESTRUCTION_PROBE&) = delete;
  TEST_DESTRUCTION_PROBE& operator=(const TEST_DESTRUCTION_PROBE&) = delete;

  ~TEST_DESTRUCTION_PROBE() {
    test_cpp_runtime_destruction_counter_increment(destroyed_count);
  }
} TEST_DESTRUCTION_PROBE;

static void test_completion_context_init(
  TEST_COMPLETION_CONTEXT *context,
  bool waitable = true) {
  test_cpp_runtime_completion_context_init(context, waitable);
}

static void test_record_completion(
  TEST_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result) {
  test_cpp_runtime_record_completion(context, result);
}

extern "C" void test_cpp_completion_callback(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  TEST_COMPLETION_CONTEXT *context =
    static_cast<TEST_COMPLETION_CONTEXT *>(completion_state);

  test_record_completion(context, result);
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

static void test_wait_no_additional_completion(
  TEST_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count) {
  ASSERT_TRUE(
    test_cpp_runtime_wait_no_additional_completion(
      context,
      expected_call_count,
      TEST_TIMEOUT_MS / 10u));
}

static void test_wait_destroyed_count(
  TEST_DESTRUCTION_COUNTER *destroyed_count,
  int expected_destroyed_count) {
  ASSERT_TRUE(
    test_cpp_runtime_wait_destruction_count(
      destroyed_count,
      expected_destroyed_count,
      TEST_TIMEOUT_MS));
}

static double test_cpp_monotonic_now_ms(void) {
  return std::chrono::duration<double, std::milli>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

static void test_cpp_yield_park_once_poll(void) {
#if defined(BOUNCE_FREERTOS)
  vTaskDelay(1u);
#elif defined(_WIN32)
  Sleep(1u);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

static void test_assert_completion_on_parker(
  TEST_COMPLETION_CONTEXT *context,
  const TEST_PARK_THREAD_CONTEXT *park_context,
  BOUNCE_COMPLETION_RESULT result) {
  ASSERT_TRUE(test_cpp_runtime_completion_call_count(context) == 1u);
  ASSERT_TRUE(test_cpp_runtime_completion_result(context) == result);
  ASSERT_TRUE(test_cpp_runtime_completion_has_executor(context));
  ASSERT_TRUE(test_cpp_runtime_completion_ran_on_parker(context, park_context));
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

static void test_start_parker_with_inline_depth(
  libbounce::bounce *bounce_instance,
  TEST_PARK_THREAD_CONTEXT *context,
  unsigned int max_inline_depth) {
  (void)test_cpp_runtime_start_parker_ex(
    bounce_instance,
    context,
    TEST_TIMEOUT_MS,
    max_inline_depth);

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

typedef struct TEST_CPP_INLINE_POST_CONTEXT {
  libbounce::bounce *bounce;
  TEST_COMPLETION_CONTEXT outer;
  TEST_COMPLETION_CONTEXT nested;
  volatile unsigned int next_order;
  volatile unsigned int outer_order;
  volatile unsigned int nested_order;
} TEST_CPP_INLINE_POST_CONTEXT;

static unsigned int test_cpp_inline_post_next_order(volatile unsigned int *next_order) {
  return __atomic_add_fetch(next_order, 1u, __ATOMIC_ACQ_REL);
}

#if defined(BOUNCE_POSIX)
static void test_backend_fd_await(
  libbounce::bounce *bounce_instance,
  int fd,
  BOUNCE_COMPLETION completion,
  void *completion_state) {
  bounce_instance->wait(fd, POLLIN, completion, completion_state, NULL);
}

template<typename COMPLETION_TYPE>
static bool test_backend_fd_await(
  libbounce::bounce *bounce_instance,
  int fd,
  COMPLETION_TYPE&& completion) {
  return bounce_instance->wait(
    fd,
    POLLIN,
    std::forward<COMPLETION_TYPE>(completion),
    NULL);
}
#endif

#if defined(BOUNCE_POSIX_GLIB)
static void test_backend_fd_await(
  libbounce::bounce *bounce_instance,
  int fd,
  BOUNCE_COMPLETION completion,
  void *completion_state) {
  bounce_instance->wait(fd, G_IO_IN, completion, completion_state, NULL);
}

template<typename COMPLETION_TYPE>
static bool test_backend_fd_await(
  libbounce::bounce *bounce_instance,
  int fd,
  COMPLETION_TYPE&& completion) {
  return bounce_instance->wait(
    fd,
    G_IO_IN,
    std::forward<COMPLETION_TYPE>(completion),
    NULL);
}
#endif

extern "C" void test_cpp_wrapper_post_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(
    bounce_instance.post(
      test_cpp_completion_callback,
      &completion_context));
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_park_once_post_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_COMPLETION_CONTEXT completion_context;

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(
    bounce_instance.post(
      test_cpp_completion_callback,
      &completion_context));
  ASSERT_TRUE(bounce_instance.park_once());
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);
}

extern "C" void test_cpp_wrapper_attach_current_timeout_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_COMPLETION_CONTEXT completion_context;
  double started_ms;

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(!libbounce::bounce::current().has_value());

  {
    libbounce::timer timer;
    auto attachment = bounce_instance.attach_current();
    auto current = libbounce::bounce::current();

    (void)attachment;
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(current->get_core() == bounce_instance.get_core());
    ASSERT_TRUE(
      timer.wait(
        *current,
        1u,
        test_cpp_completion_callback,
        &completion_context,
        NULL));

    started_ms = test_cpp_monotonic_now_ms();
    while (test_cpp_runtime_completion_call_count(&completion_context) == 0u) {
      ASSERT_TRUE(bounce_instance.park_once());
      ASSERT_TRUE((test_cpp_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
      if (test_cpp_runtime_completion_call_count(&completion_context) == 0u) {
        test_cpp_yield_park_once_poll();
      }
    }
  }

  ASSERT_TRUE(!libbounce::bounce::current().has_value());
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_COMPLETED);
}

extern "C" void test_cpp_wrapper_attach_current_restores_fallback_view(void) {
  libbounce::bounce bounce_instance;
  libbounce::bounce fallback_before;
  libbounce::bounce fallback_after;

  ::bounce_set_fallback_core(NULL);
  ::bounce_set_core(NULL);
  ASSERT_TRUE(!libbounce::bounce::current().has_value());

  ::bounce_set_fallback_core(fallback_before.get_core());
  ASSERT_TRUE(libbounce::bounce::current().has_value());
  ASSERT_TRUE(
    libbounce::bounce::current()->get_core() == fallback_before.get_core());

  {
    auto attachment = bounce_instance.attach_current();
    auto current = libbounce::bounce::current();

    (void)attachment;
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(current->get_core() == bounce_instance.get_core());
  }

  ::bounce_set_fallback_core(fallback_after.get_core());
  ASSERT_TRUE(libbounce::bounce::current().has_value());
  ASSERT_TRUE(
    libbounce::bounce::current()->get_core() == fallback_after.get_core());

  ::bounce_set_fallback_core(NULL);
  ASSERT_TRUE(!libbounce::bounce::current().has_value());
}

extern "C" void test_cpp_wrapper_park_once_returns_before_timeout_completion(void) {
  libbounce::bounce bounce_instance;
  TEST_COMPLETION_CONTEXT completion_context;

  test_completion_context_init(&completion_context);
  {
    libbounce::timer timer;
    double started_ms;
    double first_return_ms;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        200u,
        test_cpp_completion_callback,
        &completion_context,
        NULL));
    started_ms = test_cpp_monotonic_now_ms();
    ASSERT_TRUE(bounce_instance.park_once());
    first_return_ms = test_cpp_monotonic_now_ms();

    ASSERT_TRUE((first_return_ms - started_ms) < 100.0);
    ASSERT_TRUE(test_cpp_runtime_completion_call_count(&completion_context) == 0u);

    while (test_cpp_runtime_completion_call_count(&completion_context) == 0u) {
      ASSERT_TRUE(bounce_instance.park_once());
      ASSERT_TRUE((test_cpp_monotonic_now_ms() - started_ms) < (double)TEST_TIMEOUT_MS);
      if (test_cpp_runtime_completion_call_count(&completion_context) == 0u) {
        test_cpp_yield_park_once_poll();
      }
    }

    test_assert_completion_on_current_executor(
      &completion_context,
      BOUNCE_COMPLETION_COMPLETED);
  }
}

extern "C" void test_cpp_wrapper_park_once_nested_post_inlines(void) {
  libbounce::bounce bounce_instance;
  TEST_CPP_INLINE_POST_CONTEXT context{};

  context.bounce = &bounce_instance;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);

  ASSERT_TRUE(
    bounce_instance.post(
      [&context](BOUNCE_COMPLETION_RESULT result) {
        ASSERT_TRUE(
          context.bounce->post(
            [&context](BOUNCE_COMPLETION_RESULT nested_result) {
              __atomic_store_n(
                &context.nested_order,
                test_cpp_inline_post_next_order(&context.next_order),
                __ATOMIC_RELEASE);
              test_record_completion(&context.nested, nested_result);
            }));
        __atomic_store_n(
          &context.outer_order,
          test_cpp_inline_post_next_order(&context.next_order),
          __ATOMIC_RELEASE);
        test_record_completion(&context.outer, result);
      }));
  ASSERT_TRUE(bounce_instance.park_once(2u));

  test_assert_completion_on_current_executor(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_executor(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(__atomic_load_n(&context.nested_order, __ATOMIC_ACQUIRE) == 1u);
  ASSERT_TRUE(__atomic_load_n(&context.outer_order, __ATOMIC_ACQUIRE) == 2u);
}

extern "C" void test_cpp_wrapper_park_once_nested_post_falls_back_at_depth_limit(void) {
  libbounce::bounce bounce_instance;
  TEST_CPP_INLINE_POST_CONTEXT context{};

  context.bounce = &bounce_instance;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);

  ASSERT_TRUE(
    bounce_instance.post(
      [&context](BOUNCE_COMPLETION_RESULT result) {
        ASSERT_TRUE(
          context.bounce->post(
            [&context](BOUNCE_COMPLETION_RESULT nested_result) {
              __atomic_store_n(
                &context.nested_order,
                test_cpp_inline_post_next_order(&context.next_order),
                __ATOMIC_RELEASE);
              test_record_completion(&context.nested, nested_result);
            }));
        __atomic_store_n(
          &context.outer_order,
          test_cpp_inline_post_next_order(&context.next_order),
          __ATOMIC_RELEASE);
        test_record_completion(&context.outer, result);
      }));
  ASSERT_TRUE(bounce_instance.park_once(1u));

  test_assert_completion_on_current_executor(
    &context.outer,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_current_executor(
    &context.nested,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(__atomic_load_n(&context.outer_order, __ATOMIC_ACQUIRE) == 1u);
  ASSERT_TRUE(__atomic_load_n(&context.nested_order, __ATOMIC_ACQUIRE) == 2u);
}

extern "C" void test_cpp_wrapper_nested_post_inlines_with_park_ex(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_CPP_INLINE_POST_CONTEXT context{};

  context.bounce = &bounce_instance;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);
  test_start_parker_with_inline_depth(&bounce_instance, &park_context, 2u);

  ASSERT_TRUE(
    bounce_instance.post(
      [&context](BOUNCE_COMPLETION_RESULT result) {
        ASSERT_TRUE(
          context.bounce->post(
            [&context](BOUNCE_COMPLETION_RESULT nested_result) {
              __atomic_store_n(
                &context.nested_order,
                test_cpp_inline_post_next_order(&context.next_order),
                __ATOMIC_RELEASE);
              test_record_completion(&context.nested, nested_result);
            }));
        __atomic_store_n(
          &context.outer_order,
          test_cpp_inline_post_next_order(&context.next_order),
          __ATOMIC_RELEASE);
        test_record_completion(&context.outer, result);
      }));

  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);
  test_assert_completion_on_parker(
    &context.outer,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_parker(
    &context.nested,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(__atomic_load_n(&context.nested_order, __ATOMIC_ACQUIRE) == 1u);
  ASSERT_TRUE(__atomic_load_n(&context.outer_order, __ATOMIC_ACQUIRE) == 2u);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_current_post_runs_on_attached_parker(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_CPP_INLINE_POST_CONTEXT context{};

  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);
  test_start_parker(&bounce_instance, &park_context);

  ASSERT_TRUE(
    bounce_instance.post(
      [&context](BOUNCE_COMPLETION_RESULT result) {
        auto current = libbounce::bounce::current();

        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(
          current->post(
            [&context](BOUNCE_COMPLETION_RESULT nested_result) {
              __atomic_store_n(
                &context.nested_order,
                test_cpp_inline_post_next_order(&context.next_order),
                __ATOMIC_RELEASE);
              test_record_completion(&context.nested, nested_result);
            }));
        __atomic_store_n(
          &context.outer_order,
          test_cpp_inline_post_next_order(&context.next_order),
          __ATOMIC_RELEASE);
        test_record_completion(&context.outer, result);
      }));

  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);
  test_assert_completion_on_parker(
    &context.outer,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_parker(
    &context.nested,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_nested_post_falls_back_at_depth_limit(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_CPP_INLINE_POST_CONTEXT context{};

  context.bounce = &bounce_instance;
  test_completion_context_init(&context.outer);
  test_completion_context_init(&context.nested);
  test_start_parker_with_inline_depth(&bounce_instance, &park_context, 1u);

  ASSERT_TRUE(
    bounce_instance.post(
      [&context](BOUNCE_COMPLETION_RESULT result) {
        ASSERT_TRUE(
          context.bounce->post(
            [&context](BOUNCE_COMPLETION_RESULT nested_result) {
              __atomic_store_n(
                &context.nested_order,
                test_cpp_inline_post_next_order(&context.next_order),
                __ATOMIC_RELEASE);
              test_record_completion(&context.nested, nested_result);
            }));
        __atomic_store_n(
          &context.outer_order,
          test_cpp_inline_post_next_order(&context.next_order),
          __ATOMIC_RELEASE);
        test_record_completion(&context.outer, result);
      }));

  test_wait_completion_count(&context.outer, 1u);
  test_wait_completion_count(&context.nested, 1u);
  test_assert_completion_on_parker(
    &context.outer,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_parker(
    &context.nested,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  ASSERT_TRUE(__atomic_load_n(&context.outer_order, __ATOMIC_ACQUIRE) == 1u);
  ASSERT_TRUE(__atomic_load_n(&context.nested_order, __ATOMIC_ACQUIRE) == 2u);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_cancellation_cancel_timeout_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  {
    libbounce::timer timer;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        TEST_TIMEOUT_MS,
        test_cpp_completion_callback,
        &completion_context,
        cancellation.get_cancellation()));
    cancellation.cancel(bounce_instance);
    test_wait_completion_count(&completion_context, 1u);
    test_assert_completion_on_parker(
      &completion_context,
      &park_context,
      BOUNCE_COMPLETION_CANCELED);
  }

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_cancellation_precanceled_timeout_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  cancellation.cancel(bounce_instance);
  {
    libbounce::timer timer;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        TEST_TIMEOUT_MS,
        test_cpp_completion_callback,
        &completion_context,
        cancellation.get_cancellation()));
    test_wait_completion_count(&completion_context, 1u);
    test_assert_completion_on_parker(
      &completion_context,
      &park_context,
      BOUNCE_COMPLETION_CANCELED);
  }

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_timer_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  {
    libbounce::timer timer;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        50u,
        test_cpp_completion_callback,
        &completion_context,
        NULL));
    test_wait_completion_count(&completion_context, 1u);
    test_assert_completion_on_parker(
      &completion_context,
      &park_context,
      BOUNCE_COMPLETION_COMPLETED);
  }

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_timer_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  {
    libbounce::timer timer;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        50u,
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context,
         &payload](BOUNCE_COMPLETION_RESULT result) {
          payload = 42;
          test_record_completion(&completion_context, result);
        },
        NULL));
    test_wait_completion_count(&completion_context, 1u);
    test_assert_completion_on_parker(
      &completion_context,
      &park_context,
      BOUNCE_COMPLETION_COMPLETED);
    test_wait_destroyed_count(&destroyed_count, 1);
    ASSERT_TRUE(payload == 42);
    ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
  }

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_timer_await_aborts_on_deinit(void) {
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_DESTRUCTION_COUNTER destroyed_count;

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  {
    libbounce::timer timer;
    libbounce::bounce bounce_instance;

    ASSERT_TRUE(
      timer.wait(
        bounce_instance,
        TEST_TIMEOUT_MS,
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context](BOUNCE_COMPLETION_RESULT result) {
          test_record_completion(&completion_context, result);
        },
        NULL));
  }

  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_ABORTED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
}

extern "C" void test_cpp_wrapper_registration_completes_canceled(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  libbounce::cancellation_registration registration;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(
    registration.register_canceled(
      bounce_instance,
      cancellation,
      test_cpp_completion_callback,
      &completion_context));
  cancellation.cancel(bounce_instance);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(!registration.unregister());

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_registration_lambda_completes_canceled(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  libbounce::cancellation_registration registration;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(
    registration.register_canceled(
      bounce_instance,
      cancellation,
      [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
         new TEST_DESTRUCTION_PROBE(&destroyed_count)),
       &completion_context,
       &payload](BOUNCE_COMPLETION_RESULT result) {
        payload = 42;
        test_record_completion(&completion_context, result);
      }));
  cancellation.cancel(bounce_instance);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_CANCELED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
  ASSERT_TRUE(payload == 42);
  ASSERT_TRUE(!registration.unregister());

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_registration_unregister_prevents_callback(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  libbounce::cancellation_registration registration;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;

  test_completion_context_init(&completion_context);
  test_start_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(
    registration.register_canceled(
      bounce_instance,
      cancellation,
      test_cpp_completion_callback,
      &completion_context));
  ASSERT_TRUE(registration.unregister());
  ASSERT_TRUE(!registration.unregister());
  cancellation.cancel(bounce_instance);

  test_wait_no_additional_completion(&completion_context, 0u);
  ASSERT_TRUE(test_cpp_runtime_completion_call_count(&completion_context) == 0u);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_registration_precanceled_completes_canceled(void) {
  libbounce::bounce bounce_instance;
  libbounce::cancellation cancellation;
  libbounce::cancellation_registration registration;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  cancellation.cancel(bounce_instance);
  ASSERT_TRUE(
    registration.register_canceled(
      bounce_instance,
      cancellation,
      test_cpp_completion_callback,
      &completion_context));
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_CANCELED);
  ASSERT_TRUE(!registration.unregister());

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_post_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT lambda_context;
  TEST_COMPLETION_CONTEXT result_context;
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&lambda_context);
  test_completion_context_init(&result_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(
    bounce_instance.post(
      [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
         new TEST_DESTRUCTION_PROBE(&destroyed_count)),
       &lambda_context,
       &payload]() {
        payload = 42;
        test_record_completion(&lambda_context, BOUNCE_COMPLETION_COMPLETED);
      }));
  ASSERT_TRUE(
    bounce_instance.post(
      [&result_context](BOUNCE_COMPLETION_RESULT result) {
        test_record_completion(&result_context, result);
      }));

  test_wait_completion_count(&lambda_context, 1u);
  test_wait_completion_count(&result_context, 1u);
  test_assert_completion_on_parker(
    &lambda_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_assert_completion_on_parker(
    &result_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(payload == 42);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_post_aborts_on_deinit(void) {
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_DESTRUCTION_COUNTER destroyed_count;

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  {
    libbounce::bounce bounce_instance;

    ASSERT_TRUE(
      bounce_instance.post(
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context](BOUNCE_COMPLETION_RESULT result) {
          test_record_completion(&completion_context, result);
        }));
  }

  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_ABORTED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
}

#if defined(BOUNCE_POSIX) || defined(BOUNCE_FREERTOS)
extern "C" void test_cpp_wrapper_condition_await_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::condition condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  bounce_instance.wait(
    condition,
    test_cpp_completion_callback,
    &completion_context,
    NULL);
  bounce_instance.raise(condition);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_condition_await_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::condition condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(
    bounce_instance.wait(
      condition,
      [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
         new TEST_DESTRUCTION_PROBE(&destroyed_count)),
       &completion_context,
       &payload](BOUNCE_COMPLETION_RESULT result) {
        payload = 42;
        test_record_completion(&completion_context, result);
      },
      NULL));
  bounce_instance.raise(condition);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(payload == 42);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);

  test_stop_parker(&bounce_instance, &park_context);
}

extern "C" void test_cpp_wrapper_lambda_condition_await_aborts_on_deinit(void) {
  libbounce::condition condition;
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_DESTRUCTION_COUNTER destroyed_count;

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  {
    libbounce::bounce bounce_instance;

    ASSERT_TRUE(
      bounce_instance.wait(
        condition,
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context](BOUNCE_COMPLETION_RESULT result) {
          test_record_completion(&completion_context, result);
        },
        NULL));
  }

  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_ABORTED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
}
#endif

#if defined(BOUNCE_FREERTOS)
extern "C" void test_cpp_wrapper_condition_raise_from_isr_runs(void) {
  libbounce::bounce bounce_instance;
  libbounce::condition condition;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  bounce_instance.wait(
    condition,
    test_cpp_completion_callback,
    &completion_context,
    NULL);
  bounce_instance.raise_from_isr(condition);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
}
#endif

#if defined(_WIN32)
extern "C" void test_cpp_wrapper_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  HANDLE event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  ASSERT_TRUE(event_handle != NULL);
  bounce_instance.wait(
    event_handle,
    test_cpp_completion_callback,
    &completion_context,
    NULL);
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(CloseHandle(event_handle) != 0);
}

extern "C" void test_cpp_wrapper_lambda_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  HANDLE event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(event_handle != NULL);
  ASSERT_TRUE(
    bounce_instance.wait(
      event_handle,
      [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
         new TEST_DESTRUCTION_PROBE(&destroyed_count)),
       &completion_context,
       &payload](BOUNCE_COMPLETION_RESULT result) {
        payload = 42;
        test_record_completion(&completion_context, result);
      },
      NULL));
  ASSERT_TRUE(SetEvent(event_handle) != 0);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(payload == 42);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);

  test_stop_parker(&bounce_instance, &park_context);
  ASSERT_TRUE(CloseHandle(event_handle) != 0);
}

extern "C" void test_cpp_wrapper_lambda_await_aborts_on_deinit(void) {
  TEST_COMPLETION_CONTEXT completion_context;
  HANDLE event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  TEST_DESTRUCTION_COUNTER destroyed_count;

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(event_handle != NULL);
  {
    libbounce::bounce bounce_instance;

    ASSERT_TRUE(
      bounce_instance.wait(
        event_handle,
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context](BOUNCE_COMPLETION_RESULT result) {
          test_record_completion(&completion_context, result);
        },
        NULL));
  }

  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_ABORTED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
  ASSERT_TRUE(CloseHandle(event_handle) != 0);
}
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)
extern "C" void test_cpp_wrapper_fd_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  test_open_pipe(&pipe_fds[0]);
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_backend_fd_await(
    &bounce_instance,
    pipe_fds[0],
    test_cpp_completion_callback,
    &completion_context);
  test_signal_pipe_readable(&pipe_fds[0]);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);

  test_stop_parker(&bounce_instance, &park_context);
  test_close_pipe(&pipe_fds[0]);
}

extern "C" void test_cpp_wrapper_lambda_fd_await_runs(void) {
  libbounce::bounce bounce_instance;
  TEST_PARK_THREAD_CONTEXT park_context;
  TEST_COMPLETION_CONTEXT completion_context;
  int pipe_fds[2];
  int payload = 0;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  test_open_pipe(&pipe_fds[0]);
  test_start_parker(&bounce_instance, &park_context);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  ASSERT_TRUE(
    test_backend_fd_await(
      &bounce_instance,
      pipe_fds[0],
      [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
         new TEST_DESTRUCTION_PROBE(&destroyed_count)),
       &completion_context,
       &payload](BOUNCE_COMPLETION_RESULT result) {
        payload = 42;
        test_record_completion(&completion_context, result);
      }));
  test_signal_pipe_readable(&pipe_fds[0]);
  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_parker(
    &completion_context,
    &park_context,
    BOUNCE_COMPLETION_COMPLETED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(payload == 42);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);

  test_stop_parker(&bounce_instance, &park_context);
  test_close_pipe(&pipe_fds[0]);
}

extern "C" void test_cpp_wrapper_lambda_fd_await_aborts_on_deinit(void) {
  TEST_COMPLETION_CONTEXT completion_context;
  TEST_DESTRUCTION_COUNTER destroyed_count;
  int pipe_fds[2];
  test_open_pipe(&pipe_fds[0]);

  test_completion_context_init(&completion_context);
  test_cpp_runtime_destruction_counter_init(&destroyed_count);
  {
    libbounce::bounce bounce_instance;

    ASSERT_TRUE(
      test_backend_fd_await(
        &bounce_instance,
        pipe_fds[0],
        [probe = std::unique_ptr<TEST_DESTRUCTION_PROBE>(
           new TEST_DESTRUCTION_PROBE(&destroyed_count)),
         &completion_context](BOUNCE_COMPLETION_RESULT result) {
          test_record_completion(&completion_context, result);
        }));
  }

  test_wait_completion_count(&completion_context, 1u);
  test_assert_completion_on_current_executor(
    &completion_context,
    BOUNCE_COMPLETION_ABORTED);
  test_wait_destroyed_count(&destroyed_count, 1);
  ASSERT_TRUE(test_cpp_runtime_destruction_count(&destroyed_count) == 1);
  test_close_pipe(&pipe_fds[0]);
}
#endif
