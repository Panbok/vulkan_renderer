#include "net_wire_test.h"

#include "vkr_bitstream.h"
#include "vkr_wire.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The bit stream and the schema codec of the network data format
 * (docs/proposals/network-protocol.md, "Data format"). Each test names the
 * failure it detects. */

static uint64_t wire_test_state = 0x1234u;

static uint64_t wire_test_random(void) {
  uint64_t z = (wire_test_state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

/* Fails when values of random widths, varuints and aligned bytes do not
   read back as written, or when a writer or reader past its end does not
   report the overflow. */
static void test_bitstream_round_trip(void) {
  printf("  Running test_bitstream_round_trip...\n");
  static uint8_t buffer[64u * 1024u];
  uint64_t values[2000];
  uint32_t widths[2000];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, buffer, sizeof(buffer));
  for (uint32_t i = 0u; i < 2000u; ++i) {
    widths[i] = (uint32_t)(wire_test_random() % 64u) + 1u;
    values[i] = wire_test_random();
    if (widths[i] < 64u) {
      values[i] &= (1ull << widths[i]) - 1u;
    }
    if (i % 7u == 0u) {
      vkr_bit_write_varuint(&writer, values[i]);
    } else {
      vkr_bit_write(&writer, values[i], widths[i]);
    }
  }
  const uint8_t bytes[5] = {1, 2, 3, 4, 5};
  vkr_bit_write_bytes(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, 5u, 3u);
  const uint32_t size = vkr_bit_writer_finish(&writer);
  assert(size > 0u);

  VkrBitReader reader;
  vkr_bit_reader_init(&reader, buffer, size);
  for (uint32_t i = 0u; i < 2000u; ++i) {
    const uint64_t value = i % 7u == 0u ? vkr_bit_read_varuint(&reader)
                                        : vkr_bit_read(&reader, widths[i]);
    assert(value == values[i]);
  }
  const uint8_t *view = vkr_bit_read_bytes(&reader, sizeof(bytes));
  assert(view && memcmp(view, bytes, sizeof(bytes)) == 0);
  assert(vkr_bit_read(&reader, 3u) == 5u);
  vkr_bit_read_align(&reader);
  assert(vkr_bit_reader_at_end(&reader));
  assert(vkr_bit_read(&reader, 1u) == 0u && reader.overflow);

  uint8_t tiny[2];
  vkr_bit_writer_init(&writer, tiny, sizeof(tiny));
  vkr_bit_write(&writer, 0xffffffu, 24u);
  assert(writer.overflow && vkr_bit_writer_finish(&writer) == 0u);

  /* Nonzero padding is not the canonical form. */
  const uint8_t padded[1] = {0x81u};
  vkr_bit_reader_init(&reader, padded, sizeof(padded));
  assert(vkr_bit_read(&reader, 1u) == 1u);
  vkr_bit_read_align(&reader);
  assert(reader.overflow);
  printf("  test_bitstream_round_trip PASSED\n");
}

// =============================================================================
// A schema with every field kind
// =============================================================================

typedef struct WireTestPoint {
  uint64_t id;
  float32_t weight;
} WireTestPoint;

typedef struct WireTestValue {
  bool8_t flag;
  uint64_t narrow;
  int64_t delta;
  uint64_t count;
  float32_t scalar;
  float32_t quantized;
  float32_t position[3];
  float32_t packed[3];
  float32_t rotation[4];
  uint32_t mode;
  uint8_t blob[16];
  uint32_t blob_size;
  char name[12];
  WireTestPoint points[4];
  uint32_t point_count;
  WireTestPoint anchor;
  uint64_t net;
  uint8_t entity[16];
  uint64_t hash64;
  uint8_t hash256[32];
  bool8_t has_extra;
  uint64_t extra;
} WireTestValue;

static const VkrWireField wire_test_point_fields[] = {
    {.name = "id",
     .kind = VKR_WIRE_UINT,
     .bits = 12u,
     .offset = offsetof(WireTestPoint, id)},
    {.name = "weight",
     .kind = VKR_WIRE_QFLOAT,
     .bits = 10u,
     .min = 0.0f,
     .max = 1.0f,
     .offset = offsetof(WireTestPoint, weight)},
};

static const VkrWireMessage wire_test_point = {
    .name = "point",
    .size = sizeof(WireTestPoint),
    .fields = wire_test_point_fields,
    .field_count = ArrayCount(wire_test_point_fields),
};

static const VkrWireField wire_test_point_element = {
    .name = "point", .kind = VKR_WIRE_MESSAGE, .message = &wire_test_point};

static VkrWireField wire_test_fields[] = {
    {.name = "flag",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(WireTestValue, flag)},
    {.name = "narrow",
     .kind = VKR_WIRE_UINT,
     .bits = 5u,
     .offset = offsetof(WireTestValue, narrow)},
    {.name = "delta",
     .kind = VKR_WIRE_INT,
     .bits = 9u,
     .offset = offsetof(WireTestValue, delta)},
    {.name = "count",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(WireTestValue, count)},
    {.name = "scalar",
     .kind = VKR_WIRE_F32,
     .offset = offsetof(WireTestValue, scalar)},
    {.name = "quantized",
     .kind = VKR_WIRE_QFLOAT,
     .bits = 16u,
     .min = -10.0f,
     .max = 10.0f,
     .offset = offsetof(WireTestValue, quantized)},
    {.name = "position",
     .kind = VKR_WIRE_VEC3,
     .offset = offsetof(WireTestValue, position)},
    {.name = "packed",
     .kind = VKR_WIRE_VEC3Q,
     .bits = 20u,
     .min = -512.0f,
     .max = 512.0f,
     .offset = offsetof(WireTestValue, packed)},
    {.name = "rotation",
     .kind = VKR_WIRE_QUAT,
     .bits = 15u,
     .offset = offsetof(WireTestValue, rotation)},
    {.name = "mode",
     .kind = VKR_WIRE_ENUM,
     .enum_count = 5u,
     .offset = offsetof(WireTestValue, mode)},
    {.name = "blob",
     .kind = VKR_WIRE_BYTES,
     .capacity = 16u,
     .offset = offsetof(WireTestValue, blob),
     .count_offset = offsetof(WireTestValue, blob_size)},
    {.name = "name",
     .kind = VKR_WIRE_UTF8,
     .capacity = 12u,
     .offset = offsetof(WireTestValue, name)},
    {.name = "points",
     .kind = VKR_WIRE_ARRAY,
     .capacity = 4u,
     .offset = offsetof(WireTestValue, points),
     .count_offset = offsetof(WireTestValue, point_count),
     .element = &wire_test_point_element,
     .stride = sizeof(WireTestPoint)},
    {.name = "anchor",
     .kind = VKR_WIRE_MESSAGE,
     .message = &wire_test_point,
     .offset = offsetof(WireTestValue, anchor)},
    {.name = "net",
     .kind = VKR_WIRE_NETID,
     .offset = offsetof(WireTestValue, net)},
    {.name = "entity",
     .kind = VKR_WIRE_ENTITY_REF,
     .offset = offsetof(WireTestValue, entity)},
    {.name = "hash64",
     .kind = VKR_WIRE_HASH64,
     .offset = offsetof(WireTestValue, hash64)},
    {.name = "hash256",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(WireTestValue, hash256)},
    {.name = "extra",
     .kind = VKR_WIRE_UINT,
     .bits = 32u,
     .flags = VKR_WIRE_FIELD_OPTIONAL,
     .offset = offsetof(WireTestValue, extra),
     .presence_offset = offsetof(WireTestValue, has_extra)},
};

static VkrWireMessage wire_test_message = {
    .name = "value",
    .id = 7u,
    .size = sizeof(WireTestValue),
    .fields = wire_test_fields,
    .field_count = ArrayCount(wire_test_fields),
};

static const VkrWireMessage *wire_test_messages[] = {&wire_test_message,
                                                     &wire_test_point};

static VkrWireSchema wire_test_schema = {
    .name = "test",
    .version = 3u,
    .messages = wire_test_messages,
    .message_count = 1u,
};

static WireTestValue wire_test_sample(void) {
  WireTestValue value = {
      .flag = true_v,
      .narrow = 31u,
      .delta = -200,
      .count = 70000u,
      .scalar = 3.25f,
      .quantized = -4.5f,
      .position = {1.0f, -2.0f, 1e6f},
      .packed = {-511.0f, 0.0f, 100.125f},
      .rotation = {0.0f, 0.70710678f, 0.0f, 0.70710678f},
      .mode = 4u,
      .blob_size = 3u,
      .point_count = 2u,
      .points = {{.id = 4095u, .weight = 1.0f}, {.id = 1u, .weight = 0.5f}},
      .anchor = {.id = 77u, .weight = 0.25f},
      .net = 123456789u,
      .hash64 = 0x0123456789abcdefull,
      .has_extra = true_v,
      .extra = 0xdeadbeefu,
  };
  memcpy(value.blob, "abc", 3u);
  /* "привет" is 12 bytes in UTF-8; "héllo" fits. */
  memcpy(value.name, "h\xc3\xa9llo", 7u);
  for (uint32_t i = 0u; i < 16u; ++i) {
    value.entity[i] = (uint8_t)(i * 17u);
  }
  for (uint32_t i = 0u; i < 32u; ++i) {
    value.hash256[i] = (uint8_t)(255u - i);
  }
  return value;
}

static VkrWireStatus wire_test_decode(const uint8_t *data, uint32_t size,
                                      WireTestValue *out) {
  return vkr_wire_decode(&wire_test_schema, data, size, out, sizeof(*out),
                         NULL);
}

/* Fails when a message with every field kind does not decode to the values
   encoded, within each quantization's error bound. */
static void test_wire_round_trip(void) {
  printf("  Running test_wire_round_trip...\n");
  assert(vkr_wire_schema_validate(&wire_test_schema));
  const WireTestValue value = wire_test_sample();
  uint8_t bytes[256];
  uint32_t size = 0u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_OK);
  printf("    every-kind message: %u bytes for a %u-byte struct\n", size,
         (uint32_t)sizeof(value));
  assert(vkr_wire_peek_id(bytes, size) == 7u);

  WireTestValue decoded;
  assert(wire_test_decode(bytes, size, &decoded) == VKR_WIRE_OK);
  assert(decoded.flag && decoded.narrow == 31u && decoded.delta == -200);
  assert(decoded.count == 70000u && decoded.scalar == 3.25f);
  assert(fabsf(decoded.quantized - value.quantized) <= 20.0f / 65535.0f);
  assert(memcmp(decoded.position, value.position, sizeof(value.position)) == 0);
  for (uint32_t i = 0u; i < 3u; ++i) {
    assert(fabsf(decoded.packed[i] - value.packed[i]) <= 1024.0f / 1048575.0f);
  }
  float32_t dot = 0.0f;
  for (uint32_t i = 0u; i < 4u; ++i) {
    dot += decoded.rotation[i] * value.rotation[i];
  }
  assert(fabsf(dot) > 0.99999f);
  assert(decoded.mode == 4u && decoded.blob_size == 3u &&
         memcmp(decoded.blob, "abc", 3u) == 0);
  assert(strcmp(decoded.name, "h\xc3\xa9llo") == 0);
  assert(decoded.point_count == 2u && decoded.points[0].id == 4095u &&
         decoded.points[0].weight == 1.0f && decoded.points[1].id == 1u);
  assert(decoded.anchor.id == 77u &&
         fabsf(decoded.anchor.weight - 0.25f) <= 1.0f / 1023.0f);
  assert(decoded.net == 123456789u && decoded.hash64 == value.hash64);
  assert(memcmp(decoded.entity, value.entity, 16u) == 0);
  assert(memcmp(decoded.hash256, value.hash256, 32u) == 0);
  assert(decoded.has_extra && decoded.extra == 0xdeadbeefu);

  /* An absent optional field costs one bit and decodes as zero. */
  WireTestValue without = value;
  without.has_extra = false_v;
  uint32_t smaller = 0u;
  assert(vkr_wire_encode(&wire_test_message, &without, bytes, sizeof(bytes),
                         &smaller) == VKR_WIRE_OK);
  assert(smaller < size);
  assert(wire_test_decode(bytes, smaller, &decoded) == VKR_WIRE_OK);
  assert(!decoded.has_extra && decoded.extra == 0u);
  printf("  test_wire_round_trip PASSED\n");
}

