/**
 * @file string.h
 * @brief Defines string structures and manipulation functions.
 */

#pragma once

#include "defines.h"
#include "math/vec.h"
#include "memory/arena.h"

// Forward declaration for Arena type
typedef struct Arena Arena;

// todo: maybe consider using a simple string impl which is just an array of
// ascii chars and for complex string use a RichString which is a string a
// unicode string with some metadata like encoding, length, locale, etc.
// The example of string structures we can use:
// - Text: immutable C-string (const char*)
// - String8: mutable ASCII byte buffer
// - RichString: String (any UTF) + metadata (encoding, length, locale, etc.)

/////////////////////
// String8
/////////////////////

// Note : Only use with string literals, not char pointers
#define string8_lit(str)                                                       \
  string8_create_from_cstr((const uint8_t *)(str), sizeof((str)) - 1)

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
 * @brief Duplicate a string of 8-bit characters.
 * @param arena The arena to allocate the new string from.
 * @param str The string to duplicate.
 * @return A new string of 8-bit characters.
 */
String8 string8_duplicate(Arena *arena, const String8 *str);

/**
 * @brief Duplicate a null-terminated C-string.
 * @details Allocates a new String8 in the arena containing a copy of the input
 * C-string. The returned String8 is null-terminated (data[length] == '\0') and
 * has length equal to the number of characters (excluding the null terminator).
 * The data pointer points to the allocated memory in the arena.
 * @pre arena must not be NULL (asserts if NULL).
 * @pre cstr must not be NULL (asserts if NULL). NULL input is not handled
 * gracefully; the function will abort if cstr is NULL.
 * @param arena The arena to allocate the new string from.
 * @param cstr The C-string to duplicate (must not be NULL).
 * @return A new String8 with: (1) data pointing to allocated arena memory
 * containing the copied string, (2) length set to strlen(cstr) (excluding null
 * terminator), (3) data[length] == '\0' (null-terminated). For an empty string
 * (""), returns length==0 with data pointing to a single '\0' byte in the
 * arena.
 */
String8 vkr_string8_duplicate_cstr(Arena *arena, const char *cstr);

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
 * @brief Case-insensitive equality check for String8 (ASCII only)
 */
bool8_t string8_equalsi(const String8 *str1, const String8 *str2);

/**
 * @brief Trim leading and trailing ASCII whitespace in-place on a String8 view
 */
void string8_trim(String8 *s);

/**
 * @brief Check if a string equals a null-terminated C-string.
 * @param str The string to check.
 * @param cstr The null-terminated C-string to compare.
 * @return True if the string equals the C-string, false otherwise.
 */
bool8_t vkr_string8_equals_cstr(const String8 *str, const char *cstr);

/**
 * @brief Check if a string equals a null-terminated C-string ignoring case.
 * @param str The string to check.
 * @param cstr The null-terminated C-string to compare.
 * @return True if the string equals the C-string ignoring case, false
 * otherwise.
 */
bool8_t vkr_string8_equals_cstr_i(const String8 *str, const char *cstr);

/**
 * @brief Check if a String8 starts with a given prefix.
 * @param str The String8 to check.
 * @param prefix The prefix to search for (must be null-terminated).
 * @return True if the String8 starts with the prefix, false otherwise.
 * @details
 * Preconditions and edge cases:
 * - NULL arguments: If str is NULL or prefix is NULL, the implementation will
 *   assert in debug builds. Callers should validate pointers before calling.
 * - Empty prefix: An empty prefix ("") always returns true, regardless of str.
 * - Empty string: An empty str (str->length == 0) only matches an empty prefix;
 *   it returns false for any non-empty prefix.
 * - Null-termination: The prefix parameter must be null-terminated (uses
 * strlen). The str->str pointer does not need to be null-terminated; the
 * function relies on str->length to determine how many bytes to compare.
 * - Invariants: str->length must be consistent with str->str, meaning at least
 *   str->length valid bytes must be accessible at str->str. If str->str is
 * NULL, str->length must be 0.
 */
bool8_t vkr_string8_starts_with(const String8 *str, const char *prefix);

