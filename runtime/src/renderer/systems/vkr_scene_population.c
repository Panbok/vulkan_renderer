#include "renderer/systems/vkr_scene_population.h"

#include "core/logger.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_mesh_raycast.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"

#define POPULATION_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Metres terrain samples must lie above a ray's hit to replace it, past the
   gap between a height field body's triangles and the bilinear samples. */
#define POPULATION_GROUND_TOLERANCE 0.05f
/* A scatter with a proxy distance merges its copies by cells of this side
   on the ground plane; past the distance a cell draws one merged mesh of
   its copies' coarsest level per submesh in their place (large-level
   rendering, section 2). A cell turns back to its copies within this share
   of the distance, so a camera at the boundary does not flip it. */
#define POPULATION_CELL_SIZE 64.0f
#define POPULATION_CELL_MAX 1024u
#define POPULATION_PROXY_NEAR_SHARE 0.9f
#define POPULATION_PROXY_SUBMESH_MAX 8u

typedef enum PopulationKind {
  POPULATION_SPLINE_MESH = 0,
  POPULATION_SCATTER,
} PopulationKind;

/* A cell of a scatter's copies: the sphere around their bounds and the
   merged mesh standing in for them past the proxy distance. */
typedef struct PopulationCell {
  Vec3 center;
  float32_t radius;
  /* The merged mesh's mesh-manager slot, or VKR_INVALID_ID. */
  uint32_t mesh_index;
  /* The merged mesh draws in place of the copies. */
  bool8_t proxied;
} PopulationCell;

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
  /* The copies' draw distances (vkr_scene_draw_cull) as last applied. */
  uint32_t draw_cull;
  bool8_t shadowless;
  /* Proxy cells (POPULATION_CELL_MAX, allocated with the first) and the
     cell of each copy; none without a proxy distance. Cells wait for the
     copies' mesh to finish publishing (`cells_pending`). */
  float32_t proxy_distance;
  bool8_t cells_pending;
  PopulationCell *cells;
  uint32_t cell_count;
  uint16_t *copy_cells;
  char status[160];
} PopulationRule;

/* The revisions a population update saw before every rule's signature
   matched. A signature reads typed values and visibility (world revision),
   children (structure revision, and the child index revision, since a
   rebuild can list them in another order), world matrices and local
   positions (transform revision, after the transform update) and terrain
   marks (terrain revision); `rules` covers rules added, reset or removed,
   and `hidden` the editor's Hide and Isolate, which only show or hide
   copies. While all are the same, no rule needs a look. */
typedef struct PopulationMatched {
  bool8_t valid;
  uint64_t world;
  uint64_t structure;
  uint64_t children;
  uint64_t transform;
  uint64_t terrain;
  uint64_t rules;
  uint64_t hidden;
} PopulationMatched;

