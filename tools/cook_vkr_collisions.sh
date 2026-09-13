#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VKR_BUILD_TARGET=vkr_collision_cooker VKR_BUILD_LABEL="VKR collision cooker" \
  "${REPO_ROOT}/build.sh" Release
BUILD_DIR="${VKR_BUILD_DIR:-build_release}"
case "${BUILD_DIR}" in
  /*|[A-Za-z]:/*) ;;
  *) BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" ;;
esac
COOKER_BIN="${BUILD_DIR}/tools/vkr_collision_cooker"
if [ ! -x "${COOKER_BIN}" ]; then
  COOKER_BIN="${BUILD_DIR}/tools/Release/vkr_collision_cooker"
fi
exec "${COOKER_BIN}" "$@"
