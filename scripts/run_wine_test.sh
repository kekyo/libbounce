#!/bin/sh

set -eu

wine_prefix=$1
wine_prefix_archive=$2
runtime_name=$3
test_binary=$4
shared_library=$5
wine_test_command=$6

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
prepare_script="$script_dir/prepare_wine_runtime.sh"
wine_command=${WINE:-wine}
wineboot_command=${WINEBOOT:-wineboot}

archive_prefix() {
	prefix_parent=$(dirname "$wine_prefix")
	prefix_name=$(basename "$wine_prefix")

	[ -d "$wine_prefix" ] || return 0

	rm -f "$wine_prefix_archive"
	tar -C "$prefix_parent" -cf "$wine_prefix_archive" "$prefix_name"
	rm -rf "$wine_prefix"
}

restore_prefix() {
	prefix_parent=$(dirname "$wine_prefix")

	rm -rf "$wine_prefix"
	mkdir -p "$prefix_parent"
	tar -C "$prefix_parent" -xf "$wine_prefix_archive"
}

ensure_prefix() {
	if [ -d "$wine_prefix" ]; then
		return 0
	fi

	if [ -f "$wine_prefix_archive" ]; then
		restore_prefix
		return 0
	fi

	rm -rf "$wine_prefix"
	if ! "$wineboot_command" -u; then
		printf '%s\n' "Win32 test runtime initialization failed for WINEARCH=${WINEARCH:-unknown}" >&2
		return 1
	fi
}

cleanup() {
	status=$1

	trap - EXIT HUP INT TERM
	if ! archive_prefix; then
		exit 1
	fi
	exit "$status"
}

trap 'status=$?; cleanup "$status"' EXIT HUP INT TERM

ensure_prefix
sh "$prepare_script" "$wine_prefix" "$runtime_name" "$test_binary" "$shared_library"
"$wine_command" "$wine_test_command"
