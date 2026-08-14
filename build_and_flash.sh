#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage()
{
	echo "Usage: $0 <eye-1-serial-device> <eye-2-serial-device>" >&2
	echo "Example: $0 /dev/ttyACM0 /dev/ttyACM1" >&2
}

if [[ $# -ne 2 ]]; then
	usage
	exit 2
fi

role_a_device=$1
role_b_device=$2
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for board_file in conf/eye-1.conf conf/eye-2.conf; do
	if [[ ! -f "${script_dir}/${board_file}" ]]; then
		echo "Missing ${board_file}; copy and edit its .example.conf template first." >&2
		exit 2
	fi
done

if [[ ! -e "${role_a_device}" ]]; then
	echo "EYE-1 serial device does not exist: ${role_a_device}" >&2
	exit 2
fi

if [[ ! -e "${role_b_device}" ]]; then
	echo "EYE-2 serial device does not exist: ${role_b_device}" >&2
	exit 2
fi

if [[ "${role_a_device}" == "${role_b_device}" ]]; then
	echo "EYE-1 and EYE-2 must use different serial devices." >&2
	exit 2
fi

cd -- "${script_dir}"

west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-a -- \
	-DEXTRA_CONF_FILE='conf/role-a.conf;conf/eye-1.conf'

west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-b -- \
	-DEXTRA_CONF_FILE='conf/role-b.conf;conf/eye-2.conf'

west flash -d build-role-a --runner esp32 --esp-device "${role_a_device}"
west flash -d build-role-b --runner esp32 --esp-device "${role_b_device}"
