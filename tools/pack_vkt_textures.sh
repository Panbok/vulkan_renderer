#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
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
  for candidate in \
    "${REPO_ROOT}/build/tools/vkr_vkt_packer" \
    "${REPO_ROOT}/build/tools/Debug/vkr_vkt_packer" \
    "${REPO_ROOT}/build/tools/Release/vkr_vkt_packer" \
    "${REPO_ROOT}/build/vkr_vkt_packer" \
    "${REPO_ROOT}/build/Debug/vkr_vkt_packer" \
    "${REPO_ROOT}/build/Release/vkr_vkt_packer"; do
    if [ -x "${candidate}" ]; then
      PACKER_BIN="${candidate}"
      break
    fi
  done
fi

if [ -z "${PACKER_BIN}" ] || [ ! -x "${PACKER_BIN}" ]; then
  echo "Texture pack step failed: programmatic packer binary was not found."
  echo "Build target 'vkr_vkt_packer' first or set VKR_VKT_PACKER_BIN."
  if [ "${STRICT_MODE}" = "1" ]; then
    exit 1
  fi
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
