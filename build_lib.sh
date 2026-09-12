#!/bin/sh
# Build the selected public library example and its dependencies.
set -eu
case "${1:-renderer}" in
  renderer) VKR_BUILD_TARGET=vkr_renderer_example ;;
  runtime) VKR_BUILD_TARGET=vkr_host_example ;;
  *) echo "Usage: $0 [renderer|runtime]" >&2; exit 2 ;;
esac
export VKR_BUILD_TARGET
VKR_BUILD_LABEL="VKR library example" exec "$(dirname "$0")/build.sh" Release
