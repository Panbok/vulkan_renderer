#include "vkr_harness_json.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct VkrHarnessJsonParser {
  VkrHarnessJsonDocument *document;
  uint64_t cursor;
  VkrHarnessError *error;
} VkrHarnessJsonParser;

static void vkr_harness_json_skip_space(VkrHarnessJsonParser *parser) {
  while (parser->cursor < parser->document->length) {
    const char c = parser->document->json[parser->cursor];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    parser->cursor++;
  }
}

static bool8_t vkr_harness_json_is_delimiter(char c) {
  return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == ',' || c == ']' || c == '}';
}

static int32_t vkr_harness_json_allocate_token(VkrHarnessJsonParser *parser,
                                               VkrHarnessJsonTokenType type,
                                               int32_t parent, uint32_t start) {
  if (parser->document->token_count >= VKR_HARNESS_JSON_MAX_TOKENS) {
    vkr_harness_error_set(parser->error, "json.token_limit", "$",
                          "JSON exceeds the %u-token harness limit",
                          VKR_HARNESS_JSON_MAX_TOKENS);
    return -1;
  }
  const uint32_t index = parser->document->token_count++;
  parser->document->tokens[index] = (VkrHarnessJsonToken){
      .type = type,
      .start = start,
      .end = start,
      .parent = parent,
      .child_count = 0,
  };
  if (parent >= 0) {
    parser->document->tokens[parent].child_count++;
  }
  return (int32_t)index;
}

static bool8_t vkr_harness_json_hex(char c, uint32_t *out) {
  if (c >= '0' && c <= '9') {
    *out = (uint32_t)(c - '0');
    return true_v;
  }
  if (c >= 'a' && c <= 'f') {
    *out = (uint32_t)(c - 'a') + 10u;
    return true_v;
  }
  if (c >= 'A' && c <= 'F') {
    *out = (uint32_t)(c - 'A') + 10u;
    return true_v;
  }
  return false_v;
}

static int32_t vkr_harness_json_parse_value(VkrHarnessJsonParser *parser,
                                            int32_t parent);

static int32_t vkr_harness_json_parse_string_token(VkrHarnessJsonParser *parser,
                                                   int32_t parent) {
  const uint64_t length = parser->document->length;
  const char *json = parser->document->json;
  const uint32_t start = (uint32_t)++parser->cursor;
  while (parser->cursor < length) {
    const unsigned char c = (unsigned char)json[parser->cursor];
    if (c == '"') {
      const int32_t token = vkr_harness_json_allocate_token(
          parser, VKR_HARNESS_JSON_STRING, parent, start);
      if (token >= 0) {
        parser->document->tokens[token].end = (uint32_t)parser->cursor;
      }
      parser->cursor++;
      return token;
    }
    if (c < 0x20u) {
      vkr_harness_error_set(parser->error, "json.control_character", "$",
                            "Unescaped control character at byte %llu",
                            (unsigned long long)parser->cursor);
      return -1;
    }
    if (c == '\\') {
      parser->cursor++;
      if (parser->cursor >= length) {
        break;
      }
      const char escaped = json[parser->cursor];
      if (escaped == 'u') {
        if (parser->cursor + 4u >= length) {
          break;
        }
        for (uint32_t i = 1; i <= 4; ++i) {
          uint32_t unused = 0;
          if (!vkr_harness_json_hex(json[parser->cursor + i], &unused)) {
            vkr_harness_error_set(parser->error, "json.escape", "$",
                                  "Invalid Unicode escape at byte %llu",
                                  (unsigned long long)parser->cursor);
            return -1;
          }
        }
        parser->cursor += 4u;
      } else if (!strchr("\"\\/bfnrt", escaped)) {
        vkr_harness_error_set(parser->error, "json.escape", "$",
                              "Invalid escape at byte %llu",
                              (unsigned long long)parser->cursor);
        return -1;
      }
    }
    parser->cursor++;
  }
  vkr_harness_error_set(parser->error, "json.unterminated_string", "$",
                        "Unterminated JSON string");
  return -1;
}

