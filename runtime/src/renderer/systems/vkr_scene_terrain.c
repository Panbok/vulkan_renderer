#include "renderer/systems/vkr_scene_terrain.h"

#include "core/logger.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TERRAIN_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
#define TERRAIN_TILES_MAX                                                      \
  (VKR_SCENE_TERRAIN_TILES_MAX * VKR_SCENE_TERRAIN_TILES_MAX)
/* Skirts reach this many sample spacings below each tile edge, so tiles at
   different detail never show a gap between them. */
#define TERRAIN_SKIRT_SPACINGS 2.0f
/* A coarser level's error is at least this share of its cell size. */
#define TERRAIN_LOD_CELL_ERROR 0.1f

_Static_assert(VKR_HEIGHTFIELD_TILE_CELLS + 1u == VKR_GPU_TERRAIN_TILE_SIDE,
               "Shaders morph tiles of VKR_GPU_TERRAIN_TILE_SIDE samples");
_Static_assert(VKR_SCENE_TERRAIN_LOD_LEVELS <= VKR_GPU_GEOMETRY_LOD_LEVEL_MAX &&
                   (VKR_HEIGHTFIELD_TILE_CELLS >>
                    (VKR_SCENE_TERRAIN_LOD_LEVELS - 1u)) == 1u,
               "Tile levels halve 64 cells down to one");

typedef struct TerrainRecord {
  VkrEntityId entity;
  /* The component's file and layers the products were built for. */
  char source[SCENE_TERRAIN_PATH_CAPACITY];
  char material_key[VKR_MATERIAL_TERRAIN_LAYERS *
                    SCENE_TERRAIN_MATERIAL_CAPACITY];
  float32_t texture_size;
  VkrHeightfield field;
  bool8_t loaded;
  /* Samples changed since they loaded or saved. */
  bool8_t unsaved;
  bool8_t mesh_dirty;
  bool8_t collision_dirty;
  uint8_t settle;
  /* World position the collision was built at. */
  Vec3 built_position;
  bool8_t collision_built;
  /* The terrain's own layered material, released with the record. */
  VkrMaterialHandle material;
  /* One geometry per tile, its dirty bit, and the mesh serial. */
  VkrGeometryHandle tiles[TERRAIN_TILES_MAX];
  uint64_t dirty_tiles[TERRAIN_TILES_MAX / 64u];
  uint32_t serial;
  char status[160];
} TerrainRecord;

struct s_VkrSceneTerrains {
  TerrainRecord records[VKR_SCENE_TERRAIN_MAX];
  uint32_t record_count;
};

// =============================================================================
// Records
// =============================================================================

static VkrSceneTerrains *terrain_state(VkrScene *scene, bool8_t create) {
  if (!scene->terrains && create) {
    scene->terrains =
        vkr_allocator_alloc(scene->alloc, sizeof(*scene->terrains),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (scene->terrains) {
      MemZero(scene->terrains, sizeof(*scene->terrains));
    }
  }
  return scene->terrains;
}

static TerrainRecord *terrain_find(const VkrScene *scene, VkrEntityId entity) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    if (state->records[i].entity.u64 == entity.u64) {
      return &state->records[i];
    }
  }
  return NULL;
}

static uint32_t terrain_tiles(const TerrainRecord *record) {
  return record->field.cells / VKR_HEIGHTFIELD_TILE_CELLS;
}

static uint64_t terrain_collision_key(VkrEntityId entity) {
  return entity.u64 ^ 0x7465727261696e00ull;
}

/* Releases the mesh, collision, tile geometries and samples. */
static void terrain_release(VkrScene *scene, TerrainRecord *record) {
  struct VkrRenderAssets *assets = scene->assets;
  vkr_scene_detach_generated_mesh(scene, record->entity);
  vkr_scene_physics_generated_remove(scene,
                                     terrain_collision_key(record->entity));
  for (uint32_t i = 0; assets && i < TERRAIN_TILES_MAX; ++i) {
    if (record->tiles[i].id) {
      vkr_geometry_system_release(&assets->geometry_system, record->tiles[i]);
      record->tiles[i] = (VkrGeometryHandle){0};
    }
  }
  if (assets && record->material.id) {
    vkr_material_system_release(&assets->material_system, record->material);
  }
  record->material = (VkrMaterialHandle){0};
  record->material_key[0] = '\0';
  vkr_heightfield_destroy(&record->field, scene->alloc);
  record->loaded = false_v;
  record->collision_built = false_v;
}

