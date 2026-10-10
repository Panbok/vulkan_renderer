#include "vkr_lighting_system.h"

#include "math/mat.h"
#include "math/vkr_quat.h"
#include "vkr_temporal.h"

#include <math.h>

// ============================================================================
// Internal Types
// ============================================================================

/**
 * @brief Context for syncing point lights from scene.
 */
typedef struct PointLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
  uint32_t total_considered;
  /* The rendered scene's lights: its lightmap set's stationary records take
     the stationary lamps it keys. */
  bool8_t stationary_records;
} PointLightSyncContext;

typedef struct RectangleLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
} RectangleLightSyncContext;

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

vkr_internal bool8_t rectangle_light_stable_precedes(
    const VkrRectangleLight *light, const VkrRectangleLight *other) {
  return light->render_id < other->render_id;
}

/* Rectangle area and axes are authored in the entity's rigid transform frame.
 * Follow the hierarchy's quaternion chain instead of extracting axes from the
 * scaled world matrix: nonuniform parent scale must not alter either fact. */
vkr_internal bool8_t rectangle_light_world_rotation(
    const VkrScene *scene, const SceneTransform *transform,
    VkrQuat *out_rotation) {
  VkrQuat rotation = transform->rotation;
  VkrEntityId ancestor = transform->parent;
  for (uint32_t depth = 0u;
       ancestor.u64 != VKR_ENTITY_ID_INVALID.u64 && depth < scene->topo_count;
       ++depth) {
    const SceneTransform *parent =
        (const SceneTransform *)vkr_entity_get_component(scene->world, ancestor,
                                                         scene->comp_transform);
    if (!parent || !parent->trs_editable)
      return false_v;
    rotation = vkr_quat_mul(parent->rotation, rotation);
    ancestor = parent->parent;
  }
  if (ancestor.u64 != VKR_ENTITY_ID_INVALID.u64)
    return false_v;
  *out_rotation = vkr_quat_normalize(rotation);
  return true_v;
}

vkr_internal void rectangle_light_insert_stable(RectangleLightSyncContext *ctx,
                                                VkrRectangleLight candidate) {
  uint32_t insert = 0u;
  while (insert < ctx->system->rectangle_light_count &&
         !rectangle_light_stable_precedes(
             &candidate, &ctx->system->rectangle_lights[insert]))
    insert++;
  for (uint32_t i = ctx->system->rectangle_light_count; i > insert; --i)
    ctx->system->rectangle_lights[i] = ctx->system->rectangle_lights[i - 1u];
  ctx->system->rectangle_lights[insert] = candidate;
  ctx->system->rectangle_light_count++;
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
  const Vec3 closest = {
      Clamp(light->position.x, cell_min.x, cell_max.x),
      Clamp(light->position.y, cell_min.y, cell_max.y),
      Clamp(light->position.z, cell_min.z, cell_max.z),
  };
  const Vec3 delta = vec3_sub(light->position, closest);
  const float32_t range_squared = light->range * light->range;
  const float32_t conservative_epsilon = Max(range_squared * 1e-6f, 1e-5f);
  return vec3_length_squared(delta) <= range_squared + conservative_epsilon;
}