static int32_t vkr_harness_json_parse_number_token(VkrHarnessJsonParser *parser,
                                                   int32_t parent) {
  const char *json = parser->document->json;
  const uint64_t length = parser->document->length;
  const uint32_t start = (uint32_t)parser->cursor;
  if (json[parser->cursor] == '-') {
    parser->cursor++;
  }
  if (parser->cursor >= length) {
    goto invalid;
  }
  if (json[parser->cursor] == '0') {
    parser->cursor++;
    if (parser->cursor < length && json[parser->cursor] >= '0' &&
        json[parser->cursor] <= '9') {
      goto invalid;
    }
  } else if (json[parser->cursor] >= '1' && json[parser->cursor] <= '9') {
    do {
      parser->cursor++;
    } while (parser->cursor < length && json[parser->cursor] >= '0' &&
             json[parser->cursor] <= '9');
  } else {
    goto invalid;
  }
  if (parser->cursor < length && json[parser->cursor] == '.') {
    parser->cursor++;
    if (parser->cursor >= length || json[parser->cursor] < '0' ||
        json[parser->cursor] > '9') {
      goto invalid;
    }
    while (parser->cursor < length && json[parser->cursor] >= '0' &&
           json[parser->cursor] <= '9') {
      parser->cursor++;
    }
  }
  if (parser->cursor < length &&
      (json[parser->cursor] == 'e' || json[parser->cursor] == 'E')) {
    parser->cursor++;
    if (parser->cursor < length &&
        (json[parser->cursor] == '+' || json[parser->cursor] == '-')) {
      parser->cursor++;
    }
    if (parser->cursor >= length || json[parser->cursor] < '0' ||
        json[parser->cursor] > '9') {
      goto invalid;
    }
    while (parser->cursor < length && json[parser->cursor] >= '0' &&
           json[parser->cursor] <= '9') {
      parser->cursor++;
    }
  }
  if (parser->cursor < length &&
      !vkr_harness_json_is_delimiter(json[parser->cursor])) {
    goto invalid;
  }
  const int32_t token = vkr_harness_json_allocate_token(
      parser, VKR_HARNESS_JSON_NUMBER, parent, start);
  if (token >= 0) {
    parser->document->tokens[token].end = (uint32_t)parser->cursor;
  }
  return token;

invalid:
  vkr_harness_error_set(parser->error, "json.number", "$",
                        "Invalid number at byte %u", start);
  return -1;
}

static int32_t vkr_harness_json_parse_literal(VkrHarnessJsonParser *parser,
                                              int32_t parent,
                                              const char *literal,
                                              VkrHarnessJsonTokenType type) {
  const uint64_t literal_length = strlen(literal);
  const uint32_t start = (uint32_t)parser->cursor;
  if (parser->cursor + literal_length > parser->document->length ||
      memcmp(parser->document->json + parser->cursor, literal,
             literal_length) != 0) {
    return -1;
  }
  parser->cursor += literal_length;
  if (parser->cursor < parser->document->length &&
      !vkr_harness_json_is_delimiter(parser->document->json[parser->cursor])) {
    return -1;
  }
  const int32_t token =
      vkr_harness_json_allocate_token(parser, type, parent, start);
  if (token >= 0) {
    parser->document->tokens[token].end = (uint32_t)parser->cursor;
  }
  return token;
}

static int32_t vkr_harness_json_parse_array(VkrHarnessJsonParser *parser,
                                            int32_t parent) {
  const uint32_t start = (uint32_t)parser->cursor++;
  const int32_t array = vkr_harness_json_allocate_token(
      parser, VKR_HARNESS_JSON_ARRAY, parent, start);
  if (array < 0) {
    return -1;
  }
  vkr_harness_json_skip_space(parser);
  if (parser->cursor < parser->document->length &&
      parser->document->json[parser->cursor] == ']') {
    parser->document->tokens[array].end = (uint32_t)++parser->cursor;
    return array;
  }
  while (parser->cursor < parser->document->length) {
    if (vkr_harness_json_parse_value(parser, array) < 0) {
      return -1;
    }
    vkr_harness_json_skip_space(parser);
    if (parser->cursor >= parser->document->length) {
      break;
    }
    const char delimiter = parser->document->json[parser->cursor++];
    if (delimiter == ']') {
      parser->document->tokens[array].end = (uint32_t)parser->cursor;
      return array;
    }
    if (delimiter != ',') {
      break;
    }
    vkr_harness_json_skip_space(parser);
  }
  vkr_harness_error_set(parser->error, "json.array", "$",
                        "Unterminated or malformed array at byte %u", start);
  return -1;
}

