#include "assets/vkr_lightmap_set.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(CHAR_BIT == 8, "VKLM requires 8-bit bytes");
_Static_assert(sizeof(float32_t) == 4u, "VKLM requires 32-bit float32_t");

#define VKR_LIGHTMAP_SET_H_MAGIC 0u
#define VKR_LIGHTMAP_SET_H_VERSION 4u
#define VKR_LIGHTMAP_SET_H_ENDIAN 8u
#define VKR_LIGHTMAP_SET_H_SIZE 12u
#define VKR_LIGHTMAP_SET_H_FILE_SIZE 16u
#define VKR_LIGHTMAP_SET_H_PAGE_SIZE 24u
#define VKR_LIGHTMAP_SET_H_PAGE_COUNT 28u
#define VKR_LIGHTMAP_SET_H_LAYER_COUNT 32u
#define VKR_LIGHTMAP_SET_H_INSTANCE_COUNT 36u
#define VKR_LIGHTMAP_SET_H_PLANE_COUNT 40u
#define VKR_LIGHTMAP_SET_H_TEXELS_PER_UNIT 44u
#define VKR_LIGHTMAP_SET_H_LAYER_OFFSET 48u
#define VKR_LIGHTMAP_SET_H_INSTANCE_OFFSET 56u
#define VKR_LIGHTMAP_SET_H_PAYLOAD_OFFSET 64u
#define VKR_LIGHTMAP_SET_H_PAYLOAD_BYTES 72u
#define VKR_LIGHTMAP_SET_H_TABLES_CRC 80u
#define VKR_LIGHTMAP_SET_H_PAYLOAD_CRC 84u
#define VKR_LIGHTMAP_SET_H_HEADER_CRC 88u
#define VKR_LIGHTMAP_SET_H_PLANE_OFFSET 96u
/* Bytes 92 to 95 and 104 to 127 are reserved and zero. */
#define VKR_LIGHTMAP_SET_H_RESERVED_A 92u
#define VKR_LIGHTMAP_SET_H_RESERVED_B 104u

typedef struct VkrLightmapSetLayout {
  uint64_t plane_offset;
  uint64_t instance_offset;
  uint64_t payload_offset;
  uint64_t payload_bytes;
  uint64_t file_size;
} VkrLightmapSetLayout;

static uint32_t vkr_lightmap_set_header_crc(const uint8_t *header) {
  uint32_t crc = vkr_crc32_update(VKR_CRC32_INITIAL, header,
                                  VKR_LIGHTMAP_SET_H_HEADER_CRC);
  const uint8_t zero_crc[4] = {0u, 0u, 0u, 0u};
  crc = vkr_crc32_update(crc, zero_crc, sizeof(zero_crc));
  crc = vkr_crc32_update(
      crc, header + VKR_LIGHTMAP_SET_H_HEADER_CRC + sizeof(zero_crc),
      VKR_LIGHTMAP_SET_HEADER_BYTES -
          (VKR_LIGHTMAP_SET_H_HEADER_CRC + sizeof(zero_crc)));
  return ~crc;
}

bool8_t vkr_lightmap_format_desktop(VkrLightmapPlaneFormat format) {
  return format == VKR_LIGHTMAP_FORMAT_RGB9E5 ||
         format == VKR_LIGHTMAP_FORMAT_BC6H ||
         format == VKR_LIGHTMAP_FORMAT_RGBA8 ||
         format == VKR_LIGHTMAP_FORMAT_BC7;
}

uint64_t vkr_lightmap_set_plane_bytes(VkrLightmapPlaneFormat format,
                                      uint32_t page_size) {
  const uint64_t texels = (uint64_t)page_size * page_size;
  switch (format) {
  case VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR:
  case VKR_LIGHTMAP_FORMAT_ASTC_4X4_LDR:
  case VKR_LIGHTMAP_FORMAT_BC6H:
  case VKR_LIGHTMAP_FORMAT_BC7:
    return texels / 16u * VKR_LIGHTMAP_SET_BLOCK_BYTES;
  case VKR_LIGHTMAP_FORMAT_RGB9E5:
  case VKR_LIGHTMAP_FORMAT_RGBA8:
    return texels * 4u;
  }
  return 0u;
}

uint64_t vkr_lightmap_set_page_stride(const VkrLightmapSet *set) {
  uint64_t stride = 0u;
  for (uint32_t i = 0u; i < set->plane_count; ++i) {
    stride +=
        vkr_lightmap_set_plane_bytes(set->planes[i].format, set->page_size);
  }
  return stride;
}

