#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

. "$script_dir/test_results_env.sh"
libbounce_ensure_test_results_dir >/dev/null

make -f Makefile.freertos clean
make -f Makefile.freertos test
