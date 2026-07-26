#!/bin/sh
# SPDX-License-Identifier: MIT

set -e

service_name="vdsd.service"
enable_service=0
start_service=0

usage() {
	cat <<'USAGE'
usage: install-service.sh [options]

Options:
  --service NAME  systemd service name. Defaults to vdsd.service.
  --enable        Enable the service for future boots.
  --start         Start the service after daemon-reload.
  -h, --help      Show this help text.
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
	--enable)
		enable_service=1
		shift
		;;
	--start)
		start_service=1
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

if ! is_systemd_running; then
	echo "systemd is not running as the system manager; skipping ${service_name}" >&2
	exit 0
fi

systemctl daemon-reload

if [ "$enable_service" -eq 1 ] && [ "$start_service" -eq 1 ]; then
	systemctl enable --now "$service_name"
elif [ "$enable_service" -eq 1 ]; then
	systemctl enable "$service_name"
elif [ "$start_service" -eq 1 ]; then
	systemctl start "$service_name"
fi
