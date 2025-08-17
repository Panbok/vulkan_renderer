#pragma once

#include "renderer/renderer.h"

typedef struct VkrTexture {
  TextureDescription description;
  TextureHandle handle;
  uint8_t *image;
} VkrTexture;

RendererError vkr_texture_create_checkerboard(RendererFrontendHandle renderer,
                                              Arena *renderer_arena,
                                              VkrTexture *out_texture);

void vkr_texture_destroy(RendererFrontendHandle renderer, VkrTexture *texture);