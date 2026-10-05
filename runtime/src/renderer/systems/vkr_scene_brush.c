#include "renderer/systems/vkr_scene_brush.h"

#include "core/logger.h"
#include "level/vkr_brush.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BRUSH_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Updates a moved brush waits, unmoved, before it rebuilds. */
#define BRUSH_SETTLE_UPDATES 2u
/* Collision bodies hold at most this many brush hulls. */
#define BRUSH_CHUNK_HULLS VKR_PHYSICS_MAX_COLLIDERS
/* Distinct face materials one brush may use; more fall back to the
   default. */
#define BRUSH_MATERIAL_GROUPS 16u

typedef struct BrushRecord {
  VkrEntityId entity;
  /* World matrix the products were built for, and the one last seen. */
  Mat4 built_world;
  Mat4 last_world;
  /* Collision cell this brush's hull joined, or zero. */
  uint64_t cell;
  /* World-space hull points of a solid or clip brush, xyz packed. */
  float32_t *hull;
  uint32_t hull_count;
  uint32_t hull_capacity;
  uint32_t serial;
  uint8_t settle;
  bool8_t built;
  bool8_t dirty;
  bool8_t trigger_body;
  char status[96];
} BrushRecord;

typedef struct BrushMaterial {
  char path[SCENE_BRUSH_MATERIAL_CAPACITY];
  VkrMaterialHandle handle;
  /* The cache holds one reference; false for the default fallback. */
  bool8_t owned;
} BrushMaterial;

typedef struct BrushCell {
  uint64_t cell;
  uint32_t chunks;
} BrushCell;

/* A rebuilt mesh waiting for its uploads. The brush draws its old mesh until
   every geometry and material settles, so a rebuild never leaves a frame
   without the brush (level toolkit audit A1). Holds one reference to each
   geometry. */
typedef struct BrushPending {
  VkrEntityId entity;
  VkrSubMeshDesc submeshes[BRUSH_MATERIAL_GROUPS];
  uint32_t submesh_count;
} BrushPending;

struct s_VkrSceneBrushes {
  BrushRecord *records;
  uint32_t record_count;
  uint32_t record_capacity;
  uint64_t *dirty_cells;
  uint32_t dirty_cell_count;
  uint32_t dirty_cell_capacity;
  BrushCell *cells;
  uint32_t cell_count;
  uint32_t cell_capacity;
  BrushMaterial *materials;
  uint32_t material_count;
  uint32_t material_capacity;
  BrushPending *pending;
  uint32_t pending_count;
  uint32_t pending_capacity;
  /* Scratch for one rebuild; the scene rebuilds on one thread. */
  VkrBrushGeometry *geometry;
  uint32_t serial;
};

// =============================================================================
// Storage
// =============================================================================

static bool8_t brush_grow(VkrAllocator *allocator, void **data,
                          uint32_t *capacity, uint32_t needed,
                          uint64_t element) {
  if (needed <= *capacity) {
    return true_v;
  }
  const uint32_t next = Max(needed, Max(16u, *capacity * 2u));
  void *grown = vkr_allocator_realloc(allocator, *data, *capacity * element,
                                      next * element, BRUSH_TAG);
  if (!grown) {
    return false_v;
  }
  *data = grown;
  *capacity = next;
  return true_v;
}

