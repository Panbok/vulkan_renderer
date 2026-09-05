#include "renderer/systems/vkr_texture_system.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_transcode_cache.h"
#include "renderer/vkr_ibl_math.h"

#include "ktx.h"
#include "stb_image.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>

// =============================================================================
// Texture Cache Format
// =============================================================================
// Binary cache format for decoded textures to skip stb_image decoding on
// subsequent loads. Cache files are stored alongside source files with .vkt
// extension.

#define VKR_TEXTURE_CACHE_MAGIC 0x564B5448u /* 'VKTH' in little-endian */
#define VKR_TEXTURE_CACHE_VERSION 3u        /* Bump when format changes */
#define VKR_TEXTURE_CACHE_EXT ".vkt"
#define VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_INITIAL MB(1)
#define VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_RESERVE MB(16)

vkr_internal String8 vkr_texture_strip_resource_key_prefix(String8 name);
vkr_internal String8 vkr_texture_strip_query(String8 name, String8 *out_query);

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
  String8 query = {0};
  source_path = vkr_texture_strip_query(source_path, &query);
  (void)query;
  source_path = vkr_texture_strip_resource_key_prefix(source_path);
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
  VkrTextureClass texture_class;
  bool8_t has_explicit_colorspace;
  bool8_t has_explicit_class;
  bool8_t source_only;
} VkrTextureRequest;

typedef struct VkrTextureQueryColorScanResult {
  bool8_t prefers_srgb;
  bool8_t had_unknown;
  bool8_t has_explicit;
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
 * @brief Remove accidental `<type>|` resource-key prefixes from texture names.
 *
 * Async request dedupe keys use this format internally; texture I/O expects the
 * raw asset path/query. Keeping this normalization at the texture boundary
 * prevents malformed file opens like `1|assets/textures/...`.
 */
vkr_internal String8 vkr_texture_strip_resource_key_prefix(String8 name) {
  if (!name.str || name.length < 3) {
    return name;
  }

  String8 stripped = name;
  bool8_t changed = false_v;
  for (uint32_t pass = 0; pass < 4; ++pass) {
    uint64_t pipe_index = UINT64_MAX;

    for (uint64_t segment_start = 0; segment_start < stripped.length;) {
      if (segment_start > 0) {
        uint8_t prev = stripped.str[segment_start - 1];
        if (prev != '/' && prev != '\\') {
          segment_start++;
          continue;
        }
      }

      uint64_t index = segment_start;
      while (index < stripped.length && stripped.str[index] >= '0' &&
             stripped.str[index] <= '9') {
        index++;
      }

      if (index > segment_start && index < stripped.length &&
          stripped.str[index] == '|') {
        pipe_index = index;
        break;
      }

      while (segment_start < stripped.length &&
             stripped.str[segment_start] != '/' &&
             stripped.str[segment_start] != '\\') {
        segment_start++;
      }
      if (segment_start < stripped.length) {
        segment_start++;
      }
    }

    if (pipe_index == UINT64_MAX) {
      break;
    }

    String8 next =
        string8_substring(&stripped, pipe_index + 1, stripped.length);
    if (!next.str || next.length == 0) {
      break;
    }

    stripped = next;
    changed = true_v;
  }

  if (!changed) {
    return name;
  }

  vkr_local_persist uint32_t logged_prefix_count = 0;
  if (logged_prefix_count < 8u) {
    log_debug("Texture request carried internal key prefix, normalizing '%.*s'",
              (int32_t)name.length, name.str);
    logged_prefix_count++;
  }

  return stripped;
}

/**
 * @brief Iterates query parameters and returns the next valid `key=value` pair.
 * @param query Query string without leading `?`.
 * @param io_cursor In/out scan cursor.
 * @param out_key Output key slice.
 * @param out_value Output value slice.
 * @return true when a pair is produced, false when iteration completes.
 */
vkr_internal bool8_t vkr_texture_query_next_pair(String8 query,
                                                 uint64_t *io_cursor,
                                                 String8 *out_key,
                                                 String8 *out_value) {
  if (!io_cursor || !out_key || !out_value) {
    return false_v;
  }

  uint64_t cursor = *io_cursor;
  while (cursor < query.length) {
    uint64_t end = cursor;
    while (end < query.length && query.str[end] != '&') {
      end++;
    }

    String8 param = string8_substring(&query, cursor, end);
    cursor = end + 1;

    uint64_t eq_pos = UINT64_MAX;
    for (uint64_t i = 0; i < param.length; ++i) {
      if (param.str[i] == '=') {
        eq_pos = i;
        break;
      }
    }
    if (eq_pos == UINT64_MAX || eq_pos == 0 || eq_pos + 1 >= param.length) {
      continue;
    }

    *out_key = string8_substring(&param, 0, eq_pos);
    *out_value = string8_substring(&param, eq_pos + 1, param.length);
    *io_cursor = cursor;
    return true_v;
  }

  *io_cursor = query.length;
  return false_v;
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
      .has_explicit = false_v,
  };
  const String8 key_cs = string8_lit("cs");
  const String8 val_srgb = string8_lit("srgb");
  const String8 val_linear = string8_lit("linear");

  uint64_t cursor = 0;
  String8 key = {0};
  String8 value = {0};
  while (vkr_texture_query_next_pair(query, &cursor, &key, &value)) {
    if (!string8_equalsi(&key, &key_cs)) {
      continue;
    }

    if (string8_equalsi(&value, &val_srgb)) {
      result.prefers_srgb = true_v;
      result.has_explicit = true_v;
    } else if (string8_equalsi(&value, &val_linear)) {
      result.prefers_srgb = false_v;
      result.has_explicit = true_v;
    } else {
      result.had_unknown = true_v;
      if (unknown_sets_linear) {
        result.prefers_srgb = false_v;
      }
    }
  }

  return result;
}

vkr_internal bool8_t vkr_texture_class_from_string(String8 value,
                                                   VkrTextureClass *out_class) {
  if (!out_class || !value.str || value.length == 0) {
    return false_v;
  }

  const String8 color_srgb = string8_lit("color_srgb");
  const String8 srgb = string8_lit("srgb");
  const String8 color = string8_lit("color");
  const String8 color_linear = string8_lit("color_linear");
  const String8 linear = string8_lit("linear");
  const String8 normal_rg = string8_lit("normal_rg");
  const String8 normal = string8_lit("normal");
  const String8 data_mask = string8_lit("data_mask");
  const String8 data = string8_lit("data");
  const String8 mask = string8_lit("mask");

  if (string8_equalsi(&value, &color_srgb) || string8_equalsi(&value, &srgb) ||
      string8_equalsi(&value, &color)) {
    *out_class = VKR_TEXTURE_CLASS_COLOR_SRGB;
    return true_v;
  }
  if (string8_equalsi(&value, &color_linear) ||
      string8_equalsi(&value, &linear)) {
    *out_class = VKR_TEXTURE_CLASS_COLOR_LINEAR;
    return true_v;
  }
  if (string8_equalsi(&value, &normal_rg) || string8_equalsi(&value, &normal)) {
    *out_class = VKR_TEXTURE_CLASS_NORMAL_RG;
    return true_v;
  }
  if (string8_equalsi(&value, &data_mask) || string8_equalsi(&value, &data) ||
      string8_equalsi(&value, &mask)) {
    *out_class = VKR_TEXTURE_CLASS_DATA_MASK;
    return true_v;
  }

  return false_v;
}

vkr_internal bool8_t vkr_texture_scan_query_class(String8 query,
                                                  VkrTextureClass *out_class,
                                                  bool8_t *out_had_unknown) {
  if (!out_class) {
    return false_v;
  }
  if (out_had_unknown) {
    *out_had_unknown = false_v;
  }

  const String8 key_tc = string8_lit("tc");
  const String8 key_class = string8_lit("class");
  bool8_t has_explicit = false_v;

  uint64_t cursor = 0;
  String8 key = {0};
  String8 value = {0};
  while (vkr_texture_query_next_pair(query, &cursor, &key, &value)) {
    if (!string8_equalsi(&key, &key_tc) && !string8_equalsi(&key, &key_class)) {
      continue;
    }

    VkrTextureClass parsed = VKR_TEXTURE_CLASS_COLOR_LINEAR;
    if (vkr_texture_class_from_string(value, &parsed)) {
      *out_class = parsed;
      has_explicit = true_v;
    } else if (out_had_unknown) {
      *out_had_unknown = true_v;
    }
  }

  return has_explicit;
}

vkr_internal bool8_t vkr_texture_scan_query_source_only(String8 query) {
  const String8 source_key = string8_lit("source");
  const String8 only_value = string8_lit("only");
  bool8_t source_only = false_v;

  uint64_t cursor = 0;
  String8 key = {0};
  String8 value = {0};
  while (vkr_texture_query_next_pair(query, &cursor, &key, &value)) {
    if (string8_equalsi(&key, &source_key)) {
      source_only = string8_equalsi(&value, &only_value);
    }
  }

  return source_only;
}