static int32_t vkr_harness_json_parse_object(VkrHarnessJsonParser *parser,
                                             int32_t parent) {
  const uint32_t start = (uint32_t)parser->cursor++;
  const int32_t object = vkr_harness_json_allocate_token(
      parser, VKR_HARNESS_JSON_OBJECT, parent, start);
  if (object < 0) {
    return -1;
  }
  vkr_harness_json_skip_space(parser);
  if (parser->cursor < parser->document->length &&
      parser->document->json[parser->cursor] == '}') {
    parser->document->tokens[object].end = (uint32_t)++parser->cursor;
    return object;
  }
  while (parser->cursor < parser->document->length) {
    if (parser->document->json[parser->cursor] != '"' ||
        vkr_harness_json_parse_string_token(parser, object) < 0) {
      break;
    }
    vkr_harness_json_skip_space(parser);
    if (parser->cursor >= parser->document->length ||
        parser->document->json[parser->cursor++] != ':') {
      break;
    }
    vkr_harness_json_skip_space(parser);
    if (vkr_harness_json_parse_value(parser, object) < 0) {
      return -1;
    }
    vkr_harness_json_skip_space(parser);
    if (parser->cursor >= parser->document->length) {
      break;
    }
    const char delimiter = parser->document->json[parser->cursor++];
    if (delimiter == '}') {
      parser->document->tokens[object].end = (uint32_t)parser->cursor;
      return object;
    }
    if (delimiter != ',') {
      break;
    }
    vkr_harness_json_skip_space(parser);
  }
  vkr_harness_error_set(parser->error, "json.object", "$",
                        "Unterminated or malformed object at byte %u", start);
  return -1;
}

static int32_t vkr_harness_json_parse_value(VkrHarnessJsonParser *parser,
                                            int32_t parent) {
  vkr_harness_json_skip_space(parser);
  if (parser->cursor >= parser->document->length) {
    vkr_harness_error_set(parser->error, "json.value", "$",
                          "Expected a JSON value at end of input");
    return -1;
  }
  const char c = parser->document->json[parser->cursor];
  if (c == '{') {
    return vkr_harness_json_parse_object(parser, parent);
  }
  if (c == '[') {
    return vkr_harness_json_parse_array(parser, parent);
  }
  if (c == '"') {
    return vkr_harness_json_parse_string_token(parser, parent);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    return vkr_harness_json_parse_number_token(parser, parent);
  }
  int32_t token = -1;
  if (c == 't') {
    token = vkr_harness_json_parse_literal(parser, parent, "true",
                                           VKR_HARNESS_JSON_BOOL);
  } else if (c == 'f') {
    token = vkr_harness_json_parse_literal(parser, parent, "false",
                                           VKR_HARNESS_JSON_BOOL);
  } else if (c == 'n') {
    token = vkr_harness_json_parse_literal(parser, parent, "null",
                                           VKR_HARNESS_JSON_NULL);
  }
  if (token < 0 && (!parser->error || parser->error->code[0] == '\0')) {
    vkr_harness_error_set(parser->error, "json.value", "$",
                          "Invalid JSON value at byte %llu",
                          (unsigned long long)parser->cursor);
  }
  return token;
}

bool8_t vkr_harness_json_parse(VkrHarnessJsonDocument *document,
                               const char *json, uint64_t length,
                               VkrHarnessError *out_error) {
  if (!document || !json || length == 0 || length > UINT32_MAX) {
    vkr_harness_error_set(out_error, "json.input", "$",
                          "JSON input is empty or too large");
    return false_v;
  }
  memset(document, 0, sizeof(*document));
  document->json = json;
  document->length = length;
  VkrHarnessJsonParser parser = {
      .document = document,
      .cursor = 0,
      .error = out_error,
  };
  if (vkr_harness_json_parse_value(&parser, -1) != 0) {
    if (!out_error || out_error->code[0] == '\0') {
      vkr_harness_error_set(out_error, "json.root", "$",
                            "JSON root must be the first token");
    }
    return false_v;
  }
  vkr_harness_json_skip_space(&parser);
  if (parser.cursor != length) {
    vkr_harness_error_set(out_error, "json.trailing", "$",
                          "Trailing JSON content at byte %llu",
                          (unsigned long long)parser.cursor);
    return false_v;
  }
  return true_v;
}

int32_t vkr_harness_json_next(const VkrHarnessJsonDocument *document,
                              int32_t token) {
  if (!document || token < 0 || (uint32_t)token >= document->token_count) {
    return -1;
  }
  const uint32_t end = document->tokens[token].end;
  uint32_t next = (uint32_t)token + 1u;
  while (next < document->token_count && document->tokens[next].start < end) {
    next++;
  }
  return next < document->token_count ? (int32_t)next : -1;
}

