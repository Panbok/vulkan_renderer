#include "renderer/systems/vkr_scene_rooms.h"

#include <math.h>
#include <stdio.h>

#include "math/vkr_frustum.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_population.h"
#include "renderer/systems/vkr_scene_types.h"
#include "vkr_gpu_abi.h"

#define ROOMS_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* A portal's sides are sampled this far along its normal. */
#define ROOM_PORTAL_PROBE 0.3f
/* Inside a box with this tolerance, so a corner on a wall's face counts as
   inside the room it bounds. */
#define ROOM_INSIDE_EPSILON 0.02f
/* Steps of one portal walk. A node is walked again only through a wider
   rectangle than it was, so the walk ends; this bounds it besides. */
#define ROOM_WALK_STEP_MAX 8192u
#define ROOM_WORDS(count) (((count) + 63u) / 64u)

typedef struct RoomBox {
  Mat4 world;
  Mat4 inverse;
  Vec3 min;
  Vec3 max;
  float32_t volume;
  uint32_t room;
} RoomBox;

typedef struct RoomPortal {
  Vec3 corners[4];
  Vec3 center;
  float32_t radius;
  /* Unit axes of the rectangle and its normal, and half its size along
     each axis. */
  Vec3 axis_u;
  Vec3 axis_v;
  Vec3 normal;
  float32_t half_u;
  float32_t half_v;
  /* The rooms on its two sides: room indices or VKR_SCENE_ROOM_OUTSIDE. */
  uint32_t sides[2];
  bool8_t open;
} RoomPortal;

/* A room or portal entity and the world matrix the graph was built from. */
typedef struct RoomSource {
  VkrEntityId entity;
  Mat4 world;
} RoomSource;

typedef struct RoomWalk {
  uint32_t node;
  Vec4 rect;
} RoomWalk;

struct s_VkrSceneRooms {
  /* The graph. */
  uint32_t room_count;
  uint64_t sealed[ROOM_WORDS(VKR_SCENE_ROOM_MAX)];
  RoomBox *boxes;
  uint32_t box_count;
  uint32_t box_capacity;
  RoomPortal *portals;
  uint32_t portal_count;
  uint32_t portal_capacity;
  RoomSource *sources;
  uint32_t source_count;
  uint32_t source_capacity;
  uint64_t world_revision;
  uint64_t structure_revision;
  uint64_t transform_revision;
  uint64_t graph_revision;
  bool8_t graph_valid;

  /* The draws' rooms, from these generations of the mesh manager. */
  uint32_t *mesh_first;
  uint32_t mesh_capacity;
  uint32_t *instance_first;
  uint32_t instance_capacity;
  uint32_t *words;
  uint32_t word_capacity;
  uint32_t word_count;
  uint64_t membership_graph;
  uint64_t membership_topology;
  uint64_t membership_static;
  uint64_t membership_population;
  bool8_t membership_valid;
  /* What the sealed rooms keep out of the cascades, as last noted. */
  uint64_t sealed_hash;

  /* This frame's walk. */
  uint64_t visible[ROOM_WORDS(VKR_SCENE_ROOM_MAX)];
  /* Per room, then the outside: the screen rectangle the walk has looked
     through it, so a node is walked again only through a wider one. */
  Vec4 explored[VKR_SCENE_ROOM_MAX + 1u];
  RoomWalk walk[ROOM_WALK_STEP_MAX];
  VkrSceneRoomFrame frame;
};

// =============================================================================
// Boxes
// =============================================================================

static bool8_t room_box_contains(const RoomBox *box, Vec3 point,
                                 float32_t epsilon) {
  const Vec3 local = mat4_mul_vec3(box->inverse, point);
  const float32_t half = 0.5f + epsilon;
  return fabsf(local.x) <= half && fabsf(local.y) <= half &&
         fabsf(local.z) <= half;
}

/* Distance in metres from `point` to the box, zero inside. */
static float32_t room_box_distance(const RoomBox *box, Vec3 point) {
  const Vec3 local = mat4_mul_vec3(box->inverse, point);
  const Vec3 nearest =
      vec3_new(Clamp(local.x, -0.5f, 0.5f), Clamp(local.y, -0.5f, 0.5f),
               Clamp(local.z, -0.5f, 0.5f));
  return vec3_distance(mat4_mul_vec3(box->world, nearest), point);
}

