#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Compiling shaders"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cd lib/assets
  for file in *.slang; do
    echo "Compiling $file"
    slangc -o "${file%.slang}.spv" "$file"
  done
  cd ../../

  echo "Building vulkan_renderer"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
  cmake --build ./build --target vulkan_renderer

  echo "Copying shaders to build directory"
  cp -R lib/assets/*.spv build/lib
)

exec $(dirname $0)/build/app/vulkan_renderer "$@"
