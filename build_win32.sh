#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

. "$script_dir/test_results_env.sh"
libbounce_ensure_test_results_dir >/dev/null

make "$@" -f Makefile.win32 clean CC=x86_64-w64-mingw32-gcc-win32
make "$@" -f Makefile.win32 clean CC=i686-w64-mingw32-gcc-win32

make "$@" -f Makefile.win32 all CC=x86_64-w64-mingw32-gcc-win32
make "$@" -f Makefile.win32 all CC=i686-w64-mingw32-gcc-win32

make "$@" -f Makefile.win32 test CC=x86_64-w64-mingw32-gcc-win32
make "$@" -f Makefile.win32 test CC=i686-w64-mingw32-gcc-win32
