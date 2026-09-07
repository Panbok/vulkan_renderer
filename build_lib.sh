#!/bin/sh
# Build the reusable libraries without sample applications or authoring tools.
set -eu
cd "$(dirname "$0")"
VKR_LIBRARY_SET="${1:-renderer}"
case "$VKR_LIBRARY_SET" in
  renderer) VKR_RUNTIME=OFF; VKR_TARGET=vkr_renderer_example ;;
  runtime) VKR_RUNTIME=ON; VKR_TARGET=vkr_host_example ;;
  *) echo "Usage: $0 [renderer|runtime]" >&2; exit 2 ;;
esac
VKR_BUILD_DIR="build_lib_${VKR_LIBRARY_SET}"
set -- --fresh -S . -B "$VKR_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVKR_BUILD_RUNTIME="$VKR_RUNTIME" \
  -DVKR_BUILD_EXAMPLES=ON -DVKR_BUILD_TOOLS=OFF -DVKR_BUILD_APP=OFF -DVKR_BUILD_EDITOR=OFF \
  -DVKR_BUILD_HARNESS=OFF -DVKR_BUILD_TESTS=OFF
if command -v ninja >/dev/null 2>&1; then set -- "$@" -G Ninja; fi
if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
  set -- "$@" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
fi
cmake "$@"
cmake --build "$VKR_BUILD_DIR" --target "$VKR_TARGET" --config Release
