#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
zephyr_cache_dir="${script_dir}/.cache/zephyr"
ccache_dir="${script_dir}/.cache/ccache"

for board_file in conf/eye-1.conf conf/eye-2.conf; do
	if [[ ! -f "${script_dir}/${board_file}" ]]; then
		echo "Missing ${board_file}; copy and edit its .example.conf template first." >&2
		exit 2
	fi
done

cd -- "${script_dir}"
mkdir -p -- "${zephyr_cache_dir}"
mkdir -p -- "${ccache_dir}"

CCACHE_DIR="${ccache_dir}" west build -p auto -b esp32s3_eye/esp32s3/procpu . \
	-d build-role-a -- \
	-DUSER_CACHE_DIR="${zephyr_cache_dir}" \
	-DEXTRA_CONF_FILE='conf/role-a.conf;conf/eye-1.conf'

CCACHE_DIR="${ccache_dir}" west build -p auto -b esp32s3_eye/esp32s3/procpu . \
	-d build-role-b -- \
	-DUSER_CACHE_DIR="${zephyr_cache_dir}" \
	-DEXTRA_CONF_FILE='conf/role-b.conf;conf/eye-2.conf'

echo "Role images ready:"
echo "  ${script_dir}/build-role-a/zephyr/zephyr.bin"
echo "  ${script_dir}/build-role-b/zephyr/zephyr.bin"