// ============================================================================
// Chunk Callbacks
// ============================================================================

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
    const float32_t group =
        vkr_scene_light_group_factor(scene, lights[i].light_group_slot);
    if (!lights[i].enabled || group <= 0.0f ||
        !vkr_scene_entity_visible(scene, entities[i]))
      continue;
    const bool8_t baked = ctx->system->static_lights_baked &&
                          lights[i].mobility == VKR_LIGHT_MOBILITY_STATIC;
    VkrLightingSystem *system = ctx->system;
    /* A stationary lamp the set keys lights through its record; any other
       stationary lamp is a dynamic light. */
    const uint32_t record =
        ctx->stationary_records && system->static_lights_baked &&
                lights[i].mobility == VKR_LIGHT_MOBILITY_STATIONARY
            ? vkr_scene_lightmap_stationary_record(scene, entities[i])
            : UINT32_MAX;
    if (baked && (!lights[i].casts_shadow ||
                  system->baked_lamp_count >= ArrayCount(system->baked_lamps)))
      continue;

    // Get world position from transform
    Vec3 world_position = mat4_position(transforms[i].world);
    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    const Vec3 direction =
        vkr_quat_rotate_vec3(transforms[i].rotation, lights[i].direction_local);
    const VkrPointLight light = {
        .position = world_position,
        .color = lights[i].color,
        .intensity = lights[i].intensity * group,
        .constant = lights[i].constant,
        .linear = lights[i].linear,
        .quadratic = lights[i].quadratic,
        .range = lights[i].range,
        .direction = direction,
        .inner_cone_angle = lights[i].inner_cone_angle,
        .outer_cone_angle = lights[i].outer_cone_angle,
        .kind = lights[i].kind,
        .render_id = render_id,
        .source_radius = lights[i].source_radius,
        .casts_shadow = lights[i].casts_shadow,
    };
    /* A baked lamp lights through the lightmap only; it stays a candidate
       for its moving casters' shadows. */
    if (record < system->stationary_count)
      system->stationary[record].light = light;
    else if (baked)
      system->baked_lamps[system->baked_lamp_count++] = light;
    else
      point_light_insert_stable(ctx, light);
  }
}

vkr_internal void sync_rectangle_lights_cb(const VkrArchetype *arch,
                                           VkrChunk *chunk, void *user) {
  (void)arch;
  RectangleLightSyncContext *ctx = (RectangleLightSyncContext *)user;
  const VkrScene *scene = ctx->scene;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneRectangleLight *lights = (SceneRectangleLight *)vkr_entity_chunk_column(
      chunk, scene->comp_rectangle_light);
  if (!entities || !transforms || !lights)
    return;

  for (uint32_t i = 0u; i < count; ++i) {
    const float32_t group =
        vkr_scene_light_group_factor(scene, lights[i].light_group_slot);
    if (!lights[i].enabled || group <= 0.0f ||
        (ctx->system->static_lights_baked &&
         lights[i].mobility == VKR_LIGHT_MOBILITY_STATIC) ||
        !vkr_scene_entity_visible(scene, entities[i]))
      continue;
    VkrQuat world_rotation;
    if (!rectangle_light_world_rotation(scene, &transforms[i], &world_rotation))
      continue;
    const VkrRectangleLight candidate = {
        .position = mat4_position(transforms[i].world),
        .right = vec3_normalize(
            vkr_quat_rotate_vec3(world_rotation, vec3_new(1.0f, 0.0f, 0.0f))),
        .up = vec3_normalize(
            vkr_quat_rotate_vec3(world_rotation, vec3_new(0.0f, 1.0f, 0.0f))),
        .color = lights[i].color,
        .half_width = lights[i].size.x * 0.5f,
        .half_height = lights[i].size.y * 0.5f,
        .radiance = lights[i].radiance * group,
        .render_id = vkr_scene_get_render_id(scene, entities[i]),
    };
    if (vkr_rectangle_light_valid(&candidate))
      rectangle_light_insert_stable(ctx, candidate);
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
  system->directional.sun_angular_diameter_degrees =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;

  system->point_light_count = 0;
  system->rectangle_light_count = 0;
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
  system->rectangle_light_count = 0;
  system->baked_lamp_count = 0u;

  /* An enabled atmosphere turns the sun light into its sun (ADR-058): the
     published atmosphere sun replaces this empty record, so the light, sky
     and bake agree. Otherwise the light shines directly. */
  const VkrSceneSun *sun = &scene->sun;
  if (!scene->atmosphere.requested_settings.enabled && sun->found) {
    system->directional.enabled = true_v;
    system->directional.direction = sun->light.direction;
    system->directional.color = sun->light.color;
    system->directional.intensity = sun->light.intensity;
    system->directional.sun_angular_diameter_degrees =
        sun->light.sun_angular_diameter_degrees;
  }

  /* The set's stationary records start unlit with their baked terms; the
     sync fills the lights it finds (ADR-107). */
  const VkrSceneLightmaps *lightmaps = scene->lightmaps;
  system->stationary_count =
      system->static_lights_baked && lightmaps
          ? Min(lightmaps->stationary_count, VKR_LIGHTMAP_STATIONARY_MAX)
          : 0u;
  system->stationary_shadowed_count = 0u;
  for (uint32_t i = 0u; i < system->stationary_count; ++i) {
    const VkrLightmapStationaryLamp *lamp = &lightmaps->stationary[i];
    system->stationary[i] = (VkrLightmapStationaryLight){
        .light = {.position = lamp->position, .range = lamp->range},
        .baked_position = lamp->position,
        .baked_range = lamp->range,
        .baked_direction = lamp->direction,
        .baked_weight = lamp->weight,
        .baked_cos_inner = lamp->cos_inner,
        .baked_cos_outer = lamp->cos_outer,
        .baked_kind = lamp->kind,
        .channel = lamp->channel,
        .shadow_light = UINT32_MAX,
    };
  }

  // Sync point lights
  PointLightSyncContext point_ctx = {
      .system = system,
      .scene = scene,
      .stationary_records = true_v,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);

  system->point_light_dropped_count =
      point_ctx.total_considered > system->point_light_count
          ? point_ctx.total_considered - system->point_light_count
          : 0u;
  RectangleLightSyncContext rectangle_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_rectangle_lights,
      sync_rectangle_lights_cb, &rectangle_ctx);
  vkr_lighting_system_build_point_light_grid(system);
  system->dirty = true_v;
}

