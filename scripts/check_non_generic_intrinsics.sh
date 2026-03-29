#!/bin/sh

set -eu

project_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$project_root"

failures=0

check_pattern() {
  description="$1"
  pattern="$2"
  shift 2

  if matches="$(rg -n --color never -- "$pattern" "$@" 2>/dev/null)"; then
    echo "FAILED: $description"
    echo "$matches"
    echo
    failures=1
  fi
}

check_pattern \
  "non-generic source/header files must not use GNU/C11 atomic intrinsics" \
  "__atomic_|__sync_|__c11_atomic_|memory_order_|<stdatomic\\.h>" \
  src/posix src/posix-glib src/freertos src/win32 \
  include/libbounce/posix.h \
  include/libbounce/posix_glib.h \
  include/libbounce/freertos.h \
  include/libbounce/win32.h

check_pattern \
  "non-generic win32 files must not use Interlocked APIs" \
  "Interlocked[A-Za-z0-9_]*" \
  src/win32 \
  include/libbounce/win32.h

check_pattern \
  "non-generic build files must not link libatomic" \
  "(^|[^[:alnum:]_])-latomic($|[^[:alnum:]_])" \
  Makefile.posix \
  Makefile.posix_glib \
  Makefile.freertos \
  Makefile.win32 \
  build_test_utils.sh

if [ "$failures" -ne 0 ]; then
  exit 1
fi

echo "OK: non-generic intrinsic deny-list check passed"
