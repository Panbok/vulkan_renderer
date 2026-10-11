/* Wire codec and bitstream fuzzer (ADR-105, "Security").
 *
 * The input's first byte picks a target:
 *   0  vkr_wire_decode over a schema holding every field kind, nested
 *      arrays and messages and optional fields. The exact message must
 *      re-encode to the input byte for byte, since the decoder accepts
 *      only canonical bytes; the quantized message must re-encode and
 *      decode again to values within one quantization step.
 *   1  a bitstream program written, then read back: every value, varuint
 *      and byte run must return unchanged and the reader end exactly.
 *   2  a bitstream program read from arbitrary bytes.
 *   3  the transport's QUIC varints and packet-number recovery. */

#include "fuzz_common.h"
#include "vkr_bitstream.h"
#include "vkr_net_varint.h"
#include "vkr_wire.h"

#include <math.h>
#include <stddef.h>

// =============================================================================
// Schema
// =============================================================================

typedef struct FuzzWireInner {
  uint64_t count;
  int64_t delta;
  bool8_t flag;
  char label[8];
  uint64_t marks[2];
  uint32_t mark_count;
} FuzzWireInner;

typedef struct FuzzWireChunk {
  uint8_t bytes[5];
  uint32_t size;
} FuzzWireChunk;

typedef struct FuzzWireExact {
  bool8_t flag;
  uint64_t bit;
  uint64_t wide;
  int64_t signed13;
  int64_t signed64;
  uint64_t var;
  uint64_t netid;
  float32_t f;
  float32_t f_any;
  float32_t position[3];
  float32_t velocity[3];
  bool8_t has_velocity;
  uint32_t mode;
  uint32_t only;
  uint32_t wide_enum;
  uint8_t blob[24];
  uint32_t blob_size;
  char name[16];
  char note[12];
  bool8_t has_note;
  uint64_t values[6];
  uint32_t value_count;
  FuzzWireInner inner;
  FuzzWireInner items[3];
  uint32_t item_count;
  FuzzWireInner extra;
  bool8_t has_extra;
  char tags[4][6];
  uint32_t tag_count;
  FuzzWireChunk chunks[3];
  uint32_t chunk_count;
  uint8_t entity[16];
  uint64_t hash;
  uint8_t digest[32];
  uint64_t opt_small;
  bool8_t has_opt_small;
} FuzzWireExact;

typedef struct FuzzWireLossy {
  float32_t q8;
  float32_t q16;
  float32_t q32;
  float32_t position[3];
  float32_t rotation[4];
  float32_t fine[4];
  float32_t coarse[3];
  bool8_t has_coarse;
  float32_t rotations[2][4];
  uint32_t rotation_count;
} FuzzWireLossy;

typedef struct FuzzWireEmpty {
  uint8_t unused;
} FuzzWireEmpty;

static const VkrWireField fuzz_wire_mark = {
    .name = "mark", .kind = VKR_WIRE_UINT, .bits = 4u};

static const VkrWireField fuzz_wire_inner_fields[] = {
    {.name = "count",
     .kind = VKR_WIRE_UINT,
     .bits = 7u,
     .offset = offsetof(FuzzWireInner, count)},
    {.name = "delta",
     .kind = VKR_WIRE_INT,
     .bits = 20u,
     .offset = offsetof(FuzzWireInner, delta)},
    {.name = "flag",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(FuzzWireInner, flag)},
    {.name = "label",
     .kind = VKR_WIRE_UTF8,
     .offset = offsetof(FuzzWireInner, label),
     .capacity = 8u},
    {.name = "marks",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireInner, marks),
     .capacity = 2u,
     .count_offset = offsetof(FuzzWireInner, mark_count),
     .element = &fuzz_wire_mark,
     .stride = sizeof(uint64_t)},
};

static const VkrWireMessage fuzz_wire_inner = {
    .name = "inner",
    .size = sizeof(FuzzWireInner),
    .fields = fuzz_wire_inner_fields,
    .field_count = ArrayCount(fuzz_wire_inner_fields),
};

static const VkrWireField fuzz_wire_value = {.name = "value",
                                             .kind = VKR_WIRE_VARUINT};

static const VkrWireField fuzz_wire_item = {
    .name = "item", .kind = VKR_WIRE_MESSAGE, .message = &fuzz_wire_inner};

