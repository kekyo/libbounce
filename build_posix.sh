#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

. "$script_dir/test_results_env.sh"
libbounce_ensure_test_results_dir >/dev/null

make -f Makefile.posix clean
make -f Makefile.posix test
