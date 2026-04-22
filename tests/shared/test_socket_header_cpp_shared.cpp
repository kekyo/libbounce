/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <type_traits>

#include "libbounce/bounce.h"

static_assert(
  !std::is_copy_constructible<libbounce::socket_io>::value,
  "libbounce::socket_io must not be copy constructible when included from socket.h");
static_assert(
  !std::is_copy_assignable<libbounce::socket_io>::value,
  "libbounce::socket_io must not be copy assignable when included from socket.h");

bool test_socket_header_cpp_compiles(void) {
  libbounce::socket_io operation;

  return operation.get_socket_io() != nullptr;
}
