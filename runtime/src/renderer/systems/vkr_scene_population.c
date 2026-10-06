#include "renderer/systems/vkr_scene_population.h"

#include "core/logger.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define POPULATION_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Metres terrain samples must lie above a ray's hit to replace it, past the
   gap between a height field body's triangles and the bilinear samples. */
#define POPULATION_GROUND_TOLERANCE 0.05f

typedef enum PopulationKind {
  POPULATION_SPLINE_MESH = 0,
  POPULATION_SCATTER,
} PopulationKind;

typedef struct PopulationRule {
  VkrEntityId entity;
  PopulationKind kind;
  /* VKR_SCENE_POPULATION_RULE_INSTANCE_MAX handles, allocated with the
     first copies. */
  VkrMeshInstanceHandle *instances;
  uint32_t instance_count;
  /* The mesh the instances are of. */
  char mesh[SCENE_POPULATION_MESH_CAPACITY];
  uint32_t mesh_index;
  /* Inputs the instances were built from; zero forces a build. */
  uint64_t signature;
  bool8_t visible;
  char status[160];
} PopulationRule;

struct s_VkrScenePopulation {
  PopulationRule rules[VKR_SCENE_POPULATION_RULE_MAX];
  uint32_t rule_count;
  uint32_t instance_total;
};

// =============================================================================
// Rules
// =============================================================================

