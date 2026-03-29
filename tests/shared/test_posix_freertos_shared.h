#ifndef LIBBOUNCE_TEST_POSIX_FREERTOS_SHARED_H
#define LIBBOUNCE_TEST_POSIX_FREERTOS_SHARED_H

#include <stdbool.h>

typedef struct TEST_POSIX_FREERTOS_SHARED_CONFIG {
  const char *suite_name;
  bool supports_cpp_timeout_cancellation;
  bool supports_fd_await;
  bool supports_timeout_rearm;
  bool supports_timeout_cancellation;
  bool supports_mixed_wait_cancellation;
} TEST_POSIX_FREERTOS_SHARED_CONFIG;

extern int test_run_posix_freertos_shared_suite(
  const TEST_POSIX_FREERTOS_SHARED_CONFIG *config);

#endif
