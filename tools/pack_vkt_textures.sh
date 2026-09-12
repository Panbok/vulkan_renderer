#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build_release"
TEXTURE_ROOT="${VKR_TEXTURE_PACK_INPUT_DIR:-${REPO_ROOT}/assets/textures}"
STRICT_MODE="${VKR_VKT_PACK_STRICT:-0}"
FORCE_MODE="${VKR_VKT_PACK_FORCE:-0}"
VERBOSE_MODE="${VKR_VKT_PACK_VERBOSE:-0}"

if [ ! -d "${TEXTURE_ROOT}" ]; then
  echo "Texture pack step skipped: texture directory not found at ${TEXTURE_ROOT}"
  exit 0
fi

PACKER_BIN="${VKR_VKT_PACKER_BIN:-}"
if [ -z "${PACKER_BIN}" ]; then
  VKR_BUILD_TARGET=vkr_vkt_packer VKR_BUILD_LABEL="VKR texture packer" \
    VKR_BUILD_DIR="${BUILD_DIR}" "${REPO_ROOT}/build.sh" Release

  for candidate in \
    "${BUILD_DIR}/tools/vkr_vkt_packer" \
    "${BUILD_DIR}/tools/Release/vkr_vkt_packer" \
    "${BUILD_DIR}/vkr_vkt_packer" \
    "${BUILD_DIR}/Release/vkr_vkt_packer" \
    "${BUILD_DIR}/tools/vkr_vkt_packer.exe" \
    "${BUILD_DIR}/tools/Release/vkr_vkt_packer.exe" \
    "${BUILD_DIR}/vkr_vkt_packer.exe" \
    "${BUILD_DIR}/Release/vkr_vkt_packer.exe"; do
    if [ -x "${candidate}" ]; then
      PACKER_BIN="${candidate}"
      break
    fi
  done
fi

if [ -z "${PACKER_BIN}" ] || [ ! -x "${PACKER_BIN}" ]; then
  echo "Texture pack step failed: programmatic packer binary was not found." >&2
  echo "Set VKR_VKT_PACKER_BIN to use an existing packer binary." >&2
  exit 2
fi

set -- --input-dir "${TEXTURE_ROOT}"
if [ "${STRICT_MODE}" = "1" ]; then
  set -- "$@" --strict
fi
if [ "${FORCE_MODE}" = "1" ]; then
  set -- "$@" --force
fi
if [ "${VERBOSE_MODE}" = "1" ]; then
  set -- "$@" --verbose
fi

echo "Packing .vkt textures with programmatic packer: ${PACKER_BIN}"
"${PACKER_BIN}" "$@"
