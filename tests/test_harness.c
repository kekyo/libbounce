/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "test_harness.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TEST_RESULTS_ENV "LIBBOUNCE_TEST_RESULTS_DIR"
#define TEST_PATH_MAX_LENGTH 1024u

typedef struct TEST_STDIO_REDIRECT {
  int stdout_copy;
  int stderr_copy;
} TEST_STDIO_REDIRECT;

static int test_create_directory_single(const char *path) {
#ifdef _WIN32
  if (_mkdir(path) == 0) {
    return 0;
  }
#else
  if (mkdir(path, 0777) == 0) {
    return 0;
  }
#endif
  return (errno == EEXIST) ? 0 : -1;
}

static int test_ensure_directory(const char *path) {
  char buffer[ TEST_PATH_MAX_LENGTH ];
  size_t length = strlen(path);

  if ((length == 0u) || (length >= sizeof buffer)) {
    return -1;
  }

  memcpy(buffer, path, length + 1u);
  for (size_t index = 1u; index < length; index++) {
    if ((buffer[index] != '/') && (buffer[index] != '\\')) {
      continue;
    }

    if (buffer[index - 1u] == ':') {
      continue;
    }

    buffer[index] = '\0';
    if (test_create_directory_single(buffer) != 0) {
      return -1;
    }
    buffer[index] = path[index];
  }

  return test_create_directory_single(buffer);
}

static int test_format_results_root(char *buffer, size_t buffer_size) {
  const char *configured_root = getenv(TEST_RESULTS_ENV);

  if ((configured_root != NULL) && (configured_root[0] != '\0')) {
    const int written = snprintf(buffer, buffer_size, "%s", configured_root);
    return ((written >= 0) && ((size_t)written < buffer_size)) ? 0 : -1;
  }

#ifdef _WIN32
  {
    SYSTEMTIME now;
    int written;

    GetLocalTime(&now);
    written = snprintf(
      buffer,
      buffer_size,
      "test_results/%04u%02u%02u_%02u%02u%02u_%03u",
      (unsigned int)now.wYear,
      (unsigned int)now.wMonth,
      (unsigned int)now.wDay,
      (unsigned int)now.wHour,
      (unsigned int)now.wMinute,
      (unsigned int)now.wSecond,
      (unsigned int)now.wMilliseconds);
    return ((written >= 0) && ((size_t)written < buffer_size)) ? 0 : -1;
  }
#else
  {
    struct timespec now;
    struct tm local_time;
    int written;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
      return -1;
    }
    if (localtime_r(&now.tv_sec, &local_time) == NULL) {
      return -1;
    }

    written = snprintf(
      buffer,
      buffer_size,
      "test_results/%04d%02d%02d_%02d%02d%02d_%03ld",
      local_time.tm_year + 1900,
      local_time.tm_mon + 1,
      local_time.tm_mday,
      local_time.tm_hour,
      local_time.tm_min,
      local_time.tm_sec,
      now.tv_nsec / 1000000L);
    return ((written >= 0) && ((size_t)written < buffer_size)) ? 0 : -1;
  }
#endif
}

static int test_format_started_at(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  SYSTEMTIME now;
  int written;

  GetLocalTime(&now);
  written = snprintf(
    buffer,
    buffer_size,
    "%04u-%02u-%02u %02u:%02u:%02u.%03u",
    (unsigned int)now.wYear,
    (unsigned int)now.wMonth,
    (unsigned int)now.wDay,
    (unsigned int)now.wHour,
    (unsigned int)now.wMinute,
    (unsigned int)now.wSecond,
    (unsigned int)now.wMilliseconds);
  return ((written >= 0) && ((size_t)written < buffer_size)) ? 0 : -1;
#else
  struct timespec now;
  struct tm local_time;
  int written;

  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return -1;
  }
  if (localtime_r(&now.tv_sec, &local_time) == NULL) {
    return -1;
  }

  written = snprintf(
    buffer,
    buffer_size,
    "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
    local_time.tm_year + 1900,
    local_time.tm_mon + 1,
    local_time.tm_mday,
    local_time.tm_hour,
    local_time.tm_min,
    local_time.tm_sec,
    now.tv_nsec / 1000000L);
  return ((written >= 0) && ((size_t)written < buffer_size)) ? 0 : -1;
#endif
}

