/**
 * @file vkr_view_world.c
 * @brief World view layer implementation.
 *
 * The World layer is the primary 3D scene rendering layer. It manages:
 * - Mesh rendering with opaque and transparent passes
 * - 3D text rendering
 * - Camera updates from input
 * - Offscreen rendering for editor mode
 *
 * Offscreen rendering workflow:
 * 1. When editor mode is enabled, the World layer creates offscreen color and
 *    depth attachments matching the editor viewport size.
 * 2. The Skybox, World, and UI layers all render to these offscreen targets.
 * 3. The Editor layer samples the offscreen color texture to display the scene.
 * 4. Layout transitions are managed to ensure proper synchronization.
 */

#include "renderer/systems/views/vkr_view_world.h"

#include "containers/array.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_frustum.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/systems/views/vkr_view_skybox.h"
#include "renderer/systems/views/vkr_view_ui.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_layer_messages.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_view_system.h"
#include "renderer/vkr_draw_batch.h"
#include "renderer/vkr_indirect_draw.h"
#include "renderer/vulkan/vulkan_types.h"

/** Maximum number of 3D text objects per World layer. */
#define VKR_VIEW_WORLD_MAX_TEXTS 16
/** Size of the point light gizmo sphere in world units. */
#define VKR_VIEW_WORLD_LIGHT_GIZMO_SIZE 0.5f
#define VKR_VIEW_WORLD_LIGHT_GIZMO_LAT_SEGMENTS 12
#define VKR_VIEW_WORLD_LIGHT_GIZMO_LON_SEGMENTS 18
/** Initial capacity for world draw batching (per-frame). */
#define VKR_VIEW_WORLD_DRAW_BATCH_INITIAL_CAPACITY 1024

/** Offscreen renderpass names for Skybox/World/UI layering. */
#define VKR_VIEW_OFFSCREEN_SKYBOX_PASS_NAME "Renderpass.Offscreen.Skybox"
#define VKR_VIEW_OFFSCREEN_WORLD_PASS_NAME "Renderpass.Offscreen.World"

/**
 * @brief Slot for a 3D text object managed by the World layer.
 */
typedef struct VkrViewWorldTextSlot {
  VkrText3D text; /**< The 3D text instance. */
  bool8_t active; /**< Whether this slot is in use. */
} VkrViewWorldTextSlot;
Array(VkrViewWorldTextSlot);

/**
 * @brief Internal state for the World view layer.
 *
 * Contains all resources needed for 3D scene rendering including:
 * - Swapchain and offscreen pipelines
 * - Offscreen render targets and attachments
 * - 3D text slots
 * - Input state for camera control
 */
typedef struct VkrViewWorldState {
  /* Shader configurations */
  VkrShaderConfig shader_config;      /**< World mesh shader config. */
  VkrShaderConfig text_shader_config; /**< 3D text shader config. */

  /* Swapchain pipelines */
  VkrPipelineHandle pipeline;             /**< Opaque mesh pipeline. */
  VkrPipelineHandle transparent_pipeline; /**< Transparent mesh pipeline. */
  VkrPipelineHandle overlay_pipeline;     /**< Overlay pipeline (no depth). */
  VkrPipelineHandle text_pipeline;        /**< 3D text pipeline. */

  /* Offscreen pipelines (recreated for offscreen renderpass) */
  VkrPipelineHandle pipeline_offscreen; /**< Offscreen opaque. */
  VkrPipelineHandle
      transparent_pipeline_offscreen;           /**< Offscreen transparent. */
  VkrPipelineHandle overlay_pipeline_offscreen; /**< Offscreen overlay. */
  VkrPipelineHandle text_pipeline_offscreen;    /**< Offscreen text. */

  /* 3D Text slots */
  Array_VkrViewWorldTextSlot text_slots; /**< Pool of 3D text slots. */

  /* Point light gizmo rendering */
  VkrGeometryHandle light_gizmo_geometry; /**< Sphere geometry for lights. */
  VkrMaterialHandle light_gizmo_material; /**< Emissive material for lights. */
  VkrRendererInstanceStateHandle light_gizmo_instance_states
      [VKR_MAX_POINT_LIGHTS]; /**< Instance states for overlay pipeline. */
  VkrRendererInstanceStateHandle light_gizmo_instance_states_offscreen
      [VKR_MAX_POINT_LIGHTS]; /**< Offscreen overlay instances. */

  /* Input state */
  bool8_t use_gamepad;         /**< Whether gamepad input is active. */
  int8_t previous_wheel_delta; /**< Last scroll wheel delta. */
  bool8_t wheel_initialized;   /**< Whether scroll wheel state is valid. */

  /* Shadow data for the current frame */
  VkrShadowFrameData shadow_frame_data;
  bool8_t shadow_frame_valid;

  /* Draw batching */
  VkrDrawBatcher draw_batcher; /**< Persistent per-frame draw buffers. */

  /* Offscreen rendering resources */
  VkrRenderPassHandle offscreen_renderpass; /**< World offscreen pass. */
  VkrRenderPassHandle
      offscreen_skybox_renderpass;          /**< Skybox offscreen pass. */
  VkrRenderTargetHandle *offscreen_targets; /**< World render targets. */
  VkrRenderTargetHandle
      *offscreen_skybox_targets; /**< Skybox render targets. */
  VkrTextureOpaqueHandle
      *offscreen_colors; /**< Color attachments (one per swapchain image). */
  VkrTextureOpaqueHandle
      *offscreen_depths; /**< Depth attachments (one per swapchain image). */
  VkrTextureHandle
      *offscreen_color_handles; /**< Texture handles for sampling. */
  VkrTextureLayout
      *offscreen_color_layouts; /**< Layout tracking for transitions. */
  uint32_t
      offscreen_count; /**< Number of offscreen targets (swapchain count). */
  VkrTextureFormat offscreen_color_format; /**< Color attachment format. */
  uint32_t offscreen_width;                /**< Offscreen target width. */
  uint32_t offscreen_height;               /**< Offscreen target height. */
  bool8_t offscreen_enabled; /**< Whether offscreen mode is active. */
} VkrViewWorldState;

vkr_internal bool8_t vkr_submesh_uses_cutout(RendererFrontend *rf,
                                             VkrMaterial *material) {
  (void)rf;

  if (!material) {
    return false_v;
  }
  if (material->alpha_cutoff <= 0.0f) {
    return false_v;
  }

  VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  return diffuse_tex->enabled && diffuse_tex->handle.id != 0;
}

typedef struct VkrViewWorldLightGizmoContext {
  RendererFrontend *rf;
  VkrViewWorldState *state;
  VkrMaterial *material;
  const VkrScene *scene;
  VkrRendererInstanceStateHandle *instance_states;
  uint32_t instance_state_count;
  uint32_t instance_state_index;
} VkrViewWorldLightGizmoContext;

typedef struct VkrViewWorldDrawRange {
  const VkrIndexBuffer *index_buffer;
  uint32_t index_count;
  uint32_t first_index;
  int32_t vertex_offset;
  bool8_t uses_opaque_indices;
} VkrViewWorldDrawRange;

typedef struct VkrViewWorldDrawInfo {
  VkrGeometryHandle geometry;
  VkrViewWorldDrawRange range;
  bool8_t valid;
} VkrViewWorldDrawInfo;

vkr_internal VkrViewWorldDrawInfo vkr_view_world_get_draw_info(
    RendererFrontend *rf, const VkrDrawCommand *cmd, bool8_t allow_opaque);

vkr_internal VkrViewWorldDrawRange vkr_view_world_resolve_draw_range(
    RendererFrontend *rf, const VkrSubMesh *submesh, bool8_t allow_opaque) {
  VkrViewWorldDrawRange range = {
      .index_buffer = NULL,
      .index_count = submesh->index_count,
      .first_index = submesh->first_index,
      .vertex_offset = submesh->vertex_offset,
      .uses_opaque_indices = false_v,
  };

  if (!allow_opaque || submesh->opaque_index_count == 0) {
    return range;
  }

  VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
      &rf->geometry_system, submesh->geometry);
  if (!geometry || !geometry->opaque_index_buffer.handle) {
    return range;
  }

  range.index_buffer = &geometry->opaque_index_buffer;
  range.index_count = submesh->opaque_index_count;
  range.first_index = submesh->opaque_first_index;
  range.vertex_offset = submesh->opaque_vertex_offset;
  range.uses_opaque_indices = true_v;
  return range;
}

vkr_internal void vkr_view_world_acquire_light_gizmo_states(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *states, const char *label) {
  if (!rf || pipeline.id == 0 || !states)
    return;

  for (uint32_t i = 0; i < VKR_MAX_POINT_LIGHTS; ++i) {
    VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, pipeline, &states[i], &inst_err)) {
      String8 err_str = vkr_renderer_get_error_string(inst_err);
      log_warn("World view: light gizmo instance state failed (%s): %s", label,
               string8_cstr(&err_str));
      states[i] = (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID};
    }
  }
}

vkr_internal void vkr_view_world_release_light_gizmo_states(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *states) {
  if (!rf || pipeline.id == 0 || !states)
    return;

  for (uint32_t i = 0; i < VKR_MAX_POINT_LIGHTS; ++i) {
    if (states[i].id == VKR_INVALID_ID) {
      continue;
    }
    VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, pipeline, states[i], &inst_err);
  }
}

vkr_internal void
vkr_view_world_render_point_light_gizmos_cb(const VkrArchetype *arch,
                                            VkrChunk *chunk, void *user) {
  (void)arch;
  VkrViewWorldLightGizmoContext *ctx = (VkrViewWorldLightGizmoContext *)user;
  const VkrScene *scene = ctx->scene;
  VkrMaterial *material = ctx->material;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);

  if (!transforms || !lights || !material)
    return;

  RendererFrontend *rf = ctx->rf;
  VkrViewWorldState *state = ctx->state;

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    const SceneVisibility *vis =
        (const SceneVisibility *)vkr_entity_get_component(
            scene->world, entities[i], scene->comp_visibility);
    if (vis && !vis->visible)
      continue;

    if (ctx->instance_state_index >= ctx->instance_state_count) {
      return;
    }

    VkrRendererInstanceStateHandle instance_state =
        ctx->instance_states[ctx->instance_state_index++];
    if (instance_state.id == VKR_INVALID_ID) {
      continue;
    }

    Vec3 world_position = mat4_position(transforms[i].world);
    Mat4 model = mat4_mul(mat4_translate(world_position),
                          mat4_scale((Vec3){VKR_VIEW_WORLD_LIGHT_GIZMO_SIZE,
                                            VKR_VIEW_WORLD_LIGHT_GIZMO_SIZE,
                                            VKR_VIEW_WORLD_LIGHT_GIZMO_SIZE}));

    Vec3 light_color = lights[i].color;
    // material->phong.diffuse_color = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
    // material->phong.specular_color = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
    material->phong.diffuse_color =
        vec4_new(light_color.x, light_color.y, light_color.z, 1.0f);
    material->phong.specular_color =
        vec4_new(light_color.x, light_color.y, light_color.z, 1.0f);
    material->phong.emission_color = light_color;

    VkrInstanceBufferPool *instance_pool = &rf->instance_buffer_pool;
    VkrInstanceDataGPU *instance = NULL;
    uint32_t base_instance = 0;
    if (!vkr_instance_buffer_alloc(instance_pool, 1, &base_instance,
                                   &instance)) {
      log_warn("World gizmo: instance buffer allocation failed");
      return;
    }
    instance[0] = (VkrInstanceDataGPU){
        .model = model,
        .object_id = 0,
        .material_index = 0,
        .flags = 0,
        ._padding = 0,
    };
    vkr_instance_buffer_flush_range(instance_pool, base_instance, 1);

    vkr_shader_system_bind_instance(&rf->shader_system, instance_state.id);

    vkr_material_system_apply_instance(&rf->material_system, ctx->material,
                                       VKR_PIPELINE_DOMAIN_WORLD);

    vkr_geometry_system_render_instanced(rf, &rf->geometry_system,
                                         state->light_gizmo_geometry, 1,
                                         base_instance);
  }
}

vkr_internal void
vkr_view_world_render_point_light_gizmos(RendererFrontend *rf,
                                         VkrViewWorldState *state) {
  if (!rf || !state || !rf->active_scene)
    return;

  if (!state->light_gizmo_geometry.id || state->light_gizmo_material.id == 0)
    return;

  VkrScene *scene = rf->active_scene;
  if (!scene->queries_valid)
    return;

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &rf->material_system, state->light_gizmo_material);
  if (!material)
    return;

  if (state->offscreen_enabled) {
    if (state->pipeline_offscreen.id == 0) {
      return;
    }
  }

  VkrPipelineHandle pipeline = state->pipeline;
  VkrRendererInstanceStateHandle *instance_states =
      state->light_gizmo_instance_states;
  if (state->offscreen_enabled) {
    pipeline = state->pipeline_offscreen;
    instance_states = state->light_gizmo_instance_states_offscreen;
  }

  if (pipeline.id == 0)
    return;

  const char *shader_name =
      (material->shader_name && material->shader_name[0] != '\0')
          ? material->shader_name
          : "shader.default.world";
  if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.world");
  }

  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_pipeline.id != pipeline.id ||
      current_pipeline.generation != pipeline.generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                        &bind_err);
  }

  if (vkr_lighting_system_is_dirty(&rf->lighting_system)) {
    vkr_lighting_system_apply_uniforms(&rf->lighting_system);
  }

  VkrGlobalMaterialState gizmo_globals = rf->globals;
  gizmo_globals.render_mode = VKR_RENDER_MODE_UNLIT;
  vkr_material_system_apply_global(&rf->material_system, &gizmo_globals,
                                   VKR_PIPELINE_DOMAIN_WORLD);

  VkrViewWorldLightGizmoContext ctx = {
      .rf = rf,
      .state = state,
      .material = material,
      .scene = scene,
      .instance_states = instance_states,
      .instance_state_count = VKR_MAX_POINT_LIGHTS,
      .instance_state_index = 0,
  };

  vkr_entity_query_compiled_each_chunk(
      &scene->query_point_lights, vkr_view_world_render_point_light_gizmos_cb,
      &ctx);

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_WORLD);
}

