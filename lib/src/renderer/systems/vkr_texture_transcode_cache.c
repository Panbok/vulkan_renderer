#include "renderer/systems/vkr_texture_transcode_cache.h"

#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/vkr_resources.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define VKR_TEXTURE_TRANSCODE_CACHE_MAGIC 0x43544B56u /* 'VKTC' */
#define VKR_TEXTURE_TRANSCODE_CACHE_VERSION 2u
#define VKR_TEXTURE_TRANSCODE_CACHE_ENDIAN 0x01020304u
#define VKR_TEXTURE_TRANSCODE_CACHE_DEFAULT_ROOT "build/_asset_cache"
#define VKR_TEXTURE_TRANSCODE_CACHE_MAX_REGIONS 32768u
#define VKR_TEXTURE_TRANSCODE_CACHE_FLAG_COMPRESSED 0x1u
#define VKR_TEXTURE_TRANSCODE_CACHE_FLAG_TRANSPARENT 0x2u
#define VKR_TEXTURE_TRANSCODE_CACHE_FLAG_ALPHA_MASK 0x4u
#define VKR_TEXTURE_TRANSCODE_CACHE_KNOWN_FLAGS 0x7u

typedef struct VkrTextureTranscodeCacheHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t endian;
  uint32_t header_size;
  uint64_t source_hash;
  uint64_t data_size;
  uint32_t target_format;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t mip_levels;
  uint32_t array_layers;
  uint32_t region_count;
  uint32_t flags;
  uint32_t region_stride;
  uint32_t regions_crc;
  uint32_t data_crc;
  uint32_t header_crc;
  uint32_t reserved[4];
} VkrTextureTranscodeCacheHeader;

typedef struct VkrTextureTranscodeCacheRegion {
  uint32_t mip_level;
  uint32_t array_layer;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t reserved;
  uint64_t byte_offset;
  uint64_t byte_size;
} VkrTextureTranscodeCacheRegion;

_Static_assert(sizeof(VkrTextureTranscodeCacheHeader) == 96u,
               "Texture transcode cache header ABI drift");
_Static_assert(sizeof(VkrTextureTranscodeCacheRegion) == 40u,
               "Texture transcode cache region ABI drift");

static uint32_t vkr_texture_cache_u32_le(uint32_t value) {
  const union {
    uint32_t value;
    uint8_t bytes[4];
  } endian = {0x01020304u};
  if (endian.bytes[0] == 0x04u) {
    return value;
  }
  return ((value & 0xff000000u) >> 24u) | ((value & 0x00ff0000u) >> 8u) |
         ((value & 0x0000ff00u) << 8u) | ((value & 0x000000ffu) << 24u);
}

static uint64_t vkr_texture_cache_u64_le(uint64_t value) {
  const union {
    uint32_t value;
    uint8_t bytes[4];
  } endian = {0x01020304u};
  if (endian.bytes[0] == 0x04u) {
    return value;
  }
  return ((value & UINT64_C(0xff00000000000000)) >> 56u) |
         ((value & UINT64_C(0x00ff000000000000)) >> 40u) |
         ((value & UINT64_C(0x0000ff0000000000)) >> 24u) |
         ((value & UINT64_C(0x000000ff00000000)) >> 8u) |
         ((value & UINT64_C(0x00000000ff000000)) << 8u) |
         ((value & UINT64_C(0x0000000000ff0000)) << 24u) |
         ((value & UINT64_C(0x000000000000ff00)) << 40u) |
         ((value & UINT64_C(0x00000000000000ff)) << 56u);
}

static uint64_t vkr_texture_cache_fast_hash64(const void *data, uint64_t size) {
  const uint8_t *bytes = data;
  uint64_t hash =
      UINT64_C(0x9e3779b185ebca87) ^ (size * UINT64_C(0xc2b2ae3d27d4eb4f));
  while (size >= sizeof(uint64_t)) {
    uint64_t word = 0u;
    MemCopy(&word, bytes, sizeof(word));
    hash ^= word * UINT64_C(0x9e3779b185ebca87);
    hash = ((hash << 31u) | (hash >> 33u)) * UINT64_C(0xc2b2ae3d27d4eb4f) +
           UINT64_C(0x165667b19e3779f9);
    bytes += sizeof(uint64_t);
    size -= sizeof(uint64_t);
  }
  while (size-- > 0u) {
    hash ^= (uint64_t)*bytes++ * UINT64_C(0x27d4eb2f165667c5);
    hash = ((hash << 23u) | (hash >> 41u)) * UINT64_C(0x9e3779b185ebca87);
  }
  hash ^= hash >> 33u;
  hash *= UINT64_C(0xff51afd7ed558ccd);
  hash ^= hash >> 33u;
  hash *= UINT64_C(0xc4ceb9fe1a85ec53);
  return hash ^ (hash >> 33u);
}

