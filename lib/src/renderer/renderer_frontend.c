#include "renderer/renderer_frontend.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/passes/vkr_pass_copy.h"
#include "renderer/passes/vkr_pass_editor.h"
#include "renderer/passes/vkr_pass_ibl_bake.h"
#include "renderer/passes/vkr_pass_picking.h"
#include "renderer/passes/vkr_pass_shadow.h"
#include "renderer/passes/vkr_pass_skybox.h"
#include "renderer/passes/vkr_pass_tonemap.h"
#include "renderer/passes/vkr_pass_ui.h"
#include "renderer/passes/vkr_pass_world.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/resources/loaders/shader_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_capture.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vkr_rg_json.h"
#include "renderer/vulkan/vulkan_backend.h"
#if defined(PLATFORM_APPLE)
#include "renderer/metal/vkr_metal_packet_renderer.h"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static RendererFrontend *g_renderer_rt_refresh = NULL;

#define VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL MB(2)
#define VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE MB(32)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL MB(8)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE MB(256)

vkr_internal void
renderer_frontend_regenerate_render_targets(RendererFrontend *rf);
vkr_internal void renderer_frontend_on_target_refresh_required(void);
vkr_internal bool8_t
renderer_frontend_validate_render_graph(RendererFrontend *rf);
VkrRendererError vkr_renderer_begin_frame(VkrRendererFrontendHandle renderer,
                                          float64_t delta_time);
VkrRendererError vkr_renderer_end_frame(VkrRendererFrontendHandle renderer,
                                        float64_t delta_time);
VkrRendererError vkr_renderer_cancel_frame(VkrRendererFrontendHandle renderer);

vkr_internal void
renderer_frontend_destroy_loader_async_allocators(RendererFrontend *rf) {
  if (!rf) {
    return;
  }

  if (rf->scene_async_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->scene_async_mutex);
    rf->scene_async_mutex = NULL;
  }
  if (rf->scene_async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->scene_async_allocator);
    rf->scene_async_allocator = (VkrAllocator){0};
    rf->scene_async_memory = (VkrDMemory){0};
  }
  if (rf->mesh_loader.async_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->mesh_loader.async_mutex);
    rf->mesh_loader.async_mutex = NULL;
  }
  if (rf->mesh_loader.async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->mesh_loader.async_allocator);
    rf->mesh_loader.async_allocator = (VkrAllocator){0};
    rf->mesh_loader.async_memory = (VkrDMemory){0};
  }
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
    return true_v;
  }

  uint64_t packed = ((uint64_t)resize->width << 32) | (uint64_t)resize->height;
  vkr_atomic_uint64_store(&rf->pending_resize_mailbox, packed,
                          VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

vkr_internal void
renderer_frontend_regenerate_render_targets(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");
  if (rf->render_graph) {
    vkr_rg_invalidate_render_targets(rf->render_graph);
  }
}

vkr_internal void renderer_frontend_on_target_refresh_required(void) {
  if (g_renderer_rt_refresh) {
    renderer_frontend_regenerate_render_targets(g_renderer_rt_refresh);
  }
}

vkr_internal bool8_t
renderer_frontend_validate_render_graph(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  const char *graph_path = "assets/render_graphs/main.rendergraph.json";
  if (rf->render_graph_loaded) {
    return true_v;
  }
  VkrAllocator *scratch = &rf->scratch_allocator;
  VkrAllocatorScope scope = {0};
  if (vkr_allocator_supports_scopes(scratch)) {
    scope = vkr_allocator_begin_scope(scratch);
  }

  VkrRgJsonGraph graph = {0};
  bool8_t ok = vkr_rg_json_load_file(scratch, graph_path, &graph);
  vkr_rg_json_destroy(&graph);

  if (vkr_allocator_scope_is_valid(&scope)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  return ok;
}

static const VkrSubsystemMask
    vkr_renderer_subsystem_dependencies[VKR_RENDERER_SUBSYSTEM_COUNT] = {
        [VKR_RENDERER_SUBSYSTEM_CAMERA] = 0u,
        [VKR_RENDERER_SUBSYSTEM_PIPELINES] = 0u,
        [VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH] = 0u,
        [VKR_RENDERER_SUBSYSTEM_FRAME_STREAMS] = 0u,
        [VKR_RENDERER_SUBSYSTEM_SHADERS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES),
        [VKR_RENDERER_SUBSYSTEM_RESOURCES] = 0u,
        [VKR_RENDERER_SUBSYSTEM_GEOMETRY] = 0u,
        [VKR_RENDERER_SUBSYSTEM_TEXTURES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_MATERIALS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS),
        [VKR_RENDERER_SUBSYSTEM_MESHES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES),
        [VKR_RENDERER_SUBSYSTEM_FONTS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_LIGHTING] = 0u,
        [VKR_RENDERER_SUBSYSTEM_SHADOWS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_WORLD] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
        [VKR_RENDERER_SUBSYSTEM_UI] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_FONTS),
        [VKR_RENDERER_SUBSYSTEM_SKYBOX] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_EDITOR] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MESHES),
        [VKR_RENDERER_SUBSYSTEM_GIZMO] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MESHES),
        [VKR_RENDERER_SUBSYSTEM_PICKING] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PIPELINES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SHADERS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
};