/* Fails when random quantized floats and unit quaternions leave their error
   bounds after a round trip. */
static void test_wire_quantization(void) {
  printf("  Running test_wire_quantization...\n");
  WireTestValue value = wire_test_sample();
  uint8_t bytes[256];
  uint32_t size = 0u;
  WireTestValue decoded;
  float32_t worst_component = 0.0f;
  for (uint32_t i = 0u; i < 5000u; ++i) {
    value.quantized =
        (float32_t)((float64_t)(wire_test_random() % 2000001u) / 100000.0 -
                    10.0);
    float32_t q[4];
    float64_t length = 0.0;
    for (uint32_t k = 0u; k < 4u; ++k) {
      q[k] =
          (float32_t)((float64_t)(wire_test_random() % 2000001u) / 1000000.0 -
                      1.0);
      length += (float64_t)q[k] * q[k];
    }
    if (length < 1e-6) {
      continue;
    }
    for (uint32_t k = 0u; k < 4u; ++k) {
      value.rotation[k] = (float32_t)(q[k] / sqrt(length));
    }
    assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                           &size) == VKR_WIRE_OK);
    assert(wire_test_decode(bytes, size, &decoded) == VKR_WIRE_OK);
    assert(fabsf(decoded.quantized - value.quantized) <=
           0.5f * 20.0f / 65535.0f + 1e-5f);
    /* q and -q are one rotation: compare with the decoded sign. */
    float64_t dot = 0.0;
    for (uint32_t k = 0u; k < 4u; ++k) {
      dot += (float64_t)decoded.rotation[k] * value.rotation[k];
    }
    const float32_t sign = dot < 0.0 ? -1.0f : 1.0f;
    for (uint32_t k = 0u; k < 4u; ++k) {
      const float32_t error =
          fabsf(decoded.rotation[k] - sign * value.rotation[k]);
      worst_component = Max(worst_component, error);
    }
  }
  /* 15 bits over ±1/sqrt(2) step 4.3e-5; the rebuilt largest component adds
     at most a few steps more. */
  printf("    worst quaternion component error %.7f\n", worst_component);
  assert(worst_component < 2e-4f);
  printf("  test_wire_quantization PASSED\n");
}

