#!/bin/sh

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_TYPE="${VKR_BENCH_BUILD_TYPE:-Release}"
FORCE_BUILD="${VKR_BENCH_FORCE_BUILD:-0}"
SKIP_BUILD="${VKR_BENCH_SKIP_BUILD:-0}"
OUT_DIR="${ROOT_DIR}/build/_validation/multithreaded_backend/perf"
LOG_DIR="${OUT_DIR}/logs"
SUMMARY_CSV="${OUT_DIR}/summary.csv"
BIN=""
BUILD_CACHE=""

AUTOCLOSE_SECONDS="${VKR_BENCH_AUTOCLOSE_SECONDS:-8}"
MAX_WAIT_SECONDS="${VKR_BENCH_MAX_WAIT_SECONDS:-45}"
SMOKE_MODE=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [--smoke] [--build-type <Debug|Release|RelWithDebInfo>]

Environment overrides:
  VKR_BENCH_BUILD_TYPE        Build type (default: Release)
  VKR_BENCH_FORCE_BUILD       1 => always rebuild before run (default: 0)
  VKR_BENCH_SKIP_BUILD        1 => never rebuild, use existing binary (default: 0)
  VKR_BENCH_AUTOCLOSE_SECONDS Auto-close delay passed to app (default: 8)
  VKR_BENCH_MAX_WAIT_SECONDS  Max per-case wait before timeout (default: 45)

Build pipeline:
  Release            uses ./build_release.sh and ./build_release/app/vulkan_renderer
  Debug/RelWithDebInfo use ./build.sh <type> and ./build/app/vulkan_renderer
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
  --smoke)
    SMOKE_MODE=1
    shift
    ;;
  --build-type)
    if [ "$#" -lt 2 ]; then
      echo "Error: --build-type requires an argument" >&2
      usage >&2
      exit 1
    fi
    BUILD_TYPE="$2"
    shift 2
    ;;
  --help|-h)
    usage
    exit 0
    ;;
  *)
    echo "Error: unknown argument '$1'" >&2
    usage >&2
    exit 1
    ;;
  esac
done

resolve_build_layout() {
  case "${BUILD_TYPE}" in
  Release)
    BIN="${ROOT_DIR}/build_release/app/vulkan_renderer"
    BUILD_CACHE="${ROOT_DIR}/build_release/CMakeCache.txt"
    ;;
  Debug|RelWithDebInfo)
    BIN="${ROOT_DIR}/build/app/vulkan_renderer"
    BUILD_CACHE="${ROOT_DIR}/build/CMakeCache.txt"
    ;;
  *)
    echo "Error: unsupported build type '${BUILD_TYPE}'" >&2
    echo "Supported values: Release, Debug, RelWithDebInfo" >&2
    exit 1
    ;;
  esac
}

cached_build_type() {
  if [ ! -f "${BUILD_CACHE}" ]; then
    echo ""
    return 0
  fi

  awk -F= '/^CMAKE_BUILD_TYPE:STRING=/{print $2; exit}' "${BUILD_CACHE}"
}

extract_value() {
  key="$1"
  line="$2"
  for token in $line; do
    case "$token" in
    "${key}"=*)
      echo "${token#*=}"
      return 0
      ;;
    esac
  done
  echo ""
}

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

ensure_binary() {
  resolve_build_layout
  current_build_type="$(cached_build_type)"
  needs_build=0

  if [ "${SKIP_BUILD}" != "1" ]; then
    if [ "${FORCE_BUILD}" = "1" ] || [ ! -x "${BIN}" ] ||
      [ "${current_build_type}" != "${BUILD_TYPE}" ]; then
      needs_build=1
    fi

    if [ "${needs_build}" -eq 1 ]; then
      echo "Preparing benchmark binary (build_type=${BUILD_TYPE}, current=${current_build_type:-none})..."
      if [ "${BUILD_TYPE}" = "Release" ]; then
        "${ROOT_DIR}/build_release.sh"
      else
        "${ROOT_DIR}/build.sh" "${BUILD_TYPE}"
      fi
    fi
  fi

  if [ ! -x "${BIN}" ]; then
    echo "Error: app binary not found at ${BIN}" >&2
    echo "Hint: run ./build.sh ${BUILD_TYPE} or unset VKR_BENCH_SKIP_BUILD." >&2
    exit 1
  fi

  final_build_type="$(cached_build_type)"
  echo "Benchmark config: build_type=${final_build_type:-unknown} smoke=${SMOKE_MODE} autoclose=${AUTOCLOSE_SECONDS}s max_wait=${MAX_WAIT_SECONDS}s"
  if [ "${SKIP_BUILD}" = "1" ]; then
    echo "Benchmark config: VKR_BENCH_SKIP_BUILD=1 (using existing binary as-is)"
  fi
}

