/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#if defined(__linux__)
#include <liburing.h>
#endif

#include "libbounce/bounce.h"

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_POSIX_SOCKET_OP_NONE 0
#define BOUNCE_POSIX_SOCKET_OP_RECV 1
#define BOUNCE_POSIX_SOCKET_OP_SEND 2
#define BOUNCE_POSIX_SOCKET_OP_RECVFROM 3
#define BOUNCE_POSIX_SOCKET_OP_SENDTO 4
#define BOUNCE_POSIX_SOCKET_OP_RECVMSG 5
#define BOUNCE_POSIX_SOCKET_OP_SENDMSG 6

#if defined(MSG_DONTWAIT)
#define BOUNCE_POSIX_SOCKET_DONTWAIT_FLAG MSG_DONTWAIT
#else
#define BOUNCE_POSIX_SOCKET_DONTWAIT_FLAG 0
#endif

#if defined(__linux__)
static void bounce_posix_socket_io_uring_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state);
#endif

static inline int bounce_posix_socket_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : EINVAL;
}

static inline int bounce_posix_socket_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : EINVAL;
}

static inline bool bounce_posix_socket_user_requested_dontwait(
  const BOUNCE_SOCKET_IO *operation) {
#if defined(MSG_DONTWAIT)
  return (operation != NULL) &&
         ((operation->flags & MSG_DONTWAIT) != 0);
#else
  (void)operation;
  return false;
#endif
}

static inline int bounce_posix_socket_syscall_flags(
  const BOUNCE_SOCKET_IO *operation) {
  return (operation != NULL) ?
           (operation->flags | BOUNCE_POSIX_SOCKET_DONTWAIT_FLAG) :
           BOUNCE_POSIX_SOCKET_DONTWAIT_FLAG;
}

static inline bool bounce_posix_socket_error_is_would_block(int error_code) {
  return (error_code == EAGAIN)
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
         || (error_code == EWOULDBLOCK)
#endif
         ;
}

static inline bool bounce_posix_socket_should_retry_would_block(
  const BOUNCE_SOCKET_IO *operation,
  int error_code) {
  return bounce_posix_socket_error_is_would_block(error_code) &&
         !bounce_posix_socket_user_requested_dontwait(operation);
}

static inline bool bounce_posix_socket_operation_writable(int operation_kind) {
  return (operation_kind == BOUNCE_POSIX_SOCKET_OP_SEND) ||
         (operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDTO) ||
         (operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDMSG);
}

static void bounce_posix_socket_clear_active(BOUNCE_SOCKET_IO *operation) {
  if (operation == NULL) {
    return;
  }

  (void)bounce_posix_socket_lock(&operation->lock);
  operation->bounce = NULL;
  operation->completion = NULL;
  operation->completion_state = NULL;
  operation->cancellation = NULL;
  operation->active = false;
  operation->cancellation_registration_active = false;
  (void)bounce_posix_socket_unlock(&operation->lock);
}

static void bounce_posix_socket_finish(
  BOUNCE_SOCKET_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result,
  int64_t io_result,
  int error_code) {
  BOUNCE_COMPLETION completion = NULL;
  void *completion_state = NULL;

  if (operation == NULL) {
    return;
  }

  (void)bounce_posix_socket_lock(&operation->lock);
  if (operation->active) {
    operation->result = io_result;
    operation->error_code = error_code;
    completion = operation->completion;
    completion_state = operation->completion_state;
    operation->bounce = NULL;
    operation->completion = NULL;
    operation->completion_state = NULL;
    operation->cancellation = NULL;
    operation->active = false;
    operation->cancellation_registration_active = false;
  }
  (void)bounce_posix_socket_unlock(&operation->lock);

  if (completion != NULL) {
    completion(completion_result, completion_state);
  }
}

static void bounce_posix_socket_finish_terminal(
  BOUNCE_SOCKET_IO *operation,
  BOUNCE_COMPLETION_RESULT completion_result) {
  const int error_code =
    (completion_result == BOUNCE_COMPLETION_CANCELED) ? ECANCELED : EIO;

  bounce_posix_socket_finish(
    operation,
    completion_result,
    -1,
    error_code);
}

