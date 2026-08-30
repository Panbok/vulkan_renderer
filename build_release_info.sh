#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Building vulkan_renderer (RelWithDebInfo)"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  cmake --fresh -B build_release_info -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}
  cmake --build ./build_release_info --target vulkan_renderer vkr_harness vkr_mesh_cooker --config RelWithDebInfo
  VKR_MESH_COOKER_BIN="$(pwd)/build_release_info/tools/vkr_mesh_cooker"
  if [ ! -x "${VKR_MESH_COOKER_BIN}" ]; then
    VKR_MESH_COOKER_BIN="$(pwd)/build_release_info/tools/RelWithDebInfo/vkr_mesh_cooker"
  fi
  VKR_MESH_COOKER_BIN="${VKR_MESH_COOKER_BIN}" \
    VKR_MESH_COOK_STRICT_INPUTS=1 \
    ./tools/cook_vkr_meshes.sh \
    assets/models/New_Sponza_001.gltf \
    assets/models/bistro-lights.gltf
)