static const VkrWireField fuzz_wire_tag = {
    .name = "tag", .kind = VKR_WIRE_UTF8, .capacity = 6u};

static const VkrWireField fuzz_wire_chunk = {
    .name = "chunk",
    .kind = VKR_WIRE_BYTES,
    .offset = offsetof(FuzzWireChunk, bytes),
    .capacity = 5u,
    .count_offset = offsetof(FuzzWireChunk, size)};

static const VkrWireField fuzz_wire_exact_fields[] = {
    {.name = "flag",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(FuzzWireExact, flag)},
    {.name = "bit",
     .kind = VKR_WIRE_UINT,
     .bits = 1u,
     .offset = offsetof(FuzzWireExact, bit)},
    {.name = "wide",
     .kind = VKR_WIRE_UINT,
     .bits = 64u,
     .offset = offsetof(FuzzWireExact, wide)},
    {.name = "signed13",
     .kind = VKR_WIRE_INT,
     .bits = 13u,
     .offset = offsetof(FuzzWireExact, signed13)},
    {.name = "signed64",
     .kind = VKR_WIRE_INT,
     .bits = 64u,
     .offset = offsetof(FuzzWireExact, signed64)},
    {.name = "var",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(FuzzWireExact, var)},
    {.name = "netid",
     .kind = VKR_WIRE_NETID,
     .offset = offsetof(FuzzWireExact, netid)},
    {.name = "f", .kind = VKR_WIRE_F32, .offset = offsetof(FuzzWireExact, f)},
    {.name = "f_any",
     .kind = VKR_WIRE_F32,
     .flags = VKR_WIRE_FIELD_NONFINITE,
     .offset = offsetof(FuzzWireExact, f_any)},
    {.name = "position",
     .kind = VKR_WIRE_VEC3,
     .offset = offsetof(FuzzWireExact, position)},
    {.name = "velocity",
     .kind = VKR_WIRE_VEC3,
     .flags = VKR_WIRE_FIELD_OPTIONAL | VKR_WIRE_FIELD_NONFINITE,
     .offset = offsetof(FuzzWireExact, velocity),
     .presence_offset = offsetof(FuzzWireExact, has_velocity)},
    {.name = "mode",
     .kind = VKR_WIRE_ENUM,
     .offset = offsetof(FuzzWireExact, mode),
     .enum_count = 5u},
    {.name = "only",
     .kind = VKR_WIRE_ENUM,
     .offset = offsetof(FuzzWireExact, only),
     .enum_count = 1u},
    {.name = "wide_enum",
     .kind = VKR_WIRE_ENUM,
     .offset = offsetof(FuzzWireExact, wide_enum),
     .enum_count = 300u},
    {.name = "blob",
     .kind = VKR_WIRE_BYTES,
     .offset = offsetof(FuzzWireExact, blob),
     .capacity = 24u,
     .count_offset = offsetof(FuzzWireExact, blob_size)},
    {.name = "name",
     .kind = VKR_WIRE_UTF8,
     .offset = offsetof(FuzzWireExact, name),
     .capacity = 16u},
    {.name = "note",
     .kind = VKR_WIRE_UTF8,
     .flags = VKR_WIRE_FIELD_OPTIONAL,
     .offset = offsetof(FuzzWireExact, note),
     .capacity = 12u,
     .presence_offset = offsetof(FuzzWireExact, has_note)},
    {.name = "values",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireExact, values),
     .capacity = 6u,
     .count_offset = offsetof(FuzzWireExact, value_count),
     .element = &fuzz_wire_value,
     .stride = sizeof(uint64_t)},
    {.name = "inner",
     .kind = VKR_WIRE_MESSAGE,
     .offset = offsetof(FuzzWireExact, inner),
     .message = &fuzz_wire_inner},
    {.name = "items",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireExact, items),
     .capacity = 3u,
     .count_offset = offsetof(FuzzWireExact, item_count),
     .element = &fuzz_wire_item,
     .stride = sizeof(FuzzWireInner)},
    {.name = "extra",
     .kind = VKR_WIRE_MESSAGE,
     .flags = VKR_WIRE_FIELD_OPTIONAL,
     .offset = offsetof(FuzzWireExact, extra),
     .presence_offset = offsetof(FuzzWireExact, has_extra),
     .message = &fuzz_wire_inner},
    {.name = "tags",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireExact, tags),
     .capacity = 4u,
     .count_offset = offsetof(FuzzWireExact, tag_count),
     .element = &fuzz_wire_tag,
     .stride = 6u},
    {.name = "chunks",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireExact, chunks),
     .capacity = 3u,
     .count_offset = offsetof(FuzzWireExact, chunk_count),
     .element = &fuzz_wire_chunk,
     .stride = sizeof(FuzzWireChunk)},
    {.name = "entity",
     .kind = VKR_WIRE_ENTITY_REF,
     .offset = offsetof(FuzzWireExact, entity)},
    {.name = "hash",
     .kind = VKR_WIRE_HASH64,
     .offset = offsetof(FuzzWireExact, hash)},
    {.name = "digest",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(FuzzWireExact, digest)},
    {.name = "opt_small",
     .kind = VKR_WIRE_UINT,
     .flags = VKR_WIRE_FIELD_OPTIONAL,
     .bits = 9u,
     .offset = offsetof(FuzzWireExact, opt_small),
     .presence_offset = offsetof(FuzzWireExact, has_opt_small)},
};

