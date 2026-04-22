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

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "libbounce/bounce.h"

// The Win32 file helper should be implemented using only the core's public API.

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_WIN32_FILE_OP_NONE 0
#define BOUNCE_WIN32_FILE_OP_READ 1
#define BOUNCE_WIN32_FILE_OP_WRITE 2
#define BOUNCE_WIN32_FILE_OP_SEEK 3
#define BOUNCE_WIN32_FILE_OP_FLUSH 4
#define BOUNCE_WIN32_FILE_OP_POSTED_RESULT 5

static inline bool bounce_win32_file_handle_valid(HANDLE handle) {
  return (handle != NULL) && (handle != INVALID_HANDLE_VALUE);
}

static inline bool bounce_win32_file_offset_valid(int64_t offset) {
  return offset >= 0;
}

static inline bool bounce_win32_file_length_valid(size_t length) {
  return length <= (size_t)MAXDWORD;
}

static void bounce_win32_file_clear_active(BOUNCE_FILE_IO *operation) {
  if (operation == NULL) {
    return;
  }

  EnterCriticalSection(&operation->lock);
  operation->bounce = NULL;
  operation->completion = NULL;
  operation->completion_state = NULL;
  operation->handle = INVALID_HANDLE_VALUE;
  operation->active = false;
  operation->cancellation_registration_active = false;
  operation->io_pending = false;
  operation->cancel_requested = false;
  operation->settling = false;
  LeaveCriticalSection(&operation->lock);
}

static void bounce_win32_file_finish(
  BOUNCE_FILE_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result,
  int64_t io_result,
  int error_code) {
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state = NULL;

  if (operation == NULL) {
    return;
  }

  EnterCriticalSection(&operation->lock);
  if (operation->active) {
    operation->result = io_result;
    operation->error_code = error_code;
    completion = operation->completion;
    completion_state = operation->completion_state;
    operation->bounce = NULL;
    operation->completion = NULL;
    operation->completion_state = NULL;
    operation->handle = INVALID_HANDLE_VALUE;
    operation->active = false;
    operation->cancellation_registration_active = false;
    operation->io_pending = false;
    operation->cancel_requested = false;
    operation->settling = false;
  }
  LeaveCriticalSection(&operation->lock);

  if (completion != NULL) {
    completion(completion_result, completion_state);
  }
}

static void bounce_win32_file_finish_terminal(
  BOUNCE_FILE_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result) {
  bounce_win32_file_finish(
    operation,
    completion_result,
    -1,
    ERROR_OPERATION_ABORTED);
}

static bool bounce_win32_file_unregister_cancellation_if_active(
  BOUNCE_FILE_IO *operation) {
  bool cancellation_registration_active;

  EnterCriticalSection(&operation->lock);
  if (!operation->active || operation->settling) {
    LeaveCriticalSection(&operation->lock);
    return false;
  }
  cancellation_registration_active =
    operation->cancellation_registration_active;
  LeaveCriticalSection(&operation->lock);

  if (!cancellation_registration_active) {
    return true;
  }

  if (!bounce_unregister_canceled(&operation->cancellation_registration)) {
    return false;
  }

  EnterCriticalSection(&operation->lock);
  if (!operation->active || operation->settling) {
    LeaveCriticalSection(&operation->lock);
    return false;
  }
  operation->cancellation_registration_active = false;
  LeaveCriticalSection(&operation->lock);
  return true;
}

static bool bounce_win32_file_begin_settle(BOUNCE_FILE_IO *operation) {
  if (!bounce_win32_file_unregister_cancellation_if_active(operation)) {
    return false;
  }

  EnterCriticalSection(&operation->lock);
  if (!operation->active || operation->settling) {
    LeaveCriticalSection(&operation->lock);
    return false;
  }
  operation->settling = true;
  LeaveCriticalSection(&operation->lock);
  return true;
}

static bool bounce_win32_file_clear_after_start_failure(
  BOUNCE_FILE_IO *operation) {
  if (bounce_win32_file_unregister_cancellation_if_active(operation)) {
    bounce_win32_file_clear_active(operation);
    return false;
  }
  return true;
}

