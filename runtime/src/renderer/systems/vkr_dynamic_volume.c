#include "renderer/systems/vkr_dynamic_volume.h"

#include "core/logger.h"
#include "math/vkr_quat.h"
#include "memory/vkr_heap.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_texture_system.h"

/* Rectangle lights and emitters reach probes within these spheres; point
   lights within their own reach (dynamic_volume_point_reach). */
typedef struct DynamicVolumeSphere {
  Vec3 center;
  float32_t radius;
} DynamicVolumeSphere;

#define DYNAMIC_VOLUME_SPHERES_MAX                                             \
  (VKR_DYNAMIC_VOLUME_RECTANGLE_LIGHTS_MAX + 256u)
/* Indirection entries per side of a brick cell, and the indirection
   entries a volume may have for its cells. */
#define DYNAMIC_VOLUME_CELL_ENTRIES 4u
#define DYNAMIC_VOLUME_ENTRIES_MAX (1u << 24u)
/* Irradiance below which a source's bounce is left out, in the volume's
   physical units, and the farthest reach of an unbounded source. */
#define DYNAMIC_VOLUME_IRRADIANCE_FLOOR 0.05f
#define DYNAMIC_VOLUME_REACH_MAX 32.0f
/* Bake records a frame compares at most; further ones count as unchanged. */
#define DYNAMIC_VOLUME_RECORDS_MAX 1024u
/* A static light within this distance of a bake record of its kind is that
   record's light, in metres. */
#define DYNAMIC_VOLUME_MATCH_DISTANCE 0.01f

/* The bake's light kinds (VkrSceneBakedLight). */
#define DYNAMIC_VOLUME_BAKED_POINT 1u
#define DYNAMIC_VOLUME_BAKED_SPOT 2u
#define DYNAMIC_VOLUME_BAKED_POLYNOMIAL 3u
#define DYNAMIC_VOLUME_BAKED_RECTANGLE 4u

typedef struct DynamicVolumeGather {
  const VkrScene *scene;
  const VkrSceneDiffuseVolume *volume;
  VkrDynamicVolumeSelection *selection;
  VkrDynamicVolumeFrame *out;
  DynamicVolumeSphere *spheres;
  uint32_t sphere_count;
  /* Whether the point lights filled the cap, so the selection's heap
     orders their slots by luminous power. */
  bool8_t point_heap;
  /* Bake records a current static light matched this frame. */
  uint8_t *matched;
  uint32_t record_count;
} DynamicVolumeGather;

/* Distance at which a source of `luminous` (intensity times luminance,
   inverse-square) falls below the irradiance floor. */
vkr_internal float32_t dynamic_volume_reach(float32_t luminous) {
  return Min(
      vkr_sqrt_f32(Max(luminous, 0.0f) / DYNAMIC_VOLUME_IRRADIANCE_FLOOR),
      DYNAMIC_VOLUME_REACH_MAX);
}

