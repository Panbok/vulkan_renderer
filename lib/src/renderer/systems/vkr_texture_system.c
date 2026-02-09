#include "renderer/systems/vkr_texture_system.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

#include "ktx.h"
#include "stb_image.h"

// =============================================================================
// Texture Cache Format
// =============================================================================
// Binary cache format for decoded textures to skip stb_image decoding on
// subsequent loads. Cache files are stored alongside source files with .vkt
// extension.

#define VKR_TEXTURE_CACHE_MAGIC 0x564B5448u /* 'VKTH' in little-endian */
#define VKR_TEXTURE_CACHE_VERSION 3u        /* Bump when format changes */
#define VKR_TEXTURE_CACHE_EXT ".vkt"

/**
 * @brief Header for the texture cache file
 * @note Cache stores raw RGBA bytes; color space is selected at upload time.
 * @param magic The magic number for the cache file
 * @param version The version of the cache file
 * @param source_mtime The modification time of the source file
 * @param width The width of the texture
 * @param height The height of the texture
 * @param channels The number of channels in the texture
 * @param has_transparency Whether any alpha value is not fully opaque
 */
typedef struct VkrTextureCacheHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t source_mtime; // Source file modification time for invalidation
  uint32_t width;
  uint32_t height;
  uint32_t channels; // Always 4 (RGBA) after processing
  uint8_t has_transparency;
  uint8_t padding[3];
  // Followed by: width * height * channels bytes of raw pixel data
} VkrTextureCacheHeader;

/**
 * @brief Converts a 32-bit value from host endianness to little endian
 * @param value The value to convert
 * @return The converted value
 */
vkr_internal uint32_t vkr_texture_host_to_little_u32(uint32_t value) {
  const union {
    uint32_t u32;
    uint8_t u8[4];
  } endian_check = {0x01020304};
  const bool8_t is_little_endian = (endian_check.u8[0] == 0x04);

  if (is_little_endian) {
    return value;
  } else {
    return ((value & 0xFF000000) >> 24) | ((value & 0x00FF0000) >> 8) |
           ((value & 0x0000FF00) << 8) | ((value & 0x000000FF) << 24);
  }
}

/**
 * @brief Converts a 64-bit value from host endianness to little endian
 * @param value The value to convert
 * @return The converted value
 */
vkr_internal uint64_t vkr_texture_host_to_little_u64(uint64_t value) {
  const union {
    uint32_t u32;
    uint8_t u8[4];
  } endian_check = {0x01020304};
  const bool8_t is_little_endian = (endian_check.u8[0] == 0x04);

  if (is_little_endian) {
    return value;
  } else {
    return ((value & 0xFF00000000000000ULL) >> 56) |
           ((value & 0x00FF000000000000ULL) >> 40) |
           ((value & 0x0000FF0000000000ULL) >> 24) |
           ((value & 0x000000FF00000000ULL) >> 8) |
           ((value & 0x00000000FF000000ULL) << 8) |
           ((value & 0x0000000000FF0000ULL) << 24) |
           ((value & 0x000000000000FF00ULL) << 40) |
           ((value & 0x00000000000000FFULL) << 56);
  }
}

// Generate cache path from source path (e.g., "textures/foo.png" ->
// "textures/foo.png.vkt")
vkr_internal String8 vkr_texture_cache_path(VkrAllocator *allocator,
                                            String8 source_path) {
  assert_log(allocator != NULL, "Allocator is NULL");
  return string8_create_formatted(allocator, "%.*s%s",
                                  (int32_t)source_path.length, source_path.str,
                                  VKR_TEXTURE_CACHE_EXT);
}

typedef struct VkrTextureCacheWriteEntry {
  uint8_t active;
} VkrTextureCacheWriteEntry;
VkrHashTable(VkrTextureCacheWriteEntry);

typedef struct VkrTextureCacheWriteGuard {
  VkrMutex mutex;
  VkrHashTable_VkrTextureCacheWriteEntry inflight;
} VkrTextureCacheWriteGuard;

vkr_internal bool8_t vkr_texture_cache_guard_try_acquire(
    VkrTextureCacheWriteGuard *guard, const char *key) {
  if (!guard || !key) {
    return true_v;
  }

  if (!vkr_mutex_lock(guard->mutex)) {
    return false_v;
  }

  if (vkr_hash_table_contains_VkrTextureCacheWriteEntry(&guard->inflight,
                                                        key)) {
    vkr_mutex_unlock(guard->mutex);
    return false_v;
  }

  VkrTextureCacheWriteEntry entry = {.active = 1};
  bool8_t inserted = vkr_hash_table_insert_VkrTextureCacheWriteEntry(
      &guard->inflight, key, entry);
  vkr_mutex_unlock(guard->mutex);
  return inserted;
}

vkr_internal void
vkr_texture_cache_guard_release(VkrTextureCacheWriteGuard *guard,
                                const char *key) {
  if (!guard || !key) {
    return;
  }

  if (!vkr_mutex_lock(guard->mutex)) {
    return;
  }

  vkr_hash_table_remove_VkrTextureCacheWriteEntry(&guard->inflight, key);
  vkr_mutex_unlock(guard->mutex);
}

/**
 * @brief Parses common truthy/falsy environment values.
 *
 * Empty or unknown values keep the provided default so rollout toggles can
 * evolve without crashing older launch scripts.
 */
vkr_internal bool8_t vkr_texture_env_flag(const char *name,
                                          bool8_t default_value) {
  if (!name || name[0] == '\0') {
    return default_value;
  }

  const char *value = getenv(name);
  if (!value || value[0] == '\0') {
    return default_value;
  }

  switch (value[0]) {
  case '1':
  case 'y':
  case 'Y':
  case 't':
  case 'T':
    return true_v;
  case '0':
  case 'n':
  case 'N':
  case 'f':
  case 'F':
    return false_v;
  default:
    return default_value;
  }
}

/**
 * @brief Desired sampling color space for a texture request.
 */
typedef enum VkrTextureColorSpace {
  VKR_TEXTURE_COLORSPACE_LINEAR = 0,
  VKR_TEXTURE_COLORSPACE_SRGB = 1,
} VkrTextureColorSpace;

/**
 * @brief Parsed texture request with base path and requested color space.
 */
typedef struct VkrTextureRequest {
  String8 base_path;
  VkrTextureColorSpace colorspace;
} VkrTextureRequest;

typedef struct VkrTextureQueryColorScanResult {
  bool8_t prefers_srgb;
  bool8_t had_unknown;
} VkrTextureQueryColorScanResult;

vkr_internal bool8_t vkr_texture_path_has_vkt_extension(String8 path) {
  if (!path.str || path.length < 4) {
    return false_v;
  }

  for (uint64_t i = path.length; i > 0; --i) {
    if (path.str[i - 1] == '.') {
      String8 ext = string8_substring(&path, i, path.length);
      String8 vkt_ext = string8_lit("vkt");
      return string8_equalsi(&ext, &vkt_ext);
    }
  }

  return false_v;
}

/**
 * @brief Strip the query portion from a texture name.
 * @param name The requested texture name (may include a query).
 * @param out_query Optional output for the query substring (without '?').
 * @return The base path without any query parameters.
 */
vkr_internal String8 vkr_texture_strip_query(String8 name, String8 *out_query) {
  for (uint64_t i = 0; i < name.length; ++i) {
    if (name.str[i] == '?') {
      if (out_query) {
        *out_query = string8_substring(&name, i + 1, name.length);
      }
      return string8_substring(&name, 0, i);
    }
  }

  if (out_query) {
    *out_query = (String8){0};
  }

  return name;
}

/**
 * @brief Scans `cs` query parameters and resolves final colorspace preference.
 *
 * Parsing order is left-to-right so later `cs` values override earlier ones.
 * Unknown values optionally force linear fallback to match legacy behavior.
 */
vkr_internal VkrTextureQueryColorScanResult vkr_texture_scan_query_colorspace(
    String8 query, bool8_t default_prefers_srgb, bool8_t unknown_sets_linear) {
  VkrTextureQueryColorScanResult result = {
      .prefers_srgb = default_prefers_srgb,
      .had_unknown = false_v,
  };
  const String8 key_cs = string8_lit("cs");
  const String8 val_srgb = string8_lit("srgb");
  const String8 val_linear = string8_lit("linear");

  uint64_t start = 0;
  while (start < query.length) {
    uint64_t end = start;
    while (end < query.length && query.str[end] != '&') {
      end++;
    }

    String8 param = string8_substring(&query, start, end);
    uint64_t eq_pos = UINT64_MAX;
    for (uint64_t i = 0; i < param.length; ++i) {
      if (param.str[i] == '=') {
        eq_pos = i;
        break;
      }
    }

    if (eq_pos != UINT64_MAX && eq_pos > 0 && eq_pos + 1 < param.length) {
      String8 key = string8_substring(&param, 0, eq_pos);
      if (string8_equalsi(&key, &key_cs)) {
        String8 value = string8_substring(&param, eq_pos + 1, param.length);
        if (string8_equalsi(&value, &val_srgb)) {
          result.prefers_srgb = true_v;
        } else if (string8_equalsi(&value, &val_linear)) {
          result.prefers_srgb = false_v;
        } else {
          result.had_unknown = true_v;
          if (unknown_sets_linear) {
            result.prefers_srgb = false_v;
          }
        }
      }
    }

    start = end + 1;
  }

  return result;
}

/**
 * @brief Parse a texture request into a base path and desired color space.
 * @note Only the `cs` query parameter is consumed; others are ignored.
 * @note Unknown `cs` values log once and default to linear.
 */
vkr_internal VkrTextureRequest vkr_texture_parse_request(String8 name) {
  String8 query = {0};
  String8 base_path = vkr_texture_strip_query(name, &query);
  VkrTextureQueryColorScanResult scan =
      vkr_texture_scan_query_colorspace(query, false_v, true_v);

  if (scan.had_unknown) {
    vkr_local_persist bool8_t warned_unknown = false_v;
    if (!warned_unknown) {
      log_warn("Texture request has unknown colorspace value; defaulting to "
               "linear");
      warned_unknown = true_v;
    }
  }

  return (VkrTextureRequest){
      .base_path = base_path,
      .colorspace = scan.prefers_srgb ? VKR_TEXTURE_COLORSPACE_SRGB
                                      : VKR_TEXTURE_COLORSPACE_LINEAR,
  };
}

bool8_t vkr_texture_is_vkt_path(String8 path) {
  String8 query = {0};
  String8 base_path = vkr_texture_strip_query(path, &query);
  (void)query;
  return vkr_texture_path_has_vkt_extension(base_path);
}

void vkr_texture_build_resolution_candidates(VkrAllocator *allocator,
                                             String8 request_path,
                                             String8 *out_direct_vkt,
                                             String8 *out_sidecar_vkt,
                                             String8 *out_source_path) {
  assert_log(allocator != NULL, "Allocator is NULL");

  VkrTextureRequest request = vkr_texture_parse_request(request_path);
  const bool8_t direct_vkt =
      vkr_texture_path_has_vkt_extension(request.base_path);

  if (out_source_path) {
    *out_source_path = request.base_path;
  }

  if (direct_vkt) {
    if (out_direct_vkt) {
      *out_direct_vkt = request.base_path;
    }
    if (out_sidecar_vkt) {
      *out_sidecar_vkt = (String8){0};
    }
    return;
  }

  if (out_direct_vkt) {
    *out_direct_vkt = (String8){0};
  }
  if (out_sidecar_vkt) {
    *out_sidecar_vkt = vkr_texture_cache_path(allocator, request.base_path);
  }
}