static void bounce_win32_file_canceled_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;
  HANDLE handle = INVALID_HANDLE_VALUE;
  bool io_pending = false;

  (void)result;

  if (operation == NULL) {
    return;
  }

  EnterCriticalSection(&operation->lock);
  if (!operation->active || operation->settling) {
    LeaveCriticalSection(&operation->lock);
    return;
  }
  operation->settling = true;
  operation->cancel_requested = true;
  operation->cancellation_registration_active = false;
  handle = operation->handle;
  io_pending = operation->io_pending;
  LeaveCriticalSection(&operation->lock);

  if (io_pending && bounce_win32_file_handle_valid(handle)) {
    DWORD transferred = 0u;

    (void)CancelIoEx(handle, &operation->overlapped);
    (void)GetOverlappedResult(
      handle,
      &operation->overlapped,
      &transferred,
      TRUE);
  }

  bounce_win32_file_finish_terminal(
    operation,
    BOUNCE_COMPLETION_CANCELED);
}

static bool bounce_win32_file_register_cancellation(
  BOUNCE_FILE_IO *operation,
  BOUNCE_CANCELLATION *cancellation,
  bool *already_canceled) {
  if (already_canceled != NULL) {
    *already_canceled = false;
  }
  if (cancellation == NULL) {
    return true;
  }

  EnterCriticalSection(&operation->lock);
  operation->cancellation_registration_active = true;
  LeaveCriticalSection(&operation->lock);

  if (!bounce_register_canceled(
        operation->bounce,
        cancellation,
        &operation->cancellation_registration,
        bounce_win32_file_canceled_completion,
        operation)) {
    bounce_win32_file_clear_active(operation);
    return false;
  }

  if (already_canceled != NULL) {
    *already_canceled = cancellation->canceled != 0;
  }
  return true;
}

static bool bounce_win32_file_begin(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int operation_kind,
  HANDLE handle,
  void *buffer,
  const void *const_buffer,
  int64_t offset,
  size_t length,
  int whence,
  BOUNCE_FILE_FLUSH_MODE flush_mode,
  BOUNCE_COMPLETION completion,
  void *completion_state) {
  if ((r == NULL) ||
      (operation == NULL) ||
      !bounce_win32_file_handle_valid(handle) ||
      (completion == NULL) ||
      (operation->event_handle == NULL)) {
    return false;
  }

  if (((operation_kind == BOUNCE_WIN32_FILE_OP_READ) &&
       (length > 0u) &&
       (buffer == NULL)) ||
      ((operation_kind == BOUNCE_WIN32_FILE_OP_WRITE) &&
       (length > 0u) &&
       (const_buffer == NULL)) ||
      (((operation_kind == BOUNCE_WIN32_FILE_OP_READ) ||
        (operation_kind == BOUNCE_WIN32_FILE_OP_WRITE)) &&
       (!bounce_win32_file_offset_valid(offset) ||
        !bounce_win32_file_length_valid(length))) ||
      ((operation_kind == BOUNCE_WIN32_FILE_OP_FLUSH) &&
       (flush_mode != BOUNCE_FILE_FLUSH_FULL) &&
       (flush_mode != BOUNCE_FILE_FLUSH_DATA))) {
    return false;
  }

  EnterCriticalSection(&operation->lock);
  if (operation->active) {
    LeaveCriticalSection(&operation->lock);
    return false;
  }

  operation->bounce = r;
  operation->completion = completion;
  operation->completion_state = completion_state;
  operation->handle = handle;
  operation->buffer = buffer;
  operation->const_buffer = const_buffer;
  operation->offset = offset;
  operation->length = length;
  operation->whence = whence;
  operation->flush_mode = flush_mode;
  operation->operation = operation_kind;
  operation->result = -1;
  operation->error_code = 0;
  operation->active = true;
  operation->cancellation_registration_active = false;
  operation->io_pending = false;
  operation->cancel_requested = false;
  operation->settling = false;
  LeaveCriticalSection(&operation->lock);
  return true;
}

static void bounce_win32_file_run_seek(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  LARGE_INTEGER distance;
  LARGE_INTEGER new_position;

  *io_result = -1;
  *error_code = 0;
  distance.QuadPart = (LONGLONG)operation->offset;

  if (SetFilePointerEx(
        operation->handle,
        distance,
        &new_position,
        (DWORD)operation->whence)) {
    *io_result = (int64_t)new_position.QuadPart;
  } else {
    *error_code = (int)GetLastError();
  }
}

