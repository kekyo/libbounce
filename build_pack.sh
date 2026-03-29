#!/bin/sh

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARTIFACT_ROOT="$PROJECT_ROOT/artifacts"
BASE_PACKAGE_NAME=libbounce
GLIB_PACKAGE_NAME=libbounce-glib
PACKAGE_REPOSITORY_NAME=libbound-packages
BASE_PACKAGE_DESCRIPTION="A small thread dispatch library for asynchronous I/O and timer completion dispatch."
GLIB_PACKAGE_DESCRIPTION="GLib POSIX backend package for libbounce."
DEFAULT_MAINTAINER="libbounce packager <packager@localhost>"
PROJECT_HOMEPAGE="https://github.com/kekyo/libbounce"
DEFAULT_PARALLEL_JOB_CAP=14

LINUX_MATRIX=$(cat <<'EOF'
debian bookworm x86_64 linux/amd64
debian bookworm i686 linux/386
debian bookworm arm64 linux/arm64
debian bookworm armv7l linux/arm/v7
debian trixie x86_64 linux/amd64
debian trixie i686 linux/386
debian trixie arm64 linux/arm64
debian trixie armv7l linux/arm/v7
debian trixie riscv64 linux/riscv64
ubuntu 22.04 x86_64 linux/amd64
ubuntu 22.04 arm64 linux/arm64
ubuntu 24.04 x86_64 linux/amd64
ubuntu 24.04 arm64 linux/arm64
EOF
)

WIN32_MATRIX=$(cat <<'EOF'
x86 i686-w64-mingw32-gcc-win32
x64 x86_64-w64-mingw32-gcc-win32
EOF
)

print_usage() {
	cat <<'EOF'
Usage: ./build_pack.sh [options]

Options:
  --version <version>  Package version. Defaults to a screw-up-derived version.
  --target <target>    all, deb, win32, or packages. Defaults to all.
  --distro <list>      Comma-separated distro filter for deb builds.
  --release <list>     Comma-separated release filter for deb builds.
  --arch <list>        Comma-separated architecture filter.
  --jobs <count>       Maximum concurrent package jobs. Defaults to auto (up to 8).
  --print-version      Print the resolved package version and exit.
  --help               Show this help.
EOF
}

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			fail "$value_name must be a positive integer: $value"
			;;
	esac

	[ "$value" -gt 0 ] || fail "$value_name must be a positive integer: $value"
}

detect_processor_count() {
	detected_count=''

	if command -v getconf >/dev/null 2>&1; then
		detected_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v nproc >/dev/null 2>&1; then
		detected_count=$(nproc 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v sysctl >/dev/null 2>&1; then
		detected_count=$(sysctl -n hw.ncpu 2>/dev/null || true)
	fi

	case $detected_count in
		'' | *[!0-9]*)
			detected_count=1
			;;
	esac

	if [ "$detected_count" -lt 1 ]; then
		detected_count=1
	fi

	printf '%s\n' "$detected_count"
}

min_int() {
	left_value=$1
	right_value=$2

	if [ "$left_value" -le "$right_value" ]; then
		printf '%s\n' "$left_value"
	else
		printf '%s\n' "$right_value"
	fi
}

detect_version() {
	require_command screw-up
	detected_version=$(printf '%s\n' '{version}' | screw-up format | tr -d '\r')
	[ -n "$detected_version" ] || fail 'screw-up did not return a version'
	printf '%s\n' "$detected_version"
}

validate_version() {
	case $1 in
		'' | *[!0-9A-Za-z.+:~\-]*)
			fail "Invalid package version: $1"
			;;
	esac
}

matches_filter() {
	filter_value=$1
	actual_value=$2
	if [ -z "$filter_value" ]; then
		return 0
	fi

	previous_ifs=$IFS
	IFS=','
	for allowed_value in $filter_value; do
		if [ "$allowed_value" = "$actual_value" ]; then
			IFS=$previous_ifs
			return 0
		fi
	done
	IFS=$previous_ifs
	return 1
}

count_deb_builds() {
	build_count=0

	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		build_count=$((build_count + 1))
	done <<EOF
$LINUX_MATRIX
EOF

	printf '%s\n' "$build_count"
}

count_win32_builds() {
	build_count=0

	while IFS=' ' read -r arch compiler; do
		[ -n "$arch" ] || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		build_count=$((build_count + 1))
	done <<EOF
$WIN32_MATRIX
EOF

	printf '%s\n' "$build_count"
}