static bool8_t
vkr_harness_json_token_key_equals(const VkrHarnessJsonDocument *document,
                                  int32_t token, const char *name) {
  if (!document || token < 0 || (uint32_t)token >= document->token_count ||
      document->tokens[token].type != VKR_HARNESS_JSON_STRING) {
    return false_v;
  }
  const VkrHarnessJsonToken *key = &document->tokens[token];
  const uint64_t length = strlen(name);
  return key->end - key->start == length &&
         memcmp(document->json + key->start, name, length) == 0;
}

int32_t vkr_harness_json_object_get(const VkrHarnessJsonDocument *document,
                                    int32_t object_token, const char *name,
                                    bool8_t *out_duplicate) {
  if (out_duplicate) {
    *out_duplicate = false_v;
  }
  if (!document || object_token < 0 ||
      (uint32_t)object_token >= document->token_count ||
      document->tokens[object_token].type != VKR_HARNESS_JSON_OBJECT) {
    return -1;
  }
  int32_t found = -1;
  int32_t key = object_token + 1;
  const uint32_t end = document->tokens[object_token].end;
  while (key >= 0 && (uint32_t)key < document->token_count &&
         document->tokens[key].start < end) {
    if (document->tokens[key].parent != object_token) {
      key++;
      continue;
    }
    const int32_t value = key + 1;
    if ((uint32_t)value >= document->token_count ||
        document->tokens[value].parent != object_token) {
      return -1;
    }
    if (vkr_harness_json_token_key_equals(document, key, name)) {
      if (found >= 0) {
        if (out_duplicate) {
          *out_duplicate = true_v;
        }
        return found;
      }
      found = value;
    }
    key = vkr_harness_json_next(document, value);
  }
  return found;
}