vkr_internal float32_t dynamic_volume_luminance(Vec3 color) {
  return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

vkr_internal void dynamic_volume_add_sphere(DynamicVolumeGather *gather,
                                            Vec3 center, float32_t radius) {
  if (gather->sphere_count < DYNAMIC_VOLUME_SPHERES_MAX && radius > 0.0f) {
    gather->spheres[gather->sphere_count++] =
        (DynamicVolumeSphere){center, radius};
  }
}

/* The current factor of the light group named `name`; one for a group the
   scene has not registered. */
vkr_internal float32_t dynamic_volume_group_factor(const VkrScene *scene,
                                                   const char *name) {
  for (uint32_t i = 0u; i < scene->light_groups.count; ++i) {
    if (string_n_equals(scene->light_groups.names[i], name,
                        VKR_LIGHT_GROUP_NAME_BYTES)) {
      return scene->light_groups.factors[i];
    }
  }
  return 1.0f;
}

vkr_internal bool8_t dynamic_volume_close(float32_t a, float32_t b) {
  return vkr_abs_f32(a - b) <=
         1e-3f * Max(Max(vkr_abs_f32(a), vkr_abs_f32(b)), 1.0f);
}

vkr_internal bool8_t dynamic_volume_close3(Vec3 a, Vec3 b) {
  return dynamic_volume_close(a.x, b.x) && dynamic_volume_close(a.y, b.y) &&
         dynamic_volume_close(a.z, b.z);
}

/* Whether two light group names name one group: an empty name is the
   group VKR_LIGHT_GROUP_DEFAULT, which a bake record names in full. */
vkr_internal bool8_t dynamic_volume_same_group(const char *a, const char *b) {
  return string_equals(a[0] != '\0' ? a : VKR_LIGHT_GROUP_DEFAULT,
                       b[0] != '\0' ? b : VKR_LIGHT_GROUP_DEFAULT);
}

/* Whether a static point, spot or polynomial light is still the light the
   bake recorded, its spot direction `direction`. */
vkr_internal bool8_t
dynamic_volume_point_unchanged(const VkrSceneBakedLight *record,
                               const ScenePointLight *light, Vec3 direction) {
  return dynamic_volume_close3(record->color, light->color) &&
         dynamic_volume_close(record->intensity, light->intensity) &&
         dynamic_volume_close(record->range, light->range) &&
         dynamic_volume_close(record->constant, light->constant) &&
         dynamic_volume_close(record->linear, light->linear) &&
         dynamic_volume_close(record->quadratic, light->quadratic) &&
         dynamic_volume_close(record->inner_cone_angle,
                              light->inner_cone_angle) &&
         dynamic_volume_close(record->outer_cone_angle,
                              light->outer_cone_angle) &&
         (record->kind != DYNAMIC_VOLUME_BAKED_SPOT ||
          vec3_dot(record->direction, direction) > 0.9999f) &&
         dynamic_volume_same_group(record->group, light->light_group);
}

/* Whether a static rectangle light is still the light the bake recorded. */
vkr_internal bool8_t dynamic_volume_rectangle_unchanged(
    const VkrSceneBakedLight *record, const SceneRectangleLight *light,
    const VkrRectangleLight *rectangle) {
  return dynamic_volume_close3(record->color, light->color) &&
         dynamic_volume_close(record->radiance, light->radiance) &&
         dynamic_volume_close(record->half_width, rectangle->half_width) &&
         dynamic_volume_close(record->half_height, rectangle->half_height) &&
         vec3_dot(record->right, rectangle->right) > 0.9999f &&
         vec3_dot(record->up, rectangle->up) > 0.9999f &&
         dynamic_volume_same_group(record->group, light->light_group);
}

/* The slot of the light group a bake record names; the default group for
   an empty name, NONE for a group the scene has not registered. */
vkr_internal uint8_t dynamic_volume_record_slot(const VkrScene *scene,
                                                const char *name) {
  const char *group = name[0] != '\0' ? name : VKR_LIGHT_GROUP_DEFAULT;
  for (uint32_t i = 0u; i < scene->light_groups.count; ++i) {
    if (string_equals(scene->light_groups.names[i], group)) {
      return (uint8_t)i;
    }
  }
  return VKR_SCENE_LIGHT_GROUP_NONE;
}

/* The sphere a point, spot or polynomial light reaches probes within. */
vkr_internal float32_t dynamic_volume_point_reach(const VkrPointLight *point) {
  const float32_t reach = dynamic_volume_reach(
      point->intensity * dynamic_volume_luminance(point->color));
  return point->range > 0.0f ? Min(point->range, reach) : reach;
}

/* Restores the min-heap of point light slots, by luminous power, below
   position `at`. */
vkr_internal void dynamic_volume_heap_down(uint32_t *heap,
                                           const float32_t *power,
                                           uint32_t count, uint32_t at) {
  for (;;) {
    const uint32_t left = 2u * at + 1u;
    const uint32_t right = left + 1u;
    uint32_t weakest = at;
    if (left < count && power[heap[left]] < power[heap[weakest]]) {
      weakest = left;
    }
    if (right < count && power[heap[right]] < power[heap[weakest]]) {
      weakest = right;
    }
    if (weakest == at) {
      return;
    }
    const uint32_t slot = heap[at];
    heap[at] = heap[weakest];
    heap[weakest] = slot;
    at = weakest;
  }
}

/* Adds a point, spot or polynomial light, signed. Past the cap it replaces
   the weakest light kept when its luminous power is greater, so the layer
   keeps the strongest sources rather than the first in scene order. */
vkr_internal void dynamic_volume_add_point(DynamicVolumeGather *gather,
                                           const VkrPointLight *point,
                                           float32_t sign) {
  if (point->intensity <= 0.0f) {
    return;
  }
  VkrDynamicVolumeInput *input = &gather->out->input;
  VkrDynamicVolumeSelection *selection = gather->selection;
  const float32_t power =
      point->intensity * dynamic_volume_luminance(point->color);
  uint32_t slot = input->point_light_count;
  if (slot < VKR_DYNAMIC_VOLUME_LIGHTS_MAX) {
    input->point_light_count++;
  } else {
    if (!gather->point_heap) {
      for (uint32_t i = 0u; i < VKR_DYNAMIC_VOLUME_LIGHTS_MAX; ++i) {
        selection->point_heap[i] = i;
      }
      for (uint32_t i = VKR_DYNAMIC_VOLUME_LIGHTS_MAX / 2u; i-- > 0u;) {
        dynamic_volume_heap_down(selection->point_heap, selection->point_power,
                                 VKR_DYNAMIC_VOLUME_LIGHTS_MAX, i);
      }
      gather->point_heap = true_v;
    }
    slot = selection->point_heap[0];
    if (!(power > selection->point_power[slot])) {
      return;
    }
  }
  gather->out->point_lights[slot] = *point;
  gather->out->point_light_signs[slot] = sign;
  selection->point_power[slot] = power;
  if (gather->point_heap) {
    dynamic_volume_heap_down(selection->point_heap, selection->point_power,
                             VKR_DYNAMIC_VOLUME_LIGHTS_MAX, 0u);
  }
}

vkr_internal void dynamic_volume_add_rectangle(DynamicVolumeGather *gather,
                                               const VkrRectangleLight *light,
                                               float32_t sign) {
  VkrDynamicVolumeInput *input = &gather->out->input;
  if (input->rectangle_light_count >= VKR_DYNAMIC_VOLUME_RECTANGLE_LIGHTS_MAX ||
      !vkr_rectangle_light_valid(light) || light->radiance <= 0.0f) {
    return;
  }
  gather->out->rectangle_lights[input->rectangle_light_count] = *light;
  gather->out->rectangle_light_signs[input->rectangle_light_count++] = sign;
  const float32_t area = 4.0f * light->half_width * light->half_height;
  dynamic_volume_add_sphere(
      gather, light->position,
      dynamic_volume_reach(light->radiance * area *
                           dynamic_volume_luminance(light->color)));
}

/* The bake kind of a runtime point light kind. */
vkr_internal uint32_t dynamic_volume_baked_kind(VkrPointLightKind kind) {
  return kind == VKR_POINT_LIGHT_KIND_GLTF_POINT ? DYNAMIC_VOLUME_BAKED_POINT
         : kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
             ? DYNAMIC_VOLUME_BAKED_SPOT
             : DYNAMIC_VOLUME_BAKED_POLYNOMIAL;
}

/* The bake record a static light was, or UINT32_MAX for a light the bake
   did not hold: the first unmatched record of its kind at its position. */
vkr_internal uint32_t dynamic_volume_match(DynamicVolumeGather *gather,
                                           uint32_t kind,
                                           Vec3 render_position) {
  const VkrSceneDiffuseVolume *volume = gather->volume;
  const Vec3 position = vec3_add(render_position, gather->scene->origin_offset);
  for (uint32_t i = 0u; i < gather->record_count; ++i) {
    const VkrSceneBakedLight *record = &volume->baked_lights[i];
    if (gather->matched[i] || record->kind != kind ||
        vec3_distance(record->position, position) >
            DYNAMIC_VOLUME_MATCH_DISTANCE) {
      continue;
    }
    gather->matched[i] = 1u;
    return i;
  }
  return UINT32_MAX;
}

/* A bake record's light as it lit the bake, in render space, with its
   group's current factor, which also scales its baked layer. */
vkr_internal VkrPointLight dynamic_volume_record_point(
    const DynamicVolumeGather *gather, const VkrSceneBakedLight *record) {
  const VkrPointLightKind kind = record->kind == DYNAMIC_VOLUME_BAKED_POINT
                                     ? VKR_POINT_LIGHT_KIND_GLTF_POINT
                                 : record->kind == DYNAMIC_VOLUME_BAKED_SPOT
                                     ? VKR_POINT_LIGHT_KIND_GLTF_SPOT
                                     : VKR_POINT_LIGHT_KIND_POLYNOMIAL;
  return (VkrPointLight){
      .position = vec3_sub(record->position, gather->scene->origin_offset),
      .color = record->color,
      .intensity = record->intensity *
                   dynamic_volume_group_factor(gather->scene, record->group),
      .constant = record->constant,
      .linear = record->linear,
      .quadratic = record->quadratic,
      .range = record->range,
      .direction = record->direction,
      .inner_cone_angle = record->inner_cone_angle,
      .outer_cone_angle = record->outer_cone_angle,
      .kind = kind,
  };
}

vkr_internal VkrRectangleLight dynamic_volume_record_rectangle(
    const DynamicVolumeGather *gather, const VkrSceneBakedLight *record) {
  return (VkrRectangleLight){
      .position = vec3_sub(record->position, gather->scene->origin_offset),
      .right = record->right,
      .up = record->up,
      .color = record->color,
      .half_width = record->half_width,
      .half_height = record->half_height,
      .radiance = record->radiance *
                  dynamic_volume_group_factor(gather->scene, record->group),
  };
}

/* Dynamic point and spot lights: the bake holds none of their light. */
vkr_internal void dynamic_volume_point_lights_cb(const VkrArchetype *arch,
                                                 VkrChunk *chunk, void *user) {
  (void)arch;
  DynamicVolumeGather *gather = user;
  const VkrScene *scene = gather->scene;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);
  if (!entities || !transforms || !lights) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const ScenePointLight *light = &lights[i];
    const float32_t group =
        vkr_scene_light_group_factor(scene, light->light_group_slot);
    const bool8_t dynamic = light->mobility == VKR_LIGHT_MOBILITY_DYNAMIC;
    /* Static lights count only against the bake's record of them. */
    if ((!dynamic && !gather->volume->baked_lights_known) || !light->enabled ||
        group <= 0.0f || !vkr_scene_entity_visible(scene, entities[i])) {
      continue;
    }
    const Vec3 position = mat4_position(transforms[i].world);
    const VkrPointLight point = {
        .position = position,
        .color = light->color,
        .intensity = light->intensity * group,
        .constant = light->constant,
        .linear = light->linear,
        .quadratic = light->quadratic,
        .range = light->range,
        .direction = vkr_scene_light_world_direction(scene, &transforms[i],
                                                     light->direction_local),
        .inner_cone_angle = light->inner_cone_angle,
        .outer_cone_angle = light->outer_cone_angle,
        .kind = light->kind,
        .source_radius = light->source_radius,
    };
    /* A group that left the bake weighs nothing in the baked layers, so its
       lights join whole and its records take nothing away. */
    if (!dynamic &&
        !vkr_scene_light_group_left_bake(scene, light->light_group_slot)) {
      const uint32_t record_index = dynamic_volume_match(
          gather, dynamic_volume_baked_kind(light->kind), position);
      if (record_index != UINT32_MAX) {
        const VkrSceneBakedLight *record =
            &gather->volume->baked_lights[record_index];
        /* Unchanged since the bake: its light is baked. */
        if (dynamic_volume_point_unchanged(record, light, point.direction)) {
          continue;
        }
        /* Changed: its baked light leaves and its current light joins. */
        const VkrPointLight baked = dynamic_volume_record_point(gather, record);
        dynamic_volume_add_point(gather, &baked, -1.0f);
      }
    }
    dynamic_volume_add_point(gather, &point, 1.0f);
  }
}

