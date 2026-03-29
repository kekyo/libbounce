#!/bin/sh

set -eu

CC="${CC:-cc}"
OUTPUT="${OUTPUT:-/tmp/libbounce-utils-test}"

"$CC" \
  -std=c99 \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic \
  -pthread \
  -Iinclude \
  src/utils.c \
  tests/utils_test.c \
  -o "$OUTPUT"

"$OUTPUT"
