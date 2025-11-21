#include "vulkan_backend.h"
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
    log_debug("Created domain render pass for domain %u", domain);
  }

  return true;
}

vkr_internal bool32_t create_domain_framebuffers(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (!state->domain_initialized[domain]) {
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

    log_debug("Created domain framebuffers for domain %u", domain);
  }

  return true;
}

bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  if (state->is_swapchain_recreation_requested) {
    log_debug("Swapchain recreation was already requested");
    return false;
  }

  state->is_swapchain_recreation_requested = true;

  vkQueueWaitIdle(state->device.graphics_queue);

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

  if (!create_domain_framebuffers(state)) {
    log_error("Failed to recreate domain framebuffers");
    return false;
  }

  if (!create_command_buffers(state)) {
    log_error("Failed to create Vulkan command buffers");
    return false;
  }

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
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .buffer_upload = renderer_vulkan_upload_buffer,
      .texture_create = renderer_vulkan_create_texture,
      .texture_update = renderer_vulkan_update_texture,
      .texture_destroy = renderer_vulkan_destroy_texture,
      .graphics_pipeline_create = renderer_vulkan_create_graphics_pipeline,
      .pipeline_update_state = renderer_vulkan_update_pipeline_state,
      .instance_state_acquire = renderer_vulkan_instance_state_acquire,
      .instance_state_release = renderer_vulkan_instance_state_release,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .begin_render_pass = renderer_vulkan_begin_render_pass,
      .end_render_pass = renderer_vulkan_end_render_pass,
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
                           VkrDeviceRequirements *device_requirements) {
  assert_log(out_backend_state != NULL, "Out backend state is NULL");
  assert_log(type == VKR_RENDERER_BACKEND_TYPE_VULKAN,
             "Vulkan backend type is required");
  assert_log(window != NULL, "Window is NULL");
  assert_log(initial_width > 0, "Initial width is 0");
  assert_log(initial_height > 0, "Initial height is 0");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  log_debug("Initializing Vulkan backend");

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

  backend_state->current_render_pass_domain =
      VKR_PIPELINE_DOMAIN_COUNT; // Invalid domain
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

  if (!create_domain_render_passes(backend_state)) {
    log_fatal("Failed to create Vulkan domain render passes");
    return false;
  }

  if (!create_domain_framebuffers(backend_state)) {
    log_fatal("Failed to create Vulkan domain framebuffers");
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
  log_debug("Shutting down Vulkan backend");
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
      for (uint32_t i = 0; i < state->domain_framebuffers[domain].length; ++i) {
        VulkanFramebuffer *framebuffer =
            array_get_VulkanFramebuffer(&state->domain_framebuffers[domain], i);
        vulkan_framebuffer_destroy(state, framebuffer);
      }
      array_destroy_VulkanFramebuffer(&state->domain_framebuffers[domain]);
    }
  }

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (state->domain_initialized[domain] &&
        state->domain_render_passes[domain]) {
      vulkan_renderpass_destroy(state, state->domain_render_passes[domain]);
    }
  }
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
  log_debug("Resizing Vulkan backend to %d x %d", new_width, new_height);

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  if (state->is_swapchain_recreation_requested) {
    log_debug("Swapchain recreation was already requested");
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
    if (!vulkan_renderpass_end(command_buffer, state)) {
      log_fatal("Failed to end Vulkan render pass");
      return VKR_RENDERER_ERROR_NONE;
    }
    state->render_pass_active = false;
    state->current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT;
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

  // log_debug("Drawing Vulkan indexed vertices");

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
  log_debug("Creating Vulkan buffer");

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
  log_debug("Updating Vulkan buffer");

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
  log_debug("Uploading Vulkan buffer");

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
  log_debug("Destroying Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  vulkan_buffer_destroy(state, &buffer->buffer);

  return;
}

VkrBackendResourceHandle
renderer_vulkan_create_texture(void *backend_state,
                               const VkrTextureDescription *desc,
                               const void *initial_data) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  assert_log(initial_data != NULL, "Initial data is NULL");

  log_debug("Creating Vulkan texture");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

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

  const VkrBufferDescription staging_buffer_desc = {
      .size = image_size,
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
    log_fatal("Failed to allocate staging buffer");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    log_fatal("Failed to create staging buffer");
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, image_size, 0,
                               initial_data)) {
    log_fatal("Failed to load data into staging buffer");
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
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
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    log_fatal("Failed to allocate and begin single use command buffer");
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_image_transition_layout(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
    log_fatal("Failed to transition image layout");
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!vulkan_image_copy_from_buffer(state, &texture->texture.image,
                                     staging_buffer->buffer.handle,
                                     &temp_command_buffer)) {
    log_fatal("Failed to copy buffer to image");
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  bool8_t generated_mips = true_v;
  if (texture->texture.image.mip_levels > 1 && linear_blit_supported) {
    generated_mips = vulkan_image_generate_mipmaps(
        state, &texture->texture.image, image_format, &temp_command_buffer);
    if (!generated_mips) {
      // Fall back to a single mip level if generation failed
      log_error("Mipmap generation failed, falling back to single mip level. "
                "Recreating image to avoid memory waste.");

      vulkan_image_destroy(state, &texture->texture.image);

      if (!vulkan_image_create(
              state, VK_IMAGE_TYPE_2D, desc->width, desc->height, image_format,
              VK_IMAGE_TILING_OPTIMAL,
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
              VK_IMAGE_ASPECT_COLOR_BIT, &texture->texture.image)) {
        log_fatal("Failed to recreate Vulkan image with single mip level");
        vkEndCommandBuffer(temp_command_buffer.handle);
        vkFreeCommandBuffers(state->device.logical_device,
                             state->device.graphics_command_pool, 1,
                             &temp_command_buffer.handle);
        vulkan_buffer_destroy(state, &staging_buffer->buffer);
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        return (VkrBackendResourceHandle){.ptr = NULL};
      }

      if (!vulkan_image_transition_layout(
              state, &texture->texture.image, &temp_command_buffer,
              image_format, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
        log_fatal("Failed to transition recreated image layout");
        vkEndCommandBuffer(temp_command_buffer.handle);
        vkFreeCommandBuffers(state->device.logical_device,
                             state->device.graphics_command_pool, 1,
                             &temp_command_buffer.handle);
        vulkan_image_destroy(state, &texture->texture.image);
        vulkan_buffer_destroy(state, &staging_buffer->buffer);
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        return (VkrBackendResourceHandle){.ptr = NULL};
      }

      if (!vulkan_image_copy_from_buffer(state, &texture->texture.image,
                                         staging_buffer->buffer.handle,
                                         &temp_command_buffer)) {
        log_fatal("Failed to copy buffer to recreated image");
        vkEndCommandBuffer(temp_command_buffer.handle);
        vkFreeCommandBuffers(state->device.logical_device,
                             state->device.graphics_command_pool, 1,
                             &temp_command_buffer.handle);
        vulkan_image_destroy(state, &texture->texture.image);
        vulkan_buffer_destroy(state, &staging_buffer->buffer);
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        return (VkrBackendResourceHandle){.ptr = NULL};
      }

      texture->texture.image.mip_levels = 1;
      linear_blit_supported = false_v;
    }
  }

  if (texture->texture.image.mip_levels == 1 || !linear_blit_supported) {
    if (!vulkan_image_transition_layout(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      log_fatal("Failed to transition image layout");
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &texture->texture.image);
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    log_fatal("Failed to end single use command buffer");
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(texture->texture.image.mip_levels - 1);
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  VkSamplerCreateInfo sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .mipmapMode = mipmap_mode,
      .mipLodBias = 0.0f,
      .minLod = 0.0f,
      .maxLod = max_lod,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_create_info, NULL,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan sampler");
    vulkan_image_destroy(state, &texture->texture.image);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
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

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

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

  VkSamplerCreateInfo sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .mipmapMode = mipmap_mode,
      .mipLodBias = 0.0f,
      .minLod = 0.0f,
      .maxLod = max_lod,
  };

  VkSampler new_sampler = VK_NULL_HANDLE;
  if (vkCreateSampler(state->device.logical_device, &sampler_create_info, NULL,
                      &new_sampler) != VK_SUCCESS) {
    log_error("Failed to update Vulkan sampler");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  // Ensure no in-flight use of the old sampler before destroying it
  vkQueueWaitIdle(state->device.graphics_queue);

  if (texture->texture.sampler != VK_NULL_HANDLE) {
    vkDestroySampler(state->device.logical_device, texture->texture.sampler,
                     NULL);
  }
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

void renderer_vulkan_destroy_texture(void *backend_state,
                                     VkrBackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  log_debug("Destroying Vulkan texture");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  // Ensure the texture is not in use before destroying
  if (renderer_vulkan_wait_idle(backend_state) != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to wait for idle before destroying texture");
  }

  vulkan_image_destroy(state, &texture->texture.image);
  vkDestroySampler(state->device.logical_device, texture->texture.sampler,
                   NULL);

  texture->texture.sampler = VK_NULL_HANDLE;
  return;
}

VkrBackendResourceHandle renderer_vulkan_create_graphics_pipeline(
    void *backend_state, const VkrGraphicsPipelineDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Pipeline description is NULL");

  log_debug("Creating Vulkan pipeline");

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

  log_debug("Destroying Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline = (struct s_GraphicsPipeline *)handle.ptr;

  vulkan_graphics_pipeline_destroy(state, pipeline);

  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   VkrBackendResourceHandle pipeline_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  // log_debug("Binding Vulkan pipeline");

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

VkrRendererError renderer_vulkan_begin_render_pass(void *backend_state,
                                                   VkrPipelineDomain domain) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(domain != VKR_PIPELINE_DOMAIN_COUNT, "Invalid domain");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  if (state->render_pass_active) {
    return VKR_RENDERER_ERROR_NONE;
  }

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  VulkanRenderPass *render_pass = state->domain_render_passes[domain];
  VulkanFramebuffer *framebuffer = array_get_VulkanFramebuffer(
      &state->domain_framebuffers[domain], image_index);

  if (!vulkan_renderpass_begin(command_buffer, render_pass,
                               framebuffer->handle)) {
    log_fatal("Failed to begin Vulkan render pass");
    return VKR_RENDERER_ERROR_NONE;
  }

  state->current_render_pass_domain = domain;
  state->render_pass_active = true;

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

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_end_render_pass(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  if (!state->render_pass_active) {
    return VKR_RENDERER_ERROR_NONE;
  }

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);

  if (!vulkan_renderpass_end(command_buffer, state)) {
    log_fatal("Failed to end Vulkan render pass");
    return VKR_RENDERER_ERROR_NONE;
  }

  state->render_pass_active = false;
  state->current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT;

  return VKR_RENDERER_ERROR_NONE;
}