static uint64_t test_monotonic_now(void) {
#ifdef _WIN32
  LARGE_INTEGER counter;

  QueryPerformanceCounter(&counter);
  return (uint64_t)counter.QuadPart;
#else
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0u;
  }

  return ((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec;
#endif
}

static double test_elapsed_ms(uint64_t started, uint64_t finished) {
#ifdef _WIN32
  LARGE_INTEGER frequency;

  if (!QueryPerformanceFrequency(&frequency) || (frequency.QuadPart == 0)) {
    return 0.0;
  }

  return ((double)(finished - started) * 1000.0) / (double)frequency.QuadPart;
#else
  return (double)(finished - started) / 1000000.0;
#endif
}

static void test_restore_stdio(TEST_STDIO_REDIRECT *redirect) {
  fflush(stdout);
  fflush(stderr);

  if (redirect->stdout_copy >= 0) {
#ifdef _WIN32
    (void)_dup2(redirect->stdout_copy, _fileno(stdout));
    (void)_close(redirect->stdout_copy);
#else
    (void)dup2(redirect->stdout_copy, fileno(stdout));
    (void)close(redirect->stdout_copy);
#endif
    redirect->stdout_copy = -1;
  }

  if (redirect->stderr_copy >= 0) {
#ifdef _WIN32
    (void)_dup2(redirect->stderr_copy, _fileno(stderr));
    (void)_close(redirect->stderr_copy);
#else
    (void)dup2(redirect->stderr_copy, fileno(stderr));
    (void)close(redirect->stderr_copy);
#endif
    redirect->stderr_copy = -1;
  }

#ifdef _WIN32
  (void)setvbuf(stdout, NULL, _IOLBF, 0);
  (void)setvbuf(stderr, NULL, _IONBF, 0);
#endif
}

static int test_redirect_stdio(TEST_STDIO_REDIRECT *redirect, FILE *log_file) {
  redirect->stdout_copy = -1;
  redirect->stderr_copy = -1;

  fflush(stdout);
  fflush(stderr);

#ifdef _WIN32
  redirect->stdout_copy = _dup(_fileno(stdout));
  if (redirect->stdout_copy < 0) {
    return -1;
  }

  redirect->stderr_copy = _dup(_fileno(stderr));
  if (redirect->stderr_copy < 0) {
    test_restore_stdio(redirect);
    return -1;
  }

  if (_dup2(_fileno(log_file), _fileno(stdout)) != 0) {
    test_restore_stdio(redirect);
    return -1;
  }
  if (_dup2(_fileno(log_file), _fileno(stderr)) != 0) {
    test_restore_stdio(redirect);
    return -1;
  }
#else
  redirect->stdout_copy = dup(fileno(stdout));
  if (redirect->stdout_copy < 0) {
    return -1;
  }

  redirect->stderr_copy = dup(fileno(stderr));
  if (redirect->stderr_copy < 0) {
    test_restore_stdio(redirect);
    return -1;
  }

  if (dup2(fileno(log_file), fileno(stdout)) < 0) {
    test_restore_stdio(redirect);
    return -1;
  }
  if (dup2(fileno(log_file), fileno(stderr)) < 0) {
    test_restore_stdio(redirect);
    return -1;
  }
#endif

  return 0;
}

int test_run_suite(const char *suite_name, const TEST_CASE *cases, size_t case_count) {
  char results_root[ TEST_PATH_MAX_LENGTH ];
  char suite_directory[ TEST_PATH_MAX_LENGTH ];

  if ((suite_name == NULL) || (cases == NULL)) {
    fprintf(stderr, "test harness configuration is invalid\n");
    return 1;
  }

  if (test_format_results_root(results_root, sizeof results_root) != 0) {
    fprintf(stderr, "failed to resolve test results directory\n");
    return 1;
  }
  if (test_ensure_directory(results_root) != 0) {
    fprintf(stderr, "failed to create test results directory: %s\n", results_root);
    return 1;
  }

  if (snprintf(
        suite_directory,
        sizeof suite_directory,
        "%s/%s",
        results_root,
        suite_name) >= (int)sizeof suite_directory) {
    fprintf(stderr, "suite results path is too long for %s\n", suite_name);
    return 1;
  }
  if (test_ensure_directory(suite_directory) != 0) {
    fprintf(stderr, "failed to create suite results directory: %s\n", suite_directory);
    return 1;
  }

  (void)setvbuf(stdout, NULL, _IOLBF, 0);
  (void)setvbuf(stderr, NULL, _IONBF, 0);

  printf("saving test results to %s\n", suite_directory);

  for (size_t index = 0u; index < case_count; index++) {
    char started_at[64];
    char log_path[ TEST_PATH_MAX_LENGTH ];
    TEST_STDIO_REDIRECT redirect;
    FILE *log_file;
    uint64_t started;
    uint64_t finished;
    double duration_ms;

    if (snprintf(
          log_path,
          sizeof log_path,
          "%s/%s.log",
          suite_directory,
          cases[index].name) >= (int)sizeof log_path) {
      fprintf(stderr, "test results path is too long for %s\n", cases[index].name);
      return 1;
    }

    printf("running %s\n", cases[index].name);
    log_file = fopen(log_path, "w");
    if (log_file == NULL) {
      fprintf(stderr, "failed to open test result log: %s\n", log_path);
      return 1;
    }

    if (test_format_started_at(started_at, sizeof started_at) != 0) {
      fclose(log_file);
      fprintf(stderr, "failed to format test start time for %s\n", cases[index].name);
      return 1;
    }

    fprintf(log_file, "suite: %s\n", suite_name);
    fprintf(log_file, "test: %s\n", cases[index].name);
    fprintf(log_file, "started_at: %s\n", started_at);
    fprintf(log_file, "\n");
    fflush(log_file);

    if (test_redirect_stdio(&redirect, log_file) != 0) {
      fclose(log_file);
      fprintf(stderr, "failed to redirect stdio for %s\n", cases[index].name);
      return 1;
    }

    started = test_monotonic_now();
    cases[index].function();
    finished = test_monotonic_now();
    test_restore_stdio(&redirect);

    duration_ms = test_elapsed_ms(started, finished);
    fprintf(log_file, "\nstatus: PASS\n");
    fprintf(log_file, "duration_ms: %.3f\n", duration_ms);
    fclose(log_file);

    printf("passed %s (%.3f ms)\n", cases[index].name, duration_ms);
  }

  printf("all %s tests passed\n", suite_name);
  return 0;
}