void vkr_lighting_system_append_scene(VkrLightingSystem *system,
                                      const VkrScene *scene) {
  if (!system || !scene || !scene->world || !scene->queries_valid)
    return;
  const uint32_t before = system->point_light_count;
  PointLightSyncContext point_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);
  const uint32_t kept = system->point_light_count - before;
  system->point_light_dropped_count += point_ctx.total_considered > kept
                                           ? point_ctx.total_considered - kept
                                           : 0u;
  RectangleLightSyncContext rectangle_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_rectangle_lights,
      sync_rectangle_lights_cb, &rectangle_ctx);
  vkr_lighting_system_build_point_light_grid(system);
  system->dirty = true_v;
}

void vkr_lighting_system_apply_atmosphere_light(
    VkrLightingSystem *system, Vec3 toward_light, Vec3 irradiance,
    float32_t angular_diameter_degrees, float32_t display_exposure) {
  if (!system)
    return;

  /* The directional-light record points along incoming light, which shaders
     negate when forming their surface-to-light vector. A sky with neither a
     lit sun nor a lit moon has no light to light with, and a moon under
     street lamps lights nothing that shows: its four cascades would cost a
     whole frame's shadow budget for an invisible term. */
  const float32_t luminance =
      0.2126f * irradiance.x + 0.7152f * irradiance.y + 0.0722f * irradiance.z;
  const bool8_t visible =
      !(display_exposure > 0.0f) || luminance * display_exposure / VKR_PI >=
                                        VKR_LIGHTING_KEY_LIGHT_VISIBLE_MIN;
  system->directional.enabled =
      visible &&
      (irradiance.x > 0.0f || irradiance.y > 0.0f || irradiance.z > 0.0f);
  system->directional.direction = vec3_negate(toward_light);
  system->directional.color = irradiance;
  system->directional.intensity = 1.0f;
  system->directional.sun_angular_diameter_degrees = angular_diameter_degrees;
  system->dirty = true_v;
}

/* Rank of a light for the dynamic-light limit: the camera's distance to the
   light's range, zero inside it and for an unbounded light, then its
   distance to the light's centre. */
typedef struct PointLightRankKey {
  float32_t range_distance;
  float32_t center_distance;
} PointLightRankKey;

/* An incumbent ranks as if the camera were the bonus factor closer, so it
   also counts as inside its range until the camera is that factor past it. */
vkr_internal PointLightRankKey point_light_rank_key(const VkrPointLight *light,
                                                    Vec3 camera_position,
                                                    bool8_t incumbent) {
  float32_t center_distance =
      vec3_length(vec3_sub(light->position, camera_position));
  if (!isfinite(center_distance)) {
    return (PointLightRankKey){
        .range_distance = VKR_FLOAT_MAX,
        .center_distance = VKR_FLOAT_MAX,
    };
  }
  if (incumbent) {
    center_distance /= VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS;
  }

  const bool8_t unbounded =
      light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL || light->range <= 0.0f;
  return (PointLightRankKey){
      .range_distance =
          unbounded ? 0.0f : Max(center_distance - light->range, 0.0f),
      .center_distance = center_distance,
  };
}

