#include "str.h"

#include <alloca.h>
#include <ctype.h>
#include <errno.h>

/////////////////////
// String8
/////////////////////

String8 string8_create(uint8_t *data, uint64_t length) {
  assert(data != NULL && "Data is NULL");
  assert(length > 0 && "Length is 0");

  String8 str = {data, length};
  return str;
}

String8 string8_create_from_cstr(const uint8_t *data, uint64_t length) {
  assert(data != NULL && "Data is NULL");
  assert(length > 0 && "Length is 0");

  // Cast away const for string literals - this is safe because we know
  // these are read-only string literals that won't be modified
  String8 str = {(uint8_t *)data, length};
  return str;
}

String8 string8_create_formatted_v(Arena *arena, const char *fmt,
                                   va_list args) {
  assert(arena != NULL && "Arena is NULL");
  assert(fmt != NULL && "Format string is NULL");
  assert(args != NULL && "Arguments are NULL");

  va_list args_copy;
  va_copy(args_copy, args);

  int32_t required_size_i = vsnprintf(NULL, 0, fmt, args_copy);
  assert(required_size_i >= 0 && "Failed to format string");

  uint32_t required_size = (uint32_t)required_size_i;
  va_end(args_copy);

  assert(required_size >= 0 && "Failed to format string");

  uint64_t buffer_size = (uint64_t)required_size + 1;
  uint8_t *buffer = arena_alloc(arena, buffer_size, ARENA_MEMORY_TAG_STRING);

  assert(buffer != NULL && "Failed to allocate buffer");

  vsnprintf((char *)buffer, buffer_size, fmt, args);

  return string8_create(buffer, (uint64_t)required_size);
}

String8 string8_create_formatted(Arena *arena, const char *fmt, ...) {
  assert(arena != NULL && "Arena is NULL");
  assert(fmt != NULL && "Format string is NULL");

  va_list args;
  va_start(args, fmt);
  String8 result = string8_create_formatted_v(arena, fmt, args);
  va_end(args);
  return result;
}

String8 string8_substring(const String8 *str, uint64_t start, uint64_t end) {
  assert(str != NULL && "String is NULL");
  assert(start <= end && "Start is greater than end");
  assert(start <= str->length && "Start is greater than string length");
  assert(end <= str->length && "End is greater than string length");

  String8 result = {str->str + start, end - start};
  return result;
}

bool8_t string8_contains(const String8 *str, const String8 *substring) {
  assert(str != NULL && "String is NULL");
  assert(substring != NULL && "Substring is NULL");

  if (substring->length == 0) {
    return true;
  }

  if (substring->length > str->length) {
    return false;
  }

  for (uint64_t i = 0; i <= str->length - substring->length; i++) {
    bool8_t match = true;
    for (uint64_t j = 0; j < substring->length; j++) {
      if (str->str[i + j] != substring->str[j]) {
        match = false;
        break;
      }
    }

    if (match) {
      return true;
    }
  }

  return false;
}

bool8_t string8_contains_cstr(const String8 *str, const char *substring) {
  assert(str != NULL && "String is NULL");
  assert(substring != NULL && "Substring is NULL");

  uint64_t substring_len = (uint64_t)strlen(substring);

  if (substring_len == 0) {
    return true;
  }

  if (substring_len > str->length) {
    return false;
  }

  for (uint64_t i = 0; i <= str->length - substring_len; i++) {
    bool8_t match = true;
    for (uint64_t j = 0; j < substring_len; j++) {
      if (str->str[i + j] != (uint8_t)substring[j]) {
        match = false;
        break;
      }
    }

    if (match) {
      return true;
    }
  }

  return false;
}

bool8_t string8_equals(const String8 *str1, const String8 *str2) {
  assert(str1 != NULL && "String1 is NULL");
  assert(str2 != NULL && "String2 is NULL");

  if (str1->length != str2->length) {
    return false;
  }

  for (uint64_t i = 0; i < str1->length; i++) {
    if (str1->str[i] != str2->str[i]) {
      return false;
    }
  }

  return true;
}

bool8_t string8_equalsi(const String8 *str1, const String8 *str2) {
  assert(str1 != NULL && "String1 is NULL");
  assert(str2 != NULL && "String2 is NULL");

  if (str1->length != str2->length) {
    return false;
  }

  for (uint64_t i = 0; i < str1->length; i++) {
    uint8_t a = (uint8_t)str1->str[i];
    uint8_t b = (uint8_t)str2->str[i];
    if ((uint8_t)tolower(a) != (uint8_t)tolower(b)) {
      return false;
    }
  }

  return true;
}

