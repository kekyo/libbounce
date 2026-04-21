/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(BOUNCE_POSIX)

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <liburing.h>
#endif

#include "libbounce/bounce.h"

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_POSIX_FILE_OP_NONE 0
#define BOUNCE_POSIX_FILE_OP_READ 1
#define BOUNCE_POSIX_FILE_OP_WRITE 2
#define BOUNCE_POSIX_FILE_OP_SEEK 3
#define BOUNCE_POSIX_FILE_OP_FLUSH 4

#if defined(__linux__)
static void bounce_posix_file_io_uring_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state);
#endif

static inline int bounce_posix_file_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : EINVAL;
}

static inline int bounce_posix_file_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : EINVAL;
}

static inline bool bounce_posix_file_offset_valid(int64_t offset) {
  return (offset == BOUNCE_FILE_OFFSET_CURRENT) || (offset >= 0);
}

static void bounce_posix_file_clear_active(BOUNCE_FILE_IO *operation) {
  if (operation == NULL) {
    return;
  }

  (void)bounce_posix_file_lock(&operation->lock);
  operation->bounce = NULL;
  operation->completion = NULL;
  operation->completion_state = NULL;
  operation->active = false;
  operation->cancellation_registration_active = false;
  (void)bounce_posix_file_unlock(&operation->lock);
}

static void bounce_posix_file_finish(
  BOUNCE_FILE_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result,
  int64_t io_result,
  int error_code) {
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state = NULL;

  if (operation == NULL) {
    return;
  }

  (void)bounce_posix_file_lock(&operation->lock);
  if (operation->active) {
    operation->result = io_result;
    operation->error_code = error_code;
    completion = operation->completion;
    completion_state = operation->completion_state;
    operation->bounce = NULL;
    operation->completion = NULL;
    operation->completion_state = NULL;
    operation->active = false;
    operation->cancellation_registration_active = false;
  }
  (void)bounce_posix_file_unlock(&operation->lock);

  if (completion != NULL) {
    completion(completion_result, completion_state);
  }
}

static void bounce_posix_file_finish_terminal(
  BOUNCE_FILE_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result) {
  const int error_code =
    (completion_result == BOUNCE_COMPLETION_CANCELED) ? ECANCELED : EIO;

  bounce_posix_file_finish(
    operation,
    completion_result,
    -1,
    error_code);
}

static void bounce_posix_file_run_transfer(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  ssize_t result;

  *io_result = -1;
  *error_code = 0;

  if (operation->length == 0u) {
    *io_result = 0;
    return;
  }

  if (!bounce_posix_file_offset_valid(operation->offset)) {
    *error_code = EINVAL;
    return;
  }

  for (;;) {
    if (operation->operation == BOUNCE_POSIX_FILE_OP_READ) {
      result =
        (operation->offset == BOUNCE_FILE_OFFSET_CURRENT) ?
          read(
            operation->fd,
            operation->buffer,
            operation->length) :
          pread(
            operation->fd,
            operation->buffer,
            operation->length,
            (off_t)operation->offset);
    } else {
      result =
        (operation->offset == BOUNCE_FILE_OFFSET_CURRENT) ?
          write(
            operation->fd,
            operation->const_buffer,
            operation->length) :
          pwrite(
            operation->fd,
            operation->const_buffer,
            operation->length,
            (off_t)operation->offset);
    }

    if (result >= 0) {
      *io_result = (int64_t)result;
      return;
    }
    if (errno != EINTR) {
      *error_code = errno;
      return;
    }
  }
}

static void bounce_posix_file_run_seek(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  off_t result;

  *io_result = -1;
  *error_code = 0;

  do {
    result = lseek(
      operation->fd,
      (off_t)operation->offset,
      operation->whence);
  } while ((result < 0) && (errno == EINTR));

  if (result >= 0) {
    *io_result = (int64_t)result;
  } else {
    *error_code = errno;
  }
}

static void bounce_posix_file_run_flush(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  int result;

  *io_result = -1;
  *error_code = 0;

  do {
    result =
      (operation->flush_mode == BOUNCE_FILE_FLUSH_DATA) ?
        fdatasync(operation->fd) :
        fsync(operation->fd);
  } while ((result != 0) && (errno == EINTR));

  if (result == 0) {
    *io_result = 0;
  } else {
    *error_code = errno;
  }
}

static void bounce_posix_file_run_operation(
  BOUNCE_FILE_IO *operation,
  int64_t *io_result,
  int *error_code) {
  switch (operation->operation) {
    case BOUNCE_POSIX_FILE_OP_READ:
    case BOUNCE_POSIX_FILE_OP_WRITE:
      bounce_posix_file_run_transfer(
        operation,
        io_result,
        error_code);
      return;
    case BOUNCE_POSIX_FILE_OP_SEEK:
      bounce_posix_file_run_seek(
        operation,
        io_result,
        error_code);
      return;
    case BOUNCE_POSIX_FILE_OP_FLUSH:
      bounce_posix_file_run_flush(
        operation,
        io_result,
        error_code);
      return;
    default:
      *io_result = -1;
      *error_code = EINVAL;
      return;
  }
}

