#include "vkr_texture.h"

RendererError vkr_texture_create_checkerboard(RendererFrontendHandle renderer,
                                              Arena *texture_arena,
                                              VkrTexture *out_texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture_arena != NULL, "Texture arena is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");

  // Setup texture description
  out_texture->description = (TextureDescription){
      .width = 256,
      .height = 256,
      .channels = 4,
      .format = TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = TEXTURE_TYPE_2D,
      .properties = texture_property_flags_from_bits(
          TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .generation = VKR_INVALID_OBJECT_ID,
  };

  // Generate checkerboard image
  uint64_t image_size = (uint64_t)out_texture->description.width *
                        (uint64_t)out_texture->description.height *
                        (uint64_t)out_texture->description.channels;
  out_texture->image =
      arena_alloc(texture_arena, image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (out_texture->image == NULL) {
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  MemSet(out_texture->image, 255, image_size); // default white RGBA

  // Checkerboard parameters
  const uint32_t tile_size = 8; // pixels per tile
  for (uint32_t row = 0; row < out_texture->description.height; row++) {
    for (uint32_t col = 0; col < out_texture->description.width; col++) {
      uint32_t index = (row * out_texture->description.width + col) *
                       out_texture->description.channels;
      uint32_t tile_row = row / tile_size;
      uint32_t tile_col = col / tile_size;
      bool32_t is_white = ((tile_row + tile_col) % 2) == 0;
      uint8_t v = is_white ? 255 : 0;
      out_texture->image[index + 0] = v;   // R
      out_texture->image[index + 1] = v;   // G
      out_texture->image[index + 2] = v;   // B
      out_texture->image[index + 3] = 255; // A
    }
  }

  RendererError out_error = RENDERER_ERROR_NONE;
  out_texture->handle = renderer_create_texture(
      renderer, &out_texture->description, out_texture->image, &out_error);
  if (out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create checkerboard texture: %s",
              renderer_get_error_string(out_error));
  }

  return out_error;
}

RendererError vkr_texture_load(RendererFrontendHandle renderer,
                               Arena *renderer_arena, String8 path,
                               uint32_t desired_channels,
                               VkrTexture *out_texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer_arena != NULL, "Renderer arena is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");
  assert_log(path.str != NULL, "Path string is NULL");

  out_texture->file_path = file_path_create((char *)path.str, renderer_arena,
                                            FILE_PATH_TYPE_RELATIVE);

  stbi_set_flip_vertically_on_load(true);

  int32_t width, height, original_channels;
  int32_t stbi_req_comp = (desired_channels <= VKR_TEXTURE_RGBA_CHANNELS)
                              ? (int)desired_channels
                              : 0;
  if (desired_channels > VKR_TEXTURE_RGBA_CHANNELS) {
    log_warn("desired_channels=%u is invalid; falling back to auto-detect.",
             desired_channels);
  }

  uint8_t *image = stbi_load((char *)out_texture->file_path.path.str, &width,
                             &height, &original_channels, stbi_req_comp);
  if (image == NULL) {
    const char *failure_reason = stbi_failure_reason();
    if (failure_reason) {
      log_error("Failed to load texture: %s", failure_reason);
      if (string_contains(failure_reason, "can't fopen") ||
          string_contains(failure_reason, "file not found")) {
        return RENDERER_ERROR_FILE_NOT_FOUND;
      } else if (string_contains(failure_reason, "outofmem")) {
        return RENDERER_ERROR_OUT_OF_MEMORY;
      } else if (string_contains(failure_reason, "bad req_comp")) {
        return RENDERER_ERROR_INVALID_PARAMETER;
      } else {
        return RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      }
    }
    return RENDERER_ERROR_FILE_NOT_FOUND;
  }

  if (width <= 0 || height <= 0 || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION) {
    log_error("Invalid texture dimensions: %dx%d (max: %u)", width, height,
              VKR_TEXTURE_MAX_DIMENSION);
    stbi_image_free(image);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t actual_channels =
      desired_channels > 0 ? desired_channels : original_channels;

  if (original_channels < (int32_t)desired_channels && desired_channels > 0) {
    log_warn("Texture channels mismatch: %u requested but %d available (stbi "
             "padded to %u)",
             desired_channels, original_channels, desired_channels);
  }

  TextureFormat format;
  switch (actual_channels) {
  case VKR_TEXTURE_R_CHANNELS:
    format = TEXTURE_FORMAT_R8_UNORM;
    break;
  case VKR_TEXTURE_RG_CHANNELS:
    format = TEXTURE_FORMAT_R8G8_UNORM;
    break;
  case VKR_TEXTURE_RGB_CHANNELS:
    // Note: Most GPUs don't support RGB8, so we'll use RGBA8 and pad
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS; // Adjust for GPU compatibility
    break;
  case VKR_TEXTURE_RGBA_CHANNELS:
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    break;
  default:
    // Fallback for unexpected channel counts
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    log_warn("Unexpected channel count %d, defaulting to RGBA",
             actual_channels);
    break;
  }

  out_texture->description = (TextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = actual_channels,
      .format = format,
      .type = TEXTURE_TYPE_2D,
      .properties =
          texture_property_flags_create(), // Initialize as empty, set below
      .generation = VKR_INVALID_OBJECT_ID,
  };

  uint32_t loaded_channels =
      desired_channels > 0 ? desired_channels : original_channels;
  uint64_t loaded_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)loaded_channels;

  if (loaded_image_size > SIZE_MAX) {
    log_error("Image too large: %llu bytes", loaded_image_size);
    stbi_image_free(image);
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  bool32_t has_transparency = false;
  if (loaded_channels >= VKR_TEXTURE_RGBA_CHANNELS) {
    for (uint64_t i = 0; i < loaded_image_size; i += loaded_channels) {
      if (image[i + 3] < 255) {
        has_transparency = true;
        break;
      }
    }
  }

  // Heuristic for LA (luminance+alpha) inputs
  if (!has_transparency && loaded_channels == VKR_TEXTURE_RG_CHANNELS &&
      (original_channels == VKR_TEXTURE_RG_CHANNELS ||
       desired_channels == VKR_TEXTURE_RG_CHANNELS)) {
    for (uint64_t i = 1; i < loaded_image_size; i += loaded_channels) {
      if (image[i] < 255) { // alpha channel at index 1
        has_transparency = true;
        break;
      }
    }
  }

  out_texture->description.properties = texture_property_flags_from_bits(
      has_transparency ? TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT : 0);

  uint64_t final_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)actual_channels;
  if (final_image_size > SIZE_MAX) {
    log_error("Image too large after conversion: %llu bytes", final_image_size);
    stbi_image_free(image);
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  out_texture->image =
      arena_alloc(renderer_arena, final_image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (out_texture->image == NULL) {
    stbi_image_free(image);
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  if (loaded_channels == VKR_TEXTURE_RGB_CHANNELS &&
      actual_channels == VKR_TEXTURE_RGBA_CHANNELS) {
    for (uint32_t i = 0; i < (uint32_t)width * (uint32_t)height; i++) {
      uint32_t src_idx = i * VKR_TEXTURE_RGB_CHANNELS;
      uint32_t dst_idx = i * VKR_TEXTURE_RGBA_CHANNELS;
      out_texture->image[dst_idx + 0] = image[src_idx + 0]; // R
      out_texture->image[dst_idx + 1] = image[src_idx + 1]; // G
      out_texture->image[dst_idx + 2] = image[src_idx + 2]; // B
      out_texture->image[dst_idx + 3] = 255;                // A (opaque)
    }
  } else {
    MemCopy(out_texture->image, image, (size_t)loaded_image_size);
  }

  stbi_image_free(image);

  RendererError out_error = RENDERER_ERROR_NONE;
  out_texture->handle = renderer_create_texture(
      renderer, &out_texture->description, out_texture->image, &out_error);
  if (out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create texture: %s",
              renderer_get_error_string(out_error));
    return out_error;
  }

  if (out_texture->description.generation == VKR_INVALID_OBJECT_ID) {
    out_texture->description.generation = 0;
  } else {
    out_texture->description.generation++;
  }

  return out_error;
}

void vkr_texture_destroy(RendererFrontendHandle renderer, VkrTexture *texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(texture->handle != NULL, "Texture handle is NULL");

  renderer_destroy_texture(renderer, texture->handle);
  texture->handle = NULL;
  texture->image = NULL;
}