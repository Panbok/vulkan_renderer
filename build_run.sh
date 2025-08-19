#!/bin/sh

set -e

BUILD_TYPE="${1:-Debug}"

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

"${SCRIPT_DIR}/build.sh" "${BUILD_TYPE}"

exec "${SCRIPT_DIR}/build/app/vulkan_renderer" "$@"