static VkrScenePopulation *population_state(VkrScene *scene, bool8_t create) {
  if (!scene->population && create) {
    scene->population =
        vkr_allocator_alloc(scene->alloc, sizeof(*scene->population),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (scene->population) {
      MemZero(scene->population, sizeof(*scene->population));
    }
  }
  return scene->population;
}

static PopulationRule *population_find(const VkrScene *scene,
                                       VkrEntityId entity) {
  VkrScenePopulation *state = scene ? scene->population : NULL;
  for (uint32_t i = 0; state && i < state->rule_count; ++i) {
    if (state->rules[i].entity.u64 == entity.u64) {
      return &state->rules[i];
    }
  }
  return NULL;
}

static void population_release_instances(VkrScene *scene,
                                         PopulationRule *rule) {
  struct VkrRenderAssets *assets = scene->assets;
  for (uint32_t i = 0; assets && i < rule->instance_count; ++i) {
    (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager,
                                            rule->instances[i]);
  }
  scene->population->instance_total -= rule->instance_count;
  rule->instance_count = 0u;
  rule->mesh[0] = '\0';
}

static void population_remove(VkrScene *scene, PopulationRule *rule) {
  VkrScenePopulation *state = scene->population;
  population_release_instances(scene, rule);
  if (rule->instances) {
    vkr_allocator_free(scene->alloc, rule->instances,
                       sizeof(VkrMeshInstanceHandle) *
                           VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
                       POPULATION_TAG);
  }
  *rule = state->rules[--state->rule_count];
}

void vkr_scene_population_changed(VkrScene *scene, VkrEntityId entity,
                                  const VkrTypeDesc *type) {
  /* Splines and their points change rules through the signature. */
  if (type != &vkr_scene_spline_mesh_type && type != &vkr_scene_scatter_type) {
    return;
  }
  const bool8_t present = vkr_scene_get_typed(scene, entity, type) != NULL;
  PopulationRule *rule = population_find(scene, entity);
  if (!present) {
    if (rule && (rule->kind == POPULATION_SPLINE_MESH) ==
                    (type == &vkr_scene_spline_mesh_type)) {
      population_remove(scene, rule);
    }
    return;
  }
  if (rule) {
    rule->signature = 0u;
    return;
  }
  VkrScenePopulation *state = population_state(scene, true_v);
  if (!state || state->rule_count == VKR_SCENE_POPULATION_RULE_MAX) {
    log_warn("Scene: a scene holds at most %u spline meshes and scatters",
             VKR_SCENE_POPULATION_RULE_MAX);
    return;
  }
  rule = &state->rules[state->rule_count++];
  MemZero(rule, sizeof(*rule));
  rule->entity = entity;
  rule->kind = type == &vkr_scene_spline_mesh_type ? POPULATION_SPLINE_MESH
                                                   : POPULATION_SCATTER;
}

void vkr_scene_population_entity_destroying(VkrScene *scene,
                                            VkrEntityId entity) {
  PopulationRule *rule = population_find(scene, entity);
  if (rule) {
    population_remove(scene, rule);
  }
}

void vkr_scene_population_shutdown(VkrScene *scene) {
  VkrScenePopulation *state = scene ? scene->population : NULL;
  if (!state) {
    return;
  }
  while (state->rule_count) {
    population_remove(scene, &state->rules[state->rule_count - 1u]);
  }
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->population = NULL;
}

void vkr_scene_population_each_owner(const VkrScene *scene,
                                     void (*visit)(void *context,
                                                   VkrEntityId entity),
                                     void *context) {
  const VkrScenePopulation *state = scene ? scene->population : NULL;
  for (uint32_t i = 0; state && i < state->rule_count; ++i) {
    if (state->rules[i].instance_count) {
      visit(context, state->rules[i].entity);
    }
  }
}

uint32_t vkr_scene_population_instances(const VkrScene *scene,
                                        VkrEntityId entity) {
  const PopulationRule *rule = population_find(scene, entity);
  return rule ? rule->instance_count : 0u;
}

const char *vkr_scene_population_status(const VkrScene *scene,
                                        VkrEntityId entity) {
  const PopulationRule *rule = population_find(scene, entity);
  return rule && rule->status[0] ? rule->status : NULL;
}

// =============================================================================
// Splines
// =============================================================================

uint32_t vkr_scene_spline_points(const VkrScene *scene, VkrEntityId spline,
                                 Vec3 *out, uint32_t capacity,
                                 bool8_t *out_closed) {
  const SceneSpline *curve =
      vkr_scene_get_typed(scene, spline, &vkr_scene_spline_type);
  if (out_closed) {
    *out_closed = curve ? curve->closed : false_v;
  }
  if (!curve) {
    return 0u;
  }
  uint32_t child_count = 0u;
  const VkrEntityId *children =
      vkr_scene_get_children(scene, spline, &child_count);
  float32_t orders[VKR_SPLINE_POINT_MAX];
  uint32_t count = 0u;
  for (uint32_t i = 0;
       i < child_count && count < capacity && count < VKR_SPLINE_POINT_MAX;
       ++i) {
    const SceneSplinePoint *point =
        vkr_scene_get_typed(scene, children[i], &vkr_scene_spline_point_type);
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, children[i], scene->comp_transform);
    if (!point || !transform) {
      continue;
    }
    /* Insertion keeps the points sorted by order. */
    uint32_t at = count;
    while (at > 0u && orders[at - 1u] > point->order) {
      orders[at] = orders[at - 1u];
      out[at] = out[at - 1u];
      at--;
    }
    orders[at] = point->order;
    out[at] = transform->position;
    count++;
  }
  return count;
}

uint32_t vkr_scene_spline_world_points(const VkrScene *scene,
                                       VkrEntityId spline, Vec3 *out,
                                       uint32_t capacity, bool8_t *out_closed) {
  const uint32_t count =
      vkr_scene_spline_points(scene, spline, out, capacity, out_closed);
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, spline, scene->comp_transform);
  for (uint32_t i = 0; transform && i < count; ++i) {
    out[i] = mat4_mul_vec3(transform->world, out[i]);
  }
  return count;
}

// =============================================================================
// Placement
// =============================================================================

/* FNV-1a over `bytes`, folded into `hash`. */
static uint64_t population_hash(uint64_t hash, const void *bytes,
                                uint64_t size) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  }
  return hash;
}

