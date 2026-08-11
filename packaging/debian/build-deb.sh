#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'USAGE'
Usage: packaging/debian/build-deb.sh [options]

Options:
  --revision REV        Override the Debian package revision. Defaults to 1.
  --output-dir DIR      Directory for the generated .deb package.
  --build-dir DIR       Temporary build/staging directory.
  --jobs N              Parallel build jobs. Defaults to nproc.
  -h, --help            Show this help text.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/../.." && pwd)
package_name="vds"
build_root="${repo_root}/build/packaging/debian"
output_dir="${repo_root}/dist/debian"
debian_revision="1"
jobs=$(nproc)

while [ "$#" -gt 0 ]; do
	case "$1" in
	--revision)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --revision" >&2
			exit 2
		fi
		debian_revision=$2
		shift 2
		;;
	--output-dir)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --output-dir" >&2
			exit 2
		fi
		output_dir=$2
		shift 2
		;;
	--build-dir)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --build-dir" >&2
			exit 2
		fi
		build_root=$2
		shift 2
		;;
	--jobs)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --jobs" >&2
			exit 2
		fi
		jobs=$2
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

require_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "required tool not found: $1" >&2
		exit 1
	fi
}

sanitize_dkms_version() {
	printf '%s' "$1" | tr '~:' '__'
}

substitute_template() {
	local input=$1
	local output=$2
	sed \
		-e "s|@VERSION@|${version}|g" \
		-e "s|@DKMS_VERSION@|${dkms_version}|g" \
		-e "s|@ARCH@|${arch}|g" \
		-e "s|@INSTALLED_SIZE@|${installed_size}|g" \
		"$input" >"$output"
}

require_tool cmake
require_tool dpkg
require_tool dpkg-deb
require_tool make
require_tool nproc
require_tool sed

arch=$(dpkg --print-architecture)
version_args=(--internal debian)
package_args=(--package debian --name "$package_name" --pkgrel "$debian_revision" --arch "$arch")
upstream_version=$("${repo_root}/generate-version.sh" "${version_args[@]}")
version="${upstream_version}-${debian_revision}"
dkms_version=$(sanitize_dkms_version "$upstream_version")
build_dir="${build_root}/cmake"
stage_dir="${build_root}/stage"
package_root="${build_root}/${package_name}_${version}_${arch}"
debian_dir="${package_root}/DEBIAN"
dkms_source_dir="${package_root}/usr/src/vds_hcd-${dkms_version}"
output_dir=$(realpath -m "$output_dir")
deb_path="${output_dir}/$("${repo_root}/generate-version.sh" "${package_args[@]}")"

rm -rf "$build_dir" "$stage_dir" "$package_root"
mkdir -p "$build_dir" "$stage_dir" "$debian_dir" "$output_dir"

cmake -S "$repo_root" -B "$build_dir" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DINSTALL_SERVICE=OFF
cmake --build "$build_dir" --parallel "$jobs"
DESTDIR="$stage_dir" cmake --install "$build_dir"

cp -a "$stage_dir/." "$package_root/"

install -Dm0644 "$build_dir/vdsd.service" \
	"${package_root}/lib/systemd/system/vdsd.service"
install -Dm0644 "${repo_root}/99-vds-dualsense-udev.rules" \
	"${package_root}/lib/udev/rules.d/99-vds-dualsense-udev.rules"
install -Dm0755 "${repo_root}/override-bluetoothd.sh" \
	"${package_root}/usr/share/vds/override-bluetoothd.sh"
install -Dm0755 "${repo_root}/install-service.sh" \
	"${package_root}/usr/share/vds/install-service.sh"
install -Dm0755 "${repo_root}/uninstall-service.sh" \
	"${package_root}/usr/share/vds/uninstall-service.sh"
install -Dm0644 "${repo_root}/logrotate-vds.conf" \
	"${package_root}/etc/logrotate.d/vds"

install -d "$dkms_source_dir/include"
install -m0644 "${repo_root}/module/Makefile" \
	"${repo_root}/module/Kbuild" \
	"$dkms_source_dir/"
for source in "${repo_root}/module/"*.c; do
	case "$(basename "$source")" in
	*.mod.c)
		continue
		;;
	esac
	install -m0644 "$source" "$dkms_source_dir/"
done
for source in "${repo_root}/module/"*.h; do
	install -m0644 "$source" "$dkms_source_dir/"
done
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${dkms_version}\"/" \
	"${repo_root}/module/dkms.conf" >"${dkms_source_dir}/dkms.conf"
install -d "${dkms_source_dir}/include/uapi" "${dkms_source_dir}/include/vds"
install -m0644 "${repo_root}/include/uapi/"*.h \
	"${dkms_source_dir}/include/uapi/"
install -m0644 "${repo_root}/include/vds/"*.h \
	"${dkms_source_dir}/include/vds/"

install -Dm0644 "${repo_root}/README.md" \
	"${package_root}/usr/share/doc/vds/README.md"
install -Dm0644 "${repo_root}/README-LINUX.md" \
	"${package_root}/usr/share/doc/vds/README-LINUX.md"
install -Dm0644 "${repo_root}/LICENSE" \
	"${package_root}/usr/share/doc/vds/copyright"
install -Dm0644 "${repo_root}/99-vds-dualsense-wireplumber.conf" \
	"${package_root}/usr/share/doc/vds/examples/99-vds-dualsense-wireplumber.conf"

installed_size=$(du -sk "$package_root" | awk '{print $1}')
substitute_template "${script_dir}/control.in" "${debian_dir}/control"
substitute_template "${script_dir}/postinst" "${debian_dir}/postinst"
substitute_template "${script_dir}/prerm" "${debian_dir}/prerm"
substitute_template "${script_dir}/postrm" "${debian_dir}/postrm"
chmod 0755 "${debian_dir}/postinst" "${debian_dir}/prerm" "${debian_dir}/postrm"

dpkg-deb --build --root-owner-group "$package_root" "$deb_path"
echo "$deb_path"
