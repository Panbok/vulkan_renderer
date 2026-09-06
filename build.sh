#!/bin/sh

set -e # Exit early if any commands fail

BUILD_TYPE="${1:-Debug}"
VKR_BUILD_TARGET="${VKR_BUILD_TARGET:-vulkan_renderer}"
VKR_BUILD_LABEL="${VKR_BUILD_LABEL:-VKR app}"
VKR_EDITOR_LOGGING=OFF
if [ "${VKR_BUILD_TARGET}" = "vkr_editor" ]; then VKR_EDITOR_LOGGING=ON; fi

case "${BUILD_TYPE}" in
  Debug) BUILD_DIR="build_debug" ;;
  Release) BUILD_DIR="build_release" ;;
  RelWithDebInfo) BUILD_DIR="build_release_info" ;;
  MinSizeRel) BUILD_DIR="build_min_size_rel" ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'." >&2
    echo "Expected Debug, Release, RelWithDebInfo, or MinSizeRel." >&2
    exit 1
    ;;
esac

(
  echo "Building ${VKR_BUILD_LABEL} (${BUILD_TYPE})"
  cd "$(dirname "$0")"

  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi

  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi

  echo "Using build directory: ${BUILD_DIR}"
  cmake -S . -B "${BUILD_DIR}" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DVKR_EDITOR_LOGGING:BOOL="${VKR_EDITOR_LOGGING}" ${GENERATOR} ${COMPILERS}

  BUILD_TARGETS="${VKR_BUILD_TARGET} vkr_harness vkr_mesh_cooker vkr_font_cooker vkr_vkt_packer"
  cmake --build "./${BUILD_DIR}" --target $BUILD_TARGETS --config "${BUILD_TYPE}"

)
