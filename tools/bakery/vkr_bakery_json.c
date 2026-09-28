#include "vkr_bakery_json.h"

#include "vkr_bakery_buffer.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Construction
// =============================================================================

vkr_internal VkrBakeryJson *vkr_bakery_json_node(Arena *arena,
                                                 VkrBakeryJsonType type) {
  VkrBakeryJson *node = (VkrBakeryJson *)arena_alloc(
      arena, sizeof(VkrBakeryJson), ARENA_MEMORY_TAG_STRUCT);
  if (!node) {
    return NULL;
  }
  MemZero(node, sizeof(*node));
  node->type = type;
  return node;
}

vkr_internal String8 vkr_bakery_json_copy_bytes(Arena *arena,
                                                const uint8_t *bytes,
                                                uint64_t length) {
  uint8_t *copy =
      (uint8_t *)arena_alloc(arena, length + 1u, ARENA_MEMORY_TAG_STRING);
  if (!copy) {
    return (String8){0};
  }
  if (length) {
    MemCopy(copy, bytes, (size_t)length);
  }
  copy[length] = 0u;
  return (String8){.str = copy, .length = length};
}

VkrBakeryJson *vkr_bakery_json_null(Arena *arena) {
  return vkr_bakery_json_node(arena, VKR_BAKERY_JSON_NULL);
}

VkrBakeryJson *vkr_bakery_json_bool(Arena *arena, bool8_t value) {
  VkrBakeryJson *node = vkr_bakery_json_node(arena, VKR_BAKERY_JSON_BOOL);
  if (node) {
    node->boolean = value ? true_v : false_v;
  }
  return node;
}

VkrBakeryJson *vkr_bakery_json_int(Arena *arena, int64_t value) {
  VkrBakeryJson *node = vkr_bakery_json_node(arena, VKR_BAKERY_JSON_INT);
  if (node) {
    node->integer = value;
    node->number = (float64_t)value;
  }
  return node;
}

VkrBakeryJson *vkr_bakery_json_float(Arena *arena, float64_t value) {
  VkrBakeryJson *node = vkr_bakery_json_node(arena, VKR_BAKERY_JSON_FLOAT);
  if (node) {
    node->number = value;
  }
  return node;
}

VkrBakeryJson *vkr_bakery_json_string(Arena *arena, String8 value) {
  VkrBakeryJson *node = vkr_bakery_json_node(arena, VKR_BAKERY_JSON_STRING);
  if (node) {
    node->string = vkr_bakery_json_copy_bytes(arena, value.str, value.length);
    if (!node->string.str) {
      return NULL;
    }
  }
  return node;
}

VkrBakeryJson *vkr_bakery_json_cstr(Arena *arena, const char *value) {
  return vkr_bakery_json_string(
      arena,
      (String8){.str = (uint8_t *)value, .length = value ? strlen(value) : 0u});
}

VkrBakeryJson *vkr_bakery_json_array(Arena *arena) {
  return vkr_bakery_json_node(arena, VKR_BAKERY_JSON_ARRAY);
}

VkrBakeryJson *vkr_bakery_json_object(Arena *arena) {
  return vkr_bakery_json_node(arena, VKR_BAKERY_JSON_OBJECT);
}

VkrBakeryJson *vkr_bakery_json_clone(Arena *arena, const VkrBakeryJson *value) {
  if (!value) {
    return NULL;
  }
  VkrBakeryJson *copy = vkr_bakery_json_node(arena, value->type);
  if (!copy) {
    return NULL;
  }
  copy->boolean = value->boolean;
  copy->integer = value->integer;
  copy->number = value->number;
  if (value->type == VKR_BAKERY_JSON_STRING) {
    copy->string = vkr_bakery_json_copy_bytes(arena, value->string.str,
                                              value->string.length);
  }
  for (const VkrBakeryJson *child = value->first; child; child = child->next) {
    VkrBakeryJson *child_copy = vkr_bakery_json_clone(arena, child);
    if (!child_copy) {
      return NULL;
    }
    if (child->key.str) {
      child_copy->key =
          vkr_bakery_json_copy_bytes(arena, child->key.str, child->key.length);
    }
    vkr_bakery_json_append(copy, child_copy);
  }
  return copy;
}

void vkr_bakery_json_append(VkrBakeryJson *array, VkrBakeryJson *value) {
  if (!array || !value) {
    return;
  }
  value->next = NULL;
  if (array->last) {
    array->last->next = value;
  } else {
    array->first = value;
  }
  array->last = value;
  array->count += 1u;
}

vkr_internal bool8_t vkr_bakery_json_key_equals(String8 key, const char *text) {
  const uint64_t length = strlen(text);
  return key.length == length && MemCompare(key.str, text, length) == 0;
}

