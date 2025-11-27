#include "renderer/renderer_frontend.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/vec.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/shader_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/systems/views/vkr_view_skybox.h"
#include "renderer/systems/views/vkr_view_ui.h"
#include "renderer/systems/views/vkr_view_world.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vulkan/vulkan_backend.h"

static RendererFrontend *g_renderer_rt_refresh = NULL;

vkr_internal void
renderer_frontend_regenerate_render_targets(RendererFrontend *rf);
vkr_internal void renderer_frontend_on_target_refresh_required(void);

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

vkr_internal void
renderer_frontend_regenerate_render_targets(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  vkr_view_system_rebuild_targets(rf);
}

vkr_internal void renderer_frontend_on_target_refresh_required(void) {
  if (g_renderer_rt_refresh) {
    renderer_frontend_regenerate_render_targets(g_renderer_rt_refresh);
  }
}

bool32_t vkr_renderer_initialize(VkrRendererFrontendHandle renderer,
                                 VkrRendererBackendType backend_type,
                                 VkrWindow *window, EventManager *event_manager,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
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
  renderer->view_system = (VkrViewSystem){0};
  renderer->mesh_manager = (VkrMeshManager){0};
  renderer->camera_system = (VkrCameraSystem){0};
  renderer->active_camera = VKR_CAMERA_HANDLE_INVALID;
  renderer->camera_controller = (VkrCameraController){0};
  renderer->globals = (VkrGlobalMaterialState){
      .ambient_color = vec4_new(0.1, 0.1, 0.1, 1.0),
      .render_mode = VKR_RENDER_MODE_DEFAULT,
  };
  renderer->rf_mutex = NULL;
  renderer->world_layer = VKR_LAYER_HANDLE_INVALID;
  renderer->skybox_layer = VKR_LAYER_HANDLE_INVALID;
  renderer->ui_layer = VKR_LAYER_HANDLE_INVALID;
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
  VkrRenderPassConfig pass_configs[3] = {
      // Skybox renders first, clears everything
      {.name = string8_lit("Renderpass.Builtin.Skybox"),
       .prev_name = {0},
       .next_name = string8_lit("Renderpass.Builtin.World"),
       .domain = VKR_PIPELINE_DOMAIN_SKYBOX,
       .render_area = (Vec4){0, 0, (float32_t)width, (float32_t)height},
       .clear_color = (Vec4){0.0f, 0.0f, 0.0f, 1.0f},
       .clear_flags = VKR_RENDERPASS_CLEAR_COLOR | VKR_RENDERPASS_CLEAR_DEPTH},
      // World loads skybox content, uses depth without clearing
      {.name = string8_lit("Renderpass.Builtin.World"),
       .prev_name = string8_lit("Renderpass.Builtin.Skybox"),
       .next_name = string8_lit("Renderpass.Builtin.UI"),
       .domain = VKR_PIPELINE_DOMAIN_WORLD,
       .render_area = (Vec4){0, 0, (float32_t)width, (float32_t)height},
       .clear_color = (Vec4){0.1f, 0.1f, 0.2f, 1.0f},
       .clear_flags = VKR_RENDERPASS_USE_DEPTH}, // LOAD color, use depth
      // UI loads world content, no depth
      {.name = string8_lit("Renderpass.Builtin.UI"),
       .prev_name = string8_lit("Renderpass.Builtin.World"),
       .next_name = (String8){0},
       .domain = VKR_PIPELINE_DOMAIN_UI,
       .render_area = (Vec4){0, 0, (float32_t)width, (float32_t)height},
       .clear_color = (Vec4){0, 0, 0, 0},
       .clear_flags = VKR_RENDERPASS_CLEAR_NONE}};

  VkrRendererBackendConfig local_backend_config = {
      .application_name = "vulkan_renderer",
      .renderpass_count = ArrayCount(pass_configs),
      .pass_configs = pass_configs,
      .on_render_target_refresh_required =
          renderer_frontend_on_target_refresh_required,
  };

  const VkrRendererBackendConfig *backend_cfg =
      backend_config ? backend_config : &local_backend_config;
  g_renderer_rt_refresh = renderer;

  if (!renderer->backend.initialize(&renderer->backend_state, backend_type,
                                    window, width, height, device_requirements,
                                    backend_cfg)) {
    g_renderer_rt_refresh = NULL;
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

  vkr_view_system_shutdown(rf);
  vkr_pipeline_registry_shutdown(&rf->pipeline_registry);

  vkr_shader_system_shutdown(&rf->shader_system);
  vkr_texture_system_shutdown(rf, &rf->texture_system);
  vkr_mesh_manager_shutdown(&rf->mesh_manager);
  vkr_material_system_shutdown(&rf->material_system);
  vkr_geometry_system_shutdown(&rf->geometry_system);

  g_renderer_rt_refresh = NULL;

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

VkrTextureOpaqueHandle
vkr_renderer_create_writable_texture(VkrRendererFrontendHandle renderer,
                                     const VkrTextureDescription *description,
                                     VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTextureDescription desc_copy = *description;
  bitset8_set(&desc_copy.properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);

  VkrBackendResourceHandle handle = renderer->backend.texture_create(
      renderer->backend_state, &desc_copy, NULL);
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrTextureOpaqueHandle)handle.ptr;
}

VkrRendererError vkr_renderer_write_texture(VkrRendererFrontendHandle renderer,
                                            VkrTextureOpaqueHandle texture,
                                            const void *data, uint64_t size) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(data != NULL, "Data is NULL");
  assert_log(size > 0, "Size must be greater than 0");

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  return renderer->backend.texture_write(renderer->backend_state, handle, NULL,
                                         data, size);
}

