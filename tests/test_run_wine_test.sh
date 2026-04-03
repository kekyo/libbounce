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

assert_missing() {
	[ ! -e "$1" ] || fail "Unexpected path remains: $1"
}

assert_contains() {
	target_path=$1
	expected_text=$2

	grep -Fx "$expected_text" "$target_path" >/dev/null 2>&1 || fail "Missing expected text in $target_path: $expected_text"
}

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

wine_prefix="$tmp_dir/build/wineprefix"
wine_prefix_archive="$tmp_dir/build/wineprefix.tar"
runtime_name=libbounce-runtime
test_binary="$tmp_dir/build/tests/test_win32.exe"
shared_library="$tmp_dir/build/libbounce.dll"
extra_runtime_file="$tmp_dir/build/libbounce_win32_example.exe"
wine_test_command='C:/libbounce-runtime/test_win32.exe'
wineboot_log="$tmp_dir/wineboot.log"
wine_log="$tmp_dir/wine.log"
fake_bin_dir="$tmp_dir/bin"
fake_wineboot="$fake_bin_dir/fake-wineboot"
fake_wine="$fake_bin_dir/fake-wine"
tar_listing="$tmp_dir/archive.txt"

mkdir -p "$fake_bin_dir" "$(dirname "$test_binary")" "$(dirname "$shared_library")" "$(dirname "$extra_runtime_file")"

printf '%s\n' 'test-binary' >"$test_binary"
printf '%s\n' 'shared-library' >"$shared_library"
printf '%s\n' 'example-binary' >"$extra_runtime_file"

cat <<EOF >"$fake_wineboot"
#!/bin/sh
set -eu
printf '%s\n' "\$WINEARCH" >>"$wineboot_log"
mkdir -p "\$WINEPREFIX/dosdevices" "\$WINEPREFIX/drive_c/users/tester/AppData/Roaming/Microsoft/Windows"
ln -s ../drive_c "\$WINEPREFIX/dosdevices/c:"
ln -s / "\$WINEPREFIX/dosdevices/z:"
ln -s /dev/ttyS0 "\$WINEPREFIX/dosdevices/com1"
ln -s /home/tester/Documents "\$WINEPREFIX/drive_c/users/tester/Documents"
ln -s /home/tester/Templates "\$WINEPREFIX/drive_c/users/tester/AppData/Roaming/Microsoft/Windows/Templates"
EOF

cat <<EOF >"$fake_wine"
#!/bin/sh
set -eu
[ "\$1" = "$wine_test_command" ] || exit 1
[ -f "\$WINEPREFIX/drive_c/$runtime_name/test_win32.exe" ] || exit 1
[ -f "\$WINEPREFIX/drive_c/$runtime_name/libbounce.dll" ] || exit 1
[ -f "\$WINEPREFIX/drive_c/$runtime_name/libbounce_win32_example.exe" ] || exit 1
printf '%s\n' "\$1" >>"$wine_log"
EOF

chmod +x "$fake_wineboot" "$fake_wine"

WINE="$fake_wine" \
WINEBOOT="$fake_wineboot" \
WINEPREFIX="$wine_prefix" \
WINEARCH=win64 \
WINEDEBUG=-all \
sh "$repo_root/scripts/run_wine_test.sh" "$wine_prefix" "$wine_prefix_archive" "$runtime_name" "$test_binary" "$shared_library" "$wine_test_command" "$extra_runtime_file"

assert_missing "$wine_prefix"
assert_file "$wine_prefix_archive"
assert_file "$wineboot_log"
assert_file "$wine_log"
tar -tf "$wine_prefix_archive" >"$tar_listing"
assert_contains "$tar_listing" 'wineprefix/dosdevices/c:'
assert_contains "$tar_listing" 'wineprefix/drive_c/libbounce-runtime/test_win32.exe'
assert_contains "$tar_listing" 'wineprefix/drive_c/libbounce-runtime/libbounce.dll'
assert_contains "$tar_listing" 'wineprefix/drive_c/libbounce-runtime/libbounce_win32_example.exe'

WINE="$fake_wine" \
WINEBOOT="$fake_wineboot" \
WINEPREFIX="$wine_prefix" \
WINEARCH=win64 \
WINEDEBUG=-all \
sh "$repo_root/scripts/run_wine_test.sh" "$wine_prefix" "$wine_prefix_archive" "$runtime_name" "$test_binary" "$shared_library" "$wine_test_command" "$extra_runtime_file"

assert_missing "$wine_prefix"
assert_file "$wine_prefix_archive"
[ "$(wc -l <"$wineboot_log" | tr -d ' ')" = '1' ] || fail 'Wine prefix should be restored from archive on second run'
[ "$(wc -l <"$wine_log" | tr -d ' ')" = '2' ] || fail 'Wine command should run twice'
