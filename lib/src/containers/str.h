/**
 * @file string.h
 * @brief Defines string structures and manipulation functions.
 */

#pragma once

#include "memory/arena.h"
#include "pch.h"

// Forward declaration for Arena type
typedef struct Arena Arena;

// todo: maybe consider using a simple string impl which is just an array of
// ascii chars and for complex string use a RichString which is a string a
// unicode string with some metadata like encoding, length, locale, etc.

// Note : Only use with string literals, not char pointers
#define string8_lit(str)                                                       \
  string8_create_from_cstr((const uint8_t *)str, sizeof(str) - 1)

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
 * @brief Create a new string of 8-bit characters from const data (e.g., string
 * literals).
 * @param data The const data to create the string from.
 * @param length The length of the string.
 * @return A new string of 8-bit characters.
 */
String8 string8_create_from_cstr(const uint8_t *data, uint64_t length);

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
const char *string8_cstr(const String8 *str);

/**
 * @brief Concatenate two strings of 8-bit characters.
 * @param arena The arena to allocate the new string from.
 * @param str1 The first string to concatenate.
 * @param str2 The second string to concatenate.
 * @return A new string of 8-bit characters.
 */
String8 string8_concat(Arena *arena, String8 *str1, String8 *str2);

/**
 * @brief Get a byte-slice (non-owning) substring of a UTF-8 string.
 * @details Indices are byte offsets; end is exclusive. Returns a view into
 * `str` (no allocation). Preconditions: 0 <= start <= end <= str->length. Note:
 * Operates on bytes and does not validate UTF-8 boundaries; callers must ensure
 * start/end align to code point boundaries if required.
 * @param str The source string (not modified).
 * @param start The start byte offset (inclusive).
 * @param end The end byte offset (exclusive).
 * @return A String8 view into the original buffer (not NUL-terminated).
 */
String8 string8_substring(const String8 *str, uint64_t start, uint64_t end);

/**
 * @brief Bytewise substring test (no Unicode normalization/case-folding).
 * @details Matches on raw bytes. An empty substring returns true.
 * @param str The source string (not modified).
 * @param substring The substring to search for (not modified).
 * @return True if `substring` occurs in `str`, false otherwise.
 */
bool8_t string8_contains(const String8 *str, const String8 *substring);

/**
 * @brief Check if a string contains a null-terminated C-string substring.
 * @details Interprets `substring` as UTF-8; matches on raw bytes. An empty
 * substring returns true.
 * @param str The source string (not modified).
 * @param substring The null-terminated substring to search for.
 * @return True if `substring` occurs in `str`, false otherwise.
 */
bool8_t string8_contains_cstr(const String8 *str, const char *substring);

/**
 * @brief Bytewise equality check (no Unicode normalization/case-folding).
 * @param str1 The first string (not modified).
 * @param str2 The second string (not modified).
 * @return True if both length and bytes match exactly, false otherwise.
 */
bool8_t string8_equals(const String8 *str1, const String8 *str2);

/**
 * @brief Destroy a string of 8-bit characters.
 * @param str The string to destroy.
 */
void string8_destroy(String8 *str);