static void bounce_win32_file_run_flush(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  *io_result = -1;
  *error_code = 0;

  if (FlushFileBuffers(operation->handle)) {
    *io_result = 0;
  } else {
    *error_code = (int)GetLastError();
  }
}

static void bounce_win32_file_posted_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;
  int64_t io_result = -1;
  int error_code = 0;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_win32_file_finish_terminal(operation, result);
    return;
  }
  if ((operation == NULL) ||
      !bounce_win32_file_begin_settle(operation)) {
    return;
  }

  switch (operation->operation) {
    case BOUNCE_WIN32_FILE_OP_POSTED_RESULT:
      io_result = operation->result;
      error_code = operation->error_code;
      break;
    case BOUNCE_WIN32_FILE_OP_SEEK:
      bounce_win32_file_run_seek(operation, &io_result, &error_code);
      break;
    case BOUNCE_WIN32_FILE_OP_FLUSH:
      bounce_win32_file_run_flush(operation, &io_result, &error_code);
      break;
    default:
      io_result = -1;
      error_code = ERROR_INVALID_PARAMETER;
      break;
  }

  bounce_win32_file_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

static bool bounce_win32_file_post_started(
  BOUNCE_FILE_IO *operation,
  BOUNCE_CANCELLATION *cancellation) {
  bool already_canceled = false;

  if (!bounce_win32_file_register_cancellation(
        operation,
        cancellation,
        &already_canceled)) {
    return false;
  }
  if (already_canceled) {
    return true;
  }

  if (!bounce_post(
        operation->bounce,
        bounce_win32_file_posted_completion,
        operation)) {
    return bounce_win32_file_clear_after_start_failure(operation);
  }
  return true;
}

static void bounce_win32_file_collect_overlapped_result(
  BOUNCE_FILE_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result,
  BOOL wait_for_result) {
  DWORD transferred = 0u;
  BOOL result;
  int64_t io_result = -1;
  int error_code = 0;

  if (completion_result != BOUNCE_COMPLETION_COMPLETED) {
    if (bounce_win32_file_handle_valid(operation->handle)) {
      (void)CancelIoEx(operation->handle, &operation->overlapped);
      (void)GetOverlappedResult(
        operation->handle,
        &operation->overlapped,
        &transferred,
        TRUE);
    }
    bounce_win32_file_finish_terminal(operation, completion_result);
    return;
  }

  result = GetOverlappedResult(
    operation->handle,
    &operation->overlapped,
    &transferred,
    wait_for_result);
  if (result) {
    io_result = (int64_t)transferred;
  } else {
    error_code = (int)GetLastError();
  }

  bounce_win32_file_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

static void bounce_win32_file_overlapped_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;

  if ((operation == NULL) ||
      !bounce_win32_file_begin_settle(operation)) {
    return;
  }

  bounce_win32_file_collect_overlapped_result(
    operation,
    result,
    result != BOUNCE_COMPLETION_COMPLETED);
}