vkr_internal bool8_t vkr_texture_contains_token_ci(String8 name,
                                                   String8 token) {
  if (!name.str || !token.str || token.length == 0 ||
      token.length > name.length) {
    return false_v;
  }

  for (uint64_t i = 0; i <= name.length - token.length; ++i) {
    bool8_t match = true_v;
    for (uint64_t j = 0; j < token.length; ++j) {
      const unsigned char a = (unsigned char)name.str[i + j];
      const unsigned char b = (unsigned char)token.str[j];
      if (tolower(a) != tolower(b)) {
        match = false_v;
        break;
      }
    }
    if (match) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_texture_name_contains_any_ci(String8 name,
                                                      const String8 *tokens,
                                                      uint32_t token_count) {
  for (uint32_t i = 0; i < token_count; ++i) {
    if (vkr_texture_contains_token_ci(name, tokens[i])) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal VkrTextureClass vkr_texture_class_from_filename_heuristic(
    String8 base_path, VkrTextureColorSpace colorspace) {
  if (!base_path.str || base_path.length == 0) {
    return colorspace == VKR_TEXTURE_COLORSPACE_SRGB
               ? VKR_TEXTURE_CLASS_COLOR_SRGB
               : VKR_TEXTURE_CLASS_COLOR_LINEAR;
  }

  const String8 normal_tokens[] = {string8_lit("normal"), string8_lit("_n."),
                                   string8_lit("norm"), string8_lit("ddn"),
                                   string8_lit("bump")};
  if (vkr_texture_name_contains_any_ci(base_path, normal_tokens,
                                       ArrayCount(normal_tokens))) {
    return VKR_TEXTURE_CLASS_NORMAL_RG;
  }

  const String8 data_tokens[] = {
      string8_lit("roughness"), string8_lit("metallic"),
      string8_lit("metalness"), string8_lit("occlusion"),
      string8_lit("ao."),       string8_lit("orm"),
      string8_lit("rma"),       string8_lit("mask"),
      string8_lit("height"),    string8_lit("displace"),
      string8_lit("specular"),  string8_lit("gloss"),
      string8_lit("data"),      string8_lit("utility")};
  if (vkr_texture_name_contains_any_ci(base_path, data_tokens,
                                       ArrayCount(data_tokens))) {
    return VKR_TEXTURE_CLASS_DATA_MASK;
  }

  return colorspace == VKR_TEXTURE_COLORSPACE_SRGB
             ? VKR_TEXTURE_CLASS_COLOR_SRGB
             : VKR_TEXTURE_CLASS_COLOR_LINEAR;
}

/**
 * @brief Parse a texture request into a base path and desired color space.
 * @note Consumes `cs` (colorspace), `tc`/`class` (texture class), and
 * `source=only` (bypass every `.vkt` candidate) query parameters.
 * @note Unknown `cs` values log once and default to linear.
 */
vkr_internal VkrTextureRequest vkr_texture_parse_request(String8 name) {
  name = vkr_texture_strip_resource_key_prefix(name);
  String8 query = {0};
  String8 base_path = vkr_texture_strip_query(name, &query);
  VkrTextureQueryColorScanResult scan =
      vkr_texture_scan_query_colorspace(query, false_v, true_v);
  VkrTextureClass texture_class = VKR_TEXTURE_CLASS_COLOR_SRGB;
  bool8_t had_unknown_class = false_v;
  const bool8_t has_explicit_class =
      vkr_texture_scan_query_class(query, &texture_class, &had_unknown_class);

  VkrTextureColorSpace colorspace = scan.prefers_srgb
                                        ? VKR_TEXTURE_COLORSPACE_SRGB
                                        : VKR_TEXTURE_COLORSPACE_LINEAR;
  if (!has_explicit_class) {
    texture_class =
        vkr_texture_class_from_filename_heuristic(base_path, colorspace);
  }

  if (scan.had_unknown) {
    vkr_local_persist bool8_t warned_unknown = false_v;
    if (!warned_unknown) {
      log_warn("Texture request has unknown colorspace value; defaulting to "
               "linear");
      warned_unknown = true_v;
    }
  }
  if (had_unknown_class) {
    vkr_local_persist bool8_t warned_unknown_class = false_v;
    if (!warned_unknown_class) {
      log_warn("Texture request has unknown class value; falling back to "
               "inference");
      warned_unknown_class = true_v;
    }
  }

  return (VkrTextureRequest){
      .base_path = base_path,
      .colorspace = colorspace,
      .texture_class = texture_class,
      .has_explicit_colorspace = scan.has_explicit,
      .has_explicit_class = has_explicit_class,
      .source_only = vkr_texture_scan_query_source_only(query),
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

  request_path = vkr_texture_strip_resource_key_prefix(request_path);
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
  vkr_local_persist const uint8_t ktx2_signature[12] = {
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

vkr_internal bool8_t
vkr_texture_device_prefers_discrete(VkrDeviceTypeFlags device_types) {
  return bitset8_is_set(&device_types, VKR_DEVICE_TYPE_DISCRETE_BIT);
}

vkr_internal VkrTextureFormat vkr_texture_color_family_format(
    bool8_t request_srgb, VkrTextureFormat unorm, VkrTextureFormat srgb) {
  return request_srgb ? srgb : unorm;
}

VkrTextureFormat vkr_texture_select_transcode_target_format(
    VkrTextureClass texture_class, bool8_t request_srgb,
    VkrDeviceTypeFlags device_types, bool8_t supports_astc_4x4,
    bool8_t supports_bc7, bool8_t supports_etc2, bool8_t supports_bc5,
    bool8_t supports_eac_rg11) {
  const bool8_t prefer_discrete =
      vkr_texture_device_prefers_discrete(device_types);

  if (texture_class == VKR_TEXTURE_CLASS_NORMAL_RG) {
    if (prefer_discrete) {
      if (supports_bc5) {
        return VKR_TEXTURE_FORMAT_BC5_UNORM;
      }
      if (supports_astc_4x4) {
        return VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
      }
    } else {
      if (supports_astc_4x4) {
        return VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
      }
      if (supports_bc5) {
        return VKR_TEXTURE_FORMAT_BC5_UNORM;
      }
    }
    if (supports_eac_rg11) {
      return VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM;
    }
    // Not R8G8_UNORM: libktx has no uncompressed two-channel transcode target,
    // so selecting it produced KTX_TTF_NOSELECTION and failed every strict
    // .vkt load on devices with neither BC5 nor ASTC. RGBA32 wastes two
    // channels but is always transcodable.
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  }

  if (texture_class == VKR_TEXTURE_CLASS_DATA_MASK) {
    if (prefer_discrete) {
      if (supports_bc7) {
        return VKR_TEXTURE_FORMAT_BC7_UNORM;
      }
      if (supports_astc_4x4) {
        return VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
      }
      if (supports_etc2) {
        return VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM;
      }
    } else {
      if (supports_astc_4x4) {
        return VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
      }
      if (supports_etc2) {
        return VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM;
      }
      if (supports_bc7) {
        return VKR_TEXTURE_FORMAT_BC7_UNORM;
      }
    }
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  }

  // Color classes can use sRGB or UNORM variants based on request intent.
  const bool8_t allow_srgb = (texture_class == VKR_TEXTURE_CLASS_COLOR_SRGB);
  const bool8_t use_srgb = allow_srgb ? request_srgb : false_v;

  if (prefer_discrete) {
    if (supports_bc7) {
      return vkr_texture_color_family_format(
          use_srgb, VKR_TEXTURE_FORMAT_BC7_UNORM, VKR_TEXTURE_FORMAT_BC7_SRGB);
    }
    if (supports_astc_4x4) {
      return vkr_texture_color_family_format(use_srgb,
                                             VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM,
                                             VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB);
    }
    if (supports_etc2) {
      return vkr_texture_color_family_format(
          use_srgb, VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM,
          VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB);
    }
  } else {
    if (supports_astc_4x4) {
      return vkr_texture_color_family_format(use_srgb,
                                             VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM,
                                             VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB);
    }
    if (supports_etc2) {
      return vkr_texture_color_family_format(
          use_srgb, VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM,
          VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB);
    }
    if (supports_bc7) {
      return vkr_texture_color_family_format(
          use_srgb, VKR_TEXTURE_FORMAT_BC7_UNORM, VKR_TEXTURE_FORMAT_BC7_SRGB);
    }
  }

  return use_srgb ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
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
  VkrTextureFormatInfo info = {0};
  return vkr_texture_format_get_info(format, &info) && info.is_block_compressed;
}

vkr_internal uint32_t
vkr_texture_channel_count_from_format(VkrTextureFormat format) {
  VkrTextureFormatInfo info = {0};
  return vkr_texture_format_get_info(format, &info)
             ? (uint32_t)info.channel_count
             : VKR_TEXTURE_RGBA_CHANNELS;
}

vkr_internal ktx_transcode_fmt_e
vkr_texture_ktx_transcode_format_from_texture_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return KTX_TTF_BC7_RGBA;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return KTX_TTF_BC5_RG;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB:
    return KTX_TTF_ETC2_RGBA;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return KTX_TTF_ASTC_4x4_RGBA;
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return KTX_TTF_ETC2_EAC_RG11;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return KTX_TTF_RGBA32;
  default:
    return KTX_TTF_NOSELECTION;
  }
}

bool8_t vkr_texture_format_has_ktx_transcode_target(VkrTextureFormat format) {
  return vkr_texture_ktx_transcode_format_from_texture_format(format) !=
                 KTX_TTF_NOSELECTION
             ? true_v
             : false_v;
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

  String8 query = {0};
  cache_path = vkr_texture_strip_query(cache_path, &query);
  (void)query;
  cache_path = vkr_texture_strip_resource_key_prefix(cache_path);
  if (!cache_path.str || cache_path.length == 0) {
    return false_v;
  }

  String8 normalized_cache_path = string8_create_formatted(
      allocator, "%.*s", (int32_t)cache_path.length, cache_path.str);
  if (!normalized_cache_path.str || normalized_cache_path.length == 0) {
    return false_v;
  }

  FilePath fp = file_path_create((const char *)normalized_cache_path.str,
                                 allocator, FILE_PATH_TYPE_RELATIVE);
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

  String8 query = {0};
  cache_path = vkr_texture_strip_query(cache_path, &query);
  (void)query;
  cache_path = vkr_texture_strip_resource_key_prefix(cache_path);
  if (!cache_path.str || cache_path.length == 0) {
    return false_v;
  }

  String8 normalized_cache_path = string8_create_formatted(
      allocator, "%.*s", (int32_t)cache_path.length, cache_path.str);
  if (!normalized_cache_path.str || normalized_cache_path.length == 0) {
    return false_v;
  }

  FilePath fp = file_path_create((const char *)normalized_cache_path.str,
                                 allocator, FILE_PATH_TYPE_RELATIVE);

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

vkr_internal bool8_t vkr_texture_system_publish_prepared(
    VkrTextureSystem *system, VkrTextureHandle logical_handle,
    const VkrTexturePreparedLoad *prepared,
    VkrTextureOpaqueHandle *out_backend_handle, VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(prepared != NULL, "Prepared texture is NULL");
  assert_log(out_backend_handle != NULL, "Out backend handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_backend_handle = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;
  if (!system->asset_publisher || !system->asset_publisher->publish_texture) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return false_v;
  }
  if (!system->asset_publisher->publish_texture(system->asset_publisher->state,
                                                logical_handle, prepared)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  *out_backend_handle =
      (VkrTextureOpaqueHandle)&system->textures.data[logical_handle.id - 1];
  return true_v;
}

vkr_internal bool8_t vkr_texture_system_publish_pixels(
    VkrTextureSystem *system, VkrTexture *texture, const uint8_t *pixels,
    uint64_t byte_size, VkrRendererError *out_error) {
  VkrTextureUploadRegion region = {
      .mip_level = 0,
      .array_layer = 0,
      .width = texture->description.width,
      .height = texture->description.height,
      .depth = 1,
      .byte_offset = 0,
      .byte_size = byte_size,
  };
  VkrTexturePreparedLoad prepared = {
      .description = texture->description,
      .upload_data = (uint8_t *)pixels,
      .upload_data_size = byte_size,
      .upload_regions = &region,
      .upload_region_count = 1,
      .upload_mip_levels = 1,
      .upload_array_layers = 1,
      .upload_is_compressed = false_v,
  };
  VkrTextureHandle handle = {
      .id = texture->description.id,
      .generation = texture->description.generation,
  };
  return vkr_texture_system_publish_prepared(system, handle, &prepared,
                                             &texture->handle, out_error);
}

vkr_internal bool8_t vkr_texture_system_create_pixel_default(
    VkrTextureSystem *system, uint32_t texture_index, const uint8_t pixel[4],
    VkrTexturePropertyFlags properties, const char *label,
    VkrTextureHandle *out_handle) {
  VkrTexture *texture = &system->textures.data[texture_index];
  texture->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = properties,
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .id = texture_index + 1u,
      .generation = system->generation_counter++,
  };

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_publish_pixels(system, texture, pixel, 4u, &error)) {
    String8 error_string = vkr_renderer_get_error_string(error);
    log_error("Failed to create %s: %s", label, string8_cstr(&error_string));
    return false_v;
  }

  *out_handle = (VkrTextureHandle){
      .id = texture->description.id,
      .generation = texture->description.generation,
  };
  return true_v;
}

bool8_t vkr_texture_system_init(const VkrDeviceInformation *device_info,
                                const VkrTextureSystemConfig *config,
                                VkrJobSystem *job_system,
                                VkrTextureSystem *out_system) {
  assert_log(device_info != NULL, "Device information is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_system != NULL, "Out system is NULL");
  assert_log(config->max_texture_count > 0,
             "Max texture count must be greater than 0");
  assert_log(config->max_texture_count >= 5,
             "Texture system requires at least 5 textures for defaults");

  if (!config->asset_publisher || !config->asset_publisher->publish_texture ||
      !config->asset_publisher->unpublish_texture) {
    return false_v;
  }

  MemZero(out_system, sizeof(*out_system));

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  out_system->arena =
      arena_create(VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_RSV,
                   VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_CMT, app_arena_flags);
  if (!out_system->arena) {
    log_error("Failed to create texture system arena");
    return false_v;
  }

  out_system->config = *config;
  out_system->asset_publisher = config->asset_publisher;
  out_system->job_system = job_system;
  out_system->allocator = (VkrAllocator){.ctx = out_system->arena};
  if (!vkr_allocator_arena(&out_system->allocator)) {
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(*out_system));
    return false_v;
  }

  if (!vkr_dmemory_create(MB(1), MB(16), &out_system->string_memory)) {
    log_error("Failed to create texture system string allocator");
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(*out_system));
    return false_v;
  }
  out_system->string_allocator =
      (VkrAllocator){.ctx = &out_system->string_memory};
  vkr_dmemory_allocator_create(&out_system->string_allocator);

  if (!vkr_dmemory_create(VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_INITIAL,
                          VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_RESERVE,
                          &out_system->async_memory)) {
    log_error("Failed to create texture system async allocator");
    vkr_dmemory_allocator_destroy(&out_system->string_allocator);
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(*out_system));
    return false_v;
  }
  out_system->async_allocator =
      (VkrAllocator){.ctx = &out_system->async_memory};
  vkr_dmemory_allocator_create(&out_system->async_allocator);
  if (!vkr_mutex_create(&out_system->allocator, &out_system->async_mutex)) {
    log_error("Failed to create texture system async allocator mutex");
    vkr_dmemory_allocator_destroy(&out_system->async_allocator);
    vkr_dmemory_allocator_destroy(&out_system->string_allocator);
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(*out_system));
    return false_v;
  }

  out_system->device_types = bitset8_create();
  out_system->supports_texture_astc_4x4 = false_v;
  out_system->supports_texture_bc7 = false_v;
  out_system->supports_texture_etc2 = false_v;
  out_system->supports_texture_bc5 = false_v;
  out_system->supports_texture_eac_rg11 = false_v;
  out_system->device_types = device_info->device_types;
  out_system->supports_texture_astc_4x4 =
      device_info->supports_texture_astc_4x4;
  out_system->supports_texture_bc7 = device_info->supports_texture_bc7;
  out_system->supports_texture_etc2 = device_info->supports_texture_etc2;
  out_system->supports_texture_bc5 = device_info->supports_texture_bc5;
  out_system->supports_texture_eac_rg11 =
      device_info->supports_texture_eac_rg11;

  out_system->strict_vkt_only_mode =
      vkr_texture_env_flag("VKR_TEXTURE_VKT_STRICT", true_v);
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
  if (!out_system->textures.data) {
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }
  for (uint64_t i = 0; i < out_system->textures.length; ++i) {
    out_system->textures.data[i] = (VkrTexture){
        .description = {.id = VKR_INVALID_ID, .generation = VKR_INVALID_ID}};
  }
  out_system->texture_map = vkr_hash_table_create_VkrTextureEntry(
      &out_system->allocator, ((uint64_t)config->max_texture_count) * 2ULL);
  if (!out_system->texture_map.entries) {
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }
  out_system->texture_keys_by_index = (const char **)vkr_allocator_alloc(
      &out_system->allocator,
      sizeof(*out_system->texture_keys_by_index) * config->max_texture_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!out_system->texture_keys_by_index) {
    log_error("Failed to allocate texture reverse lookup table");
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }
  MemZero((void *)out_system->texture_keys_by_index,
          sizeof(*out_system->texture_keys_by_index) *
              config->max_texture_count);
  out_system->cache_guard =
      (struct VkrTextureCacheWriteGuard *)vkr_allocator_alloc(
          &out_system->allocator, sizeof(VkrTextureCacheWriteGuard),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!out_system->cache_guard) {
    log_error("Failed to allocate texture cache write guard");
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }

  MemZero(out_system->cache_guard, sizeof(VkrTextureCacheWriteGuard));
  if (!vkr_mutex_create(&out_system->allocator,
                        &out_system->cache_guard->mutex)) {
    log_error("Failed to create texture cache write guard mutex");
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }

  uint64_t guard_capacity =
      Max(16ULL, (uint64_t)config->max_texture_count * 2ULL);
  out_system->cache_guard->inflight =
      vkr_hash_table_create_VkrTextureCacheWriteEntry(&out_system->allocator,
                                                      guard_capacity);
  if (!out_system->cache_guard->inflight.entries) {
    log_error("Failed to create texture cache write guard hash table");
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }

  out_system->next_free_index = 0;
  out_system->generation_counter = 1;

  // Create default checkerboard texture at index 0
  VkrTexture *default_texture = &out_system->textures.data[0];
  default_texture->description = (VkrTextureDescription){
      .width = 256,
      .height = 256,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
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
    vkr_texture_system_shutdown(out_system);
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

  // Assign the shared identity before GPU publication.
  default_texture->description.id = 1; // slot 0 -> id 1
  default_texture->description.generation = out_system->generation_counter++;

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_publish_pixels(out_system, default_texture,
                                         default_texture->image, image_size,
                                         &renderer_error)) {
    String8 error_string = vkr_renderer_get_error_string(renderer_error);
    log_error("Failed to create default checkerboard texture: %s",
              string8_cstr(&error_string));
    vkr_allocator_end_scope(&image_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return false_v;
  }

  out_system->default_texture =
      (VkrTextureHandle){.id = default_texture->description.id,
                         .generation = default_texture->description.generation};

  vkr_allocator_end_scope(&image_scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  default_texture->image = NULL;

  const uint8_t flat_normal_pixel[4] = {128, 128, 255, 255};
  const uint8_t flat_specular_pixel[4] = {255, 255, 255, 255};
  const uint8_t white_pixel[4] = {255, 255, 255, 255};
  const uint8_t black_pixel[4] = {0, 0, 0, 255};
  if (!vkr_texture_system_create_pixel_default(
          out_system, 1u, flat_normal_pixel, bitset8_create(),
          "default normal texture", &out_system->default_normal_texture) ||
      !vkr_texture_system_create_pixel_default(
          out_system, 2u, flat_specular_pixel, bitset8_create(),
          "default specular texture", &out_system->default_specular_texture) ||
      !vkr_texture_system_create_pixel_default(
          out_system, 3u, white_pixel, bitset8_create(),
          "default diffuse texture", &out_system->default_diffuse_texture) ||
      !vkr_texture_system_create_pixel_default(
          out_system, 4u, black_pixel, bitset8_create(),
          "default emissive texture", &out_system->default_emissive_texture)) {
    vkr_texture_system_shutdown(out_system);
    return false_v;
  }

  // Ensure first free search starts after reserved defaults
  out_system->next_free_index = 5;

  return true_v;
}

void vkr_texture_system_shutdown(VkrTextureSystem *system) {
  if (!system)
    return;

  for (uint32_t texture_id = 0; texture_id < system->textures.length;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation != VKR_INVALID_ID && texture->handle) {
      if (!vkr_texture_destroy(system, texture)) {
        log_warn("TextureSystem: texture %u:%u could not be destroyed during "
                 "shutdown",
                 texture->description.id, texture->description.generation);
      }
    }
  }

  if (system->cache_guard) {
    vkr_hash_table_destroy_VkrTextureCacheWriteEntry(
        &system->cache_guard->inflight);
    vkr_mutex_destroy(&system->allocator, &system->cache_guard->mutex);
    system->cache_guard = NULL;
  }

  array_destroy_VkrTexture(&system->textures);
  if (system->async_mutex) {
    vkr_mutex_destroy(&system->allocator, &system->async_mutex);
  }
  if (system->async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&system->async_allocator);
  }
  if (system->string_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&system->string_allocator);
  }
  if (system->arena) {
    vkr_allocator_release_global_accounting(&system->allocator);
    arena_destroy(system->arena);
  }
  MemZero(system, sizeof(*system));
}

VkrTextureHandle vkr_texture_system_acquire(VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  texture_name = vkr_texture_strip_resource_key_prefix(texture_name);
  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  char *queryless_key = NULL;
  if (!entry) {
    String8 query = {0};
    String8 queryless_name = vkr_texture_strip_query(texture_name, &query);
    if (queryless_name.str && queryless_name.length > 0 &&
        queryless_name.length < texture_name.length) {
      queryless_key = (char *)malloc((size_t)queryless_name.length + 1);
      if (queryless_key) {
        MemCopy(queryless_key, queryless_name.str,
                (size_t)queryless_name.length);
        queryless_key[queryless_name.length] = '\0';
        entry = vkr_hash_table_get_VkrTextureEntry(&system->texture_map,
                                                   queryless_key);
        if (entry) {
          texture_key = queryless_key;
        }
      }
    }
  }

  if (entry) {
    if (entry->ref_count == 0) {
      entry->auto_release = auto_release;
    }
    entry->ref_count++;
    *out_error = VKR_RENDERER_ERROR_NONE;
    VkrTexture *texture = &system->textures.data[entry->index];
    VkrTextureHandle handle = {.id = texture->description.id,
                               .generation = texture->description.generation};
    if (queryless_key) {
      free(queryless_key);
    }
    return handle;
  }

  if (queryless_key) {
    free(queryless_key);
  }

  // Async loading intentionally exposes not-ready states; keep this diagnostic
  // low-noise to avoid flooding logs while dependencies converge.
  log_debug("Texture '%s' not yet loaded, use resource system to load first",
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
  const VkrTextureHandle logical_handle = {.id = desc_copy.id,
                                           .generation = desc_copy.generation};

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->string_allocator, name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for texture map");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    system->next_free_index = Min(system->next_free_index, free_slot_index);
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = desc_copy;

  VkrTextureEntry entry = {
      .index = free_slot_index,
      .ref_count = 1,
      .auto_release = true_v,
      .name = stable_key,
  };
  bool8_t insert_success = vkr_hash_table_insert_VkrTextureEntry(
      &system->texture_map, stable_key, entry);
  if (!insert_success) {
    log_error("Failed to insert texture '%s' into hash table", stable_key);
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    texture->description.id = VKR_INVALID_ID;
    texture->description.generation = VKR_INVALID_ID;
    system->next_free_index = Min(system->next_free_index, free_slot_index);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  system->texture_keys_by_index[free_slot_index] = stable_key;

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrTextureOpaqueHandle handle = NULL;
  if (!system->asset_publisher ||
      !system->asset_publisher->publish_writable_texture) {
    renderer_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  } else if (system->asset_publisher->publish_writable_texture(
                 system->asset_publisher->state, logical_handle, &desc_copy)) {
    handle = (VkrTextureOpaqueHandle)texture;
  } else {
    renderer_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }
  if (renderer_error != VKR_RENDERER_ERROR_NONE || !handle) {
    (void)vkr_hash_table_remove_VkrTextureEntry(&system->texture_map,
                                                stable_key);
    system->texture_keys_by_index[free_slot_index] = NULL;
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    texture->description.id = VKR_INVALID_ID;
    texture->description.generation = VKR_INVALID_ID;
    system->next_free_index = Min(system->next_free_index, free_slot_index);
    *out_error = renderer_error != VKR_RENDERER_ERROR_NONE
                     ? renderer_error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  texture->handle = handle;

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_texture_system_release(VkrTextureSystem *system,
                                   String8 texture_name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(texture_name.str != NULL, "Name is NULL");

  texture_name = vkr_texture_strip_resource_key_prefix(texture_name);
  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  char *queryless_key = NULL;
  if (!entry) {
    String8 query = {0};
    String8 queryless_name = vkr_texture_strip_query(texture_name, &query);
    if (queryless_name.str && queryless_name.length > 0 &&
        queryless_name.length < texture_name.length) {
      queryless_key = (char *)malloc((size_t)queryless_name.length + 1);
      if (queryless_key) {
        MemCopy(queryless_key, queryless_name.str,
                (size_t)queryless_name.length);
        queryless_key[queryless_name.length] = '\0';
        entry = vkr_hash_table_get_VkrTextureEntry(&system->texture_map,
                                                   queryless_key);
        if (entry) {
          texture_key = queryless_key;
        }
      }
    }
  }

  if (!entry) {
    /*
     * Async load/cancel/release ordering can legitimately race texture
     * teardown, so a missing key here is not a correctness failure.
     */
    log_debug("Texture '%s' already released before texture-system release",
              texture_key);
    if (queryless_key) {
      free(queryless_key);
    }
    return true_v;
  }

  if (entry->ref_count > 0) {
    entry->ref_count--;
  } else if (!entry->auto_release) {
    log_warn("Over-release detected for texture '%s'", texture_key);
    if (queryless_key) {
      free(queryless_key);
    }
    return false_v;
  }

  bool8_t released = true_v;
  if (entry->ref_count == 0 && entry->auto_release) {
    uint32_t texture_index = entry->index;
    if (texture_index != system->default_texture.id - 1) {
      String8 unload_name = texture_name;
      if (entry->name) {
        unload_name = string8_create_from_cstr((const uint8_t *)entry->name,
                                               string_length(entry->name));
      }
      VkrResourceHandleInfo handle_info = {
          .type = VKR_RESOURCE_TYPE_TEXTURE,
          .loader_id = vkr_resource_system_get_loader_id(
              VKR_RESOURCE_TYPE_TEXTURE, unload_name),
          .as.texture = (VkrTextureHandle){
              .id = system->textures.data[texture_index].description.id,
              .generation =
                  system->textures.data[texture_index].description.generation}};
      vkr_resource_system_unload(&handle_info, unload_name);
      released = vkr_texture_system_get_by_handle(
                     system, handle_info.as.texture) == NULL;
    }
  }

  if (queryless_key) {
    free(queryless_key);
  }
  return released;
}

void vkr_texture_system_add_ref_by_handle(VkrTextureSystem *system,
                                          VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0 || handle.generation == VKR_INVALID_ID) {
    return;
  }

  uint32_t texture_index = handle.id - 1;
  if (!system->texture_keys_by_index ||
      texture_index >= system->textures.length) {
    return;
  }

  VkrTexture *texture = &system->textures.data[texture_index];
  if (texture->description.generation != handle.generation) {
    return;
  }

  const char *key = system->texture_keys_by_index[texture_index];
  if (!key) {
    return;
  }

  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, key);
  if (entry) {
    entry->ref_count++;
  }
}

uint32_t vkr_texture_system_get_ref_count_by_handle(VkrTextureSystem *system,
                                                    VkrTextureHandle handle) {
  if (!system || handle.id == 0u || handle.generation == VKR_INVALID_ID) {
    return 0u;
  }

  const uint32_t texture_index = handle.id - 1u;
  if (!system->texture_keys_by_index ||
      texture_index >= system->textures.length ||
      system->textures.data[texture_index].description.generation !=
          handle.generation) {
    return 0u;
  }

  const char *key = system->texture_keys_by_index[texture_index];
  const VkrTextureEntry *entry =
      key ? vkr_hash_table_get_VkrTextureEntry(&system->texture_map, key)
          : NULL;
  return entry ? entry->ref_count : 0u;
}

bool8_t vkr_texture_system_release_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0 || handle.generation == VKR_INVALID_ID) {
    log_warn("Attempted to release invalid texture handle");
    return false_v;
  }

  uint32_t texture_index = handle.id - 1;
  if (!system->texture_keys_by_index ||
      texture_index >= system->textures.length) {
    return false_v;
  }

  VkrTexture *texture = &system->textures.data[texture_index];
  if (texture->description.generation != handle.generation) {
    return false_v;
  }

  const char *key = system->texture_keys_by_index[texture_index];
  if (!key) {
    return false_v;
  }

  uint64_t key_length = string_length(key);
  if (key_length == 0) {
    return false_v;
  }

  String8 texture_name =
      string8_create_from_cstr((const uint8_t *)key, key_length);
  return vkr_texture_system_release(system, texture_name);
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

  if (!system->asset_publisher ||
      !system->asset_publisher->update_texture_sampler) {
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }
  if (!system->asset_publisher->update_texture_sampler(
          system->asset_publisher->state, handle, &updated_desc)) {
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }
  texture->description = updated_desc;
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t vkr_texture_destroy(VkrTextureSystem *system, VkrTexture *texture) {
  assert_log(system != NULL, "System is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  if (texture->handle && !bitset8_is_set(&texture->description.properties,
                                         VKR_TEXTURE_PROPERTY_EXTERNAL_BIT)) {
    VkrTextureHandle handle = {
        .id = texture->description.id,
        .generation = texture->description.generation,
    };
    bool8_t destroyed = system->asset_publisher &&
                        system->asset_publisher->unpublish_texture &&
                        system->asset_publisher->unpublish_texture(
                            system->asset_publisher->state, handle);
    if (!destroyed) {
      log_warn("TextureSystem: failed to destroy texture %u:%u; ownership "
               "retained for retry",
               handle.id, handle.generation);
      return false_v;
    }
  }

  MemZero(texture, sizeof(VkrTexture));
  return true_v;
}

VkrTexture *vkr_texture_system_get_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle) {
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

VkrTextureHandle
vkr_texture_system_get_default_emissive_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_emissive_texture;
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
  VkrTextureType upload_type;
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
 * @param colorspace Requested sampling color space
 * @param texture_class Requested semantic texture class
 * @param has_explicit_colorspace Whether colorspace came from explicit query
 * input
 * @param has_explicit_class Whether texture class came from explicit query
 * input
 * @param source_only Whether every packed sidecar must be bypassed
 * @param system The texture system owning the cache guard
 * @param result The result of the texture decoding
 */
typedef struct VkrTextureDecodeJobPayload {
  String8 file_path;
  uint32_t desired_channels;
  bool8_t flip_vertical;
  VkrTextureColorSpace colorspace;
  VkrTextureClass texture_class;
  bool8_t has_explicit_colorspace;
  bool8_t has_explicit_class;
  bool8_t source_only;
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
  result->upload_type = VKR_TEXTURE_TYPE_2D;
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

vkr_internal bool8_t vkr_texture_ktx_metadata_string(ktxTexture *texture,
                                                     const char *key,
                                                     String8 *out_value) {
  if (out_value) {
    *out_value = (String8){0};
  }
  if (!texture || !key || !out_value) {
    return false_v;
  }

  unsigned int value_len = 0;
  void *value = NULL;
  if (ktxHashList_FindValue(&texture->kvDataHead, key, &value_len, &value) !=
          KTX_SUCCESS ||
      !value || value_len == 0) {
    return false_v;
  }

  const uint8_t *bytes = (const uint8_t *)value;
  uint64_t len = 0;
  while (len < value_len && bytes[len] != '\0') {
    len++;
  }
  if (len == 0) {
    return false_v;
  }

  *out_value = (String8){.str = (uint8_t *)bytes, .length = len};
  return true_v;
}

vkr_internal VkrTextureColorSpace vkr_texture_ktx_metadata_colorspace(
    ktxTexture *texture, VkrTextureColorSpace default_value,
    bool8_t *out_found) {
  if (out_found) {
    *out_found = false_v;
  }

  String8 value = {0};
  if (!vkr_texture_ktx_metadata_string(texture, "vkr.colorspace_hint",
                                       &value)) {
    return default_value;
  }

  const String8 srgb = string8_lit("srgb");
  const String8 linear = string8_lit("linear");
  if (string8_equalsi(&value, &srgb)) {
    if (out_found) {
      *out_found = true_v;
    }
    return VKR_TEXTURE_COLORSPACE_SRGB;
  }
  if (string8_equalsi(&value, &linear)) {
    if (out_found) {
      *out_found = true_v;
    }
    return VKR_TEXTURE_COLORSPACE_LINEAR;
  }

  return default_value;
}

vkr_internal VkrTextureClass vkr_texture_ktx_metadata_class(
    ktxTexture *texture, VkrTextureClass default_value, bool8_t *out_found) {
  if (out_found) {
    *out_found = false_v;
  }

  String8 value = {0};
  if (!vkr_texture_ktx_metadata_string(texture, "vkr.texture_class", &value)) {
    return default_value;
  }

  VkrTextureClass parsed = default_value;
  if (!vkr_texture_class_from_string(value, &parsed)) {
    return default_value;
  }

  if (out_found) {
    *out_found = true_v;
  }
  return parsed;
}

vkr_internal bool8_t vkr_texture_decode_from_ktx2(
    VkrAllocator *allocator, VkrTextureSystem *system, String8 vkt_path,
    VkrTextureColorSpace colorspace, bool8_t has_explicit_colorspace,
    VkrTextureClass texture_class, bool8_t has_explicit_class,
    VkrTextureDecodeResult *out_result) {
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
  const bool8_t cubemap = base_texture->isCubemap ? true_v : false_v;
  const uint32_t face_count = base_texture->numFaces;
  const uint64_t physical_layers_u64 =
      (uint64_t)base_texture->numLayers * face_count;
  if (base_texture->numDimensions != 2 || base_texture->numLayers == 0u ||
      (face_count != 1u && face_count != 6u) || cubemap != (face_count == 6u) ||
      physical_layers_u64 == 0u ||
      physical_layers_u64 > VKR_TEXTURE_MAX_ARRAY_LAYERS) {
    log_error("Unsupported KTX2 texture shape for '%s' (dims=%u layers=%u "
              "faces=%u cubemap=%u)",
              path_cstr, base_texture->numDimensions, base_texture->numLayers,
              face_count, base_texture->isCubemap);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }
  const uint32_t physical_layers = (uint32_t)physical_layers_u64;
  const VkrTextureType texture_type =
      cubemap ? (base_texture->numLayers > 1u ? VKR_TEXTURE_TYPE_CUBE_MAP_ARRAY
                                              : VKR_TEXTURE_TYPE_CUBE_MAP)
              : (base_texture->numLayers > 1u ? VKR_TEXTURE_TYPE_2D_ARRAY
                                              : VKR_TEXTURE_TYPE_2D);

  if (base_texture->baseWidth == 0 || base_texture->baseHeight == 0 ||
      base_texture->baseWidth > VKR_TEXTURE_MAX_DIMENSION ||
      base_texture->baseHeight > VKR_TEXTURE_MAX_DIMENSION ||
      (cubemap && base_texture->baseWidth != base_texture->baseHeight)) {
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

  bool8_t metadata_class_found = false_v;
  VkrTextureColorSpace effective_colorspace =
      has_explicit_colorspace
          ? colorspace
          : vkr_texture_ktx_metadata_colorspace(base_texture, colorspace, NULL);
  VkrTextureClass effective_class =
      has_explicit_class
          ? texture_class
          : vkr_texture_ktx_metadata_class(base_texture, texture_class,
                                           &metadata_class_found);

  // If class metadata is absent, keep filename-driven non-color classes
  // (normal/data), but realign color class to effective colorspace metadata.
  if (!has_explicit_class && !metadata_class_found &&
      (effective_class == VKR_TEXTURE_CLASS_COLOR_SRGB ||
       effective_class == VKR_TEXTURE_CLASS_COLOR_LINEAR)) {
    effective_class = (effective_colorspace == VKR_TEXTURE_COLORSPACE_SRGB)
                          ? VKR_TEXTURE_CLASS_COLOR_SRGB
                          : VKR_TEXTURE_CLASS_COLOR_LINEAR;
  }

  const bool8_t request_srgb =
      (effective_colorspace == VKR_TEXTURE_COLORSPACE_SRGB);
  const VkrTextureFormat target_format =
      vkr_texture_select_transcode_target_format(
          effective_class, request_srgb, system->device_types,
          system->supports_texture_astc_4x4, system->supports_texture_bc7,
          system->supports_texture_etc2, system->supports_texture_bc5,
          system->supports_texture_eac_rg11);
  const ktx_transcode_fmt_e target_transcode_format =
      vkr_texture_ktx_transcode_format_from_texture_format(target_format);
  if (target_transcode_format == KTX_TTF_NOSELECTION) {
    // The selector picked a format libktx cannot transcode to. That is a bug in
    // the selector, not a property of this texture, so say so loudly rather
    // than failing the load silently.
    log_error("No KTX2 transcode target for the format selected for '%s' "
              "(texture class %d, format %d); this device's capability "
              "combination is unhandled by the transcode selector",
              path_cstr, (int)effective_class, (int)target_format);
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  const bool8_t has_transparency = vkr_texture_ktx_metadata_bool(
      base_texture, "vkr.has_transparency", false_v);
  const bool8_t alpha_mask =
      vkr_texture_ktx_metadata_bool(base_texture, "vkr.alpha_mask", false_v);
  VkrTextureTranscodeCacheRecord cached = {0};
  if (vkr_texture_transcode_cache_load(
          allocator, vkt_path, file_data, file_size, target_format,
          base_texture->baseWidth, base_texture->baseHeight,
          base_texture->numLevels, physical_layers, &cached)) {
    vkr_atomic_uint64_fetch_add(&system->transcode_cache_hits, 1u,
                                VKR_MEMORY_ORDER_RELAXED);
    out_result->upload_data = cached.data;
    out_result->upload_data_size = cached.data_size;
    out_result->upload_regions = cached.regions;
    out_result->upload_region_count = cached.region_count;
    out_result->upload_mip_levels = cached.mip_levels;
    out_result->upload_array_layers = cached.array_layers;
    out_result->upload_format = cached.format;
    out_result->upload_type = texture_type;
    out_result->upload_is_compressed = cached.is_compressed;
    out_result->width = (int32_t)cached.width;
    out_result->height = (int32_t)cached.height;
    out_result->original_channels = (int32_t)cached.channels;
    out_result->has_transparency = cached.has_transparency;
    out_result->alpha_mask = cached.alpha_mask;
    out_result->loaded_from_cache = true_v;
    out_result->success = true_v;
    cached.data = NULL;
    cached.regions = NULL;
    success = true_v;
    goto cleanup;
  }
  vkr_atomic_uint64_fetch_add(&system->transcode_cache_misses, 1u,
                              VKR_MEMORY_ORDER_RELAXED);

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

  const uint64_t region_count_u64 =
      (uint64_t)base_texture->numLevels * physical_layers;
  if (region_count_u64 == 0u ||
      region_count_u64 > VKR_TEXTURE_MAX_UPLOAD_REGIONS) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }
  const uint32_t region_count = (uint32_t)region_count_u64;

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
    for (uint32_t face = 0; face < face_count; ++face) {
      for (uint32_t mip = 0; mip < base_texture->numLevels; ++mip) {
        ktx_size_t image_offset = 0;
        ktx_result = ktxTexture_GetImageOffset(base_texture, mip, layer, face,
                                               &image_offset);
        if (ktx_result != KTX_SUCCESS || image_offset > ktx_data_size) {
          out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
          goto cleanup;
        }

        const ktx_size_t image_size =
            ktxTexture_GetImageSize(base_texture, mip);
        if (image_offset + image_size > ktx_data_size) {
          out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
          goto cleanup;
        }

        const uint32_t mip_width = Max(1u, base_texture->baseWidth >> mip);
        const uint32_t mip_height = Max(1u, base_texture->baseHeight >> mip);
        upload_regions[region_index++] = (VkrTextureUploadRegion){
            .mip_level = mip,
            .array_layer = layer * face_count + face,
            .width = mip_width,
            .height = mip_height,
            .depth = 1,
            .byte_offset = image_offset,
            .byte_size = image_size,
        };
      }
    }
  }

  out_result->upload_data = upload_data;
  out_result->upload_data_size = ktx_data_size;
  out_result->upload_regions = upload_regions;
  out_result->upload_region_count = region_count;
  out_result->upload_mip_levels = base_texture->numLevels;
  out_result->upload_array_layers = physical_layers;
  out_result->upload_format = target_format;
  out_result->upload_type = texture_type;
  out_result->upload_is_compressed =
      vkr_texture_format_is_block_compressed(target_format);
  out_result->width = (int32_t)base_texture->baseWidth;
  out_result->height = (int32_t)base_texture->baseHeight;
  out_result->original_channels =
      (int32_t)vkr_texture_channel_count_from_format(target_format);
  out_result->has_transparency = has_transparency;
  out_result->alpha_mask = alpha_mask;
  const VkrTextureTranscodeCacheRecord cache_record = {
      .width = base_texture->baseWidth,
      .height = base_texture->baseHeight,
      .channels = out_result->original_channels,
      .format = target_format,
      .mip_levels = base_texture->numLevels,
      .array_layers = physical_layers,
      .is_compressed = out_result->upload_is_compressed,
      .has_transparency = has_transparency,
      .alpha_mask = alpha_mask,
      .data = upload_data,
      .data_size = ktx_data_size,
      .regions = upload_regions,
      .region_count = region_count,
  };
  if (vkr_texture_transcode_cache_store(allocator, vkt_path, file_data,
                                        file_size, &cache_record)) {
    vkr_atomic_uint64_fetch_add(&system->transcode_cache_writes, 1u,
                                VKR_MEMORY_ORDER_RELAXED);
  }
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

  /*
   * Filesystem operations should ignore request query metadata (e.g.
   * `?cs=srgb`) and any leaked resource-key prefixes from async dedupe
   * plumbing.
   */
  String8 query = {0};
  path = vkr_texture_strip_query(path, &query);
  (void)query;
  path = vkr_texture_strip_resource_key_prefix(path);
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

/** Probes the Radiance signature without decoding or trusting an extension. */
vkr_internal bool8_t vkr_texture_probe_hdr_source(VkrAllocator *allocator,
                                                  String8 source_path) {
  char *path_cstr = vkr_texture_path_to_cstr(allocator, source_path);
  if (!path_cstr) {
    return false_v;
  }

  FilePath fp = file_path_create(path_cstr, allocator, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return false_v;
  }

  uint8_t *probe = NULL;
  uint64_t bytes_read = 0;
  const FileError read_error =
      file_read(&fh, allocator, 16u, &bytes_read, &probe);
  file_close(&fh);
  return read_error == FILE_ERROR_NONE && probe && bytes_read >= 11u &&
                 stbi_is_hdr_from_memory(probe, (int)bytes_read)
             ? true_v
             : false_v;
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
vkr_internal bool8_t vkr_texture_decode_hdr_image(
    const uint8_t *file_data, uint64_t file_size, const char *source_cstr,
    VkrTextureDecodeResult *out_result) {
  if (!file_data || file_size == 0u || file_size > INT_MAX || !source_cstr ||
      !out_result) {
    if (out_result) {
      out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }

  int32_t width = 0;
  int32_t height = 0;
  int32_t source_channels = 0;
  stbi_set_flip_vertically_on_load_thread(0);
  float32_t *rgb =
      stbi_loadf_from_memory(file_data, (int)file_size, &width, &height,
                             &source_channels, VKR_TEXTURE_RGB_CHANNELS);
  if (!rgb) {
    const char *reason = stbi_failure_reason();
    log_error("Failed to decode HDR texture '%s': %s", source_cstr,
              reason ? reason : "unknown");
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  uint16_t *rgba16 = NULL;
  VkrTextureUploadRegion *upload_region = NULL;
  bool8_t success = false_v;
  if (width <= 0 || height <= 0 || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION || width != height * 2) {
    log_error("HDR environment '%s' must have a positive 2:1 equirectangular "
              "extent (got %dx%d)",
              source_cstr, width, height);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }

  const uint64_t pixel_count = (uint64_t)width * (uint64_t)height;
  if (pixel_count > SIZE_MAX / (VKR_TEXTURE_RGBA_CHANNELS * sizeof(uint16_t))) {
    out_result->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }

  const uint64_t upload_size =
      pixel_count * VKR_TEXTURE_RGBA_CHANNELS * sizeof(uint16_t);
  rgba16 = (uint16_t *)malloc((size_t)upload_size);
  upload_region =
      (VkrTextureUploadRegion *)malloc(sizeof(VkrTextureUploadRegion));
  if (!rgba16 || !upload_region) {
    out_result->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }

  uint64_t clamped_count = 0u;
  float32_t observed_min = FLT_MAX;
  float32_t observed_max = -FLT_MAX;
  for (uint64_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
    for (uint32_t channel = 0; channel < VKR_TEXTURE_RGB_CHANNELS; ++channel) {
      float32_t value = rgb[pixel_index * VKR_TEXTURE_RGB_CHANNELS + channel];
      if (!isfinite(value)) {
        log_error("HDR environment '%s' contains a non-finite sample at "
                  "pixel %llu channel %u",
                  source_cstr, (unsigned long long)pixel_index, channel);
        out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
        goto cleanup;
      }
      observed_min = Min(observed_min, value);
      observed_max = Max(observed_max, value);
      if (value < 0.0f) {
        value = 0.0f;
        clamped_count++;
      } else if (value > 65504.0f) {
        value = 65504.0f;
        clamped_count++;
      }
      rgba16[pixel_index * VKR_TEXTURE_RGBA_CHANNELS + channel] =
          vkr_float32_to_float16(value);
    }
    rgba16[pixel_index * VKR_TEXTURE_RGBA_CHANNELS + 3u] = 0x3c00u;
  }

  if (clamped_count > 0u) {
    log_warn("HDR environment '%s' clamped %llu radiance samples to [0, "
             "65504] (observed finite min=%g max=%g)",
             source_cstr, (unsigned long long)clamped_count,
             (double)observed_min, (double)observed_max);
  }

  *upload_region = (VkrTextureUploadRegion){
      .mip_level = 0u,
      .array_layer = 0u,
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .depth = 1u,
      .byte_offset = 0u,
      .byte_size = upload_size,
  };
  out_result->upload_data = (uint8_t *)rgba16;
  out_result->upload_data_size = upload_size;
  out_result->upload_regions = upload_region;
  out_result->upload_region_count = 1u;
  out_result->upload_mip_levels = 1u;
  out_result->upload_array_layers = 1u;
  out_result->upload_format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  out_result->upload_is_compressed = false_v;
  out_result->width = width;
  out_result->height = height;
  out_result->original_channels = source_channels;
  out_result->has_transparency = false_v;
  out_result->alpha_mask = false_v;
  out_result->success = true_v;
  rgba16 = NULL;
  upload_region = NULL;
  success = true_v;

cleanup:
  stbi_image_free(rgb);
  if (rgba16) {
    free(rgba16);
  }
  if (upload_region) {
    free(upload_region);
  }
  return success;
}

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

  if (file_size > INT_MAX) {
    log_error("Texture file is too large for stb_image: %s", source_cstr);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (stbi_is_hdr_from_memory(file_data, (int)file_size)) {
    const float64_t start_seconds = vkr_platform_get_absolute_time();
    const bool8_t decoded = vkr_texture_decode_hdr_image(
        file_data, file_size, source_cstr, out_result);
    (void)vkr_metrics_event_record(
        system ? system->hdr_decode_metrics : (VkrMetricEventProducer){0},
        source_path, (uint64_t)(start_seconds * 1000000000.0),
        vkr_metrics_elapsed_ns(start_seconds), file_size,
        decoded ? VKR_METRIC_EVENT_STATUS_SUCCESS
                : VKR_METRIC_EVENT_STATUS_FAILED);
    return decoded;
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
  direct_vkt = vkr_texture_strip_resource_key_prefix(direct_vkt);
  sidecar_vkt = vkr_texture_strip_resource_key_prefix(sidecar_vkt);
  source_path = vkr_texture_strip_resource_key_prefix(source_path);

  if (source_path.str &&
      vkr_texture_probe_hdr_source(scratch_allocator, source_path)) {
    return vkr_texture_decode_from_source_image(
        scratch_allocator, job->system, source_path, false_v, (String8){0},
        false_v, NULL, result);
  }

  if (job->source_only) {
    if (!source_path.str ||
        !vkr_texture_path_exists(scratch_allocator, source_path)) {
      result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }
    return vkr_texture_decode_from_source_image(
        scratch_allocator, job->system, source_path, job->flip_vertical,
        (String8){0}, false_v, NULL, result);
  }

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
      if (vkr_texture_decode_from_ktx2(
              scratch_allocator, job->system, selected_vkt, job->colorspace,
              job->has_explicit_colorspace, job->texture_class,
              job->has_explicit_class, result)) {
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

void vkr_texture_system_release_prepared_load(
    VkrTexturePreparedLoad *prepared) {
  if (!prepared) {
    return;
  }

  if (prepared->upload_data) {
    free(prepared->upload_data);
    prepared->upload_data = NULL;
  }
  if (prepared->upload_regions) {
    free(prepared->upload_regions);
    prepared->upload_regions = NULL;
  }

  prepared->upload_data_size = 0;
  prepared->upload_region_count = 0;
  prepared->upload_mip_levels = 0;
  prepared->upload_array_layers = 0;
  prepared->upload_is_compressed = false_v;
  MemZero(&prepared->description, sizeof(prepared->description));
}

bool8_t vkr_texture_system_prepare_load_from_file(
    VkrTextureSystem *system, String8 file_path, uint32_t desired_channels,
    VkrAllocator *temp_alloc, VkrTexturePreparedLoad *out_prepared,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(file_path.str != NULL, "Path is NULL");
  assert_log(out_prepared != NULL, "Out prepared is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  MemZero(out_prepared, sizeof(*out_prepared));
  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrTextureRequest request = vkr_texture_parse_request(file_path);
  String8 base_path = request.base_path;

  VkrTextureDecodeResult decode_result = {0};
  vkr_texture_decode_result_reset(&decode_result);

  VkrTextureDecodeJobPayload job_payload = {
      .file_path = base_path,
      .desired_channels = desired_channels,
      .flip_vertical = true_v,
      .colorspace = request.colorspace,
      .texture_class = request.texture_class,
      .has_explicit_colorspace = request.has_explicit_colorspace,
      .has_explicit_class = request.has_explicit_class,
      .source_only = request.source_only,
      .system = system,
      .result = &decode_result,
  };

  VkrAllocator *decode_allocator = temp_alloc ? temp_alloc : &system->allocator;
  VkrAllocatorScope decode_scope = vkr_allocator_begin_scope(decode_allocator);
  VkrJobContext fake_ctx = {.system = NULL,
                            .worker_index = 0,
                            .thread_id = 0,
                            .allocator = decode_allocator,
                            .scope = decode_scope};
  vkr_texture_decode_job_run(&fake_ctx, &job_payload);
  vkr_allocator_end_scope(&decode_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);

  const bool8_t has_upload_payload = decode_result.upload_data &&
                                     decode_result.upload_regions &&
                                     decode_result.upload_region_count > 0;
  if (!decode_result.success ||
      (!decode_result.decoded_pixels && !has_upload_payload)) {
    *out_error = decode_result.error;
    vkr_texture_decode_result_release(&decode_result);
    return false_v;
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

  out_prepared->description = (VkrTextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = actual_channels,
      .mip_levels = has_upload_payload ? decode_result.upload_mip_levels : 1u,
      .array_layers =
          has_upload_payload ? decode_result.upload_array_layers : 1u,
      .format = format,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .type =
          has_upload_payload ? decode_result.upload_type : VKR_TEXTURE_TYPE_2D,
      .properties = props,
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT
                           ? VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE
                           : VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = (has_upload_payload && decode_result.upload_mip_levels <= 1)
                        ? VKR_MIP_FILTER_NONE
                        : VKR_MIP_FILTER_LINEAR,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  if (has_upload_payload) {
    out_prepared->upload_data = decode_result.upload_data;
    out_prepared->upload_data_size = decode_result.upload_data_size;
    out_prepared->upload_regions = decode_result.upload_regions;
    out_prepared->upload_region_count = decode_result.upload_region_count;
    out_prepared->upload_mip_levels = decode_result.upload_mip_levels;
    out_prepared->upload_array_layers = decode_result.upload_array_layers;
    out_prepared->upload_is_compressed = decode_result.upload_is_compressed;
    decode_result.upload_data = NULL;
    decode_result.upload_regions = NULL;
  } else {
    uint8_t *decoded_pixels = decode_result.decoded_pixels;
    uint32_t loaded_channels =
        desired_channels > 0 ? desired_channels : (uint32_t)original_channels;
    uint64_t pixel_count = (uint64_t)width * (uint64_t)height;
    uint64_t loaded_image_size = pixel_count * (uint64_t)loaded_channels;
    uint64_t upload_size = pixel_count * (uint64_t)actual_channels;

    uint8_t *upload_data = (uint8_t *)malloc((size_t)upload_size);
    VkrTextureUploadRegion *upload_region =
        (VkrTextureUploadRegion *)malloc(sizeof(VkrTextureUploadRegion));
    if (!upload_data || !upload_region) {
      if (upload_data) {
        free(upload_data);
      }
      if (upload_region) {
        free(upload_region);
      }
      vkr_texture_decode_result_release(&decode_result);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    if (loaded_channels == VKR_TEXTURE_RGB_CHANNELS &&
        actual_channels == VKR_TEXTURE_RGBA_CHANNELS) {
      for (uint64_t pixel_index = 0; pixel_index < pixel_count; pixel_index++) {
        uint64_t src_idx = pixel_index * VKR_TEXTURE_RGB_CHANNELS;
        uint64_t dst_idx = pixel_index * VKR_TEXTURE_RGBA_CHANNELS;
        upload_data[dst_idx + 0] = decoded_pixels[src_idx + 0];
        upload_data[dst_idx + 1] = decoded_pixels[src_idx + 1];
        upload_data[dst_idx + 2] = decoded_pixels[src_idx + 2];
        upload_data[dst_idx + 3] = 255;
      }
    } else {
      uint64_t copy_size =
          loaded_image_size < upload_size ? loaded_image_size : upload_size;
      MemCopy(upload_data, decoded_pixels, (size_t)copy_size);
    }

    *upload_region = (VkrTextureUploadRegion){
        .mip_level = 0,
        .array_layer = 0,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .depth = 1,
        .byte_offset = 0,
        .byte_size = upload_size,
    };

    out_prepared->upload_data = upload_data;
    out_prepared->upload_data_size = upload_size;
    out_prepared->upload_regions = upload_region;
    out_prepared->upload_region_count = 1;
    out_prepared->upload_mip_levels = 1;
    out_prepared->upload_array_layers = 1;
    out_prepared->upload_is_compressed = false_v;
  }

  vkr_texture_decode_result_release(&decode_result);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_texture_system_finalize_prepared_load(
    VkrTextureSystem *system, String8 name,
    const VkrTexturePreparedLoad *prepared, VkrTextureHandle *out_handle,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(prepared != NULL, "Prepared payload is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  name = vkr_texture_strip_resource_key_prefix(name);
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  *out_error = VKR_RENDERER_ERROR_NONE;

  if (!prepared->upload_data || !prepared->upload_regions ||
      prepared->upload_region_count == 0 || prepared->upload_data_size == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrTextureEntry *existing_entry = vkr_hash_table_get_VkrTextureEntry(
      &system->texture_map, (const char *)name.str);
  if (existing_entry) {
    /* The async request is not an owner. Its consumer retains this canonical
       handle after resolving the request. */
    VkrTexture *existing_texture =
        &system->textures.data[existing_entry->index];
    *out_handle = (VkrTextureHandle){
        .id = existing_texture->description.id,
        .generation = existing_texture->description.generation,
    };
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  const VkrAssetPublisher *publisher = system->asset_publisher;
  if (publisher->texture_upload_available &&
      !publisher->texture_upload_available(publisher->state,
                                           prepared->upload_data_size)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_BUSY;
    return false_v;
  }

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->string_allocator, name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = prepared->description;
  texture->resident_bytes = prepared->upload_data_size;
  texture->description.id = free_slot_index + 1;
  if (texture->description.generation == VKR_INVALID_ID) {
    texture->description.generation = system->generation_counter++;
  }

  VkrTextureHandle logical_handle = {
      .id = texture->description.id,
      .generation = texture->description.generation,
  };
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_publish_prepared(system, logical_handle, prepared,
                                           &texture->handle, &renderer_error)) {
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(texture, sizeof(*texture));
    texture->description.id = VKR_INVALID_ID;
    texture->description.generation = VKR_INVALID_ID;
    if (free_slot_index < system->next_free_index) {
      system->next_free_index = free_slot_index;
    }
    *out_error = renderer_error;
    return false_v;
  }

  VkrTextureEntry new_entry = {
      .index = free_slot_index,
      .ref_count = 0,
      .auto_release = true_v,
      .name = stable_key,
  };
  if (!vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                             new_entry)) {
    vkr_allocator_free(&system->string_allocator, stable_key, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    const bool8_t destroyed = vkr_texture_destroy(system, texture);
    if (destroyed && free_slot_index < system->next_free_index) {
      system->next_free_index = free_slot_index;
    }
    if (!destroyed) {
      log_warn("Texture %u:%u remains owned after map insertion rollback",
               logical_handle.id, logical_handle.generation);
    }
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  system->texture_keys_by_index[free_slot_index] = stable_key;

  *out_handle = logical_handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_texture_system_load(VkrTextureSystem *system, String8 name,
                                VkrTextureHandle *out_handle,
                                VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  name = vkr_texture_strip_resource_key_prefix(name);
  VkrTexturePreparedLoad prepared = {0};
  if (!vkr_texture_system_prepare_load_from_file(
          system, name, VKR_TEXTURE_RGBA_CHANNELS, &system->allocator,
          &prepared, out_error)) {
    return false_v;
  }

  bool8_t result = vkr_texture_system_finalize_prepared_load(
      system, name, &prepared, out_handle, out_error);
  vkr_texture_system_release_prepared_load(&prepared);
  return result;
}

uint32_t vkr_texture_system_load_batch(VkrTextureSystem *system,
                                       const String8 *paths, uint32_t count,
                                       VkrTextureHandle *out_handles,
                                       VkrRendererError *out_errors) {
  assert_log(system != NULL, "System is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; ++i) {
    bool8_t duplicate = false_v;
    for (uint32_t previous = 0; previous < i; ++previous) {
      if (string8_equals(&paths[i], &paths[previous])) {
        out_handles[i] = out_handles[previous];
        out_errors[i] = out_errors[previous];
        duplicate = true_v;
        break;
      }
    }
    if (!duplicate) {
      out_handles[i] = VKR_TEXTURE_HANDLE_INVALID;
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
      (void)vkr_texture_system_load(system, paths[i], &out_handles[i],
                                    &out_errors[i]);
    }
    loaded += out_handles[i].id != 0 ? 1u : 0u;
  }
  return loaded;
}
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
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return NULL;
  }
  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_err = file_read_all(&fh, allocator, &file_data, &file_size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !file_data || file_size == 0) {
    return NULL;
  }
  stbi_set_flip_vertically_on_load_thread(0);
  int32_t channels = 0;
  return stbi_load_from_memory(file_data, (int)file_size, out_width, out_height,
                               &channels, 4);
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
  vkr_local_persist const char *face_suffixes[6] = {"_r", "_l", "_u",
                                                    "_d", "_f", "_b"};

  VkrAllocator *temp_alloc = &system->allocator;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  uint64_t key_buffer_size = base_path.length + 16;
  char *key_buffer = (char *)vkr_allocator_alloc(
      temp_alloc, key_buffer_size, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!key_buffer) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  snprintf(key_buffer, key_buffer_size, "%.*s_cube", (int)base_path.length,
           base_path.str);

  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, key_buffer);
  if (existing_entry) {
    if (existing_entry->ref_count == 0) {
      existing_entry->auto_release = false_v;
    }
    existing_entry->ref_count++;
    VkrTexture *existing_texture =
        &system->textures.data[existing_entry->index];
    *out_handle = (VkrTextureHandle){
        .id = existing_texture->description.id,
        .generation = existing_texture->description.generation};
    *out_error = VKR_RENDERER_ERROR_NONE;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return true_v;
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
      .mip_levels = 1u,
      .array_layers = 6u,
      // LDR cubemap source faces (jpg/png) are authored in sRGB space.
      // Sampling through an sRGB format ensures bake shaders receive linear
      // radiance values.
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
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

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Create stable key for the cube map
  char *stable_key = (char *)vkr_allocator_alloc(
      &system->string_allocator, base_path.length + 16,
      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
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
  texture->resident_bytes = total_size;
  texture->description.id = free_slot_index + 1;
  texture->description.generation = system->generation_counter++;
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  {
    VkrTextureUploadRegion upload_regions[6];
    for (uint32_t face = 0; face < ArrayCount(upload_regions); ++face) {
      upload_regions[face] = (VkrTextureUploadRegion){
          .mip_level = 0,
          .array_layer = face,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .depth = 1,
          .byte_offset = (uint64_t)face * face_size,
          .byte_size = face_size,
      };
    }
    const VkrTexturePreparedLoad prepared = {
        .description = texture->description,
        .upload_data = cube_data,
        .upload_data_size = total_size,
        .upload_regions = upload_regions,
        .upload_region_count = ArrayCount(upload_regions),
        .upload_mip_levels = 1,
        .upload_array_layers = 6,
        .upload_is_compressed = false_v,
    };
    const VkrTextureHandle logical_handle = {
        .id = texture->description.id,
        .generation = texture->description.generation,
    };
    (void)vkr_texture_system_publish_prepared(
        system, logical_handle, &prepared, &texture->handle, &renderer_error);
  }
  if (renderer_error != VKR_RENDERER_ERROR_NONE || !texture->handle) {
    log_error("Failed to create cube map texture in backend");
    vkr_allocator_free(&system->string_allocator, stable_key,
                       base_path.length + 16, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(texture, sizeof(*texture));
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = renderer_error != VKR_RENDERER_ERROR_NONE
                     ? renderer_error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
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
    if (!vkr_texture_destroy(system, texture)) {
      log_warn("Cube map %u:%u remains owned after map insertion rollback",
               texture->description.id, texture->description.generation);
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  system->texture_keys_by_index[free_slot_index] = stable_key;

  *out_handle =
      (VkrTextureHandle){.id = texture->description.id,
                         .generation = texture->description.generation};
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  log_debug("Loaded cube map texture: %s (%dx%d)", stable_key, width, height);

  return true_v;
}
