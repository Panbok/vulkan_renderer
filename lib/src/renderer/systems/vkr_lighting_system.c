#include "vkr_lighting_system.h"

#include "math/mat.h"
#include "math/vkr_quat.h"

void vkr_lighting_system_pack_point_light(const VkrPointLight *light,
                                          VkrGpuPointLightRow *row) {
  row->p0 = (Vec4){light->position.x, light->position.y, light->position.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->inner_cone_angle)
                       : light->constant};
  row->p1 = (Vec4){light->color.x, light->color.y, light->color.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->outer_cone_angle)
                       : light->linear};
  row->p2 = (Vec4){light->intensity, light->quadratic, light->range,
                   (float32_t)light->kind};
  row->p3 =
      (Vec4){light->direction.x, light->direction.y, light->direction.z, 0.0f};
  row->p4 = (Vec4){light->influence_min.x, light->influence_min.y,
                   light->influence_min.z, 0.0f};
  row->p5 = (Vec4){light->influence_max.x, light->influence_max.y,
                   light->influence_max.z, 0.0f};
}

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
  uint32_t total_considered;
} PointLightSyncContext;

vkr_internal bool8_t point_light_stable_precedes(const VkrPointLight *light,
                                                 const VkrPointLight *other) {
  const uint32_t stable_id = light->render_id ? light->render_id : UINT32_MAX;
  const uint32_t other_id = other->render_id ? other->render_id : UINT32_MAX;
  if (stable_id != other_id) {
    return stable_id < other_id;
  }
  if (light->position.x != other->position.x) {
    return light->position.x < other->position.x;
  }
  if (light->position.y != other->position.y) {
    return light->position.y < other->position.y;
  }
  return light->position.z < other->position.z;
}

vkr_internal void point_light_insert_stable(PointLightSyncContext *ctx,
                                            VkrPointLight candidate) {
  if (!ctx) {
    return;
  }
  ctx->total_considered++;

  uint32_t insert = 0u;
  while (insert < ctx->system->point_light_count &&
         !point_light_stable_precedes(&candidate,
                                      &ctx->system->point_lights[insert])) {
    insert++;
  }
  if (insert == VKR_MAX_SCENE_POINT_LIGHTS) {
    return;
  }

  const uint32_t shifted_count =
      Min(ctx->system->point_light_count, VKR_MAX_SCENE_POINT_LIGHTS - 1u);
  for (uint32_t i = shifted_count; i > insert; --i) {
    ctx->system->point_lights[i] = ctx->system->point_lights[i - 1u];
  }
  ctx->system->point_lights[insert] = candidate;
  ctx->system->point_light_count =
      Min(ctx->system->point_light_count + 1u, VKR_MAX_SCENE_POINT_LIGHTS);
}

vkr_internal void point_light_mask_add(VkrPointLightMask *mask,
                                       uint32_t light_index) {
  if (!mask || light_index >= VKR_MAX_SCENE_POINT_LIGHTS) {
    return;
  }
  mask->words[light_index / 32u] |= 1u << (light_index % 32u);
}

vkr_internal uint32_t point_light_mask_count(VkrPointLightMask mask) {
  uint32_t count = 0u;
  for (uint32_t word = 0u; word < VKR_POINT_LIGHT_GRID_MASK_WORDS; ++word) {
    uint32_t bits = mask.words[word];
    while (bits) {
      bits &= bits - 1u;
      count++;
    }
  }
  return count;
}

vkr_internal uint32_t point_light_grid_index(const VkrPointLightGrid *grid,
                                             uint32_t x, uint32_t y,
                                             uint32_t z) {
  return x + grid->dimensions[0] * (y + grid->dimensions[1] * z);
}

vkr_internal uint32_t point_light_grid_dimension(float32_t extent,
                                                 float32_t cell_size) {
  return Max((uint32_t)ceilf(Max(extent, 0.0f) / cell_size), 1u);
}

vkr_internal uint64_t point_light_grid_dimensions_for_size(
    Vec3 extent, float32_t cell_size, uint32_t dimensions[3]) {
  dimensions[0] = point_light_grid_dimension(extent.x, cell_size);
  dimensions[1] = point_light_grid_dimension(extent.y, cell_size);
  dimensions[2] = point_light_grid_dimension(extent.z, cell_size);
  return (uint64_t)dimensions[0] * (uint64_t)dimensions[1] *
         (uint64_t)dimensions[2];
}

