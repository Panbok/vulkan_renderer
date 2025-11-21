#include "renderer/renderer_frontend.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_transform.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/shader_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vulkan/vulkan_backend.h"

typedef struct VkrRenderMeshEntry {
  uint32_t index;
  float depth;
} VkrRenderMeshEntry;

vkr_internal int vkr_render_mesh_entry_compare(const void *a, const void *b) {
  const VkrRenderMeshEntry *ea = (const VkrRenderMeshEntry *)a;
  const VkrRenderMeshEntry *eb = (const VkrRenderMeshEntry *)b;
  if (ea->depth < eb->depth)
    return 1;
  if (ea->depth > eb->depth)
    return -1;
  return 0;
}

vkr_internal void renderer_frontend_recompute_ui_globals(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");
  assert_log(rf->window != NULL, "Window is NULL");

  VkrWindowPixelSize sz = vkr_window_get_pixel_size(rf->window);
  rf->globals.ui_view = mat4_identity();
  rf->globals.ui_projection = mat4_ortho(
      0.0f, (float32_t)sz.width, (float32_t)sz.height, 0.0f, -1.0f, 1.0f);
}

vkr_internal bool8_t vkr_renderer_on_window_resize(Event *event,
                                                   UserData user_data) {
  assert(event != NULL && "Event is NULL");
  assert(event->type == EVENT_TYPE_WINDOW_RESIZE &&
         "Event is not a window resize event");

  RendererFrontend *rf = (RendererFrontend *)user_data;
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return false_v;
  }

  VkrWindowResizeEventData *resize = (VkrWindowResizeEventData *)event->data;
  if (!resize) {
    log_error("VkrWindowResizeEventData is NULL");
    return false_v;
  }

  if (resize->width == 0 || resize->height == 0) {
    log_debug("Skipping resize with zero dimensions: %ux%u", resize->width,
              resize->height);
    return true_v;
  }

  vkr_renderer_resize(rf, resize->width, resize->height);
  return true_v;
}

bool32_t vkr_renderer_initialize(VkrRendererFrontendHandle renderer,
                                 VkrRendererBackendType backend_type,
                                 VkrWindow *window, EventManager *event_manager,
                                 VkrDeviceRequirements *device_requirements,
                                 VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(window != NULL, "Window is NULL");
  assert_log(event_manager != NULL, "Event manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  log_debug("Creating renderer");

  renderer->arena = arena_create(MB(6));
  if (!renderer->arena) {
    log_fatal("Failed to create renderer arena!");
    return false_v;
  }

  renderer->scratch_arena = arena_create(MB(1), KB(8));
  if (!renderer->scratch_arena) {
    log_fatal("Failed to create scratch_arena!");
    return false_v;
  }

  // Initialize struct in-place
  renderer->backend_type = backend_type;
  renderer->window = window;
  renderer->event_manager = event_manager;
  renderer->frame_active = false;
  renderer->backend_state = NULL;

  // Clear high-level state
  renderer->pipeline_registry = (VkrPipelineRegistry){0};
  renderer->shader_system = (VkrShaderSystem){0};
  renderer->geometry_system = (VkrGeometrySystem){0};
  renderer->texture_system = (VkrTextureSystem){0};
  renderer->material_system = (VkrMaterialSystem){0};
  renderer->mesh_manager = (VkrMeshManager){0};
  renderer->camera = (VkrCamera){0};
  renderer->camera_controller = (VkrCameraController){0};
  renderer->globals = (VkrGlobalMaterialState){
      .ambient_color = vec4_new(0.1, 0.1, 0.1, 1.0),
      .render_mode = VKR_RENDER_MODE_DEFAULT,
  };
  renderer->rf_mutex = NULL;
  renderer->world_shader_config = (VkrShaderConfig){0};
  renderer->ui_shader_config = (VkrShaderConfig){0};
  renderer->world_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  renderer->ui_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  renderer->ui_material = VKR_MATERIAL_HANDLE_INVALID;
  renderer->ui_instance_state = (VkrRendererInstanceStateHandle){0};
  renderer->draw_state = (VkrShaderStateObject){.instance_state = {0}};
  renderer->frame_number = 0;

  // Create renderer mutex and initialize size tracking
  if (!vkr_mutex_create(renderer->arena, &renderer->rf_mutex)) {
    log_fatal("Failed to create renderer mutex!");
    return false_v;
  }

  VkrWindowPixelSize initial = vkr_window_get_pixel_size(window);
  renderer->last_window_width = initial.width;
  renderer->last_window_height = initial.height;

  if (backend_type == VKR_RENDERER_BACKEND_TYPE_VULKAN) {
    renderer->backend = renderer_vulkan_get_interface();
  } else {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return false_v;
  }

  uint32_t width = (uint32_t)window->width;
  uint32_t height = (uint32_t)window->height;
  if (!renderer->backend.initialize(&renderer->backend_state, backend_type,
                                    window, width, height,
                                    device_requirements)) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }

  // Subscribe to window resize events internally
  event_manager_subscribe(renderer->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                          vkr_renderer_on_window_resize, renderer);

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_renderer_destroy(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  log_debug("Destroying renderer");

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Ensure GPU idle before tearing down
  vkr_renderer_wait_idle(rf);

  // Release per-mesh local renderer state before destroying pipelines
  uint32_t mesh_capacity = vkr_mesh_manager_capacity(&rf->mesh_manager);
  for (uint32_t i = 0; i < mesh_capacity; ++i) {
    VkrMesh *m = vkr_mesh_manager_get(&rf->mesh_manager, i);
    if (!m)
      continue;
    uint32_t submesh_count = vkr_mesh_manager_submesh_count(m);
    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, i, submesh_index);
      if (!submesh || submesh->pipeline.id == 0)
        continue;
      vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, submesh->pipeline, submesh->instance_state,
          &(VkrRendererError){0});
      submesh->pipeline = VKR_PIPELINE_HANDLE_INVALID;
      submesh->instance_state = (VkrRendererInstanceStateHandle){0};
    }
  }

  if (rf->world_pipeline.id)
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           rf->world_pipeline);
  if (rf->ui_pipeline.id)
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           rf->ui_pipeline);
  vkr_pipeline_registry_shutdown(&rf->pipeline_registry);

  vkr_shader_system_shutdown(&rf->shader_system);
  vkr_texture_system_shutdown(rf, &rf->texture_system);
  vkr_mesh_manager_shutdown(&rf->mesh_manager);
  vkr_material_system_shutdown(&rf->material_system);
  vkr_geometry_system_shutdown(&rf->geometry_system);

  if (renderer->backend_state && renderer->backend.shutdown) {
    renderer->backend.shutdown(renderer->backend_state);
  }

  if (rf->rf_mutex) {
    vkr_mutex_destroy(renderer->arena, &rf->rf_mutex);
  }

  arena_destroy(renderer->arena);
  arena_destroy(renderer->scratch_arena);
}