static const VkrWireField fuzz_wire_rotation = {
    .name = "rotation", .kind = VKR_WIRE_QUAT, .bits = 6u};

static const VkrWireField fuzz_wire_lossy_fields[] = {
    {.name = "q8",
     .kind = VKR_WIRE_QFLOAT,
     .bits = 8u,
     .offset = offsetof(FuzzWireLossy, q8),
     .min = -1.0f,
     .max = 1.0f},
    {.name = "q16",
     .kind = VKR_WIRE_QFLOAT,
     .bits = 16u,
     .offset = offsetof(FuzzWireLossy, q16),
     .min = 0.0f,
     .max = 100.0f},
    {.name = "q32",
     .kind = VKR_WIRE_QFLOAT,
     .bits = 32u,
     .offset = offsetof(FuzzWireLossy, q32),
     .min = -1000.0f,
     .max = 1000.0f},
    {.name = "position",
     .kind = VKR_WIRE_VEC3Q,
     .bits = 12u,
     .offset = offsetof(FuzzWireLossy, position),
     .min = -50.0f,
     .max = 50.0f},
    {.name = "rotation",
     .kind = VKR_WIRE_QUAT,
     .bits = 10u,
     .offset = offsetof(FuzzWireLossy, rotation)},
    {.name = "fine",
     .kind = VKR_WIRE_QUAT,
     .bits = 31u,
     .offset = offsetof(FuzzWireLossy, fine)},
    {.name = "coarse",
     .kind = VKR_WIRE_VEC3Q,
     .flags = VKR_WIRE_FIELD_OPTIONAL,
     .bits = 1u,
     .offset = offsetof(FuzzWireLossy, coarse),
     .presence_offset = offsetof(FuzzWireLossy, has_coarse),
     .min = 2.0f,
     .max = 3.0f},
    {.name = "rotations",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(FuzzWireLossy, rotations),
     .capacity = 2u,
     .count_offset = offsetof(FuzzWireLossy, rotation_count),
     .element = &fuzz_wire_rotation,
     .stride = sizeof(float32_t) * 4u},
};

enum {
  FUZZ_WIRE_EXACT = 1,
  FUZZ_WIRE_LOSSY = 2,
  FUZZ_WIRE_EMPTY = 0x1234,
};

static const VkrWireMessage fuzz_wire_exact = {
    .name = "exact",
    .id = FUZZ_WIRE_EXACT,
    .size = sizeof(FuzzWireExact),
    .fields = fuzz_wire_exact_fields,
    .field_count = ArrayCount(fuzz_wire_exact_fields),
};

static const VkrWireMessage fuzz_wire_lossy = {
    .name = "lossy",
    .id = FUZZ_WIRE_LOSSY,
    .size = sizeof(FuzzWireLossy),
    .fields = fuzz_wire_lossy_fields,
    .field_count = ArrayCount(fuzz_wire_lossy_fields),
};

