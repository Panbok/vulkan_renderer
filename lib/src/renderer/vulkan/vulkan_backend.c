#include "vulkan_backend.h"
#include "containers/str.h"
#include "defines.h"
#include "vulkan_buffer.h"
#include "vulkan_command.h"
#include "vulkan_device.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_instance.h"
#include "vulkan_pipeline.h"
#include "vulkan_renderpass.h"
#include "vulkan_shaders.h"
#include "vulkan_swapchain.h"

#ifndef NDEBUG
#include "vulkan_debug.h"
#endif

// todo: we are having issues with image ghosting when camera moves
// too fast, need to figure out why (clues VSync/present mode issues)

vkr_internal uint32_t vulkan_calculate_mip_levels(uint32_t width,
                                                  uint32_t height) {
  uint32_t mip_levels = 1;
  uint32_t max_dim = Max(width, height);
  while (max_dim > 1) {
    max_dim >>= 1;
    mip_levels++;
  }
  return mip_levels;
}

vkr_internal void vulkan_select_filter_modes(
    const VkrTextureDescription *desc, bool32_t anisotropy_supported,
    uint32_t mip_levels, VkFilter *out_min_filter, VkFilter *out_mag_filter,
    VkSamplerMipmapMode *out_mipmap_mode, VkBool32 *out_anisotropy_enable,
    float32_t *out_max_lod) {
  VkFilter min_filter = (desc->min_filter == VKR_FILTER_LINEAR)
                            ? VK_FILTER_LINEAR
                            : VK_FILTER_NEAREST;
  VkFilter mag_filter = (desc->mag_filter == VKR_FILTER_LINEAR)
                            ? VK_FILTER_LINEAR
                            : VK_FILTER_NEAREST;

  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  float32_t max_lod = (mip_levels > 0) ? (float32_t)(mip_levels - 1) : 0.0f;
  switch (desc->mip_filter) {
  case VKR_MIP_FILTER_NONE:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    max_lod = 0.0f;
    break;
  case VKR_MIP_FILTER_NEAREST:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    break;
  case VKR_MIP_FILTER_LINEAR:
  default:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    break;
  }

  VkBool32 anisotropy_enable =
      (desc->anisotropy_enable && anisotropy_supported) ? VK_TRUE : VK_FALSE;

  if (out_min_filter)
    *out_min_filter = min_filter;
  if (out_mag_filter)
    *out_mag_filter = mag_filter;
  if (out_mipmap_mode)
    *out_mipmap_mode = mipmap_mode;
  if (out_anisotropy_enable)
    *out_anisotropy_enable = anisotropy_enable;
  if (out_max_lod)
    *out_max_lod = max_lod;
}

vkr_internal bool32_t create_command_buffers(VulkanBackendState *state) {
  Scratch scratch = scratch_create(state->arena);
  state->graphics_command_buffers = array_create_VulkanCommandBuffer(
      scratch.arena, state->swapchain.images.length);
  for (uint32_t i = 0; i < state->swapchain.images.length; i++) {
    VulkanCommandBuffer *command_buffer =
        array_get_VulkanCommandBuffer(&state->graphics_command_buffers, i);
    if (!vulkan_command_buffer_allocate(state, command_buffer)) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);
      log_fatal("Failed to create Vulkan command buffer");
      return false;
    }
  }

  return true;
}

vkr_internal bool32_t create_domain_render_passes(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (state->domain_initialized[domain]) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
      continue;
    }

    state->domain_render_passes[domain] = arena_alloc(
        state->arena, sizeof(VulkanRenderPass), ARENA_MEMORY_TAG_RENDERER);
    if (!state->domain_render_passes[domain]) {
      log_fatal("Failed to allocate domain render pass for domain %u", domain);
      return false;
    }

    MemZero(state->domain_render_passes[domain], sizeof(VulkanRenderPass));

    if (!vulkan_renderpass_create_for_domain(
            state, (VkrPipelineDomain)domain,
            state->domain_render_passes[domain])) {
      log_fatal("Failed to create domain render pass for domain %u", domain);
      return false;
    }

    state->domain_initialized[domain] = true;
    // log_debug("Created domain render pass for domain %u", domain);
  }

  if (state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] &&
      !state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT]) {
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] =
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD];
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] = true;
    // log_debug("Linked WORLD_TRANSPARENT to WORLD render pass");
  }

  return true;
}

vkr_internal bool32_t create_domain_framebuffers(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (!state->domain_initialized[domain]) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
      continue;
    }

    if (state->domain_framebuffers[domain].length > 0) {
      for (uint32_t i = 0; i < state->domain_framebuffers[domain].length; ++i) {
        VulkanFramebuffer *old_fb =
            array_get_VulkanFramebuffer(&state->domain_framebuffers[domain], i);
        vulkan_framebuffer_destroy(state, old_fb);
      }
      array_destroy_VulkanFramebuffer(&state->domain_framebuffers[domain]);
    }

    state->domain_framebuffers[domain] = array_create_VulkanFramebuffer(
        state->swapchain_arena, state->swapchain.images.length);

    for (uint32_t i = 0; i < state->swapchain.images.length; i++) {
      array_set_VulkanFramebuffer(&state->domain_framebuffers[domain], i,
                                  (VulkanFramebuffer){
                                      .handle = VK_NULL_HANDLE,
                                      .attachments = {0},
                                      .renderpass = NULL,
                                  });
    }

    if (!vulkan_framebuffer_regenerate_for_domain(
            state, &state->swapchain, state->domain_render_passes[domain],
            (VkrPipelineDomain)domain, &state->domain_framebuffers[domain])) {
      log_fatal("Failed to regenerate framebuffers for domain %u", domain);
      return false;
    }

    // log_debug("Created domain framebuffers for domain %u", domain);
  }

  if (state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD]) {
    state->domain_framebuffers[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] =
        state->domain_framebuffers[VKR_PIPELINE_DOMAIN_WORLD];
  }

  return true;
}

vkr_internal VkrTextureFormat vulkan_vk_format_to_vkr(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  case VK_FORMAT_B8G8R8A8_UNORM:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  case VK_FORMAT_R8G8B8A8_SRGB:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  default:
    log_warn("Unmapped VkFormat %d, defaulting to R8G8B8A8_UNORM", format);
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  }
}

vkr_internal void
vulkan_backend_destroy_attachment_wrappers(VulkanBackendState *state) {
  state->swapchain_image_textures = NULL;
  state->depth_texture = NULL;
}

vkr_internal bool32_t
vulkan_backend_create_attachment_wrappers(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.image_count > 0, "Swapchain image count is 0");

  uint32_t image_count = state->swapchain.image_count;

  state->swapchain_image_textures = arena_alloc(
      state->swapchain_arena, sizeof(struct s_TextureHandle *) * image_count,
      ARENA_MEMORY_TAG_RENDERER);
  if (!state->swapchain_image_textures) {
    log_fatal("Failed to allocate swapchain image texture wrappers");
    return false;
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    struct s_TextureHandle *wrapper =
        arena_alloc(state->swapchain_arena, sizeof(struct s_TextureHandle),
                    ARENA_MEMORY_TAG_RENDERER);
    if (!wrapper) {
      log_fatal("Failed to allocate swapchain image wrapper");
      return false;
    }
    MemZero(wrapper, sizeof(struct s_TextureHandle));

    wrapper->texture.image.handle =
        *array_get_VkImage(&state->swapchain.images, i);
    wrapper->texture.image.view =
        *array_get_VkImageView(&state->swapchain.image_views, i);
    wrapper->texture.image.width = state->swapchain.extent.width;
    wrapper->texture.image.height = state->swapchain.extent.height;
    wrapper->texture.image.mip_levels = 1;
    wrapper->texture.image.array_layers = 1;
    wrapper->texture.sampler = VK_NULL_HANDLE;

    wrapper->description.width = state->swapchain.extent.width;
    wrapper->description.height = state->swapchain.extent.height;
    wrapper->description.channels = 4;
    wrapper->description.format =
        vulkan_vk_format_to_vkr(state->swapchain.format);

    state->swapchain_image_textures[i] = wrapper;
  }

  struct s_TextureHandle *depth_wrapper =
      arena_alloc(state->swapchain_arena, sizeof(struct s_TextureHandle),
                  ARENA_MEMORY_TAG_RENDERER);
  if (!depth_wrapper) {
    log_fatal("Failed to allocate depth attachment wrapper");
    return false;
  }
  MemZero(depth_wrapper, sizeof(struct s_TextureHandle));
  depth_wrapper->texture.image = state->swapchain.depth_attachment;
  depth_wrapper->texture.sampler = VK_NULL_HANDLE;
  depth_wrapper->description.width = state->swapchain.extent.width;
  depth_wrapper->description.height = state->swapchain.extent.height;
  depth_wrapper->description.channels = 1;
  depth_wrapper->description.format =
      (state->device.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
          ? VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT
          : VKR_TEXTURE_FORMAT_D32_SFLOAT;

  state->depth_texture = depth_wrapper;

  return true;
}