choose_container_engine() {
	if [ -n "${CONTAINER_ENGINE:-}" ]; then
		require_command "$CONTAINER_ENGINE"
		printf '%s\n' "$CONTAINER_ENGINE"
		return 0
	fi
	if command -v podman >/dev/null 2>&1; then
		printf '%s\n' 'podman'
		return 0
	fi
	if command -v docker >/dev/null 2>&1; then
		printf '%s\n' 'docker'
		return 0
	fi
	fail 'Missing required container engine: podman or docker'
}

container_image_for_target() {
	distro=$1
	release=$2
	arch=$3

	if [ "$arch" = 'riscv64' ]; then
		printf 'docker.io/library/%s:%s\n' "$distro" "$release"
		return 0
	fi

	case $arch in
		x86_64)
			repository_prefix='amd64'
			;;
		i686)
			repository_prefix='i386'
			;;
		arm64)
			repository_prefix='arm64v8'
			;;
		armv7l)
			repository_prefix='arm32v7'
			;;
		*)
			fail "Unsupported Debian package architecture: $arch"
			;;
	esac

	printf 'docker.io/%s/%s:%s\n' "$repository_prefix" "$distro" "$release"
}

build_deb_packages() {
	distro=$1
	release=$2
	arch=$3
	platform=$4
	image=$(container_image_for_target "$distro" "$release" "$arch")
	work_root="$TMP_ROOT/deb/$distro/$release/$arch"
	container_root="/workspace/artifacts/.tmp/$RUN_ID/deb/$distro/$release/$arch"
	work_dir="$container_root/work"
	meta_dir="$container_root/meta"
	package_dir="$ARTIFACT_ROOT/deb"

	printf '%s\n' "[deb] $distro $release $arch"
	mkdir -p "$package_dir"

	"$CONTAINER_ENGINE_BIN" run --rm \
		--platform "$platform" \
		-v "$PROJECT_ROOT:/workspace" \
		-w /workspace \
		-e LIBBOUNCE_WORK_DIR="$work_dir" \
		-e LIBBOUNCE_META_DIR="$meta_dir" \
		-e LIBBOUNCE_HOST_UID="$(id -u)" \
		-e LIBBOUNCE_HOST_GID="$(id -g)" \
		-e LIBBOUNCE_PACKAGE_VERSION="$VERSION" \
		-e LIBBOUNCE_BASE_PACKAGE_NAME="$BASE_PACKAGE_NAME" \
		-e LIBBOUNCE_GLIB_PACKAGE_NAME="$GLIB_PACKAGE_NAME" \
		-e LIBBOUNCE_BASE_PACKAGE_DESCRIPTION="$BASE_PACKAGE_DESCRIPTION" \
		-e LIBBOUNCE_GLIB_PACKAGE_DESCRIPTION="$GLIB_PACKAGE_DESCRIPTION" \
		-e LIBBOUNCE_PACKAGE_MAINTAINER="${DEB_MAINTAINER:-$DEFAULT_MAINTAINER}" \
		-e LIBBOUNCE_MAKE_JOBS="$MAKE_JOBS" \
		"$image" \
		./scripts/build_linux_dist_container.sh

	deb_arch=$(cat "$work_root/meta/deb_arch")
	dpkg-deb --root-owner-group --build \
		"$work_root/work/stage/$BASE_PACKAGE_NAME" \
		"$package_dir/${BASE_PACKAGE_NAME}-${VERSION}-${distro}-${release}-${deb_arch}.deb" >/dev/null
	dpkg-deb --root-owner-group --build \
		"$work_root/work/stage/$GLIB_PACKAGE_NAME" \
		"$package_dir/${GLIB_PACKAGE_NAME}-${VERSION}-${distro}-${release}-${deb_arch}.deb" >/dev/null
}