static const VkrWireMessage fuzz_wire_empty = {
    .name = "empty",
    .id = FUZZ_WIRE_EMPTY,
    .size = sizeof(FuzzWireEmpty),
};

static const VkrWireMessage *const fuzz_wire_messages[] = {
    &fuzz_wire_exact,
    &fuzz_wire_lossy,
    &fuzz_wire_empty,
};

static const VkrWireSchema fuzz_wire_schema = {
    .name = "fuzz",
    .version = 1u,
    .messages = fuzz_wire_messages,
    .message_count = ArrayCount(fuzz_wire_messages),
};

// =============================================================================
// Schema decoding
// =============================================================================

#define FUZZ_WIRE_ENCODE_MAX 4096u
#define FUZZ_WIRE_QUAT_RANGE 0.70710678118654752f

/* One quantization step of `bits` over [min, max], plus float rounding of
   values near `value`. */
static float64_t fuzz_wire_tolerance(float32_t value, float32_t min,
                                     float32_t max, uint32_t bits) {
  const float64_t steps =
      bits >= 64u ? 18446744073709551615.0 : (float64_t)((1ull << bits) - 1u);
  return ((float64_t)max - min) / steps * 1.01 + fabs(value) * 1e-6 + 1e-7;
}

static void fuzz_wire_check_close(const float32_t *a, const float32_t *b,
                                  uint32_t count, float32_t min, float32_t max,
                                  uint32_t bits) {
  for (uint32_t i = 0u; i < count; ++i) {
    FUZZ_CHECK(fabs((float64_t)a[i] - b[i]) <=
               fuzz_wire_tolerance(a[i], min, max, bits));
  }
}

/* q and -q are one rotation. The decoder accepts components whose squares
   sum to 1 + 1e-4 and rebuilds the dropped one from them, so a component may
   move by that slack over the half-or-more value it divides by, beyond the
   quantization step. */
#define FUZZ_WIRE_QUAT_SLACK 2e-4

static void fuzz_wire_check_quat(const float32_t *a, const float32_t *b,
                                 uint32_t bits) {
  float64_t dot = 0.0;
  for (uint32_t i = 0u; i < 4u; ++i) {
    dot += (float64_t)a[i] * b[i];
  }
  const float32_t sign = dot < 0.0 ? -1.0f : 1.0f;
  for (uint32_t i = 0u; i < 4u; ++i) {
    FUZZ_CHECK(fabs((float64_t)a[i] - sign * b[i]) <=
               4.0 * fuzz_wire_tolerance(a[i], -FUZZ_WIRE_QUAT_RANGE,
                                         FUZZ_WIRE_QUAT_RANGE, bits) +
                   FUZZ_WIRE_QUAT_SLACK);
  }
}

static void fuzz_wire_check_lossy(const FuzzWireLossy *a,
                                  const FuzzWireLossy *b) {
  fuzz_wire_check_close(&a->q8, &b->q8, 1u, -1.0f, 1.0f, 8u);
  fuzz_wire_check_close(&a->q16, &b->q16, 1u, 0.0f, 100.0f, 16u);
  fuzz_wire_check_close(&a->q32, &b->q32, 1u, -1000.0f, 1000.0f, 32u);
  fuzz_wire_check_close(a->position, b->position, 3u, -50.0f, 50.0f, 12u);
  fuzz_wire_check_quat(a->rotation, b->rotation, 10u);
  fuzz_wire_check_quat(a->fine, b->fine, 31u);
  FUZZ_CHECK(a->has_coarse == b->has_coarse);
  fuzz_wire_check_close(a->coarse, b->coarse, 3u, 2.0f, 3.0f, 1u);
  FUZZ_CHECK(a->rotation_count == b->rotation_count);
  for (uint32_t i = 0u; i < a->rotation_count; ++i) {
    fuzz_wire_check_quat(a->rotations[i], b->rotations[i], 6u);
  }
}