/* The layers a terrain material is built from, as one comparable string. */
static void terrain_material_key(const SceneTerrain *terrain, char *out,
                                 uint32_t capacity) {
  snprintf(out, capacity, "%s|%s|%s|%s", terrain->layer0, terrain->layer1,
           terrain->layer2, terrain->layer3);
}

static void terrain_mark_all(TerrainRecord *record) {
  MemSet(record->dirty_tiles, 0xFF, sizeof(record->dirty_tiles));
  record->mesh_dirty = true_v;
  record->collision_dirty = true_v;
  record->settle = 0u;
}

/* Marks the tiles whose vertices or normals `rect` reaches. */
static void terrain_mark(TerrainRecord *record, VkrHeightfieldRect rect) {
  const uint32_t tiles = terrain_tiles(record);
  /* Normals read one neighbour, and edge samples belong to two tiles. */
  const uint32_t x0 = rect.x0 > 1u ? rect.x0 - 2u : 0u;
  const uint32_t z0 = rect.z0 > 1u ? rect.z0 - 2u : 0u;
  const uint32_t tx0 = x0 / VKR_HEIGHTFIELD_TILE_CELLS;
  const uint32_t tz0 = z0 / VKR_HEIGHTFIELD_TILE_CELLS;
  const uint32_t tx1 =
      Min(tiles - 1u, (rect.x1 + 1u) / VKR_HEIGHTFIELD_TILE_CELLS);
  const uint32_t tz1 =
      Min(tiles - 1u, (rect.z1 + 1u) / VKR_HEIGHTFIELD_TILE_CELLS);
  for (uint32_t tz = tz0; tz <= tz1; ++tz) {
    for (uint32_t tx = tx0; tx <= tx1; ++tx) {
      const uint32_t index = tz * tiles + tx;
      record->dirty_tiles[index / 64u] |= 1ull << (index % 64u);
    }
  }
  record->mesh_dirty = true_v;
  record->collision_dirty = true_v;
  record->unsaved = true_v;
  record->settle = 0u;
}

bool8_t vkr_scene_terrain_resolve(const VkrScene *scene, const char *relative,
                                  char *out, uint32_t capacity) {
  if (!relative[0]) {
    return false_v;
  }
  const bool8_t absolute =
      relative[0] == '/' || (relative[0] && relative[1] == ':');
  const int written = absolute || !scene->physics_asset_root[0]
                          ? snprintf(out, capacity, "%s", relative)
                          : snprintf(out, capacity, "%s/%s",
                                     scene->physics_asset_root, relative);
  return written > 0 && (uint32_t)written < capacity;
}

static void terrain_load(VkrScene *scene, TerrainRecord *record,
                         const SceneTerrain *terrain) {
  terrain_release(scene, record);
  snprintf(record->source, sizeof(record->source), "%s", terrain->heightfield);
  record->unsaved = false_v;
  char path[1100];
  if (!vkr_scene_terrain_resolve(scene, terrain->heightfield, path,
                                 sizeof(path))) {
    snprintf(record->status, sizeof(record->status),
             "The terrain names no heightfield file");
    return;
  }
  if (!vkr_heightfield_load(&record->field, path, scene->alloc, record->status,
                            sizeof(record->status))) {
    return;
  }
  record->loaded = true_v;
  record->status[0] = '\0';
  terrain_mark_all(record);
}

void vkr_scene_terrain_changed(VkrScene *scene, VkrEntityId entity) {
  const SceneTerrain *terrain =
      vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type);
  TerrainRecord *record = terrain_find(scene, entity);
  if (!terrain) {
    if (record) {
      vkr_scene_terrain_entity_destroying(scene, entity);
    }
    return;
  }
  if (!record) {
    VkrSceneTerrains *state = terrain_state(scene, true_v);
    if (!state || state->record_count == VKR_SCENE_TERRAIN_MAX) {
      log_warn("Scene: a scene holds at most %u terrains",
               VKR_SCENE_TERRAIN_MAX);
      return;
    }
    record = &state->records[state->record_count++];
    MemZero(record, sizeof(*record));
    record->entity = entity;
  }
  if (!record->loaded || strcmp(record->source, terrain->heightfield) != 0) {
    terrain_load(scene, record, terrain);
    return;
  }
  /* A new layer material or texture size rebuilds every tile. */
  char key[sizeof(record->material_key)];
  terrain_material_key(terrain, key, sizeof(key));
  if (strcmp(record->material_key, key) != 0 ||
      record->texture_size != terrain->texture_size) {
    terrain_mark_all(record);
  }
}