void vkr_bakery_json_set(Arena *arena, VkrBakeryJson *object, const char *key,
                         VkrBakeryJson *value) {
  if (!object || object->type != VKR_BAKERY_JSON_OBJECT || !value) {
    return;
  }
  VkrBakeryJson *previous = NULL;
  for (VkrBakeryJson *child = object->first; child; child = child->next) {
    if (vkr_bakery_json_key_equals(child->key, key)) {
      /* Python dicts keep the original position of a replaced key. */
      value->key = child->key;
      value->next = child->next;
      if (previous) {
        previous->next = value;
      } else {
        object->first = value;
      }
      if (object->last == child) {
        object->last = value;
      }
      return;
    }
    previous = child;
  }
  value->key =
      vkr_bakery_json_copy_bytes(arena, (const uint8_t *)key, strlen(key));
  vkr_bakery_json_append(object, value);
}

bool8_t vkr_bakery_json_remove(VkrBakeryJson *object, const char *key) {
  if (!object || object->type != VKR_BAKERY_JSON_OBJECT) {
    return false_v;
  }
  VkrBakeryJson *previous = NULL;
  for (VkrBakeryJson *child = object->first; child; child = child->next) {
    if (vkr_bakery_json_key_equals(child->key, key)) {
      if (previous) {
        previous->next = child->next;
      } else {
        object->first = child->next;
      }
      if (object->last == child) {
        object->last = previous;
      }
      object->count -= 1u;
      return true_v;
    }
    previous = child;
  }
  return false_v;
}

// =============================================================================
// Queries
// =============================================================================

VkrBakeryJson *vkr_bakery_json_get(const VkrBakeryJson *object,
                                   const char *key) {
  if (!object || object->type != VKR_BAKERY_JSON_OBJECT) {
    return NULL;
  }
  for (VkrBakeryJson *child = object->first; child; child = child->next) {
    if (vkr_bakery_json_key_equals(child->key, key)) {
      return child;
    }
  }
  return NULL;
}

VkrBakeryJson *vkr_bakery_json_at(const VkrBakeryJson *array, uint32_t index) {
  if (!array || array->type != VKR_BAKERY_JSON_ARRAY) {
    return NULL;
  }
  VkrBakeryJson *child = array->first;
  for (uint32_t i = 0u; child && i < index; ++i) {
    child = child->next;
  }
  return child;
}

bool8_t vkr_bakery_json_get_string(const VkrBakeryJson *object, const char *key,
                                   String8 *out_value) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  if (!value || value->type != VKR_BAKERY_JSON_STRING) {
    return false_v;
  }
  *out_value = value->string;
  return true_v;
}

bool8_t vkr_bakery_json_get_int(const VkrBakeryJson *object, const char *key,
                                int64_t *out_value) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  if (!value || value->type != VKR_BAKERY_JSON_INT) {
    return false_v;
  }
  *out_value = value->integer;
  return true_v;
}

bool8_t vkr_bakery_json_get_number(const VkrBakeryJson *object, const char *key,
                                   float64_t *out_value) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  if (!value || (value->type != VKR_BAKERY_JSON_INT &&
                 value->type != VKR_BAKERY_JSON_FLOAT)) {
    return false_v;
  }
  *out_value = value->number;
  return true_v;
}

bool8_t vkr_bakery_json_get_bool(const VkrBakeryJson *object, const char *key,
                                 bool8_t *out_value) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  if (!value || value->type != VKR_BAKERY_JSON_BOOL) {
    return false_v;
  }
  *out_value = value->boolean;
  return true_v;
}

bool8_t vkr_bakery_json_is_string(const VkrBakeryJson *value,
                                  const char *text) {
  return value && value->type == VKR_BAKERY_JSON_STRING &&
         vkr_bakery_json_key_equals(value->string, text);
}

const char *vkr_bakery_json_cstr_value(Arena *arena,
                                       const VkrBakeryJson *value) {
  if (!value || value->type != VKR_BAKERY_JSON_STRING) {
    return NULL;
  }
  /* Parsed and constructed strings are already NUL-terminated copies. */
  if (value->string.str && value->string.str[value->string.length] == 0u) {
    return (const char *)value->string.str;
  }
  return (const char *)vkr_bakery_json_copy_bytes(arena, value->string.str,
                                                  value->string.length)
      .str;
}

