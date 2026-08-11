#!/usr/bin/env sh
set -eu

usage() {
	cat <<'USAGE'
Usage: ./generate-version.sh [--internal debian|arch]
       ./generate-version.sh --package debian|arch --arch ARCH [--name NAME] [--pkgrel REL]

Options:
  Default      Print the nearest X.X.X[-rcN] tag version.
  --internal   Print an internal package metadata version:
               debian => Debian Version field without Debian revision.
               arch   => Arch pkgver field without pkgrel.
  --package    Print a package filename for the selected package format.
  --name       Package name for --package. Defaults to this script's directory name.
  --arch       Package architecture for --package.
  --pkgrel     Package release for --package. Omitted from filenames if unset.
  -h, --help   Show this help text.
USAGE
}

script_dir=$(
	unset CDPATH
	cd -- "$(dirname -- "$0")" && pwd
)
mode=git
package_format=""
package_name=${script_dir##*/}
package_release=""
package_arch=""
version_tag_pattern='^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*(-rc[0-9][0-9]*)?$'

while [ "$#" -gt 0 ]; do
	case "$1" in
	--internal)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --internal" >&2
			exit 2
		fi
		case "$2" in
		debian | arch)
			mode=$2
			;;
		*)
			echo "unknown internal version format: $2" >&2
			usage >&2
			exit 2
			;;
		esac
		shift 2
		;;
	--package)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --package" >&2
			exit 2
		fi
		package_format=$2
		shift 2
		;;
	--arch)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --arch" >&2
			exit 2
		fi
		package_arch=$2
		shift 2
		;;
	--name)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --name" >&2
			exit 2
		fi
		package_name=$2
		shift 2
		;;
	--pkgrel)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --pkgrel" >&2
			exit 2
		fi
		package_release=$2
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

git_version() {
	if ! command -v git >/dev/null 2>&1; then
		printf '%s\n' unknown
		return
	fi

	if ! git -C "$script_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		printf '%s\n' unknown
		return
	fi

	dirty=""
	if ! git -C "$script_dir" diff-index --quiet HEAD -- 2>/dev/null; then
		dirty="+"
	fi

	best_tag=""
	best_distance=""
	for tag in $(git -C "$script_dir" tag --merged HEAD --list); do
		if ! printf '%s' "$tag" | grep -Eq "$version_tag_pattern"; then
			continue
		fi

		commit=$(git -C "$script_dir" rev-list -n 1 "$tag" 2>/dev/null) ||
			continue
		distance=$(git -C "$script_dir" rev-list --count "$commit..HEAD" 2>/dev/null) ||
			continue

		if [ -z "$best_tag" ] ||
			[ "$distance" -lt "$best_distance" ] ||
			{ [ "$distance" -eq "$best_distance" ] &&
				version_tag_is_greater "$tag" "$best_tag"; }; then
			best_tag=$tag
			best_distance=$distance
		fi
	done

	if [ -z "$best_tag" ]; then
		printf '%s\n' unknown
		return
	fi

	if [ "$best_distance" -eq 0 ]; then
		printf '%s%s\n' "$best_tag" "$dirty"
		return
	fi

	sha=$(git -C "$script_dir" rev-parse --short=7 HEAD)
	printf '%s-%s-g%s%s\n' "$best_tag" "$best_distance" "$sha" "$dirty"
}

version_tag_is_greater() {
	awk -v left="$1" -v right="$2" '
    function parse(tag, out) {
      if (match(tag, /^([0-9]+)\.([0-9]+)\.([0-9]+)(-rc([0-9]+))?$/, m) == 0) {
        return 0
      }
      out[1] = m[1] + 0
      out[2] = m[2] + 0
      out[3] = m[3] + 0
      out[4] = (m[5] == "") ? -1 : m[5] + 0
      return 1
    }
    BEGIN {
      if (!parse(left, l) || !parse(right, r)) {
        exit 1
      }
      for (i = 1; i <= 3; ++i) {
        if (l[i] > r[i]) {
          exit 0
        }
        if (l[i] < r[i]) {
          exit 1
        }
      }
      if (l[4] == -1 && r[4] != -1) {
        exit 0
      }
      if (l[4] != -1 && r[4] == -1) {
        exit 1
      }
      exit (l[4] > r[4]) ? 0 : 1
    }
  '
}