/* ============================================================================
 * Layer Lifecycle Callbacks
 * ============================================================================
 */
vkr_internal bool32_t vkr_view_world_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_resize(VkrLayerContext *ctx, uint32_t width,
                                           uint32_t height);
vkr_internal void vkr_view_world_on_render(VkrLayerContext *ctx,
                                           const VkrLayerRenderInfo *info);
vkr_internal bool8_t vkr_view_world_on_update(VkrLayerContext *ctx,
                                              const VkrLayerUpdateInfo *info);
vkr_internal void vkr_view_world_on_detach(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_destroy(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_data_received(VkrLayerContext *ctx,
                                                  const VkrLayerMsgHeader *msg,
                                                  void *out_rsp,
                                                  uint64_t out_rsp_capacity,
                                                  uint64_t *out_rsp_size);

/* ============================================================================
 * Layer Lookup
 * ============================================================================
 */

/** Finds the World layer by handle in the view system. */
vkr_internal VkrLayer *vkr_view_world_find_layer(VkrViewSystem *vs,
                                                 VkrLayerHandle handle);

/* ============================================================================
 * Offscreen Target Management
 * ============================================================================
 */

/** Returns the swapchain color format for offscreen target matching. */
vkr_internal VkrTextureFormat
vkr_view_world_get_swapchain_format(RendererFrontend *rf);

/** Creates offscreen color/depth attachments and render targets. */
vkr_internal bool8_t vkr_view_world_create_offscreen_targets(
    VkrLayerContext *ctx, VkrViewWorldState *state);

/** Destroys all offscreen render targets and attachments. */
vkr_internal void
vkr_view_world_destroy_offscreen_targets(RendererFrontend *rf,
                                         VkrViewWorldState *state);

/** Resizes offscreen targets to new dimensions. */
vkr_internal void
vkr_view_world_resize_offscreen_targets(VkrLayerContext *ctx,
                                        VkrViewWorldState *state,
                                        uint32_t width, uint32_t height);

/** Applies offscreen targets to World, Skybox, and UI layers. */
vkr_internal void vkr_view_world_apply_offscreen_targets(
    RendererFrontend *rf, VkrViewWorldState *state, VkrLayerPass *pass);

/** Toggles between offscreen and swapchain rendering. */
vkr_internal bool8_t vkr_view_world_set_offscreen_enabled(
    VkrLayerContext *ctx, VkrViewWorldState *state, bool8_t enabled);

/* ============================================================================
 * 3D Text Management
 * ============================================================================
 */

/** Ensures a text slot is available for the given ID. */
vkr_internal bool8_t
vkr_view_world_ensure_text_slot(VkrViewWorldState *state, uint32_t text_id,
                                VkrViewWorldTextSlot **out_slot);

/** Gets an active text slot by ID, or NULL if not found/inactive. */
vkr_internal VkrViewWorldTextSlot *
vkr_view_world_get_text_slot(VkrViewWorldState *state, uint32_t text_id);

/** Rebuilds all active text objects with a new pipeline. */
vkr_internal void vkr_view_world_rebuild_texts(RendererFrontend *rf,
                                               VkrViewWorldState *state,
                                               VkrPipelineHandle pipeline);

bool32_t vkr_view_world_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register world view");
    return false_v;
  }

  if (rf->world_layer.id != 0) {
    return true_v;
  }

  VkrLayerPassConfig world_passes[1] = {{
      .renderpass_name = string8_lit("Renderpass.Builtin.World"),
      .use_swapchain_color = true_v,
      .use_depth = true_v,
  }};

  VkrViewWorldState *state =
      vkr_allocator_alloc(&rf->allocator, sizeof(VkrViewWorldState),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate world view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));

  VkrLayerConfig world_cfg = {
      .name = string8_lit("Layer.World"),
      .order = 0,
      .width = 0,
      .height = 0,
      .view = rf->globals.view,
      .projection = rf->globals.projection,
      .pass_count = ArrayCount(world_passes),
      .passes = world_passes,
      .callbacks = {.on_create = vkr_view_world_on_create,
                    .on_attach = vkr_view_world_on_attach,
                    .on_resize = vkr_view_world_on_resize,
                    .on_render = vkr_view_world_on_render,
                    .on_update = vkr_view_world_on_update,
                    .on_detach = vkr_view_world_on_detach,
                    .on_destroy = vkr_view_world_on_destroy,
                    .on_data_received = vkr_view_world_on_data_received},
      .user_data = state,
      .enabled = true_v,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &world_cfg, &rf->world_layer,
                                      &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register world view: %s", string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

void vkr_view_world_render_picking_text(RendererFrontend *rf,
                                        VkrPipelineHandle pipeline) {
  if (!rf || !rf->view_system.initialized || pipeline.id == 0) {
    return;
  }

  VkrLayer *world_layer =
      vkr_view_world_find_layer(&rf->view_system, rf->world_layer);
  if (!world_layer || !world_layer->enabled || !world_layer->user_data) {
    return;
  }

  VkrViewWorldState *state = (VkrViewWorldState *)world_layer->user_data;
  if (!state->text_slots.data) {
    return;
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking_text")) {
    log_warn("Failed to use picking text shader for world");
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                           &bind_err)) {
    String8 err_str = vkr_renderer_get_error_string(bind_err);
    log_warn("Failed to bind picking text pipeline for world: %s",
             string8_cstr(&err_str));
    return;
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_WORLD);

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewWorldTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }

    vkr_text_3d_update(&slot->text);
    if (slot->text.quad_count == 0) {
      continue;
    }

    uint32_t object_id =
        vkr_picking_encode_id(VKR_PICKING_ID_KIND_WORLD_TEXT, (uint32_t)i);
    if (object_id == 0) {
      continue;
    }

    Mat4 model = vkr_transform_get_world(&slot->text.transform);
    if (slot->text.texture_width > 0 && slot->text.texture_height > 0) {
      Vec3 scale = vec3_new(
          slot->text.world_width / (float32_t)slot->text.texture_width,
          slot->text.world_height / (float32_t)slot->text.texture_height, 1.0f);
      model = mat4_mul(model, mat4_scale(scale));
    }

    vkr_material_system_apply_local(
        &rf->material_system,
        &(VkrLocalMaterialState){.model = model, .object_id = object_id});

    if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
      continue;
    }

    VkrVertexBufferBinding vbb = {
        .buffer = slot->text.vertex_buffer.handle,
        .binding = 0,
        .offset = 0,
    };
    vkr_renderer_bind_vertex_buffer(rf, &vbb);

    VkrIndexBufferBinding ibb = {
        .buffer = slot->text.index_buffer.handle,
        .type = VKR_INDEX_TYPE_UINT32,
        .offset = 0,
    };
    vkr_renderer_bind_index_buffer(rf, &ibb);

    uint32_t index_count = slot->text.quad_count * 6;
    vkr_renderer_draw_indexed(rf, index_count, 1, 0, 0, 0);
  }
}

vkr_internal VkrLayer *vkr_view_world_find_layer(VkrViewSystem *vs,
                                                 VkrLayerHandle handle) {
  if (!vs || !vs->initialized || handle.id == 0) {
    return NULL;
  }

  if (handle.id - 1 >= vs->layers.length) {
    return NULL;
  }

  VkrLayer *layer = array_get_VkrLayer(&vs->layers, handle.id - 1);
  if (!layer->active) {
    return NULL;
  }

  if (layer->handle.generation != handle.generation) {
    return NULL;
  }

  return layer;
}

vkr_internal bool8_t
vkr_view_world_ensure_text_slot(VkrViewWorldState *state, uint32_t text_id,
                                VkrViewWorldTextSlot **out_slot) {
  if (!state || !out_slot) {
    return false_v;
  }

  if (!state->text_slots.data) {
    log_error("World text slots not initialized");
    return false_v;
  }

  if (text_id >= state->text_slots.length) {
    log_error("World text id %u exceeds max (%llu)", text_id,
              (unsigned long long)state->text_slots.length);
    return false_v;
  }

  *out_slot = &state->text_slots.data[text_id];
  return true_v;
}

vkr_internal VkrViewWorldTextSlot *
vkr_view_world_get_text_slot(VkrViewWorldState *state, uint32_t text_id) {
  if (!state || !state->text_slots.data ||
      text_id >= state->text_slots.length) {
    return NULL;
  }

  VkrViewWorldTextSlot *slot = &state->text_slots.data[text_id];
  return slot->active ? slot : NULL;
}

/* Switches world/skybox passes between offscreen and swapchain output. */
vkr_internal bool8_t vkr_view_world_set_offscreen_enabled(
    VkrLayerContext *ctx, VkrViewWorldState *state, bool8_t enabled) {
  if (!ctx || !state) {
    return false_v;
  }

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrLayer *layer = ctx->layer;
  if (!layer || layer->pass_count == 0) {
    return false_v;
  }

  uint32_t target_width =
      state->offscreen_width > 0 ? state->offscreen_width : layer->width;
  uint32_t target_height =
      state->offscreen_height > 0 ? state->offscreen_height : layer->height;

  if (state->offscreen_enabled == enabled) {
    return true_v;
  }

  VkrRendererError wait_err = vkr_renderer_wait_idle(rf);
  if (wait_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(wait_err);
    log_warn("Wait idle failed before toggling offscreen: %s",
             string8_cstr(&err_str));
  }

  VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, 0);

  if (enabled) {
    VkrRenderTargetHandle *old_targets = NULL;
    uint32_t old_target_count = 0;
    if (!pass->use_custom_render_targets && pass->render_targets &&
        pass->render_target_count > 0) {
      old_targets = pass->render_targets;
      old_target_count = pass->render_target_count;
    }

    if (state->offscreen_targets && state->offscreen_count > 0) {
      vkr_view_world_resize_offscreen_targets(ctx, state, target_width,
                                              target_height);
      vkr_view_world_apply_offscreen_targets(rf, state, pass);
    } else {
      if (!vkr_view_world_create_offscreen_targets(ctx, state)) {
        return false_v;
      }
      vkr_view_world_apply_offscreen_targets(rf, state, pass);
    }

    if (old_targets && old_target_count > 0) {
      for (uint32_t i = 0; i < old_target_count; ++i) {
        if (old_targets[i]) {
          vkr_renderer_render_target_destroy(rf, old_targets[i]);
        }
      }
      vkr_allocator_free(&rf->view_system.allocator, (void *)old_targets,
                         sizeof(VkrRenderTargetHandle) *
                             (uint64_t)old_target_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    state->offscreen_enabled = true_v;
    if (!vkr_view_ui_set_offscreen_enabled(
            rf, true_v, state->offscreen_colors, state->offscreen_color_layouts,
            state->offscreen_count, target_width, target_height)) {
      log_warn("Failed to switch UI to offscreen targets");
    }
    VkrPipelineHandle text_pipeline = state->text_pipeline;
    if (state->text_pipeline_offscreen.id != 0) {
      text_pipeline = state->text_pipeline_offscreen;
    }
    vkr_view_world_rebuild_texts(rf, state, text_pipeline);
    vkr_camera_registry_resize_all(&rf->camera_system, target_width,
                                   target_height);
    return true_v;
  }

  if (pass->use_custom_render_targets) {
    pass->render_targets = NULL;
    pass->render_target_count = 0;
    pass->custom_color_attachments = NULL;
    pass->custom_color_layouts = NULL;
  }

  pass->use_custom_render_targets = false_v;
  pass->use_swapchain_color = true_v;
  pass->use_depth = true_v;
  pass->renderpass_name = string8_lit("Renderpass.Builtin.World");
  pass->renderpass = NULL;

  vkr_view_skybox_use_swapchain_targets(rf);

  rf->offscreen_color_handles = NULL;
  rf->offscreen_color_handle_count = 0;
  state->offscreen_enabled = false_v;

  if (!vkr_view_ui_set_offscreen_enabled(rf, false_v, NULL, NULL, 0, 0, 0)) {
    log_warn("Failed to switch UI to builtin renderpass");
    vkr_view_system_rebuild_targets(rf);
  }

  vkr_view_world_rebuild_texts(rf, state, state->text_pipeline);

  vkr_camera_registry_resize_all(&rf->camera_system, layer->width,
                                 layer->height);

  return true_v;
}

/* Applies offscreen render targets to the world pass and skybox pass. */
vkr_internal void vkr_view_world_apply_offscreen_targets(
    RendererFrontend *rf, VkrViewWorldState *state, VkrLayerPass *pass) {
  if (!rf || !state || !pass) {
    return;
  }

  pass->use_custom_render_targets = true_v;
  pass->use_swapchain_color = false_v;
  pass->renderpass_name = string8_lit(VKR_VIEW_OFFSCREEN_WORLD_PASS_NAME);
  pass->renderpass = state->offscreen_renderpass;
  pass->render_targets = state->offscreen_targets;
  pass->render_target_count = state->offscreen_count;
  pass->custom_color_attachments = state->offscreen_colors;
  pass->custom_color_attachment_count = state->offscreen_count;
  pass->custom_color_layouts = state->offscreen_color_layouts;

  VkrRenderTargetHandle *skybox_targets = state->offscreen_skybox_targets
                                              ? state->offscreen_skybox_targets
                                              : state->offscreen_targets;
  if (!vkr_view_skybox_set_custom_targets(
          rf, string8_lit(VKR_VIEW_OFFSCREEN_SKYBOX_PASS_NAME),
          state->offscreen_skybox_renderpass, skybox_targets,
          state->offscreen_count, state->offscreen_colors,
          state->offscreen_count, state->offscreen_color_layouts)) {
    log_warn("Failed to bind offscreen skybox targets");
  }

  rf->offscreen_color_handles = state->offscreen_color_handles;
  rf->offscreen_color_handle_count = state->offscreen_count;

  if (state->offscreen_enabled) {
    uint32_t ui_width = state->offscreen_width > 0 ? state->offscreen_width
                                                   : rf->last_window_width;
    uint32_t ui_height = state->offscreen_height > 0 ? state->offscreen_height
                                                     : rf->last_window_height;
    if (!vkr_view_ui_set_offscreen_enabled(
            rf, true_v, state->offscreen_colors, state->offscreen_color_layouts,
            state->offscreen_count, ui_width, ui_height)) {
      log_warn("Failed to refresh offscreen UI targets after resize");
    }
  }
}

vkr_internal VkrTextureFormat
vkr_view_world_get_swapchain_format(RendererFrontend *rf) {
  VkrTextureFormat format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  if (!rf) {
    return format;
  }

  if (rf->backend_type != VKR_RENDERER_BACKEND_TYPE_VULKAN) {
    return format;
  }

  VkrTextureOpaqueHandle swapchain_tex =
      vkr_renderer_window_attachment_get(rf, 0);
  if (!swapchain_tex) {
    return format;
  }

  struct s_TextureHandle *handle = (struct s_TextureHandle *)swapchain_tex;
  return handle->description.format;
}

vkr_internal VkrTextureFormat
vkr_view_world_get_depth_format(RendererFrontend *rf) {
  if (!rf) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }

  VkrTextureOpaqueHandle depth_tex = vkr_renderer_depth_attachment_get(rf);
  if (!depth_tex) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }

  struct s_TextureHandle *handle = (struct s_TextureHandle *)depth_tex;
  return handle->description.format;
}