void vkr_scene_terrain_entity_destroying(VkrScene *scene, VkrEntityId entity) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  TerrainRecord *record = terrain_find(scene, entity);
  if (!state || !record) {
    return;
  }
  terrain_release(scene, record);
  *record = state->records[--state->record_count];
}

const VkrHeightfield *vkr_scene_terrain_field(const VkrScene *scene,
                                              VkrEntityId entity) {
  const TerrainRecord *record = terrain_find(scene, entity);
  return record && record->loaded ? &record->field : NULL;
}

const char *vkr_scene_terrain_status(const VkrScene *scene,
                                     VkrEntityId entity) {
  const TerrainRecord *record = terrain_find(scene, entity);
  if (!record) {
    return "not a terrain";
  }
  return record->loaded ? NULL : record->status;
}

bool8_t vkr_scene_terrain_to_local(const VkrScene *scene, VkrEntityId entity,
                                   Vec3 world, Vec3 *out_local) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (!transform) {
    return false_v;
  }
  *out_local = vec3_sub(world, mat4_position(transform->world));
  return true_v;
}

bool8_t vkr_scene_terrain_apply(VkrScene *scene, VkrEntityId entity,
                                const VkrHeightfieldOp *op,
                                VkrAllocator *scratch,
                                VkrHeightfieldRect *out_touched) {
  TerrainRecord *record = terrain_find(scene, entity);
  if (!record || !record->loaded ||
      !vkr_heightfield_op_apply(&record->field, op, scratch, out_touched)) {
    return false_v;
  }
  terrain_mark(record, *out_touched);
  return true_v;
}

bool8_t vkr_scene_terrain_write(VkrScene *scene, VkrEntityId entity,
                                VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights) {
  TerrainRecord *record = terrain_find(scene, entity);
  const uint32_t samples =
      record && record->loaded ? vkr_heightfield_samples(&record->field) : 0u;
  if (!samples || rect.x1 >= samples || rect.z1 >= samples ||
      rect.x0 > rect.x1 || rect.z0 > rect.z1) {
    return false_v;
  }
  vkr_heightfield_write_rect(&record->field, rect, heights, weights);
  terrain_mark(record, rect);
  return true_v;
}

bool8_t vkr_scene_terrain_save(const VkrScene *scene, char *error,
                               uint32_t capacity) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    TerrainRecord *record = &state->records[i];
    char path[1100];
    if (!record->loaded || !record->unsaved) {
      continue;
    }
    if (!vkr_scene_terrain_resolve(scene, record->source, path, sizeof(path)) ||
        !vkr_heightfield_save(&record->field, path, error, capacity)) {
      return false_v;
    }
    record->unsaved = false_v;
  }
  return true_v;
}

// =============================================================================
// Products
// =============================================================================

/* Normal of sample (x, z) from its neighbours' heights. */
static Vec3 terrain_normal(const VkrHeightfield *field, uint32_t x,
                           uint32_t z) {
  const uint32_t last = field->cells;
  const float32_t left = vkr_heightfield_at(field, x ? x - 1u : x, z);
  const float32_t right = vkr_heightfield_at(field, Min(last, x + 1u), z);
  const float32_t down = vkr_heightfield_at(field, x, z ? z - 1u : z);
  const float32_t up = vkr_heightfield_at(field, x, Min(last, z + 1u));
  const float32_t dx =
      (right - left) /
      ((float32_t)(Min(last, x + 1u) - (x ? x - 1u : x)) * field->spacing);
  const float32_t dz =
      (up - down) /
      ((float32_t)(Min(last, z + 1u) - (z ? z - 1u : z)) * field->spacing);
  return vec3_normalize(vec3_new(-dx, 1.0f, -dz));
}

/* Layer weights of sample (x, z), each 0 to 1. */
static Vec4 terrain_weights(const VkrHeightfield *field, uint32_t x,
                            uint32_t z) {
  const uint32_t w =
      field->weights[(size_t)z * vkr_heightfield_samples(field) + x];
  return vec4_new(
      (float32_t)(w & 0xFFu) / 255.0f, (float32_t)((w >> 8u) & 0xFFu) / 255.0f,
      (float32_t)((w >> 16u) & 0xFFu) / 255.0f, (float32_t)(w >> 24u) / 255.0f);
}