void string8_trim(String8 *s) {
  assert(s != NULL && "String is NULL");

  if (!s->str || s->length == 0) {
    return;
  }

  uint64_t start = 0;
  while (start < s->length &&
         (s->str[start] == ' ' || s->str[start] == '\t' ||
          s->str[start] == '\r' || s->str[start] == '\n')) {
    start++;
  }

  uint64_t end = s->length;
  while (end > start && (s->str[end - 1] == ' ' || s->str[end - 1] == '\t' ||
                         s->str[end - 1] == '\r' || s->str[end - 1] == '\n')) {
    end--;
  }

  *s = string8_substring(s, start, end);
}

const char *string8_cstr(const String8 *str) {
  assert(str != NULL && "String is NULL");
  return (char *)str->str;
}

void string8_destroy(String8 *str) {
  assert(str != NULL && "String is NULL");

  str->str = NULL;
  str->length = 0;
}

String8 string8_concat(Arena *arena, String8 *str1, String8 *str2) {
  assert(arena != NULL && "Arena is NULL");
  assert(str1 != NULL && "String1 is NULL");
  assert(str2 != NULL && "String2 is NULL");

  String8 str = {NULL, 0};

  str.length = str1->length + str2->length;
  str.str = arena_alloc(arena, str.length + 1, ARENA_MEMORY_TAG_STRING);

  MemCopy(str.str, str1->str, str1->length);
  MemCopy(str.str + str1->length, str2->str, str2->length);
  str.str[str.length] = '\0';

  return str;
}

/////////////////////
// Native CString
/////////////////////

bool8_t string_equals(const char *str1, const char *str2) {
  assert(str1 != NULL && "String1 is NULL");
  assert(str2 != NULL && "String2 is NULL");

  return strcmp(str1, str2) == 0;
}

bool8_t string_equalsi(const char *str1, const char *str2) {
  assert(str1 != NULL && "String1 is NULL");
  assert(str2 != NULL && "String2 is NULL");

#if defined(PLATFORM_WINDOWS)
  return _stricmp(str1, str2) == 0;
#else
  return strcasecmp(str1, str2) == 0;
#endif
}

uint64_t string_length(const char *str) {
  assert(str != NULL && "String is NULL");
  return strlen(str);
}

char *string_duplicate(const char *str) {
  assert(str != NULL && "String is NULL");
#if defined(PLATFORM_WINDOWS)
  return _strdup(str);
#else
  return strdup(str);
#endif
}

bool8_t string_contains(const char *str, const char *substring) {
  assert(str != NULL && "String is NULL");
  assert(substring != NULL && "Substring is NULL");

  return strstr(str, substring) != NULL;
}

char *string_substring(Arena *arena, const char *str, int32_t start,
                       int32_t length) {
  assert(str != NULL && "String is NULL");
  assert(start >= 0 && "Start is negative");
  assert(length >= 0 && "Length is negative");

  uint64_t src_len = string_length(str);
  if ((uint64_t)start >= src_len) {
    char *out = (char *)arena_alloc(arena, 1, ARENA_MEMORY_TAG_STRING);
    assert(out != NULL && "Allocation failed");
    out[0] = '\0';
    return out;
  }

  uint64_t max_available = src_len - (uint64_t)start;
  uint64_t count = (uint64_t)length;
  if (count > max_available) {
    count = max_available;
  }

  char *out =
      (char *)arena_alloc(arena, (size_t)count + 1, ARENA_MEMORY_TAG_STRING);
  assert(out != NULL && "Allocation failed");
  if (count > 0) {
    MemCopy(out, str + start, (size_t)count);
  }
  out[count] = '\0';
  return out;
}

int32_t string_format(char *dest, uint64_t dest_size, const char *format, ...) {
  assert(dest != NULL && "Destination is NULL");
  assert(dest_size > 0 && "Destination size must be > 0");
  assert(format != NULL && "Format is NULL");

  va_list args;
  va_start(args, format);
  int32_t result = vsnprintf(dest, (size_t)dest_size, format, args);
  va_end(args);
  return result;
}