/* Fails when the encoder accepts a value outside its field's bounds. */
static void test_wire_encode_bounds(void) {
  printf("  Running test_wire_encode_bounds...\n");
  uint8_t bytes[256];
  uint32_t size = 0u;
  WireTestValue value = wire_test_sample();
  value.narrow = 32u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.delta = 256;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.quantized = 10.5f;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.scalar = NAN;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.mode = 5u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.blob_size = 17u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  memset(value.name, 'x', sizeof(value.name));
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  memcpy(value.name, "\xff\xfe", 3u);
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.point_count = 5u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.rotation[3] = 0.0f;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  value.flag = 2u;
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, sizeof(bytes),
                         &size) == VKR_WIRE_VIOLATION);
  value = wire_test_sample();
  assert(vkr_wire_encode(&wire_test_message, &value, bytes, 8u, &size) ==
         VKR_WIRE_NO_ROOM);
  printf("  test_wire_encode_bounds PASSED\n");
}

typedef struct WireTestLimits {
  uint32_t mode;
  char name[4];
  uint8_t blob[2];
  uint32_t blob_size;
  float32_t scalar;
} WireTestLimits;

static const VkrWireField wire_limit_fields[] = {
    {.name = "mode",
     .kind = VKR_WIRE_ENUM,
     .enum_count = 3u,
     .offset = offsetof(WireTestLimits, mode)},
    {.name = "name",
     .kind = VKR_WIRE_UTF8,
     .capacity = 4u,
     .offset = offsetof(WireTestLimits, name)},
    {.name = "blob",
     .kind = VKR_WIRE_BYTES,
     .capacity = 2u,
     .offset = offsetof(WireTestLimits, blob),
     .count_offset = offsetof(WireTestLimits, blob_size)},
    {.name = "scalar",
     .kind = VKR_WIRE_F32,
     .offset = offsetof(WireTestLimits, scalar)},
};