bool8_t vkr_renderer_subsystem_plan_build(VkrBootProfile profile,
                                          VkrSubsystemMask requested_mask,
                                          VkrSubsystemMask excluded_mask,
                                          VkrSubsystemPlan *out_plan,
                                          VkrRendererError *out_error) {
  VkrRendererError discarded_error = VKR_RENDERER_ERROR_NONE;
  if (!out_error) {
    out_error = &discarded_error;
  }
  if (!out_plan || profile > VKR_BOOT_PROFILE_AUTOMATION ||
      ((requested_mask | excluded_mask) & ~VKR_RENDERER_SUBSYSTEM_ALL) != 0u ||
      (profile == VKR_BOOT_PROFILE_FULL && excluded_mask != 0u)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSubsystemMask effective =
      profile == VKR_BOOT_PROFILE_FULL
          ? VKR_RENDERER_SUBSYSTEM_ALL
          : VKR_RENDERER_SUBSYSTEM_MANDATORY | requested_mask;
  /* Iterated to a fixed point rather than swept once: the dependency table is
     ordered for readability, not topologically, so a later unit may pull in an
     earlier one that has dependencies of its own. The mask only ever grows
     within a bounded bit space, so this terminates. */
  VkrSubsystemMask prior = 0u;
  while (prior != effective) {
    prior = effective;
    for (uint32_t subsystem = 0u; subsystem < VKR_RENDERER_SUBSYSTEM_COUNT;
         ++subsystem) {
      if ((effective & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u) {
        effective |= vkr_renderer_subsystem_dependencies[subsystem];
      }
    }
  }
  if ((effective & excluded_mask) != 0u) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  *out_plan = (VkrSubsystemPlan){
      .profile = profile,
      .requested_mask = profile == VKR_BOOT_PROFILE_FULL
                            ? VKR_RENDERER_SUBSYSTEM_ALL
                            : requested_mask,
      .excluded_mask = excluded_mask,
      .effective_mask = effective,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_renderer_subsystem_plan_includes(const VkrSubsystemPlan *plan,
                                             VkrRendererSubsystem subsystem) {
  return plan && subsystem < VKR_RENDERER_SUBSYSTEM_COUNT &&
         (plan->effective_mask & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u;
}

/**
 * Narrows the plan to what actually came up.
 *
 * Editor, gizmo, and picking retain non-fatal startup behavior; picking is also
 * skipped on a zero-extent window. UI and skybox failures abort initialization
 * before this point. The reported mask is comparison identity: leaving an
 * absent unit set would make two runs that rendered different work compare as
 * the same observation.
 */
static void renderer_frontend_narrow_plan_to_initialized(RendererFrontend *rf) {
  const struct {
    VkrRendererSubsystem subsystem;
    bool8_t initialized;
  } observed[] = {
      {VKR_RENDERER_SUBSYSTEM_UI, rf->ui_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_SKYBOX, rf->skybox_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_EDITOR, rf->editor_viewport.initialized},
      {VKR_RENDERER_SUBSYSTEM_GIZMO, rf->gizmo_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_PICKING, rf->picking.initialized},
  };
  for (uint32_t i = 0u; i < ArrayCount(observed); ++i) {
    const VkrSubsystemMask bit =
        VKR_RENDERER_SUBSYSTEM_BIT(observed[i].subsystem);
    if (!observed[i].initialized && (rf->subsystem_plan.effective_mask & bit)) {
      log_warn("Planned renderer subsystem %u did not initialize; the reported "
               "mask omits it",
               (uint32_t)observed[i].subsystem);
      rf->subsystem_plan.effective_mask &= ~bit;
    }
  }
}

bool32_t vkr_renderer_initialize(VkrRendererFrontendHandle renderer,
                                 VkrRendererBackendType backend_type,
                                 VkrWindow *window, EventManager *event_manager,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 uint64_t target_frame_rate,
                                 VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(event_manager != NULL, "Event manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  VkrPresentTargetConfig requested_target = backend_config
                                                ? backend_config->present_target
                                                : (VkrPresentTargetConfig){0};
  if (requested_target.kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    if (requested_target.width == 0 || requested_target.height == 0 ||
        requested_target.image_count == 0) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      log_error("Offscreen target requires non-zero width, height, and image "
                "count");
      return false_v;
    }
  } else if (!window) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Windowed target requires a window");
    return false_v;
  }

  // if (!vkr_dmemory_create(MB(100), MB(500), &renderer->dmemory)) {
  //   log_fatal("Failed to create dmemory!");
  //   return false_v;
  // }

  // renderer->dmemory_allocator = (VkrAllocator){.ctx = &renderer->dmemory};
  // vkr_dmemory_allocator_create(&renderer->dmemory_allocator);

  renderer->arena = arena_create(
      backend_type == VKR_RENDERER_BACKEND_TYPE_METAL ? MB(64) : MB(6));
  if (!renderer->arena) {
    log_fatal("Failed to create renderer arena!");
    return false_v;
  }

  renderer->allocator = (VkrAllocator){.ctx = renderer->arena};
  if (!vkr_allocator_arena(&renderer->allocator)) {
    arena_destroy(renderer->arena);
    log_fatal("Failed to initialize renderer allocator!");
    return false_v;
  }

  renderer->scratch_arena = backend_type == VKR_RENDERER_BACKEND_TYPE_METAL
                                ? arena_create(MB(32), MB(1))
                                : arena_create(MB(1), KB(8));
  if (!renderer->scratch_arena) {
    log_fatal("Failed to create scratch_arena!");
    return false_v;
  }

  renderer->scratch_allocator = (VkrAllocator){.ctx = renderer->scratch_arena};
  if (!vkr_allocator_arena(&renderer->scratch_allocator)) {
    arena_destroy(renderer->scratch_arena);
    log_fatal("Failed to initialize scratch allocator!");
    return false_v;
  }

  // Initialize struct in-place
  renderer->backend_type = backend_type;
  renderer->window = window;
  renderer->present_target = requested_target;
  renderer->event_manager = event_manager;
  renderer->frame_active = false;
  renderer->backend_state = NULL;
  renderer->metal_renderer = NULL;
  renderer->asset_publisher = (VkrAssetPublisher){0};
  renderer->metal_timing_result = NULL;
  renderer->metal_timing_source_cpu_frame_index = 0;
  renderer->metal_last_completed_timing_submit_value = 0;
  renderer->metal_completed_timing_ready = false_v;
  renderer->supports_multi_draw_indirect = false_v;
  renderer->supports_draw_indirect_first_instance = false_v;

  // Clear high-level state
  renderer->pipeline_registry = (VkrPipelineRegistry){0};
  renderer->shader_system = (VkrShaderSystem){0};
  renderer->geometry_system = (VkrGeometrySystem){0};
  renderer->texture_system = (VkrTextureSystem){0};
  renderer->material_system = (VkrMaterialSystem){0};
  renderer->rg_executor_registry = (VkrRgExecutorRegistry){0};
  renderer->render_graph = NULL;
  renderer->render_graph_json = (VkrRgJsonGraph){0};
  renderer->render_graph_dmemory = (VkrDMemory){0};
  renderer->render_graph_allocator = (VkrAllocator){0};
  renderer->render_graph_loaded = false_v;
  renderer->render_graph_enabled = false_v;
  renderer->mesh_manager = (VkrMeshManager){0};
  renderer->mesh_loader = (VkrMeshLoaderContext){0};
  renderer->scene_async_memory = (VkrDMemory){0};
  renderer->scene_async_allocator = (VkrAllocator){0};
  renderer->scene_async_mutex = NULL;
  renderer->gizmo_system = (VkrGizmoSystem){0};
  renderer->lighting_system = (VkrLightingSystem){0};
  renderer->world_resources = (VkrWorldResources){0};
  renderer->ui_system = (VkrUiSystem){0};
  renderer->skybox_system = (VkrSkyboxSystem){0};
  renderer->instance_buffer_pool = (VkrInstanceBufferPool){0};
  renderer->indirect_draw_system = (VkrIndirectDrawSystem){0};
  renderer->active_scene = NULL;
  renderer->camera_system = (VkrCameraSystem){0};
  renderer->active_camera = VKR_CAMERA_HANDLE_INVALID;
  renderer->camera_controller = (VkrCameraController){0};
  renderer->globals = (VkrGlobalMaterialState){
      .ambient_color = vec4_new(0.1, 0.1, 0.1, 1.0),
      .exposure = VKR_DEFAULT_EXPOSURE,
      .render_mode = VKR_RENDER_MODE_DEFAULT,
  };
  renderer->frame_metrics = (VkrRendererFrameMetrics){0};
  renderer->boot_metrics = (VkrRendererBootMetrics){0};
  renderer->subsystem_plan = (VkrSubsystemPlan){0};
  renderer->rf_mutex = NULL;
  renderer->offscreen_color_handles = NULL;
  renderer->offscreen_color_handle_count = 0;
  renderer->draw_state =
      (VkrShaderStateObject){.instance_state = {.id = VKR_INVALID_ID}};
  renderer->frame_number = 0;

  if (!vkr_dmemory_create(MB(2), MB(16), &renderer->render_graph_dmemory)) {
    log_fatal("Failed to create render graph allocator!");
    arena_destroy(renderer->scratch_arena);
    arena_destroy(renderer->arena);
    return false_v;
  }
  renderer->render_graph_allocator =
      (VkrAllocator){.ctx = &renderer->render_graph_dmemory};
  vkr_dmemory_allocator_create(&renderer->render_graph_allocator);
  renderer->target_frame_rate = target_frame_rate;
  vkr_atomic_uint64_store(&renderer->pending_resize_mailbox, 0,
                          VKR_MEMORY_ORDER_RELAXED);

  // Create renderer mutex and initialize size tracking
  if (!vkr_mutex_create(&renderer->allocator, &renderer->rf_mutex)) {
    log_fatal("Failed to create renderer mutex!");
    return false_v;
  }

  VkrWindowPixelSize initial = requested_target.kind ==
                                       VKR_PRESENT_TARGET_OFFSCREEN
                                   ? (VkrWindowPixelSize){
                                         .width = requested_target.width,
                                         .height = requested_target.height,
                                     }
                                   : vkr_window_get_pixel_size(window);
  renderer->last_window_width = initial.width;
  renderer->last_window_height = initial.height;
  if (renderer->window) {
    renderer->window->width = initial.width;
    renderer->window->height = initial.height;
  }
  uint32_t width = initial.width;
  uint32_t height = initial.height;

  if (backend_type == VKR_RENDERER_BACKEND_TYPE_VULKAN) {
    renderer->backend = renderer_vulkan_get_interface();
  } else if (backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return false_v;
  }

  VkrRendererBackendConfig resolved_backend_config = {
      .application_name = "vulkan_renderer",
      .renderpass_desc_count = 0,
      .pass_descs = NULL,
      .on_render_target_refresh_required =
          renderer_frontend_on_target_refresh_required,
      .boot_metrics = &renderer->boot_metrics,
      .present_target = requested_target,
  };
  if (backend_config) {
    resolved_backend_config = *backend_config;
    resolved_backend_config.on_render_target_refresh_required =
        renderer_frontend_on_target_refresh_required;
    resolved_backend_config.boot_metrics = &renderer->boot_metrics;
  }
  const VkrRendererBackendConfig *backend_cfg = &resolved_backend_config;
  g_renderer_rt_refresh = renderer;

  if (backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    const uint32_t frame_slot_count = 3u;
    const uint32_t capture_capacity = backend_cfg->capture_ring_capacity > 0
                                          ? backend_cfg->capture_ring_capacity
                                          : frame_slot_count;
    const uint64_t capture_bytes = backend_cfg->capture_max_batch_bytes > 0
                                       ? backend_cfg->capture_max_batch_bytes
                                       : MB(32);
    VkrMetalPacketRendererConfig metal_config = {
        .allocator = &renderer->render_graph_allocator,
        .graph_path = "assets/render_graphs/main.rendergraph.json",
        .slang_msl_path = VKR_METAL_PACKET_SLANG_MSL,
        .fragment_msl_path = VKR_METAL_PACKET_FRAGMENT_MSL,
        .pipeline_archive_path = VKR_METAL_PACKET_ARCHIVE_PATH,
        .target_kind = requested_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
                           ? VKR_METAL_PACKET_TARGET_OFFSCREEN
                           : VKR_METAL_PACKET_TARGET_WINDOW,
        .metal_layer = window ? vkr_window_get_metal_layer(window) : NULL,
        // The measured Metal Bistro Gate A runs peak below 4.7 GiB. Keep one
        // explicit production heap with headroom for placement alignment and
        // render-graph images; exhaustion remains reported.
        .heap_size = GB(6),
        // Bistro's largest prepared mip payload exceeds 16 MiB. Three 128 MiB
        // upload slots cover the measured stream, while three 32 MiB readback
        // slots match the configured maximum capture batch.
        .upload_ring_size = MB(384),
        .readback_ring_size = MB(96),
        .frame_slot_count = frame_slot_count,
        .capture_ring_capacity = capture_capacity,
        .capture_max_batch_bytes = capture_bytes,
        .synchronous_validation_readback = false_v,
        .srgb_output = true_v,
        .convert_vulkan_clip_y = true_v,
        .max_images = 128,
        .max_passes = 64,
        .max_material_rows = 8192,
        .max_meshes = 16384,
        .max_submeshes_per_mesh = 512,
        .max_textures = 16384,
        .max_draws = 262144,
        .max_instances = 262144,
    };
    if (!vkr_metal_packet_renderer_create(&metal_config,
                                          &renderer->metal_renderer)) {
      g_renderer_rt_refresh = NULL;
      *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
      return false_v;
    }
    renderer->backend_state = renderer->metal_renderer;
    renderer->metal_timing_result = calloc(1, sizeof(VkrMetalPacketResult));
    if (!renderer->metal_timing_result) {
      vkr_metal_packet_renderer_destroy(renderer->metal_renderer);
      renderer->metal_renderer = NULL;
      renderer->backend_state = NULL;
      free(renderer->metal_timing_result);
      renderer->metal_timing_result = NULL;
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    vkr_metal_packet_renderer_get_asset_publisher(renderer->metal_renderer,
                                                  &renderer->asset_publisher);
    if (requested_target.kind != VKR_PRESENT_TARGET_OFFSCREEN) {
      event_manager_subscribe(renderer->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                              vkr_renderer_on_window_resize, renderer);
    }
    *out_error = VKR_RENDERER_ERROR_NONE;
    log_info("Selected Metal 4 packet renderer");
    return true_v;
#else
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return false_v;
#endif
  }

  if (!renderer->backend.initialize(&renderer->backend_state, backend_type,
                                    window, width, height, device_requirements,
                                    backend_cfg)) {
    g_renderer_rt_refresh = NULL;
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }

  VkrDeviceInformation device_info = {0};
  renderer->backend.get_device_information(
      renderer->backend_state, &device_info, renderer->scratch_arena);
  renderer->supports_multi_draw_indirect =
      device_info.supports_multi_draw_indirect;
  renderer->supports_draw_indirect_first_instance =
      device_info.supports_draw_indirect_first_instance;

  if (requested_target.kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    event_manager_subscribe(renderer->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                            vkr_renderer_on_window_resize, renderer);
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_renderer_destroy(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Destroying renderer");

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Ensure GPU idle before tearing down
  vkr_renderer_wait_idle(rf);

  if (rf->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    // Release per-mesh local renderer state before destroying pipelines.
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
        if (submesh->instance_state.id != VKR_INVALID_ID) {
          vkr_pipeline_registry_release_instance_state(
              &rf->pipeline_registry, submesh->pipeline,
              submesh->instance_state, &(VkrRendererError){0});
        }
        submesh->pipeline = VKR_PIPELINE_HANDLE_INVALID;
        submesh->instance_state =
            (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID};
      }
    }
  }

  // Shutdown picking system if initialized
  if (rf->picking.initialized) {
    vkr_picking_shutdown(rf, &rf->picking);
  }

  if (rf->editor_viewport.initialized) {
    vkr_editor_viewport_shutdown(rf, &rf->editor_viewport);
  }

  if (rf->ui_system.initialized) {
    vkr_ui_system_shutdown(rf, &rf->ui_system);
  }
  if (rf->world_resources.initialized) {
    vkr_world_resources_shutdown(rf, &rf->world_resources);
  }
  if (rf->skybox_system.initialized) {
    vkr_skybox_system_shutdown(rf, &rf->skybox_system);
  }

  if (rf->shadow_system.initialized) {
    vkr_shadow_system_shutdown(&rf->shadow_system, rf);
  }

  if (rf->gizmo_system.initialized) {
    vkr_gizmo_system_shutdown(&rf->gizmo_system, rf);
  }

  if (rf->render_graph) {
    vkr_rg_destroy(rf->render_graph);
    rf->render_graph = NULL;
  }
  if (rf->render_graph_loaded) {
    vkr_rg_json_destroy(&rf->render_graph_json);
    rf->render_graph_loaded = false_v;
  }

  vkr_rg_executor_registry_destroy(&rf->rg_executor_registry);
  vkr_lighting_system_shutdown(&rf->lighting_system);
  vkr_mesh_manager_shutdown(&rf->mesh_manager);
  if (rf->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    vkr_shader_system_shutdown(&rf->shader_system);
    vkr_pipeline_registry_shutdown(&rf->pipeline_registry);
    vkr_instance_buffer_pool_shutdown(&rf->instance_buffer_pool, rf);
    vkr_indirect_draw_shutdown(&rf->indirect_draw_system, rf);
  }
  vkr_font_system_shutdown(&rf->font_system);
  vkr_material_system_shutdown(&rf->material_system);
  vkr_geometry_system_shutdown(&rf->geometry_system);
  vkr_texture_system_shutdown(rf, &rf->texture_system);

  g_renderer_rt_refresh = NULL;

  if (rf->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    vkr_metal_packet_renderer_destroy(rf->metal_renderer);
    rf->metal_renderer = NULL;
    rf->backend_state = NULL;
    free(rf->metal_timing_result);
    rf->metal_timing_result = NULL;
#endif
  } else if (renderer->backend_state && renderer->backend.shutdown) {
    renderer->backend.shutdown(renderer->backend_state);
  }

  if (rf->rf_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->rf_mutex);
  }

  // Destroy mesh arena pool
  if (rf->mesh_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->mesh_arena_pool);
  }
  if (rf->bitmap_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->bitmap_font_arena_pool);
  }
  if (rf->system_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->system_font_arena_pool);
  }
  if (rf->mtsdf_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->mtsdf_font_arena_pool);
  }

  renderer_frontend_destroy_loader_async_allocators(rf);

  if (rf->render_graph_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->render_graph_allocator);
  }

  // vkr_dmemory_destroy(&renderer->dmemory);
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
  case VKR_RENDERER_ERROR_INITIALIZATION_FAILED:
    return string8_lit("Initialization failed");
  case VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE:
    return string8_lit("Incompatible signature");
  case VKR_RENDERER_ERROR_FRAME_SKIPPED:
    return string8_lit("Frame skipped");
  case VKR_RENDERER_ERROR_SUBMISSION_FAILED:
    return string8_lit("Queue submission failed");
  case VKR_RENDERER_ERROR_CAPTURE_BUSY:
    return string8_lit("Capture ring busy");
  case VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE:
    return string8_lit("Capture unavailable");
  case VKR_RENDERER_ERROR_COUNT:
    break;
  }
  return string8_lit("Unknown error");
}

VkrWindow *vkr_renderer_get_window(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->window;
}

uint64_t
vkr_renderer_get_target_frame_rate(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->target_frame_rate;
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
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    VkrDeviceTypeFlags device_types = bitset8_create();
    VkrDeviceQueueFlags device_queues = bitset8_create();
    VkrSamplerFilterFlags sampler_filters = bitset8_create();
    bitset8_set(&device_types, VKR_DEVICE_TYPE_INTEGRATED_BIT);
    bitset8_set(&device_queues, VKR_DEVICE_QUEUE_GRAPHICS_BIT);
    bitset8_set(&device_queues, VKR_DEVICE_QUEUE_TRANSFER_BIT);
    bitset8_set(&device_queues, VKR_DEVICE_QUEUE_PRESENT_BIT);
    bitset8_set(&sampler_filters, VKR_SAMPLER_FILTER_LINEAR_BIT);
    bitset8_set(&sampler_filters, VKR_SAMPLER_FILTER_ANISOTROPIC_BIT);
    *device_information = (VkrDeviceInformation){
        .device_name = string8_lit("Apple Metal 4 GPU"),
        .vendor_name = string8_lit("Apple"),
        .driver_version = string8_lit("Metal 4"),
        .api_version = string8_lit("Metal 4"),
        .device_types = device_types,
        .device_queues = device_queues,
        .sampler_filters = sampler_filters,
        .max_sampler_anisotropy = 16.0,
        .supports_texture_astc_4x4 = true_v,
        .supports_texture_bc7 = true_v,
        .supports_texture_bc5 = true_v,
        .supports_hdr_ibl = true_v,
        .hdr_ibl_max_cube_extent = VKR_IBL_PREFILTER_SIZE,
        .hdr_ibl_max_mip_levels = VKR_IBL_PREFILTER_MIP_COUNT,
        .actual_target_kind = renderer->present_target.kind,
        .actual_present_mode = VKR_PRESENT_MODE_FIFO,
        .actual_target_image_count = 3,
        .actual_target_width = renderer->last_window_width,
        .actual_target_height = renderer->last_window_height,
        .actual_color_format =
            renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
                ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
                : VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB,
        .actual_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT,
        .actual_color_space = VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR,
    };
    return;
  }
  renderer->backend.get_device_information(renderer->backend_state,
                                           device_information, temp_arena);
}

bool32_t vkr_renderer_is_frame_active(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->frame_active;
}

VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_wait_idle(renderer->metal_renderer)
               ? VKR_RENDERER_ERROR_NONE
               : VKR_RENDERER_ERROR_DEVICE_ERROR;
#else
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
  }
  return renderer->backend.wait_idle(renderer->backend_state);
}

uint64_t vkr_renderer_get_submit_serial(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_submit_value(renderer->metal_renderer);
#else
    return 0;
#endif
  }
  if (!renderer->backend.get_submit_serial) {
    return 0;
  }
  return renderer->backend.get_submit_serial(renderer->backend_state);
}

uint64_t
vkr_renderer_get_completed_submit_serial(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_completed_value(renderer->metal_renderer);
#else
    return 0;
#endif
  }
  if (!renderer->backend.get_completed_submit_serial) {
    return 0;
  }
  return renderer->backend.get_completed_submit_serial(renderer->backend_state);
}

bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRendererFrontendHandle renderer, VkrRendererUploadWaitStats *out_stats) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_stats != NULL, "Out stats is NULL");

  out_stats->fence_wait_count = 0;
  out_stats->queue_wait_idle_count = 0;
  out_stats->device_wait_idle_count = 0;

  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_get_and_reset_upload_wait_count(
        renderer->metal_renderer, &out_stats->fence_wait_count);
#else
    return false_v;
#endif
  }

  if (!renderer->backend.get_and_reset_upload_wait_stats) {
    return false_v;
  }

  return renderer->backend.get_and_reset_upload_wait_stats(
      renderer->backend_state, out_stats);
}

bool8_t vkr_renderer_get_and_reset_command_slot_wait_count(
    VkrRendererFrontendHandle renderer, uint64_t *out_wait_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_wait_count != NULL, "Out wait count is NULL");

  *out_wait_count = 0;
  if (renderer->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    return false_v;
  }

#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_get_and_reset_command_slot_wait_count(
      renderer->metal_renderer, out_wait_count);
#else
  return false_v;
#endif
}

bool8_t
vkr_renderer_get_last_present_duration(VkrRendererFrontendHandle renderer,
                                       uint64_t *out_duration_ns) {
  if (!renderer || !out_duration_ns ||
      !renderer->backend.get_last_present_duration) {
    return false_v;
  }
  return renderer->backend.get_last_present_duration(renderer->backend_state,
                                                     out_duration_ns);
}

