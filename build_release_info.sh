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
  ./tools/pack_vkt_textures.sh
)