bool8_t vkr_bakery_json_equal(const VkrBakeryJson *lhs,
                              const VkrBakeryJson *rhs) {
  if (!lhs || !rhs) {
    return lhs == rhs;
  }
  const bool8_t lhs_number =
      lhs->type == VKR_BAKERY_JSON_INT || lhs->type == VKR_BAKERY_JSON_FLOAT;
  const bool8_t rhs_number =
      rhs->type == VKR_BAKERY_JSON_INT || rhs->type == VKR_BAKERY_JSON_FLOAT;
  if (lhs_number && rhs_number) {
    /* Python compares 1 == 1.0 as equal. */
    if (lhs->type == VKR_BAKERY_JSON_INT && rhs->type == VKR_BAKERY_JSON_INT) {
      return lhs->integer == rhs->integer;
    }
    return lhs->number == rhs->number;
  }
  if (lhs->type != rhs->type) {
    return false_v;
  }
  switch (lhs->type) {
  case VKR_BAKERY_JSON_NULL:
    return true_v;
  case VKR_BAKERY_JSON_BOOL:
    return lhs->boolean == rhs->boolean;
  case VKR_BAKERY_JSON_STRING:
    return lhs->string.length == rhs->string.length &&
           MemCompare(lhs->string.str, rhs->string.str,
                      (size_t)lhs->string.length) == 0;
  case VKR_BAKERY_JSON_ARRAY: {
    if (lhs->count != rhs->count) {
      return false_v;
    }
    const VkrBakeryJson *b = rhs->first;
    for (const VkrBakeryJson *a = lhs->first; a; a = a->next, b = b->next) {
      if (!vkr_bakery_json_equal(a, b)) {
        return false_v;
      }
    }
    return true_v;
  }
  case VKR_BAKERY_JSON_OBJECT: {
    if (lhs->count != rhs->count) {
      return false_v;
    }
    for (const VkrBakeryJson *a = lhs->first; a; a = a->next) {
      const VkrBakeryJson *b = rhs->first;
      while (b && !(b->key.length == a->key.length &&
                    MemCompare(b->key.str, a->key.str, (size_t)a->key.length) ==
                        0)) {
        b = b->next;
      }
      if (!b || !vkr_bakery_json_equal(a, b)) {
        return false_v;
      }
    }
    return true_v;
  }
  default:
    return false_v;
  }
}

// =============================================================================
// Parsing
// =============================================================================

typedef struct VkrBakeryJsonParser {
  Arena *arena;
  const uint8_t *data;
  uint64_t length;
  uint64_t cursor;
  uint32_t max_depth;
  uint32_t flags;
  VkrBakeryJsonError *error;
  VkrBakeryBuffer scratch;
} VkrBakeryJsonParser;

vkr_internal void vkr_bakery_json_fail(VkrBakeryJsonParser *parser,
                                       const char *message) {
  if (!parser->error || parser->error->message[0]) {
    return;
  }
  uint32_t line = 1u;
  uint32_t column = 1u;
  for (uint64_t i = 0u; i < parser->cursor && i < parser->length; ++i) {
    if (parser->data[i] == '\n') {
      line += 1u;
      column = 1u;
    } else {
      column += 1u;
    }
  }
  parser->error->offset = parser->cursor;
  parser->error->line = line;
  parser->error->column = column;
  (void)snprintf(parser->error->message, sizeof(parser->error->message), "%s",
                 message);
}

vkr_internal void vkr_bakery_json_skip_space(VkrBakeryJsonParser *parser) {
  while (parser->cursor < parser->length) {
    const uint8_t c = parser->data[parser->cursor];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    parser->cursor += 1u;
  }
}

vkr_internal bool8_t vkr_bakery_json_literal(VkrBakeryJsonParser *parser,
                                             const char *literal) {
  const uint64_t length = strlen(literal);
  if (parser->length - parser->cursor < length ||
      MemCompare(parser->data + parser->cursor, literal, length) != 0) {
    vkr_bakery_json_fail(parser, "invalid literal");
    return false_v;
  }
  parser->cursor += length;
  return true_v;
}

vkr_internal int32_t vkr_bakery_json_hex4(VkrBakeryJsonParser *parser) {
  if (parser->length - parser->cursor < 4u) {
    return -1;
  }
  int32_t value = 0;
  for (uint32_t i = 0u; i < 4u; ++i) {
    const uint8_t c = parser->data[parser->cursor + i];
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      value |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      value |= c - 'A' + 10;
    } else {
      return -1;
    }
  }
  parser->cursor += 4u;
  return value;
}

vkr_internal void vkr_bakery_json_append_utf8(VkrBakeryBuffer *buffer,
                                              uint32_t codepoint) {
  uint8_t bytes[4];
  uint32_t count;
  if (codepoint < 0x80u) {
    bytes[0] = (uint8_t)codepoint;
    count = 1u;
  } else if (codepoint < 0x800u) {
    bytes[0] = (uint8_t)(0xC0u | (codepoint >> 6));
    bytes[1] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    count = 2u;
  } else if (codepoint < 0x10000u) {
    bytes[0] = (uint8_t)(0xE0u | (codepoint >> 12));
    bytes[1] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
    bytes[2] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    count = 3u;
  } else {
    bytes[0] = (uint8_t)(0xF0u | (codepoint >> 18));
    bytes[1] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3Fu));
    bytes[2] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
    bytes[3] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    count = 4u;
  }
  vkr_bakery_buffer_append(buffer, bytes, count);
}

