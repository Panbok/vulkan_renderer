#include "renderer/systems/vkr_texture_system.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_asset_path.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

#include "ktx.h"
#include "stb_image.h"

#include <ctype.h>
#include <limits.h>

// =============================================================================
// Texture Cache Format
// =============================================================================
// Binary cache format for decoded textures to skip stb_image decoding on
// subsequent loads. Cache files are stored alongside source files with .vkt
// extension.

#define VKR_TEXTURE_CACHE_EXT ".vkt"
#define VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_INITIAL MB(1)
#define VKR_TEXTURE_SYSTEM_ASYNC_DMEMORY_RESERVE MB(16)
/* Vulkan VkFormat 97, recorded directly in KTX2 without a Vulkan dependency. */
#define VKR_KTX2_VK_FORMAT_R16G16B16A16_SFLOAT 97u
#define VKR_KTX2_VK_FORMAT_BC5_UNORM_BLOCK 141u
#define VKR_KTX2_VK_FORMAT_BC7_UNORM_BLOCK 145u
#define VKR_KTX2_VK_FORMAT_BC7_SRGB_BLOCK 146u
#define VKR_KTX2_VK_FORMAT_ASTC_4x4_UNORM_BLOCK 157u
#define VKR_KTX2_VK_FORMAT_ASTC_4x4_SRGB_BLOCK 158u
#define VKR_KTX2_VK_FORMAT_ASTC_6x6_UNORM_BLOCK 165u
#define VKR_KTX2_VK_FORMAT_ASTC_6x6_SRGB_BLOCK 166u

vkr_internal String8 vkr_texture_strip_resource_key_prefix(String8 name);