bool8_t vkr_renderer_get_device_memory_stats(VkrRendererFrontendHandle renderer,
                                             VkrDeviceMemoryStats *out_stats) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_stats != NULL, "Out stats is NULL");

  MemZero(out_stats, sizeof(*out_stats));
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    VkrMetalMemoryDeviceMetrics metrics = {0};
    if (!vkr_metal_packet_renderer_get_memory_metrics(renderer->metal_renderer,
                                                      &metrics)) {
      return false_v;
    }
    /* These legacy rows count native device allocations. Placement records are
       published separately as memory.gpu.suballocations.*; mapping them here
       would silently change the meaning of the cross-backend metric. */
    out_stats->live_allocation_count = metrics.native_heap_size > 0 ? 1u : 0u;
    out_stats->peak_allocation_count = out_stats->live_allocation_count;
    out_stats->total_allocation_count = out_stats->live_allocation_count;
    out_stats->max_allocation_count = 1u;
    out_stats->live_bytes = metrics.native_heap_allocated_size;
    out_stats->peak_bytes = metrics.native_heap_peak_allocated_size;
    out_stats->live_totals_exact = true_v;
    out_stats->memory_type_count = 1;
    out_stats->live_bytes_by_type[0] = out_stats->live_bytes;
    out_stats->live_count_by_type[0] = out_stats->live_allocation_count;
    out_stats->heap_index_by_type[0] = 0;
    out_stats->heap_count = 1;
    out_stats->heap_size_bytes[0] = metrics.native_heap_size;
    out_stats->heap_usage_bytes[0] = metrics.native_heap_used_size;
    out_stats->heap_budget_bytes[0] =
        metrics.driver_recommended_working_set_size;
    out_stats->heap_usage_valid = true_v;
    return true_v;
#else
    return false_v;
#endif
  }
  if (!renderer->backend.get_device_memory_stats) {
    return false_v;
  }
  return renderer->backend.get_device_memory_stats(renderer->backend_state,
                                                   out_stats);
}

VkrBufferHandle vkr_renderer_create_buffer(
    VkrRendererFrontendHandle renderer, const VkrBufferDescription *description,
    const void *initial_data, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrBufferUploadPayload upload = {
      .data = initial_data,
      .size = description->size,
      .offset = 0,
  };
  VkrBufferBatchCreateRequest request = {
      .description = description,
      .upload = initial_data ? &upload : NULL,
  };
  VkrBufferHandle handle = NULL;
  if (vkr_renderer_create_buffer_batch(renderer, &request, 1, &handle,
                                       out_error) != 1 ||
      !handle) {
    if (*out_error == VKR_RENDERER_ERROR_NONE) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return NULL;
  }
  return handle;
}

uint32_t
vkr_renderer_create_buffer_batch(VkrRendererFrontendHandle renderer,
                                 const VkrBufferBatchCreateRequest *requests,
                                 uint32_t count, VkrBufferHandle *out_handles,
                                 VkrRendererError *out_errors) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(requests != NULL, "Requests is NULL");
  assert_log(count > 0, "Count must be > 0");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i] = NULL;
    out_errors[i] = VKR_RENDERER_ERROR_UNKNOWN;
  }

  if (renderer->backend.buffer_create_batch) {
    VkrBackendResourceHandle *backend_handles = vkr_allocator_alloc(
        &renderer->scratch_allocator, sizeof(VkrBackendResourceHandle) * count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!backend_handles) {
      for (uint32_t i = 0; i < count; ++i) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
      return 0;
    }

    uint32_t created = renderer->backend.buffer_create_batch(
        renderer->backend_state, requests, count, backend_handles, out_errors);
    for (uint32_t i = 0; i < count; ++i) {
      out_handles[i] = (VkrBufferHandle)backend_handles[i].ptr;
      if (out_handles[i] && out_errors[i] == VKR_RENDERER_ERROR_UNKNOWN) {
        out_errors[i] = VKR_RENDERER_ERROR_NONE;
      }
    }
    vkr_allocator_free(&renderer->scratch_allocator, backend_handles,
                       sizeof(VkrBackendResourceHandle) * count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return created;
  }

  uint32_t created = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrBufferBatchCreateRequest *request = &requests[i];
    if (!request->description) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }

    const void *initial_data = NULL;
    bool8_t requires_followup_upload = false_v;
    if (request->upload && request->upload->data && request->upload->size > 0) {
      const uint64_t upload_end =
          request->upload->offset + request->upload->size;
      if (upload_end < request->upload->offset ||
          upload_end > request->description->size) {
        out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
        continue;
      }
      if (request->upload->offset == 0 &&
          request->upload->size == request->description->size) {
        initial_data = request->upload->data;
      } else {
        requires_followup_upload = true_v;
      }
    }

    VkrBackendResourceHandle handle = renderer->backend.buffer_create(
        renderer->backend_state, request->description, initial_data);
    if (!handle.ptr) {
      out_errors[i] = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      continue;
    }

    if (requires_followup_upload) {
      VkrRendererError upload_error = renderer->backend.buffer_upload(
          renderer->backend_state, handle, request->upload->offset,
          request->upload->size, request->upload->data);
      if (upload_error != VKR_RENDERER_ERROR_NONE) {
        renderer->backend.buffer_destroy(renderer->backend_state, handle);
        out_errors[i] = upload_error;
        continue;
      }
    }

    out_handles[i] = (VkrBufferHandle)handle.ptr;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    created++;
  }

  return created;
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
      .buffer_type = buffer_type,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_MESH,
  };

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
      .buffer_type = buffer_type,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_MESH,
  };

  return vkr_renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

VkrBufferHandle vkr_renderer_create_vertex_buffer_dynamic(
    VkrRendererFrontendHandle renderer, uint64_t size, const void *initial_data,
    VkrRendererError *out_error) {
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  VkrBufferDescription desc = {
      .size = size,
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_VERTEX_BUFFER |
                                                VKR_BUFFER_USAGE_TRANSFER_DST),
      .bind_on_create = true_v,
      .buffer_type = buffer_type,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_MESH,
  };

  return vkr_renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

VkrBufferHandle vkr_renderer_create_index_buffer_dynamic(
    VkrRendererFrontendHandle renderer, uint64_t size, VkrIndexType type,
    const void *initial_data, VkrRendererError *out_error) {
  (void)type; // Suppress unused parameter warning

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  VkrBufferDescription desc = {
      .size = size,
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER |
                                                VKR_BUFFER_USAGE_TRANSFER_DST),
      .bind_on_create = true_v,
      .buffer_type = buffer_type,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_MESH,
  };

  return vkr_renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  // log_debug("Destroying buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.buffer_destroy(renderer->backend_state, handle);
}

/**
 * @brief Converts backend handle-returning create calls to frontend contract.
 *
 * Frontend create APIs consistently map NULL backend handles to
 * `VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED`.
 */
vkr_internal VkrTextureOpaqueHandle vkr_renderer_texture_create_result(
    VkrBackendResourceHandle handle, VkrRendererError *out_error) {
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrTextureOpaqueHandle)handle.ptr;
}

VkrTextureOpaqueHandle
vkr_renderer_create_texture(VkrRendererFrontendHandle renderer,
                            const VkrTextureDescription *description,
                            const void *initial_data,
                            VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // log_debug("Creating texture");

  VkrBackendResourceHandle handle = renderer->backend.texture_create(
      renderer->backend_state, description, initial_data);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_texture_with_payload(
    VkrRendererFrontendHandle renderer,
    const VkrTextureDescription *description,
    const VkrTextureUploadPayload *payload, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTextureBatchCreateRequest request = {
      .description = description,
      .payload = payload,
  };
  VkrTextureOpaqueHandle handle = NULL;
  if (vkr_renderer_create_texture_with_payload_batch(renderer, &request, 1,
                                                     &handle, out_error) != 1 ||
      !handle) {
    if (*out_error == VKR_RENDERER_ERROR_NONE) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return NULL;
  }
  return handle;
}

uint32_t vkr_renderer_create_texture_with_payload_batch(
    VkrRendererFrontendHandle renderer,
    const VkrTextureBatchCreateRequest *requests, uint32_t count,
    VkrTextureOpaqueHandle *out_handles, VkrRendererError *out_errors) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(requests != NULL, "Requests is NULL");
  assert_log(count > 0, "Count must be > 0");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i] = NULL;
    out_errors[i] = VKR_RENDERER_ERROR_UNKNOWN;
  }

  if (renderer->backend.texture_create_with_payload_batch) {
    VkrBackendResourceHandle *backend_handles = vkr_allocator_alloc(
        &renderer->scratch_allocator, sizeof(VkrBackendResourceHandle) * count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!backend_handles) {
      for (uint32_t i = 0; i < count; ++i) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
      return 0;
    }
    uint32_t created = renderer->backend.texture_create_with_payload_batch(
        renderer->backend_state, requests, count, backend_handles, out_errors);
    for (uint32_t i = 0; i < count; ++i) {
      out_handles[i] = (VkrTextureOpaqueHandle)backend_handles[i].ptr;
      if (out_handles[i] && out_errors[i] == VKR_RENDERER_ERROR_UNKNOWN) {
        out_errors[i] = VKR_RENDERER_ERROR_NONE;
      }
    }
    vkr_allocator_free(&renderer->scratch_allocator, backend_handles,
                       sizeof(VkrBackendResourceHandle) * count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return created;
  }

  if (!renderer->backend.texture_create_with_payload) {
    for (uint32_t i = 0; i < count; ++i) {
      out_errors[i] = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return 0;
  }

  uint32_t created = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!requests[i].description || !requests[i].payload) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }
    VkrBackendResourceHandle handle =
        renderer->backend.texture_create_with_payload(renderer->backend_state,
                                                      requests[i].description,
                                                      requests[i].payload);
    out_handles[i] = (VkrTextureOpaqueHandle)handle.ptr;
    if (!out_handles[i]) {
      out_errors[i] = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      continue;
    }
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    created++;
  }

  return created;
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
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture(
    VkrRendererFrontendHandle renderer, const VkrRenderTargetTextureDesc *desc,
    VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!renderer->backend.render_target_texture_create) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  VkrBackendResourceHandle handle =
      renderer->backend.render_target_texture_create(renderer->backend_state,
                                                     desc);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_depth_attachment(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    VkrTextureUsageFlags usage, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!renderer->backend.depth_attachment_create) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  VkrBackendResourceHandle handle = renderer->backend.depth_attachment_create(
      renderer->backend_state, width, height, usage);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_sampled_depth_attachment(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    VkrTextureUsageFlags usage, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!renderer->backend.sampled_depth_attachment_create) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  VkrBackendResourceHandle handle =
      renderer->backend.sampled_depth_attachment_create(renderer->backend_state,
                                                        width, height, usage);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_sampled_depth_attachment_array(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    uint32_t layers, VkrTextureUsageFlags usage, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (width == 0 || height == 0 || layers == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return NULL;
  }

  if (!renderer->backend.sampled_depth_attachment_array_create) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  VkrBackendResourceHandle handle =
      renderer->backend.sampled_depth_attachment_array_create(
          renderer->backend_state, width, height, layers, usage);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture_msaa(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    VkrTextureFormat format, VkrSampleCount samples,
    VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!renderer->backend.render_target_texture_msaa_create) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  VkrBackendResourceHandle handle =
      renderer->backend.render_target_texture_msaa_create(
          renderer->backend_state, width, height, format, samples);
  return vkr_renderer_texture_create_result(handle, out_error);
}

VkrRendererError vkr_renderer_image_barrier(
    VkrRendererFrontendHandle renderer, VkrTextureOpaqueHandle texture,
    VkrImageAccessFlags src_access, VkrImageAccessFlags dst_access,
    VkrTextureLayout old_layout, VkrTextureLayout new_layout,
    const VkrImageSubresourceRange *range) {
  const VkrGpuDependency dependency =
      vkr_gpu_image_dependency_default(src_access, dst_access);
  return vkr_renderer_image_barrier_scoped(renderer, texture, src_access,
                                           dst_access, old_layout, new_layout,
                                           range, &dependency);
}

VkrRendererError vkr_renderer_image_barrier_scoped(
    VkrRendererFrontendHandle renderer, VkrTextureOpaqueHandle texture,
    VkrImageAccessFlags src_access, VkrImageAccessFlags dst_access,
    VkrTextureLayout old_layout, VkrTextureLayout new_layout,
    const VkrImageSubresourceRange *range, const VkrGpuDependency *dependency) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");
  assert_log(dependency != NULL, "Dependency is NULL");

  if (!dependency) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (!renderer->backend.image_barrier) {
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  return renderer->backend.image_barrier(renderer->backend_state, handle,
                                         src_access, dst_access, old_layout,
                                         new_layout, range, dependency);
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

VkrRendererError vkr_renderer_copy_texture(VkrRendererFrontendHandle renderer,
                                           VkrTextureOpaqueHandle source,
                                           VkrTextureOpaqueHandle destination) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(source != NULL, "Source texture is NULL");
  assert_log(destination != NULL, "Destination texture is NULL");
  if (!renderer->backend.texture_copy) {
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }
  return renderer->backend.texture_copy(
      renderer->backend_state, (VkrBackendResourceHandle){.ptr = source},
      (VkrBackendResourceHandle){.ptr = destination});
}

bool8_t vkr_renderer_destroy_texture(VkrRendererFrontendHandle renderer,
                                     VkrTextureOpaqueHandle texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  if (!renderer->backend.texture_destroy) {
    return false_v;
  }

  VkrBackendResourceHandle handle = {.ptr = (void *)texture};
  renderer->backend.texture_destroy(renderer->backend_state, handle);
  return true_v;
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

  // log_debug("Creating pipeline");

  VkrBackendResourceHandle handle = renderer->backend.graphics_pipeline_create(
      renderer->backend_state, description);
  if (handle.ptr == NULL) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrPipelineOpaqueHandle)handle.ptr;
}

bool8_t vkr_renderer_pipeline_get_shader_runtime_layout(
    VkrRendererFrontendHandle renderer, VkrPipelineOpaqueHandle pipeline,
    VkrShaderRuntimeLayout *out_layout) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");
  assert_log(out_layout != NULL, "Out layout is NULL");

  if (!renderer->backend.pipeline_get_shader_runtime_layout) {
    return false_v;
  }

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  return renderer->backend.pipeline_get_shader_runtime_layout(
      renderer->backend_state, handle, out_layout);
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

  // log_debug("Destroying pipeline");

  // Wait for GPU to be idle to ensure no command buffers are still using this
  // pipeline
  renderer->backend.wait_idle(renderer->backend_state);

  VkrBackendResourceHandle handle = {.ptr = (void *)pipeline};
  renderer->backend.pipeline_destroy(renderer->backend_state, handle);
}

bool8_t vkr_renderer_create_ui_text(VkrRendererFrontendHandle renderer,
                                    const VkrUiTextCreateData *payload,
                                    uint32_t *out_text_id) {
  if (!renderer || !payload) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->ui_system.initialized) {
    return false_v;
  }
  return vkr_ui_system_text_create(rf, &rf->ui_system, payload, out_text_id);
}

bool8_t vkr_renderer_destroy_ui_text(VkrRendererFrontendHandle renderer,
                                     uint32_t text_id) {
  if (!renderer) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->ui_system.initialized) {
    return false_v;
  }
  return vkr_ui_system_text_destroy(rf, &rf->ui_system, text_id);
}

