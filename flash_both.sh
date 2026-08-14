#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage()
{
	echo "Usage: $0 <eye-1-serial-device> <eye-2-serial-device>" >&2
	echo "Example: $0 /dev/serial/by-id/eye-1 /dev/serial/by-id/eye-2" >&2
}

if [[ $# -ne 2 ]]; then
	usage
	exit 2
fi

role_a_device=$1
role_b_device=$2
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
role_a_image="${script_dir}/build-role-a/zephyr/zephyr.bin"
role_b_image="${script_dir}/build-role-b/zephyr/zephyr.bin"

if ! command -v esptool >/dev/null 2>&1; then
	echo "esptool is required on the host; install it with: pipx install esptool" >&2
	exit 2
fi

for image in "${role_a_image}" "${role_b_image}"; do
	if [[ ! -f "${image}" ]]; then
		echo "Missing build image: ${image}" >&2
		echo "Run ./build_both.sh inside the development container first." >&2
		exit 2
	fi
done

for device in "${role_a_device}" "${role_b_device}"; do
	if [[ ! -e "${device}" ]]; then
		echo "Serial device does not exist: ${device}" >&2
		exit 2
	fi
done

if [[ "${role_a_device}" == "${role_b_device}" ]]; then
	echo "EYE-1 and EYE-2 must use different serial devices." >&2
	exit 2
fi

flash_image()
{
	local device=$1
	local image=$2
	local eye=$3

	echo "Flashing EYE-${eye} to ${device}"
	sha256sum "${image}"
	esptool --chip esp32s3 --port "${device}" --baud 921600 \
		write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB \
		0x0 "${image}"
}

flash_image "${role_a_device}" "${role_a_image}" 1
flash_image "${role_b_device}" "${role_b_image}" 2
