#!/bin/sh

set -eu

require_env() {
	var_name=$1
	eval "var_value=\${$var_name:-}"
	[ -n "$var_value" ] || {
		printf '%s\n' "Missing required environment variable: $var_name" >&2
		exit 1
	}
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || {
		printf '%s\n' "Missing required command: $1" >&2
		exit 1
	}
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			printf '%s\n' "$value_name must be a positive integer: $value" >&2
			exit 1
			;;
	esac

	[ "$value" -gt 0 ] || {
		printf '%s\n' "$value_name must be a positive integer: $value" >&2
		exit 1
	}
}

stage_package() {
	package_name=$1
	package_description=$2
	stage_dir=$3
	shared_lib_name=$4
	static_lib_name=$5
	build_dir=$6
	include_headers=$7
	depends_value=$8

	lib_dir="$stage_dir/usr/lib/$multiarch"
	doc_dir="$stage_dir/usr/share/doc/$package_name"
	control_dir="$stage_dir/DEBIAN"
	control_path="$control_dir/control"

	rm -rf "$stage_dir"
	mkdir -p "$lib_dir" "$doc_dir" "$control_dir"

	cp "$build_dir/$shared_lib_name" "$lib_dir/"
	cp "$build_dir/$static_lib_name" "$lib_dir/"
	cp LICENSE "$doc_dir/"
	cp README.md "$doc_dir/"
	cp README_ja.md "$doc_dir/"

	if [ "$include_headers" = 'true' ]; then
		include_dir="$stage_dir/usr/include/libbounce"
		mkdir -p "$include_dir"
		cp include/libbounce/*.h "$include_dir/"
	fi

	{
		printf 'Package: %s\n' "$package_name"
		printf 'Version: %s\n' "$LIBBOUNCE_PACKAGE_VERSION"
		printf 'Section: libs\n'
		printf 'Priority: optional\n'
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Maintainer: %s\n' "$LIBBOUNCE_PACKAGE_MAINTAINER"
		if [ -n "$depends_value" ]; then
			printf 'Depends: %s\n' "$depends_value"
		fi
		printf 'Description: %s\n' "$package_description"
	} >"$control_path"
}

require_env LIBBOUNCE_WORK_DIR
require_env LIBBOUNCE_META_DIR
require_env LIBBOUNCE_HOST_UID
require_env LIBBOUNCE_HOST_GID
require_env LIBBOUNCE_PACKAGE_VERSION
require_env LIBBOUNCE_BASE_PACKAGE_NAME
require_env LIBBOUNCE_GLIB_PACKAGE_NAME
require_env LIBBOUNCE_BASE_PACKAGE_DESCRIPTION
require_env LIBBOUNCE_GLIB_PACKAGE_DESCRIPTION
require_env LIBBOUNCE_PACKAGE_MAINTAINER

require_command apt-get

LIBBOUNCE_MAKE_JOBS=${LIBBOUNCE_MAKE_JOBS:-1}
validate_positive_integer 'LIBBOUNCE_MAKE_JOBS' "$LIBBOUNCE_MAKE_JOBS"

work_dir=$LIBBOUNCE_WORK_DIR
meta_dir=$LIBBOUNCE_META_DIR
posix_build_dir="$work_dir/build/posix"
glib_build_dir="$work_dir/build/posix_glib"
base_stage_dir="$work_dir/stage/$LIBBOUNCE_BASE_PACKAGE_NAME"
glib_stage_dir="$work_dir/stage/$LIBBOUNCE_GLIB_PACKAGE_NAME"

rm -rf "$work_dir" "$meta_dir"
mkdir -p "$posix_build_dir" "$glib_build_dir" "$meta_dir"

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
	dpkg-dev \
	libglib2.0-dev \
	liburing-dev \
	pkg-config

require_command make
require_command dpkg-architecture

make -j "$LIBBOUNCE_MAKE_JOBS" -f Makefile.posix all BUILD_DIR="$posix_build_dir"
make -j "$LIBBOUNCE_MAKE_JOBS" -f Makefile.posix_glib all BUILD_DIR="$glib_build_dir"

deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

stage_package \
	"$LIBBOUNCE_BASE_PACKAGE_NAME" \
	"$LIBBOUNCE_BASE_PACKAGE_DESCRIPTION" \
	"$base_stage_dir" \
	"libbounce.so" \
	"libbounce.a" \
	"$posix_build_dir" \
	'true' \
	''

stage_package \
	"$LIBBOUNCE_GLIB_PACKAGE_NAME" \
	"$LIBBOUNCE_GLIB_PACKAGE_DESCRIPTION" \
	"$glib_stage_dir" \
	"libbounce-glib.so" \
	"libbounce-glib.a" \
	"$glib_build_dir" \
	'false' \
	"$LIBBOUNCE_BASE_PACKAGE_NAME (= $LIBBOUNCE_PACKAGE_VERSION)"

printf '%s\n' "$deb_arch" >"$meta_dir/deb_arch"
printf '%s\n' "$multiarch" >"$meta_dir/multiarch"

chown -R "$LIBBOUNCE_HOST_UID:$LIBBOUNCE_HOST_GID" "$work_dir" "$meta_dir"
