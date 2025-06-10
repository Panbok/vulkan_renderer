#include "vulkan_backend.h"
#include "core/logger.h"
#include "memory/arena.h"
#include "renderer/vulkan/vulkan_command.h"
#include "renderer/vulkan/vulkan_types.h"
#include <vulkan/vulkan_core.h>

RendererBackendInterface renderer_vulkan_get_interface() {
  return (RendererBackendInterface){
      .initialize = renderer_vulkan_initialize,
      .shutdown = renderer_vulkan_shutdown,
      .on_resize = renderer_vulkan_on_resize,
      .wait_idle = renderer_vulkan_wait_idle,
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .shader_create_from_source = renderer_vulkan_create_shader,
      .shader_destroy = renderer_vulkan_destroy_shader,
      .pipeline_create = renderer_vulkan_create_pipeline,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .bind_pipeline = renderer_vulkan_bind_pipeline,
      .bind_vertex_buffer = renderer_vulkan_bind_vertex_buffer,
      .draw = renderer_vulkan_draw,
  };
}

// todo: set up event manager for window stuff and maybe other events
bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    RendererBackendType type, Window *window,
                                    uint32_t initial_width,
                                    uint32_t initial_height) {
  assert_log(out_backend_state != NULL, "Out backend state is NULL");
  assert_log(type == RENDERER_BACKEND_TYPE_VULKAN,
             "Vulkan backend type is required");
  assert_log(window != NULL, "Window is NULL");
  assert_log(initial_width > 0, "Initial width is 0");
  assert_log(initial_height > 0, "Initial height is 0");

  log_debug("Initializing Vulkan backend");

  ArenaFlags temp_arena_flags = bitset8_create();
  Arena *temp_arena = arena_create(MB(4), KB(64), temp_arena_flags);
  if (!temp_arena) {
    log_fatal("Failed to create temporary arena");
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
    return false;
  }

  MemZero(backend_state, sizeof(VulkanBackendState));
  backend_state->arena = arena;
  backend_state->temp_arena = temp_arena;
  backend_state->window = window;

  *out_backend_state = backend_state;

  backend_state->validation_layers =
      array_create_String8(backend_state->arena, 1);
  array_set_String8(&backend_state->validation_layers, 0,
                    string8_lit("VK_LAYER_KHRONOS_validation"));

  if (!vulkan_instance_create(backend_state, window)) {
    log_fatal("Failed to create Vulkan instance");
    return false;
  }

  if (!vulkan_debug_create_debug_messenger(backend_state)) {
    log_fatal("Failed to create Vulkan debug messenger");
    return false;
  }

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

  VulkanRenderPass *main_render_pass =
      arena_alloc(backend_state->arena, sizeof(VulkanRenderPass),
                  ARENA_MEMORY_TAG_RENDERER);
  if (!main_render_pass) {
    log_fatal("Failed to allocate main render pass");
    return false;
  }

  MemZero(main_render_pass, sizeof(VulkanRenderPass));

  if (!vulkan_renderpass_create(backend_state, main_render_pass)) {
    log_fatal("Failed to create Vulkan render pass");
    return false;
  }

  backend_state->main_render_pass = main_render_pass;

  backend_state->swapChainFramebuffers = array_create_VkFramebuffer(
      backend_state->arena, backend_state->swapChainImages.length);
  for (uint32_t i = 0; i < backend_state->swapChainImages.length; i++) {
    VkImageView *image_view =
        array_get_VkImageView(&backend_state->swapChainImageViews, i);
    VkFramebuffer framebuffer;
    if (!vulkan_framebuffer_create(backend_state, image_view, &framebuffer)) {
      array_destroy_VkFramebuffer(&backend_state->swapChainFramebuffers);
      log_fatal("Failed to create Vulkan framebuffer");
      return false;
    }

    array_set_VkFramebuffer(&backend_state->swapChainFramebuffers, i,
                            framebuffer);
  }

  if (backend_state->graphics_command_buffers.length == 0) {
    Scratch scratch = scratch_create(backend_state->arena);
    backend_state->graphics_command_buffers = array_create_VulkanCommandBuffer(
        scratch.arena, backend_state->swapChainImages.length);
    for (uint32_t i = 0; i < backend_state->swapChainImages.length; i++) {
      VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
          &backend_state->graphics_command_buffers, i);
      if (!vulkan_command_buffer_create(backend_state, command_buffer)) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        array_destroy_VulkanCommandBuffer(
            &backend_state->graphics_command_buffers);
        log_fatal("Failed to create Vulkan command buffer");
        return false;
      }
    }
  }

  backend_state->image_available_semaphores = array_create_VkSemaphore(
      backend_state->arena, backend_state->swapchain_max_in_flight_frames);
  backend_state->queue_complete_semaphores = array_create_VkSemaphore(
      backend_state->arena, backend_state->swapchain_image_count);
  backend_state->in_flight_fences = array_create_VulkanFence(
      backend_state->arena, backend_state->swapchain_max_in_flight_frames);
  for (uint32_t i = 0; i < backend_state->swapchain_max_in_flight_frames; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    if (vkCreateSemaphore(backend_state->device, &semaphore_info, NULL,
                          array_get_VkSemaphore(
                              &backend_state->image_available_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create Vulkan image available semaphore");
      return false;
    }

    // fence is created with is_signaled set to true, because we want to wait on
    // the fence until the previous frame is finished
    vulkan_fence_create(
        backend_state, true_v,
        array_get_VulkanFence(&backend_state->in_flight_fences, i));
  }

  // Create queue complete semaphores for each swapchain image
  for (uint32_t i = 0; i < backend_state->swapchain_image_count; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    vkCreateSemaphore(
        backend_state->device, &semaphore_info, NULL,
        array_get_VkSemaphore(&backend_state->queue_complete_semaphores, i));
  }

  backend_state->images_in_flight = array_create_VulkanFencePtr(
      backend_state->arena, backend_state->swapchain_image_count);
  for (uint32_t i = 0; i < backend_state->swapchain_image_count; i++) {
    array_set_VulkanFencePtr(&backend_state->images_in_flight, i, NULL);
  }

  return true;
}

void renderer_vulkan_shutdown(void *backend_state) {
  log_debug("Shutting down Vulkan backend");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Ensure all GPU work is complete before destroying any resources
  vkDeviceWaitIdle(state->device);

  // Free command buffers first to release references to pipelines
  for (uint32_t i = 0; i < state->graphics_command_buffers.length; i++) {
    vulkan_command_buffer_destroy(
        state,
        array_get_VulkanCommandBuffer(&state->graphics_command_buffers, i));
  }
  array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);

  // Wait again to ensure command buffer cleanup is complete
  vkDeviceWaitIdle(state->device);

  for (uint32_t i = 0; i < state->swapchain_max_in_flight_frames; i++) {
    vulkan_fence_destroy(state,
                         array_get_VulkanFence(&state->in_flight_fences, i));
    vkDestroySemaphore(
        state->device,
        *array_get_VkSemaphore(&state->image_available_semaphores, i), NULL);
  }
  for (uint32_t i = 0; i < state->swapchain_image_count; i++) {
    vkDestroySemaphore(
        state->device,
        *array_get_VkSemaphore(&state->queue_complete_semaphores, i), NULL);
  }
  for (uint32_t i = 0; i < state->swapChainFramebuffers.length; i++) {
    vulkan_framebuffer_destroy(
        state, array_get_VkFramebuffer(&state->swapChainFramebuffers, i));
  }
  array_destroy_VkFramebuffer(&state->swapChainFramebuffers);
  vulkan_renderpass_destroy(state, state->main_render_pass);
  vulkan_swapchain_destroy(state);
  vulkan_device_destroy_logical_device(state);
  vulkan_device_release_physical_device(state);
  vulkan_platform_destroy_surface(state);
  vulkan_debug_destroy_debug_messenger(state);
  vulkan_instance_destroy(state);
  array_destroy_String8(&state->validation_layers);
  arena_destroy(state->temp_arena);
  arena_destroy(state->arena);
  return;
}