/* Grid position of sample `i` along tile edge `edge` (-Z, +X, +Z, -X). */
static void terrain_edge_sample(uint32_t edge, uint32_t i, uint32_t *out_x,
                                uint32_t *out_z) {
  const uint32_t last = VKR_HEIGHTFIELD_TILE_CELLS;
  *out_x = edge == 0u ? i : edge == 1u ? last : edge == 2u ? last - i : 0u;
  *out_z = edge == 0u ? 0u : edge == 1u ? i : edge == 2u ? last : last - i;
}

/* Largest height difference between a tile's samples and the surface of
   the level drawing every `stride`-th grid line. */
static float32_t terrain_level_error(const VkrVertex3d *vertices, uint32_t side,
                                     uint32_t stride) {
  float32_t error = 0.0f;
  for (uint32_t z = 0; z < side; ++z) {
    for (uint32_t x = 0; x < side; ++x) {
      const uint32_t x0 = Min(x / stride * stride, side - 1u - stride);
      const uint32_t z0 = Min(z / stride * stride, side - 1u - stride);
      const float32_t u = (float32_t)(x - x0) / (float32_t)stride;
      const float32_t v = (float32_t)(z - z0) / (float32_t)stride;
      const float32_t ha = vertices[z0 * side + x0].position.y;
      const float32_t hb = vertices[z0 * side + x0 + stride].position.y;
      const float32_t hc = vertices[(z0 + stride) * side + x0].position.y;
      const float32_t hd =
          vertices[(z0 + stride) * side + x0 + stride].position.y;
      /* The cell splits along its +X to +Z diagonal. */
      const float32_t surface =
          u + v <= 1.0f ? ha + u * (hb - ha) + v * (hc - ha)
                        : hd + (1.0f - u) * (hc - hd) + (1.0f - v) * (hb - hd);
      error = Max(error, fabsf(vertices[z * side + x].position.y - surface));
    }
  }
  return error;
}

uint32_t vkr_scene_terrain_tile_index_count(void) {
  /* A level of n cells a side has n squared cells and 4n skirt quads, six
     indices each. */
  uint32_t index_count = 0u;
  for (uint32_t level = 0; level < VKR_SCENE_TERRAIN_LOD_LEVELS; ++level) {
    const uint32_t cells = VKR_HEIGHTFIELD_TILE_CELLS >> level;
    index_count += (cells * cells + 4u * cells) * 6u;
  }
  return index_count;
}

uint32_t vkr_scene_terrain_tile_indices(const VkrVertex3d *vertices,
                                        float32_t spacing, uint32_t *indices,
                                        uint32_t capacity,
                                        VkrGpuGeometryLodRow *out_lod) {
  const uint32_t side = VKR_HEIGHTFIELD_TILE_CELLS + 1u;
  const uint32_t grid = side * side;
  if (capacity < vkr_scene_terrain_tile_index_count()) {
    return 0u;
  }
  /* Level L draws every 2^L-th grid line with level 0's diagonal (ADR-084);
     its error is its surface's largest height difference from the samples,
     at least a share of its cell size so that painted weights thin out
     with distance too. */
  VkrGpuGeometryLodRow lod = {.level_count = VKR_SCENE_TERRAIN_LOD_LEVELS,
                              .flags = VKR_GPU_GEOMETRY_LOD_TERRAIN_GRID};
  uint32_t count = 0u;
  for (uint32_t level = 0; level < VKR_SCENE_TERRAIN_LOD_LEVELS; ++level) {
    const uint32_t stride = 1u << level;
    const uint32_t first = count;
    for (uint32_t z = 0; z < VKR_HEIGHTFIELD_TILE_CELLS; z += stride) {
      for (uint32_t x = 0; x < VKR_HEIGHTFIELD_TILE_CELLS; x += stride) {
        const uint32_t a = z * side + x;
        const uint32_t b = a + stride;
        const uint32_t c = a + stride * side;
        const uint32_t d = c + stride;
        /* Counter-clockwise seen from above. */
        indices[count++] = a;
        indices[count++] = c;
        indices[count++] = b;
        indices[count++] = b;
        indices[count++] = c;
        indices[count++] = d;
      }
    }
    for (uint32_t edge = 0; edge < 4u; ++edge) {
      for (uint32_t i = 0; i < VKR_HEIGHTFIELD_TILE_CELLS; i += stride) {
        uint32_t xa = 0u;
        uint32_t za = 0u;
        uint32_t xb = 0u;
        uint32_t zb = 0u;
        terrain_edge_sample(edge, i, &xa, &za);
        terrain_edge_sample(edge, i + stride, &xb, &zb);
        const uint32_t top_a = za * side + xa;
        const uint32_t top_b = zb * side + xb;
        const uint32_t low_a = grid + edge * side + i;
        const uint32_t low_b = grid + edge * side + i + stride;
        indices[count++] = top_a;
        indices[count++] = top_b;
        indices[count++] = low_a;
        indices[count++] = low_a;
        indices[count++] = top_b;
        indices[count++] = low_b;
      }
    }
    lod.levels[level] = (VkrGpuGeometryLodLevel){
        .first_index = first,
        .index_count = count - first,
        .error = Max(terrain_level_error(vertices, side, stride),
                     TERRAIN_LOD_CELL_ERROR * spacing *
                         (level ? (float32_t)stride : 0.0f)),
    };
    if (level > 0u) {
      lod.levels[level].error =
          Max(lod.levels[level].error, lod.levels[level - 1u].error);
    }
  }
  *out_lod = lod;
  return count;
}