vkr_internal VkrRenderTargetHandle vkr_view_world_create_color_depth_target(
    RendererFrontend *rf, VkrRenderPassHandle pass,
    VkrTextureOpaqueHandle color, VkrTextureOpaqueHandle depth, uint32_t width,
    uint32_t height, VkrRendererError *out_error) {
  if (!rf || !pass || !color || !depth) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  VkrRenderTargetAttachmentRef attachments[2] = {
      {.texture = color, .mip_level = 0, .base_layer = 0, .layer_count = 1},
      {.texture = depth, .mip_level = 0, .base_layer = 0, .layer_count = 1},
  };
  VkrRenderTargetDesc desc = {
      .sync_to_window_size = false_v,
      .attachment_count = 2,
      .attachments = attachments,
      .width = width,
      .height = height,
  };

  return vkr_renderer_render_target_create(rf, &desc, pass, out_error);
}

vkr_internal bool8_t vkr_view_world_create_offscreen_targets(
    VkrLayerContext *ctx, VkrViewWorldState *state) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(state != NULL, "State is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrLayer *layer = ctx->layer;
  if (!layer || layer->pass_count == 0) {
    return false_v;
  }

  uint32_t target_width =
      state->offscreen_width > 0 ? state->offscreen_width : layer->width;
  uint32_t target_height =
      state->offscreen_height > 0 ? state->offscreen_height : layer->height;
  if (target_width == 0 || target_height == 0) {
    log_error("Offscreen target size invalid (%ux%u)", target_width,
              target_height);
    return false_v;
  }

  uint32_t count = vkr_renderer_window_attachment_count(rf);
  if (count == 0) {
    log_error("Offscreen targets unavailable: swapchain image count is 0");
    return false_v;
  }

  state->offscreen_color_format = vkr_view_world_get_swapchain_format(rf);
  VkrTextureFormat depth_format = vkr_view_world_get_depth_format(rf);
  VkrClearValue clear_world = {.color_f32 = {0.1f, 0.1f, 0.2f, 1.0f}};
  VkrClearValue clear_depth = {.depth_stencil = {1.0f, 0}};

  if (!state->offscreen_skybox_renderpass) {
    VkrRenderPassAttachmentDesc skybox_color = {
        .format = state->offscreen_color_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = clear_world,
    };
    VkrRenderPassAttachmentDesc skybox_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc skybox_desc = {
        .name = string8_lit(VKR_VIEW_OFFSCREEN_SKYBOX_PASS_NAME),
        .domain = VKR_PIPELINE_DOMAIN_SKYBOX,
        .color_attachment_count = 1,
        .color_attachments = &skybox_color,
        .depth_stencil_attachment = &skybox_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_skybox_renderpass =
        vkr_renderer_renderpass_create_desc(rf, &skybox_desc, &pass_err);
    if (!state->offscreen_skybox_renderpass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("Failed to create offscreen skybox renderpass");
      log_error("Renderpass error: %s", string8_cstr(&err));
      return false_v;
    }
  }

  if (!state->offscreen_renderpass) {
    VkrRenderPassAttachmentDesc world_color = {
        .format = state->offscreen_color_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = clear_world,
    };
    VkrRenderPassAttachmentDesc world_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc world_desc = {
        .name = string8_lit(VKR_VIEW_OFFSCREEN_WORLD_PASS_NAME),
        .domain = VKR_PIPELINE_DOMAIN_WORLD,
        .color_attachment_count = 1,
        .color_attachments = &world_color,
        .depth_stencil_attachment = &world_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_renderpass =
        vkr_renderer_renderpass_create_desc(rf, &world_desc, &pass_err);
    if (!state->offscreen_renderpass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("Failed to create offscreen world renderpass");
      log_error("Renderpass error: %s", string8_cstr(&err));
      return false_v;
    }
  }

  VkrAllocator *alloc = &rf->allocator;
  state->offscreen_targets = (VkrRenderTargetHandle *)vkr_allocator_alloc(
      alloc, sizeof(VkrRenderTargetHandle) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  state->offscreen_skybox_targets =
      (VkrRenderTargetHandle *)vkr_allocator_alloc(
          alloc, sizeof(VkrRenderTargetHandle) * (uint64_t)count,
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  state->offscreen_colors = (VkrTextureOpaqueHandle *)vkr_allocator_alloc(
      alloc, sizeof(VkrTextureOpaqueHandle) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  state->offscreen_depths = (VkrTextureOpaqueHandle *)vkr_allocator_alloc(
      alloc, sizeof(VkrTextureOpaqueHandle) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  state->offscreen_color_handles = (VkrTextureHandle *)vkr_allocator_alloc(
      alloc, sizeof(VkrTextureHandle) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  state->offscreen_color_layouts = (VkrTextureLayout *)vkr_allocator_alloc(
      alloc, sizeof(VkrTextureLayout) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!state->offscreen_targets || !state->offscreen_skybox_targets ||
      !state->offscreen_colors || !state->offscreen_depths ||
      !state->offscreen_color_handles || !state->offscreen_color_layouts) {
    log_error("Failed to allocate offscreen target buffers");
    return false_v;
  }

  MemZero((void *)state->offscreen_targets,
          sizeof(VkrRenderTargetHandle) * (uint64_t)count);
  MemZero((void *)state->offscreen_skybox_targets,
          sizeof(VkrRenderTargetHandle) * (uint64_t)count);
  MemZero((void *)state->offscreen_colors,
          sizeof(VkrTextureOpaqueHandle) * (uint64_t)count);
  MemZero((void *)state->offscreen_depths,
          sizeof(VkrTextureOpaqueHandle) * (uint64_t)count);
  MemZero((void *)state->offscreen_color_handles,
          sizeof(VkrTextureHandle) * (uint64_t)count);

  state->offscreen_count = count;

  for (uint32_t i = 0; i < count; ++i) {
    VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
    VkrRenderTargetTextureDesc tex_desc = {
        .width = target_width,
        .height = target_height,
        .format = state->offscreen_color_format,
        .usage = vkr_texture_usage_flags_from_bits(
            VKR_TEXTURE_USAGE_COLOR_ATTACHMENT | VKR_TEXTURE_USAGE_SAMPLED),
    };
    state->offscreen_colors[i] =
        vkr_renderer_create_render_target_texture(rf, &tex_desc, &tex_err);
    if (!state->offscreen_colors[i]) {
      String8 err = vkr_renderer_get_error_string(tex_err);
      log_error("Failed to create offscreen color target: %s",
                string8_cstr(&err));
      return false_v;
    }

    VkrTextureDescription desc = {
        .width = target_width,
        .height = target_height,
        .channels = 4,
        .type = VKR_TEXTURE_TYPE_2D,
        .format = state->offscreen_color_format,
        .properties = vkr_texture_property_flags_create(),
        .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
        .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
        .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
        .min_filter = VKR_FILTER_LINEAR,
        .mag_filter = VKR_FILTER_LINEAR,
        .mip_filter = VKR_MIP_FILTER_NONE,
        .anisotropy_enable = false_v,
    };
    bitset8_set(&desc.properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
    bitset8_set(&desc.properties, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);

    String8 name =
        string8_create_formatted(alloc, "RenderTarget.Offscreen.World.%u", i);
    if (!vkr_texture_system_register_external(
            &rf->texture_system, name, state->offscreen_colors[i], &desc,
            &state->offscreen_color_handles[i])) {
      log_error("Failed to register offscreen color target %u", i);
      return false_v;
    }

    VkrRendererError depth_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_depths[i] = vkr_renderer_create_depth_attachment(
        rf, target_width, target_height, &depth_err);
    if (!state->offscreen_depths[i]) {
      String8 err = vkr_renderer_get_error_string(depth_err);
      log_error("Failed to create offscreen depth target: %s",
                string8_cstr(&err));
      return false_v;
    }

    VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_targets[i] = vkr_view_world_create_color_depth_target(
        rf, state->offscreen_renderpass, state->offscreen_colors[i],
        state->offscreen_depths[i], target_width, target_height, &rt_err);
    if (!state->offscreen_targets[i]) {
      String8 err = vkr_renderer_get_error_string(rt_err);
      log_error("Failed to create offscreen render target %u", i);
      log_error("Render target error: %s", string8_cstr(&err));
      return false_v;
    }

    if (state->offscreen_skybox_renderpass) {
      VkrRendererError skybox_err = VKR_RENDERER_ERROR_NONE;
      state->offscreen_skybox_targets[i] =
          vkr_view_world_create_color_depth_target(
              rf, state->offscreen_skybox_renderpass, state->offscreen_colors[i],
              state->offscreen_depths[i], target_width, target_height,
              &skybox_err);
      if (!state->offscreen_skybox_targets[i]) {
        String8 err = vkr_renderer_get_error_string(skybox_err);
        log_error("Failed to create offscreen skybox render target %u", i);
        log_error("Render target error: %s", string8_cstr(&err));
        return false_v;
      }
    } else {
      log_error("Offscreen skybox renderpass unavailable");
      return false_v;
    }

    state->offscreen_color_layouts[i] = VKR_TEXTURE_LAYOUT_UNDEFINED;
  }

  return true_v;
}

vkr_internal void
vkr_view_world_destroy_offscreen_targets(RendererFrontend *rf,
                                         VkrViewWorldState *state) {
  if (!rf || !state) {
    return;
  }

  VkrRendererError wait_err = vkr_renderer_wait_idle(rf);
  if (wait_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(wait_err);
    log_warn("Wait idle failed before destroying offscreen targets: %s",
             string8_cstr(&err_str));
  }

  if (state->offscreen_targets) {
    for (uint32_t i = 0; i < state->offscreen_count; ++i) {
      if (state->offscreen_targets[i]) {
        vkr_renderer_render_target_destroy(rf, state->offscreen_targets[i]);
      }
    }
  }
  if (state->offscreen_skybox_targets) {
    for (uint32_t i = 0; i < state->offscreen_count; ++i) {
      if (state->offscreen_skybox_targets[i]) {
        vkr_renderer_render_target_destroy(rf, state->offscreen_skybox_targets[i]);
      }
    }
  }

  if (state->offscreen_depths) {
    for (uint32_t i = 0; i < state->offscreen_count; ++i) {
      if (state->offscreen_depths[i]) {
        vkr_renderer_destroy_texture(rf, state->offscreen_depths[i]);
      }
    }
  }

  if (state->offscreen_targets) {
    vkr_allocator_free(&rf->allocator, (void *)state->offscreen_targets,
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_targets = NULL;
  }
  if (state->offscreen_skybox_targets) {
    vkr_allocator_free(&rf->allocator, (void *)state->offscreen_skybox_targets,
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_skybox_targets = NULL;
  }

  if (state->offscreen_colors) {
    vkr_allocator_free(&rf->allocator, (void *)state->offscreen_colors,
                       sizeof(VkrTextureOpaqueHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_colors = NULL;
  }

  if (state->offscreen_depths) {
    vkr_allocator_free(&rf->allocator, (void *)state->offscreen_depths,
                       sizeof(VkrTextureOpaqueHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_depths = NULL;
  }

  if (state->offscreen_color_handles) {
    vkr_allocator_free(&rf->allocator, state->offscreen_color_handles,
                       sizeof(VkrTextureHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_color_handles = NULL;
  }

  if (state->offscreen_color_layouts) {
    vkr_allocator_free(&rf->allocator, state->offscreen_color_layouts,
                       sizeof(VkrTextureLayout) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->offscreen_color_layouts = NULL;
  }

  if (state->offscreen_renderpass) {
    vkr_renderer_renderpass_destroy(rf, state->offscreen_renderpass);
    state->offscreen_renderpass = NULL;
  }
  if (state->offscreen_skybox_renderpass) {
    vkr_renderer_renderpass_destroy(rf, state->offscreen_skybox_renderpass);
    state->offscreen_skybox_renderpass = NULL;
  }

  state->offscreen_count = 0;
}

vkr_internal void
vkr_view_world_resize_offscreen_targets(VkrLayerContext *ctx,
                                        VkrViewWorldState *state,
                                        uint32_t width, uint32_t height) {
  if (!ctx || !state) {
    return;
  }

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrRendererError wait_err = vkr_renderer_wait_idle(rf);
  if (wait_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(wait_err);
    log_warn("Wait idle failed before resizing offscreen targets: %s",
             string8_cstr(&err_str));
  }

  if (!state->offscreen_targets || state->offscreen_count == 0) {
    vkr_view_world_create_offscreen_targets(ctx, state);
    return;
  }

  uint32_t count = vkr_renderer_window_attachment_count(rf);
  if (count > state->offscreen_count) {
    log_warn("Offscreen target count mismatch (%u > %u); extra images ignored",
             count, state->offscreen_count);
  }

  for (uint32_t i = 0; i < state->offscreen_count; ++i) {
    if (state->offscreen_color_handles) {
      VkrTextureHandle updated_handle = state->offscreen_color_handles[i];
      VkrRendererError resize_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_texture_system_resize(
              &rf->texture_system, state->offscreen_color_handles[i], width,
              height, false_v, &updated_handle, &resize_err)) {
        String8 err = vkr_renderer_get_error_string(resize_err);
        log_error("Failed to resize offscreen color target %u: %s", i,
                  string8_cstr(&err));
      } else {
        state->offscreen_color_handles[i] = updated_handle;
      }
    }

    if (state->offscreen_depths && state->offscreen_depths[i]) {
      vkr_renderer_destroy_texture(rf, state->offscreen_depths[i]);
      VkrRendererError depth_err = VKR_RENDERER_ERROR_NONE;
      state->offscreen_depths[i] =
          vkr_renderer_create_depth_attachment(rf, width, height, &depth_err);
      if (!state->offscreen_depths[i]) {
        String8 err = vkr_renderer_get_error_string(depth_err);
        log_error("Failed to resize offscreen depth target %u: %s", i,
                  string8_cstr(&err));
      }
    }

    if (state->offscreen_targets && state->offscreen_targets[i]) {
      vkr_renderer_render_target_destroy(rf, state->offscreen_targets[i]);
      state->offscreen_targets[i] = NULL;
    }
    if (state->offscreen_skybox_targets && state->offscreen_skybox_targets[i]) {
      vkr_renderer_render_target_destroy(rf, state->offscreen_skybox_targets[i]);
      state->offscreen_skybox_targets[i] = NULL;
    }

    if (state->offscreen_colors && state->offscreen_depths &&
        state->offscreen_renderpass) {
      VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
      state->offscreen_targets[i] = vkr_view_world_create_color_depth_target(
          rf, state->offscreen_renderpass, state->offscreen_colors[i],
          state->offscreen_depths[i], width, height, &rt_err);
      if (!state->offscreen_targets[i]) {
        String8 err = vkr_renderer_get_error_string(rt_err);
        log_error("Failed to recreate offscreen render target %u", i);
        log_error("Render target error: %s", string8_cstr(&err));
      }

      if (state->offscreen_skybox_renderpass &&
          state->offscreen_skybox_targets) {
        VkrRendererError skybox_err = VKR_RENDERER_ERROR_NONE;
        state->offscreen_skybox_targets[i] =
            vkr_view_world_create_color_depth_target(
                rf, state->offscreen_skybox_renderpass,
                state->offscreen_colors[i], state->offscreen_depths[i], width,
                height, &skybox_err);
        if (!state->offscreen_skybox_targets[i]) {
          String8 err = vkr_renderer_get_error_string(skybox_err);
          log_error("Failed to recreate offscreen skybox target %u", i);
          log_error("Render target error: %s", string8_cstr(&err));
        }
      }
    }

    if (state->offscreen_color_layouts) {
      state->offscreen_color_layouts[i] = VKR_TEXTURE_LAYOUT_UNDEFINED;
    }
  }

  rf->offscreen_color_handles = state->offscreen_color_handles;
  rf->offscreen_color_handle_count = state->offscreen_count;

  if (state->offscreen_enabled) {
    if (!vkr_view_ui_set_offscreen_enabled(
            rf, true_v, state->offscreen_colors, state->offscreen_color_layouts,
            state->offscreen_count, width, height)) {
      log_warn("Failed to refresh offscreen UI targets after resize");
    }
  }
}

vkr_internal void vkr_view_world_rebuild_texts(RendererFrontend *rf,
                                               VkrViewWorldState *state,
                                               VkrPipelineHandle pipeline) {
  if (!rf || !state || pipeline.id == 0 || !state->text_slots.data) {
    return;
  }

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewWorldTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }

    VkrText3DConfig config = VKR_TEXT_3D_CONFIG_DEFAULT;
    config.text = slot->text.text;
    config.font = slot->text.font;
    config.font_size = slot->text.font_size;
    config.color = slot->text.color;
    config.texture_width = slot->text.texture_width;
    config.texture_height = slot->text.texture_height;
    config.uv_inset_px = slot->text.uv_inset_px;
    config.pipeline = pipeline;

    VkrText3D new_text = {0};
    VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_text_3d_create(&new_text, rf, &rf->font_system, &rf->allocator,
                            &config, &text_err)) {
      String8 err = vkr_renderer_get_error_string(text_err);
      log_error("Failed to rebuild world text pipeline: %s",
                string8_cstr(&err));
      continue;
    }

    VkrTransform transform = slot->text.transform;
    vkr_text_3d_destroy(&slot->text);
    slot->text = new_text;
    vkr_text_3d_set_transform(&slot->text, transform);
    slot->active = true_v;
  }
}

vkr_internal bool32_t vkr_view_world_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  for (uint32_t i = 0; i < VKR_MAX_POINT_LIGHTS; ++i) {
    state->light_gizmo_instance_states[i].id = VKR_INVALID_ID;
    state->light_gizmo_instance_states_offscreen[i].id = VKR_INVALID_ID;
  }

  state->offscreen_enabled = false_v;

  if (!vkr_draw_batcher_init(&state->draw_batcher, &rf->allocator,
                             VKR_VIEW_WORLD_DRAW_BATCH_INITIAL_CAPACITY)) {
    log_error("Failed to initialize world draw batcher");
    return false_v;
  }

  if (!vkr_view_world_create_offscreen_targets(ctx, state)) {
    return false_v;
  }

  VkrResourceHandleInfo world_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world.shadercfg"),
          &rf->scratch_allocator, &world_cfg_info, &shadercfg_err)) {
    state->shader_config = *(VkrShaderConfig *)world_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("World shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  if (!vkr_shader_system_create(&rf->shader_system, &state->shader_config)) {
    log_error("Failed to create shader system from config");
    return false_v;
  }

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world"), &state->pipeline,
          &pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_error("Config world pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  if (state->shader_config.name.str && state->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->pipeline, state->shader_config.name,
        &alias_err);
  }

  // Create transparent world pipeline (same shader, different domain settings)
  VkrRendererError transparent_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
          string8_lit("world_transparent"), &state->transparent_pipeline,
          &transparent_pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(transparent_pipeline_error);
    log_error("Config world transparent pipeline failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError overlay_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_OVERLAY, string8_lit("world_overlay"),
          &state->overlay_pipeline, &overlay_pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(overlay_pipeline_error);
    log_warn("Config world overlay pipeline failed: %s",
             string8_cstr(&err_str));
    state->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (state->offscreen_renderpass) {
    VkrShaderConfig offscreen_world_cfg = state->shader_config;
    offscreen_world_cfg.renderpass_name =
        string8_lit(VKR_VIEW_OFFSCREEN_WORLD_PASS_NAME);
    offscreen_world_cfg.name = (String8){0};

    VkrRendererError offscreen_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &offscreen_world_cfg,
            VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world_offscreen"),
            &state->pipeline_offscreen, &offscreen_err)) {
      String8 err_str = vkr_renderer_get_error_string(offscreen_err);
      log_warn("Config offscreen world pipeline failed: %s",
               string8_cstr(&err_str));
      state->pipeline_offscreen = VKR_PIPELINE_HANDLE_INVALID;
    }

    VkrRendererError offscreen_transparent_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &offscreen_world_cfg,
            VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
            string8_lit("world_transparent_offscreen"),
            &state->transparent_pipeline_offscreen,
            &offscreen_transparent_err)) {
      String8 err_str =
          vkr_renderer_get_error_string(offscreen_transparent_err);
      log_warn("Config offscreen transparent pipeline failed: %s",
               string8_cstr(&err_str));
      state->transparent_pipeline_offscreen = VKR_PIPELINE_HANDLE_INVALID;
    }

    VkrRendererError offscreen_overlay_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &offscreen_world_cfg,
            VKR_PIPELINE_DOMAIN_WORLD_OVERLAY,
            string8_lit("world_overlay_offscreen"),
            &state->overlay_pipeline_offscreen, &offscreen_overlay_err)) {
      String8 err_str = vkr_renderer_get_error_string(offscreen_overlay_err);
      log_warn("Config offscreen overlay pipeline failed: %s",
               string8_cstr(&err_str));
      state->overlay_pipeline_offscreen = VKR_PIPELINE_HANDLE_INVALID;
    }
  }

  VkrResourceHandleInfo text_cfg_info = {0};
  VkrRendererError text_shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world_text.shadercfg"),
          &rf->scratch_allocator, &text_cfg_info, &text_shadercfg_err)) {
    state->text_shader_config = *(VkrShaderConfig *)text_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(text_shadercfg_err);
    log_error("World text shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  if (!vkr_shader_system_create(&rf->shader_system,
                                &state->text_shader_config)) {
    log_error("Failed to create text shader system");
    return false_v;
  }

  // Create text pipeline with culling disabled and depth-tested blending.
  VkrShaderConfig text_shader_config = state->text_shader_config;
  text_shader_config.cull_mode = VKR_CULL_MODE_NONE;
  VkrRendererError text_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &text_shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, string8_lit("world_text_3d"),
          &state->text_pipeline, &text_pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(text_pipeline_error);
    log_warn("Config world text pipeline failed: %s", string8_cstr(&err_str));
    state->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (state->offscreen_renderpass) {
    VkrShaderConfig offscreen_text_cfg = text_shader_config;
    offscreen_text_cfg.renderpass_name =
        string8_lit(VKR_VIEW_OFFSCREEN_WORLD_PASS_NAME);
    offscreen_text_cfg.name = (String8){0};
    VkrRendererError offscreen_text_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &offscreen_text_cfg,
            VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
            string8_lit("world_text_offscreen"),
            &state->text_pipeline_offscreen, &offscreen_text_err)) {
      String8 err_str = vkr_renderer_get_error_string(offscreen_text_err);
      log_warn("Config offscreen world text pipeline failed: %s",
               string8_cstr(&err_str));
      state->text_pipeline_offscreen = VKR_PIPELINE_HANDLE_INVALID;
    }
  }

  if (state->text_pipeline.id != VKR_PIPELINE_HANDLE_INVALID.id &&
      text_shader_config.name.str && text_shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->text_pipeline, text_shader_config.name,
        &alias_err);
  }

  VkrRendererError light_geom_err = VKR_RENDERER_ERROR_NONE;
  state->light_gizmo_geometry = vkr_geometry_system_create_sphere(
      &rf->geometry_system, 1.0f, VKR_VIEW_WORLD_LIGHT_GIZMO_LAT_SEGMENTS,
      VKR_VIEW_WORLD_LIGHT_GIZMO_LON_SEGMENTS, vec3_zero(), vec3_zero(),
      "world_light_gizmo", &light_geom_err);
  if (!state->light_gizmo_geometry.id) {
    String8 err_str = vkr_renderer_get_error_string(light_geom_err);
    log_warn("World view: light gizmo geometry creation failed: %s",
             string8_cstr(&err_str));
  }

  VkrRendererError light_mat_err = VKR_RENDERER_ERROR_NONE;
  String8 light_mat_name = string8_lit("__light_gizmo");
  VkrMaterialHandle light_mat = vkr_material_system_acquire(
      &rf->material_system, light_mat_name, true_v, &light_mat_err);
  if (light_mat.id == 0) {
    light_mat = vkr_material_system_create_colored(
        &rf->material_system, "__light_gizmo", vec4_new(0.0f, 0.0f, 0.0f, 1.0f),
        &light_mat_err);
  }

  if (light_mat.id != 0) {
    VkrMaterial *material =
        vkr_material_system_get_by_handle(&rf->material_system, light_mat);
    if (material) {
      material->phong.diffuse_color = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
      material->phong.specular_color = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
      material->phong.emission_color = vec3_new(4.0f, 4.0f, 4.0f);
      material->phong.shininess = 1.0f;
      material->shader_name = "shader.default.world";
    }
    state->light_gizmo_material = light_mat;
  } else {
    String8 err_str = vkr_renderer_get_error_string(light_mat_err);
    log_warn("World view: light gizmo material creation failed: %s",
             string8_cstr(&err_str));
  }

  if (state->light_gizmo_material.id != 0) {
    vkr_view_world_acquire_light_gizmo_states(
        rf, state->pipeline, state->light_gizmo_instance_states, "onscreen");
    vkr_view_world_acquire_light_gizmo_states(
        rf, state->pipeline_offscreen,
        state->light_gizmo_instance_states_offscreen, "offscreen");
  }

  state->text_slots = array_create_VkrViewWorldTextSlot(
      &rf->allocator, VKR_VIEW_WORLD_MAX_TEXTS);
  MemZero(state->text_slots.data,
          sizeof(VkrViewWorldTextSlot) * (uint64_t)state->text_slots.length);

  log_debug("World view initialized.");

  return true_v;
}