String8 vkr_renderer_get_error_string(VkrRendererError error) {
  switch (error) {
  case VKR_RENDERER_ERROR_NONE:
    return string8_lit("No error");
  case VKR_RENDERER_ERROR_UNKNOWN:
    return string8_lit("Unknown error");
  case VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED:
    return string8_lit("Backend not supported");
  case VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED:
    return string8_lit("Resource creation failed");
  case VKR_RENDERER_ERROR_INVALID_HANDLE:
    return string8_lit("Invalid handle");
  case VKR_RENDERER_ERROR_INVALID_PARAMETER:
    return string8_lit("Invalid parameter");
  case VKR_RENDERER_ERROR_SHADER_COMPILATION_FAILED:
    return string8_lit("Shader compilation failed");
  case VKR_RENDERER_ERROR_OUT_OF_MEMORY:
    return string8_lit("Out of memory");
  case VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED:
    return string8_lit("Command recording failed");
  case VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED:
    return string8_lit("Frame preparation failed");
  case VKR_RENDERER_ERROR_PRESENTATION_FAILED:
    return string8_lit("Presentation failed");
  case VKR_RENDERER_ERROR_FRAME_IN_PROGRESS:
    return string8_lit("Frame in progress");
  case VKR_RENDERER_ERROR_DEVICE_ERROR:
    return string8_lit("Device error");
  case VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED:
    return string8_lit("Pipeline state update failed");
  case VKR_RENDERER_ERROR_FILE_NOT_FOUND:
    return string8_lit("File not found");
  case VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED:
    return string8_lit("Resource not loaded");
  default:
    return string8_lit("Unknown error");
  }
}

VkrWindow *vkr_renderer_get_window(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->window;
}

VkrRendererBackendType
vkr_renderer_get_backend_type(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->backend_type;
}

void vkr_renderer_get_device_information(
    VkrRendererFrontendHandle renderer,
    VkrDeviceInformation *device_information, Arena *temp_arena) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  renderer->backend.get_device_information(renderer->backend_state,
                                           device_information, temp_arena);
}

bool32_t vkr_renderer_is_frame_active(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->frame_active;
}

VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->backend.wait_idle(renderer->backend_state);
}

VkrBufferHandle vkr_renderer_create_buffer(
    VkrRendererFrontendHandle renderer, const VkrBufferDescription *description,
    const void *initial_data, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating buffer");

  VkrBackendResourceHandle handle = renderer->backend.buffer_create(
      renderer->backend_state, description, initial_data);
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrBufferHandle)handle.ptr;
}

