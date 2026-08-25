#!/bin/sh

set -e # Exit early if any commands fail

(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  # Configure step
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  cmake --fresh -B build_test -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}
  # Build only the test target
  cmake --build ./build_test --target vulkan_renderer_tester --config Debug
  ./tools/pack_vkt_textures.sh
)

# Execute the test runner
export VKR_TEXTURE_VKT_STRICT=0
export VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1
export VKR_TEXTURE_VKT_ALLOW_LEGACY=1
exec "$(dirname "$0")/build_test/tests/vulkan_renderer_tester" "$@"