static void bounce_posix_socket_run_operation(
  BOUNCE_SOCKET_IO *operation,
  int64_t *io_result,
  int *error_code) {
  ssize_t result;
  const int flags = bounce_posix_socket_syscall_flags(operation);

  *io_result = -1;
  *error_code = 0;

  for (;;) {
    switch (operation->operation) {
      case BOUNCE_POSIX_SOCKET_OP_RECV:
        result = recv(
          operation->fd,
          operation->buffer,
          operation->length,
          flags);
        break;
      case BOUNCE_POSIX_SOCKET_OP_SEND:
        result = send(
          operation->fd,
          operation->const_buffer,
          operation->length,
          flags);
        break;
      case BOUNCE_POSIX_SOCKET_OP_RECVFROM:
        result = recvfrom(
          operation->fd,
          operation->buffer,
          operation->length,
          flags,
          operation->address,
          operation->address_length_pointer);
        break;
      case BOUNCE_POSIX_SOCKET_OP_SENDTO:
        result = sendto(
          operation->fd,
          operation->const_buffer,
          operation->length,
          flags,
          operation->const_address,
          operation->address_length);
        break;
      case BOUNCE_POSIX_SOCKET_OP_RECVMSG:
        result = recvmsg(
          operation->fd,
          operation->message,
          flags);
        break;
      case BOUNCE_POSIX_SOCKET_OP_SENDMSG:
        result = sendmsg(
          operation->fd,
          operation->const_message,
          flags);
        break;
      default:
        *error_code = EINVAL;
        return;
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

static bool bounce_posix_socket_begin(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int operation_kind,
  int fd,
  void *buffer,
  const void *const_buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  const struct sockaddr *const_address,
  socklen_t address_length,
  socklen_t *address_length_pointer,
  struct msghdr *message,
  const struct msghdr *const_message,
  BOUNCE_COMPLETION completion,
  void *completion_state) {
  if ((r == NULL) ||
      (operation == NULL) ||
      (fd < 0) ||
      (completion == NULL)) {
    return false;
  }

  if (((operation_kind == BOUNCE_POSIX_SOCKET_OP_RECV) &&
       (length > 0u) &&
       (buffer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_SEND) &&
       (length > 0u) &&
       (const_buffer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_RECVFROM) &&
       (length > 0u) &&
       (buffer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_RECVFROM) &&
       (address != NULL) &&
       (address_length_pointer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDTO) &&
       (length > 0u) &&
       (const_buffer == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDTO) &&
       (const_address == NULL) &&
       (address_length != 0u)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_RECVMSG) &&
       (message == NULL)) ||
      ((operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDMSG) &&
       (const_message == NULL))) {
    return false;
  }

  (void)bounce_posix_socket_lock(&operation->lock);
  if (operation->active) {
    (void)bounce_posix_socket_unlock(&operation->lock);
    return false;
  }

  operation->bounce = r;
  operation->completion = completion;
  operation->completion_state = completion_state;
  operation->fd = fd;
  operation->buffer = buffer;
  operation->const_buffer = const_buffer;
  operation->address = address;
  operation->const_address = const_address;
  operation->address_length = address_length;
  operation->address_length_pointer = address_length_pointer;
  operation->message = message;
  operation->const_message = const_message;
  operation->length = length;
  operation->flags = flags;
  operation->operation = operation_kind;
  operation->result = -1;
  operation->error_code = 0;
  operation->active = true;
  operation->cancellation_registration_active = false;
  (void)bounce_posix_socket_unlock(&operation->lock);
  return true;
}

static bool bounce_posix_socket_is_active(BOUNCE_SOCKET_IO *operation) {
  bool active;

  (void)bounce_posix_socket_lock(&operation->lock);
  active = operation->active;
  (void)bounce_posix_socket_unlock(&operation->lock);
  return active;
}

static void bounce_posix_socket_canceled_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  (void)result;
  bounce_posix_socket_finish_terminal(
    (BOUNCE_SOCKET_IO *)completion_state,
    BOUNCE_COMPLETION_CANCELED);
}

static bool bounce_posix_socket_claim_posted_operation(
  BOUNCE_SOCKET_IO *operation) {
  bool cancellation_registration_active;

  (void)bounce_posix_socket_lock(&operation->lock);
  if (!operation->active) {
    (void)bounce_posix_socket_unlock(&operation->lock);
    return false;
  }
  cancellation_registration_active =
    operation->cancellation_registration_active;
  (void)bounce_posix_socket_unlock(&operation->lock);

  if (!cancellation_registration_active) {
    return true;
  }

  if (!bounce_unregister_canceled(&operation->cancellation_registration)) {
    return false;
  }

  (void)bounce_posix_socket_lock(&operation->lock);
  if (!operation->active) {
    (void)bounce_posix_socket_unlock(&operation->lock);
    return false;
  }
  operation->cancellation_registration_active = false;
  (void)bounce_posix_socket_unlock(&operation->lock);
  return true;
}

static void bounce_posix_socket_posted_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_SOCKET_IO *operation = (BOUNCE_SOCKET_IO *)completion_state;
  int64_t io_result;
  int error_code;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_socket_finish_terminal(operation, result);
    return;
  }
  if (!bounce_posix_socket_claim_posted_operation(operation)) {
    return;
  }

  bounce_posix_socket_run_operation(
    operation,
    &io_result,
    &error_code);
  bounce_posix_socket_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

