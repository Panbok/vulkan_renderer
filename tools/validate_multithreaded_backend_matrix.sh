#!/bin/sh

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/_validation/multithreaded_backend"
LOG_DIR="${BUILD_ROOT}/logs"

SMOKE_MODE=0
if [ "${1:-}" = "--smoke" ]; then
  SMOKE_MODE=1
fi

GENERATOR_ARGS=""
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS="-G Ninja"
fi

COMPILER_ARGS=""
if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
  COMPILER_ARGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
fi

PASS_COUNT=0
FAIL_COUNT=0

log_has_runtime_errors() {
  log_file="$1"
  if [ ! -f "${log_file}" ]; then
    return 1
  fi

  if rg -n \
    -e "validation layer:" \
    -e "VUID-" \
    -e "AddressSanitizer" \
    -e "ABORTING" \
    -e "Abort trap" \
    -e "Segmentation fault" \
    "${log_file}" >/dev/null 2>&1; then
    return 0
  fi

  return 1
}

run_runtime_case() {
  build_dir="$1"
  compile_case="$2"
  runtime_case="$3"

  runner="${build_dir}/tests/vulkan_renderer_tester"
  log_file="${LOG_DIR}/${compile_case}__${runtime_case}.log"

  printf "  [runtime:%s] " "$runtime_case"
  if [ "$runtime_case" = "default_env" ]; then
    if "$runner" >"${log_file}" 2>&1; then
      if log_has_runtime_errors "${log_file}"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "FAIL (validation/crash markers, see ${log_file})"
      else
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "PASS"
      fi
    else
      FAIL_COUNT=$((FAIL_COUNT + 1))
      echo "FAIL (see ${log_file})"
    fi
    return
  fi

  upload_flag=0
  case "$runtime_case" in
  force_serial)
    upload_flag=0
    ;;
  upload_only)
    upload_flag=1
    ;;
  *)
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL (unknown runtime case)"
    return
    ;;
  esac

  if VKR_PARALLEL_UPLOAD="${upload_flag}" \
    "$runner" >"${log_file}" 2>&1; then
    if log_has_runtime_errors "${log_file}"; then
      FAIL_COUNT=$((FAIL_COUNT + 1))
      echo "FAIL (validation/crash markers, see ${log_file})"
    else
      PASS_COUNT=$((PASS_COUNT + 1))
      echo "PASS"
    fi
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL (see ${log_file})"
  fi
}

configure_and_build() {
  build_dir="$1"
  compile_flags="$2"

  mkdir -p "$build_dir"
  if [ -n "$compile_flags" ]; then
    cmake --fresh -S "${ROOT_DIR}" -B "$build_dir" -U CMAKE_TOOLCHAIN_FILE \
      -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
      -DCMAKE_C_FLAGS:STRING="$compile_flags" \
      -DCMAKE_CXX_FLAGS:STRING="$compile_flags" \
      ${GENERATOR_ARGS} ${COMPILER_ARGS}
  else
    cmake --fresh -S "${ROOT_DIR}" -B "$build_dir" -U CMAKE_TOOLCHAIN_FILE \
      -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
      ${GENERATOR_ARGS} ${COMPILER_ARGS}
  fi
  cmake --build "$build_dir" --target vulkan_renderer_tester
}

run_compile_case() {
  compile_case="$1"
  compile_flags="$2"
  runtime_case_list="$3"

  build_dir="${BUILD_ROOT}/${compile_case}"
  echo "[compile:${compile_case}] configuring/building"
  configure_and_build "$build_dir" "$compile_flags"

  for runtime_case in $runtime_case_list; do
    run_runtime_case "$build_dir" "$compile_case" "$runtime_case"
  done
}

mkdir -p "${LOG_DIR}"

if [ "$SMOKE_MODE" -eq 1 ]; then
  echo "Running smoke validation matrix (single case)"
  run_compile_case "default_compile" "" "default_env"
else
  echo "Running multithreaded Vulkan backend validation matrix"
  run_compile_case "default_compile" "" \
    "default_env force_serial upload_only"
  run_compile_case "compile_serial" \
    "-DVKR_VULKAN_PARALLEL_UPLOAD=0" \
    "default_env"
  run_compile_case "compile_upload_parallel" \
    "-DVKR_VULKAN_PARALLEL_UPLOAD=1" \
    "default_env"
fi

echo ""
echo "Validation matrix complete: PASS=${PASS_COUNT}, FAIL=${FAIL_COUNT}"
echo "Logs: ${LOG_DIR}"

if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
