#!/bin/sh

set -e # Exit early if any commands fail

(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  # Configure step (can often be skipped if build dir exists and config hasn't changed)
  # Re-running ensures VCPKG path is picked up if environment changed.
  cmake -B build -S . #-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake # Optional: uncomment if using vcpkg
  # Build only the test target
  cmake --build ./build --target vulkan_renderer_tester
)

# Execute the test runner
exec $(dirname $0)/build/tests/vulkan_renderer_tester "$@" 