/* The room of the smallest box holding `point`, or the outside. */
static uint32_t room_at(const VkrSceneRooms *rooms, Vec3 point,
                        float32_t epsilon) {
  uint32_t found = VKR_SCENE_ROOM_OUTSIDE;
  float32_t volume = INFINITY;
  for (uint32_t i = 0; i < rooms->box_count; ++i) {
    const RoomBox *box = &rooms->boxes[i];
    if (box->volume < volume && room_box_contains(box, point, epsilon)) {
      found = box->room;
      volume = box->volume;
    }
  }
  return found;
}

// =============================================================================
// Graph
// =============================================================================

static VkrSceneRooms *rooms_state(VkrScene *scene) {
  if (!scene->rooms) {
    scene->rooms = vkr_allocator_alloc(scene->alloc, sizeof(*scene->rooms),
                                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (scene->rooms) {
      MemZero(scene->rooms, sizeof(*scene->rooms));
    }
  }
  return scene->rooms;
}

/* Grows `*array` of `*capacity` elements of `size` bytes to hold `count`. */
static bool8_t rooms_reserve(VkrScene *scene, void **array, uint32_t *capacity,
                             uint32_t count, uint64_t size) {
  if (count <= *capacity) {
    return true_v;
  }
  void *grown =
      vkr_allocator_realloc(scene->alloc, *array, (uint64_t)*capacity * size,
                            (uint64_t)count * size, ROOMS_TAG);
  if (!grown) {
    return false_v;
  }
  *array = grown;
  *capacity = count;
  return true_v;
}

static Mat4 rooms_entity_world(const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  return transform ? transform->world : mat4_identity();
}

/* Whether the graph still matches the scene: no world or structure change,
   and no room or portal moved. */
static bool8_t rooms_graph_current(const VkrScene *scene,
                                   const VkrSceneRooms *rooms) {
  if (!rooms->graph_valid || rooms->world_revision != scene->world_revision ||
      rooms->structure_revision != scene->structure_revision) {
    return false_v;
  }
  if (rooms->transform_revision == scene->transform_revision) {
    return true_v;
  }
  for (uint32_t i = 0; i < rooms->source_count; ++i) {
    const Mat4 world = rooms_entity_world(scene, rooms->sources[i].entity);
    if (MemCompare(&world, &rooms->sources[i].world, sizeof(Mat4)) != 0) {
      return false_v;
    }
  }
  return true_v;
}

/* The room entity a `room` entity's box belongs to: its parent when that is
   a room too. */
static VkrEntityId rooms_owner(const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  const VkrEntityId parent =
      transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  return parent.u64 && vkr_scene_get_typed(scene, parent, &vkr_scene_room_type)
             ? parent
             : entity;
}

static bool8_t rooms_rebuild_graph(VkrScene *scene, VkrSceneRooms *rooms) {
  VkrEntityId room_entities[VKR_SCENE_ROOM_BOX_MAX];
  VkrEntityId portal_entities[VKR_SCENE_PORTAL_MAX];
  const uint32_t box_count =
      Min(vkr_scene_find_typed(scene, &vkr_scene_room_type, room_entities,
                               VKR_SCENE_ROOM_BOX_MAX),
          VKR_SCENE_ROOM_BOX_MAX);
  const uint32_t portal_count =
      Min(vkr_scene_find_typed(scene, &vkr_scene_portal_type, portal_entities,
                               VKR_SCENE_PORTAL_MAX),
          VKR_SCENE_PORTAL_MAX);
  if (!rooms_reserve(scene, (void **)&rooms->boxes, &rooms->box_capacity,
                     box_count, sizeof(RoomBox)) ||
      !rooms_reserve(scene, (void **)&rooms->portals, &rooms->portal_capacity,
                     portal_count, sizeof(RoomPortal)) ||
      !rooms_reserve(scene, (void **)&rooms->sources, &rooms->source_capacity,
                     box_count + portal_count, sizeof(RoomSource))) {
    return false_v;
  }

  /* Rooms are numbered by their owner entity's first box. */
  VkrEntityId owners[VKR_SCENE_ROOM_MAX];
  rooms->room_count = 0u;
  rooms->box_count = 0u;
  rooms->source_count = 0u;
  MemZero(rooms->sealed, sizeof(rooms->sealed));
  for (uint32_t i = 0; i < box_count; ++i) {
    const VkrEntityId owner = rooms_owner(scene, room_entities[i]);
    uint32_t room = 0u;
    while (room < rooms->room_count && owners[room].u64 != owner.u64) {
      ++room;
    }
    if (room == rooms->room_count) {
      if (rooms->room_count == VKR_SCENE_ROOM_MAX) {
        continue;
      }
      owners[rooms->room_count++] = owner;
    }
    const SceneRoom *value =
        vkr_scene_get_typed(scene, owner, &vkr_scene_room_type);
    if (value && value->sealed) {
      rooms->sealed[room / 64u] |= UINT64_C(1) << (room % 64u);
    }
    const Mat4 world = rooms_entity_world(scene, room_entities[i]);
    RoomBox *box = &rooms->boxes[rooms->box_count++];
    box->world = world;
    box->inverse = mat4_inverse_affine(world);
    box->room = room;
    box->volume = vec3_length(world.cols[0]) * vec3_length(world.cols[1]) *
                  vec3_length(world.cols[2]);
    box->min = vec3_new(INFINITY, INFINITY, INFINITY);
    box->max = vec3_new(-INFINITY, -INFINITY, -INFINITY);
    for (uint32_t corner = 0; corner < 8u; ++corner) {
      const Vec3 point =
          mat4_mul_vec3(world, vec3_new(corner & 1u ? 0.5f : -0.5f,
                                        corner & 2u ? 0.5f : -0.5f,
                                        corner & 4u ? 0.5f : -0.5f));
      box->min = vkr_simd_min_f32x4(box->min, point);
      box->max = vkr_simd_max_f32x4(box->max, point);
    }
    rooms->sources[rooms->source_count++] =
        (RoomSource){.entity = room_entities[i], .world = world};
  }

  /* Each portal joins the rooms on its two sides. */
  rooms->portal_count = 0u;
  for (uint32_t i = 0; i < portal_count; ++i) {
    const ScenePortal *value =
        vkr_scene_get_typed(scene, portal_entities[i], &vkr_scene_portal_type);
    const Mat4 world = rooms_entity_world(scene, portal_entities[i]);
    RoomPortal *portal = &rooms->portals[rooms->portal_count];
    portal->center = mat4_mul_vec3(world, vec3_zero());
    portal->radius = 0.0f;
    for (uint32_t corner = 0; corner < 4u; ++corner) {
      portal->corners[corner] = mat4_mul_vec3(
          world, vec3_new(corner == 1u || corner == 2u ? 0.5f : -0.5f,
                          corner >= 2u ? 0.5f : -0.5f, 0.0f));
      portal->radius =
          Max(portal->radius,
              vec3_distance(portal->corners[corner], portal->center));
    }
    /* The rectangle's axes and normal, also when its Z scale is zero. */
    const Vec3 edge_u = vec3_sub(portal->corners[1], portal->corners[0]);
    const Vec3 edge_v = vec3_sub(portal->corners[3], portal->corners[0]);
    portal->half_u = 0.5f * vec3_length(edge_u);
    portal->half_v = 0.5f * vec3_length(edge_v);
    portal->axis_u = vec3_normalize(edge_u);
    portal->axis_v = vec3_normalize(edge_v);
    portal->normal = vec3_normalize(vec3_cross(edge_u, edge_v));
    const Vec3 probe = vec3_scale(portal->normal, ROOM_PORTAL_PROBE);
    portal->sides[0] =
        room_at(rooms, vec3_add(portal->center, probe), ROOM_INSIDE_EPSILON);
    portal->sides[1] =
        room_at(rooms, vec3_sub(portal->center, probe), ROOM_INSIDE_EPSILON);
    portal->open = value && value->open;
    rooms->sources[rooms->source_count++] =
        (RoomSource){.entity = portal_entities[i], .world = world};
    /* A portal with one room on both sides, or the outside on both, joins
       nothing. */
    if (portal->sides[0] != portal->sides[1]) {
      rooms->portal_count++;
    }
  }

  rooms->world_revision = scene->world_revision;
  rooms->structure_revision = scene->structure_revision;
  rooms->transform_revision = scene->transform_revision;
  rooms->graph_revision++;
  rooms->graph_valid = true_v;
  return true_v;
}

// =============================================================================
// Draws' rooms
// =============================================================================

/* The rooms of a static draw whose local box `min`..`max` the matrix
   `model` places: the rooms within the margin of its bounds, and the
   outside when a corner or its centre lies in no room. */
static uint32_t rooms_of_box(const VkrSceneRooms *rooms, Mat4 model, Vec3 min,
                             Vec3 max) {
  uint32_t entries[2];
  uint32_t entry_count = 0u;
  Vec3 world_min = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 world_max = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  bool8_t outside = false_v;
  for (uint32_t corner = 0; corner < 9u; ++corner) {
    const Vec3 local = corner == 8u ? vec3_scale(vec3_add(min, max), 0.5f)
                                    : vec3_new(corner & 1u ? max.x : min.x,
                                               corner & 2u ? max.y : min.y,
                                               corner & 4u ? max.z : min.z);
    const Vec3 point = mat4_mul_vec3(model, local);
    world_min = vkr_simd_min_f32x4(world_min, point);
    world_max = vkr_simd_max_f32x4(world_max, point);
    outside = outside || room_at(rooms, point, ROOM_INSIDE_EPSILON) ==
                             VKR_SCENE_ROOM_OUTSIDE;
  }
  if (outside) {
    entries[entry_count++] = VKR_SCENE_ROOM_OUTSIDE;
  }
  const Vec3 center = vec3_scale(vec3_add(world_min, world_max), 0.5f);
  const float32_t radius =
      0.5f * vec3_distance(world_max, world_min) + VKR_SCENE_ROOM_MARGIN;
  for (uint32_t i = 0; i < rooms->box_count; ++i) {
    const RoomBox *box = &rooms->boxes[i];
    if (box->min.x > world_max.x + VKR_SCENE_ROOM_MARGIN ||
        box->max.x < world_min.x - VKR_SCENE_ROOM_MARGIN ||
        box->min.y > world_max.y + VKR_SCENE_ROOM_MARGIN ||
        box->max.y < world_min.y - VKR_SCENE_ROOM_MARGIN ||
        box->min.z > world_max.z + VKR_SCENE_ROOM_MARGIN ||
        box->max.z < world_min.z - VKR_SCENE_ROOM_MARGIN ||
        room_box_distance(box, center) > radius) {
      continue;
    }
    const uint32_t room = box->room;
    if ((entry_count > 0u && entries[0] == room) ||
        (entry_count > 1u && entries[1] == room)) {
      continue;
    }
    if (entry_count == 2u) {
      return VKR_SCENE_ROOMS_UNASSIGNED;
    }
    entries[entry_count++] = room;
  }
  if (entry_count == 0u) {
    return VKR_SCENE_ROOM_OUTSIDE | (VKR_SCENE_ROOM_NONE << 16u);
  }
  return entries[0] |
         ((entry_count > 1u ? entries[1] : VKR_SCENE_ROOM_NONE) << 16u);
}

/* Marks `slot` of the mesh or instance table as outside: terrains and
   scatter copies belong to it whatever their bounds touch. */
static void rooms_mark_outside(uint32_t *first, uint32_t capacity,
                               uint32_t slot) {
  if (slot < capacity) {
    first[slot] = UINT32_MAX - 1u;
  }
}

static bool8_t rooms_rebuild_membership(VkrScene *scene, VkrSceneRooms *rooms) {
  VkrMeshManager *meshes = &scene->assets->mesh_manager;
  const uint32_t mesh_slots = vkr_mesh_manager_capacity(meshes);
  const uint32_t instance_slots = vkr_mesh_manager_instance_capacity(meshes);
  if (!rooms_reserve(scene, (void **)&rooms->mesh_first, &rooms->mesh_capacity,
                     mesh_slots, sizeof(uint32_t)) ||
      !rooms_reserve(scene, (void **)&rooms->instance_first,
                     &rooms->instance_capacity, instance_slots,
                     sizeof(uint32_t))) {
    return false_v;
  }
  /* UINT32_MAX: unassigned; UINT32_MAX - 1: the outside, set below. */
  MemSet(rooms->mesh_first, 0xFF, (uint64_t)mesh_slots * sizeof(uint32_t));
  MemSet(rooms->instance_first, 0xFF,
         (uint64_t)instance_slots * sizeof(uint32_t));

  VkrEntityId terrains[64];
  const uint32_t terrain_count =
      Min(vkr_scene_find_typed(scene, &vkr_scene_terrain_type, terrains,
                               ArrayCount(terrains)),
          ArrayCount(terrains));
  for (uint32_t i = 0; i < terrain_count; ++i) {
    const SceneShape *shape = vkr_entity_get_component_if_alive_const(
        scene->world, terrains[i], scene->comp_shape);
    if (shape && shape->mesh_index != VKR_INVALID_ID) {
      rooms_mark_outside(rooms->mesh_first, mesh_slots, shape->mesh_index);
    }
  }
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&scene->assets->scratch_allocator);
  VkrMeshInstanceHandle *copies = vkr_allocator_alloc(
      &scene->assets->scratch_allocator,
      sizeof(VkrMeshInstanceHandle) * VKR_SCENE_POPULATION_INSTANCE_MAX,
      ROOMS_TAG);
  const uint32_t copy_count =
      copies ? vkr_scene_population_copies(scene, copies,
                                           VKR_SCENE_POPULATION_INSTANCE_MAX)
             : 0u;
  for (uint32_t i = 0; i < Min(copy_count, VKR_SCENE_POPULATION_INSTANCE_MAX);
       ++i) {
    rooms_mark_outside(rooms->instance_first, instance_slots,
                       copies[i].id - 1u);
  }
  vkr_allocator_end_scope(&scope, ROOMS_TAG);

  /* Count the static submeshes, then fill their words. */
  uint32_t word_count = 0u;
  const uint32_t mesh_count = vkr_mesh_manager_count(meshes);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t slot = 0u;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &slot);
    if (mesh && mesh->shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_STATIC &&
        mesh->bounds_valid && rooms->mesh_first[slot] == UINT32_MAX) {
      word_count += vkr_mesh_manager_submesh_count(mesh);
    }
  }
  const uint32_t instance_count = vkr_mesh_manager_instance_count(meshes);
  for (uint32_t i = 0; i < instance_count; ++i) {
    uint32_t slot = 0u;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &slot);
    VkrMeshAsset *asset =
        instance ? vkr_mesh_manager_get_live_asset(meshes, instance->asset)
                 : NULL;
    if (asset && !instance->skinning &&
        instance->shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_STATIC &&
        rooms->instance_first[slot] == UINT32_MAX) {
      word_count += (uint32_t)asset->submeshes.length;
    }
  }
  /* One word stands for every draw of the outside. */
  word_count += 1u;
  if (!rooms_reserve(scene, (void **)&rooms->words, &rooms->word_capacity,
                     word_count, sizeof(uint32_t))) {
    return false_v;
  }
  const uint32_t outside_word = 0u;
  rooms->words[outside_word] =
      VKR_SCENE_ROOM_OUTSIDE | (VKR_SCENE_ROOM_NONE << 16u);
  uint32_t cursor = 1u;
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t slot = 0u;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &slot);
    if (!mesh) {
      continue;
    }
    if (rooms->mesh_first[slot] == UINT32_MAX - 1u) {
      rooms->mesh_first[slot] = outside_word | 0x80000000u;
      continue;
    }
    if (mesh->shadow_mobility != VKR_SHADOW_CASTER_MOBILITY_STATIC ||
        !mesh->bounds_valid) {
      continue;
    }
    rooms->mesh_first[slot] = cursor;
    const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      const VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(meshes, slot, s);
      rooms->words[cursor++] = rooms_of_box(
          rooms, mesh->model, submesh->min_extents, submesh->max_extents);
    }
  }
  for (uint32_t i = 0; i < instance_count; ++i) {
    uint32_t slot = 0u;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &slot);
    VkrMeshAsset *asset =
        instance ? vkr_mesh_manager_get_live_asset(meshes, instance->asset)
                 : NULL;
    if (!asset) {
      continue;
    }
    if (rooms->instance_first[slot] == UINT32_MAX - 1u) {
      rooms->instance_first[slot] = outside_word | 0x80000000u;
      continue;
    }
    if (instance->skinning ||
        instance->shadow_mobility != VKR_SHADOW_CASTER_MOBILITY_STATIC) {
      continue;
    }
    rooms->instance_first[slot] = cursor;
    for (uint32_t s = 0; s < asset->submeshes.length; ++s) {
      const VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      rooms->words[cursor++] = rooms_of_box(
          rooms, instance->model, submesh->min_extents, submesh->max_extents);
    }
  }
  /* Slots marked outside but not live stay unassigned. */
  for (uint32_t slot = 0; slot < mesh_slots; ++slot) {
    if (rooms->mesh_first[slot] == UINT32_MAX - 1u) {
      rooms->mesh_first[slot] = UINT32_MAX;
    }
  }
  for (uint32_t slot = 0; slot < instance_slots; ++slot) {
    if (rooms->instance_first[slot] == UINT32_MAX - 1u) {
      rooms->instance_first[slot] = UINT32_MAX;
    }
  }
  rooms->word_count = cursor;
  return true_v;
}

