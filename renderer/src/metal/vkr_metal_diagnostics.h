#pragma once

#include "defines.h"

#define VKR_METAL_DIAGNOSTIC_SEGMENT_BYTES MB(4)
#define VKR_METAL_DIAGNOSTIC_RECORD_BYTES 4096u

/** Renderer-owned, single-writer diagnostic output. Zero initialization is a
 * disabled sink and may be closed safely. */
typedef struct VkrMetalDiagnostics {
  int segment_fds[2];
  uint64_t sequence;
  uint64_t active_bytes;
  uint32_t active_segment;
  bool8_t enabled;
  bool8_t error_reported;
} VkrMetalDiagnostics;

/** Creates a new, exclusive directory containing two rotating JSONL segments.
 */
bool8_t vkr_metal_diagnostics_open(VkrMetalDiagnostics *diagnostics,
                                   const char *directory);

/** Appends one bounded JSON object. The caller owns serialization and must call
 * only from the renderer's single diagnostic writer. */
#if defined(__clang__) || defined(__GNUC__)
#define VKR_METAL_DIAGNOSTIC_PRINTF_FORMAT __attribute__((format(printf, 5, 6)))
#else
#define VKR_METAL_DIAGNOSTIC_PRINTF_FORMAT
#endif
void vkr_metal_diagnostics_record(VkrMetalDiagnostics *diagnostics,
                                  const char *event, uint64_t submitted,
                                  uint64_t completed, const char *format,
                                  ...) VKR_METAL_DIAGNOSTIC_PRINTF_FORMAT;
#undef VKR_METAL_DIAGNOSTIC_PRINTF_FORMAT

/** Requests fsync for both segments at a cold submission or wait boundary.
 * This does not guarantee persistence after a sudden system failure. */
void vkr_metal_diagnostics_flush(VkrMetalDiagnostics *diagnostics);

/** Closes every owned descriptor. Safe for a zero-initialized sink. */
void vkr_metal_diagnostics_close(VkrMetalDiagnostics *diagnostics);
