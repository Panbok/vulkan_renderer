#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Compiling shaders"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cd assets
  
  # Check if slangc compiler is available
  if ! command -v slangc >/dev/null 2>&1; then
    echo "Error: slangc compiler not found. Please install slangc." >&2
    exit 1
  fi
  
  # Check if any .slang files exist
  if ls *.slang >/dev/null 2>&1; then
    for file in *.slang; do
      echo "Compiling $file"
      slangc -target spirv -o "${file%.slang}.spv" "$file"
    done
  else
    echo "No .slang files found to compile"
  fi
  
  cd ../../

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
  TOOLCHAIN=""
  if [ -n "${VCPKG_ROOT}" ]; then
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  fi
  cmake -B build_release_info -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS} ${TOOLCHAIN}
  cmake --build ./build_release_info --target vulkan_renderer --config RelWithDebInfo

  echo "Copying shaders to release build directory"
  mkdir -p build_release_info/app/assets
  if ls assets/*.spv >/dev/null 2>&1; then
    cp -R assets/*.spv build_release_info/app/assets
  else
    echo "No .spv files to copy – skipping"
  fi
)