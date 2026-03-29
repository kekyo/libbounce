#ifndef LIBBOUNCE_TEST_HARNESS_H
#define LIBBOUNCE_TEST_HARNESS_H

#include <stddef.h>

typedef void (*TEST_CASE_FUNCTION)(void);

typedef struct TEST_CASE {
  const char *name;
  TEST_CASE_FUNCTION function;
} TEST_CASE;

#define TEST_CASE_ENTRY(function_name) { #function_name, function_name }

int test_run_suite(const char *suite_name, const TEST_CASE *cases, size_t case_count);

#endif
