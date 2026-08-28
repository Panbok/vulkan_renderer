#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
COOKER_BIN="${VKR_MESH_COOKER_BIN:-}"
if [ -z "${COOKER_BIN}" ]; then
  "${REPO_ROOT}/build_release.sh"
  COOKER_BIN="${REPO_ROOT}/build_release/tools/vkr_mesh_cooker"
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
    echo "Mesh cook step skipped missing source: ${source_path}" >&2
    return
  fi
  output_path="${cook_source%.*}.vkb"
  echo "Cooking ${cook_source} -> ${output_path}"
  "${COOKER_BIN}" --input "${cook_source}" --output "${output_path}"
}

if [ "$#" -gt 0 ]; then
  for source_path in "$@"; do
    cook_one "${source_path}"
  done
  "${REPO_ROOT}/tools/pack_vkt_textures.sh"
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

"${REPO_ROOT}/tools/pack_vkt_textures.sh"
