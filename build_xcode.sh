#!/bin/bash

set -e 

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

echo "--- Step 1: Compiling Shaders ---"
if [ -d "assets/shaders" ]; then
  cd assets/shaders
  for file in *.slang; do
    if [ -f "$file" ]; then
      echo "Compiling $file"
      slangc -target spirv -o "${file%.slang}.spv" "$file"
    fi
  done
  cd "$PROJECT_ROOT"
fi

echo "--- Step 2: Generating Xcode Project ---"

BUILD_DIR="build_xcode"

# IMPORTANT: Remove the old build directory to clear the broken compiler cache
if [ -d "$BUILD_DIR" ]; then
  echo "Cleaning old cache..."
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# Note: We REMOVED CMAKE_C_COMPILER and CMAKE_CXX_COMPILER.
# Xcode will automatically use its internal clang.
cmake -S . -B "$BUILD_DIR" \
  --fresh \
  -U CMAKE_TOOLCHAIN_FILE \
  -G Xcode \
  -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE

echo "--- Step 3: Setup Complete ---"
echo "Project generated at: $PROJECT_ROOT/$BUILD_DIR/vulkan_renderer.xcodeproj"
