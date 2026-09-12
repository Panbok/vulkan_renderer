#!/bin/sh

set -eu

BUILD_TYPE="${1:-Debug}"
VKR_BUILD_TARGET="${VKR_BUILD_TARGET:-vulkan_renderer}"
VKR_BUILD_LABEL="${VKR_BUILD_LABEL:-VKR app}"
case "${BUILD_TYPE}" in
  Debug) BUILD_DIR=build_debug ;;
  Release) BUILD_DIR=build_release ;;
  RelWithDebInfo) BUILD_DIR=build_release_info ;;
  MinSizeRel) BUILD_DIR=build_min_size_rel ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'." >&2
    echo "Expected Debug, Release, RelWithDebInfo, or MinSizeRel." >&2
    exit 1
    ;;
esac
VKR_DEBUG_SANITIZER="${VKR_DEBUG_SANITIZER:-default}"
case "${VKR_DEBUG_SANITIZER}" in
  default|address|thread|memory|leak|none) ;;
  *)
    echo "Error: VKR_DEBUG_SANITIZER must be default, address, thread, memory, leak, or none." >&2
    exit 1
    ;;
esac
if [ "${VKR_DEBUG_SANITIZER}" != default ]; then
  if [ "${BUILD_TYPE}" != Debug ]; then
    echo "Error: explicit VKR_DEBUG_SANITIZER profiles require a Debug build." >&2
    exit 1
  fi
  BUILD_DIR="build_debug_${VKR_DEBUG_SANITIZER}"
fi
BUILD_DIR="${VKR_BUILD_DIR:-${BUILD_DIR}}"
cd "$(dirname "$0")"
echo "Building ${VKR_BUILD_LABEL} (${BUILD_TYPE}) in ${BUILD_DIR}"

# Keep a stable target graph so every entry point reuses the same dependencies.
set -- -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
  -DVKR_DEBUG_SANITIZER:STRING="${VKR_DEBUG_SANITIZER}" \
  -DVKR_BUILD_RUNTIME=ON -DVKR_BUILD_TOOLS=ON -DVKR_BUILD_APP=ON \
  -DVKR_BUILD_EDITOR=ON -DVKR_BUILD_HARNESS=ON -DVKR_BUILD_TESTS=ON \
  -DVKR_BUILD_EXAMPLES=ON
# Compiler, generator and toolchain selections belong to the existing cache.
if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
  if command -v ninja >/dev/null 2>&1; then set -- "$@" -G Ninja; fi
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    set -- "$@" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  fi
fi
if [ -n "${VKR_METRICS_ENABLED:-}" ]; then
  case "${VKR_METRICS_ENABLED}" in
    0|OFF|off|FALSE|false) METRICS_VALUE=OFF ;;
    1|ON|on|TRUE|true) METRICS_VALUE=ON ;;
    *) echo "Error: VKR_METRICS_ENABLED must be ON/OFF or 1/0." >&2; exit 1 ;;
  esac
  set -- "$@" -DVKR_METRICS_ENABLED:BOOL="${METRICS_VALUE}"
fi
if [ -n "${VKR_EDITOR_LOGGING:-}" ]; then
  set -- "$@" -DVKR_EDITOR_LOGGING:BOOL="${VKR_EDITOR_LOGGING}"
fi
cmake "$@"
set -- "${VKR_BUILD_TARGET}"
case "${VKR_BUILD_TARGET}" in
  vulkan_renderer|vkr_editor) set -- "$@" vkr_harness ;;
esac
cmake --build "${BUILD_DIR}" --target "$@" --config "${BUILD_TYPE}"
