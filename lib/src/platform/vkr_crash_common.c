#include "platform/vkr_crash_internal.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "platform/vkr_platform.h"

#define VKR_CRASH_DIRECTORY_CAPACITY 2048u
#define VKR_CRASH_PROGRAM_CAPACITY 64u

/* Written at install and by each slot's single writer; read by a crash. */
vkr_global char g_crash_directory[VKR_CRASH_DIRECTORY_CAPACITY];
vkr_global char g_crash_program[VKR_CRASH_PROGRAM_CAPACITY] = "vkr";
vkr_global VkrAtomicUint64 g_crash_frame;
vkr_global char g_crash_text[VKR_CRASH_TEXT_COUNT][VKR_CRASH_TEXT_CAPACITY];

vkr_global const char *const CRASH_TEXT_NAMES[VKR_CRASH_TEXT_COUNT] = {
    "scene", "stage", "pass", "operation"};

/* Copies at most capacity - 1 bytes of `text` and terminates; NULL clears. */
vkr_internal void crash_copy_text(char *out, uint64_t capacity,
                                  const char *text) {
  const uint64_t length =
      text ? string_length_bounded(text, capacity - 1u) : 0u;
  if (length > 0u) {
    MemCopy(out, text, length);
  }
  out[length] = '\0';
}

void vkr_crash_set_frame(uint64_t frame) {
  vkr_atomic_uint64_store(&g_crash_frame, frame, VKR_MEMORY_ORDER_RELAXED);
}

void vkr_crash_set_text(VkrCrashText slot, const char *text) {
  if ((uint32_t)slot >= VKR_CRASH_TEXT_COUNT) {
    return;
  }
  crash_copy_text(g_crash_text[slot], VKR_CRASH_TEXT_CAPACITY, text);
}

void vkr_crash_configure(const char *report_directory,
                         const char *program_name) {
  crash_copy_text(g_crash_directory, sizeof(g_crash_directory),
                  report_directory);
  crash_copy_text(g_crash_program, sizeof(g_crash_program),
                  program_name && program_name[0] ? program_name : "vkr");
}

/* Decimal digits of `value` into `out`, most significant first; returns the
   digit count. */