VkrBufferHandle
vkr_renderer_create_vertex_buffer(VkrRendererFrontendHandle renderer,
                                  uint64_t size, const void *initial_data,
                                  VkrRendererError *out_error) {
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  VkrBufferDescription desc = {
      .size = size,
      .memory_properties =
          vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_VERTEX_BUFFER |
                                                VKR_BUFFER_USAGE_TRANSFER_DST |
                                                VKR_BUFFER_USAGE_TRANSFER_SRC),
      .bind_on_create = true_v,
      .buffer_type = buffer_type};

  return vkr_renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

VkrBufferHandle vkr_renderer_create_index_buffer(
    VkrRendererFrontendHandle renderer, uint64_t size, VkrIndexType type,
    const void *initial_data, VkrRendererError *out_error) {
  // Note: type parameter is for documentation/validation, the actual buffer
  // doesn't need to know the index type (that's specified at bind time)
  (void)type; // Suppress unused parameter warning

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  VkrBufferDescription desc = {
      .size = size,
      .memory_properties =
          vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER |
                                                VKR_BUFFER_USAGE_TRANSFER_DST |
                                                VKR_BUFFER_USAGE_TRANSFER_SRC),
      .bind_on_create = true_v,
      .buffer_type = buffer_type};

  return vkr_renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  log_debug("Destroying buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.buffer_destroy(renderer->backend_state, handle);
}

VkrTextureOpaqueHandle
vkr_renderer_create_texture(VkrRendererFrontendHandle renderer,
                            const VkrTextureDescription *description,
                            const void *initial_data,
                            VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating texture");

  VkrBackendResourceHandle handle = renderer->backend.texture_create(
      renderer->backend_state, description, initial_data);
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrTextureOpaqueHandle)handle.ptr;
}

void vkr_renderer_destroy_texture(VkrRendererFrontendHandle renderer,
                                  VkrTextureOpaqueHandle texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  log_debug("Destroying texture");

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  renderer->backend.texture_destroy(renderer->backend_state, handle);
}

VkrRendererError
vkr_renderer_update_texture(VkrRendererFrontendHandle renderer,
                            VkrTextureOpaqueHandle texture,
                            const VkrTextureDescription *description) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(description != NULL, "Description is NULL");

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  return renderer->backend.texture_update(renderer->backend_state, handle,
                                          description);
}

VkrPipelineOpaqueHandle vkr_renderer_create_graphics_pipeline(
    VkrRendererFrontendHandle renderer,
    const VkrGraphicsPipelineDescription *description,
    VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating pipeline");

  VkrBackendResourceHandle handle = renderer->backend.graphics_pipeline_create(
      renderer->backend_state, description);
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrPipelineOpaqueHandle)handle.ptr;
}

VkrRendererError vkr_renderer_update_pipeline_state(
    VkrRendererFrontendHandle renderer, VkrPipelineOpaqueHandle pipeline,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  return renderer->backend.pipeline_update_state(
      renderer->backend_state, handle, uniform, data, material);
}

VkrRendererError
vkr_renderer_update_global_state(VkrRendererFrontendHandle renderer,
                                 VkrPipelineOpaqueHandle pipeline,
                                 const void *uniform) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");
  assert_log(uniform != NULL, "Uniform is NULL");

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  return renderer->backend.pipeline_update_state(renderer->backend_state,
                                                 handle, uniform, NULL, NULL);
}

VkrRendererError
vkr_renderer_update_instance_state(VkrRendererFrontendHandle renderer,
                                   VkrPipelineOpaqueHandle pipeline,
                                   const VkrShaderStateObject *data,
                                   const VkrRendererMaterialState *material) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");
  assert_log(data != NULL, "Data is NULL");

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  return renderer->backend.pipeline_update_state(renderer->backend_state,
                                                 handle, NULL, data, material);
}

VkrRendererError vkr_renderer_acquire_instance_state(
    VkrRendererFrontendHandle renderer, VkrPipelineOpaqueHandle pipeline,
    VkrRendererInstanceStateHandle *out_handle) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  return renderer->backend.instance_state_acquire(renderer->backend_state,
                                                  handle, out_handle);
}

VkrRendererError
vkr_renderer_release_instance_state(VkrRendererFrontendHandle renderer,
                                    VkrPipelineOpaqueHandle pipeline,
                                    VkrRendererInstanceStateHandle handle) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  VkrBackendResourceHandle h = {.ptr = (void *)pipeline};
  return renderer->backend.instance_state_release(renderer->backend_state, h,
                                                  handle);
}

void vkr_renderer_destroy_pipeline(VkrRendererFrontendHandle renderer,
                                   VkrPipelineOpaqueHandle pipeline) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  log_debug("Destroying pipeline");

  // Wait for GPU to be idle to ensure no command buffers are still using this
  // pipeline
  renderer->backend.wait_idle(renderer->backend_state);

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  renderer->backend.pipeline_destroy(renderer->backend_state, handle);
}

VkrRendererError vkr_renderer_update_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  log_debug("Updating buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_update(renderer->backend_state, handle,
                                         offset, size, data);
}

