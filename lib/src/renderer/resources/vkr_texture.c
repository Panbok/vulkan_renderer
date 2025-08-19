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

void vkr_texture_destroy(RendererFrontendHandle renderer, VkrTexture *texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(texture->handle != NULL, "Texture handle is NULL");

  renderer_destroy_texture(renderer, texture->handle);
  texture->handle = NULL;
  texture->image = NULL;
}