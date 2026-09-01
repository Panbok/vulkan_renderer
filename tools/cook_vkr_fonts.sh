#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
COOKER_BIN="${VKR_FONT_COOKER_BIN:-}"
if [ -z "${COOKER_BIN}" ]; then
  BUILD_DIR="${VKR_FONT_COOKER_BUILD_DIR:-${REPO_ROOT}/build_font_cooker}"
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then GENERATOR="-G Ninja"; fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  echo "Building vkr_font_cooker in ${BUILD_DIR}"
  cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -U CMAKE_TOOLCHAIN_FILE \
    -DCMAKE_BUILD_TYPE:STRING=Release ${GENERATOR} ${COMPILERS}
  cmake --build "${BUILD_DIR}" --target vkr_font_cooker --config Release
  for candidate in \
    "${BUILD_DIR}/tools/vkr_font_cooker" \
    "${BUILD_DIR}/tools/Release/vkr_font_cooker" \
    "${BUILD_DIR}/vkr_font_cooker" \
    "${BUILD_DIR}/Release/vkr_font_cooker"; do
    if [ -x "${candidate}" ]; then COOKER_BIN="${candidate}"; break; fi
  done
fi

if [ -z "${COOKER_BIN}" ] || [ ! -x "${COOKER_BIN}" ]; then
  echo "Font cook step failed: vkr_font_cooker was not found." >&2
  echo "Set VKR_FONT_COOKER_BIN to use an existing cooker binary." >&2
  exit 2
fi

cd "${REPO_ROOT}"
if [ "$#" -eq 0 ]; then
  set -- "assets/fonts/UbuntuMono-cooked.fontcfg"
fi
for config in "$@"; do
  echo "Cooking ${config}"
  "${COOKER_BIN}" --config "${config}"
done