static bool bounce_posix_file_begin(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int operation_kind,
  int fd,
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
      (fd < 0) ||
      (completion == NULL)) {
    return false;
  }

  if (((operation_kind == BOUNCE_POSIX_FILE_OP_READ) &&
       (length > 0u) &&
       (buffer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_FILE_OP_WRITE) &&
       (length > 0u) &&
       (const_buffer == NULL)) ||
      (((operation_kind == BOUNCE_POSIX_FILE_OP_READ) ||
        (operation_kind == BOUNCE_POSIX_FILE_OP_WRITE)) &&
       !bounce_posix_file_offset_valid(offset)) ||
      ((operation_kind == BOUNCE_POSIX_FILE_OP_FLUSH) &&
       (flush_mode != BOUNCE_FILE_FLUSH_FULL) &&
       (flush_mode != BOUNCE_FILE_FLUSH_DATA))) {
    return false;
  }

  (void)bounce_posix_file_lock(&operation->lock);
  if (operation->active) {
    (void)bounce_posix_file_unlock(&operation->lock);
    return false;
  }

  operation->bounce = r;
  operation->completion = completion;
  operation->completion_state = completion_state;
  operation->fd = fd;
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
  (void)bounce_posix_file_unlock(&operation->lock);
  return true;
}

static bool bounce_posix_file_is_active(BOUNCE_FILE_IO *operation) {
  bool active;

  (void)bounce_posix_file_lock(&operation->lock);
  active = operation->active;
  (void)bounce_posix_file_unlock(&operation->lock);
  return active;
}

static void bounce_posix_file_canceled_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  (void)result;
  bounce_posix_file_finish_terminal(
    (BOUNCE_FILE_IO *)completion_state,
    BOUNCE_COMPLETION_CANCELED);
}

static bool bounce_posix_file_claim_posted_operation(
  BOUNCE_FILE_IO *operation) {
  bool cancellation_registration_active;

  (void)bounce_posix_file_lock(&operation->lock);
  if (!operation->active) {
    (void)bounce_posix_file_unlock(&operation->lock);
    return false;
  }
  cancellation_registration_active =
    operation->cancellation_registration_active;
  (void)bounce_posix_file_unlock(&operation->lock);

  if (!cancellation_registration_active) {
    return true;
  }

  if (!bounce_unregister_canceled(&operation->cancellation_registration)) {
    return false;
  }

  (void)bounce_posix_file_lock(&operation->lock);
  if (!operation->active) {
    (void)bounce_posix_file_unlock(&operation->lock);
    return false;
  }
  operation->cancellation_registration_active = false;
  (void)bounce_posix_file_unlock(&operation->lock);
  return true;
}

static void bounce_posix_file_posted_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;
  int64_t io_result;
  int error_code;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_file_finish_terminal(operation, result);
    return;
  }
  if (!bounce_posix_file_claim_posted_operation(operation)) {
    return;
  }

  bounce_posix_file_run_operation(
    operation,
    &io_result,
    &error_code);
  bounce_posix_file_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

static bool bounce_posix_file_post_started(
  BOUNCE_FILE_IO *operation,
  BOUNCE_CANCELLATION *cancellation) {
  bool cancellation_registered = false;

  if (cancellation != NULL) {
    (void)bounce_posix_file_lock(&operation->lock);
    operation->cancellation_registration_active = true;
    (void)bounce_posix_file_unlock(&operation->lock);

    if (!bounce_register_canceled(
          operation->bounce,
          cancellation,
          &operation->cancellation_registration,
          bounce_posix_file_canceled_completion,
          operation)) {
      bounce_posix_file_clear_active(operation);
      return false;
    }
    cancellation_registered = true;

    if (!bounce_posix_file_is_active(operation)) {
      return true;
    }
  }

  if (!bounce_post(
        operation->bounce,
        bounce_posix_file_posted_completion,
        operation)) {
    if (cancellation_registered &&
        !bounce_unregister_canceled(&operation->cancellation_registration)) {
      return true;
    }
    bounce_posix_file_clear_active(operation);
    return false;
  }
  return true;
}

static void bounce_posix_file_ready_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;
  int64_t io_result;
  int error_code;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_file_finish_terminal(operation, result);
    return;
  }

  bounce_posix_file_run_transfer(
    operation,
    &io_result,
    &error_code);
  bounce_posix_file_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

#if defined(__linux__)
static uint64_t bounce_posix_file_io_uring_offset(int64_t offset) {
  return (offset == BOUNCE_FILE_OFFSET_CURRENT) ?
           UINT64_MAX :
           (uint64_t)offset;
}

static void bounce_posix_file_io_uring_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)prepare_state;

  switch (operation->operation) {
    case BOUNCE_POSIX_FILE_OP_READ:
      io_uring_prep_read(
        sqe,
        operation->fd,
        operation->buffer,
        operation->length,
        bounce_posix_file_io_uring_offset(operation->offset));
      return;
    case BOUNCE_POSIX_FILE_OP_WRITE:
      io_uring_prep_write(
        sqe,
        operation->fd,
        operation->const_buffer,
        operation->length,
        bounce_posix_file_io_uring_offset(operation->offset));
      return;
    case BOUNCE_POSIX_FILE_OP_FLUSH:
      io_uring_prep_fsync(
        sqe,
        operation->fd,
        (operation->flush_mode == BOUNCE_FILE_FLUSH_DATA) ?
          IORING_FSYNC_DATASYNC :
          0u);
      return;
    default:
      io_uring_prep_nop(sqe);
      return;
  }
}

