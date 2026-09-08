#include "assets/vkr_diffuse_volume.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(CHAR_BIT == 8, "DVOL requires 8-bit bytes");
_Static_assert(sizeof(float32_t) == 4u, "DVOL requires 32-bit float32_t");
_Static_assert(sizeof(VkrShL2Packed) == VKR_SH_SLOT_BYTES, "DVOL SH ABI drift");

#define VKR_DIFFUSE_VOLUME_H_MAGIC 0u
#define VKR_DIFFUSE_VOLUME_H_VERSION 4u
#define VKR_DIFFUSE_VOLUME_H_ENDIAN 8u
#define VKR_DIFFUSE_VOLUME_H_SIZE 12u
#define VKR_DIFFUSE_VOLUME_H_FILE_SIZE 16u
#define VKR_DIFFUSE_VOLUME_H_DIM_X 24u
#define VKR_DIFFUSE_VOLUME_H_DIM_Y 28u
#define VKR_DIFFUSE_VOLUME_H_DIM_Z 32u
#define VKR_DIFFUSE_VOLUME_H_PROBE_COUNT 36u
#define VKR_DIFFUSE_VOLUME_H_CELL_COUNT 40u
#define VKR_DIFFUSE_VOLUME_H_RESERVED0 44u
#define VKR_DIFFUSE_VOLUME_H_ORIGIN 48u
#define VKR_DIFFUSE_VOLUME_H_SPACING 60u
#define VKR_DIFFUSE_VOLUME_H_PROBE_OFFSET 72u
#define VKR_DIFFUSE_VOLUME_H_CELL_OFFSET 80u
#define VKR_DIFFUSE_VOLUME_H_PROBE_BYTES 88u
#define VKR_DIFFUSE_VOLUME_H_CELL_BYTES 92u
#define VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC 96u
#define VKR_DIFFUSE_VOLUME_H_HEADER_CRC 100u
#define VKR_DIFFUSE_VOLUME_H_RESERVED1 104u
#define VKR_DIFFUSE_VOLUME_H_RESERVED2 108u

static bool8_t vkr_diffuse_volume_add(uint64_t left, uint64_t right,
                                      uint64_t *out_result) {
  if (left > UINT64_MAX - right)
    return false_v;
  *out_result = left + right;
  return true_v;
}

static bool8_t vkr_diffuse_volume_mul(uint64_t left, uint64_t right,
                                      uint64_t *out_result) {
  if (left != 0u && right > UINT64_MAX / left)
    return false_v;
  *out_result = left * right;
  return true_v;
}

static uint32_t vkr_diffuse_volume_read_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t vkr_diffuse_volume_read_u64(const uint8_t *bytes) {
  return (uint64_t)vkr_diffuse_volume_read_u32(bytes) |
         ((uint64_t)vkr_diffuse_volume_read_u32(bytes + 4u) << 32u);
}

static void vkr_diffuse_volume_write_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void vkr_diffuse_volume_write_u64(uint8_t *bytes, uint64_t value) {
  vkr_diffuse_volume_write_u32(bytes, (uint32_t)value);
  vkr_diffuse_volume_write_u32(bytes + 4u, (uint32_t)(value >> 32u));
}