VkrTextureVktContainerType
vkr_texture_detect_vkt_container(const uint8_t *bytes, uint64_t size) {
  static const uint8_t ktx2_signature[12] = {
      0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

  if (!bytes || size < 4) {
    return VKR_TEXTURE_VKT_CONTAINER_UNKNOWN;
  }

  if (size >= sizeof(ktx2_signature) &&
      MemCompare(bytes, ktx2_signature, sizeof(ktx2_signature)) == 0) {
    return VKR_TEXTURE_VKT_CONTAINER_KTX2;
  }

  const uint32_t magic = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                         ((uint32_t)bytes[2] << 16) |
                         ((uint32_t)bytes[3] << 24);
  if (magic == VKR_TEXTURE_CACHE_MAGIC) {
    return VKR_TEXTURE_VKT_CONTAINER_LEGACY_RAW;
  }

  return VKR_TEXTURE_VKT_CONTAINER_UNKNOWN;
}

bool8_t vkr_texture_request_prefers_srgb(String8 request_path,
                                         bool8_t default_srgb) {
  String8 query = {0};
  (void)vkr_texture_strip_query(request_path, &query);
  return vkr_texture_scan_query_colorspace(query, default_srgb, false_v)
      .prefers_srgb;
}

VkrTextureFormat vkr_texture_select_transcode_target_format(
    bool8_t prefer_astc_platform, bool8_t request_srgb,
    bool8_t supports_astc_4x4, bool8_t supports_bc7) {
  if (prefer_astc_platform) {
    if (supports_astc_4x4) {
      return request_srgb ? VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB
                          : VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
    }
  } else {
    if (supports_bc7) {
      return request_srgb ? VKR_TEXTURE_FORMAT_BC7_SRGB
                          : VKR_TEXTURE_FORMAT_BC7_UNORM;
    }
  }

  return request_srgb ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
                      : VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
}

/**
 * @brief Choose a GPU format based on channel count and color space.
 * @note sRGB applies only to 4-channel color textures; single/dual channels
 * stay linear.
 */
vkr_internal VkrTextureFormat vkr_texture_format_from_channels(
    uint32_t channels, VkrTextureColorSpace colorspace) {
  switch (channels) {
  case VKR_TEXTURE_R_CHANNELS:
    return VKR_TEXTURE_FORMAT_R8_UNORM;
  case VKR_TEXTURE_RG_CHANNELS:
    return VKR_TEXTURE_FORMAT_R8G8_UNORM;
  case VKR_TEXTURE_RGB_CHANNELS:
  case VKR_TEXTURE_RGBA_CHANNELS:
    return colorspace == VKR_TEXTURE_COLORSPACE_SRGB
               ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
               : VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  default:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  }
}

typedef struct VkrTextureAlphaAnalysis {
  bool8_t has_transparency;
  bool8_t alpha_mask;
} VkrTextureAlphaAnalysis;

// Treat alpha as a cutout mask when only a small fraction of transparent texels
// have intermediate coverage (typical for foliage with anti-aliased edges).
#define VKR_TEXTURE_ALPHA_MASK_INTERMEDIATE_RATIO 0.30f

vkr_internal VkrTextureAlphaAnalysis vkr_texture_analyze_alpha(
    const uint8_t *pixels, uint64_t pixel_count, uint32_t channels) {
  VkrTextureAlphaAnalysis analysis = {false_v, false_v};
  if (!pixels || channels < VKR_TEXTURE_RGBA_CHANNELS || pixel_count == 0) {
    return analysis;
  }

  uint64_t transparent_count = 0;
  uint64_t intermediate_count = 0;
  for (uint64_t pixel_index = 0; pixel_index < pixel_count; pixel_index++) {
    uint8_t alpha = pixels[pixel_index * channels + 3];
    if (alpha < 255) {
      transparent_count++;
      if (alpha > 0 && alpha < 255) {
        intermediate_count++;
      }
    }
  }

  if (transparent_count == 0) {
    return analysis;
  }

  analysis.has_transparency = true_v;
  float32_t ratio =
      (float32_t)intermediate_count / (float32_t)transparent_count;
  analysis.alpha_mask = (ratio <= VKR_TEXTURE_ALPHA_MASK_INTERMEDIATE_RATIO);
  return analysis;
}

vkr_internal bool8_t vkr_texture_has_transparency(const uint8_t *pixels,
                                                  uint64_t pixel_count,
                                                  uint32_t channels) {
  return vkr_texture_analyze_alpha(pixels, pixel_count, channels)
      .has_transparency;
}

vkr_internal bool8_t
vkr_texture_format_is_block_compressed(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal uint32_t
vkr_texture_channel_count_from_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return VKR_TEXTURE_R_CHANNELS;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return VKR_TEXTURE_RG_CHANNELS;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return VKR_TEXTURE_RGBA_CHANNELS;
  default:
    return VKR_TEXTURE_RGBA_CHANNELS;
  }
}

vkr_internal ktx_transcode_fmt_e
vkr_texture_ktx_transcode_format_from_texture_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return KTX_TTF_BC7_RGBA;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return KTX_TTF_ASTC_4x4_RGBA;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return KTX_TTF_RGBA32;
  default:
    return KTX_TTF_NOSELECTION;
  }
}

/**
 * @brief Writes decoded texture data to cache file
 * @param allocator The allocator to use
 * @param cache_path The path to the cache file
 * @param source_mtime The modification time of the source file
 * @param width The width of the texture
 * @param height The height of the texture
 * @param channels The number of channels in the texture
 * @param has_transparency Whether the texture has transparency
 * @param pixel_data The pixel data of the texture
 * @return true on success, false on failure
 */
vkr_internal bool8_t vkr_texture_cache_write(
    VkrAllocator *allocator, String8 cache_path, uint64_t source_mtime,
    uint32_t width, uint32_t height, uint32_t channels,
    bool8_t has_transparency, const uint8_t *pixel_data) {
  assert_log(allocator != NULL, "Allocator is NULL");

  if (!cache_path.str || !pixel_data) {
    return false_v;
  }

  FilePath fp = file_path_create((const char *)cache_path.str, allocator,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    return false_v;
  }

  VkrTextureCacheHeader header = {
      .magic = vkr_texture_host_to_little_u32(VKR_TEXTURE_CACHE_MAGIC),
      .version = vkr_texture_host_to_little_u32(VKR_TEXTURE_CACHE_VERSION),
      .source_mtime = vkr_texture_host_to_little_u64(source_mtime),
      .width = vkr_texture_host_to_little_u32(width),
      .height = vkr_texture_host_to_little_u32(height),
      .channels = vkr_texture_host_to_little_u32(channels),
      .has_transparency = has_transparency ? 1 : 0,
      .padding = {0, 0, 0},
  };

  uint64_t written = 0;
  FileError write_err =
      file_write(&fh, sizeof(header), (const uint8_t *)&header, &written);
  if (write_err != FILE_ERROR_NONE || written != sizeof(header)) {
    file_close(&fh);
    return false_v;
  }

  uint64_t pixel_size = (uint64_t)width * (uint64_t)height * (uint64_t)channels;
  write_err = file_write(&fh, pixel_size, pixel_data, &written);
  file_close(&fh);

  if (write_err != FILE_ERROR_NONE || written != pixel_size) {
    return false_v;
  }

  return true_v;
}

/**
 * @brief Reads texture from cache file. Returns allocated pixel data on
 * success. Caller must free with stbi_image_free() for consistency with decode
 * path.
 * @param allocator The allocator to use
 * @param cache_path The path to the cache file
 * @param validate_source_mtime Whether source_mtime mismatch should reject
 * cache usage
 * @param source_mtime The modification time of the source file
 * @param out_width The width of the texture
 * @param out_height The height of the texture
 * @param out_channels The number of channels in the texture
 * @param out_has_transparency Whether any alpha value is not fully opaque
 */
vkr_internal bool8_t vkr_texture_cache_read(
    VkrAllocator *allocator, String8 cache_path, bool8_t validate_source_mtime,
    uint64_t source_mtime, uint32_t *out_width, uint32_t *out_height,
    uint32_t *out_channels, bool8_t *out_has_transparency,
    uint8_t **out_pixel_data) {
  assert_log(allocator != NULL, "Allocator is NULL");

  if (!cache_path.str || !out_pixel_data) {
    return false_v;
  }

  FilePath fp = file_path_create((const char *)cache_path.str, allocator,
                                 FILE_PATH_TYPE_RELATIVE);

  if (!file_exists(&fp)) {
    return false_v;
  }

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    return false_v;
  }

  uint64_t bytes_read = 0;
  uint8_t *header_buf = NULL;
  FileError read_err = file_read(&fh, allocator, sizeof(VkrTextureCacheHeader),
                                 &bytes_read, &header_buf);
  if (read_err != FILE_ERROR_NONE ||
      bytes_read != sizeof(VkrTextureCacheHeader) || !header_buf) {
    file_close(&fh);
    return false_v;
  }
  VkrTextureCacheHeader header;
  MemCopy(&header, header_buf, sizeof(header));

  uint32_t magic = vkr_texture_host_to_little_u32(header.magic);
  uint32_t version = vkr_texture_host_to_little_u32(header.version);
  uint64_t cached_mtime = vkr_texture_host_to_little_u64(header.source_mtime);

  if (magic != VKR_TEXTURE_CACHE_MAGIC ||
      version != VKR_TEXTURE_CACHE_VERSION) {
    file_close(&fh);
    return false_v;
  }

  if (validate_source_mtime && cached_mtime != source_mtime) {
    file_close(&fh);
    return false_v;
  }

  uint32_t width = vkr_texture_host_to_little_u32(header.width);
  uint32_t height = vkr_texture_host_to_little_u32(header.height);
  uint32_t channels = vkr_texture_host_to_little_u32(header.channels);
  if (width == 0 || height == 0 || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION || channels == 0 || channels > 4) {
    file_close(&fh);
    return false_v;
  }

  uint64_t pixel_size = (uint64_t)width * (uint64_t)height * (uint64_t)channels;

  uint8_t *temp_pixels = NULL;
  read_err = file_read(&fh, allocator, pixel_size, &bytes_read, &temp_pixels);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE || bytes_read != pixel_size || !temp_pixels) {
    return false_v;
  }

  // Allocate using malloc so caller can free with stbi_image_free (which uses
  // free)
  uint8_t *pixels = (uint8_t *)malloc((size_t)pixel_size);
  if (!pixels) {
    return false_v;
  }
  MemCopy(pixels, temp_pixels, (size_t)pixel_size);

  *out_width = width;
  *out_height = height;
  *out_channels = channels;
  *out_has_transparency = header.has_transparency != 0;
  *out_pixel_data = pixels;

  return true_v;
}

uint32_t vkr_texture_system_find_free_slot(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");

  for (uint32_t texture_id = system->next_free_index;
       texture_id < system->config.max_texture_count; texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation == VKR_INVALID_ID) {
      system->next_free_index = texture_id + 1;
      return texture_id;
    }
  }

  for (uint32_t texture_id = 0; texture_id < system->next_free_index;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation == VKR_INVALID_ID) {
      system->next_free_index = texture_id + 1;
      return texture_id;
    }
  }

  return VKR_INVALID_ID;
}

bool8_t vkr_texture_system_init(VkrRendererFrontendHandle renderer,
                                const VkrTextureSystemConfig *config,
                                VkrJobSystem *job_system,
                                VkrTextureSystem *out_system) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_system != NULL, "Out system is NULL");
  assert_log(config->max_texture_count > 0,
             "Max texture count must be greater than 0");
  assert_log(config->max_texture_count >= 3,
             "Texture system requires at least 3 textures for defaults");

  MemZero(out_system, sizeof(*out_system));

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  out_system->arena =
      arena_create(VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_RSV,
                   VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_CMT, app_arena_flags);
  if (!out_system->arena) {
    log_fatal("Failed to create texture system arena");
    return false_v;
  }

  out_system->renderer = renderer;
  out_system->config = *config;
  out_system->job_system = job_system;
  out_system->allocator = (VkrAllocator){.ctx = out_system->arena};
  vkr_allocator_arena(&out_system->allocator);

  if (!vkr_dmemory_create(MB(1), MB(16), &out_system->string_memory)) {
    log_error("Failed to create texture system string allocator");
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(*out_system));
    return false_v;
  }
  out_system->string_allocator =
      (VkrAllocator){.ctx = &out_system->string_memory};
  vkr_dmemory_allocator_create(&out_system->string_allocator);

#if defined(PLATFORM_APPLE)
  out_system->prefer_astc_transcode = true_v;
#else
  out_system->prefer_astc_transcode = false_v;