/* Whether key `a` ranks strictly before key `b`. */
vkr_internal bool8_t point_light_rank_less(PointLightRankKey a,
                                           PointLightRankKey b) {
  if (a.range_distance != b.range_distance) {
    return a.range_distance < b.range_distance;
  }
  return a.center_distance < b.center_distance;
}

vkr_internal bool8_t point_light_rank_precedes(const VkrPointLight *lights,
                                               const PointLightRankKey *keys,
                                               uint32_t index, uint32_t other) {
  if (keys[index].range_distance != keys[other].range_distance) {
    return keys[index].range_distance < keys[other].range_distance;
  }
  if (keys[index].center_distance != keys[other].center_distance) {
    return keys[index].center_distance < keys[other].center_distance;
  }
  if (point_light_stable_precedes(&lights[index], &lights[other])) {
    return true_v;
  }
  if (point_light_stable_precedes(&lights[other], &lights[index])) {
    return false_v;
  }
  return index < other;
}

/* Indices of the `count` lights in rank order (an insertion sort: the table
   holds at most VKR_MAX_SCENE_POINT_LIGHTS). */
vkr_internal void point_light_order_by_rank(const VkrPointLight *lights,
                                            const PointLightRankKey *keys,
                                            uint32_t count, uint32_t *order) {
  for (uint32_t i = 0u; i < count; ++i) {
    uint32_t insert = i;
    while (insert > 0u &&
           point_light_rank_precedes(lights, keys, i, order[insert - 1u])) {
      order[insert] = order[insert - 1u];
      --insert;
    }
    order[insert] = i;
  }
}

/* The previous call's state of the light with `render_id`, or zero. */
vkr_internal VkrPointLightLimitEntry
point_light_limit_find(const VkrLightingSystem *system, uint32_t render_id) {
  uint32_t low = 0u;
  uint32_t high = system->point_light_limit_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    const uint32_t middle_id = system->point_light_limit[middle].render_id;
    if (middle_id == render_id) {
      return system->point_light_limit[middle];
    }
    if (middle_id < render_id) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return (VkrPointLightLimitEntry){0};
}

/* Fade state needs identity: a nonzero render id no other light in the table
   shares. The scene syncs keep the table in render id order, so equal ids
   are neighbours. */
vkr_internal bool8_t
point_light_limit_identified(const VkrLightingSystem *system, uint32_t index) {
  const uint32_t render_id = system->point_lights[index].render_id;
  if (render_id == 0u) {
    return false_v;
  }
  if (index > 0u && system->point_lights[index - 1u].render_id == render_id) {
    return false_v;
  }
  return index + 1u >= system->point_light_count ||
         system->point_lights[index + 1u].render_id != render_id;
}

/* Records the state the next call ranks and fades from, sorted by render id
   for its lookups. */
vkr_internal void point_light_limit_store(VkrLightingSystem *system,
                                          VkrPointLightLimitEntry entry) {
  uint32_t insert = system->point_light_limit_count;
  while (insert > 0u &&
         system->point_light_limit[insert - 1u].render_id > entry.render_id) {
    system->point_light_limit[insert] = system->point_light_limit[insert - 1u];
    --insert;
  }
  system->point_light_limit[insert] = entry;
  system->point_light_limit_count++;
}

