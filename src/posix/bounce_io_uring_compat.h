/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

 #ifndef LIBBOUNCE_IO_URING_COMPAT_H
#define LIBBOUNCE_IO_URING_COMPAT_H

#include <stdint.h>

#include <liburing.h>

/*
 * Older liburing releases expose only pointer-based user_data helpers.
 * libbounce stores pointer-derived tokens, so the pointer variants preserve
 * the encoded value while keeping older distro headers buildable.
 */
static inline void bounce_io_uring_prep_cancel_user_data(
  struct io_uring_sqe *sqe,
  uint64_t user_data,
  int flags) {
  io_uring_prep_cancel(sqe, (void *)(uintptr_t)user_data, flags);
}

static inline void bounce_io_uring_sqe_set_user_data(
  struct io_uring_sqe *sqe,
  uint64_t user_data) {
  io_uring_sqe_set_data(sqe, (void *)(uintptr_t)user_data);
}

#endif
