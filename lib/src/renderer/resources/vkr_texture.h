#pragma once

#include "filesystem/filesystem.h"
#include "renderer/renderer.h"

#define VKR_TEXTURE_MAX_DIMENSION 16384
#define VKR_TEXTURE_RGBA_CHANNELS 4
#define VKR_TEXTURE_RGB_CHANNELS 3
#define VKR_TEXTURE_RG_CHANNELS 2
#define VKR_TEXTURE_R_CHANNELS 1

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
                               uint32_t desired_channels,
                               VkrTexture *out_texture);

void vkr_texture_destroy(RendererFrontendHandle renderer, VkrTexture *texture);