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
  TOOLCHAIN=""
  if [ -n "${VCPKG_ROOT}" ]; then
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  fi
  cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS} ${TOOLCHAIN}
  # Build only the test target
  cmake --build ./build --target vulkan_renderer_tester
)

# Execute the test runner
exec $(dirname $0)/build/tests/vulkan_renderer_tester "$@" 