#pragma once

#include "core/vkr_atomic.h"
#include "defines.h"

/*
 * Apple GPU counters from Instruments into a recording (ADR-130). Apple
 * GPUs give occupancy and their ALU, texture and memory limiters only to
 * Instruments, so a recording takes a trace of the Metal GPU Counters and
 * os_signpost instruments of its own process and imports it afterwards into
 * its bundle's VKR_RECORDING_GPU_COUNTERS_FILE
 * (assets/vkr_recording_format.h), which the analysis places on frames.
 *
 * The trace's times map to the CPU clock recordings use through the
 * os_signpost "Clock" events the frame loop emits while signposts are on
 * (vkr_platform_signpost_clock): the import takes the median of their
 * offsets. Both calls block for seconds to minutes (xctrace exports about
 * 47 s per traced second on an M1 Pro), so they run on a job worker. macOS
 * only: elsewhere they fail with an error.
 */

/* Microseconds the import averages samples over by default. */
#define VKR_RECORDING_COUNTERS_BUCKET_US 50u
/* Longest trace the editor takes, in seconds. */
#define VKR_RECORDING_COUNTERS_TRACE_MAX_S 10u
#define VKR_RECORDING_COUNTERS_ERROR_MAX 192u

/* The tables an import reads from a trace, in the order it reads them. */
typedef enum VkrRecordingCountersTable {
  VKR_RECORDING_COUNTERS_TABLE_SIGNPOST = 0,
  VKR_RECORDING_COUNTERS_TABLE_INFO,
  VKR_RECORDING_COUNTERS_TABLE_VALUES,
  VKR_RECORDING_COUNTERS_TABLE_COUNT,
} VkrRecordingCountersTable;

/** Receives a table's XML as xctrace writes it, in chunks of any size;
 * false stops the export. */
typedef bool8_t (*VkrRecordingCountersSink)(void *context, const uint8_t *bytes,
                                            uint64_t size);

/** Writes table `table` of the trace to `sink`; false when it could not.
 * The default runs `xcrun xctrace export`; tests feed their own XML. */
typedef bool8_t (*VkrRecordingCountersSource)(void *context,
                                              VkrRecordingCountersTable table,
                                              VkrRecordingCountersSink sink,
                                              void *sink_context);

typedef struct VkrRecordingCountersImport {
  /* The .trace bundle; read through `source` when that is set. */
  const char *trace_path;
  /* The recording bundle the counter file is written into. */
  const char *bundle_path;
  /* Zero takes VKR_RECORDING_COUNTERS_BUCKET_US. */
  uint32_t bucket_us;
  VkrRecordingCountersSource source;
  void *source_context;
  /* Optional: polled while it runs; true stops the import. */
  bool8_t (*is_cancelled)(void *context);
  void *cancel_context;
  /* Optional: the samples read so far, for a progress display. */
  VkrAtomicUint64 *samples_read;
} VkrRecordingCountersImport;

typedef struct VkrRecordingCountersResult {
  uint32_t counters;
  uint64_t samples;
  uint32_t buckets;
  uint32_t bucket_ns;
  /* The Clock events found and how far their offsets spread. */
  uint32_t anchors;
  uint64_t spread_ns;
  uint64_t file_bytes;
  char error[VKR_RECORDING_COUNTERS_ERROR_MAX];
} VkrRecordingCountersResult;

/**
 * Traces process `pid` for `seconds` with Instruments' Metal GPU Counters
 * and os_signpost instruments into `trace_path` (a .trace bundle that must
 * not exist), blocking until the trace is saved. The process needs
 * signposts on for the import to place the samples. False with the reason
 * in `error`.
 */
bool8_t vkr_recording_counters_trace(uint32_t pid, uint32_t seconds,
                                     const char *trace_path,
                                     bool8_t (*is_cancelled)(void *context),
                                     void *cancel_context, char *error,
                                     uint64_t error_capacity);

/**
 * Imports a trace's GPU counters into a recording bundle, replacing its
 * counter file. False with the reason in out->error; a failed import
 * leaves an earlier file in place.
 */
bool8_t vkr_recording_counters_import(const VkrRecordingCountersImport *import,
                                      VkrRecordingCountersResult *out);

/** Removes a .trace bundle the trace call wrote; false for another path. */
bool8_t vkr_recording_counters_remove_trace(const char *trace_path);