/* A float in [0, 1) from a seed, an index and a stream. */
static float32_t population_random(uint32_t seed, uint32_t index,
                                   uint32_t stream) {
  uint32_t x = seed * 0x9E3779B9u ^ index * 0x85EBCA6Bu ^ stream * 0xC2B2AE35u;
  x ^= x >> 16u;
  x *= 0x7FEB352Du;
  x ^= x >> 15u;
  x *= 0x846CA68Bu;
  x ^= x >> 16u;
  return (float32_t)(x >> 8u) / 16777216.0f;
}

/* A copy's model: axes `right`, `up` and `forward` (orthonormal) at
   `position`, then the rule's `rotation` (degrees) and `scale`. */
static Mat4 population_frame(Vec3 position, Vec3 right, Vec3 up, Vec3 forward,
                             Vec3 rotation, Vec3 scale) {
  const float32_t to_radians = 3.14159265358979f / 180.0f;
  const Mat4 turn = vkr_quat_to_mat4(
      vkr_quat_from_euler(rotation.x * to_radians, rotation.y * to_radians,
                          rotation.z * to_radians));
  Mat4 model = mat4_identity();
  model.cols[0] =
      vec4_new(right.x * scale.x, right.y * scale.x, right.z * scale.x, 0.0f);
  model.cols[1] =
      vec4_new(up.x * scale.y, up.y * scale.y, up.z * scale.y, 0.0f);
  model.cols[2] = vec4_new(forward.x * scale.z, forward.y * scale.z,
                           forward.z * scale.z, 0.0f);
  model.cols[3] = vec4_new(position.x, position.y, position.z, 1.0f);
  return mat4_mul(model, turn);
}

/* Right and forward axes perpendicular to `up`, forward turned by `yaw`. */
static void population_axes(Vec3 up, float32_t yaw, Vec3 *out_right,
                            Vec3 *out_forward) {
  const Vec3 reference = fabsf(up.y) < 0.99f ? vec3_new(0.0f, 1.0f, 0.0f)
                                             : vec3_new(0.0f, 0.0f, 1.0f);
  Vec3 forward = vec3_normalize(vec3_cross(vec3_cross(up, reference), up));
  if (fabsf(up.y) >= 0.99f) {
    forward = vec3_new(0.0f, 0.0f, up.y > 0.0f ? 1.0f : -1.0f);
  }
  const Vec3 side = vec3_normalize(vec3_cross(up, forward));
  forward =
      vec3_add(vec3_scale(forward, cosf(yaw)), vec3_scale(side, sinf(yaw)));
  *out_forward = forward;
  *out_right = vec3_normalize(vec3_cross(up, forward));
}

/* Copies along a spline: one every `spacing` metres, upright unless they
   follow the slope. */
static uint32_t population_spline_models(const VkrScene *scene,
                                         VkrEntityId entity,
                                         const SceneSplineMesh *rule,
                                         VkrAllocator *scratch, Mat4 *out,
                                         uint32_t capacity) {
  Vec3 points[VKR_SPLINE_POINT_MAX];
  bool8_t closed = false_v;
  const uint32_t count = vkr_scene_spline_points(scene, entity, points,
                                                 VKR_SPLINE_POINT_MAX, &closed);
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (count < 2u || !transform) {
    return 0u;
  }
  VkrSplineSample *samples = vkr_allocator_alloc(
      scratch, sizeof(VkrSplineSample) * capacity, POPULATION_TAG);
  if (!samples) {
    return 0u;
  }
  const uint32_t sampled = vkr_spline_sample(points, count, closed,
                                             rule->spacing, samples, capacity);
  for (uint32_t i = 0; i < sampled; ++i) {
    Vec3 forward = samples[i].tangent;
    if (!rule->follow_slope) {
      forward.y = 0.0f;
      forward = vec3_length(forward) > 1.0e-4f ? vec3_normalize(forward)
                                               : vec3_new(0.0f, 0.0f, 1.0f);
    }
    const Vec3 right =
        vec3_normalize(vec3_cross(vec3_new(0.0f, 1.0f, 0.0f), forward));
    const Vec3 up = vec3_cross(forward, right);
    const Vec3 position =
        vec3_add(samples[i].position,
                 vec3_add(vec3_scale(right, rule->offset.x),
                          vec3_add(vec3_scale(up, rule->offset.y),
                                   vec3_scale(forward, rule->offset.z))));
    out[i] = mat4_mul(transform->world,
                      population_frame(position, right, up, forward,
                                       rule->rotation, rule->scale));
  }
  return sampled;
}

