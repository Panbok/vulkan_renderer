#include "renderer/passes/vkr_pass_copy.h"

#include "renderer/renderer_frontend.h"

typedef struct VkrPassCopyNames {
  const char *source;
  const char *destination;
} VkrPassCopyNames;

vkr_internal void vkr_pass_copy_execute(VkrRgPassContext *ctx,
                                        void *user_data) {
  const VkrPassCopyNames *names = (const VkrPassCopyNames *)user_data;
  if (!ctx || !ctx->renderer || !ctx->graph || !names) {
    return;
  }

  const String8 source_name = string8_create_from_cstr(
      (const uint8_t *)names->source, string_length(names->source));
  const String8 destination_name = string8_create_from_cstr(
      (const uint8_t *)names->destination, string_length(names->destination));
  const VkrRgImageHandle source = vkr_rg_find_image(ctx->graph, source_name);
  const VkrRgImageHandle destination =
      vkr_rg_find_image(ctx->graph, destination_name);
  VkrTextureOpaqueHandle source_texture =
      vkr_rg_get_image_texture(ctx->graph, source, ctx->image_index);
  VkrTextureOpaqueHandle destination_texture =
      vkr_rg_get_image_texture(ctx->graph, destination, ctx->image_index);
  if (!source_texture || !destination_texture) {
    ctx->error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return;
  }

  ctx->error = vkr_renderer_copy_texture((RendererFrontend *)ctx->renderer,
                                         source_texture, destination_texture);
}

bool8_t vkr_pass_copy_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  static const VkrPassCopyNames fullscreen = {
      .source = "hdr_pre_transmission",
      .destination = "hdr_scene_color",
  };
  static const VkrPassCopyNames editor = {
      .source = "scene_pre_transmission",
      .destination = "scene_color",
  };
  const VkrRgPassExecutor entries[] = {
      {.name = string8_lit("pass.copy.pre_transmission.fullscreen"),
       .execute = vkr_pass_copy_execute,
       .user_data = (void *)&fullscreen},
      {.name = string8_lit("pass.copy.pre_transmission.editor"),
       .execute = vkr_pass_copy_execute,
       .user_data = (void *)&editor},
  };
  for (uint32_t i = 0u; i < ArrayCount(entries); ++i) {
    if (!vkr_rg_executor_registry_register(registry, &entries[i])) {
      return false_v;
    }
  }
  return true_v;
}