#endif
  out_system->supports_texture_astc_4x4 = false_v;
  out_system->supports_texture_bc7 = false_v;
  VkrDeviceInformation device_info = {0};
  vkr_renderer_get_device_information(renderer, &device_info,
                                      out_system->arena);
  out_system->supports_texture_astc_4x4 = device_info.supports_texture_astc_4x4;
  out_system->supports_texture_bc7 = device_info.supports_texture_bc7;

  out_system->strict_vkt_only_mode =
      vkr_texture_env_flag("VKR_TEXTURE_VKT_STRICT", false_v);
  out_system->allow_source_fallback =
      vkr_texture_env_flag("VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK",
                           out_system->strict_vkt_only_mode ? false_v : true_v);
  out_system->allow_legacy_vkt =
      vkr_texture_env_flag("VKR_TEXTURE_VKT_ALLOW_LEGACY",
                           out_system->strict_vkt_only_mode ? false_v : true_v);
  out_system->allow_legacy_cache_write =
      vkr_texture_env_flag("VKR_TEXTURE_VKT_WRITE_LEGACY_CACHE", false_v);

  if (out_system->strict_vkt_only_mode) {
    out_system->allow_source_fallback = false_v;
    out_system->allow_legacy_vkt = false_v;
    out_system->allow_legacy_cache_write = false_v;
  }

  log_info("Texture `.vkt` policy: strict=%u, allow_source_fallback=%u, "
           "allow_legacy=%u, allow_legacy_cache_write=%u",
           (uint32_t)out_system->strict_vkt_only_mode,
           (uint32_t)out_system->allow_source_fallback,
           (uint32_t)out_system->allow_legacy_vkt,
           (uint32_t)out_system->allow_legacy_cache_write);

  out_system->textures = array_create_VkrTexture(&out_system->allocator,
                                                 config->max_texture_count);
  out_system->texture_map = vkr_hash_table_create_VkrTextureEntry(
      &out_system->allocator, ((uint64_t)config->max_texture_count) * 2ULL);
  out_system->cache_guard =
      (struct VkrTextureCacheWriteGuard *)vkr_allocator_alloc(
          &out_system->allocator, sizeof(VkrTextureCacheWriteGuard),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!out_system->cache_guard) {
    log_error("Failed to allocate texture cache write guard");
    vkr_texture_system_shutdown(renderer, out_system);
    return false_v;
  }

  MemZero(out_system->cache_guard, sizeof(VkrTextureCacheWriteGuard));
  if (!vkr_mutex_create(&out_system->allocator,
                        &out_system->cache_guard->mutex)) {
    log_error("Failed to create texture cache write guard mutex");
    vkr_texture_system_shutdown(renderer, out_system);
    return false_v;
  }

  uint64_t guard_capacity =
      Max(16ULL, (uint64_t)config->max_texture_count * 2ULL);
  out_system->cache_guard->inflight =
      vkr_hash_table_create_VkrTextureCacheWriteEntry(&out_system->allocator,
                                                      guard_capacity);
  if (!out_system->cache_guard->inflight.entries) {
    log_error("Failed to create texture cache write guard hash table");
    vkr_texture_system_shutdown(renderer, out_system);
    return false_v;
  }

  out_system->next_free_index = 0;
  out_system->generation_counter = 1;

  // Initialize slots as invalid
  for (uint32_t texture_index = 0; texture_index < config->max_texture_count;
       texture_index++) {
    out_system->textures.data[texture_index].description.id = VKR_INVALID_ID;
    out_system->textures.data[texture_index].description.generation =
        VKR_INVALID_ID;
  }

  // Create default checkerboard texture at index 0
  VkrTexture *default_texture = &out_system->textures.data[0];
  default_texture->description = (VkrTextureDescription){
      .width = 256,
      .height = 256,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  uint64_t image_size = (uint64_t)default_texture->description.width *
                        (uint64_t)default_texture->description.height *
                        (uint64_t)default_texture->description.channels;

  VkrAllocatorScope image_scope =
      vkr_allocator_begin_scope(&out_system->allocator);
  if (!vkr_allocator_scope_is_valid(&image_scope)) {
    log_error("Failed to allocate memory for default texture");
    vkr_texture_system_shutdown(renderer, out_system);
    return false_v;
  }
  default_texture->image = vkr_allocator_alloc(
      &out_system->allocator, image_size, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!default_texture->image) {
    log_error("Failed to allocate memory for default texture");
    vkr_allocator_end_scope(&image_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return false_v;
  }
  MemSet(default_texture->image, 255, image_size);

  const uint32_t tile_size = 8;
  for (uint32_t row = 0; row < default_texture->description.height; row++) {
    for (uint32_t col = 0; col < default_texture->description.width; col++) {
      uint32_t pixel_index = (row * default_texture->description.width + col) *
                             default_texture->description.channels;
      uint32_t tile_row = row / tile_size;
      uint32_t tile_col = col / tile_size;
      bool32_t is_white = ((tile_row + tile_col) % 2) == 0;
      uint8_t channel_value = is_white ? 255 : 0;
      default_texture->image[pixel_index + 0] = channel_value;
      default_texture->image[pixel_index + 1] = channel_value;
      default_texture->image[pixel_index + 2] = channel_value;
      default_texture->image[pixel_index + 3] = 255;
    }
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  default_texture->handle =
      vkr_renderer_create_texture(renderer, &default_texture->description,
                                  default_texture->image, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(renderer_error);
    log_error("Failed to create default checkerboard texture: %s",
              string8_cstr(&error_string));
    vkr_allocator_end_scope(&image_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return false_v;
  }

  // Assign a stable id for default texture and lock index 0 as occupied
  default_texture->description.id = 1; // slot 0 -> id 1
  default_texture->description.generation = out_system->generation_counter++;

  out_system->default_texture =
      (VkrTextureHandle){.id = default_texture->description.id,
                         .generation = default_texture->description.generation};

  vkr_allocator_end_scope(&image_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  default_texture->image = NULL;

  // Create a 1x1 flat normal texture for cases where no normal map is provided
  VkrTexture *default_normal = &out_system->textures.data[1];
  default_normal->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  const uint8_t flat_normal_pixel[4] = {128, 128, 255, 255};
  VkrRendererError normal_err = VKR_RENDERER_ERROR_NONE;
  default_normal->handle = vkr_renderer_create_texture(
      renderer, &default_normal->description, flat_normal_pixel, &normal_err);
  if (normal_err != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(normal_err);
    log_error("Failed to create default normal texture: %s",
              string8_cstr(&error_string));
    return false_v;
  }

  default_normal->description.id = 2; // slot 1 -> id 2
  default_normal->description.generation = out_system->generation_counter++;
  default_normal->image = NULL;
  out_system->default_normal_texture =
      (VkrTextureHandle){.id = default_normal->description.id,
                         .generation = default_normal->description.generation};

  // Create a 1x1 flat specular texture for cases where no specular map is
  // provided
  VkrTexture *default_specular = &out_system->textures.data[2];
  default_specular->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  const uint8_t flat_specular_pixel[4] = {255, 255, 255, 255};
  VkrRendererError specular_err = VKR_RENDERER_ERROR_NONE;
  default_specular->handle =
      vkr_renderer_create_texture(renderer, &default_specular->description,
                                  flat_specular_pixel, &specular_err);
  if (specular_err != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(specular_err);
    log_error("Failed to create default specular texture: %s",
              string8_cstr(&error_string));
    // Clean up the already-created default normal texture
    vkr_renderer_destroy_texture(renderer, default_normal->handle);
    default_normal->handle = NULL;
    default_normal->description.generation = VKR_INVALID_ID;
    out_system->default_normal_texture.id = VKR_INVALID_ID;
    out_system->default_normal_texture.generation = VKR_INVALID_ID;
    return false_v;
  }

  default_specular->description.id = 3; // slot 2 -> id 3
  default_specular->description.generation = out_system->generation_counter++;
  default_specular->image = NULL;
  out_system->default_specular_texture = (VkrTextureHandle){
      .id = default_specular->description.id,
      .generation = default_specular->description.generation};

  // Create a 1x1 white diffuse texture for materials without diffuse maps.
  // Using white (1,1,1,1) ensures material diffuse_color is preserved.
  VkrTexture *default_diffuse = &out_system->textures.data[3];
  default_diffuse->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = bitset8_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  const uint8_t white_pixel[4] = {255, 255, 255, 255};
  VkrRendererError diffuse_err = VKR_RENDERER_ERROR_NONE;
  default_diffuse->handle = vkr_renderer_create_texture(
      renderer, &default_diffuse->description, white_pixel, &diffuse_err);
  if (diffuse_err != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(diffuse_err);
    log_error("Failed to create default diffuse texture: %s",
              string8_cstr(&error_string));
    vkr_texture_system_shutdown(renderer, out_system);
    return false_v;
  }

  default_diffuse->description.id = 4; // slot 3 -> id 4
  default_diffuse->description.generation = out_system->generation_counter++;
  default_diffuse->image = NULL;
  out_system->default_diffuse_texture =
      (VkrTextureHandle){.id = default_diffuse->description.id,
                         .generation = default_diffuse->description.generation};

  // Ensure first free search starts after reserved defaults
  out_system->next_free_index = 4;

  return true_v;
}

void vkr_texture_system_shutdown(VkrRendererFrontendHandle renderer,
                                 VkrTextureSystem *system) {
  if (!system)
    return;

  for (uint32_t texture_id = 0; texture_id < system->textures.length;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation != VKR_INVALID_ID && texture->handle) {
      vkr_texture_destroy(renderer, texture);
    }
  }

  if (system->cache_guard) {
    vkr_hash_table_destroy_VkrTextureCacheWriteEntry(
        &system->cache_guard->inflight);
    vkr_mutex_destroy(&system->allocator, &system->cache_guard->mutex);
    system->cache_guard = NULL;
  }

  array_destroy_VkrTexture(&system->textures);
  if (system->string_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&system->string_allocator);
  }
  arena_destroy(system->arena);
  MemZero(system, sizeof(*system));
}

VkrTextureHandle vkr_texture_system_acquire(VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (entry) {
    if (entry->ref_count == 0) {
      entry->auto_release = auto_release;
    }
    entry->ref_count++;
    *out_error = VKR_RENDERER_ERROR_NONE;
    VkrTexture *texture = &system->textures.data[entry->index];
    return (VkrTextureHandle){.id = texture->description.id,
                              .generation = texture->description.generation};
  }

  // Texture not loaded - return error
  log_warn("Texture '%s' not yet loaded, use resource system to load first",
           string8_cstr(&texture_name));
  *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
  return VKR_TEXTURE_HANDLE_INVALID;
}

bool8_t vkr_texture_system_create_writable(VkrTextureSystem *system,
                                           String8 name,
                                           const VkrTextureDescription *desc,
                                           VkrTextureHandle *out_handle,
                                           VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!name.str) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Check for duplicate name before allocating resources
  const char *texture_key = (const char *)name.str;
  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (existing_entry) {
    log_error("Texture with name '%s' already exists", texture_key);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrTextureDescription desc_copy = *desc;
  bitset8_set(&desc_copy.properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
  desc_copy.id = free_slot_index + 1;
  desc_copy.generation = system->generation_counter++;

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrTextureOpaqueHandle handle = vkr_renderer_create_writable_texture(
      system->renderer, &desc_copy, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE || handle == NULL) {
    *out_error = renderer_error;
    return false_v;
  }

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->string_allocator, name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for texture map");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_renderer_destroy_texture(system->renderer, handle);
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = desc_copy;
  texture->handle = handle;

  VkrTextureEntry entry = {
      .index = free_slot_index,
      .ref_count = 1,
      .auto_release = false_v,
      .name = stable_key,
  };
  bool8_t insert_success = vkr_hash_table_insert_VkrTextureEntry(
      &system->texture_map, stable_key, entry);
  if (!insert_success) {
    log_error("Failed to insert texture '%s' into hash table", stable_key);
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    vkr_renderer_destroy_texture(system->renderer, handle);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_texture_system_release(VkrTextureSystem *system,
                                String8 texture_name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(texture_name.str != NULL, "Name is NULL");

  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);

  if (!entry) {
    log_warn("Attempted to release unknown texture '%s'", texture_key);
    return;
  }

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for texture '%s'", texture_key);
    return;
  }

  entry->ref_count--;

  if (entry->ref_count == 0 && entry->auto_release) {
    uint32_t texture_index = entry->index;
    if (texture_index != system->default_texture.id - 1) {
      VkrResourceHandleInfo handle_info = {
          .type = VKR_RESOURCE_TYPE_TEXTURE,
          .loader_id = vkr_resource_system_get_loader_id(
              VKR_RESOURCE_TYPE_TEXTURE, texture_name),
          .as.texture = (VkrTextureHandle){
              .id = system->textures.data[texture_index].description.id,
              .generation =
                  system->textures.data[texture_index].description.generation}};
      vkr_resource_system_unload(&handle_info, texture_name);
    }
  }
}

void vkr_texture_system_release_by_handle(VkrTextureSystem *system,
                                          VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0) {
    log_warn("Attempted to release invalid texture handle");
    return;
  }

  for (uint64_t i = 0; i < system->texture_map.capacity; i++) {

    VkrHashEntry_VkrTextureEntry *entry = &system->texture_map.entries[i];
    if (entry->occupied != VKR_OCCUPIED) {
      continue;
    }

    uint32_t texture_index = entry->value.index;

    if (texture_index < system->textures.length) {
      VkrTexture *texture = &system->textures.data[texture_index];
      uint64_t key_length = string_length(entry->key);
      if (key_length == 0) {
        continue;
      }

      if (texture->description.id == handle.id &&
          texture->description.generation == handle.generation) {
        String8 texture_name =
            string8_create_from_cstr((const uint8_t *)entry->key, key_length);
        vkr_texture_system_release(system, texture_name);
        return;
      }
    }
  }
}

VkrRendererError vkr_texture_system_update_sampler(
    VkrTextureSystem *system, VkrTextureHandle handle, VkrFilter min_filter,
    VkrFilter mag_filter, VkrMipFilter mip_filter, bool8_t anisotropy_enable,
    VkrTextureRepeatMode u_repeat_mode, VkrTextureRepeatMode v_repeat_mode,
    VkrTextureRepeatMode w_repeat_mode) {
  assert_log(system != NULL, "System is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  VkrTextureDescription updated_desc = texture->description;
  updated_desc.min_filter = min_filter;
  updated_desc.mag_filter = mag_filter;
  updated_desc.mip_filter = mip_filter;
  updated_desc.anisotropy_enable = anisotropy_enable;
  updated_desc.u_repeat_mode = u_repeat_mode;
  updated_desc.v_repeat_mode = v_repeat_mode;
  updated_desc.w_repeat_mode = w_repeat_mode;

  VkrRendererError err = vkr_renderer_update_texture(
      system->renderer, texture->handle, &updated_desc);
  if (err == VKR_RENDERER_ERROR_NONE) {
    texture->description = updated_desc;
  }
  return err;
}

VkrRendererError vkr_texture_system_write(VkrTextureSystem *system,
                                          VkrTextureHandle handle,
                                          const void *data, uint64_t size) {
  assert_log(system != NULL, "System is NULL");
  assert_log(data != NULL, "Data is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)texture->description.width *
                           (uint64_t)texture->description.height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  return vkr_renderer_write_texture(system->renderer, texture->handle, data,
                                    size);
}

VkrRendererError vkr_texture_system_write_region(
    VkrTextureSystem *system, VkrTextureHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size) {
  assert_log(system != NULL, "System is NULL");
  assert_log(region != NULL, "Region is NULL");
  assert_log(data != NULL, "Data is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (region->mip_level >= 32) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (region->width == 0 || region->height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t mip_width = Max(1u, texture->description.width >> region->mip_level);
  uint32_t mip_height =
      Max(1u, texture->description.height >> region->mip_level);
  if (region->x + region->width > mip_width ||
      region->y + region->height > mip_height) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)region->width * (uint64_t)region->height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  return vkr_renderer_write_texture_region(system->renderer, texture->handle,
                                           region, data, size);
}

bool8_t vkr_texture_system_resize(VkrTextureSystem *system,
                                  VkrTextureHandle handle, uint32_t new_width,
                                  uint32_t new_height,
                                  bool8_t preserve_contents,
                                  VkrTextureHandle *out_handle,
                                  VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (new_width == 0 || new_height == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err =
      vkr_renderer_resize_texture(system->renderer, texture->handle, new_width,
                                  new_height, preserve_contents);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  texture->description.width = new_width;
  texture->description.height = new_height;
  texture->description.generation = system->generation_counter++;

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t
vkr_texture_system_register_external(VkrTextureSystem *system, String8 name,
                                     VkrTextureOpaqueHandle backend_handle,
                                     const VkrTextureDescription *desc,
                                     VkrTextureHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(backend_handle != NULL, "Backend handle is NULL");
  const bool8_t is_external =
      bitset8_is_set(&desc->properties, VKR_TEXTURE_PROPERTY_EXTERNAL_BIT);

  const char *texture_key = (const char *)name.str;
  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (existing_entry) {
    log_error("Texture with name '%s' is already registered",
              string8_cstr(&name));
    return false_v;
  }

  for (uint32_t i = 0; i < system->textures.length; ++i) {
    VkrTexture *texture = &system->textures.data[i];
    if (texture->handle == backend_handle) {
      log_error("Backend handle is already registered for texture '%s'",
                string8_cstr(&name));
      return false_v;
    }
  }

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    return false_v;
  }

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->string_allocator, name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for external texture map");
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = *desc;
  texture->description.id = free_slot_index + 1;
  texture->description.generation = system->generation_counter++;
  texture->handle = backend_handle;

  VkrTextureEntry entry = {
      .index = free_slot_index,
      .ref_count = 1,
      .auto_release = false_v,
      .name = stable_key,
  };
  bool8_t insert_success = vkr_hash_table_insert_VkrTextureEntry(
      &system->texture_map, stable_key, entry);
  if (!insert_success) {
    log_error("Failed to insert external texture '%s' into hash table",
              stable_key);
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!is_external) {
      vkr_renderer_destroy_texture(system->renderer, backend_handle);
    }
    texture->description.generation = VKR_INVALID_ID;
    return false_v;
  }

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  return true_v;
}

void vkr_texture_destroy(VkrRendererFrontendHandle renderer,
                         VkrTexture *texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  if (texture->handle && !bitset8_is_set(&texture->description.properties,
                                         VKR_TEXTURE_PROPERTY_EXTERNAL_BIT)) {
    vkr_renderer_destroy_texture(renderer, texture->handle);
  }

  MemZero(texture, sizeof(VkrTexture));
}

VkrTexture *vkr_texture_system_get_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == VKR_INVALID_ID)
    return NULL;

  uint32_t idx = handle.id - 1;
  if (idx >= system->textures.length)
    return NULL;
  VkrTexture *texture = &system->textures.data[idx];
  if (texture->description.generation != handle.generation)
    return NULL;
  return texture;
}

VkrTexture *vkr_texture_system_get_by_index(VkrTextureSystem *system,
                                            uint32_t texture_index) {
  if (!system || texture_index >= system->textures.length)
    return NULL;

  return array_get_VkrTexture(&system->textures, texture_index);
}

VkrTexture *vkr_texture_system_get_default(VkrTextureSystem *system) {
  return vkr_texture_system_get_by_index(system,
                                         system->default_texture.id - 1);
}

VkrTextureHandle
vkr_texture_system_get_default_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");

  if (system->textures.length == 0)
    return VKR_TEXTURE_HANDLE_INVALID;

  VkrTexture *texture = &system->textures.data[0];
  if (texture->description.id == VKR_INVALID_ID ||
      texture->description.generation == VKR_INVALID_ID)
    return VKR_TEXTURE_HANDLE_INVALID;
  return (VkrTextureHandle){.id = texture->description.id,
                            .generation = texture->description.generation};
}

VkrTextureHandle
vkr_texture_system_get_default_diffuse_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_diffuse_texture;
}