bool8_t vkr_renderer_create_world_text(VkrRendererFrontendHandle renderer,
                                       const VkrWorldTextCreateData *payload) {
  if (!renderer || !payload) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->world_resources.initialized) {
    return false_v;
  }
  return vkr_world_resources_text_create(rf, &rf->world_resources, payload);
}

bool8_t vkr_renderer_destroy_world_text(VkrRendererFrontendHandle renderer,
                                        uint32_t text_id) {
  if (!renderer) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->world_resources.initialized) {
    return false_v;
  }
  return vkr_world_resources_text_destroy(rf, &rf->world_resources, text_id);
}

VkrRendererError vkr_renderer_update_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  // log_debug("Updating buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_update(renderer->backend_state, handle,
                                         offset, size, data);
}

void *vkr_renderer_buffer_get_mapped_ptr(VkrRendererFrontendHandle renderer,
                                         VkrBufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  if (!renderer->backend.buffer_get_mapped_ptr) {
    return NULL;
  }
  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_get_mapped_ptr(renderer->backend_state,
                                                 handle);
}

VkrRendererError vkr_renderer_flush_buffer(VkrRendererFrontendHandle renderer,
                                           VkrBufferHandle buffer,
                                           uint64_t offset, uint64_t size) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  if (!renderer->backend.buffer_flush) {
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }
  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_flush(renderer->backend_state, handle, offset,
                                        size);
}

VkrRendererError vkr_renderer_buffer_barrier(VkrRendererFrontendHandle renderer,
                                             VkrBufferHandle buffer,
                                             VkrBufferAccessFlags src_access,
                                             VkrBufferAccessFlags dst_access) {
  const VkrGpuDependency dependency =
      vkr_gpu_buffer_dependency_default(src_access, dst_access);
  return vkr_renderer_buffer_barrier_scoped(renderer, buffer, src_access,
                                            dst_access, &dependency);
}

VkrRendererError vkr_renderer_buffer_barrier_scoped(
    VkrRendererFrontendHandle renderer, VkrBufferHandle buffer,
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access,
    const VkrGpuDependency *dependency) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  assert_log(dependency != NULL, "Dependency is NULL");
  if (!dependency) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (!renderer->backend.buffer_barrier) {
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }
  // The write-aware skip lives in the backend, which owns the access-to-Vulkan
  // mapping. Skipping here on equality alone would drop write-after-write
  // ordering before the backend ever sees it.
  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_barrier(renderer->backend_state, handle,
                                          src_access, dst_access, dependency);
}

void vkr_renderer_set_instance_buffer(VkrRendererFrontendHandle renderer,
                                      VkrBufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.set_instance_buffer) {
    return;
  }
  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.set_instance_buffer(renderer->backend_state, handle);
}

VkrRendererError vkr_renderer_upload_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  // log_debug("Uploading buffer");

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_upload(renderer->backend_state, handle,
                                         offset, size, data);
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
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&rf->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return NULL;
  }
  char *cstr = vkr_allocator_alloc(&rf->allocator, name.length + 1,
                                   VKR_ALLOCATOR_MEMORY_TAG_STRING);
  MemCopy(cstr, name.str, (size_t)name.length);
  cstr[name.length] = '\0';
  VkrRenderPassHandle handle =
      renderer->backend.renderpass_get(renderer->backend_state, cstr);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return handle;
}

bool8_t
vkr_renderer_renderpass_get_signature(VkrRendererFrontendHandle renderer,
                                      VkrRenderPassHandle pass,
                                      VkrRenderPassSignature *out_signature) {
  if (!renderer || !pass || !out_signature) {
    return false_v;
  }

  struct s_RenderPass *rp = (struct s_RenderPass *)pass;
  if (!rp->vk || rp->vk->handle == VK_NULL_HANDLE) {
    return false_v;
  }

  *out_signature = rp->vk->signature;
  return true_v;
}

bool8_t vkr_renderpass_signature_compatible(const VkrRenderPassSignature *a,
                                            const VkrRenderPassSignature *b) {
  assert_log(a != NULL, "A is NULL");
  assert_log(b != NULL, "B is NULL");

  if (a->color_attachment_count != b->color_attachment_count) {
    return false_v;
  }

  for (uint8_t i = 0; i < a->color_attachment_count; ++i) {
    if (a->color_formats[i] != b->color_formats[i] ||
        a->color_samples[i] != b->color_samples[i]) {
      return false_v;
    }
  }

  if (a->has_depth_stencil != b->has_depth_stencil) {
    return false_v;
  }

  if (a->has_depth_stencil) {
    if (a->depth_stencil_format != b->depth_stencil_format ||
        a->depth_stencil_samples != b->depth_stencil_samples) {
      return false_v;
    }
  }

  if (a->has_resolve_attachments != b->has_resolve_attachments ||
      a->resolve_attachment_count != b->resolve_attachment_count) {
    return false_v;
  }

  return true_v;
}

bool8_t vkr_renderer_domain_renderpass_set(VkrRendererFrontendHandle renderer,
                                           VkrPipelineDomain domain,
                                           VkrRenderPassHandle pass,
                                           VkrDomainOverridePolicy policy,
                                           VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pass != NULL, "Pass is NULL");

  if (!renderer->backend.domain_renderpass_set) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return false_v;
  }

  return renderer->backend.domain_renderpass_set(
      renderer->backend_state, domain, pass, policy, out_error);
}

VkrRenderPassHandle
vkr_renderer_renderpass_create_desc(VkrRendererFrontendHandle renderer,
                                    const VkrRenderPassDesc *desc,
                                    VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(desc != NULL, "Desc is NULL");

  if (!renderer->backend.renderpass_create_desc) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return NULL;
  }

  return renderer->backend.renderpass_create_desc(renderer->backend_state, desc,
                                                  out_error);
}

VkrRenderTargetHandle vkr_renderer_render_target_create(
    VkrRendererFrontendHandle renderer, const VkrRenderTargetDesc *desc,
    VkrRenderPassHandle pass, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(desc != NULL, "Desc is NULL");
  assert_log(pass != NULL, "Pass is NULL");

  if (!renderer->backend.render_target_create) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return NULL;
  }

  return renderer->backend.render_target_create(renderer->backend_state, desc,
                                                pass, out_error);
}

void vkr_renderer_render_target_destroy(VkrRendererFrontendHandle renderer,
                                        VkrRenderTargetHandle target) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(target != NULL, "Target is NULL");

  if (!renderer->backend.render_target_destroy) {
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

VkrTextureOpaqueHandle vkr_renderer_present_target_attachment_get(
    VkrRendererFrontendHandle renderer, VkrPresentTargetAttachment attachment,
    uint32_t image_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.present_target_attachment_get) {
    return NULL;
  }
  return renderer->backend.present_target_attachment_get(
      renderer->backend_state, attachment, image_index);
}

uint32_t
vkr_renderer_present_target_image_count(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    return 3u;
  }
  if (!renderer->backend.present_target_image_count_get) {
    return 0;
  }
  return renderer->backend.present_target_image_count_get(
      renderer->backend_state);
}

uint32_t
vkr_renderer_present_target_image_index(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.present_target_image_index_get) {
    return 0;
  }
  return renderer->backend.present_target_image_index_get(
      renderer->backend_state);
}

VkrPresentTargetKind
vkr_renderer_present_target_kind(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    return renderer->present_target.kind;
  }
  if (!renderer->backend.present_target_kind_get) {
    return VKR_PRESENT_TARGET_WINDOWED;
  }
  return renderer->backend.present_target_kind_get(renderer->backend_state);
}

void vkr_renderer_present_target_extent(VkrRendererFrontendHandle renderer,
                                        uint32_t *out_width,
                                        uint32_t *out_height) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend.present_target_extent_get) {
    renderer->backend.present_target_extent_get(renderer->backend_state,
                                                out_width, out_height);
    return;
  }
  if (out_width) {
    *out_width = renderer->last_window_width;
  }
  if (out_height) {
    *out_height = renderer->last_window_height;
  }
}

VkrTextureFormat
vkr_renderer_present_target_format(VkrRendererFrontendHandle renderer,
                                   VkrPresentTargetAttachment attachment) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    if (attachment == VKR_PRESENT_TARGET_ATTACHMENT_DEPTH)
      return VKR_TEXTURE_FORMAT_D32_SFLOAT;
    return renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
               ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
               : VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  }
  if (!renderer->backend.present_target_format_get) {
    return attachment == VKR_PRESENT_TARGET_ATTACHMENT_COLOR
               ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
               : VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }
  return renderer->backend.present_target_format_get(renderer->backend_state,
                                                     attachment);
}

VkrPresentTargetImageState
vkr_renderer_present_target_image_state(VkrRendererFrontendHandle renderer,
                                        VkrPresentTargetAttachment attachment,
                                        uint32_t image_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.present_target_image_state_get) {
    return (VkrPresentTargetImageState){0};
  }
  return renderer->backend.present_target_image_state_get(
      renderer->backend_state, attachment, image_index);
}

VkrPresentTargetImageState
vkr_renderer_present_target_terminal_state(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.present_target_terminal_state_get) {
    return (VkrPresentTargetImageState){
        .access = VKR_IMAGE_ACCESS_PRESENT,
        .layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
    };
  }
  return renderer->backend.present_target_terminal_state_get(
      renderer->backend_state);
}

VkrRendererError
vkr_renderer_present_target_recreate(VkrRendererFrontendHandle renderer,
                                     uint32_t width, uint32_t height,
                                     uint32_t image_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    if (width == 0 || height == 0 || image_count == 0) {
      return VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    VkrRendererError idle = vkr_renderer_wait_idle(renderer);
    if (idle != VKR_RENDERER_ERROR_NONE) {
      return idle;
    }
    renderer->present_target.width = width;
    renderer->present_target.height = height;
    renderer->present_target.image_count = 3u;
    renderer->last_window_width = width;
    renderer->last_window_height = height;
    if (renderer->ui_system.initialized) {
      vkr_ui_system_resize(renderer, &renderer->ui_system, width, height);
    }
    return VKR_RENDERER_ERROR_NONE;
  }
  if (!renderer->backend.present_target_recreate || width == 0 || height == 0 ||
      image_count == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkrRendererError result = renderer->backend.present_target_recreate(
      renderer->backend_state, width, height, image_count);
  if (result != VKR_RENDERER_ERROR_NONE) {
    return result;
  }

  vkr_renderer_present_target_extent(renderer, &width, &height);
  renderer->present_target.width = width;
  renderer->present_target.height = height;
  renderer->present_target.image_count =
      vkr_renderer_present_target_image_count(renderer);
  renderer->last_window_width = width;
  renderer->last_window_height = height;
  if (renderer->ui_system.initialized) {
    vkr_ui_system_resize(renderer, &renderer->ui_system, width, height);
  }
  vkr_pipeline_registry_mark_global_state_dirty(&renderer->pipeline_registry);
  return VKR_RENDERER_ERROR_NONE;
}

VkrTextureFormat
vkr_renderer_get_shadow_depth_format(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.shadow_depth_format_get) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }
  return renderer->backend.shadow_depth_format_get(renderer->backend_state);
}

uint32_t
vkr_renderer_frame_in_flight_index(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    return (uint32_t)(renderer->frame_number % 3u);
  }
  if (!renderer->backend.frame_in_flight_index_get) {
    return 0;
  }
  return renderer->backend.frame_in_flight_index_get(renderer->backend_state);
}

uint32_t
vkr_renderer_frame_in_flight_count(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    return 3;
  }
  if (!renderer->backend.frame_in_flight_count_get) {
    return 1;
  }
  return renderer->backend.frame_in_flight_count_get(renderer->backend_state);
}

static VkrRendererError
vkr_renderer_validation_fail(VkrValidationError *out_error,
                             VkrRendererError code, const char *field_path,
                             const char *message) {
  if (out_error) {
    out_error->code = code;
    out_error->field_path = field_path;
    out_error->message = message;
  }
  return code;
}

static VkrRendererError
vkr_renderer_validation_failf(VkrValidationError *out_error,
                              VkrRendererError code, const char *message,
                              const char *field_fmt, ...) {
  if (!out_error) {
    return code;
  }

  static _Thread_local char field_path[128];
  va_list args;
  va_start(args, field_fmt);
  vsnprintf(field_path, sizeof(field_path), field_fmt, args);
  va_end(args);
  out_error->code = code;
  out_error->field_path = field_path;
  out_error->message = message;
  return code;
}

static bool8_t
vkr_renderer_validate_pipeline_override(RendererFrontend *rf,
                                        VkrPipelineHandle pipeline_override,
                                        VkrPipelineDomain domain) {
  if (!rf || pipeline_override.id == 0) {
    return true_v;
  }

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(&rf->pipeline_registry,
                                          pipeline_override, &pipeline) ||
      !pipeline) {
    return false_v;
  }

  return pipeline->domain == domain;
}

