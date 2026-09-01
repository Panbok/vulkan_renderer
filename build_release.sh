#!/bin/sh

set -e # Exit early if any commands fail

(
  echo "Building vulkan_renderer (Release)"
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  VKR_METRICS_CMAKE_VALUE="${VKR_METRICS_ENABLED:-ON}"
  case "${VKR_METRICS_CMAKE_VALUE}" in
    0|OFF|off|FALSE|false) VKR_METRICS_CMAKE_VALUE=OFF ;;
    1|ON|on|TRUE|true) VKR_METRICS_CMAKE_VALUE=ON ;;
    *)
      echo "Error: VKR_METRICS_ENABLED must be ON/OFF or 1/0." >&2
      exit 1
      ;;
  esac
  cmake --fresh -B build_release -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DVKR_METRICS_ENABLED="${VKR_METRICS_CMAKE_VALUE}" ${GENERATOR} ${COMPILERS}
  cmake --build ./build_release --target vulkan_renderer vkr_harness vkr_mesh_cooker vkr_font_cooker --config Release
  FONT_COOKER_BIN="./build_release/tools/vkr_font_cooker"
  if [ ! -x "${FONT_COOKER_BIN}" ]; then
    FONT_COOKER_BIN="./build_release/tools/Release/vkr_font_cooker"
  fi
  VKR_FONT_COOKER_BIN="${FONT_COOKER_BIN}" ./tools/cook_vkr_fonts.sh
  ./tools/pack_vkt_textures.sh
)