VkrTextureHandle
vkr_texture_system_get_default_normal_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_normal_texture;
}

VkrTextureHandle
vkr_texture_system_get_default_specular_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_specular_texture;
}

// =============================================================================
// Async Texture Loading Job Support
// =============================================================================

/**
 * @brief Output structure that the job writes to (caller-owned memory)
 * @note Decoded pixels are owned by stbi and must be freed with stbi_image_free
 * @param decoded_pixels The decoded pixels
 * @param width The width of the texture
 * @param height The height of the texture
 * @param original_channels The number of channels in the original texture
 * @param has_transparency Whether the texture has transparency (Set when loaded
 * from cache)
 * @param loaded_from_cache True if loaded from .vkt cache
 * @param error The error code
 * @param success True if the texture was loaded successfully
 */
typedef struct VkrTextureDecodeResult {
  uint8_t *decoded_pixels;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureUploadRegion *upload_regions;
  uint32_t upload_region_count;
  uint32_t upload_mip_levels;
  uint32_t upload_array_layers;
  VkrTextureFormat upload_format;
  bool8_t upload_is_compressed;
  bool8_t alpha_mask;
  int32_t width;
  int32_t height;
  int32_t original_channels;
  bool8_t has_transparency;
  bool8_t loaded_from_cache;
  VkrRendererError error;
  bool8_t success;
} VkrTextureDecodeResult;

/**
 * @brief Payload for the texture decoding job
 * @param file_path The path to the texture file
 * @param desired_channels The number of channels to request from the texture
 * @param flip_vertical Whether to flip the texture vertically
 * @param system The texture system owning the cache guard
 * @param result The result of the texture decoding
 */
typedef struct VkrTextureDecodeJobPayload {
  String8 file_path;
  uint32_t desired_channels;
  bool8_t flip_vertical;
  VkrTextureColorSpace colorspace;
  VkrTextureSystem *system;

  VkrTextureDecodeResult *result;
} VkrTextureDecodeJobPayload;

vkr_internal char *vkr_texture_path_to_cstr(VkrAllocator *allocator,
                                            String8 path);

vkr_internal void
vkr_texture_decode_result_reset(VkrTextureDecodeResult *result) {
  if (!result) {
    return;
  }

  result->success = false_v;
  result->error = VKR_RENDERER_ERROR_NONE;
  result->decoded_pixels = NULL;
  result->upload_data = NULL;
  result->upload_data_size = 0;
  result->upload_regions = NULL;
  result->upload_region_count = 0;
  result->upload_mip_levels = 0;
  result->upload_array_layers = 0;
  result->upload_is_compressed = false_v;
  result->upload_format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  result->alpha_mask = false_v;
  result->loaded_from_cache = false_v;
}

vkr_internal void
vkr_texture_decode_result_release(VkrTextureDecodeResult *result) {
  if (!result) {
    return;
  }
  if (result->decoded_pixels) {
    stbi_image_free(result->decoded_pixels);
    result->decoded_pixels = NULL;
  }
  if (result->upload_data) {
    free(result->upload_data);
    result->upload_data = NULL;
  }
  if (result->upload_regions) {
    free(result->upload_regions);
    result->upload_regions = NULL;
  }
  result->upload_data_size = 0;
  result->upload_region_count = 0;
}

vkr_internal bool8_t vkr_texture_ktx_metadata_bool(ktxTexture *texture,
                                                   const char *key,
                                                   bool8_t default_value) {
  if (!texture || !key) {
    return default_value;
  }

  unsigned int value_len = 0;
  void *value = NULL;
  if (ktxHashList_FindValue(&texture->kvDataHead, key, &value_len, &value) !=
          KTX_SUCCESS ||
      !value || value_len == 0) {
    return default_value;
  }

  const uint8_t first = ((const uint8_t *)value)[0];
  if (first == 1 || first == '1' || first == 't' || first == 'T' ||
      first == 'y' || first == 'Y') {
    return true_v;
  }
  if (first == 0 || first == '0' || first == 'f' || first == 'F' ||
      first == 'n' || first == 'N') {
    return false_v;
  }

  return default_value;
}