vkr_internal void vkr_view_world_on_attach(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  vkr_layer_context_set_camera(ctx, &rf->globals.view, &rf->globals.projection);
}

vkr_internal void vkr_view_world_on_resize(VkrLayerContext *ctx, uint32_t width,
                                           uint32_t height) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  uint32_t target_width = width;
  uint32_t target_height = height;
  if (state && state->offscreen_enabled && state->offscreen_width > 0 &&
      state->offscreen_height > 0) {
    // Phase 4 hardening: when an explicit offscreen size is set
    // (editor-driven), avoid resizing offscreen targets on window resize
    // events. The editor sends VKR_VIEW_WORLD_DATA_SET_OFFSCREEN_SIZE when the
    // viewport panel changes.
    return;
  }

  if (state && state->offscreen_enabled) {
    vkr_view_world_resize_offscreen_targets(ctx, state, target_width,
                                            target_height);
  }

  vkr_camera_registry_resize_all(&rf->camera_system, target_width,
                                 target_height);
}

vkr_internal void vkr_view_world_request_shadow_data(RendererFrontend *rf,
                                                     VkrViewWorldState *state,
                                                     uint32_t image_index) {
  if (!rf || !state) {
    return;
  }

  MemZero(&state->shadow_frame_data, sizeof(state->shadow_frame_data));
  state->shadow_frame_valid = false_v;

  if (rf->shadow_layer.id == 0 ||
      !vkr_view_system_is_layer_enabled(rf, rf->shadow_layer)) {
    vkr_material_system_set_shadow_map(&rf->material_system, NULL, false_v);
    return;
  }

  VkrLayerMsg_ShadowGetFrameData msg = {
      .h = VKR_LAYER_MSG_HEADER_INIT_WITH_RSP(
          VKR_LAYER_MSG_SHADOW_GET_FRAME_DATA, VkrShadowFrameDataRequest),
      .payload = {.frame_index = image_index},
  };
  VkrLayerRsp_ShadowFrameData rsp = {0};
  uint64_t rsp_size = 0;

  if (vkr_view_system_send_msg(rf, rf->shadow_layer, &msg.h, &rsp, sizeof(rsp),
                               &rsp_size) &&
      rsp_size == sizeof(rsp) &&
      rsp.h.kind == VKR_LAYER_RSP_SHADOW_FRAME_DATA &&
      rsp.h.error == VKR_RENDERER_ERROR_NONE) {
    state->shadow_frame_data = rsp.data;
    state->shadow_frame_valid = true_v;
  }

  if (state->shadow_frame_valid) {
    // Keep depth textures bound even when shadows are disabled so that the
    // world shader's comparison sampling descriptors remain format-compatible.
    vkr_material_system_set_shadow_map(&rf->material_system,
                                       state->shadow_frame_data.shadow_map,
                                       true_v);
  } else {
    vkr_material_system_set_shadow_map(&rf->material_system, NULL, false_v);
  }
}

