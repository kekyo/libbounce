#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

assert_dir() {
	[ -d "$1" ] || fail "Missing expected directory: $1"
}

assert_missing() {
	[ ! -e "$1" ] || fail "Unexpected path remains: $1"
}

assert_symlink_target() {
	path=$1
	expected=$2

	[ -L "$path" ] || fail "Missing expected symlink: $path"
	[ "$(readlink "$path")" = "$expected" ] || fail "Unexpected symlink target for $path"
}

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

wine_prefix="$tmp_dir/wineprefix"
runtime_name=libbounce-runtime
test_binary="$tmp_dir/build/tests/test_win32.exe"
shared_library="$tmp_dir/build/libbounce.dll"
extra_runtime_file="$tmp_dir/build/libbounce_win32_example.exe"
runtime_dir="$wine_prefix/drive_c/$runtime_name"

mkdir -p \
	"$wine_prefix/dosdevices" \
	"$wine_prefix/drive_c/users/tester/AppData/Roaming/Microsoft/Windows" \
	"$(dirname "$test_binary")" \
	"$(dirname "$shared_library")" \
	"$(dirname "$extra_runtime_file")" \
	"$runtime_dir"

printf '%s\n' 'test-binary' >"$test_binary"
printf '%s\n' 'shared-library' >"$shared_library"
printf '%s\n' 'example-binary' >"$extra_runtime_file"
printf '%s\n' 'stale' >"$runtime_dir/stale.txt"

ln -s ../drive_c "$wine_prefix/dosdevices/c:"
ln -s / "$wine_prefix/dosdevices/z:"
ln -s /dev/ttyS0 "$wine_prefix/dosdevices/com1"
ln -s /home/tester/Documents "$wine_prefix/drive_c/users/tester/Documents"
ln -s /home/tester/Templates "$wine_prefix/drive_c/users/tester/AppData/Roaming/Microsoft/Windows/Templates"

sh "$repo_root/scripts/prepare_wine_runtime.sh" "$wine_prefix" "$runtime_name" "$test_binary" "$shared_library" "$extra_runtime_file"

assert_symlink_target "$wine_prefix/dosdevices/c:" '../drive_c'
assert_missing "$wine_prefix/dosdevices/z:"
assert_missing "$wine_prefix/dosdevices/com1"
assert_dir "$wine_prefix/drive_c/users/tester/Documents"
assert_dir "$wine_prefix/drive_c/users/tester/AppData/Roaming/Microsoft/Windows/Templates"
assert_missing "$runtime_dir/stale.txt"
assert_file "$runtime_dir/test_win32.exe"
assert_file "$runtime_dir/libbounce.dll"
assert_file "$runtime_dir/libbounce_win32_example.exe"

cmp -s "$test_binary" "$runtime_dir/test_win32.exe" || fail 'Staged test binary does not match source'
cmp -s "$shared_library" "$runtime_dir/libbounce.dll" || fail 'Staged shared library does not match source'
cmp -s "$extra_runtime_file" "$runtime_dir/libbounce_win32_example.exe" || fail 'Staged example binary does not match source'