static uint32_t vkr_texture_cache_fast_hash32(const void *data, uint64_t size) {
  const uint64_t hash = vkr_texture_cache_fast_hash64(data, size);
  return (uint32_t)(hash ^ (hash >> 32u));
}

static uint32_t vkr_texture_cache_crc32(const void *data, uint64_t size) {
  const uint8_t *bytes = data;
  uint32_t crc = 0xffffffffu;
  for (uint64_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
      crc = (crc >> 1u) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
}

static bool8_t vkr_texture_cache_add_u64(uint64_t a, uint64_t b,
                                         uint64_t *out) {
  if (!out || a > UINT64_MAX - b) {
    return false_v;
  }
  *out = a + b;
  return true_v;
}

static bool8_t vkr_texture_cache_mul_u64(uint64_t a, uint64_t b,
                                         uint64_t *out) {
  if (!out || (a != 0u && b > UINT64_MAX / a)) {
    return false_v;
  }
  *out = a * b;
  return true_v;
}

static bool8_t vkr_texture_cache_shape_is_valid(VkrTextureFormat format,
                                                uint32_t width, uint32_t height,
                                                uint32_t mip_levels,
                                                uint32_t array_layers,
                                                uint32_t region_count,
                                                bool8_t is_compressed) {
  VkrTextureFormatInfo format_info = {0};
  if (format < 0 || format >= VKR_TEXTURE_FORMAT_COUNT || width == 0u ||
      height == 0u || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION || mip_levels == 0u ||
      array_layers == 0u || array_layers > VKR_TEXTURE_MAX_ARRAY_LAYERS ||
      !vkr_texture_format_get_info(format, &format_info) ||
      is_compressed != format_info.is_block_compressed) {
    return false_v;
  }
  uint32_t max_mip_levels = 1u;
  for (uint32_t extent = Max(width, height); extent > 1u; extent >>= 1u) {
    max_mip_levels++;
  }
  uint64_t expected_region_count = 0u;
  return mip_levels <= max_mip_levels &&
         vkr_texture_cache_mul_u64(mip_levels, array_layers,
                                   &expected_region_count) &&
         expected_region_count == region_count &&
         region_count <= VKR_TEXTURE_TRANSCODE_CACHE_MAX_REGIONS;
}

static bool8_t vkr_texture_cache_payload_size_is_valid(
    VkrTextureFormat format, uint32_t width, uint32_t height,
    uint32_t mip_levels, uint32_t array_layers, uint32_t region_count,
    uint64_t data_size) {
  uint64_t minimum_size = 0u;
  for (uint32_t mip = 0u; mip < mip_levels; ++mip) {
    const uint64_t region_size = vkr_texture_format_region_size(
        format, Max(1u, width >> mip), Max(1u, height >> mip));
    uint64_t layer_size = 0u;
    if (region_size == 0u ||
        !vkr_texture_cache_mul_u64(region_size, array_layers, &layer_size) ||
        !vkr_texture_cache_add_u64(minimum_size, layer_size, &minimum_size)) {
      return false_v;
    }
  }
  uint64_t alignment_allowance = 0u;
  uint64_t maximum_size = 0u;
  return vkr_texture_cache_mul_u64(region_count, 7u, &alignment_allowance) &&
         vkr_texture_cache_add_u64(minimum_size, alignment_allowance,
                                   &maximum_size) &&
         data_size >= minimum_size && data_size <= maximum_size;
}

static bool8_t vkr_texture_cache_record_is_valid(
    const VkrTextureTranscodeCacheRecord *record) {
  if (!record || !record->data || record->data_size == 0u ||
      record->data_size > SIZE_MAX || !record->regions ||
      record->channels == 0u || record->channels > 4u ||
      (record->alpha_mask && !record->has_transparency) ||
      !vkr_texture_cache_shape_is_valid(
          record->format, record->width, record->height, record->mip_levels,
          record->array_layers, record->region_count, record->is_compressed) ||
      !vkr_texture_cache_payload_size_is_valid(
          record->format, record->width, record->height, record->mip_levels,
          record->array_layers, record->region_count, record->data_size)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < record->region_count; ++i) {
    const VkrTextureUploadRegion *region = &record->regions[i];
    const uint32_t expected_mip = i % record->mip_levels;
    const uint32_t expected_layer = i / record->mip_levels;
    const uint32_t expected_width = Max(1u, record->width >> expected_mip);
    const uint32_t expected_height = Max(1u, record->height >> expected_mip);
    const uint64_t expected_size = vkr_texture_format_region_size(
        record->format, expected_width, expected_height);
    if (region->mip_level != expected_mip ||
        region->array_layer != expected_layer ||
        region->width != expected_width || region->height != expected_height ||
        region->depth != 1u || region->byte_size != expected_size ||
        region->byte_offset > record->data_size ||
        region->byte_size > record->data_size - region->byte_offset) {
      return false_v;
    }
  }
  return true_v;
}

static String8 vkr_texture_transcode_cache_root(VkrAllocator *allocator) {
  const char *root = getenv("VKR_ASSET_CACHE_ROOT");
  if (!root || root[0] == '\0') {
    root = VKR_TEXTURE_TRANSCODE_CACHE_DEFAULT_ROOT;
  }
  return string8_create_formatted(allocator, "%s/textures", root);
}

bool8_t vkr_texture_transcode_cache_path(VkrAllocator *allocator,
                                         String8 source_path,
                                         VkrTextureFormat target_format,
                                         String8 *out_path) {
  if (!allocator || !source_path.str || source_path.length == 0u || !out_path ||
      target_format < 0 || target_format >= VKR_TEXTURE_FORMAT_COUNT) {
    return false_v;
  }
  const uint64_t path_hash =
      vkr_texture_cache_fast_hash64(source_path.str, source_path.length);
  const String8 root = vkr_texture_transcode_cache_root(allocator);
  if (!root.str) {
    return false_v;
  }
  *out_path = string8_create_formatted(
      allocator, "%.*s/%016llx_%08x.vktc", (int32_t)root.length, root.str,
      (unsigned long long)path_hash, (uint32_t)target_format);
  return out_path->str && out_path->length > 0u;
}

static FilePathType vkr_texture_cache_path_type(String8 path) {
  return path.length > 0u && (path.str[0] == '/' ||
                              (path.length > 2u && path.str[1] == ':'))
             ? FILE_PATH_TYPE_ABSOLUTE
             : FILE_PATH_TYPE_RELATIVE;
}

static void vkr_texture_cache_remove(String8 path) {
  if (path.str && path.length > 0u) {
    remove((const char *)path.str);
  }
}

static bool8_t vkr_texture_cache_header_valid(
    VkrTextureTranscodeCacheHeader *header, uint64_t source_hash,
    VkrTextureFormat target_format, uint32_t expected_width,
    uint32_t expected_height, uint32_t expected_mip_levels,
    uint32_t expected_array_layers) {
  const uint32_t stored_crc = vkr_texture_cache_u32_le(header->header_crc);
  header->header_crc = 0u;
  const uint32_t observed_crc =
      vkr_texture_cache_crc32(header, sizeof(*header));
  header->header_crc = vkr_texture_cache_u32_le(stored_crc);
  const uint32_t flags = vkr_texture_cache_u32_le(header->flags);
  return stored_crc == observed_crc &&
         vkr_texture_cache_u32_le(header->magic) ==
             VKR_TEXTURE_TRANSCODE_CACHE_MAGIC &&
         vkr_texture_cache_u32_le(header->version) ==
             VKR_TEXTURE_TRANSCODE_CACHE_VERSION &&
         vkr_texture_cache_u32_le(header->endian) ==
             VKR_TEXTURE_TRANSCODE_CACHE_ENDIAN &&
         vkr_texture_cache_u32_le(header->header_size) == sizeof(*header) &&
         vkr_texture_cache_u64_le(header->source_hash) == source_hash &&
         vkr_texture_cache_u32_le(header->target_format) == target_format &&
         vkr_texture_cache_u32_le(header->width) == expected_width &&
         vkr_texture_cache_u32_le(header->height) == expected_height &&
         vkr_texture_cache_u32_le(header->mip_levels) == expected_mip_levels &&
         vkr_texture_cache_u32_le(header->array_layers) ==
             expected_array_layers &&
         vkr_texture_cache_u32_le(header->channels) >= 1u &&
         vkr_texture_cache_u32_le(header->channels) <= 4u &&
         vkr_texture_cache_shape_is_valid(
             target_format, expected_width, expected_height,
             expected_mip_levels, expected_array_layers,
             vkr_texture_cache_u32_le(header->region_count),
             (flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_COMPRESSED) != 0u) &&
         vkr_texture_cache_u32_le(header->region_stride) ==
             sizeof(VkrTextureTranscodeCacheRegion) &&
         (flags & ~VKR_TEXTURE_TRANSCODE_CACHE_KNOWN_FLAGS) == 0u &&
         (!(flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_ALPHA_MASK) ||
          (flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_TRANSPARENT)) &&
         vkr_texture_cache_u32_le(header->reserved[0]) == 0u &&
         vkr_texture_cache_u32_le(header->reserved[1]) == 0u &&
         vkr_texture_cache_u32_le(header->reserved[2]) == 0u &&
         vkr_texture_cache_u32_le(header->reserved[3]) == 0u &&
         vkr_texture_cache_u64_le(header->data_size) > 0u &&
         vkr_texture_cache_u64_le(header->data_size) <= SIZE_MAX &&
         vkr_texture_cache_payload_size_is_valid(
             target_format, expected_width, expected_height,
             expected_mip_levels, expected_array_layers,
             vkr_texture_cache_u32_le(header->region_count),
             vkr_texture_cache_u64_le(header->data_size));
}

bool8_t vkr_texture_transcode_cache_load(
    VkrAllocator *scratch, String8 source_path, const uint8_t *source_data,
    uint64_t source_size, VkrTextureFormat target_format,
    uint32_t expected_width, uint32_t expected_height,
    uint32_t expected_mip_levels, uint32_t expected_array_layers,
    VkrTextureTranscodeCacheRecord *out_record) {
  if (!scratch || !source_data || source_size == 0u || !out_record) {
    return false_v;
  }
  *out_record = (VkrTextureTranscodeCacheRecord){0};
  String8 cache_path = {0};
  if (!vkr_texture_transcode_cache_path(scratch, source_path, target_format,
                                        &cache_path)) {
    return false_v;
  }
  FilePath fp = file_path_create((const char *)cache_path.str, scratch,
                                 vkr_texture_cache_path_type(cache_path));
  FileStats cache_stats = {0};
  if (file_stats(&fp, &cache_stats) != FILE_ERROR_NONE ||
      cache_stats.size < sizeof(VkrTextureTranscodeCacheHeader)) {
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&fp, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  VkrTextureTranscodeCacheHeader header = {0};
  uint64_t bytes_read = 0u;
  const FileError header_read_error =
      file_read_into(&file, &header, sizeof(header), &bytes_read);
  file_close(&file);
  if (header_read_error != FILE_ERROR_NONE || bytes_read != sizeof(header)) {
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  const uint64_t source_hash =
      vkr_texture_cache_fast_hash64(source_data, source_size);
  if (!vkr_texture_cache_header_valid(
          &header, source_hash, target_format, expected_width, expected_height,
          expected_mip_levels, expected_array_layers)) {
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  const uint32_t region_count = vkr_texture_cache_u32_le(header.region_count);
  const uint64_t data_size = vkr_texture_cache_u64_le(header.data_size);
  uint64_t region_bytes = 0u;
  uint64_t data_offset = 0u;
  uint64_t expected_file_size = 0u;
  if (!vkr_texture_cache_mul_u64(region_count,
                                 sizeof(VkrTextureTranscodeCacheRegion),
                                 &region_bytes) ||
      !vkr_texture_cache_add_u64(sizeof(header), region_bytes, &data_offset) ||
      !vkr_texture_cache_add_u64(data_offset, data_size, &expected_file_size) ||
      expected_file_size != cache_stats.size) {
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  if (file_open(&fp, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t file_size = 0u;
  const FileError read_error =
      file_read_all(&file, scratch, &bytes, &file_size);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !bytes ||
      file_size != expected_file_size ||
      vkr_texture_cache_fast_hash32(bytes + sizeof(header), region_bytes) !=
          vkr_texture_cache_u32_le(header.regions_crc) ||
      vkr_texture_cache_fast_hash32(bytes + data_offset, data_size) !=
          vkr_texture_cache_u32_le(header.data_crc)) {
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  uint8_t *data = malloc((size_t)data_size);
  VkrTextureUploadRegion *regions =
      malloc((size_t)region_count * sizeof(*regions));
  if (!data || !regions) {
    free(data);
    free(regions);
    return false_v;
  }
  const VkrTextureTranscodeCacheRegion *stored_regions =
      (const VkrTextureTranscodeCacheRegion *)(bytes + sizeof(header));
  bool8_t valid = true_v;
  for (uint32_t i = 0; i < region_count; ++i) {
    const uint32_t mip = vkr_texture_cache_u32_le(stored_regions[i].mip_level);
    const uint32_t layer =
        vkr_texture_cache_u32_le(stored_regions[i].array_layer);
    const uint32_t width = vkr_texture_cache_u32_le(stored_regions[i].width);
    const uint32_t height = vkr_texture_cache_u32_le(stored_regions[i].height);
    const uint32_t depth = vkr_texture_cache_u32_le(stored_regions[i].depth);
    const uint64_t offset =
        vkr_texture_cache_u64_le(stored_regions[i].byte_offset);
    const uint64_t size = vkr_texture_cache_u64_le(stored_regions[i].byte_size);
    if (vkr_texture_cache_u32_le(stored_regions[i].reserved) != 0u ||
        mip >= expected_mip_levels || layer >= expected_array_layers ||
        width == 0u || height == 0u || depth == 0u || size == 0u ||
        offset > data_size || size > data_size - offset) {
      valid = false_v;
      break;
    }
    regions[i] = (VkrTextureUploadRegion){
        .mip_level = mip,
        .array_layer = layer,
        .width = width,
        .height = height,
        .depth = depth,
        .byte_offset = offset,
        .byte_size = size,
    };
  }
  const uint32_t flags = vkr_texture_cache_u32_le(header.flags);
  VkrTextureTranscodeCacheRecord loaded = {
      .width = expected_width,
      .height = expected_height,
      .channels = vkr_texture_cache_u32_le(header.channels),
      .format = target_format,
      .mip_levels = expected_mip_levels,
      .array_layers = expected_array_layers,
      .is_compressed =
          (flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_COMPRESSED) != 0u,
      .has_transparency =
          (flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_TRANSPARENT) != 0u,
      .alpha_mask = (flags & VKR_TEXTURE_TRANSCODE_CACHE_FLAG_ALPHA_MASK) != 0u,
      .data = data,
      .data_size = data_size,
      .regions = regions,
      .region_count = region_count,
  };
  if (!valid || !vkr_texture_cache_record_is_valid(&loaded)) {
    free(data);
    free(regions);
    vkr_texture_cache_remove(cache_path);
    return false_v;
  }
  MemCopy(data, bytes + data_offset, (size_t)data_size);
  *out_record = loaded;
  return true_v;
}

bool8_t vkr_texture_transcode_cache_store(
    VkrAllocator *scratch, String8 source_path, const uint8_t *source_data,
    uint64_t source_size, const VkrTextureTranscodeCacheRecord *record) {
  if (!scratch || !source_data || source_size == 0u ||
      !vkr_texture_cache_record_is_valid(record)) {
    return false_v;
  }
  const String8 root = vkr_texture_transcode_cache_root(scratch);
  if (!root.str || !file_ensure_directory(scratch, &root)) {
    return false_v;
  }
  String8 cache_path = {0};
  if (!vkr_texture_transcode_cache_path(scratch, source_path, record->format,
                                        &cache_path)) {
    return false_v;
  }
  String8 temp_path = string8_create_formatted(
      scratch, "%.*s.tmp.%u.%llu", (int32_t)cache_path.length, cache_path.str,
      vkr_platform_get_process_id(),
      (unsigned long long)vkr_thread_current_id());
  if (!temp_path.str) {
    return false_v;
  }
  VkrTextureTranscodeCacheRegion *stored_regions = malloc(
      (size_t)record->region_count * sizeof(VkrTextureTranscodeCacheRegion));
  if (!stored_regions) {
    return false_v;
  }
  for (uint32_t i = 0; i < record->region_count; ++i) {
    const VkrTextureUploadRegion *region = &record->regions[i];
    stored_regions[i] = (VkrTextureTranscodeCacheRegion){
        .mip_level = vkr_texture_cache_u32_le(region->mip_level),
        .array_layer = vkr_texture_cache_u32_le(region->array_layer),
        .width = vkr_texture_cache_u32_le(region->width),
        .height = vkr_texture_cache_u32_le(region->height),
        .depth = vkr_texture_cache_u32_le(region->depth),
        .reserved = 0u,
        .byte_offset = vkr_texture_cache_u64_le(region->byte_offset),
        .byte_size = vkr_texture_cache_u64_le(region->byte_size),
    };
  }
  uint32_t flags = 0u;
  flags |=
      record->is_compressed ? VKR_TEXTURE_TRANSCODE_CACHE_FLAG_COMPRESSED : 0u;
  flags |= record->has_transparency
               ? VKR_TEXTURE_TRANSCODE_CACHE_FLAG_TRANSPARENT
               : 0u;
  flags |=
      record->alpha_mask ? VKR_TEXTURE_TRANSCODE_CACHE_FLAG_ALPHA_MASK : 0u;
  VkrTextureTranscodeCacheHeader header = {
      .magic = vkr_texture_cache_u32_le(VKR_TEXTURE_TRANSCODE_CACHE_MAGIC),
      .version = vkr_texture_cache_u32_le(VKR_TEXTURE_TRANSCODE_CACHE_VERSION),
      .endian = vkr_texture_cache_u32_le(VKR_TEXTURE_TRANSCODE_CACHE_ENDIAN),
      .header_size = vkr_texture_cache_u32_le(sizeof(header)),
      .source_hash = vkr_texture_cache_u64_le(
          vkr_texture_cache_fast_hash64(source_data, source_size)),
      .data_size = vkr_texture_cache_u64_le(record->data_size),
      .target_format = vkr_texture_cache_u32_le(record->format),
      .width = vkr_texture_cache_u32_le(record->width),
      .height = vkr_texture_cache_u32_le(record->height),
      .channels = vkr_texture_cache_u32_le(record->channels),
      .mip_levels = vkr_texture_cache_u32_le(record->mip_levels),
      .array_layers = vkr_texture_cache_u32_le(record->array_layers),
      .region_count = vkr_texture_cache_u32_le(record->region_count),
      .flags = vkr_texture_cache_u32_le(flags),
      .region_stride =
          vkr_texture_cache_u32_le(sizeof(VkrTextureTranscodeCacheRegion)),
      .regions_crc = vkr_texture_cache_u32_le(vkr_texture_cache_fast_hash32(
          stored_regions,
          (uint64_t)record->region_count * sizeof(*stored_regions))),
      .data_crc = vkr_texture_cache_u32_le(
          vkr_texture_cache_fast_hash32(record->data, record->data_size)),
      .header_crc = 0u,
      .reserved = {0u, 0u, 0u, 0u},
  };
  header.header_crc = vkr_texture_cache_u32_le(
      vkr_texture_cache_crc32(&header, sizeof(header)));

  FilePath temp_fp = file_path_create((const char *)temp_path.str, scratch,
                                      vkr_texture_cache_path_type(temp_path));
  FilePath cache_fp = file_path_create((const char *)cache_path.str, scratch,
                                       vkr_texture_cache_path_type(cache_path));
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  bool8_t ok = file_open(&temp_fp, mode, &file) == FILE_ERROR_NONE;
  uint64_t written = 0u;
  if (ok) {
    ok = file_write(&file, sizeof(header), (const uint8_t *)&header,
                    &written) == FILE_ERROR_NONE &&
         written == sizeof(header);
  }
  const uint64_t region_bytes =
      (uint64_t)record->region_count * sizeof(*stored_regions);
  if (ok) {
    ok = file_write(&file, region_bytes, (const uint8_t *)stored_regions,
                    &written) == FILE_ERROR_NONE &&
         written == region_bytes;
  }
  if (ok) {
    ok = file_write(&file, record->data_size, record->data, &written) ==
             FILE_ERROR_NONE &&
         written == record->data_size;
  }
  file_close(&file);
  free(stored_regions);
  if (!ok || file_rename(&temp_fp, &cache_fp, true_v) != FILE_ERROR_NONE) {
    vkr_texture_cache_remove(temp_path);
    return false_v;
  }
  return true_v;
}

void vkr_texture_transcode_cache_release(
    VkrTextureTranscodeCacheRecord *record) {
  if (!record) {
    return;
  }
  free(record->data);
  free(record->regions);
  *record = (VkrTextureTranscodeCacheRecord){0};
}