vkr_internal void
vkr_view_world_apply_shadow_globals(RendererFrontend *rf,
                                    const VkrViewWorldState *state) {
  if (!rf || !state) {
    return;
  }

  uint32_t shadow_enabled = 0;
  uint32_t cascade_count = 0;
  Vec4 shadow_map_inv_size[2] = {vec4_zero(), vec4_zero()};
  float32_t shadow_pcf_radius = 0.0f;
  float32_t shadow_bias = 0.0f;
  float32_t shadow_normal_bias = 0.0f;
  float32_t shadow_slope_bias = 0.0f;
  float32_t shadow_bias_texel_scale = 0.0f;
  float32_t shadow_slope_bias_texel_scale = 0.0f;
  float32_t shadow_distance_fade_range = 0.0f;
  float32_t shadow_cascade_blend_range = 0.0f;
  uint32_t shadow_debug_cascades = 0;
  uint32_t shadow_debug_mode = 0;
  Vec4 shadow_split_far[2] = {vec4_zero(), vec4_zero()};
  Vec4 shadow_world_units_per_texel[2] = {vec4_zero(), vec4_zero()};
  Mat4 shadow_view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    shadow_view_projection[i] = mat4_identity();
  }

  if (state->shadow_frame_valid) {
    const VkrShadowFrameData *data = &state->shadow_frame_data;
    shadow_enabled = data->enabled ? 1u : 0u;
    cascade_count = data->cascade_count;
    for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
      uint32_t vec_index = i / 4;
      uint32_t lane = i % 4;
      float32_t inv = data->shadow_map_inv_size[i];
      float32_t split = data->split_far[i];
      float32_t wupt = data->world_units_per_texel[i];

      if (lane == 0) {
        shadow_map_inv_size[vec_index].x = inv;
        shadow_split_far[vec_index].x = split;
        shadow_world_units_per_texel[vec_index].x = wupt;
      } else if (lane == 1) {
        shadow_map_inv_size[vec_index].y = inv;
        shadow_split_far[vec_index].y = split;
        shadow_world_units_per_texel[vec_index].y = wupt;
      } else if (lane == 2) {
        shadow_map_inv_size[vec_index].z = inv;
        shadow_split_far[vec_index].z = split;
        shadow_world_units_per_texel[vec_index].z = wupt;
      } else {
        shadow_map_inv_size[vec_index].w = inv;
        shadow_split_far[vec_index].w = split;
        shadow_world_units_per_texel[vec_index].w = wupt;
      }
    }
    shadow_pcf_radius = data->pcf_radius;
    shadow_bias = data->shadow_bias;
    shadow_normal_bias = data->normal_bias;
    shadow_slope_bias = data->shadow_slope_bias;
    shadow_bias_texel_scale = data->shadow_bias_texel_scale;
    shadow_slope_bias_texel_scale = data->shadow_slope_bias_texel_scale;
    shadow_distance_fade_range = data->shadow_distance_fade_range;
    shadow_cascade_blend_range = data->cascade_blend_range;
    shadow_debug_cascades = data->debug_show_cascades ? 1u : 0u;
    for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
      shadow_view_projection[i] = data->view_projection[i];
    }
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_enabled",
                                &shadow_enabled);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_cascade_count",
                                &cascade_count);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_map_inv_size",
                                shadow_map_inv_size);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_pcf_radius",
                                &shadow_pcf_radius);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_split_far",
                                shadow_split_far);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_world_units_per_texel",
                                shadow_world_units_per_texel);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_bias",
                                &shadow_bias);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_normal_bias",
                                &shadow_normal_bias);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_slope_bias",
                                &shadow_slope_bias);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_bias_texel_scale",
                                &shadow_bias_texel_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_slope_bias_texel_scale",
                                &shadow_slope_bias_texel_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_distance_fade_range",
                                &shadow_distance_fade_range);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_cascade_blend_range",
                                &shadow_cascade_blend_range);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_debug_cascades",
                                &shadow_debug_cascades);
  shadow_debug_mode = rf->shadow_debug_mode;
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_debug_mode",
                                &shadow_debug_mode);

  Vec4 screen_params = vec4_zero();
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;

  // Prefer the renderer's last known size (typically swapchain pixel size on
  // platforms with a backing scale factor), but fall back to the window size
  // when unavailable.
  if (rf->last_window_width > 0 && rf->last_window_height > 0) {
    viewport_width = rf->last_window_width;
    viewport_height = rf->last_window_height;
  } else if (rf->window && rf->window->width > 0 && rf->window->height > 0) {
    viewport_width = rf->window->width;
    viewport_height = rf->window->height;
  }

  if (viewport_width > 0 && viewport_height > 0) {
    screen_params.x = 1.0f / (float32_t)viewport_width;
    screen_params.y = 1.0f / (float32_t)viewport_height;
    screen_params.z = (float32_t)viewport_width;
    screen_params.w = (float32_t)viewport_height;
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "screen_params",
                                &screen_params);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_view_projection",
                                shadow_view_projection);
}

/**
 * @brief Resolves the pipeline handle for the domain, honoring offscreen
 * overrides when enabled.
 */
vkr_internal VkrPipelineHandle vkr_view_world_resolve_pipeline(
    const VkrViewWorldState *state, VkrPipelineDomain domain) {
  VkrPipelineHandle resolved = (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT)
                                   ? state->transparent_pipeline
                                   : state->pipeline;
  if (state->offscreen_enabled) {
    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT &&
        state->transparent_pipeline_offscreen.id != 0) {
      resolved = state->transparent_pipeline_offscreen;
    } else if (domain != VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT &&
               state->pipeline_offscreen.id != 0) {
      resolved = state->pipeline_offscreen;
    }
  }
  return resolved;
}

// todo: look into optimizing this (vkr_material_system_apply_instance is the
// culprit)
vkr_internal bool8_t vkr_view_world_bind_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t mesh_index,
    uint32_t submesh_index, VkrPipelineDomain domain,
    VkrPipelineHandle *globals_pipeline, VkrSubMesh **out_submesh) {
  VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, mesh_index);
  if (!mesh || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
    return false_v;
  if (!mesh->visible)
    return false_v;

  VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(&rf->mesh_manager,
                                                     mesh_index, submesh_index);
  if (!submesh)
    return false_v;

  VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
      &rf->geometry_system, submesh->geometry);
  if (!geometry)
    return false_v;

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &rf->material_system, submesh->material);
  if (!material && rf->material_system.default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->material_system.default_material);
  }
  const char *material_shader =
      (material && material->shader_name && material->shader_name[0] != '\0')
          ? material->shader_name
          : "shader.default.world";
  if (!vkr_shader_system_use(&rf->shader_system, material_shader)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.world");
  }

  VkrPipelineHandle resolved = vkr_view_world_resolve_pipeline(state, domain);

  VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_refresh_pipeline(&rf->mesh_manager, mesh_index,
                                         submesh_index, resolved,
                                         &refresh_err)) {
    String8 err_str = vkr_renderer_get_error_string(refresh_err);
    log_error("Mesh %u submesh %u failed to refresh pipeline: %s", mesh_index,
              submesh_index, string8_cstr(&err_str));
    return false_v;
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

  if (!globals_pipeline || globals_pipeline->id != resolved.id ||
      globals_pipeline->generation != resolved.generation) {
    vkr_lighting_system_apply_uniforms(&rf->lighting_system);
    vkr_view_world_apply_shadow_globals(rf, state);
    vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                     VKR_PIPELINE_DOMAIN_WORLD);
    if (globals_pipeline) {
      *globals_pipeline = resolved;
    }
  }

  if (material) {
    vkr_shader_system_bind_instance(&rf->shader_system,
                                    submesh->instance_state.id);

    bool8_t should_apply_instance =
        (submesh->last_render_frame != rf->frame_number);
    if (should_apply_instance) {
      vkr_material_system_apply_instance(&rf->material_system, material,
                                         VKR_PIPELINE_DOMAIN_WORLD);
      submesh->last_render_frame = rf->frame_number;
    } else {
      vkr_shader_system_apply_instance(&rf->shader_system);
    }
  }

  if (out_submesh) {
    *out_submesh = submesh;
  }
  return true_v;
}

