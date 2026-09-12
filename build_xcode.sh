#!/bin/bash

set -e 

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

echo "--- Step 1: Generating Xcode Project ---"

VKR_DEBUG_SANITIZER="${VKR_DEBUG_SANITIZER:-default}"
case "${VKR_DEBUG_SANITIZER}" in
  default|address|thread|memory|leak|none) ;;
  *)
    echo "Error: VKR_DEBUG_SANITIZER must be default, address, thread, memory, leak, or none." >&2
    exit 1
    ;;
esac
BUILD_DIR=build_xcode
if [ "${VKR_DEBUG_SANITIZER}" != default ]; then
  BUILD_DIR="build_xcode_${VKR_DEBUG_SANITIZER}"
fi
BUILD_DIR="${VKR_BUILD_DIR:-${BUILD_DIR}}"

# Reconfigure in place; Xcode owns incremental compilation.
cmake -S . -B "$BUILD_DIR" \
  -G Xcode \
  -DVKR_DEBUG_SANITIZER:STRING="${VKR_DEBUG_SANITIZER}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE

echo "--- Step 2: Setup Complete ---"
echo "Project generated at: $PROJECT_ROOT/$BUILD_DIR/vulkan_renderer.xcodeproj"