vkr_internal bool8_t vkr_bakery_json_parse_string(VkrBakeryJsonParser *parser,
                                                  String8 *out_value) {
  parser->cursor += 1u; /* Opening quote. */
  vkr_bakery_buffer_reset(&parser->scratch);
  uint64_t run_start = parser->cursor;
  while (parser->cursor < parser->length) {
    const uint8_t c = parser->data[parser->cursor];
    if (c == '"') {
      vkr_bakery_buffer_append(&parser->scratch, parser->data + run_start,
                               parser->cursor - run_start);
      parser->cursor += 1u;
      if (parser->scratch.failed) {
        vkr_bakery_json_fail(parser, "out of memory");
        return false_v;
      }
      *out_value = vkr_bakery_json_copy_bytes(
          parser->arena,
          parser->scratch.data ? parser->scratch.data : (const uint8_t *)"",
          parser->scratch.length);
      return out_value->str != NULL;
    }
    if (c < 0x20u) {
      vkr_bakery_json_fail(parser, "control character in string");
      return false_v;
    }
    if (c != '\\') {
      parser->cursor += 1u;
      continue;
    }
    vkr_bakery_buffer_append(&parser->scratch, parser->data + run_start,
                             parser->cursor - run_start);
    parser->cursor += 1u;
    if (parser->cursor >= parser->length) {
      break;
    }
    const uint8_t escape = parser->data[parser->cursor++];
    char decoded = 0;
    switch (escape) {
    case '"':
      decoded = '"';
      break;
    case '\\':
      decoded = '\\';
      break;
    case '/':
      decoded = '/';
      break;
    case 'b':
      decoded = '\b';
      break;
    case 'f':
      decoded = '\f';
      break;
    case 'n':
      decoded = '\n';
      break;
    case 'r':
      decoded = '\r';
      break;
    case 't':
      decoded = '\t';
      break;
    case 'u': {
      int32_t unit = vkr_bakery_json_hex4(parser);
      if (unit < 0) {
        vkr_bakery_json_fail(parser, "invalid unicode escape");
        return false_v;
      }
      uint32_t codepoint = (uint32_t)unit;
      if (codepoint >= 0xD800u && codepoint <= 0xDBFFu &&
          parser->length - parser->cursor >= 6u &&
          parser->data[parser->cursor] == '\\' &&
          parser->data[parser->cursor + 1u] == 'u') {
        const uint64_t saved = parser->cursor;
        parser->cursor += 2u;
        const int32_t low = vkr_bakery_json_hex4(parser);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) +
                      ((uint32_t)low - 0xDC00u);
        } else {
          parser->cursor = saved;
        }
      }
      vkr_bakery_json_append_utf8(&parser->scratch, codepoint);
      run_start = parser->cursor;
      continue;
    }
    default:
      vkr_bakery_json_fail(parser, "invalid escape");
      return false_v;
    }
    vkr_bakery_buffer_append(&parser->scratch, &decoded, 1u);
    run_start = parser->cursor;
  }
  vkr_bakery_json_fail(parser, "unterminated string");
  return false_v;
}

vkr_internal VkrBakeryJson *
vkr_bakery_json_parse_number(VkrBakeryJsonParser *parser) {
  const uint64_t start = parser->cursor;
  bool8_t is_float = false_v;
  if (parser->data[parser->cursor] == '-') {
    parser->cursor += 1u;
  }
  const uint64_t digits_start = parser->cursor;
  while (parser->cursor < parser->length &&
         parser->data[parser->cursor] >= '0' &&
         parser->data[parser->cursor] <= '9') {
    parser->cursor += 1u;
  }
  if (parser->cursor == digits_start || (parser->data[digits_start] == '0' &&
                                         parser->cursor - digits_start > 1u)) {
    vkr_bakery_json_fail(parser, "invalid number");
    return NULL;
  }
  if (parser->cursor < parser->length && parser->data[parser->cursor] == '.') {
    is_float = true_v;
    parser->cursor += 1u;
    const uint64_t fraction_start = parser->cursor;
    while (parser->cursor < parser->length &&
           parser->data[parser->cursor] >= '0' &&
           parser->data[parser->cursor] <= '9') {
      parser->cursor += 1u;
    }
    if (parser->cursor == fraction_start) {
      vkr_bakery_json_fail(parser, "invalid number");
      return NULL;
    }
  }
  if (parser->cursor < parser->length &&
      (parser->data[parser->cursor] == 'e' ||
       parser->data[parser->cursor] == 'E')) {
    is_float = true_v;
    parser->cursor += 1u;
    if (parser->cursor < parser->length &&
        (parser->data[parser->cursor] == '+' ||
         parser->data[parser->cursor] == '-')) {
      parser->cursor += 1u;
    }
    const uint64_t exponent_start = parser->cursor;
    while (parser->cursor < parser->length &&
           parser->data[parser->cursor] >= '0' &&
           parser->data[parser->cursor] <= '9') {
      parser->cursor += 1u;
    }
    if (parser->cursor == exponent_start) {
      vkr_bakery_json_fail(parser, "invalid number");
      return NULL;
    }
  }
  char text[128];
  const uint64_t length = parser->cursor - start;
  if (length >= sizeof(text)) {
    vkr_bakery_json_fail(parser, "number too long");
    return NULL;
  }
  MemCopy(text, parser->data + start, (size_t)length);
  text[length] = 0;
  if (!is_float) {
    char *end = NULL;
    const long long value = strtoll(text, &end, 10);
    /* Out-of-range integers keep their magnitude as floats. */
    if (end && *end == 0 && !(value == LLONG_MAX || value == LLONG_MIN)) {
      return vkr_bakery_json_int(parser->arena, (int64_t)value);
    }
  }
  const float64_t value = strtod(text, NULL);
  if (!isfinite(value)) {
    vkr_bakery_json_fail(parser, "number out of range");
    return NULL;
  }
  return vkr_bakery_json_float(parser->arena, value);
}

