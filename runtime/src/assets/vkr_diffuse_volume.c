#include "assets/vkr_diffuse_volume.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(CHAR_BIT == 8, "DVOL requires 8-bit bytes");
_Static_assert(sizeof(float32_t) == 4u, "DVOL requires 32-bit float32_t");
_Static_assert(sizeof(VkrShL2Packed) == VKR_SH_SLOT_BYTES, "DVOL SH ABI drift");
_Static_assert(VKR_DIFFUSE_VOLUME_SH_BYTES == VKR_SH_SLOT_BYTES,
               "DVOL SH record drift");

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
#define VKR_DIFFUSE_VOLUME_H_LAYER_COUNT 44u
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

static uint32_t vkr_diffuse_volume_header_crc(const uint8_t *header) {
  uint32_t crc = vkr_crc32_update(VKR_CRC32_INITIAL, header,
                                  VKR_DIFFUSE_VOLUME_H_HEADER_CRC);
  const uint8_t zero_crc[4] = {0u, 0u, 0u, 0u};
  crc = vkr_crc32_update(crc, zero_crc, sizeof(zero_crc));
  crc = vkr_crc32_update(
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
  if (!vkr_checked_mul_u64(dimension_x, dimension_y, &probe_count) ||
      !vkr_checked_mul_u64(probe_count, dimension_z, &probe_count) ||
      probe_count > VKR_DIFFUSE_VOLUME_MAX_PROBES ||
      !vkr_checked_mul_u64(dimension_x - 1u, dimension_y - 1u, &cell_count) ||
      !vkr_checked_mul_u64(cell_count, dimension_z - 1u, &cell_count) ||
      cell_count > UINT32_MAX)
    return false_v;

  *out_probe_count = (uint32_t)probe_count;
  *out_cell_count = (uint32_t)cell_count;
  return true_v;
}

/* A probe record: its region, then one SH per layer. */
static uint64_t vkr_diffuse_volume_probe_bytes(uint32_t layer_count) {
  return sizeof(uint32_t) + (uint64_t)layer_count * VKR_DIFFUSE_VOLUME_SH_BYTES;
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
    if (volume->probe_region_ids[corners[corner]] != region_id)
      return false_v;
  return true_v;
}

static bool8_t vkr_diffuse_volume_valid(const VkrDiffuseVolume *volume) {
  if (!volume || !volume->probe_region_ids || !volume->probe_sh ||
      !volume->cell_region_ids || volume->layer_count == 0u ||
      volume->layer_count > VKR_DIFFUSE_VOLUME_MAX_LAYERS ||
      !vkr_light_layers_valid(volume->layers, volume->layer_count) ||
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
    if (volume->probe_region_ids[probe_index] >
        VKR_DIFFUSE_VOLUME_MAX_REGION_ID)
      return false_v;
  }
  const uint64_t sh_count = (uint64_t)volume->probe_count * volume->layer_count;
  for (uint64_t sh_index = 0u; sh_index < sh_count; ++sh_index)
    for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
         ++vector_index)
      for (uint32_t component = 0u; component < 4u; ++component)
        if (!isfinite(volume->probe_sh[sh_index].v[vector_index][component]))
          return false_v;

  uint32_t cell_index = 0u;
  for (uint32_t z = 0u; z + 1u < volume->dimensions[2]; ++z)
    for (uint32_t y = 0u; y + 1u < volume->dimensions[1]; ++y)
      for (uint32_t x = 0u; x + 1u < volume->dimensions[0]; ++x) {
        const uint32_t region_id = volume->cell_region_ids[cell_index++];
        if (region_id > VKR_DIFFUSE_VOLUME_MAX_REGION_ID ||
            !vkr_diffuse_volume_cell_region_valid(volume, x, y, z, region_id))
          return false_v;
      }
  return true_v;
}

static void vkr_diffuse_volume_write_probe(uint8_t *bytes, uint32_t region_id,
                                           const VkrShL2Packed *sh,
                                           uint32_t layer_count) {
  vkr_store_le_u32(bytes, region_id);
  bytes += sizeof(uint32_t);
  for (uint32_t layer = 0u; layer < layer_count; ++layer)
    for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
         ++vector_index)
      for (uint32_t component = 0u; component < 4u; ++component) {
        vkr_store_le_f32(bytes, sh[layer].v[vector_index][component]);
        bytes += sizeof(float32_t);
      }
}