/* Dynamic rectangle lights, approximated at their centre by the tracer. */
vkr_internal void dynamic_volume_rectangle_lights_cb(const VkrArchetype *arch,
                                                     VkrChunk *chunk,
                                                     void *user) {
  (void)arch;
  DynamicVolumeGather *gather = user;
  const VkrScene *scene = gather->scene;
  VkrDynamicVolumeInput *input = &gather->out->input;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneRectangleLight *lights = (SceneRectangleLight *)vkr_entity_chunk_column(
      chunk, scene->comp_rectangle_light);
  if (!entities || !transforms || !lights) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const SceneRectangleLight *light = &lights[i];
    const float32_t group =
        vkr_scene_light_group_factor(scene, light->light_group_slot);
    const bool8_t dynamic = light->mobility == VKR_LIGHT_MOBILITY_DYNAMIC;
    if ((!dynamic && !gather->volume->baked_lights_known) || !light->enabled ||
        group <= 0.0f || !vkr_scene_entity_visible(scene, entities[i]) ||
        input->rectangle_light_count >=
            VKR_DYNAMIC_VOLUME_RECTANGLE_LIGHTS_MAX) {
      continue;
    }
    const Mat4 world = transforms[i].world;
    const VkrRectangleLight rectangle = {
        .position = mat4_position(world),
        .right = vec3_normalize(vec3_new(world.m00, world.m10, world.m20)),
        .up = vec3_normalize(vec3_new(world.m01, world.m11, world.m21)),
        .color = light->color,
        .half_width = 0.5f * light->size.x,
        .half_height = 0.5f * light->size.y,
        .radiance = light->radiance * group,
    };
    if (!vkr_rectangle_light_valid(&rectangle)) {
      continue;
    }
    if (!dynamic &&
        !vkr_scene_light_group_left_bake(scene, light->light_group_slot)) {
      const uint32_t record_index = dynamic_volume_match(
          gather, DYNAMIC_VOLUME_BAKED_RECTANGLE, rectangle.position);
      if (record_index != UINT32_MAX) {
        const VkrSceneBakedLight *record =
            &gather->volume->baked_lights[record_index];
        if (dynamic_volume_rectangle_unchanged(record, light, &rectangle)) {
          continue;
        }
        const VkrRectangleLight baked =
            dynamic_volume_record_rectangle(gather, record);
        dynamic_volume_add_rectangle(gather, &baked, -1.0f);
      }
    }
    dynamic_volume_add_rectangle(gather, &rectangle, 1.0f);
  }
}

/* Bake records no current light matched: lights removed, switched off or
   moved since the bake, whose baked light leaves. */
vkr_internal void dynamic_volume_removed_records(DynamicVolumeGather *gather) {
  for (uint32_t i = 0u; i < gather->record_count; ++i) {
    const VkrSceneBakedLight *record = &gather->volume->baked_lights[i];
    if (gather->matched[i] ||
        vkr_scene_light_group_left_bake(
            gather->scene,
            dynamic_volume_record_slot(gather->scene, record->group))) {
      continue;
    }
    if (record->kind == DYNAMIC_VOLUME_BAKED_RECTANGLE) {
      const VkrRectangleLight baked =
          dynamic_volume_record_rectangle(gather, record);
      dynamic_volume_add_rectangle(gather, &baked, -1.0f);
    } else {
      const VkrPointLight baked = dynamic_volume_record_point(gather, record);
      dynamic_volume_add_point(gather, &baked, -1.0f);
    }
  }
}

/* Emissive rows past the static candidates: the bake holds only static
   emission. */
vkr_internal void dynamic_volume_emitters(DynamicVolumeGather *gather,
                                          const VkrWorldPassPayload *world,
                                          VkrMaterialSystem *materials) {
  if (!world || !world->gpu_candidates || !materials) {
    return;
  }
  for (uint32_t i = world->static_candidate_count;
       i < world->gpu_candidate_count; ++i) {
    const VkrWorldDrawCandidate *candidate = &world->gpu_candidates[i];
    const VkrMaterial *material =
        vkr_material_system_get_live(materials, candidate->material);
    if (!material) {
      continue;
    }
    const float32_t emission =
        dynamic_volume_luminance(material->pbr.emissive_factor);
    if (emission <= 0.0f) {
      continue;
    }
    const Vec4 sphere = candidate->local_bounding_sphere;
    const Mat4 model = candidate->instance.model;
    const Vec4 center =
        mat4_mul_vec4(model, vec4_new(sphere.x, sphere.y, sphere.z, 1.0f));
    const float32_t scale =
        Max(vec3_length(vec3_new(model.m00, model.m10, model.m20)),
            Max(vec3_length(vec3_new(model.m01, model.m11, model.m21)),
                vec3_length(vec3_new(model.m02, model.m12, model.m22))));
    const float32_t radius = sphere.w * scale;
    /* An emitter of radiance L and area about pi r^2 gives L A / d^2. */
    dynamic_volume_add_sphere(
        gather, vec3_new(center.x, center.y, center.z),
        radius +
            dynamic_volume_reach(emission * 3.14159265f * radius * radius));
  }
}

/* Brick b's box, from its lowest indirection entry and level, in render
   space. */
vkr_internal void dynamic_volume_brick_box(const VkrSceneDiffuseVolume *volume,
                                           Vec3 origin, uint32_t b, Vec3 *low,
                                           Vec3 *high) {
  const uint32_t packed = volume->brick_entries[b];
  const uint32_t level = packed >> 30u;
  const float32_t scale = level == 0u ? 1.0f : (level == 1u ? 3.0f : 9.0f);
  const float32_t spacing = volume->spacing;
  *low = vec3_add(origin,
                  vec3_scale(vec3_new((float32_t)(packed & 1023u),
                                      (float32_t)((packed >> 10u) & 1023u),
                                      (float32_t)((packed >> 20u) & 1023u)),
                             3.0f * spacing));
  *high =
      vec3_add(*low, vec3_new(3.0f * scale * spacing, 3.0f * scale * spacing,
                              3.0f * scale * spacing));
}

