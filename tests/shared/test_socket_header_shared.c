/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include "libbounce/bounce.h"

unsigned int test_socket_header_c_compiles(void) {
  BOUNCE_SOCKET_IO operation;

  bounce_socket_io_init(&operation);
  bounce_socket_io_deinit(&operation);

  return (unsigned int)sizeof operation;
}