vkr_internal void vkr_view_world_render_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t mesh_index,
    uint32_t submesh_index, VkrPipelineDomain domain, uint32_t instance_count,
    uint32_t first_instance, VkrPipelineHandle *globals_pipeline) {
  VkrSubMesh *submesh = NULL;
  if (!vkr_view_world_bind_submesh(rf, state, mesh_index, submesh_index, domain,
                                   globals_pipeline, &submesh)) {
    return;
  }

  const VkrIndexBuffer *index_buffer = NULL;
  VkrViewWorldDrawRange range = vkr_view_world_resolve_draw_range(
      rf, submesh, domain == VKR_PIPELINE_DOMAIN_WORLD);
  uint32_t index_count = range.index_count;
  uint32_t first_index = range.first_index;
  int32_t vertex_offset = range.vertex_offset;
  index_buffer = range.index_buffer;

  if (index_buffer) {
    vkr_geometry_system_render_instanced_range_with_index_buffer(
        rf, &rf->geometry_system, submesh->geometry, index_buffer, index_count,
        first_index, vertex_offset, instance_count, first_instance);
  } else {
    vkr_geometry_system_render_instanced_range(
        rf, &rf->geometry_system, submesh->geometry, index_count, first_index,
        vertex_offset, instance_count, first_instance);
  }
}

// =============================================================================
// Instance rendering (VkrMeshInstance + VkrMeshAsset)
// =============================================================================

vkr_internal VkrViewWorldDrawRange vkr_view_world_resolve_instance_draw_range(
    RendererFrontend *rf, const VkrMeshAssetSubmesh *submesh,
    bool8_t allow_opaque) {
  VkrViewWorldDrawRange range = {
      .index_buffer = NULL,
      .index_count = submesh->index_count,
      .first_index = submesh->first_index,
      .vertex_offset = submesh->vertex_offset,
      .uses_opaque_indices = false_v,
  };

  if (!allow_opaque || submesh->opaque_index_count == 0) {
    return range;
  }

  VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
      &rf->geometry_system, submesh->geometry);
  if (!geometry || !geometry->opaque_index_buffer.handle) {
    return range;
  }

  range.index_buffer = &geometry->opaque_index_buffer;
  range.index_count = submesh->opaque_index_count;
  range.first_index = submesh->opaque_first_index;
  range.vertex_offset = submesh->opaque_vertex_offset;
  range.uses_opaque_indices = true_v;
  return range;
}

vkr_internal VkrViewWorldDrawInfo vkr_view_world_get_draw_info(
    RendererFrontend *rf, const VkrDrawCommand *cmd, bool8_t allow_opaque) {
  VkrViewWorldDrawInfo info = {0};
  if (!rf || !cmd) {
    return info;
  }

  if (cmd->is_instance) {
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_index(
        &rf->mesh_manager, cmd->mesh_index);
    if (!instance || instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      return info;
    }
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
    if (!asset || cmd->submesh_index >= asset->submeshes.length) {
      return info;
    }
    VkrMeshAssetSubmesh *submesh =
        array_get_VkrMeshAssetSubmesh(&asset->submeshes, cmd->submesh_index);
    if (!submesh) {
      return info;
    }
    info.geometry = submesh->geometry;
    info.range = vkr_view_world_resolve_instance_draw_range(rf, submesh,
                                                            allow_opaque);
    info.valid = true_v;
    return info;
  }

  VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(
      &rf->mesh_manager, cmd->mesh_index, cmd->submesh_index);
  if (!submesh) {
    return info;
  }
  info.geometry = submesh->geometry;
  info.range = vkr_view_world_resolve_draw_range(rf, submesh, allow_opaque);
  info.valid = true_v;
  return info;
}

vkr_internal bool8_t vkr_view_world_bind_instance_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t instance_index,
    uint32_t submesh_index, VkrPipelineDomain domain,
    VkrPipelineHandle *globals_pipeline, VkrMeshAssetSubmesh **out_asset_submesh,
    VkrMeshSubmeshInstanceState **out_instance_state) {
  VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance_by_index(&rf->mesh_manager, instance_index);
  if (!instance || instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }
  if (!instance->visible) {
    return false_v;
  }

  VkrMeshAsset *asset =
      vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
  if (!asset || submesh_index >= asset->submeshes.length) {
    return false_v;
  }

  VkrMeshAssetSubmesh *submesh =
      array_get_VkrMeshAssetSubmesh(&asset->submeshes, submesh_index);
  if (!submesh) {
    return false_v;
  }

  if (submesh_index >= instance->submesh_state.length) {
    return false_v;
  }
  VkrMeshSubmeshInstanceState *inst_state =
      array_get_VkrMeshSubmeshInstanceState(&instance->submesh_state,
                                            submesh_index);
  if (!inst_state) {
    return false_v;
  }

  VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
      &rf->geometry_system, submesh->geometry);
  if (!geometry) {
    return false_v;
  }

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &rf->material_system, submesh->material);
  if (!material && rf->material_system.default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->material_system.default_material);
  }
  const char *material_shader =
      (material && material->shader_name && material->shader_name[0] != '\0')
          ? material->shader_name
          : "shader.default.world";
  if (!vkr_shader_system_use(&rf->shader_system, material_shader)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.world");
  }

  VkrPipelineHandle resolved = vkr_view_world_resolve_pipeline(state, domain);

  // Handle id is 1-indexed (slot 0 = id 1).
  VkrMeshInstanceHandle inst_handle = {
      .id = instance_index + 1,
      .generation = instance->generation,
  };

  VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_instance_refresh_pipeline(
          &rf->mesh_manager, inst_handle, submesh_index, resolved,
          &refresh_err)) {
    String8 err_str = vkr_renderer_get_error_string(refresh_err);
    log_error("Instance %u submesh %u failed to refresh pipeline: %s",
              instance_index, submesh_index, string8_cstr(&err_str));
    return false_v;
  }

  // Re-fetch instance state after refresh (it may have been updated)
  inst_state = array_get_VkrMeshSubmeshInstanceState(&instance->submesh_state,
                                                     submesh_index);

  rf->draw_state.instance_state = inst_state->instance_state;

  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_pipeline.id != resolved.id ||
      current_pipeline.generation != resolved.generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, resolved,
                                        &bind_err);
  }

  if (!globals_pipeline || globals_pipeline->id != resolved.id ||
      globals_pipeline->generation != resolved.generation) {
    vkr_lighting_system_apply_uniforms(&rf->lighting_system);
    vkr_view_world_apply_shadow_globals(rf, state);
    vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                     VKR_PIPELINE_DOMAIN_WORLD);
    if (globals_pipeline) {
      *globals_pipeline = resolved;
    }
  }

  if (material) {
    vkr_shader_system_bind_instance(&rf->shader_system,
                                    inst_state->instance_state.id);

    bool8_t should_apply_instance =
        (inst_state->last_render_frame != rf->frame_number);
    if (should_apply_instance) {
      vkr_material_system_apply_instance(&rf->material_system, material,
                                         VKR_PIPELINE_DOMAIN_WORLD);
      inst_state->last_render_frame = rf->frame_number;
    } else {
      vkr_shader_system_apply_instance(&rf->shader_system);
    }
  }

  if (out_asset_submesh) {
    *out_asset_submesh = submesh;
  }
  if (out_instance_state) {
    *out_instance_state = inst_state;
  }
  return true_v;
}

vkr_internal void vkr_view_world_render_instance_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t instance_index,
    uint32_t submesh_index, VkrPipelineDomain domain, uint32_t instance_count,
    uint32_t first_instance, VkrPipelineHandle *globals_pipeline) {
  VkrMeshAssetSubmesh *submesh = NULL;
  VkrMeshSubmeshInstanceState *inst_state = NULL;
  if (!vkr_view_world_bind_instance_submesh(rf, state, instance_index,
                                            submesh_index, domain,
                                            globals_pipeline, &submesh,
                                            &inst_state)) {
    return;
  }

  const VkrIndexBuffer *index_buffer = NULL;
  VkrViewWorldDrawRange range = vkr_view_world_resolve_instance_draw_range(
      rf, submesh, domain == VKR_PIPELINE_DOMAIN_WORLD);
  uint32_t index_count = range.index_count;
  uint32_t first_index = range.first_index;
  int32_t vertex_offset = range.vertex_offset;
  index_buffer = range.index_buffer;

  if (index_buffer) {
    vkr_geometry_system_render_instanced_range_with_index_buffer(
        rf, &rf->geometry_system, submesh->geometry, index_buffer, index_count,
        first_index, vertex_offset, instance_count, first_instance);
  } else {
    vkr_geometry_system_render_instanced_range(
        rf, &rf->geometry_system, submesh->geometry, index_count, first_index,
        vertex_offset, instance_count, first_instance);
  }
}