vkr_internal VkrBakeryJson *
vkr_bakery_json_parse_value(VkrBakeryJsonParser *parser, uint32_t depth) {
  vkr_bakery_json_skip_space(parser);
  if (parser->cursor >= parser->length) {
    vkr_bakery_json_fail(parser, "unexpected end of input");
    return NULL;
  }
  const uint8_t c = parser->data[parser->cursor];
  if (c == '{' || c == '[') {
    if (depth >= parser->max_depth) {
      vkr_bakery_json_fail(parser, "nesting too deep");
      return NULL;
    }
    const bool8_t is_object = c == '{';
    VkrBakeryJson *container = is_object ? vkr_bakery_json_object(parser->arena)
                                         : vkr_bakery_json_array(parser->arena);
    if (!container) {
      vkr_bakery_json_fail(parser, "out of memory");
      return NULL;
    }
    parser->cursor += 1u;
    vkr_bakery_json_skip_space(parser);
    const uint8_t close = is_object ? '}' : ']';
    if (parser->cursor < parser->length &&
        parser->data[parser->cursor] == close) {
      parser->cursor += 1u;
      return container;
    }
    for (;;) {
      String8 key = {0};
      if (is_object) {
        vkr_bakery_json_skip_space(parser);
        if (parser->cursor >= parser->length ||
            parser->data[parser->cursor] != '"') {
          vkr_bakery_json_fail(parser, "expected member name");
          return NULL;
        }
        if (!vkr_bakery_json_parse_string(parser, &key)) {
          return NULL;
        }
        vkr_bakery_json_skip_space(parser);
        if (parser->cursor >= parser->length ||
            parser->data[parser->cursor] != ':') {
          vkr_bakery_json_fail(parser, "expected ':'");
          return NULL;
        }
        parser->cursor += 1u;
      }
      VkrBakeryJson *value = vkr_bakery_json_parse_value(parser, depth + 1u);
      if (!value) {
        return NULL;
      }
      if (is_object) {
        /* Duplicate names keep the last value, as Python json does, unless
           the caller rejects them. */
        if (vkr_bakery_json_get(container, (const char *)key.str)) {
          if (parser->flags & VKR_BAKERY_JSON_REJECT_DUPLICATES) {
            vkr_bakery_json_fail(parser, "duplicate JSON member");
            return NULL;
          }
          vkr_bakery_json_set(parser->arena, container, (const char *)key.str,
                              value);
        } else {
          value->key = key;
          vkr_bakery_json_append(container, value);
        }
      } else {
        vkr_bakery_json_append(container, value);
      }
      vkr_bakery_json_skip_space(parser);
      if (parser->cursor >= parser->length) {
        vkr_bakery_json_fail(parser, "unexpected end of input");
        return NULL;
      }
      const uint8_t separator = parser->data[parser->cursor++];
      if (separator == close) {
        return container;
      }
      if (separator != ',') {
        vkr_bakery_json_fail(parser, "expected ',' or closing bracket");
        return NULL;
      }
    }
  }
  if (c == '"') {
    String8 value = {0};
    if (!vkr_bakery_json_parse_string(parser, &value)) {
      return NULL;
    }
    VkrBakeryJson *node =
        vkr_bakery_json_node(parser->arena, VKR_BAKERY_JSON_STRING);
    if (node) {
      node->string = value;
    }
    return node;
  }
  if (c == 't') {
    return vkr_bakery_json_literal(parser, "true")
               ? vkr_bakery_json_bool(parser->arena, true_v)
               : NULL;
  }
  if (c == 'f') {
    return vkr_bakery_json_literal(parser, "false")
               ? vkr_bakery_json_bool(parser->arena, false_v)
               : NULL;
  }
  if (c == 'n') {
    return vkr_bakery_json_literal(parser, "null")
               ? vkr_bakery_json_null(parser->arena)
               : NULL;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    return vkr_bakery_json_parse_number(parser);
  }
  vkr_bakery_json_fail(parser, "unexpected character");
  return NULL;
}