/* A hash of what the sealed rooms keep out of the cascades: the sealed set
   and every draw's rooms, or zero without a sealed room. */
static uint64_t rooms_sealed_hash(const VkrSceneRooms *rooms) {
  uint64_t any = 0u;
  for (uint32_t i = 0; i < ArrayCount(rooms->sealed); ++i) {
    any |= rooms->sealed[i];
  }
  if (!any) {
    return 0u;
  }
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  const uint8_t *bytes = (const uint8_t *)rooms->sealed;
  for (uint64_t i = 0; i < sizeof(rooms->sealed); ++i) {
    hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
  }
  bytes = (const uint8_t *)rooms->words;
  for (uint64_t i = 0; i < (uint64_t)rooms->word_count * sizeof(uint32_t);
       ++i) {
    hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
  }
  return hash | 1u;
}

// =============================================================================
// Walk
// =============================================================================

/* Index of a node in `explored`: a room, or the outside after them. */
static uint32_t rooms_node_slot(const VkrSceneRooms *rooms, uint32_t node) {
  return node == VKR_SCENE_ROOM_OUTSIDE ? rooms->room_count : node;
}

/* Widens what the walk has looked through `node` by `rect` and returns the
   rectangle to walk it with, or false when `rect` adds nothing. */
static bool8_t rooms_explore(VkrSceneRooms *rooms, uint32_t node, Vec4 rect,
                             Vec4 *out_rect) {
  Vec4 *explored = &rooms->explored[rooms_node_slot(rooms, node)];
  if (rect.x >= explored->x && rect.y >= explored->y && rect.z <= explored->z &&
      rect.w <= explored->w) {
    return false_v;
  }
  *explored = vec4_new(Min(explored->x, rect.x), Min(explored->y, rect.y),
                       Max(explored->z, rect.z), Max(explored->w, rect.w));
  *out_rect = *explored;
  return true_v;
}

