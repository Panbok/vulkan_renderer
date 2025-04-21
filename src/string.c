#include "string.h"

String string_create(char *data, size_t length) {
  String str = {data, length};
  return str;
}

char *string_cstr(String *str) { return str->str; }

void string_destroy(String *str) {
  str->str = NULL;
  str->length = 0;
}

String string_concat(Arena *arena, String *str1, String *str2) {
  String str = {NULL, 0};

  str.length = str1->length + str2->length;
  str.str = arena_alloc(arena, str.length);

  MemCopy(str.str, str1->str, str1->length);
  MemCopy(str.str + str1->length, str2->str, str2->length);
  str.str[str.length] = '\0';

  return str;
}
