/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef LIBBOUNCE_TEST_CPP_RUNTIME_H
#define LIBBOUNCE_TEST_CPP_RUNTIME_H

#include <stdbool.h>

#if defined(BOUNCE_FREERTOS)
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
using TEST_CPP_RUNTIME_EXECUTOR_ID = TaskHandle_t;
#elif defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
using TEST_CPP_RUNTIME_EXECUTOR_ID = DWORD;
#else
#include <condition_variable>
#include <mutex>
#include <thread>
using TEST_CPP_RUNTIME_EXECUTOR_ID = std::thread::id;
#endif

#include "libbounce/bounce.h"

#if defined(BOUNCE_FREERTOS)
typedef struct TEST_CPP_RUNTIME_COMPLETION_CONTEXT {
  SemaphoreHandle_t signal_semaphore;
  StaticSemaphore_t signal_semaphore_buffer;
  volatile unsigned int signal_sequence;
  volatile unsigned int call_count;
  volatile int result;
  TEST_CPP_RUNTIME_EXECUTOR_ID callback_executor;
  volatile unsigned int callback_executor_set;

  TEST_CPP_RUNTIME_COMPLETION_CONTEXT()
    : signal_semaphore(NULL),
      signal_sequence(0u),
      call_count(0u),
      result((int)BOUNCE_COMPLETION_ABORTED),
      callback_executor(),
      callback_executor_set(0u) {
  }

  ~TEST_CPP_RUNTIME_COMPLETION_CONTEXT() {
    if (signal_semaphore != NULL) {
      vSemaphoreDelete(signal_semaphore);
      signal_semaphore = NULL;
    }
  }
} TEST_CPP_RUNTIME_COMPLETION_CONTEXT;

typedef struct TEST_CPP_RUNTIME_PARKER_CONTEXT {
  libbounce::bounce *bounce;
  TaskHandle_t task_handle;
  SemaphoreHandle_t started_semaphore;
  StaticSemaphore_t started_semaphore_buffer;
  SemaphoreHandle_t finished_semaphore;
  StaticSemaphore_t finished_semaphore_buffer;
  StaticTask_t task_tcb;
  StackType_t task_stack[ configMINIMAL_STACK_SIZE * 8u ];
  unsigned int max_inline_depth;
  volatile unsigned int started;
  volatile unsigned int finished;
  volatile unsigned int park_result;
  TEST_CPP_RUNTIME_EXECUTOR_ID parker_executor;

  TEST_CPP_RUNTIME_PARKER_CONTEXT()
    : bounce(nullptr),
      task_handle(NULL),
      started_semaphore(NULL),
      finished_semaphore(NULL),
      max_inline_depth(0u),
      started(0u),
      finished(0u),
      park_result(0u),
      parker_executor() {
  }

  ~TEST_CPP_RUNTIME_PARKER_CONTEXT() {
    if (started_semaphore != NULL) {
      vSemaphoreDelete(started_semaphore);
      started_semaphore = NULL;
    }
    if (finished_semaphore != NULL) {
      vSemaphoreDelete(finished_semaphore);
      finished_semaphore = NULL;
    }
  }
} TEST_CPP_RUNTIME_PARKER_CONTEXT;

typedef struct TEST_CPP_RUNTIME_DESTRUCTION_COUNTER {
  SemaphoreHandle_t signal_semaphore;
  StaticSemaphore_t signal_semaphore_buffer;
  volatile unsigned int signal_sequence;
  volatile int count;

  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER()
    : signal_semaphore(NULL),
      signal_sequence(0u),
      count(0) {
  }

  ~TEST_CPP_RUNTIME_DESTRUCTION_COUNTER() {
    if (signal_semaphore != NULL) {
      vSemaphoreDelete(signal_semaphore);
      signal_semaphore = NULL;
    }
  }
} TEST_CPP_RUNTIME_DESTRUCTION_COUNTER;
#elif defined(_WIN32)
typedef struct TEST_CPP_RUNTIME_COMPLETION_CONTEXT {
  CRITICAL_SECTION mutex;
  HANDLE signal_event;
  unsigned int signal_sequence;
  unsigned int call_count;
  BOUNCE_COMPLETION_RESULT result;
  TEST_CPP_RUNTIME_EXECUTOR_ID callback_executor;
  bool callback_executor_set;

  TEST_CPP_RUNTIME_COMPLETION_CONTEXT()
    : signal_event(NULL),
      signal_sequence(0u),
      call_count(0u),
      result(BOUNCE_COMPLETION_ABORTED),
      callback_executor(0u),
      callback_executor_set(false) {
    InitializeCriticalSection(&mutex);
    signal_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  }

  ~TEST_CPP_RUNTIME_COMPLETION_CONTEXT() {
    if (signal_event != NULL) {
      CloseHandle(signal_event);
      signal_event = NULL;
    }
    DeleteCriticalSection(&mutex);
  }
} TEST_CPP_RUNTIME_COMPLETION_CONTEXT;