vkr_internal uint32_t crash_format_u64(char out[20], uint64_t value) {
  char reversed[20];
  uint32_t count = 0u;
  do {
    reversed[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value > 0u);
  for (uint32_t i = 0u; i < count; ++i) {
    out[i] = reversed[count - 1u - i];
  }
  return count;
}

void vkr_crash_writer_flush(VkrCrashWriter *writer) {
  if (writer->used > 0u) {
    writer->sink(writer->context, writer->buffer, writer->used);
    writer->used = 0u;
  }
}

void vkr_crash_writer_bytes(VkrCrashWriter *writer, const char *bytes,
                            uint64_t length) {
  if (length > sizeof(writer->buffer) - writer->used) {
    vkr_crash_writer_flush(writer);
  }
  if (length > sizeof(writer->buffer)) {
    writer->sink(writer->context, bytes, length);
    return;
  }
  MemCopy(writer->buffer + writer->used, bytes, length);
  writer->used += (uint32_t)length;
}

void vkr_crash_writer_text(VkrCrashWriter *writer, const char *text) {
  if (text) {
    vkr_crash_writer_bytes(writer, text, string_length(text));
  }
}

void vkr_crash_writer_u64(VkrCrashWriter *writer, uint64_t value) {
  char digits[20];
  vkr_crash_writer_bytes(writer, digits, crash_format_u64(digits, value));
}

void vkr_crash_writer_i64(VkrCrashWriter *writer, int64_t value) {
  if (value < 0) {
    vkr_crash_writer_bytes(writer, "-", 1u);
    vkr_crash_writer_u64(writer, (uint64_t)0 - (uint64_t)value);
    return;
  }
  vkr_crash_writer_u64(writer, (uint64_t)value);
}

void vkr_crash_writer_hex(VkrCrashWriter *writer, uint64_t value) {
  char text[18] = "0x";
  for (uint32_t i = 0u; i < 16u; ++i) {
    const uint32_t nibble = (uint32_t)(value >> (60u - 4u * i)) & 0xfu;
    text[2u + i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
  }
  vkr_crash_writer_bytes(writer, text, sizeof(text));
}

/* Appends `length` bytes to a path being built; false when they do not fit
   with the terminator. */
vkr_internal bool8_t crash_path_append(char *path, uint64_t capacity,
                                       uint64_t *used, const char *bytes,
                                       uint64_t length) {
  if (*used + length >= capacity) {
    return false_v;
  }
  MemCopy(path + *used, bytes, length);
  *used += length;
  path[*used] = '\0';
  return true_v;
}

bool8_t vkr_crash_report_path(const char *extension, char *path,
                              uint64_t capacity) {
  if (!path || capacity == 0u) {
    return false_v;
  }
  path[0] = '\0';
  char directory[VKR_CRASH_DIRECTORY_CAPACITY];
  if (g_crash_directory[0]) {
    crash_copy_text(directory, sizeof(directory), g_crash_directory);
  } else if (!log_file_directory(directory, sizeof(directory))) {
    return false_v;
  }
  char pid[20];
  const uint32_t pid_length =
      crash_format_u64(pid, vkr_platform_get_process_id());
  char seconds[20];
  const int64_t now = vkr_platform_get_unix_time();
  const uint32_t seconds_length =
      crash_format_u64(seconds, now > 0 ? (uint64_t)now : 0u);
  uint64_t used = 0u;
  return crash_path_append(path, capacity, &used, directory,
                           string_length(directory)) &&
         crash_path_append(path, capacity, &used, "/crash-", 7u) &&
         crash_path_append(path, capacity, &used, g_crash_program,
                           string_length(g_crash_program)) &&
         crash_path_append(path, capacity, &used, "-", 1u) &&
         crash_path_append(path, capacity, &used, pid, pid_length) &&
         crash_path_append(path, capacity, &used, "-", 1u) &&
         crash_path_append(path, capacity, &used, seconds, seconds_length) &&
         crash_path_append(path, capacity, &used, extension,
                           string_length(extension));
}

void vkr_crash_report_begin(VkrCrashWriter *writer) {
  vkr_crash_writer_text(writer, "VKR crash report\nreason: ");
}

/* Forwards log lines into the report. */
vkr_internal void crash_log_sink(void *context, const char *bytes,
                                 uint64_t length) {
  vkr_crash_writer_bytes((VkrCrashWriter *)context, bytes, length);
}

void vkr_crash_report_context(VkrCrashWriter *writer) {
  vkr_crash_writer_text(writer, "program: ");
  vkr_crash_writer_text(writer, g_crash_program);
  vkr_crash_writer_text(writer, "\npid: ");
  vkr_crash_writer_u64(writer, vkr_platform_get_process_id());
  vkr_crash_writer_text(writer, "\nunix_time: ");
  vkr_crash_writer_i64(writer, vkr_platform_get_unix_time());
  vkr_crash_writer_text(writer, "\nframe: ");
  vkr_crash_writer_u64(
      writer, vkr_atomic_uint64_load(&g_crash_frame, VKR_MEMORY_ORDER_RELAXED));
  vkr_crash_writer_text(writer, "\n");

  /* A slot can be mid-copy: copy it once, bounded, and print the copy. */
  for (uint32_t i = 0u; i < VKR_CRASH_TEXT_COUNT; ++i) {
    char text[VKR_CRASH_TEXT_CAPACITY];
    MemCopy(text, g_crash_text[i], sizeof(text));
    text[sizeof(text) - 1u] = '\0';
    vkr_crash_writer_text(writer, CRASH_TEXT_NAMES[i]);
    vkr_crash_writer_text(writer, ": ");
    vkr_crash_writer_text(writer, text);
    vkr_crash_writer_text(writer, "\n");
  }

  vkr_crash_writer_text(writer, "\nlog (newest records, oldest first):\n");
  if (log_history_emergency_write(VKR_CRASH_LOG_RECORDS, crash_log_sink,
                                  writer) == 0u) {
    vkr_crash_writer_text(
        writer, "none: no log history is kept, or nothing was logged\n");
  }
}
