#!/bin/sh

set -e # Exit early if any commands fail

(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  # Configure step
  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi
  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi
  cmake --fresh -B build_test -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}
  # Build only the test target
  cmake --build ./build_test --target vulkan_renderer_tester vkr_font_cooker --config Debug
  FONT_COOKER_BIN="./build_test/tools/vkr_font_cooker"
  if [ ! -x "${FONT_COOKER_BIN}" ]; then
    FONT_COOKER_BIN="./build_test/tools/Debug/vkr_font_cooker"
  fi
  "${FONT_COOKER_BIN}" --identity-self-test
  VKR_FONT_COOK_TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vkr_font_cooker.XXXXXX")"
  trap 'rm -rf "${VKR_FONT_COOK_TEST_DIR}"' 0 HUP INT TERM
  "${FONT_COOKER_BIN}" --config assets/fonts/UbuntuMono-cooked.fontcfg \
    --output "${VKR_FONT_COOK_TEST_DIR}/first.vkfa" --force
  "${FONT_COOKER_BIN}" --config assets/fonts/UbuntuMono-cooked.fontcfg \
    --output "${VKR_FONT_COOK_TEST_DIR}/second.vkfa" --force
  if ! cmp -s "${VKR_FONT_COOK_TEST_DIR}/first.vkfa" \
    "${VKR_FONT_COOK_TEST_DIR}/second.vkfa"; then
    echo "Font cooker produced non-deterministic artifacts." >&2
    exit 1
  fi
  VKR_FONT_SKIP_OUTPUT="$(
    "${FONT_COOKER_BIN}" --config assets/fonts/UbuntuMono-cooked.fontcfg \
      --output "${VKR_FONT_COOK_TEST_DIR}/second.vkfa"
  )"
  printf '%s\n' "${VKR_FONT_SKIP_OUTPUT}"
  case "${VKR_FONT_SKIP_OUTPUT}" in
  "status=skipped "*) ;;
  *)
    echo "Font cooker did not skip an unchanged artifact." >&2
    exit 1
    ;;
  esac
  rm -rf "${VKR_FONT_COOK_TEST_DIR}"
  trap - 0 HUP INT TERM
  VKR_FONT_COOKER_BIN="${FONT_COOKER_BIN}" ./tools/cook_vkr_fonts.sh
  ./tools/pack_vkt_textures.sh
)

# Execute the test runner
export VKR_TEXTURE_VKT_STRICT=0
export VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1
export VKR_TEXTURE_VKT_ALLOW_LEGACY=1
exec "$(dirname "$0")/build_test/tests/vulkan_renderer_tester" "$@"
