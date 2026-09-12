#include "core/vkr_json.h"

#include "containers/str.h"

#include <float.h>

VkrJsonReader vkr_json_reader_create(const uint8_t *data, uint64_t length) {
  return (VkrJsonReader){.data = data, .length = length, .pos = 0};
}

VkrJsonReader vkr_json_reader_from_string(String8 str) {
  return vkr_json_reader_create(str.str, str.length);
}

void vkr_json_reader_reset(VkrJsonReader *reader) { reader->pos = 0; }

void vkr_json_skip_whitespace(VkrJsonReader *reader) {
  while (reader->pos < reader->length) {
    uint8_t c = reader->data[reader->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      reader->pos++;
    } else {
      break;
    }
  }
}

void vkr_json_skip_to(VkrJsonReader *reader, uint8_t target) {
  while (reader->pos < reader->length && reader->data[reader->pos] != target) {
    reader->pos++;
  }
}

bool8_t vkr_json_find_field(VkrJsonReader *reader, const char *field_name) {
  uint64_t field_len = string_length(field_name);
  uint64_t saved_pos = reader->pos;

  while (reader->pos < reader->length) {
    if (reader->data[reader->pos] == '"') {
      String8 key = {0};
      if (!vkr_json_parse_string(reader, &key)) {
        break;
      }
      if (key.length == field_len &&
          MemCompare(key.str, field_name, field_len) == 0) {
        vkr_json_skip_whitespace(reader);
        if (reader->pos < reader->length && reader->data[reader->pos] == ':') {
          reader->pos++;
          vkr_json_skip_whitespace(reader);
          return true_v;
        }
      }
    } else {
      reader->pos++;
    }
  }

  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_parse_float(VkrJsonReader *reader, float32_t *out_value) {
  float64_t val = 0.0;
  if (!vkr_json_parse_double(reader, &val) || val < -FLT_MAX || val > FLT_MAX) {
    return false_v;
  }
  *out_value = (float32_t)val;
  return true_v;
}

bool8_t vkr_json_parse_double(VkrJsonReader *reader, float64_t *out_value) {
  vkr_json_skip_whitespace(reader);

  uint64_t start = reader->pos;

  if (reader->pos < reader->length &&
      (reader->data[reader->pos] == '-' || reader->data[reader->pos] == '+')) {
    reader->pos++;
  }

  while (reader->pos < reader->length) {
    uint8_t c = reader->data[reader->pos];
    if ((c >= '0' && c <= '9') || c == '.') {
      reader->pos++;
    } else if (c == 'e' || c == 'E') {
      reader->pos++;
      if (reader->pos < reader->length && (reader->data[reader->pos] == '-' ||
                                           reader->data[reader->pos] == '+')) {
        reader->pos++;
      }
      while (reader->pos < reader->length && reader->data[reader->pos] >= '0' &&
             reader->data[reader->pos] <= '9') {
        reader->pos++;
      }
      break;
    } else {
      break;
    }
  }

  if (reader->pos == start) {
    return false_v;
  }

  String8 num_str = {.str = (uint8_t *)(reader->data + start),
                     .length = reader->pos - start};
  return string8_to_f64(&num_str, out_value);
}

static bool8_t vkr_json_value_ended(const VkrJsonReader *reader) {
  if (reader->pos == reader->length) {
    return true_v;
  }
  const uint8_t next = reader->data[reader->pos];
  return next == ',' || next == ']' || next == '}' || next == ' ' ||
         next == '\t' || next == '\n' || next == '\r';
}

bool8_t vkr_json_parse_int(VkrJsonReader *reader, int32_t *out_value) {
  vkr_json_skip_whitespace(reader);
  bool8_t negative = false_v;
  if (reader->pos < reader->length && reader->data[reader->pos] == '-') {
    negative = true_v;
    reader->pos++;
  }
  const uint64_t digits_start = reader->pos;
  const uint32_t limit = negative ? (uint32_t)INT32_MAX + 1u : INT32_MAX;
  uint32_t value = 0;
  while (reader->pos < reader->length && reader->data[reader->pos] >= '0' &&
         reader->data[reader->pos] <= '9') {
    const uint32_t digit = reader->data[reader->pos] - '0';
    if (value > (limit - digit) / 10u) {
      return false_v;
    }
    value = value * 10u + digit;
    reader->pos++;
  }
  if (reader->pos == digits_start || !vkr_json_value_ended(reader)) {
    return false_v;
  }
  *out_value = negative ? (int32_t)-(int64_t)value : (int32_t)value;
  return true_v;
}

bool8_t vkr_json_parse_string(VkrJsonReader *reader, String8 *out_value) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos >= reader->length || reader->data[reader->pos] != '"') {
    return false_v;
  }

  reader->pos++; // skip opening quote
  uint64_t start = reader->pos;

  while (reader->pos < reader->length && reader->data[reader->pos] != '"') {
    if (reader->data[reader->pos] == '\\') {
      reader->pos++;
      if (reader->pos == reader->length) {
        return false_v;
      }
    }
    reader->pos++;
  }
  if (reader->pos == reader->length) {
    return false_v;
  }

  out_value->str = (uint8_t *)(reader->data + start);
  out_value->length = reader->pos - start;

  reader->pos++; // skip closing quote
  return true_v;
}