struct s_VkrScenePopulation {
  PopulationRule rules[VKR_SCENE_POPULATION_RULE_MAX];
  uint32_t rule_count;
  uint32_t instance_total;
  /* Counts builds, which may replace any rule's copies. */
  uint64_t revision;
  /* Advances when a rule is added, reset or removed. */
  uint64_t rule_revision;
  PopulationMatched matched;
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

/* Removes the rule's merged cell meshes. */
static void population_release_cells(VkrScene *scene, PopulationRule *rule) {
  struct VkrRenderAssets *assets = scene->assets;
  for (uint32_t i = 0; assets && i < rule->cell_count; ++i) {
    if (rule->cells[i].mesh_index != VKR_INVALID_ID) {
      (void)vkr_mesh_manager_remove(&assets->mesh_manager,
                                    rule->cells[i].mesh_index);
    }
  }
  rule->cell_count = 0u;
}

static void population_release_instances(VkrScene *scene,
                                         PopulationRule *rule) {
  struct VkrRenderAssets *assets = scene->assets;
  /* Cells share the copies' materials, so they go first. */
  population_release_cells(scene, rule);
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
  state->rule_revision++;
  population_release_instances(scene, rule);
  if (rule->instances) {
    vkr_allocator_free(scene->alloc, rule->instances,
                       sizeof(VkrMeshInstanceHandle) *
                           VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
                       POPULATION_TAG);
  }
  if (rule->cells) {
    vkr_allocator_free(scene->alloc, rule->cells,
                       sizeof(PopulationCell) * POPULATION_CELL_MAX,
                       POPULATION_TAG);
    vkr_allocator_free(scene->alloc, rule->copy_cells,
                       sizeof(uint16_t) *
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
    scene->population->rule_revision++;
    return;
  }
  VkrScenePopulation *state = population_state(scene, true_v);
  if (!state || state->rule_count == VKR_SCENE_POPULATION_RULE_MAX) {
    log_warn("Scene: a scene holds at most %u spline meshes and scatters",
             VKR_SCENE_POPULATION_RULE_MAX);
    return;
  }
  state->rule_revision++;
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

uint64_t vkr_scene_population_revision(const VkrScene *scene) {
  return scene && scene->population ? scene->population->revision : 0u;
}

uint32_t vkr_scene_population_copies(const VkrScene *scene,
                                     VkrMeshInstanceHandle *out,
                                     uint32_t capacity) {
  const VkrScenePopulation *state = scene ? scene->population : NULL;
  uint32_t count = 0u;
  for (uint32_t r = 0; state && r < state->rule_count; ++r) {
    const PopulationRule *rule = &state->rules[r];
    for (uint32_t i = 0; i < rule->instance_count && count < capacity; ++i) {
      out[count++] = rule->instances[i];
    }
  }
  return count;
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
  const Vec3 reference = vkr_abs_f32(up.y) < 0.99f ? vec3_new(0.0f, 1.0f, 0.0f)
                                                   : vec3_new(0.0f, 0.0f, 1.0f);
  Vec3 forward = vec3_normalize(vec3_cross(vec3_cross(up, reference), up));
  if (vkr_abs_f32(up.y) >= 0.99f) {
    forward = vec3_new(0.0f, 0.0f, up.y > 0.0f ? 1.0f : -1.0f);
  }
  const Vec3 side = vec3_normalize(vec3_cross(up, forward));
  forward = vec3_add(vec3_scale(forward, vkr_cos_f32(yaw)),
                     vec3_scale(side, vkr_sin_f32(yaw)));
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

/* Drawn meshes a scatter with `render_surfaces` lands on: the scene's mesh
   renderers whose bounds meet the region its copies fall through, gathered
   once a build. Its own copies are no mesh renderers, so none stack. */
typedef struct PopulationSurfaces {
  VkrMeshRaycastCache *cache;
  VkrMeshManager *meshes;
  VkrMeshInstanceHandle *instances;
  uint32_t count;
} PopulationSurfaces;

static void population_surfaces_collect(VkrScene *scene, Vec3 lo, Vec3 hi,
                                        VkrAllocator *scratch,
                                        PopulationSurfaces *out) {
  *out = (PopulationSurfaces){0};
  VkrMeshRaycastCache *cache = vkr_render_assets_mesh_raycast(scene->assets);
  if (!cache || !scene->topo_count) {
    return;
  }
  out->instances = vkr_allocator_alloc(
      scratch, sizeof(VkrMeshInstanceHandle) * scene->topo_count,
      POPULATION_TAG);
  if (!out->instances) {
    return;
  }
  out->cache = cache;
  out->meshes = &scene->assets->mesh_manager;
  for (uint32_t i = 0; i < scene->topo_count; ++i) {
    const VkrEntityId entity = scene->topo_order[i];
    const SceneMeshRenderer *renderer = vkr_entity_get_component(
        scene->world, entity, scene->comp_mesh_renderer);
    const VkrMeshInstance *instance =
        renderer
            ? vkr_mesh_manager_get_instance(out->meshes, renderer->instance)
            : NULL;
    /* Editor hiding does not take a surface away, only visibility does. */
    if (!instance || !instance->bounds_valid ||
        !vkr_scene_entity_visible(scene, entity)) {
      continue;
    }
    const Vec3 c = instance->bounds_world_center;
    const float32_t r = instance->bounds_world_radius;
    if (c.x + r < lo.x || c.x - r > hi.x || c.y + r < lo.y || c.y - r > hi.y ||
        c.z + r < lo.z || c.z - r > hi.z) {
      continue;
    }
    out->instances[out->count++] = renderer->instance;
  }
}

/* Drops one copy straight down from `top` to `bottom` onto the first
   surface between them, turned and scaled from `seed` and `index` as the
   rule asks; false when the column meets nothing. A streamed terrain has
   collision only near its streaming sources, so terrain samples answer
   where they lie above what the ray met. With `surfaces`, drawn meshes
   count as surfaces too. */
static bool8_t population_drop(VkrScene *scene, const SceneScatter *rule,
                               const PopulationSurfaces *surfaces, Vec3 top,
                               Vec3 bottom, uint32_t seed, uint32_t index,
                               Vec3 *out_position, Mat4 *out_model) {
  VkrPhysicsRayHit hit = {0};
  const Vec3 delta = vec3_sub(bottom, top);
  bool8_t struck = vkr_scene_physics_raycast(scene, top, delta, &hit);
  Vec3 position = vec3_new(hit.position[0], hit.position[1], hit.position[2]);
  Vec3 normal = vec3_new(hit.normal[0], hit.normal[1], hit.normal[2]);
  const float32_t length_sq = Max(vec3_dot(delta, delta), 1.0e-12f);
  float32_t nearest =
      struck ? vec3_dot(vec3_sub(position, top), delta) / length_sq : 2.0f;
  for (uint32_t i = 0; surfaces && i < surfaces->count; ++i) {
    VkrMeshRaycastHit mesh_hit;
    if (vkr_mesh_raycast_instance(surfaces->cache, surfaces->meshes,
                                  surfaces->instances[i], top, delta,
                                  &mesh_hit) &&
        mesh_hit.fraction < nearest) {
      nearest = mesh_hit.fraction;
      position = mesh_hit.position;
      normal = mesh_hit.normal;
      struck = true_v;
    }
  }
  Vec3 ground = vec3_zero();
  Vec3 ground_normal = vec3_zero();
  if (vkr_scene_terrain_ground(scene, top, bottom.y, &ground, &ground_normal) &&
      (!struck || ground.y > position.y + POPULATION_GROUND_TOLERANCE)) {
    position = ground;
    normal = ground_normal;
  } else if (!struck) {
    return false_v;
  }
  Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  if (rule->align_to_surface && vec3_length(normal) > 1.0e-4f) {
    up = vec3_normalize(normal);
  }
  const float32_t yaw =
      rule->random_yaw ? population_random(seed, index, 2u) * 6.28318530718f
                       : 0.0f;
  const float32_t lo = Min(rule->scale_min, rule->scale_max);
  const float32_t hi = Max(rule->scale_min, rule->scale_max);
  const float32_t scale = lo + (hi - lo) * population_random(seed, index, 3u);
  Vec3 right;
  Vec3 forward;
  population_axes(up, yaw, &right, &forward);
  *out_position = position;
  *out_model = population_frame(position, right, up, forward, rule->rotation,
                                vec3_new(scale, scale, scale));
  return true_v;
}

/* Seeded copies in the scatter's box, each dropped straight down onto the
   first surface below the box's top; a column that meets nothing places
   none. */
static uint32_t population_scatter_models(VkrScene *scene, VkrEntityId entity,
                                          const SceneScatter *rule,
                                          VkrAllocator *scratch, Mat4 *out,
                                          uint32_t capacity) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (!transform) {
    return 0u;
  }
  PopulationSurfaces surfaces = {0};
  if (rule->render_surfaces) {
    Vec3 lo =
        vec3_new(VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY);
    Vec3 hi =
        vec3_new(-VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY);
    for (uint32_t corner = 0; corner < 8u; ++corner) {
      const Vec3 point = mat4_mul_vec3(
          transform->world,
          vec3_new((corner & 1u) ? rule->extents.x : -rule->extents.x,
                   (corner & 2u) ? rule->extents.y : -rule->extents.y,
                   (corner & 4u) ? rule->extents.z : -rule->extents.z));
      lo = vec3_new(Min(lo.x, point.x), Min(lo.y, point.y), Min(lo.z, point.z));
      hi = vec3_new(Max(hi.x, point.x), Max(hi.y, point.y), Max(hi.z, point.z));
    }
    population_surfaces_collect(scene, lo, hi, scratch, &surfaces);
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
    Vec3 position;
    if (population_drop(scene, rule, &surfaces, top, bottom, rule->seed, i,
                        &position, &out[placed])) {
      placed++;
    }
  }
  return placed;
}

/* Copies an area asks for: its density over its disc, the fraction kept by
   chance so small areas still place some. */
static uint32_t population_area_count(const SceneScatterArea *area) {
  const float32_t expected =
      area->density * 3.14159265f * area->radius * area->radius;
  const float32_t whole = vkr_floor_f32(expected);
  return (uint32_t)whole +
         (population_random(area->seed, 0xFFFFu, 7u) < expected - whole ? 1u
                                                                        : 0u);
}

/* The scatter's painted areas in ascending order, ties in child order:
   each fills its disc with its own seeded copies, dropped from a column a
   radius and a metre above and below its position. Copies within an
   area's spacing of an earlier copy are left out, so later strokes never
   move earlier copies, and an undo that brings an area back keeps its
   place. `wanted` counts every copy the areas ask for. */
static uint32_t population_area_models(VkrScene *scene, VkrEntityId entity,
                                       const SceneScatter *rule,
                                       VkrAllocator *scratch, Mat4 *out,
                                       uint32_t capacity, uint32_t *wanted) {
  uint32_t child_count = 0u;
  const VkrEntityId *children =
      vkr_scene_get_children(scene, entity, &child_count);
  Vec3 *positions =
      vkr_allocator_alloc(scratch, sizeof(Vec3) * capacity, POPULATION_TAG);
  uint32_t *sorted = vkr_allocator_alloc(
      scratch, sizeof(uint32_t) * Max(child_count, 1u), POPULATION_TAG);
  *wanted = 0u;
  if (!positions || !sorted) {
    return 0u;
  }
  /* Insertion keeps equal orders in child order. */
  uint32_t area_count = 0u;
  for (uint32_t c = 0; c < child_count; ++c) {
    const SceneScatterArea *area =
        vkr_scene_get_typed(scene, children[c], &vkr_scene_scatter_area_type);
    if (!area) {
      continue;
    }
    uint32_t at = area_count++;
    while (at > 0u) {
      const SceneScatterArea *before = vkr_scene_get_typed(
          scene, children[sorted[at - 1u]], &vkr_scene_scatter_area_type);
      if (before->order <= area->order) {
        break;
      }
      sorted[at] = sorted[at - 1u];
      at--;
    }
    sorted[at] = c;
  }
  PopulationSurfaces surfaces = {0};
  if (rule->render_surfaces) {
    Vec3 lo =
        vec3_new(VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY);
    Vec3 hi =
        vec3_new(-VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY, -VKR_FLOAT_INFINITY);
    for (uint32_t a = 0; a < area_count; ++a) {
      const VkrEntityId child = children[sorted[a]];
      const SceneScatterArea *area =
          vkr_scene_get_typed(scene, child, &vkr_scene_scatter_area_type);
      const SceneTransform *transform =
          vkr_entity_get_component(scene->world, child, scene->comp_transform);
      if (!transform) {
        continue;
      }
      const Vec3 center = mat4_position(transform->world);
      const float32_t reach = area->radius + 1.0f;
      lo = vec3_new(Min(lo.x, center.x - reach), Min(lo.y, center.y - reach),
                    Min(lo.z, center.z - reach));
      hi = vec3_new(Max(hi.x, center.x + reach), Max(hi.y, center.y + reach),
                    Max(hi.z, center.z + reach));
    }
    population_surfaces_collect(scene, lo, hi, scratch, &surfaces);
  }
  uint32_t placed = 0u;
  for (uint32_t a = 0; a < area_count; ++a) {
    const VkrEntityId child = children[sorted[a]];
    const SceneScatterArea *area =
        vkr_scene_get_typed(scene, child, &vkr_scene_scatter_area_type);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, child, scene->comp_transform);
    if (!transform) {
      continue;
    }
    const Vec3 center = mat4_position(transform->world);
    const float32_t reach = area->radius + 1.0f;
    const uint32_t count = population_area_count(area);
    *wanted += count;
    /* No area tries more columns than the rule may hold, so a huge
       density costs bounded raycasts. */
    const uint32_t tries = Min(count, capacity);
    for (uint32_t i = 0; i < tries && placed < capacity; ++i) {
      const float32_t angle =
          population_random(area->seed, i, 0u) * 6.28318530718f;
      const float32_t distance =
          area->radius * vkr_sqrt_f32(population_random(area->seed, i, 1u));
      const Vec3 column =
          vec3_new(center.x + vkr_cos_f32(angle) * distance, center.y,
                   center.z + vkr_sin_f32(angle) * distance);
      /* An area painted onto one model drops only through that model's
         box, so what hangs above it cannot catch the copy, and a column
         beside the box drops none. */
      const Vec3 lo = area->within_min;
      const Vec3 hi = area->within_max;
      const bool8_t within = lo.x < hi.x && lo.y < hi.y && lo.z < hi.z;
      if (within && (column.x < lo.x || column.x > hi.x || column.z < lo.z ||
                     column.z > hi.z)) {
        continue;
      }
      const float32_t top =
          within ? Min(center.y + reach, hi.y) : center.y + reach;
      const float32_t bottom =
          within ? Max(center.y - reach, lo.y) : center.y - reach;
      Vec3 position;
      Mat4 model;
      if (top <= bottom ||
          !population_drop(scene, rule, &surfaces,
                           vec3_new(column.x, top, column.z),
                           vec3_new(column.x, bottom, column.z), area->seed, i,
                           &position, &model)) {
        continue;
      }
      bool8_t crowded = false_v;
      const float32_t spacing_sq = area->spacing * area->spacing;
      for (uint32_t p = 0; spacing_sq > 0.0f && p < placed && !crowded; ++p) {
        const Vec3 gap = vec3_sub(positions[p], position);
        crowded = vec3_dot(gap, gap) < spacing_sq;
      }
      if (crowded) {
        continue;
      }
      positions[placed] = position;
      out[placed++] = model;
    }
  }
  return placed;
}

/* Whether the scatter has painted areas, which replace its box. */
static bool8_t population_painted(const VkrScene *scene, VkrEntityId entity) {
  uint32_t child_count = 0u;
  const VkrEntityId *children =
      vkr_scene_get_children(scene, entity, &child_count);
  for (uint32_t c = 0; c < child_count; ++c) {
    if (vkr_scene_get_typed(scene, children[c], &vkr_scene_scatter_area_type)) {
      return true_v;
    }
  }
  return false_v;
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
    /* Painted areas: each one's values and world position. */
    uint32_t child_count = 0u;
    const VkrEntityId *children =
        vkr_scene_get_children(scene, rule->entity, &child_count);
    for (uint32_t c = 0; c < child_count; ++c) {
      const SceneScatterArea *area =
          vkr_scene_get_typed(scene, children[c], &vkr_scene_scatter_area_type);
      const SceneTransform *child = vkr_entity_get_component(
          scene->world, children[c], scene->comp_transform);
      if (area && child) {
        const Vec3 center = mat4_position(child->world);
        hash = population_hash(hash, area, sizeof(*area));
        hash = population_hash(hash, &center, sizeof(center));
      }
    }
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
      string8_create_from_cstr((const uint8_t *)mesh, string_length(mesh));
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
    string_format(rule->status, sizeof(rule->status),
                  "The mesh '%s' did not load", mesh);
    goto cleanup;
  }
  const VkrMeshSource *source = &resolved.as.mesh->source;
  if (mesh_index >= source->meshes.length ||
      !source->meshes.data[mesh_index].range_count) {
    string_format(rule->status, sizeof(rule->status),
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
      string_format(rule->status, sizeof(rule->status),
                    "Only %u of %u copies fit the mesh manager", i, count);
      break;
    }
    (void)vkr_mesh_manager_instance_set_shadow_mobility(
        &assets->mesh_manager, instance, VKR_SHADOW_CASTER_MOBILITY_STATIC);
    rule->instances[rule->instance_count++] = instance;
    scene->population->instance_total++;
  }
  string_format(rule->mesh, sizeof(rule->mesh), "%s", mesh);
  rule->mesh_index = mesh_index;
  ok = true_v;

cleanup:
  if (request.as.mesh || request.request_id) {
    vkr_resource_system_unload(&request, path);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return ok;
}

/* Whether the rule's copies draw: its entity is visible and the editor's
   Hide and Isolate leave it shown (vkr_scene_editor_shown). */
static bool8_t population_visible(const VkrScene *scene,
                                  const PopulationRule *rule) {
  return vkr_scene_entity_visible(scene, rule->entity) &&
         vkr_scene_editor_shown(scene, rule->entity);
}

/* Shows or hides the placed copies where they stand. */
static void population_show(VkrScene *scene, PopulationRule *rule,
                            bool8_t visible) {
  VkrMeshManager *manager = &scene->assets->mesh_manager;
  rule->visible = visible;
  for (uint32_t i = 0; i < rule->cell_count; ++i) {
    if (rule->cells[i].mesh_index != VKR_INVALID_ID) {
      (void)vkr_mesh_manager_set_visible(manager, rule->cells[i].mesh_index,
                                         visible);
    }
  }
  for (uint32_t i = 0; i < rule->instance_count; ++i) {
    const VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance(manager, rule->instances[i]);
    if (instance) {
      vkr_mesh_manager_instance_sync_render_state(manager, rule->instances[i],
                                                  instance->model,
                                                  instance->render_id, visible);
    }
  }
}

/* Gives the copies the draw distances of the rule's entity, when they
   changed or with `force`. */
static void population_apply_draw_cull(VkrScene *scene, PopulationRule *rule,
                                       bool8_t force) {
  bool8_t shadowless = false_v;
  const uint32_t cull = vkr_scene_draw_cull(
      scene, rule->entity, VKR_SCENE_DRAW_KIND_COPY, &shadowless);
  if (!force && cull == rule->draw_cull && shadowless == rule->shadowless) {
    return;
  }
  rule->draw_cull = cull;
  rule->shadowless = shadowless;
  for (uint32_t i = 0; i < rule->instance_count; ++i) {
    (void)vkr_mesh_manager_instance_set_draw_cull(
        &scene->assets->mesh_manager, rule->instances[i], cull, shadowless);
  }
}

/* Index of the cell of ground position (`x`, `z`) in the rule's cells,
   added when new; UINT32_MAX when the cells are full. `keys` holds each
   cell's packed grid coordinates. */
static uint32_t population_cell_of(PopulationRule *rule, int32_t *keys,
                                   float32_t x, float32_t z) {
  const int32_t cx = (int32_t)vkr_floor_f32(x / POPULATION_CELL_SIZE);
  const int32_t cz = (int32_t)vkr_floor_f32(z / POPULATION_CELL_SIZE);
  for (uint32_t i = 0; i < rule->cell_count; ++i) {
    if (keys[i * 2u] == cx && keys[i * 2u + 1u] == cz) {
      return i;
    }
  }
  if (rule->cell_count == POPULATION_CELL_MAX) {
    return UINT32_MAX;
  }
  keys[rule->cell_count * 2u] = cx;
  keys[rule->cell_count * 2u + 1u] = cz;
  rule->cells[rule->cell_count] = (PopulationCell){
      .center = vec3_zero(), .radius = 0.0f, .mesh_index = VKR_INVALID_ID};
  return rule->cell_count++;
}

/* `direction` turned and scaled by `model`'s upper 3x3. */
static Vec3 population_direction(Mat4 model, Vec3 direction) {
  return vec3_add(vec3_add(vec3_scale(model.cols[0], direction.x),
                           vec3_scale(model.cols[1], direction.y)),
                  vec3_scale(model.cols[2], direction.z));
}

/* Appends `coarse`'s triangles placed by `model` to a cell's vertices and
   indices at `*vertex_count` and `*index_count`. */
static void population_cell_append(const VkrMeshCoarseRange *coarse, Mat4 model,
                                   VkrVertex3d *vertices, uint32_t *indices,
                                   uint32_t *vertex_count,
                                   uint32_t *index_count) {
  const uint32_t base = *vertex_count;
  for (uint32_t v = 0; v < coarse->vertex_count; ++v) {
    VkrVertex3d vertex = coarse->vertices[v];
    const Vec3 position =
        mat4_mul_vec3(model, vec3_new(vertex.position.x, vertex.position.y,
                                      vertex.position.z));
    /* Copies turn about Y and scale evenly, so the model rotates
       directions as it does positions. */
    const Vec3 normal = vec3_normalize(population_direction(
        model, vec3_new(vertex.normal.x, vertex.normal.y, vertex.normal.z)));
    const Vec3 tangent = vec3_normalize(population_direction(
        model, vec3_new(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z)));
    vertex.position = vkr_vertex_pack_vec3(position);
    vertex.normal = vkr_vertex_pack_vec3(normal);
    vertex.tangent =
        vec4_new(tangent.x, tangent.y, tangent.z, vertex.tangent.w);
    vertices[(*vertex_count)++] = vertex;
  }
  for (uint32_t i = 0; i < coarse->index_count; ++i) {
    indices[(*index_count)++] = base + coarse->indices[i];
  }
}

/* Builds the rule's proxy cells from its copies: each copy's cell, and per
   cell one mesh of their coarsest level per submesh. The meshes cast no
   shadow, since the copies keep casting. A copy mesh that has not finished
   publishing leaves the rule pending, for a later update to build. */
static void population_build_cells(VkrScene *scene, PopulationRule *rule) {
  struct VkrRenderAssets *assets = scene->assets;
  population_release_cells(scene, rule);
  rule->cells_pending = false_v;
  const SceneScatter *scatter =
      rule->kind == POPULATION_SCATTER
          ? vkr_scene_get_typed(scene, rule->entity, &vkr_scene_scatter_type)
          : NULL;
  rule->proxy_distance = scatter ? scatter->proxy_distance : 0.0f;
  const uint32_t count = rule->instance_count;
  if (!(rule->proxy_distance > 0.0f) || count == 0u) {
    return;
  }
  if (!rule->cells) {
    rule->cells = vkr_allocator_alloc(
        scene->alloc, sizeof(PopulationCell) * POPULATION_CELL_MAX,
        POPULATION_TAG);
    rule->copy_cells = vkr_allocator_alloc(
        scene->alloc, sizeof(uint16_t) * VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
        POPULATION_TAG);
    if (!rule->cells || !rule->copy_cells) {
      string_format(rule->status, sizeof(rule->status), "Out of memory");
      return;
    }
  }
  VkrMeshManager *manager = &assets->mesh_manager;
  const VkrMeshInstance *first =
      vkr_mesh_manager_get_instance(manager, rule->instances[0]);
  VkrMeshAsset *asset =
      first ? vkr_mesh_manager_get_live_asset(manager, first->asset) : NULL;
  if (!asset || asset->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    rule->cells_pending = asset != NULL;
    return;
  }

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  VkrAllocator *scratch = &assets->scratch_allocator;
  VkrMeshCoarseRange coarse[POPULATION_PROXY_SUBMESH_MAX];
  const uint32_t submesh_count =
      Min(vkr_mesh_raycast_coarse_ranges(vkr_render_assets_mesh_raycast(assets),
                                         manager, first->asset, scratch, coarse,
                                         POPULATION_PROXY_SUBMESH_MAX),
          (uint32_t)asset->submeshes.length);
  int32_t *keys = vkr_allocator_alloc(
      scratch, sizeof(int32_t) * 2u * POPULATION_CELL_MAX, POPULATION_TAG);
  uint32_t *cell_copies = vkr_allocator_alloc(
      scratch, sizeof(uint32_t) * POPULATION_CELL_MAX, POPULATION_TAG);
  if (!submesh_count || !keys || !cell_copies) {
    string_format(rule->status, sizeof(rule->status),
                  "The far proxies could not read the mesh's levels");
    vkr_allocator_end_scope(&scope, POPULATION_TAG);
    return;
  }

  /* Each copy's cell, and each cell's sphere around its copies' bounds. */
  MemZero(cell_copies, sizeof(uint32_t) * POPULATION_CELL_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMeshInstance *copy =
        vkr_mesh_manager_get_instance(manager, rule->instances[i]);
    const Vec3 at = copy ? mat4_position(copy->model) : vec3_zero();
    const uint32_t cell = population_cell_of(rule, keys, at.x, at.z);
    rule->copy_cells[i] = cell == UINT32_MAX ? UINT16_MAX : (uint16_t)cell;
    if (cell == UINT32_MAX || !copy || !copy->bounds_valid) {
      continue;
    }
    PopulationCell *target = &rule->cells[cell];
    cell_copies[cell]++;
    target->center = vec3_add(target->center, copy->bounds_world_center);
    target->radius = Max(target->radius, copy->bounds_world_radius);
  }
  for (uint32_t c = 0; c < rule->cell_count; ++c) {
    if (cell_copies[c]) {
      rule->cells[c].center =
          vec3_scale(rule->cells[c].center, 1.0f / (float32_t)cell_copies[c]);
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMeshInstance *copy =
        vkr_mesh_manager_get_instance(manager, rule->instances[i]);
    const uint32_t cell = rule->copy_cells[i];
    if (cell != UINT16_MAX && copy && copy->bounds_valid) {
      PopulationCell *target = &rule->cells[cell];
      target->radius =
          Max(target->radius,
              vec3_distance(target->center, copy->bounds_world_center) +
                  copy->bounds_world_radius);
    }
  }

  /* One merged mesh per cell, one submesh per mesh submesh. */
  const uint32_t render_id = vkr_scene_get_render_id(scene, rule->entity);
  const uint32_t picking_id =
      render_id ? scene->render_id_base + render_id : 0u;
  for (uint32_t c = 0; c < rule->cell_count; ++c) {
    if (!cell_copies[c]) {
      continue;
    }
    VkrSubMeshDesc submeshes[POPULATION_PROXY_SUBMESH_MAX];
    uint32_t built = 0u;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      if (!coarse[s].index_count) {
        continue;
      }
      VkrAllocatorScope cell_scope = vkr_allocator_begin_scope(scratch);
      const uint32_t vertex_capacity = coarse[s].vertex_count * cell_copies[c];
      const uint32_t index_capacity = coarse[s].index_count * cell_copies[c];
      VkrVertex3d *vertices = vkr_allocator_alloc(
          scratch, sizeof(VkrVertex3d) * vertex_capacity, POPULATION_TAG);
      uint32_t *indices = vkr_allocator_alloc(
          scratch, sizeof(uint32_t) * index_capacity, POPULATION_TAG);
      uint32_t vertex_count = 0u;
      uint32_t index_count = 0u;
      for (uint32_t i = 0; vertices && indices && i < count; ++i) {
        const VkrMeshInstance *copy =
            vkr_mesh_manager_get_instance(manager, rule->instances[i]);
        if (copy && rule->copy_cells[i] == c) {
          population_cell_append(&coarse[s], copy->model, vertices, indices,
                                 &vertex_count, &index_count);
        }
      }
      const Vec3 extent = vec3_new(rule->cells[c].radius, rule->cells[c].radius,
                                   rule->cells[c].radius);
      VkrGeometryConfig config = {
          .vertex_size = sizeof(VkrVertex3d),
          .vertex_count = vertex_count,
          .vertices = vertices,
          .index_size = sizeof(uint32_t),
          .index_count = index_count,
          .indices = indices,
          .center = rule->cells[c].center,
          .min_extents = vec3_sub(rule->cells[c].center, extent),
          .max_extents = vec3_add(rule->cells[c].center, extent),
      };
      string_format(config.name, sizeof(config.name),
                    "scatter_cell_%u_%u_%u_%u",
                    (unsigned)rule->entity.parts.index,
                    (unsigned)rule->entity.parts.generation, c, s);
      VkrRendererError error = VKR_RENDERER_ERROR_NONE;
      const VkrGeometryHandle geometry =
          vertices && indices && index_count
              ? vkr_geometry_system_create(&assets->geometry_system, &config,
                                           true_v, &error)
              : (VkrGeometryHandle){0};
      vkr_allocator_end_scope(&cell_scope, POPULATION_TAG);
      if (!geometry.id) {
        continue;
      }
      submeshes[built++] = (VkrSubMeshDesc){
          .geometry = geometry,
          .material = asset->submeshes.data[s].material,
          .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
          .owns_geometry = true_v,
      };
    }
    if (!built) {
      continue;
    }
    const VkrMeshDesc desc = {
        .transform = vkr_transform_identity(),
        .submeshes = submeshes,
        .submesh_count = built,
        .shadow_mobility = VKR_SHADOW_CASTER_MOBILITY_STATIC,
    };
    uint32_t mesh_index = VKR_INVALID_ID;
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_mesh_manager_add(manager, &desc, &mesh_index, &error)) {
      for (uint32_t s = 0; s < built; ++s) {
        vkr_geometry_system_release(&assets->geometry_system,
                                    submeshes[s].geometry);
      }
      continue;
    }
    rule->cells[c].mesh_index = mesh_index;
    rule->cells[c].proxied = false_v;
    (void)vkr_mesh_manager_set_model(manager, mesh_index, mat4_identity());
    (void)vkr_mesh_manager_set_visible(manager, mesh_index, rule->visible);
    (void)vkr_mesh_manager_set_render_id(manager, mesh_index, picking_id);
    /* The copies' draw distances, without their shadows. */
    (void)vkr_mesh_manager_set_draw_cull(manager, mesh_index, rule->draw_cull,
                                         true_v);
  }
  vkr_allocator_end_scope(&scope, POPULATION_TAG);
}

/* Places the rule's copies anew. */
static void population_build(VkrScene *scene, PopulationRule *rule,
                             uint64_t signature) {
  struct VkrRenderAssets *assets = scene->assets;
  VkrScenePopulation *state = scene->population;
  state->revision++;
  rule->signature = signature;
  rule->status[0] = '\0';
  rule->visible = population_visible(scene, rule);
  if (!rule->instances) {
    rule->instances = vkr_allocator_alloc(
        scene->alloc,
        sizeof(VkrMeshInstanceHandle) * VKR_SCENE_POPULATION_RULE_INSTANCE_MAX,
        POPULATION_TAG);
    if (!rule->instances) {
      string_format(rule->status, sizeof(rule->status), "Out of memory");
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
    string_format(rule->status, sizeof(rule->status), "No mesh is named");
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
  /* A spline's count follows its length; a painted scatter's its areas. */
  uint32_t wanted = scatter ? scatter->count : 0u;
  const bool8_t painted = scatter && population_painted(scene, rule->entity);
  if (models && spline_mesh) {
    count = population_spline_models(scene, rule->entity, spline_mesh,
                                     &assets->scratch_allocator, models, room);
    wanted = count;
  } else if (models && painted) {
    count = population_area_models(scene, rule->entity, scatter,
                                   &assets->scratch_allocator, models, room,
                                   &wanted);
  } else if (models) {
    count = population_scatter_models(scene, rule->entity, scatter,
                                      &assets->scratch_allocator, models, room);
  }
  if (painted && count < Min(wanted, room)) {
    string_format(rule->status, sizeof(rule->status),
                  "%u of %u copies found ground in the areas and room by their "
                  "spacing",
                  count, wanted);
  } else if (scatter && count < Min(wanted, room)) {
    string_format(rule->status, sizeof(rule->status),
                  "%u of %u copies found ground below the box", count, wanted);
  } else if (wanted > room || (spline_mesh && count == room)) {
    string_format(rule->status, sizeof(rule->status),
                  "Capped at %u copies by the scene's population bound", room);
  }

  /* The same mesh and count only move; anything else starts over. */
  const bool8_t same = rule->instance_count == count &&
                       string_equals(rule->mesh, mesh) &&
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
  population_apply_draw_cull(scene, rule, true_v);
  population_build_cells(scene, rule);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

void vkr_scene_population_update(VkrScene *scene) {
  VkrScenePopulation *state = scene ? scene->population : NULL;
  if (!state || !scene->assets) {
    return;
  }
  /* Proxy cells wait for their copies' mesh to finish publishing. */
  for (uint32_t i = 0; i < state->rule_count; ++i) {
    if (state->rules[i].cells_pending) {
      population_build_cells(scene, &state->rules[i]);
    }
  }
  /* Taken before any build, so a build that moves a revision leaves the
     next update to look again. */
  const PopulationMatched seen = {
      .valid = true_v,
      .world = scene->world_revision,
      .structure = scene->structure_revision,
      .children = scene->child_index_revision,
      .transform = scene->transform_revision,
      .terrain = vkr_scene_terrain_revision(scene),
      .rules = state->rule_revision,
      .hidden = scene->editor_hidden_revision,
  };
  const PopulationMatched *last = &state->matched;
  if (last->valid && last->world == seen.world &&
      last->structure == seen.structure && last->children == seen.children &&
      last->transform == seen.transform && last->terrain == seen.terrain &&
      last->rules == seen.rules && last->hidden == seen.hidden) {
    return;
  }
  bool8_t matched = true_v;
  for (uint32_t i = 0; i < state->rule_count; ++i) {
    PopulationRule *rule = &state->rules[i];
    /* Spline points and painted areas are children. While the child index
       is invalid (a brush build or this loop's first copies of a rule
       invalidated it) a signature would see none, so the rule waits for
       the update after the index rebuilds. */
    if (!scene->child_index_valid) {
      matched = false_v;
      continue;
    }
    const uint64_t signature = population_signature(scene, rule);
    if (signature == rule->signature) {
      /* Hide and Isolate only change what draws, not where. */
      const bool8_t visible = population_visible(scene, rule);
      if (visible != rule->visible) {
        population_show(scene, rule, visible);
      }
      population_apply_draw_cull(scene, rule, false_v);
      continue;
    }
    /* A scatter rests on collision, so it waits for terrain to settle. */
    if (rule->kind == POPULATION_SCATTER && !vkr_scene_terrain_settled(scene)) {
      matched = false_v;
      continue;
    }
    population_build(scene, rule, signature);
  }
  /* A rule waiting for terrain or the child index keeps the next update
     looking. */
  state->matched = matched ? seen : (PopulationMatched){0};
}

bool8_t vkr_scene_population_has_proxies(const VkrScene *scene) {
  const VkrScenePopulation *state = scene ? scene->population : NULL;
  for (uint32_t r = 0; state && r < state->rule_count; ++r) {
    if (state->rules[r].cell_count > 0u) {
      return true_v;
    }
  }
  return false_v;
}

void vkr_scene_population_mark_proxies(VkrScene *scene, Vec3 camera,
                                       bool8_t copies_only,
                                       VkrScenePopulationFrame *frame) {
  VkrScenePopulation *state = scene ? scene->population : NULL;
  for (uint32_t r = 0; state && r < state->rule_count; ++r) {
    PopulationRule *rule = &state->rules[r];
    if (!rule->cell_count) {
      continue;
    }
    for (uint32_t c = 0; c < rule->cell_count; ++c) {
      PopulationCell *cell = &rule->cells[c];
      if (cell->mesh_index == VKR_INVALID_ID) {
        continue;
      }
      const float32_t distance =
          vec3_distance(cell->center, camera) - cell->radius;
      cell->proxied =
          !copies_only &&
          (cell->proxied
               ? distance > rule->proxy_distance * POPULATION_PROXY_NEAR_SHARE
               : distance > rule->proxy_distance);
      if (!cell->proxied && cell->mesh_index < frame->mesh_slot_count) {
        frame->mesh_hidden[cell->mesh_index] = 1u;
      }
    }
    for (uint32_t i = 0; i < rule->instance_count; ++i) {
      const uint32_t cell = rule->copy_cells[i];
      const uint32_t slot = rule->instances[i].id - 1u;
      if (cell != UINT16_MAX && cell < rule->cell_count &&
          rule->cells[cell].proxied && slot < frame->instance_slot_count) {
        frame->instance_hidden[slot] = 1u;
      }
    }
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
