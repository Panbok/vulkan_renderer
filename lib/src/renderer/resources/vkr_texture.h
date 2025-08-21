#pragma once

#include "filesystem/filesystem.h"
#include "renderer/renderer.h"
typedef struct VkrTexture {
  TextureDescription description;
  TextureHandle handle;
  FilePath file_path;
  uint8_t *image;
} VkrTexture;

RendererError vkr_texture_create_checkerboard(RendererFrontendHandle renderer,
                                              Arena *renderer_arena,
                                              VkrTexture *out_texture);

RendererError vkr_texture_load(RendererFrontendHandle renderer,
                               Arena *renderer_arena, String8 path,
                               VkrTexture *out_texture);

void vkr_texture_destroy(RendererFrontendHandle renderer, VkrTexture *texture);