static void fuzz_wire_decode(const uint8_t *data, uint32_t size) {
  /* Decode into storage of exactly the peeked message's size, so a write
     past the struct reaches ASan; unknown IDs get the schema's largest. */
  const uint32_t id = vkr_wire_peek_id(data, size);
  uint32_t capacity = vkr_wire_schema_max_size(&fuzz_wire_schema);
  for (uint32_t i = 0u; i < fuzz_wire_schema.message_count; ++i) {
    if (fuzz_wire_messages[i]->id == id) {
      capacity = fuzz_wire_messages[i]->size;
    }
  }
  uint8_t *value = malloc(capacity);
  FUZZ_CHECK(value);
  MemSet(value, 0xcd, capacity);
  const VkrWireMessage *message = NULL;
  const VkrWireStatus status =
      vkr_wire_decode(&fuzz_wire_schema, data, size, value, capacity, &message);
  if (status != VKR_WIRE_OK) {
    FUZZ_CHECK(status == VKR_WIRE_VIOLATION ||
               status == VKR_WIRE_UNKNOWN_MESSAGE);
    free(value);
    return;
  }
  FUZZ_CHECK(message && message->id == id);

  uint8_t encoded[FUZZ_WIRE_ENCODE_MAX];
  uint32_t encoded_size = 0u;
  FUZZ_CHECK(vkr_wire_encode(message, value, encoded, sizeof(encoded),
                             &encoded_size) == VKR_WIRE_OK);
  if (message->id != FUZZ_WIRE_LOSSY) {
    /* One value has one encoding: the input was that encoding. */
    FUZZ_CHECK(encoded_size == size);
    FUZZ_CHECK(MemCompare(encoded, data, size) == 0);
    free(value);
    return;
  }

  FuzzWireLossy again;
  const VkrWireMessage *again_message = NULL;
  FUZZ_CHECK(vkr_wire_decode(&fuzz_wire_schema, encoded, encoded_size, &again,
                             sizeof(again), &again_message) == VKR_WIRE_OK);
  FUZZ_CHECK(again_message == message);
  fuzz_wire_check_lossy((const FuzzWireLossy *)value, &again);
  free(value);
}

// =============================================================================
// Bitstream programs
// =============================================================================

#define FUZZ_BIT_OPS_MAX 256u

typedef enum FuzzBitOp {
  FUZZ_BIT_VALUE = 0,
  FUZZ_BIT_VARUINT = 1,
  FUZZ_BIT_ALIGN = 2,
  FUZZ_BIT_BYTES = 3,
} FuzzBitOp;

typedef struct FuzzBitStep {
  uint8_t op;
  uint32_t bits;
  uint64_t value;
  const uint8_t *bytes;
  uint32_t size;
} FuzzBitStep;

static uint64_t fuzz_bit_mask(uint32_t bits) {
  return bits >= 64u ? UINT64_MAX : (1ull << bits) - 1u;
}

static uint64_t fuzz_u64(FuzzInput *input) {
  const uint64_t low = fuzz_u32(input);
  const uint64_t high = fuzz_u32(input);
  return low | (high << 32);
}

/* Op byte: the op in the low two bits, its parameter above. */
static uint32_t fuzz_bit_program(FuzzInput *input, FuzzBitStep *steps) {
  uint32_t count = 0u;
  while (count < FUZZ_BIT_OPS_MAX && fuzz_remaining(input) > 0u) {
    const uint8_t code = fuzz_u8(input);
    FuzzBitStep step = {.op = (uint8_t)(code & 0x03u)};
    const uint32_t parameter = code >> 2;
    if (step.op == FUZZ_BIT_VALUE) {
      step.bits = parameter % 65u;
      step.value = fuzz_u64(input);
    } else if (step.op == FUZZ_BIT_VARUINT) {
      /* The parameter picks the size class the value needs. */
      static const uint64_t masks[4] = {0xffull, 0xffffull, 0xffffffffull,
                                        UINT64_MAX};
      step.value = fuzz_u64(input) & masks[parameter & 0x03u];
    } else if (step.op == FUZZ_BIT_BYTES) {
      step.bytes = fuzz_bytes(input, parameter, &step.size);
    }
    steps[count++] = step;
  }
  return count;
}