/* One tile's geometry: its 65 x 65 samples and a skirt down from each edge,
   in the terrain's local space. */
static VkrGeometryHandle terrain_tile_geometry(VkrScene *scene,
                                               TerrainRecord *record,
                                               uint32_t tx, uint32_t tz) {
  struct VkrRenderAssets *assets = scene->assets;
  const VkrHeightfield *field = &record->field;
  const uint32_t side = VKR_HEIGHTFIELD_TILE_CELLS + 1u;
  const uint32_t grid = side * side;
  const uint32_t vertex_count = grid + 4u * side;
  const uint32_t index_count = vkr_scene_terrain_tile_index_count();
  VkrVertex3d *vertices =
      vkr_allocator_alloc(&assets->scratch_allocator,
                          vertex_count * sizeof(*vertices), TERRAIN_TAG);
  uint32_t *indices = vkr_allocator_alloc(
      &assets->scratch_allocator, index_count * sizeof(*indices), TERRAIN_TAG);
  if (!vertices || !indices) {
    return (VkrGeometryHandle){0};
  }
  const float32_t half = vkr_heightfield_half_size(field);
  const float32_t uv_scale = 1.0f / Max(0.01f, record->texture_size);
  const float32_t skirt = TERRAIN_SKIRT_SPACINGS * field->spacing;
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t z = 0; z < side; ++z) {
    for (uint32_t x = 0; x < side; ++x) {
      const uint32_t sx = tx * VKR_HEIGHTFIELD_TILE_CELLS + x;
      const uint32_t sz = tz * VKR_HEIGHTFIELD_TILE_CELLS + z;
      const Vec3 p = vec3_new((float32_t)sx * field->spacing - half,
                              vkr_heightfield_at(field, sx, sz),
                              (float32_t)sz * field->spacing - half);
      const Vec3 n = terrain_normal(field, sx, sz);
      vertices[z * side + x] = (VkrVertex3d){
          .position = {p.x, p.y, p.z},
          .normal = {n.x, n.y, n.z},
          .texcoord = vec2_new(p.x * uv_scale, p.z * uv_scale),
          /* The terrain material reads the four layer weights here. */
          .colour = terrain_weights(field, sx, sz),
          .tangent = vec4_new(1.0f, 0.0f, 0.0f, 1.0f),
      };
      lo = vec3_new(Min(lo.x, p.x), Min(lo.y, p.y), Min(lo.z, p.z));
      hi = vec3_new(Max(hi.x, p.x), Max(hi.y, p.y), Max(hi.z, p.z));
    }
  }
  /* Skirts: each edge's samples again, lowered, facing out. Edges run
     -Z, +X, +Z, -X; a skirt vertex keeps its edge sample's normal. */
  for (uint32_t edge = 0; edge < 4u; ++edge) {
    for (uint32_t i = 0; i < side; ++i) {
      uint32_t x = 0u;
      uint32_t z = 0u;
      terrain_edge_sample(edge, i, &x, &z);
      VkrVertex3d v = vertices[z * side + x];
      v.position.y -= skirt;
      vertices[grid + edge * side + i] = v;
      lo.y = Min(lo.y, v.position.y);
    }
  }
  VkrGpuGeometryLodRow lod = {0};
  const uint32_t count = vkr_scene_terrain_tile_indices(
      vertices, field->spacing, indices, index_count, &lod);
  VkrGeometryConfig config = {
      .vertex_size = sizeof(VkrVertex3d),
      .vertex_count = vertex_count,
      .vertices = vertices,
      .index_size = sizeof(uint32_t),
      .index_count = count,
      .indices = indices,
      .range_index_count = lod.levels[0].index_count,
      .lod = lod,
      .center = vec3_scale(vec3_add(lo, hi), 0.5f),
      .min_extents = lo,
      .max_extents = hi,
  };
  snprintf(config.name, sizeof(config.name), "terrain_%u_%u_%u_%u_%u_%u",
           (unsigned)record->entity.parts.world,
           (unsigned)record->entity.parts.index,
           (unsigned)record->entity.parts.generation, record->serial, tx, tz);
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  return vkr_geometry_system_create(&assets->geometry_system, &config, true_v,
                                    &error);
}

