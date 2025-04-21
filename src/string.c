#include "string.h"

String8 string8_create(uint8_t *data, size_t length) {
  String8 str = {data, length};
  return str;
}

uint8_t *string8_cstr(String8 *str) { return str->str; }

void string8_destroy(String8 *str) {
  str->str = NULL;
  str->length = 0;
}

String8 string8_concat(Arena *arena, String8 *str1, String8 *str2) {
  String8 str = {NULL, 0};

  str.length = str1->length + str2->length;
  str.str = arena_alloc(arena, str.length);

  MemCopy(str.str, str1->str, str1->length);
  MemCopy(str.str + str1->length, str2->str, str2->length);
  str.str[str.length] = '\0';

  return str;
}
