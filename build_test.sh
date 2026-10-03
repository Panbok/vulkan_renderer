#!/bin/sh

set -e # Exit early if any commands fail

python3 "$(dirname "$0")/tools/checks/check_format.py"
python3 "$(dirname "$0")/tools/checks/check_path_boundaries.py"
python3 "$(dirname "$0")/tools/checks/report_long_functions.py" --quiet \
  --max-lines 300

# The tester target cooks the fixture textures it reads (vkr_engine_textures).
VKR_BUILD_TARGET=vulkan_renderer_tester VKR_BUILD_LABEL="VKR CPU tests" \
  "$(dirname "$0")/build.sh" Debug
# check_bakery_package.py packages a project with the Debug player template.
VKR_BUILD_TARGET=vkr_player VKR_BUILD_LABEL="VKR player template" \
  "$(dirname "$0")/build.sh" Debug

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR=build_debug
if [ "${VKR_DEBUG_SANITIZER:-default}" != default ]; then
  BUILD_DIR="build_debug_${VKR_DEBUG_SANITIZER}"
fi
BUILD_DIR="${VKR_BUILD_DIR:-${BUILD_DIR}}"
case "${BUILD_DIR}" in
  /*|[A-Za-z]:/*) ;;
  *) BUILD_DIR="${SCRIPT_DIR}/${BUILD_DIR}" ;;
esac
# ADR-015 lets a build compile the metric writers out; keep that
# configuration compiling without a second full build.
python3 "${SCRIPT_DIR}/tools/checks/check_metrics_disabled.py" \
  "${BUILD_DIR}/compile_commands.json"

# The managed path grammar runs through the project runner this build made.
python3 "${SCRIPT_DIR}/tools/checks/check_path_contract.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# The editor's daemon protocol, watches, cancellation and shutdown (ADR-077).
python3 "${SCRIPT_DIR}/tools/checks/check_bakery_serve.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# Bundle closure and the .vkpak layout, read back independently (ADR-077).
python3 "${SCRIPT_DIR}/tools/checks/check_bakery_bundle.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# Project packages: layout, portable documents, kept package on failure and
# cancellation (docs/proposals/project-packaging.md).
python3 "${SCRIPT_DIR}/tools/checks/check_bakery_package.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# Preview-tier imports and their finalization to the final tier (ADR-077).
python3 "${SCRIPT_DIR}/tools/checks/check_editor_texture_tiers.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# Spec-gloss conversion memo: reuse, invalidation and PNG output (ADR-077).
python3 "${SCRIPT_DIR}/tools/checks/check_spec_gloss_memo.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"
# C script modules: compile, link, archive, cache and diagnostics (ADR-077).
python3 "${SCRIPT_DIR}/tools/checks/check_bakery_script.py" \
  --bakery "${BUILD_DIR}/tools/bakery/vkr_bakery"

TEST_BIN="${BUILD_DIR}/tests/vulkan_renderer_tester"
if [ ! -x "${TEST_BIN}" ]; then
  TEST_BIN="${BUILD_DIR}/tests/Debug/vulkan_renderer_tester"
fi

# Execute the test runner
exec "${TEST_BIN}" "$@"