static void fuzz_bit_roundtrip(const uint8_t *data, uint32_t size) {
  FuzzInput input = fuzz_input(data, size);
  const uint32_t capacity = fuzz_u16(&input) & 0x0fffu;
  FuzzBitStep steps[FUZZ_BIT_OPS_MAX];
  const uint32_t count = fuzz_bit_program(&input, steps);

  /* Exactly `capacity` bytes, so a write past it reaches ASan. */
  uint8_t *buffer = malloc(capacity ? capacity : 1u);
  FUZZ_CHECK(buffer);
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, buffer, capacity);
  for (uint32_t i = 0u; i < count; ++i) {
    const FuzzBitStep *step = &steps[i];
    switch (step->op) {
    case FUZZ_BIT_VALUE:
      vkr_bit_write(&writer, step->value, step->bits);
      break;
    case FUZZ_BIT_VARUINT:
      vkr_bit_write_varuint(&writer, step->value);
      break;
    case FUZZ_BIT_ALIGN:
      vkr_bit_write_align(&writer);
      break;
    default:
      vkr_bit_write_bytes(&writer, step->bytes, step->size);
      break;
    }
  }
  const uint32_t written = vkr_bit_writer_finish(&writer);
  FUZZ_CHECK(written <= capacity);
  if (writer.overflow) {
    FUZZ_CHECK(written == 0u);
    free(buffer);
    return;
  }

  VkrBitReader reader;
  vkr_bit_reader_init(&reader, buffer, written);
  for (uint32_t i = 0u; i < count; ++i) {
    const FuzzBitStep *step = &steps[i];
    switch (step->op) {
    case FUZZ_BIT_VALUE:
      FUZZ_CHECK(vkr_bit_read(&reader, step->bits) ==
                 (step->value & fuzz_bit_mask(step->bits)));
      break;
    case FUZZ_BIT_VARUINT:
      FUZZ_CHECK(vkr_bit_read_varuint(&reader) == step->value);
      break;
    case FUZZ_BIT_ALIGN:
      vkr_bit_read_align(&reader);
      break;
    default: {
      const uint8_t *view = vkr_bit_read_bytes(&reader, step->size);
      FUZZ_CHECK(view);
      FUZZ_CHECK(step->size == 0u ||
                 MemCompare(view, step->bytes, step->size) == 0);
      break;
    }
    }
    FUZZ_CHECK(!reader.overflow);
  }
  vkr_bit_read_align(&reader);
  FUZZ_CHECK(vkr_bit_reader_at_end(&reader));
  free(buffer);
}

/* The program's ops read the bytes after a 16-bit split point. */
static void fuzz_bit_read_raw(const uint8_t *data, uint32_t size) {
  FuzzInput input = fuzz_input(data, size);
  const uint32_t requested = fuzz_u16(&input);
  const uint32_t split = Min(requested, fuzz_remaining(&input));
  FuzzInput program_input = fuzz_input(data + input.offset, split);
  const uint32_t stream_size = fuzz_remaining(&input) - split;
  /* A copy of exactly the stream, so a read past it reaches ASan. */
  uint8_t *stream = malloc(stream_size ? stream_size : 1u);
  FUZZ_CHECK(stream);
  if (stream_size > 0u) {
    MemCopy(stream, data + input.offset + split, stream_size);
  }
  FuzzBitStep steps[FUZZ_BIT_OPS_MAX];
  const uint32_t count = fuzz_bit_program(&program_input, steps);

  VkrBitReader reader;
  vkr_bit_reader_init(&reader, stream, stream_size);
  uint64_t sum = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    const FuzzBitStep *step = &steps[i];
    const bool8_t overflowed = reader.overflow;
    uint64_t value = 0u;
    switch (step->op) {
    case FUZZ_BIT_VALUE:
      value = vkr_bit_read(&reader, step->bits);
      FUZZ_CHECK((value & ~fuzz_bit_mask(step->bits)) == 0u);
      break;
    case FUZZ_BIT_VARUINT:
      value = vkr_bit_read_varuint(&reader);
      break;
    case FUZZ_BIT_ALIGN:
      vkr_bit_read_align(&reader);
      break;
    default: {
      const uint8_t *view = vkr_bit_read_bytes(&reader, step->size);
      if (view) {
        FUZZ_CHECK(view >= stream && view + step->size <= stream + stream_size);
        for (uint32_t k = 0u; k < step->size; ++k) {
          sum += view[k];
        }
      }
      FUZZ_CHECK(!overflowed || !view);
      break;
    }
    }
    /* Once past the end, every read yields zero and the flag stays. */
    if (overflowed) {
      FUZZ_CHECK(reader.overflow && value == 0u);
    }
    sum += value;
  }
  (void)vkr_bit_reader_at_end(&reader);
  (void)sum;
  free(stream);
}