vkr_internal bool8_t point_light_intersects_grid_cell(
    const VkrPointLightGrid *grid, const VkrPointLight *light, uint32_t x,
    uint32_t y, uint32_t z) {
  const Vec3 cell_min = {
      grid->origin.x + (float32_t)x * grid->cell_size,
      grid->origin.y + (float32_t)y * grid->cell_size,
      grid->origin.z + (float32_t)z * grid->cell_size,
  };
  const Vec3 cell_max = {
      cell_min.x + grid->cell_size,
      cell_min.y + grid->cell_size,
      cell_min.z + grid->cell_size,
  };
  if (light->kind != VKR_POINT_LIGHT_KIND_POLYNOMIAL && light->range > 0.0f) {
    const Vec3 closest = {
        Clamp(light->position.x, cell_min.x, cell_max.x),
        Clamp(light->position.y, cell_min.y, cell_max.y),
        Clamp(light->position.z, cell_min.z, cell_max.z),
    };
    const Vec3 delta = vec3_sub(light->position, closest);
    const float32_t range_squared = light->range * light->range;
    const float32_t conservative_epsilon = Max(range_squared * 1e-6f, 1e-5f);
    if (vec3_length_squared(delta) > range_squared + conservative_epsilon) {
      return false_v;
    }
  }

  return light->influence_max.x >= cell_min.x &&
         light->influence_min.x <= cell_max.x &&
         light->influence_max.y >= cell_min.y &&
         light->influence_min.y <= cell_max.y &&
         light->influence_max.z >= cell_min.z &&
         light->influence_min.z <= cell_max.z;
}

vkr_internal bool8_t point_light_is_bounded(const VkrPointLight *light) {
  return light->influence_min.x != -VKR_FLOAT_MAX ||
         light->influence_min.y != -VKR_FLOAT_MAX ||
         light->influence_min.z != -VKR_FLOAT_MAX ||
         light->influence_max.x != VKR_FLOAT_MAX ||
         light->influence_max.y != VKR_FLOAT_MAX ||
         light->influence_max.z != VKR_FLOAT_MAX;
}

vkr_internal bool8_t point_light_grid_bounds(const VkrPointLight *light,
                                             Vec3 *out_min, Vec3 *out_max) {
  const bool8_t bounded = point_light_is_bounded(light);
  if (light->kind != VKR_POINT_LIGHT_KIND_POLYNOMIAL && light->range > 0.0f) {
    const Vec3 sphere_min = vec3_new(light->position.x - light->range,
                                     light->position.y - light->range,
                                     light->position.z - light->range);
    const Vec3 sphere_max = vec3_new(light->position.x + light->range,
                                     light->position.y + light->range,
                                     light->position.z + light->range);
    if (!bounded) {
      *out_min = sphere_min;
      *out_max = sphere_max;
      return true_v;
    }
    *out_min = vec3_new(Max(sphere_min.x, light->influence_min.x),
                        Max(sphere_min.y, light->influence_min.y),
                        Max(sphere_min.z, light->influence_min.z));
    *out_max = vec3_new(Min(sphere_max.x, light->influence_max.x),
                        Min(sphere_max.y, light->influence_max.y),
                        Min(sphere_max.z, light->influence_max.z));
    return out_min->x <= out_max->x && out_min->y <= out_max->y &&
           out_min->z <= out_max->z;
  }
  if (!bounded) {
    return false_v;
  }
  *out_min = light->influence_min;
  *out_max = light->influence_max;
  return true_v;
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
  SceneDirectionalLight *lights =
      (SceneDirectionalLight *)vkr_entity_chunk_column(
          chunk, scene->comp_directional_light);

  if (!entities || !lights)
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
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);

  if (!entities || !transforms || !lights)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    // Get world position from transform
    Vec3 world_position = mat4_position(transforms[i].world);
    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    const Vec3 direction =
        vkr_quat_rotate_vec3(transforms[i].rotation, lights[i].direction_local);
    const Vec3 influence_min =
        lights[i].has_influence_bounds
            ? lights[i].influence_min
            : vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
    const Vec3 influence_max =
        lights[i].has_influence_bounds
            ? lights[i].influence_max
            : vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
    point_light_insert_stable(
        ctx, (VkrPointLight){
                 .position = world_position,
                 .color = lights[i].color,
                 .intensity = lights[i].intensity,
                 .constant = lights[i].constant,
                 .linear = lights[i].linear,
                 .quadratic = lights[i].quadratic,
                 .range = lights[i].range,
                 .direction = direction,
                 .inner_cone_angle = lights[i].inner_cone_angle,
                 .outer_cone_angle = lights[i].outer_cone_angle,
                 .influence_min = influence_min,
                 .influence_max = influence_max,
                 .kind = lights[i].kind,
                 .render_id = render_id,
             });
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
  system->point_light_dropped_count = 0;

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
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);

  system->point_light_dropped_count =
      point_ctx.total_considered > system->point_light_count
          ? point_ctx.total_considered - system->point_light_count
          : 0u;
  vkr_lighting_system_build_point_light_grid(system);
  system->dirty = true_v;
}

