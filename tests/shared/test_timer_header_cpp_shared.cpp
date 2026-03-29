/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <type_traits>

#include "libbounce/bounce.h"

static_assert(
  !std::is_copy_constructible<libbounce::timer>::value,
  "libbounce::timer must not be copy constructible when included from timer.h");
static_assert(
  !std::is_copy_assignable<libbounce::timer>::value,
  "libbounce::timer must not be copy assignable when included from timer.h");

bool test_timer_header_cpp_compiles(void) {
  libbounce::timer timer;

  return timer.get_timer() != nullptr;
}
