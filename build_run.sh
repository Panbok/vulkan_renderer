#!/bin/sh

set -e

BUILD_TYPE="${1:-Debug}"

if [ "$#" -ge 1 ]; then
  shift
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

case "${BUILD_TYPE}" in
  Debug) BUILD_DIR="build_debug" ;;
  Release) BUILD_DIR="build_release" ;;
  RelWithDebInfo) BUILD_DIR="build_release_info" ;;
  MinSizeRel) BUILD_DIR="build_min_size_rel" ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'." >&2
    exit 1
    ;;
esac

"${SCRIPT_DIR}/build.sh" "${BUILD_TYPE}"

BIN="${SCRIPT_DIR}/${BUILD_DIR}/app/vulkan_renderer"
if [ ! -x "$BIN" ] && [ -x "${SCRIPT_DIR}/${BUILD_DIR}/app/${BUILD_TYPE}/vulkan_renderer" ]; then
  BIN="${SCRIPT_DIR}/${BUILD_DIR}/app/${BUILD_TYPE}/vulkan_renderer"
fi
if [ ! -x "$BIN" ]; then
  echo "Error: built binary not found or not executable at: $BIN" >&2
  exit 1
fi

if [ "$(uname -s)" = "Darwin" ] &&
  { [ "${MTL_DEBUG_LAYER:-0}" != "0" ] ||
    [ "${MTL_SHADER_VALIDATION:-0}" != "0" ]; }; then
  echo "WARNING: Metal validation is enabled" \
    "(MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-0}," \
    "MTL_SHADER_VALIDATION=${MTL_SHADER_VALIDATION:-0})." \
    "It can severely reduce FPS; do not use this run for performance evidence." \
    >&2
fi

exec "$BIN" "$@"
