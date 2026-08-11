#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
zephyr_cache_dir="${script_dir}/.cache/zephyr"
ccache_dir="${script_dir}/.cache/ccache"

for site_file in conf/site-a.conf conf/site-b.conf; do
	if [[ ! -f "${script_dir}/${site_file}" ]]; then
		echo "Missing ${site_file}; copy and edit its .example.conf template first." >&2
		exit 2
	fi
done

cd -- "${script_dir}"
mkdir -p -- "${zephyr_cache_dir}"
mkdir -p -- "${ccache_dir}"

CCACHE_DIR="${ccache_dir}" west build -p auto -b esp32s3_eye/esp32s3/procpu . \
	-d build-role-a -- \
	-DUSER_CACHE_DIR="${zephyr_cache_dir}" \
	-DEXTRA_CONF_FILE='conf/role-a.conf;conf/site-a.conf'

CCACHE_DIR="${ccache_dir}" west build -p auto -b esp32s3_eye/esp32s3/procpu . \
	-d build-role-b -- \
	-DUSER_CACHE_DIR="${zephyr_cache_dir}" \
	-DEXTRA_CONF_FILE='conf/role-b.conf;conf/site-b.conf'

echo "Role images ready:"
echo "  ${script_dir}/build-role-a/zephyr/zephyr.bin"
echo "  ${script_dir}/build-role-b/zephyr/zephyr.bin"