static void bounce_posix_file_io_uring_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_FILE_IO *operation = (BOUNCE_FILE_IO *)completion_state;
  const int io_uring_result =
    bounce_posix_io_uring_op_result(&operation->io_uring_operation);

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_file_finish_terminal(operation, result);
    return;
  }

  if (io_uring_result >= 0) {
    bounce_posix_file_finish(
      operation,
      BOUNCE_COMPLETION_COMPLETED,
      (int64_t)io_uring_result,
      0);
  } else {
    bounce_posix_file_finish(
      operation,
      BOUNCE_COMPLETION_COMPLETED,
      -1,
      -io_uring_result);
  }
}

static bool bounce_posix_file_can_use_io_uring(BOUNCE_CORE *r) {
  return (r != NULL) && r->linux_unified_wait_enabled;
}
#endif

//////////////////////////////////////////////////////////////////////////////////

void bounce_file_io_init(BOUNCE_FILE_IO *operation) {
  if (operation == NULL) {
    return;
  }

  memset(operation, 0, sizeof *operation);
  (void)pthread_mutex_init(&operation->lock, NULL);
  bounce_cancellation_registration_init(&operation->cancellation_registration);
#if defined(__linux__)
  bounce_posix_io_uring_op_init(
    &operation->io_uring_operation,
    bounce_posix_file_io_uring_prepare,
    operation);
#endif
  operation->fd = -1;
  operation->result = -1;
}

void bounce_file_io_deinit(BOUNCE_FILE_IO *operation) {
  if (operation == NULL) {
    return;
  }

#if defined(__linux__)
  bounce_posix_io_uring_op_deinit(&operation->io_uring_operation);
#endif
  bounce_cancellation_registration_deinit(&operation->cancellation_registration);
  (void)pthread_mutex_destroy(&operation->lock);
}

int64_t bounce_file_io_result(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) ? operation->result : -1;
}

int bounce_file_io_error(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) ? operation->error_code : EINVAL;
}

bool bounce_file_io_active(const BOUNCE_FILE_IO *operation) {
  return (operation != NULL) && operation->active;
}

bool bounce_await_file_read(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  if (!bounce_posix_file_begin(
        r,
        operation,
        BOUNCE_POSIX_FILE_OP_READ,
        fd,
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
    return bounce_posix_file_post_started(operation, cancellation);
  }

#if defined(__linux__)
  if (bounce_posix_file_can_use_io_uring(r)) {
    bounce_await_posix_io_uring_op(
      r,
      &operation->io_uring_operation,
      bounce_posix_file_io_uring_completion,
      operation,
      cancellation);
    return true;
  }
#endif

  bounce_await_posix_fd(
    r,
    fd,
    POLLIN,
    bounce_posix_file_ready_completion,
    operation,
    cancellation);
  return true;
}

bool bounce_await_file_write(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  if (!bounce_posix_file_begin(
        r,
        operation,
        BOUNCE_POSIX_FILE_OP_WRITE,
        fd,
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
    return bounce_posix_file_post_started(operation, cancellation);
  }

#if defined(__linux__)
  if (bounce_posix_file_can_use_io_uring(r)) {
    bounce_await_posix_io_uring_op(
      r,
      &operation->io_uring_operation,
      bounce_posix_file_io_uring_completion,
      operation,
      cancellation);
    return true;
  }
#endif

  bounce_await_posix_fd(
    r,
    fd,
    POLLOUT,
    bounce_posix_file_ready_completion,
    operation,
    cancellation);
  return true;
}

bool bounce_await_file_seek(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  int64_t offset,
  int whence,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_file_begin(
           r,
           operation,
           BOUNCE_POSIX_FILE_OP_SEEK,
           fd,
           NULL,
           NULL,
           offset,
           0u,
           whence,
           BOUNCE_FILE_FLUSH_FULL,
           completion,
           completion_state) &&
         bounce_posix_file_post_started(operation, cancellation);
}

bool bounce_await_file_flush(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  BOUNCE_FILE_FLUSH_MODE mode,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  if (!bounce_posix_file_begin(
        r,
        operation,
        BOUNCE_POSIX_FILE_OP_FLUSH,
        fd,
        NULL,
        NULL,
        0,
        0u,
        0,
        mode,
        completion,
        completion_state)) {
    return false;
  }

#if defined(__linux__)
  if (bounce_posix_file_can_use_io_uring(r)) {
    bounce_await_posix_io_uring_op(
      r,
      &operation->io_uring_operation,
      bounce_posix_file_io_uring_completion,
      operation,
      cancellation);
    return true;
  }
#endif

  return bounce_posix_file_post_started(operation, cancellation);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
