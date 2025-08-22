#include "str.h"

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

  uint64_t substring_len = strlen(substring);

  if (substring_len == 0) {
    return true;
  }

  if (substring_len > str->length) {
    return false;
  }

  for (uint64_t i = 0; i <= str->length - substring_len; i++) {
    bool8_t match = true;
    for (uint64_t j = 0; j < substring_len; j++) {
      if (str->str[i + j] != substring[j]) {
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
