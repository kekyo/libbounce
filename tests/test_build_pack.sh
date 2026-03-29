#!/bin/sh

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARTIFACT_ROOT="$PROJECT_ROOT/artifacts"
BASE_PACKAGE_NAME=libbounce
GLIB_PACKAGE_NAME=libbounce-glib
PACKAGE_REPOSITORY_NAME=libbound-packages

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

assert_contains() {
	target_path=$1
	expected_text=$2
	grep -F "$expected_text" "$target_path" >/dev/null 2>&1 || fail "Missing expected text in $target_path: $expected_text"
}

expected_elf_class() {
	case $1 in
		x86_64 | arm64 | riscv64)
			printf '%s\n' 'ELF64'
			;;
		i686 | armv7l)
			printf '%s\n' 'ELF32'
			;;
		*)
			fail "Unsupported ELF class lookup: $1"
			;;
	esac
}

expected_elf_machine() {
	case $1 in
		x86_64)
			printf '%s\n' 'Advanced Micro Devices X86-64'
			;;
		i686)
			printf '%s\n' 'Intel 80386'
			;;
		arm64)
			printf '%s\n' 'AArch64'
			;;
		armv7l)
			printf '%s\n' 'ARM'
			;;
		riscv64)
			printf '%s\n' 'RISC-V'
			;;
		*)
			fail "Unsupported ELF machine lookup: $1"
			;;
	esac
}

deb_arch_name() {
	case $1 in
		x86_64)
			printf '%s\n' 'amd64'
			;;
		i686)
			printf '%s\n' 'i386'
			;;
		arm64)
			printf '%s\n' 'arm64'
			;;
		armv7l)
			printf '%s\n' 'armhf'
			;;
		riscv64)
			printf '%s\n' 'riscv64'
			;;
		*)
			fail "Unsupported Debian architecture lookup: $1"
			;;
	esac
}

deb_artifact_path() {
	package_name=$1
	distro=$2
	release=$3
	arch=$4
	deb_arch=$(deb_arch_name "$arch")

	printf '%s\n' "$ARTIFACT_ROOT/deb/${package_name}-${VERSION}-${distro}-${release}-${deb_arch}.deb"
}

