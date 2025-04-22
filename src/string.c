#include "string.h"

String8 string8_create(uint8_t *data, size_t length) {
  assert(data != NULL);
  assert(length > 0);

  String8 str = {data, length};
  return str;
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