uint64_t vkr_lightmap_set_plane_offset(const VkrLightmapSet *set, uint32_t page,
                                       uint32_t plane) {
  uint64_t offset = (uint64_t)page * vkr_lightmap_set_page_stride(set);
  for (uint32_t i = 0u; i < plane; ++i) {
    offset +=
        vkr_lightmap_set_plane_bytes(set->planes[i].format, set->page_size);
  }
  return offset;
}

uint32_t vkr_lightmap_set_find_plane(const VkrLightmapSet *set, uint32_t layer,
                                     VkrLightmapPlaneKind kind,
                                     bool8_t desktop) {
  for (uint32_t i = 0u; i < set->plane_count; ++i) {
    const VkrLightmapPlane *plane = &set->planes[i];
    if (plane->layer == layer && plane->kind == kind &&
        vkr_lightmap_format_desktop(plane->format) == desktop) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* Sizes and offsets that follow from the counts and plane encodings; false
   when out of range. `planes` is validated by the caller. */
static bool8_t vkr_lightmap_set_compute_layout(const VkrLightmapSet *set,
                                               VkrLightmapSetLayout *out) {
  if (set->page_size < 4u || set->page_size > VKR_LIGHTMAP_SET_MAX_PAGE_SIZE ||
      set->page_size % 4u != 0u ||
      set->page_count > VKR_LIGHTMAP_SET_MAX_PAGES || set->layer_count == 0u ||
      set->layer_count > VKR_LIGHTMAP_SET_MAX_LAYERS ||
      set->plane_count == 0u ||
      set->plane_count > VKR_LIGHTMAP_SET_MAX_PLANES ||
      (set->plane_count != 0u && !set->planes)) {
    return false_v;
  }
  const uint64_t plane_offset =
      VKR_LIGHTMAP_SET_HEADER_BYTES +
      (uint64_t)set->layer_count * VKR_LIGHT_LAYER_RECORD_BYTES;
  const uint64_t instance_offset =
      plane_offset + (uint64_t)set->plane_count * VKR_LIGHTMAP_SET_PLANE_BYTES;
  uint64_t tables_end = 0u;
  uint64_t payload_bytes = 0u;
  uint64_t file_size = 0u;
  if (!vkr_checked_mul_u64(set->instance_count, VKR_LIGHTMAP_SET_INSTANCE_BYTES,
                           &tables_end) ||
      !vkr_checked_add_u64(tables_end, instance_offset, &tables_end) ||
      !vkr_checked_mul_u64(vkr_lightmap_set_page_stride(set), set->page_count,
                           &payload_bytes)) {
    return false_v;
  }
  const uint64_t payload_offset =
      vkr_align_up_u64(tables_end, VKR_LIGHTMAP_SET_PAYLOAD_ALIGNMENT);
  if (payload_offset < tables_end ||
      !vkr_checked_add_u64(payload_offset, payload_bytes, &file_size)) {
    return false_v;
  }
  out->plane_offset = plane_offset;
  out->instance_offset = instance_offset;
  out->payload_offset = payload_offset;
  out->payload_bytes = payload_bytes;
  out->file_size = file_size;
  return true_v;
}

/* Known encodings of a kind they can hold, desktop encodings only on lamp
   groups, in strict (layer, kind, format) order. */
static bool8_t vkr_lightmap_set_planes_valid(const VkrLightmapSet *set) {
  for (uint32_t i = 0u; i < set->plane_count; ++i) {
    const VkrLightmapPlane *plane = &set->planes[i];
    const bool8_t irradiance_format =
        plane->format == VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR ||
        plane->format == VKR_LIGHTMAP_FORMAT_RGB9E5 ||
        plane->format == VKR_LIGHTMAP_FORMAT_BC6H;
    const bool8_t direction_format =
        plane->format == VKR_LIGHTMAP_FORMAT_ASTC_4X4_LDR ||
        plane->format == VKR_LIGHTMAP_FORMAT_RGBA8 ||
        plane->format == VKR_LIGHTMAP_FORMAT_BC7;
    if (plane->layer >= set->layer_count ||
        (plane->kind == VKR_LIGHTMAP_PLANE_IRRADIANCE && !irradiance_format) ||
        (plane->kind == VKR_LIGHTMAP_PLANE_DIRECTION && !direction_format) ||
        (plane->kind != VKR_LIGHTMAP_PLANE_IRRADIANCE &&
         plane->kind != VKR_LIGHTMAP_PLANE_DIRECTION) ||
        (vkr_lightmap_format_desktop(plane->format) &&
         set->layers[plane->layer].kind != VKR_LIGHT_LAYER_LAMP_GROUP)) {
      return false_v;
    }
    if (i > 0u) {
      const VkrLightmapPlane *previous = &set->planes[i - 1u];
      const bool8_t ordered = previous->layer != plane->layer
                                  ? previous->layer < plane->layer
                                  : (previous->kind != plane->kind
                                         ? previous->kind < plane->kind
                                         : previous->format < plane->format);
      if (!ordered) {
        return false_v;
      }
      /* One tiled and one desktop encoding of each kind at most. */
      if (previous->layer == plane->layer && previous->kind == plane->kind &&
          vkr_lightmap_format_desktop(previous->format) ==
              vkr_lightmap_format_desktop(plane->format)) {
        return false_v;
      }
    }
  }
  return true_v;
}

static bool8_t vkr_lightmap_set_instance_valid(const VkrLightmapSet *set,
                                               const VkrLightmapInstance *i) {
  return i->page < set->page_count && i->width >= 4u && i->height >= 4u &&
         i->x % 4u == 0u && i->y % 4u == 0u && i->width % 4u == 0u &&
         i->height % 4u == 0u && i->x <= set->page_size &&
         i->y <= set->page_size && i->width <= set->page_size - i->x &&
         i->height <= set->page_size - i->y;
}

/* Strict (entity, instance) order: sorted and unique. */
static bool8_t vkr_lightmap_set_instance_before(const VkrLightmapInstance *a,
                                                const VkrLightmapInstance *b) {
  if (a->entity_index != b->entity_index) {
    return a->entity_index < b->entity_index;
  }
  return a->instance_index < b->instance_index;
}

static bool8_t vkr_lightmap_set_tables_valid(const VkrLightmapSet *set) {
  if (!isfinite(set->texels_per_unit) || !(set->texels_per_unit > 0.0f) ||
      (set->layer_count != 0u && !set->layers) ||
      (set->instance_count != 0u && !set->instances)) {
    return false_v;
  }
  if (!vkr_light_layers_valid(set->layers, set->layer_count) ||
      !vkr_lightmap_set_planes_valid(set)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < set->instance_count; ++i) {
    if (!vkr_lightmap_set_instance_valid(set, &set->instances[i])) {
      return false_v;
    }
    if (i > 0u && !vkr_lightmap_set_instance_before(&set->instances[i - 1u],
                                                    &set->instances[i])) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_lightmap_set_layout(const VkrLightmapSet *set,
                                uint64_t *out_payload_offset,
                                uint64_t *out_file_size) {
  VkrLightmapSetLayout layout;
  if (!set || !out_payload_offset || !out_file_size ||
      !vkr_lightmap_set_tables_valid(set) ||
      !vkr_lightmap_set_compute_layout(set, &layout)) {
    return false_v;
  }
  *out_payload_offset = layout.payload_offset;
  *out_file_size = layout.file_size;
  return true_v;
}

static void vkr_lightmap_set_write_instance(uint8_t *dst,
                                            const VkrLightmapInstance *i) {
  MemCopy(dst, i->document_id, sizeof(i->document_id));
  vkr_store_le_u32(dst + 16u, i->entity_index);
  vkr_store_le_u32(dst + 20u, i->instance_index);
  vkr_store_le_u32(dst + 24u, i->page);
  vkr_store_le_u32(dst + 28u, i->x);
  vkr_store_le_u32(dst + 32u, i->y);
  vkr_store_le_u32(dst + 36u, i->width);
  vkr_store_le_u32(dst + 40u, i->height);
}

bool8_t vkr_lightmap_set_write_prefix(const VkrLightmapSet *set,
                                      uint32_t payload_crc, uint8_t *out_prefix,
                                      uint64_t prefix_size) {
  VkrLightmapSetLayout layout;
  if (!set || !out_prefix || !vkr_f32_is_binary32() ||
      !vkr_lightmap_set_tables_valid(set) ||
      !vkr_lightmap_set_compute_layout(set, &layout) ||
      prefix_size != layout.payload_offset) {
    return false_v;
  }
  MemZero(out_prefix, prefix_size);

  uint8_t *tables = out_prefix + VKR_LIGHTMAP_SET_HEADER_BYTES;
  for (uint32_t i = 0u; i < set->layer_count; ++i) {
    vkr_light_layer_write(tables + (uint64_t)i * VKR_LIGHT_LAYER_RECORD_BYTES,
                          &set->layers[i]);
  }
  uint8_t *planes = out_prefix + layout.plane_offset;
  for (uint32_t i = 0u; i < set->plane_count; ++i) {
    uint8_t *record = planes + (uint64_t)i * VKR_LIGHTMAP_SET_PLANE_BYTES;
    vkr_store_le_u32(record, set->planes[i].layer);
    vkr_store_le_u32(record + 4u, (uint32_t)set->planes[i].kind);
    vkr_store_le_u32(record + 8u, (uint32_t)set->planes[i].format);
  }
  uint8_t *instances = out_prefix + layout.instance_offset;
  for (uint32_t i = 0u; i < set->instance_count; ++i) {
    vkr_lightmap_set_write_instance(
        instances + (uint64_t)i * VKR_LIGHTMAP_SET_INSTANCE_BYTES,
        &set->instances[i]);
  }
  const uint64_t tables_bytes =
      layout.instance_offset - VKR_LIGHTMAP_SET_HEADER_BYTES +
      (uint64_t)set->instance_count * VKR_LIGHTMAP_SET_INSTANCE_BYTES;

  uint8_t *header = out_prefix;
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_MAGIC, VKR_LIGHTMAP_SET_MAGIC);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_VERSION,
                   VKR_LIGHTMAP_SET_VERSION);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_ENDIAN,
                   VKR_LIGHTMAP_SET_ENDIAN_TAG);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_SIZE,
                   VKR_LIGHTMAP_SET_HEADER_BYTES);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_FILE_SIZE, layout.file_size);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_PAGE_SIZE, set->page_size);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_PAGE_COUNT, set->page_count);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_LAYER_COUNT, set->layer_count);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_INSTANCE_COUNT,
                   set->instance_count);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_PLANE_COUNT, set->plane_count);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_PLANE_OFFSET,
                   layout.plane_offset);
  vkr_store_le_f32(header + VKR_LIGHTMAP_SET_H_TEXELS_PER_UNIT,
                   set->texels_per_unit);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_LAYER_OFFSET,
                   VKR_LIGHTMAP_SET_HEADER_BYTES);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_INSTANCE_OFFSET,
                   layout.instance_offset);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_PAYLOAD_OFFSET,
                   layout.payload_offset);
  vkr_store_le_u64(header + VKR_LIGHTMAP_SET_H_PAYLOAD_BYTES,
                   layout.payload_bytes);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_TABLES_CRC,
                   vkr_crc32(tables, tables_bytes));
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_PAYLOAD_CRC, payload_crc);
  vkr_store_le_u32(header + VKR_LIGHTMAP_SET_H_HEADER_CRC,
                   vkr_lightmap_set_header_crc(header));
  return true_v;
}