vkr_internal void vkr_view_world_on_render(VkrLayerContext *ctx,
                                           const VkrLayerRenderInfo *info) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(info != NULL, "Layer render info is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    log_error("World view state is NULL");
    return;
  }

  // Sync lighting from active scene (if set)
  if (rf->active_scene) {
    vkr_lighting_system_sync_from_scene(&rf->lighting_system, rf->active_scene);
  }

  vkr_view_world_request_shadow_data(rf, state, info->image_index);

  uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  VkrPipelineHandle globals_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  VkrDrawBatcher *batcher = &state->draw_batcher;
  vkr_draw_batcher_reset(batcher);

  VkrIndirectDrawSystem *indirect_system = &rf->indirect_draw_system;
  bool8_t use_mdi = indirect_system && indirect_system->initialized &&
                    indirect_system->enabled &&
                    rf->backend.draw_indexed_indirect != NULL &&
                    rf->supports_multi_draw_indirect &&
                    rf->supports_draw_indirect_first_instance;
  bool8_t mdi_available = use_mdi;
  bool8_t mdi_warned = false_v;
  VkrPipelineHandle opaque_pipeline =
      vkr_view_world_resolve_pipeline(state, VKR_PIPELINE_DOMAIN_WORLD);
  VkrPipelineHandle transparent_pipeline = vkr_view_world_resolve_pipeline(
      state, VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT);

  Vec3 camera_pos = rf->globals.view_position;

  // Build view frustum for culling
  VkrFrustum frustum = vkr_frustum_from_view_projection(rf->globals.view,
                                                        rf->globals.projection);

  // Frustum culling stats
  uint32_t meshes_total = 0;
  uint32_t meshes_culled = 0;

  // first pass: collect opaque and transparent submeshes
  for (uint32_t i = 0; i < mesh_count; i++) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager, i,
                                                &mesh_slot);
    if (!mesh)
      continue;
    if (!mesh->visible)
      continue;

    meshes_total++;

    // Frustum culling: skip mesh if outside view frustum
    if (mesh->bounds_valid) {
      if (!vkr_frustum_test_sphere(&frustum, mesh->bounds_world_center,
                                   mesh->bounds_world_radius)) {
        meshes_culled++;
        continue; // Culled - skip all submeshes
      }
    }

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0)
      continue;

    Mat4 mesh_world_pos = mesh->model;
    uint32_t object_id =
        mesh->render_id
            ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, mesh->render_id)
            : 0;
    Vec3 mesh_pos =
        vec3_new(mesh_world_pos.elements[12], mesh_world_pos.elements[13],
                 mesh_world_pos.elements[14]);
    float32_t mesh_distance = vkr_abs_f32(vec3_distance(mesh_pos, camera_pos));

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot,
                                       submesh_index);
      if (!submesh)
        continue;

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      if (!material && rf->material_system.default_material.id != 0) {
        material = vkr_material_system_get_by_handle(
            &rf->material_system, rf->material_system.default_material);
      }

      if (vkr_submesh_uses_cutout(rf, material)) {
        VkrDrawCommand cmd = {
            .key = {.pipeline_id = transparent_pipeline.id,
                    .material_id = material ? material->id : 0,
                    .geometry_id = submesh->geometry.id,
                    .range_id = submesh->range_id},
            .mesh_index = mesh_slot,
            .submesh_index = submesh_index,
            .model = mesh_world_pos,
            .object_id = object_id,
            .camera_distance = mesh_distance,
            .is_instance = false_v};
        vkr_draw_batcher_add_transparent(batcher, &cmd);
      } else {
        uint32_t range_id = use_mdi ? 0 : submesh->range_id;
        VkrDrawCommand cmd = {
            .key = {.pipeline_id = opaque_pipeline.id,
                    .material_id = material ? material->id : 0,
                    .geometry_id = submesh->geometry.id,
                    .range_id = range_id},
            .mesh_index = mesh_slot,
            .submesh_index = submesh_index,
            .model = mesh_world_pos,
            .object_id = object_id,
            .camera_distance = 0.0f,
            .is_instance = false_v};
        vkr_draw_batcher_add_opaque(batcher, &cmd);
      }
    }
  }

  // Collect mesh instances
  uint32_t instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);
  for (uint32_t inst_i = 0; inst_i < instance_count; inst_i++) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *inst =
        vkr_mesh_manager_get_instance_by_live_index(&rf->mesh_manager, inst_i,
                                                    &instance_slot);
    if (!inst) {
      continue;
    }
    if (!inst->visible)
      continue;
    if (inst->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;

    meshes_total++;

    // Frustum culling
    if (inst->bounds_valid) {
      if (!vkr_frustum_test_sphere(&frustum, inst->bounds_world_center,
                                   inst->bounds_world_radius)) {
        meshes_culled++;
        continue;
      }
    }

    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, inst->asset);
    if (!asset)
      continue;

    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    if (submesh_count == 0)
      continue;

    Mat4 inst_world_pos = inst->model;
    uint32_t object_id =
        inst->render_id
            ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, inst->render_id)
            : 0;
    Vec3 inst_pos = vec3_new(inst_world_pos.elements[12],
                             inst_world_pos.elements[13],
                             inst_world_pos.elements[14]);
    float32_t inst_distance = vkr_abs_f32(vec3_distance(inst_pos, camera_pos));

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, submesh_index);
      if (!submesh)
        continue;

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      if (!material && rf->material_system.default_material.id != 0) {
        material = vkr_material_system_get_by_handle(
            &rf->material_system, rf->material_system.default_material);
      }

      if (vkr_submesh_uses_cutout(rf, material)) {
        VkrDrawCommand cmd = {
            .key = {.pipeline_id = transparent_pipeline.id,
                    .material_id = material ? material->id : 0,
                    .geometry_id = submesh->geometry.id,
                    .range_id = submesh->range_id},
            .mesh_index = instance_slot,
            .submesh_index = submesh_index,
            .model = inst_world_pos,
            .object_id = object_id,
            .camera_distance = inst_distance,
            .is_instance = true_v};
        vkr_draw_batcher_add_transparent(batcher, &cmd);
      } else {
        uint32_t range_id = use_mdi ? 0 : submesh->range_id;
        VkrDrawCommand cmd = {
            .key = {.pipeline_id = opaque_pipeline.id,
                    .material_id = material ? material->id : 0,
                    .geometry_id = submesh->geometry.id,
                    .range_id = range_id},
            .mesh_index = instance_slot,
            .submesh_index = submesh_index,
            .model = inst_world_pos,
            .object_id = object_id,
            .camera_distance = 0.0f,
            .is_instance = true_v};
        vkr_draw_batcher_add_opaque(batcher, &cmd);
      }
    }
  }

  vkr_draw_batcher_finalize(batcher);

  uint32_t opaque_batch_count = vkr_draw_batcher_opaque_batch_count(batcher);
  uint32_t transparent_draw_count =
      (uint32_t)batcher->transparent_commands.length;
  uint32_t total_opaque_batch_size = 0;
  uint32_t max_opaque_batch_size = 0;
  for (uint32_t b = 0; b < opaque_batch_count; ++b) {
    uint32_t batch_size = batcher->opaque_batches.data[b].command_count;
    total_opaque_batch_size += batch_size;
    if (batch_size > max_opaque_batch_size) {
      max_opaque_batch_size = batch_size;
    }
  }
  rf->frame_metrics.world.draws_collected = batcher->total_draws_collected;
  rf->frame_metrics.world.opaque_draws =
      (uint32_t)batcher->opaque_commands.length;
  rf->frame_metrics.world.transparent_draws = transparent_draw_count;
  rf->frame_metrics.world.opaque_batches = opaque_batch_count;
  rf->frame_metrics.world.draws_issued =
      opaque_batch_count + transparent_draw_count;
  rf->frame_metrics.world.batches_created = batcher->batches_created;
  rf->frame_metrics.world.draws_merged = batcher->draws_merged;
  rf->frame_metrics.world.max_batch_size = max_opaque_batch_size;
  rf->frame_metrics.world.avg_batch_size =
      (opaque_batch_count > 0)
          ? ((float32_t)total_opaque_batch_size / (float32_t)opaque_batch_count)
          : 0.0f;

  VkrInstanceBufferPool *instance_pool = &rf->instance_buffer_pool;
  if (!instance_pool->initialized) {
    log_error("Instance buffer pool not initialized; skipping world draw");
    return;
  }

  for (uint32_t b = 0; b < opaque_batch_count; ++b) {
    VkrDrawBatch *batch = &batcher->opaque_batches.data[b];
    VkrInstanceDataGPU *instances = NULL;
    uint32_t base_instance = 0;
    if (!vkr_instance_buffer_alloc(instance_pool, batch->command_count,
                                   &base_instance, &instances)) {
      log_error("World view: instance buffer allocation failed for batch");
      continue;
    }
    batch->first_instance = base_instance;
    for (uint32_t c = 0; c < batch->command_count; ++c) {
      const VkrDrawCommand *cmd =
          &batcher->opaque_commands.data[batch->first_command + c];
      instances[c] = (VkrInstanceDataGPU){
          .model = cmd->model,
          .object_id = cmd->object_id,
          .material_index = 0,
          .flags = 0,
          ._padding = 0,
      };
    }
  }

  vkr_instance_buffer_flush_current(instance_pool);

  for (uint32_t b = 0; b < opaque_batch_count; ++b) {
    VkrDrawBatch *batch = &batcher->opaque_batches.data[b];
    const VkrDrawCommand *cmd =
        &batcher->opaque_commands.data[batch->first_command];
    if (!mdi_available) {
      if (!use_mdi) {
        if (cmd->is_instance) {
          vkr_view_world_render_instance_submesh(
              rf, state, cmd->mesh_index, cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, batch->command_count,
              batch->first_instance, &globals_pipeline);
        } else {
          vkr_view_world_render_submesh(
              rf, state, cmd->mesh_index, cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, batch->command_count,
              batch->first_instance, &globals_pipeline);
        }
      } else {
        for (uint32_t c = 0; c < batch->command_count; ++c) {
          const VkrDrawCommand *fallback_cmd =
              &batcher->opaque_commands.data[batch->first_command + c];
          if (fallback_cmd->is_instance) {
            vkr_view_world_render_instance_submesh(
                rf, state, fallback_cmd->mesh_index,
                fallback_cmd->submesh_index, VKR_PIPELINE_DOMAIN_WORLD, 1,
                batch->first_instance + c, &globals_pipeline);
          } else {
            vkr_view_world_render_submesh(
                rf, state, fallback_cmd->mesh_index,
                fallback_cmd->submesh_index, VKR_PIPELINE_DOMAIN_WORLD, 1,
                batch->first_instance + c, &globals_pipeline);
          }
        }
      }
      continue;
    }

    VkrGeometryHandle batch_geometry = VKR_GEOMETRY_HANDLE_INVALID;
    VkrViewWorldDrawRange batch_range = {0};
    if (cmd->is_instance) {
      VkrMeshAssetSubmesh *batch_submesh = NULL;
      if (!vkr_view_world_bind_instance_submesh(
              rf, state, cmd->mesh_index, cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, &globals_pipeline, &batch_submesh,
              NULL)) {
        continue;
      }
      batch_geometry = batch_submesh->geometry;
      batch_range =
          vkr_view_world_resolve_instance_draw_range(rf, batch_submesh, true_v);
    } else {
      VkrSubMesh *batch_submesh = NULL;
      if (!vkr_view_world_bind_submesh(
              rf, state, cmd->mesh_index, cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, &globals_pipeline, &batch_submesh)) {
        continue;
      }
      batch_geometry = batch_submesh->geometry;
      batch_range = vkr_view_world_resolve_draw_range(rf, batch_submesh, true_v);
    }

    const VkrIndexBuffer *opaque_index_buffer = batch_range.index_buffer;
    bool8_t use_opaque_indices = batch_range.uses_opaque_indices;

    uint32_t command_index = 0;
    while (command_index < batch->command_count) {
      uint32_t remaining = vkr_indirect_draw_remaining(indirect_system);
      if (remaining == 0) {
        if (!mdi_warned) {
          log_warn("World view: indirect draw buffer full, falling back");
          mdi_warned = true_v;
        }
        mdi_available = false_v;
        break;
      }

      uint32_t pending = batch->command_count - command_index;
      uint32_t chunk = remaining < pending ? remaining : pending;

      uint32_t base_draw = 0;
      VkrIndirectDrawCommand *draw_cmds = NULL;
      if (!vkr_indirect_draw_alloc(indirect_system, chunk, &base_draw,
                                   &draw_cmds)) {
        if (!mdi_warned) {
          log_warn("World view: indirect draw alloc failed, falling back");
          mdi_warned = true_v;
        }
        mdi_available = false_v;
        break;
      }

      bool8_t commands_valid = true_v;
      for (uint32_t c = 0; c < chunk; ++c) {
        const VkrDrawCommand *range_cmd = &batcher->opaque_commands
                                               .data[batch->first_command +
                                                     command_index + c];
        VkrViewWorldDrawInfo range_info =
            vkr_view_world_get_draw_info(rf, range_cmd, use_opaque_indices);
        if (!range_info.valid ||
            range_info.geometry.id != batch_geometry.id) {
          commands_valid = false_v;
          break;
        }
        if (use_opaque_indices && !range_info.range.uses_opaque_indices) {
          commands_valid = false_v;
          break;
        }

        draw_cmds[c] = (VkrIndirectDrawCommand){
            .index_count = range_info.range.index_count,
            .instance_count = 1,
            .first_index = range_info.range.first_index,
            .vertex_offset = range_info.range.vertex_offset,
            .first_instance = batch->first_instance + command_index + c,
        };
      }

      if (!commands_valid) {
        if (!mdi_warned) {
          log_warn("World view: invalid submesh in MDI batch, falling back");
          mdi_warned = true_v;
        }
        mdi_available = false_v;
        break;
      }

      vkr_indirect_draw_flush_range(indirect_system, base_draw, chunk);
      uint64_t offset_bytes =
          (uint64_t)base_draw * sizeof(VkrIndirectDrawCommand);
      if (use_opaque_indices && opaque_index_buffer) {
        vkr_geometry_system_render_indirect_with_index_buffer(
            rf, &rf->geometry_system, batch_geometry, opaque_index_buffer,
            vkr_indirect_draw_get_current(indirect_system), offset_bytes, chunk,
            sizeof(VkrIndirectDrawCommand));
      } else {
        vkr_geometry_system_render_indirect(
            rf, &rf->geometry_system, batch_geometry,
            vkr_indirect_draw_get_current(indirect_system), offset_bytes, chunk,
            sizeof(VkrIndirectDrawCommand));
      }
      rf->frame_metrics.world.indirect_draws_issued += 1;
      command_index += chunk;
    }

    if (command_index < batch->command_count) {
      for (uint32_t c = command_index; c < batch->command_count; ++c) {
        const VkrDrawCommand *fallback_cmd =
            &batcher->opaque_commands.data[batch->first_command + c];
        if (fallback_cmd->is_instance) {
          vkr_view_world_render_instance_submesh(
              rf, state, fallback_cmd->mesh_index, fallback_cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, 1, batch->first_instance + c,
              &globals_pipeline);
        } else {
          vkr_view_world_render_submesh(
              rf, state, fallback_cmd->mesh_index, fallback_cmd->submesh_index,
              VKR_PIPELINE_DOMAIN_WORLD, 1, batch->first_instance + c,
              &globals_pipeline);
        }
      }
    }
  }

  for (uint64_t t = 0; t < batcher->transparent_commands.length; ++t) {
    const VkrDrawCommand *cmd = &batcher->transparent_commands.data[t];
    VkrInstanceDataGPU *instance = NULL;
    uint32_t base_instance = 0;
    if (!vkr_instance_buffer_alloc(instance_pool, 1, &base_instance,
                                   &instance)) {
      log_error(
          "World view: instance buffer allocation failed for transparent");
      continue;
    }
    instance[0] = (VkrInstanceDataGPU){
        .model = cmd->model,
        .object_id = cmd->object_id,
        .material_index = 0,
        .flags = 0,
        ._padding = 0,
    };
    vkr_instance_buffer_flush_range(instance_pool, base_instance, 1);

    if (cmd->is_instance) {
      vkr_view_world_render_instance_submesh(
          rf, state, cmd->mesh_index, cmd->submesh_index,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, 1, base_instance,
          &globals_pipeline);
    } else {
      vkr_view_world_render_submesh(rf, state, cmd->mesh_index,
                                    cmd->submesh_index,
                                    VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, 1,
                                    base_instance, &globals_pipeline);
    }
  }

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewWorldTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_text_3d_draw(&slot->text);
  }

  vkr_view_world_render_point_light_gizmos(rf, state);

  if (rf->gizmo_system.initialized) {
    VkrCamera *camera = vkr_camera_registry_get_by_handle(&rf->camera_system,
                                                          rf->active_camera);
    uint32_t viewport_height = vkr_layer_context_get_height(ctx);
    if (!state->offscreen_enabled ||
        state->overlay_pipeline_offscreen.id != 0) {
      VkrPipelineHandle gizmo_pipeline = state->overlay_pipeline;
      if (state->offscreen_enabled &&
          state->overlay_pipeline_offscreen.id != 0) {
        gizmo_pipeline = state->overlay_pipeline_offscreen;
      }
      vkr_gizmo_system_render(&rf->gizmo_system, rf, camera, viewport_height,
                              gizmo_pipeline);
    }
  }
}

