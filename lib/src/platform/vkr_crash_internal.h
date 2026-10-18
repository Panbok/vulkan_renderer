#pragma once

#include "platform/vkr_crash.h"

/* Seam between the portable report code in vkr_crash_common.c and each
 * native backend. Everything here is async-signal-safe: no locks, no
 * allocation and no stdio. */

#define VKR_CRASH_PATH_CAPACITY 2304u
/* Log records a report carries. */
#define VKR_CRASH_LOG_RECORDS 64u

/** Receives report bytes, such as a write to the report file. */
typedef void (*VkrCrashSink)(void *context, const char *bytes, uint64_t length);

/** Buffers report text in front of a sink. */
typedef struct VkrCrashWriter {
  VkrCrashSink sink;
  void *context;
  uint32_t used;
  char buffer[512];
} VkrCrashWriter;

void vkr_crash_writer_bytes(VkrCrashWriter *writer, const char *bytes,
                            uint64_t length);
/** Writes a C string; NULL writes nothing. */
void vkr_crash_writer_text(VkrCrashWriter *writer, const char *text);
void vkr_crash_writer_u64(VkrCrashWriter *writer, uint64_t value);
void vkr_crash_writer_i64(VkrCrashWriter *writer, int64_t value);
/** Writes "0x" and 16 hexadecimal digits. */
void vkr_crash_writer_hex(VkrCrashWriter *writer, uint64_t value);
void vkr_crash_writer_flush(VkrCrashWriter *writer);

/** Copies the report directory and program name; install calls this. */
void vkr_crash_configure(const char *report_directory,
                         const char *program_name);

/** Builds <directory>/crash-<program>-<pid>-<unix seconds><extension>;
 * false when no directory is known or the path does not fit. */
bool8_t vkr_crash_report_path(const char *extension, char *path,
                              uint64_t capacity);

/** Writes the report title; the backend writes the "reason: " line next. */
void vkr_crash_report_begin(VkrCrashWriter *writer);

/** Writes everything after the reason line except the backtrace: program,
 * pid, time, frame, text breadcrumbs and the newest log records. */
void vkr_crash_report_context(VkrCrashWriter *writer);
