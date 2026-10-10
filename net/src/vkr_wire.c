#include "vkr_wire.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "vkr_bitstream.h"

#include <math.h>
#include <string.h>

/* Nested messages and arrays may go this deep; descriptors are static, so
   this bounds a schema mistake, not data. */
#define WIRE_DEPTH_MAX 8u
/* Quaternion components other than the largest lie within ±1/sqrt(2). */
#define WIRE_QUAT_RANGE 0.70710678118654752f

// =============================================================================
// Descriptor checks
// =============================================================================

/* Bytes a field occupies in its struct. */
static uint64_t wire_storage(const VkrWireField *field) {
  switch (field->kind) {
  case VKR_WIRE_BOOL:
    return sizeof(bool8_t);
  case VKR_WIRE_UINT:
  case VKR_WIRE_INT:
  case VKR_WIRE_VARUINT:
  case VKR_WIRE_NETID:
  case VKR_WIRE_HASH64:
    return 8u;
  case VKR_WIRE_F32:
  case VKR_WIRE_QFLOAT:
  case VKR_WIRE_ENUM:
    return 4u;
  case VKR_WIRE_VEC3:
  case VKR_WIRE_VEC3Q:
    return 12u;
  case VKR_WIRE_QUAT:
    return 16u;
  case VKR_WIRE_BYTES:
  case VKR_WIRE_UTF8:
    return field->capacity;
  case VKR_WIRE_ARRAY:
    return (uint64_t)field->capacity * field->stride;
  case VKR_WIRE_MESSAGE:
    return field->message ? field->message->size : 0u;
  case VKR_WIRE_ENTITY_REF:
    return 16u;
  case VKR_WIRE_HASH256:
    return 32u;
  default:
    return 0u;
  }
}

static bool8_t wire_message_validate(const VkrWireMessage *message,
                                     uint32_t depth);

static bool8_t wire_field_validate(const VkrWireField *field, uint64_t size,
                                   uint32_t depth) {
  if (field->kind == 0u || field->kind >= VKR_WIRE_KIND_COUNT) {
    return false_v;
  }
  if ((uint64_t)field->offset + wire_storage(field) > size) {
    return false_v;
  }
  if ((field->flags & VKR_WIRE_FIELD_OPTIONAL) &&
      (uint64_t)field->presence_offset + sizeof(bool8_t) > size) {
    return false_v;
  }
  switch (field->kind) {
  case VKR_WIRE_UINT:
  case VKR_WIRE_INT:
    return field->bits >= 1u && field->bits <= 64u;
  case VKR_WIRE_QFLOAT:
  case VKR_WIRE_VEC3Q:
    return field->bits >= 1u && field->bits <= 32u && isfinite(field->min) &&
           isfinite(field->max) && field->min < field->max;
  case VKR_WIRE_QUAT:
    return field->bits >= 2u && field->bits <= 31u;
  case VKR_WIRE_ENUM:
    return field->enum_count >= 1u;
  case VKR_WIRE_BYTES:
  case VKR_WIRE_UTF8:
    return field->capacity >= 1u &&
           (field->kind == VKR_WIRE_UTF8 ||
            (uint64_t)field->count_offset + 4u <= size);
  case VKR_WIRE_ARRAY:
    return field->capacity >= 1u && field->element && field->stride > 0u &&
           (uint64_t)field->count_offset + 4u <= size &&
           depth < WIRE_DEPTH_MAX &&
           !(field->element->flags & VKR_WIRE_FIELD_OPTIONAL) &&
           wire_field_validate(field->element, field->stride, depth + 1u);
  case VKR_WIRE_MESSAGE:
    return field->message && depth < WIRE_DEPTH_MAX &&
           wire_message_validate(field->message, depth + 1u);
  default:
    return true_v;
  }
}