static void rooms_show(VkrSceneRooms *rooms, uint32_t node) {
  if (node == VKR_SCENE_ROOM_OUTSIDE) {
    rooms->frame.outside_visible = true_v;
  } else if (node < rooms->room_count) {
    rooms->visible[node / 64u] |= UINT64_C(1) << (node % 64u);
  }
}

/* Clip-space W below which a point lies behind the camera. */
#define ROOM_CLIP_W_MIN 1e-4f

/* The screen rectangle (NDC min x, min y, max x, max y) a portal covers
   inside `rect`, and whether any of it is left. The rectangle is clipped to
   the camera's side first, so a portal partly behind it covers only what
   lies in front. */
static bool8_t rooms_portal_rect(const RoomPortal *portal, Mat4 view_projection,
                                 Vec4 rect, Vec4 *out) {
  Vec4 clip[4];
  for (uint32_t i = 0; i < 4u; ++i) {
    const Vec3 p = portal->corners[i];
    clip[i] = mat4_mul_vec4(view_projection, vec4_new(p.x, p.y, p.z, 1.0f));
  }
  Vec4 covered = vec4_new(INFINITY, INFINITY, -INFINITY, -INFINITY);
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < 4u; ++i) {
    const Vec4 a = clip[i];
    const Vec4 b = clip[(i + 1u) % 4u];
    /* Each corner in front, and where an edge crosses to the back. */
    Vec4 points[2];
    uint32_t count = 0u;
    if (a.w > ROOM_CLIP_W_MIN) {
      points[count++] = a;
    }
    if ((a.w > ROOM_CLIP_W_MIN) != (b.w > ROOM_CLIP_W_MIN)) {
      const float32_t t = (ROOM_CLIP_W_MIN - a.w) / (b.w - a.w);
      points[count++] = vec4_new(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                 a.z + (b.z - a.z) * t, ROOM_CLIP_W_MIN);
    }
    for (uint32_t k = 0; k < count; ++k) {
      const float32_t x = points[k].x / points[k].w;
      const float32_t y = points[k].y / points[k].w;
      covered = vec4_new(Min(covered.x, x), Min(covered.y, y),
                         Max(covered.z, x), Max(covered.w, y));
      ++kept;
    }
  }
  if (!kept) {
    return false_v;
  }
  *out = vec4_new(Max(covered.x, rect.x), Max(covered.y, rect.y),
                  Min(covered.z, rect.z), Min(covered.w, rect.w));
  return out->x < out->z && out->y < out->w;
}

