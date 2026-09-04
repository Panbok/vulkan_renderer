#!/bin/sh

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
VKR_BUILD_TARGET=vkr_editor VKR_BUILD_LABEL="VKR editor" \
  "${SCRIPT_DIR}/build.sh" "$@"