vkr_internal bool8_t vkr_view_world_on_update(VkrLayerContext *ctx,
                                              const VkrLayerUpdateInfo *info) {
  if (!ctx || !info || !info->input_state) {
    return false_v;
  }

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf || !rf->window) {
    return false_v;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  VkrCamera *camera = NULL;
  if (info->camera_system) {
    camera = vkr_camera_registry_get_by_handle(info->camera_system,
                                               info->active_camera);
  }
  if (!camera) {
    log_error("World view update: active camera invalid");
    return false_v;
  }

  VkrCameraController *controller = &rf->camera_controller;
  controller->camera = camera;

  InputState *input_state = info->input_state;

  if (input_is_key_down(input_state, KEY_TAB) &&
      input_was_key_up(input_state, KEY_TAB)) {
    bool8_t should_capture = !vkr_window_is_mouse_captured(rf->window);
    vkr_window_set_mouse_capture(rf->window, should_capture);
  }

  if (input_is_button_down(input_state, BUTTON_GAMEPAD_A) &&
      input_was_button_up(input_state, BUTTON_GAMEPAD_A)) {
    bool8_t should_capture = !vkr_window_is_mouse_captured(rf->window);
    vkr_window_set_mouse_capture(rf->window, should_capture);
    state->use_gamepad = !state->use_gamepad;
  }

  if (!vkr_window_is_mouse_captured(rf->window)) {
    return false_v;
  }

  if (!state->wheel_initialized) {
    int8_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    state->previous_wheel_delta = wheel_delta;
    state->wheel_initialized = true_v;
  }

  bool8_t should_rotate = false_v;
  float32_t yaw_input = 0.0f;
  float32_t pitch_input = 0.0f;

  if (!state->use_gamepad) {
    if (input_is_key_down(input_state, KEY_W)) {
      vkr_camera_controller_move_forward(controller, 1.0f);
    }
    if (input_is_key_down(input_state, KEY_S)) {
      vkr_camera_controller_move_forward(controller, -1.0f);
    }
    if (input_is_key_down(input_state, KEY_D)) {
      vkr_camera_controller_move_right(controller, 1.0f);
    }
    if (input_is_key_down(input_state, KEY_A)) {
      vkr_camera_controller_move_right(controller, -1.0f);
    }

    int8_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    if (wheel_delta != state->previous_wheel_delta) {
      float32_t zoom_delta = -(float32_t)wheel_delta * 0.1f;
      vkr_camera_zoom(camera, zoom_delta);
      state->previous_wheel_delta = wheel_delta;
    }

    int32_t x = 0;
    int32_t y = 0;
    input_get_mouse_position(input_state, &x, &y);

    int32_t last_x = 0;
    int32_t last_y = 0;
    input_get_previous_mouse_position(input_state, &last_x, &last_y);

    if (!((x == last_x && y == last_y) || (x == 0 && y == 0) ||
          (last_x == 0 && last_y == 0))) {
      float32_t x_offset = (float32_t)(x - last_x);
      float32_t y_offset = (float32_t)(last_y - y);

      float32_t max_mouse_delta = VKR_MAX_MOUSE_DELTA / camera->sensitivity;
      x_offset = vkr_clamp_f32(x_offset, -max_mouse_delta, max_mouse_delta);
      y_offset = vkr_clamp_f32(y_offset, -max_mouse_delta, max_mouse_delta);

      yaw_input = -x_offset;
      pitch_input = y_offset;
      should_rotate = true_v;
    }
  } else {
    float right_x = 0.0f;
    float right_y = 0.0f;
    input_get_right_stick(input_state, &right_x, &right_y);

    float32_t movement_deadzone = VKR_GAMEPAD_MOVEMENT_DEADZONE;
    if (vkr_abs_f32(right_y) > movement_deadzone) {
      vkr_camera_controller_move_forward(controller, -right_y);
    }
    if (vkr_abs_f32(right_x) > movement_deadzone) {
      vkr_camera_controller_move_right(controller, right_x);
    }

    float left_x = 0.0f;
    float left_y = 0.0f;
    input_get_left_stick(input_state, &left_x, &left_y);

    float rotation_deadzone = 0.1f;
    if (vkr_abs_f32(left_x) < rotation_deadzone) {
      left_x = 0.0f;
    }
    if (vkr_abs_f32(left_y) < rotation_deadzone) {
      left_y = 0.0f;
    }

    if (left_x != 0.0f || left_y != 0.0f) {
      float32_t x_offset = left_x * VKR_GAMEPAD_ROTATION_SCALE;
      float32_t y_offset = -left_y * VKR_GAMEPAD_ROTATION_SCALE;
      yaw_input = -x_offset;
      pitch_input = y_offset;
      should_rotate = true_v;
    }
  }

  if (should_rotate) {
    vkr_camera_controller_rotate(controller, yaw_input, pitch_input);
  }

  return false_v;
}

vkr_internal void vkr_view_world_on_data_received(VkrLayerContext *ctx,
                                                  const VkrLayerMsgHeader *msg,
                                                  void *out_rsp,
                                                  uint64_t out_rsp_capacity,
                                                  uint64_t *out_rsp_size) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(msg != NULL, "Message is NULL");

  (void)out_rsp;
  (void)out_rsp_capacity;
  (void)out_rsp_size;

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  switch (msg->kind) {
  case VKR_LAYER_MSG_WORLD_TOGGLE_OFFSCREEN: {
    bool8_t next_enabled = state->offscreen_enabled ? false_v : true_v;
    if (!vkr_view_world_set_offscreen_enabled(ctx, state, next_enabled)) {
      log_error("Failed to toggle offscreen rendering");
    }
  } break;
  case VKR_LAYER_MSG_WORLD_SET_OFFSCREEN_SIZE: {
    const VkrViewWorldOffscreenSizeData *payload =
        (const VkrViewWorldOffscreenSizeData *)((const uint8_t *)msg +
                                                sizeof(VkrLayerMsgHeader));
    const uint32_t prev_width = state->offscreen_width;
    const uint32_t prev_height = state->offscreen_height;
    uint32_t target_width = payload->width;
    uint32_t target_height = payload->height;
    const bool8_t requested_size_changed =
        (payload->width > 0 && payload->height > 0)
            ? (payload->width != prev_width || payload->height != prev_height)
            : true_v;

    if (target_width == 0 || target_height == 0) {
      state->offscreen_width = 0;
      state->offscreen_height = 0;
      if (ctx->layer) {
        target_width = ctx->layer->width;
        target_height = ctx->layer->height;
      }
    } else {
      state->offscreen_width = target_width;
      state->offscreen_height = target_height;
    }

    if (state->offscreen_enabled && target_width > 0 && target_height > 0 &&
        requested_size_changed) {
      vkr_view_world_resize_offscreen_targets(ctx, state, target_width,
                                              target_height);
      vkr_camera_registry_resize_all(&rf->camera_system, target_width,
                                     target_height);
    }
  } break;
  case VKR_LAYER_MSG_WORLD_TEXT_CREATE: {
    const VkrViewWorldTextCreateData *payload =
        (const VkrViewWorldTextCreateData *)((const uint8_t *)msg +
                                             sizeof(VkrLayerMsgHeader));
    VkrPipelineHandle text_pipeline = state->text_pipeline;
    if (state->offscreen_enabled && state->text_pipeline_offscreen.id != 0) {
      text_pipeline = state->text_pipeline_offscreen;
    }

    if (text_pipeline.id == 0) {
      log_error("World text pipeline not ready");
      return;
    }

    VkrViewWorldTextSlot *slot = NULL;
    if (!vkr_view_world_ensure_text_slot(state, payload->text_id, &slot)) {
      return;
    }

    if (slot->active) {
      vkr_text_3d_destroy(&slot->text);
      slot->active = false_v;
    }

    VkrText3DConfig config =
        payload->has_config ? payload->config : VKR_TEXT_3D_CONFIG_DEFAULT;
    config.text = payload->content;
    config.pipeline = text_pipeline;

    VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_text_3d_create(&slot->text, rf, &rf->font_system, &rf->allocator,
                            &config, &text_err)) {
      String8 err = vkr_renderer_get_error_string(text_err);
      log_error("Failed to create 3D text: %s", string8_cstr(&err));
      return;
    }

    vkr_text_3d_set_transform(&slot->text, payload->transform);
    slot->active = true_v;
  } break;
  case VKR_LAYER_MSG_WORLD_TEXT_UPDATE: {
    const VkrViewWorldTextUpdateData *payload =
        (const VkrViewWorldTextUpdateData *)((const uint8_t *)msg +
                                             sizeof(VkrLayerMsgHeader));
    VkrViewWorldTextSlot *slot =
        vkr_view_world_get_text_slot(state, payload->text_id);
    if (!slot) {
      log_warn("World text id %u not found for update", payload->text_id);
      return;
    }

    vkr_text_3d_set_text(&slot->text, payload->content);
  } break;
  case VKR_LAYER_MSG_WORLD_TEXT_SET_TRANSFORM: {
    const VkrViewWorldTextTransformData *payload =
        (const VkrViewWorldTextTransformData *)((const uint8_t *)msg +
                                                sizeof(VkrLayerMsgHeader));
    VkrViewWorldTextSlot *slot =
        vkr_view_world_get_text_slot(state, payload->text_id);
    if (!slot) {
      log_warn("World text id %u not found for transform", payload->text_id);
      return;
    }

    vkr_text_3d_set_transform(&slot->text, payload->transform);
  } break;
  case VKR_LAYER_MSG_WORLD_TEXT_DESTROY: {
    const VkrViewWorldTextDestroyData *payload =
        (const VkrViewWorldTextDestroyData *)((const uint8_t *)msg +
                                              sizeof(VkrLayerMsgHeader));
    VkrViewWorldTextSlot *slot =
        vkr_view_world_get_text_slot(state, payload->text_id);
    if (!slot) {
      log_warn("World text id %u not found for destroy", payload->text_id);
      return;
    }

    vkr_text_3d_destroy(&slot->text);
    slot->active = false_v;
  } break;
  default:
    log_warn("World view received unsupported message kind %u", msg->kind);
    break;
  }
}

vkr_internal void vkr_view_world_on_detach(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  log_debug("World view detached");
}

vkr_internal void vkr_view_world_on_destroy(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  log_debug("World view destroyed");

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (state) {
    if (!vkr_view_ui_set_offscreen_enabled(rf, false_v, NULL, NULL, 0, 0, 0)) {
      log_warn("Failed to reset UI offscreen state during world teardown");
    }
    vkr_view_world_destroy_offscreen_targets(rf, state);
    rf->offscreen_color_handles = NULL;
    rf->offscreen_color_handle_count = 0;

    vkr_view_world_release_light_gizmo_states(
        rf, state->pipeline, state->light_gizmo_instance_states);
    vkr_view_world_release_light_gizmo_states(
        rf, state->pipeline_offscreen,
        state->light_gizmo_instance_states_offscreen);
    if (state->light_gizmo_geometry.id != 0) {
      vkr_geometry_system_release(&rf->geometry_system,
                                  state->light_gizmo_geometry);
    }
    if (state->light_gizmo_material.id != 0) {
      vkr_material_system_release(&rf->material_system,
                                  state->light_gizmo_material);
    }

    if (ctx->layer && ctx->layer->pass_count > 0) {
      VkrLayerPass *pass = array_get_VkrLayerPass(&ctx->layer->passes, 0);
      if (pass->use_custom_render_targets) {
        pass->render_targets = NULL;
        pass->render_target_count = 0;
        pass->custom_color_attachments = NULL;
        pass->custom_color_layouts = NULL;
      }
      pass->use_custom_render_targets = false_v;
      pass->use_swapchain_color = true_v;
      pass->use_depth = true_v;
      pass->renderpass_name = string8_lit("Renderpass.Builtin.World");
      pass->renderpass = NULL;
    }
    vkr_view_skybox_use_swapchain_targets(rf);
    state->offscreen_enabled = false_v;

    vkr_draw_batcher_shutdown(&state->draw_batcher);

    for (uint64_t i = 0; i < state->text_slots.length; ++i) {
      VkrViewWorldTextSlot *slot = &state->text_slots.data[i];
      if (!slot->active) {
        continue;
      }
      vkr_text_3d_destroy(&slot->text);
      slot->active = false_v;
    }
    array_destroy_VkrViewWorldTextSlot(&state->text_slots);

    if (state->pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->pipeline);
    }
    if (state->transparent_pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->transparent_pipeline);
    }
    if (state->overlay_pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->overlay_pipeline);
    }
    if (state->text_pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->text_pipeline);
    }
    if (state->pipeline_offscreen.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->pipeline_offscreen);
    }
    if (state->transparent_pipeline_offscreen.id) {
      vkr_pipeline_registry_destroy_pipeline(
          &rf->pipeline_registry, state->transparent_pipeline_offscreen);
    }
    if (state->overlay_pipeline_offscreen.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->overlay_pipeline_offscreen);
    }
    if (state->text_pipeline_offscreen.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->text_pipeline_offscreen);
    }
  }
}
