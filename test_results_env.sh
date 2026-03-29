#!/bin/sh

libbounce_ensure_test_results_dir() {
  if [ -n "${LIBBOUNCE_TEST_RESULTS_DIR:-}" ]; then
    printf '%s\n' "$LIBBOUNCE_TEST_RESULTS_DIR"
    return 0
  fi

  results_dir="test_results/$(date '+%Y%m%d_%H%M%S_%3N')"
  mkdir -p "$results_dir"
  LIBBOUNCE_TEST_RESULTS_DIR="$results_dir"
  export LIBBOUNCE_TEST_RESULTS_DIR
  printf '%s\n' "$results_dir"
}