VkrBakeryJson *vkr_bakery_json_parse(Arena *arena, const uint8_t *data,
                                     uint64_t length, uint32_t max_depth,
                                     VkrBakeryJsonError *out_error) {
  return vkr_bakery_json_parse_ex(arena, data, length, max_depth, 0u,
                                  out_error);
}

VkrBakeryJson *vkr_bakery_json_parse_ex(Arena *arena, const uint8_t *data,
                                        uint64_t length, uint32_t max_depth,
                                        uint32_t flags,
                                        VkrBakeryJsonError *out_error) {
  if (out_error) {
    MemZero(out_error, sizeof(*out_error));
  }
  VkrBakeryJsonParser parser = {
      .arena = arena,
      .data = data,
      .length = length,
      .max_depth = max_depth ? max_depth : 256u,
      .flags = flags,
      .error = out_error,
  };
  /* A UTF-8 byte order mark is not JSON; Python rejects it as well. */
  VkrBakeryJson *root = vkr_bakery_json_parse_value(&parser, 0u);
  if (root) {
    vkr_bakery_json_skip_space(&parser);
    if (parser.cursor != parser.length) {
      vkr_bakery_json_fail(&parser, "trailing data");
      root = NULL;
    }
  }
  vkr_bakery_buffer_free(&parser.scratch);
  return root;
}

// =============================================================================
// Serialization
// =============================================================================

uint32_t vkr_bakery_json_format_float(float64_t value, char *out,
                                      uint32_t capacity) {
  if (!isfinite(value) || capacity < 32u) {
    return 0u;
  }
  if (value == 0.0) {
    return (uint32_t)snprintf(out, capacity, "%s",
                              signbit(value) ? "-0.0" : "0.0");
  }
  /* Shortest round-trip digits, then Python's repr layout: fixed notation
   * for decimal exponents in [-5, 16), scientific otherwise. */
  char scientific[40];
  int precision = 1;
  for (; precision <= 17; ++precision) {
    (void)snprintf(scientific, sizeof(scientific), "%.*e", precision - 1,
                   value);
    if (strtod(scientific, NULL) == value) {
      break;
    }
  }
  char digits[24];
  uint32_t digit_count = 0u;
  const char *cursor = scientific;
  const bool8_t negative = *cursor == '-';
  if (negative) {
    cursor += 1;
  }
  while (*cursor && *cursor != 'e') {
    if (*cursor >= '0' && *cursor <= '9') {
      digits[digit_count++] = *cursor;
    }
    cursor += 1;
  }
  const int exponent = *cursor == 'e' ? atoi(cursor + 1) : 0;
  while (digit_count > 1u && digits[digit_count - 1u] == '0') {
    digit_count -= 1u;
  }
  uint32_t length = 0u;
  if (negative) {
    out[length++] = '-';
  }
  if (exponent < -4 || exponent >= 16) {
    out[length++] = digits[0];
    if (digit_count > 1u) {
      out[length++] = '.';
      for (uint32_t i = 1u; i < digit_count; ++i) {
        out[length++] = digits[i];
      }
    }
    length += (uint32_t)snprintf(out + length, capacity - length, "e%c%02d",
                                 exponent < 0 ? '-' : '+',
                                 exponent < 0 ? -exponent : exponent);
    return length;
  }
  if (exponent < 0) {
    out[length++] = '0';
    out[length++] = '.';
    for (int i = -1; i > exponent; --i) {
      out[length++] = '0';
    }
    for (uint32_t i = 0u; i < digit_count; ++i) {
      out[length++] = digits[i];
    }
  } else {
    for (int i = 0; i <= exponent; ++i) {
      out[length++] = (uint32_t)i < digit_count ? digits[i] : '0';
    }
    out[length++] = '.';
    if ((uint32_t)exponent + 1u < digit_count) {
      for (uint32_t i = (uint32_t)exponent + 1u; i < digit_count; ++i) {
        out[length++] = digits[i];
      }
    } else {
      out[length++] = '0';
    }
  }
  out[length] = 0;
  return length;
}

/* Python ensure_ascii: code points above 0x7F as \uXXXX, astral ones as
 * surrogate pairs, lowercase hex. */