static bool8_t wire_message_validate(const VkrWireMessage *message,
                                     uint32_t depth) {
  if (!message || message->size == 0u ||
      (message->field_count > 0u && !message->fields)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < message->field_count; ++i) {
    if (!wire_field_validate(&message->fields[i], message->size, depth)) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_wire_schema_validate(const VkrWireSchema *schema) {
  if (!schema || (schema->message_count > 0u && !schema->messages)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < schema->message_count; ++i) {
    const VkrWireMessage *message = schema->messages[i];
    if (!wire_message_validate(message, 0u)) {
      return false_v;
    }
    for (uint32_t j = 0u; j < i; ++j) {
      if (schema->messages[j]->id == message->id) {
        return false_v;
      }
    }
  }
  return true_v;
}

uint32_t vkr_wire_schema_max_size(const VkrWireSchema *schema) {
  uint32_t size = 0u;
  for (uint32_t i = 0u; i < schema->message_count; ++i) {
    size = Max(size, schema->messages[i]->size);
  }
  return size;
}

// =============================================================================
// Schema hash
// =============================================================================

static void wire_hash_u32(VkrSha256 *hash, uint32_t value) {
  uint8_t bytes[4];
  vkr_store_le_u32(bytes, value);
  vkr_sha256_update(hash, bytes, sizeof(bytes));
}

static void wire_hash_message(VkrSha256 *hash, const VkrWireMessage *message);

static void wire_hash_field(VkrSha256 *hash, const VkrWireField *field) {
  wire_hash_u32(hash, field->kind);
  wire_hash_u32(hash, field->flags);
  wire_hash_u32(hash, field->bits);
  const bool8_t sized = field->kind == VKR_WIRE_BYTES ||
                        field->kind == VKR_WIRE_UTF8 ||
                        field->kind == VKR_WIRE_ARRAY;
  wire_hash_u32(hash, sized ? field->capacity : 0u);
  wire_hash_u32(hash, field->kind == VKR_WIRE_ENUM ? field->enum_count : 0u);
  const bool8_t ranged =
      field->kind == VKR_WIRE_QFLOAT || field->kind == VKR_WIRE_VEC3Q;
  wire_hash_u32(hash, ranged ? vkr_f32_bits(field->min) : 0u);
  wire_hash_u32(hash, ranged ? vkr_f32_bits(field->max) : 0u);
  if (field->kind == VKR_WIRE_MESSAGE) {
    wire_hash_message(hash, field->message);
  } else if (field->kind == VKR_WIRE_ARRAY) {
    wire_hash_field(hash, field->element);
  }
}

static void wire_hash_message(VkrSha256 *hash, const VkrWireMessage *message) {
  wire_hash_u32(hash, message->id);
  wire_hash_u32(hash, message->field_count);
  for (uint32_t i = 0u; i < message->field_count; ++i) {
    wire_hash_field(hash, &message->fields[i]);
  }
}

uint64_t vkr_wire_schema_hash(const VkrWireSchema *schema) {
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  static const uint8_t label[] = "vkr wire schema 1";
  vkr_sha256_update(&hash, label, sizeof(label) - 1u);
  wire_hash_u32(&hash, schema->version);
  wire_hash_u32(&hash, schema->message_count);
  /* Canonical order: by message ID. */
  uint32_t previous = 0u;
  for (uint32_t emitted = 0u; emitted < schema->message_count; ++emitted) {
    const VkrWireMessage *next = NULL;
    for (uint32_t i = 0u; i < schema->message_count; ++i) {
      const VkrWireMessage *message = schema->messages[i];
      if ((emitted == 0u || message->id > previous) &&
          (!next || message->id < next->id)) {
        next = message;
      }
    }
    if (!next) {
      break;
    }
    wire_hash_message(&hash, next);
    previous = next->id;
  }
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(&hash, digest);
  return vkr_load_le_u64(digest);
}

// =============================================================================
// Values
// =============================================================================

bool8_t vkr_wire_utf8_valid(const uint8_t *text, uint32_t size) {
  uint32_t i = 0u;
  while (i < size) {
    const uint8_t c = text[i];
    uint32_t extra = 0u;
    uint32_t codepoint = 0u;
    if (c == 0u) {
      return false_v;
    }
    if (c < 0x80u) {
      i += 1u;
      continue;
    }
    if ((c & 0xe0u) == 0xc0u) {
      extra = 1u;
      codepoint = c & 0x1fu;
    } else if ((c & 0xf0u) == 0xe0u) {
      extra = 2u;
      codepoint = c & 0x0fu;
    } else if ((c & 0xf8u) == 0xf0u) {
      extra = 3u;
      codepoint = c & 0x07u;
    } else {
      return false_v;
    }
    if (size - i <= extra) {
      return false_v;
    }
    for (uint32_t k = 1u; k <= extra; ++k) {
      const uint8_t next = text[i + k];
      if ((next & 0xc0u) != 0x80u) {
        return false_v;
      }
      codepoint = (codepoint << 6) | (next & 0x3fu);
    }
    /* Overlong forms, surrogates and values past U+10FFFF. */
    static const uint32_t minimum[4] = {0u, 0x80u, 0x800u, 0x10000u};
    if (codepoint < minimum[extra] || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
      return false_v;
    }
    i += extra + 1u;
  }
  return true_v;
}

static uint32_t wire_enum_bits(uint32_t count) {
  uint32_t bits = 0u;
  while (bits < 32u && (1ull << bits) < count) {
    ++bits;
  }
  return bits;
}

static uint64_t wire_bits_max(uint32_t bits) {
  return bits >= 64u ? UINT64_MAX : (1ull << bits) - 1u;
}

static bool8_t wire_quantize(float32_t value, float32_t min, float32_t max,
                             uint32_t bits, uint64_t *out) {
  if (!isfinite(value) || value < min || value > max) {
    return false_v;
  }
  const float64_t steps = (float64_t)wire_bits_max(bits);
  const float64_t unit = ((float64_t)value - min) / ((float64_t)max - min);
  *out = (uint64_t)floor(unit * steps + 0.5);
  return true_v;
}

static float32_t wire_dequantize(uint64_t q, float32_t min, float32_t max,
                                 uint32_t bits) {
  const float64_t steps = (float64_t)wire_bits_max(bits);
  return (float32_t)((float64_t)min +
                     ((float64_t)max - min) * ((float64_t)q / steps));
}

// =============================================================================
// Encoding
// =============================================================================

static bool8_t wire_encode_fields(VkrBitWriter *writer,
                                  const VkrWireMessage *message,
                                  const uint8_t *base);

static bool8_t wire_encode_f32(VkrBitWriter *writer, float32_t value,
                               bool8_t nonfinite) {
  if (!nonfinite && !isfinite(value)) {
    return false_v;
  }
  vkr_bit_write(writer, vkr_f32_bits(value), 32u);
  return true_v;
}

static bool8_t wire_encode_quat(VkrBitWriter *writer, const float32_t q[4],
                                uint32_t bits) {
  float64_t length = 0.0;
  uint32_t largest = 0u;
  for (uint32_t i = 0u; i < 4u; ++i) {
    if (!isfinite(q[i])) {
      return false_v;
    }
    length += (float64_t)q[i] * q[i];
    if (fabsf(q[i]) > fabsf(q[largest])) {
      largest = i;
    }
  }
  if (fabs(length - 1.0) > 1e-3) {
    return false_v;
  }
  /* q and -q are one rotation: make the dropped component positive. */
  const float32_t sign = q[largest] < 0.0f ? -1.0f : 1.0f;
  vkr_bit_write(writer, largest, 2u);
  for (uint32_t i = 0u; i < 4u; ++i) {
    if (i == largest) {
      continue;
    }
    const float32_t value =
        Clamp(q[i] * sign, -WIRE_QUAT_RANGE, WIRE_QUAT_RANGE);
    uint64_t quantized = 0u;
    (void)wire_quantize(value, -WIRE_QUAT_RANGE, WIRE_QUAT_RANGE, bits,
                        &quantized);
    vkr_bit_write(writer, quantized, bits);
  }
  return true_v;
}

static bool8_t wire_encode_field(VkrBitWriter *writer,
                                 const VkrWireField *field,
                                 const uint8_t *base) {
  if (field->flags & VKR_WIRE_FIELD_OPTIONAL) {
    const bool8_t present = base[field->presence_offset];
    if (present > 1u) {
      return false_v;
    }
    vkr_bit_write(writer, present, 1u);
    if (!present) {
      return true_v;
    }
  }
  const uint8_t *value = base + field->offset;
  const bool8_t nonfinite = (field->flags & VKR_WIRE_FIELD_NONFINITE) != 0u;
  switch (field->kind) {
  case VKR_WIRE_BOOL:
    if (*value > 1u) {
      return false_v;
    }
    vkr_bit_write(writer, *value, 1u);
    return true_v;
  case VKR_WIRE_UINT: {
    uint64_t v = 0u;
    memcpy(&v, value, 8u);
    if (v > wire_bits_max(field->bits)) {
      return false_v;
    }
    vkr_bit_write(writer, v, field->bits);
    return true_v;
  }
  case VKR_WIRE_INT: {
    int64_t v = 0;
    memcpy(&v, value, 8u);
    const uint64_t zigzag = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
    if (zigzag > wire_bits_max(field->bits)) {
      return false_v;
    }
    vkr_bit_write(writer, zigzag, field->bits);
    return true_v;
  }
  case VKR_WIRE_VARUINT:
  case VKR_WIRE_NETID: {
    uint64_t v = 0u;
    memcpy(&v, value, 8u);
    vkr_bit_write_varuint(writer, v);
    return true_v;
  }
  case VKR_WIRE_HASH64: {
    uint64_t v = 0u;
    memcpy(&v, value, 8u);
    vkr_bit_write(writer, v, 64u);
    return true_v;
  }
  case VKR_WIRE_F32: {
    float32_t v = 0.0f;
    memcpy(&v, value, 4u);
    return wire_encode_f32(writer, v, nonfinite);
  }
  case VKR_WIRE_VEC3: {
    float32_t v[3];
    memcpy(v, value, sizeof(v));
    for (uint32_t i = 0u; i < 3u; ++i) {
      if (!wire_encode_f32(writer, v[i], nonfinite)) {
        return false_v;
      }
    }
    return true_v;
  }
  case VKR_WIRE_QFLOAT:
  case VKR_WIRE_VEC3Q: {
    const uint32_t count = field->kind == VKR_WIRE_QFLOAT ? 1u : 3u;
    float32_t v[3];
    memcpy(v, value, count * sizeof(float32_t));
    for (uint32_t i = 0u; i < count; ++i) {
      uint64_t quantized = 0u;
      if (!wire_quantize(v[i], field->min, field->max, field->bits,
                         &quantized)) {
        return false_v;
      }
      vkr_bit_write(writer, quantized, field->bits);
    }
    return true_v;
  }
  case VKR_WIRE_QUAT: {
    float32_t q[4];
    memcpy(q, value, sizeof(q));
    return wire_encode_quat(writer, q, field->bits);
  }
  case VKR_WIRE_ENUM: {
    uint32_t v = 0u;
    memcpy(&v, value, 4u);
    if (v >= field->enum_count) {
      return false_v;
    }
    vkr_bit_write(writer, v, wire_enum_bits(field->enum_count));
    return true_v;
  }
  case VKR_WIRE_BYTES: {
    uint32_t size = 0u;
    memcpy(&size, base + field->count_offset, 4u);
    if (size > field->capacity) {
      return false_v;
    }
    vkr_bit_write_varuint(writer, size);
    vkr_bit_write_bytes(writer, value, size);
    return true_v;
  }
  case VKR_WIRE_UTF8: {
    const uint8_t *terminator = memchr(value, 0, field->capacity);
    if (!terminator) {
      return false_v;
    }
    const uint32_t size = (uint32_t)(terminator - value);
    if (!vkr_wire_utf8_valid(value, size)) {
      return false_v;
    }
    vkr_bit_write_varuint(writer, size);
    vkr_bit_write_bytes(writer, value, size);
    return true_v;
  }
  case VKR_WIRE_ARRAY: {
    uint32_t count = 0u;
    memcpy(&count, base + field->count_offset, 4u);
    if (count > field->capacity) {
      return false_v;
    }
    vkr_bit_write_varuint(writer, count);
    for (uint32_t i = 0u; i < count; ++i) {
      if (!wire_encode_field(writer, field->element,
                             value + (uint64_t)i * field->stride)) {
        return false_v;
      }
    }
    return true_v;
  }
  case VKR_WIRE_MESSAGE:
    return wire_encode_fields(writer, field->message, value);
  case VKR_WIRE_ENTITY_REF:
  case VKR_WIRE_HASH256: {
    const uint32_t words = field->kind == VKR_WIRE_ENTITY_REF ? 2u : 4u;
    for (uint32_t i = 0u; i < words; ++i) {
      vkr_bit_write(writer, vkr_load_le_u64(value + 8u * i), 64u);
    }
    return true_v;
  }
  default:
    return false_v;
  }
}

static bool8_t wire_encode_fields(VkrBitWriter *writer,
                                  const VkrWireMessage *message,
                                  const uint8_t *base) {
  for (uint32_t i = 0u; i < message->field_count; ++i) {
    if (!wire_encode_field(writer, &message->fields[i], base)) {
      return false_v;
    }
  }
  return true_v;
}

VkrWireStatus vkr_wire_encode(const VkrWireMessage *message, const void *value,
                              uint8_t *out, uint32_t capacity,
                              uint32_t *out_size) {
  if (!message || !value || !out || !out_size) {
    return VKR_WIRE_VIOLATION;
  }
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, out, capacity);
  vkr_bit_write_varuint(&writer, message->id);
  if (!wire_encode_fields(&writer, message, value)) {
    return VKR_WIRE_VIOLATION;
  }
  const uint32_t size = vkr_bit_writer_finish(&writer);
  if (size == 0u) {
    return VKR_WIRE_NO_ROOM;
  }
  *out_size = size;
  return VKR_WIRE_OK;
}

// =============================================================================
// Decoding
// =============================================================================

static bool8_t wire_decode_fields(VkrBitReader *reader,
                                  const VkrWireMessage *message, uint8_t *base);

/* A varuint in its smallest size class: one value has one encoding. */
static bool8_t wire_read_varuint(VkrBitReader *reader, uint64_t *out) {
  const uint32_t size_class = (uint32_t)vkr_bit_read(reader, 2u);
  const uint64_t value = vkr_bit_read(reader, 8u << size_class);
  if (size_class > 0u && value <= wire_bits_max(8u << (size_class - 1u))) {
    return false_v;
  }
  *out = value;
  return !reader->overflow;
}

static bool8_t wire_decode_f32(VkrBitReader *reader, float32_t *out,
                               bool8_t nonfinite) {
  const float32_t value =
      vkr_f32_from_bits((uint32_t)vkr_bit_read(reader, 32u));
  if (!nonfinite && !isfinite(value)) {
    return false_v;
  }
  *out = value;
  return true_v;
}

static bool8_t wire_decode_field(VkrBitReader *reader,
                                 const VkrWireField *field, uint8_t *base) {
  if (field->flags & VKR_WIRE_FIELD_OPTIONAL) {
    const bool8_t present = (bool8_t)vkr_bit_read(reader, 1u);
    base[field->presence_offset] = present;
    if (!present) {
      return !reader->overflow;
    }
  }
  uint8_t *value = base + field->offset;
  const bool8_t nonfinite = (field->flags & VKR_WIRE_FIELD_NONFINITE) != 0u;
  switch (field->kind) {
  case VKR_WIRE_BOOL:
    *value = (uint8_t)vkr_bit_read(reader, 1u);
    break;
  case VKR_WIRE_UINT: {
    const uint64_t v = vkr_bit_read(reader, field->bits);
    memcpy(value, &v, 8u);
    break;
  }
  case VKR_WIRE_INT: {
    const uint64_t zigzag = vkr_bit_read(reader, field->bits);
    const int64_t v = (int64_t)(zigzag >> 1) ^ -(int64_t)(zigzag & 1u);
    memcpy(value, &v, 8u);
    break;
  }
  case VKR_WIRE_VARUINT:
  case VKR_WIRE_NETID: {
    uint64_t v = 0u;
    if (!wire_read_varuint(reader, &v)) {
      return false_v;
    }
    memcpy(value, &v, 8u);
    break;
  }
  case VKR_WIRE_HASH64: {
    const uint64_t v = vkr_bit_read(reader, 64u);
    memcpy(value, &v, 8u);
    break;
  }
  case VKR_WIRE_F32: {
    float32_t v = 0.0f;
    if (!wire_decode_f32(reader, &v, nonfinite)) {
      return false_v;
    }
    memcpy(value, &v, 4u);
    break;
  }
  case VKR_WIRE_VEC3: {
    float32_t v[3];
    for (uint32_t i = 0u; i < 3u; ++i) {
      if (!wire_decode_f32(reader, &v[i], nonfinite)) {
        return false_v;
      }
    }
    memcpy(value, v, sizeof(v));
    break;
  }
  case VKR_WIRE_QFLOAT:
  case VKR_WIRE_VEC3Q: {
    const uint32_t count = field->kind == VKR_WIRE_QFLOAT ? 1u : 3u;
    float32_t v[3];
    for (uint32_t i = 0u; i < count; ++i) {
      v[i] = wire_dequantize(vkr_bit_read(reader, field->bits), field->min,
                             field->max, field->bits);
    }
    memcpy(value, v, count * sizeof(float32_t));
    break;
  }
  case VKR_WIRE_QUAT: {
    const uint32_t largest = (uint32_t)vkr_bit_read(reader, 2u);
    float32_t q[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float64_t sum = 0.0;
    for (uint32_t i = 0u; i < 4u; ++i) {
      if (i == largest) {
        continue;
      }
      q[i] = wire_dequantize(vkr_bit_read(reader, field->bits),
                             -WIRE_QUAT_RANGE, WIRE_QUAT_RANGE, field->bits);
      sum += (float64_t)q[i] * q[i];
    }
    if (sum > 1.0 + 1e-4) {
      return false_v;
    }
    q[largest] = (float32_t)sqrt(Max(0.0, 1.0 - sum));
    memcpy(value, q, sizeof(q));
    break;
  }
  case VKR_WIRE_ENUM: {
    const uint32_t v =
        (uint32_t)vkr_bit_read(reader, wire_enum_bits(field->enum_count));
    if (v >= field->enum_count) {
      return false_v;
    }
    memcpy(value, &v, 4u);
    break;
  }
  case VKR_WIRE_BYTES:
  case VKR_WIRE_UTF8: {
    uint64_t size = 0u;
    if (!wire_read_varuint(reader, &size)) {
      return false_v;
    }
    /* UTF8 keeps room for its terminator. */
    const uint64_t limit = field->kind == VKR_WIRE_UTF8
                               ? (uint64_t)field->capacity - 1u
                               : field->capacity;
    if (size > limit) {
      return false_v;
    }
    const uint8_t *bytes = vkr_bit_read_bytes(reader, (uint32_t)size);
    if (!bytes) {
      return false_v;
    }
    if (field->kind == VKR_WIRE_UTF8 &&
        !vkr_wire_utf8_valid(bytes, (uint32_t)size)) {
      return false_v;
    }
    memcpy(value, bytes, (size_t)size);
    if (field->kind == VKR_WIRE_UTF8) {
      value[size] = 0u;
    } else {
      const uint32_t stored = (uint32_t)size;
      memcpy(base + field->count_offset, &stored, 4u);
    }
    break;
  }
  case VKR_WIRE_ARRAY: {
    uint64_t count = 0u;
    if (!wire_read_varuint(reader, &count) || count > field->capacity) {
      return false_v;
    }
    const uint32_t stored = (uint32_t)count;
    memcpy(base + field->count_offset, &stored, 4u);
    for (uint32_t i = 0u; i < stored; ++i) {
      if (!wire_decode_field(reader, field->element,
                             value + (uint64_t)i * field->stride)) {
        return false_v;
      }
    }
    break;
  }
  case VKR_WIRE_MESSAGE:
    return wire_decode_fields(reader, field->message, value);
  case VKR_WIRE_ENTITY_REF:
  case VKR_WIRE_HASH256: {
    const uint32_t words = field->kind == VKR_WIRE_ENTITY_REF ? 2u : 4u;
    for (uint32_t i = 0u; i < words; ++i) {
      vkr_store_le_u64(value + 8u * i, vkr_bit_read(reader, 64u));
    }
    break;
  }
  default:
    return false_v;
  }
  return !reader->overflow;
}

static bool8_t wire_decode_fields(VkrBitReader *reader,
                                  const VkrWireMessage *message,
                                  uint8_t *base) {
  for (uint32_t i = 0u; i < message->field_count; ++i) {
    if (!wire_decode_field(reader, &message->fields[i], base)) {
      return false_v;
    }
  }
  return true_v;
}

uint32_t vkr_wire_peek_id(const uint8_t *data, uint32_t size) {
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, data, size);
  uint64_t id = 0u;
  if (!wire_read_varuint(&reader, &id) || id > 0xffffu) {
    return UINT32_MAX;
  }
  return (uint32_t)id;
}

VkrWireStatus vkr_wire_decode(const VkrWireSchema *schema, const uint8_t *data,
                              uint32_t size, void *out_value, uint32_t capacity,
                              const VkrWireMessage **out_message) {
  if (!schema || !data || !out_value) {
    return VKR_WIRE_VIOLATION;
  }
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, data, size);
  uint64_t id = 0u;
  if (!wire_read_varuint(&reader, &id)) {
    return VKR_WIRE_VIOLATION;
  }
  const VkrWireMessage *message = NULL;
  for (uint32_t i = 0u; i < schema->message_count; ++i) {
    if (schema->messages[i]->id == id) {
      message = schema->messages[i];
      break;
    }
  }
  if (!message) {
    return VKR_WIRE_UNKNOWN_MESSAGE;
  }
  if (message->size > capacity) {
    return VKR_WIRE_NO_ROOM;
  }
  MemZero(out_value, message->size);
  if (!wire_decode_fields(&reader, message, out_value)) {
    return VKR_WIRE_VIOLATION;
  }
  vkr_bit_read_align(&reader);
  if (!vkr_bit_reader_at_end(&reader)) {
    return VKR_WIRE_VIOLATION;
  }
  if (out_message) {
    *out_message = message;
  }
  return VKR_WIRE_OK;
}
