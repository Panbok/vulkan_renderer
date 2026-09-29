#!/bin/sh

set -e

# Builds the Release editor and installs the relocatable editor distribution
# (docs/adr/078-project-build-and-packaging.md) into the given folder, by
# default build_release/dist/VKR Editor. The folder is replaced.
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
PREFIX="${1:-${SCRIPT_DIR}/build_release/dist/VKR Editor}"
"${SCRIPT_DIR}/build_editor.sh" Release
rm -rf "${PREFIX}"
cmake --install "${SCRIPT_DIR}/build_release" --prefix "${PREFIX}" \
  --component editor --config Release
echo "Installed the editor distribution in ${PREFIX}"
