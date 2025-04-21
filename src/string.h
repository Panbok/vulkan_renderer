#pragma once

#include <stddef.h>

#include "arena.h"

#define string_lit(str) string_create((char *)str, sizeof(str) - 1)

typedef struct String {
  char *str;
  size_t length;
} String;

String string_create(char *data, size_t length);
char *string_cstr(String *str);
void string_destroy(String *str);

String string_concat(Arena *arena, String *str1, String *str2);
