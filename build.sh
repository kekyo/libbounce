#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

. "$script_dir/test_results_env.sh"
libbounce_ensure_test_results_dir >/dev/null

sh "$script_dir/build_test_utils.sh"
sh "$script_dir/tests/test_prepare_wine_runtime.sh"
sh "$script_dir/tests/test_run_wine_test.sh"

sh "$script_dir/build_generic.sh" "$@"
sh "$script_dir/build_posix.sh" "$@"
sh "$script_dir/build_posix_glib.sh" "$@"
sh "$script_dir/build_win32.sh" "$@"
sh "$script_dir/build_freertos.sh" "$@"
sh "$script_dir/build_cxx20.sh" "$@"