strip_dirty_suffix() {
	printf '%s' "$1" | sed 's/+$//'
}

has_dirty_suffix() {
	case "$1" in
	*+)
		return 0
		;;
	*)
		return 1
		;;
	esac
}

sanitize_debian_base() {
	printf '%s' "$1" |
		sed \
			-e 's/^v//' \
			-e 's/-rc/~rc/g' \
			-e 's/[^A-Za-z0-9.+~]/./g'
}

debian_version() {
	raw=$1
	dirty=""
	if has_dirty_suffix "$raw"; then
		dirty=".dirty"
	fi
	raw=$(strip_dirty_suffix "$raw")

	if printf '%s' "$raw" | grep -Eq '^.+-[0-9]+-g[0-9A-Fa-f]+$'; then
		tag=${raw%-g*}
		sha=${raw##*-g}
		distance=${tag##*-}
		tag=${tag%-"$distance"}
		base=$(sanitize_debian_base "$tag")
		printf '%s+%s.g%s%s\n' "$base" "$distance" "$sha" "$dirty"
		return
	fi

	if printf '%s' "$raw" | grep -Eq '^[0-9A-Fa-f]{7,40}$|^unknown$'; then
		printf '0.0.0+g%s%s\n' "$raw" "$dirty"
		return
	fi

	base=$(sanitize_debian_base "$raw")
	printf '%s%s\n' "$base" "$dirty"
}

sanitize_arch_base() {
	printf '%s' "$1" |
		sed \
			-e 's/^v//' \
			-e 's/-/_/g' \
			-e 's/[^A-Za-z0-9_.+]/_/g'
}

arch_version() {
	raw=$1
	dirty=""
	if has_dirty_suffix "$raw"; then
		dirty=".dirty"
	fi
	raw=$(strip_dirty_suffix "$raw")

	if printf '%s' "$raw" | grep -Eq '^.+-[0-9]+-g[0-9A-Fa-f]+$'; then
		tag=${raw%-g*}
		sha=${raw##*-g}
		distance=${tag##*-}
		tag=${tag%-"$distance"}
		base=$(sanitize_arch_base "$tag")
		printf '%s.r%s.g%s%s\n' "$base" "$distance" "$sha" "$dirty"
		return
	fi

	if printf '%s' "$raw" | grep -Eq '^[0-9A-Fa-f]{7,40}$|^unknown$'; then
		printf '0.0.0.g%s%s\n' "$raw" "$dirty"
		return
	fi

	base=$(sanitize_arch_base "$raw")
	printf '%s%s\n' "$base" "$dirty"
}

package_filename() {
	if [ -z "$package_arch" ]; then
		echo "--package requires --arch ARCH" >&2
		exit 2
	fi

	release_part=""
	if [ -n "$package_release" ]; then
		release_part="-$package_release"
	fi

	case "$1" in
	debian)
		pkgver=$(debian_version "$2")
		printf '%s_%s%s_%s.deb\n' \
			"$package_name" "$pkgver" "$release_part" "$package_arch"
		;;
	arch)
		pkgver=$(arch_version "$2")
		printf '%s-%s%s-%s.pkg.tar.zst\n' \
			"$package_name" "$pkgver" "$release_part" "$package_arch"
		;;
	*)
		echo "unknown package format: $1" >&2
		usage >&2
		exit 2
		;;
	esac
}

raw_version=$(git_version)
if [ -n "$package_format" ]; then
	package_filename "$package_format" "$raw_version"
	exit 0
fi

case "$mode" in
git)
	printf '%s\n' "$raw_version"
	;;
debian)
	debian_version "$raw_version"
	;;
arch)
	arch_version "$raw_version"
	;;
esac