/* Seeded copies in the scatter's box, each dropped straight down onto the
   first surface below the box's top; a column that meets nothing places
   none. A streamed terrain has collision only near its streaming sources,
   so terrain samples answer where they lie above what the ray met. */
static uint32_t population_scatter_models(VkrScene *scene, VkrEntityId entity,
                                          const SceneScatter *rule, Mat4 *out,
                                          uint32_t capacity) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (!transform) {
    return 0u;
  }
  uint32_t placed = 0u;
  const uint32_t wanted = Min(rule->count, capacity);
  for (uint32_t i = 0; i < wanted; ++i) {
    const float32_t u = population_random(rule->seed, i, 0u) * 2.0f - 1.0f;
    const float32_t v = population_random(rule->seed, i, 1u) * 2.0f - 1.0f;
    const Vec3 top = mat4_mul_vec3(
        transform->world,
        vec3_new(u * rule->extents.x, rule->extents.y, v * rule->extents.z));
    const Vec3 bottom = mat4_mul_vec3(
        transform->world,
        vec3_new(u * rule->extents.x, -rule->extents.y, v * rule->extents.z));
    VkrPhysicsRayHit hit = {0};
    const bool8_t struck =
        vkr_scene_physics_raycast(scene, top, vec3_sub(bottom, top), &hit);
    Vec3 position = vec3_new(hit.position[0], hit.position[1], hit.position[2]);
    Vec3 normal = vec3_new(hit.normal[0], hit.normal[1], hit.normal[2]);
    Vec3 ground = vec3_zero();
    Vec3 ground_normal = vec3_zero();
    if (vkr_scene_terrain_ground(scene, top, bottom.y, &ground,
                                 &ground_normal) &&
        (!struck || ground.y > position.y + POPULATION_GROUND_TOLERANCE)) {
      position = ground;
      normal = ground_normal;
    } else if (!struck) {
      continue;
    }
    Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
    if (rule->align_to_surface && vec3_length(normal) > 1.0e-4f) {
      up = vec3_normalize(normal);
    }
    const float32_t yaw =
        rule->random_yaw ? population_random(rule->seed, i, 2u) * 6.28318530718f
                         : 0.0f;
    const float32_t lo = Min(rule->scale_min, rule->scale_max);
    const float32_t hi = Max(rule->scale_min, rule->scale_max);
    const float32_t scale =
        lo + (hi - lo) * population_random(rule->seed, i, 3u);
    Vec3 right;
    Vec3 forward;
    population_axes(up, yaw, &right, &forward);
    out[placed++] =
        population_frame(position, right, up, forward, rule->rotation,
                         vec3_new(scale, scale, scale));
  }
  return placed;
}

/* Inputs the rule's copies follow; a different value rebuilds them. */
static uint64_t population_signature(const VkrScene *scene,
                                     const PopulationRule *rule) {
  const VkrTypeDesc *type = rule->kind == POPULATION_SPLINE_MESH
                                ? &vkr_scene_spline_mesh_type
                                : &vkr_scene_scatter_type;
  const void *value = vkr_scene_get_typed(scene, rule->entity, type);
  const SceneTransform *transform = vkr_entity_get_component(
      scene->world, rule->entity, scene->comp_transform);
  uint64_t hash = 0xcbf29ce484222325ull;
  if (!value || !transform) {
    return 0u;
  }
  hash = population_hash(hash, value, type->size);
  hash = population_hash(hash, &transform->world, sizeof(transform->world));
  const bool8_t visible = vkr_scene_entity_visible(scene, rule->entity);
  hash = population_hash(hash, &visible, sizeof(visible));
  if (rule->kind == POPULATION_SPLINE_MESH) {
    Vec3 points[VKR_SPLINE_POINT_MAX];
    bool8_t closed = false_v;
    const uint32_t count = vkr_scene_spline_points(
        scene, rule->entity, points, VKR_SPLINE_POINT_MAX, &closed);
    hash = population_hash(hash, points, sizeof(Vec3) * count);
    hash = population_hash(hash, &closed, sizeof(closed));
  } else {
    const uint64_t revision = vkr_scene_terrain_revision(scene);
    hash = population_hash(hash, &revision, sizeof(revision));
  }
  return hash ? hash : 1u;
}

