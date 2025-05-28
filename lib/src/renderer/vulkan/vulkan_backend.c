#include "vulkan_backend.h"

RendererBackendInterface renderer_vulkan_get_interface() {
  return (RendererBackendInterface){
      .initialize = renderer_vulkan_initialize,
      .shutdown = renderer_vulkan_shutdown,
      .on_resize = renderer_vulkan_on_resize,
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .shader_create_from_source = renderer_vulkan_create_shader,
      .shader_destroy = renderer_vulkan_destroy_shader,
      .pipeline_create = renderer_vulkan_create_pipeline,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .set_viewport = renderer_vulkan_set_viewport,
      .set_scissor = renderer_vulkan_set_scissor,
      .clear_color = renderer_vulkan_clear_color,
      .bind_pipeline = renderer_vulkan_bind_pipeline,
      .bind_vertex_buffer = renderer_vulkan_bind_vertex_buffer,
      .draw = renderer_vulkan_draw,
  };
}

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
    arena_destroy(temp_arena);
    return false;
  }

  ArenaFlags arena_flags = bitset8_create();
  bitset8_set(&arena_flags, ARENA_FLAG_LARGE_PAGES);
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

  if (!vulkan_device_pick_physical_device(backend_state)) {
    log_fatal("Failed to create Vulkan physical device");
    return false;
  }

  if (!vulkan_device_create_logical_device(backend_state)) {
    log_fatal("Failed to create Vulkan logical device");
    return false;
  }

  return true;
}

void renderer_vulkan_shutdown(void *backend_state) {
  log_debug("Shutting down Vulkan backend");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  vulkan_device_destroy_logical_device(state);
  vulkan_device_release_physical_device(state);
  vulkan_debug_destroy_debug_messenger(state);
  vulkan_instance_destroy(state);
  array_destroy_String8(&state->validation_layers);
  arena_destroy(state->temp_arena);
  arena_destroy(state->arena);
  return;
}

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height) {
  log_debug("Resizing Vulkan backend to %d x %d", new_width, new_height);
  return;
}

RendererError renderer_vulkan_begin_frame(void *backend_state,
                                          float64_t delta_time) {
  log_debug("Beginning Vulkan frame");
  return RENDERER_ERROR_NONE;
}

RendererError renderer_vulkan_end_frame(void *backend_state,
                                        float64_t delta_time) {
  log_debug("Ending Vulkan frame");
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
  log_debug("Creating Vulkan shader");
  return (BackendResourceHandle){.ptr = NULL};
}

void renderer_vulkan_destroy_shader(void *backend_state,
                                    BackendResourceHandle handle) {
  log_debug("Destroying Vulkan shader");
  return;
}

BackendResourceHandle
renderer_vulkan_create_pipeline(void *backend_state,
                                const GraphicsPipelineDescription *desc) {
  log_debug("Creating Vulkan pipeline");
  return (BackendResourceHandle){.ptr = NULL};
}

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      BackendResourceHandle handle) {
  log_debug("Destroying Vulkan pipeline");
  return;
}

void renderer_vulkan_set_viewport(void *backend_state, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height,
                                  float32_t min_depth, float32_t max_depth) {
  log_debug("Setting Vulkan viewport to %d x %d with min depth %f and max "
            "depth %f",
            width, height, min_depth, max_depth);
  return;
}

void renderer_vulkan_set_scissor(void *backend_state, int32_t x, int32_t y,
                                 uint32_t width, uint32_t height) {
  log_debug("Setting Vulkan scissor to %d x %d", width, height);
  return;
}

void renderer_vulkan_clear_color(void *backend_state, float r, float g, float b,
                                 float a) {
  log_debug("Clearing Vulkan color to %f %f %f %f", r, g, b, a);
  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   BackendResourceHandle pipeline_handle) {
  log_debug("Binding Vulkan pipeline");
  return;
}

void renderer_vulkan_bind_vertex_buffer(void *backend_state,
                                        BackendResourceHandle buffer_handle,
                                        uint32_t binding_index,
                                        uint64_t offset) {
  log_debug("Binding Vulkan vertex buffer");
  return;
}

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance) {
  log_debug("Drawing Vulkan vertices");
  return;
}
