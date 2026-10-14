#include "metal/vkr_metal_diagnostics.h"

#include "core/logger.h"

enum {
  VKR_METAL_DIAGNOSTIC_PATH_BYTES = 1024,
  VKR_METAL_DIAGNOSTIC_EVENT_BYTES = 128,
  VKR_METAL_DIAGNOSTIC_DETAIL_BYTES = 512,
};

typedef struct VkrMetalDiagnosticText {
  char *data;
  uint32_t capacity;
  uint32_t count;
  bool8_t overflowed;
} VkrMetalDiagnosticText;

/* Reports the first failure, then closes both segments and disables the sink.
 */
vkr_internal void vkr_metal_diagnostic_fail(VkrMetalDiagnostics *diagnostics,
                                            const char *operation,
                                            const char *reason) {
  if (!diagnostics->error_reported) {
    log_error("metal.diagnostics: %s: %s", operation, reason);
    diagnostics->error_reported = true_v;
  }

  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segments); ++i) {
    file_close(&diagnostics->segments[i]);
  }
  diagnostics->enabled = false_v;
}

vkr_internal void
vkr_metal_diagnostic_fail_file(VkrMetalDiagnostics *diagnostics,
                               const char *operation, FileError error) {
  const String8 reason = file_get_error_string(error);
  char text[128];
  string_format(text, sizeof(text), "%.*s", (int)reason.length,
                (const char *)reason.str);
  vkr_metal_diagnostic_fail(diagnostics, operation, text);
}

vkr_internal bool8_t vkr_metal_diagnostic_path(char *path, uint64_t path_size,
                                               const char *directory,
                                               uint32_t segment) {
  const int32_t written = string_format(
      path, path_size, "%s/metal-diagnostics.%u.jsonl", directory, segment);
  return written >= 0 && (uint64_t)written < path_size;
}

/* Removes what a failed open created: the owned segments and the directory.
 */
vkr_internal void
vkr_metal_diagnostic_discard_open(VkrMetalDiagnostics *diagnostics,
                                  const char *directory) {
  char path[VKR_METAL_DIAGNOSTIC_PATH_BYTES];
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segments); ++i) {
    const bool8_t owns_segment = diagnostics->segments[i].handle != NULL;
    file_close(&diagnostics->segments[i]);
    if (owns_segment &&
        vkr_metal_diagnostic_path(path, sizeof(path), directory, i)) {
      const FilePath segment_path = file_path_view(path);
      (void)file_remove(&segment_path);
    }
  }
  const FilePath directory_path = file_path_view(directory);
  (void)file_remove_directory(&directory_path);
}

vkr_internal void vkr_metal_diagnostic_text_append(VkrMetalDiagnosticText *text,
                                                   const char *source,
                                                   uint32_t source_count) {
  if (source_count > text->capacity - text->count) {
    text->overflowed = true_v;
    return;
  }
  MemCopy(text->data + text->count, source, source_count);
  text->count += source_count;
}

vkr_internal void vkr_metal_diagnostic_text_format(VkrMetalDiagnosticText *text,
                                                   const char *format, ...) {
  if (text->count >= text->capacity) {
    text->overflowed = true_v;
    return;
  }
  va_list args;
  va_start(args, format);
  const int written = string_format_v(
      text->data + text->count, text->capacity - text->count, format, args);
  va_end(args);
  if (written < 0 || (uint32_t)written >= text->capacity - text->count) {
    text->overflowed = true_v;
    return;
  }
  text->count += (uint32_t)written;
}

vkr_internal uint32_t vkr_metal_diagnostic_utf8_size(const char *source,
                                                     uint32_t remaining) {
  const uint8_t first = (uint8_t)source[0];
  if (first < 0xc2u || remaining < 2u) {
    return 0u;
  }
  const uint8_t second = (uint8_t)source[1];
  if ((second & 0xc0u) != 0x80u) {
    return 0u;
  }
  if (first < 0xe0u) {
    return 2u;
  }
  if (remaining < 3u) {
    return 0u;
  }
  const uint8_t third = (uint8_t)source[2];
  if ((third & 0xc0u) != 0x80u || (first == 0xe0u && second < 0xa0u) ||
      (first == 0xedu && second >= 0xa0u)) {
    return 0u;
  }
  if (first < 0xf0u) {
    return 3u;
  }
  if (first > 0xf4u || remaining < 4u || (first == 0xf0u && second < 0x90u) ||
      (first == 0xf4u && second >= 0x90u)) {
    return 0u;
  }
  const uint8_t fourth = (uint8_t)source[3];
  return (fourth & 0xc0u) == 0x80u ? 4u : 0u;
}

