#!/bin/sh

set -e # Exit early if any commands fail

BUILD_TYPE="${1:-Debug}"

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
  echo "Building vulkan_renderer (${BUILD_TYPE})"
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
  cmake -S . -B "${BUILD_DIR}" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}

  BUILD_TARGETS="vulkan_renderer vkr_harness vkr_mesh_cooker"
  cmake --build "./${BUILD_DIR}" --target $BUILD_TARGETS --config "${BUILD_TYPE}"
  VKR_MESH_COOKER_BIN="$(pwd)/${BUILD_DIR}/tools/vkr_mesh_cooker"
  if [ ! -x "${VKR_MESH_COOKER_BIN}" ]; then
    VKR_MESH_COOKER_BIN="$(pwd)/${BUILD_DIR}/tools/${BUILD_TYPE}/vkr_mesh_cooker"
  fi
  VKR_MESH_COOKER_BIN="${VKR_MESH_COOKER_BIN}" \
    VKR_MESH_COOK_STRICT_INPUTS=1 \
    ./tools/cook_vkr_meshes.sh \
    assets/models/New_Sponza_001.gltf \
    assets/models/bistro-lights.gltf
)
