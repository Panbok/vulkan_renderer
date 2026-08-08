#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_type="${1:-Debug}"
build_type_slug="$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')"
build_dir="$repo_root/build_bindless_vulkan_v0_$build_type_slug"

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

cmake --fresh -S "$repo_root" -B "$build_dir" \
    -U CMAKE_TOOLCHAIN_FILE \
    -DCMAKE_BUILD_TYPE:STRING="$build_type" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
    "${generator[@]}"
cmake --build "$build_dir" --target vkr_bindless_vulkan_v0 \
    --config "$build_type"