bool8_t vkr_lightmap_set_decode(const uint8_t *bytes, uint64_t size,
                                Arena *arena, VkrLightmapSet *out_set) {
  if (!bytes || !arena || !out_set || !vkr_f32_is_binary32() ||
      size < VKR_LIGHTMAP_SET_HEADER_BYTES) {
    return false_v;
  }
  const uint8_t *header = bytes;
  if (vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_MAGIC) !=
          VKR_LIGHTMAP_SET_MAGIC ||
      vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_VERSION) !=
          VKR_LIGHTMAP_SET_VERSION ||
      vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_ENDIAN) !=
          VKR_LIGHTMAP_SET_ENDIAN_TAG ||
      vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_SIZE) !=
          VKR_LIGHTMAP_SET_HEADER_BYTES ||
      vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_HEADER_CRC) !=
          vkr_lightmap_set_header_crc(header) ||
      vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_RESERVED_A) != 0u) {
    return false_v;
  }
  for (uint32_t offset = VKR_LIGHTMAP_SET_H_RESERVED_B;
       offset < VKR_LIGHTMAP_SET_HEADER_BYTES; ++offset) {
    if (header[offset] != 0u) {
      return false_v;
    }
  }

  VkrLightmapSet set = {
      .page_size = vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_PAGE_SIZE),
      .page_count = vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_PAGE_COUNT),
      .layer_count = vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_LAYER_COUNT),
      .plane_count = vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_PLANE_COUNT),
      .instance_count =
          vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_INSTANCE_COUNT),
      .texels_per_unit =
          vkr_load_le_f32(header + VKR_LIGHTMAP_SET_H_TEXELS_PER_UNIT),
  };
  const uint64_t plane_offset =
      VKR_LIGHTMAP_SET_HEADER_BYTES +
      (uint64_t)set.layer_count * VKR_LIGHT_LAYER_RECORD_BYTES;
  if (set.layer_count == 0u || set.layer_count > VKR_LIGHTMAP_SET_MAX_LAYERS ||
      set.plane_count == 0u || set.plane_count > VKR_LIGHTMAP_SET_MAX_PLANES ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_PLANE_OFFSET) !=
          plane_offset ||
      plane_offset + (uint64_t)set.plane_count * VKR_LIGHTMAP_SET_PLANE_BYTES >
          size) {
    return false_v;
  }
  VkrLightmapPlane *planes = (VkrLightmapPlane *)arena_alloc(
      arena, (uint64_t)set.plane_count * sizeof(VkrLightmapPlane),
      ARENA_MEMORY_TAG_ARRAY);
  if (!planes) {
    return false_v;
  }
  for (uint32_t i = 0u; i < set.plane_count; ++i) {
    const uint8_t *record =
        bytes + plane_offset + (uint64_t)i * VKR_LIGHTMAP_SET_PLANE_BYTES;
    planes[i] = (VkrLightmapPlane){
        .layer = vkr_load_le_u32(record),
        .kind = (VkrLightmapPlaneKind)vkr_load_le_u32(record + 4u),
        .format = (VkrLightmapPlaneFormat)vkr_load_le_u32(record + 8u),
    };
    if (vkr_load_le_u32(record + 12u) != 0u ||
        vkr_lightmap_set_plane_bytes(planes[i].format, set.page_size) == 0u) {
      return false_v;
    }
  }
  set.planes = planes;
  VkrLightmapSetLayout layout;
  if (!vkr_lightmap_set_compute_layout(&set, &layout) ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_FILE_SIZE) != size ||
      layout.file_size != size ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_LAYER_OFFSET) !=
          VKR_LIGHTMAP_SET_HEADER_BYTES ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_INSTANCE_OFFSET) !=
          layout.instance_offset ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_PAYLOAD_OFFSET) !=
          layout.payload_offset ||
      vkr_load_le_u64(header + VKR_LIGHTMAP_SET_H_PAYLOAD_BYTES) !=
          layout.payload_bytes) {
    return false_v;
  }

  const uint8_t *tables = bytes + VKR_LIGHTMAP_SET_HEADER_BYTES;
  const uint64_t tables_bytes =
      layout.instance_offset - VKR_LIGHTMAP_SET_HEADER_BYTES +
      (uint64_t)set.instance_count * VKR_LIGHTMAP_SET_INSTANCE_BYTES;
  if (vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_TABLES_CRC) !=
      vkr_crc32(tables, tables_bytes)) {
    return false_v;
  }
  for (uint64_t offset = VKR_LIGHTMAP_SET_HEADER_BYTES + tables_bytes;
       offset < layout.payload_offset; ++offset) {
    if (bytes[offset] != 0u) {
      return false_v;
    }
  }
  const uint8_t *payload = bytes + layout.payload_offset;
  if (vkr_load_le_u32(header + VKR_LIGHTMAP_SET_H_PAYLOAD_CRC) !=
      vkr_crc32(payload, layout.payload_bytes)) {
    return false_v;
  }

  VkrLightLayer *layers = (VkrLightLayer *)arena_alloc(
      arena, (uint64_t)set.layer_count * sizeof(VkrLightLayer),
      ARENA_MEMORY_TAG_ARRAY);
  VkrLightmapInstance *instances = NULL;
  if (set.instance_count != 0u) {
    instances = (VkrLightmapInstance *)arena_alloc(
        arena, (uint64_t)set.instance_count * sizeof(VkrLightmapInstance),
        ARENA_MEMORY_TAG_ARRAY);
  }
  if (!layers || (set.instance_count != 0u && !instances)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < set.layer_count; ++i) {
    if (!vkr_light_layer_read(
            tables + (uint64_t)i * VKR_LIGHT_LAYER_RECORD_BYTES, &layers[i])) {
      return false_v;
    }
  }
  const uint8_t *instance_table = bytes + layout.instance_offset;
  for (uint32_t i = 0u; i < set.instance_count; ++i) {
    const uint8_t *src =
        instance_table + (uint64_t)i * VKR_LIGHTMAP_SET_INSTANCE_BYTES;
    instances[i] = (VkrLightmapInstance){
        .entity_index = vkr_load_le_u32(src + 16u),
        .instance_index = vkr_load_le_u32(src + 20u),
        .page = vkr_load_le_u32(src + 24u),
        .x = vkr_load_le_u32(src + 28u),
        .y = vkr_load_le_u32(src + 32u),
        .width = vkr_load_le_u32(src + 36u),
        .height = vkr_load_le_u32(src + 40u),
    };
    MemCopy(instances[i].document_id, src, sizeof(instances[i].document_id));
  }
  set.layers = layers;
  set.instances = instances;
  set.payload = payload;
  if (!vkr_lightmap_set_tables_valid(&set)) {
    return false_v;
  }
  *out_set = set;
  return true_v;
}