static const VkrWireMessage wire_limit_message = {
    .name = "limits",
    .id = 1u,
    .size = sizeof(WireTestLimits),
    .fields = wire_limit_fields,
    .field_count = ArrayCount(wire_limit_fields),
};

static const VkrWireMessage *wire_limit_messages[] = {&wire_limit_message};

static const VkrWireSchema wire_limit_schema = {
    .name = "limits",
    .version = 1u,
    .messages = wire_limit_messages,
    .message_count = 1u,
};

/* Writes a limits message by hand, so the decoder sees values no encoder
   would produce. */
static uint32_t wire_limit_bytes(uint8_t *out, uint32_t capacity, uint64_t id,
                                 uint32_t mode, const char *name,
                                 uint32_t name_size, uint32_t blob_size,
                                 uint32_t scalar_bits, bool8_t trailing) {
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, out, capacity);
  vkr_bit_write_varuint(&writer, id);
  vkr_bit_write(&writer, mode, 2u);
  vkr_bit_write_varuint(&writer, name_size);
  vkr_bit_write_bytes(&writer, name, name_size);
  vkr_bit_write_varuint(&writer, blob_size);
  const uint8_t blob[8] = {0};
  vkr_bit_write_bytes(&writer, blob, Min(blob_size, 8u));
  vkr_bit_write(&writer, scalar_bits, 32u);
  if (trailing) {
    vkr_bit_write(&writer, 0u, 8u);
  }
  return vkr_bit_writer_finish(&writer);
}

