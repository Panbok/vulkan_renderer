#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Building renderer_lib"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  cmake --fresh -B build -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}
  cmake --build ./build --target renderer_lib
  cmake --install ./build || true

  echo "Copying shaders to release build directory"
  mkdir -p build/lib/assets
  if ls assets/shaders/*.spv >/dev/null 2>&1; then
    cp -R assets/shaders/*.spv build/lib/assets
  else
    echo "No .spv files to copy – skipping"
  fi
)