static VkrSceneBrushes *brush_state(VkrScene *scene, bool8_t create) {
  if (scene->brushes || !create) {
    return scene->brushes;
  }
  VkrSceneBrushes *state = vkr_allocator_alloc(scene->alloc, sizeof(*state),
                                               VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry = vkr_allocator_alloc(
      scene->alloc, sizeof(*geometry), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state || !geometry) {
    if (state) {
      vkr_allocator_free(scene->alloc, state, sizeof(*state),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    }
    if (geometry) {
      vkr_allocator_free(scene->alloc, geometry, sizeof(*geometry),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    }
    return NULL;
  }
  MemZero(state, sizeof(*state));
  state->geometry = geometry;
  scene->brushes = state;
  return state;
}

static int32_t brush_find(const VkrSceneBrushes *state, VkrEntityId entity) {
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    if (state->records[i].entity.u64 == entity.u64) {
      return (int32_t)i;
    }
  }
  return -1;
}

static void brush_mark_cell(VkrScene *scene, VkrSceneBrushes *state,
                            uint64_t cell) {
  if (!cell) {
    return;
  }
  for (uint32_t i = 0; i < state->dirty_cell_count; ++i) {
    if (state->dirty_cells[i] == cell) {
      return;
    }
  }
  if (brush_grow(scene->alloc, (void **)&state->dirty_cells,
                 &state->dirty_cell_capacity, state->dirty_cell_count + 1u,
                 sizeof(*state->dirty_cells))) {
    state->dirty_cells[state->dirty_cell_count++] = cell;
  }
}

/* Marks the brush `entity` for a rebuild in the next update. */
static void brush_mark(VkrScene *scene, VkrEntityId entity) {
  if (!entity.u64 ||
      !vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type)) {
    return;
  }
  VkrSceneBrushes *state = brush_state(scene, true_v);
  if (!state) {
    return;
  }
  int32_t index = brush_find(state, entity);
  if (index < 0) {
    if (!brush_grow(scene->alloc, (void **)&state->records,
                    &state->record_capacity, state->record_count + 1u,
                    sizeof(*state->records))) {
      log_error("Scene: no memory to track brush %u",
                (unsigned)entity.parts.index);
      return;
    }
    index = (int32_t)state->record_count++;
    BrushRecord *record = &state->records[index];
    MemZero(record, sizeof(*record));
    record->entity = entity;
    snprintf(record->status, sizeof(record->status), "pending");
  }
  BrushRecord *record = &state->records[index];
  record->dirty = true_v;
  record->settle = BRUSH_SETTLE_UPDATES;
}

static uint64_t brush_trigger_key(VkrEntityId entity) {
  return (UINT64_C(3) << 62) | (entity.u64 & UINT64_C(0xFFFFFFFFFFFF));
}

/* Drops a brush's mesh and collision; the record stays. */
static void brush_pending_release(VkrScene *scene, BrushPending *pending) {
  for (uint32_t i = 0; i < pending->submesh_count; ++i) {
    vkr_geometry_system_release(&scene->assets->geometry_system,
                                pending->submeshes[i].geometry);
  }
  pending->submesh_count = 0u;
}

/* Drops `entity`'s waiting mesh, if it has one. */
static void brush_pending_drop(VkrScene *scene, VkrSceneBrushes *state,
                               VkrEntityId entity) {
  for (uint32_t i = 0; i < state->pending_count; ++i) {
    if (state->pending[i].entity.u64 == entity.u64) {
      brush_pending_release(scene, &state->pending[i]);
      state->pending[i] = state->pending[--state->pending_count];
      return;
    }
  }
}

static void brush_clear(VkrScene *scene, VkrSceneBrushes *state,
                        BrushRecord *record) {
  brush_pending_drop(scene, state, record->entity);
  vkr_scene_detach_generated_mesh(scene, record->entity);
  if (record->cell) {
    brush_mark_cell(scene, state, record->cell);
    record->cell = 0u;
  }
  record->hull_count = 0u;
  if (record->trigger_body) {
    vkr_scene_physics_generated_remove(scene,
                                       brush_trigger_key(record->entity));
    record->trigger_body = false_v;
  }
  record->built = false_v;
}

static void brush_forget(VkrScene *scene, VkrSceneBrushes *state,
                         uint32_t index) {
  BrushRecord *record = &state->records[index];
  brush_clear(scene, state, record);
  if (record->hull) {
    vkr_allocator_free(scene->alloc, record->hull,
                       record->hull_capacity * 3u * sizeof(float32_t),
                       BRUSH_TAG);
  }
  state->records[index] = state->records[--state->record_count];
}

// =============================================================================
// Change hooks
// =============================================================================

static VkrEntityId brush_parent(const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  return transform ? transform->parent : VKR_ENTITY_ID_INVALID;
}

void vkr_scene_brush_changed(VkrScene *scene, VkrEntityId entity,
                             const VkrTypeDesc *type) {
  if (!scene || !entity.u64) {
    return;
  }
  if (type == &vkr_scene_brush_type) {
    if (vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type)) {
      brush_mark(scene, entity);
      return;
    }
    VkrSceneBrushes *state = brush_state(scene, false_v);
    const int32_t index = brush_find(state, entity);
    if (index >= 0) {
      brush_forget(scene, state, (uint32_t)index);
    }
    return;
  }
  if (type == &vkr_scene_brush_face_type) {
    brush_mark(scene, brush_parent(scene, entity));
  }
}