/* Whether the camera stands in a portal's opening, where its view passes
   through all of it. */
static bool8_t rooms_in_opening(const RoomPortal *portal, Vec3 camera) {
  const Vec3 offset = vec3_sub(camera, portal->center);
  const float32_t reach = 0.5f;
  return fabsf(vec3_dot(offset, portal->normal)) <= reach &&
         fabsf(vec3_dot(offset, portal->axis_u)) <= portal->half_u + reach &&
         fabsf(vec3_dot(offset, portal->axis_v)) <= portal->half_v + reach;
}

static void rooms_walk(VkrSceneRooms *rooms, uint32_t start, Vec3 camera,
                       Mat4 view_projection) {
  const VkrFrustum frustum = vkr_frustum_from_matrix(view_projection);
  for (uint32_t i = 0; i <= rooms->room_count; ++i) {
    rooms->explored[i] = vec4_new(INFINITY, INFINITY, -INFINITY, -INFINITY);
  }
  uint32_t top = 0u;
  uint32_t steps = 0u;
  const Vec4 screen = vec4_new(-1.0f, -1.0f, 1.0f, 1.0f);
  rooms->walk[top++] = (RoomWalk){.node = start, .rect = screen};
  rooms->explored[rooms_node_slot(rooms, start)] = screen;
  rooms_show(rooms, start);
  while (top > 0u && steps < ROOM_WALK_STEP_MAX) {
    const RoomWalk at = rooms->walk[--top];
    ++steps;
    for (uint32_t i = 0; i < rooms->portal_count; ++i) {
      const RoomPortal *portal = &rooms->portals[i];
      const uint32_t side = portal->sides[0] == at.node   ? 1u
                            : portal->sides[1] == at.node ? 0u
                                                          : 2u;
      if (side == 2u || !portal->open ||
          !vkr_frustum_test_sphere(&frustum, portal->center, portal->radius)) {
        continue;
      }
      Vec4 rect = at.rect;
      /* A camera in the opening sees through all of it. */
      if (!rooms_in_opening(portal, camera) &&
          !rooms_portal_rect(portal, view_projection, at.rect, &rect)) {
        continue;
      }
      const uint32_t next = portal->sides[side];
      rooms_show(rooms, next);
      Vec4 walk_rect = rect;
      if (top < ROOM_WALK_STEP_MAX &&
          rooms_explore(rooms, next, rect, &walk_rect)) {
        rooms->walk[top++] = (RoomWalk){.node = next, .rect = walk_rect};
      }
    }
  }
}

