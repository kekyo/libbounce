/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include "libbounce/bounce.h"

unsigned int test_timer_header_c_compiles(void) {
  BOUNCE_TIMER timer;

  bounce_timer_init(&timer);
  bounce_timer_deinit(&timer);

  return (unsigned int)sizeof timer;
}