vkr_internal void vkr_bakery_json_write_ascii_string(VkrBakeryBuffer *buffer,
                                                     String8 value) {
  static const char hex[] = "0123456789abcdef";
  vkr_bakery_buffer_append(buffer, "\"", 1u);
  for (uint64_t i = 0u; i < value.length;) {
    const uint8_t c = value.str[i];
    uint32_t codepoint = c;
    uint32_t width = 1u;
    if (c >= 0xF0u && i + 3u < value.length) {
      codepoint = ((uint32_t)(c & 0x07u) << 18) |
                  ((uint32_t)(value.str[i + 1u] & 0x3Fu) << 12) |
                  ((uint32_t)(value.str[i + 2u] & 0x3Fu) << 6) |
                  (uint32_t)(value.str[i + 3u] & 0x3Fu);
      width = 4u;
    } else if (c >= 0xE0u && i + 2u < value.length) {
      codepoint = ((uint32_t)(c & 0x0Fu) << 12) |
                  ((uint32_t)(value.str[i + 1u] & 0x3Fu) << 6) |
                  (uint32_t)(value.str[i + 2u] & 0x3Fu);
      width = 3u;
    } else if (c >= 0xC0u && i + 1u < value.length) {
      codepoint =
          ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(value.str[i + 1u] & 0x3Fu);
      width = 2u;
    }
    i += width;
    if (codepoint >= 0x20u && codepoint < 0x7Fu && codepoint != '"' &&
        codepoint != '\\') {
      const char plain = (char)codepoint;
      vkr_bakery_buffer_append(buffer, &plain, 1u);
      continue;
    }
    const char *named = codepoint == '"'    ? "\\\""
                        : codepoint == '\\' ? "\\\\"
                        : codepoint == '\n' ? "\\n"
                        : codepoint == '\r' ? "\\r"
                        : codepoint == '\t' ? "\\t"
                        : codepoint == '\b' ? "\\b"
                        : codepoint == '\f' ? "\\f"
                                            : NULL;
    if (named) {
      vkr_bakery_buffer_append_cstr(buffer, named);
      continue;
    }
    uint32_t units[2] = {codepoint, 0u};
    uint32_t unit_count = 1u;
    if (codepoint >= 0x10000u) {
      const uint32_t offset = codepoint - 0x10000u;
      units[0] = 0xD800u + (offset >> 10);
      units[1] = 0xDC00u + (offset & 0x3FFu);
      unit_count = 2u;
    }
    for (uint32_t u = 0u; u < unit_count; ++u) {
      const char escape[6] = {'\\',
                              'u',
                              hex[(units[u] >> 12) & 15u],
                              hex[(units[u] >> 8) & 15u],
                              hex[(units[u] >> 4) & 15u],
                              hex[units[u] & 15u]};
      vkr_bakery_buffer_append(buffer, escape, 6u);
    }
  }
  vkr_bakery_buffer_append(buffer, "\"", 1u);
}

void vkr_bakery_json_write_string(VkrBakeryBuffer *buffer, String8 value) {
  static const char hex[] = "0123456789abcdef";
  vkr_bakery_buffer_append(buffer, "\"", 1u);
  uint64_t run_start = 0u;
  for (uint64_t i = 0u; i < value.length; ++i) {
    const uint8_t c = value.str[i];
    if (c >= 0x20u && c != '"' && c != '\\') {
      continue;
    }
    vkr_bakery_buffer_append(buffer, value.str + run_start, i - run_start);
    run_start = i + 1u;
    switch (c) {
    case '"':
      vkr_bakery_buffer_append(buffer, "\\\"", 2u);
      break;
    case '\\':
      vkr_bakery_buffer_append(buffer, "\\\\", 2u);
      break;
    case '\b':
      vkr_bakery_buffer_append(buffer, "\\b", 2u);
      break;
    case '\f':
      vkr_bakery_buffer_append(buffer, "\\f", 2u);
      break;
    case '\n':
      vkr_bakery_buffer_append(buffer, "\\n", 2u);
      break;
    case '\r':
      vkr_bakery_buffer_append(buffer, "\\r", 2u);
      break;
    case '\t':
      vkr_bakery_buffer_append(buffer, "\\t", 2u);
      break;
    default: {
      const char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15u]};
      vkr_bakery_buffer_append(buffer, escape, 6u);
      break;
    }
    }
  }
  vkr_bakery_buffer_append(buffer, value.str + run_start,
                           value.length - run_start);
  vkr_bakery_buffer_append(buffer, "\"", 1u);
}

vkr_internal void vkr_bakery_json_indent(VkrBakeryBuffer *buffer,
                                         uint32_t depth) {
  vkr_bakery_buffer_append(buffer, "\n", 1u);
  for (uint32_t i = 0u; i < depth; ++i) {
    vkr_bakery_buffer_append(buffer, "  ", 2u);
  }
}

vkr_internal int vkr_bakery_json_compare_keys(const void *lhs,
                                              const void *rhs) {
  const VkrBakeryJson *a = *(const VkrBakeryJson *const *)lhs;
  const VkrBakeryJson *b = *(const VkrBakeryJson *const *)rhs;
  const uint64_t shared = Min(a->key.length, b->key.length);
  const int order = MemCompare(a->key.str, b->key.str, (size_t)shared);
  if (order != 0) {
    return order;
  }
  return a->key.length < b->key.length ? -1 : (a->key.length > b->key.length);
}