/**
 * @brief Create a trimmed suffix view of a String8 starting from the given
 * index.
 * @details Creates a view from start to the end of the string, then trims
 * leading and trailing whitespace. If start >= str->length, returns an empty
 * view. For arbitrary ranges without trimming, use string8_substring (line
 * 119).
 * @param str The String8 to create a suffix view from.
 * @param start The start byte offset (inclusive).
 * @return A trimmed String8 view of the suffix.
 */
String8 vkr_string8_trimmed_suffix(const String8 *str, uint64_t start);

/**
 * @brief Destroy a string of 8-bit characters.
 * @param str The string to destroy.
 */
void string8_destroy(String8 *str);

/////////////////////
// Native CString
/////////////////////

/**
 * @brief Check if two strings are equal.
 * @param str1 The first string.
 * @param str2 The second string.
 * @return True if the strings are equal, false otherwise.
 */
bool8_t string_equals(const char *str1, const char *str2);

/**
 * @brief Check if two strings are equal ignoring case.
 * @param str1 The first string.
 * @param str2 The second string.
 * @return True if the strings are equal, false otherwise.
 */
bool8_t string_equalsi(const char *str1, const char *str2);

/**
 * @brief Alias for case-insensitive equality on C-strings (ASCII only)
 */
static inline bool8_t string_equali(const char *a, const char *b) {
  return string_equalsi(a, b);
}

/**
 * @brief Get the length of a string.
 * @param str The string.
 * @return The length of the string.
 */
uint64_t string_length(const char *str);

/**
 * @brief Duplicate a string.
 * @param str The string to duplicate.
 * @return A pointer to the duplicated string.
 */
char *string_duplicate(const char *str);

/**
 * @brief Check if a string contains a substring.
 * @param str The string.
 * @param substring The substring to search for.
 * @return True if the string contains the substring, false otherwise.
 */
bool8_t string_contains(const char *str, const char *substring);

/**
 * @brief Get a substring of a string.
 * @param arena The arena to allocate the new string from.
 * @param str The string.
 * @param start The start index.
 * @param length The length of the substring.
 * @return A pointer to the substring.
 */
char *string_substring(Arena *arena, const char *str, int32_t start,
                       int32_t length);

/**
 * @brief Format a string into a destination buffer.
 * @param dest The destination buffer to write the formatted string to.
 * @param dest_size The size of the destination buffer in bytes (including space
 * for NUL).
 * @param format The format string.
 * @param ... The arguments to the format string.
 * @return The number of characters that would have been written (excluding
 * NUL). If the return value is >= dest_size, the output was truncated.
 */
int32_t string_format(char *dest, uint64_t dest_size, const char *format, ...);

/**
 * @brief Format a string from a va_list into a destination buffer.
 * @param dest The destination buffer to write the formatted string to.
 * @param dest_size The size of the destination buffer in bytes (including space
 * for NUL).
 * @param format The format string.
 * @param args The va_list of arguments.
 * @return The number of characters that would have been written (excluding
 * NUL). If the return value is >= dest_size, the output was truncated.
 */
int32_t string_format_v(char *dest, uint64_t dest_size, const char *format,
                        va_list args);

/**
 * @brief Empty a string.
 * @param str The string to empty.
 * @return A pointer to the empty string.
 */
char *string_empty(char *str);

/**
 * @brief Copy a string.
 * @param dest The destination string.
 * @param source The source string.
 * @return A pointer to the copied string.
 */
char *string_copy(char *dest, const char *source);

/**
 * @brief Copy a string with a length.
 * @param dest The destination string.
 * @param source The source string.
 * @param length The length of the string to copy.
 * @return A pointer to the copied string.
 */
char *string_ncopy(char *dest, const char *source, int64_t length);

/**
 * @brief Trim a string.
 * @param str The string to trim.
 * @return A pointer to the trimmed string.
 */
char *string_trim(char *str);

/**
 * @brief Copy a substring of a string.
 * @param dest The destination string.
 * @param source The source string.
 * @param start The start index.
 * @param length The length of the substring.
 */
void string_mid(char *dest, const char *source, int32_t start, int32_t length);

/**
 * @brief Get the index of a character in a string.
 * @param str The string.
 * @param c The character to search for.
 * @return The index of the character, -1 if not found.
 */
int32_t string_index_of(const char *str, char c);

