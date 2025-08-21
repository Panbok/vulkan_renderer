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
                               VkrTexture *out_texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer_arena != NULL, "Renderer arena is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");
  assert_log(path.str != NULL, "Path string is NULL");

  out_texture->file_path = file_path_create((char *)path.str, renderer_arena,
                                            FILE_PATH_TYPE_RELATIVE);

  stbi_set_flip_vertically_on_load(true);

  uint64_t req_channels = 4;

  int32_t width, height, channels;
  uint8_t *image = stbi_load((char *)out_texture->file_path.path.str, &width,
                             &height, &channels, req_channels);
  if (image == NULL) {
    if (stbi_failure_reason()) {
      log_error("Failed to load texture: %s", stbi_failure_reason());
    }

    return RENDERER_ERROR_FILE_NOT_FOUND;
  }

  // TODO: Handle different image formats
  out_texture->description = (TextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = (uint32_t)req_channels,
      .format = TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = TEXTURE_TYPE_2D,
      .properties = texture_property_flags_from_bits(
          TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .generation = VKR_INVALID_OBJECT_ID,
  };

  uint64_t image_size = (uint64_t)width * (uint64_t)height * (uint64_t)channels;

  bool32_t has_transparency = false;
  for (uint64_t i = 0; i < image_size; i += req_channels) {
    if (image[i + 3] < 255) {
      has_transparency = true;
      break;
    }
  }

  out_texture->description.properties = texture_property_flags_from_bits(
      has_transparency ? TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT : 0);

  out_texture->image =
      arena_alloc(renderer_arena, image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (out_texture->image == NULL) {
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  MemCopy(out_texture->image, image, image_size);

  stbi_image_free(image);

  RendererError out_error = RENDERER_ERROR_NONE;
  out_texture->handle = renderer_create_texture(
      renderer, &out_texture->description, out_texture->image, &out_error);
  if (out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create texture: %s",
              renderer_get_error_string(out_error));
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