void vkr_scene_brush_parent_changed(VkrScene *scene, VkrEntityId entity,
                                    VkrEntityId old_parent,
                                    VkrEntityId new_parent) {
  if (scene && vkr_scene_get_typed(scene, entity, &vkr_scene_brush_face_type)) {
    brush_mark(scene, old_parent);
    brush_mark(scene, new_parent);
  }
}

void vkr_scene_brush_entity_destroying(VkrScene *scene, VkrEntityId entity) {
  if (!scene) {
    return;
  }
  if (vkr_scene_get_typed(scene, entity, &vkr_scene_brush_face_type)) {
    brush_mark(scene, brush_parent(scene, entity));
  }
  VkrSceneBrushes *state = brush_state(scene, false_v);
  const int32_t index = brush_find(state, entity);
  if (index >= 0) {
    brush_forget(scene, state, (uint32_t)index);
  }
}

// =============================================================================
// Materials
// =============================================================================

/* The material for `path`, cached for the scene's lifetime; the default
   material when the file does not load. */
static VkrMaterialHandle brush_material(VkrScene *scene, VkrSceneBrushes *state,
                                        const char *path, bool8_t *out_owned) {
  struct VkrRenderAssets *assets = scene->assets;
  if (!path[0]) {
    path = VKR_SCENE_BRUSH_DEFAULT_MATERIAL;
  }
  for (uint32_t i = 0; i < state->material_count; ++i) {
    if (strcmp(state->materials[i].path, path) == 0) {
      *out_owned = state->materials[i].owned;
      return state->materials[i].handle;
    }
  }
  (void)assets;
  BrushMaterial entry = {0};
  snprintf(entry.path, sizeof(entry.path), "%s", path);
  entry.handle = vkr_scene_material_load(scene, path, &entry.owned);
  if (brush_grow(scene->alloc, (void **)&state->materials,
                 &state->material_capacity, state->material_count + 1u,
                 sizeof(*state->materials))) {
    state->materials[state->material_count++] = entry;
  }
  *out_owned = entry.owned;
  return entry.handle;
}

// =============================================================================
// Rebuild
// =============================================================================

uint32_t vkr_scene_brush_faces(const VkrScene *scene, VkrEntityId brush,
                               VkrEntityId *out, uint32_t capacity) {
  uint32_t total = 0u;
  for (uint32_t i = 0; scene && i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    if (vkr_scene_entity_alive(scene, child) &&
        brush_parent(scene, child).u64 == brush.u64 &&
        vkr_scene_get_typed(scene, child, &vkr_scene_brush_face_type)) {
      if (total < capacity) {
        out[total] = child;
      }
      total++;
    }
  }
  return total;
}

/* Direction `v` through the inverse transpose of `m`'s upper 3x3. */
static Vec3 brush_normal_to_world(const Mat4 *inverse, Vec3 v) {
  const float32_t *e = inverse->elements;
  return vec3_normalize(vec3_new(e[0] * v.x + e[1] * v.y + e[2] * v.z,
                                 e[4] * v.x + e[5] * v.y + e[6] * v.z,
                                 e[8] * v.x + e[9] * v.y + e[10] * v.z));
}

static Vec3 brush_direction(const Mat4 *m, Vec3 v) {
  const Vec4 r = mat4_mul_vec4(*m, vec3_to_vec4(v, 0.0f));
  return vec3_new(r.x, r.y, r.z);
}

static uint64_t brush_cell_key(Vec3 point) {
  const int64_t x = (int64_t)floorf(point.x / VKR_SCENE_BRUSH_CELL_SIZE);
  const int64_t y = (int64_t)floorf(point.y / VKR_SCENE_BRUSH_CELL_SIZE);
  const int64_t z = (int64_t)floorf(point.z / VKR_SCENE_BRUSH_CELL_SIZE);
  const uint64_t mask = (UINT64_C(1) << 18) - 1u;
  return (UINT64_C(1) << 63) | (((uint64_t)x & mask) << 44) |
         (((uint64_t)y & mask) << 26) | (((uint64_t)z & mask) << 8);
}

