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

  echo "Building renderer_lib"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
  cmake --install ./build

  echo "Copying shaders to build directory"
  cp -R lib/assets/*.spv build/lib
)