int32_t string_format_v(char *dest, uint64_t dest_size, const char *format,
                        va_list args) {
  assert(dest != NULL && "Destination is NULL");
  assert(dest_size > 0 && "Destination size must be > 0");
  assert(format != NULL && "Format is NULL");

  va_list args_copy;
  va_copy(args_copy, args);
  int32_t result = vsnprintf(dest, (size_t)dest_size, format, args_copy);
  va_end(args_copy);
  return result;
}

char *string_empty(char *str) {
  assert(str != NULL && "String is NULL");

  str[0] = '\0';
  return str;
}

char *string_copy(char *dest, const char *source) {
  assert(dest != NULL && "Destination is NULL");
  assert(source != NULL && "Source is NULL");

  return strcpy(dest, source);
}

char *string_ncopy(char *dest, const char *source, int64_t length) {
  assert(dest != NULL && "Destination is NULL");
  assert(source != NULL && "Source is NULL");
  assert(length > 0 && "Length must be positive");

  return strncpy(dest, source, length);
}

char *string_trim(char *str) {
  assert(str != NULL && "String is NULL");

  while (isspace(*str)) {
    str++;
  }

  char *end = str + strlen(str) - 1;

  while (end > str && isspace(*end)) {
    end--;
  }

  *(end + 1) = '\0';

  return str;
}

void string_mid(char *dest, const char *source, int32_t start, int32_t length) {
  assert(dest != NULL && "Destination is NULL");
  assert(source != NULL && "Source is NULL");
  assert(start >= 0 && "Start must be non-negative");

  if (length == 0) {
    return;
  }

  uint64_t src_length = string_length(source);
  if ((uint64_t)start >= src_length) {
    dest[0] = '\0';
    return;
  }

  if (length > 0) {
    uint64_t j = 0;
    for (uint64_t i = (uint64_t)start; j < (uint64_t)length && i < src_length;
         ++i, ++j) {
      dest[j] = source[i];
    }
    dest[j] = '\0';
  } else {
    // If a negative value is passed, proceed to the end of the string.
    uint64_t j = 0;
    for (uint64_t i = (uint64_t)start; i < src_length; ++i, ++j) {
      dest[j] = source[i];
    }
    dest[j] = '\0';
  }
}

int32_t string_index_of(const char *str, char c) {
  if (!str) {
    return -1;
  }

  uint32_t length = string_length(str);
  if (length > 0) {
    for (uint32_t i = 0; i < length; ++i) {
      if (str[i] == c) {
        return i;
      }
    }
  }

  return -1;
}

/////////////////////
// Conversions
/////////////////////

static inline const char *string__skip_ws(const char *p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
    p++;
  }
  return p;
}

static inline bool8_t string__parse_f64(const char *s, float64_t *out) {
  if (!s || !out)
    return false_v;

  char *endptr = NULL;
  errno = 0;
  const char *start = string__skip_ws(s);

  float64_t v = strtod(start, &endptr);
  if (start == endptr || errno == ERANGE)
    return false_v;

  const char *trail = string__skip_ws(endptr);
  if (*trail != '\0')
    return false_v;

  *out = (float64_t)v;
  return true_v;
}

static inline bool8_t string__parse_f32(const char *s, float32_t *out) {
  float64_t d;
  if (!string__parse_f64(s, &d))
    return false_v;

  *out = (float32_t)d;
  return true_v;
}

static inline bool8_t string__parse_i64(const char *s, int64_t *out) {
  if (!s || !out)
    return false_v;

  char *endptr = NULL;
  errno = 0;

  const char *start = string__skip_ws(s);
  int64_t v = strtoll(start, &endptr, 10);
  if (start == endptr || errno == ERANGE)
    return false_v;
  const char *trail = string__skip_ws(endptr);
  if (*trail != '\0')
    return false_v;
  *out = (int64_t)v;
  return true_v;
}

static inline bool8_t string__parse_u64(const char *s, uint64_t *out) {
  if (!s || !out)
    return false_v;

  char *endptr = NULL;
  errno = 0;

  const char *start = string__skip_ws(s);
  uint64_t v = strtoull(start, &endptr, 10);
  if (start == endptr || errno == ERANGE)
    return false_v;

  const char *trail = string__skip_ws(endptr);
  if (*trail != '\0')
    return false_v;

  *out = (uint64_t)v;
  return true_v;
}