/* Writes one face's polygon as a triangle fan with Hammer-style UVs. */
static void brush_write_face(const VkrBrushGeometry *geometry, uint32_t face,
                             const SceneBrushFace *settings, const Mat4 *world,
                             const Mat4 *world_inverse, VkrVertex3d *vertices,
                             uint32_t *vertex_count, uint32_t *indices,
                             uint32_t *index_count) {
  const VkrBrushPolygon polygon = geometry->polygons[face];
  const Vec3 normal = geometry->normals[face];
  /* World-aligned faces project in world space; the tangent returns to the
     brush's space, where the mesh lives. */
  const bool8_t world_uv = settings->uv_world;
  const Vec3 projection_normal =
      world_uv ? brush_normal_to_world(world_inverse, normal) : normal;
  Vec3 u = {0};
  Vec3 v = {0};
  vkr_brush_texture_axes(projection_normal, &u, &v);
  const float32_t c = cosf(settings->uv_rotation);
  const float32_t s = sinf(settings->uv_rotation);
  Vec3 tangent = vec3_sub(vec3_scale(u, c), vec3_scale(v, s));
  Vec3 bitangent = vec3_add(vec3_scale(u, s), vec3_scale(v, c));
  if (world_uv) {
    tangent = vec3_normalize(brush_direction(world_inverse, tangent));
    bitangent = brush_direction(world_inverse, bitangent);
  }
  const float32_t handedness =
      vec3_dot(vec3_cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
  const uint32_t first = *vertex_count;
  for (uint32_t i = 0; i < polygon.count; ++i) {
    const Vec3 local = geometry->vertices[polygon.first + i];
    const Vec3 projected = world_uv ? mat4_mul_vec3(*world, local) : local;
    vertices[*vertex_count] = (VkrVertex3d){
        .position = vkr_vertex_pack_vec3(local),
        .normal = vkr_vertex_pack_vec3(normal),
        .texcoord =
            vkr_brush_uv(projected, projection_normal, settings->uv_offset,
                         settings->uv_scale, settings->uv_rotation),
        .colour = vec4_one(),
        .tangent = vec4_new(tangent.x, tangent.y, tangent.z, handedness),
    };
    (*vertex_count)++;
  }
  for (uint32_t i = 2; i < polygon.count; ++i) {
    indices[(*index_count)++] = first;
    indices[(*index_count)++] = first + i - 1u;
    indices[(*index_count)++] = first + i;
  }
}

/* Builds the brush's mesh, one submesh per distinct face material. */
static bool8_t brush_build_mesh(VkrScene *scene, VkrSceneBrushes *state,
                                BrushRecord *record,
                                const SceneBrushFace *const *faces,
                                const Mat4 *world) {
  struct VkrRenderAssets *assets = scene->assets;
  const VkrBrushGeometry *geometry = state->geometry;
  const Mat4 world_inverse = mat4_inverse_affine(*world);
  /* Group faces by material file. */
  const char *group_paths[BRUSH_MATERIAL_GROUPS];
  uint32_t group_count = 0u;
  uint32_t face_group[VKR_BRUSH_FACE_MAX];
  for (uint32_t face = 0; face < geometry->face_count; ++face) {
    const char *path = faces[face]->material;
    uint32_t group = 0u;
    while (group < group_count && strcmp(group_paths[group], path) != 0) {
      group++;
    }
    if (group == group_count) {
      if (group_count == BRUSH_MATERIAL_GROUPS) {
        group = 0u;
      } else {
        group_paths[group_count++] = path;
      }
    }
    face_group[face] = group;
  }
  VkrSubMeshDesc submeshes[BRUSH_MATERIAL_GROUPS];
  uint32_t submesh_count = 0u;
  bool8_t ok = true_v;
  record->serial = ++state->serial;
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  for (uint32_t group = 0; group < group_count && ok; ++group) {
    uint32_t vertex_capacity = 0u;
    uint32_t index_capacity = 0u;
    for (uint32_t face = 0; face < geometry->face_count; ++face) {
      if (face_group[face] == group) {
        vertex_capacity += geometry->polygons[face].count;
        index_capacity += (geometry->polygons[face].count - 2u) * 3u;
      }
    }
    VkrVertex3d *vertices =
        vkr_allocator_alloc(&assets->scratch_allocator,
                            vertex_capacity * sizeof(*vertices), BRUSH_TAG);
    uint32_t *indices =
        vkr_allocator_alloc(&assets->scratch_allocator,
                            index_capacity * sizeof(*indices), BRUSH_TAG);
    if (!vertices || !indices) {
      ok = false_v;
      break;
    }
    uint32_t vertex_count = 0u;
    uint32_t index_count = 0u;
    for (uint32_t face = 0; face < geometry->face_count; ++face) {
      if (face_group[face] == group) {
        brush_write_face(geometry, face, faces[face], world, &world_inverse,
                         vertices, &vertex_count, indices, &index_count);
      }
    }
    VkrGeometryConfig config = {
        .vertex_size = sizeof(VkrVertex3d),
        .vertex_count = vertex_count,
        .vertices = vertices,
        .index_size = sizeof(uint32_t),
        .index_count = index_count,
        .indices = indices,
        .center = vec3_scale(vec3_add(geometry->min, geometry->max), 0.5f),
        .min_extents = geometry->min,
        .max_extents = geometry->max,
    };
    snprintf(config.name, sizeof(config.name), "brush_%u_%u_%u_%u_%u",
             (unsigned)record->entity.parts.world,
             (unsigned)record->entity.parts.index,
             (unsigned)record->entity.parts.generation, record->serial, group);
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrGeometryHandle handle = vkr_geometry_system_create(
        &assets->geometry_system, &config, true_v, &error);
    if (!handle.id) {
      ok = false_v;
      break;
    }
    bool8_t owned = false_v;
    const VkrMaterialHandle material =
        brush_material(scene, state, group_paths[group], &owned);
    submeshes[submesh_count++] = (VkrSubMeshDesc){
        .geometry = handle,
        .material = material,
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .owns_geometry = true_v,
        .owns_material = owned,
    };
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  /* The new mesh waits beside the old one for its uploads; an older waiting
     mesh is superseded. */
  brush_pending_drop(scene, state, record->entity);
  if (ok && brush_grow(scene->alloc, (void **)&state->pending,
                       &state->pending_capacity, state->pending_count + 1u,
                       sizeof(*state->pending))) {
    BrushPending *pending = &state->pending[state->pending_count++];
    pending->entity = record->entity;
    pending->submesh_count = submesh_count;
    MemCopy(pending->submeshes, submeshes, submesh_count * sizeof(*submeshes));
    return true_v;
  }
  for (uint32_t i = 0; i < submesh_count; ++i) {
    vkr_geometry_system_release(&assets->geometry_system,
                                submeshes[i].geometry);
  }
  return false_v;
}

/* Whether every geometry and material of a waiting mesh finished uploading.
   A failed upload settles too and attaches as the terrain's does. */
static bool8_t brush_pending_settled(VkrScene *scene,
                                     const BrushPending *pending) {
  struct VkrRenderAssets *assets = scene->assets;
  for (uint32_t i = 0; i < pending->submesh_count; ++i) {
    const VkrSubMeshDesc *submesh = &pending->submeshes[i];
    const VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
        &assets->geometry_system, submesh->geometry);
    if (geometry && !vkr_publication_state_settled(&geometry->publication)) {
      return false_v;
    }
    const VkrPublicationState *material =
        submesh->material.id ? vkr_material_system_publication(
                                   &assets->material_system, submesh->material)
                             : NULL;
    if (material && !vkr_publication_state_settled(material)) {
      return false_v;
    }
  }
  return true_v;
}

/* Swaps in each waiting mesh whose uploads settled. */
static void brush_pending_attach(VkrScene *scene, VkrSceneBrushes *state) {
  for (uint32_t i = 0; i < state->pending_count;) {
    BrushPending *pending = &state->pending[i];
    if (!brush_pending_settled(scene, pending)) {
      ++i;
      continue;
    }
    const VkrEntityId entity = pending->entity;
    VkrSceneError error = VKR_SCENE_ERROR_NONE;
    bool8_t ok = vkr_scene_replace_generated_mesh(
        scene, entity, pending->submeshes, pending->submesh_count, NULL, NULL,
        0u, &error);
    if (!ok) {
      vkr_scene_detach_generated_mesh(scene, entity);
      ok = vkr_scene_attach_generated_mesh(scene, entity, pending->submeshes,
                                           pending->submesh_count, &error);
    }
    /* The mesh holds its own references now, or none after a failure. */
    brush_pending_release(scene, pending);
    state->pending[i] = state->pending[--state->pending_count];
    const int32_t index = brush_find(state, entity);
    if (!ok && index >= 0) {
      BrushRecord *record = &state->records[index];
      brush_clear(scene, state, record);
      snprintf(record->status, sizeof(record->status), "the mesh failed");
    }
  }
}

/* World-space hull points of the brush, for solid and clip collision. */
static bool8_t brush_store_hull(VkrScene *scene, BrushRecord *record,
                                const VkrBrushGeometry *geometry,
                                const Mat4 *world) {
  if (!brush_grow(scene->alloc, (void **)&record->hull, &record->hull_capacity,
                  geometry->vertex_count, 3u * sizeof(float32_t))) {
    return false_v;
  }
  record->hull_count = 0u;
  for (uint32_t i = 0; i < geometry->vertex_count; ++i) {
    const Vec3 p = mat4_mul_vec3(*world, geometry->vertices[i]);
    bool8_t duplicate = false_v;
    for (uint32_t j = 0; j < record->hull_count && !duplicate; ++j) {
      const float32_t *q = record->hull + j * 3u;
      duplicate = fabsf(q[0] - p.x) < VKR_BRUSH_WELD &&
                  fabsf(q[1] - p.y) < VKR_BRUSH_WELD &&
                  fabsf(q[2] - p.z) < VKR_BRUSH_WELD;
    }
    if (!duplicate) {
      float32_t *out = record->hull + record->hull_count++ * 3u;
      out[0] = p.x;
      out[1] = p.y;
      out[2] = p.z;
    }
  }
  return true_v;
}

static void brush_rebuild(VkrScene *scene, VkrSceneBrushes *state,
                          BrushRecord *record) {
  record->dirty = false_v;
  const SceneBrushSettings *settings =
      vkr_scene_get_typed(scene, record->entity, &vkr_scene_brush_type);
  const SceneTransform *transform = vkr_entity_get_component(
      scene->world, record->entity, scene->comp_transform);
  if (!settings || !transform) {
    brush_clear(scene, state, record);
    snprintf(record->status, sizeof(record->status), "no brush or transform");
    return;
  }
  const Mat4 world = transform->world;
  VkrEntityId face_entities[VKR_BRUSH_FACE_MAX + 1u];
  const uint32_t face_count = vkr_scene_brush_faces(
      scene, record->entity, face_entities, ArrayCount(face_entities));
  const SceneBrushFace *faces[VKR_BRUSH_FACE_MAX];
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  VkrBrushError error = face_count > VKR_BRUSH_FACE_MAX
                            ? VKR_BRUSH_ERROR_FACE_COUNT
                            : VKR_BRUSH_OK;
  for (uint32_t i = 0; error == VKR_BRUSH_OK && i < face_count; ++i) {
    faces[i] = vkr_scene_get_typed(scene, face_entities[i],
                                   &vkr_scene_brush_face_type);
    planes[i] = (VkrBrushPlane){.normal = faces[i]->normal,
                                .distance = faces[i]->distance};
  }
  uint32_t failed_face = UINT32_MAX;
  if (error == VKR_BRUSH_OK) {
    error = vkr_brush_build(planes, face_count, state->geometry, &failed_face);
  }
  if (error != VKR_BRUSH_OK) {
    brush_clear(scene, state, record);
    if (failed_face != UINT32_MAX) {
      snprintf(record->status, sizeof(record->status), "%s (face %u)",
               vkr_brush_error_text(error),
               (unsigned)face_entities[failed_face].parts.index);
    } else {
      snprintf(record->status, sizeof(record->status), "%s",
               vkr_brush_error_text(error));
    }
    return;
  }
  if (!brush_build_mesh(scene, state, record, faces, &world)) {
    brush_clear(scene, state, record);
    snprintf(record->status, sizeof(record->status), "the mesh failed");
    return;
  }
  /* Collision: solid and clip join their cell; a trigger owns a sensor. */
  const uint64_t old_cell = record->cell;
  record->cell = 0u;
  if (record->trigger_body) {
    vkr_scene_physics_generated_remove(scene,
                                       brush_trigger_key(record->entity));
    record->trigger_body = false_v;
  }
  const SceneBrushRole role = settings->role;
  if (role != SCENE_BRUSH_ROLE_VISUAL &&
      brush_store_hull(scene, record, state->geometry, &world)) {
    if (role == SCENE_BRUSH_ROLE_TRIGGER) {
      const VkrPhysicsColliderDesc hull = {
          .entity_id = record->entity.u64,
          .shape = VKR_PHYSICS_CONVEX_HULL,
          .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
          .scale = {1.0f, 1.0f, 1.0f},
          .geometry = {.positions = record->hull,
                       .vertex_count = record->hull_count},
          .enabled = true_v,
      };
      const char *physics_error = NULL;
      record->trigger_body = vkr_scene_physics_generated_set(
          scene, brush_trigger_key(record->entity), record->entity, &hull, 1u,
          true_v, &physics_error);
      if (!record->trigger_body && physics_error) {
        log_warn("Scene: trigger brush collision failed: %s", physics_error);
      }
      record->hull_count = 0u;
    } else {
      const Vec3 center = mat4_mul_vec3(
          world,
          vec3_scale(vec3_add(state->geometry->min, state->geometry->max),
                     0.5f));
      record->cell = brush_cell_key(center);
    }
  }
  if (old_cell != record->cell) {
    brush_mark_cell(scene, state, old_cell);
  }
  brush_mark_cell(scene, state, record->cell);
  record->built = true_v;
  record->built_world = world;
  record->last_world = world;
  record->status[0] = '\0';
}

/* Rebuilds the collision bodies of one cell: its brush hulls in chunks. */
static void brush_rebuild_cell(VkrScene *scene, VkrSceneBrushes *state,
                               uint64_t cell) {
  int32_t cell_index = -1;
  for (uint32_t i = 0; i < state->cell_count; ++i) {
    if (state->cells[i].cell == cell) {
      cell_index = (int32_t)i;
      break;
    }
  }
  const uint32_t old_chunks =
      cell_index >= 0 ? state->cells[cell_index].chunks : 0u;
  VkrPhysicsColliderDesc colliders[BRUSH_CHUNK_HULLS];
  uint32_t collider_count = 0u;
  uint32_t chunks = 0u;
  VkrEntityId chunk_entity = VKR_ENTITY_ID_INVALID;
  for (uint32_t i = 0; i <= state->record_count; ++i) {
    const BrushRecord *record =
        i < state->record_count ? &state->records[i] : NULL;
    if (record && (record->cell != cell || record->hull_count < 4u)) {
      continue;
    }
    if (record) {
      if (!collider_count) {
        chunk_entity = record->entity;
      }
      colliders[collider_count++] = (VkrPhysicsColliderDesc){
          .entity_id = record->entity.u64,
          .shape = VKR_PHYSICS_CONVEX_HULL,
          .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
          .scale = {1.0f, 1.0f, 1.0f},
          .geometry = {.positions = record->hull,
                       .vertex_count = record->hull_count},
          .enabled = true_v,
      };
    }
    if (collider_count && (collider_count == BRUSH_CHUNK_HULLS || !record)) {
      const char *error = NULL;
      if (!vkr_scene_physics_generated_set(scene, cell | chunks, chunk_entity,
                                           colliders, collider_count, false_v,
                                           &error)) {
        log_warn("Scene: brush collision failed: %s",
                 error ? error : "unknown");
      }
      chunks++;
      collider_count = 0u;
    }
  }
  for (uint32_t chunk = chunks; chunk < old_chunks; ++chunk) {
    vkr_scene_physics_generated_remove(scene, cell | chunk);
  }
  if (cell_index < 0 && chunks) {
    if (!brush_grow(scene->alloc, (void **)&state->cells, &state->cell_capacity,
                    state->cell_count + 1u, sizeof(*state->cells))) {
      return;
    }
    cell_index = (int32_t)state->cell_count++;
    state->cells[cell_index].cell = cell;
  }
  if (cell_index >= 0) {
    state->cells[cell_index].chunks = chunks;
  }
}

static bool8_t brush_matrix_equal(const Mat4 *a, const Mat4 *b) {
  return MemCompare(a->elements, b->elements, sizeof(a->elements)) == 0;
}

void vkr_scene_brush_update(VkrScene *scene) {
  VkrSceneBrushes *state = scene ? brush_state(scene, false_v) : NULL;
  if (!state || !scene->assets) {
    return;
  }
  uint32_t budget = VKR_SCENE_BRUSH_REBUILD_BUDGET;
  for (uint32_t i = 0; i < state->record_count; ++i) {
    BrushRecord *record = &state->records[i];
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, record->entity, scene->comp_transform);
    if (record->built && transform) {
      /* A moved brush rebuilds once its transform rests, so a drag does not
         rebuild every update; until then the mesh follows its model. */
      if (!brush_matrix_equal(&transform->world, &record->last_world)) {
        record->last_world = transform->world;
        record->settle = 0u;
      } else if (record->settle < UINT8_MAX) {
        record->settle++;
      }
      if (!brush_matrix_equal(&transform->world, &record->built_world)) {
        record->dirty = true_v;
      }
    }
    if (record->dirty && record->settle >= BRUSH_SETTLE_UPDATES && budget) {
      brush_rebuild(scene, state, record);
      budget--;
    }
  }
  for (uint32_t i = 0; i < state->dirty_cell_count; ++i) {
    brush_rebuild_cell(scene, state, state->dirty_cells[i]);
  }
  state->dirty_cell_count = 0u;
  brush_pending_attach(scene, state);
}