vkr_internal bool8_t vkr_metal_diagnostic_escape(VkrMetalDiagnosticText *text,
                                                 const char *source,
                                                 uint32_t source_limit) {
  uint32_t i = 0u;
  while (i < source_limit) {
    const uint8_t byte = (uint8_t)source[i];
    if (byte == '\0') {
      return false_v;
    }
    if (byte == '"' || byte == '\\') {
      const char escaped[] = {'\\', (char)byte};
      vkr_metal_diagnostic_text_append(text, escaped, sizeof(escaped));
    } else if (byte == '\b') {
      vkr_metal_diagnostic_text_append(text, "\\b", 2u);
    } else if (byte == '\f') {
      vkr_metal_diagnostic_text_append(text, "\\f", 2u);
    } else if (byte == '\n') {
      vkr_metal_diagnostic_text_append(text, "\\n", 2u);
    } else if (byte == '\r') {
      vkr_metal_diagnostic_text_append(text, "\\r", 2u);
    } else if (byte == '\t') {
      vkr_metal_diagnostic_text_append(text, "\\t", 2u);
    } else if (byte < 0x20u || byte > 0x7eu) {
      const uint32_t expected_utf8_size = byte >= 0xc2u && byte < 0xe0u   ? 2u
                                          : byte >= 0xe0u && byte < 0xf0u ? 3u
                                          : byte >= 0xf0u && byte < 0xf5u ? 4u
                                                                          : 0u;
      if (expected_utf8_size > source_limit - i) {
        return true_v;
      }
      /* vsnprintf can end its bounded output inside a source code point. */
      for (uint32_t continuation = 1u; continuation < expected_utf8_size;
           ++continuation) {
        if (source[i + continuation] == '\0')
          return true_v;
      }
      const uint32_t utf8_size =
          vkr_metal_diagnostic_utf8_size(source + i, source_limit - i);
      if (utf8_size > 0u) {
        vkr_metal_diagnostic_text_append(text, source + i, utf8_size);
        i += utf8_size;
        continue;
      }
      vkr_metal_diagnostic_text_format(text, "\\u%04x", (unsigned int)byte);
    } else {
      vkr_metal_diagnostic_text_append(text, (const char *)&source[i], 1u);
    }
    ++i;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metal_diagnostics_rotate(VkrMetalDiagnostics *diagnostics) {
  diagnostics->active_segment = 1u - diagnostics->active_segment;
  const FileError error =
      file_truncate(&diagnostics->segments[diagnostics->active_segment]);
  if (error != FILE_ERROR_NONE) {
    vkr_metal_diagnostic_fail_file(diagnostics, "rotate diagnostic segment",
                                   error);
    return false_v;
  }
  diagnostics->active_bytes = 0u;
  return true_v;
}

bool8_t vkr_metal_diagnostics_open(VkrMetalDiagnostics *diagnostics,
                                   const char *directory) {
  if (!diagnostics || !directory || directory[0] == '\0') {
    return false_v;
  }

  if (diagnostics->enabled) {
    vkr_metal_diagnostics_close(diagnostics);
  }
  MemZero(diagnostics, sizeof(*diagnostics));
  const FilePath directory_path = file_path_view(directory);
  FileError error = file_create_directory_exclusive(&directory_path);
  if (error != FILE_ERROR_NONE) {
    vkr_metal_diagnostic_fail_file(diagnostics, "create diagnostic directory",
                                   error);
    return false_v;
  }

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_EXCLUSIVE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  char path[VKR_METAL_DIAGNOSTIC_PATH_BYTES];
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segments); ++i) {
    if (!vkr_metal_diagnostic_path(path, sizeof(path), directory, i)) {
      vkr_metal_diagnostic_discard_open(diagnostics, directory);
      vkr_metal_diagnostic_fail(diagnostics, "form diagnostic segment path",
                                "path too long");
      return false_v;
    }
    const FilePath segment_path = file_path_view(path);
    error = file_open(&segment_path, mode, &diagnostics->segments[i]);
    if (error != FILE_ERROR_NONE) {
      vkr_metal_diagnostic_discard_open(diagnostics, directory);
      vkr_metal_diagnostic_fail_file(diagnostics, "create diagnostic segment",
                                     error);
      return false_v;
    }
  }
  diagnostics->enabled = true_v;
  return true_v;
}