run_case() {
  case_name="$1"
  upload_flag="$2"

  log_file="${LOG_DIR}/${case_name}.log"
  echo "[case:${case_name}] upload=${upload_flag}"

  VKR_BENCHMARK_LOG=1 \
    VKR_BENCHMARK_LABEL="${case_name}" \
    VKR_RG_GPU_TIMING=1 \
    VKR_AUTOCLOSE_SECONDS="${AUTOCLOSE_SECONDS}" \
    VKR_PARALLEL_UPLOAD="${upload_flag}" \
    "${BIN}" >"${log_file}" 2>&1 &
  app_pid=$!

  waited=0
  timed_out=0
  while kill -0 "${app_pid}" 2>/dev/null; do
    if [ "${waited}" -ge "${MAX_WAIT_SECONDS}" ]; then
      timed_out=1
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done

  if [ "${timed_out}" -eq 1 ]; then
    echo "  timeout after ${MAX_WAIT_SECONDS}s, terminating pid ${app_pid}"
    kill "${app_pid}" 2>/dev/null || true
    sleep 1
    kill -9 "${app_pid}" 2>/dev/null || true
    wait "${app_pid}" 2>/dev/null || true
    echo "  FAIL (timeout, see ${log_file})"
    return 1
  fi

  if ! wait "${app_pid}"; then
    echo "  FAIL (app exited non-zero, see ${log_file})"
    return 1
  fi

  if log_has_runtime_errors "${log_file}"; then
    echo "  FAIL (validation/crash markers found, see ${log_file})"
    return 1
  fi

  summary_line="$(grep "BENCHMARK_SUMMARY" "${log_file}" | tail -n 1 || true)"
  if [ -z "${summary_line}" ]; then
    echo "  FAIL (missing BENCHMARK_SUMMARY, see ${log_file})"
    return 1
  fi

  samples="$(extract_value "samples" "${summary_line}")"
  avg_frame_ms="$(extract_value "avg_frame_ms" "${summary_line}")"
  min_frame_ms="$(extract_value "min_frame_ms" "${summary_line}")"
  max_frame_ms="$(extract_value "max_frame_ms" "${summary_line}")"
  rg_cpu_samples="$(extract_value "rg_cpu_samples" "${summary_line}")"
  avg_rg_cpu_ms="$(extract_value "avg_rg_cpu_ms" "${summary_line}")"

  if [ -z "${samples}" ] || [ -z "${avg_frame_ms}" ]; then
    echo "  FAIL (could not parse summary, see ${log_file})"
    return 1
  fi

  echo "${case_name},${upload_flag},${samples},${avg_frame_ms},${min_frame_ms},${max_frame_ms},${rg_cpu_samples},${avg_rg_cpu_ms}" >>"${SUMMARY_CSV}"
  echo "  PASS avg_frame_ms=${avg_frame_ms} avg_rg_cpu_ms=${avg_rg_cpu_ms}"
  return 0
}

mkdir -p "${LOG_DIR}"
ensure_binary

echo "case,upload,samples,avg_frame_ms,min_frame_ms,max_frame_ms,rg_cpu_samples,avg_rg_cpu_ms" >"${SUMMARY_CSV}"

PASS_COUNT=0
FAIL_COUNT=0

if [ "${SMOKE_MODE}" -eq 1 ]; then
  if run_case "serial" 0; then
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
else
  if run_case "serial" 0; then
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi

  if run_case "upload_only" 1; then
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
fi

echo ""
echo "Performance benchmark runs complete: PASS=${PASS_COUNT}, FAIL=${FAIL_COUNT}"
echo "Summary CSV: ${SUMMARY_CSV}"
echo "Logs: ${LOG_DIR}"

if [ -f "${SUMMARY_CSV}" ]; then
  echo ""
  echo "Collected summaries:"
  awk 'NR==1{print "  " $0; next} {print "  " $0}' "${SUMMARY_CSV}"
fi

if [ "${FAIL_COUNT}" -ne 0 ]; then
  exit 1
fi