static float32_t vkr_diffuse_volume_read_f32(const uint8_t *bytes) {
  const uint32_t bits = vkr_diffuse_volume_read_u32(bytes);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static void vkr_diffuse_volume_write_f32(uint8_t *bytes, float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_diffuse_volume_write_u32(bytes, bits);
}

static bool8_t vkr_diffuse_volume_float_format_supported(void) {
  const float32_t one = 1.0f;
  const float32_t negative_half = -0.5f;
  uint32_t one_bits = 0u;
  uint32_t negative_half_bits = 0u;
  MemCopy(&one_bits, &one, sizeof(one_bits));
  MemCopy(&negative_half_bits, &negative_half, sizeof(negative_half_bits));
  return FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
         one_bits == 0x3f800000u && negative_half_bits == 0xbf000000u;
}

static uint32_t vkr_diffuse_volume_crc32_update(uint32_t crc,
                                                const uint8_t *bytes,
                                                uint64_t size) {
  for (uint64_t index = 0u; index < size; ++index) {
    crc ^= bytes[index];
    for (uint32_t bit = 0u; bit < 8u; ++bit)
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc;
}

static uint32_t vkr_diffuse_volume_crc32(const uint8_t *bytes, uint64_t size) {
  return ~vkr_diffuse_volume_crc32_update(0xffffffffu, bytes, size);
}

static uint32_t vkr_diffuse_volume_header_crc(const uint8_t *header) {
  uint32_t crc = vkr_diffuse_volume_crc32_update(
      0xffffffffu, header, VKR_DIFFUSE_VOLUME_H_HEADER_CRC);
  const uint8_t zero_crc[4] = {0u, 0u, 0u, 0u};
  crc = vkr_diffuse_volume_crc32_update(crc, zero_crc, sizeof(zero_crc));
  crc = vkr_diffuse_volume_crc32_update(
      crc, header + VKR_DIFFUSE_VOLUME_H_HEADER_CRC + sizeof(zero_crc),
      VKR_DIFFUSE_VOLUME_HEADER_BYTES -
          (VKR_DIFFUSE_VOLUME_H_HEADER_CRC + sizeof(zero_crc)));
  return ~crc;
}

static bool8_t vkr_diffuse_volume_finite_vec3(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static bool8_t vkr_diffuse_volume_layout(uint32_t dimension_x,
                                         uint32_t dimension_y,
                                         uint32_t dimension_z,
                                         uint32_t *out_probe_count,
                                         uint32_t *out_cell_count) {
  if (dimension_x < 2u || dimension_y < 2u || dimension_z < 2u ||
      dimension_x > VKR_DIFFUSE_VOLUME_MAX_PROBES ||
      dimension_y > VKR_DIFFUSE_VOLUME_MAX_PROBES ||
      dimension_z > VKR_DIFFUSE_VOLUME_MAX_PROBES)
    return false_v;

  uint64_t probe_count = 0u;
  uint64_t cell_count = 0u;
  if (!vkr_diffuse_volume_mul(dimension_x, dimension_y, &probe_count) ||
      !vkr_diffuse_volume_mul(probe_count, dimension_z, &probe_count) ||
      probe_count > VKR_DIFFUSE_VOLUME_MAX_PROBES ||
      !vkr_diffuse_volume_mul(dimension_x - 1u, dimension_y - 1u,
                              &cell_count) ||
      !vkr_diffuse_volume_mul(cell_count, dimension_z - 1u, &cell_count) ||
      cell_count > UINT32_MAX)
    return false_v;

  *out_probe_count = (uint32_t)probe_count;
  *out_cell_count = (uint32_t)cell_count;
  return true_v;
}

static uint32_t vkr_diffuse_volume_probe_index(const VkrDiffuseVolume *volume,
                                               uint32_t x, uint32_t y,
                                               uint32_t z) {
  return x + volume->dimensions[0] * (y + volume->dimensions[1] * z);
}

static bool8_t
vkr_diffuse_volume_cell_region_valid(const VkrDiffuseVolume *volume,
                                     uint32_t cell_x, uint32_t cell_y,
                                     uint32_t cell_z, uint32_t region_id) {
  if (region_id == 0u)
    return true_v;
  const uint32_t corners[] = {
      vkr_diffuse_volume_probe_index(volume, cell_x, cell_y, cell_z),
      vkr_diffuse_volume_probe_index(volume, cell_x + 1u, cell_y, cell_z),
      vkr_diffuse_volume_probe_index(volume, cell_x, cell_y + 1u, cell_z),
      vkr_diffuse_volume_probe_index(volume, cell_x + 1u, cell_y + 1u, cell_z),
      vkr_diffuse_volume_probe_index(volume, cell_x, cell_y, cell_z + 1u),
      vkr_diffuse_volume_probe_index(volume, cell_x + 1u, cell_y, cell_z + 1u),
      vkr_diffuse_volume_probe_index(volume, cell_x, cell_y + 1u, cell_z + 1u),
      vkr_diffuse_volume_probe_index(volume, cell_x + 1u, cell_y + 1u,
                                     cell_z + 1u),
  };
  for (uint32_t corner = 0u; corner < ArrayCount(corners); ++corner)
    if (volume->probes[corners[corner]].region_id != region_id)
      return false_v;
  return true_v;
}

static bool8_t vkr_diffuse_volume_valid(const VkrDiffuseVolume *volume) {
  if (!volume || !volume->probes || !volume->cell_region_ids ||
      !vkr_diffuse_volume_finite_vec3(volume->origin) ||
      !vkr_diffuse_volume_finite_vec3(volume->spacing) ||
      !(volume->spacing.x > 0.0f) || !(volume->spacing.y > 0.0f) ||
      !(volume->spacing.z > 0.0f))
    return false_v;

  uint32_t expected_probe_count = 0u;
  uint32_t expected_cell_count = 0u;
  if (!vkr_diffuse_volume_layout(volume->dimensions[0], volume->dimensions[1],
                                 volume->dimensions[2], &expected_probe_count,
                                 &expected_cell_count) ||
      volume->probe_count != expected_probe_count ||
      volume->cell_count != expected_cell_count)
    return false_v;

  for (uint32_t probe_index = 0u; probe_index < volume->probe_count;
       ++probe_index) {
    if (volume->probes[probe_index].region_id >
        VKR_DIFFUSE_VOLUME_MAX_REGION_ID)
      return false_v;
    for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
         ++vector_index)
      for (uint32_t component = 0u; component < 4u; ++component)
        if (!isfinite(
                volume->probes[probe_index].sh.v[vector_index][component]))
          return false_v;
  }

  uint32_t cell_index = 0u;
  for (uint32_t z = 0u; z + 1u < volume->dimensions[2]; ++z)
    for (uint32_t y = 0u; y + 1u < volume->dimensions[1]; ++y)
      for (uint32_t x = 0u; x + 1u < volume->dimensions[0]; ++x) {
        const uint32_t region_id = volume->cell_region_ids[cell_index++];
        if (region_id > VKR_DIFFUSE_VOLUME_MAX_REGION_ID ||
            !vkr_diffuse_volume_cell_region_valid(volume, x, y, z,
                                                   region_id))
          return false_v;
      }
  return true_v;
}

static void vkr_diffuse_volume_write_probe(uint8_t *bytes,
                                           const VkrDiffuseVolumeProbe *probe) {
  vkr_diffuse_volume_write_u32(bytes, probe->region_id);
  bytes += sizeof(uint32_t);
  for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
       ++vector_index)
    for (uint32_t component = 0u; component < 4u; ++component) {
      vkr_diffuse_volume_write_f32(bytes, probe->sh.v[vector_index][component]);
      bytes += sizeof(float32_t);
    }
}

static void vkr_diffuse_volume_read_probe(const uint8_t *bytes,
                                          VkrDiffuseVolumeProbe *out_probe) {
  out_probe->region_id = vkr_diffuse_volume_read_u32(bytes);
  bytes += sizeof(uint32_t);
  for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
       ++vector_index)
    for (uint32_t component = 0u; component < 4u; ++component) {
      out_probe->sh.v[vector_index][component] =
          vkr_diffuse_volume_read_f32(bytes);
      bytes += sizeof(float32_t);
    }
}

