#!/bin/sh
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
BIN="${SCRIPT_DIR}/build/app/vulkan_renderer"
CACHE_PATH="/tmp/vkr_pipeline_cache_phase7.bin"
RUN1_LOG="/tmp/vkr_phase7_run1.log"
RUN2_LOG="/tmp/vkr_phase7_run2.log"
RUN1_METRICS="/tmp/vkr_phase7_run1.metrics.json"
RUN2_METRICS="/tmp/vkr_phase7_run2.metrics.json"

if [ "${VKR_SKIP_BUILD:-0}" != "1" ]; then
  "${SCRIPT_DIR}/build.sh" Debug
fi

rm -f "${CACHE_PATH}" "${CACHE_PATH}.tmp"
rm -f "${RUN1_LOG}" "${RUN2_LOG}" "${RUN1_METRICS}" "${RUN2_METRICS}"

run_once() {
  run_name="$1"
  run_log="$2"
  run_metrics="$3"

  if ! VKR_PIPELINE_CACHE_PATH="${CACHE_PATH}" VKR_AUTOCLOSE_SECONDS=2 \
      "${BIN}" --metrics-json "${run_metrics}" > "${run_log}" 2>&1; then
    echo "Run ${run_name} failed. Tail of ${run_log}:"
    tail -n 120 "${run_log}" || true
    exit 1
  fi
}

run_once "1" "${RUN1_LOG}" "${RUN1_METRICS}"
run_once "2" "${RUN2_LOG}" "${RUN2_METRICS}"

rg -n "Pipeline cache path|Loaded pipeline cache data|Initialized Vulkan pipeline cache|Saved pipeline cache data|Auto-close" \
  "${RUN1_LOG}" "${RUN2_LOG}"

summarize_pipeline_time() {
  run_metrics="$1"
  awk '
    {
      remaining = $0;
      while (match(remaining, /"source":"pipeline.create"/)) {
        count += 1;
        remaining = substr(remaining, RSTART + RLENGTH);
        if (match(remaining, /"duration_ns":[0-9]+/)) {
          token = substr(remaining, RSTART, RLENGTH);
          sub(/"duration_ns":/, "", token);
          sum_ns += token;
        }
      }
    }
    END {
      if (count == 0) {
        printf("count=0 sum_ms=0.000 avg_ms=0.000");
      } else {
        sum_ms = sum_ns / 1000000.0;
        printf("count=%d sum_ms=%.3f avg_ms=%.3f", count, sum_ms, sum_ms / count);
      }
    }' "${run_metrics}"
}

RUN1_SUMMARY="$(summarize_pipeline_time "${RUN1_METRICS}")"
RUN2_SUMMARY="$(summarize_pipeline_time "${RUN2_METRICS}")"
echo "run1 ${RUN1_SUMMARY}"
echo "run2 ${RUN2_SUMMARY}"

RUN1_COUNT="$(echo "${RUN1_SUMMARY}" | awk -F'[ =]' '{print $2}')"
RUN2_COUNT="$(echo "${RUN2_SUMMARY}" | awk -F'[ =]' '{print $2}')"
if [ "${RUN1_COUNT}" -eq 0 ] || [ "${RUN1_COUNT}" -ne "${RUN2_COUNT}" ]; then
  echo "Pipeline creation event count mismatch: run1=${RUN1_COUNT} run2=${RUN2_COUNT}" >&2
  exit 1
fi

stat -f "size=%z mtime=%m" "${CACHE_PATH}"
