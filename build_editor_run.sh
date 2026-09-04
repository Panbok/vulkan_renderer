#!/bin/sh

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
VKR_BUILD_TARGET=vkr_editor \
  VKR_BUILD_LABEL="VKR editor" \
  VKR_RUN_SUBDIR=editor \
  VKR_RUN_BINARY=vkr_editor \
  exec "${SCRIPT_DIR}/build_run.sh" "$@"
