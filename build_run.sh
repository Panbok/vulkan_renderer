#!/bin/sh

set -e

BUILD_TYPE="${1:-Debug}"

if [ "$#" -ge 1 ]; then
  shift
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

"${SCRIPT_DIR}/build.sh" "${BUILD_TYPE}"

BIN="${SCRIPT_DIR}/build/app/vulkan_renderer"
if [ ! -x "$BIN" ]; then
  echo "Error: built binary not found or not executable at: $BIN" >&2
  echo "Hint: With multi-config generators (e.g., Visual Studio/Xcode), the binary may live under a config subdir (e.g., build/app/Debug/)." >&2
  exit 1
fi
exec "$BIN" "$@"



