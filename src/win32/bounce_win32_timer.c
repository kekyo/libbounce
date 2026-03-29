/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(_WIN32)

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <string.h>
#include <windows.h>

#include "libbounce/bounce.h"

// The timer should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

static inline LARGE_INTEGER bounce_win32_timer_due_time(
  uint32_t duration_msec) {
  LARGE_INTEGER due_time;

  due_time.QuadPart = -((LONGLONG)duration_msec * 10000LL);
  if (due_time.QuadPart == 0LL) {
    due_time.QuadPart = -1LL;
  }
  return due_time;
}

static void bounce_win32_timer_wait_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_TIMER *timer = (BOUNCE_TIMER *)completion_state;
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state_ = NULL;
  BOUNCE_COMPLETION_RESULT final_result = result;

  EnterCriticalSection(&timer->lock);
  if (timer->active == 0) {
    WakeAllConditionVariable(&timer->settled_cv);
    LeaveCriticalSection(&timer->lock);
    return;
  }

  if (timer->abort_requested != 0) {
    final_result = BOUNCE_COMPLETION_ABORTED;
  }

  if (timer->timer_handle != NULL) {
    (void)CancelWaitableTimer(timer->timer_handle);
  }

  completion = timer->completion;
  completion_state_ = timer->completion_state;
  timer->bounce = NULL;
  timer->completion = NULL;
  timer->completion_state = NULL;
  timer->active = 0;
  timer->abort_requested = 0;
  WakeAllConditionVariable(&timer->settled_cv);
  LeaveCriticalSection(&timer->lock);

  if (completion != NULL) {
    completion(final_result, completion_state_);
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_init(BOUNCE_TIMER *timer) {
  memset(timer, 0, sizeof *timer);
  InitializeCriticalSection(&timer->lock);
  InitializeConditionVariable(&timer->settled_cv);

  timer->timer_handle = CreateWaitableTimer(NULL, TRUE, NULL);
  timer->deinitialized = (timer->timer_handle != NULL) ? 0 : 1;
}

/**
 * @brief Set timeout.
 * @param duration_msec Timeout when reached this duration later.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when succeeded.
 */
bool bounce_await_timeout(BOUNCE_CORE *r,
  BOUNCE_TIMER *timer,
  uint32_t duration_msec,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  const LARGE_INTEGER due_time = bounce_win32_timer_due_time(duration_msec);

  if ((r == NULL) ||
      (timer == NULL) ||
      (completion == NULL)) {
    return false;
  }

  EnterCriticalSection(&timer->lock);
  if ((timer->deinitialized != 0) ||
      (timer->timer_handle == NULL) ||
      (timer->active != 0)) {
    LeaveCriticalSection(&timer->lock);
    return false;
  }

  timer->bounce = r;
  timer->completion = completion;
  timer->completion_state = completion_state;
  timer->active = 1;
  timer->abort_requested = 0;
  if (!SetWaitableTimer(
        timer->timer_handle,
        &due_time,
        0,
        NULL,
        NULL,
        FALSE)) {
    timer->bounce = NULL;
    timer->completion = NULL;
    timer->completion_state = NULL;
    timer->active = 0;
    WakeAllConditionVariable(&timer->settled_cv);
    LeaveCriticalSection(&timer->lock);
    return false;
  }
  LeaveCriticalSection(&timer->lock);

  bounce_await_win32_handle(
    r,
    timer->timer_handle,
    bounce_win32_timer_wait_completion,
    timer,
    cancellation);
  return true;
}

/**
 * @brief Deinitialize a timer.
 * @param cancellation Timer storage provided by the caller.
 */
void bounce_timer_deinit(BOUNCE_TIMER *timer) {
  HANDLE timer_handle = NULL;
  bool wait_settle = false;

  if (timer == NULL) {
    return;
  }

  EnterCriticalSection(&timer->lock);
  timer->deinitialized = 1;
  timer_handle = timer->timer_handle;
  if (timer->active != 0) {
    LARGE_INTEGER due_time;

    timer->abort_requested = 1;
    wait_settle = true;
    due_time.QuadPart = -1LL;
    if (timer_handle != NULL) {
      (void)SetWaitableTimer(
        timer_handle,
        &due_time,
        0,
        NULL,
        NULL,
        FALSE);
    }
  }
  while (wait_settle &&
         (timer->active != 0)) {
    (void)SleepConditionVariableCS(
      &timer->settled_cv,
      &timer->lock,
      INFINITE);
  }
  timer->timer_handle = NULL;
  LeaveCriticalSection(&timer->lock);

  if (timer_handle != NULL) {
    CloseHandle(timer_handle);
  }
  DeleteCriticalSection(&timer->lock);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