static VkrRendererError
vkr_renderer_validate_draws(RendererFrontend *rf, const VkrDrawItem *draws,
                            uint32_t draw_count, uint32_t instance_count,
                            const char *field_prefix, VkrPipelineDomain domain,
                            VkrValidationError *out_error) {
  if (draw_count == 0) {
    return VKR_RENDERER_ERROR_NONE;
  }
  if (!draws) {
    return vkr_renderer_validation_fail(out_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        field_prefix, "draw list is NULL");
  }

  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->mesh.id == 0) {
      return vkr_renderer_validation_failf(
          out_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "mesh handle is invalid", "%s[%u].mesh", field_prefix, i);
    }
    if (draw->geometry.id == 0 || draw->geometry.generation == VKR_INVALID_ID) {
      return vkr_renderer_validation_failf(
          out_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "geometry handle is invalid", "%s[%u].geometry", field_prefix, i);
    }
    if (draw->instance_count == 0) {
      continue;
    }

    uint64_t end =
        (uint64_t)draw->first_instance + (uint64_t)draw->instance_count;
    if (end > instance_count) {
      return vkr_renderer_validation_failf(
          out_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "instance range exceeds payload instance_count",
          "%s[%u].first_instance", field_prefix, i);
    }

    if (draw->pipeline_override.id != 0 &&
        !vkr_renderer_validate_pipeline_override(rf, draw->pipeline_override,
                                                 domain)) {
      return vkr_renderer_validation_failf(
          out_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "pipeline_override is invalid for this pass domain",
          "%s[%u].pipeline_override", field_prefix, i);
    }
  }

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError vkr_renderer_prepare_frame(VkrRendererFrontendHandle renderer,
                                            VkrFrameSetup *out_setup) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_setup != NULL, "Frame setup is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;

  if (rf->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    if (rf->frame_active) {
      return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
    }
    if (rf->window) {
      VkrWindowPixelSize pixels = vkr_window_get_pixel_size(rf->window);
      if (pixels.width == 0 || pixels.height == 0) {
        return VKR_RENDERER_ERROR_FRAME_SKIPPED;
      }
      rf->last_window_width = pixels.width;
      rf->last_window_height = pixels.height;
    }
    rf->frame_active = true_v;
    vkr_resource_system_pump(NULL);
    vkr_mesh_manager_pump_async(&rf->mesh_manager);
    VkrRenderGraphFrameInfo frame = {
        .frame_index = (uint32_t)(rf->frame_number + 1u),
        .image_index = 0,
        .delta_time = 1.0 / 60.0,
        .target_width = rf->last_window_width,
        .target_height = rf->last_window_height,
        .window_width = rf->last_window_width,
        .window_height = rf->last_window_height,
        .viewport_width = rf->last_window_width,
        .viewport_height = rf->last_window_height,
        /* The packet is not available until submit. The Metal packet renderer
           patches this value from packet->picking before it builds the graph;
           subsystem initialization alone must not schedule picking work. */
        .picking_pending = false_v,
        .target_color_format = rf->window ? VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB
                                          : VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
        .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
        .target_color_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                       .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
        .target_depth_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                       .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
        .target_terminal_state = {.access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
                                  .layout =
                                      VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
        .shadow_map_size =
            rf->shadow_system.initialized
                ? vkr_shadow_config_get_max_map_size(&rf->shadow_system.config)
                : 2048,
        .shadow_cascade_count = rf->shadow_system.initialized
                                    ? rf->shadow_system.config.cascade_count
                                    : 0,
    };
    if (!vkr_metal_packet_renderer_prepare_frame(rf->metal_renderer, &frame)) {
      rf->frame_active = false_v;
      return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
    }
    VkrMetalPacketResult *timing_result =
        (VkrMetalPacketResult *)rf->metal_timing_result;
    rf->metal_completed_timing_ready = false_v;
    if (timing_result &&
        vkr_metal_packet_renderer_pass_timings_poll_latest(
            rf->metal_renderer, rf->metal_last_completed_timing_submit_value,
            timing_result)) {
      rf->metal_last_completed_timing_submit_value =
          timing_result->submit_value;
      rf->metal_timing_source_cpu_frame_index =
          timing_result->source_frame_index;
      rf->metal_completed_timing_ready = true_v;
    }
    rf->frame_number++;
    MemZero(&rf->frame_metrics, sizeof(rf->frame_metrics));
    *out_setup = (VkrFrameSetup){
        .image_index = 0,
        .window_width = rf->last_window_width,
        .window_height = rf->last_window_height,
        .swapchain_format = frame.target_color_format,
        .swapchain_depth_format = frame.target_depth_format,
    };
    return VKR_RENDERER_ERROR_NONE;
#else
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
  }

  VkrRendererError result = vkr_renderer_begin_frame(renderer, 0.0);
  if (result != VKR_RENDERER_ERROR_NONE) {
    return result;
  }
  rf->frame_number++;

  out_setup->image_index = vkr_renderer_present_target_image_index(renderer);
  out_setup->window_width = rf->last_window_width;
  out_setup->window_height = rf->last_window_height;
  out_setup->swapchain_format = vkr_renderer_present_target_format(
      renderer, VKR_PRESENT_TARGET_ATTACHMENT_COLOR);
  out_setup->swapchain_depth_format = vkr_renderer_present_target_format(
      renderer, VKR_PRESENT_TARGET_ATTACHMENT_DEPTH);

  if (out_setup->window_width == 0 || out_setup->window_height == 0) {
    vkr_renderer_present_target_extent(renderer, &out_setup->window_width,
                                       &out_setup->window_height);
  }

  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkrRendererError vkr_renderer_validate_text_draws(
    const VkrPreparedTextDraw *draws, uint32_t draw_count, const char *path,
    VkrValidationError *out_validation_error) {
  if (draw_count > 0 && !draws) {
    return vkr_renderer_validation_fail(out_validation_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        path, "text draw list is NULL");
  }
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    if (!draw->vertices || draw->vertex_count == 0 || !draw->indices ||
        draw->index_count == 0 || draw->max_index >= draw->vertex_count ||
        draw->atlas.id == 0 || draw->atlas.generation == VKR_INVALID_ID ||
        !isfinite(draw->screen_px_range) || draw->screen_px_range < 0.0f ||
        draw->font_mode > 1u) {
      return vkr_renderer_validation_failf(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "prepared text draw is malformed", "%s[%u]", path, i);
    }
    for (uint32_t element = 0; element < ArrayCount(draw->model.elements);
         ++element) {
      if (!isfinite(draw->model.elements[element])) {
        return vkr_renderer_validation_failf(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "text model contains a non-finite value", "%s[%u].model", path, i);
      }
    }
  }
  return VKR_RENDERER_ERROR_NONE;
}

/**
 * @brief Validates a render packet without touching any renderer state.
 *
 * Pure: it reads the packet and the retained systems it must check against, and
 * mutates nothing. That is what lets the caller reject a packet and cancel the
 * frame cleanly -- if validation had side effects, a rejected packet would
 * leave the renderer half-updated.
 *
 * @return VKR_RENDERER_ERROR_NONE when the packet may be submitted.
 */