// todo: resizing functionality requires swapchain recreation and recording
// the framebuffer params
void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height) {
  log_debug("Resizing Vulkan backend to %d x %d", new_width, new_height);
  return;
}

RendererError renderer_vulkan_wait_idle(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkResult result = vkDeviceWaitIdle(state->device);
  if (result != VK_SUCCESS) {
    log_warn("Failed to wait for Vulkan device to be idle");
    return RENDERER_ERROR_DEVICE_ERROR;
  }

  return RENDERER_ERROR_NONE;
}

RendererError renderer_vulkan_begin_frame(void *backend_state,
                                          float64_t delta_time) {
  // log_debug("Beginning Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Wait for the current frame's fence to be signaled (previous frame finished)
  if (!vulkan_fence_wait(state, UINT64_MAX,
                         array_get_VulkanFence(&state->in_flight_fences,
                                               state->current_frame))) {
    log_warn("Vulkan fence timed out");
    return RENDERER_ERROR_NONE;
  }

  // Acquire the next image from the swapchain
  if (!vulkan_swapchain_acquire_next_image(
          state, UINT64_MAX,
          array_get_VkSemaphore(&state->image_available_semaphores,
                                state->current_frame),
          &state->image_index)) {
    log_warn("Failed to acquire next image");
    return RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  vulkan_command_buffer_reset(command_buffer);

  if (!vulkan_command_buffer_begin(command_buffer)) {
    log_fatal("Failed to begin Vulkan command buffer");
    return RENDERER_ERROR_NONE;
  }

  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)state->swapChainExtent.width,
      .height = (float32_t)state->swapChainExtent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  VkRect2D scissor = {
      .offset = {0, 0},
      .extent = state->swapChainExtent,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle, 0, 1, &scissor);

  if (!vulkan_renderpass_begin(
          command_buffer, state->main_render_pass,
          *array_get_VkFramebuffer(&state->swapChainFramebuffers,
                                   state->image_index))) {
    log_fatal("Failed to begin Vulkan render pass");
    return RENDERER_ERROR_NONE;
  }

  // here we end command buffer recording, next you should call binding
  // functions like bind pipeline, bind vertex buffer, etc. then call draw
  // functions and finally call end frame

  return RENDERER_ERROR_NONE;
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

RendererError renderer_vulkan_end_frame(void *backend_state,
                                        float64_t delta_time) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(delta_time > 0, "Delta time is 0");

  // log_debug("Ending Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  if (!vulkan_renderpass_end(command_buffer)) {
    log_fatal("Failed to end Vulkan render pass");
    return RENDERER_ERROR_NONE;
  }

  if (!vulkan_command_buffer_end(command_buffer)) {
    log_fatal("Failed to end Vulkan command buffer");
    return RENDERER_ERROR_NONE;
  }

  // Make sure the previous frame is not using this image (i.e. its fence is
  // being waited on)
  VulkanFencePtr *image_fence =
      array_get_VulkanFencePtr(&state->images_in_flight, state->image_index);
  if (*image_fence != NULL) { // was frame
    if (!vulkan_fence_wait(state, UINT64_MAX, *image_fence)) {
      log_warn("Failed to wait for Vulkan fence");
      return RENDERER_ERROR_NONE;
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
      state->graphics_queue, 1, &submit_info,
      array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
          ->handle);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to submit Vulkan command buffer");
    return RENDERER_ERROR_NONE;
  }

  vulkan_command_buffer_update_submitted(command_buffer);

  if (!vulkan_swapchain_present(
          state,
          array_get_VkSemaphore(&state->queue_complete_semaphores,
                                state->image_index),
          state->image_index)) {
    log_fatal("Failed to present Vulkan image");
    return RENDERER_ERROR_NONE;
  }

  return RENDERER_ERROR_NONE;
}

BackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const BufferDescription *desc,
                              const void *initial_data) {
  log_debug("Creating Vulkan buffer");
  return (BackendResourceHandle){.ptr = NULL};
}

RendererError renderer_vulkan_update_buffer(void *backend_state,
                                            BackendResourceHandle handle,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  log_debug("Updating Vulkan buffer");
  return RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    BackendResourceHandle handle) {
  log_debug("Destroying Vulkan buffer");
  return;
}

BackendResourceHandle
renderer_vulkan_create_shader(void *backend_state,
                              const ShaderModuleDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Shader module description is NULL");

  log_debug("Creating Vulkan shader");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_ShaderModule *shader = arena_alloc(
      state->arena, sizeof(struct s_ShaderModule), ARENA_MEMORY_TAG_RENDERER);
  if (!shader) {
    log_fatal("Failed to allocate shader");
    return (BackendResourceHandle){.ptr = NULL};
  }

  MemZero(shader, sizeof(struct s_ShaderModule));

  if (!vulkan_shader_module_create(state, desc, shader)) {
    log_fatal("Failed to create Vulkan shader");
    return (BackendResourceHandle){.ptr = NULL};
  }

  return (BackendResourceHandle){.ptr = shader};
}

void renderer_vulkan_destroy_shader(void *backend_state,
                                    BackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  log_debug("Destroying Vulkan shader");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_ShaderModule *shader = (struct s_ShaderModule *)handle.ptr;

  vulkan_shader_module_destroy(state, shader);

  return;
}

BackendResourceHandle
renderer_vulkan_create_pipeline(void *backend_state,
                                const GraphicsPipelineDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Pipeline description is NULL");

  log_debug("Creating Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline =
      arena_alloc(state->arena, sizeof(struct s_GraphicsPipeline),
                  ARENA_MEMORY_TAG_RENDERER);
  if (!pipeline) {
    log_fatal("Failed to allocate pipeline");
    return (BackendResourceHandle){.ptr = NULL};
  }

  MemZero(pipeline, sizeof(struct s_GraphicsPipeline));

  pipeline->desc = desc;

  if (!vulkan_pipeline_create(state, desc, pipeline)) {
    log_fatal("Failed to create Vulkan pipeline layout");
    return (BackendResourceHandle){.ptr = NULL};
  }

  return (BackendResourceHandle){.ptr = pipeline};
}

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      BackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  log_debug("Destroying Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline = (struct s_GraphicsPipeline *)handle.ptr;

  vulkan_pipeline_destroy(state, pipeline);

  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   BackendResourceHandle pipeline_handle) {
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

void renderer_vulkan_bind_vertex_buffer(void *backend_state,
                                        BackendResourceHandle buffer_handle,
                                        uint32_t binding_index,
                                        uint64_t offset) {
  log_debug("Binding Vulkan vertex buffer");
  return;
}