/* Fails when the decoder accepts bytes that break a bound: an enum value
   past its count, oversized or invalid text, an oversized blob, NaN, an
   unknown ID, a truncated or overlong message, nonzero padding or a varuint
   wider than its value needs. */
static void test_wire_decode_bounds(void) {
  printf("  Running test_wire_decode_bounds...\n");
  uint8_t bytes[64];
  WireTestLimits out;
  const uint32_t one = 0x3f800000u;
  uint32_t size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 2u, "abc", 3u, 2u,
                                   one, false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_OK);
  assert(out.mode == 2u && strcmp(out.name, "abc") == 0 && out.scalar == 1.0f);

  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 3u, "abc", 3u, 2u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "abcd", 4u, 2u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "a\0c", 3u, 2u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "\xc0\xaf", 2u, 2u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "ab", 2u, 3u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "ab", 2u, 1u,
                          0x7fc00000u, false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 9u, 0u, "ab", 2u, 1u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_UNKNOWN_MESSAGE);
  size =
      wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "ab", 2u, 1u, one, true_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);
  size = wire_limit_bytes(bytes, sizeof(bytes), 1u, 0u, "ab", 2u, 1u, one,
                          false_v);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size - 1u, &out,
                         sizeof(out), NULL) == VKR_WIRE_VIOLATION);

  /* The ID 1 written in the 16-bit class is not canonical. */
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, 1u, 2u);
  vkr_bit_write(&writer, 1u, 16u);
  size = vkr_bit_writer_finish(&writer);
  assert(vkr_wire_decode(&wire_limit_schema, bytes, size, &out, sizeof(out),
                         NULL) == VKR_WIRE_VIOLATION);

  /* Random bytes never crash the decoder. */
  for (uint32_t i = 0u; i < 20000u; ++i) {
    const uint32_t length = (uint32_t)(wire_test_random() % sizeof(bytes));
    for (uint32_t b = 0u; b < length; ++b) {
      bytes[b] = (uint8_t)wire_test_random();
    }
    WireTestValue big;
    (void)vkr_wire_decode(&wire_test_schema, bytes, length, &big, sizeof(big),
                          NULL);
  }
  printf("  test_wire_decode_bounds PASSED\n");
}