void vkr_lighting_system_build_point_light_grid(VkrLightingSystem *system) {
  if (!system) {
    return;
  }
  VkrPointLightGrid *grid = &system->point_light_grid;
  MemZero(grid, sizeof(*grid));
  if (system->point_light_count == 0u) {
    return;
  }

  Vec3 bounds_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 bounds_max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  uint32_t finite_count = 0u;
  for (uint32_t i = 0; i < system->point_light_count; ++i) {
    const VkrPointLight *light = &system->point_lights[i];
    Vec3 light_min = {0};
    Vec3 light_max = {0};
    if (!point_light_grid_bounds(light, &light_min, &light_max)) {
      if (!point_light_is_bounded(light) &&
          (light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL ||
           light->range <= 0.0f)) {
        point_light_mask_add(&grid->global_mask, i);
      }
      continue;
    }
    bounds_min.x = Min(bounds_min.x, light_min.x);
    bounds_min.y = Min(bounds_min.y, light_min.y);
    bounds_min.z = Min(bounds_min.z, light_min.z);
    bounds_max.x = Max(bounds_max.x, light_max.x);
    bounds_max.y = Max(bounds_max.y, light_max.y);
    bounds_max.z = Max(bounds_max.z, light_max.z);
    finite_count++;
  }

  if (finite_count == 0u) {
    grid->global_light_count = point_light_mask_count(grid->global_mask);
    grid->reference_count = grid->global_light_count;
    grid->max_lights_per_cell = grid->global_light_count;
    return;
  }

  const Vec3 extent = vec3_sub(bounds_max, bounds_min);
  float32_t cell_size = VKR_POINT_LIGHT_GRID_MIN_CELL_SIZE;
  uint32_t dimensions[3] = {0};
  uint64_t cell_count =
      point_light_grid_dimensions_for_size(extent, cell_size, dimensions);
  if (cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS) {
    float32_t rejected_size = cell_size;
    do {
      rejected_size = cell_size;
      cell_size *= 1.25f;
      cell_count =
          point_light_grid_dimensions_for_size(extent, cell_size, dimensions);
    } while (cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS);

    // The coarse growth step only brackets the answer. Bisection chooses the
    // densest representable grid so a quantization jump cannot leave a large
    // part of the fixed uniform budget unused.
    for (uint32_t iteration = 0u; iteration < 16u; ++iteration) {
      const float32_t candidate_size = (rejected_size + cell_size) * 0.5f;
      uint32_t candidate_dimensions[3];
      const uint64_t candidate_count = point_light_grid_dimensions_for_size(
          extent, candidate_size, candidate_dimensions);
      if (candidate_count > VKR_POINT_LIGHT_GRID_MAX_CELLS) {
        rejected_size = candidate_size;
      } else {
        cell_size = candidate_size;
        dimensions[0] = candidate_dimensions[0];
        dimensions[1] = candidate_dimensions[1];
        dimensions[2] = candidate_dimensions[2];
        cell_count = candidate_count;
      }
    }
  }

  grid->origin = bounds_min;
  grid->cell_size = cell_size;
  grid->dimensions[0] = dimensions[0];
  grid->dimensions[1] = dimensions[1];
  grid->dimensions[2] = dimensions[2];
  grid->cell_count = (uint32_t)cell_count;

  for (uint32_t i = 0; i < system->point_light_count; ++i) {
    const VkrPointLight *light = &system->point_lights[i];
    Vec3 light_min = {0};
    Vec3 light_max = {0};
    if (!point_light_grid_bounds(light, &light_min, &light_max)) {
      continue;
    }
    int32_t min_cell[3] = {
        (int32_t)floorf((light_min.x - grid->origin.x) / grid->cell_size),
        (int32_t)floorf((light_min.y - grid->origin.y) / grid->cell_size),
        (int32_t)floorf((light_min.z - grid->origin.z) / grid->cell_size),
    };
    int32_t max_cell[3] = {
        (int32_t)floorf((light_max.x - grid->origin.x) / grid->cell_size),
        (int32_t)floorf((light_max.y - grid->origin.y) / grid->cell_size),
        (int32_t)floorf((light_max.z - grid->origin.z) / grid->cell_size),
    };
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      min_cell[axis] =
          Clamp(min_cell[axis], 0, (int32_t)grid->dimensions[axis] - 1);
      max_cell[axis] =
          Clamp(max_cell[axis], 0, (int32_t)grid->dimensions[axis] - 1);
    }
    for (int32_t z = min_cell[2]; z <= max_cell[2]; ++z) {
      for (int32_t y = min_cell[1]; y <= max_cell[1]; ++y) {
        for (int32_t x = min_cell[0]; x <= max_cell[0]; ++x) {
          if (!point_light_intersects_grid_cell(grid, light, (uint32_t)x,
                                                (uint32_t)y, (uint32_t)z)) {
            continue;
          }
          const uint32_t cell = point_light_grid_index(
              grid, (uint32_t)x, (uint32_t)y, (uint32_t)z);
          point_light_mask_add(&grid->masks[cell], i);
          grid->reference_count++;
        }
      }
    }
  }

  grid->global_light_count = point_light_mask_count(grid->global_mask);
  grid->reference_count += grid->global_light_count;
  for (uint32_t cell = 0u; cell < grid->cell_count; ++cell) {
    const uint32_t local_count = point_light_mask_count(grid->masks[cell]);
    grid->max_lights_per_cell =
        Max(grid->max_lights_per_cell, local_count + grid->global_light_count);
  }
}