// Generate cache path from source path (e.g., "textures/foo.png" ->
// "textures/foo.png.vkt")
vkr_internal String8 vkr_texture_cache_path(VkrAllocator *allocator,
                                            String8 source_path) {
  assert_log(allocator != NULL, "Allocator is NULL");
  String8 query = {0};
  source_path = string8_split_query(source_path, &query);
  (void)query;
  source_path = vkr_texture_strip_resource_key_prefix(source_path);
  return string8_create_formatted(allocator, "%.*s%s",
                                  (int32_t)source_path.length, source_path.str,
                                  VKR_TEXTURE_CACHE_EXT);
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
  while (string8_query_next_pair(query, &cursor, &key, &value)) {
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
  while (string8_query_next_pair(query, &cursor, &key, &value)) {
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
  while (string8_query_next_pair(query, &cursor, &key, &value)) {
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
  String8 base_path = string8_split_query(name, &query);
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
  String8 base_path = string8_split_query(path, &query);
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

bool8_t vkr_texture_request_prefers_srgb(String8 request_path,
                                         bool8_t default_srgb) {
  String8 query = {0};
  (void)string8_split_query(request_path, &query);
  return vkr_texture_scan_query_colorspace(query, default_srgb, false_v)
      .prefers_srgb;
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

vkr_internal uint32_t
vkr_texture_channel_count_from_format(VkrTextureFormat format) {
  VkrTextureFormatInfo info = {0};
  return vkr_texture_format_get_info(format, &info)
             ? (uint32_t)info.channel_count
             : VKR_TEXTURE_RGBA_CHANNELS;
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
  const VkrRendererError publish_error =
      system->asset_publisher->publish_texture(system->asset_publisher->state,
                                               logical_handle, prepared);
  if (publish_error != VKR_RENDERER_ERROR_NONE) {
    *out_error = publish_error;
    return false_v;
  }
  VkrTexture *texture = &system->textures.data[logical_handle.id - 1];
  vkr_publication_state_recorded_by(system->asset_publisher,
                                    &texture->publication);
  *out_backend_handle = (VkrTextureOpaqueHandle)texture;
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

  out_system->supports_texture_astc_4x4 =
      device_info->supports_texture_astc_4x4;
  out_system->supports_texture_bc7 = device_info->supports_texture_bc7;
  out_system->supports_texture_bc5 = device_info->supports_texture_bc5;

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

/* Requests may carry `?cs=...` metadata that the map key omits, so a miss
 * retries the query-less path. `out_key`, when given, receives the view that
 * matched. */
vkr_internal VkrTextureEntry *
vkr_texture_system_find_entry(VkrTextureSystem *system, String8 name,
                              String8 *out_key) {
  VkrTextureEntry *entry =
      vkr_hash_table_get_string8_VkrTextureEntry(&system->texture_map, name);
  String8 key = name;
  if (!entry) {
    String8 query = {0};
    key = string8_split_query(name, &query);
    if (!key.str || key.length == 0 || key.length >= name.length) {
      return NULL;
    }
    entry =
        vkr_hash_table_get_string8_VkrTextureEntry(&system->texture_map, key);
  }

  if (entry && out_key) {
    *out_key = key;
  }
  return entry;
}

VkrTextureHandle vkr_texture_system_acquire(VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  texture_name = vkr_texture_strip_resource_key_prefix(texture_name);
  VkrTextureEntry *entry =
      vkr_texture_system_find_entry(system, texture_name, NULL);
  if (entry) {
    if (entry->ref_count == 0) {
      entry->auto_release = auto_release;
    }
    entry->ref_count++;
    *out_error = VKR_RENDERER_ERROR_NONE;
    VkrTexture *texture = &system->textures.data[entry->index];
    VkrTextureHandle handle = {.id = texture->description.id,
                               .generation = texture->description.generation};
    return handle;
  }

  // Async loading intentionally exposes not-ready states; keep this diagnostic
  // low-noise to avoid flooding logs while dependencies converge.
  log_debug("Texture '%.*s' not yet loaded, use resource system to load first",
            (int)texture_name.length, texture_name.str);
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
  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_string8_VkrTextureEntry(&system->texture_map, name);
  if (existing_entry) {
    log_error("Texture with name '%.*s' already exists", (int)name.length,
              name.str);
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
    vkr_publication_state_recorded_by(system->asset_publisher,
                                      &texture->publication);
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

/* Final destruction belongs to the texture system. Loader unload releases
 * its acquired reference and never dispatches back through the resource table.
 */
vkr_internal bool8_t vkr_texture_system_destroy_unreferenced(
    VkrTextureSystem *system, VkrTextureEntry *entry) {
  const uint32_t texture_index = entry->index;
  if (texture_index == system->default_texture.id - 1u)
    return true_v;
  VkrTexture *texture = &system->textures.data[texture_index];
  const char *stable_name = entry->name;
  if (!vkr_texture_destroy(system, texture)) {
    log_warn("Texture '%s' remains registered because GPU destruction failed",
             stable_name);
    return false_v;
  }
  const bool8_t removed =
      vkr_hash_table_remove_VkrTextureEntry(&system->texture_map, stable_name);
  assert_log(removed, "Texture lookup changed during native destruction");
  texture->description.id = VKR_INVALID_ID;
  texture->description.generation = VKR_INVALID_ID;
  system->texture_keys_by_index[texture_index] = NULL;
  if (vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_name)) {
    vkr_allocator_free(&system->string_allocator, (void *)stable_name,
                       string_length(stable_name) + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  system->next_free_index = Min(system->next_free_index, texture_index);
  return true_v;
}

bool8_t vkr_texture_system_release(VkrTextureSystem *system,
                                   String8 texture_name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(texture_name.str != NULL, "Name is NULL");

  texture_name = vkr_texture_strip_resource_key_prefix(texture_name);
  String8 texture_key = texture_name;
  VkrTextureEntry *entry =
      vkr_texture_system_find_entry(system, texture_name, &texture_key);
  if (!entry) {
    /*
     * Async load/cancel/release ordering can legitimately race texture
     * teardown, so a missing key here is not a correctness failure.
     */
    log_debug("Texture '%.*s' already released before texture-system release",
              (int)texture_key.length, texture_key.str);
    return true_v;
  }

  if (entry->ref_count > 0) {
    entry->ref_count--;
  } else if (!entry->auto_release) {
    log_warn("Over-release detected for texture '%.*s'",
             (int)texture_key.length, texture_key.str);
    return false_v;
  }

  bool8_t released = true_v;
  if (entry->ref_count == 0 && entry->auto_release)
    released = vkr_texture_system_destroy_unreferenced(system, entry);
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

  /* The sampler it already has, such as a restored filter mode at startup,
     needs no publication. */
  const VkrTextureDescription *current = &texture->description;
  if (current->min_filter == min_filter && current->mag_filter == mag_filter &&
      current->mip_filter == mip_filter &&
      current->anisotropy_enable == anisotropy_enable &&
      current->u_repeat_mode == u_repeat_mode &&
      current->v_repeat_mode == v_repeat_mode &&
      current->w_repeat_mode == w_repeat_mode) {
    return VKR_RENDERER_ERROR_NONE;
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

bool8_t vkr_texture_system_publication_confirmed(VkrTextureSystem *system,
                                                 VkrTextureHandle handle) {
  const VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  return texture && texture->handle &&
         vkr_publication_state_confirmed(&texture->publication);
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
/* `decoded_pixels` pairs with stbi_image_free. Upload bytes follow the
 * VkrTexturePreparedLoad heap contract. */
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

/** State of one KTX2 decode. The file bytes and request are borrowed. The
 * decode always destroys `ktx_texture`; it frees the malloc'd `upload_data`
 * and `upload_regions` unless it succeeds and the result owns them. */
typedef struct VkrTextureKtx2Decode {
  VkrAllocator *allocator;
  VkrTextureSystem *system;
  String8 vkt_path;
  const char *path_cstr;
  /* The request's sampling intent; see vkr_texture_native_view_format. */
  VkrTextureColorSpace colorspace;
  bool8_t has_explicit_colorspace;
  VkrTextureClass texture_class;
  bool8_t has_explicit_class;
  VkrTextureDecodeResult *out_result;
  ktxTexture2 *ktx_texture;
  ktxTexture *base_texture;
  bool8_t cubemap;
  uint32_t face_count;
  uint32_t physical_layers;
  VkrTextureType texture_type;
  uint8_t *upload_data;
  VkrTextureUploadRegion *upload_regions;
} VkrTextureKtx2Decode;

/* Rejects KTX2 shapes the renderer cannot upload and records the face count,
 * physical layer count and texture type of an accepted one. */
vkr_internal bool8_t
vkr_texture_ktx2_validate_shape(VkrTextureKtx2Decode *decode) {
  ktxTexture *base_texture = decode->base_texture;
  VkrTextureDecodeResult *out_result = decode->out_result;
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
              decode->path_cstr, base_texture->numDimensions,
              base_texture->numLayers, face_count, base_texture->isCubemap);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
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
    return false_v;
  }

  decode->cubemap = cubemap;
  decode->face_count = face_count;
  decode->physical_layers = physical_layers;
  decode->texture_type = texture_type;
  return true_v;
}

/* Copies an uncompressed RGBA16F six-face cubemap into upload storage without
 * transcoding and hands that storage to the result. */
vkr_internal bool8_t
vkr_texture_ktx2_decode_direct(VkrTextureKtx2Decode *decode) {
  ktxTexture *base_texture = decode->base_texture;
  const char *path_cstr = decode->path_cstr;
  const uint32_t face_count = decode->face_count;
  VkrTextureDecodeResult *out_result = decode->out_result;
  const uint64_t direct_region_count_u64 =
      (uint64_t)base_texture->numLevels * decode->physical_layers;
  if (decode->ktx_texture->vkFormat != VKR_KTX2_VK_FORMAT_R16G16B16A16_SFLOAT ||
      !decode->cubemap || base_texture->numLayers != 1u ||
      base_texture->baseDepth != 1u || direct_region_count_u64 == 0u ||
      direct_region_count_u64 > VKR_TEXTURE_MAX_UPLOAD_REGIONS) {
    log_error("Unsupported direct KTX2 texture '%s': expected uncompressed "
              "R16G16B16A16_SFLOAT with one six-face cubemap layer",
              path_cstr);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  uint8_t *direct_data = ktxTexture_GetData(base_texture);
  const ktx_size_t direct_data_size = ktxTexture_GetDataSize(base_texture);
  if (!direct_data || direct_data_size == 0u) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  const uint32_t direct_region_count = (uint32_t)direct_region_count_u64;
  uint8_t *upload_data = (uint8_t *)malloc((size_t)direct_data_size);
  VkrTextureUploadRegion *upload_regions = (VkrTextureUploadRegion *)malloc(
      sizeof(VkrTextureUploadRegion) * direct_region_count);
  decode->upload_data = upload_data;
  decode->upload_regions = upload_regions;
  if (!upload_data || !upload_regions) {
    out_result->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(upload_data, direct_data, (size_t)direct_data_size);

  uint64_t expected_data_size = 0u;
  uint32_t region_index = 0u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    for (uint32_t mip = 0u; mip < base_texture->numLevels; ++mip) {
      const uint32_t mip_width = Max(1u, base_texture->baseWidth >> mip);
      const uint32_t mip_height = Max(1u, base_texture->baseHeight >> mip);
      const uint64_t expected_image_size =
          (uint64_t)mip_width * mip_height * 8u;
      ktx_size_t image_offset = 0u;
      const ktxResult ktx_result =
          ktxTexture_GetImageOffset(base_texture, mip, 0u, face, &image_offset);
      const ktx_size_t image_size = ktxTexture_GetImageSize(base_texture, mip);
      if (ktx_result != KTX_SUCCESS || image_size != expected_image_size ||
          image_offset > direct_data_size ||
          image_size > direct_data_size - image_offset ||
          expected_data_size > UINT64_MAX - image_size) {
        log_error("Invalid direct RGBA16F KTX2 image layout for '%s' "
                  "(face=%u mip=%u)",
                  path_cstr, face, mip);
        out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      expected_data_size += image_size;
      upload_regions[region_index++] = (VkrTextureUploadRegion){
          .mip_level = mip,
          .array_layer = face,
          .width = mip_width,
          .height = mip_height,
          .depth = 1u,
          .byte_offset = image_offset,
          .byte_size = image_size,
      };
    }
  }
  if (expected_data_size != direct_data_size) {
    log_error("Invalid direct RGBA16F KTX2 data size for '%s' "
              "(expected=%llu actual=%llu)",
              path_cstr, (unsigned long long)expected_data_size,
              (unsigned long long)direct_data_size);
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  out_result->upload_data = upload_data;
  out_result->upload_data_size = direct_data_size;
  out_result->upload_regions = upload_regions;
  out_result->upload_region_count = direct_region_count;
  out_result->upload_mip_levels = base_texture->numLevels;
  out_result->upload_array_layers = face_count;
  out_result->upload_format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  out_result->upload_type = VKR_TEXTURE_TYPE_CUBE_MAP;
  out_result->upload_is_compressed = false_v;
  out_result->width = (int32_t)base_texture->baseWidth;
  out_result->height = (int32_t)base_texture->baseHeight;
  out_result->original_channels = VKR_TEXTURE_RGBA_CHANNELS;
  out_result->has_transparency = false_v;
  out_result->alpha_mask = false_v;
  out_result->success = true_v;
  decode->upload_data = NULL;
  decode->upload_regions = NULL;
  return true_v;
}

/* Copies every mip, layer and face of the texture's device-ready payload into
 * upload storage owned by the decode. */
vkr_internal bool8_t vkr_texture_ktx2_copy_upload(VkrTextureKtx2Decode *decode,
                                                  ktx_size_t *out_data_size,
                                                  uint32_t *out_region_count) {
  ktxTexture *base_texture = decode->base_texture;
  const uint32_t face_count = decode->face_count;
  VkrTextureDecodeResult *out_result = decode->out_result;
  ktxResult ktx_result = KTX_SUCCESS;
  uint8_t *ktx_data = ktxTexture_GetData(base_texture);
  ktx_size_t ktx_data_size = ktxTexture_GetDataSize(base_texture);
  if (!ktx_data || ktx_data_size == 0) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  const uint64_t region_count_u64 =
      (uint64_t)base_texture->numLevels * decode->physical_layers;
  if (region_count_u64 == 0u ||
      region_count_u64 > VKR_TEXTURE_MAX_UPLOAD_REGIONS) {
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  const uint32_t region_count = (uint32_t)region_count_u64;

  uint8_t *upload_data = (uint8_t *)malloc((size_t)ktx_data_size);
  VkrTextureUploadRegion *upload_regions = (VkrTextureUploadRegion *)malloc(
      sizeof(VkrTextureUploadRegion) * region_count);
  decode->upload_data = upload_data;
  decode->upload_regions = upload_regions;
  if (!upload_data || !upload_regions) {
    out_result->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
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
          return false_v;
        }

        const ktx_size_t image_size =
            ktxTexture_GetImageSize(base_texture, mip);
        if (image_offset + image_size > ktx_data_size) {
          out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
          return false_v;
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

  *out_data_size = ktx_data_size;
  *out_region_count = region_count;
  return true_v;
}

/* Block formats a workspace import stores for its host (ADR-012): native
 * ASTC 4x4 and 6x6 on ASTC hosts, BC7 and BC5 on BC hosts. */
typedef struct VkrTextureNativeBlockFormat {
  uint32_t vk_format;
  VkrTextureFormat format;
  const char *name;
} VkrTextureNativeBlockFormat;

vkr_global const VkrTextureNativeBlockFormat
    vkr_texture_native_block_formats[] = {
        {VKR_KTX2_VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
         VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM, "ASTC 4x4"},
        {VKR_KTX2_VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
         VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB, "ASTC 4x4"},
        {VKR_KTX2_VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
         VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM, "ASTC 6x6"},
        {VKR_KTX2_VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
         VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB, "ASTC 6x6"},
        {VKR_KTX2_VK_FORMAT_BC7_UNORM_BLOCK, VKR_TEXTURE_FORMAT_BC7_UNORM,
         "BC7"},
        {VKR_KTX2_VK_FORMAT_BC7_SRGB_BLOCK, VKR_TEXTURE_FORMAT_BC7_SRGB, "BC7"},
        {VKR_KTX2_VK_FORMAT_BC5_UNORM_BLOCK, VKR_TEXTURE_FORMAT_BC5_UNORM,
         "BC5"},
};

vkr_internal const VkrTextureNativeBlockFormat *
vkr_texture_native_block_format(uint32_t vk_format) {
  for (uint32_t i = 0; i < ArrayCount(vkr_texture_native_block_formats); ++i) {
    if (vkr_texture_native_block_formats[i].vk_format == vk_format) {
      return &vkr_texture_native_block_formats[i];
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_texture_system_samples_format(
    const VkrTextureSystem *system, VkrTextureFormat format) {
  switch (format) {
  /* ASTC LDR support covers every block size on Metal and Vulkan. */
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB:
    return system->supports_texture_astc_4x4;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return system->supports_texture_bc7;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return system->supports_texture_bc5;
  default:
    return false_v;
  }
}

/* The sRGB or UNORM variant of a native block format that a request samples
 * through: an explicit `cs=` names the colour space and an explicit `tc=` the
 * class, else the file's own format stands for both. Only a colour-sRGB class
 * in sRGB samples the sRGB variant; normals and data stay UNORM. The blocks
 * are the same in both variants. */
vkr_internal VkrTextureFormat vkr_texture_native_view_format(
    const VkrTextureKtx2Decode *decode, VkrTextureFormat stored) {
  VkrTextureFormat unorm = stored;
  VkrTextureFormat srgb = stored;
  switch (stored) {
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    unorm = VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
    srgb = VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB;
    break;
  case VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB:
    unorm = VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM;
    srgb = VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB;
    break;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    unorm = VKR_TEXTURE_FORMAT_BC7_UNORM;
    srgb = VKR_TEXTURE_FORMAT_BC7_SRGB;
    break;
  default:
    return stored;
  }
  if (!decode->has_explicit_colorspace && !decode->has_explicit_class) {
    return stored;
  }
  const bool8_t stored_srgb = stored == srgb;
  const bool8_t colour_srgb =
      decode->has_explicit_class
          ? decode->texture_class == VKR_TEXTURE_CLASS_COLOR_SRGB
          : stored_srgb;
  const bool8_t sample_srgb =
      decode->has_explicit_colorspace
          ? decode->colorspace == VKR_TEXTURE_COLORSPACE_SRGB
          : stored_srgb;
  return colour_srgb && sample_srgb ? srgb : unorm;
}

/* Uploads a native block payload as stored, through the sRGB or UNORM view
 * the request samples. A device that cannot sample the format refuses it: the
 * asset must be rebuilt on this platform. */
vkr_internal bool8_t vkr_texture_ktx2_decode_native(
    VkrTextureKtx2Decode *decode, const VkrTextureNativeBlockFormat *native) {
  ktxTexture *base_texture = decode->base_texture;
  VkrTextureDecodeResult *out_result = decode->out_result;
  if (!vkr_texture_system_samples_format(decode->system, native->format)) {
    log_error("Texture '%s' holds %s blocks, which this device cannot "
              "sample; rebuild the asset on this platform",
              decode->path_cstr, native->name);
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  ktx_size_t data_size = 0;
  uint32_t region_count = 0;
  if (!vkr_texture_ktx2_copy_upload(decode, &data_size, &region_count)) {
    return false_v;
  }
  const VkrTextureFormat format =
      vkr_texture_native_view_format(decode, native->format);
  out_result->upload_data = decode->upload_data;
  out_result->upload_data_size = data_size;
  out_result->upload_regions = decode->upload_regions;
  out_result->upload_region_count = region_count;
  out_result->upload_mip_levels = base_texture->numLevels;
  out_result->upload_array_layers = decode->physical_layers;
  out_result->upload_format = format;
  out_result->upload_type = decode->texture_type;
  out_result->upload_is_compressed = true_v;
  out_result->width = (int32_t)base_texture->baseWidth;
  out_result->height = (int32_t)base_texture->baseHeight;
  out_result->original_channels =
      (int32_t)vkr_texture_channel_count_from_format(format);
  out_result->has_transparency = vkr_texture_ktx_metadata_bool(
      base_texture, "vkr.has_transparency", false_v);
  out_result->alpha_mask =
      vkr_texture_ktx_metadata_bool(base_texture, "vkr.alpha_mask", false_v);
  out_result->success = true_v;
  decode->upload_data = NULL;
  decode->upload_regions = NULL;
  return true_v;
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

  FilePath fp = vkr_asset_path_file(
      allocator,
      string8_create_from_cstr((const uint8_t *)path_cstr, strlen(path_cstr)));
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

  VkrTextureKtx2Decode decode = {
      .allocator = allocator,
      .system = system,
      .vkt_path = vkt_path,
      .path_cstr = path_cstr,
      .colorspace = colorspace,
      .has_explicit_colorspace = has_explicit_colorspace,
      .texture_class = texture_class,
      .has_explicit_class = has_explicit_class,
      .out_result = out_result,
      .ktx_texture = NULL,
      .base_texture = NULL,
      .upload_data = NULL,
      .upload_regions = NULL,
  };
  bool8_t success = false_v;

  ktxResult ktx_result = ktxTexture2_CreateFromMemory(
      file_data, (ktx_size_t)file_size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
      &decode.ktx_texture);
  if (ktx_result != KTX_SUCCESS || !decode.ktx_texture) {
    log_error("Failed to parse KTX2 texture '%s': %s", path_cstr,
              ktxErrorString(ktx_result));
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }

  decode.base_texture = ktxTexture(decode.ktx_texture);
  if (!vkr_texture_ktx2_validate_shape(&decode)) {
    goto cleanup;
  }

  const VkrTextureNativeBlockFormat *native =
      vkr_texture_native_block_format(decode.ktx_texture->vkFormat);
  if (native) {
    success = vkr_texture_ktx2_decode_native(&decode, native);
  } else if (!ktxTexture2_NeedsTranscoding(decode.ktx_texture)) {
    success = vkr_texture_ktx2_decode_direct(&decode);
  } else {
    log_error("Texture '%s' holds Basis blocks, which the runtime no longer "
              "transcodes; rebuild it with vkr_bakery on this host",
              path_cstr);
    out_result->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

cleanup:
  if (decode.ktx_texture) {
    ktxTexture2_Destroy(decode.ktx_texture);
  }
  if (!success) {
    if (decode.upload_data) {
      free(decode.upload_data);
    }
    if (decode.upload_regions) {
      free(decode.upload_regions);
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
  path = string8_split_query(path, &query);
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

  FilePath fp = vkr_asset_path_file(
      allocator,
      string8_create_from_cstr((const uint8_t *)path_cstr, strlen(path_cstr)));
  return file_exists(&fp);
}

/**
 * @brief Decodes a source image file: the `source=only` requests of
 * generated UI images and the faces of a cubemap source.
 */
vkr_internal bool8_t vkr_texture_decode_from_source_image(
    VkrAllocator *allocator, String8 source_path, bool8_t flip_vertical,
    VkrTextureDecodeResult *out_result) {
  char *source_cstr = vkr_texture_path_to_cstr(allocator, source_path);
  if (!source_cstr) {
    out_result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  FilePath source_fp = vkr_asset_path_file(
      allocator, string8_create_from_cstr((const uint8_t *)source_cstr,
                                          strlen(source_cstr)));

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

  /* Radiance HDR sources only ever fed global environments, which the
     atmosphere replaced (ADR-058). Reject them instead of an 8-bit decode. */
  if (stbi_is_hdr_from_memory(file_data, (int)file_size)) {
    log_error("Radiance HDR texture '%s' is unsupported; author $.atmosphere "
              "or $.environment.constant for sky lighting",
              source_cstr);
    out_result->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
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

  if (job->source_only) {
    if (!source_path.str ||
        !vkr_texture_path_exists(scratch_allocator, source_path)) {
      result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }
    return vkr_texture_decode_from_source_image(scratch_allocator, source_path,
                                                job->flip_vertical, result);
  }

  /* A request loads its cooked host-native `.vkt`: the path itself when it
     names one, else the source's `<source>.vkt` sidecar. */
  const String8 selected_vkt = direct_vkt.str ? direct_vkt : sidecar_vkt;
  if (!selected_vkt.str ||
      !vkr_texture_path_exists(scratch_allocator, selected_vkt)) {
    char *requested = vkr_texture_path_to_cstr(
        scratch_allocator, selected_vkt.str ? selected_vkt : source_path);
    log_error("Texture '%s' has no cooked `.vkt`; build its assets with "
              "vkr_bakery on this host",
              requested ? requested : "");
    result->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }
  return vkr_texture_decode_from_ktx2(
      scratch_allocator, job->system, selected_vkt, job->colorspace,
      job->has_explicit_colorspace, job->texture_class, job->has_explicit_class,
      result);
}

/* Mip regions a capped 2D load can hold: one per level of the largest
 * supported extent. */
#define VKR_TEXTURE_CAP_MAX_REGIONS 32u
/* The 16-byte block size bounds the alignment the kept images retain. */
#define VKR_TEXTURE_CAP_REGION_ALIGNMENT_MAX 16u

/* Drops the mips of a decoded 2D chain whose extent exceeds `max_dimension`,
 * so the texture loads as its first mip at or below the limit. The kept mips
 * move to the front of the upload bytes in offset order. Cubemaps, arrays,
 * single-level images and loads within the limit stay unchanged; the
 * smallest mip always remains. */
vkr_internal void
vkr_texture_decode_result_cap_extent(VkrTextureDecodeResult *result,
                                     uint32_t max_dimension) {
  if (max_dimension == 0u || result->upload_type != VKR_TEXTURE_TYPE_2D ||
      result->upload_array_layers != 1u || result->upload_mip_levels <= 1u ||
      result->upload_region_count != result->upload_mip_levels ||
      result->upload_region_count > VKR_TEXTURE_CAP_MAX_REGIONS ||
      result->width <= 0 || result->height <= 0) {
    return;
  }

  const uint32_t width = (uint32_t)result->width;
  const uint32_t height = (uint32_t)result->height;
  uint32_t skip = 0u;
  while (skip + 1u < result->upload_mip_levels &&
         Max(Max(1u, width >> skip), Max(1u, height >> skip)) > max_dimension) {
    ++skip;
  }
  if (skip == 0u) {
    return;
  }

  /* Validate before changing anything, so a mismatched payload still loads
   * every mip. The kept images keep the largest power-of-two alignment, up
   * to the block size, that all their decoded offsets share. */
  VkrTextureUploadRegion *regions = result->upload_regions;
  uint32_t kept_count = 0u;
  uint64_t alignment = VKR_TEXTURE_CAP_REGION_ALIGNMENT_MAX;
  for (uint32_t i = 0u; i < result->upload_region_count; ++i) {
    if (regions[i].mip_level >= result->upload_mip_levels) {
      kept_count = 0u;
      break;
    }
    if (regions[i].mip_level >= skip) {
      ++kept_count;
      while (alignment > 1u && regions[i].byte_offset % alignment != 0u) {
        alignment /= 2u;
      }
    }
  }
  if (kept_count != result->upload_mip_levels - skip) {
    log_error("Texture mip regions do not match the mip count; loading "
              "every mip");
    return;
  }

  kept_count = 0u;
  for (uint32_t i = 0u; i < result->upload_region_count; ++i) {
    if (regions[i].mip_level < skip) {
      continue;
    }
    regions[kept_count] = regions[i];
    regions[kept_count].mip_level -= skip;
    ++kept_count;
  }

  /* Insertion sort by byte offset. Every decoded offset is a multiple of
   * `alignment`, so the aligned cursor never passes the next image's start:
   * moving the images in offset order never overwrites one not yet moved. */
  uint32_t order[VKR_TEXTURE_CAP_MAX_REGIONS];
  for (uint32_t i = 0u; i < kept_count; ++i) {
    uint32_t slot = i;
    while (slot > 0u &&
           regions[order[slot - 1u]].byte_offset > regions[i].byte_offset) {
      order[slot] = order[slot - 1u];
      --slot;
    }
    order[slot] = i;
  }

  uint64_t cursor = 0u;
  for (uint32_t i = 0u; i < kept_count; ++i) {
    VkrTextureUploadRegion *region = &regions[order[i]];
    cursor = AlignPow2(cursor, alignment);
    if (cursor != region->byte_offset) {
      MemCopy(result->upload_data + cursor,
              result->upload_data + region->byte_offset,
              (size_t)region->byte_size);
      region->byte_offset = cursor;
    }
    cursor += region->byte_size;
  }

  result->upload_region_count = kept_count;
  result->upload_mip_levels = kept_count;
  result->upload_data_size = cursor;
  result->width = (int32_t)Max(1u, width >> skip);
  result->height = (int32_t)Max(1u, height >> skip);
  uint8_t *shrunk = (uint8_t *)realloc(result->upload_data, (size_t)cursor);
  if (shrunk) {
    result->upload_data = shrunk;
  }
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
  if (has_upload_payload) {
    vkr_texture_decode_result_cap_extent(&decode_result,
                                         system->config.max_load_dimension);
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

  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_string8_VkrTextureEntry(&system->texture_map, name);
  if (existing_entry) {
    /* The loader retains this canonical result for its request. Direct
       texture-system callers acquire their own reference separately. */
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

  FilePath fp = vkr_asset_path_file(
      allocator, string8_create_from_cstr((const uint8_t *)path, strlen(path)));
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
