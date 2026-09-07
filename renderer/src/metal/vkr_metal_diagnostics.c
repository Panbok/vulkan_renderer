#include "metal/vkr_metal_diagnostics.h"

#if defined(PLATFORM_APPLE)

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

vkr_internal void vkr_metal_diagnostic_fail(VkrMetalDiagnostics *diagnostics,
                                            const char *operation,
                                            int error_number) {
  if (!diagnostics->error_reported) {
    fprintf(stderr, "metal.diagnostics: %s: %s\n", operation,
            strerror(error_number));
    diagnostics->error_reported = true_v;
  }

  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segment_fds); ++i) {
    if (diagnostics->segment_fds[i] >= 0) {
      close(diagnostics->segment_fds[i]);
      diagnostics->segment_fds[i] = -1;
    }
  }
  diagnostics->enabled = false_v;
}

vkr_internal bool8_t vkr_metal_diagnostic_path(char *path, size_t path_size,
                                               const char *directory,
                                               uint32_t segment) {
  const int written = snprintf(path, path_size, "%s/metal-diagnostics.%u.jsonl",
                               directory, segment);
  if (written < 0 || (size_t)written >= path_size) {
    errno = ENAMETOOLONG;
    return false_v;
  }
  return true_v;
}

vkr_internal void
vkr_metal_diagnostic_discard_open(VkrMetalDiagnostics *diagnostics,
                                  const char *directory) {
  char path[VKR_METAL_DIAGNOSTIC_PATH_BYTES];
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segment_fds); ++i) {
    const bool8_t owns_segment = diagnostics->segment_fds[i] >= 0;
    if (owns_segment) {
      close(diagnostics->segment_fds[i]);
      diagnostics->segment_fds[i] = -1;
    }
    if (owns_segment &&
        vkr_metal_diagnostic_path(path, sizeof(path), directory, i)) {
      unlink(path);
    }
  }
  rmdir(directory);
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
  const int written = vsnprintf(text->data + text->count,
                                text->capacity - text->count, format, args);
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

vkr_internal bool8_t vkr_metal_diagnostic_write(int fd, const char *data,
                                                uint32_t data_size) {
  uint32_t written = 0u;
  while (written < data_size) {
    const ssize_t result = write(fd, data + written, data_size - written);
    if (result > 0) {
      written += (uint32_t)result;
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result == 0) {
      errno = EIO;
    }
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metal_diagnostics_rotate(VkrMetalDiagnostics *diagnostics) {
  diagnostics->active_segment = 1u - diagnostics->active_segment;
  const int fd = diagnostics->segment_fds[diagnostics->active_segment];
  if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
    vkr_metal_diagnostic_fail(diagnostics, "rotate diagnostic segment", errno);
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
  diagnostics->segment_fds[0] = -1;
  diagnostics->segment_fds[1] = -1;
  if (mkdir(directory, 0700) != 0) {
    vkr_metal_diagnostic_fail(diagnostics, "create diagnostic directory",
                              errno);
    return false_v;
  }

  char path[VKR_METAL_DIAGNOSTIC_PATH_BYTES];
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segment_fds); ++i) {
    if (!vkr_metal_diagnostic_path(path, sizeof(path), directory, i)) {
      const int error_number = errno;
      vkr_metal_diagnostic_discard_open(diagnostics, directory);
      vkr_metal_diagnostic_fail(diagnostics, "form diagnostic segment path",
                                error_number);
      return false_v;
    }
    diagnostics->segment_fds[i] = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (diagnostics->segment_fds[i] < 0) {
      const int error_number = errno;
      vkr_metal_diagnostic_discard_open(diagnostics, directory);
      vkr_metal_diagnostic_fail(diagnostics, "create diagnostic segment",
                                error_number);
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
    const int detail_length = vsnprintf(detail, sizeof(detail), format, args);
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
                              EOVERFLOW);
    return;
  }

  if (diagnostics->active_bytes + text.count >
      VKR_METAL_DIAGNOSTIC_SEGMENT_BYTES) {
    if (!vkr_metal_diagnostics_rotate(diagnostics)) {
      return;
    }
  }
  if (!vkr_metal_diagnostic_write(
          diagnostics->segment_fds[diagnostics->active_segment], record,
          text.count)) {
    vkr_metal_diagnostic_fail(diagnostics, "write diagnostic record", errno);
    return;
  }
  diagnostics->active_bytes += text.count;
}

void vkr_metal_diagnostics_flush(VkrMetalDiagnostics *diagnostics) {
  if (!diagnostics || !diagnostics->enabled) {
    return;
  }
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segment_fds); ++i) {
    if (fsync(diagnostics->segment_fds[i]) != 0) {
      vkr_metal_diagnostic_fail(diagnostics, "flush diagnostic segment", errno);
      return;
    }
  }
}

void vkr_metal_diagnostics_close(VkrMetalDiagnostics *diagnostics) {
  if (!diagnostics || !diagnostics->enabled) {
    return;
  }
  int error_number = 0;
  for (uint32_t i = 0u; i < ArrayCount(diagnostics->segment_fds); ++i) {
    if (diagnostics->segment_fds[i] >= 0 &&
        close(diagnostics->segment_fds[i]) != 0 && error_number == 0) {
      error_number = errno;
    }
  }
  MemZero(diagnostics, sizeof(*diagnostics));
  if (error_number != 0) {
    fprintf(stderr, "metal.diagnostics: close diagnostic segment: %s\n",
            strerror(error_number));
  }
}

#else

bool8_t vkr_metal_diagnostics_open(VkrMetalDiagnostics *diagnostics,
                                   const char *directory) {
  (void)directory;
  if (diagnostics) {
    MemZero(diagnostics, sizeof(*diagnostics));
  }
  return false_v;
}

void vkr_metal_diagnostics_record(VkrMetalDiagnostics *diagnostics,
                                  const char *event, uint64_t submitted,
                                  uint64_t completed, const char *format, ...) {
  (void)diagnostics;
  (void)event;
  (void)submitted;
  (void)completed;
  (void)format;
}

void vkr_metal_diagnostics_flush(VkrMetalDiagnostics *diagnostics) {
  (void)diagnostics;
}

void vkr_metal_diagnostics_close(VkrMetalDiagnostics *diagnostics) {
  if (diagnostics) {
    MemZero(diagnostics, sizeof(*diagnostics));
  }
}

#endif
