/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <type_traits>

#include "libbounce/bounce.h"

static_assert(
  !std::is_copy_constructible<libbounce::file_io>::value,
  "libbounce::file_io must not be copy constructible when included from file.h");
static_assert(
  !std::is_copy_assignable<libbounce::file_io>::value,
  "libbounce::file_io must not be copy assignable when included from file.h");

bool test_file_header_cpp_compiles(void) {
  libbounce::file_io operation;

  return operation.get_file_io() != nullptr;
}
