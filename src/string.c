#include "string.h"

String8 string8_create(uint8_t *data, size_t length) {
  assert(data != NULL);
  assert(length > 0);

  String8 str = {data, length};
  return str;
}

String8 string8_create_formatted_v(Arena *arena, const char *fmt,
                                   va_list args) {
  assert(arena != NULL);
  assert(fmt != NULL);

  va_list args_copy;
  va_copy(args_copy, args);

  uint32_t required_size = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (required_size < 0) {
    return (String8){NULL, 0};
  }

  size_t buffer_size = (size_t)required_size + 1;
  uint8_t *buffer = arena_alloc(arena, buffer_size);

  if (buffer == NULL) {
    return (String8){NULL, 0};
  }

  vsnprintf((char *)buffer, buffer_size, fmt, args);

  return string8_create(buffer, (size_t)required_size);
}

String8 string8_create_formatted(Arena *arena, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = string8_create_formatted_v(arena, fmt, args);
  va_end(args);
  return result;
}

uint8_t *string8_cstr(String8 *str) { return str->str; }

void string8_destroy(String8 *str) {
  assert(str != NULL);

  str->str = NULL;
  str->length = 0;
}

String8 string8_concat(Arena *arena, String8 *str1, String8 *str2) {
  assert(arena != NULL);
  assert(str1 != NULL);
  assert(str2 != NULL);

  String8 str = {NULL, 0};

  str.length = str1->length + str2->length;
  str.str = arena_alloc(arena, str.length);

  MemCopy(str.str, str1->str, str1->length);
  MemCopy(str.str + str1->length, str2->str, str2->length);
  str.str[str.length] = '\0';

  return str;
}
