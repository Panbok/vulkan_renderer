#include "str.h"

String8 string8_create(uint8_t *data, uint64_t length) {
  assert(data != NULL && "Data is NULL");
  assert(length > 0 && "Length is 0");

  String8 str = {data, length};
  return str;
}

String8 string8_create_formatted_v(Arena *arena, const char *fmt,
                                   va_list args) {
  assert(arena != NULL && "Arena is NULL");
  assert(fmt != NULL && "Format string is NULL");
  assert(args != NULL && "Arguments are NULL");

  va_list args_copy;
  va_copy(args_copy, args);

  uint32_t required_size = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  assert(required_size >= 0 && "Failed to format string");

  uint64_t buffer_size = (uint64_t)required_size + 1;
  uint8_t *buffer = arena_alloc(arena, buffer_size);

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

uint8_t *string8_cstr(String8 *str) {
  assert(str != NULL && "String is NULL");
  return str->str;
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
  str.str = arena_alloc(arena, str.length + 1); // +1 for null terminator

  MemCopy(str.str, str1->str, str1->length);
  MemCopy(str.str + str1->length, str2->str, str2->length);
  str.str[str.length] = '\0';

  return str;
}
