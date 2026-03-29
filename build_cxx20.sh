#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

. "$script_dir/test_results_env.sh"
libbounce_ensure_test_results_dir >/dev/null

HOST_CPPFLAGS='-Iinclude -I. -Itests -DLIBBOUNCE_ENABLE_COROUTINE_TESTS=1'
HOST_CXXFLAGS='-std=c++20 -O2 -g -Wall -Wextra -Werror -pedantic'
WIN32_CXXFLAGS="$HOST_CXXFLAGS -fcoroutines"
FREERTOS_CPPFLAGS='-D_XOPEN_SOURCE=700 -Iinclude -I. -Itests -Itests/freertos -DLIBBOUNCE_ENABLE_COROUTINE_TESTS=1'
FREERTOS_CXXFLAGS='-std=c++20 -O2 -Wall -Wextra -pedantic -Wno-unused-parameter -Wno-unused-function -fcoroutines'

make -f Makefile.win32 \
  clean \
  CC=x86_64-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/x86_64-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"
make -f Makefile.win32 \
  clean \
  CC=i686-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/i686-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"

make -f Makefile.win32 \
  all \
  CC=x86_64-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/x86_64-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"
make -f Makefile.win32 \
  all \
  CC=i686-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/i686-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"

make -f Makefile.win32 \
  test \
  CC=x86_64-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/x86_64-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"
make -f Makefile.win32 \
  test \
  CC=i686-w64-mingw32-gcc-win32 \
  BUILD_DIR=build/i686-w64-mingw32-gcc-win32_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$WIN32_CXXFLAGS"

make -f Makefile.freertos \
  clean \
  BUILD_DIR=build/freertos_cxx20 \
  CPPFLAGS="$FREERTOS_CPPFLAGS" \
  CXXFLAGS="$FREERTOS_CXXFLAGS"
make -f Makefile.freertos \
  test \
  BUILD_DIR=build/freertos_cxx20 \
  CPPFLAGS="$FREERTOS_CPPFLAGS" \
  CXXFLAGS="$FREERTOS_CXXFLAGS"

make -f Makefile.posix \
  clean \
  BUILD_DIR=build/posix_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"
make -f Makefile.posix \
  test \
  BUILD_DIR=build/posix_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"

make -f Makefile.generic \
  clean \
  BUILD_DIR=build/generic_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"
make -f Makefile.generic \
  test \
  BUILD_DIR=build/generic_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"

make -f Makefile.posix_glib \
  clean \
  BUILD_DIR=build/posix_glib_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"
make -f Makefile.posix_glib \
  test \
  BUILD_DIR=build/posix_glib_cxx20 \
  CPPFLAGS="$HOST_CPPFLAGS" \
  CXXFLAGS="$HOST_CXXFLAGS"