void vkr_scene_brush_shutdown(VkrScene *scene) {
  VkrSceneBrushes *state = scene ? scene->brushes : NULL;
  if (!state) {
    return;
  }
  for (uint32_t i = 0; i < state->record_count; ++i) {
    if (state->records[i].hull) {
      vkr_allocator_free(
          scene->alloc, state->records[i].hull,
          state->records[i].hull_capacity * 3u * sizeof(float32_t), BRUSH_TAG);
    }
  }
  for (uint32_t i = 0; scene->assets && i < state->pending_count; ++i) {
    brush_pending_release(scene, &state->pending[i]);
  }
  if (state->pending) {
    vkr_allocator_free(scene->alloc, state->pending,
                       state->pending_capacity * sizeof(*state->pending),
                       BRUSH_TAG);
  }
  for (uint32_t i = 0; scene->assets && i < state->material_count; ++i) {
    if (state->materials[i].owned) {
      vkr_material_system_release(&scene->assets->material_system,
                                  state->materials[i].handle);
    }
  }
  if (state->records) {
    vkr_allocator_free(scene->alloc, state->records,
                       state->record_capacity * sizeof(*state->records),
                       BRUSH_TAG);
  }
  if (state->dirty_cells) {
    vkr_allocator_free(scene->alloc, state->dirty_cells,
                       state->dirty_cell_capacity * sizeof(*state->dirty_cells),
                       BRUSH_TAG);
  }
  if (state->cells) {
    vkr_allocator_free(scene->alloc, state->cells,
                       state->cell_capacity * sizeof(*state->cells), BRUSH_TAG);
  }
  if (state->materials) {
    vkr_allocator_free(scene->alloc, state->materials,
                       state->material_capacity * sizeof(*state->materials),
                       BRUSH_TAG);
  }
  vkr_allocator_free(scene->alloc, state->geometry, sizeof(*state->geometry),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->brushes = NULL;
}

const char *vkr_scene_brush_status(const VkrScene *scene, VkrEntityId brush) {
  const VkrSceneBrushes *state = scene ? scene->brushes : NULL;
  const int32_t index = brush_find(state, brush);
  if (index < 0) {
    return "pending";
  }
  const BrushRecord *record = &state->records[index];
  return record->built ? NULL : record->status;
}
