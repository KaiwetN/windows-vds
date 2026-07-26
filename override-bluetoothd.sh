#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

readonly NOPLUGIN_INPUT="--noplugin=input"

usage() {
	cat >&2 <<'EOF'
usage: override-bluetoothd.sh [--restart] enable-input|disable-input

  enable-input   Remove the --noplugin=input override.
  disable-input  Run bluetoothd with --noplugin=input.
  --restart      Restart the detected bluetoothd service after daemon-reload.
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

require_root() {
	if [[ "${EUID}" -ne 0 ]]; then
		die "this script must be run as root"
	fi
}

require_systemd() {
	command -v systemctl >/dev/null 2>&1 || die "systemctl is not available"
	systemctl --system show-environment >/dev/null 2>&1 ||
		die "systemd is not running as the system manager"
}

candidate_services() {
	printf '%s\n' bluetooth.service
	systemctl list-unit-files --type=service --no-legend --no-pager |
		awk '{ print $1 }'
	systemctl list-units --all --type=service --no-legend --no-pager |
		awk '{ print $1 }'
}

service_execstart_command() {
	local service=$1

	systemctl cat "${service}" 2>/dev/null |
		awk '
      /^[[:space:]]*ExecStart=.*bluetoothd/ {
        sub(/^[[:space:]]*ExecStart=/, "")
        print
        exit
      }
    '
}

find_bluetooth_service() {
	local service
	while IFS= read -r service; do
		[[ "${service}" == *.service ]] || continue
		if [[ -n "$(service_execstart_command "${service}")" ]]; then
			printf '%s\n' "${service}"
			return
		fi
	done < <(candidate_services | awk '!seen[$0]++')

	die "no systemd service running bluetoothd was found"
}

remove_noplugin_input() {
	local text=$1

	awk -v option="${NOPLUGIN_INPUT}" '
    {
      pattern = "(^|[[:space:]])" option "([[:space:]]|$)"
      while ($0 ~ pattern) {
        sub(pattern, " ")
      }
      sub(/^[[:space:]]+/, "")
      sub(/[[:space:]]+$/, "")
      print
    }
  ' <<<"${text}"
}

execstart_command_path() {
	local command=$1
	local first

	first=${command%%[[:space:]]*}
	first=${first#-}
	first=${first#+}
	first=${first#!}
	first=${first#@}
	first=${first#:}
	printf '%s\n' "${first}"
}

base_execstart_command() {
	local service=$1
	local command
	local path

	command=$(service_execstart_command "${service}")
	[[ -n "${command}" ]] ||
		die "failed to read bluetoothd ExecStart from ${service}"

	command=$(remove_noplugin_input "${command}")
	path=$(execstart_command_path "${command}")
	[[ -n "${path}" && "${path}" == */bluetoothd ]] ||
		die "ExecStart for ${service} does not run bluetoothd"

	printf '%s\n' "${command}"
}

override_file() {
	local service=$1

	systemctl cat "${service}" 2>/dev/null |
		awk -v suffix="/${service}.d/override.conf" '
      /^# / {
        path = substr($0, 3)
        if (path ~ suffix "$") {
          print path
          exit
        }
      }
    '
}

read_override() {
	local service=$1
	local file

	file=$(override_file "${service}")
	if [[ -n "${file}" && -f "${file}" ]]; then
		cat "${file}"
	fi
}

write_override() {
	local service=$1
	local source_file=$2

	systemctl edit --stdin "${service}" <"${source_file}"
}

remove_override() {
	local service=$1
	local file
	local dir

	file=$(override_file "${service}")
	[[ -n "${file}" ]] || return
	rm -f "${file}"
	dir=$(dirname "${file}")
	rmdir "${dir}" 2>/dev/null || true
}

default_execstart() {
	local base_command=$1

	printf 'ExecStart=%s %s\n' "${base_command}" "${NOPLUGIN_INPUT}"
}

default_override() {
	local base_command=$1

	cat <<EOF
[Service]
ExecStart=
$(default_execstart "${base_command}")
EOF
}

input_enabled_override() {
	local base_command=$1

	cat <<EOF
[Service]
ExecStart=
ExecStart=${base_command}
EOF
}

write_default_override() {
	local service=$1
	local base_command=$2
	local tmp

	tmp=$(mktemp)
	default_override "${base_command}" >"${tmp}"
	write_override "${service}" "${tmp}"
	rm -f "${tmp}"
}

write_input_enabled_override() {
	local service=$1
	local base_command=$2
	local tmp

	tmp=$(mktemp)
	input_enabled_override "${base_command}" >"${tmp}"
	write_override "${service}" "${tmp}"
	rm -f "${tmp}"
}

is_exact_default_override() {
	local service=$1
	local base_command=$2
	local tmp_current tmp_default
	local same=1

	tmp_current=$(mktemp)
	tmp_default=$(mktemp)
	read_override "${service}" >"${tmp_current}"
	default_override "${base_command}" >"${tmp_default}"
	if cmp -s "${tmp_current}" "${tmp_default}"; then
		same=0
	fi
	rm -f "${tmp_current}" "${tmp_default}"
	return "${same}"
}

has_execstart_command() {
	local file=$1

	grep -Eq '^[[:space:]]*ExecStart=.*[^[:space:]]' "${file}"
}

has_bluetoothd_execstart_command() {
	local file=$1

	grep -Eq '^[[:space:]]*ExecStart=.*bluetoothd' "${file}"
}

has_effective_override_setting() {
	local file=$1

	grep -Eq '^[[:space:]]*[A-Za-z][A-Za-z0-9]*[[:space:]]*=' "${file}"
}

disable_input() {
	local service=$1
	local base_command=$2
	local tmp

	tmp=$(mktemp)
	read_override "${service}" >"${tmp}"

	if [[ ! -s "${tmp}" ]]; then
		rm -f "${tmp}"
		write_default_override "${service}" "${base_command}"
		return
	fi

	if ! has_execstart_command "${tmp}" ||
		! has_bluetoothd_execstart_command "${tmp}"; then
		{
			cat "${tmp}"
			printf '\n'
			default_override "${base_command}"
		} >"${tmp}.new"
		mv "${tmp}.new" "${tmp}"
	else
		awk -v option="${NOPLUGIN_INPUT}" '
      /^[[:space:]]*ExecStart=.*bluetoothd/ {
        pattern = "(^|[[:space:]])" option "([[:space:]]|$)"
        if ($0 !~ pattern) {
          $0 = $0 " " option
        }
      }
      { print }
    ' "${tmp}" >"${tmp}.new"
		mv "${tmp}.new" "${tmp}"
	fi

	write_override "${service}" "${tmp}"
	rm -f "${tmp}"
}

enable_input() {
	local service=$1
	local base_command=$2
	local tmp_block tmp_clean

	if [[ -z "$(override_file "${service}")" ]]; then
		if [[ "$(service_execstart_command "${service}")" == *"${NOPLUGIN_INPUT}"* ]]; then
			write_input_enabled_override "${service}" "${base_command}"
		fi
		return
	fi

	if is_exact_default_override "${service}" "${base_command}"; then
		remove_override "${service}"
		return
	fi

	tmp_block=$(mktemp)
	tmp_clean=$(mktemp)

	read_override "${service}" >"${tmp_block}"
	awk -v default_line="$(default_execstart "${base_command}")" '
    {
      lines[++count] = $0
    }
    END {
      line_index = 1
      while (line_index <= count) {
        if (lines[line_index] == "ExecStart=" &&
            line_index < count &&
            lines[line_index + 1] == default_line) {
          line_index += 2
          continue
        }
        print lines[line_index]
        ++line_index
      }
    }
  ' "${tmp_block}" >"${tmp_clean}"

	awk -v option="${NOPLUGIN_INPUT}" '
    /^[[:space:]]*ExecStart=/ {
      pattern = "(^|[[:space:]])" option "([[:space:]]|$)"
      while ($0 ~ pattern) {
        sub(pattern, " ")
      }
      sub(/[[:space:]]+$/, "")
    }
    { print }
  ' "${tmp_clean}" >"${tmp_block}"

	if ! has_effective_override_setting "${tmp_block}"; then
		remove_override "${service}"
	else
		write_override "${service}" "${tmp_block}"
	fi

	rm -f "${tmp_block}" "${tmp_clean}"
}

daemon_reload() {
	systemctl daemon-reload
}

restart_bluetooth() {
	local service=$1

	systemctl restart "${service}"
}

main() {
	local action=""
	local restart="no"
	local service
	local base_command

	while (($# > 0)); do
		case "$1" in
		enable-input | disable-input)
			if [[ -n "${action}" ]]; then
				usage
				exit 2
			fi
			action=$1
			;;
		--restart)
			restart="yes"
			;;
		-h | --help)
			usage
			exit 0
			;;
		*)
			usage
			exit 2
			;;
		esac
		shift
	done

	if [[ -z "${action}" ]]; then
		usage
		exit 2
	fi

	require_root
	require_systemd

	service=$(find_bluetooth_service)
	base_command=$(base_execstart_command "${service}")

	case "${action}" in
	enable-input)
		enable_input "${service}" "${base_command}"
		;;
	disable-input)
		disable_input "${service}" "${base_command}"
		;;
	esac

	daemon_reload
	if [[ "${restart}" == "yes" ]]; then
		restart_bluetooth "${service}"
	fi
}

main "$@"
