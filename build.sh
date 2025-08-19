#!/bin/sh

set -e # Exit early if any commands fail

BUILD_TYPE="${1:-Debug}"

(
  echo "Compiling shaders"
  cd "$(dirname "$0")"
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

  cd ..

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

  TOOLCHAIN=""
  if [ -n "${VCPKG_ROOT}" ]; then
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  fi

  cmake -S . -B build -DCMAKE_BUILD_TYPE:STRING=${BUILD_TYPE} -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS} ${TOOLCHAIN}
  cmake --build ./build --target vulkan_renderer --config ${BUILD_TYPE}

  echo "Copying shaders to build/app/assets"
  mkdir -p build/app/assets
  if ls assets/*.spv >/dev/null 2>&1; then
    cp -R assets/*.spv build/app/assets
  else
    echo "No .spv files to copy – skipping"
  fi
)