VkrRendererError vkr_renderer_write_texture_region(
    VkrRendererFrontendHandle renderer, VkrTextureOpaqueHandle texture,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(region != NULL, "Region is NULL");
  assert_log(data != NULL, "Data is NULL");
  assert_log(size > 0, "Size must be greater than 0");

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  return renderer->backend.texture_write(renderer->backend_state, handle,
                                         region, data, size);
}

VkrRendererError vkr_renderer_resize_texture(VkrRendererFrontendHandle renderer,
                                             VkrTextureOpaqueHandle texture,
                                             uint32_t new_width,
                                             uint32_t new_height,
                                             bool8_t preserve_contents) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(new_width > 0, "New width must be greater than 0");
  assert_log(new_height > 0, "New height must be greater than 0");

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  return renderer->backend.texture_resize(renderer->backend_state, handle,
                                          new_width, new_height,
                                          preserve_contents);
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

VkrRenderPassHandle
vkr_renderer_renderpass_create(VkrRendererFrontendHandle renderer,
                               const VkrRenderPassConfig *cfg) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(cfg != NULL, "Render pass config is NULL");
  if (!renderer->backend.renderpass_create) {
    return NULL;
  }
  return renderer->backend.renderpass_create(renderer->backend_state, cfg);
}

void vkr_renderer_renderpass_destroy(VkrRendererFrontendHandle renderer,
                                     VkrRenderPassHandle pass) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!pass || !renderer->backend.renderpass_destroy) {
    return;
  }
  renderer->backend.renderpass_destroy(renderer->backend_state, pass);
}

VkrRenderPassHandle
vkr_renderer_renderpass_get(VkrRendererFrontendHandle renderer, String8 name) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.renderpass_get || name.length == 0) {
    return NULL;
  }
  RendererFrontend *rf = (RendererFrontend *)renderer;
  Scratch scratch = scratch_create(rf->scratch_arena);
  char *cstr =
      arena_alloc(scratch.arena, name.length + 1, ARENA_MEMORY_TAG_STRING);
  MemCopy(cstr, name.str, (size_t)name.length);
  cstr[name.length] = '\0';
  VkrRenderPassHandle handle =
      renderer->backend.renderpass_get(renderer->backend_state, cstr);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return handle;
}

VkrRenderTargetHandle
vkr_renderer_render_target_create(VkrRendererFrontendHandle renderer,
                                  const VkrRenderTargetDesc *desc,
                                  VkrRenderPassHandle pass) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(desc != NULL, "Render target description is NULL");
  if (!renderer->backend.render_target_create) {
    return NULL;
  }
  return renderer->backend.render_target_create(renderer->backend_state, desc,
                                                pass);
}