bool8_t string_to_f64(const char *s, float64_t *out) {
  return string__parse_f64(s, out);
}
bool8_t string_to_f32(const char *s, float32_t *out) {
  return string__parse_f32(s, out);
}
bool8_t string_to_i64(const char *s, int64_t *out) {
  return string__parse_i64(s, out);
}
bool8_t string_to_u64(const char *s, uint64_t *out) {
  return string__parse_u64(s, out);
}

bool8_t string_to_i32(const char *s, int32_t *out) {
  int64_t v;
  if (!string__parse_i64(s, &v))
    return false_v;
  if (v < INT32_MIN || v > INT32_MAX)
    return false_v;
  *out = (int32_t)v;
  return true_v;
}

bool8_t string_to_u32(const char *s, uint32_t *out) {
  uint64_t v;
  if (!string__parse_u64(s, &v))
    return false_v;
  if (v > UINT32_MAX)
    return false_v;
  *out = (uint32_t)v;
  return true_v;
}

bool8_t string_to_bool(const char *s, bool8_t *out) {
  if (!s || !out)
    return false_v;
  const char *p = string__skip_ws(s);
  if (string_equalsi(p, "true") || string_equalsi(p, "yes") ||
      string_equalsi(p, "on") || string_equals(p, "1")) {
    *out = true_v;
    return true_v;
  }
  if (string_equalsi(p, "false") || string_equalsi(p, "no") ||
      string_equalsi(p, "off") || string_equals(p, "0")) {
    *out = false_v;
    return true_v;
  }
  return false_v;
}

static inline bool8_t string__parse_vecn(const char *s, double *dst, int n) {
  if (!s || !dst)
    return false_v;
  int32_t matched = 0;
  switch (n) {
  case 2:
    matched = sscanf(s, "%lf , %lf", &dst[0], &dst[1]);
    break;
  case 3:
    matched = sscanf(s, "%lf , %lf , %lf", &dst[0], &dst[1], &dst[2]);
    break;
  case 4:
    matched =
        sscanf(s, "%lf , %lf , %lf , %lf", &dst[0], &dst[1], &dst[2], &dst[3]);
    break;
  default:
    return false_v;
  }
  return matched == n ? true_v : false_v;
}

bool8_t string_to_vec2(const char *s, Vec2 *out) {
  float64_t v[2];
  if (!string__parse_vecn(s, v, 2))
    return false_v;
  *out = vec2_new((float)v[0], (float)v[1]);
  return true_v;
}

bool8_t string_to_vec3(const char *s, Vec3 *out) {
  float64_t v[3];
  if (!string__parse_vecn(s, v, 3))
    return false_v;
  *out = vec3_new((float)v[0], (float)v[1], (float)v[2]);
  return true_v;
}

bool8_t string_to_vec4(const char *s, Vec4 *out) {
  float64_t v[4];
  if (!string__parse_vecn(s, v, 4))
    return false_v;
  *out = vec4_new((float)v[0], (float)v[1], (float)v[2], (float)v[3]);
  return true_v;
}

// String8 wrappers
static inline bool8_t string8__to_cbuf(const String8 *s, char **out_buf) {
  if (!s || !s->str || s->length == 0)
    return false_v;
  char *buf = (char *)alloca((size_t)s->length + 1);
  MemCopy(buf, s->str, (size_t)s->length);
  buf[s->length] = '\0';
  *out_buf = buf;
  return true_v;
}

bool8_t string8_to_f64(const String8 *s, float64_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_f64(buf, out);
}
bool8_t string8_to_f32(const String8 *s, float32_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_f32(buf, out);
}
bool8_t string8_to_i64(const String8 *s, int64_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_i64(buf, out);
}
bool8_t string8_to_u64(const String8 *s, uint64_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_u64(buf, out);
}
bool8_t string8_to_i32(const String8 *s, int32_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_i32(buf, out);
}
bool8_t string8_to_u32(const String8 *s, uint32_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_u32(buf, out);
}
bool8_t string8_to_bool(const String8 *s, bool8_t *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_bool(buf, out);
}
bool8_t string8_to_vec2(const String8 *s, Vec2 *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_vec2(buf, out);
}
bool8_t string8_to_vec3(const String8 *s, Vec3 *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_vec3(buf, out);
}
bool8_t string8_to_vec4(const String8 *s, Vec4 *out) {
  char *buf;
  if (!string8__to_cbuf(s, &buf))
    return false_v;
  return string_to_vec4(buf, out);
}