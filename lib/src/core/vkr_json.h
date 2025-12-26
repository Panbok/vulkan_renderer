#pragma once

#include "containers/str.h"
#include "defines.h"

// =============================================================================
// JSON Reader - Lightweight field-matching JSON parser
// =============================================================================

/**
 * @brief Lightweight JSON reader for field matching.
 *
 * This is NOT a full JSON parser. It provides simple field-matching
 * functionality for extracting specific values from JSON data.
 *
 * Usage:
 *   VkrJsonReader reader = vkr_json_reader_create(data, length);
 *   if (vkr_json_find_field(&reader, "fieldName")) {
 *     float32_t value;
 *     vkr_json_parse_float(&reader, &value);
 *   }
 */
typedef struct VkrJsonReader {
  const uint8_t *data; // JSON data buffer (not owned)
  uint64_t length;     // Length of data buffer
  uint64_t pos;        // Current read position
} VkrJsonReader;

// =============================================================================
// Creation
// =============================================================================

/**
 * @brief Creates a JSON reader from a data buffer.
 * @param data Pointer to JSON data (not copied, must remain valid)
 * @param length Length of the data buffer
 * @return Initialized JSON reader
 */
VkrJsonReader vkr_json_reader_create(const uint8_t *data, uint64_t length);

/**
 * @brief Creates a JSON reader from a String8.
 * @param str String containing JSON data
 * @return Initialized JSON reader
 */
VkrJsonReader vkr_json_reader_from_string(String8 str);

/**
 * @brief Resets reader position to start.
 * @param reader The reader to reset
 */
void vkr_json_reader_reset(VkrJsonReader *reader);

// =============================================================================
// Navigation
// =============================================================================

/**
 * @brief Skips whitespace at current position.
 * @param reader The reader
 */
void vkr_json_skip_whitespace(VkrJsonReader *reader);

/**
 * @brief Skips to a specific character.
 * @param reader The reader
 * @param target Character to skip to
 */
void vkr_json_skip_to(VkrJsonReader *reader, uint8_t target);

/**
 * @brief Finds a field by name and positions reader after ':'.
 * @param reader The reader
 * @param field_name Name of the field to find (without quotes)
 * @return true if field found, reader positioned at value
 */
bool8_t vkr_json_find_field(VkrJsonReader *reader, const char *field_name);

/**
 * @brief Finds an array field and positions reader at first element.
 * @param reader The reader
 * @param array_name Name of the array field
 * @return true if array found
 */
bool8_t vkr_json_find_array(VkrJsonReader *reader, const char *array_name);

/**
 * @brief Advances to the next object in an array.
 * @param reader The reader (must be inside an array)
 * @return true if next element found, false if end of array
 */
bool8_t vkr_json_next_array_element(VkrJsonReader *reader);

/**
 * @brief Creates a sub-reader for the current object scope.
 * @param reader The parent reader (must be at '{')
 * @param out_sub_reader Output sub-reader covering the object
 * @return true if object scope extracted
 */
bool8_t vkr_json_enter_object(VkrJsonReader *reader,
                              VkrJsonReader *out_sub_reader);

// =============================================================================
// Value Parsing
// =============================================================================

/**
 * @brief Parses a float value at current position.
 * @param reader The reader
 * @param out_value Output float value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_float(VkrJsonReader *reader, float32_t *out_value);

/**
 * @brief Parses a double value at current position.
 * @param reader The reader
 * @param out_value Output double value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_double(VkrJsonReader *reader, float64_t *out_value);

/**
 * @brief Parses an integer value at current position.
 * @param reader The reader
 * @param out_value Output integer value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_int(VkrJsonReader *reader, int32_t *out_value);

/**
 * @brief Parses a string value at current position.
 *
 * Returns a view into the original buffer (not copied).
 * The string does NOT include quotes.
 *
 * @param reader The reader
 * @param out_value Output string (points into original buffer)
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_string(VkrJsonReader *reader, String8 *out_value);

/**
 * @brief Parses a boolean value at current position.
 * @param reader The reader
 * @param out_value Output boolean value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_bool(VkrJsonReader *reader, bool8_t *out_value);

// =============================================================================
// Convenience Functions
// =============================================================================

/**
 * @brief Finds a field and parses its float value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_float(VkrJsonReader *reader, const char *field_name,
                           float32_t *out_value);

/**
 * @brief Finds a field and parses its integer value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_int(VkrJsonReader *reader, const char *field_name,
                         int32_t *out_value);

/**
 * @brief Finds a field and parses its string value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value (points into original buffer)
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_string(VkrJsonReader *reader, const char *field_name,
                            String8 *out_value);