/* Creates `count` instances of the rule's mesh; false with the reason in
   the rule's status. */
static bool8_t population_create(VkrScene *scene, PopulationRule *rule,
                                 const char *mesh, uint32_t mesh_index,
                                 uint32_t count) {
  struct VkrRenderAssets *assets = scene->assets;
  const String8 path =
      string8_create_from_cstr((const uint8_t *)mesh, strlen(mesh));
  VkrResourceHandleInfo request = {0};
  VkrResourceHandleInfo resolved = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  bool8_t ok = false_v;
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  if (!vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_MESH, path,
                                     &assets->scratch_allocator, &request,
                                     &error) ||
      !vkr_resource_system_try_get_resolved(&request, &resolved) ||
      !resolved.as.mesh) {
    snprintf(rule->status, sizeof(rule->status), "The mesh '%s' did not load",
             mesh);
    goto cleanup;
  }
  const VkrMeshSource *source = &resolved.as.mesh->source;
  if (mesh_index >= source->meshes.length ||
      !source->meshes.data[mesh_index].range_count) {
    snprintf(rule->status, sizeof(rule->status),
             "'%s' has no mesh %u with geometry", mesh, mesh_index);
    goto cleanup;
  }
  uint32_t render_id = 0u;
  (void)vkr_scene_ensure_render_id(scene, rule->entity, &render_id);
  const uint32_t picking_id =
      render_id ? scene->render_id_base + render_id : 0u;
  const VkrMeshLoadDesc desc = {
      .mesh_path = path,
      .source_mesh_index_plus_one = mesh_index + 1u,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .transform = vkr_transform_identity(),
  };
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMeshInstanceHandle instance =
        vkr_mesh_manager_create_instance_from_resource(
            &assets->mesh_manager, &desc, &resolved, picking_id, rule->visible,
            &error);
    if (!instance.id) {
      snprintf(rule->status, sizeof(rule->status),
               "Only %u of %u copies fit the mesh manager", i, count);
      break;
    }
    (void)vkr_mesh_manager_instance_set_shadow_mobility(
        &assets->mesh_manager, instance, VKR_SHADOW_CASTER_MOBILITY_STATIC);
    rule->instances[rule->instance_count++] = instance;
    scene->population->instance_total++;
  }
  snprintf(rule->mesh, sizeof(rule->mesh), "%s", mesh);
  rule->mesh_index = mesh_index;
  ok = true_v;

cleanup:
  if (request.as.mesh || request.request_id) {
    vkr_resource_system_unload(&request, path);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return ok;
}