// =============================================================================
// Frame
// =============================================================================

const VkrSceneRoomFrame *vkr_scene_rooms_frame(VkrScene *scene, Vec3 camera,
                                               Mat4 view_projection) {
  if (!scene || vkr_scene_type_id(scene, &vkr_scene_room_type) ==
                    VKR_COMPONENT_TYPE_INVALID) {
    return NULL;
  }
  VkrSceneRooms *rooms = scene->rooms;
  if (!rooms) {
    /* Adding a room changes the structure, so a scene without one is
       searched once per structure revision. */
    if (scene->rooms_absent_structure == scene->structure_revision + 1u) {
      return NULL;
    }
    if (!vkr_scene_find_typed(scene, &vkr_scene_room_type, NULL, 0u)) {
      scene->rooms_absent_structure = scene->structure_revision + 1u;
      return NULL;
    }
  }
  rooms = rooms ? rooms : rooms_state(scene);
  if (!rooms) {
    return NULL;
  }
  if (!rooms_graph_current(scene, rooms) &&
      !rooms_rebuild_graph(scene, rooms)) {
    rooms->graph_valid = false_v;
    return NULL;
  }
  if (rooms->room_count == 0u) {
    return NULL;
  }

  /* A scene without assets, as in a CPU test, has no draws to assign. */
  const VkrMeshManager *meshes =
      scene->assets ? &scene->assets->mesh_manager : NULL;
  const uint64_t population = vkr_scene_population_revision(scene);
  if (meshes &&
      (!rooms->membership_valid ||
       rooms->membership_graph != rooms->graph_revision ||
       rooms->membership_topology != meshes->generations.topology ||
       rooms->membership_static != meshes->generations.static_content ||
       rooms->membership_population != population)) {
    rooms->membership_valid = rooms_rebuild_membership(scene, rooms);
    rooms->membership_graph = rooms->graph_revision;
    rooms->membership_topology = meshes->generations.topology;
    rooms->membership_static = meshes->generations.static_content;
    rooms->membership_population = population;
    if (!rooms->membership_valid) {
      return NULL;
    }
    /* Retained cascades hold the casters of the old sealed set; a change
       redraws them all, once. */
    const uint64_t sealed_hash = rooms_sealed_hash(rooms);
    if (sealed_hash != rooms->sealed_hash) {
      rooms->sealed_hash = sealed_hash;
      vkr_mesh_manager_note_caster_change(&scene->assets->mesh_manager);
      rooms->membership_topology = meshes->generations.topology;
      rooms->membership_static = meshes->generations.static_content;
    }
  }

  MemZero(rooms->visible, sizeof(rooms->visible));
  rooms->frame.outside_visible = false_v;
  rooms_walk(rooms, room_at(rooms, camera, 0.0f), camera, view_projection);
  rooms->frame = (VkrSceneRoomFrame){
      .mesh_first = rooms->mesh_first,
      .mesh_slot_count = meshes ? vkr_mesh_manager_capacity(meshes) : 0u,
      .instance_first = rooms->instance_first,
      .instance_slot_count =
          meshes ? vkr_mesh_manager_instance_capacity(meshes) : 0u,
      .words = rooms->words,
      .word_count = meshes ? rooms->word_count : 0u,
      .visible = rooms->visible,
      .sealed = rooms->sealed,
      .room_count = rooms->room_count,
      .outside_visible = rooms->frame.outside_visible,
  };
  return &rooms->frame;
}

