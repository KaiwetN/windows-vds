#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'USAGE'
Usage: packaging/ArchLinux/build-arch.sh [options]

Options:
  --pkgrel REL          Override the Arch package release. Defaults to 1.
  --output-dir DIR      Directory for the generated package.
  --build-dir DIR       Temporary build/staging directory.
  --jobs N              Parallel build jobs. Defaults to nproc.
  -h, --help            Show this help text.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/../.." && pwd)
package_name="vds"
build_root="${repo_root}/build/packaging/ArchLinux"
output_dir="${repo_root}/dist/ArchLinux"
pkgrel="1"
jobs=$(nproc)

while [ "$#" -gt 0 ]; do
	case "$1" in
	--pkgrel)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --pkgrel" >&2
			exit 2
		fi
		pkgrel=$2
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

substitute_template() {
	local input=$1
	local output=$2

	sed \
		-e "s|@PKGVER@|${pkgver}|g" \
		-e "s|@PKGREL@|${pkgrel}|g" \
		-e "s|@DKMS_VERSION@|${dkms_version}|g" \
		-e "s|@ARCH@|${arch}|g" \
		-e "s|@SIZE@|${installed_size}|g" \
		-e "s|@BUILDDATE@|${build_date}|g" \
		-e "s|@BUILDROOT@|${build_root}|g" \
		-e "s|@REPOROOT@|${repo_root}|g" \
		"$input" >"$output"
}

install_dkms_sources() {
	local dkms_source_dir=$1
	local source

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
}

require_tool bsdtar
require_tool cmake
require_tool gzip
require_tool install
require_tool make
require_tool mktemp
require_tool nproc
require_tool sed
require_tool zstd

arch=$(uname -m)
version_args=(--internal arch)
package_args=(--package arch --name "$package_name" --pkgrel "$pkgrel" --arch "$arch")
pkgver=$("${repo_root}/generate-version.sh" "${version_args[@]}")
dkms_version=$pkgver
build_date=$(date +%s)
build_dir="${build_root}/cmake"
stage_dir="${build_root}/stage"
package_root="${build_root}/${package_name}-${pkgver}-${pkgrel}-${arch}"
dkms_source_dir="${package_root}/usr/src/vds_hcd-${dkms_version}"
output_dir=$(realpath -m "$output_dir")
package_path="${output_dir}/$("${repo_root}/generate-version.sh" "${package_args[@]}")"

rm -rf "$build_dir" "$stage_dir" "$package_root"
mkdir -p "$build_dir" "$stage_dir" "$package_root" "$output_dir"

cmake -S "$repo_root" -B "$build_dir" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DINSTALL_SERVICE=OFF
cmake --build "$build_dir" --parallel "$jobs"
DESTDIR="$stage_dir" cmake --install "$build_dir"

cp -a "$stage_dir/." "$package_root/"

install -Dm0644 "$build_dir/vdsd.service" \
	"${package_root}/usr/lib/systemd/system/vdsd.service"
install -Dm0644 "${repo_root}/99-vds-dualsense-udev.rules" \
	"${package_root}/usr/lib/udev/rules.d/99-vds-dualsense-udev.rules"
install -Dm0755 "${repo_root}/override-bluetoothd.sh" \
	"${package_root}/usr/share/vds/override-bluetoothd.sh"
install -Dm0755 "${repo_root}/install-service.sh" \
	"${package_root}/usr/share/vds/install-service.sh"
install -Dm0755 "${repo_root}/uninstall-service.sh" \
	"${package_root}/usr/share/vds/uninstall-service.sh"
install -Dm0644 "${repo_root}/logrotate-vds.conf" \
	"${package_root}/etc/logrotate.d/vds"

install_dkms_sources "$dkms_source_dir"

install -Dm0644 "${repo_root}/README.md" \
	"${package_root}/usr/share/doc/vds/README.md"
install -Dm0644 "${repo_root}/README-LINUX.md" \
	"${package_root}/usr/share/doc/vds/README-LINUX.md"
install -Dm0644 "${repo_root}/LICENSE" \
	"${package_root}/usr/share/licenses/vds/LICENSE"
install -Dm0644 "${repo_root}/99-vds-dualsense-wireplumber.conf" \
	"${package_root}/usr/share/doc/vds/examples/99-vds-dualsense-wireplumber.conf"

installed_size=$(du -sb "$package_root" | awk '{print $1}')
substitute_template "${script_dir}/PKGINFO.in" "${package_root}/.PKGINFO"
substitute_template "${script_dir}/BUILDINFO.in" "${package_root}/.BUILDINFO"
substitute_template "${script_dir}/vds.install.in" "${package_root}/.INSTALL"

(
	cd "$package_root"
	mtree_tmp=$(mktemp)
	trap 'rm -f "$mtree_tmp"' EXIT
	bsdtar --format=mtree \
		--options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
		-cf - \
		--exclude .BUILDINFO \
		--exclude .INSTALL \
		--exclude .MTREE \
		--exclude .PKGINFO \
		. | gzip -c >"$mtree_tmp"
	mv "$mtree_tmp" .MTREE
	trap - EXIT
		bsdtar -cf - .PKGINFO .BUILDINFO .INSTALL .MTREE etc usr |
			zstd -T0 -q -c >"$package_path"
	)

echo "$package_path"