bool8_t vkr_harness_json_object_validate(
    const VkrHarnessJsonDocument *document, int32_t object_token,
    const char *const *allowed, uint32_t allowed_count,
    const char *const *required, uint32_t required_count, const char *field,
    VkrHarnessError *out_error) {
  if (!document || object_token < 0 ||
      (uint32_t)object_token >= document->token_count ||
      document->tokens[object_token].type != VKR_HARNESS_JSON_OBJECT) {
    vkr_harness_error_set(out_error, "manifest.type", field,
                          "Expected an object");
    return false_v;
  }
  int32_t key = object_token + 1;
  const uint32_t end = document->tokens[object_token].end;
  while (key >= 0 && (uint32_t)key < document->token_count &&
         document->tokens[key].start < end) {
    if (document->tokens[key].parent != object_token) {
      key++;
      continue;
    }
    char decoded[128];
    if (!vkr_harness_json_string(document, key, decoded, sizeof(decoded), field,
                                 out_error)) {
      return false_v;
    }
    bool8_t known = false_v;
    for (uint32_t i = 0; i < allowed_count; ++i) {
      if (strcmp(decoded, allowed[i]) == 0) {
        known = true_v;
        break;
      }
    }
    if (!known) {
      vkr_harness_error_set(out_error, "manifest.unknown_field", field,
                            "Unknown field '%s'", decoded);
      return false_v;
    }
    bool8_t duplicate = false_v;
    (void)vkr_harness_json_object_get(document, object_token, decoded,
                                      &duplicate);
    if (duplicate) {
      vkr_harness_error_set(out_error, "manifest.duplicate_field", field,
                            "Duplicate field '%s'", decoded);
      return false_v;
    }
    const int32_t value = key + 1;
    key = vkr_harness_json_next(document, value);
  }
  for (uint32_t i = 0; i < required_count; ++i) {
    if (vkr_harness_json_object_get(document, object_token, required[i], NULL) <
        0) {
      vkr_harness_error_set(out_error, "manifest.required", field,
                            "Missing required field '%s'", required[i]);
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_harness_json_append_utf8(uint32_t codepoint, char *out,
                                            uint32_t capacity,
                                            uint32_t *cursor) {
  if (codepoint <= 0x7fu) {
    if (*cursor + 1u >= capacity) {
      return false_v;
    }
    out[(*cursor)++] = (char)codepoint;
  } else if (codepoint <= 0x7ffu) {
    if (*cursor + 2u >= capacity) {
      return false_v;
    }
    out[(*cursor)++] = (char)(0xc0u | (codepoint >> 6u));
    out[(*cursor)++] = (char)(0x80u | (codepoint & 0x3fu));
  } else {
    if (*cursor + 3u >= capacity ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
      return false_v;
    }
    out[(*cursor)++] = (char)(0xe0u | (codepoint >> 12u));
    out[(*cursor)++] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
    out[(*cursor)++] = (char)(0x80u | (codepoint & 0x3fu));
  }
  return true_v;
}

bool8_t vkr_harness_json_string(const VkrHarnessJsonDocument *document,
                                int32_t token, char *out, uint32_t out_capacity,
                                const char *field, VkrHarnessError *out_error) {
  if (!document || token < 0 || (uint32_t)token >= document->token_count ||
      !out || out_capacity == 0 ||
      document->tokens[token].type != VKR_HARNESS_JSON_STRING) {
    vkr_harness_error_set(out_error, "manifest.type", field,
                          "Expected a string");
    return false_v;
  }
  const VkrHarnessJsonToken *value = &document->tokens[token];
  uint32_t cursor = 0;
  for (uint32_t i = value->start; i < value->end; ++i) {
    unsigned char c = (unsigned char)document->json[i];
    if (c != '\\') {
      if (cursor + 1u >= out_capacity) {
        goto too_long;
      }
      out[cursor++] = (char)c;
      continue;
    }
    c = (unsigned char)document->json[++i];
    if (c == 'u') {
      uint32_t codepoint = 0;
      for (uint32_t digit = 0; digit < 4; ++digit) {
        uint32_t nibble = 0;
        (void)vkr_harness_json_hex(document->json[++i], &nibble);
        codepoint = (codepoint << 4u) | nibble;
      }
      if (!vkr_harness_json_append_utf8(codepoint, out, out_capacity,
                                        &cursor)) {
        goto too_long;
      }
      continue;
    }
    char decoded = (char)c;
    switch (c) {
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
    default:
      break;
    }
    if (cursor + 1u >= out_capacity) {
      goto too_long;
    }
    out[cursor++] = decoded;
  }
  out[cursor] = '\0';
  return true_v;

too_long:
  vkr_harness_error_set(out_error, "manifest.string_too_long", field,
                        "String exceeds the %u-byte field capacity",
                        out_capacity - 1u);
  return false_v;
}

bool8_t vkr_harness_json_f64(const VkrHarnessJsonDocument *document,
                             int32_t token, float64_t *out, const char *field,
                             VkrHarnessError *out_error) {
  if (!document || token < 0 || (uint32_t)token >= document->token_count ||
      !out || document->tokens[token].type != VKR_HARNESS_JSON_NUMBER) {
    vkr_harness_error_set(out_error, "manifest.type", field,
                          "Expected a number");
    return false_v;
  }
  const VkrHarnessJsonToken *value = &document->tokens[token];
  const uint32_t length = value->end - value->start;
  if (length >= 64u) {
    vkr_harness_error_set(out_error, "manifest.number", field,
                          "Numeric token is too long");
    return false_v;
  }
  char buffer[64];
  memcpy(buffer, document->json + value->start, length);
  buffer[length] = '\0';
  errno = 0;
  char *end = NULL;
  const double parsed = strtod(buffer, &end);
  if (errno != 0 || end != buffer + length || !isfinite(parsed)) {
    vkr_harness_error_set(out_error, "manifest.number", field,
                          "Number is non-finite or out of range");
    return false_v;
  }
  *out = parsed;
  return true_v;
}

bool8_t vkr_harness_json_u64(const VkrHarnessJsonDocument *document,
                             int32_t token, uint64_t *out, const char *field,
                             VkrHarnessError *out_error) {
  float64_t parsed = 0.0;
  if (!vkr_harness_json_f64(document, token, &parsed, field, out_error) ||
      parsed < 0.0 || parsed > (float64_t)UINT64_MAX ||
      floor(parsed) != parsed) {
    vkr_harness_error_set(out_error, "manifest.integer", field,
                          "Expected a non-negative integer");
    return false_v;
  }
  *out = (uint64_t)parsed;
  return true_v;
}

bool8_t vkr_harness_json_bool(const VkrHarnessJsonDocument *document,
                              int32_t token, bool8_t *out, const char *field,
                              VkrHarnessError *out_error) {
  if (!document || token < 0 || (uint32_t)token >= document->token_count ||
      !out || document->tokens[token].type != VKR_HARNESS_JSON_BOOL) {
    vkr_harness_error_set(out_error, "manifest.type", field,
                          "Expected a boolean");
    return false_v;
  }
  *out =
      document->json[document->tokens[token].start] == 't' ? true_v : false_v;
  return true_v;
}