// =============================================================================
// QUIC varints and packet numbers
// =============================================================================

static void fuzz_varints(const uint8_t *data, uint32_t size) {
  FuzzInput input = fuzz_input(data, size);
  const uint64_t largest = fuzz_u64(&input) & VKR_NET_VARINT_MAX;
  const uint8_t length = (uint8_t)((fuzz_u8(&input) & 0x03u) + 1u);
  const uint64_t truncated = fuzz_u32(&input) & fuzz_bit_mask(length * 8u);
  const uint32_t bits = length * 8u;
  const uint64_t pn = vkr_net_packet_number_decode(largest, truncated, bits);
  const uint64_t window = 1ull << bits;
  /* The low bits are what the packet carried. */
  FUZZ_CHECK((pn & (window - 1u)) == truncated);
  const uint64_t expected = largest + 1u;
  if (expected >= window && expected < VKR_NET_VARINT_MAX - 2u * window) {
    /* Nearest to the expected number: within half a window. */
    const uint64_t distance = pn > expected ? pn - expected : expected - pn;
    FUZZ_CHECK(distance <= window / 2u);
  }

  const uint8_t *cursor = data + input.offset;
  const uint8_t *end = data + size;
  while (cursor < end) {
    uint64_t value = 0u;
    const uint32_t used = vkr_net_varint_read(cursor, end, &value);
    if (used == 0u) {
      FUZZ_CHECK((uint64_t)(end - cursor) < (1u << (cursor[0] >> 6)));
      break;
    }
    FUZZ_CHECK(value <= VKR_NET_VARINT_MAX);
    uint8_t written[8];
    const uint32_t written_size =
        vkr_net_varint_write(written, written + sizeof(written), value);
    FUZZ_CHECK(written_size > 0u && written_size <= used);
    FUZZ_CHECK(written_size == vkr_net_varint_size(value));
    uint64_t again = 0u;
    FUZZ_CHECK(vkr_net_varint_read(written, written + written_size, &again) ==
               written_size);
    FUZZ_CHECK(again == value);
    cursor += used;
  }
}

// =============================================================================
// Entry and seeds
// =============================================================================

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1u || size > 65536u) {
    return 0;
  }
  const uint32_t rest = (uint32_t)size - 1u;
  switch (data[0] % 4u) {
  case 0u:
    fuzz_wire_decode(data + 1, rest);
    break;
  case 1u:
    fuzz_bit_roundtrip(data + 1, rest);
    break;
  case 2u:
    fuzz_bit_read_raw(data + 1, rest);
    break;
  default:
    fuzz_varints(data + 1, rest);
    break;
  }
  return 0;
}

static void fuzz_wire_seed_message(FuzzSeedWriter *writer,
                                   const VkrWireMessage *message,
                                   const void *value) {
  FuzzSeed seed = {0};
  fuzz_seed_u8(&seed, 0u);
  uint32_t size = 0u;
  FUZZ_CHECK(vkr_wire_encode(message, value, seed.bytes + 1,
                             sizeof(seed.bytes) - 1u, &size) == VKR_WIRE_OK);
  seed.size += size;
  fuzz_seed_emit(writer, &seed);
}

