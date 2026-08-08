#!/bin/sh

set -e # Exit early if any commands fail

BUILD_TYPE="${1:-Debug}"

case "${BUILD_TYPE}" in
  Debug) BUILD_DIR="build_debug" ;;
  Release) BUILD_DIR="build_release" ;;
  RelWithDebInfo) BUILD_DIR="build_release_info" ;;
  MinSizeRel) BUILD_DIR="build_min_size_rel" ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'." >&2
    echo "Expected Debug, Release, RelWithDebInfo, or MinSizeRel." >&2
    exit 1
    ;;
esac

(
  echo "Building vulkan_renderer (${BUILD_TYPE})"
  cd "$(dirname "$0")"

  GENERATOR=""
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="-G Ninja"
  fi

  COMPILERS=""
  if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  fi

  echo "Using build directory: ${BUILD_DIR}"
  cmake -S . -B "${BUILD_DIR}" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}

  BUILD_TARGETS="vulkan_renderer vkr_harness"
  cmake --build "./${BUILD_DIR}" --target $BUILD_TARGETS --config "${BUILD_TYPE}"

  echo "Copying shaders to ${BUILD_DIR}/app/assets"
  mkdir -p "${BUILD_DIR}/app/assets"
  if ls assets/shaders/*.spv >/dev/null 2>&1; then
    cp -R assets/shaders/*.spv "${BUILD_DIR}/app/assets"
  else
    echo "No .spv files to copy – skipping"
  fi
)