static bool bounce_win32_file_start_overlapped(
  BOUNCE_FILE_IO *operation,
  BOUNCE_CANCELLATION *cancellation) {
  bool already_canceled = false;
  BOOL call_result;
  DWORD error_code;
  uint64_t offset;

  if (!bounce_win32_file_register_cancellation(
        operation,
        cancellation,
        &already_canceled)) {
    return false;
  }
  if (already_canceled) {
    return true;
  }

  EnterCriticalSection(&operation->lock);
  if (!operation->active || operation->settling) {
    LeaveCriticalSection(&operation->lock);
    return true;
  }

  (void)ResetEvent(operation->event_handle);
  memset(&operation->overlapped, 0, sizeof operation->overlapped);
  operation->overlapped.hEvent = operation->event_handle;
  offset = (uint64_t)operation->offset;
  operation->overlapped.Offset = (DWORD)(offset & UINT64_C(0xffffffff));
  operation->overlapped.OffsetHigh = (DWORD)(offset >> 32);
  operation->io_pending = true;

  if (operation->operation == BOUNCE_WIN32_FILE_OP_READ) {
    call_result = ReadFile(
      operation->handle,
      operation->buffer,
      (DWORD)operation->length,
      NULL,
      &operation->overlapped);
  } else {
    call_result = WriteFile(
      operation->handle,
      operation->const_buffer,
      (DWORD)operation->length,
      NULL,
      &operation->overlapped);
  }

  if (call_result) {
    operation->io_pending = false;
    LeaveCriticalSection(&operation->lock);
    if (!bounce_post(
          operation->bounce,
          bounce_win32_file_overlapped_completion,
          operation)) {
      return bounce_win32_file_clear_after_start_failure(operation);
    }
    return true;
  }

  error_code = GetLastError();
  if (error_code == ERROR_IO_PENDING) {
    LeaveCriticalSection(&operation->lock);
    bounce_await_win32_handle(
      operation->bounce,
      operation->event_handle,
      bounce_win32_file_overlapped_completion,
      operation,
      NULL);
    return true;
  }

  operation->io_pending = false;
  operation->operation = BOUNCE_WIN32_FILE_OP_POSTED_RESULT;
  operation->result = -1;
  operation->error_code = (int)error_code;
  LeaveCriticalSection(&operation->lock);

  if (!bounce_post(
        operation->bounce,
        bounce_win32_file_posted_completion,
        operation)) {
    return bounce_win32_file_clear_after_start_failure(operation);
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////////

void bounce_file_io_init(BOUNCE_FILE_IO *operation) {
  if (operation == NULL) {
    return;
  }

  memset(operation, 0, sizeof *operation);
  InitializeCriticalSection(&operation->lock);
  bounce_cancellation_registration_init(&operation->cancellation_registration);
  operation->event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  operation->handle = INVALID_HANDLE_VALUE;
  operation->result = -1;
}

void bounce_file_io_deinit(BOUNCE_FILE_IO *operation) {
  HANDLE event_handle;

  if (operation == NULL) {
    return;
  }

  event_handle = operation->event_handle;
  operation->event_handle = NULL;
  bounce_cancellation_registration_deinit(&operation->cancellation_registration);
  if (event_handle != NULL) {
    CloseHandle(event_handle);
  }
  DeleteCriticalSection(&operation->lock);
}

int64_t bounce_file_io_result(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) ? operation->result : -1;
}

int bounce_file_io_error(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) ? operation->error_code : ERROR_INVALID_PARAMETER;
}

bool bounce_file_io_active(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) && operation->active;
}

bool bounce_await_file_read(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  HANDLE handle,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  if (!bounce_win32_file_begin(
        r,
        operation,
        BOUNCE_WIN32_FILE_OP_READ,
        handle,
        buffer,
        NULL,
        offset,
        length,
        0,
        BOUNCE_FILE_FLUSH_FULL,
        completion,
        completion_state)) {
    return false;
  }

  if (length == 0u) {
    operation->operation = BOUNCE_WIN32_FILE_OP_POSTED_RESULT;
    operation->result = 0;
    operation->error_code = 0;
    return bounce_win32_file_post_started(operation, cancellation);
  }

  return bounce_win32_file_start_overlapped(operation, cancellation);
}

bool bounce_await_file_write(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  HANDLE handle,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  if (!bounce_win32_file_begin(
        r,
        operation,
        BOUNCE_WIN32_FILE_OP_WRITE,
        handle,
        NULL,
        buffer,
        offset,
        length,
        0,
        BOUNCE_FILE_FLUSH_FULL,
        completion,
        completion_state)) {
    return false;
  }

  if (length == 0u) {
    operation->operation = BOUNCE_WIN32_FILE_OP_POSTED_RESULT;
    operation->result = 0;
    operation->error_code = 0;
    return bounce_win32_file_post_started(operation, cancellation);
  }

  return bounce_win32_file_start_overlapped(operation, cancellation);
}

bool bounce_await_file_seek(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  HANDLE handle,
  int64_t offset,
  int whence,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_win32_file_begin(
           r,
           operation,
           BOUNCE_WIN32_FILE_OP_SEEK,
           handle,
           NULL,
           NULL,
           offset,
           0u,
           whence,
           BOUNCE_FILE_FLUSH_FULL,
           completion,
           completion_state) &&
         bounce_win32_file_post_started(operation, cancellation);
}

bool bounce_await_file_flush(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  HANDLE handle,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_win32_file_begin(
           r,
           operation,
           BOUNCE_WIN32_FILE_OP_FLUSH,
           handle,
           NULL,
           NULL,
           0,
           0u,
           0,
           mode,
           completion,
           completion_state) &&
         bounce_win32_file_post_started(operation, cancellation);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