/* Whether an entry of a draw's rooms is visible; none is not. */
static bool8_t rooms_entry_visible(const VkrSceneRoomFrame *frame,
                                   uint32_t entry) {
  if (entry == VKR_SCENE_ROOM_OUTSIDE) {
    return frame->outside_visible;
  }
  return entry < frame->room_count &&
         (frame->visible[entry / 64u] >> (entry % 64u)) & 1u;
}

static bool8_t rooms_entry_sealed(const VkrSceneRoomFrame *frame,
                                  uint32_t entry) {
  return entry < frame->room_count &&
         (frame->sealed[entry / 64u] >> (entry % 64u)) & 1u;
}

uint32_t vkr_scene_room_cull_bits(const VkrSceneRoomFrame *frame,
                                  bool8_t instance, uint32_t slot,
                                  uint32_t submesh) {
  if (!frame) {
    return 0u;
  }
  const uint32_t *first = instance ? frame->instance_first : frame->mesh_first;
  const uint32_t slot_count =
      instance ? frame->instance_slot_count : frame->mesh_slot_count;
  if (slot >= slot_count || first[slot] == UINT32_MAX) {
    return 0u;
  }
  /* The high bit marks a draw whose every submesh is the outside. */
  const uint32_t index = first[slot] & 0x80000000u ? first[slot] & 0x7FFFFFFFu
                                                   : first[slot] + submesh;
  const uint32_t word = index < frame->word_count ? frame->words[index]
                                                  : VKR_SCENE_ROOMS_UNASSIGNED;
  if (word == VKR_SCENE_ROOMS_UNASSIGNED) {
    return 0u;
  }
  const uint32_t a = word & 0xFFFFu;
  const uint32_t b = word >> 16u;
  uint32_t bits = 0u;
  if (!rooms_entry_visible(frame, a) &&
      (b == VKR_SCENE_ROOM_NONE || !rooms_entry_visible(frame, b))) {
    bits |= VKR_GPU_DRAW_CULL_HIDDEN_BIT;
  }
  if (rooms_entry_sealed(frame, a) &&
      (b == VKR_SCENE_ROOM_NONE || rooms_entry_sealed(frame, b))) {
    bits |= VKR_GPU_DRAW_CULL_SEALED_BIT;
  }
  return bits;
}

