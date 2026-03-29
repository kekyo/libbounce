/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include "shared/test_posix_freertos_shared.h"

int main(void) {
  static const TEST_POSIX_FREERTOS_SHARED_CONFIG config = {
    "posix",
    true,
    true,
    true,
    true,
    true
  };

  return test_run_posix_freertos_shared_suite(&config);
}