/* Places the rule's copies anew. */
static void population_build(VkrScene *scene, PopulationRule *rule,
                             uint64_t signature) {
  struct VkrRenderAssets *assets = scene->assets;
  VkrScenePopulation *state = scene->population;
  rule->signature = signature;
  rule->status[0] = '\0';
  rule->visible = vkr_scene_entity_visible(scene, rule->entity);
  if (!rule->instances) {
    rule->instances = vkr_allocator_alloc(
        scene->alloc,
        sizeof(VkrMeshInstanceHandle) * VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
        POPULATION_TAG);
    if (!rule->instances) {
      snprintf(rule->status, sizeof(rule->status), "Out of memory");
      return;
    }
  }
  const char *mesh = NULL;
  uint32_t mesh_index = 0u;
  const SceneSplineMesh *spline_mesh = NULL;
  const SceneScatter *scatter = NULL;
  if (rule->kind == POPULATION_SPLINE_MESH) {
    spline_mesh =
        vkr_scene_get_typed(scene, rule->entity, &vkr_scene_spline_mesh_type);
    mesh = spline_mesh ? spline_mesh->mesh : NULL;
    mesh_index = spline_mesh ? spline_mesh->mesh_index : 0u;
  } else {
    scatter = vkr_scene_get_typed(scene, rule->entity, &vkr_scene_scatter_type);
    mesh = scatter ? scatter->mesh : NULL;
    mesh_index = scatter ? scatter->mesh_index : 0u;
  }
  if (!mesh || !mesh[0]) {
    population_release_instances(scene, rule);
    snprintf(rule->status, sizeof(rule->status), "No mesh is named");
    return;
  }

  /* The scene's bound counts this rule's present copies as free. */
  const uint32_t room = Min(VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
                            VKR_SCENE_POPULATION_INSTANCE_MAX -
                                (state->instance_total - rule->instance_count));
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  Mat4 *models = vkr_allocator_alloc(&assets->scratch_allocator,
                                     sizeof(Mat4) * room, POPULATION_TAG);
  uint32_t count = 0u;
  if (models) {
    count =
        spline_mesh
            ? population_spline_models(scene, rule->entity, spline_mesh,
                                       &assets->scratch_allocator, models, room)
            : population_scatter_models(scene, rule->entity, scatter, models,
                                        room);
  }
  const uint32_t wanted =
      scatter ? scatter->count
              : count; /* A spline's count follows its length. */
  if (scatter && count < Min(wanted, room)) {
    snprintf(rule->status, sizeof(rule->status),
             "%u of %u copies found ground below the box", count, wanted);
  } else if (wanted > room || (spline_mesh && count == room)) {
    snprintf(rule->status, sizeof(rule->status),
             "Capped at %u copies by the scene's population bound", room);
  }

  /* The same mesh and count only move; anything else starts over. */
  const bool8_t same = rule->instance_count == count &&
                       strcmp(rule->mesh, mesh) == 0 &&
                       rule->mesh_index == mesh_index;
  if (!same) {
    population_release_instances(scene, rule);
    if (count && !population_create(scene, rule, mesh, mesh_index, count)) {
      population_release_instances(scene, rule);
    }
  }
  const uint32_t render_id = vkr_scene_get_render_id(scene, rule->entity);
  const uint32_t picking_id =
      render_id ? scene->render_id_base + render_id : 0u;
  for (uint32_t i = 0; i < rule->instance_count && i < count; ++i) {
    vkr_mesh_manager_instance_sync_render_state(&assets->mesh_manager,
                                                rule->instances[i], models[i],
                                                picking_id, rule->visible);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

void vkr_scene_population_update(VkrScene *scene) {
  VkrScenePopulation *state = scene ? scene->population : NULL;
  if (!state || !scene->assets) {
    return;
  }
  for (uint32_t i = 0; i < state->rule_count; ++i) {
    PopulationRule *rule = &state->rules[i];
    const uint64_t signature = population_signature(scene, rule);
    if (signature == rule->signature) {
      continue;
    }
    /* A scatter rests on collision, so it waits for terrain to settle. */
    if (rule->kind == POPULATION_SCATTER && !vkr_scene_terrain_settled(scene)) {
      continue;
    }
    population_build(scene, rule, signature);
  }
}

uint32_t vkr_scene_population_pending(const VkrScene *scene) {
  const VkrScenePopulation *state = scene ? scene->population : NULL;
  uint32_t pending = 0u;
  for (uint32_t i = 0; state && i < state->rule_count; ++i) {
    pending += population_signature(scene, &state->rules[i]) !=
               state->rules[i].signature;
  }
  return pending;
}