/* Fails when the schema hash ignores a wire change or changes with names,
   offsets or message order, which do not change the wire. */
static void test_wire_schema_hash(void) {
  printf("  Running test_wire_schema_hash...\n");
  const uint64_t base = vkr_wire_schema_hash(&wire_test_schema);

  wire_test_fields[1].name = "renamed";
  assert(vkr_wire_schema_hash(&wire_test_schema) == base);
  wire_test_fields[1].name = "narrow";

  wire_test_fields[1].bits = 6u;
  assert(vkr_wire_schema_hash(&wire_test_schema) != base);
  wire_test_fields[1].bits = 5u;

  wire_test_fields[9].enum_count = 6u;
  assert(vkr_wire_schema_hash(&wire_test_schema) != base);
  wire_test_fields[9].enum_count = 5u;

  wire_test_fields[5].max = 11.0f;
  assert(vkr_wire_schema_hash(&wire_test_schema) != base);
  wire_test_fields[5].max = 10.0f;

  wire_test_message.id = 8u;
  assert(vkr_wire_schema_hash(&wire_test_schema) != base);
  wire_test_message.id = 7u;

  wire_test_schema.version = 4u;
  assert(vkr_wire_schema_hash(&wire_test_schema) != base);
  wire_test_schema.version = 3u;
  assert(vkr_wire_schema_hash(&wire_test_schema) == base);

  /* Message order in the table does not matter. */
  static VkrWireMessage first = {.name = "a", .id = 1u, .size = 8u};
  static VkrWireMessage second = {.name = "b", .id = 2u, .size = 8u};
  const VkrWireMessage *forward[] = {&first, &second};
  const VkrWireMessage *backward[] = {&second, &first};
  VkrWireSchema a = {.version = 1u, .messages = forward, .message_count = 2u};
  VkrWireSchema b = {.version = 1u, .messages = backward, .message_count = 2u};
  assert(vkr_wire_schema_hash(&a) == vkr_wire_schema_hash(&b));

  /* Inconsistent descriptors fail validation. */
  VkrWireField bad = {.name = "x", .kind = VKR_WIRE_UINT, .bits = 65u};
  VkrWireMessage bad_message = {
      .id = 1u, .size = 8u, .fields = &bad, .field_count = 1u};
  const VkrWireMessage *bad_list[] = {&bad_message};
  VkrWireSchema bad_schema = {.messages = bad_list, .message_count = 1u};
  assert(!vkr_wire_schema_validate(&bad_schema));
  bad.bits = 8u;
  bad.offset = 4u;
  assert(!vkr_wire_schema_validate(&bad_schema));
  const VkrWireMessage *duplicate[] = {&first, &first};
  VkrWireSchema dup_schema = {.messages = duplicate, .message_count = 2u};
  assert(!vkr_wire_schema_validate(&dup_schema));
  printf("  test_wire_schema_hash PASSED\n");
}

bool32_t run_net_wire_tests(void) {
  printf("--- Starting network data format tests ---\n");
  test_bitstream_round_trip();
  test_wire_round_trip();
  test_wire_quantization();
  test_wire_encode_bounds();
  test_wire_decode_bounds();
  test_wire_schema_hash();
  printf("--- Network data format tests completed ---\n");
  return true;
}