void vkr_metal_diagnostics_record(VkrMetalDiagnostics *diagnostics,
                                  const char *event, uint64_t submitted,
                                  uint64_t completed, const char *format, ...) {
  if (!diagnostics || !diagnostics->enabled) {
    return;
  }

  char detail[VKR_METAL_DIAGNOSTIC_DETAIL_BYTES] = {0};
  bool8_t truncated = false_v;
  if (format) {
    va_list args;
    va_start(args, format);
    const int detail_length =
        string_format_v(detail, sizeof(detail), format, args);
    va_end(args);
    truncated = detail_length < 0 || (size_t)detail_length >= sizeof(detail);
  } else {
    MemCopy(detail, "<null format>", sizeof("<null format>"));
  }
  if (!event) {
    event = "<null event>";
  }

  char record[VKR_METAL_DIAGNOSTIC_RECORD_BYTES];
  VkrMetalDiagnosticText text = {.data = record,
                                 .capacity = sizeof(record),
                                 .count = 0u,
                                 .overflowed = false_v};
  vkr_metal_diagnostic_text_format(&text, "{\"seq\":%llu,\"event\":\"",
                                   (unsigned long long)diagnostics->sequence++);
  truncated |= vkr_metal_diagnostic_escape(&text, event,
                                           VKR_METAL_DIAGNOSTIC_EVENT_BYTES);
  vkr_metal_diagnostic_text_append(
      &text, "\",\"submitted\":", sizeof("\",\"submitted\":") - 1u);
  vkr_metal_diagnostic_text_format(
      &text, "%llu,\"completed\":%llu,\"detail\":\"",
      (unsigned long long)submitted, (unsigned long long)completed);
  truncated |= vkr_metal_diagnostic_escape(&text, detail,
                                           VKR_METAL_DIAGNOSTIC_DETAIL_BYTES);
  vkr_metal_diagnostic_text_append(&text, "\"", 1u);
  if (truncated) {
    vkr_metal_diagnostic_text_append(&text, ",\"truncated\":true",
                                     sizeof(",\"truncated\":true") - 1u);
  }
  vkr_metal_diagnostic_text_append(&text, "}\n", 2u);
  if (text.overflowed) {
    vkr_metal_diagnostic_fail(diagnostics, "format diagnostic record",
                              "record exceeds its buffer");
    return;
  }

  if (diagnostics->active_bytes + text.count >
      VKR_METAL_DIAGNOSTIC_SEGMENT_BYTES) {
    if (!vkr_metal_diagnostics_rotate(diagnostics)) {
      return;
    }
  }
  /* FileHandle writes are unbuffered, so each record reaches the kernel
     before this returns, even when the process then exits at once. */
  uint64_t written = 0u;
  const FileError error =
      file_write(&diagnostics->segments[diagnostics->active_segment],
                 text.count, (const uint8_t *)record, &written);
  if (error != FILE_ERROR_NONE || written != text.count) {
    vkr_metal_diagnostic_fail_file(
        diagnostics, "write diagnostic record",
        error != FILE_ERROR_NONE ? error : FILE_ERROR_IO_ERROR);
    return;
  }
  diagnostics->active_bytes += text.count;
}

void vkr_metal_diagnostics_flush(VkrMetalDiagnostics *diagnostics) {
  if (!diagnostics || !diagnostics->enabled) {
    return;
  }
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segments); ++i) {
    const FileError error = file_sync(&diagnostics->segments[i]);
    if (error != FILE_ERROR_NONE) {
      vkr_metal_diagnostic_fail_file(diagnostics, "flush diagnostic segment",
                                     error);
      return;
    }
  }
}

void vkr_metal_diagnostics_close(VkrMetalDiagnostics *diagnostics) {
  if (!diagnostics || !diagnostics->enabled) {
    return;
  }
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segments); ++i) {
    file_close(&diagnostics->segments[i]);
  }
  MemZero(diagnostics, sizeof(*diagnostics));
}