static void vkr_diffuse_volume_read_probe(const uint8_t *bytes,
                                          uint32_t layer_count,
                                          uint32_t *out_region_id,
                                          VkrShL2Packed *out_sh) {
  *out_region_id = vkr_load_le_u32(bytes);
  bytes += sizeof(uint32_t);
  for (uint32_t layer = 0u; layer < layer_count; ++layer)
    for (uint32_t vector_index = 0u; vector_index < VKR_SH_PACKED_VECTOR_COUNT;
         ++vector_index)
      for (uint32_t component = 0u; component < 4u; ++component) {
        out_sh[layer].v[vector_index][component] = vkr_load_le_f32(bytes);
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
  if (!arena || !vkr_f32_is_binary32() || !vkr_diffuse_volume_valid(volume))
    return false_v;

  const uint64_t probe_record =
      vkr_diffuse_volume_probe_bytes(volume->layer_count);
  uint64_t probe_bytes = 0u;
  uint64_t cell_bytes = 0u;
  const uint64_t probe_offset =
      VKR_DIFFUSE_VOLUME_HEADER_BYTES +
      (uint64_t)volume->layer_count * VKR_LIGHT_LAYER_RECORD_BYTES;
  uint64_t cell_offset = 0u;
  uint64_t file_size = 0u;
  if (!vkr_checked_mul_u64(volume->probe_count, probe_record, &probe_bytes) ||
      !vkr_checked_add_u64(probe_offset, probe_bytes, &cell_offset) ||
      !vkr_checked_mul_u64(volume->cell_count, VKR_DIFFUSE_VOLUME_CELL_BYTES,
                           &cell_bytes) ||
      !vkr_checked_add_u64(cell_offset, cell_bytes, &file_size))
    return false_v;

  uint8_t *bytes =
      (uint8_t *)arena_alloc(arena, file_size, ARENA_MEMORY_TAG_FILE);
  if (!bytes)
    return false_v;
  MemZero(bytes, file_size);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC,
                   VKR_DIFFUSE_VOLUME_MAGIC);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION,
                   VKR_DIFFUSE_VOLUME_VERSION);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN,
                   VKR_DIFFUSE_VOLUME_ENDIAN_TAG);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE,
                   VKR_DIFFUSE_VOLUME_HEADER_BYTES);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE, file_size);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_X, volume->dimensions[0]);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Y, volume->dimensions[1]);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Z, volume->dimensions[2]);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_COUNT,
                   volume->probe_count);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_COUNT, volume->cell_count);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAYER_COUNT,
                   volume->layer_count);
  for (uint32_t component = 0u; component < 3u; ++component) {
    vkr_store_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN +
                         component * sizeof(float32_t),
                     volume->origin.elements[component]);
    vkr_store_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_SPACING +
                         component * sizeof(float32_t),
                     volume->spacing.elements[component]);
  }
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_OFFSET, probe_offset);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_CELL_OFFSET, cell_offset);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_BYTES,
                   (uint32_t)probe_record);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_BYTES,
                   VKR_DIFFUSE_VOLUME_CELL_BYTES);

  for (uint32_t layer = 0u; layer < volume->layer_count; ++layer)
    vkr_light_layer_write(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES +
                              (uint64_t)layer * VKR_LIGHT_LAYER_RECORD_BYTES,
                          &volume->layers[layer]);
  for (uint32_t index = 0u; index < volume->probe_count; ++index)
    vkr_diffuse_volume_write_probe(
        bytes + probe_offset + (uint64_t)index * probe_record,
        volume->probe_region_ids[index],
        &volume->probe_sh[(uint64_t)index * volume->layer_count],
        volume->layer_count);
  for (uint32_t index = 0u; index < volume->cell_count; ++index)
    vkr_store_le_u32(bytes + cell_offset +
                         (uint64_t)index * VKR_DIFFUSE_VOLUME_CELL_BYTES,
                     volume->cell_region_ids[index]);

  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC,
                   vkr_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                             file_size - VKR_DIFFUSE_VOLUME_HEADER_BYTES));
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC,
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
      !vkr_f32_is_binary32())
    return false_v;
  if (vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC) !=
          VKR_DIFFUSE_VOLUME_MAGIC ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION) !=
          VKR_DIFFUSE_VOLUME_VERSION ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN) !=
          VKR_DIFFUSE_VOLUME_ENDIAN_TAG ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE) !=
          VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE) != size ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_RESERVED1) != 0u ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_RESERVED2) != 0u ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC) !=
          vkr_diffuse_volume_header_crc(bytes))
    return false_v;

  const uint32_t dimensions[3] = {
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_X),
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Y),
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIM_Z),
  };
  const uint32_t layer_count =
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAYER_COUNT);
  if (layer_count == 0u || layer_count > VKR_DIFFUSE_VOLUME_MAX_LAYERS)
    return false_v;
  const uint64_t probe_record = vkr_diffuse_volume_probe_bytes(layer_count);
  const uint64_t probe_offset =
      VKR_DIFFUSE_VOLUME_HEADER_BYTES +
      (uint64_t)layer_count * VKR_LIGHT_LAYER_RECORD_BYTES;
  uint32_t expected_probe_count = 0u;
  uint32_t expected_cell_count = 0u;
  if (!vkr_diffuse_volume_layout(dimensions[0], dimensions[1], dimensions[2],
                                 &expected_probe_count, &expected_cell_count) ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_COUNT) !=
          expected_probe_count ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_COUNT) !=
          expected_cell_count ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_BYTES) !=
          probe_record ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_CELL_BYTES) !=
          VKR_DIFFUSE_VOLUME_CELL_BYTES)
    return false_v;

  uint64_t probe_bytes = 0u;
  uint64_t cell_bytes = 0u;
  uint64_t expected_cell_offset = 0u;
  uint64_t expected_file_size = 0u;
  if (!vkr_checked_mul_u64(expected_probe_count, probe_record, &probe_bytes) ||
      !vkr_checked_add_u64(probe_offset, probe_bytes, &expected_cell_offset) ||
      !vkr_checked_mul_u64(expected_cell_count, VKR_DIFFUSE_VOLUME_CELL_BYTES,
                           &cell_bytes) ||
      !vkr_checked_add_u64(expected_cell_offset, cell_bytes,
                           &expected_file_size) ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_PROBE_OFFSET) !=
          probe_offset ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_CELL_OFFSET) !=
          expected_cell_offset ||
      expected_file_size != size ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC) !=
          vkr_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                    size - VKR_DIFFUSE_VOLUME_HEADER_BYTES))
    return false_v;

  Vec3 origin = vec3_zero();
  Vec3 spacing = vec3_zero();
  for (uint32_t component = 0u; component < 3u; ++component) {
    origin.elements[component] = vkr_load_le_f32(
        bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN + component * sizeof(float32_t));
    spacing.elements[component] = vkr_load_le_f32(
        bytes + VKR_DIFFUSE_VOLUME_H_SPACING + component * sizeof(float32_t));
  }
  if (!vkr_diffuse_volume_finite_vec3(origin) ||
      !vkr_diffuse_volume_finite_vec3(spacing) || !(spacing.x > 0.0f) ||
      !(spacing.y > 0.0f) || !(spacing.z > 0.0f))
    return false_v;

  const Scratch scope = scratch_create(arena);
  VkrLightLayer *layers = (VkrLightLayer *)arena_alloc(
      arena, (uint64_t)layer_count * sizeof(*layers), ARENA_MEMORY_TAG_ARRAY);
  uint32_t *probe_region_ids = (uint32_t *)arena_alloc(
      arena, (uint64_t)expected_probe_count * sizeof(*probe_region_ids),
      ARENA_MEMORY_TAG_ARRAY);
  VkrShL2Packed *probe_sh = (VkrShL2Packed *)arena_alloc(
      arena, (uint64_t)expected_probe_count * layer_count * sizeof(*probe_sh),
      ARENA_MEMORY_TAG_ARRAY);
  uint32_t *cell_region_ids = (uint32_t *)arena_alloc(
      arena, (uint64_t)expected_cell_count * sizeof(*cell_region_ids),
      ARENA_MEMORY_TAG_ARRAY);
  if (!layers || !probe_region_ids || !probe_sh || !cell_region_ids) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }

  for (uint32_t layer = 0u; layer < layer_count; ++layer) {
    if (!vkr_light_layer_read(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES +
                                  (uint64_t)layer *
                                      VKR_LIGHT_LAYER_RECORD_BYTES,
                              &layers[layer])) {
      scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
      return false_v;
    }
  }
  const uint64_t cell_offset = expected_cell_offset;
  for (uint32_t index = 0u; index < expected_probe_count; ++index)
    vkr_diffuse_volume_read_probe(
        bytes + probe_offset + (uint64_t)index * probe_record, layer_count,
        &probe_region_ids[index], &probe_sh[(uint64_t)index * layer_count]);
  for (uint32_t index = 0u; index < expected_cell_count; ++index)
    cell_region_ids[index] = vkr_load_le_u32(
        bytes + cell_offset + (uint64_t)index * VKR_DIFFUSE_VOLUME_CELL_BYTES);

  VkrDiffuseVolume volume = {
      .origin = origin,
      .spacing = spacing,
      .dimensions = {dimensions[0], dimensions[1], dimensions[2]},
      .probe_region_ids = probe_region_ids,
      .probe_sh = probe_sh,
      .probe_count = expected_probe_count,
      .layers = layers,
      .layer_count = layer_count,
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
