#include "core/vkr_json_writer.h"

#include "core/vkr_threads.h"

#if defined(PLATFORM_WINDOWS)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

vkr_internal bool8_t vkr_json_writer_emit(VkrJsonWriter *writer,
                                          const uint8_t *data,
                                          uint64_t length) {
  if (!writer || writer->failed || (!data && length > 0)) {
    return false_v;
  }
  while (length > 0) {
    const uint32_t available =
        VKR_JSON_WRITER_BUFFER_SIZE - writer->buffer_count;
    const uint32_t copy_count = (uint32_t)Min(length, (uint64_t)available);
    MemCopy(writer->buffer + writer->buffer_count, data, copy_count);
    writer->buffer_count += copy_count;
    data += copy_count;
    length -= copy_count;
    if (writer->buffer_count == VKR_JSON_WRITER_BUFFER_SIZE &&
        !vkr_json_writer_flush(writer)) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_json_writer_emit_cstr(VkrJsonWriter *writer,
                                               const char *value) {
  return vkr_json_writer_emit(writer, (const uint8_t *)value,
                              string_length(value));
}

void vkr_json_writer_init(VkrJsonWriter *writer, VkrJsonWriteSink sink,
                          void *sink_context) {
  assert(writer != NULL && sink != NULL);
  *writer = (VkrJsonWriter){
      .sink = sink,
      .sink_context = sink_context,
  };
}

bool8_t vkr_json_writer_flush(VkrJsonWriter *writer) {
  if (!writer || writer->failed) {
    return false_v;
  }
  if (writer->buffer_count == 0) {
    return true_v;
  }
  if (!writer->sink(writer->sink_context, writer->buffer,
                    writer->buffer_count)) {
    writer->failed = true_v;
    return false_v;
  }
  writer->buffer_count = 0;
  return true_v;
}

vkr_internal bool8_t vkr_json_writer_before_value(VkrJsonWriter *writer) {
  if (!writer || writer->failed) {
    return false_v;
  }
  if (writer->depth == 0) {
    if (writer->root_written) {
      writer->failed = true_v;
      return false_v;
    }
    writer->root_written = true_v;
    return true_v;
  }

  VkrJsonWriterLevel *level = &writer->levels[writer->depth - 1u];
  if (level->kind == VKR_JSON_CONTAINER_OBJECT) {
    if (!level->expecting_value) {
      writer->failed = true_v;
      return false_v;
    }
    level->expecting_value = false_v;
    level->item_count++;
    return true_v;
  }
  if (level->item_count > 0 && !vkr_json_writer_emit_cstr(writer, ",")) {
    return false_v;
  }
  level->item_count++;
  return true_v;
}

vkr_internal bool8_t vkr_json_writer_begin_container(VkrJsonWriter *writer,
                                                     VkrJsonContainerKind kind,
                                                     const char *token) {
  if (!writer || writer->depth >= VKR_JSON_WRITER_MAX_DEPTH ||
      !vkr_json_writer_before_value(writer) ||
      !vkr_json_writer_emit_cstr(writer, token)) {
    if (writer) {
      writer->failed = true_v;
    }
    return false_v;
  }
  writer->levels[writer->depth++] = (VkrJsonWriterLevel){.kind = kind};
  return true_v;
}

vkr_internal bool8_t vkr_json_writer_end_container(VkrJsonWriter *writer,
                                                   VkrJsonContainerKind kind,
                                                   const char *token) {
  if (!writer || writer->failed || writer->depth == 0) {
    return false_v;
  }
  VkrJsonWriterLevel *level = &writer->levels[writer->depth - 1u];
  if (level->kind != kind || level->expecting_value) {
    writer->failed = true_v;
    return false_v;
  }
  writer->depth--;
  return vkr_json_writer_emit_cstr(writer, token);
}

bool8_t vkr_json_writer_begin_object(VkrJsonWriter *writer) {
  return vkr_json_writer_begin_container(writer, VKR_JSON_CONTAINER_OBJECT,
                                         "{");
}

bool8_t vkr_json_writer_end_object(VkrJsonWriter *writer) {
  return vkr_json_writer_end_container(writer, VKR_JSON_CONTAINER_OBJECT, "}");
}

bool8_t vkr_json_writer_begin_array(VkrJsonWriter *writer) {
  return vkr_json_writer_begin_container(writer, VKR_JSON_CONTAINER_ARRAY, "[");
}

bool8_t vkr_json_writer_end_array(VkrJsonWriter *writer) {
  return vkr_json_writer_end_container(writer, VKR_JSON_CONTAINER_ARRAY, "]");
}

/**
 * @brief Length of the well-formed UTF-8 sequence at `offset`, or 0.
 *
 * Rejects overlong encodings, surrogate halves, values above U+10FFFF, and
 * truncated sequences. Subjects reaching this writer are asset paths that may
 * have been cut to a fixed byte budget upstream, so "looks like text" is not
 * a safe assumption.
 */
vkr_internal uint32_t vkr_json_utf8_sequence_length(const uint8_t *data,
                                                    uint64_t offset,
                                                    uint64_t length) {
  const uint8_t lead = data[offset];
  uint32_t needed = 0;
  uint32_t codepoint = 0;
  if (lead < 0x80u) {
    return 1u;
  } else if ((lead & 0xE0u) == 0xC0u) {
    needed = 1u;
    codepoint = lead & 0x1Fu;
  } else if ((lead & 0xF0u) == 0xE0u) {
    needed = 2u;
    codepoint = lead & 0x0Fu;
  } else if ((lead & 0xF8u) == 0xF0u) {
    needed = 3u;
    codepoint = lead & 0x07u;
  } else {
    return 0u;
  }
  if (offset + needed >= length) {
    return 0u; // Truncated sequence.
  }
  for (uint32_t i = 1u; i <= needed; ++i) {
    const uint8_t continuation = data[offset + i];
    if ((continuation & 0xC0u) != 0x80u) {
      return 0u;
    }
    codepoint = (codepoint << 6u) | (continuation & 0x3Fu);
  }
  const uint32_t minimum[3] = {0x80u, 0x800u, 0x10000u};
  if (codepoint < minimum[needed - 1u] || codepoint > 0x10FFFFu ||
      (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
    return 0u;
  }
  return needed + 1u;
}

vkr_internal bool8_t vkr_json_writer_escaped(VkrJsonWriter *writer,
                                             String8 value) {
  static const char hex[] = "0123456789abcdef";
  if (!vkr_json_writer_emit_cstr(writer, "\"")) {
    return false_v;
  }

  uint64_t i = 0;
  while (i < value.length) {
    // Emit the longest run that needs no transformation in one call rather
    // than paying an emit per character; paths are almost entirely such runs.
    uint64_t run_start = i;
    while (i < value.length) {
      const uint8_t c = value.str[i];
      if (c < 0x20u || c == '"' || c == '\\' || c == 0x7Fu) {
        break;
      }
      const uint32_t sequence =
          c < 0x80u ? 1u
                    : vkr_json_utf8_sequence_length(value.str, i, value.length);
      if (sequence == 0) {
        break;
      }
      i += sequence;
    }
    if (i > run_start &&
        !vkr_json_writer_emit(writer, value.str + run_start, i - run_start)) {
      return false_v;
    }
    if (i >= value.length) {
      break;
    }

    const uint8_t c = value.str[i];
    const char *escape = NULL;
    switch (c) {
    case '"':
      escape = "\\\"";
      break;
    case '\\':
      escape = "\\\\";
      break;
    case '\b':
      escape = "\\b";
      break;
    case '\f':
      escape = "\\f";
      break;
    case '\n':
      escape = "\\n";
      break;
    case '\r':
      escape = "\\r";
      break;
    case '\t':
      escape = "\\t";
      break;
    default:
      break;
    }
    if (escape) {
      if (!vkr_json_writer_emit_cstr(writer, escape)) {
        return false_v;
      }
      i++;
      continue;
    }
    if (c < 0x20u || c == 0x7Fu) {
      const uint8_t sequence[] = {'\\',
                                  'u',
                                  '0',
                                  '0',
                                  (uint8_t)hex[c >> 4u],
                                  (uint8_t)hex[c & 0x0Fu]};
      if (!vkr_json_writer_emit(writer, sequence, sizeof(sequence))) {
        return false_v;
      }
      i++;
      continue;
    }
    // Not valid UTF-8. Substitute U+FFFD rather than emitting the raw byte:
    // one unreadable character beats a report no strict parser will load.
    if (!vkr_json_writer_emit_cstr(writer, "\\ufffd")) {
      return false_v;
    }
    i++;
  }
  return vkr_json_writer_emit_cstr(writer, "\"");
}

bool8_t vkr_json_writer_name(VkrJsonWriter *writer, String8 name) {
  if (!writer || writer->failed) {
    return false_v;
  }
  // Every rejection is sticky. A writer that refused one token but still let
  // complete() succeed would publish a silently truncated document.
  if (writer->depth == 0 || !name.str) {
    writer->failed = true_v;
    return false_v;
  }
  VkrJsonWriterLevel *level = &writer->levels[writer->depth - 1u];
  if (level->kind != VKR_JSON_CONTAINER_OBJECT || level->expecting_value) {
    writer->failed = true_v;
    return false_v;
  }
  if (level->item_count > 0 && !vkr_json_writer_emit_cstr(writer, ",")) {
    return false_v;
  }
  if (!vkr_json_writer_escaped(writer, name) ||
      !vkr_json_writer_emit_cstr(writer, ":")) {
    return false_v;
  }
  level->expecting_value = true_v;
  return true_v;
}

bool8_t vkr_json_writer_string(VkrJsonWriter *writer, String8 value) {
  if (!value.str && value.length > 0) {
    if (writer) {
      writer->failed = true_v;
    }
    return false_v;
  }
  if (!vkr_json_writer_before_value(writer)) {
    return false_v;
  }
  return vkr_json_writer_escaped(writer, value);
}

vkr_internal bool8_t vkr_json_writer_number(VkrJsonWriter *writer,
                                            const char *buffer,
                                            int32_t length) {
  if (length <= 0 || !vkr_json_writer_before_value(writer)) {
    if (writer) {
      writer->failed = true_v;
    }
    return false_v;
  }
  return vkr_json_writer_emit(writer, (const uint8_t *)buffer,
                              (uint64_t)length);
}

bool8_t vkr_json_writer_u64(VkrJsonWriter *writer, uint64_t value) {
  char buffer[32];
  const int32_t length =
      snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
  return vkr_json_writer_number(writer, buffer, length);
}

bool8_t vkr_json_writer_i64(VkrJsonWriter *writer, int64_t value) {
  char buffer[32];
  const int32_t length =
      snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
  return vkr_json_writer_number(writer, buffer, length);
}

bool8_t vkr_json_writer_f64(VkrJsonWriter *writer, float64_t value) {
  if (!isfinite(value)) {
    if (writer) {
      writer->failed = true_v;
    }
    return false_v;
  }
  char buffer[32];
  const int32_t length = snprintf(buffer, sizeof(buffer), "%.17g", value);
  return vkr_json_writer_number(writer, buffer, length);
}

bool8_t vkr_json_writer_bool(VkrJsonWriter *writer, bool8_t value) {
  if (!vkr_json_writer_before_value(writer)) {
    return false_v;
  }
  return vkr_json_writer_emit_cstr(writer, value ? "true" : "false");
}

bool8_t vkr_json_writer_null(VkrJsonWriter *writer) {
  if (!vkr_json_writer_before_value(writer)) {
    return false_v;
  }
  return vkr_json_writer_emit_cstr(writer, "null");
}

bool8_t vkr_json_writer_complete(VkrJsonWriter *writer) {
  if (!writer || writer->failed || !writer->root_written ||
      writer->depth != 0) {
    return false_v;
  }
  return vkr_json_writer_flush(writer);
}

vkr_internal bool8_t vkr_json_file_sink(void *context, const uint8_t *data,
                                        uint64_t length) {
  FILE *file = context;
  return file && fwrite(data, 1u, (size_t)length, file) == length;
}

bool8_t vkr_json_file_writer_begin(VkrJsonFileWriter *file_writer,
                                   String8 final_path) {
  if (!file_writer || !final_path.str || final_path.length == 0 ||
      final_path.length > VKR_JSON_WRITER_PATH_MAX - 32u) {
    return false_v;
  }
  for (uint64_t i = 0; i < final_path.length; ++i) {
    if (final_path.str[i] == '\0') {
      return false_v;
    }
  }
  MemZero(file_writer, sizeof(*file_writer));
  MemCopy(file_writer->final_path, final_path.str, final_path.length);
  file_writer->final_path[final_path.length] = '\0';
#if defined(PLATFORM_WINDOWS)
  const unsigned long long process_id = (unsigned long long)GetCurrentProcessId();
#else
  const unsigned long long process_id = (unsigned long long)getpid();
#endif
  // Process and thread both participate: concurrent runs of the harness may
  // target the same final path, and a temp name they share would corrupt both.
  const int32_t length = snprintf(
      file_writer->temp_path, sizeof(file_writer->temp_path), "%s.tmp.%llu.%llu",
      file_writer->final_path, process_id,
      (unsigned long long)vkr_thread_current_id());
  if (length <= 0 || (uint64_t)length >= sizeof(file_writer->temp_path)) {
    return false_v;
  }
  file_writer->file = fopen(file_writer->temp_path, "wb");
  if (!file_writer->file) {
    return false_v;
  }
  file_writer->active = true_v;
  vkr_json_writer_init(&file_writer->writer, vkr_json_file_sink,
                       file_writer->file);
  return true_v;
}

bool8_t vkr_json_file_writer_commit(VkrJsonFileWriter *file_writer) {
  if (!file_writer || !file_writer->active || !file_writer->file ||
      !vkr_json_writer_complete(&file_writer->writer) ||
      fflush(file_writer->file) != 0) {
    vkr_json_file_writer_abort(file_writer);
    return false_v;
  }

#if defined(PLATFORM_WINDOWS)
  const bool8_t durable = _commit(_fileno(file_writer->file)) == 0;
#else
  const bool8_t durable = fsync(fileno(file_writer->file)) == 0;
#endif
  const bool8_t closed = fclose(file_writer->file) == 0;
  file_writer->file = NULL;
  if (!durable || !closed) {
    remove(file_writer->temp_path);
    file_writer->active = false_v;
    return false_v;
  }

#if defined(PLATFORM_WINDOWS)
  const bool8_t promoted =
      MoveFileExA(file_writer->temp_path, file_writer->final_path,
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  const bool8_t promoted =
      rename(file_writer->temp_path, file_writer->final_path) == 0;
#endif
  if (!promoted) {
    remove(file_writer->temp_path);
  }
  file_writer->active = false_v;
  return promoted;
}

void vkr_json_file_writer_abort(VkrJsonFileWriter *file_writer) {
  if (!file_writer) {
    return;
  }
  if (file_writer->file) {
    fclose(file_writer->file);
    file_writer->file = NULL;
  }
  if (file_writer->temp_path[0]) {
    remove(file_writer->temp_path);
  }
  file_writer->active = false_v;
}
