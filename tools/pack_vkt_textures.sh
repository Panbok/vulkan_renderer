#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build_vkt_packer"
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
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi

  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi

  echo "Building the configuration-independent texture packer"
  cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -U CMAKE_TOOLCHAIN_FILE \
    -DCMAKE_BUILD_TYPE:STRING=Release \
    ${GENERATOR} ${COMPILERS}
  cmake --build "${BUILD_DIR}" --target vkr_vkt_packer --config Release

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