VkrPointLightMask
vkr_lighting_system_point_light_mask_at(const VkrLightingSystem *system,
                                        Vec3 world_position) {
  VkrPointLightMask result = {0};
  if (!system) {
    return result;
  }
  const VkrPointLightGrid *grid = &system->point_light_grid;
  result = grid->global_mask;
  if (grid->cell_count == 0u || grid->cell_size <= 0.0f) {
    return result;
  }
  const int32_t cell[3] = {
      (int32_t)floorf((world_position.x - grid->origin.x) / grid->cell_size),
      (int32_t)floorf((world_position.y - grid->origin.y) / grid->cell_size),
      (int32_t)floorf((world_position.z - grid->origin.z) / grid->cell_size),
  };
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (cell[axis] < 0 || cell[axis] >= (int32_t)grid->dimensions[axis]) {
      return result;
    }
  }
  const uint32_t index = point_light_grid_index(
      grid, (uint32_t)cell[0], (uint32_t)cell[1], (uint32_t)cell[2]);
  for (uint32_t word = 0u; word < VKR_POINT_LIGHT_GRID_MASK_WORDS; ++word) {
    result.words[word] |= grid->masks[index].words[word];
  }
  return result;
}

bool8_t
vkr_lighting_system_point_light_mask_contains(const VkrPointLightMask *mask,
                                              uint32_t light_index) {
  if (!mask || light_index >= VKR_MAX_SCENE_POINT_LIGHTS) {
    return false_v;
  }
  return (mask->words[light_index / 32u] & (1u << (light_index % 32u))) != 0u;
}