vkr_internal struct s_RenderPass *
vulkan_backend_renderpass_lookup(VulkanBackendState *state, String8 name) {
  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry->pass && entry->pass->vk &&
        entry->pass->vk->handle != VK_NULL_HANDLE &&
        string8_equalsi(&entry->name, &name)) {
      return entry->pass;
    }
  }
  return NULL;
}

vkr_internal bool32_t vulkan_backend_renderpass_register(
    VulkanBackendState *state, struct s_RenderPass *pass) {
  if (array_is_null_VkrRenderPassEntry(&state->render_pass_registry)) {
    state->render_pass_registry =
        array_create_VkrRenderPassEntry(state->arena, 4);
    state->render_pass_count = 0;
  }

  uint32_t slot = state->render_pass_count;
  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (!entry->pass || !entry->pass->vk ||
        entry->pass->vk->handle == VK_NULL_HANDLE) {
      slot = i;
      break;
    }
  }

  if (slot >= state->render_pass_registry.length) {
    log_error("Render pass registry capacity exceeded");
    return false;
  }

  VkrRenderPassEntry entry = {.name = pass->name, .pass = pass};
  array_set_VkrRenderPassEntry(&state->render_pass_registry, slot, entry);
  if (slot == state->render_pass_count) {
    state->render_pass_count++;
  }
  return true;
}

vkr_internal struct s_RenderPass *
vulkan_backend_renderpass_create_internal(VulkanBackendState *state,
                                          const VkrRenderPassConfig *cfg) {
  assert_log(cfg != NULL, "Render pass config is NULL");

  struct s_RenderPass *pass =
      arena_alloc(state->arena, sizeof(*pass), ARENA_MEMORY_TAG_RENDERER);
  if (!pass) {
    log_fatal("Failed to allocate render pass wrapper");
    return NULL;
  }
  MemZero(pass, sizeof(*pass));

  pass->cfg = *cfg;
  pass->name = string8_duplicate(state->arena, &cfg->name);

  pass->vk = arena_alloc(state->arena, sizeof(VulkanRenderPass),
                         ARENA_MEMORY_TAG_RENDERER);
  if (!pass->vk) {
    log_fatal("Failed to allocate Vulkan render pass");
    return NULL;
  }
  MemZero(pass->vk, sizeof(VulkanRenderPass));

  if (!vulkan_renderpass_create_from_config(state, cfg, pass->vk)) {
    log_error("Failed to create Vulkan render pass from config");
    return NULL;
  }

  if (!vulkan_backend_renderpass_register(state, pass)) {
    return NULL;
  }

  return pass;
}

vkr_internal bool32_t vulkan_backend_create_builtin_passes(
    VulkanBackendState *state, const VkrRendererBackendConfig *backend_config) {
  uint16_t cfg_count =
      backend_config ? backend_config->renderpass_count : (uint16_t)0;
  VkrRenderPassConfig *configs =
      backend_config ? backend_config->pass_configs : NULL;

  if (!array_is_null_VkrRenderPassEntry(&state->render_pass_registry)) {
    state->render_pass_count = 0;
  } else {
    uint16_t capacity = cfg_count > 0 ? (uint16_t)(cfg_count + 2) : 4;
    if (capacity < 4) {
      capacity = 4;
    }
    state->render_pass_registry =
        array_create_VkrRenderPassEntry(state->arena, capacity);
    state->render_pass_count = 0;
  }

  if (configs && cfg_count > 0) {
    for (uint16_t i = 0; i < cfg_count; ++i) {
      struct s_RenderPass *created =
          vulkan_backend_renderpass_create_internal(state, &configs[i]);
      if (!created) {
        return false;
      }

      if (vkr_string8_equals_cstr_i(&configs[i].name,
                                    "renderpass.builtin.world")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] = true;
      } else if (vkr_string8_equals_cstr_i(&configs[i].name,
                                           "renderpass.builtin.ui")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_UI] = true;
      } else if (vkr_string8_equals_cstr_i(&configs[i].name,
                                           "renderpass.builtin.skybox")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_SKYBOX] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_SKYBOX] = true;
      }
    }
  }

  if (!state->domain_render_passes[VKR_PIPELINE_DOMAIN_SKYBOX]) {
    VkrRenderPassConfig skybox_cfg = {
        .name = string8_lit("Renderpass.Builtin.Skybox"),
        .prev_name = {0},
        .next_name = string8_lit("Renderpass.Builtin.World"),
        .domain = VKR_PIPELINE_DOMAIN_SKYBOX,
        .render_area = (Vec4){0, 0, (float32_t)state->swapchain.extent.width,
                              (float32_t)state->swapchain.extent.height},
        .clear_color = (Vec4){1.0f, 0.0f, 1.0f, 1.0f}, // Magenta for debugging
        .clear_flags = VKR_RENDERPASS_CLEAR_COLOR | VKR_RENDERPASS_CLEAR_DEPTH,
    };
    struct s_RenderPass *skybox =
        vulkan_backend_renderpass_create_internal(state, &skybox_cfg);
    if (!skybox) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_SKYBOX] = skybox->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_SKYBOX] = true;
  }

  if (!state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD]) {
    VkrRenderPassConfig world_cfg = {
        .name = string8_lit("Renderpass.Builtin.World"),
        .prev_name = string8_lit("Renderpass.Builtin.Skybox"),
        .next_name = string8_lit("Renderpass.Builtin.UI"),
        .domain = VKR_PIPELINE_DOMAIN_WORLD,
        .render_area = (Vec4){0, 0, (float32_t)state->swapchain.extent.width,
                              (float32_t)state->swapchain.extent.height},
        .clear_color = (Vec4){0.1f, 0.1f, 0.2f, 1.0f},
        .clear_flags = VKR_RENDERPASS_USE_DEPTH, // Use depth without clearing
                                                 // (skybox already cleared)
    };
    struct s_RenderPass *world =
        vulkan_backend_renderpass_create_internal(state, &world_cfg);
    if (!world) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD] = world->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] = true;
  }

  if (!state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI]) {
    VkrRenderPassConfig ui_cfg = {
        .name = string8_lit("Renderpass.Builtin.UI"),
        .prev_name = string8_lit("Renderpass.Builtin.World"),
        .next_name = {0},
        .domain = VKR_PIPELINE_DOMAIN_UI,
        .render_area = (Vec4){0, 0, (float32_t)state->swapchain.extent.width,
                              (float32_t)state->swapchain.extent.height},
        .clear_color = (Vec4){0, 0, 0, 0},
        .clear_flags = VKR_RENDERPASS_CLEAR_NONE,
    };
    struct s_RenderPass *ui =
        vulkan_backend_renderpass_create_internal(state, &ui_cfg);
    if (!ui) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI] = ui->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_UI] = true;
  }

  return true;
}

bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  if (state->is_swapchain_recreation_requested) {
    // log_debug("Swapchain recreation was already requested");
    return false;
  }

  state->is_swapchain_recreation_requested = true;

  vkQueueWaitIdle(state->device.graphics_queue);

  vulkan_backend_destroy_attachment_wrappers(state);

  for (uint32_t i = 0; i < state->swapchain.image_count; ++i) {
    array_set_VulkanFencePtr(&state->images_in_flight, i, NULL);
  }

  if (!vulkan_swapchain_recreate(state)) {
    log_error("Failed to recreate swapchain");
    return false;
  }

  for (uint32_t i = 0; i < state->swapchain.image_count; ++i) {
    vulkan_command_buffer_free(state, array_get_VulkanCommandBuffer(
                                          &state->graphics_command_buffers, i));
  }

  for (uint32_t i = 0; i < state->swapchain.image_count; ++i) {
    vulkan_framebuffer_destroy(
        state, array_get_VulkanFramebuffer(&state->swapchain.framebuffers, i));
  }

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (state->domain_initialized[domain]) {
      state->domain_render_passes[domain]->position = (Vec2){0, 0};
      state->domain_render_passes[domain]->width =
          state->swapchain.extent.width;
      state->domain_render_passes[domain]->height =
          state->swapchain.extent.height;
    }
  }

  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry && entry->pass && entry->pass->vk) {
      entry->pass->cfg.render_area.z = (float32_t)state->swapchain.extent.width;
      entry->pass->cfg.render_area.w =
          (float32_t)state->swapchain.extent.height;
      entry->pass->vk->width = state->swapchain.extent.width;
      entry->pass->vk->height = state->swapchain.extent.height;
    }
  }

  if (!create_domain_framebuffers(state)) {
    log_error("Failed to recreate domain framebuffers");
    return false;
  }

  if (!create_command_buffers(state)) {
    log_error("Failed to create Vulkan command buffers");
    return false;
  }

  if (!vulkan_backend_create_attachment_wrappers(state)) {
    log_error("Failed to recreate swapchain attachment wrappers");
    return false;
  }

  if (state->on_render_target_refresh_required) {
    state->on_render_target_refresh_required();
  }

  state->active_named_render_pass = NULL;
  state->is_swapchain_recreation_requested = false;

  return true;
}