/* Whether a sphere reaches the box from `low` to `high`. */
vkr_internal bool8_t dynamic_volume_sphere_reaches(Vec3 center,
                                                   float32_t radius, Vec3 low,
                                                   Vec3 high) {
  const float32_t dx = Max(Max(low.x - center.x, center.x - high.x), 0.0f);
  const float32_t dy = Max(Max(low.y - center.y, center.y - high.y), 0.0f);
  const float32_t dz = Max(Max(low.z - center.z, center.z - high.z), 0.0f);
  return dx * dx + dy * dy + dz * dz <= radius * radius;
}

/* The cells, inclusive, that brick b's indirection entries overlap. */
vkr_internal void
dynamic_volume_brick_cells(const VkrSceneDiffuseVolume *volume, uint32_t b,
                           uint32_t low[3], uint32_t high[3]) {
  const uint32_t packed = volume->brick_entries[b];
  const uint32_t level = packed >> 30u;
  const uint32_t scale = level == 0u ? 1u : (level == 1u ? 3u : 9u);
  const uint32_t first[3] = {packed & 1023u, (packed >> 10u) & 1023u,
                             (packed >> 20u) & 1023u};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const uint32_t last = volume->dimensions[axis] - 1u;
    const uint32_t first_entry = Min(first[axis], last);
    const uint32_t last_entry = Min(first[axis] + scale - 1u, last);
    low[axis] = first_entry / DYNAMIC_VOLUME_CELL_ENTRIES;
    high[axis] = Max(first_entry, last_entry) / DYNAMIC_VOLUME_CELL_ENTRIES;
  }
}

vkr_internal uint32_t
dynamic_volume_cell_index(const VkrDynamicVolumeSelection *selection,
                          uint32_t x, uint32_t y, uint32_t z) {
  return (z * selection->cell_dimensions[1] + y) *
             selection->cell_dimensions[0] +
         x;
}

/* Marks brick b active and counts it off every cell it overlaps. */
vkr_internal void
dynamic_volume_mark_brick(VkrDynamicVolumeSelection *selection,
                          const VkrSceneDiffuseVolume *volume, uint32_t b) {
  selection->next_bricks[b / 64u] |= UINT64_C(1) << (b % 64u);
  uint32_t low[3];
  uint32_t high[3];
  dynamic_volume_brick_cells(volume, b, low, high);
  for (uint32_t z = low[2]; z <= high[2]; ++z) {
    for (uint32_t y = low[1]; y <= high[1]; ++y) {
      for (uint32_t x = low[0]; x <= high[0]; ++x) {
        --selection
              ->cell_unmarked[dynamic_volume_cell_index(selection, x, y, z)];
      }
    }
  }
}

/* Marks active the bricks whose boxes a sphere reaches. The sphere walks
   the cells of 4 x 4 x 4 indirection entries inside its bounds and tests
   the bricks each lists that are not yet marked; a cell whose bricks are all
   marked costs one read, so overlapping sources stop paying for bricks an
   earlier one marked. */
vkr_internal void
dynamic_volume_mark_sphere(VkrDynamicVolumeSelection *selection,
                           const VkrSceneDiffuseVolume *volume, Vec3 origin,
                           Vec3 center, float32_t radius) {
  if (!(radius > 0.0f)) {
    return;
  }
  const float32_t entry = 3.0f * volume->spacing;
  const float32_t offset[3] = {center.x - origin.x, center.y - origin.y,
                               center.z - origin.z};
  uint32_t low[3];
  uint32_t high[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    /* Clamped as floats first, so a far source cannot overflow the cast. */
    const float32_t last = (float32_t)volume->dimensions[axis] - 1.0f;
    const float32_t first_entry =
        vkr_floor_f32((offset[axis] - radius) / entry);
    const float32_t last_entry = vkr_floor_f32((offset[axis] + radius) / entry);
    if (!(last_entry >= 0.0f) || !(first_entry <= last)) {
      return;
    }
    low[axis] = (uint32_t)Max(first_entry, 0.0f) / DYNAMIC_VOLUME_CELL_ENTRIES;
    high[axis] = (uint32_t)Min(last_entry, last) / DYNAMIC_VOLUME_CELL_ENTRIES;
  }
  for (uint32_t z = low[2]; z <= high[2]; ++z) {
    for (uint32_t y = low[1]; y <= high[1]; ++y) {
      for (uint32_t x = low[0]; x <= high[0]; ++x) {
        const uint32_t cell = dynamic_volume_cell_index(selection, x, y, z);
        for (uint32_t i = selection->cell_first[cell];
             i < selection->cell_first[cell + 1u] &&
             selection->cell_unmarked[cell] > 0u;
             ++i) {
          const uint32_t b = selection->cell_bricks[i];
          if (selection->next_bricks[b / 64u] & (UINT64_C(1) << (b % 64u))) {
            continue;
          }
          Vec3 box_low;
          Vec3 box_high;
          dynamic_volume_brick_box(volume, origin, b, &box_low, &box_high);
          if (dynamic_volume_sphere_reaches(center, radius, box_low,
                                            box_high)) {
            dynamic_volume_mark_brick(selection, volume, b);
          }
        }
      }
    }
  }
}

void vkr_dynamic_volume_selection_destroy(
    VkrDynamicVolumeSelection *selection) {
  if (!selection) {
    return;
  }
  vkr_heap_free(selection->active_bricks);
  vkr_heap_free(selection->next_bricks);
  vkr_heap_free(selection->active_probes);
  vkr_heap_free(selection->cell_first);
  vkr_heap_free(selection->cell_bricks);
  vkr_heap_free(selection->cell_unmarked);
  vkr_heap_free(selection->point_power);
  vkr_heap_free(selection->point_heap);
  MemZero(selection, sizeof(*selection));
}

/* Sizes the state for `volume`; a different volume starts from no dynamic
   light, which its freshly cleared texture holds. */
