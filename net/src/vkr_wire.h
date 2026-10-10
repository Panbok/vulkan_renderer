#pragma once

#include "defines.h"

/* Schema messages (docs/proposals/network-protocol.md, "Data format"). A
 * schema is a set of C descriptor tables, the pattern of VkrTypeDesc: each
 * message describes a plain C struct field by field, and one table-driven
 * codec encodes and decodes every message. A message on the wire is its ID
 * (a bit varuint) followed by its fields in declaration order, bit-packed,
 * without names, tags or padding. The decoder checks every bound and refuses
 * the message on any violation; it never skips an unknown field.
 *
 * Struct storage per kind:
 *   BOOL                       bool8_t
 *   UINT, INT (bits <= 64)     uint64_t / int64_t
 *   VARUINT, NETID             uint64_t
 *   F32, QFLOAT                float32_t
 *   VEC3, VEC3Q                float32_t[3]
 *   QUAT                       float32_t[4] (x, y, z, w)
 *   ENUM                       uint32_t
 *   BYTES                      uint8_t[capacity] and uint32_t size at
 *                              `count_offset`
 *   UTF8                       char[capacity], null-terminated
 *   ARRAY                      elements of `stride` bytes and uint32_t count
 *                              at `count_offset`
 *   MESSAGE                    the nested struct
 *   ENTITY_REF                 uint8_t[16]
 *   HASH64                     uint64_t
 *   HASH256                    uint8_t[32]
 * An OPTIONAL field also has a bool8_t at `presence_offset`. */

typedef enum VkrWireKind {
  VKR_WIRE_BOOL = 1,
  VKR_WIRE_UINT,
  VKR_WIRE_INT,
  VKR_WIRE_VARUINT,
  VKR_WIRE_F32,
  VKR_WIRE_QFLOAT,
  VKR_WIRE_VEC3,
  VKR_WIRE_VEC3Q,
  VKR_WIRE_QUAT,
  VKR_WIRE_ENUM,
  VKR_WIRE_BYTES,
  VKR_WIRE_UTF8,
  VKR_WIRE_ARRAY,
  VKR_WIRE_MESSAGE,
  VKR_WIRE_NETID,
  VKR_WIRE_ENTITY_REF,
  VKR_WIRE_HASH64,
  VKR_WIRE_HASH256,
  VKR_WIRE_KIND_COUNT
} VkrWireKind;

typedef enum VkrWireFieldFlags {
  VKR_WIRE_FIELD_NONE = 0u,
  /* One presence bit; absent fields keep zero in the struct. */
  VKR_WIRE_FIELD_OPTIONAL = 1u << 0,
  /* F32 and VEC3 accept NaN and infinities. */
  VKR_WIRE_FIELD_NONFINITE = 1u << 1,
} VkrWireFieldFlags;

typedef struct VkrWireMessage VkrWireMessage;

typedef struct VkrWireField {
  /** For diagnostics only; names are not part of the wire or the hash. */
  const char *name;
  uint8_t kind;  /**< VkrWireKind. */
  uint8_t flags; /**< VkrWireFieldFlags. */
  /** UINT, INT: width 1 to 64. QFLOAT, VEC3Q: bits per value, 1 to 32.
      QUAT: bits per component, 2 to 31. */
  uint16_t bits;
  uint32_t offset;
  /** BYTES: byte capacity. UTF8: capacity including the terminator. ARRAY:
      element capacity. */
  uint32_t capacity;
  uint32_t count_offset;
  uint32_t presence_offset;
  /** QFLOAT, VEC3Q range. */
  float32_t min;
  float32_t max;
  /** ENUM: number of values. */
  uint32_t enum_count;
  /** MESSAGE: the nested message. */
  const VkrWireMessage *message;
  /** ARRAY: one element, its offset relative to the element start, and the
      element size. */
  const struct VkrWireField *element;
  uint32_t stride;
} VkrWireField;

struct VkrWireMessage {
  const char *name;
  /** Unique within its schema. */
  uint16_t id;
  /** sizeof the described struct. */
  uint32_t size;
  const VkrWireField *fields;
  uint32_t field_count;
};

typedef struct VkrWireSchema {
  const char *name;
  uint16_t version;
  const VkrWireMessage *const *messages;
  uint32_t message_count;
} VkrWireSchema;

typedef enum VkrWireStatus {
  VKR_WIRE_OK = 0,
  /* The output buffer is too small. */
  VKR_WIRE_NO_ROOM,
  /* No message of the schema has the ID. */
  VKR_WIRE_UNKNOWN_MESSAGE,
  /* A value breaks its field's bounds, or the bytes end early, run long or
     carry nonzero padding. */
  VKR_WIRE_VIOLATION,
} VkrWireStatus;

/* True when the descriptors are consistent: unique IDs, widths in range,
   storage inside each struct, nested messages and array elements valid. */
bool8_t vkr_wire_schema_validate(const VkrWireSchema *schema);

/* 64 bits of the SHA-256 of the schema's canonical form: version, message
   IDs and every field's kind, flags, widths, capacities, ranges and enum
   counts, in order. Names and struct offsets do not count. */
uint64_t vkr_wire_schema_hash(const VkrWireSchema *schema);

/* Encodes `value` as `message`. Stores the size in `out_size`. */
VkrWireStatus vkr_wire_encode(const VkrWireMessage *message, const void *value,
                              uint8_t *out, uint32_t capacity,
                              uint32_t *out_size);

/* Decodes one message of `schema` into `out_value`, which must hold the
   largest message struct of the schema (`capacity` bytes); fields it does
   not carry are zero. Stores the message's descriptor. */
VkrWireStatus vkr_wire_decode(const VkrWireSchema *schema, const uint8_t *data,
                              uint32_t size, void *out_value, uint32_t capacity,
                              const VkrWireMessage **out_message);

/* The message ID at the start of `data`, or UINT32_MAX. */
uint32_t vkr_wire_peek_id(const uint8_t *data, uint32_t size);

/* The largest struct size of the schema's messages. */
uint32_t vkr_wire_schema_max_size(const VkrWireSchema *schema);

/* True when `text[0..size)` is valid UTF-8 without NUL bytes. */
bool8_t vkr_wire_utf8_valid(const uint8_t *text, uint32_t size);