VkrRendererBackendInterface renderer_vulkan_get_interface() {
  return (VkrRendererBackendInterface){
      .initialize = renderer_vulkan_initialize,
      .shutdown = renderer_vulkan_shutdown,
      .on_resize = renderer_vulkan_on_resize,
      .get_device_information = renderer_vulkan_get_device_information,
      .wait_idle = renderer_vulkan_wait_idle,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .renderpass_create = renderer_vulkan_renderpass_create,
      .renderpass_destroy = renderer_vulkan_renderpass_destroy,
      .renderpass_get = renderer_vulkan_renderpass_get,
      .render_target_create = renderer_vulkan_render_target_create,
      .render_target_destroy = renderer_vulkan_render_target_destroy,
      .begin_render_pass = renderer_vulkan_begin_render_pass,
      .end_render_pass = renderer_vulkan_end_render_pass,
      .window_attachment_get = renderer_vulkan_window_attachment_get,
      .depth_attachment_get = renderer_vulkan_depth_attachment_get,
      .window_attachment_count_get = renderer_vulkan_window_attachment_count,
      .window_attachment_index_get = renderer_vulkan_window_attachment_index,
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .buffer_upload = renderer_vulkan_upload_buffer,
      .texture_create = renderer_vulkan_create_texture,
      .texture_update = renderer_vulkan_update_texture,
      .texture_write = renderer_vulkan_write_texture,
      .texture_resize = renderer_vulkan_resize_texture,
      .texture_destroy = renderer_vulkan_destroy_texture,
      .graphics_pipeline_create = renderer_vulkan_create_graphics_pipeline,
      .pipeline_update_state = renderer_vulkan_update_pipeline_state,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .instance_state_acquire = renderer_vulkan_instance_state_acquire,
      .instance_state_release = renderer_vulkan_instance_state_release,
      .bind_buffer = renderer_vulkan_bind_buffer,
      .draw = renderer_vulkan_draw,
      .draw_indexed = renderer_vulkan_draw_indexed,
      .get_and_reset_descriptor_writes_avoided =
          renderer_vulkan_get_and_reset_descriptor_writes_avoided,
  };
}
uint64_t
renderer_vulkan_get_and_reset_descriptor_writes_avoided(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  uint64_t value = state->descriptor_writes_avoided;
  state->descriptor_writes_avoided = 0;
  return value;
}

// todo: set up event manager for window stuff and maybe other events
bool32_t
renderer_vulkan_initialize(void **out_backend_state,
                           VkrRendererBackendType type, VkrWindow *window,
                           uint32_t initial_width, uint32_t initial_height,
                           VkrDeviceRequirements *device_requirements,
                           const VkrRendererBackendConfig *backend_config) {
  assert_log(out_backend_state != NULL, "Out backend state is NULL");
  assert_log(type == VKR_RENDERER_BACKEND_TYPE_VULKAN,
             "Vulkan backend type is required");
  assert_log(window != NULL, "Window is NULL");
  assert_log(initial_width > 0, "Initial width is 0");
  assert_log(initial_height > 0, "Initial height is 0");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  // log_debug("Initializing Vulkan backend");

  ArenaFlags temp_arena_flags = bitset8_create();
  Arena *temp_arena = arena_create(MB(4), KB(64), temp_arena_flags);
  if (!temp_arena) {
    log_fatal("Failed to create temporary arena");
    return false;
  }

  ArenaFlags swapchain_arena_flags = bitset8_create();
  Arena *swapchain_arena = arena_create(KB(64), KB(64), swapchain_arena_flags);
  if (!swapchain_arena) {
    log_fatal("Failed to create swapchain arena");
    arena_destroy(temp_arena);
    return false;
  }

  ArenaFlags arena_flags = bitset8_create();
  Arena *arena = arena_create(MB(1), MB(1), arena_flags);
  if (!arena) {
    log_fatal("Failed to create arena");
    return false;
  }

  VulkanBackendState *backend_state =
      arena_alloc(arena, sizeof(VulkanBackendState), ARENA_MEMORY_TAG_RENDERER);
  if (!backend_state) {
    log_fatal("Failed to allocate backend state");
    arena_destroy(arena);
    arena_destroy(temp_arena);
    return false;
  }

  MemZero(backend_state, sizeof(VulkanBackendState));
  backend_state->arena = arena;
  backend_state->temp_arena = temp_arena;
  backend_state->swapchain_arena = swapchain_arena;
  backend_state->window = window;
  backend_state->device_requirements = device_requirements;
  backend_state->descriptor_writes_avoided = 0;
  backend_state->render_pass_registry = (Array_VkrRenderPassEntry){0};
  backend_state->render_pass_count = 0;
  backend_state->swapchain_image_textures = NULL;
  backend_state->depth_texture = NULL;
  backend_state->on_render_target_refresh_required =
      backend_config ? backend_config->on_render_target_refresh_required : NULL;

  backend_state->current_render_pass_domain =
      VKR_PIPELINE_DOMAIN_COUNT; // Invalid domain
  backend_state->active_named_render_pass = NULL;
  backend_state->render_pass_active = false;
  backend_state->active_image_index = 0;

  for (uint32_t i = 0; i < VKR_PIPELINE_DOMAIN_COUNT; i++) {
    backend_state->domain_render_passes[i] = NULL;
    backend_state->domain_framebuffers[i] = (Array_VulkanFramebuffer){0};
    backend_state->domain_initialized[i] = false;
  }

  *out_backend_state = backend_state;
  backend_state->allocator = VK_NULL_HANDLE;

  if (!vulkan_instance_create(backend_state, window)) {
    log_fatal("Failed to create Vulkan instance");
    return false;
  }

#ifndef NDEBUG
  if (!vulkan_debug_create_debug_messenger(backend_state)) {
    log_fatal("Failed to create Vulkan debug messenger");
    return false;
  }
#endif

  if (!vulkan_platform_create_surface(backend_state)) {
    log_fatal("Failed to create Vulkan surface");
    return false;
  }

  if (!vulkan_device_pick_physical_device(backend_state)) {
    log_fatal("Failed to create Vulkan physical device");
    return false;
  }

  if (!vulkan_device_create_logical_device(backend_state)) {
    log_fatal("Failed to create Vulkan logical device");
    return false;
  }

  if (!vulkan_swapchain_create(backend_state)) {
    log_fatal("Failed to create Vulkan swapchain");
    return false;
  }

  if (!vulkan_backend_create_builtin_passes(backend_state, backend_config)) {
    log_fatal("Failed to create built-in render passes");
    return false;
  }

  if (!create_domain_render_passes(backend_state)) {
    log_fatal("Failed to create Vulkan domain render passes");
    return false;
  }

  if (!create_domain_framebuffers(backend_state)) {
    log_fatal("Failed to create Vulkan domain framebuffers");
    return false;
  }

  if (!vulkan_backend_create_attachment_wrappers(backend_state)) {
    log_fatal("Failed to create swapchain attachment wrappers");
    return false;
  }

  backend_state->swapchain.framebuffers = array_create_VulkanFramebuffer(
      backend_state->swapchain_arena, backend_state->swapchain.images.length);
  for (uint32_t i = 0; i < backend_state->swapchain.images.length; i++) {
    array_set_VulkanFramebuffer(&backend_state->swapchain.framebuffers, i,
                                (VulkanFramebuffer){
                                    .handle = VK_NULL_HANDLE,
                                    .attachments = {0},
                                    .renderpass = VK_NULL_HANDLE,
                                });
  }

  if (!create_command_buffers(backend_state)) {
    log_fatal("Failed to create Vulkan command buffers");
    return false;
  }

  backend_state->image_available_semaphores = array_create_VkSemaphore(
      backend_state->arena, backend_state->swapchain.max_in_flight_frames);
  backend_state->queue_complete_semaphores = array_create_VkSemaphore(
      backend_state->arena, backend_state->swapchain.image_count);
  backend_state->in_flight_fences = array_create_VulkanFence(
      backend_state->arena, backend_state->swapchain.max_in_flight_frames);
  for (uint32_t i = 0; i < backend_state->swapchain.max_in_flight_frames; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    if (vkCreateSemaphore(backend_state->device.logical_device, &semaphore_info,
                          backend_state->allocator,
                          array_get_VkSemaphore(
                              &backend_state->image_available_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create Vulkan image available semaphore");
      return false;
    }

    // fence is created with is_signaled set to true, because we want to wait
    // on the fence until the previous frame is finished
    vulkan_fence_create(
        backend_state, true_v,
        array_get_VulkanFence(&backend_state->in_flight_fences, i));
  }

  // Create queue complete semaphores for each swapchain image
  for (uint32_t i = 0; i < backend_state->swapchain.image_count; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    if (vkCreateSemaphore(backend_state->device.logical_device, &semaphore_info,
                          backend_state->allocator,
                          array_get_VkSemaphore(
                              &backend_state->queue_complete_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create Vulkan queue complete semaphore");
      return false;
    }
  }

  backend_state->images_in_flight = array_create_VulkanFencePtr(
      backend_state->arena, backend_state->swapchain.image_count);
  for (uint32_t i = 0; i < backend_state->swapchain.image_count; i++) {
    array_set_VulkanFencePtr(&backend_state->images_in_flight, i, NULL);
  }

  return true;
}

void renderer_vulkan_get_device_information(
    void *backend_state, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  vulkan_device_get_information(state, device_information, temp_arena);
}

void renderer_vulkan_shutdown(void *backend_state) {
  // log_debug("Shutting down Vulkan backend");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Ensure all GPU work is complete before destroying any resources
  vkDeviceWaitIdle(state->device.logical_device);

  // Free command buffers first to release references to pipelines
  for (uint32_t i = 0; i < state->graphics_command_buffers.length; i++) {
    vulkan_command_buffer_free(state, array_get_VulkanCommandBuffer(
                                          &state->graphics_command_buffers, i));
  }
  array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);

  // Wait again to ensure command buffer cleanup is complete
  vkDeviceWaitIdle(state->device.logical_device);

  for (uint32_t i = 0; i < state->swapchain.max_in_flight_frames; i++) {
    vulkan_fence_destroy(state,
                         array_get_VulkanFence(&state->in_flight_fences, i));
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->image_available_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->swapchain.image_count; i++) {
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->queue_complete_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->swapchain.framebuffers.length; i++) {
    VulkanFramebuffer *framebuffer =
        array_get_VulkanFramebuffer(&state->swapchain.framebuffers, i);
    vulkan_framebuffer_destroy(state, framebuffer);
  }
  array_destroy_VulkanFramebuffer(&state->swapchain.framebuffers);

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (state->domain_initialized[domain]) {
      if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
        continue;
      }
      for (uint32_t i = 0; i < state->domain_framebuffers[domain].length; ++i) {
        VulkanFramebuffer *framebuffer =
            array_get_VulkanFramebuffer(&state->domain_framebuffers[domain], i);
        vulkan_framebuffer_destroy(state, framebuffer);
      }
      array_destroy_VulkanFramebuffer(&state->domain_framebuffers[domain]);
    }
  }

  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry && entry->pass && entry->pass->vk) {
      vulkan_renderpass_destroy(state, entry->pass->vk);
    }
  }

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (!state->domain_initialized[domain]) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
      state->domain_render_passes[domain] = NULL;
      continue;
    }

    VulkanRenderPass *domain_pass = state->domain_render_passes[domain];
    if (!domain_pass) {
      continue;
    }

    bool skip_destroy = false;
    for (uint32_t i = 0; i < state->render_pass_count; ++i) {
      VkrRenderPassEntry *entry =
          array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
      if (entry && entry->pass && entry->pass->vk == domain_pass) {
        skip_destroy = true;
        break;
      }
    }

    if (!skip_destroy) {
      vulkan_renderpass_destroy(state, domain_pass);
    }

    state->domain_render_passes[domain] = NULL;
  }
  vulkan_backend_destroy_attachment_wrappers(state);
  vulkan_swapchain_destroy(state);
  vulkan_device_destroy_logical_device(state);
  vulkan_device_release_physical_device(state);
  vulkan_platform_destroy_surface(state);