vkr_internal bool8_t dynamic_volume_follow(
    VkrDynamicVolumeSelection *selection, const VkrSceneDiffuseVolume *volume) {
  if (selection->volume == volume &&
      selection->brick_count == volume->brick_count) {
    return true_v;
  }
  vkr_dynamic_volume_selection_destroy(selection);
  const uint64_t entries = (uint64_t)volume->dimensions[0] *
                           volume->dimensions[1] * volume->dimensions[2];
  if (entries == 0u || entries > DYNAMIC_VOLUME_ENTRIES_MAX) {
    log_error("Dynamic volume of %llu indirection entries is outside the "
              "brick cells' 1 to %u",
              (unsigned long long)entries, DYNAMIC_VOLUME_ENTRIES_MAX);
    return false_v;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    selection->cell_dimensions[axis] =
        (volume->dimensions[axis] + DYNAMIC_VOLUME_CELL_ENTRIES - 1u) /
        DYNAMIC_VOLUME_CELL_ENTRIES;
  }
  const uint32_t cells = selection->cell_dimensions[0] *
                         selection->cell_dimensions[1] *
                         selection->cell_dimensions[2];
  const uint32_t words = (volume->brick_count + 63u) / 64u;
  selection->active_bricks =
      vkr_heap_alloc_zeroed(Max(words, 1u), sizeof(uint64_t));
  selection->next_bricks =
      vkr_heap_alloc_zeroed(Max(words, 1u), sizeof(uint64_t));
  selection->cell_first = vkr_heap_alloc_zeroed(cells + 1u, sizeof(uint32_t));
  selection->cell_unmarked = vkr_heap_alloc_zeroed(cells, sizeof(uint32_t));
  selection->point_power =
      vkr_heap_alloc_zeroed(VKR_DYNAMIC_VOLUME_LIGHTS_MAX, sizeof(float32_t));
  selection->point_heap =
      vkr_heap_alloc_zeroed(VKR_DYNAMIC_VOLUME_LIGHTS_MAX, sizeof(uint32_t));
  if (!selection->active_bricks || !selection->next_bricks ||
      !selection->cell_first || !selection->cell_unmarked ||
      !selection->point_power || !selection->point_heap) {
    vkr_dynamic_volume_selection_destroy(selection);
    return false_v;
  }
  /* Each cell lists the bricks that overlap it, in brick order: count, sum,
     then fill with cell_unmarked as each cell's cursor. */
  for (uint32_t b = 0u; b < volume->brick_count; ++b) {
    uint32_t low[3];
    uint32_t high[3];
    dynamic_volume_brick_cells(volume, b, low, high);
    for (uint32_t z = low[2]; z <= high[2]; ++z) {
      for (uint32_t y = low[1]; y <= high[1]; ++y) {
        for (uint32_t x = low[0]; x <= high[0]; ++x) {
          ++selection
                ->cell_first[dynamic_volume_cell_index(selection, x, y, z) +
                             1u];
        }
      }
    }
  }
  for (uint32_t cell = 0u; cell < cells; ++cell) {
    selection->cell_first[cell + 1u] += selection->cell_first[cell];
    selection->cell_unmarked[cell] = selection->cell_first[cell];
  }
  selection->cell_bricks =
      vkr_heap_alloc(Max(selection->cell_first[cells], 1u) * sizeof(uint32_t));
  if (!selection->cell_bricks) {
    vkr_dynamic_volume_selection_destroy(selection);
    return false_v;
  }
  for (uint32_t b = 0u; b < volume->brick_count; ++b) {
    uint32_t low[3];
    uint32_t high[3];
    dynamic_volume_brick_cells(volume, b, low, high);
    for (uint32_t z = low[2]; z <= high[2]; ++z) {
      for (uint32_t y = low[1]; y <= high[1]; ++y) {
        for (uint32_t x = low[0]; x <= high[0]; ++x) {
          const uint32_t cell = dynamic_volume_cell_index(selection, x, y, z);
          selection->cell_bricks[selection->cell_unmarked[cell]++] = b;
        }
      }
    }
  }
  selection->volume = volume;
  selection->brick_count = volume->brick_count;
  selection->brick_words = words;
  return true_v;
}

vkr_internal bool8_t dynamic_volume_reserve_probes(
    VkrDynamicVolumeSelection *selection, uint32_t count) {
  if (count <= selection->active_probe_capacity) {
    return true_v;
  }
  uint32_t capacity = Max(selection->active_probe_capacity, 4096u);
  while (capacity < count) {
    capacity *= 2u;
  }
  uint32_t *probes = vkr_heap_realloc(selection->active_probes,
                                      (size_t)capacity * sizeof(uint32_t));
  if (!probes) {
    return false_v;
  }
  selection->active_probes = probes;
  selection->active_probe_capacity = capacity;
  return true_v;
}

bool8_t vkr_dynamic_volume_select(VkrDynamicVolumeSelection *selection,
                                  const VkrScene *scene,
                                  const VkrWorldPassPayload *world,
                                  VkrMaterialSystem *materials,
                                  uint32_t probe_budget,
                                  VkrDynamicVolumeFrame *out) {
  const VkrSceneDiffuseVolume *volume = scene ? scene->diffuse_volume : NULL;
  if (!selection || !out || !volume || !volume->dynamic_sh.id ||
      !volume->brick_entries || !volume->probe_valid || !scene->world ||
      !scene->queries_valid || !dynamic_volume_follow(selection, volume)) {
    return false_v;
  }
  MemZero(&out->input, sizeof(out->input));

  DynamicVolumeSphere spheres[DYNAMIC_VOLUME_SPHERES_MAX];
  uint8_t matched[DYNAMIC_VOLUME_RECORDS_MAX] = {0};
  DynamicVolumeGather gather = {
      .scene = scene,
      .volume = volume,
      .selection = selection,
      .out = out,
      .spheres = spheres,
      .matched = matched,
      .record_count =
          volume->baked_lights_known
              ? Min(volume->baked_light_count, DYNAMIC_VOLUME_RECORDS_MAX)
              : 0u,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights,
      dynamic_volume_point_lights_cb, &gather);
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_rectangle_lights,
      dynamic_volume_rectangle_lights_cb, &gather);
  dynamic_volume_removed_records(&gather);
  dynamic_volume_emitters(&gather, world, materials);

  /* Bricks are placed in document space; lights in render space. */
  const Vec3 origin = vec3_sub(volume->origin, scene->origin_offset);
  MemZero(selection->next_bricks,
          (uint64_t)selection->brick_words * sizeof(uint64_t));
  uint32_t active_probe_count = 0u;
  /* A grid's probes all hold its light and take turns; a bake's probes hold
     dynamic light only within its sources' reach. */
  if (volume->runtime_grid) {
    if (!dynamic_volume_reserve_probes(selection, volume->probe_count)) {
      return false_v;
    }
    for (uint32_t probe = 0u; probe < volume->probe_count; ++probe) {
      selection->active_probes[active_probe_count++] = probe;
    }
  }
  if (!volume->runtime_grid) {
    const uint32_t cells = selection->cell_dimensions[0] *
                           selection->cell_dimensions[1] *
                           selection->cell_dimensions[2];
    for (uint32_t cell = 0u; cell < cells; ++cell) {
      selection->cell_unmarked[cell] =
          selection->cell_first[cell + 1u] - selection->cell_first[cell];
    }
    for (uint32_t i = 0u; i < out->input.point_light_count; ++i) {
      dynamic_volume_mark_sphere(
          selection, volume, origin, out->point_lights[i].position,
          dynamic_volume_point_reach(&out->point_lights[i]));
    }
    for (uint32_t i = 0u; i < gather.sphere_count; ++i) {
      dynamic_volume_mark_sphere(selection, volume, origin,
                                 gather.spheres[i].center,
                                 gather.spheres[i].radius);
    }
  }
  for (uint32_t b = 0u; b < volume->brick_count && !volume->runtime_grid; ++b) {
    if (!(selection->next_bricks[b / 64u] & (UINT64_C(1) << (b % 64u)))) {
      continue;
    }
    if (!dynamic_volume_reserve_probes(selection, active_probe_count + 64u)) {
      return false_v;
    }
    for (uint32_t i = 0u; i < 64u; ++i) {
      const uint32_t probe = b * 64u + i;
      if (probe < volume->probe_count && volume->probe_valid[probe]) {
        selection->active_probes[active_probe_count++] = probe;
      }
    }
  }

  /* Bricks the light left clear; those past the frame's cap stay marked and
     clear on a later frame. */
  VkrDynamicVolumeInput *input = &out->input;
  for (uint32_t b = 0u; b < volume->brick_count && !volume->runtime_grid; ++b) {
    const uint64_t bit = UINT64_C(1) << (b % 64u);
    if (!(selection->active_bricks[b / 64u] & bit) ||
        (selection->next_bricks[b / 64u] & bit)) {
      continue;
    }
    if (input->clear_probe_count + 64u > VKR_DYNAMIC_VOLUME_CLEAR_PROBES_MAX) {
      selection->next_bricks[b / 64u] |= bit;
      continue;
    }
    for (uint32_t i = 0u; i < 64u; ++i) {
      const uint32_t probe = b * 64u + i;
      if (probe < volume->probe_count) {
        out->clear_probes[input->clear_probe_count++] = probe;
      }
    }
  }
  uint64_t *swap = selection->active_bricks;
  selection->active_bricks = selection->next_bricks;
  selection->next_bricks = swap;

  /* The active probes take turns, as many a frame as the renderer's
     budget affords. */
  const uint32_t trace_count =
      Min(active_probe_count,
          Min(probe_budget, VKR_DYNAMIC_VOLUME_TRACE_PROBES_MAX));
  if (active_probe_count) {
    selection->cursor %= active_probe_count;
  }
  for (uint32_t i = 0u; i < trace_count; ++i) {
    out->trace_probes[i] =
        selection->active_probes[(selection->cursor + i) % active_probe_count];
  }
  selection->cursor += trace_count;
  input->trace_probe_count = trace_count;
  if (!input->trace_probe_count && !input->clear_probe_count) {
    return false_v;
  }
  input->trace_probes = out->trace_probes;
  input->clear_probes = out->clear_probes;
  input->point_lights = out->point_lights;
  input->point_light_signs = out->point_light_signs;
  input->rectangle_lights = out->rectangle_lights;
  input->rectangle_light_signs = out->rectangle_light_signs;
  input->blend = VKR_DYNAMIC_VOLUME_BLEND;
  input->sequence = ++selection->sequence;
  return true_v;
}