bool8_t vkr_json_parse_bool(VkrJsonReader *reader, bool8_t *out_value) {
  vkr_json_skip_whitespace(reader);
  const uint64_t remaining = reader->length - reader->pos;
  bool8_t value;
  uint32_t length;
  if (remaining >= 4 &&
      MemCompare(reader->data + reader->pos, "true", 4) == 0) {
    value = true_v;
    length = 4;
  } else if (remaining >= 5 &&
             MemCompare(reader->data + reader->pos, "false", 5) == 0) {
    value = false_v;
    length = 5;
  } else {
    return false_v;
  }
  reader->pos += length;
  if (!vkr_json_value_ended(reader)) {
    reader->pos -= length;
    return false_v;
  }
  *out_value = value;
  return true_v;
}

bool8_t vkr_json_find_array(VkrJsonReader *reader, const char *array_name) {
  if (!vkr_json_find_field(reader, array_name)) {
    return false_v;
  }
  vkr_json_skip_to(reader, '[');
  if (reader->pos < reader->length) {
    reader->pos++; // skip '['
    return true_v;
  }
  return false_v;
}

bool8_t vkr_json_next_array_element(VkrJsonReader *reader) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos >= reader->length || reader->data[reader->pos] == ']') {
    return false_v;
  }

  if (reader->data[reader->pos] == ',') {
    reader->pos++;
    vkr_json_skip_whitespace(reader);
  }

  if (reader->pos >= reader->length || reader->data[reader->pos] == ']') {
    return false_v;
  }

  return true_v;
}

bool8_t vkr_json_enter_object(VkrJsonReader *reader,
                              VkrJsonReader *out_sub_reader) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos >= reader->length || reader->data[reader->pos] != '{') {
    return false_v;
  }

  uint64_t obj_start = reader->pos;
  int brace_depth = 1;
  reader->pos++;

  while (reader->pos < reader->length && brace_depth > 0) {
    uint8_t c = reader->data[reader->pos];

    if (c == '"') {
      String8 ignored = {0};
      if (!vkr_json_parse_string(reader, &ignored)) {
        return false_v;
      }
      continue;
    }

    if (c == '{') {
      brace_depth++;
    } else if (c == '}') {
      brace_depth--;
    }
    reader->pos++;
  }

  if (brace_depth != 0) {
    return false_v;
  }
  *out_sub_reader = (VkrJsonReader){.data = reader->data + obj_start,
                                    .length = reader->pos - obj_start,
                                    .pos = 0};

  return true_v;
}