vkr_internal VkrRendererError vkr_renderer_validate_packet(
    RendererFrontend *rf, const VkrRenderPacket *packet,
    VkrValidationError *out_validation_error) {
  if (!packet) {
    return vkr_renderer_validation_fail(out_validation_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        "packet", "packet is NULL");
  }

  if (packet->packet_version != VKR_RENDER_PACKET_VERSION) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "packet_version", "unsupported packet version");
  }

  if (packet->frame.window_width == 0 || packet->frame.window_height == 0) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "frame.window_width", "frame dimensions must be non-zero");
  }

  if (!isfinite(packet->globals.exposure) || packet->globals.exposure < 0.0f) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "globals.exposure", "exposure must be finite and non-negative");
  }

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting && (!isfinite(lighting->directional_direction.x) ||
                   !isfinite(lighting->directional_direction.y) ||
                   !isfinite(lighting->directional_direction.z) ||
                   !isfinite(lighting->directional_color.x) ||
                   !isfinite(lighting->directional_color.y) ||
                   !isfinite(lighting->directional_color.z) ||
                   !isfinite(lighting->directional_intensity) ||
                   !isfinite(lighting->ibl_intensity) ||
                   !isfinite(lighting->ibl_diffuse_intensity) ||
                   !isfinite(lighting->ibl_specular_intensity) ||
                   lighting->directional_color.x < 0.0f ||
                   lighting->directional_color.y < 0.0f ||
                   lighting->directional_color.z < 0.0f ||
                   lighting->directional_intensity < 0.0f ||
                   lighting->ibl_intensity < 0.0f ||
                   lighting->ibl_diffuse_intensity < 0.0f ||
                   lighting->ibl_specular_intensity < 0.0f)) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER, "lighting",
        "lighting values must be finite and non-negative");
  }
  if (lighting &&
      (lighting->point_light_count > VKR_MAX_SCENE_POINT_LIGHTS ||
       (lighting->point_light_count > 0 &&
        (!lighting->point_lights || !lighting->point_light_grid)))) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "lighting.point_lights", "point-light table or grid is invalid");
  }
  if (lighting && lighting->ibl_source.id != 0) {
    VkrTexture *ibl_source = vkr_texture_system_get_by_handle(
        &rf->texture_system, lighting->ibl_source);
    if (lighting->ibl_source.generation == VKR_INVALID_ID || !ibl_source ||
        !ibl_source->handle ||
        ibl_source->description.type != VKR_TEXTURE_TYPE_CUBE_MAP) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_HANDLE,
          "lighting.ibl_source", "IBL source must resolve to a cubemap");
    }
  }
  if (lighting && lighting->point_light_count > 0) {
    const VkrPointLightGrid *grid = lighting->point_light_grid;
    const uint64_t grid_cells = (uint64_t)grid->dimensions[0] *
                                grid->dimensions[1] * grid->dimensions[2];
    if (!isfinite(grid->origin.x) || !isfinite(grid->origin.y) ||
        !isfinite(grid->origin.z) || !isfinite(grid->cell_size) ||
        grid->cell_size < 0.0f ||
        grid->cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS ||
        grid_cells != grid->cell_count) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "lighting.point_light_grid", "point-light grid is malformed");
    }
    for (uint32_t i = 0; i < lighting->point_light_count; ++i) {
      const VkrPointLight *light = &lighting->point_lights[i];
      const float32_t values[] = {
          light->position.x,       light->position.y,  light->position.z,
          light->color.x,          light->color.y,     light->color.z,
          light->intensity,        light->constant,    light->linear,
          light->quadratic,        light->range,       light->direction.x,
          light->direction.y,      light->direction.z, light->inner_cone_angle,
          light->outer_cone_angle,
      };
      for (uint32_t value = 0; value < ArrayCount(values); ++value) {
        if (!isfinite(values[value])) {
          return vkr_renderer_validation_fail(
              out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
              "lighting.point_lights", "point-light values must be finite");
        }
      }
      if (light->color.x < 0.0f || light->color.y < 0.0f ||
          light->color.z < 0.0f || light->intensity < 0.0f ||
          light->range < 0.0f || light->kind > VKR_POINT_LIGHT_KIND_GLTF_SPOT) {
        return vkr_renderer_validation_fail(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "lighting.point_lights", "point-light domain is invalid");
      }
    }
  }
  if (lighting && (lighting->ibl_probe_count > VKR_FRAME_IBL_PROBE_MAX ||
                   (lighting->ibl_probe_count > 0 && !lighting->ibl_probes))) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "lighting.ibl_probes", "IBL probe table is invalid");
  }
  if (lighting) {
    for (uint32_t i = 0; i < lighting->ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
      const float32_t values[] = {
          probe->center.x,           probe->center.y,
          probe->center.z,           probe->extents.x,
          probe->extents.y,          probe->extents.z,
          probe->blend_distance,     probe->weight,
          probe->intensity,          probe->diffuse_intensity,
          probe->specular_intensity,
      };
      for (uint32_t value = 0; value < ArrayCount(values); ++value) {
        if (!isfinite(values[value])) {
          return vkr_renderer_validation_fail(
              out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
              "lighting.ibl_probes", "IBL probe values must be finite");
        }
      }
      if (probe->irradiance.id == 0 || probe->prefilter.id == 0 ||
          probe->irradiance.generation == VKR_INVALID_ID ||
          probe->prefilter.generation == VKR_INVALID_ID ||
          probe->extents.x < 0.0f || probe->extents.y < 0.0f ||
          probe->extents.z < 0.0f || probe->blend_distance < 0.0f ||
          probe->weight < 0.0f || probe->intensity < 0.0f ||
          probe->diffuse_intensity < 0.0f || probe->specular_intensity < 0.0f) {
        return vkr_renderer_validation_fail(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "lighting.ibl_probes", "IBL probe domain is invalid");
      }
    }
  }

  const VkrWorldPassPayload *world = packet->world;
  if (world) {
    if (world->instance_count > 0 && !world->instances) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "world.instances", "instances pointer is NULL");
    }
    VkrRendererError err = vkr_renderer_validate_draws(
        rf, world->opaque_draws, world->opaque_draw_count,
        world->instance_count, "world.opaque_draws", VKR_PIPELINE_DOMAIN_WORLD,
        out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
    err = vkr_renderer_validate_text_draws(
        world->text_draws, world->text_draw_count, "world.text_draws",
        out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
    err = vkr_renderer_validate_draws(
        rf, world->transmission_draws, world->transmission_draw_count,
        world->instance_count, "world.transmission_draws",
        VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
    err = vkr_renderer_validate_draws(
        rf, world->transparent_draws, world->transparent_draw_count,
        world->instance_count, "world.transparent_draws",
        VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
  }

  const VkrShadowPassPayload *shadow = packet->shadow;
  if (shadow) {
    if (shadow->cascade_count == 0 ||
        shadow->cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "shadow.cascade_count", "cascade_count is out of range");
    }
    if (rf->shadow_system.initialized &&
        shadow->cascade_count > rf->shadow_system.config.cascade_count) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "shadow.cascade_count", "cascade_count exceeds shadow system config");
    }
    if (shadow->instance_count > 0 && !shadow->instances) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "shadow.instances", "instances pointer is NULL");
    }
    VkrRendererError err = vkr_renderer_validate_draws(
        rf, shadow->opaque_draws, shadow->opaque_draw_count,
        shadow->instance_count, "shadow.opaque_draws",
        VKR_PIPELINE_DOMAIN_SHADOW, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
    err = vkr_renderer_validate_draws(
        rf, shadow->alpha_draws, shadow->alpha_draw_count,
        shadow->instance_count, "shadow.alpha_draws",
        VKR_PIPELINE_DOMAIN_SHADOW, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
  }

  const VkrUiPassPayload *ui = packet->ui;
  if (ui) {
    if (ui->instance_count > 0 && !ui->instances) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "ui.instances", "instances pointer is NULL");
    }
    VkrRendererError err = vkr_renderer_validate_draws(
        rf, ui->draws, ui->draw_count, ui->instance_count, "ui.draws",
        VKR_PIPELINE_DOMAIN_UI, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
    err =
        vkr_renderer_validate_text_draws(ui->text_draws, ui->text_draw_count,
                                         "ui.text_draws", out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
  }

  const VkrEditorPassPayload *editor = packet->editor;
  if (editor) {
    if (editor->instance_count > 0 && !editor->instances) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "editor.instances", "instances pointer is NULL");
    }
    VkrRendererError err = vkr_renderer_validate_draws(
        rf, editor->draws, editor->draw_count, editor->instance_count,
        "editor.draws", VKR_PIPELINE_DOMAIN_UI, out_validation_error);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
  }

  const VkrPickingPassPayload *picking = packet->picking;
  if (picking && picking->pending) {
    if (!rf->picking.initialized) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "picking.pending", "picking system is not initialized");
    }
    if (picking->x >= rf->picking.width || picking->y >= rf->picking.height) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "picking.x", "picking coordinates out of bounds");
    }
    if (picking->draws == NULL) {
      if (!world) {
        return vkr_renderer_validation_fail(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "picking.draws", "world payload required when draws are NULL");
      }
      if (picking->instances && picking->instances != world->instances) {
        return vkr_renderer_validation_fail(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "picking.instances",
            "instances must be NULL or match world instances");
      }
    } else {
      if (picking->instance_count > 0 && !picking->instances) {
        return vkr_renderer_validation_fail(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "picking.instances", "instances pointer is NULL");
      }
      VkrRendererError err = vkr_renderer_validate_draws(
          rf, picking->draws, picking->draw_count, picking->instance_count,
          "picking.draws", VKR_PIPELINE_DOMAIN_PICKING, out_validation_error);
      if (err != VKR_RENDERER_ERROR_NONE) {
        return err;
      }
    }
  }

  const VkrTextUpdatesPayload *text_updates = packet->text_updates;
  if (text_updates) {
    if (text_updates->world_text_update_count > 0 &&
        !text_updates->world_text_updates) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "text_updates.world_text_updates", "update list is NULL");
    }
    if (text_updates->ui_text_update_count > 0 &&
        !text_updates->ui_text_updates) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
          "text_updates.ui_text_updates", "update list is NULL");
    }
    for (uint32_t i = 0; i < text_updates->world_text_update_count; ++i) {
      const VkrTextUpdate *update = &text_updates->world_text_updates[i];
      if (update->text_id == VKR_INVALID_ID) {
        return vkr_renderer_validation_failf(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "text_id is invalid", "text_updates.world[%u].text_id", i);
      }
      if (update->content.length > 0 && !update->content.str) {
        return vkr_renderer_validation_failf(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "content is NULL", "text_updates.world[%u].content", i);
      }
    }
    for (uint32_t i = 0; i < text_updates->ui_text_update_count; ++i) {
      const VkrTextUpdate *update = &text_updates->ui_text_updates[i];
      if (update->text_id == VKR_INVALID_ID) {
        return vkr_renderer_validation_failf(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "text_id is invalid", "text_updates.ui[%u].text_id", i);
      }
      if (update->content.length > 0 && !update->content.str) {
        return vkr_renderer_validation_failf(
            out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
            "content is NULL", "text_updates.ui[%u].content", i);
      }
    }
  }

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
vkr_renderer_submit_packet(VkrRendererFrontendHandle renderer,
                           const VkrRenderPacket *packet,
                           VkrRendererFrameMetrics *out_metrics,
                           VkrValidationError *out_validation_error) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->frame_active) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_FRAME_IN_PROGRESS, "frame",
        "frame is not active; call vkr_renderer_prepare_frame first");
  }

  float64_t safe_dt = 1.0 / 60.0;
  if (packet && packet->frame.delta_time > 0.0) {
    safe_dt = packet->frame.delta_time;
  }
  bool8_t cancel_new_picking_work = false_v;

  VkrRendererError err =
      vkr_renderer_validate_packet(rf, packet, out_validation_error);
  if (err != VKR_RENDERER_ERROR_NONE) {
    // Nothing has been mutated and no pass has touched the acquired image.
    goto cancel;
  }

  if (rf->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    const VkrTextUpdatesPayload *updates = packet->text_updates;
    if (updates) {
      for (uint32_t i = 0; i < updates->world_text_update_count; ++i) {
        const VkrTextUpdate *update = &updates->world_text_updates[i];
        if (rf->world_resources.initialized) {
          vkr_world_resources_text_update(rf, &rf->world_resources,
                                          update->text_id, update->content);
          if (update->transform) {
            vkr_world_resources_text_set_transform(
                rf, &rf->world_resources, update->text_id, update->transform);
          }
        }
      }
      for (uint32_t i = 0; i < updates->ui_text_update_count; ++i) {
        const VkrTextUpdate *update = &updates->ui_text_updates[i];
        if (rf->ui_system.initialized) {
          vkr_ui_system_text_update(rf, &rf->ui_system, update->text_id,
                                    update->content);
        }
      }
    }

    VkrPreparedTextDraw world_text_draws[VKR_PREPARED_TEXT_DRAW_MAX] = {0};
    VkrPreparedTextDraw ui_text_draws[VKR_PREPARED_TEXT_DRAW_MAX] = {0};
    VkrWorldPassPayload world =
        packet->world ? *packet->world : (VkrWorldPassPayload){0};
    VkrUiPassPayload ui = packet->ui ? *packet->ui : (VkrUiPassPayload){0};
    VkrRenderPacket prepared_packet = *packet;
    if (rf->world_resources.initialized) {
      world.text_draw_count = vkr_world_resources_prepare_text_draws(
          rf, &rf->world_resources, world_text_draws,
          VKR_PREPARED_TEXT_DRAW_MAX);
      world.text_draws =
          world.text_draw_count > 0
              ? world_text_draws
              : (packet->world ? packet->world->text_draws : NULL);
      if (world.text_draw_count == 0 && packet->world) {
        world.text_draw_count = packet->world->text_draw_count;
      }
      prepared_packet.world = &world;
    }
    if (rf->ui_system.initialized) {
      ui.text_draw_count = vkr_ui_system_prepare_text_draws(
          rf, &rf->ui_system, ui_text_draws, VKR_PREPARED_TEXT_DRAW_MAX);
      ui.text_draws = ui.text_draw_count > 0
                          ? ui_text_draws
                          : (packet->ui ? packet->ui->text_draws : NULL);
      if (ui.text_draw_count == 0 && packet->ui) {
        ui.text_draw_count = packet->ui->text_draw_count;
      }
      prepared_packet.ui = &ui;
    }

    VkrMetalPacketResult result = {0};
    const bool8_t submitted = vkr_metal_packet_renderer_submit_packet(
        rf->metal_renderer, &prepared_packet, &result);
    rf->frame_active = false_v;
    if (!submitted) {
      return vkr_renderer_validation_fail(
          out_validation_error, VKR_RENDERER_ERROR_SUBMISSION_FAILED, "metal",
          "Metal packet submission failed");
    }
    if (rf->metal_timing_result && !rf->metal_completed_timing_ready) {
      *(VkrMetalPacketResult *)rf->metal_timing_result = result;
      rf->metal_timing_source_cpu_frame_index = packet->frame.frame_index;
    }
    rf->frame_metrics.world.draws_collected = result.indexed_draw_count;
    rf->frame_metrics.world.opaque_draws = result.opaque_draw_count;
    rf->frame_metrics.world.transmission_draws = result.transmission_draw_count;
    rf->frame_metrics.world.transparent_draws = result.blend_draw_count;
    rf->frame_metrics.world.draws_issued = result.indexed_draw_count;
    rf->frame_metrics.world.draw_calls_issued = result.indexed_draw_count;
    rf->frame_metrics.shadow.shadow_draw_calls_opaque[0] =
        result.shadow_draw_count;
    if (out_metrics) {
      *out_metrics = rf->frame_metrics;
    }
    return VKR_RENDERER_ERROR_NONE;
#else
    rf->frame_active = false_v;
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
  }

  VkrShadowConfig shadow_cfg_fallback = VKR_SHADOW_CONFIG_DEFAULT;
  const VkrShadowConfig *shadow_cfg = rf->shadow_system.initialized
                                          ? &rf->shadow_system.config
                                          : &shadow_cfg_fallback;
  uint32_t cascade_count = shadow_cfg->cascade_count;
  if (cascade_count == 0) {
    cascade_count = 1;
  }
  if (cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
    cascade_count = VKR_SHADOW_CASCADE_COUNT_MAX;
  }
  err = vkr_capture_frame_reserve(
      rf, packet, vkr_shadow_config_get_max_map_size(shadow_cfg), cascade_count,
      out_validation_error);
  if (err != VKR_RENDERER_ERROR_NONE) {
    goto cancel;
  }

  // ---- Retained-state mutation begins here. ----
  // Failures past this point still cancel the frame, but do not roll back what
  // has already been applied: rolling back would need a shadow copy of every
  // retained system. The next accepted packet renders the applied state.
  const VkrTextUpdatesPayload *text_updates = packet->text_updates;
  if (text_updates) {
    for (uint32_t i = 0; i < text_updates->world_text_update_count; ++i) {
      const VkrTextUpdate *update = &text_updates->world_text_updates[i];
      if (rf->world_resources.initialized) {
        vkr_world_resources_text_update(rf, &rf->world_resources,
                                        update->text_id, update->content);
        if (update->transform) {
          vkr_world_resources_text_set_transform(
              rf, &rf->world_resources, update->text_id, update->transform);
        }
      }
    }
    for (uint32_t i = 0; i < text_updates->ui_text_update_count; ++i) {
      const VkrTextUpdate *update = &text_updates->ui_text_updates[i];
      if (rf->ui_system.initialized) {
        vkr_ui_system_text_update(rf, &rf->ui_system, update->text_id,
                                  update->content);
      }
    }
  }

  uint32_t viewport_width = packet->frame.viewport_width;
  uint32_t viewport_height = packet->frame.viewport_height;
  if (viewport_width == 0 || viewport_height == 0) {
    viewport_width = packet->frame.window_width;
    viewport_height = packet->frame.window_height;
  }

  uint32_t ui_width = packet->frame.window_width;
  uint32_t ui_height = packet->frame.window_height;
  if (ui_width == 0 || ui_height == 0) {
    ui_width = viewport_width;
    ui_height = viewport_height;
  }

  rf->globals = (VkrGlobalMaterialState){
      .projection = packet->globals.projection,
      .view = packet->globals.view,
      .ui_projection = mat4_ortho(0.0f, (float32_t)ui_width,
                                  (float32_t)ui_height, 0.0f, -1.0f, 1.0f),
      .ui_view = mat4_identity(),
      .ambient_color = packet->globals.ambient_color,
      .view_position = packet->globals.view_position,
      .exposure = packet->globals.exposure,
      .render_mode = (VkrRenderMode)packet->globals.render_mode,
  };

  if (rf->ui_system.initialized) {
    vkr_ui_system_set_offscreen_size(rf, &rf->ui_system,
                                     packet->frame.editor_enabled,
                                     viewport_width, viewport_height);
    vkr_ui_system_resize(rf, &rf->ui_system, ui_width, ui_height);
  }

  if (!rf->render_graph_enabled || !rf->render_graph ||
      !rf->render_graph_loaded) {
    err = vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "render_graph", "render graph is not available");
    goto cancel;
  }

  uint32_t target_width = 0;
  uint32_t target_height = 0;
  vkr_renderer_present_target_extent(renderer, &target_width, &target_height);
  const uint32_t target_image_index =
      vkr_renderer_present_target_image_index(renderer);
  VkrRenderGraphFrameInfo frame = {
      .frame_index = packet->frame.frame_index,
      .image_index = target_image_index,
      .delta_time = packet->frame.delta_time,
      .target_width = target_width,
      .target_height = target_height,
      .window_width = packet->frame.window_width,
      .window_height = packet->frame.window_height,
      .viewport_width = viewport_width,
      .viewport_height = viewport_height,
      .editor_enabled = packet->frame.editor_enabled,
      .picking_pending =
          (packet->picking && packet->picking->pending) ? true_v : false_v,
      .target_color_format = vkr_renderer_present_target_format(
          renderer, VKR_PRESENT_TARGET_ATTACHMENT_COLOR),
      .target_depth_format = vkr_renderer_present_target_format(
          renderer, VKR_PRESENT_TARGET_ATTACHMENT_DEPTH),
      .target_color_initial_state = vkr_renderer_present_target_image_state(
          renderer, VKR_PRESENT_TARGET_ATTACHMENT_COLOR, target_image_index),
      .target_depth_initial_state = vkr_renderer_present_target_image_state(
          renderer, VKR_PRESENT_TARGET_ATTACHMENT_DEPTH, target_image_index),
      .target_terminal_state =
          vkr_renderer_present_target_terminal_state(renderer),
      .shadow_depth_format = vkr_renderer_get_shadow_depth_format(renderer),
      .shadow_map_size = vkr_shadow_config_get_max_map_size(shadow_cfg),
      .shadow_cascade_count = cascade_count,
  };

  vkr_rg_begin_frame(rf->render_graph, &frame);
  if (!vkr_rg_build_from_json(rf->render_graph, &rf->render_graph_json, &frame,
                              &rf->rg_executor_registry)) {
    err = vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED,
        "render_graph", "render graph build failed");
    goto cancel;
  }
  if (!vkr_capture_graph_overlay_build(rf)) {
    err = vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
        "debug.capture", "capture graph source is unavailable");
    goto cancel;
  }

  VkrPreparedTextDraw world_text_draws[VKR_PREPARED_TEXT_DRAW_MAX] = {0};
  VkrPreparedTextDraw ui_text_draws[VKR_PREPARED_TEXT_DRAW_MAX] = {0};
  VkrWorldPassPayload prepared_world =
      packet->world ? *packet->world : (VkrWorldPassPayload){0};
  VkrUiPassPayload prepared_ui =
      packet->ui ? *packet->ui : (VkrUiPassPayload){0};
  VkrRenderPacket prepared_packet = *packet;
  if (rf->world_resources.initialized) {
    prepared_world.text_draw_count = vkr_world_resources_prepare_text_draws(
        rf, &rf->world_resources, world_text_draws, VKR_PREPARED_TEXT_DRAW_MAX);
    prepared_world.text_draws = prepared_world.text_draw_count > 0
                                    ? world_text_draws
                                : packet->world ? packet->world->text_draws
                                                : NULL;
    if (prepared_world.text_draw_count == 0 && packet->world)
      prepared_world.text_draw_count = packet->world->text_draw_count;
    prepared_packet.world = &prepared_world;
  }
  if (rf->ui_system.initialized) {
    prepared_ui.text_draw_count = vkr_ui_system_prepare_text_draws(
        rf, &rf->ui_system, ui_text_draws, VKR_PREPARED_TEXT_DRAW_MAX);
    prepared_ui.text_draws = prepared_ui.text_draw_count > 0 ? ui_text_draws
                             : packet->ui ? packet->ui->text_draws
                                          : NULL;
    if (prepared_ui.text_draw_count == 0 && packet->ui)
      prepared_ui.text_draw_count = packet->ui->text_draw_count;
    prepared_packet.ui = &prepared_ui;
  }

  vkr_rg_set_packet(rf->render_graph, &prepared_packet);
  VkrPickingState picking_state_before = rf->picking.state;
  err = vkr_rg_execute(rf->render_graph, rf);
  if (err != VKR_RENDERER_ERROR_NONE) {
    // A barrier or render pass failed, so the recorded commands are not a
    // coherent frame. Cancelling discards the image instead of presenting work
    // whose layout transitions never happened.
    vkr_renderer_validation_fail(out_validation_error, err, "render_graph",
                                 "render graph execution failed");
    cancel_new_picking_work =
        picking_state_before == VKR_PICKING_STATE_RENDER_PENDING &&
        (rf->picking.state == VKR_PICKING_STATE_RENDER_RECORDED ||
         rf->picking.state == VKR_PICKING_STATE_READBACK_PENDING);
    goto cancel;
  }

  // end_frame runs before the out-parameter copy because the present duration
  // only exists after presentation completes. Anything end_frame writes into
  // frame_metrics is therefore included in what the caller receives, which is
  // the intent: the caller's copy describes the whole submitted frame.
  err = vkr_renderer_end_frame(renderer, safe_dt);
#if VKR_METRICS_ENABLED
  rf->frame_metrics.backend_present_valid =
      vkr_renderer_get_last_present_duration(
          renderer, &rf->frame_metrics.backend_present_ns);