/* -------------------------------------------------------------------------
   The runtime grid: a scene whose volume is enabled but never baked gets a
   uniform level-0 grid over its static geometry, and the dynamic layer
   traces all of its light (ADR-119).
   ------------------------------------------------------------------------- */

/* The grid covers at most this many bricks (64 probes each), its probe
   spacing never falls below the minimum, in metres, and it keeps the margin
   around the static geometry's bounds. */
#define DYNAMIC_VOLUME_GRID_BRICKS_MAX 1024u
#define DYNAMIC_VOLUME_GRID_SPACING_MIN 0.5f
#define DYNAMIC_VOLUME_GRID_MARGIN 0.5f

/* The render-space bounds of `world`'s static candidates' spheres; false
   without any. */
vkr_internal bool8_t dynamic_volume_static_bounds(
    const VkrWorldPassPayload *world, Vec3 *out_min, Vec3 *out_max) {
  if (!world || !world->gpu_candidates || !world->static_candidate_count) {
    return false_v;
  }
  Vec3 low =
      vec3_new(VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY);
  Vec3 high =
      vec3_new(-VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY);
  for (uint32_t i = 0u; i < world->static_candidate_count; ++i) {
    const VkrWorldDrawCandidate *candidate = &world->gpu_candidates[i];
    const Mat4 model = candidate->instance.model;
    const Vec4 sphere = candidate->local_bounding_sphere;
    const Vec3 center = vec3_new(model.m00 * sphere.x + model.m01 * sphere.y +
                                     model.m02 * sphere.z + model.m03,
                                 model.m10 * sphere.x + model.m11 * sphere.y +
                                     model.m12 * sphere.z + model.m13,
                                 model.m20 * sphere.x + model.m21 * sphere.y +
                                     model.m22 * sphere.z + model.m23);
    const float32_t scale = vkr_sqrt_f32(Max(
        Max(model.m00 * model.m00 + model.m10 * model.m10 +
                model.m20 * model.m20,
            model.m01 * model.m01 + model.m11 * model.m11 +
                model.m21 * model.m21),
        model.m02 * model.m02 + model.m12 * model.m12 + model.m22 * model.m22));
    const float32_t radius = sphere.w * scale;
    if (!vkr_is_finite_f32(center.x) || !vkr_is_finite_f32(center.y) ||
        !vkr_is_finite_f32(center.z) || !vkr_is_finite_f32(radius)) {
      continue;
    }
    low = vec3_new(Min(low.x, center.x - radius), Min(low.y, center.y - radius),
                   Min(low.z, center.z - radius));
    high =
        vec3_new(Max(high.x, center.x + radius), Max(high.y, center.y + radius),
                 Max(high.z, center.z + radius));
  }
  if (!(low.x <= high.x && low.y <= high.y && low.z <= high.z)) {
    return false_v;
  }
  *out_min = low;
  *out_max = high;
  return true_v;
}

/* Whether the grid `volume` still covers `low` to `high`, document space. */
vkr_internal bool8_t dynamic_volume_grid_covers(
    const VkrSceneDiffuseVolume *volume, Vec3 low, Vec3 high) {
  const float32_t span = 3.0f * volume->spacing;
  const Vec3 end = vec3_add(volume->origin,
                            vec3_new(span * (float32_t)volume->dimensions[0],
                                     span * (float32_t)volume->dimensions[1],
                                     span * (float32_t)volume->dimensions[2]));
  return low.x >= volume->origin.x && low.y >= volume->origin.y &&
         low.z >= volume->origin.z && high.x <= end.x && high.y <= end.y &&
         high.z <= end.z;
}

/* Finalizes `prepared` as the grid's immutable texture `name`, holding the
   reference the volume's reset releases. */
vkr_internal bool8_t dynamic_volume_grid_texture(
    VkrRenderAssets *assets, const char *name, VkrTexturePreparedLoad *prepared,
    VkrTextureHandle *out_handle) {
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const bool8_t created = vkr_texture_system_finalize_prepared_load(
      &assets->texture_system,
      string8_create_from_cstr((const uint8_t *)name, string_length(name)),
      prepared, out_handle, &error);
  vkr_texture_system_release_prepared_load(prepared);
  if (!created) {
    String8 error_text = vkr_renderer_get_error_string(error);
    log_error("Dynamic volume: grid texture '%s' failed: %.*s", name,
              (int)error_text.length, error_text.str);
    return false_v;
  }
  vkr_texture_system_add_ref_by_handle(&assets->texture_system, *out_handle);
  return true_v;
}

/* Creates the grid's writable texture `name`; the renderer clears it before
   its first read. */