void vkr_renderer_render_target_destroy(VkrRendererFrontendHandle renderer,
                                        VkrRenderTargetHandle target,
                                        bool8_t free_internal_memory) {
  (void)free_internal_memory;
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!target || !renderer->backend.render_target_destroy) {
    return;
  }
  renderer->backend.render_target_destroy(renderer->backend_state, target);
}

VkrRendererError
vkr_renderer_begin_render_pass(VkrRendererFrontendHandle renderer,
                               VkrRenderPassHandle pass,
                               VkrRenderTargetHandle target) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active,
             "Begin render pass called outside of frame");
  if (!renderer->backend.begin_render_pass) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  return renderer->backend.begin_render_pass(renderer->backend_state, pass,
                                             target);
}

VkrRendererError
vkr_renderer_end_render_pass(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "End render pass called outside of frame");
  if (!renderer->backend.end_render_pass) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  return renderer->backend.end_render_pass(renderer->backend_state);
}

VkrTextureOpaqueHandle
vkr_renderer_window_attachment_get(VkrRendererFrontendHandle renderer,
                                   uint32_t image_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.window_attachment_get) {
    return NULL;
  }
  return renderer->backend.window_attachment_get(renderer->backend_state,
                                                 image_index);
}

VkrTextureOpaqueHandle
vkr_renderer_depth_attachment_get(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.depth_attachment_get) {
    return NULL;
  }
  return renderer->backend.depth_attachment_get(renderer->backend_state);
}

uint32_t
vkr_renderer_window_attachment_count(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.window_attachment_count_get) {
    return 0;
  }
  return renderer->backend.window_attachment_count_get(renderer->backend_state);
}

uint32_t vkr_renderer_window_image_index(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.window_attachment_index_get) {
    return 0;
  }
  return renderer->backend.window_attachment_index_get(renderer->backend_state);
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

  rf->backend.on_resize(rf->backend_state, width, height);

  if (rf->rf_mutex) {
    vkr_mutex_lock(rf->rf_mutex);
  }
  rf->window->width = width;
  rf->window->height = height;
  rf->last_window_width = width;
  rf->last_window_height = height;
  if (rf->rf_mutex) {
    if (!vkr_mutex_unlock(rf->rf_mutex)) {
      log_error("Failed to unlock renderer mutex");
    }
  }

  vkr_view_system_on_resize(renderer, width, height);
  renderer_frontend_regenerate_render_targets(rf);
  vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);
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

  VkrCameraSystemConfig camera_cfg = {.max_camera_count = 24};
  if (!vkr_camera_registry_init(&camera_cfg, &rf->camera_system)) {
    log_fatal("Failed to initialize camera system");
    return false_v;
  }
  VkrCameraHandle default_camera = VKR_CAMERA_HANDLE_INVALID;
  if (!vkr_camera_registry_create_perspective(
          &rf->camera_system, string8_lit("camera.default"), rf->window, 60.0f,
          0.1f, 100.0f, &default_camera)) {
    log_fatal("Failed to create default camera");
    return false_v;
  }
  vkr_camera_registry_set_active(&rf->camera_system, default_camera);
  rf->active_camera = default_camera;

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

  if (!vkr_view_system_init(rf)) {
    log_fatal("Failed to initialize view system");
    return false_v;
  }

  if (!vkr_view_skybox_register(rf)) {
    log_error("Failed to register skybox view");
    return false_v;
  }

  if (!vkr_view_world_register(rf)) {
    log_error("Failed to register world view");
    return false_v;
  }

  if (!vkr_view_ui_register(rf)) {
    log_error("Failed to register UI view");
    return false_v;
  }

  return true_v;
}

void vkr_renderer_draw_frame(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;

  rf->frame_number++;

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; skipping draw frame");
    return;
  }

  uint32_t image_index = vkr_renderer_window_image_index(renderer);
  vkr_view_system_draw_all(renderer, image_index);
}