static bool bounce_posix_socket_post_started(
  BOUNCE_SOCKET_IO *operation,
  BOUNCE_CANCELLATION *cancellation) {
  bool cancellation_registered = false;

  if (cancellation != NULL) {
    (void)bounce_posix_socket_lock(&operation->lock);
    operation->cancellation_registration_active = true;
    (void)bounce_posix_socket_unlock(&operation->lock);

    if (!bounce_register_canceled(
          operation->bounce,
          cancellation,
          &operation->cancellation_registration,
          bounce_posix_socket_canceled_completion,
          operation)) {
      bounce_posix_socket_clear_active(operation);
      return false;
    }
    cancellation_registered = true;

    if (!bounce_posix_socket_is_active(operation)) {
      return true;
    }
  }

  if (!bounce_post(
        operation->bounce,
        bounce_posix_socket_posted_completion,
        operation)) {
    if (cancellation_registered &&
        !bounce_unregister_canceled(&operation->cancellation_registration)) {
      return true;
    }
    bounce_posix_socket_clear_active(operation);
    return false;
  }
  return true;
}

static void bounce_posix_socket_await_ready(
  BOUNCE_CORE *r,
  int fd,
  bool writable,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
#if defined(BOUNCE_POSIX_GLIB)
  bounce_await_posix_glib_fd(
    r,
    fd,
    writable ? G_IO_OUT : G_IO_IN,
    completion,
    completion_state,
    cancellation);
#else
  bounce_await_posix_fd(
    r,
    fd,
    writable ? POLLOUT : POLLIN,
    completion,
    completion_state,
    cancellation);
#endif
}

static void bounce_posix_socket_ready_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_SOCKET_IO *operation = (BOUNCE_SOCKET_IO *)completion_state;
  int64_t io_result;
  int error_code;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_socket_finish_terminal(operation, result);
    return;
  }

  bounce_posix_socket_run_operation(
    operation,
    &io_result,
    &error_code);
  if (bounce_posix_socket_should_retry_would_block(operation, error_code)) {
    bounce_posix_socket_await_ready(
      operation->bounce,
      operation->fd,
      bounce_posix_socket_operation_writable(operation->operation),
      bounce_posix_socket_ready_completion,
      operation,
      operation->cancellation);
    return;
  }

  bounce_posix_socket_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    io_result,
    error_code);
}