vkr_internal bool8_t dynamic_volume_grid_writable(
    VkrRenderAssets *assets, const char *name, uint32_t width, uint32_t height,
    VkrTextureFormat format, uint32_t channels, bool8_t filtered,
    VkrTextureHandle *out_handle) {
  const VkrFilter filter = filtered ? VKR_FILTER_LINEAR : VKR_FILTER_NEAREST;
  const VkrTextureDescription description = {
      .width = width,
      .height = height,
      .channels = channels,
      .mip_levels = 1u,
      .array_layers = 1u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = format,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = filter,
      .mag_filter = filter,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
  };
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_create_writable(
          &assets->texture_system,
          string8_create_from_cstr((const uint8_t *)name, string_length(name)),
          &description, out_handle, &error)) {
    String8 error_text = vkr_renderer_get_error_string(error);
    log_error("Dynamic volume: grid texture '%s' failed: %.*s", name,
              (int)error_text.length, error_text.str);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_dynamic_volume_grid_update(VkrScene *scene, VkrRenderAssets *assets,
                                       const VkrWorldPassPayload *world) {
  if (!scene || !assets || !scene->world_state.diffuse_volume.enabled ||
      !scene->diffuse_volume_unbaked) {
    return false_v;
  }
  /* The grid traces every lamp; lamp lightmaps would add the baked lamps a
     second time (ADR-119). */
  if (scene->lightmaps && scene->lightmaps->lamps_baked) {
    static bool8_t s_warned = false_v;
    if (!s_warned) {
      log_warn("Dynamic volume: the runtime grid needs a scene without baked "
               "lamp lightmaps; the volume stays off");
      s_warned = true_v;
    }
    return false_v;
  }
  if (scene->diffuse_volume && !scene->diffuse_volume->runtime_grid) {
    return false_v;
  }
  Vec3 low;
  Vec3 high;
  if (!dynamic_volume_static_bounds(world, &low, &high)) {
    return scene->diffuse_volume != NULL;
  }
  /* Bricks are placed in document space (ADR-086). */
  low = vec3_add(low, scene->origin_offset);
  high = vec3_add(high, scene->origin_offset);
  if (scene->diffuse_volume &&
      dynamic_volume_grid_covers(scene->diffuse_volume, low, high)) {
    return true_v;
  }

  const Vec3 margin =
      vec3_new(DYNAMIC_VOLUME_GRID_MARGIN, DYNAMIC_VOLUME_GRID_MARGIN,
               DYNAMIC_VOLUME_GRID_MARGIN);
  low = vec3_sub(low, margin);
  high = vec3_add(high, margin);
  const Vec3 extent = vec3_sub(high, low);
  /* The finest spacing whose level-0 entries, 3 spacings wide, number at
     most the cap. */
  float32_t spacing =
      Max(vkr_cbrt_f32(extent.x * extent.y * extent.z /
                       (float32_t)DYNAMIC_VOLUME_GRID_BRICKS_MAX) /
              3.0f,
          DYNAMIC_VOLUME_GRID_SPACING_MIN);
  uint32_t dimensions[3] = {0u, 0u, 0u};
  for (;;) {
    const float32_t span = 3.0f * spacing;
    dimensions[0] = Max((uint32_t)vkr_ceil_f32(extent.x / span), 1u);
    dimensions[1] = Max((uint32_t)vkr_ceil_f32(extent.y / span), 1u);
    dimensions[2] = Max((uint32_t)vkr_ceil_f32(extent.z / span), 1u);
    if ((uint64_t)dimensions[0] * dimensions[1] * dimensions[2] <=
        DYNAMIC_VOLUME_GRID_BRICKS_MAX) {
      break;
    }
    spacing *= 1.05f;
  }
  const uint32_t brick_count = dimensions[0] * dimensions[1] * dimensions[2];
  const uint32_t probe_count = brick_count * 64u;
  const uint32_t rows = (probe_count + VKR_DIFFUSE_VOLUME_ROW_PROBES - 1u) /
                        VKR_DIFFUSE_VOLUME_ROW_PROBES;
  const uint32_t tile_rows =
      (probe_count + VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES - 1u) /
      VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES;

  if (scene->diffuse_volume) {
    vkr_scene_reset_diffuse_volume(scene, assets);
  }
  VkrSceneDiffuseVolume *volume = vkr_heap_alloc_zeroed(1u, sizeof(*volume));
  uint32_t *entries = vkr_heap_alloc_zeroed(brick_count, sizeof(uint32_t));
  uint32_t *bricks = vkr_heap_alloc_zeroed(
      (size_t)rows * VKR_DIFFUSE_VOLUME_ROW_PROBES, sizeof(uint32_t));
  uint32_t *brick_entries =
      vkr_heap_alloc_zeroed(brick_count, sizeof(uint32_t));
  uint8_t *probe_valid = vkr_heap_alloc(probe_count);
  /* One empty sun-key layer: the binding needs a layer, and the dynamic
     layer holds all of the grid's light. */
  const uint64_t layer_bytes = (uint64_t)rows * VKR_DIFFUSE_VOLUME_ROW_PROBES *
                               3u * 4u * sizeof(uint16_t);
  uint8_t *layer_sh = vkr_heap_alloc_zeroed(1u, layer_bytes);
  if (!volume || !entries || !bricks || !brick_entries || !probe_valid ||
      !layer_sh) {
    vkr_heap_free(volume);
    vkr_heap_free(entries);
    vkr_heap_free(bricks);
    vkr_heap_free(brick_entries);
    vkr_heap_free(probe_valid);
    vkr_heap_free(layer_sh);
    return false_v;
  }
  /* Entry (x, y, z) is brick x + nx (y + ny z), whose level-0 bits are its
     own index. */
  for (uint32_t z = 0u; z < dimensions[2]; ++z) {
    for (uint32_t y = 0u; y < dimensions[1]; ++y) {
      for (uint32_t x = 0u; x < dimensions[0]; ++x) {
        const uint32_t b = x + dimensions[0] * (y + dimensions[1] * z);
        entries[b] = b;
        bricks[b] = x | y << 10u | z << 20u;
        brick_entries[b] = bricks[b];
      }
    }
  }
  MemSet(probe_valid, 1, probe_count);
  *volume = (VkrSceneDiffuseVolume){
      .origin = low,
      .spacing = spacing,
      .sh_scale = 1.0f,
      .dimensions = {dimensions[0], dimensions[1], dimensions[2]},
      .probe_count = probe_count,
      .layers = {{.kind = VKR_LIGHT_LAYER_SUN_KEY,
                  .index = 0u,
                  .sun_direction = vec3_new(0.0f, 1.0f, 0.0f)}},
      .layer_count = 1u,
      .brick_entries = brick_entries,
      .brick_count = brick_count,
      .probe_valid = probe_valid,
      /* No record: every scene light is light the empty layer lacks. */
      .baked_lights_known = true_v,
      .runtime_grid = true_v,
  };
  scene->diffuse_volume = volume;

  static uint32_t s_grid_serial = 0u;
  const uint32_t serial = ++s_grid_serial;
  char name[96];
  /* Each prepared texture takes its pixels, freeing them on failure; the
     ones a failure leaves unclaimed are freed here. */
  VkrTexturePreparedLoad prepared;
  bool8_t ready = false_v;
  string_format(name, sizeof(name), "diffuse_volume#grid#%u#indirection",
                serial);
  const bool8_t entries_ready = vkr_texture_prepared_load_2d(
      (uint8_t *)entries, (uint64_t)brick_count * sizeof(uint32_t),
      dimensions[0], dimensions[1] * dimensions[2], VKR_TEXTURE_FORMAT_R32_UINT,
      1u, false_v, &prepared);
  if (entries_ready && dynamic_volume_grid_texture(assets, name, &prepared,
                                                   &volume->indirection)) {
    string_format(name, sizeof(name), "diffuse_volume#grid#%u#bricks", serial);
    const bool8_t bricks_ready = vkr_texture_prepared_load_2d(
        (uint8_t *)bricks,
        (uint64_t)rows * VKR_DIFFUSE_VOLUME_ROW_PROBES * sizeof(uint32_t),
        VKR_DIFFUSE_VOLUME_ROW_PROBES, rows, VKR_TEXTURE_FORMAT_R32_UINT, 1u,
        false_v, &prepared);
    bricks = NULL;
    if (bricks_ready &&
        dynamic_volume_grid_texture(assets, name, &prepared, &volume->bricks)) {
      string_format(name, sizeof(name), "diffuse_volume#grid#%u#layers",
                    serial);
      const bool8_t layers_ready = vkr_texture_prepared_load_2d(
          layer_sh, layer_bytes, VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u, rows,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, 4u, false_v, &prepared);
      layer_sh = NULL;
      ready = layers_ready && dynamic_volume_grid_texture(
                                  assets, name, &prepared, &volume->layer_sh);
    }
  }
  vkr_heap_free(bricks);
  vkr_heap_free(layer_sh);
  string_format(name, sizeof(name), "diffuse_volume#grid#%u#probes", serial);
  ready = ready && dynamic_volume_grid_writable(
                       assets, name, VKR_DIFFUSE_VOLUME_ROW_PROBES, rows,
                       VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, 4u, false_v,
                       &volume->probes);
  string_format(name, sizeof(name), "diffuse_volume#grid#%u#moments", serial);
  ready =
      ready &&
      dynamic_volume_grid_writable(
          assets, name,
          VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES * VKR_DIFFUSE_VOLUME_MOMENT_TILE,
          tile_rows * VKR_DIFFUSE_VOLUME_MOMENT_TILE,
          VKR_TEXTURE_FORMAT_R16G16_SFLOAT, 2u, true_v, &volume->moments);
  string_format(name, sizeof(name), "diffuse_volume#grid#%u#sh", serial);
  ready = ready &&
          dynamic_volume_grid_writable(
              assets, name, VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u, 2u * rows,
              VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, 4u, false_v, &volume->sh);
  if (!ready) {
    vkr_scene_reset_diffuse_volume(scene, assets);
    return false_v;
  }
  vkr_scene_light_layer_weights(scene, volume->layers, volume->layer_count,
                                volume->weights);
  volume->since_compose = 0.0;
  volume->composition = 1u;
  log_info("Dynamic volume: runtime grid %ux%ux%u bricks at %.2f m spacing, "
           "%u probes",
           dimensions[0], dimensions[1], dimensions[2], (double)spacing,
           probe_count);
  return true_v;
}

/* -------------------------------------------------------------------------
   Groups that left the bake (ADR-119): lamp lightmaps hold light per group
   and cannot take one lamp away, so with them a group with any changed
   static light leaves the bake whole. Without them, the layer takes a
   changed light's baked light away by itself.
   ------------------------------------------------------------------------- */

typedef struct DynamicVolumeGroupScan {
  const VkrScene *scene;
  const VkrSceneDiffuseVolume *volume;
  uint8_t *matched;
  uint32_t record_count;
  uint32_t changed;
} DynamicVolumeGroupScan;

/* The first unmatched record of `kind` at `render_position`, marked. */
vkr_internal uint32_t dynamic_volume_scan_match(DynamicVolumeGroupScan *scan,
                                                uint32_t kind,
                                                Vec3 render_position) {
  const Vec3 position = vec3_add(render_position, scan->scene->origin_offset);
  for (uint32_t i = 0u; i < scan->record_count; ++i) {
    const VkrSceneBakedLight *record = &scan->volume->baked_lights[i];
    if (scan->matched[i] || record->kind != kind ||
        vec3_distance(record->position, position) >
            DYNAMIC_VOLUME_MATCH_DISTANCE) {
      continue;
    }
    scan->matched[i] = 1u;
    return i;
  }
  return UINT32_MAX;
}

vkr_internal void dynamic_volume_scan_mark(DynamicVolumeGroupScan *scan,
                                           uint8_t slot) {
  if (slot < VKR_SCENE_LIGHT_GROUP_MAX) {
    scan->changed |= 1u << slot;
  }
}

vkr_internal void dynamic_volume_scan_points_cb(const VkrArchetype *arch,
                                                VkrChunk *chunk, void *user) {
  (void)arch;
  DynamicVolumeGroupScan *scan = user;
  const VkrScene *scene = scan->scene;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);
  if (!transforms || !lights) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const ScenePointLight *light = &lights[i];
    if (light->mobility != VKR_LIGHT_MOBILITY_STATIC || !light->enabled) {
      continue;
    }
    const uint32_t record_index =
        dynamic_volume_scan_match(scan, dynamic_volume_baked_kind(light->kind),
                                  mat4_position(transforms[i].world));
    const Vec3 direction = vkr_scene_light_world_direction(
        scene, &transforms[i], light->direction_local);
    if (record_index == UINT32_MAX ||
        !dynamic_volume_point_unchanged(
            &scan->volume->baked_lights[record_index], light, direction)) {
      dynamic_volume_scan_mark(scan, light->light_group_slot);
    }
  }
}

