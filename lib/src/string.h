/**
 * @file string.h
 * @brief Defines string structures and manipulation functions.
 */

#pragma once

#include "arena.h"
#include "pch.h"

// Forward declaration for Arena type
typedef struct Arena Arena;

#define string8_lit(str) string8_create((uint8_t *)str, sizeof(str) - 1)

/**
 * @brief A string representation of UTF-8 encoded characters.
 */
typedef struct String8 {
  uint8_t *str;
  uint64_t length;
} String8;

/**
 * @brief Create a new string of 8-bit characters.
 * @param data The data to create the string from.
 * @param length The length of the string.
 * @return A new string of 8-bit characters.
 */
String8 string8_create(uint8_t *data, uint64_t length);

/**
 * @brief Create a new string of 8-bit characters from a format string and
 * arguments.
 * @param arena The arena to allocate the new string from.
 * @param fmt The format string.
 * @param ... The arguments to the format string.
 * @return A new string of 8-bit characters.
 */
String8 string8_create_formatted(Arena *arena, const char *fmt, ...);

/**
 * @brief Create a new string of 8-bit characters from a format string and
 * a va_list of arguments.
 * @param arena The arena to allocate the new string from.
 * @param fmt The format string.
 * @param args The va_list of arguments.
 * @return A new string of 8-bit characters.
 */
String8 string8_create_formatted_v(Arena *arena, const char *fmt, va_list args);

/**
 * @brief Get the C string representation of a string of 8-bit characters.
 * @param str The string to get the C string representation of.
 * @return A pointer to the C string representation of the string.
 */
uint8_t *string8_cstr(String8 *str);

/**
 * @brief Destroy a string of 8-bit characters.
 * @param str The string to destroy.
 */
void string8_destroy(String8 *str);

/**
 * @brief Concatenate two strings of 8-bit characters.
 * @param arena The arena to allocate the new string from.
 * @param str1 The first string to concatenate.
 * @param str2 The second string to concatenate.
 * @return A new string of 8-bit characters.
 */
String8 string8_concat(Arena *arena, String8 *str1, String8 *str2);