#ifndef NDEBUG
  vulkan_debug_destroy_debug_messenger(state);
#endif
  vulkan_instance_destroy(state);
  arena_destroy(state->swapchain_arena);
  arena_destroy(state->temp_arena);
  arena_destroy(state->arena);
  return;
}

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height) {
  // log_debug("Resizing Vulkan backend to %d x %d", new_width, new_height);

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  if (state->is_swapchain_recreation_requested) {
    // log_debug("Swapchain recreation was already requested");
    return;
  }

  state->swapchain.extent.width = new_width;
  state->swapchain.extent.height = new_height;

  if (!vulkan_backend_recreate_swapchain(state)) {
    log_error("Failed to recreate swapchain");
    return;
  }

  return;
}

VkrRendererError renderer_vulkan_wait_idle(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkResult result = vkDeviceWaitIdle(state->device.logical_device);
  if (result != VK_SUCCESS) {
    log_warn("Failed to wait for Vulkan device to be idle");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

/**
 * @brief Begin a new rendering frame
 *
 * AUTOMATIC RENDER PASS MANAGEMENT:
 * This function deliberately does NOT start any render pass. Instead, render
 * passes are started automatically when the first pipeline is bound via
 * vulkan_graphics_pipeline_update_state(). This enables automatic multi-pass
 * rendering based on pipeline domains.
 *
 * FRAME LIFECYCLE:
 * 1. Wait for previous frame fence (GPU finished previous frame)
 * 2. Acquire next swapchain image
 * 3. Reset and begin command buffer recording
 * 4. Set initial viewport and scissor (may be overridden by render pass
 * switches)
 * 5. Mark render pass as inactive (render_pass_active = false)
 * 6. Set domain to invalid (current_render_pass_domain = COUNT)
 *
 * RENDER PASS STATE:
 * - render_pass_active = false: No render pass is active at frame start
 * - current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT: Invalid domain
 * - swapchain_image_is_present_ready = false: Image not yet transitioned to
 * PRESENT
 *
 * NEXT STEPS:
 * After begin_frame, the application should:
 * 1. Update global uniforms (view/projection matrices)
 * 2. Bind pipelines (automatically starts domain-specific render passes)
 * 3. Draw geometry
 * 4. Call end_frame (automatically ends any active render pass)
 *
 * @param backend_state Vulkan backend state
 * @param delta_time Frame delta time in seconds
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError renderer_vulkan_begin_frame(void *backend_state,
                                             float64_t delta_time) {
  // log_debug("Beginning Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  state->frame_delta = delta_time;
  state->swapchain_image_is_present_ready = false;

  // Wait for the current frame's fence to be signaled (previous frame
  // finished)
  if (!vulkan_fence_wait(state, UINT64_MAX,
                         array_get_VulkanFence(&state->in_flight_fences,
                                               state->current_frame))) {
    log_warn("Vulkan fence timed out");
    return VKR_RENDERER_ERROR_NONE;
  }

  // Acquire the next image from the swapchain
  if (!vulkan_swapchain_acquire_next_image(
          state, UINT64_MAX,
          *array_get_VkSemaphore(&state->image_available_semaphores,
                                 state->current_frame),
          VK_NULL_HANDLE, // Don't use fence with acquire - it conflicts with
                          // queue submit
          &state->image_index)) {
    log_warn("Failed to acquire next image");
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  vulkan_command_buffer_reset(command_buffer);

  if (!vulkan_command_buffer_begin(command_buffer)) {
    log_fatal("Failed to begin Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)state->swapchain.extent.width,
      .height = (float32_t)state->swapchain.extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  VkRect2D scissor = {
      .offset = {0, 0},
      .extent = state->swapchain.extent,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle, 0, 1, &scissor);

  state->render_pass_active = false;
  state->current_render_pass_domain =
      VKR_PIPELINE_DOMAIN_COUNT; // Invalid domain (no pass active)
  state->active_named_render_pass = NULL;

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(vertex_count > 0, "Vertex count is 0");
  assert_log(instance_count > 0, "Instance count is 0");
  assert_log(first_vertex < vertex_count, "First vertex is out of bounds");
  assert_log(first_instance < instance_count,
             "First instance is out of bounds");

  // log_debug("Drawing Vulkan vertices");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdDraw(command_buffer->handle, vertex_count, instance_count, first_vertex,
            first_instance);

  return;
}

/**
 * @brief End the current rendering frame and submit to GPU
 *
 * IMAGE LAYOUT TRANSITIONS:
 * The function handles a critical layout transition case:
 * - If WORLD domain was last active: Image is in COLOR_ATTACHMENT_OPTIMAL
 * - Image must be transitioned to PRESENT_SRC_KHR for presentation
 * - If UI/POST domain was last: Image is already in PRESENT_SRC_KHR (no-op)
 *
 * This is tracked via swapchain_image_is_present_ready flag:
 * - Set by UI/POST render passes (finalLayout = PRESENT_SRC_KHR)
 * - If false: Manual transition required (WORLD was last)
 * - If true: No transition needed (UI/POST was last)
 *
 * FRAME SUBMISSION FLOW:
 * 1. End any active render pass
 * 2. Transition image to PRESENT layout if needed
 * 3. End command buffer recording
 * 4. Wait for previous frame using this image (fence)
 * 5. Submit command buffer to GPU queue
 * 6. Present image to swapchain
 * 7. Advance frame counter for triple buffering
 *
 * SYNCHRONIZATION:
 * - Image available semaphore: Signals when image is acquired from swapchain
 * - Queue complete semaphore: Signals when GPU finishes rendering
 * - In-flight fence: Ensures previous frame using this image has completed
 *
 * @param backend_state Vulkan backend state
 * @param delta_time Frame delta time in seconds
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError renderer_vulkan_end_frame(void *backend_state,
                                           float64_t delta_time) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(delta_time > 0, "Delta time is 0");

  // log_debug("Ending Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  if (state->render_pass_active) {
    VkrRendererError end_err = renderer_vulkan_end_render_pass(state);
    if (end_err != VKR_RENDERER_ERROR_NONE) {
      log_fatal("Failed to end active render pass");
      return end_err;
    }
  }

  // ============================================================================
  // CRITICAL IMAGE LAYOUT TRANSITION
  // ============================================================================
  // Handle the case where WORLD domain was the last (or only) pass active:
  //
  // WORLD render pass: finalLayout = COLOR_ATTACHMENT_OPTIMAL
  //   → Image is left in attachment-optimal layout for efficient UI chaining
  //   → If no UI pass runs, we must transition to PRESENT_SRC_KHR here
  //
  // UI render pass: finalLayout = PRESENT_SRC_KHR
  //   → Image is already in present layout, no transition needed
  //   → swapchain_image_is_present_ready = true (set by UI pass)
  //
  // POST render pass: finalLayout = PRESENT_SRC_KHR
  //   → Image is already in present layout, no transition needed
  //   → swapchain_image_is_present_ready = true (set by POST pass)
  //
  // This design allows efficient WORLD→UI chaining without extra transitions,
  // while still supporting WORLD-only frames via manual transition here.
  // ============================================================================
  if (!state->swapchain_image_is_present_ready) {
    VkImageMemoryBarrier present_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image =
            *array_get_VkImage(&state->swapchain.images, state->image_index),
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };

    vkCmdPipelineBarrier(command_buffer->handle,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &present_barrier);
  }

  if (!vulkan_command_buffer_end(command_buffer)) {
    log_fatal("Failed to end Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  // Make sure the previous frame is not using this image (i.e. its fence is
  // being waited on)
  VulkanFencePtr *image_fence =
      array_get_VulkanFencePtr(&state->images_in_flight, state->image_index);
  if (*image_fence != NULL) { // was frame
    if (!vulkan_fence_wait(state, UINT64_MAX, *image_fence)) {
      log_warn("Failed to wait for Vulkan fence");
      return VKR_RENDERER_ERROR_NONE;
    }
  }

  // Mark the image fence as in-use by this frame.
  *image_fence =
      array_get_VulkanFence(&state->in_flight_fences, state->current_frame);

  // Reset the fence for use on the next frame
  vulkan_fence_reset(state, array_get_VulkanFence(&state->in_flight_fences,
                                                  state->current_frame));

  VkPipelineStageFlags flags[1] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer->handle,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = array_get_VkSemaphore(
          &state->queue_complete_semaphores, state->image_index),
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = array_get_VkSemaphore(
          &state->image_available_semaphores, state->current_frame),
      .pWaitDstStageMask = flags,
  };

  VkResult result = vkQueueSubmit(
      state->device.graphics_queue, 1, &submit_info,
      array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
          ->handle);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to submit Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  vulkan_command_buffer_update_submitted(command_buffer);

  if (!vulkan_swapchain_present(
          state,
          *array_get_VkSemaphore(&state->queue_complete_semaphores,
                                 state->image_index),
          state->image_index)) {
    log_warn("Failed to present Vulkan image");
    return VKR_RENDERER_ERROR_NONE;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_draw_indexed(void *backend_state, uint32_t index_count,
                                  uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset,
                                  uint32_t first_instance) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(index_count > 0, "Index count is 0");
  assert_log(instance_count > 0, "Instance count is 0");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdDrawIndexed(command_buffer->handle, index_count, instance_count,
                   first_index, vertex_offset, first_instance);

  return;
}

VkrBackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const VkrBufferDescription *desc,
                              const void *initial_data) {
  // log_debug("Creating Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_BufferHandle *buffer = arena_alloc(
      state->arena, sizeof(struct s_BufferHandle), ARENA_MEMORY_TAG_RENDERER);
  if (!buffer) {
    log_fatal("Failed to allocate buffer");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(buffer, sizeof(struct s_BufferHandle));

  // Copy the description so we can access usage flags later
  buffer->description = *desc;

  if (!vulkan_buffer_create(state, desc, buffer)) {
    log_fatal("Failed to create Vulkan buffer");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // If initial data is provided, load it into the buffer
  if (initial_data && desc->size > 0) {
    if (renderer_vulkan_upload_buffer(
            backend_state, (VkrBackendResourceHandle){.ptr = buffer}, 0,
            desc->size, initial_data) != VKR_RENDERER_ERROR_NONE) {
      vulkan_buffer_destroy(state, &buffer->buffer);
      log_error("Failed to upload initial data into buffer");
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  }

  return (VkrBackendResourceHandle){.ptr = buffer};
}

VkrRendererError renderer_vulkan_update_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data) {
  // log_debug("Updating Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  if (!vulkan_buffer_load_data(state, &buffer->buffer, offset, size, 0, data)) {
    log_fatal("Failed to update Vulkan buffer");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_upload_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data) {
  // log_debug("Uploading Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;

  Scratch scratch = scratch_create(state->temp_arena);
  // Create a host-visible staging buffer to upload to. Mark it as the source
  // of the transfer.
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription staging_buffer_desc = {
      .size = size,
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };
  struct s_BufferHandle *staging_buffer = arena_alloc(
      scratch.arena, sizeof(struct s_BufferHandle), ARENA_MEMORY_TAG_RENDERER);

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_fatal("Failed to create staging buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, size, 0,
                               data)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_fatal("Failed to load data into staging buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  if (!vulkan_buffer_copy_to(state, &staging_buffer->buffer,
                             staging_buffer->buffer.handle, 0,
                             buffer->buffer.handle, offset, size)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_fatal("Failed to copy Vulkan buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    VkrBackendResourceHandle handle) {
  // log_debug("Destroying Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  vulkan_buffer_destroy(state, &buffer->buffer);

  return;
}

vkr_internal VkrBackendResourceHandle renderer_vulkan_create_cube_texture(
    VulkanBackendState *state, const VkrTextureDescription *desc,
    const void *initial_data);

VkrBackendResourceHandle
renderer_vulkan_create_texture(void *backend_state,
                               const VkrTextureDescription *desc,
                               const void *initial_data) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  bool32_t writable =
      bitset8_is_set(&desc->properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
  assert_log(initial_data != NULL || writable,
             "Initial data is NULL and texture is not writable");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Branch to cube map creation if type is cube map
  if (desc->type == VKR_TEXTURE_TYPE_CUBE_MAP) {
    return renderer_vulkan_create_cube_texture(state, desc, initial_data);
  }

  // log_debug("Creating Vulkan texture");

  struct s_TextureHandle *texture = arena_alloc(
      state->arena, sizeof(struct s_TextureHandle), ARENA_MEMORY_TAG_RENDERER);
  if (!texture) {
    log_fatal("Failed to allocate texture");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  texture->description = *desc;

  VkDeviceSize image_size = (VkDeviceSize)desc->width *
                            (VkDeviceSize)desc->height *
                            (VkDeviceSize)desc->channels;

  VkFormat image_format = vulkan_image_format_from_texture_format(desc->format);
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                      image_format, &format_props);
  bool32_t linear_blit_supported =
      (format_props.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  uint32_t mip_levels =
      linear_blit_supported
          ? vulkan_calculate_mip_levels(desc->width, desc->height)
          : 1;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  Scratch scratch = {0};
  bool8_t scratch_valid = false_v;
  struct s_BufferHandle *staging_buffer = NULL;

  if (initial_data) {
    const VkrBufferDescription staging_buffer_desc = {
        .size = image_size,
        .usage =
            vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_HOST_VISIBLE |
            VKR_MEMORY_PROPERTY_HOST_COHERENT),
        .buffer_type = buffer_type,
        .bind_on_create = true_v,
    };

    scratch = scratch_create(state->temp_arena);
    scratch_valid = true_v;
    staging_buffer = arena_alloc(scratch.arena, sizeof(struct s_BufferHandle),
                                 ARENA_MEMORY_TAG_RENDERER);
    if (!staging_buffer) {
      log_fatal("Failed to allocate staging buffer");
      if (scratch_valid)
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }

    if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
      log_fatal("Failed to create staging buffer");
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }

    if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, image_size,
                                 0, initial_data)) {
      log_fatal("Failed to load data into staging buffer");
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  }

  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, desc->width, desc->height, image_format,
          VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels, 1,
          VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
          &texture->texture.image)) {
    log_fatal("Failed to create Vulkan image");
    if (staging_buffer)
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
    if (scratch_valid)
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (initial_data) {
    // Use two-phase upload: transfer queue for base level, graphics for mipmaps
    bool8_t generate_mipmaps =
        (texture->texture.image.mip_levels > 1) && linear_blit_supported;

    if (!vulkan_image_upload_with_mipmaps(state, &texture->texture.image,
                                          staging_buffer->buffer.handle,
                                          image_format, generate_mipmaps)) {
      log_fatal("Failed to upload texture via transfer queue");
      vulkan_image_destroy(state, &texture->texture.image);
      if (staging_buffer)
        vulkan_buffer_destroy(state, &staging_buffer->buffer);
      if (scratch_valid)
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  } else {
    // Writable texture - just transition layout on graphics queue
    VulkanCommandBuffer temp_command_buffer = {0};
    if (!vulkan_command_buffer_allocate_and_begin_single_use(
            state, &temp_command_buffer)) {
      log_fatal("Failed to allocate command buffer for writable texture");
      vulkan_image_destroy(state, &texture->texture.image);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }

    if (!vulkan_image_transition_layout(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      log_fatal("Failed to transition writable image layout");
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &texture->texture.image);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }

    if (!vulkan_command_buffer_end_single_use(
            state, &temp_command_buffer, state->device.graphics_queue,
            array_get_VulkanFence(&state->in_flight_fences,
                                  state->current_frame)
                ->handle)) {
      log_fatal("Failed to end single use command buffer");
      vulkan_image_destroy(state, &texture->texture.image);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }

    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
  }

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(texture->texture.image.mip_levels - 1);
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  // Create sampler
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy = anisotropy_enable
                           ? state->device.properties.limits.maxSamplerAnisotropy
                           : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info, NULL,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create texture sampler");
    vulkan_image_destroy(state, &texture->texture.image);
    if (staging_buffer)
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
    if (scratch_valid)
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // Only set transparency bit for formats that support alpha channel
  if (desc->channels == 4 ||
      desc->format == VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM ||
      desc->format == VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB ||
      desc->format == VKR_TEXTURE_FORMAT_R8G8B8A8_UINT ||
      desc->format == VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM ||
      desc->format == VKR_TEXTURE_FORMAT_R8G8B8A8_SINT) {
    bitset8_set(&texture->description.properties,
                VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
  }
  texture->description.generation++;

  if (staging_buffer)
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
  if (scratch_valid)
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

  return (VkrBackendResourceHandle){.ptr = texture};
}

vkr_internal VkrBackendResourceHandle renderer_vulkan_create_cube_texture(
    VulkanBackendState *state, const VkrTextureDescription *desc,
    const void *initial_data) {
  assert_log(state != NULL, "State is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  assert_log(initial_data != NULL,
             "Cube map requires initial data for all 6 faces");

  // log_debug("Creating Vulkan cube map texture");

  struct s_TextureHandle *texture = arena_alloc(
      state->arena, sizeof(struct s_TextureHandle), ARENA_MEMORY_TAG_RENDERER);
  if (!texture) {
    log_fatal("Failed to allocate cube texture");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));
  texture->description = *desc;

  // Each face has the same dimensions
  VkDeviceSize face_size = (VkDeviceSize)desc->width *
                           (VkDeviceSize)desc->height *
                           (VkDeviceSize)desc->channels;
  VkDeviceSize total_size = face_size * 6;

  VkFormat image_format = vulkan_image_format_from_texture_format(desc->format);

  // Cube maps typically don't use mipmaps initially for simplicity
  uint32_t mip_levels = 1;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  Scratch scratch = scratch_create(state->temp_arena);
  const VkrBufferDescription staging_buffer_desc = {
      .size = total_size,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };

  struct s_BufferHandle *staging_buffer = arena_alloc(
      scratch.arena, sizeof(struct s_BufferHandle), ARENA_MEMORY_TAG_RENDERER);
  if (!staging_buffer) {
    log_fatal("Failed to allocate staging buffer");
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    log_fatal("Failed to create staging buffer for cube map");
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, total_size, 0,
                               initial_data)) {
    log_fatal("Failed to load cube map data into staging buffer");
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // Create cube map image with 6 array layers
  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, desc->width, desc->height,
                           image_format, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels, 6,
                           VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT,
                           &texture->texture.image)) {
    log_fatal("Failed to create Vulkan cube map image");
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // Upload cube map faces via transfer queue
  if (!vulkan_image_upload_cube_via_transfer(state, &texture->texture.image,
                                             staging_buffer->buffer.handle,
                                             image_format, face_size)) {
    log_fatal("Failed to upload cube map via transfer queue");
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // Create sampler for cube map (clamp to edge is typical for skyboxes)
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_FALSE,
      .maxAnisotropy = 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info, NULL,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create cube map sampler");
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  texture->description.generation++;

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

  // log_debug("Created Vulkan cube map texture: %p",
  //           texture->texture.image.handle);

  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrRendererError
renderer_vulkan_update_texture(void *backend_state,
                               VkrBackendResourceHandle handle,
                               const VkrTextureDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");
  assert_log(desc != NULL, "Texture description is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  if (desc->width != texture->description.width ||
      desc->height != texture->description.height ||
      desc->channels != texture->description.channels ||
      desc->format != texture->description.format) {
    log_error("Texture update rejected: description dimensions or format "
              "differ from existing texture");
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(texture->texture.image.mip_levels - 1);
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  // Create new sampler for texture update
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy = anisotropy_enable
                           ? state->device.properties.limits.maxSamplerAnisotropy
                           : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  VkSampler new_sampler;
  if (vkCreateSampler(state->device.logical_device, &sampler_info, NULL,
                      &new_sampler) != VK_SUCCESS) {
    log_error("Failed to create sampler for texture update");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  // Ensure no in-flight use of the old sampler before switching
  vkQueueWaitIdle(state->device.graphics_queue);

  // Destroy old sampler and use new one
  vkDestroySampler(state->device.logical_device, texture->texture.sampler, NULL);
  texture->texture.sampler = new_sampler;

  texture->description.u_repeat_mode = desc->u_repeat_mode;
  texture->description.v_repeat_mode = desc->v_repeat_mode;
  texture->description.w_repeat_mode = desc->w_repeat_mode;
  texture->description.min_filter = desc->min_filter;
  texture->description.mag_filter = desc->mag_filter;
  texture->description.mip_filter = desc->mip_filter;
  texture->description.anisotropy_enable = desc->anisotropy_enable;
  texture->description.generation++;

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_write_texture(
    void *backend_state, VkrBackendResourceHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");
  assert_log(data != NULL, "Texture data is NULL");
  assert_log(size > 0, "Texture data size must be greater than zero");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  uint32_t mip_level = region ? region->mip_level : 0;
  uint32_t array_layer = region ? region->array_layer : 0;
  uint32_t x = region ? region->x : 0;
  uint32_t y = region ? region->y : 0;
  uint32_t width =
      region ? region->width : (uint32_t)texture->texture.image.width;
  uint32_t height =
      region ? region->height : (uint32_t)texture->texture.image.height;

  if (width == 0 || height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (mip_level >= texture->texture.image.mip_levels ||
      array_layer >= texture->texture.image.array_layers) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t mip_width = Max(1u, texture->texture.image.width >> mip_level);
  uint32_t mip_height = Max(1u, texture->texture.image.height >> mip_level);

  if (x + width > mip_width || y + height > mip_height) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)width * (uint64_t)height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription staging_buffer_desc = {
      .size = size,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };

  Scratch scratch = scratch_create(state->temp_arena);
  struct s_BufferHandle *staging_buffer = arena_alloc(
      scratch.arena, sizeof(struct s_BufferHandle), ARENA_MEMORY_TAG_RENDERER);
  if (!staging_buffer) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, size, 0,
                               data)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkImageSubresourceRange subresource_range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = mip_level,
      .levelCount = 1,
      .baseArrayLayer = array_layer,
      .layerCount = 1,
  };

  VkFormat image_format =
      vulkan_image_format_from_texture_format(texture->description.format);
  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &subresource_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkBufferImageCopy copy_region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip_level,
                           .baseArrayLayer = array_layer,
                           .layerCount = 1},
      .imageOffset = {(int32_t)x, (int32_t)y, 0},
      .imageExtent = {width, height, 1}};

  vkCmdCopyBufferToImage(temp_command_buffer.handle,
                         staging_buffer->buffer.handle,
                         texture->texture.image.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &subresource_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

  texture->description.generation++;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_resize_texture(void *backend_state,
                                                VkrBackendResourceHandle handle,
                                                uint32_t new_width,
                                                uint32_t new_height,
                                                bool8_t preserve_contents) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");

  if (new_width == 0 || new_height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  VkFormat image_format =
      vulkan_image_format_from_texture_format(texture->description.format);
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                      image_format, &format_props);
  bool32_t linear_blit_supported =
      (format_props.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  uint32_t mip_levels = linear_blit_supported
                            ? vulkan_calculate_mip_levels(new_width, new_height)
                            : 1;

  VulkanImage new_image = {0};
  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, new_width, new_height, image_format,
          VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels,
          texture->texture.image.array_layers, VK_IMAGE_VIEW_TYPE_2D,
          VK_IMAGE_ASPECT_COLOR_BIT, &new_image)) {
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkImageSubresourceRange new_range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = new_image.mip_levels,
                                       .baseArrayLayer = 0,
                                       .layerCount = new_image.array_layers};

  if (preserve_contents) {
    VkImageSubresourceRange old_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = texture->texture.image.mip_levels,
        .baseArrayLayer = 0,
        .layerCount = texture->texture.image.array_layers,
    };

    if (!vulkan_image_transition_layout_range(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &old_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    uint32_t copy_width = Min(texture->texture.image.width, new_width);
    uint32_t copy_height = Min(texture->texture.image.height, new_height);

    if (linear_blit_supported) {
      VkImageBlit blit = {
          .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = texture->texture.image.array_layers},
          .srcOffsets = {{0, 0, 0},
                         {(int32_t)texture->texture.image.width,
                          (int32_t)texture->texture.image.height, 1}},
          .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = new_image.array_layers},
          .dstOffsets = {{0, 0, 0},
                         {(int32_t)new_width, (int32_t)new_height, 1}}};

      vkCmdBlitImage(temp_command_buffer.handle, texture->texture.image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
    } else {
      VkImageCopy copy_region = {
          .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = texture->texture.image.array_layers},
          .srcOffset = {0, 0, 0},
          .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = new_image.array_layers},
          .dstOffset = {0, 0, 0},
          .extent = {copy_width, copy_height, 1},
      };

      vkCmdCopyImage(temp_command_buffer.handle, texture->texture.image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    }

    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    if (!vulkan_image_transition_layout_range(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &old_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
  } else {
    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(new_image.mip_levels - 1);
  vulkan_select_filter_modes(&texture->description,
                             state->device.features.samplerAnisotropy,
                             new_image.mip_levels, &min_filter, &mag_filter,
                             &mipmap_mode, &anisotropy_enable, &max_lod);

  // Create new sampler for resized texture
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU = vulkan_sampler_address_mode_from_repeat(
          texture->description.u_repeat_mode),
      .addressModeV = vulkan_sampler_address_mode_from_repeat(
          texture->description.v_repeat_mode),
      .addressModeW = vulkan_sampler_address_mode_from_repeat(
          texture->description.w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy = anisotropy_enable
                           ? state->device.properties.limits.maxSamplerAnisotropy
                           : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  VkSampler new_sampler;
  if (vkCreateSampler(state->device.logical_device, &sampler_info, NULL,
                      &new_sampler) != VK_SUCCESS) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  // Ensure previous operations complete before swapping resources
  vkQueueWaitIdle(state->device.graphics_queue);

  VulkanImage old_image = texture->texture.image;
  VkSampler old_sampler = texture->texture.sampler;

  texture->texture.image = new_image;
  texture->texture.sampler = new_sampler;

  // Destroy old sampler
  vkDestroySampler(state->device.logical_device, old_sampler, NULL);

  vulkan_image_destroy(state, &old_image);

  texture->description.width = new_width;
  texture->description.height = new_height;
  texture->description.generation++;

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_texture(void *backend_state,
                                     VkrBackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  // log_debug("Destroying Vulkan texture");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  // Ensure the texture is not in use before destroying
  if (renderer_vulkan_wait_idle(backend_state) != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to wait for idle before destroying texture");
  }

  vulkan_image_destroy(state, &texture->texture.image);

  // Destroy the sampler
  vkDestroySampler(state->device.logical_device, texture->texture.sampler, NULL);
  texture->texture.sampler = VK_NULL_HANDLE;
  return;
}

VkrBackendResourceHandle renderer_vulkan_create_graphics_pipeline(
    void *backend_state, const VkrGraphicsPipelineDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Pipeline description is NULL");

  // log_debug("Creating Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline =
      arena_alloc(state->arena, sizeof(struct s_GraphicsPipeline),
                  ARENA_MEMORY_TAG_RENDERER);
  if (!pipeline) {
    log_fatal("Failed to allocate pipeline");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(pipeline, sizeof(struct s_GraphicsPipeline));

  if (!vulkan_graphics_graphics_pipeline_create(state, desc, pipeline)) {
    log_fatal("Failed to create Vulkan pipeline layout");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  return (VkrBackendResourceHandle){.ptr = pipeline};
}

VkrRendererError renderer_vulkan_update_pipeline_state(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  // log_debug("Updating Vulkan pipeline state");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  return vulkan_graphics_pipeline_update_state(state, pipeline, uniform, data,
                                               material);
}

VkrRendererError renderer_vulkan_instance_state_acquire(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrRendererInstanceStateHandle *out_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  uint32_t object_id = 0;
  if (!vulkan_shader_acquire_instance(state, &pipeline->shader_object,
                                      &object_id)) {
    return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
  }

  out_handle->id = object_id;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
renderer_vulkan_instance_state_release(void *backend_state,
                                       VkrBackendResourceHandle pipeline_handle,
                                       VkrRendererInstanceStateHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  if (!vulkan_shader_release_instance(state, &pipeline->shader_object,
                                      handle.id)) {
    return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      VkrBackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  // log_debug("Destroying Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline = (struct s_GraphicsPipeline *)handle.ptr;

  vulkan_graphics_pipeline_destroy(state, pipeline);

  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   VkrBackendResourceHandle pipeline_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  // todo: add support for multiple command buffers
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdBindPipeline(command_buffer->handle, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->pipeline);

  return;
}

void renderer_vulkan_bind_buffer(void *backend_state,
                                 VkrBackendResourceHandle buffer_handle,
                                 uint64_t offset) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(buffer_handle.ptr != NULL, "Buffer handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)buffer_handle.ptr;

  // log_debug("Binding Vulkan buffer with usage flags");

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  // log_debug("Current command buffer handle: %p", command_buffer->handle);

  if (bitset8_is_set(&buffer->description.usage,
                     VKR_BUFFER_USAGE_VERTEX_BUFFER)) {
    vulkan_buffer_bind_vertex_buffer(state, command_buffer, 0,
                                     buffer->buffer.handle, offset);
  } else if (bitset8_is_set(&buffer->description.usage,
                            VKR_BUFFER_USAGE_INDEX_BUFFER)) {
    // Default to uint32 index type - could be improved by storing in buffer
    // description
    vulkan_buffer_bind_index_buffer(
        state, command_buffer, buffer->buffer.handle, offset,
        VK_INDEX_TYPE_UINT32); // todo: append index type to buffer
                               // description
  } else {
    log_warn("Buffer has unknown usage flags for pipeline binding");
  }

  return;
}

VkrRenderPassHandle
renderer_vulkan_renderpass_create(void *backend_state,
                                  const VkrRenderPassConfig *cfg) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(cfg != NULL, "Render pass config is NULL");
  assert_log(cfg->name.length > 0, "Render pass name is empty");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_RenderPass *existing =
      vulkan_backend_renderpass_lookup(state, cfg->name);
  if (existing) {
    return (VkrRenderPassHandle)existing;
  }

  struct s_RenderPass *created =
      vulkan_backend_renderpass_create_internal(state, cfg);
  if (!created) {
    return NULL;
  }

  if (vkr_string8_equals_cstr_i(&created->name, "renderpass.builtin.world")) {
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD] = created->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] = true;
  } else if (vkr_string8_equals_cstr_i(&created->name,
                                       "renderpass.builtin.ui")) {
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI] = created->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_UI] = true;
  }

  return (VkrRenderPassHandle)created;
}

void renderer_vulkan_renderpass_destroy(void *backend_state,
                                        VkrRenderPassHandle pass_handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !pass_handle) {
    return;
  }

  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  vulkan_renderpass_destroy(state, pass->vk);
  if (state->active_named_render_pass == pass) {
    state->active_named_render_pass = NULL;
  }

  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry->pass == pass) {
      entry->pass = NULL;
      entry->name = (String8){0};
      break;
    }
  }

  for (uint32_t i = 0; i < VKR_PIPELINE_DOMAIN_COUNT; ++i) {
    if (state->domain_render_passes[i] == pass->vk) {
      state->domain_render_passes[i] = NULL;
      state->domain_initialized[i] = false;
    }
  }
}

VkrRenderPassHandle renderer_vulkan_renderpass_get(void *backend_state,
                                                   const char *name) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !name) {
    return NULL;
  }

  uint64_t len = strlen(name);
  String8 lookup = string8_create_from_cstr((const uint8_t *)name, len);
  struct s_RenderPass *found = vulkan_backend_renderpass_lookup(state, lookup);
  return (VkrRenderPassHandle)found;
}

VkrRenderTargetHandle
renderer_vulkan_render_target_create(void *backend_state,
                                     const VkrRenderTargetDesc *desc,
                                     VkrRenderPassHandle pass_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Render target desc is NULL");
  assert_log(pass_handle != NULL, "Render pass handle is NULL");
  assert_log(desc->attachments != NULL, "Render target attachments are NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  if (!pass->vk || pass->vk->handle == VK_NULL_HANDLE ||
      desc->attachment_count == 0) {
    return NULL;
  }

  struct s_RenderTarget *target = arena_alloc(
      state->arena, sizeof(struct s_RenderTarget), ARENA_MEMORY_TAG_RENDERER);
  if (!target) {
    log_fatal("Failed to allocate render target");
    return NULL;
  }
  MemZero(target, sizeof(struct s_RenderTarget));

  target->attachment_count = desc->attachment_count;
  target->sync_to_window_size = desc->sync_to_window_size;
  target->width =
      desc->sync_to_window_size ? state->swapchain.extent.width : desc->width;
  target->height =
      desc->sync_to_window_size ? state->swapchain.extent.height : desc->height;

  target->attachments = arena_alloc(
      state->arena, sizeof(struct s_TextureHandle *) * target->attachment_count,
      ARENA_MEMORY_TAG_RENDERER);
  if (!target->attachments) {
    log_fatal("Failed to allocate render target attachments");
    return NULL;
  }

  Scratch scratch = scratch_create(state->temp_arena);
  VkImageView *views = arena_alloc(
      scratch.arena, sizeof(VkImageView) * (uint64_t)target->attachment_count,
      ARENA_MEMORY_TAG_ARRAY);
  if (!views) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_fatal("Failed to allocate render target image views");
    return NULL;
  }

  for (uint32_t i = 0; i < target->attachment_count; ++i) {
    struct s_TextureHandle *tex =
        (struct s_TextureHandle *)desc->attachments[i];
    if (!tex) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      log_error("Render target attachment %u is NULL", i);
      return NULL;
    }
    if (tex->texture.image.view == VK_NULL_HANDLE) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      log_error("Render target attachment %u has no image view", i);
      return NULL;
    }
    target->attachments[i] = tex;
    views[i] = tex->texture.image.view;
  }

  VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = pass->vk->handle,
      .attachmentCount = target->attachment_count,
      .pAttachments = views,
      .width = target->width,
      .height = target->height,
      .layers = 1,
  };

  if (vkCreateFramebuffer(state->device.logical_device, &fb_info,
                          state->allocator, &target->handle) != VK_SUCCESS) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_fatal("Failed to create framebuffer for render target");
    return NULL;
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return (VkrRenderTargetHandle)target;
}

void renderer_vulkan_render_target_destroy(
    void *backend_state, VkrRenderTargetHandle target_handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !target_handle) {
    return;
  }

  struct s_RenderTarget *target = (struct s_RenderTarget *)target_handle;
  if (target->handle != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(state->device.logical_device, target->handle,
                         state->allocator);
    target->handle = VK_NULL_HANDLE;
  }
  target->attachments = NULL;
  target->attachment_count = 0;
}

VkrRendererError
renderer_vulkan_begin_render_pass(void *backend_state,
                                  VkrRenderPassHandle pass_handle,
                                  VkrRenderTargetHandle target_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  struct s_RenderTarget *target = (struct s_RenderTarget *)target_handle;

  if (!pass || !target || !pass->vk || target->handle == VK_NULL_HANDLE) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  Scratch scratch = scratch_create(state->temp_arena);
  VkClearValue *clear_values = arena_alloc(
      scratch.arena, sizeof(VkClearValue) * target->attachment_count,
      ARENA_MEMORY_TAG_ARRAY);
  if (!clear_values) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  MemZero(clear_values,
          sizeof(VkClearValue) * (uint64_t)target->attachment_count);

  if (target->attachment_count > 0) {
    clear_values[0].color.float32[0] = pass->cfg.clear_color.r;
    clear_values[0].color.float32[1] = pass->cfg.clear_color.g;
    clear_values[0].color.float32[2] = pass->cfg.clear_color.b;
    clear_values[0].color.float32[3] = pass->cfg.clear_color.a;
  }

  if (target->attachment_count > 1) {
    clear_values[1].depthStencil.depth = 1.0f;
    clear_values[1].depthStencil.stencil = 0;
  }

  float32_t render_width = (pass->cfg.render_area.z > 0.0f)
                               ? pass->cfg.render_area.z
                               : (float32_t)target->width;
  float32_t render_height = (pass->cfg.render_area.w > 0.0f)
                                ? pass->cfg.render_area.w
                                : (float32_t)target->height;

  uint32_t extent_w =
      Max(1u, (uint32_t)Min(render_width, (float32_t)target->width));
  uint32_t extent_h =
      Max(1u, (uint32_t)Min(render_height, (float32_t)target->height));

  VkRect2D render_area = {
      .offset = {(int32_t)Max(0, (int32_t)pass->cfg.render_area.x),
                 (int32_t)Max(0, (int32_t)pass->cfg.render_area.y)},
      .extent = {extent_w, extent_h},
  };

  VkRenderPassBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass->vk->handle,
      .framebuffer = target->handle,
      .renderArea = render_area,
      .clearValueCount = target->attachment_count,
      .pClearValues = clear_values,
  };

  vkCmdBeginRenderPass(command_buffer->handle, &begin_info,
                       VK_SUBPASS_CONTENTS_INLINE);

  state->render_pass_active = true;
  state->current_render_pass_domain = pass->vk->domain;
  state->active_named_render_pass = pass;

  VkViewport viewport = {
      .x = (float32_t)render_area.offset.x,
      .y = (float32_t)render_area.offset.y,
      .width = (float32_t)render_area.extent.width,
      .height = (float32_t)render_area.extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle, 0, 1, &render_area);

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_end_render_pass(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state->render_pass_active) {
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdEndRenderPass(command_buffer->handle);

  if (state->active_named_render_pass &&
      state->active_named_render_pass->cfg.next_name.length == 0) {
    state->swapchain_image_is_present_ready = true;
  }

  state->active_named_render_pass = NULL;
  state->render_pass_active = false;
  state->current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT;
  return VKR_RENDERER_ERROR_NONE;
}

VkrTextureOpaqueHandle
renderer_vulkan_window_attachment_get(void *backend_state,
                                      uint32_t image_index) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !state->swapchain_image_textures ||
      image_index >= state->swapchain.image_count) {
    return NULL;
  }

  return (VkrTextureOpaqueHandle)state->swapchain_image_textures[image_index];
}

VkrTextureOpaqueHandle
renderer_vulkan_depth_attachment_get(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return NULL;
  }
  return (VkrTextureOpaqueHandle)state->depth_texture;
}

uint32_t renderer_vulkan_window_attachment_count(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return 0;
  }
  return state->swapchain.image_count;
}

uint32_t renderer_vulkan_window_attachment_index(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return 0;
  }
  return state->image_index;
}