vkr_internal void dynamic_volume_scan_rectangles_cb(const VkrArchetype *arch,
                                                    VkrChunk *chunk,
                                                    void *user) {
  (void)arch;
  DynamicVolumeGroupScan *scan = user;
  const VkrScene *scene = scan->scene;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneRectangleLight *lights = (SceneRectangleLight *)vkr_entity_chunk_column(
      chunk, scene->comp_rectangle_light);
  if (!transforms || !lights) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const SceneRectangleLight *light = &lights[i];
    if (light->mobility != VKR_LIGHT_MOBILITY_STATIC || !light->enabled) {
      continue;
    }
    const Mat4 world = transforms[i].world;
    const VkrRectangleLight rectangle = {
        .position = mat4_position(world),
        .right = vec3_normalize(vec3_new(world.m00, world.m10, world.m20)),
        .up = vec3_normalize(vec3_new(world.m01, world.m11, world.m21)),
        .color = light->color,
        .half_width = 0.5f * light->size.x,
        .half_height = 0.5f * light->size.y,
        .radiance = light->radiance,
    };
    const uint32_t record_index = dynamic_volume_scan_match(
        scan, DYNAMIC_VOLUME_BAKED_RECTANGLE, rectangle.position);
    if (record_index == UINT32_MAX ||
        !dynamic_volume_rectangle_unchanged(
            &scan->volume->baked_lights[record_index], light, &rectangle)) {
      dynamic_volume_scan_mark(scan, light->light_group_slot);
    }
  }
}

void vkr_dynamic_volume_update_baked_groups(VkrScene *scene, bool8_t traced) {
  if (!scene) {
    return;
  }
  const VkrSceneDiffuseVolume *volume = scene->diffuse_volume;
  const bool8_t lamp_lightmaps = scene->lightmaps &&
                                 scene->lightmaps->texture.id != 0u &&
                                 scene->lightmaps->lamps_baked;
  uint32_t changed = 0u;
  if (traced && lamp_lightmaps && volume && volume->baked_lights_known &&
      !volume->runtime_grid && scene->world && scene->queries_valid) {
    uint8_t matched[DYNAMIC_VOLUME_RECORDS_MAX] = {0};
    DynamicVolumeGroupScan scan = {
        .scene = scene,
        .volume = volume,
        .matched = matched,
        .record_count =
            Min(volume->baked_light_count, DYNAMIC_VOLUME_RECORDS_MAX),
    };
    vkr_entity_query_compiled_each_chunk(
        (VkrQueryCompiled *)&scene->query_point_lights,
        dynamic_volume_scan_points_cb, &scan);
    vkr_entity_query_compiled_each_chunk(
        (VkrQueryCompiled *)&scene->query_rectangle_lights,
        dynamic_volume_scan_rectangles_cb, &scan);
    /* A record no current light matched was removed or moved. */
    for (uint32_t i = 0u; i < scan.record_count; ++i) {
      if (!matched[i]) {
        dynamic_volume_scan_mark(
            &scan,
            dynamic_volume_record_slot(scene, volume->baked_lights[i].group));
      }
    }
    changed = scan.changed;
  }
  if (changed != scene->light_groups.left_bake) {
    log_info("Dynamic volume: light groups that left the bake 0x%x -> 0x%x",
             scene->light_groups.left_bake, changed);
  }
  scene->light_groups.left_bake = changed;
}