VkrRendererError vkr_renderer_upload_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  log_debug("Uploading buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_upload(renderer->backend_state, handle,
                                         offset, size, data);
}
VkrRendererError vkr_renderer_begin_frame(VkrRendererFrontendHandle renderer,
                                          float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }

  VkrRendererError result =
      renderer->backend.begin_frame(renderer->backend_state, delta_time);
  if (result == VKR_RENDERER_ERROR_NONE) {
    renderer->frame_active = true;
  }

  return result;
}

void vkr_renderer_resize(VkrRendererFrontendHandle renderer, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  log_debug("Resizing renderer to %d %d", width, height);

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Synchronize resize as it may be called from an event thread
  vkr_mutex_lock(rf->rf_mutex);
  rf->backend.on_resize(rf->backend_state, width, height);
  rf->window->width = width;
  rf->window->height = height;

  rf->last_window_width = width;
  rf->last_window_height = height;

  renderer_frontend_recompute_ui_globals(rf);
  vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);

  if (!vkr_mutex_unlock(rf->rf_mutex)) {
    log_error("Failed to unlock renderer mutex");
  }
}

void vkr_renderer_bind_vertex_buffer(VkrRendererFrontendHandle renderer,
                                     const VkrVertexBufferBinding *binding) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(binding != NULL, "Binding is NULL");
  assert_log(binding->buffer != NULL, "Buffer is NULL");
  assert_log(renderer->frame_active,
             "Bind vertex buffer called outside of frame");

  VkrBackendResourceHandle handle = {.ptr = (void *)binding->buffer};
  renderer->backend.bind_buffer(renderer->backend_state, handle,
                                binding->offset);
}

void vkr_renderer_bind_index_buffer(VkrRendererFrontendHandle renderer,
                                    const VkrIndexBufferBinding *binding) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(binding != NULL, "Binding is NULL");
  assert_log(binding->buffer != NULL, "Buffer is NULL");
  assert_log(renderer->frame_active,
             "Bind index buffer called outside of frame");

  VkrBackendResourceHandle handle = {.ptr = (void *)binding->buffer};
  renderer->backend.bind_buffer(renderer->backend_state, handle,
                                binding->offset);
}

void vkr_renderer_draw(VkrRendererFrontendHandle renderer,
                       uint32_t vertex_count, uint32_t instance_count,
                       uint32_t first_vertex, uint32_t first_instance) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "Draw called outside of frame");

  renderer->backend.draw(renderer->backend_state, vertex_count, instance_count,
                         first_vertex, first_instance);
}

void vkr_renderer_draw_indexed(VkrRendererFrontendHandle renderer,
                               uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "Draw indexed called outside of frame");

  renderer->backend.draw_indexed(renderer->backend_state, index_count,
                                 instance_count, first_index, vertex_offset,
                                 first_instance);
}

VkrRendererError
vkr_renderer_begin_render_pass(VkrRendererFrontendHandle renderer,
                               VkrPipelineDomain domain) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active,
             "Begin render pass called outside of frame");
  assert_log(domain != VKR_PIPELINE_DOMAIN_COUNT, "Invalid domain");

  return renderer->backend.begin_render_pass(renderer->backend_state, domain);
}

VkrRendererError
vkr_renderer_end_render_pass(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "End render pass called outside of frame");

  return renderer->backend.end_render_pass(renderer->backend_state);
}

VkrRendererError vkr_renderer_end_frame(VkrRendererFrontendHandle renderer,
                                        float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  if (!renderer->frame_active) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkrRendererError result =
      renderer->backend.end_frame(renderer->backend_state, delta_time);
  renderer->frame_active = false;

  // Collect backend telemetry metrics
  vkr_pipeline_registry_collect_backend_telemetry(&renderer->pipeline_registry);

  return result;
}

uint64_t vkr_renderer_get_and_reset_descriptor_writes_avoided(
    VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.get_and_reset_descriptor_writes_avoided) {
    return 0;
  }
  return renderer->backend.get_and_reset_descriptor_writes_avoided(
      renderer->backend_state);
}

