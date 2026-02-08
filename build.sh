#!/bin/sh

set -e # Exit early if any commands fail

BUILD_TYPE="${1:-Debug}"

(
  echo "Compiling shaders"
  cd "$(dirname "$0")"
  cd assets

  # Check if slangc compiler is available
  if ! command -v slangc >/dev/null 2>&1; then
    echo "Error: slangc compiler not found. Please install slangc." >&2
    exit 1
  fi

  # Check if any .slang files exist in shaders subdirectory
  cd shaders 2>/dev/null || exit 0
  if ls *.slang >/dev/null 2>&1; then
    for file in *.slang; do
      echo "Compiling $file"
      slangc -target spirv -o "${file%.slang}.spv" "$file"
    done
  else
    echo "No .slang files found to compile"
  fi

  cd ../..

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

  cmake --fresh -S . -B build -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=${BUILD_TYPE} -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE ${GENERATOR} ${COMPILERS}

  if [ "${BUILD_TYPE}" = "Release" ] && [ "${VKR_VKT_PACK:-1}" != "1" ]; then
    echo "Release build requires texture packing. Set VKR_VKT_PACK=1."
    exit 1
  fi
  if [ "${BUILD_TYPE}" = "Release" ] && [ -z "${VKR_VKT_PACK_STRICT+x}" ]; then
    export VKR_VKT_PACK_STRICT=1
    echo "Release build: enabling strict texture packing (VKR_VKT_PACK_STRICT=1)"
  fi

  if [ "${VKR_VKT_PACK:-1}" = "1" ]; then
    echo "Building texture packer"
    cmake --build ./build --target vkr_vkt_packer --config ${BUILD_TYPE}
    echo "Packing textures (.vkt)"
    ./tools/pack_vkt_textures.sh
  else
    echo "Skipping texture pack step (set VKR_VKT_PACK=1 to enable)"
  fi

  cmake --build ./build --target vulkan_renderer --config ${BUILD_TYPE}

  echo "Copying shaders to build/app/assets"
  mkdir -p build/app/assets
  if ls assets/shaders/*.spv >/dev/null 2>&1; then
    cp -R assets/shaders/*.spv build/app/assets
  else
    echo "No .spv files to copy – skipping"
  fi
)