void vkr_lighting_system_limit_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds,
                                            uint32_t light_max,
                                            uint32_t shadow_max) {
  if (!system)
    return;
  const uint32_t count = system->point_light_count;

  /* A first call, a camera cut or an unusable step has no state worth
     following, so the selected lights show at once. Lights do not depend on
     the view direction: only the position half of the cut test applies. */
  const bool8_t snap = !system->point_light_limit_valid ||
                       !isfinite(delta_seconds) || delta_seconds < 0.0f ||
                       vkr_temporal_is_camera_cut(
                           system->point_light_limit_camera, mat4_identity(),
                           camera_position, mat4_identity());
  const float32_t step =
      snap ? 1.0f
           : Min(delta_seconds / VKR_POINT_LIGHT_LIMIT_FADE_SECONDS, 1.0f);

  VkrPointLightLimitEntry previous[VKR_MAX_SCENE_POINT_LIGHTS];
  bool8_t identified[VKR_MAX_SCENE_POINT_LIGHTS];
  PointLightRankKey keys[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t order[VKR_MAX_SCENE_POINT_LIGHTS];
  bool8_t selected[VKR_MAX_SCENE_POINT_LIGHTS];
  float32_t weights[VKR_MAX_SCENE_POINT_LIGHTS];
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrPointLight *light = &system->point_lights[i];
    identified[i] = point_light_limit_identified(system, i);
    previous[i] = identified[i] && !snap
                      ? point_light_limit_find(system, light->render_id)
                      : (VkrPointLightLimitEntry){0};
    keys[i] =
        point_light_rank_key(light, camera_position, previous[i].selected);
    weights[i] = 0.0f;
  }
  point_light_order_by_rank(system->point_lights, keys, count, order);
  for (uint32_t rank = 0u; rank < count; ++rank) {
    selected[order[rank]] = rank < light_max;
  }

  /* Lights still showing keep their slots, in rank order, and fade toward
     their selection; newly selected lights then take the free slots. A
     light without identity shows at once and leaves at once. */
  uint32_t drawn_count = 0u;
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    for (uint32_t rank = 0u; rank < count; ++rank) {
      const uint32_t i = order[rank];
      const bool8_t showing = previous[i].weight > 0.0f;
      float32_t weight = 0.0f;
      if (pass == 0u) {
        if (!showing) {
          continue;
        }
        weight = selected[i] ? Min(previous[i].weight + step, 1.0f)
                             : Max(previous[i].weight - step, 0.0f);
      } else {
        if (showing || !selected[i]) {
          continue;
        }
        weight = identified[i] ? step : 1.0f;
      }
      if (weight <= 0.0f || drawn_count >= light_max) {
        continue;
      }
      weights[i] = weight;
      drawn_count++;
    }
  }

  /* Shadows go to the nearest drawn and selected casters, incumbents
     weighted. A fading-out light loses its shadow, which the local shadow
     cache fades out. */
  for (uint32_t i = 0u; i < count; ++i) {
    keys[i] = point_light_rank_key(&system->point_lights[i], camera_position,
                                   previous[i].shadowed);
  }
  point_light_order_by_rank(system->point_lights, keys, count, order);
  uint32_t shadowed = 0u;
  for (uint32_t rank = 0u; rank < count; ++rank) {
    const uint32_t i = order[rank];
    VkrPointLight *light = &system->point_lights[i];
    light->casts_shadow = light->casts_shadow && weights[i] > 0.0f &&
                          selected[i] && shadowed < shadow_max;
    if (light->casts_shadow) {
      ++shadowed;
    }
  }

  system->point_light_limit_count = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (!identified[i] || (!selected[i] && weights[i] <= 0.0f)) {
      continue;
    }
    point_light_limit_store(
        system, (VkrPointLightLimitEntry){
                    .render_id = system->point_lights[i].render_id,
                    .weight = weights[i],
                    .selected = selected[i],
                    .shadowed = system->point_lights[i].casts_shadow,
                });
  }
  system->point_light_limit_camera = camera_position;
  system->point_light_limit_valid = true_v;

  /* The drawn lights keep their table order, scaled by their fade. */
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (weights[i] <= 0.0f) {
      continue;
    }
    VkrPointLight light = system->point_lights[i];
    light.intensity *= weights[i];
    system->point_lights[kept++] = light;
  }
  system->point_light_count = kept;
  system->point_light_dropped_count += count - kept;
  if (kept != count) {
    vkr_lighting_system_build_point_light_grid(system);
  }
  system->dirty = true_v;
}