uint32_t vkr_scene_room_at(const VkrScene *scene, Vec3 point) {
  const VkrSceneRooms *rooms = scene ? scene->rooms : NULL;
  return rooms && rooms->graph_valid ? room_at(rooms, point, 0.0f)
                                     : VKR_SCENE_ROOM_OUTSIDE;
}

void vkr_scene_rooms_shutdown(VkrScene *scene) {
  VkrSceneRooms *rooms = scene ? scene->rooms : NULL;
  if (!rooms) {
    return;
  }
  vkr_allocator_free(scene->alloc, rooms->boxes,
                     (uint64_t)rooms->box_capacity * sizeof(RoomBox),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms->portals,
                     (uint64_t)rooms->portal_capacity * sizeof(RoomPortal),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms->sources,
                     (uint64_t)rooms->source_capacity * sizeof(RoomSource),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms->mesh_first,
                     (uint64_t)rooms->mesh_capacity * sizeof(uint32_t),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms->instance_first,
                     (uint64_t)rooms->instance_capacity * sizeof(uint32_t),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms->words,
                     (uint64_t)rooms->word_capacity * sizeof(uint32_t),
                     ROOMS_TAG);
  vkr_allocator_free(scene->alloc, rooms, sizeof(*rooms),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->rooms = NULL;
}