bool32_t vkr_renderer_systems_initialize(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;

  if (!vkr_pipeline_registry_init(&rf->pipeline_registry, rf, NULL)) {
    log_fatal("Failed to initialize pipeline registry");
    return false_v;
  }

  VkrShaderSystemConfig shader_cfg = VKR_SHADER_SYSTEM_CONFIG_DEFAULT;
  if (!vkr_shader_system_initialize(&rf->shader_system, shader_cfg)) {
    log_fatal("Failed to initialize shader system");
    return false_v;
  }
  // todo: shader sys should accepts pipeline registry as a parameter
  vkr_shader_system_set_registry(&rf->shader_system, &rf->pipeline_registry);

  if (!vkr_resource_system_init(rf->arena, rf)) {
    log_fatal("Failed to initialize resource system");
    return false_v;
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrGeometrySystemConfig geo_cfg = {.max_geometries = 200000};
  if (!vkr_geometry_system_init(&rf->geometry_system, rf, &geo_cfg,
                                &renderer_error)) {
    String8 err_str = vkr_renderer_get_error_string(renderer_error);
    log_fatal("Failed to initialize geometry system: %s",
              string8_cstr(&err_str));
    return false_v;
  }
  log_info("Geometry system max geometries=%u", geo_cfg.max_geometries);

  VkrTextureSystemConfig tex_cfg = {.max_texture_count = 1024};
  if (!vkr_texture_system_init(rf, &tex_cfg, &rf->texture_system)) {
    log_fatal("Failed to initialize texture system");
    return false_v;
  }

  VkrMaterialSystemConfig mat_cfg = {.max_material_count = 1024};
  if (!vkr_material_system_init(&rf->material_system, rf->arena,
                                &rf->texture_system, &rf->shader_system,
                                &mat_cfg)) {
    log_fatal("Failed to initialize material system");
    return false_v;
  }

  VkrMeshManagerConfig mesh_cfg = {.max_mesh_count = 1024};
  if (!vkr_mesh_manager_init(&rf->mesh_manager, &rf->geometry_system,
                             &rf->material_system, &rf->pipeline_registry,
                             &mesh_cfg)) {
    log_fatal("Failed to initialize mesh manager");
    return false_v;
  }

  rf->mesh_loader =
      (VkrMeshLoaderContext){.arena = rf->arena,
                             .scratch_arena = rf->scratch_arena,
                             .geometry_system = &rf->geometry_system,
                             .material_system = &rf->material_system,
                             .mesh_manager = &rf->mesh_manager};
  rf->mesh_loader.allocator.ctx = rf->mesh_loader.scratch_arena;
  vkr_allocator_arena(&rf->mesh_loader.allocator);

  vkr_resource_system_register_loader((void *)&rf->texture_system,
                                      vkr_texture_loader_create());
  vkr_resource_system_register_loader((void *)&rf->material_system,
                                      vkr_material_loader_create());
  vkr_resource_system_register_loader((void *)&rf->shader_system,
                                      vkr_shader_loader_create());
  vkr_resource_system_register_loader((void *)&rf->mesh_loader,
                                      vkr_mesh_loader_create(&rf->mesh_loader));

  // Compute initial cached globals (camera initialized by application)
  renderer_frontend_recompute_ui_globals(rf);

  return true_v;
}

bool32_t vkr_renderer_default_scene(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Load shader configs via resource system
  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;

  VkrResourceHandleInfo world_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world.shadercfg"),
          rf->scratch_arena, &world_cfg_info, &shadercfg_err)) {
    rf->world_shader_config = *(VkrShaderConfig *)world_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_fatal("World shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  VkrResourceHandleInfo ui_cfg_info = {0};
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.ui.shadercfg"), rf->scratch_arena,
          &ui_cfg_info, &shadercfg_err)) {
    rf->ui_shader_config = *(VkrShaderConfig *)ui_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_fatal("UI shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  // Create shaders in shader system
  vkr_shader_system_create(&rf->shader_system, &rf->world_shader_config);
  vkr_shader_system_create(&rf->shader_system, &rf->ui_shader_config);

  // Load default materials via resource system
  VkrResourceHandleInfo default_material_info = {0};
  VkrRendererError material_load_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/materials/default.world.mt"),
                               rf->scratch_arena, &default_material_info,
                               &material_load_error)) {
    log_info("Successfully loaded default material from "
             "assets/materials/default.world.mt");
    rf->world_material = default_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn("Failed to load default material from "
             "assets/materials/default.world.mt; using "
             "built-in default: %s",
             string8_cstr(&error_string));
  }

  VkrResourceHandleInfo default_ui_material_info = {0};
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/materials/default.ui.mt"),
                               rf->scratch_arena, &default_ui_material_info,
                               &material_load_error)) {
    log_info("Successfully loaded default UI material from "
             "assets/materials/default.ui.mt");
    rf->ui_material = default_ui_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn("Failed to load default UI material from"
             "assets/materials/default.ui.mt; using "
             "built-in default: %s",
             string8_cstr(&error_string));
  }

  // Create pipelines from shader configs
  if (rf->world_pipeline.id == 0 &&
      vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &rf->world_shader_config,
          VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world"), &rf->world_pipeline,
          &pipeline_error)) {
    log_debug("Config-first world pipeline created");
    if (rf->world_shader_config.name.str &&
        rf->world_shader_config.name.length > 0) {
      VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_alias_pipeline_name(
          &rf->pipeline_registry, rf->world_pipeline,
          rf->world_shader_config.name, &alias_err);
    }
  } else {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_fatal("Config world pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  if (rf->ui_pipeline.id == 0 &&
      vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &rf->ui_shader_config, VKR_PIPELINE_DOMAIN_UI,
          string8_lit("ui"), &rf->ui_pipeline, &pipeline_error)) {
    log_debug("Config-first UI pipeline created");
    if (rf->ui_shader_config.name.str && rf->ui_shader_config.name.length > 0) {
      VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_alias_pipeline_name(
          &rf->pipeline_registry, rf->ui_pipeline, rf->ui_shader_config.name,
          &alias_err);
    }
  } else {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_fatal("Config UI pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  // VkrRendererError mesh_error = VKR_RENDERER_ERROR_NONE;
  // VkrSubMeshDesc cube_submeshes[] = {{
  //     .geometry =
  //         vkr_geometry_system_get_default_geometry(&rf->geometry_system),
  //     .material = rf->world_material,
  //     .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  //     .owns_geometry = true_v,
  //     .owns_material = true_v,
  // }};
  // VkrMeshDesc cube_desc = {
  //     .transform = vkr_transform_from_position_scale_rotation(
  //         vec3_new(0.0f, 1.0f, 0.0f), vec3_new(0.15f, 0.15f, 0.15f),
  //         vkr_quat_identity()),
  //     .submeshes = cube_submeshes,
  //     .submesh_count = ArrayCount(cube_submeshes),
  // };

  // VkrMesh *cube_mesh_ptr = NULL;
  // if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc, &mesh_error,
  //                              &cube_mesh_ptr)) {
  //   String8 err_str = vkr_renderer_get_error_string(mesh_error);
  //   log_fatal("Failed to create cube mesh: %s", string8_cstr(&err_str));
  //   return false_v;
  // }

  // VkrSubMeshDesc cube2_submeshes[] = {{
  //     .geometry =
  //         vkr_geometry_system_get_default_geometry(&rf->geometry_system),
  //     .material = rf->world_material,
  //     .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  //     .owns_geometry = true_v,
  //     .owns_material = true_v,
  // }};
  // VkrMeshDesc cube_desc_2 = {
  //     .transform = vkr_transform_from_position_scale_rotation(
  //         vec3_new(12.0f, 0.0f, 0.0f), vec3_new(0.5f, 0.5f, 0.5f),
  //         vkr_quat_identity()),
  //     .submeshes = cube2_submeshes,
  //     .submesh_count = ArrayCount(cube2_submeshes),
  // };

  // VkrMesh *cube_mesh_2_ptr = NULL;
  // if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc_2, &mesh_error,
  //                              &cube_mesh_2_ptr)) {
  //   String8 err_str = vkr_renderer_get_error_string(mesh_error);
  //   log_fatal("Failed to create cube mesh 2: %s", string8_cstr(&err_str));
  //   return false_v;
  // }
  // vkr_transform_set_parent(&cube_mesh_2_ptr->transform,
  //                          &cube_mesh_ptr->transform);

  // VkrSubMeshDesc cube3_submeshes[] = {{
  //     .geometry =
  //         vkr_geometry_system_get_default_geometry(&rf->geometry_system),
  //     .material = rf->world_material,
  //     .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  //     .owns_geometry = true_v,
  //     .owns_material = true_v,
  // }};
  // VkrMeshDesc cube_desc_3 = {
  //     .transform = vkr_transform_from_position_scale_rotation(
  //         vec3_new(10.0f, 0.0f, 0.0f), vec3_new(0.3f, 0.3f, 0.3f),
  //         vkr_quat_identity()),
  //     .submeshes = cube3_submeshes,
  //     .submesh_count = ArrayCount(cube3_submeshes),
  // };

  // VkrMesh *cube_mesh_3_ptr = NULL;
  // if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc_3, &mesh_error,
  //                              &cube_mesh_3_ptr)) {
  //   String8 err_str = vkr_renderer_get_error_string(mesh_error);
  //   log_fatal("Failed to create cube mesh 3: %s", string8_cstr(&err_str));
  //   return false_v;
  // }
  // vkr_transform_set_parent(&cube_mesh_3_ptr->transform,
  //                          &cube_mesh_2_ptr->transform);

  VkrRendererError mesh_load_err = VKR_RENDERER_ERROR_NONE;

  uint32_t falcon_mesh_index = VKR_INVALID_ID;
  VkrMeshLoadDesc falcon_desc = {
      .mesh_path = string8_lit("assets/models/falcon.obj"),
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(0.0f, 0.2f, -15.0f), vec3_new(0.2f, 0.2f, 0.2f),
          vkr_quat_identity()),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .shader_override = {0},
  };
  if (!vkr_mesh_manager_load(&rf->mesh_manager, &falcon_desc,
                             &falcon_mesh_index, NULL, &mesh_load_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_load_err);
    log_error("Failed to load falcon mesh: %s", string8_cstr(&err));
  }

  VkrMesh *falcon_mesh =
      vkr_mesh_manager_get(&rf->mesh_manager, falcon_mesh_index);
  if (!falcon_mesh) {
    log_error("Falcon mesh not found");
    return false_v;
  }

  VkrSubMeshDesc *falcon_submeshes =
      arena_alloc(rf->mesh_manager.arena,
                  sizeof(VkrSubMeshDesc) * falcon_mesh->submeshes.length,
                  ARENA_MEMORY_TAG_ARRAY);
  for (uint32_t i = 0; i < falcon_mesh->submeshes.length; i++) {
    falcon_submeshes[i] = (VkrSubMeshDesc){
        .geometry = falcon_mesh->submeshes.data[i].geometry,
        .material = falcon_mesh->submeshes.data[i].material,
        .pipeline_domain = falcon_mesh->submeshes.data[i].pipeline_domain,
        .shader_override = falcon_mesh->submeshes.data[i].shader_override,
        .owns_geometry = falcon_mesh->submeshes.data[i].owns_geometry,
        .owns_material = falcon_mesh->submeshes.data[i].owns_material,
    };
  }

  VkrMeshDesc falcon_desc2 = {
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(5.0f, 0.2f, -15.0f), vec3_new(0.2f, 0.2f, 0.2f),
          vkr_quat_identity()),
      .submeshes = falcon_submeshes,
      .submesh_count = falcon_mesh->submeshes.length,
  };
  if (!vkr_mesh_manager_add(&rf->mesh_manager, &falcon_desc2, NULL,
                            &mesh_load_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_load_err);
    log_error("Failed to add falcon mesh: %s", string8_cstr(&err));
  }

  VkrMeshLoadDesc sponza_desc = {
      .mesh_path = string8_lit("assets/models/sponza.obj"),
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(0.0f, 0.0f, -15.0f), vec3_new(0.0085f, 0.0085f, 0.0085f),
          vkr_quat_identity()),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .shader_override = {0},
  };
  mesh_load_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_load(&rf->mesh_manager, &sponza_desc, NULL, NULL,
                             &mesh_load_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_load_err);
    log_error("Failed to load sponza mesh: %s", string8_cstr(&err));
  }

  rf->ui_transform = vkr_transform_from_position_scale_rotation(
      vec3_new(0.0f, 0.0f, 0.0f), vec3_new(150.0f, 150.0f, 1.0f),
      vkr_quat_identity());

  // Acquire per-instance local state for UI
  VkrRendererError ui_ls_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, rf->ui_pipeline, &rf->ui_instance_state,
          &ui_ls_err)) {
    log_fatal("Failed to acquire local renderer state for UI pipeline");
    return false_v;
  }

  return true_v;
}