vkr_internal bool8_t vkr_texture_decode_from_ktx2(
    VkrAllocator *allocator, VkrTextureSystem *system, String8 vkt_path,
    VkrTextureColorSpace colorspace, VkrTextureDecodeResult *out_result) {
  if (!allocator || !system || !vkt_path.str || !out_result) {
    return false_v;
  }

  char *path_cstr = vkr_texture_path_to_cstr(allocator, vkt_path);
  if (!path_cstr) {
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  FilePath fp = file_path_create(path_cstr, allocator, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_err = file_read_all(&fh, allocator, &file_data, &file_size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !file_data || file_size == 0) {
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  ktxTexture2 *ktx_texture = NULL;
  ktxTexture *base_texture = NULL;
  uint8_t *upload_data = NULL;
  VkrTextureUploadRegion *upload_regions = NULL;
  bool8_t success = false_v;

  ktxResult ktx_result = ktxTexture2_CreateFromMemory(
      file_data, (ktx_size_t)file_size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
      &ktx_texture);
  if (ktx_result != KTX_SUCCESS || !ktx_texture) {
    log_error("Failed to parse KTX2 texture '%s': %s", path_cstr,
              ktxErrorString(ktx_result));
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  base_texture = ktxTexture(ktx_texture);
  if (base_texture->numDimensions != 2 || base_texture->isCubemap ||
      base_texture->numFaces != 1 || base_texture->numLayers != 1) {
    log_error("Unsupported KTX2 texture shape for '%s' (dims=%u layers=%u "
              "faces=%u cubemap=%u)",
              path_cstr, base_texture->numDimensions, base_texture->numLayers,
              base_texture->numFaces, base_texture->isCubemap);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }

  if (base_texture->baseWidth == 0 || base_texture->baseHeight == 0 ||
      base_texture->baseWidth > VKR_TEXTURE_MAX_DIMENSION ||
      base_texture->baseHeight > VKR_TEXTURE_MAX_DIMENSION) {
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }

  if (!ktxTexture2_NeedsTranscoding(ktx_texture)) {
    log_error("KTX2 texture '%s' does not require Basis transcoding; this "
              "runtime path currently expects UASTC/Basis payloads.",
              path_cstr);
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  const bool8_t request_srgb = (colorspace == VKR_TEXTURE_COLORSPACE_SRGB);
  const VkrTextureFormat target_format =
      vkr_texture_select_transcode_target_format(
          system->prefer_astc_transcode, request_srgb,
          system->supports_texture_astc_4x4, system->supports_texture_bc7);
  const ktx_transcode_fmt_e target_transcode_format =
      vkr_texture_ktx_transcode_format_from_texture_format(target_format);
  if (target_transcode_format == KTX_TTF_NOSELECTION) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  ktx_result =
      ktxTexture2_TranscodeBasis(ktx_texture, target_transcode_format, 0);
  if (ktx_result != KTX_SUCCESS) {
    log_error("Failed to transcode KTX2 texture '%s' to '%s': %s", path_cstr,
              ktxTranscodeFormatString(target_transcode_format),
              ktxErrorString(ktx_result));
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  uint8_t *ktx_data = ktxTexture_GetData(base_texture);
  ktx_size_t ktx_data_size = ktxTexture_GetDataSize(base_texture);
  if (!ktx_data || ktx_data_size == 0) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  const uint32_t region_count =
      base_texture->numLevels * base_texture->numLayers;
  if (region_count == 0) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  upload_data = (uint8_t *)malloc((size_t)ktx_data_size);
  upload_regions = (VkrTextureUploadRegion *)malloc(
      sizeof(VkrTextureUploadRegion) * region_count);
  if (!upload_data || !upload_regions) {
    out_result->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }

  MemCopy(upload_data, ktx_data, (size_t)ktx_data_size);
  uint32_t region_index = 0;
  for (uint32_t layer = 0; layer < base_texture->numLayers; ++layer) {
    for (uint32_t mip = 0; mip < base_texture->numLevels; ++mip) {
      ktx_size_t image_offset = 0;
      ktx_result =
          ktxTexture_GetImageOffset(base_texture, mip, layer, 0, &image_offset);
      if (ktx_result != KTX_SUCCESS || image_offset > ktx_data_size) {
        out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        goto cleanup;
      }

      const ktx_size_t image_size = ktxTexture_GetImageSize(base_texture, mip);
      if (image_offset + image_size > ktx_data_size) {
        out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        goto cleanup;
      }

      const uint32_t mip_width = Max(1u, base_texture->baseWidth >> mip);
      const uint32_t mip_height = Max(1u, base_texture->baseHeight >> mip);
      upload_regions[region_index++] = (VkrTextureUploadRegion){
          .mip_level = mip,
          .array_layer = layer,
          .width = mip_width,
          .height = mip_height,
          .depth = 1,
          .byte_offset = image_offset,
          .byte_size = image_size,
      };
    }
  }

  out_result->upload_data = upload_data;
  out_result->upload_data_size = ktx_data_size;
  out_result->upload_regions = upload_regions;
  out_result->upload_region_count = region_count;
  out_result->upload_mip_levels = base_texture->numLevels;
  out_result->upload_array_layers = base_texture->numLayers;
  out_result->upload_format = target_format;
  out_result->upload_is_compressed =
      vkr_texture_format_is_block_compressed(target_format);
  out_result->width = (int32_t)base_texture->baseWidth;
  out_result->height = (int32_t)base_texture->baseHeight;
  out_result->original_channels =
      (int32_t)vkr_texture_channel_count_from_format(target_format);
  out_result->has_transparency = vkr_texture_ktx_metadata_bool(
      base_texture, "vkr.has_transparency", false_v);
  out_result->alpha_mask =
      vkr_texture_ktx_metadata_bool(base_texture, "vkr.alpha_mask", false_v);
  out_result->success = true_v;
  success = true_v;

cleanup:
  if (ktx_texture) {
    ktxTexture2_Destroy(ktx_texture);
  }
  if (!success) {
    if (upload_data) {
      free(upload_data);
    }
    if (upload_regions) {
      free(upload_regions);
    }
  }
  return success;
}

/**
 * @brief Creates a temporary null-terminated copy of a String8 path.
 */
vkr_internal char *vkr_texture_path_to_cstr(VkrAllocator *allocator,
                                            String8 path) {
  if (!allocator || !path.str || path.length == 0) {
    return NULL;
  }

  char *path_cstr = vkr_allocator_alloc(allocator, path.length + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!path_cstr) {
    return NULL;
  }

  MemCopy(path_cstr, path.str, path.length);
  path_cstr[path.length] = '\0';
  return path_cstr;
}

/**
 * @brief Returns true when a path currently exists on disk.
 */
vkr_internal bool8_t vkr_texture_path_exists(VkrAllocator *allocator,
                                             String8 path) {
  char *path_cstr = vkr_texture_path_to_cstr(allocator, path);
  if (!path_cstr) {
    return false_v;
  }

  FilePath fp = file_path_create(path_cstr, allocator, FILE_PATH_TYPE_RELATIVE);
  return file_exists(&fp);
}

/**
 * @brief Probes the `.vkt` container type from the file signature.
 */
vkr_internal VkrTextureVktContainerType
vkr_texture_probe_vkt_container(VkrAllocator *allocator, String8 vkt_path) {
  char *path_cstr = vkr_texture_path_to_cstr(allocator, vkt_path);
  if (!path_cstr) {
    return VKR_TEXTURE_VKT_CONTAINER_UNKNOWN;
  }

  FilePath fp = file_path_create(path_cstr, allocator, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return VKR_TEXTURE_VKT_CONTAINER_UNKNOWN;
  }

  uint8_t *probe = NULL;
  uint64_t bytes_read = 0;
  const uint64_t probe_size = 16;
  FileError read_err =
      file_read(&fh, allocator, probe_size, &bytes_read, &probe);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE || !probe || bytes_read == 0) {
    return VKR_TEXTURE_VKT_CONTAINER_UNKNOWN;
  }

  return vkr_texture_detect_vkt_container(probe, bytes_read);
}

/**
 * @brief Populates result from a legacy `.vkt` cache file.
 *
 * For sidecar legacy caches, callers should pass source mtime validation.
 * Direct legacy `.vkt` requests can disable mtime validation to preserve
 * compatibility when source files are unavailable.
 */
vkr_internal bool8_t vkr_texture_try_read_legacy_cache(
    VkrAllocator *allocator, VkrTextureSystem *system, String8 cache_path,
    bool8_t validate_source_mtime, uint64_t source_mtime,
    const char *cache_guard_key, VkrTextureDecodeResult *out_result) {
  uint32_t cached_width = 0;
  uint32_t cached_height = 0;
  uint32_t cached_channels = 0;
  bool8_t cached_transparency = false_v;
  uint8_t *cached_pixels = NULL;

  if (!vkr_texture_cache_read(allocator, cache_path, validate_source_mtime,
                              source_mtime, &cached_width, &cached_height,
                              &cached_channels, &cached_transparency,
                              &cached_pixels)) {
    return false_v;
  }

  if (!cached_transparency && cached_channels == VKR_TEXTURE_RGBA_CHANNELS) {
    uint64_t pixel_count = (uint64_t)cached_width * (uint64_t)cached_height;
    if (vkr_texture_has_transparency(cached_pixels, pixel_count,
                                     cached_channels)) {
      cached_transparency = true_v;
      VkrTextureCacheWriteGuard *cache_guard =
          system ? system->cache_guard : NULL;
      bool8_t cache_lock_acquired = true_v;
      if (cache_guard && cache_guard_key) {
        cache_lock_acquired =
            vkr_texture_cache_guard_try_acquire(cache_guard, cache_guard_key);
      }
      if (cache_lock_acquired) {
        const uint64_t mtime_to_write =
            validate_source_mtime ? source_mtime : 0;
        vkr_texture_cache_write(allocator, cache_path, mtime_to_write,
                                cached_width, cached_height, cached_channels,
                                cached_transparency, cached_pixels);
        if (cache_guard && cache_guard_key) {
          vkr_texture_cache_guard_release(cache_guard, cache_guard_key);
        }
      }
    }
  }

  out_result->decoded_pixels = cached_pixels;
  out_result->width = (int32_t)cached_width;
  out_result->height = (int32_t)cached_height;
  out_result->original_channels = (int32_t)cached_channels;
  out_result->has_transparency = cached_transparency;
  out_result->alpha_mask = false_v;
  if (cached_channels == VKR_TEXTURE_RGBA_CHANNELS) {
    uint64_t pixel_count = (uint64_t)cached_width * (uint64_t)cached_height;
    out_result->alpha_mask =
        vkr_texture_analyze_alpha(cached_pixels, pixel_count, cached_channels)
            .alpha_mask;
  }
  out_result->loaded_from_cache = true_v;
  out_result->success = true_v;
  return true_v;
}

/**
 * @brief Decodes a source image file and optionally refreshes sidecar cache.
 */
vkr_internal bool8_t vkr_texture_decode_from_source_image(
    VkrAllocator *allocator, VkrTextureSystem *system, String8 source_path,
    bool8_t flip_vertical, String8 sidecar_cache_path,
    bool8_t allow_cache_write, const char *cache_guard_key,
    VkrTextureDecodeResult *out_result) {
  char *source_cstr = vkr_texture_path_to_cstr(allocator, source_path);
  if (!source_cstr) {
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  FilePath source_fp =
      file_path_create(source_cstr, allocator, FILE_PATH_TYPE_RELATIVE);
  FileStats source_stats = {0};
  if (file_stats(&source_fp, &source_stats) != FILE_ERROR_NONE) {
    log_error("Failed to stat texture file: %s", source_cstr);
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&source_fp, mode, &fh) != FILE_ERROR_NONE) {
    log_error("Failed to open texture file: %s", source_cstr);
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_err = file_read_all(&fh, allocator, &file_data, &file_size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !file_data || file_size == 0) {
    log_error("Failed to read texture file: %s", source_cstr);
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  stbi_set_flip_vertically_on_load_thread(flip_vertical ? 1 : 0);
  out_result->decoded_pixels = stbi_load_from_memory(
      file_data, (int)file_size, &out_result->width, &out_result->height,
      &out_result->original_channels, VKR_TEXTURE_RGBA_CHANNELS);
  if (!out_result->decoded_pixels) {
    const char *reason = stbi_failure_reason();
    log_error("Failed to decode texture '%s': %s", source_cstr,
              reason ? reason : "unknown");
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  if (out_result->width <= 0 || out_result->height <= 0 ||
      out_result->width > VKR_TEXTURE_MAX_DIMENSION ||
      out_result->height > VKR_TEXTURE_MAX_DIMENSION) {
    stbi_image_free(out_result->decoded_pixels);
    out_result->decoded_pixels = NULL;
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  const uint64_t pixel_count =
      (uint64_t)out_result->width * (uint64_t)out_result->height;
  VkrTextureAlphaAnalysis alpha = vkr_texture_analyze_alpha(
      out_result->decoded_pixels, pixel_count, VKR_TEXTURE_RGBA_CHANNELS);
  out_result->has_transparency = alpha.has_transparency;
  out_result->alpha_mask = alpha.alpha_mask;

  if (allow_cache_write && sidecar_cache_path.str) {
    VkrTextureCacheWriteGuard *cache_guard =
        system ? system->cache_guard : NULL;
    bool8_t cache_lock_acquired = true_v;
    if (cache_guard && cache_guard_key) {
      cache_lock_acquired =
          vkr_texture_cache_guard_try_acquire(cache_guard, cache_guard_key);
    }
    if (cache_lock_acquired) {
      vkr_texture_cache_write(
          allocator, sidecar_cache_path, source_stats.last_modified,
          (uint32_t)out_result->width, (uint32_t)out_result->height,
          VKR_TEXTURE_RGBA_CHANNELS, out_result->has_transparency,
          out_result->decoded_pixels);
      if (cache_guard && cache_guard_key) {
        vkr_texture_cache_guard_release(cache_guard, cache_guard_key);
      }
    }
  }

  out_result->success = true_v;
  return true_v;
}

/**
 * @brief Runs the texture decoding job
 * @param ctx The job context
 * @param payload The payload for the job
 * @return True if the job ran successfully, false otherwise
 */
vkr_internal bool8_t vkr_texture_decode_job_run(VkrJobContext *ctx,
                                                void *payload) {
  assert_log(ctx != NULL, "Job context is NULL");
  assert_log(payload != NULL, "Payload is NULL");

  VkrTextureDecodeJobPayload *job = (VkrTextureDecodeJobPayload *)payload;
  VkrTextureDecodeResult *result = job->result;
  VkrAllocator *scratch_allocator = ctx->allocator;
  assert_log(scratch_allocator != NULL, "Job allocator is NULL");

  vkr_texture_decode_result_reset(result);
  String8 direct_vkt = {0};
  String8 sidecar_vkt = {0};
  String8 source_path = {0};
  vkr_texture_build_resolution_candidates(scratch_allocator, job->file_path,
                                          &direct_vkt, &sidecar_vkt,
                                          &source_path);

  const bool8_t has_direct_vkt =
      direct_vkt.str && vkr_texture_path_exists(scratch_allocator, direct_vkt);
  const bool8_t has_sidecar_vkt =
      sidecar_vkt.str &&
      vkr_texture_path_exists(scratch_allocator, sidecar_vkt);

  String8 selected_vkt = {0};
  bool8_t selected_is_direct = false_v;
  if (has_direct_vkt) {
    selected_vkt = direct_vkt;
    selected_is_direct = true_v;
  } else if (has_sidecar_vkt) {
    selected_vkt = sidecar_vkt;
  }

  char *source_cstr = vkr_texture_path_to_cstr(scratch_allocator, source_path);
  char *selected_vkt_cstr =
      selected_vkt.str
          ? vkr_texture_path_to_cstr(scratch_allocator, selected_vkt)
          : NULL;
  const bool8_t strict_vkt_only =
      job->system ? job->system->strict_vkt_only_mode : false_v;
  const bool8_t allow_legacy_vkt =
      job->system ? job->system->allow_legacy_vkt : true_v;
  const bool8_t allow_source_fallback =
      job->system ? job->system->allow_source_fallback : true_v;
  bool8_t allow_sidecar_cache_write =
      (job->system && job->system->allow_legacy_cache_write) ? true_v : false_v;

  if (selected_vkt.str) {
    VkrTextureVktContainerType container =
        vkr_texture_probe_vkt_container(scratch_allocator, selected_vkt);

    switch (container) {
    case VKR_TEXTURE_VKT_CONTAINER_LEGACY_RAW: {
      if (!allow_legacy_vkt) {
        if (selected_is_direct || !allow_source_fallback) {
          log_error("Legacy `.vkt` support is disabled for '%s'",
                    selected_vkt_cstr ? selected_vkt_cstr : "");
          result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
          return false_v;
        }
        log_warn("Ignoring legacy sidecar `.vkt` for '%s' because legacy "
                 "support is disabled. Falling back to source image decode.",
                 source_cstr ? source_cstr : "");
        allow_sidecar_cache_write = false_v;
        break;
      }

      vkr_local_persist bool8_t warned_legacy = false_v;
      if (!warned_legacy) {
        log_warn("Legacy raw `.vkt` cache detected. Migrate to KTX2/UASTC "
                 "assets.");
        warned_legacy = true_v;
      }

      bool8_t validate_source_mtime = false_v;
      uint64_t source_mtime = 0;
      if (!selected_is_direct && source_cstr) {
        FilePath source_fp = file_path_create(source_cstr, scratch_allocator,
                                              FILE_PATH_TYPE_RELATIVE);
        FileStats source_stats = {0};
        if (file_stats(&source_fp, &source_stats) == FILE_ERROR_NONE) {
          validate_source_mtime = true_v;
          source_mtime = source_stats.last_modified;
        }
      }

      const char *cache_guard_key =
          source_cstr ? source_cstr : selected_vkt_cstr;
      if (vkr_texture_try_read_legacy_cache(
              scratch_allocator, job->system, selected_vkt,
              validate_source_mtime, source_mtime, cache_guard_key, result)) {
        return true_v;
      }

      if (selected_is_direct || !allow_source_fallback) {
        log_error("Failed to read legacy `.vkt` file: %s",
                  selected_vkt_cstr ? selected_vkt_cstr : "");
        result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      break;
    }

    case VKR_TEXTURE_VKT_CONTAINER_KTX2:
      if (vkr_texture_decode_from_ktx2(scratch_allocator, job->system,
                                       selected_vkt, job->colorspace, result)) {
        return true_v;
      }
      if (selected_is_direct) {
        log_error("Failed to decode KTX2 `.vkt` texture '%s'",
                  selected_vkt_cstr ? selected_vkt_cstr : "");
        result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      if (!allow_source_fallback || strict_vkt_only) {
        log_error("Failed to decode sidecar `.vkt` texture '%s' and source "
                  "fallback is disabled",
                  selected_vkt_cstr ? selected_vkt_cstr : "");
        result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      log_warn("Failed to decode KTX2 sidecar `.vkt` for '%s'. Falling back "
               "to source image decode.",
               source_cstr ? source_cstr : "");
      allow_sidecar_cache_write = false_v;
      break;

    case VKR_TEXTURE_VKT_CONTAINER_UNKNOWN:
    default:
      if (selected_is_direct) {
        log_error("Unsupported `.vkt` container for '%s'",
                  selected_vkt_cstr ? selected_vkt_cstr : "");
        result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      if (!allow_source_fallback || strict_vkt_only) {
        log_error("Unsupported sidecar `.vkt` container for '%s' and source "
                  "fallback is disabled",
                  selected_vkt_cstr ? selected_vkt_cstr : "");
        result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      log_warn("Unknown sidecar `.vkt` format for '%s'. Falling back to "
               "source image decode.",
               source_cstr ? source_cstr : "");
      allow_sidecar_cache_write = false_v;
      break;
    }
  }

  if (!selected_vkt.str && !allow_source_fallback) {
    log_error("Texture request '%s' has no `.vkt` asset and source fallback "
              "is disabled",
              source_cstr ? source_cstr : "");
    result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  if (!selected_vkt.str && allow_source_fallback) {
    vkr_local_persist bool8_t warned_source_fallback = false_v;
    if (!warned_source_fallback) {
      log_warn("Source-image fallback is enabled. Missing `.vkt` files will "
               "still load from authoring textures. Set "
               "`VKR_TEXTURE_VKT_STRICT=1` to enforce `.vkt`-only runtime.");
      warned_source_fallback = true_v;
    }
  }

  if (!source_path.str ||
      !vkr_texture_path_exists(scratch_allocator, source_path)) {
    result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  const String8 sidecar_path_for_write =
      sidecar_vkt.str ? sidecar_vkt : (String8){0};
  return vkr_texture_decode_from_source_image(
      scratch_allocator, job->system, source_path, job->flip_vertical,
      sidecar_path_for_write, allow_sidecar_cache_write, source_cstr, result);
}

VkrRendererError vkr_texture_system_load_from_file(VkrTextureSystem *system,
                                                   String8 file_path,
                                                   uint32_t desired_channels,
                                                   VkrTexture *out_texture) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");
  assert_log(file_path.str != NULL, "Path is NULL");

  VkrTextureRequest request = vkr_texture_parse_request(file_path);
  String8 base_path = request.base_path;
  out_texture->file_path = (FilePath){0};

  VkrTextureDecodeResult decode_result = {0};
  vkr_texture_decode_result_reset(&decode_result);

  VkrTextureDecodeJobPayload job_payload = {
      .file_path = base_path,
      .desired_channels = desired_channels,
      .flip_vertical = true_v,
      .colorspace = request.colorspace,
      .system = system,
      .result = &decode_result,
  };

  if (system->job_system) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);

    VkrJobDesc job_desc = {
        .priority = VKR_JOB_PRIORITY_NORMAL,
        .type_mask = type_mask,
        .run = vkr_texture_decode_job_run,
        .on_success = NULL,
        .on_failure = NULL,
        .payload = &job_payload,
        .payload_size = sizeof(job_payload),
        .dependencies = NULL,
        .dependency_count = 0,
        .defer_enqueue = false_v,
    };

    VkrJobHandle job_handle = {0};
    if (vkr_job_submit(system->job_system, &job_desc, &job_handle)) {
      vkr_job_wait(system->job_system, job_handle);
    }
  } else {
    // Fallback: run synchronously using a fake context
    VkrAllocatorScope sync_scope =
        vkr_allocator_begin_scope(&system->allocator);
    VkrJobContext fake_ctx = {.system = NULL,
                              .worker_index = 0,
                              .thread_id = 0,
                              .allocator = &system->allocator,
                              .scope = sync_scope};
    vkr_texture_decode_job_run(&fake_ctx, &job_payload);
    vkr_allocator_end_scope(&sync_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }

  const bool8_t has_upload_payload = decode_result.upload_data &&
                                     decode_result.upload_regions &&
                                     decode_result.upload_region_count > 0;
  if (!decode_result.success ||
      (!decode_result.decoded_pixels && !has_upload_payload)) {
    VkrRendererError decode_error = decode_result.error;
    vkr_texture_decode_result_release(&decode_result);
    return decode_error;
  }

  int32_t width = decode_result.width;
  int32_t height = decode_result.height;
  int32_t original_channels = decode_result.original_channels;

  uint32_t actual_channels = 0;
  VkrTextureFormat format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  if (has_upload_payload) {
    format = decode_result.upload_format;
    actual_channels = vkr_texture_channel_count_from_format(format);
  } else {
    actual_channels =
        desired_channels > 0 ? desired_channels : (uint32_t)original_channels;

    switch (actual_channels) {
    case VKR_TEXTURE_R_CHANNELS:
    case VKR_TEXTURE_RG_CHANNELS:
    case VKR_TEXTURE_RGB_CHANNELS:
    case VKR_TEXTURE_RGBA_CHANNELS:
      break;
    default:
      actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
      break;
    }
    if (actual_channels == VKR_TEXTURE_RGB_CHANNELS) {
      actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    }

    format =
        vkr_texture_format_from_channels(actual_channels, request.colorspace);
  }

  VkrTexturePropertyFlags props = vkr_texture_property_flags_create();
  if (decode_result.has_transparency) {
    bitset8_set(&props, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
    if (decode_result.alpha_mask) {
      bitset8_set(&props, VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT);
    }
  }

  out_texture->description = (VkrTextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = actual_channels,
      .format = format,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = props,
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = (has_upload_payload && decode_result.upload_mip_levels <= 1)
                        ? VKR_MIP_FILTER_NONE
                        : VKR_MIP_FILTER_LINEAR,
      .anisotropy_enable = false_v,
      // Must be stable for the lifetime of this backend texture handle so
      // descriptor-set generation tracking can invalidate correctly on reload.
      .generation = system->generation_counter++,
  };

  if (has_upload_payload) {
    VkrTextureUploadPayload upload_payload = {
        .data = decode_result.upload_data,
        .data_size = decode_result.upload_data_size,
        .mip_levels = decode_result.upload_mip_levels,
        .array_layers = decode_result.upload_array_layers,
        .is_compressed = decode_result.upload_is_compressed,
        .region_count = decode_result.upload_region_count,
        .regions = decode_result.upload_regions,
    };
    VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
    out_texture->handle = vkr_renderer_create_texture_with_payload(
        system->renderer, &out_texture->description, &upload_payload,
        &renderer_error);
    vkr_texture_decode_result_release(&decode_result);
    out_texture->image = NULL;
    return renderer_error;
  }

  uint8_t *loaded_image_data = decode_result.decoded_pixels;

  uint32_t loaded_channels =
      desired_channels > 0 ? desired_channels : (uint32_t)original_channels;
  uint64_t loaded_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)loaded_channels;
  uint64_t final_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)actual_channels;

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    vkr_texture_decode_result_release(&decode_result);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  out_texture->image = vkr_allocator_alloc(&system->allocator, final_image_size,
                                           VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!out_texture->image) {
    vkr_texture_decode_result_release(&decode_result);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  if (loaded_channels == VKR_TEXTURE_RGB_CHANNELS &&
      actual_channels == VKR_TEXTURE_RGBA_CHANNELS) {
    for (uint32_t pixel_index = 0;
         pixel_index < (uint32_t)width * (uint32_t)height; pixel_index++) {
      uint32_t src_idx = pixel_index * VKR_TEXTURE_RGB_CHANNELS;
      uint32_t dst_idx = pixel_index * VKR_TEXTURE_RGBA_CHANNELS;
      out_texture->image[dst_idx + 0] = loaded_image_data[src_idx + 0];
      out_texture->image[dst_idx + 1] = loaded_image_data[src_idx + 1];
      out_texture->image[dst_idx + 2] = loaded_image_data[src_idx + 2];
      out_texture->image[dst_idx + 3] = 255;
    }
  } else {
    MemCopy(out_texture->image, loaded_image_data, (size_t)loaded_image_size);
  }

  vkr_texture_decode_result_release(&decode_result);

  // GPU upload happens on calling thread (synchronized)
  VkrTextureUploadRegion upload_region = {
      .mip_level = 0,
      .array_layer = 0,
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .depth = 1,
      .byte_offset = 0,
      .byte_size = final_image_size,
  };
  VkrTextureUploadPayload upload_payload = {
      .data = out_texture->image,
      .data_size = final_image_size,
      .mip_levels = 1,
      .array_layers = 1,
      .is_compressed = false_v,
      .region_count = 1,
      .regions = &upload_region,
  };
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  out_texture->handle = vkr_renderer_create_texture_with_payload(
      system->renderer, &out_texture->description, &upload_payload,
      &renderer_error);

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  out_texture->image = NULL;
  return renderer_error;
}

bool8_t vkr_texture_system_load(VkrTextureSystem *system, String8 name,
                                VkrTextureHandle *out_handle,
                                VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTexture loaded_texture = {0};
  *out_error = vkr_texture_system_load_from_file(
      system, name, VKR_TEXTURE_RGBA_CHANNELS, &loaded_texture);
  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }

  // Find free slot in system
  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    if (loaded_texture.handle) {
      vkr_renderer_destroy_texture(system->renderer, loaded_texture.handle);
    }
    return false_v;
  }

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->string_allocator, name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for texture map");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    if (loaded_texture.handle) {
      vkr_renderer_destroy_texture(system->renderer, loaded_texture.handle);
    }
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  // Copy texture data to system
  VkrTexture *texture = &system->textures.data[free_slot_index];
  *texture = loaded_texture;

  // Assign stable id and generation
  texture->description.id = free_slot_index + 1;
  if (texture->description.generation == VKR_INVALID_ID) {
    texture->description.generation = system->generation_counter++;
  }

  // Add to hash table with 0 ref count
  VkrTextureEntry new_entry = {
      .index = free_slot_index,
      .ref_count = 0,
      .auto_release = true_v,
      .name = stable_key,
  };
  if (!vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                             new_entry)) {
    log_error("Failed to insert texture '%s' into hash table", stable_key);
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    vkr_renderer_destroy_texture(system->renderer, texture->handle);
    MemZero(texture, sizeof(*texture));
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrTextureHandle handle = {.id = texture->description.id,
                             .generation = texture->description.generation};

  *out_handle = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

uint32_t vkr_texture_system_load_batch(VkrTextureSystem *system,
                                       const String8 *paths, uint32_t count,
                                       VkrTextureHandle *out_handles,
                                       VkrRendererError *out_errors) {
  assert_log(system != NULL, "System is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  // Initialize outputs
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i] = VKR_TEXTURE_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  // Allocate scratch for deduplication mapping
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }
  uint32_t *first_occurrence =
      vkr_allocator_alloc(&system->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureRequest *requests =
      vkr_allocator_alloc(&system->allocator, sizeof(VkrTextureRequest) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!first_occurrence || !requests) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // DEDUPLICATION: First check which textures are already loaded
  // and which need to be loaded. Also track duplicates within the batch.
  uint32_t already_loaded = 0;
  uint32_t unique_in_batch = 0;
  for (uint32_t i = 0; i < count; i++) {
    first_occurrence[i] = i; // Default: each is its own first occurrence

    if (!paths[i].str || paths[i].length == 0) {
      requests[i] = (VkrTextureRequest){0};
      continue;
    }

    requests[i] = vkr_texture_parse_request(paths[i]);

    // Check if this texture is already loaded in the system
    const char *texture_key = (const char *)paths[i].str;
    VkrTextureEntry *entry =
        vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
    if (entry) {
      // Already loaded - just return the existing handle
      VkrTexture *texture = &system->textures.data[entry->index];
      out_handles[i] =
          (VkrTextureHandle){.id = texture->description.id,
                             .generation = texture->description.generation};
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
      already_loaded++;
      continue;
    }

    // Check for duplicate within the batch (earlier occurrence)
    bool8_t is_duplicate = false_v;
    for (uint32_t j = 0; j < i; j++) {
      if (!paths[j].str || paths[j].length == 0) {
        continue;
      }
      if (string8_equalsi(&paths[i], &paths[j])) {
        first_occurrence[i] = first_occurrence[j];
        is_duplicate = true_v;
        break;
      }
    }
    if (!is_duplicate) {
      unique_in_batch++;
    }
  }

  // Count how many textures still need loading (only unique ones)
  uint32_t need_loading = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (paths[i].str && paths[i].length > 0 && out_handles[i].id == 0 &&
        first_occurrence[i] == i) {
      need_loading++;
    }
  }

  log_debug(
      "Texture batch: %u paths, %u already loaded, %u unique need loading",
      count, already_loaded, need_loading);

  if (need_loading == 0) {
    // Copy handles from first occurrence to duplicates
    for (uint32_t i = 0; i < count; i++) {
      if (first_occurrence[i] != i &&
          out_handles[first_occurrence[i]].id != 0) {
        out_handles[i] = out_handles[first_occurrence[i]];
      }
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return already_loaded;
  }

  // If no job system, fall back to sequential loading
  if (!system->job_system) {
    uint32_t loaded = already_loaded;
    for (uint32_t i = 0; i < count; i++) {
      if (!paths[i].str || paths[i].length == 0) {
        continue;
      }
      if (out_handles[i].id != 0) {
        continue; // Already loaded above
      }
      // Skip duplicates within batch
      if (first_occurrence[i] != i) {
        continue;
      }
      if (vkr_texture_system_load(system, paths[i], &out_handles[i],
                                  &out_errors[i])) {
        loaded++;
      }
    }
    // Copy handles to duplicates
    for (uint32_t i = 0; i < count; i++) {
      if (first_occurrence[i] != i &&
          out_handles[first_occurrence[i]].id != 0) {
        out_handles[i] = out_handles[first_occurrence[i]];
      }
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return loaded;
  }

  // Allocate arrays for job handles and results (temp scope)
  VkrAllocatorScope decode_scope =
      vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&decode_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }
  VkrJobHandle *job_handles =
      vkr_allocator_alloc(&system->allocator, sizeof(VkrJobHandle) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureDecodeResult *results = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureDecodeResult) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureDecodeJobPayload *payloads = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureDecodeJobPayload) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *job_submitted =
      vkr_allocator_alloc(&system->allocator, sizeof(bool8_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!job_handles || !results || !payloads || !job_submitted) {
    vkr_allocator_end_scope(&decode_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    // Fall back to sequential
    uint32_t loaded = already_loaded;
    for (uint32_t i = 0; i < count; i++) {
      if (!paths[i].str || paths[i].length == 0) {
        continue;
      }

      if (out_handles[i].id != 0) {
        continue; // Already loaded above
      }

      if (first_occurrence[i] != i) {
        continue;
      }

      if (vkr_texture_system_load(system, paths[i], &out_handles[i],
                                  &out_errors[i])) {
        loaded++;
      }
    }

    for (uint32_t i = 0; i < count; i++) {
      if (first_occurrence[i] != i &&
          out_handles[first_occurrence[i]].id != 0) {
        out_handles[i] = out_handles[first_occurrence[i]];
      }
    }

    return loaded;
  }

  // Initialize and submit decode jobs ONLY for unique textures not yet loaded
  Bitset8 type_mask = bitset8_create();
  bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);

  for (uint32_t i = 0; i < count; i++) {
    job_submitted[i] = false_v;
    results[i] = (VkrTextureDecodeResult){0};
    vkr_texture_decode_result_reset(&results[i]);

    if (!paths[i].str || paths[i].length == 0) {
      continue;
    }

    // Skip textures that were already loaded (deduplication above)
    if (out_handles[i].id != 0) {
      continue;
    }

    // Skip duplicates within batch - only decode first occurrence
    if (first_occurrence[i] != i) {
      continue;
    }

    payloads[i] = (VkrTextureDecodeJobPayload){
        .file_path = requests[i].base_path,
        .desired_channels = VKR_TEXTURE_RGBA_CHANNELS,
        .flip_vertical = true_v,
        .colorspace = requests[i].colorspace,
        .system = system,
        .result = &results[i],
    };

    VkrJobDesc job_desc = {
        .priority = VKR_JOB_PRIORITY_NORMAL,
        .type_mask = type_mask,
        .run = vkr_texture_decode_job_run,
        .on_success = NULL,
        .on_failure = NULL,
        .payload = &payloads[i],
        .payload_size = sizeof(VkrTextureDecodeJobPayload),
        .dependencies = NULL,
        .dependency_count = 0,
        .defer_enqueue = false_v,
    };

    if (vkr_job_submit(system->job_system, &job_desc, &job_handles[i])) {
      job_submitted[i] = true_v;
    }
  }

  // Wait for all jobs to complete
  for (uint32_t i = 0; i < count; i++) {
    if (job_submitted[i]) {
      vkr_job_wait(system->job_system, job_handles[i]);
    }
  }

  // Build one backend batch of texture uploads from decode results.
  VkrTextureDescription *batch_descriptions = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureDescription) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureUploadPayload *batch_payloads = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureUploadPayload) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureUploadRegion *batch_single_regions = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureUploadRegion) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureBatchCreateRequest *batch_requests = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureBatchCreateRequest) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *batch_source_indices =
      vkr_allocator_alloc(&system->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrTextureOpaqueHandle *batch_gpu_handles = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrTextureOpaqueHandle) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *batch_gpu_errors = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrRendererError) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!batch_descriptions || !batch_payloads || !batch_single_regions ||
      !batch_requests || !batch_source_indices || !batch_gpu_handles ||
      !batch_gpu_errors) {
    for (uint32_t i = 0; i < count; ++i) {
      if (results[i].decoded_pixels || results[i].upload_data) {
        vkr_texture_decode_result_release(&results[i]);
      }
      if (out_errors[i] == VKR_RENDERER_ERROR_UNKNOWN) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    vkr_allocator_end_scope(&decode_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return already_loaded;
  }

  uint32_t batch_count = 0;
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!paths[i].str || paths[i].length == 0) {
      continue;
    }

    const bool8_t has_upload_payload = results[i].upload_data &&
                                       results[i].upload_regions &&
                                       results[i].upload_region_count > 0;
    if (!results[i].success ||
        (!results[i].decoded_pixels && !has_upload_payload)) {
      out_errors[i] = results[i].error;
      vkr_texture_decode_result_release(&results[i]);
      continue;
    }

    const char *check_key = (const char *)paths[i].str;
    VkrTextureEntry *existing =
        vkr_hash_table_get_VkrTextureEntry(&system->texture_map, check_key);
    if (existing) {
      VkrTexture *tex = &system->textures.data[existing->index];
      out_handles[i] = (VkrTextureHandle){
          .id = tex->description.id, .generation = tex->description.generation};
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
      loaded++;
      vkr_texture_decode_result_release(&results[i]);
      continue;
    }

    uint32_t actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    VkrTextureFormat format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
    if (has_upload_payload) {
      format = results[i].upload_format;
      actual_channels = vkr_texture_channel_count_from_format(format);
    } else {
      format = vkr_texture_format_from_channels(actual_channels,
                                                requests[i].colorspace);
    }
    bool8_t has_transparency = results[i].has_transparency;

    VkrTexturePropertyFlags props = vkr_texture_property_flags_create();
    if (has_transparency) {
      bitset8_set(&props, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
      if (results[i].alpha_mask) {
        bitset8_set(&props, VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT);
      }
    }

    VkrTextureDescription *desc = &batch_descriptions[batch_count];
    *desc = (VkrTextureDescription){
        .width = (uint32_t)results[i].width,
        .height = (uint32_t)results[i].height,
        .channels = actual_channels,
        .format = format,
        .type = VKR_TEXTURE_TYPE_2D,
        .properties = props,
        .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
        .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
        .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
        .min_filter = VKR_FILTER_LINEAR,
        .mag_filter = VKR_FILTER_LINEAR,
        .mip_filter = (has_upload_payload && results[i].upload_mip_levels <= 1)
                          ? VKR_MIP_FILTER_NONE
                          : VKR_MIP_FILTER_LINEAR,
        .anisotropy_enable = false_v,
        .generation = VKR_INVALID_ID,
    };

    VkrTextureUploadPayload *upload_payload = &batch_payloads[batch_count];
    if (has_upload_payload) {
      *upload_payload = (VkrTextureUploadPayload){
          .data = results[i].upload_data,
          .data_size = results[i].upload_data_size,
          .mip_levels = results[i].upload_mip_levels,
          .array_layers = results[i].upload_array_layers,
          .is_compressed = results[i].upload_is_compressed,
          .region_count = results[i].upload_region_count,
          .regions = results[i].upload_regions,
      };
    } else {
      uint64_t payload_size =
          (uint64_t)results[i].width * (uint64_t)results[i].height *
          (uint64_t)actual_channels;
      batch_single_regions[batch_count] = (VkrTextureUploadRegion){
          .mip_level = 0,
          .array_layer = 0,
          .width = (uint32_t)results[i].width,
          .height = (uint32_t)results[i].height,
          .depth = 1,
          .byte_offset = 0,
          .byte_size = payload_size,
      };
      *upload_payload = (VkrTextureUploadPayload){
          .data = results[i].decoded_pixels,
          .data_size = payload_size,
          .mip_levels = 1,
          .array_layers = 1,
          .is_compressed = false_v,
          .region_count = 1,
          .regions = &batch_single_regions[batch_count],
      };
    }

    batch_requests[batch_count] = (VkrTextureBatchCreateRequest){
        .description = desc,
        .payload = upload_payload,
    };
    batch_source_indices[batch_count] = i;
    batch_count++;
  }

  if (batch_count > 0) {
    vkr_renderer_create_texture_with_payload_batch(
        system->renderer, batch_requests, batch_count, batch_gpu_handles,
        batch_gpu_errors);
  }

  for (uint32_t batch_index = 0; batch_index < batch_count; ++batch_index) {
    const uint32_t source_index = batch_source_indices[batch_index];
    const char *key = (const char *)paths[source_index].str;
    VkrTextureOpaqueHandle gpu_handle = batch_gpu_handles[batch_index];
    VkrRendererError create_error = batch_gpu_errors[batch_index];

    if (create_error != VKR_RENDERER_ERROR_NONE || !gpu_handle) {
      out_errors[source_index] = (create_error == VKR_RENDERER_ERROR_UNKNOWN)
                                     ? VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED
                                     : create_error;
      vkr_texture_decode_result_release(&results[source_index]);
      continue;
    }

    VkrTextureEntry *existing =
        vkr_hash_table_get_VkrTextureEntry(&system->texture_map, key);
    if (existing) {
      VkrTexture *tex = &system->textures.data[existing->index];
      out_handles[source_index] = (VkrTextureHandle){
          .id = tex->description.id, .generation = tex->description.generation};
      out_errors[source_index] = VKR_RENDERER_ERROR_NONE;
      loaded++;
      vkr_renderer_destroy_texture(system->renderer, gpu_handle);
      vkr_texture_decode_result_release(&results[source_index]);
      continue;
    }

    uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
    if (free_slot_index == VKR_INVALID_ID) {
      out_errors[source_index] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      vkr_renderer_destroy_texture(system->renderer, gpu_handle);
      vkr_texture_decode_result_release(&results[source_index]);
      continue;
    }

    char *stable_key = (char *)vkr_allocator_alloc(
        &system->string_allocator, paths[source_index].length + 1,
        VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!stable_key) {
      out_errors[source_index] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      vkr_renderer_destroy_texture(system->renderer, gpu_handle);
      vkr_texture_decode_result_release(&results[source_index]);
      continue;
    }
    MemCopy(stable_key, paths[source_index].str,
            (size_t)paths[source_index].length);
    stable_key[paths[source_index].length] = '\0';

    VkrTexture *texture = &system->textures.data[free_slot_index];
    MemZero(texture, sizeof(VkrTexture));
    texture->description = batch_descriptions[batch_index];
    texture->description.id = free_slot_index + 1;
    texture->description.generation = system->generation_counter++;
    texture->handle = gpu_handle;

    VkrTextureEntry new_entry = {
        .index = free_slot_index,
        .ref_count = 0,
        .auto_release = true_v,
        .name = stable_key,
    };
    if (!vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                               new_entry)) {
      out_errors[source_index] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      vkr_allocator_free(&system->string_allocator, stable_key,
                         paths[source_index].length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
      vkr_renderer_destroy_texture(system->renderer, gpu_handle);
      MemZero(texture, sizeof(*texture));
      vkr_texture_decode_result_release(&results[source_index]);
      continue;
    }

    out_handles[source_index] = (VkrTextureHandle){
        .id = texture->description.id,
        .generation = texture->description.generation,
    };
    out_errors[source_index] = VKR_RENDERER_ERROR_NONE;
    loaded++;
    vkr_texture_decode_result_release(&results[source_index]);
  }

  // Copy handles from first occurrence to all duplicates
  for (uint32_t i = 0; i < count; i++) {
    if (first_occurrence[i] != i) {
      uint32_t first = first_occurrence[i];
      if (out_handles[first].id != 0) {
        out_handles[i] = out_handles[first];
        out_errors[i] = VKR_RENDERER_ERROR_NONE;
      }
    }
  }

  vkr_allocator_end_scope(&decode_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return loaded + already_loaded;
}

// Helper to load a single cube face from memory
vkr_internal uint8_t *vkr_texture_load_cube_face(VkrAllocator *allocator,
                                                 const char *path,
                                                 int32_t *out_width,
                                                 int32_t *out_height) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(path != NULL, "Path is NULL");
  assert_log(out_width != NULL, "Out width is NULL");
  assert_log(out_height != NULL, "Out height is NULL");

  FilePath fp = file_path_create(path, allocator, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    return NULL;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_err = file_read_all(&fh, allocator, &file_data, &file_size);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE || !file_data || file_size == 0) {
    return NULL;
  }

  // Cube maps don't flip vertically
  stbi_set_flip_vertically_on_load_thread(0);

  int32_t channels = 0;
  uint8_t *pixels = stbi_load_from_memory(file_data, (int)file_size, out_width,
                                          out_height, &channels, 4);
  return pixels;
}

bool8_t vkr_texture_system_load_cube_map(VkrTextureSystem *system,
                                         String8 base_path, String8 extension,
                                         VkrTextureHandle *out_handle,
                                         VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(base_path.str != NULL, "Base path is NULL");
  assert_log(extension.str != NULL, "Extension is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // Face suffixes: +X, -X, +Y, -Y, +Z, -Z -> r, l, u, d, f, b
  static const char *face_suffixes[6] = {"_r", "_l", "_u", "_d", "_f", "_b"};

  VkrAllocator *temp_alloc = &system->allocator;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Build full path for first face to get dimensions
  uint64_t path_buffer_size = base_path.length + 16 + extension.length;
  char *path_buffer = (char *)vkr_allocator_alloc(
      temp_alloc, path_buffer_size, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!path_buffer) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Load first face to get dimensions
  snprintf(path_buffer, path_buffer_size, "%.*s%s.%.*s", (int)base_path.length,
           base_path.str, face_suffixes[0], (int)extension.length,
           extension.str);

  int32_t width = 0, height = 0;
  uint8_t *first_face =
      vkr_texture_load_cube_face(temp_alloc, path_buffer, &width, &height);
  if (!first_face) {
    log_error("Failed to load cube map face 0: %s", path_buffer);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  if (width <= 0 || height <= 0 || width != height ||
      width > VKR_TEXTURE_MAX_DIMENSION) {
    log_error("Cube map faces must be square and within max dimension: %dx%d",
              width, height);
    stbi_image_free(first_face);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  uint64_t face_size = (uint64_t)width * (uint64_t)height * 4;
  uint64_t total_size = face_size * 6;

  // Allocate buffer for all 6 faces
  uint8_t *cube_data = (uint8_t *)vkr_allocator_alloc(
      temp_alloc, total_size, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!cube_data) {
    stbi_image_free(first_face);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Copy first face
  MemCopy(cube_data, first_face, face_size);
  stbi_image_free(first_face);

  // Load remaining 5 faces
  for (uint32_t face = 1; face < 6; face++) {
    snprintf(path_buffer, path_buffer_size, "%.*s%s.%.*s",
             (int)base_path.length, base_path.str, face_suffixes[face],
             (int)extension.length, extension.str);

    int32_t face_width = 0, face_height = 0;
    uint8_t *face_data = vkr_texture_load_cube_face(temp_alloc, path_buffer,
                                                    &face_width, &face_height);
    if (!face_data) {
      log_error("Failed to load cube map face %u: %s", face, path_buffer);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }

    if (face_width != width || face_height != height) {
      log_error("Cube map face %u has different dimensions: %dx%d vs %dx%d",
                face, face_width, face_height, width, height);
      stbi_image_free(face_data);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return false_v;
    }

    MemCopy(cube_data + face * face_size, face_data, face_size);
    stbi_image_free(face_data);
  }

  // Create texture description for cube map
  VkrTextureDescription desc = {
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_CUBE_MAP,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  // Create the cube map texture via renderer
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrTextureOpaqueHandle backend_handle = vkr_renderer_create_texture(
      system->renderer, &desc, cube_data, &renderer_error);

  if (renderer_error != VKR_RENDERER_ERROR_NONE || !backend_handle) {
    log_error("Failed to create cube map texture in backend");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = renderer_error;
    return false_v;
  }

  // Find free slot in system
  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full");
    vkr_renderer_destroy_texture(system->renderer, backend_handle);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Create stable key for the cube map
  char *stable_key = (char *)vkr_allocator_alloc(
      &system->string_allocator, base_path.length + 16,
      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    vkr_renderer_destroy_texture(system->renderer, backend_handle);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  snprintf(stable_key, base_path.length + 16, "%.*s_cube",
           (int)base_path.length, base_path.str);

  // Store texture in system
  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(VkrTexture));
  texture->description = desc;
  texture->description.id = free_slot_index + 1;
  texture->description.generation = system->generation_counter++;
  texture->handle = backend_handle;
  texture->image = NULL; // Data already uploaded

  // Add to hash table
  VkrTextureEntry new_entry = {
      .index = free_slot_index,
      .ref_count = 1,
      .auto_release = false_v,
      .name = stable_key,
  };
  bool8_t insert_success = vkr_hash_table_insert_VkrTextureEntry(
      &system->texture_map, stable_key, new_entry);
  if (!insert_success) {
    log_error("Failed to insert cube map '%s' into hash table", stable_key);
    vkr_allocator_free(&system->string_allocator, stable_key,
                       base_path.length + 16, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    vkr_renderer_destroy_texture(system->renderer, backend_handle);
    texture->description.generation = VKR_INVALID_ID;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  *out_handle =
      (VkrTextureHandle){.id = texture->description.id,
                         .generation = texture->description.generation};
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  log_debug("Loaded cube map texture: %s (%dx%d)", stable_key, width, height);

  return true_v;
}