bool8_t vkr_diffuse_volume_encode(const VkrDiffuseVolume *volume, Arena *arena,
                                  const uint8_t **out_bytes,
                                  uint64_t *out_size) {
  if (!out_bytes || !out_size)
    return false_v;
  *out_bytes = NULL;
  *out_size = 0u;
  if (!arena || !vkr_diffuse_volume_float_format_supported() ||
      !vkr_diffuse_volume_valid(volume))
    return false_v;

  uint64_t probe_bytes = 0u;
  uint64_t cell_bytes = 0u;
  uint64_t probe_offset = VKR_DIFFUSE_VOLUME_HEADER_BYTES;
  uint64_t cell_offset = 0u;
  uint64_t file_size = 0u;
  if (!vkr_diffuse_volume_mul(volume->probe_count,
                              VKR_DIFFUSE_VOLUME_PROBE_BYTES, &probe_bytes) ||
      !vkr_diffuse_volume_add(probe_offset, probe_bytes, &cell_offset) ||
      !vkr_diffuse_volume_mul(volume->cell_count, VKR_DIFFUSE_VOLUME_CELL_BYTES,
                              &cell_bytes) ||
      !vkr_diffuse_volume_add(cell_offset, cell_bytes, &file_size))
    return false_v;

  uint8_t *bytes =
      (uint8_t *)arena_alloc(arena, file_size, ARENA_MEMORY_TAG_FILE);
  if (!bytes)
    return false_v;
  MemZero(bytes, file_size);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC,
                               VKR_DIFFUSE_VOLUME_MAGIC);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION,
                               VKR_DIFFUSE_VOLUME_VERSION);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN,
                               VKR_DIFFUSE_VOLUME_ENDIAN_TAG);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE,
                               VKR_DIFFUSE_VOLUME_HEADER_BYTES);
  vkr_diffuse_volume_write_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE,
                               file_size);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_X,
                               volume->dimensions[0]);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Y,
                               volume->dimensions[1]);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Z,
                               volume->dimensions[2]);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_COUNT,
                               volume->probe_count);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_COUNT,
                               volume->cell_count);
  for (uint32_t component = 0u; component < 3u; ++component) {
    vkr_diffuse_volume_write_f32(bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN +
                                     component * sizeof(float32_t),
                                 volume->origin.elements[component]);
    vkr_diffuse_volume_write_f32(bytes + VKR_DIFFUSE_VOLUME_H_SPACING +
                                     component * sizeof(float32_t),
                                 volume->spacing.elements[component]);
  }
  vkr_diffuse_volume_write_u64(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_OFFSET,
                               probe_offset);
  vkr_diffuse_volume_write_u64(bytes + VKR_DIFFUSE_VOLUME_H_CELL_OFFSET,
                               cell_offset);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_BYTES,
                               VKR_DIFFUSE_VOLUME_PROBE_BYTES);
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_BYTES,
                               VKR_DIFFUSE_VOLUME_CELL_BYTES);

  for (uint32_t index = 0u; index < volume->probe_count; ++index)
    vkr_diffuse_volume_write_probe(
        bytes + probe_offset + (uint64_t)index * VKR_DIFFUSE_VOLUME_PROBE_BYTES,
        &volume->probes[index]);
  for (uint32_t index = 0u; index < volume->cell_count; ++index)
    vkr_diffuse_volume_write_u32(
        bytes + cell_offset + (uint64_t)index * VKR_DIFFUSE_VOLUME_CELL_BYTES,
        volume->cell_region_ids[index]);

  vkr_diffuse_volume_write_u32(
      bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC,
      vkr_diffuse_volume_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                               file_size - VKR_DIFFUSE_VOLUME_HEADER_BYTES));
  vkr_diffuse_volume_write_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC,
                               vkr_diffuse_volume_header_crc(bytes));
  *out_bytes = bytes;
  *out_size = file_size;
  return true_v;
}