static void fuzz_wire_seeds(FuzzSeedWriter *writer) {
  FuzzWireExact exact = {
      .flag = true_v,
      .bit = 1u,
      .wide = 0x0123456789abcdefull,
      .signed13 = -1000,
      .signed64 = INT64_MIN,
      .var = 70000u,
      .netid = 42u,
      .f = -0.0f,
      .f_any = NAN,
      .position = {1.0f, -2.5f, 3.25f},
      .velocity = {INFINITY, 0.0f, 1.0f},
      .has_velocity = true_v,
      .mode = 4u,
      .wide_enum = 299u,
      .blob = {1, 2, 3, 4, 5},
      .blob_size = 5u,
      .name = "na\xc3\xafve",
      .note = "\xf0\x9f\x98\x80",
      .has_note = true_v,
      .values = {0u, 255u, 256u, 65536u, UINT64_MAX},
      .value_count = 5u,
      .inner = {.count = 127u,
                .delta = -524288,
                .flag = true_v,
                .label = "abc",
                .marks = {15u, 1u},
                .mark_count = 2u},
      .items = {{.count = 1u, .label = "x"}, {.delta = 7, .mark_count = 1u}},
      .item_count = 2u,
      .extra = {.count = 3u},
      .has_extra = true_v,
      .tags = {"red", "green", ""},
      .tag_count = 3u,
      .chunks = {{.bytes = {9, 8}, .size = 2u}, {.size = 0u}},
      .chunk_count = 2u,
      .entity = {0xaa, 0xbb},
      .hash = 0xfeedfacecafebeefull,
      .digest = {1, 2, 3},
      .opt_small = 511u,
      .has_opt_small = true_v,
  };
  fuzz_wire_seed_message(writer, &fuzz_wire_exact, &exact);
  const FuzzWireExact empty_exact = {0};
  fuzz_wire_seed_message(writer, &fuzz_wire_exact, &empty_exact);

  const FuzzWireLossy lossy = {
      .q8 = 0.5f,
      .q16 = 33.3f,
      .q32 = -999.5f,
      .position = {-50.0f, 0.0f, 49.0f},
      .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
      .fine = {0.5f, -0.5f, 0.5f, -0.5f},
      .coarse = {2.0f, 3.0f, 2.4f},
      .has_coarse = true_v,
      .rotations = {{0.70710678f, 0.70710678f, 0.0f, 0.0f},
                    {0.0f, -1.0f, 0.0f, 0.0f}},
      .rotation_count = 2u,
  };
  fuzz_wire_seed_message(writer, &fuzz_wire_lossy, &lossy);
  const FuzzWireEmpty empty = {0};
  fuzz_wire_seed_message(writer, &fuzz_wire_empty, &empty);

  /* A bitstream program touching every op and size class. */
  FuzzSeed seed = {0};
  fuzz_seed_u8(&seed, 1u);
  fuzz_seed_u16(&seed, 256u);
  const uint8_t value_ops[] = {(uint8_t)(FUZZ_BIT_VALUE | (3u << 2)),
                               (uint8_t)(FUZZ_BIT_VALUE | (33u << 2)),
                               (uint8_t)(FUZZ_BIT_VALUE | (63u << 2))};
  for (uint32_t i = 0u; i < ArrayCount(value_ops); ++i) {
    fuzz_seed_u8(&seed, value_ops[i]);
    fuzz_seed_u32(&seed, 0x89abcdefu);
    fuzz_seed_u32(&seed, 0x01234567u);
  }
  for (uint8_t size_class = 0u; size_class < 4u; ++size_class) {
    fuzz_seed_u8(&seed, (uint8_t)(FUZZ_BIT_VARUINT | (size_class << 2)));
    fuzz_seed_u32(&seed, 0xfffffff0u);
    fuzz_seed_u32(&seed, 0x7u);
  }
  fuzz_seed_u8(&seed, FUZZ_BIT_ALIGN);
  fuzz_seed_u8(&seed, (uint8_t)(FUZZ_BIT_BYTES | (5u << 2)));
  fuzz_seed_bytes(&seed, "hello", 5u);
  fuzz_seed_emit(writer, &seed);

  /* The same program over raw bytes. */
  seed.bytes[0] = 2u;
  fuzz_seed_emit(writer, &seed);

  /* Varints of every length after a packet-number case. */
  seed = (FuzzSeed){0};
  fuzz_seed_u8(&seed, 3u);
  fuzz_seed_u32(&seed, 0xa82f9b32u);
  fuzz_seed_u32(&seed, 0u);
  fuzz_seed_u8(&seed, 1u);
  fuzz_seed_u32(&seed, 0x9b32u);
  static const uint8_t varints[] = {0x25, 0x7b, 0xbd, 0x9d, 0x7f,
                                    0x3e, 0x7d, 0xc2, 0x19, 0x7c,
                                    0x5e, 0xff, 0x14, 0xe8, 0x8c};
  fuzz_seed_bytes(&seed, varints, sizeof(varints));
  fuzz_seed_emit(writer, &seed);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  FUZZ_CHECK(vkr_wire_schema_validate(&fuzz_wire_schema));
  fuzz_seed_mode(fuzz_wire_seeds);
  return 0;
}
