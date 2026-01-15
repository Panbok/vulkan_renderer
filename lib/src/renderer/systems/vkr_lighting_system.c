#include "vkr_lighting_system.h"

#include "math/mat.h"
#include "math/vkr_quat.h"

// ============================================================================
// Internal Types
// ============================================================================

/**
 * @brief Context for syncing directional light from scene.
 */
typedef struct DirectionalLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
  bool8_t found;
  uint32_t best_render_id;
  bool8_t best_has_render_id;
} DirectionalLightSyncContext;

/**
 * @brief Context for syncing point lights from scene.
 */
typedef struct PointLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
  struct PointLightCandidate {
    uint32_t render_id;
    Vec3 position;
    Vec3 color;
    float32_t intensity;
    float32_t constant;
    float32_t linear;
    float32_t quadratic;
  } candidates[VKR_MAX_POINT_LIGHTS];
  uint32_t count;
} PointLightSyncContext;

vkr_internal void point_light_insert_sorted(PointLightSyncContext *ctx,
                                            uint32_t render_id,
                                            Vec3 position, Vec3 color,
                                            float32_t intensity,
                                            float32_t constant,
                                            float32_t linear,
                                            float32_t quadratic) {
  if (!ctx)
    return;

  uint32_t count = ctx->count;
  uint32_t sort_key = render_id ? render_id : UINT32_MAX;
  uint32_t insert = 0;
  while (insert < count &&
         ctx->candidates[insert].render_id < sort_key) {
    insert++;
  }

  if (count < VKR_MAX_POINT_LIGHTS) {
    for (uint32_t j = count; j > insert; j--) {
      ctx->candidates[j] = ctx->candidates[j - 1];
    }
    ctx->candidates[insert] = (struct PointLightCandidate){
        .render_id = sort_key,
        .position = position,
        .color = color,
        .intensity = intensity,
        .constant = constant,
        .linear = linear,
        .quadratic = quadratic,
    };
    ctx->count = count + 1;
    return;
  }

  if (sort_key >= ctx->candidates[count - 1].render_id)
    return;

  for (uint32_t j = count - 1; j > insert; j--) {
    ctx->candidates[j] = ctx->candidates[j - 1];
  }
  ctx->candidates[insert] = (struct PointLightCandidate){
      .render_id = sort_key,
      .position = position,
      .color = color,
      .intensity = intensity,
      .constant = constant,
      .linear = linear,
      .quadratic = quadratic,
  };
}

// ============================================================================
// Chunk Callbacks
// ============================================================================

vkr_internal void sync_directional_light_cb(const VkrArchetype *arch,
                                            VkrChunk *chunk, void *user) {
  (void)arch;
  DirectionalLightSyncContext *ctx = (DirectionalLightSyncContext *)user;

  const VkrScene *scene = ctx->scene;
  uint32_t count = vkr_entity_chunk_count(chunk);

  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneDirectionalLight *lights = (SceneDirectionalLight *)vkr_entity_chunk_column(
      chunk, scene->comp_directional_light);

  if (!lights)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    bool8_t has_render_id = (render_id != 0);

    if (ctx->found) {
      if (ctx->best_has_render_id) {
        if (!has_render_id || render_id >= ctx->best_render_id) {
          continue;
        }
      } else {
        if (!has_render_id) {
          continue;
        }
      }
    }

    // Get transform to compute world direction
    const SceneTransform *transform =
        (const SceneTransform *)vkr_entity_get_component(
            scene->world, entities[i], scene->comp_transform);

    Vec3 world_direction = lights[i].direction_local;
    if (transform) {
      world_direction =
          vkr_quat_rotate_vec3(transform->rotation, lights[i].direction_local);
    }

    ctx->system->directional.enabled = true_v;
    ctx->system->directional.direction = world_direction;
    ctx->system->directional.color = lights[i].color;
    ctx->system->directional.intensity = lights[i].intensity;
    ctx->found = true_v;
    ctx->best_render_id = render_id;
    ctx->best_has_render_id = has_render_id;
  }
}

vkr_internal void sync_point_lights_cb(const VkrArchetype *arch,
                                       VkrChunk *chunk, void *user) {
  (void)arch;
  PointLightSyncContext *ctx = (PointLightSyncContext *)user;

  const VkrScene *scene = ctx->scene;
  uint32_t count = vkr_entity_chunk_count(chunk);

  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights =
      (ScenePointLight *)vkr_entity_chunk_column(chunk, scene->comp_point_light);

  if (!transforms || !lights)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    // Get world position from transform
    Vec3 world_position = mat4_position(transforms[i].world);
    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    point_light_insert_sorted(ctx, render_id, world_position, lights[i].color,
                              lights[i].intensity, lights[i].constant,
                              lights[i].linear, lights[i].quadratic);
  }
}

