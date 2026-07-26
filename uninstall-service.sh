#!/bin/sh
# SPDX-License-Identifier: MIT

set -e

service_name="vdsd.service"
stop_service=0
disable_service=0
reload_systemd=1
remove_files=""

usage() {
	cat <<'USAGE'
usage: uninstall-service.sh [options]

Options:
  --service NAME      systemd service name. Defaults to vdsd.service.
  --stop              Stop the service if systemd is running.
  --disable           Disable the service if systemd is running.
  --remove-file PATH  Remove an installed service file.
  --no-reload         Do not run systemctl daemon-reload.
  -h, --help          Show this help text.
USAGE
}

is_systemd_running() {
	command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--service)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --service" >&2
			exit 2
		fi
		service_name=$2
		shift 2
		;;
	--stop)
		stop_service=1
		shift
		;;
	--disable)
		disable_service=1
		shift
		;;
	--remove-file)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --remove-file" >&2
			exit 2
		fi
		remove_files="${remove_files}
$2"
		shift 2
		;;
	--no-reload)
		reload_systemd=0
		shift
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

if is_systemd_running; then
	if [ "$stop_service" -eq 1 ]; then
		systemctl stop "$service_name" >/dev/null 2>&1 || true
	fi
	if [ "$disable_service" -eq 1 ]; then
		systemctl disable "$service_name" >/dev/null 2>&1 || true
	fi
fi

printf '%s\n' "$remove_files" | while IFS= read -r service_file; do
	[ -n "$service_file" ] || continue
	rm -f -- "$service_file"
done

if [ "$reload_systemd" -eq 1 ] && is_systemd_running; then
	systemctl daemon-reload
fi
