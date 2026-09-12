#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
COOKER_BIN="${VKR_MESH_COOKER_BIN:-}"
if [ -z "${COOKER_BIN}" ]; then
  VKR_BUILD_TARGET=vkr_mesh_cooker VKR_BUILD_LABEL="VKR mesh cooker" \
    "${REPO_ROOT}/build.sh" Release
  BUILD_DIR="${VKR_BUILD_DIR:-build_release}"
  case "${BUILD_DIR}" in
    /*|[A-Za-z]:/*) ;;
    *) BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" ;;
  esac
  COOKER_BIN="${BUILD_DIR}/tools/vkr_mesh_cooker"
  if [ ! -x "${COOKER_BIN}" ]; then
    COOKER_BIN="${BUILD_DIR}/tools/Release/vkr_mesh_cooker"
  fi
fi
cd "${REPO_ROOT}"
if [ ! -x "${COOKER_BIN}" ]; then
  echo "Mesh cook step failed: vkr_mesh_cooker was not found at ${COOKER_BIN}" >&2
  exit 2
fi

cook_one() {
  source_path="$1"
  case "${source_path}" in
    "${REPO_ROOT}"/*) cook_source="${source_path#"${REPO_ROOT}"/}" ;;
    /*) cook_source="${source_path}" ;;
    ./*) cook_source="${source_path#./}" ;;
    *) cook_source="${source_path}" ;;
  esac
  if [ ! -f "${cook_source}" ]; then
    # Model sources are local and optional; strictness starts once one exists.
    echo "Mesh cook step skipped missing source: ${source_path}" >&2
    return 0
  fi
  if [ "${VKR_MESH_COOK_STRICT_INPUTS:-0}" = "1" ] &&
    [ ! -f "${cook_source}.vkr.json" ]; then
    echo "Mesh cook step failed: required import sidecar is missing: ${cook_source}.vkr.json" >&2
    return 1
  fi
  output_path="${cook_source%.*}.vkb"
  echo "Cooking ${cook_source} -> ${output_path}"
  "${COOKER_BIN}" --input "${cook_source}" --output "${output_path}"
}

if [ "$#" -gt 0 ]; then
  for source_path in "$@"; do
    cook_one "${source_path}"
  done
  exit 0
fi

cook_one "assets/models/falcon.obj"
cook_one "assets/models/sponza.obj"
cook_one "assets/models/New_Sponza_001.gltf"
cook_one "assets/models/NewSponza_Curtains_glTF.gltf"
cook_one "assets/models/bistro-lights.gltf"
cook_one "assets/models/bistrox.gltf"
cook_one "assets/models/bistro.gltf"
cook_one "assets/models/san-miguel-low-poly.obj"

# Small cooked witnesses for tracked runtime scene fixtures.
cook_one "tests/fixtures/rendering/specgloss_factor_parity.gltf"
cook_one "tests/fixtures/rendering/editor_nodes.gltf"
cook_one "tests/fixtures/rendering/editor_lights.gltf"

# Main Bistro's authored light ranges are scene-specific; retain the untuned
# artifact for other scenes sharing the same source.
if [ -f "assets/models/bistro-lights.gltf" ]; then
  "${COOKER_BIN}" --input "assets/models/bistro-lights.gltf" \
    --output "assets/models/bistro-lights-main.vkb" \
    --light-range "LMBR_000019c_Paris_StringLights_01_Yellow_Color=5.0" \
    --light-range "LMBR_000019a_Paris_StringLights_01_Pink_Color=5.0" \
    --light-range "LMBR_0000197_Paris_StringLights_01_Red_Color=5.0" \
    --light-range "LMBR_0000199_Paris_StringLights_01_Green_Color=5.0" \
    --light-range "LMBR_000019b_Paris_StringLights_01_Orange_Color=5.0" \
    --light-range "LMBR_0000198_Paris_StringLights_01_Blue_Color=5.0"
fi