bool8_t vkr_diffuse_volume_decode(const uint8_t *bytes, uint64_t size,
                                  Arena *arena, VkrDiffuseVolume *out_volume) {
  if (!out_volume)
    return false_v;
  *out_volume = (VkrDiffuseVolume){0};
  if (!bytes || !arena || size < VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      !vkr_diffuse_volume_float_format_supported())
    return false_v;
  if (vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC) !=
          VKR_DIFFUSE_VOLUME_MAGIC ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION) !=
          VKR_DIFFUSE_VOLUME_VERSION ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN) !=
          VKR_DIFFUSE_VOLUME_ENDIAN_TAG ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE) !=
          VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      vkr_diffuse_volume_read_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE) !=
          size ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_RESERVED0) !=
          0u ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_RESERVED1) !=
          0u ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_RESERVED2) !=
          0u ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC) !=
          vkr_diffuse_volume_header_crc(bytes))
    return false_v;

  const uint32_t dimensions[3] = {
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_X),
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Y),
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Z),
  };
  uint32_t expected_probe_count = 0u;
  uint32_t expected_cell_count = 0u;
  if (!vkr_diffuse_volume_layout(dimensions[0], dimensions[1], dimensions[2],
                                 &expected_probe_count, &expected_cell_count) ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_COUNT) !=
          expected_probe_count ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_COUNT) !=
          expected_cell_count ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_BYTES) !=
          VKR_DIFFUSE_VOLUME_PROBE_BYTES ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_BYTES) !=
          VKR_DIFFUSE_VOLUME_CELL_BYTES)
    return false_v;

  uint64_t probe_bytes = 0u;
  uint64_t cell_bytes = 0u;
  uint64_t expected_cell_offset = 0u;
  uint64_t expected_file_size = 0u;
  if (!vkr_diffuse_volume_mul(expected_probe_count,
                              VKR_DIFFUSE_VOLUME_PROBE_BYTES, &probe_bytes) ||
      !vkr_diffuse_volume_add(VKR_DIFFUSE_VOLUME_HEADER_BYTES, probe_bytes,
                              &expected_cell_offset) ||
      !vkr_diffuse_volume_mul(expected_cell_count,
                              VKR_DIFFUSE_VOLUME_CELL_BYTES, &cell_bytes) ||
      !vkr_diffuse_volume_add(expected_cell_offset, cell_bytes,
                              &expected_file_size) ||
      vkr_diffuse_volume_read_u64(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_OFFSET) !=
          VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      vkr_diffuse_volume_read_u64(bytes + VKR_DIFFUSE_VOLUME_H_CELL_OFFSET) !=
          expected_cell_offset ||
      expected_file_size != size ||
      vkr_diffuse_volume_read_u32(bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC) !=
          vkr_diffuse_volume_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                                   size - VKR_DIFFUSE_VOLUME_HEADER_BYTES))
    return false_v;

  Vec3 origin = vec3_zero();
  Vec3 spacing = vec3_zero();
  for (uint32_t component = 0u; component < 3u; ++component) {
    origin.elements[component] = vkr_diffuse_volume_read_f32(
        bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN + component * sizeof(float32_t));
    spacing.elements[component] = vkr_diffuse_volume_read_f32(
        bytes + VKR_DIFFUSE_VOLUME_H_SPACING + component * sizeof(float32_t));
  }
  if (!vkr_diffuse_volume_finite_vec3(origin) ||
      !vkr_diffuse_volume_finite_vec3(spacing) || !(spacing.x > 0.0f) ||
      !(spacing.y > 0.0f) || !(spacing.z > 0.0f))
    return false_v;

  const Scratch scope = scratch_create(arena);
  VkrDiffuseVolumeProbe *probes = (VkrDiffuseVolumeProbe *)arena_alloc(
      arena, (uint64_t)expected_probe_count * sizeof(*probes),
      ARENA_MEMORY_TAG_ARRAY);
  uint32_t *cell_region_ids = (uint32_t *)arena_alloc(
      arena, (uint64_t)expected_cell_count * sizeof(*cell_region_ids),
      ARENA_MEMORY_TAG_ARRAY);
  if (!probes || !cell_region_ids) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }

  const uint64_t probe_offset = VKR_DIFFUSE_VOLUME_HEADER_BYTES;
  const uint64_t cell_offset = expected_cell_offset;
  for (uint32_t index = 0u; index < expected_probe_count; ++index)
    vkr_diffuse_volume_read_probe(
        bytes + probe_offset + (uint64_t)index * VKR_DIFFUSE_VOLUME_PROBE_BYTES,
        &probes[index]);
  for (uint32_t index = 0u; index < expected_cell_count; ++index)
    cell_region_ids[index] = vkr_diffuse_volume_read_u32(
        bytes + cell_offset + (uint64_t)index * VKR_DIFFUSE_VOLUME_CELL_BYTES);

  VkrDiffuseVolume volume = {
      .origin = origin,
      .spacing = spacing,
      .dimensions = {dimensions[0], dimensions[1], dimensions[2]},
      .probes = probes,
      .probe_count = expected_probe_count,
      .cell_region_ids = cell_region_ids,
      .cell_count = expected_cell_count,
  };
  if (!vkr_diffuse_volume_valid(&volume)) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  *out_volume = volume;
  return true_v;
}