vkr_internal bool8_t vkr_bakery_json_write_value(VkrBakeryBuffer *buffer,
                                                 const VkrBakeryJson *value,
                                                 VkrBakeryJsonStyle style,
                                                 uint32_t depth) {
  switch (value->type) {
  case VKR_BAKERY_JSON_NULL:
    vkr_bakery_buffer_append(buffer, "null", 4u);
    return true_v;
  case VKR_BAKERY_JSON_BOOL:
    vkr_bakery_buffer_append_cstr(buffer, value->boolean ? "true" : "false");
    return true_v;
  case VKR_BAKERY_JSON_INT:
    vkr_bakery_buffer_appendf(buffer, "%lld", (long long)value->integer);
    return true_v;
  case VKR_BAKERY_JSON_FLOAT: {
    char text[48];
    const uint32_t length =
        vkr_bakery_json_format_float(value->number, text, sizeof(text));
    if (!length) {
      return false_v;
    }
    vkr_bakery_buffer_append(buffer, text, length);
    return true_v;
  }
  case VKR_BAKERY_JSON_STRING:
    if (style == VKR_BAKERY_JSON_PYTHON_SORTED ||
        style == VKR_BAKERY_JSON_PYTHON_COMPACT) {
      vkr_bakery_json_write_ascii_string(buffer, value->string);
    } else {
      vkr_bakery_json_write_string(buffer, value->string);
    }
    return true_v;
  case VKR_BAKERY_JSON_ARRAY:
  case VKR_BAKERY_JSON_OBJECT: {
    const bool8_t is_object = value->type == VKR_BAKERY_JSON_OBJECT;
    vkr_bakery_buffer_append(buffer, is_object ? "{" : "[", 1u);
    if (!value->count) {
      vkr_bakery_buffer_append(buffer, is_object ? "}" : "]", 1u);
      return true_v;
    }
    const VkrBakeryJson **members = NULL;
    const bool8_t sorted = style == VKR_BAKERY_JSON_CANONICAL ||
                           style == VKR_BAKERY_JSON_PYTHON_SORTED;
    if (is_object && sorted) {
      members = (const VkrBakeryJson **)malloc(sizeof(*members) *
                                               (size_t)value->count);
      if (!members) {
        return false_v;
      }
      uint32_t index = 0u;
      for (const VkrBakeryJson *child = value->first; child;
           child = child->next) {
        members[index++] = child;
      }
      qsort(members, value->count, sizeof(*members),
            vkr_bakery_json_compare_keys);
    }
    const VkrBakeryJson *child = value->first;
    bool8_t ok = true_v;
    for (uint32_t i = 0u; i < value->count && ok; ++i) {
      const VkrBakeryJson *item = members ? members[i] : child;
      child = child ? child->next : NULL;
      if (i) {
        vkr_bakery_buffer_append_cstr(
            buffer, style == VKR_BAKERY_JSON_PYTHON_SORTED ? ", " : ",");
      }
      if (style == VKR_BAKERY_JSON_PRETTY) {
        vkr_bakery_json_indent(buffer, depth + 1u);
      }
      if (is_object) {
        if (style == VKR_BAKERY_JSON_PYTHON_SORTED ||
            style == VKR_BAKERY_JSON_PYTHON_COMPACT) {
          vkr_bakery_json_write_ascii_string(buffer, item->key);
        } else {
          vkr_bakery_json_write_string(buffer, item->key);
        }
        vkr_bakery_buffer_append_cstr(
            buffer, style == VKR_BAKERY_JSON_PRETTY ||
                            style == VKR_BAKERY_JSON_PYTHON_SORTED
                        ? ": "
                        : ":");
      }
      ok = vkr_bakery_json_write_value(buffer, item, style, depth + 1u);
    }
    free(members);
    if (style == VKR_BAKERY_JSON_PRETTY) {
      vkr_bakery_json_indent(buffer, depth);
    }
    vkr_bakery_buffer_append(buffer, is_object ? "}" : "]", 1u);
    return ok;
  }
  }
  return false_v;
}

bool8_t vkr_bakery_json_write(Arena *arena, const VkrBakeryJson *value,
                              VkrBakeryJsonStyle style, String8 *out_text) {
  VkrBakeryBuffer buffer = {0};
  bool8_t ok = value && vkr_bakery_json_write_value(&buffer, value, style, 0u);
  ok = ok && !buffer.failed;
  if (ok) {
    *out_text = vkr_bakery_json_copy_bytes(
        arena, buffer.data ? buffer.data : (const uint8_t *)"", buffer.length);
    ok = out_text->str != NULL;
  }
  vkr_bakery_buffer_free(&buffer);
  return ok;
}
