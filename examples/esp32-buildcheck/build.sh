#!/usr/bin/env bash

set -eu

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

idf_path="${IDF_PATH:-}"

if [ -z "$idf_path" ]; then
  for candidate in \
    "$HOME/.espressif/v6.0/esp-idf" \
    "$HOME/esp-idf" \
    "$HOME/esp/esp-idf"
  do
    if [ -f "$candidate/export.sh" ]; then
      idf_path="$candidate"
      break
    fi
  done
fi

if [ -z "$idf_path" ] || [ ! -f "$idf_path/export.sh" ]; then
  echo "ESP-IDF export.sh was not found. Set IDF_PATH before running this script." >&2
  exit 1
fi

. "$idf_path/export.sh" >/dev/null

cd "$script_dir"

if [ ! -f sdkconfig ]; then
  idf.py set-target esp32
fi

idf.py build
