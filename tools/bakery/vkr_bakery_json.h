#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/arena.h"

typedef struct VkrBakeryBuffer VkrBakeryBuffer;

/* Mutable JSON document tree for recipes, manifests, action records and the
 * managed project documents. Every node and string is allocated from one
 * caller-owned arena and lives until that arena is reset or destroyed; nodes
 * are never freed individually. Objects keep insertion order, and assigning an
 * existing key replaces its value in place, matching Python dict semantics. */

typedef enum VkrBakeryJsonType {
  VKR_BAKERY_JSON_NULL = 0,
  VKR_BAKERY_JSON_BOOL,
  VKR_BAKERY_JSON_INT,
  VKR_BAKERY_JSON_FLOAT,
  VKR_BAKERY_JSON_STRING,
  VKR_BAKERY_JSON_ARRAY,
  VKR_BAKERY_JSON_OBJECT,
} VkrBakeryJsonType;

typedef struct VkrBakeryJson VkrBakeryJson;
struct VkrBakeryJson {
  VkrBakeryJsonType type;
  bool8_t boolean;
  int64_t integer;
  float64_t number;
  String8 string; /* STRING value; not null-terminated by contract. */
  String8 key;    /* Member name when the node belongs to an object. */
  VkrBakeryJson *first;
  VkrBakeryJson *last;
  VkrBakeryJson *next;
  uint32_t count;
};

typedef struct VkrBakeryJsonError {
  uint64_t offset;
  uint32_t line;
  uint32_t column;
  char message[96];
} VkrBakeryJsonError;

/** Parses UTF-8 JSON. Nesting deeper than `max_depth` fails. */
VkrBakeryJson *vkr_bakery_json_parse(Arena *arena, const uint8_t *data,
                                     uint64_t length, uint32_t max_depth,
                                     VkrBakeryJsonError *out_error);

typedef enum VkrBakeryJsonParseFlags {
  /** Fail on a repeated object member instead of keeping the last one. */
  VKR_BAKERY_JSON_REJECT_DUPLICATES = 1u << 0,
} VkrBakeryJsonParseFlags;

VkrBakeryJson *vkr_bakery_json_parse_ex(Arena *arena, const uint8_t *data,
                                        uint64_t length, uint32_t max_depth,
                                        uint32_t flags,
                                        VkrBakeryJsonError *out_error);

VkrBakeryJson *vkr_bakery_json_null(Arena *arena);
VkrBakeryJson *vkr_bakery_json_bool(Arena *arena, bool8_t value);
VkrBakeryJson *vkr_bakery_json_int(Arena *arena, int64_t value);
VkrBakeryJson *vkr_bakery_json_float(Arena *arena, float64_t value);
/** Copies `value` into the arena. */
VkrBakeryJson *vkr_bakery_json_string(Arena *arena, String8 value);
VkrBakeryJson *vkr_bakery_json_cstr(Arena *arena, const char *value);
VkrBakeryJson *vkr_bakery_json_array(Arena *arena);
VkrBakeryJson *vkr_bakery_json_object(Arena *arena);

/** Deep copy into `arena`. */
VkrBakeryJson *vkr_bakery_json_clone(Arena *arena, const VkrBakeryJson *value);

void vkr_bakery_json_append(VkrBakeryJson *array, VkrBakeryJson *value);
/** Sets or replaces a member; the key is copied into the arena. */
void vkr_bakery_json_set(Arena *arena, VkrBakeryJson *object, const char *key,
                         VkrBakeryJson *value);
bool8_t vkr_bakery_json_remove(VkrBakeryJson *object, const char *key);

VkrBakeryJson *vkr_bakery_json_get(const VkrBakeryJson *object,
                                   const char *key);
VkrBakeryJson *vkr_bakery_json_at(const VkrBakeryJson *array, uint32_t index);
/** Typed member reads; each returns false when absent or of another type. */
bool8_t vkr_bakery_json_get_string(const VkrBakeryJson *object, const char *key,
                                   String8 *out_value);
bool8_t vkr_bakery_json_get_int(const VkrBakeryJson *object, const char *key,
                                int64_t *out_value);
bool8_t vkr_bakery_json_get_number(const VkrBakeryJson *object, const char *key,
                                   float64_t *out_value);
bool8_t vkr_bakery_json_get_bool(const VkrBakeryJson *object, const char *key,
                                 bool8_t *out_value);
bool8_t vkr_bakery_json_is_string(const VkrBakeryJson *value, const char *text);
/** Null-terminated arena copy of a STRING value, or NULL. */
const char *vkr_bakery_json_cstr_value(Arena *arena,
                                       const VkrBakeryJson *value);
bool8_t vkr_bakery_json_equal(const VkrBakeryJson *lhs,
                              const VkrBakeryJson *rhs);

typedef enum VkrBakeryJsonStyle {
  /** Python json.dump(indent=2, ensure_ascii=False). */
  VKR_BAKERY_JSON_PRETTY = 0,
  /** One line, sorted keys, no spaces: the canonical key encoding. */
  VKR_BAKERY_JSON_CANONICAL,
  /** One line, insertion order, no spaces: event and record lines. */
  VKR_BAKERY_JSON_COMPACT,
  /** Python json.dumps(sort_keys=True): ", " and ": " separators and
   * \uXXXX escapes for non-ASCII text. */
  VKR_BAKERY_JSON_PYTHON_SORTED,
  /** Python json.dumps(separators=(",", ":")): insertion order, no spaces,
   * \uXXXX escapes for non-ASCII text. */
  VKR_BAKERY_JSON_PYTHON_COMPACT,
} VkrBakeryJsonStyle;

/** Serializes into the arena. Non-finite floats fail, as allow_nan=False. */
bool8_t vkr_bakery_json_write(Arena *arena, const VkrBakeryJson *value,
                              VkrBakeryJsonStyle style, String8 *out_text);

/** Appends Python-repr text for a finite double. Returns bytes written. */
uint32_t vkr_bakery_json_format_float(float64_t value, char *out,
                                      uint32_t capacity);

/** Appends `value` as a quoted JSON string with Python's escaping rules. */
void vkr_bakery_json_write_string(VkrBakeryBuffer *buffer, String8 value);
