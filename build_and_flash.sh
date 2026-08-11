#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage()
{
	echo "Usage: $0 <role-a-serial-device> <role-b-serial-device>" >&2
	echo "Example: $0 /dev/ttyACM0 /dev/ttyACM1" >&2
}

if [[ $# -ne 2 ]]; then
	usage
	exit 2
fi

role_a_device=$1
role_b_device=$2
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for site_file in conf/site-a.conf conf/site-b.conf; do
	if [[ ! -f "${script_dir}/${site_file}" ]]; then
		echo "Missing ${site_file}; copy and edit its .example.conf template first." >&2
		exit 2
	fi
done

if [[ ! -e "${role_a_device}" ]]; then
	echo "Role A serial device does not exist: ${role_a_device}" >&2
	exit 2
fi

if [[ ! -e "${role_b_device}" ]]; then
	echo "Role B serial device does not exist: ${role_b_device}" >&2
	exit 2
fi

if [[ "${role_a_device}" == "${role_b_device}" ]]; then
	echo "Role A and role B must use different serial devices." >&2
	exit 2
fi

cd -- "${script_dir}"

west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-a -- \
	-DEXTRA_CONF_FILE='conf/role-a.conf;conf/site-a.conf'

west build -p always -b esp32s3_eye/esp32s3/procpu . -d build-role-b -- \
	-DEXTRA_CONF_FILE='conf/role-b.conf;conf/site-b.conf'

west flash -d build-role-a --runner esp32 --esp-device "${role_a_device}"
west flash -d build-role-b --runner esp32 --esp-device "${role_b_device}"