#endif
  if (out_metrics) {
    *out_metrics = rf->frame_metrics;
  }
  if (err != VKR_RENDERER_ERROR_NONE &&
      err != VKR_RENDERER_ERROR_PRESENTATION_FAILED &&
      picking_state_before == VKR_PICKING_STATE_RENDER_PENDING &&
      (rf->picking.state == VKR_PICKING_STATE_RENDER_RECORDED ||
       rf->picking.state == VKR_PICKING_STATE_READBACK_PENDING)) {
    // No queue submission owns the newly-recorded readback. The backend has
    // returned its ring slot to IDLE; mirror that rollback in the picking state
    // so a failed submit cannot leave picking pending forever.
    vkr_picking_cancel(&rf->picking);
  }
  vkr_capture_frame_clear(rf);
  return err;

cancel:
  if (out_metrics) {
    *out_metrics = rf->frame_metrics;
  }
  if (cancel_new_picking_work) {
    vkr_picking_cancel(&rf->picking);
  }
  VkrRendererError cancel_err = vkr_renderer_cancel_frame(renderer);
  vkr_capture_frame_clear(rf);
  if (cancel_err != VKR_RENDERER_ERROR_NONE) {
    String8 cancel_error_string = vkr_renderer_get_error_string(cancel_err);
    log_error("Failed to cancel renderer frame: %s",
              string8_cstr(&cancel_error_string));
    // The backend lifecycle error is now the actionable failure. Structured
    // packet/graph detail remains available in out_validation_error.
    return cancel_err;
  }
  return err;
}

VkrRendererError vkr_renderer_begin_frame(VkrRendererFrontendHandle renderer,
                                          float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }

  uint64_t packed = vkr_atomic_uint64_exchange(
      &renderer->pending_resize_mailbox, 0, VKR_MEMORY_ORDER_ACQ_REL);
  if (packed != 0) {
    uint32_t width = (uint32_t)(packed >> 32);
    uint32_t height = (uint32_t)(packed & 0xFFFFFFFFu);
    if (width > 0 && height > 0) {
      vkr_renderer_resize(renderer, width, height);
    }
  }
  if (renderer->window) {
    VkrWindowPixelSize pixel_size = vkr_window_get_pixel_size(renderer->window);
    if (pixel_size.width > 0 && pixel_size.height > 0 &&
        (pixel_size.width != renderer->last_window_width ||
         pixel_size.height != renderer->last_window_height)) {
      vkr_renderer_resize(renderer, pixel_size.width, pixel_size.height);
    }
  }

  VkrRendererError result =
      renderer->backend.begin_frame(renderer->backend_state, delta_time);
  if (result == VKR_RENDERER_ERROR_NONE) {
    renderer->frame_active = true;
    // Pump async resource finalization only after frame activation so GPU-bound
    // requests can stamp submit_serial + 1 for this frame's in-flight submit.
    vkr_resource_system_pump(NULL);
    vkr_mesh_manager_pump_async(&renderer->mesh_manager);
    MemZero(&renderer->frame_metrics, sizeof(renderer->frame_metrics));
    // Per-frame streams are indexed by the frame-in-flight slot, never by the
    // swapchain image index: begin_frame waited on this slot's fence, so its
    // previous contents are guaranteed to have no GPU readers left. A swapchain
    // with more images than in-flight slots would alias two live images onto
    // one buffer.
    uint32_t frame_slot = vkr_renderer_frame_in_flight_index(renderer);
    if (renderer->instance_buffer_pool.initialized) {
      vkr_instance_buffer_begin_frame(&renderer->instance_buffer_pool,
                                      frame_slot);
    }
    if (renderer->indirect_draw_system.initialized &&
        renderer->indirect_draw_system.enabled) {
      vkr_indirect_draw_begin_frame(&renderer->indirect_draw_system,
                                    frame_slot);
    }
  }

  return result;
}

void vkr_renderer_resize(VkrRendererFrontendHandle renderer, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Resizing renderer to %d %d", width, height);

  RendererFrontend *rf = (RendererFrontend *)renderer;

  if (rf->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL &&
      rf->backend.on_resize) {
    rf->backend.on_resize(rf->backend_state, width, height);
  }

  if (rf->rf_mutex) {
    vkr_mutex_lock(rf->rf_mutex);
  }
  if (rf->window) {
    rf->window->width = width;
    rf->window->height = height;
  }
  rf->last_window_width = width;
  rf->last_window_height = height;
  if (rf->rf_mutex) {
    if (!vkr_mutex_unlock(rf->rf_mutex)) {
      log_error("Failed to unlock renderer mutex");
    }
  }

  if (rf->ui_system.initialized) {
    vkr_ui_system_resize(rf, &rf->ui_system, width, height);
  }
  if (rf->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    renderer_frontend_regenerate_render_targets(rf);
    vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);
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

void vkr_renderer_set_viewport(VkrRendererFrontendHandle renderer,
                               const VkrViewport *viewport) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(viewport != NULL, "Viewport is NULL");
  assert_log(renderer->frame_active, "Set viewport called outside of frame");

  renderer->backend.set_viewport(renderer->backend_state, viewport);
}

void vkr_renderer_set_scissor(VkrRendererFrontendHandle renderer,
                              const VkrScissor *scissor) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(scissor != NULL, "Scissor is NULL");
  assert_log(renderer->frame_active, "Set scissor called outside of frame");

  renderer->backend.set_scissor(renderer->backend_state, scissor);
}

void vkr_renderer_set_depth_bias(VkrRendererFrontendHandle renderer,
                                 float32_t constant_factor, float32_t clamp,
                                 float32_t slope_factor) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "Set depth bias called outside of frame");

  renderer->backend.set_depth_bias(renderer->backend_state, constant_factor,
                                   clamp, slope_factor);
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

void vkr_renderer_draw_indexed_indirect(VkrRendererFrontendHandle renderer,
                                        VkrBufferHandle indirect_buffer,
                                        uint64_t offset, uint32_t draw_count,
                                        uint32_t stride) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active,
             "Draw indexed indirect called outside of frame");
  assert_log(indirect_buffer != NULL, "Indirect buffer is NULL");

  if (!renderer->backend.draw_indexed_indirect) {
    return;
  }

  renderer->backend.draw_indexed_indirect(
      renderer->backend_state,
      (VkrBackendResourceHandle){.ptr = (void *)indirect_buffer}, offset,
      draw_count, stride);
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

/**
 * @brief Abandons the active frame without rendering it.
 *
 * Used when a packet is rejected or graph execution fails after the swapchain
 * image has already been acquired. The backend still submits and presents so
 * the acquire semaphore is consumed and the image is returned to the
 * presentation engine; see the cancel_frame backend entry for why discarding is
 * not an option. Always clears frame_active, so a caller can never be left
 * holding a frame it cannot end.
 */
VkrRendererError vkr_renderer_cancel_frame(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  if (!renderer->frame_active) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    renderer->frame_active = false_v;
    return VKR_RENDERER_ERROR_NONE;
  }

  VkrRendererError result = VKR_RENDERER_ERROR_NONE;
  if (renderer->backend.cancel_frame) {
    result = renderer->backend.cancel_frame(renderer->backend_state);
  } else {
    // A backend without a cancel entry must still terminate the frame, or the
    // next prepare_frame would fail with FRAME_IN_PROGRESS forever.
    result = renderer->backend.end_frame(renderer->backend_state, 1.0 / 60.0);
  }
  renderer->frame_active = false;

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

bool8_t vkr_renderer_rg_timing_begin_frame(VkrRendererFrontendHandle renderer,
                                           uint32_t pass_count,
                                           uint64_t source_frame_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.rg_timing_begin_frame) {
    return false_v;
  }
  return renderer->backend.rg_timing_begin_frame(
      renderer->backend_state, pass_count, source_frame_index);
}

void vkr_renderer_rg_timing_begin_pass(VkrRendererFrontendHandle renderer,
                                       uint32_t pass_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.rg_timing_begin_pass) {
    return;
  }
  renderer->backend.rg_timing_begin_pass(renderer->backend_state, pass_index);
}

void vkr_renderer_rg_timing_end_pass(VkrRendererFrontendHandle renderer,
                                     uint32_t pass_index) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.rg_timing_end_pass) {
    return;
  }
  renderer->backend.rg_timing_end_pass(renderer->backend_state, pass_index);
}

bool8_t vkr_renderer_rg_timing_get_results(VkrRendererFrontendHandle renderer,
                                           uint32_t *out_pass_count,
                                           const float64_t **out_pass_ms,
                                           const bool8_t **out_pass_valid,
                                           uint64_t *out_source_frame_index,
                                           uint64_t *out_source_submit_serial) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (!renderer->backend.rg_timing_get_results) {
    if (out_pass_count) {
      *out_pass_count = 0;
    }
    if (out_pass_ms) {
      *out_pass_ms = NULL;
    }
    if (out_pass_valid) {
      *out_pass_valid = NULL;
    }
    if (out_source_frame_index) {
      *out_source_frame_index = 0;
    }
    if (out_source_submit_serial) {
      *out_source_submit_serial = 0;
    }
    return false_v;
  }
  return renderer->backend.rg_timing_get_results(
      renderer->backend_state, out_pass_count, out_pass_ms, out_pass_valid,
      out_source_frame_index, out_source_submit_serial);
}

vkr_internal bool8_t renderer_frontend_initialize_metal_systems(
    RendererFrontend *rf, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers) {
  if (!vkr_resource_system_init(&rf->allocator, rf, job_system,
                                metrics_producers)) {
    return false_v;
  }

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrGeometrySystemConfig geometry_config = {
      .max_geometries = 16384,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_geometry_system_init(&rf->geometry_system, rf, &geometry_config,
                                &error)) {
    return false_v;
  }
  VkrTextureSystemConfig texture_config = {
      .max_texture_count = 16384,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_texture_system_init(rf, &texture_config, job_system,
                               &rf->texture_system)) {
    return false_v;
  }
  rf->texture_system.hdr_decode_metrics = rf->hdr_decode_metrics;
  VkrMaterialSystemConfig material_config = {
      .max_material_count = 8192,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_material_system_init(&rf->material_system, rf->arena,
                                &rf->texture_system, NULL, &material_config)) {
    return false_v;
  }
  VkrMeshManagerConfig mesh_config = {.max_mesh_count = 16384};
  if (!vkr_mesh_manager_init(&rf->mesh_manager, &rf->geometry_system,
                             &rf->material_system, &rf->pipeline_registry,
                             &mesh_config)) {
    return false_v;
  }

  const uint32_t pool_chunk_count =
      job_system ? job_system->worker_count + 4 : 8;
  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mesh_arena_pool)) {
    return false_v;
  }
  rf->mesh_loader =
      (VkrMeshLoaderContext){.geometry_system = &rf->geometry_system,
                             .material_system = &rf->material_system,
                             .mesh_manager = &rf->mesh_manager,
                             .job_system = job_system,
                             .arena_pool = &rf->mesh_arena_pool};
  rf->mesh_loader.allocator.ctx = rf->arena;
  vkr_allocator_arena(&rf->mesh_loader.allocator);
  if (!vkr_dmemory_create(VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->mesh_loader.async_memory)) {
    return false_v;
  }
  rf->mesh_loader.async_allocator =
      (VkrAllocator){.ctx = &rf->mesh_loader.async_memory};
  vkr_dmemory_allocator_create(&rf->mesh_loader.async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->mesh_loader.async_mutex)) {
    return false_v;
  }
  if (!vkr_dmemory_create(VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->scene_async_memory)) {
    return false_v;
  }
  rf->scene_async_allocator = (VkrAllocator){.ctx = &rf->scene_async_memory};
  vkr_dmemory_allocator_create(&rf->scene_async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->scene_async_mutex)) {
    return false_v;
  }
  rf->mesh_manager.loader_context = &rf->mesh_loader;

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->bitmap_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->system_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mtsdf_font_arena_pool)) {
    return false_v;
  }
  rf->bitmap_font_loader = (VkrBitmapFontLoaderContext){
      .job_system = job_system, .arena_pool = &rf->bitmap_font_arena_pool};
  rf->system_font_loader = (VkrSystemFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->system_font_arena_pool,
      .texture_system = &rf->texture_system,
  };
  rf->mtsdf_font_loader = (VkrMtsdfFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->mtsdf_font_arena_pool,
      .texture_system = &rf->texture_system,
  };

  vkr_resource_system_register_loader((void *)&rf->texture_system,
                                      vkr_texture_loader_create());
  vkr_resource_system_register_loader((void *)&rf->material_system,
                                      vkr_material_loader_create());
  vkr_resource_system_register_loader((void *)&rf->mesh_loader,
                                      vkr_mesh_loader_create(&rf->mesh_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->bitmap_font_loader,
      vkr_bitmap_font_loader_create(&rf->bitmap_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->system_font_loader,
      vkr_system_font_loader_create(&rf->system_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->mtsdf_font_loader,
      vkr_mtsdf_font_loader_create(&rf->mtsdf_font_loader));
  vkr_resource_system_register_loader((void *)rf, vkr_scene_loader_create());

  VkrFontSystemConfig font_config = {
      .max_system_font_count = 16,
      .max_bitmap_font_count = 16,
      .max_mtsdf_font_count = 16,
  };
  if (!vkr_font_system_init(&rf->font_system, rf, &font_config, &error) ||
      !vkr_lighting_system_init(&rf->lighting_system) ||
      !vkr_world_resources_init_retained(rf, &rf->world_resources)) {
    return false_v;
  }
  VkrShadowConfig shadow_config = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&rf->shadow_system, rf, &shadow_config)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_UI) &&
      !vkr_ui_system_init_retained(rf, &rf->ui_system)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_SKYBOX) &&
      !vkr_skybox_system_init(rf, &rf->skybox_system)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_PICKING) &&
      !vkr_picking_init(rf, &rf->picking, rf->last_window_width,
                        rf->last_window_height)) {
    return false_v;
  }
  renderer_frontend_narrow_plan_to_initialized(rf);
  return true_v;
}

bool32_t vkr_renderer_systems_initialize(
    VkrRendererFrontendHandle renderer, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers,
    const VkrSubsystemPlan *subsystem_plan) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!subsystem_plan) {
    log_error("Renderer subsystem plan is required");
    return false_v;
  }

  VkrSubsystemPlan resolved_plan = {0};
  VkrRendererError plan_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_renderer_subsystem_plan_build(
          subsystem_plan->profile, subsystem_plan->requested_mask,
          subsystem_plan->excluded_mask, &resolved_plan, &plan_error) ||
      resolved_plan.effective_mask != subsystem_plan->effective_mask) {
    log_error("Renderer subsystem plan is invalid or not dependency-closed");
    return false_v;
  }
  rf->subsystem_plan = resolved_plan;
  if (metrics_producers) {
    rf->hdr_decode_metrics = metrics_producers->hdr_decode;
    rf->ibl_conversion_metrics = metrics_producers->ibl_conversion;
    rf->ibl_convolution_metrics = metrics_producers->ibl_convolution;
  }
