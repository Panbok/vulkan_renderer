#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Compiling shaders"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cd app/assets
  
  # Check if slangc compiler is available
  if ! command -v slangc >/dev/null 2>&1; then
    echo "Error: slangc compiler not found. Please install slangc." >&2
    exit 1
  fi
  
  # Check if any .slang files exist
  if ls *.slang >/dev/null 2>&1; then
    for file in *.slang; do
      echo "Compiling $file"
      slangc -o "${file%.slang}.spv" "$file"
    done
  else
    echo "No .slang files found to compile"
  fi
  
  cd ../../

  echo "Building vulkan_renderer (Release)"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cmake -B build_release -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
  cmake --build ./build_release --target vulkan_renderer --config Release

  echo "Copying shaders to release build directory"
  cp -R app/assets/*.spv assets
)

exec $(dirname $0)/build_release/app/vulkan_renderer "$@" 