static bool bounce_posix_socket_should_post_without_wait(
  const BOUNCE_SOCKET_IO *operation) {
  return bounce_posix_socket_user_requested_dontwait(operation) ||
         (((operation->operation == BOUNCE_POSIX_SOCKET_OP_RECV) ||
           (operation->operation == BOUNCE_POSIX_SOCKET_OP_SEND) ||
           (operation->operation == BOUNCE_POSIX_SOCKET_OP_RECVFROM) ||
           (operation->operation == BOUNCE_POSIX_SOCKET_OP_SENDTO)) &&
          (operation->length == 0u));
}

#if defined(__linux__)
static void bounce_posix_socket_io_uring_prepare(
  struct io_uring_sqe *sqe,
  void *prepare_state) {
  BOUNCE_SOCKET_IO *operation = (BOUNCE_SOCKET_IO *)prepare_state;

  switch (operation->operation) {
    case BOUNCE_POSIX_SOCKET_OP_RECV:
      io_uring_prep_recv(
        sqe,
        operation->fd,
        operation->buffer,
        operation->length,
        operation->flags);
      return;
    case BOUNCE_POSIX_SOCKET_OP_SEND:
      io_uring_prep_send(
        sqe,
        operation->fd,
        operation->const_buffer,
        operation->length,
        operation->flags);
      return;
    case BOUNCE_POSIX_SOCKET_OP_SENDTO:
      io_uring_prep_sendto(
        sqe,
        operation->fd,
        operation->const_buffer,
        operation->length,
        operation->flags,
        operation->const_address,
        operation->address_length);
      return;
    case BOUNCE_POSIX_SOCKET_OP_RECVMSG:
      io_uring_prep_recvmsg(
        sqe,
        operation->fd,
        operation->message,
        operation->flags);
      return;
    case BOUNCE_POSIX_SOCKET_OP_SENDMSG:
      io_uring_prep_sendmsg(
        sqe,
        operation->fd,
        operation->const_message,
        operation->flags);
      return;
    default:
      io_uring_prep_nop(sqe);
      return;
  }
}

static bool bounce_posix_socket_can_use_io_uring(
  BOUNCE_CORE *r,
  int operation_kind) {
  bool backend_ready;

#if defined(BOUNCE_POSIX_GLIB)
  backend_ready = (r != NULL) && (r->io_uring_ring != NULL);
#else
  backend_ready = (r != NULL) && r->linux_unified_wait_enabled;
#endif

  return backend_ready &&
         ((operation_kind == BOUNCE_POSIX_SOCKET_OP_RECV) ||
          (operation_kind == BOUNCE_POSIX_SOCKET_OP_SEND) ||
          (operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDTO) ||
          (operation_kind == BOUNCE_POSIX_SOCKET_OP_RECVMSG) ||
          (operation_kind == BOUNCE_POSIX_SOCKET_OP_SENDMSG));
}

static void bounce_posix_socket_await_io_uring(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_IO_URING_OP *io_uring_operation,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
#if defined(BOUNCE_POSIX_GLIB)
  bounce_await_posix_glib_io_uring_op(
    r,
    io_uring_operation,
    completion,
    completion_state,
    cancellation);
#else
  bounce_await_posix_io_uring_op(
    r,
    io_uring_operation,
    completion,
    completion_state,
    cancellation);
#endif
}

static void bounce_posix_socket_io_uring_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  BOUNCE_SOCKET_IO *operation = (BOUNCE_SOCKET_IO *)completion_state;
  const int io_uring_result =
    bounce_posix_io_uring_op_result(&operation->io_uring_operation);
  const int error_code =
    (io_uring_result < 0) ? -io_uring_result : 0;

  if (result != BOUNCE_COMPLETION_COMPLETED) {
    bounce_posix_socket_finish_terminal(operation, result);
    return;
  }

  if (io_uring_result >= 0) {
    bounce_posix_socket_finish(
      operation,
      BOUNCE_COMPLETION_COMPLETED,
      (int64_t)io_uring_result,
      0);
    return;
  }

  if (bounce_posix_socket_should_retry_would_block(operation, error_code)) {
    bounce_posix_socket_await_ready(
      operation->bounce,
      operation->fd,
      bounce_posix_socket_operation_writable(operation->operation),
      bounce_posix_socket_ready_completion,
      operation,
      operation->cancellation);
    return;
  }

  bounce_posix_socket_finish(
    operation,
    BOUNCE_COMPLETION_COMPLETED,
    -1,
    error_code);
}
#endif