#if VKR_METRICS_ENABLED
  const float64_t systems_start = vkr_platform_get_absolute_time();
#endif

  if (rf->backend.set_job_system) {
    rf->backend.set_job_system(rf->backend_state, job_system);
  }

  VkrCameraSystemConfig camera_cfg = {.max_camera_count = 24};
  if (!vkr_camera_registry_init(&camera_cfg, &rf->camera_system)) {
    log_fatal("Failed to initialize camera system");
    return false_v;
  }
  const float32_t default_vertical_fov_degrees = 70.0f;
  const float32_t default_near_clip = 0.1f;
  const float32_t default_far_clip = 500.0f;
  VkrCameraHandle default_camera = VKR_CAMERA_HANDLE_INVALID;
  if (!vkr_camera_registry_create_perspective(
          &rf->camera_system, string8_lit("camera.default"), rf->window,
          default_vertical_fov_degrees, default_near_clip, default_far_clip,
          &default_camera)) {
    log_fatal("Failed to create default camera");
    return false_v;
  }
  vkr_camera_registry_set_active(&rf->camera_system, default_camera);
  rf->active_camera = default_camera;
  /* Creation seeds the aspect from the window, which an offscreen renderer does
     not have. Restate the lens against the actual target extent so both target
     kinds project identically. */
  VkrCamera *initial_camera =
      vkr_camera_registry_get_by_handle(&rf->camera_system, default_camera);
  if (!initial_camera ||
      !vkr_camera_set_perspective_lens(
          initial_camera, default_vertical_fov_degrees, default_near_clip,
          default_far_clip, rf->last_window_width, rf->last_window_height)) {
    log_fatal("Failed to configure default camera target extent");
    return false_v;
  }
  vkr_camera_system_update(initial_camera);

  if (rf->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    if (!renderer_frontend_initialize_metal_systems(rf, job_system,
                                                    metrics_producers)) {
      log_fatal("Failed to initialize Metal packet renderer systems");
      return false_v;
    }
#if VKR_METRICS_ENABLED
    rf->boot_metrics.systems_ns = vkr_metrics_elapsed_ns(systems_start);
#endif
    return true_v;
  }

  if (!vkr_pipeline_registry_init(&rf->pipeline_registry, rf, NULL)) {
    log_fatal("Failed to initialize pipeline registry");
    return false_v;
  }

  if (!vkr_rg_executor_registry_init(&rf->rg_executor_registry,
                                     &rf->allocator)) {
    log_fatal("Failed to initialize render graph executor registry");
    return false_v;
  }

  if (!vkr_pass_shadow_register(&rf->rg_executor_registry) ||
      !vkr_pass_picking_register(&rf->rg_executor_registry) ||
      !vkr_pass_ibl_bake_register(&rf->rg_executor_registry) ||
      !vkr_pass_skybox_register(&rf->rg_executor_registry) ||
      !vkr_pass_world_register(&rf->rg_executor_registry) ||
      !vkr_pass_copy_register(&rf->rg_executor_registry) ||
      !vkr_pass_tonemap_register(&rf->rg_executor_registry) ||
      !vkr_pass_ui_register(&rf->rg_executor_registry) ||
      !vkr_pass_editor_register(&rf->rg_executor_registry)) {
    log_fatal("Failed to register render graph pass executors");
    return false_v;
  }

#if VKR_METRICS_ENABLED
  const float64_t graph_start = vkr_platform_get_absolute_time();
#endif
  rf->render_graph = vkr_rg_create(&rf->render_graph_allocator);
  if (!rf->render_graph) {
    log_fatal("Failed to create render graph");
    return false_v;
  }

  const char *graph_path = "assets/render_graphs/main.rendergraph.json";
  if (!vkr_rg_json_load_file(&rf->render_graph_allocator, graph_path,
                             &rf->render_graph_json)) {
    log_fatal("Failed to load render graph JSON");
    return false_v;
  }
  rf->render_graph_loaded = true_v;
  rf->render_graph_enabled = true_v;
#if VKR_METRICS_ENABLED
  rf->boot_metrics.graph_ns = vkr_metrics_elapsed_ns(graph_start);
#endif

  // Per-frame streams are indexed directly by the backend's frame-in-flight
  // slot, so their slot counts must cover every slot the backend can produce.
  // Checked once here rather than per frame; a mismatch would alias two
  // in-flight frames onto one buffer.
  uint32_t frame_slot_count = vkr_renderer_frame_in_flight_count(rf);
  if (frame_slot_count > VKR_INSTANCE_BUFFER_FRAMES ||
      frame_slot_count > VKR_INDIRECT_DRAW_FRAMES) {
    log_fatal(
        "Backend reports %u frames in flight; per-frame stream pools hold "
        "%u/%u slots",
        frame_slot_count, (uint32_t)VKR_INSTANCE_BUFFER_FRAMES,
        (uint32_t)VKR_INDIRECT_DRAW_FRAMES);
    return false_v;
  }

  if (!vkr_instance_buffer_pool_init(&rf->instance_buffer_pool, rf,
                                     VKR_INSTANCE_BUFFER_MAX_INSTANCES)) {
    log_fatal("Failed to initialize instance buffer pool");
    return false_v;
  }

  if (!vkr_indirect_draw_init(&rf->indirect_draw_system, rf,
                              VKR_INDIRECT_DRAW_MAX_DRAWS)) {
    log_warn("Indirect draw system unavailable; falling back to direct draws");
  }

  VkrShaderSystemConfig shader_cfg = VKR_SHADER_SYSTEM_CONFIG_DEFAULT;
  if (!vkr_shader_system_initialize(&rf->shader_system, shader_cfg)) {
    log_fatal("Failed to initialize shader system");
    return false_v;
  }
  // todo: shader sys should accepts pipeline registry as a parameter
  vkr_shader_system_set_registry(&rf->shader_system, &rf->pipeline_registry);

  if (!vkr_resource_system_init(&rf->allocator, rf, job_system,
                                metrics_producers)) {
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
  log_debug("Geometry system max geometries=%u", geo_cfg.max_geometries);

  VkrTextureSystemConfig tex_cfg = {.max_texture_count = 1024};
  if (!vkr_texture_system_init(rf, &tex_cfg, job_system, &rf->texture_system)) {
    log_fatal("Failed to initialize texture system");
    return false_v;
  }
  rf->texture_system.hdr_decode_metrics = rf->hdr_decode_metrics;

  // Set default 2D texture in backend for fallback in empty sampler slots
  VkrTexture *default_tex = vkr_texture_system_get_default(&rf->texture_system);
  if (default_tex && rf->backend.set_default_2d_texture) {
    rf->backend.set_default_2d_texture(rf->backend_state, default_tex->handle);
  }

  VkrMaterialSystemConfig mat_cfg = {.max_material_count = 1024};
  if (!vkr_material_system_init(&rf->material_system, rf->arena,
                                &rf->texture_system, &rf->shader_system,
                                &mat_cfg)) {
    log_fatal("Failed to initialize material system");
    return false_v;
  }

  VkrMeshManagerConfig mesh_cfg = {.max_mesh_count = 100000};
  if (!vkr_mesh_manager_init(&rf->mesh_manager, &rf->geometry_system,
                             &rf->material_system, &rf->pipeline_registry,
                             &mesh_cfg)) {
    log_fatal("Failed to initialize mesh manager");
    return false_v;
  }

  // Create arena pool for mesh loading
  // Use worker_count + 4 chunks to allow for some buffer
  uint32_t pool_chunk_count = job_system ? job_system->worker_count + 4
                                         : 8; // Default to 8 if no job system
  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mesh_arena_pool)) {
    log_fatal("Failed to create mesh arena pool");
    return false_v;
  }

  rf->mesh_loader =
      (VkrMeshLoaderContext){.geometry_system = &rf->geometry_system,
                             .material_system = &rf->material_system,
                             .mesh_manager = &rf->mesh_manager,
                             .job_system = job_system,
                             .arena_pool = &rf->mesh_arena_pool};
  rf->mesh_loader.allocator.ctx = rf->arena;
  vkr_allocator_arena(&rf->mesh_loader.allocator);
  if (!vkr_dmemory_create(VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->mesh_loader.async_memory)) {
    log_fatal("Failed to create mesh loader async allocator");
    return false_v;
  }
  rf->mesh_loader.async_allocator =
      (VkrAllocator){.ctx = &rf->mesh_loader.async_memory};
  vkr_dmemory_allocator_create(&rf->mesh_loader.async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->mesh_loader.async_mutex)) {
    log_fatal("Failed to create mesh loader async allocator mutex");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (!vkr_dmemory_create(VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->scene_async_memory)) {
    log_fatal("Failed to create scene loader async allocator");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }
  rf->scene_async_allocator = (VkrAllocator){.ctx = &rf->scene_async_memory};
  vkr_dmemory_allocator_create(&rf->scene_async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->scene_async_mutex)) {
    log_fatal("Failed to create scene loader async allocator mutex");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  // Provide the mesh manager access to the mesh loader context so it can
  // throttle large batch loads to the arena pool capacity.
  rf->mesh_manager.loader_context = &rf->mesh_loader;

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->bitmap_font_arena_pool)) {
    log_fatal("Failed to create bitmap font arena pool");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  rf->bitmap_font_loader = (VkrBitmapFontLoaderContext){
      .job_system = job_system, .arena_pool = &rf->bitmap_font_arena_pool};

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->system_font_arena_pool)) {
    log_fatal("Failed to create system font arena pool");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  rf->system_font_loader = (VkrSystemFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->system_font_arena_pool,
      .texture_system = &rf->texture_system,
  };

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mtsdf_font_arena_pool)) {
    log_fatal("Failed to create mtsdf font arena pool");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  rf->mtsdf_font_loader = (VkrMtsdfFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->mtsdf_font_arena_pool,
      .texture_system = &rf->texture_system,
  };

  vkr_resource_system_register_loader((void *)&rf->texture_system,
                                      vkr_texture_loader_create());
  vkr_resource_system_register_loader((void *)&rf->material_system,
                                      vkr_material_loader_create());
  vkr_resource_system_register_loader((void *)&rf->shader_system,
                                      vkr_shader_loader_create());
  vkr_resource_system_register_loader((void *)&rf->mesh_loader,
                                      vkr_mesh_loader_create(&rf->mesh_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->bitmap_font_loader,
      vkr_bitmap_font_loader_create(&rf->bitmap_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->system_font_loader,
      vkr_system_font_loader_create(&rf->system_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->mtsdf_font_loader,
      vkr_mtsdf_font_loader_create(&rf->mtsdf_font_loader));
  vkr_resource_system_register_loader((void *)rf, vkr_scene_loader_create());

  VkrFontSystemConfig font_cfg = {
      .max_system_font_count = 16,
      .max_bitmap_font_count = 16,
      .max_mtsdf_font_count = 16,
  };

  VkrRendererError font_sys_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_font_system_init(&rf->font_system, rf, &font_cfg, &font_sys_err)) {
    String8 err_str = vkr_renderer_get_error_string(font_sys_err);
    log_error("Failed to initialize font system: %s", string8_cstr(&err_str));
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (!vkr_lighting_system_init(&rf->lighting_system)) {
    log_fatal("Failed to initialize lighting system");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }
  rf->lighting_system.shader_system = &rf->shader_system;

  VkrShadowConfig shadow_cfg = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&rf->shadow_system, rf, &shadow_cfg)) {
    log_error("Failed to initialize shadow system");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (!vkr_world_resources_init(rf, &rf->world_resources)) {
    log_error("Failed to initialize world resources");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_UI) &&
      !vkr_ui_system_init(rf, &rf->ui_system)) {
    log_error("Failed to initialize UI system");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_SKYBOX) &&
      !vkr_skybox_system_init(rf, &rf->skybox_system)) {
    log_error("Failed to initialize skybox system");
    renderer_frontend_destroy_loader_async_allocators(rf);
    return false_v;
  }

  if (!vkr_world_resources_prepare_default_ibl(rf, &rf->world_resources)) {
    log_warn("Default HDR IBL preparation failed; legacy skybox fallback is "
             "still available");
  }

  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_EDITOR) &&
      !vkr_editor_viewport_init(rf, &rf->editor_viewport)) {
    log_warn("Editor viewport resources unavailable (non-fatal)");
  }

  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_GIZMO)) {
    VkrGizmoConfig gizmo_cfg = VKR_GIZMO_CONFIG_DEFAULT;
    if (!vkr_gizmo_system_init(&rf->gizmo_system, rf, &gizmo_cfg)) {
      log_warn("Failed to initialize gizmo system (non-fatal)");
    }
  }

  VkrWindowPixelSize initial_size =
      rf->window
          ? vkr_window_get_pixel_size(rf->window)
          : (VkrWindowPixelSize){rf->last_window_width, rf->last_window_height};

  // Initialize picking system with initial window dimensions
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_PICKING) &&
      initial_size.width > 0 && initial_size.height > 0) {
    if (!vkr_picking_init(rf, &rf->picking, initial_size.width,
                          initial_size.height)) {
      log_warn("Failed to initialize picking system (non-fatal)");
    }
  }

  if (initial_size.width > 0 && initial_size.height > 0) {
    vkr_renderer_resize(rf, initial_size.width, initial_size.height);
  }
  renderer_frontend_narrow_plan_to_initialized(rf);

#if VKR_METRICS_ENABLED
  rf->boot_metrics.systems_ns = vkr_metrics_elapsed_ns(systems_start);
#endif
  return true_v;
}

VkrSubsystemMask
vkr_renderer_get_subsystem_mask(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return ((RendererFrontend *)renderer)->subsystem_plan.effective_mask;
}

// =============================================================================
// Pixel Readback API (for picking and screenshots)
// =============================================================================

VkrRendererError
vkr_renderer_request_pixel_readback(VkrRendererFrontendHandle renderer,
                                    VkrTextureOpaqueHandle texture, uint32_t x,
                                    uint32_t y) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  struct s_TextureHandle *tex = (struct s_TextureHandle *)texture;

  VkrBackendResourceHandle handle = {.ptr = tex};
  return rf->backend.request_pixel_readback(rf->backend_state, handle, x, y);
}

VkrRendererError
vkr_renderer_get_pixel_readback_result(VkrRendererFrontendHandle renderer,
                                       VkrPixelReadbackResult *out_result) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_result != NULL, "Output result is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  return rf->backend.get_pixel_readback_result(rf->backend_state, out_result);
}

void vkr_renderer_update_readback_ring(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  assert_log(rf->backend.update_readback_ring != NULL,
             "Update readback ring function is not supported");
  rf->backend.update_readback_ring(rf->backend_state);
}

VkrAllocator *
vkr_renderer_get_backend_allocator(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;
  assert_log(rf->backend_state != NULL, "Backend state is NULL");
  if (rf->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    return &rf->allocator;
  }
  return rf->backend.get_allocator(rf->backend_state);
}
