#!/bin/sh

set -e # Exit early if any commands fail

# Tests consume the checked-in cooked fixtures. Bakery owns regeneration.
VKR_BUILD_TARGET=vulkan_renderer_tester VKR_BUILD_LABEL="VKR CPU tests" \
  "$(dirname "$0")/build.sh" Debug

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR=build_debug
if [ "${VKR_DEBUG_SANITIZER:-default}" != default ]; then
  BUILD_DIR="build_debug_${VKR_DEBUG_SANITIZER}"
fi
BUILD_DIR="${VKR_BUILD_DIR:-${BUILD_DIR}}"
case "${BUILD_DIR}" in
  /*|[A-Za-z]:/*) ;;
  *) BUILD_DIR="${SCRIPT_DIR}/${BUILD_DIR}" ;;
esac
TEST_BIN="${BUILD_DIR}/tests/vulkan_renderer_tester"
if [ ! -x "${TEST_BIN}" ]; then
  TEST_BIN="${BUILD_DIR}/tests/Debug/vulkan_renderer_tester"
fi

# Execute the test runner
export VKR_TEXTURE_VKT_STRICT=0
export VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1
export VKR_TEXTURE_VKT_ALLOW_LEGACY=1
exec "${TEST_BIN}" "$@"
