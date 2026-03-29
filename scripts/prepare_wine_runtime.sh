#!/bin/sh

set -eu

wine_prefix=$1
runtime_name=$2
test_binary=$3
shared_library=$4

runtime_dir="$wine_prefix/drive_c/$runtime_name"

remove_external_dosdevice_links() {
	dosdevices_dir=$1

	[ -d "$dosdevices_dir" ] || return 0

	find "$dosdevices_dir" -mindepth 1 -maxdepth 1 -type l ! -name 'c:' -exec rm -f {} +
}

replace_external_user_links() {
	users_dir=$1

	[ -d "$users_dir" ] || return 0

	find "$users_dir" -type l -exec sh -eu -c '
		for path do
			target=$(readlink "$path")
			case $target in
				/*)
					rm -f "$path"
					mkdir -p "$path"
					;;
			esac
		done
	' sh {} +
}

stage_runtime_file() {
	source_path=$1

	[ -f "$source_path" ] || return 0

	cp "$source_path" "$runtime_dir/"
}

remove_external_dosdevice_links "$wine_prefix/dosdevices"
replace_external_user_links "$wine_prefix/drive_c/users"

rm -rf "$runtime_dir"
mkdir -p "$runtime_dir"

stage_runtime_file "$test_binary"
stage_runtime_file "$shared_library"