/////////////////////
// Conversions (CString)
/////////////////////

/*
 * @brief Convert a string to a float64_t
 * @param s The string to convert.
 * @param out The float64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_f64(const char *s, float64_t *out);

/*
 * @brief Convert a string to a float32_t
 * @param s The string to convert.
 * @param out The float32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_f32(const char *s, float32_t *out);

/*
 * @brief Convert a string to a int64_t
 * @param s The string to convert.
 * @param out The int64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_i64(const char *s, int64_t *out);

/*
 * @brief Convert a string to a uint64_t
 * @param s The string to convert.
 * @param out The uint64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_u64(const char *s, uint64_t *out);

/*
 * @brief Convert a string to a int32_t
 * @param s The string to convert.
 * @param out The int32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_i32(const char *s, int32_t *out);

/*
 * @brief Convert a string to a uint32_t
 * @param s The string to convert.
 * @param out The uint32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_u32(const char *s, uint32_t *out);

/*
 * @brief Convert a string to a bool8_t
 * @param s The string to convert.
 * @param out The bool8_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_bool(const char *s, bool8_t *out);

/*
 * @brief Convert a string to a Vec2
 * @param s The string to convert.
 * @param out The Vec2 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_vec2(const char *s, Vec2 *out);

/*
 * @brief Convert a string to a Vec3
 * @param s The string to convert.
 * @param out The Vec3 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_vec3(const char *s, Vec3 *out);

/*
 * @brief Convert a string to a Vec4
 * @param s The string to convert.
 * @param out The Vec4 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string_to_vec4(const char *s, Vec4 *out);

/////////////////////
// Conversions (String8)
/////////////////////

/**
 * @brief Convert a string to a float64_t
 * @param s The string to convert.
 * @param out The float64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_f64(const String8 *s, float64_t *out);

/*
 * @brief Convert a string to a float32_t
 * @param s The string to convert.
 * @param out The float32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_f32(const String8 *s, float32_t *out);

/**
 * @brief Convert a string to a int64_t
 * @param s The string to convert.
 * @param out The int64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_i64(const String8 *s, int64_t *out);

/**
 * @brief Convert a string to a uint64_t
 * @param s The string to convert.
 * @param out The uint64_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_u64(const String8 *s, uint64_t *out);

/**
 * @brief Convert a string to a int32_t
 * @param s The string to convert.
 * @param out The int32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_i32(const String8 *s, int32_t *out);

/**
 * @brief Convert a string to a uint32_t
 * @param s The string to convert.
 * @param out The uint32_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_u32(const String8 *s, uint32_t *out);

/**
 * @brief Convert a string to a bool8_t
 * @param s The string to convert.
 * @param out The bool8_t to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_bool(const String8 *s, bool8_t *out);

/**
 * @brief Convert a string to a Vec2
 * @param s The string to convert.
 * @param out The Vec2 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_vec2(const String8 *s, Vec2 *out);

/**
 * @brief Convert a string to a Vec3
 * @param s The string to convert.
 * @param out The Vec3 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_vec3(const String8 *s, Vec3 *out);

/**
 * @brief Convert a string to a Vec4
 * @param s The string to convert.
 * @param out The Vec4 to store the result.
 * @return True if the conversion was successful, false otherwise.
 */
bool8_t string8_to_vec4(const String8 *s, Vec4 *out);

/**
 * @brief Extract the stem (filename without extension) from a file path.
 *
 * Given a file path, returns the filename component without the extension.
 * Handles both forward and backward slashes as path separators.
 *
 * @param arena Arena to allocate the result string from.
 * @param path The file path to extract stem from.
 * @return A new string containing the stem, or empty string if invalid.
 */
String8 string8_get_stem(Arena *arena, String8 path);

/**
 * @brief Split a string by whitespace into tokens.
 *
 * Parses the input string and splits it into tokens separated by spaces or
 * tabs. Stores up to max_tokens in the provided array.
 *
 * @param line The string to split.
 * @param tokens Array to store the resulting tokens.
 * @param max_tokens Maximum number of tokens to extract.
 * @return The number of tokens extracted.
 */
uint32_t string8_split_whitespace(const String8 *line, String8 *tokens,
                                  uint32_t max_tokens);