bool8_t vkr_json_get_float(VkrJsonReader *reader, const char *field_name,
                           float32_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_float(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_double(VkrJsonReader *reader, const char *field_name,
                            float64_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_double(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_int(VkrJsonReader *reader, const char *field_name,
                         int32_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_int(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_string(VkrJsonReader *reader, const char *field_name,
                            String8 *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_string(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_bool(VkrJsonReader *reader, const char *field_name,
                          bool8_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_bool(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

static bool8_t json_decode_hex4(const uint8_t *bytes, uint32_t *value) {
  *value = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint8_t c = bytes[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false_v;
    }
    *value = (*value << 4) | digit;
  }
  return true_v;
}

bool8_t vkr_json_parse_string_decoded(VkrJsonReader *reader,
                                      VkrAllocator *allocator,
                                      String8 *out_value) {
  if (!reader || !allocator || !out_value) {
    return false_v;
  }
  *out_value = (String8){0};
  VkrJsonReader cursor = *reader;
  String8 raw = {0};
  if (!vkr_json_parse_string(&cursor, &raw) || raw.length == UINT64_MAX) {
    return false_v;
  }
  uint8_t *buffer = vkr_allocator_alloc(allocator, raw.length + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return false_v;
  }
  uint64_t used = 0;
  for (uint64_t i = 0; i < raw.length;) {
    uint32_t codepoint = raw.str[i++];
    if (codepoint < 0x20) {
      goto invalid;
    }
    if (codepoint == '\\') {
      if (i >= raw.length) {
        goto invalid;
      }
      codepoint = raw.str[i++];
      switch (codepoint) {
      case '"':
      case '\\':
      case '/':
        break;
      case 'b':
        codepoint = '\b';
        break;
      case 'f':
        codepoint = '\f';
        break;
      case 'n':
        codepoint = '\n';
        break;
      case 'r':
        codepoint = '\r';
        break;
      case 't':
        codepoint = '\t';
        break;
      case 'u':
        if (raw.length - i < 4 || !json_decode_hex4(raw.str + i, &codepoint)) {
          goto invalid;
        }
        i += 4;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          uint32_t low = 0;
          if (raw.length - i < 6 || raw.str[i] != '\\' ||
              raw.str[i + 1] != 'u' ||
              !json_decode_hex4(raw.str + i + 2, &low) || low < 0xdc00 ||
              low > 0xdfff) {
            goto invalid;
          }
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
          i += 6;
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          goto invalid;
        }
        break;
      default:
        goto invalid;
      }
    } else if (codepoint >= 0x80) {
      uint32_t remaining;
      uint32_t minimum;
      if (codepoint >= 0xc2 && codepoint <= 0xdf) {
        remaining = 1;
        minimum = 0x80;
        codepoint &= 0x1f;
      } else if (codepoint >= 0xe0 && codepoint <= 0xef) {
        remaining = 2;
        minimum = 0x800;
        codepoint &= 0x0f;
      } else if (codepoint >= 0xf0 && codepoint <= 0xf4) {
        remaining = 3;
        minimum = 0x10000;
        codepoint &= 7;
      } else {
        goto invalid;
      }
      if (raw.length - i < remaining) {
        goto invalid;
      }
      for (uint32_t j = 0; j < remaining; ++j) {
        uint8_t next = raw.str[i++];
        if ((next & 0xc0) != 0x80) {
          goto invalid;
        }
        codepoint = (codepoint << 6) | (next & 0x3f);
      }
      if (codepoint < minimum || codepoint > 0x10ffff ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        goto invalid;
      }
    }
    if (codepoint == 0) {
      goto invalid;
    }
    if (codepoint < 0x80) {
      buffer[used++] = (uint8_t)codepoint;
    } else if (codepoint < 0x800) {
      buffer[used++] = (uint8_t)(0xc0 | (codepoint >> 6));
      buffer[used++] = (uint8_t)(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
      buffer[used++] = (uint8_t)(0xe0 | (codepoint >> 12));
      buffer[used++] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f));
      buffer[used++] = (uint8_t)(0x80 | (codepoint & 0x3f));
    } else {
      buffer[used++] = (uint8_t)(0xf0 | (codepoint >> 18));
      buffer[used++] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3f));
      buffer[used++] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f));
      buffer[used++] = (uint8_t)(0x80 | (codepoint & 0x3f));
    }
  }
  buffer[used] = 0;
  if (used == raw.length) {
    *out_value = (String8){.str = buffer, .length = used};
  } else {
    uint8_t *exact = vkr_allocator_alloc(allocator, used + 1,
                                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!exact) {
      goto invalid;
    }
    MemCopy(exact, buffer, used + 1);
    *out_value = (String8){.str = exact, .length = used};
    vkr_allocator_free(allocator, buffer, raw.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  *reader = cursor;
  return true_v;
invalid:
  vkr_allocator_free(allocator, buffer, raw.length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return false_v;
}