// ============================================================================
// Public API
// ============================================================================

bool8_t vkr_lighting_system_init(VkrLightingSystem *system) {
  if (!system)
    return false_v;

  MemZero(system, sizeof(VkrLightingSystem));

  // Initialize with default directional light (disabled)
  system->directional.enabled = false_v;
  system->directional.direction = (Vec3){0.0f, -1.0f, 0.0f};
  system->directional.color = (Vec3){1.0f, 1.0f, 1.0f};
  system->directional.intensity = 1.0f;

  system->point_light_count = 0;
  system->dirty = true_v;

  return true_v;
}

void vkr_lighting_system_shutdown(VkrLightingSystem *system) {
  if (!system)
    return;
  MemZero(system, sizeof(VkrLightingSystem));
}

void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                         const VkrScene *scene) {
  if (!system || !scene || !scene->world)
    return;

  // Compile queries if needed (should already be done by scene update)
  if (!scene->queries_valid)
    return;

  // Reset state
  system->directional.enabled = false_v;
  system->point_light_count = 0;

  // Sync directional light (take first enabled)
  DirectionalLightSyncContext dir_ctx = {
      .system = system,
      .scene = scene,
      .found = false_v,
      .best_render_id = 0,
      .best_has_render_id = false_v,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_directional_light,
      sync_directional_light_cb, &dir_ctx);

  // Sync point lights
  PointLightSyncContext point_ctx = {
      .system = system,
      .scene = scene,
      .count = 0,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);

  system->point_light_count = point_ctx.count;
  for (uint32_t i = 0; i < point_ctx.count; i++) {
    system->point_lights[i].position = point_ctx.candidates[i].position;
    system->point_lights[i].color = point_ctx.candidates[i].color;
    system->point_lights[i].intensity = point_ctx.candidates[i].intensity;
    system->point_lights[i].constant = point_ctx.candidates[i].constant;
    system->point_lights[i].linear = point_ctx.candidates[i].linear;
    system->point_lights[i].quadratic = point_ctx.candidates[i].quadratic;
  }
  system->dirty = true_v;
}

void vkr_lighting_system_apply_uniforms(VkrLightingSystem *system) {
  if (!system || !system->shader_system)
    return;

  VkrShaderSystem *ss = system->shader_system;

  // Apply directional light uniforms
  uint32_t dir_enabled = system->directional.enabled ? 1u : 0u;
  vkr_shader_system_uniform_set(ss, "dir_enabled", &dir_enabled);

  Vec3 dir_direction = system->directional.direction;
  vkr_shader_system_uniform_set(ss, "dir_direction", &dir_direction);

  // Pack color * intensity into vec4
  Vec4 dir_color = {
      system->directional.color.x * system->directional.intensity,
      system->directional.color.y * system->directional.intensity,
      system->directional.color.z * system->directional.intensity,
      1.0f,
  };
  vkr_shader_system_uniform_set(ss, "dir_color", &dir_color);

  // Apply point light count
  vkr_shader_system_uniform_set(ss, "point_light_count",
                                &system->point_light_count);

  // Pack point light data into vec4 array (3 vec4s per light)
  // [i*3+0] = {position.xyz, constant}
  // [i*3+1] = {color.rgb * intensity, linear}
  // [i*3+2] = {intensity, quadratic, 0, 0}
  Vec4 point_light_data[VKR_MAX_POINT_LIGHTS * 3];
  MemZero(point_light_data, sizeof(point_light_data));

  for (uint32_t i = 0; i < system->point_light_count; i++) {
    point_light_data[i * 3 + 0] = (Vec4){
        system->point_lights[i].position.x,
        system->point_lights[i].position.y,
        system->point_lights[i].position.z,
        system->point_lights[i].constant,
    };
    point_light_data[i * 3 + 1] = (Vec4){
        system->point_lights[i].color.x * system->point_lights[i].intensity,
        system->point_lights[i].color.y * system->point_lights[i].intensity,
        system->point_lights[i].color.z * system->point_lights[i].intensity,
        system->point_lights[i].linear,
    };
    point_light_data[i * 3 + 2] = (Vec4){
        system->point_lights[i].intensity,
        system->point_lights[i].quadratic,
        0.0f,
        0.0f,
    };
  }

  vkr_shader_system_uniform_set(ss, "point_light_data", point_light_data);

  system->dirty = false_v;
}

void vkr_lighting_system_mark_dirty(VkrLightingSystem *system) {
  if (system) {
    system->dirty = true_v;
  }
}

bool8_t vkr_lighting_system_is_dirty(const VkrLightingSystem *system) {
  return system ? system->dirty : false_v;
}