static bool bounce_posix_socket_start(
  BOUNCE_SOCKET_IO *operation,
  BOUNCE_CANCELLATION *cancellation) {
  operation->cancellation = cancellation;

  if (bounce_posix_socket_should_post_without_wait(operation)) {
    return bounce_posix_socket_post_started(operation, cancellation);
  }

#if defined(__linux__)
  if (bounce_posix_socket_can_use_io_uring(
        operation->bounce,
        operation->operation)) {
    bounce_posix_socket_await_io_uring(
      operation->bounce,
      &operation->io_uring_operation,
      bounce_posix_socket_io_uring_completion,
      operation,
      cancellation);
    return true;
  }
#endif

  bounce_posix_socket_await_ready(
    operation->bounce,
    operation->fd,
    bounce_posix_socket_operation_writable(operation->operation),
    bounce_posix_socket_ready_completion,
    operation,
    cancellation);
  return true;
}

//////////////////////////////////////////////////////////////////////////////////

void bounce_socket_io_init(BOUNCE_SOCKET_IO *operation) {
  if (operation == NULL) {
    return;
  }

  memset(operation, 0, sizeof *operation);
  (void)pthread_mutex_init(&operation->lock, NULL);
  bounce_cancellation_registration_init(&operation->cancellation_registration);
#if defined(__linux__)
  bounce_posix_io_uring_op_init(
    &operation->io_uring_operation,
    bounce_posix_socket_io_uring_prepare,
    operation);
#endif
  operation->fd = -1;
  operation->result = -1;
}

void bounce_socket_io_deinit(BOUNCE_SOCKET_IO *operation) {
  if (operation == NULL) {
    return;
  }

#if defined(__linux__)
  bounce_posix_io_uring_op_deinit(&operation->io_uring_operation);
#endif
  bounce_cancellation_registration_deinit(&operation->cancellation_registration);
  (void)pthread_mutex_destroy(&operation->lock);
}

int64_t bounce_socket_io_result(const BOUNCE_SOCKET_IO *operation) {
  return (operation != NULL) ? operation->result : -1;
}

int bounce_socket_io_error(const BOUNCE_SOCKET_IO *operation) {
  return (operation != NULL) ? operation->error_code : EINVAL;
}

bool bounce_socket_io_active(const BOUNCE_SOCKET_IO *operation) {
  return (operation != NULL) && operation->active;
}

bool bounce_await_socket_recv(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_RECV,
           fd,
           buffer,
           NULL,
           length,
           flags,
           NULL,
           NULL,
           0u,
           NULL,
           NULL,
           NULL,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

bool bounce_await_socket_send(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_SEND,
           fd,
           NULL,
           buffer,
           length,
           flags,
           NULL,
           NULL,
           0u,
           NULL,
           NULL,
           NULL,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

bool bounce_await_socket_recvfrom(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_RECVFROM,
           fd,
           buffer,
           NULL,
           length,
           flags,
           address,
           NULL,
           0u,
           address_length,
           NULL,
           NULL,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

bool bounce_await_socket_sendto(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_SENDTO,
           fd,
           NULL,
           buffer,
           length,
           flags,
           NULL,
           address,
           address_length,
           NULL,
           NULL,
           NULL,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

bool bounce_await_socket_recvmsg(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_RECVMSG,
           fd,
           NULL,
           NULL,
           0u,
           flags,
           NULL,
           NULL,
           0u,
           NULL,
           message,
           NULL,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

bool bounce_await_socket_sendmsg(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  return bounce_posix_socket_begin(
           r,
           operation,
           BOUNCE_POSIX_SOCKET_OP_SENDMSG,
           fd,
           NULL,
           NULL,
           0u,
           flags,
           NULL,
           NULL,
           0u,
           NULL,
           NULL,
           message,
           completion,
           completion_state) &&
         bounce_posix_socket_start(operation, cancellation);
}

//////////////////////////////////////////////////////////////////////////////////

#endif
