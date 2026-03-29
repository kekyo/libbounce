#!/bin/sh

set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
temp_dir="$(mktemp -d)"

cleanup() {
  rm -rf "$temp_dir"
}

trap cleanup EXIT INT TERM

cd "$temp_dir"
. "$repo_root/test_results_env.sh"
unset LIBBOUNCE_TEST_RESULTS_DIR

libbounce_ensure_test_results_dir >/dev/null
first_dir="$LIBBOUNCE_TEST_RESULTS_DIR"
[ -d "$first_dir" ]

case "$first_dir" in
  test_results/*)
    ;;
  *)
    echo "unexpected results dir: $first_dir" >&2
    exit 1
    ;;
esac

libbounce_ensure_test_results_dir >/dev/null
second_dir="$LIBBOUNCE_TEST_RESULTS_DIR"
[ "$second_dir" = "$first_dir" ]

third_dir="$(LIBBOUNCE_TEST_RESULTS_DIR="$first_dir" sh "$repo_root/create_test_results_dir.sh")"
[ "$third_dir" = "$first_dir" ]