typedef struct TEST_CPP_RUNTIME_PARKER_CONTEXT {
  libbounce::bounce *bounce;
  CRITICAL_SECTION mutex;
  HANDLE started_event;
  HANDLE finished_event;
  HANDLE thread_handle;
  unsigned int max_inline_depth;
  bool started;
  bool finished;
  bool park_result;
  TEST_CPP_RUNTIME_EXECUTOR_ID parker_executor;

  TEST_CPP_RUNTIME_PARKER_CONTEXT()
    : bounce(nullptr),
      started_event(NULL),
      finished_event(NULL),
      thread_handle(NULL),
      max_inline_depth(0u),
      started(false),
      finished(false),
      park_result(false),
      parker_executor(0u) {
    InitializeCriticalSection(&mutex);
    started_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    finished_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  }

  ~TEST_CPP_RUNTIME_PARKER_CONTEXT() {
    if (thread_handle != NULL) {
      CloseHandle(thread_handle);
      thread_handle = NULL;
    }
    if (started_event != NULL) {
      CloseHandle(started_event);
      started_event = NULL;
    }
    if (finished_event != NULL) {
      CloseHandle(finished_event);
      finished_event = NULL;
    }
    DeleteCriticalSection(&mutex);
  }
} TEST_CPP_RUNTIME_PARKER_CONTEXT;

typedef struct TEST_CPP_RUNTIME_DESTRUCTION_COUNTER {
  CRITICAL_SECTION mutex;
  HANDLE signal_event;
  unsigned int signal_sequence;
  int count;

  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER()
    : signal_event(NULL),
      signal_sequence(0u),
      count(0) {
    InitializeCriticalSection(&mutex);
    signal_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  }

  ~TEST_CPP_RUNTIME_DESTRUCTION_COUNTER() {
    if (signal_event != NULL) {
      CloseHandle(signal_event);
      signal_event = NULL;
    }
    DeleteCriticalSection(&mutex);
  }
} TEST_CPP_RUNTIME_DESTRUCTION_COUNTER;
#else
typedef struct TEST_CPP_RUNTIME_COMPLETION_CONTEXT {
  mutable std::mutex mutex;
  std::condition_variable cond;
  unsigned int signal_sequence = 0u;
  unsigned int call_count = 0u;
  BOUNCE_COMPLETION_RESULT result = BOUNCE_COMPLETION_ABORTED;
  TEST_CPP_RUNTIME_EXECUTOR_ID callback_executor{};
  bool callback_executor_set = false;
} TEST_CPP_RUNTIME_COMPLETION_CONTEXT;

typedef struct TEST_CPP_RUNTIME_PARKER_CONTEXT {
  libbounce::bounce *bounce = nullptr;
  mutable std::mutex mutex;
  std::condition_variable cond;
  std::thread thread;
  unsigned int max_inline_depth = 0u;
  bool started = false;
  bool finished = false;
  bool park_result = false;
  TEST_CPP_RUNTIME_EXECUTOR_ID parker_executor{};
} TEST_CPP_RUNTIME_PARKER_CONTEXT;

typedef struct TEST_CPP_RUNTIME_DESTRUCTION_COUNTER {
  mutable std::mutex mutex;
  std::condition_variable cond;
  unsigned int signal_sequence = 0u;
  int count = 0;
} TEST_CPP_RUNTIME_DESTRUCTION_COUNTER;
#endif

extern TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_current_executor(void);
extern bool test_cpp_runtime_executor_equals(
  TEST_CPP_RUNTIME_EXECUTOR_ID lhs,
  TEST_CPP_RUNTIME_EXECUTOR_ID rhs);
extern void test_cpp_runtime_completion_context_init(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  bool waitable);
extern void test_cpp_runtime_record_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  BOUNCE_COMPLETION_RESULT result);
extern bool test_cpp_runtime_wait_completion_count(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms);
extern bool test_cpp_runtime_wait_no_additional_completion(
  TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  unsigned int expected_call_count,
  unsigned int timeout_ms);
/** Returns the current completion callback count observed by the runtime. */
extern unsigned int test_cpp_runtime_completion_call_count(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context);
/** Returns the latest completion result observed by the runtime. */
extern BOUNCE_COMPLETION_RESULT test_cpp_runtime_completion_result(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context);
/** Returns whether a completion executor has been recorded. */
extern bool test_cpp_runtime_completion_has_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context);
extern void test_cpp_runtime_destruction_counter_init(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter);
extern void test_cpp_runtime_destruction_counter_increment(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter);
extern bool test_cpp_runtime_wait_destruction_count(
  TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter,
  int expected_destroyed_count,
  unsigned int timeout_ms);
/** Returns the current destruction count observed by the runtime. */
extern int test_cpp_runtime_destruction_count(
  const TEST_CPP_RUNTIME_DESTRUCTION_COUNTER *counter);
extern bool test_cpp_runtime_completion_ran_on_parker(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context,
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *parker);
extern bool test_cpp_runtime_completion_ran_on_current_executor(
  const TEST_CPP_RUNTIME_COMPLETION_CONTEXT *context);
/** Returns whether the parker has entered its park execution. */
extern bool test_cpp_runtime_parker_started(
  const TEST_CPP_RUNTIME_PARKER_CONTEXT *context);
extern TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_start_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms);
extern TEST_CPP_RUNTIME_EXECUTOR_ID test_cpp_runtime_start_parker_ex(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms,
  unsigned int max_inline_depth);
extern bool test_cpp_runtime_stop_parker(
  libbounce::bounce *bounce_instance,
  TEST_CPP_RUNTIME_PARKER_CONTEXT *context,
  unsigned int timeout_ms);

#endif