/* A new material of the terrain's own blending its four layers; layer 0
   defaults to the dev grid and an unnamed later layer is plain white. Its
   textures stream in; until then it draws white. */
static VkrMaterialHandle terrain_material_create(VkrScene *scene,
                                                 const TerrainRecord *record,
                                                 const SceneTerrain *terrain) {
  struct VkrRenderAssets *assets = scene->assets;
  static uint32_t serial = 0u;
  char name[64];
  snprintf(name, sizeof(name), "terrain/%016llx/%u",
           (unsigned long long)record->entity.u64, ++serial);
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrMaterialHandle handle = vkr_material_system_create_colored(
      &assets->material_system, name, vec4_new(1.0f, 1.0f, 1.0f, 1.0f), &error);
  if (!handle.id) {
    log_warn("Scene: the terrain material could not be created (%d)",
             (int)error);
    return handle;
  }
  const char *layers[VKR_MATERIAL_TERRAIN_LAYERS] = {
      terrain->layer0[0] ? terrain->layer0 : VKR_SCENE_BRUSH_DEFAULT_MATERIAL,
      terrain->layer1, terrain->layer2, terrain->layer3};
  String8 paths[VKR_MATERIAL_TERRAIN_LAYERS];
  for (uint32_t i = 0; i < VKR_MATERIAL_TERRAIN_LAYERS; ++i) {
    paths[i] =
        string8_create_from_cstr((const uint8_t *)layers[i], strlen(layers[i]));
  }
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  if (!vkr_material_loader_replace_terrain(&assets->material_system, handle,
                                           paths, &assets->scratch_allocator,
                                           &error)) {
    log_warn("Scene: the terrain layers did not load (%d); drawing it white",
             (int)error);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

/* Rebuilds dirty tiles and re-attaches the mesh with every tile. */
static bool8_t terrain_build_mesh(VkrScene *scene, TerrainRecord *record) {
  struct VkrRenderAssets *assets = scene->assets;
  const SceneTerrain *terrain =
      vkr_scene_get_typed(scene, record->entity, &vkr_scene_terrain_type);
  if (!terrain) {
    return false_v;
  }
  char key[sizeof(record->material_key)];
  terrain_material_key(terrain, key, sizeof(key));
  if (strcmp(record->material_key, key) != 0 || !record->material.id) {
    if (record->material.id) {
      vkr_material_system_release(&assets->material_system, record->material);
    }
    snprintf(record->material_key, sizeof(record->material_key), "%s", key);
    record->material = terrain_material_create(scene, record, terrain);
  }
  record->texture_size = terrain->texture_size;
  record->serial++;
  const uint32_t tiles = terrain_tiles(record);
  bool8_t ok = true_v;
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  for (uint32_t tz = 0; ok && tz < tiles; ++tz) {
    for (uint32_t tx = 0; ok && tx < tiles; ++tx) {
      const uint32_t index = tz * tiles + tx;
      if (!(record->dirty_tiles[index / 64u] & (1ull << (index % 64u))) &&
          record->tiles[index].id) {
        continue;
      }
      VkrAllocatorScope tile_scope =
          vkr_allocator_begin_scope(&assets->scratch_allocator);
      const VkrGeometryHandle handle =
          terrain_tile_geometry(scene, record, tx, tz);
      vkr_allocator_end_scope(&tile_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!handle.id) {
        ok = false_v;
        break;
      }
      if (record->tiles[index].id) {
        vkr_geometry_system_release(&assets->geometry_system,
                                    record->tiles[index]);
      }
      record->tiles[index] = handle;
    }
  }
  VkrSubMeshDesc *submeshes =
      ok ? vkr_allocator_alloc(&assets->scratch_allocator,
                               tiles * tiles * sizeof(*submeshes), TERRAIN_TAG)
         : NULL;
  ok = ok && submeshes;
  for (uint32_t i = 0; ok && i < tiles * tiles; ++i) {
    /* The mesh acquires its own references; the record keeps its. */
    submeshes[i] = (VkrSubMeshDesc){
        .geometry = record->tiles[i],
        .material = record->material,
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .owns_geometry = true_v,
        .owns_material = record->material.id != 0u,
    };
  }
  vkr_scene_detach_generated_mesh(scene, record->entity);
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (ok) {
    ok = vkr_scene_attach_generated_mesh(scene, record->entity, submeshes,
                                         tiles * tiles, &scene_error);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  MemZero(record->dirty_tiles, sizeof(record->dirty_tiles));
  record->mesh_dirty = false_v;
  return ok;
}

/* One height field body over the whole terrain, holes padding the grid to a
   multiple of four samples. */
static void terrain_build_collision(VkrScene *scene, TerrainRecord *record,
                                    Vec3 position) {
  const VkrHeightfield *field = &record->field;
  const uint32_t samples = vkr_heightfield_samples(field);
  const uint32_t padded = (samples + 3u) & ~3u;
  const uint64_t bytes = sizeof(float32_t) * (uint64_t)padded * padded;
  float32_t *heights = vkr_allocator_alloc(scene->alloc, bytes, TERRAIN_TAG);
  if (!heights) {
    return;
  }
  for (uint32_t z = 0; z < padded; ++z) {
    for (uint32_t x = 0; x < padded; ++x) {
      heights[(size_t)z * padded + x] = x < samples && z < samples
                                            ? vkr_heightfield_at(field, x, z)
                                            : VKR_PHYSICS_HEIGHT_HOLE;
    }
  }
  const float32_t half = vkr_heightfield_half_size(field);
  const VkrPhysicsColliderDesc collider = {
      .entity_id = record->entity.u64,
      .shape = VKR_PHYSICS_HEIGHT_FIELD,
      .position = {position.x - half, position.y, position.z - half},
      .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
      .scale = {1.0f, 1.0f, 1.0f},
      .geometry = {.positions = heights,
                   .vertex_count = padded * padded,
                   .height_samples = padded,
                   .height_spacing = field->spacing},
      .enabled = true_v,
  };
  const char *error = NULL;
  record->collision_built = vkr_scene_physics_generated_set(
      scene, terrain_collision_key(record->entity), record->entity, &collider,
      1u, false_v, &error);
  if (!record->collision_built && error) {
    log_warn("Scene: terrain collision failed: %s", error);
  }
  vkr_allocator_free(scene->alloc, heights, bytes, TERRAIN_TAG);
  record->built_position = position;
  record->collision_dirty = false_v;
}

void vkr_scene_terrain_update(VkrScene *scene) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  if (!state || !scene->assets) {
    return;
  }
  for (uint32_t i = 0; i < state->record_count; ++i) {
    TerrainRecord *record = &state->records[i];
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, record->entity, scene->comp_transform);
    if (!record->loaded || !transform) {
      continue;
    }
    if (record->mesh_dirty && !terrain_build_mesh(scene, record)) {
      snprintf(record->status, sizeof(record->status),
               "The terrain mesh failed");
    }
    const Vec3 position = mat4_position(transform->world);
    if (record->collision_built &&
        MemCompare(&position, &record->built_position, sizeof(Vec3)) != 0) {
      record->collision_dirty = true_v;
      record->settle = 0u;
    }
    if (record->settle < UINT8_MAX) {
      record->settle++;
    }
    if (record->collision_dirty &&
        record->settle >= VKR_SCENE_TERRAIN_COLLISION_SETTLE) {
      terrain_build_collision(scene, record, position);
    }
  }
}

void vkr_scene_terrain_shutdown(VkrScene *scene) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  if (!state) {
    return;
  }
  for (uint32_t i = 0; i < state->record_count; ++i) {
    terrain_release(scene, &state->records[i]);
  }
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->terrains = NULL;
}
