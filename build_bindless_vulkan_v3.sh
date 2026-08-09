#!/bin/sh

set -e

BUILD_TYPE="${1:-Debug}"

case "${BUILD_TYPE}" in
  Debug|Release|RelWithDebInfo|MinSizeRel) ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'." >&2
    echo "Expected Debug, Release, RelWithDebInfo, or MinSizeRel." >&2
    exit 1
    ;;
esac

BUILD_DIR="build_bindless_vulkan_v3_${BUILD_TYPE}"

(
  cd "$(dirname "$0")"

  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi

  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi

  cmake --fresh -S . -B "${BUILD_DIR}" -U CMAKE_TOOLCHAIN_FILE \
    -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}
  cmake --build "${BUILD_DIR}" --target vkr_bindless_vulkan_v3 \
    --config "${BUILD_TYPE}"

  echo "Built ${BUILD_DIR}/tools/vkr_bindless_vulkan_v3"
)