void vkr_lighting_system_shadow_stationary(VkrLightingSystem *system,
                                           Vec3 camera_position,
                                           uint32_t shadow_max) {
  if (!system) {
    return;
  }
  shadow_max = Min(shadow_max, VKR_LIGHTMAP_STATIONARY_SHADOWED_MAX);
  uint32_t chosen[VKR_LIGHTMAP_STATIONARY_SHADOWED_MAX];
  PointLightRankKey chosen_keys[VKR_LIGHTMAP_STATIONARY_SHADOWED_MAX];
  uint32_t chosen_count = 0u;
  /* An insertion into the nearest `shadow_max`, ties by record order. */
  for (uint32_t record = 0u; record < system->stationary_count; ++record) {
    const VkrPointLight *light = &system->stationary[record].light;
    if (!light->casts_shadow || !(light->intensity > 0.0f) ||
        system->stationary[record].channel ==
            VKR_LIGHTMAP_STATIONARY_NO_CHANNEL) {
      continue;
    }
    const PointLightRankKey key = point_light_rank_key(
        light, camera_position, system->stationary_was_shadowed[record]);
    uint32_t at = chosen_count;
    while (at > 0u && point_light_rank_less(key, chosen_keys[at - 1u])) {
      --at;
    }
    if (at >= shadow_max) {
      continue;
    }
    const uint32_t end = Min(chosen_count, shadow_max - 1u);
    for (uint32_t j = end; j > at; --j) {
      chosen[j] = chosen[j - 1u];
      chosen_keys[j] = chosen_keys[j - 1u];
    }
    chosen[at] = record;
    chosen_keys[at] = key;
    chosen_count = Min(chosen_count + 1u, shadow_max);
  }
  for (uint32_t record = 0u; record < system->stationary_count; ++record) {
    system->stationary_was_shadowed[record] = false_v;
    system->stationary[record].shadow_light = UINT32_MAX;
  }
  for (uint32_t i = 0u; i < chosen_count; ++i) {
    system->stationary_shadowed[i] = chosen[i];
    system->stationary_was_shadowed[chosen[i]] = true_v;
    system->stationary[chosen[i]].shadow_light = system->point_light_count + i;
  }
  system->stationary_shadowed_count = chosen_count;
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
    if (light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL ||
        light->range <= 0.0f) {
      point_light_mask_add(&grid->global_mask, i);
      continue;
    }
    const float32_t range = light->range;
    bounds_min.x = Min(bounds_min.x, light->position.x - range);
    bounds_min.y = Min(bounds_min.y, light->position.y - range);
    bounds_min.z = Min(bounds_min.z, light->position.z - range);
    bounds_max.x = Max(bounds_max.x, light->position.x + range);
    bounds_max.y = Max(bounds_max.y, light->position.y + range);
    bounds_max.z = Max(bounds_max.z, light->position.z + range);
    finite_count++;
  }

  if (finite_count == 0u) {
    grid->global_light_count = point_light_mask_count(grid->global_mask);
    grid->reference_count = grid->global_light_count;
    grid->max_lights_per_cell = grid->global_light_count;
    return;
  }

  const Vec3 extent = vec3_sub(bounds_max, bounds_min);
  float32_t cell_size = 0.0f;
  uint32_t dimensions[3] = {0};
  const uint32_t cell_count = vkr_world_grid_fit(
      extent, VKR_POINT_LIGHT_GRID_MIN_CELL_SIZE,
      VKR_POINT_LIGHT_GRID_MAX_CELLS, &cell_size, dimensions);

  grid->origin = bounds_min;
  grid->cell_size = cell_size;
  grid->dimensions[0] = dimensions[0];
  grid->dimensions[1] = dimensions[1];
  grid->dimensions[2] = dimensions[2];
  grid->cell_count = cell_count;

  for (uint32_t i = 0; i < system->point_light_count; ++i) {
    const VkrPointLight *light = &system->point_lights[i];
    if (light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL ||
        light->range <= 0.0f) {
      continue;
    }
    int32_t min_cell[3] = {
        (int32_t)floorf((light->position.x - light->range - grid->origin.x) /
                        grid->cell_size),
        (int32_t)floorf((light->position.y - light->range - grid->origin.y) /
                        grid->cell_size),
        (int32_t)floorf((light->position.z - light->range - grid->origin.z) /
                        grid->cell_size),
    };
    int32_t max_cell[3] = {
        (int32_t)floorf((light->position.x + light->range - grid->origin.x) /
                        grid->cell_size),
        (int32_t)floorf((light->position.y + light->range - grid->origin.y) /
                        grid->cell_size),
        (int32_t)floorf((light->position.z + light->range - grid->origin.z) /
                        grid->cell_size),
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