void vkr_renderer_draw_frame(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;

  rf->frame_number++;

  //====================== WORLD START =======================

  VkrRendererError begin_err = VKR_RENDERER_ERROR_NONE;
  begin_err =
      vkr_renderer_begin_render_pass(renderer, VKR_PIPELINE_DOMAIN_WORLD);
  if (begin_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(begin_err);
    log_error("Failed to begin world render pass: %s", string8_cstr(&err_str));
    return;
  }

  uint32_t mesh_capacity = vkr_mesh_manager_capacity(&rf->mesh_manager);
  bool8_t globals_applied = false_v;
  for (uint32_t i = 0; i < mesh_capacity; i++) {
    VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, i);
    if (!mesh)
      continue;

    Mat4 model = mesh->model;
    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0)
      continue;

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, i, submesh_index);
      if (!submesh)
        continue;

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      const char *material_shader = (material && material->shader_name &&
                                     material->shader_name[0] != '\0')
                                        ? material->shader_name
                                        : "shader.default.world";
      if (!vkr_shader_system_use(&rf->shader_system, material_shader)) {
        vkr_shader_system_use(&rf->shader_system, "shader.default.world");
      }

      uint32_t mat_pipeline_id =
          (material && material->pipeline_id != VKR_INVALID_ID)
              ? material->pipeline_id
              : (uint32_t)submesh->pipeline_domain;

      VkrPipelineHandle resolved = VKR_PIPELINE_HANDLE_INVALID;
      VkrRendererError get_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_get_pipeline_for_material(
          &rf->pipeline_registry, NULL, mat_pipeline_id, &resolved, &get_err);

      VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_mesh_manager_refresh_pipeline(
              &rf->mesh_manager, i, submesh_index, resolved, &refresh_err)) {
        String8 err_str = vkr_renderer_get_error_string(refresh_err);
        log_error("Mesh %u submesh %u failed to refresh pipeline: %s", i,
                  submesh_index, string8_cstr(&err_str));
        continue;
      }

      rf->draw_state.instance_state = submesh->instance_state;

      VkrPipelineHandle current_pipeline =
          vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
      if (current_pipeline.id != resolved.id ||
          current_pipeline.generation != resolved.generation) {
        VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
        vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, resolved,
                                            &bind_err);
      }

      if (!globals_applied) {
        vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                         VKR_PIPELINE_DOMAIN_WORLD);
        globals_applied = true_v;
      }

      vkr_material_system_apply_local(&rf->material_system,
                                      &(VkrLocalMaterialState){.model = model});

      if (material) {
        vkr_shader_system_bind_instance(&rf->shader_system,
                                        submesh->instance_state.id);

        bool8_t should_apply_instance =
            (submesh->last_render_frame != rf->frame_number);
        if (should_apply_instance) {
          vkr_material_system_apply_instance(&rf->material_system, material,
                                             VKR_PIPELINE_DOMAIN_WORLD);
          submesh->last_render_frame = rf->frame_number;
        }
      }

      vkr_geometry_system_render(rf, &rf->geometry_system, submesh->geometry,
                                 1);
    }
  }

  VkrRendererError end_err = VKR_RENDERER_ERROR_NONE;
  end_err = vkr_renderer_end_render_pass(renderer);
  if (end_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(end_err);
    log_error("Failed to end world render pass: %s", string8_cstr(&err_str));
    return;
  }

  //====================== WORLD END =======================

  //====================== UI START ========================

  begin_err = VKR_RENDERER_ERROR_NONE;
  begin_err = vkr_renderer_begin_render_pass(renderer, VKR_PIPELINE_DOMAIN_UI);
  if (begin_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(begin_err);
    log_error("Failed to begin UI render pass: %s", string8_cstr(&err_str));
    return;
  }

  // Resolve material via handle
  VkrMaterial *ui_material =
      vkr_material_system_get_by_handle(&rf->material_system, rf->ui_material);

  // Prepare draw state
  rf->draw_state.instance_state = rf->ui_instance_state;

  // Resolve UI pipeline from material's shader_name/pipeline id and geometry
  VkrPipelineHandle ui_resolved = VKR_PIPELINE_HANDLE_INVALID;
  VkrRendererError ui_get_err = VKR_RENDERER_ERROR_NONE;
  uint32_t ui_mat_pipeline_id =
      ui_material ? ui_material->pipeline_id : VKR_INVALID_ID;
  const char *ui_shader =
      (ui_material && ui_material->shader_name && ui_material->shader_name[0])
          ? ui_material->shader_name
          : "shader.default.ui";
  if (!vkr_shader_system_use(&rf->shader_system, ui_shader)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.ui");
  }

  vkr_pipeline_registry_get_pipeline_for_material(&rf->pipeline_registry,
                                                  ui_shader, ui_mat_pipeline_id,
                                                  &ui_resolved, &ui_get_err);

  // If pipeline changed, reacquire instance state
  if (rf->ui_pipeline.id != ui_resolved.id ||
      rf->ui_pipeline.generation != ui_resolved.generation) {
    if (rf->ui_pipeline.id != 0) {
      VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, rf->ui_pipeline, rf->ui_instance_state,
          &rel_err);
    }
    VkrRendererError acq_err = VKR_RENDERER_ERROR_NONE;
    if (vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, ui_resolved, &rf->ui_instance_state,
            &acq_err)) {
      rf->ui_pipeline = ui_resolved;
    } else {
      String8 err_str = vkr_renderer_get_error_string(acq_err);
      log_error("Failed to acquire instance state for resolved pipeline: %s",
                string8_cstr(&err_str));
    }
  }

  // Ensure shader is selected before binding pipeline
  if (ui_material && ui_material->shader_name) {
    vkr_shader_system_use(&rf->shader_system, ui_material->shader_name);
  }

  // Ensure correct pipeline is bound
  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_pipeline.id != ui_resolved.id ||
      current_pipeline.generation != ui_resolved.generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, ui_resolved,
                                        &bind_err);
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_UI);

  // Apply local state
  vkr_material_system_apply_local(
      &rf->material_system,
      &(VkrLocalMaterialState){.model =
                                   vkr_transform_get_world(&rf->ui_transform)});

  if (ui_material) {
    // Apply instance state if it has changed since last frame (Vulkan
    // doesn't like applying the same instance state twice)
    // bool8_t should_apply_instance =
    //     ui_material->render_frame_number != rf->frame_number;
    // if (should_apply_instance) {
    //   ui_material->render_frame_number = rf->frame_number;
    // }
    vkr_shader_system_bind_instance(&rf->shader_system,
                                    rf->ui_instance_state.id);
    vkr_material_system_apply_instance(&rf->material_system, ui_material,
                                       VKR_PIPELINE_DOMAIN_UI);
  }

  vkr_geometry_system_render(
      rf, &rf->geometry_system,
      vkr_geometry_system_get_default_plane2d(&rf->geometry_system), 1);

  end_err = VKR_RENDERER_ERROR_NONE;
  end_err = vkr_renderer_end_render_pass(renderer);
  if (end_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(end_err);
    log_error("Failed to end UI render pass: %s", string8_cstr(&err_str));
    return;
  }

  //====================== UI END ==========================
}