build_win32_package() {
	label=$1
	compiler=$2
	work_root="$TMP_ROOT/win32/$label"
	build_dir="$work_root/build"
	stage_root="$work_root/stage"
	top_dir_name="${BASE_PACKAGE_NAME}-${VERSION}-win32-$label"
	package_root="$stage_root/$top_dir_name"
	package_dir="$ARTIFACT_ROOT/win32"
	package_path="$package_dir/${top_dir_name}.zip"

	printf '%s\n' "[win32] $label"
	rm -rf "$work_root"
	mkdir -p "$package_root/bin" "$package_root/lib" "$package_root/include/libbounce" "$package_dir"

	make -j "$MAKE_JOBS" -f Makefile.win32 all BUILD_DIR="$build_dir" CC="$compiler"

	cp "$build_dir/libbounce.dll" "$package_root/bin/"
	cp "$build_dir/libbounce.dll.a" "$package_root/lib/"
	cp "$build_dir/libbounce.a" "$package_root/lib/"
	cp include/libbounce/*.h "$package_root/include/libbounce/"
	cp LICENSE "$package_root/"
	cp README.md "$package_root/"
	cp README_ja.md "$package_root/"

	rm -f "$package_path"
	(
		cd "$stage_root"
		zip -qr "$package_path" "$top_dir_name"
	)
}

build_package_repository() {
	package_root="$ARTIFACT_ROOT/$PACKAGE_REPOSITORY_NAME"
	src_root="$package_root/src"

	printf '%s\n' "[packages] $PACKAGE_REPOSITORY_NAME"
	rm -rf "$package_root"
	mkdir -p "$src_root/libbounce"

	cp "$PROJECT_ROOT"/include/libbounce/*.h "$src_root/libbounce/"
	cp -R "$PROJECT_ROOT/src/." "$src_root/"
	cp "$PROJECT_ROOT/LICENSE" "$package_root/"
	cp "$PROJECT_ROOT/README.md" "$package_root/"
	cp "$PROJECT_ROOT/README_ja.md" "$package_root/"

	cat >"$package_root/library.properties" <<EOF
name=libbounce
version=$VERSION
author=Kouji Matsui
maintainer=Kouji Matsui <packager@localhost>
sentence=A small thread dispatch library for asynchronous I/O and timer completion dispatch.
paragraph=Provides C and C++ helper APIs with backend adapters for FreeRTOS, ESP-IDF, POSIX, POSIX+GLib, and Win32.
category=Other
url=$PROJECT_HOMEPAGE
architectures=esp32
includes=libbounce/bounce.h,libbounce/timer.h,libbounce/freertos.h,libbounce/promise.h
EOF

	cat >"$package_root/library.json" <<EOF
{
  "name": "libbounce",
  "version": "$VERSION",
  "description": "$BASE_PACKAGE_DESCRIPTION",
  "keywords": [
    "async",
    "dispatch",
    "freertos",
    "esp-idf",
    "arduino",
    "coroutine"
  ],
  "homepage": "$PROJECT_HOMEPAGE",
  "repository": {
    "type": "git",
    "url": "$PROJECT_HOMEPAGE.git"
  },
  "authors": [
    {
      "name": "Kouji Matsui",
      "maintainer": true
    }
  ],
  "license": "MIT",
  "frameworks": [
    "arduino",
    "espidf"
  ],
  "platforms": [
    "espressif32"
  ],
  "headers": [
    "libbounce/bounce.h",
    "libbounce/timer.h",
    "libbounce/freertos.h",
    "libbounce/promise.h",
    "libbounce/utils.h"
  ],
  "build": {
    "includeDir": "src",
    "srcDir": "src"
  }
}
EOF

	cat >"$package_root/idf_component.yml" <<EOF
version: "$VERSION"
description: "$BASE_PACKAGE_DESCRIPTION"
url: "$PROJECT_HOMEPAGE"
license: "MIT"
maintainers:
  - "Kouji Matsui <packager@localhost>"
dependencies:
  idf: ">=5.0"
EOF

	cat >"$package_root/CMakeLists.txt" <<'EOF'
idf_component_register(
  SRCS
    "src/utils.c"
    "src/freertos/bounce_tls_freertos.c"
    "src/freertos/bounce_freertos.c"
    "src/freertos/bounce_freertos_timer.c"
    "src/freertos/bounce_freertos_fd_espidf.c"
  INCLUDE_DIRS
    "src"
  REQUIRES
    freertos
  PRIV_REQUIRES
    lwip
    vfs
)
EOF
}

wait_for_oldest_job() {
	[ "$ACTIVE_JOB_COUNT" -gt 0 ] || return 0

	set -- $ACTIVE_JOB_PIDS
	wait_pid=$1
	shift

	if wait "$wait_pid"; then
		:
	else
		JOB_FAILURE=1
	fi

	ACTIVE_JOB_PIDS=$*
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT - 1))
}

run_parallel_job() {
	while [ "$ACTIVE_JOB_COUNT" -ge "$PARALLEL_JOBS" ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'

	"$@" &
	ACTIVE_JOB_PIDS="${ACTIVE_JOB_PIDS}${ACTIVE_JOB_PIDS:+ }$!"
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT + 1))
}

wait_for_all_jobs() {
	while [ "$ACTIVE_JOB_COUNT" -gt 0 ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'
}

schedule_deb_builds() {
	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_deb_packages "$distro" "$release" "$arch" "$platform"
	done <<EOF
$LINUX_MATRIX
EOF
}

schedule_win32_builds() {
	while IFS=' ' read -r arch compiler; do
		[ -n "$arch" ] || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_win32_package "$arch" "$compiler"
	done <<EOF
$WIN32_MATRIX
EOF
}

VERSION=''
TARGET='all'
DISTRO_FILTER=''
RELEASE_FILTER=''
ARCH_FILTER=''
PARALLEL_JOBS=''
PRINT_VERSION='false'

while [ "$#" -gt 0 ]; do
	case $1 in
		--version)
			[ "$#" -ge 2 ] || fail 'Missing value for --version'
			VERSION=$2
			shift 2
			;;
		--target)
			[ "$#" -ge 2 ] || fail 'Missing value for --target'
			TARGET=$2
			shift 2
			;;
		--distro)
			[ "$#" -ge 2 ] || fail 'Missing value for --distro'
			DISTRO_FILTER=$2
			shift 2
			;;
		--release)
			[ "$#" -ge 2 ] || fail 'Missing value for --release'
			RELEASE_FILTER=$2
			shift 2
			;;
		--arch)
			[ "$#" -ge 2 ] || fail 'Missing value for --arch'
			ARCH_FILTER=$2
			shift 2
			;;
		--jobs)
			[ "$#" -ge 2 ] || fail 'Missing value for --jobs'
			PARALLEL_JOBS=$2
			shift 2
			;;
		--help)
			print_usage
			exit 0
			;;
		--print-version)
			PRINT_VERSION='true'
			shift
			;;
		*)
			fail "Unknown argument: $1"
			;;
	esac
done

if [ -z "$VERSION" ]; then
	VERSION=$(detect_version)
fi
validate_version "$VERSION"
if [ -n "$PARALLEL_JOBS" ]; then
	validate_positive_integer 'Parallel job count' "$PARALLEL_JOBS"
fi

if [ "$PRINT_VERSION" = 'true' ]; then
	printf '%s\n' "$VERSION"
	exit 0
fi

case $TARGET in
	all | deb | win32 | packages)
		;;
	*)
		fail "Unsupported target: $TARGET"
		;;
esac

require_command make

CPU_COUNT=$(detect_processor_count)
if [ -z "$PARALLEL_JOBS" ]; then
	PARALLEL_JOBS=$(min_int "$CPU_COUNT" "$DEFAULT_PARALLEL_JOB_CAP")
fi

BUILD_TASK_COUNT=0
if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
	BUILD_TASK_COUNT=$((BUILD_TASK_COUNT + $(count_deb_builds)))
fi
if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'win32' ]; then
	BUILD_TASK_COUNT=$((BUILD_TASK_COUNT + $(count_win32_builds)))
fi

if [ "$BUILD_TASK_COUNT" -gt 0 ]; then
	EFFECTIVE_BUILD_JOBS=$(min_int "$PARALLEL_JOBS" "$BUILD_TASK_COUNT")
else
	EFFECTIVE_BUILD_JOBS=1
fi

MAKE_JOBS=$((CPU_COUNT / EFFECTIVE_BUILD_JOBS))
if [ "$MAKE_JOBS" -lt 1 ]; then
	MAKE_JOBS=1
fi

RUN_ID="run-$(date +%Y%m%d%H%M%S)-$$"
TMP_ROOT="$ARTIFACT_ROOT/.tmp/$RUN_ID"
ACTIVE_JOB_PIDS=''
ACTIVE_JOB_COUNT=0
JOB_FAILURE=0

mkdir -p "$ARTIFACT_ROOT"
rm -rf "$ARTIFACT_ROOT/deb" "$ARTIFACT_ROOT/win32" "$ARTIFACT_ROOT/$PACKAGE_REPOSITORY_NAME"
mkdir -p "$TMP_ROOT"

printf '%s\n' "Using up to $PARALLEL_JOBS package jobs with make -j$MAKE_JOBS"

if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
	require_command dpkg-deb
	CONTAINER_ENGINE_BIN=$(choose_container_engine)
	schedule_deb_builds
fi

if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'win32' ]; then
	require_command zip
	schedule_win32_builds
fi

if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'packages' ]; then
	run_parallel_job build_package_repository
fi

wait_for_all_jobs

printf '%s\n' "Artifacts generated in $ARTIFACT_ROOT"