validate_base_deb_package() {
	package_path=$1
	expected_arch=$2
	expected_deb_arch=$(deb_arch_name "$expected_arch")
	tmp_dir=$(mktemp -d)

	assert_file "$package_path"
	[ "$(dpkg-deb -f "$package_path" Architecture)" = "$expected_deb_arch" ] || fail "Unexpected Architecture field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Version)" = "$VERSION" ] || fail "Unexpected Version field in $package_path"

	dpkg-deb -x "$package_path" "$tmp_dir"

	for header_path in "$PROJECT_ROOT"/include/libbounce/*.h; do
		header_name=$(basename "$header_path")
		assert_file "$tmp_dir/usr/include/libbounce/$header_name"
	done
	assert_file "$tmp_dir/usr/share/doc/$BASE_PACKAGE_NAME/LICENSE"
	assert_file "$tmp_dir/usr/share/doc/$BASE_PACKAGE_NAME/README.md"
	assert_file "$tmp_dir/usr/share/doc/$BASE_PACKAGE_NAME/README_ja.md"

	lib_file=$(find "$tmp_dir/usr/lib" -type f -name 'libbounce.so' | head -n 1)
	static_lib=$(find "$tmp_dir/usr/lib" -type f -name 'libbounce.a' | head -n 1)
	[ -n "$lib_file" ] || fail "Missing libbounce.so in $package_path"
	[ -n "$static_lib" ] || fail "Missing libbounce.a in $package_path"

	readelf -h "$lib_file" >"$tmp_dir/readelf.txt"
	assert_contains "$tmp_dir/readelf.txt" "$(expected_elf_class "$expected_arch")"
	assert_contains "$tmp_dir/readelf.txt" "$(expected_elf_machine "$expected_arch")"

	rm -rf "$tmp_dir"
}

validate_glib_deb_package() {
	package_path=$1
	expected_arch=$2
	expected_deb_arch=$(deb_arch_name "$expected_arch")
	tmp_dir=$(mktemp -d)

	assert_file "$package_path"
	[ "$(dpkg-deb -f "$package_path" Architecture)" = "$expected_deb_arch" ] || fail "Unexpected Architecture field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Version)" = "$VERSION" ] || fail "Unexpected Version field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Depends)" = "$BASE_PACKAGE_NAME (= $VERSION)" ] || fail "Unexpected Depends field in $package_path"

	dpkg-deb -x "$package_path" "$tmp_dir"

	[ ! -e "$tmp_dir/usr/include/libbounce" ] || fail "Unexpected headers in $package_path"
	assert_file "$tmp_dir/usr/share/doc/$GLIB_PACKAGE_NAME/LICENSE"
	assert_file "$tmp_dir/usr/share/doc/$GLIB_PACKAGE_NAME/README.md"
	assert_file "$tmp_dir/usr/share/doc/$GLIB_PACKAGE_NAME/README_ja.md"

	lib_file=$(find "$tmp_dir/usr/lib" -type f -name 'libbounce-glib.so' | head -n 1)
	static_lib=$(find "$tmp_dir/usr/lib" -type f -name 'libbounce-glib.a' | head -n 1)
	[ -n "$lib_file" ] || fail "Missing libbounce-glib.so in $package_path"
	[ -n "$static_lib" ] || fail "Missing libbounce-glib.a in $package_path"

	readelf -h "$lib_file" >"$tmp_dir/readelf.txt"
	assert_contains "$tmp_dir/readelf.txt" "$(expected_elf_class "$expected_arch")"
	assert_contains "$tmp_dir/readelf.txt" "$(expected_elf_machine "$expected_arch")"

	rm -rf "$tmp_dir"
}

validate_win32_package() {
	package_path=$1
	label=$2
	tmp_dir=$(mktemp -d)
	top_dir="$BASE_PACKAGE_NAME-$VERSION-win32-$label"

	assert_file "$package_path"
	unzip -q "$package_path" -d "$tmp_dir"

	for header_path in "$PROJECT_ROOT"/include/libbounce/*.h; do
		header_name=$(basename "$header_path")
		assert_file "$tmp_dir/$top_dir/include/libbounce/$header_name"
	done
	assert_file "$tmp_dir/$top_dir/LICENSE"
	assert_file "$tmp_dir/$top_dir/README.md"
	assert_file "$tmp_dir/$top_dir/README_ja.md"
	assert_file "$tmp_dir/$top_dir/bin/libbounce.dll"
	assert_file "$tmp_dir/$top_dir/lib/libbounce.dll.a"
	assert_file "$tmp_dir/$top_dir/lib/libbounce.a"

	file "$tmp_dir/$top_dir/bin/libbounce.dll" >"$tmp_dir/file.txt"
	case $label in
		x86)
			assert_contains "$tmp_dir/file.txt" 'PE32 executable (DLL)'
			assert_contains "$tmp_dir/file.txt" 'Intel 80386'
			;;
		x64)
			assert_contains "$tmp_dir/file.txt" 'PE32+ executable (DLL)'
			assert_contains "$tmp_dir/file.txt" 'x86-64'
			;;
		*)
			fail "Unsupported Win32 label: $label"
			;;
	esac

	rm -rf "$tmp_dir"
}

validate_package_repository() {
	package_root="$ARTIFACT_ROOT/$PACKAGE_REPOSITORY_NAME"

	assert_file "$package_root/library.properties"
	assert_file "$package_root/library.json"
	assert_file "$package_root/idf_component.yml"
	assert_file "$package_root/CMakeLists.txt"
	assert_file "$package_root/LICENSE"
	assert_file "$package_root/README.md"
	assert_file "$package_root/README_ja.md"

	for header_path in "$PROJECT_ROOT"/include/libbounce/*.h; do
		header_name=$(basename "$header_path")
		assert_file "$package_root/src/libbounce/$header_name"
	done

	find "$PROJECT_ROOT/src" -type f \( -name '*.c' -o -name '*.h' \) | while IFS= read -r source_path; do
		relative_path=${source_path#"$PROJECT_ROOT/"}
		assert_file "$package_root/$relative_path"
	done

	assert_contains "$package_root/library.properties" "version=$VERSION"
	assert_contains "$package_root/library.properties" 'architectures=esp32'
	assert_contains "$package_root/library.json" "\"version\": \"$VERSION\""
	assert_contains "$package_root/library.json" '"frameworks": ['
	assert_contains "$package_root/library.json" '"arduino"'
	assert_contains "$package_root/library.json" '"espidf"'
	assert_contains "$package_root/idf_component.yml" "version: \"$VERSION\""
	assert_contains "$package_root/CMakeLists.txt" 'idf_component_register('
	assert_contains "$package_root/CMakeLists.txt" '"src/freertos/bounce_freertos_fd_espidf.c"'
}

cd "$PROJECT_ROOT"
help_output=$(./build_pack.sh --help)
printf '%s\n' "$help_output" | grep -F -- '--jobs <count>' >/dev/null 2>&1 || fail 'Missing --jobs option in help output'

expected_default_version=$(printf '%s\n' '{version}' | screw-up format | tr -d '\r')
actual_default_version=$(./build_pack.sh --print-version)
[ "$actual_default_version" = "$expected_default_version" ] || fail "Unexpected default version: $actual_default_version"
parallel_version=$(./build_pack.sh --jobs 2 --print-version)
[ "$parallel_version" = "$expected_default_version" ] || fail "Unexpected version with --jobs: $parallel_version"

if ./build_pack.sh --jobs 0 --print-version >/dev/null 2>&1; then
	fail '--jobs 0 unexpectedly succeeded'
fi

VERSION=${LIBBOUNCE_PACK_TEST_VERSION:-$expected_default_version}
PACK_BUILD_JOBS=${LIBBOUNCE_PACK_TEST_JOBS:-14}

if [ -n "${LIBBOUNCE_PACK_TEST_VERSION:-}" ]; then
	./build_pack.sh --version "$VERSION" --jobs "$PACK_BUILD_JOBS"
else
	./build_pack.sh --jobs "$PACK_BUILD_JOBS"
fi

deb_count=$(find "$ARTIFACT_ROOT/deb" -type f -name '*.deb' | wc -l | tr -d ' ')
zip_count=$(find "$ARTIFACT_ROOT/win32" -type f -name '*.zip' | wc -l | tr -d ' ')
[ "$deb_count" = '26' ] || fail "Unexpected deb artifact count: $deb_count"
[ "$zip_count" = '2' ] || fail "Unexpected zip artifact count: $zip_count"

for distro in debian ubuntu; do
	case $distro in
		debian)
			releases='bookworm trixie'
			;;
		ubuntu)
			releases='22.04 24.04'
			;;
	esac
	for release in $releases; do
		case $distro:$release in
			debian:bookworm)
				architectures='x86_64 i686 arm64 armv7l'
				;;
			debian:trixie)
				architectures='x86_64 i686 arm64 armv7l riscv64'
				;;
			ubuntu:22.04 | ubuntu:24.04)
				architectures='x86_64 arm64'
				;;
		esac
		for arch in $architectures; do
			validate_base_deb_package "$(deb_artifact_path "$BASE_PACKAGE_NAME" "$distro" "$release" "$arch")" "$arch"
			validate_glib_deb_package "$(deb_artifact_path "$GLIB_PACKAGE_NAME" "$distro" "$release" "$arch")" "$arch"
		done
	done
done

validate_win32_package "$ARTIFACT_ROOT/win32/${BASE_PACKAGE_NAME}-$VERSION-win32-x86.zip" x86
validate_win32_package "$ARTIFACT_ROOT/win32/${BASE_PACKAGE_NAME}-$VERSION-win32-x64.zip" x64
validate_package_repository
