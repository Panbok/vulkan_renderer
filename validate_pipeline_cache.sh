#!/bin/sh
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
BIN="${SCRIPT_DIR}/build/app/vulkan_renderer"
CACHE_PATH="/tmp/vkr_pipeline_cache_phase7.bin"
RUN1_LOG="/tmp/vkr_phase7_run1.log"
RUN2_LOG="/tmp/vkr_phase7_run2.log"

if [ "${VKR_SKIP_BUILD:-0}" != "1" ]; then
  "${SCRIPT_DIR}/build.sh" Debug
fi

rm -f "${CACHE_PATH}" "${CACHE_PATH}.tmp"
rm -f "${RUN1_LOG}" "${RUN2_LOG}"

run_once() {
  run_name="$1"
  run_log="$2"

  if ! VKR_PIPELINE_CACHE_PATH="${CACHE_PATH}" VKR_AUTOCLOSE_SECONDS=2 \
      "${BIN}" > "${run_log}" 2>&1; then
    echo "Run ${run_name} failed. Tail of ${run_log}:"
    tail -n 120 "${run_log}" || true
    exit 1
  fi
}

run_once "1" "${RUN1_LOG}"
run_once "2" "${RUN2_LOG}"

rg -n "Pipeline cache path|Loaded pipeline cache data|Initialized Vulkan pipeline cache|Saved pipeline cache data|Auto-close" \
  "${RUN1_LOG}" "${RUN2_LOG}"

rg -n "Pipeline create time:" "${RUN1_LOG}" "${RUN2_LOG}" || true

summarize_pipeline_time() {
  run_log="$1"
  awk -F'Pipeline create time: | ms' '
    /Pipeline create time:/ {
      sum += $2;
      count += 1;
    }
    END {
      if (count == 0) {
        printf("count=0 sum_ms=0.000 avg_ms=0.000");
      } else {
        printf("count=%d sum_ms=%.3f avg_ms=%.3f", count, sum, sum / count);
      }
    }' "${run_log}"
}

echo "run1 $(summarize_pipeline_time "${RUN1_LOG}")"
echo "run2 $(summarize_pipeline_time "${RUN2_LOG}")"

stat -f "size=%z mtime=%m" "${CACHE_PATH}"
