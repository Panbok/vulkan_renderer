#include "renderer/systems/vkr_scene_terrain.h"

#include "core/logger.h"
#include "core/vkr_job_system.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
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
#include <stdlib.h>
#include <string.h>

#define TERRAIN_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
#define TERRAIN_TILES_MAX                                                      \
  (VKR_SCENE_TERRAIN_TILES_MAX * VKR_SCENE_TERRAIN_TILES_MAX)
/* Skirts reach this many sample spacings below each tile edge, so tiles at
   different detail never show a gap between them. */
#define TERRAIN_SKIRT_SPACINGS 2.0f
/* A coarser level's error is at least this share of its cell size. */
#define TERRAIN_LOD_CELL_ERROR 0.1f
/* A streamed terrain's distant ground draws overview tiles: 65 x 65
   overview samples, each TERRAIN_FINE_PER_OVERVIEW fine tiles a side. */
#define TERRAIN_OVERVIEW_SCALE ((float32_t)VKR_HEIGHTFIELD_OVERVIEW_STRIDE)
#define TERRAIN_FINE_PER_OVERVIEW VKR_HEIGHTFIELD_OVERVIEW_STRIDE
#define TERRAIN_CELLS_PER_FINE                                                 \
  (VKR_HEIGHTFIELD_TILE_CELLS / VKR_HEIGHTFIELD_OVERVIEW_STRIDE)
#define TERRAIN_OVERVIEW_SIDE_MAX                                              \
  (VKR_HEIGHTFIELD_CELLS_MAX / VKR_HEIGHTFIELD_STREAMED_CELLS)
#define TERRAIN_OVERVIEW_MAX                                                   \
  (TERRAIN_OVERVIEW_SIDE_MAX * TERRAIN_OVERVIEW_SIDE_MAX)
/* Words of one overview tile's hole bits, a bit per fine tile. */
#define TERRAIN_HOLE_WORDS                                                     \
  (TERRAIN_FINE_PER_OVERVIEW * TERRAIN_FINE_PER_OVERVIEW / 64u)
/* Levels an overview tile with holes keeps: those whose cells each lie in
   one fine tile, so a hole's edge is exact at every level kept. */
#define TERRAIN_HOLED_LEVELS 3u
/* Fine tiles a side the body of several far-apart sources may span; past
   it the body covers the first source alone. */
#define TERRAIN_BODY_TILES_MAX 16u
/* The nearest missing tiles one streaming step considers; the frame budget
   builds far fewer. */
#define TERRAIN_STREAM_CANDIDATES 64u
/* Boxes of what changed between mesh attachments; past it the whole terrain
   counts as changed. */
#define TERRAIN_CHANGE_MAX 64u
/* Tiles one batch prepares on the job workers; a terrain has one batch at
   a time, which bounds the geometry one update uploads. */
#define TERRAIN_BATCH_MAX 8u
/* Updates a batch may take on the workers before the main thread finishes
   it, so busy workers delay tiles by at most this many frames. */
#define TERRAIN_BATCH_WAIT_MAX 4u
/* Grid samples a side a tile's copied samples span: its 65 and the
   neighbour on each side its edge normals read. */
#define TERRAIN_SNAPSHOT_SIDE (VKR_HEIGHTFIELD_TILE_CELLS + 3u)

_Static_assert(VKR_HEIGHTFIELD_TILE_CELLS + 1u == VKR_GPU_TERRAIN_TILE_SIDE,
               "Shaders morph tiles of VKR_GPU_TERRAIN_TILE_SIDE samples");
_Static_assert(VKR_SCENE_TERRAIN_LOD_LEVELS <= VKR_GPU_GEOMETRY_LOD_LEVEL_MAX &&
                   (VKR_HEIGHTFIELD_TILE_CELLS >>
                    (VKR_SCENE_TERRAIN_LOD_LEVELS - 1u)) == 1u,
               "Tile levels halve 64 cells down to one");
_Static_assert(TERRAIN_OVERVIEW_MAX <= 64u,
               "One bit of overview_dirty per overview tile");
_Static_assert((1u << (TERRAIN_HOLED_LEVELS - 1u)) == TERRAIN_CELLS_PER_FINE,
               "The coarsest holed level's cell is one fine tile");

/* What a terrain's batch builds. */
typedef enum TerrainBatchKind {
  /* Fine tiles streaming in. */
  TERRAIN_BATCH_LOADS,
  /* Changed fine tiles. */
  TERRAIN_BATCH_TILES,
  TERRAIN_BATCH_OVERVIEW,
} TerrainBatchKind;

/* Up to TERRAIN_BATCH_MAX tiles whose geometry the job workers prepare from
   copied samples while the main thread goes on; it publishes them once the
   workers finish. `works` and their buffers share one allocation, made with
   the first batch and kept until the record is released. */
typedef struct TerrainBatch {
  struct TerrainTileWork *works;
  uint64_t bytes;
  VkrJobFor *loop;
  uint32_t count;
  TerrainBatchKind kind;
  /* Updates since it began. */
  uint32_t age;
  bool8_t active;
} TerrainBatch;

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
  /* Changed tiles are being rebuilt, a few each update within the frame
     budget; the mesh is re-attached once all are and their geometry has
     uploaded. */
  bool8_t building;
  bool8_t attach_pending;
  /* Local-space boxes of the ground whose drawing changed since the mesh
     last attached, so retained shadows elsewhere stay valid; `change_all`
     once they do not fit. */
  Vec3 change_min[TERRAIN_CHANGE_MAX];
  Vec3 change_max[TERRAIN_CHANGE_MAX];
  uint32_t change_count;
  bool8_t change_all;
  bool8_t collision_dirty;
  uint8_t settle;
  /* World position the collision was built at. */
  Vec3 built_position;
  bool8_t collision_built;
  /* The terrain's own layered material, released with the record. */
  VkrMaterialHandle material;
  /* One geometry per tile, its dirty bit, and the mesh serial. A streamed
     terrain builds only the tiles near streaming sources. */
  VkrGeometryHandle tiles[TERRAIN_TILES_MAX];
  uint64_t dirty_tiles[TERRAIN_TILES_MAX / 64u];
  /* A streamed terrain (ADR-086): its window radius, overview tiles with
     their hole seams and dirty bits, and the fine tiles near the sources
     its one body covers, inclusive, whose samples may have changed. */
  bool8_t streamed;
  float32_t stream_radius;
  VkrGeometryHandle overview[TERRAIN_OVERVIEW_MAX];
  VkrGeometryHandle seams[TERRAIN_OVERVIEW_MAX];
  uint64_t overview_dirty;
  VkrHeightfieldRect body_tiles;
  bool8_t body_built;
  bool8_t body_edited;
  /* Physics refused the body; it waits until the samples change. */
  bool8_t collision_rejected;
  /* The tiles building on the job workers, published once they finish. */
  TerrainBatch batch;
  /* The body's next shape building on a physics worker, from `body_heights`
     (`body_side` samples a side whose first sits at local `body_corner`
     metres) at `built_position`; the current body stays until it
     finishes. */
  VkrPhysicsShapeBuild *body_build;
  float32_t *body_heights;
  uint32_t body_side;
  Vec2 body_corner;
  uint32_t serial;
  /* Times the samples or products were marked changed. */
  uint64_t marks;
  char status[160];
} TerrainRecord;

struct s_VkrSceneTerrains {
  TerrainRecord records[VKR_SCENE_TERRAIN_MAX];
  uint32_t record_count;
  /* Terrains removed so far; with each record's marks it is the revision. */
  uint64_t removed;
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

/* Overview tiles a side; zero for a resident terrain. */
static uint32_t terrain_overview_tiles(const TerrainRecord *record) {
  return record->streamed ? record->field.cells / VKR_HEIGHTFIELD_STREAMED_CELLS
                          : 0u;
}

/* The overview tile holding fine tile `index`. */
static uint32_t terrain_overview_of(const TerrainRecord *record,
                                    uint32_t index) {
  const uint32_t tiles = terrain_tiles(record);
  return index / tiles / TERRAIN_FINE_PER_OVERVIEW *
             terrain_overview_tiles(record) +
         index % tiles / TERRAIN_FINE_PER_OVERVIEW;
}

static bool8_t terrain_bit(const uint64_t *bits, uint32_t index) {
  return (bits[index / 64u] >> (index % 64u)) & 1u;
}

static void terrain_bit_set(uint64_t *bits, uint32_t index, bool8_t on) {
  if (on) {
    bits[index / 64u] |= 1ull << (index % 64u);
  } else {
    bits[index / 64u] &= ~(1ull << (index % 64u));
  }
}

static float32_t terrain_skirt(const TerrainRecord *record);
static void terrain_batch_release(VkrScene *scene, TerrainRecord *record);

/* Records that the ground over samples [x0, x1] x [z0, z1], clamped to the
   terrain, draws differently; a box sharing a full edge with an earlier one
   joins it. */
static void terrain_change(TerrainRecord *record, int64_t x0, int64_t z0,
                           int64_t x1, int64_t z1) {
  if (record->change_all) {
    return;
  }
  const VkrHeightfield *field = &record->field;
  const float32_t half = vkr_heightfield_half_size(field);
  const int64_t last = field->cells;
  const Vec3 min =
      vec3_new((float32_t)Max(x0, (int64_t)0) * field->spacing - half,
               field->height_min - terrain_skirt(record),
               (float32_t)Max(z0, (int64_t)0) * field->spacing - half);
  const Vec3 max = vec3_new((float32_t)Min(x1, last) * field->spacing - half,
                            field->height_max,
                            (float32_t)Min(z1, last) * field->spacing - half);
  for (uint32_t i = 0; i < record->change_count; ++i) {
    Vec3 *a = &record->change_min[i];
    Vec3 *b = &record->change_max[i];
    const bool8_t rows =
        a->z == min.z && b->z == max.z &&
        (b->x == min.x || a->x == max.x || (a->x <= min.x && b->x >= max.x));
    const bool8_t columns =
        a->x == min.x && b->x == max.x &&
        (b->z == min.z || a->z == max.z || (a->z <= min.z && b->z >= max.z));
    if (rows || columns) {
      *a = vec3_new(Min(a->x, min.x), a->y, Min(a->z, min.z));
      *b = vec3_new(Max(b->x, max.x), b->y, Max(b->z, max.z));
      return;
    }
  }
  if (record->change_count == TERRAIN_CHANGE_MAX) {
    record->change_all = true_v;
    return;
  }
  record->change_min[record->change_count] = min;
  record->change_max[record->change_count] = max;
  record->change_count++;
}

/* Fine tile (tx, tz)'s ground, whose overview hole and seams follow it. */
static void terrain_change_tile(TerrainRecord *record, uint32_t tx,
                                uint32_t tz) {
  const int64_t size = VKR_HEIGHTFIELD_TILE_CELLS;
  terrain_change(record, (int64_t)tx * size, (int64_t)tz * size,
                 ((int64_t)tx + 1) * size, ((int64_t)tz + 1) * size);
}

static uint64_t terrain_collision_key(VkrEntityId entity) {
  return entity.u64 ^ 0x7465727261696e00ull;
}

static void terrain_geometry_release(VkrScene *scene,
                                     VkrGeometryHandle *handle) {
  if (handle->id && scene->assets) {
    vkr_geometry_system_release(&scene->assets->geometry_system, *handle);
  }
  *handle = (VkrGeometryHandle){0};
}

/* Drops the body build in flight, if any. */
static void terrain_body_cancel(VkrScene *scene, TerrainRecord *record) {
  vkr_physics_shape_build_release(record->body_build);
  record->body_build = NULL;
  if (record->body_heights) {
    const uint64_t side = record->body_side;
    vkr_allocator_free(scene->alloc, record->body_heights,
                       sizeof(float32_t) * side * side, TERRAIN_TAG);
  }
  record->body_heights = NULL;
}

/* Releases the mesh, collision, tile geometries and samples. */
static void terrain_release(VkrScene *scene, TerrainRecord *record) {
  struct VkrRenderAssets *assets = scene->assets;
  terrain_body_cancel(scene, record);
  terrain_batch_release(scene, record);
  vkr_scene_detach_generated_mesh(scene, record->entity);
  vkr_scene_physics_generated_remove(scene,
                                     terrain_collision_key(record->entity));
  for (uint32_t i = 0; i < TERRAIN_TILES_MAX; ++i) {
    terrain_geometry_release(scene, &record->tiles[i]);
  }
  for (uint32_t i = 0; i < TERRAIN_OVERVIEW_MAX; ++i) {
    terrain_geometry_release(scene, &record->overview[i]);
    terrain_geometry_release(scene, &record->seams[i]);
  }
  record->body_built = false_v;
  record->body_edited = false_v;
  record->collision_rejected = false_v;
  record->overview_dirty = 0u;
  record->building = false_v;
  record->attach_pending = false_v;
  record->change_count = 0u;
  record->change_all = false_v;
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
  record->marks++;
  record->change_all = true_v;
  MemSet(record->dirty_tiles, 0xFF, sizeof(record->dirty_tiles));
  record->body_edited = true_v;
  record->collision_rejected = false_v;
  record->overview_dirty = UINT64_MAX;
  record->mesh_dirty = true_v;
  record->collision_dirty = true_v;
  record->settle = 0u;
}

/* Marks the tiles whose vertices or normals `rect` reaches. */
static void terrain_mark(TerrainRecord *record, VkrHeightfieldRect rect) {
  record->marks++;
  /* Overview triangles reach one overview stride past a changed sample. */
  const int64_t reach = record->streamed ? VKR_HEIGHTFIELD_OVERVIEW_STRIDE : 2;
  terrain_change(record, (int64_t)rect.x0 - reach, (int64_t)rect.z0 - reach,
                 (int64_t)rect.x1 + reach, (int64_t)rect.z1 + reach);
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
      terrain_bit_set(record->dirty_tiles, index, true_v);
      if (record->streamed) {
        record->overview_dirty |= 1ull << terrain_overview_of(record, index);
      }
    }
  }
  record->body_edited =
      record->body_edited ||
      ((record->body_built || record->body_build) &&
       tx0 <= record->body_tiles.x1 && tx1 >= record->body_tiles.x0 &&
       tz0 <= record->body_tiles.z1 && tz1 >= record->body_tiles.z0);
  record->mesh_dirty = true_v;
  record->collision_dirty = true_v;
  record->collision_rejected = false_v;
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

/* New terrains whose files the first save writes, by absolute path. Undo
   and redo of a creation find the entry again until a save writes the
   file; entries of discarded scenes stay until the process ends. */
#define TERRAIN_STAGED_MAX 32u

typedef struct TerrainStaged {
  char path[1100];
  uint32_t cells;
  float32_t spacing;
  float32_t height_min;
  float32_t height_max;
  float32_t height;
} TerrainStaged;

static TerrainStaged s_terrain_staged[TERRAIN_STAGED_MAX];
static uint32_t s_terrain_staged_count;

static TerrainStaged *terrain_staged_find(const char *path) {
  for (uint32_t i = 0; i < s_terrain_staged_count; ++i) {
    if (strcmp(s_terrain_staged[i].path, path) == 0) {
      return &s_terrain_staged[i];
    }
  }
  return NULL;
}

bool8_t vkr_scene_terrain_stage(const char *path, uint32_t cells,
                                float32_t spacing, float32_t height_min,
                                float32_t height_max, float32_t height) {
  if (!path || strlen(path) >= sizeof(s_terrain_staged[0].path) ||
      cells > VKR_HEIGHTFIELD_RESIDENT_CELLS ||
      s_terrain_staged_count == TERRAIN_STAGED_MAX) {
    return false_v;
  }
  TerrainStaged *staged = &s_terrain_staged[s_terrain_staged_count++];
  *staged = (TerrainStaged){.cells = cells,
                            .spacing = spacing,
                            .height_min = height_min,
                            .height_max = height_max,
                            .height = height};
  snprintf(staged->path, sizeof(staged->path), "%s", path);
  return true_v;
}

static void terrain_unstage(const char *path) {
  TerrainStaged *staged = terrain_staged_find(path);
  if (staged) {
    *staged = s_terrain_staged[--s_terrain_staged_count];
  }
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
  /* A staged terrain starts in memory and is unsaved until its scene
     saves. */
  const TerrainStaged *staged = terrain_staged_find(path);
  if (staged) {
    if (!vkr_heightfield_create(&record->field, staged->cells, staged->spacing,
                                staged->height_min, staged->height_max,
                                staged->height, scene->alloc)) {
      snprintf(record->status, sizeof(record->status),
               "The new terrain could not be created");
      return;
    }
    record->unsaved = true_v;
  } else if (!vkr_heightfield_load(&record->field, path, scene->alloc,
                                   record->status, sizeof(record->status))) {
    return;
  }
  record->loaded = true_v;
  record->streamed = vkr_heightfield_streamed(&record->field);
  record->stream_radius = terrain->stream_radius;
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
  record->stream_radius = terrain->stream_radius;
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
  state->removed += record->marks + 1u;
  *record = state->records[--state->record_count];
}

uint64_t vkr_scene_terrain_revision(const VkrScene *scene) {
  const VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  uint64_t revision = state ? state->removed : 0u;
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    revision += state->records[i].marks;
  }
  return revision;
}

bool8_t vkr_scene_terrain_settled(const VkrScene *scene) {
  const VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    const TerrainRecord *record = &state->records[i];
    if (record->loaded && (record->mesh_dirty || record->building ||
                           record->attach_pending || record->collision_dirty ||
                           record->body_build || record->batch.active)) {
      return false_v;
    }
  }
  return true_v;
}

const VkrHeightfield *vkr_scene_terrain_field(const VkrScene *scene,
                                              VkrEntityId entity) {
  const TerrainRecord *record = terrain_find(scene, entity);
  return record && record->loaded ? &record->field : NULL;
}

bool8_t vkr_scene_terrain_streaming(const VkrScene *scene, VkrEntityId entity,
                                    VkrSceneTerrainStreaming *out) {
  const TerrainRecord *record = terrain_find(scene, entity);
  if (!record || !record->loaded) {
    return false_v;
  }
  const VkrHeightfield *field = &record->field;
  const uint32_t tiles = terrain_tiles(record);
  const uint32_t storage = field->tiles_per_side * field->tiles_per_side;
  *out = (VkrSceneTerrainStreaming){.streamed = record->streamed,
                                    .resident_tiles = field->resident_tiles};
  out->body_tiles =
      record->body_built
          ? (record->body_tiles.x1 - record->body_tiles.x0 + 1u) *
                (record->body_tiles.z1 - record->body_tiles.z0 + 1u)
          : 0u;
  for (uint32_t i = 0; i < tiles * tiles; ++i) {
    out->fine_tiles += record->tiles[i].id != 0u;
  }
  for (uint32_t i = 0; i < TERRAIN_OVERVIEW_MAX; ++i) {
    out->overview_tiles += record->overview[i].id != 0u;
  }
  for (uint32_t i = 0; i < storage; ++i) {
    out->unsaved_tiles += field->tiles[i] && field->tile_dirty[i];
  }
  if (!record->streamed && record->collision_built) {
    out->body_tiles = tiles * tiles;
  }
  return true_v;
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

bool8_t vkr_scene_terrain_require(VkrScene *scene, VkrEntityId entity,
                                  VkrHeightfieldRect rect) {
  TerrainRecord *record = terrain_find(scene, entity);
  const uint32_t samples =
      record && record->loaded ? vkr_heightfield_samples(&record->field) : 0u;
  return samples && rect.x1 < samples && rect.z1 < samples &&
         rect.x0 <= rect.x1 && rect.z0 <= rect.z1 &&
         vkr_heightfield_load_rect(&record->field, rect);
}

bool8_t vkr_scene_terrain_ground(VkrScene *scene, Vec3 top, float32_t bottom,
                                 Vec3 *out_position, Vec3 *out_normal) {
  VkrSceneTerrains *state = scene ? scene->terrains : NULL;
  bool8_t found = false_v;
  for (uint32_t i = 0; state && i < state->record_count; ++i) {
    TerrainRecord *record = &state->records[i];
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, record->entity, scene->comp_transform);
    if (!record->loaded || !transform) {
      continue;
    }
    VkrHeightfield *field = &record->field;
    const Vec3 origin = mat4_position(transform->world);
    const float32_t x = top.x - origin.x;
    const float32_t z = top.z - origin.z;
    const float32_t d = field->spacing;
    /* The cells under the point and their neighbours, which the normal
       reads. */
    VkrHeightfieldRect rect;
    float32_t height = 0.0f;
    if (!vkr_heightfield_rect_around(field, x, z, 2.0f * d, &rect) ||
        !vkr_heightfield_load_rect(field, rect) ||
        !vkr_heightfield_sample(field, x, z, &height)) {
      continue;
    }
    const float32_t y = origin.y + height;
    if (y > top.y || y < bottom || (found && y <= out_position->y)) {
      continue;
    }
    float32_t left = height;
    float32_t right = height;
    float32_t back = height;
    float32_t front = height;
    (void)vkr_heightfield_sample(field, x - d, z, &left);
    (void)vkr_heightfield_sample(field, x + d, z, &right);
    (void)vkr_heightfield_sample(field, x, z - d, &back);
    (void)vkr_heightfield_sample(field, x, z + d, &front);
    *out_position = vec3_new(top.x, y, top.z);
    *out_normal =
        vec3_normalize(vec3_new(left - right, 2.0f * d, back - front));
    found = true_v;
  }
  return found;
}

bool8_t vkr_scene_terrain_write(VkrScene *scene, VkrEntityId entity,
                                VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights) {
  TerrainRecord *record = terrain_find(scene, entity);
  const uint32_t samples =
      record && record->loaded ? vkr_heightfield_samples(&record->field) : 0u;
  if (!samples || rect.x1 >= samples || rect.z1 >= samples ||
      rect.x0 > rect.x1 || rect.z0 > rect.z1 ||
      !vkr_heightfield_load_rect(&record->field, rect)) {
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
    if (!vkr_scene_terrain_resolve(scene, record->source, path, sizeof(path))) {
      return false_v;
    }
    const bool8_t staged = terrain_staged_find(path) != NULL;
    if (staged) {
      /* The directory of a staged file may not exist yet. */
      char directory[1100];
      snprintf(directory, sizeof(directory), "%s", path);
      char *slash = strrchr(directory, '/');
      char *backslash = strrchr(directory, '\\');
      if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
      }
      if (slash) {
        *slash = '\0';
        const String8 text = string8_create_from_cstr(
            (const uint8_t *)directory, strlen(directory));
        if (!file_ensure_directory(scene->alloc, &text)) {
          snprintf(error, capacity, "Could not create %s", directory);
          return false_v;
        }
      }
    }
    if (!vkr_heightfield_save(&record->field, path, error, capacity)) {
      return false_v;
    }
    if (staged) {
      terrain_unstage(path);
    }
    record->unsaved = false_v;
  }
  return true_v;
}

// =============================================================================
// Products
// =============================================================================

/* A tile's 65 x 65 grid: field samples, or overview samples on a streamed
   terrain's distant tiles. Grid coordinates count the grid's own samples. */
typedef struct TerrainGrid {
  const VkrHeightfield *field;
  bool8_t overview;
  /* The last grid sample a side. */
  uint32_t last;
  /* Metres between grid samples. */
  float32_t spacing;
  float32_t half_size;
  /* A copy of the heights in metres and the weights of the grid samples
     from (x0, z0), TERRAIN_SNAPSHOT_SIDE a row, read instead of the field
     so that a job worker can read them while the field changes. */
  const float32_t *heights;
  const uint32_t *weights;
  uint32_t x0;
  uint32_t z0;
} TerrainGrid;

static TerrainGrid terrain_grid(const VkrHeightfield *field, bool8_t overview) {
  return (TerrainGrid){
      .field = field,
      .overview = overview,
      .last = overview ? vkr_heightfield_overview_samples(field) - 1u
                       : field->cells,
      .spacing =
          overview ? field->spacing * TERRAIN_OVERVIEW_SCALE : field->spacing,
      .half_size = vkr_heightfield_half_size(field),
  };
}

static float32_t terrain_grid_height(const TerrainGrid *grid, uint32_t x,
                                     uint32_t z) {
  if (grid->heights) {
    return grid
        ->heights[(z - grid->z0) * TERRAIN_SNAPSHOT_SIDE + (x - grid->x0)];
  }
  return grid->overview ? vkr_heightfield_overview_at(grid->field, x, z)
                        : vkr_heightfield_at(grid->field, x, z);
}

/* Normal of grid sample (x, z) from its neighbours' heights. */
static Vec3 terrain_normal(const TerrainGrid *grid, uint32_t x, uint32_t z) {
  const uint32_t last = grid->last;
  const float32_t left = terrain_grid_height(grid, x ? x - 1u : x, z);
  const float32_t right = terrain_grid_height(grid, Min(last, x + 1u), z);
  const float32_t down = terrain_grid_height(grid, x, z ? z - 1u : z);
  const float32_t up = terrain_grid_height(grid, x, Min(last, z + 1u));
  const float32_t dx =
      (right - left) /
      ((float32_t)(Min(last, x + 1u) - (x ? x - 1u : x)) * grid->spacing);
  const float32_t dz =
      (up - down) /
      ((float32_t)(Min(last, z + 1u) - (z ? z - 1u : z)) * grid->spacing);
  return vec3_normalize(vec3_new(-dx, 1.0f, -dz));
}

/* Layer weights of grid sample (x, z), each 0 to 1. */
static uint32_t terrain_grid_weights(const TerrainGrid *grid, uint32_t x,
                                     uint32_t z) {
  const VkrHeightfield *field = grid->field;
  if (grid->weights) {
    return grid
        ->weights[(z - grid->z0) * TERRAIN_SNAPSHOT_SIDE + (x - grid->x0)];
  }
  return grid->overview ? field->overview_weights
                              [z * vkr_heightfield_overview_samples(field) + x]
                        : vkr_heightfield_weights_at(field, x, z);
}

static Vec4 terrain_weights(const TerrainGrid *grid, uint32_t x, uint32_t z) {
  const uint32_t w = terrain_grid_weights(grid, x, z);
  return vec4_new(
      (float32_t)(w & 0xFFu) / 255.0f, (float32_t)((w >> 8u) & 0xFFu) / 255.0f,
      (float32_t)((w >> 16u) & 0xFFu) / 255.0f, (float32_t)(w >> 24u) / 255.0f);
}

/* The vertex of grid sample (x, z). */
static VkrVertex3d terrain_grid_vertex(const TerrainGrid *grid, uint32_t x,
                                       uint32_t z, float32_t uv_scale) {
  const float32_t half = grid->half_size;
  const Vec3 p = vec3_new((float32_t)x * grid->spacing - half,
                          terrain_grid_height(grid, x, z),
                          (float32_t)z * grid->spacing - half);
  const Vec3 n = terrain_normal(grid, x, z);
  return (VkrVertex3d){
      .position = {p.x, p.y, p.z},
      .normal = {n.x, n.y, n.z},
      .texcoord = vec2_new(p.x * uv_scale, p.z * uv_scale),
      /* The terrain material reads the four layer weights here. */
      .colour = terrain_weights(grid, x, z),
      .tangent = vec4_new(1.0f, 0.0f, 0.0f, 1.0f),
  };
}

/* Whether grid cell (x, z) of an overview tile lies under a fine tile that
   draws; `holes` holds one bit per fine tile, rows of
   TERRAIN_FINE_PER_OVERVIEW. */
static bool8_t terrain_hole(const uint64_t *holes, uint32_t x, uint32_t z) {
  if (!holes) {
    return false_v;
  }
  const uint32_t bit =
      (z / TERRAIN_CELLS_PER_FINE) * TERRAIN_FINE_PER_OVERVIEW +
      x / TERRAIN_CELLS_PER_FINE;
  return (holes[bit / 64u] >> (bit % 64u)) & 1u;
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

/* vkr_scene_terrain_tile_indices, leaving out the cells and edge skirts
   `holes` covers; a tile with holes keeps only the levels whose cells each
   lie in one fine tile. */
static uint32_t terrain_grid_indices(const VkrVertex3d *vertices,
                                     float32_t spacing, const uint64_t *holes,
                                     uint32_t *indices, uint32_t capacity,
                                     VkrGpuGeometryLodRow *out_lod) {
  const uint32_t side = VKR_HEIGHTFIELD_TILE_CELLS + 1u;
  const uint32_t grid = side * side;
  const uint32_t levels =
      holes ? TERRAIN_HOLED_LEVELS : VKR_SCENE_TERRAIN_LOD_LEVELS;
  if (capacity < vkr_scene_terrain_tile_index_count()) {
    return 0u;
  }
  /* Level L draws every 2^L-th grid line with level 0's diagonal (ADR-084);
     its error is its surface's largest height difference from the samples,
     at least a share of its cell size so that painted weights thin out
     with distance too. */
  VkrGpuGeometryLodRow lod = {.level_count = levels,
                              .flags = VKR_GPU_GEOMETRY_LOD_TERRAIN_GRID};
  uint32_t count = 0u;
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t stride = 1u << level;
    const uint32_t first = count;
    for (uint32_t z = 0; z < VKR_HEIGHTFIELD_TILE_CELLS; z += stride) {
      for (uint32_t x = 0; x < VKR_HEIGHTFIELD_TILE_CELLS; x += stride) {
        if (terrain_hole(holes, x, z)) {
          continue;
        }
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
        if (terrain_hole(holes,
                         Min(Min(xa, xb), VKR_HEIGHTFIELD_TILE_CELLS - 1u),
                         Min(Min(za, zb), VKR_HEIGHTFIELD_TILE_CELLS - 1u))) {
          continue;
        }
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

uint32_t vkr_scene_terrain_tile_indices(const VkrVertex3d *vertices,
                                        float32_t spacing, uint32_t *indices,
                                        uint32_t capacity,
                                        VkrGpuGeometryLodRow *out_lod) {
  return terrain_grid_indices(vertices, spacing, NULL, indices, capacity,
                              out_lod);
}

/* Metres a skirt reaches below its edge. A streamed terrain's skirts reach
   far enough to hide where full detail meets the overview. */
static float32_t terrain_skirt(const TerrainRecord *record) {
  return TERRAIN_SKIRT_SPACINGS * record->field.spacing *
         (record->streamed ? TERRAIN_OVERVIEW_SCALE : 1.0f);
}

static void terrain_geometry_name(const TerrainRecord *record, const char *kind,
                                  uint32_t tx, uint32_t tz, char *out,
                                  uint32_t capacity) {
  snprintf(out, capacity, "terrain_%s_%u_%u_%u_%u_%u_%u", kind,
           (unsigned)record->entity.parts.world,
           (unsigned)record->entity.parts.index,
           (unsigned)record->entity.parts.generation, record->serial, tx, tz);
}

/* One fine or overview tile of a batch (TerrainBatch). The main thread
   copies its samples and inputs, a job worker fills the packed `config`
   from them alone, and the main thread publishes it. */
typedef struct TerrainTileWork {
  TerrainGrid grid;
  uint32_t tx;
  uint32_t tz;
  /* Overview tiles leave out the cells under the fine tiles that draw. */
  bool8_t holed;
  uint64_t holes[TERRAIN_HOLE_WORDS];
  float32_t uv_scale;
  float32_t skirt;
  char name[GEOMETRY_NAME_MAX_LENGTH];
  float32_t *heights;
  uint32_t *weights;
  VkrVertex3d *vertices;
  uint32_t *indices;
  VkrPackedStaticVertex *packed;
  VkrGpuGeometryDecodeRecord decodes[VKR_GEOMETRY_PACKED_DECODES_MAX];
  VkrGeometryConfig config;
  bool8_t ok;
} TerrainTileWork;

#define TERRAIN_TILE_VERTICES                                                  \
  ((VKR_HEIGHTFIELD_TILE_CELLS + 1u) * (VKR_HEIGHTFIELD_TILE_CELLS + 1u) +     \
   4u * (VKR_HEIGHTFIELD_TILE_CELLS + 1u))

/* One tile's geometry, packed: 65 x 65 grid samples from grid sample
   (x0, z0) and a skirt down from each edge, in the terrain's local space,
   leaving out the cells `holes` covers. Reads the work alone, so job
   workers prepare a batch while the field changes. */
static void terrain_tile_prepare(void *context, uint32_t item) {
  TerrainTileWork *work = &((TerrainTileWork *)context)[item];
  const TerrainGrid *grid = &work->grid;
  const uint64_t *holes = work->holed ? work->holes : NULL;
  VkrVertex3d *vertices = work->vertices;
  const uint32_t side = VKR_HEIGHTFIELD_TILE_CELLS + 1u;
  const uint32_t vertex_grid = side * side;
  const uint32_t x0 = work->tx * VKR_HEIGHTFIELD_TILE_CELLS;
  const uint32_t z0 = work->tz * VKR_HEIGHTFIELD_TILE_CELLS;
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t z = 0; z < side; ++z) {
    for (uint32_t x = 0; x < side; ++x) {
      const VkrVertex3d v =
          terrain_grid_vertex(grid, x0 + x, z0 + z, work->uv_scale);
      vertices[z * side + x] = v;
      lo = vec3_new(Min(lo.x, v.position.x), Min(lo.y, v.position.y),
                    Min(lo.z, v.position.z));
      hi = vec3_new(Max(hi.x, v.position.x), Max(hi.y, v.position.y),
                    Max(hi.z, v.position.z));
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
      v.position.y -= work->skirt;
      vertices[vertex_grid + edge * side + i] = v;
      lo.y = Min(lo.y, v.position.y);
    }
  }
  const uint32_t index_capacity = vkr_scene_terrain_tile_index_count();
  VkrGpuGeometryLodRow lod = {0};
  const uint32_t count = terrain_grid_indices(
      vertices, grid->spacing, holes, work->indices, index_capacity, &lod);
  VkrGeometryConfig config = {
      .vertex_size = sizeof(VkrVertex3d),
      .vertex_count = TERRAIN_TILE_VERTICES,
      .vertices = vertices,
      .index_size = sizeof(uint32_t),
      .index_count = count,
      .indices = work->indices,
      .range_index_count = lod.levels[0].index_count,
      .lod = lod,
      .center = vec3_scale(vec3_add(lo, hi), 0.5f),
      .min_extents = lo,
      .max_extents = hi,
  };
  MemCopy(config.name, work->name, sizeof(config.name));
  work->ok = count != 0u && vkr_geometry_pack(&config, work->packed,
                                              work->decodes, &work->config);
}

/* Skirts down from the edges of overview tile (tx, tz)'s holes, facing into
   them, so full detail meets the overview without a gap; none without
   holes. Edges between tiles take the neighbour's own skirt. */
static VkrGeometryHandle terrain_seam_geometry(VkrScene *scene,
                                               TerrainRecord *record,
                                               const TerrainGrid *grid,
                                               uint32_t tx, uint32_t tz,
                                               const uint64_t *holes) {
  /* Neighbour offsets and the edge corners of a cell, ordered so that each
     skirt faces its neighbour: -Z, +X, +Z, -X. */
  static const int32_t s_step[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
  static const uint32_t s_corner[4][4] = {
      {0u, 0u, 1u, 0u}, {1u, 0u, 1u, 1u}, {1u, 1u, 0u, 1u}, {0u, 1u, 0u, 0u}};
  struct VkrRenderAssets *assets = scene->assets;
  const uint32_t cells = VKR_HEIGHTFIELD_TILE_CELLS;
  uint32_t edges = 0u;
  for (uint32_t z = 0; z < cells; ++z) {
    for (uint32_t x = 0; x < cells; ++x) {
      for (uint32_t e = 0; !terrain_hole(holes, x, z) && e < 4u; ++e) {
        const int32_t nx = (int32_t)x + s_step[e][0];
        const int32_t nz = (int32_t)z + s_step[e][1];
        edges += nx >= 0 && nz >= 0 && nx < (int32_t)cells &&
                 nz < (int32_t)cells &&
                 terrain_hole(holes, (uint32_t)nx, (uint32_t)nz);
      }
    }
  }
  if (!edges) {
    return (VkrGeometryHandle){0};
  }
  VkrVertex3d *vertices = vkr_allocator_alloc(
      &assets->scratch_allocator, edges * 4u * sizeof(*vertices), TERRAIN_TAG);
  uint32_t *indices = vkr_allocator_alloc(
      &assets->scratch_allocator, edges * 6u * sizeof(*indices), TERRAIN_TAG);
  if (!vertices || !indices) {
    return (VkrGeometryHandle){0};
  }
  const uint32_t x0 = tx * cells;
  const uint32_t z0 = tz * cells;
  const float32_t uv_scale = 1.0f / Max(0.01f, record->texture_size);
  const float32_t skirt = terrain_skirt(record);
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  uint32_t vertex_count = 0u;
  uint32_t index_count = 0u;
  for (uint32_t z = 0; z < cells; ++z) {
    for (uint32_t x = 0; x < cells; ++x) {
      for (uint32_t e = 0; !terrain_hole(holes, x, z) && e < 4u; ++e) {
        const int32_t nx = (int32_t)x + s_step[e][0];
        const int32_t nz = (int32_t)z + s_step[e][1];
        if (nx < 0 || nz < 0 || nx >= (int32_t)cells || nz >= (int32_t)cells ||
            !terrain_hole(holes, (uint32_t)nx, (uint32_t)nz)) {
          continue;
        }
        const uint32_t first = vertex_count;
        for (uint32_t k = 0; k < 2u; ++k) {
          VkrVertex3d top =
              terrain_grid_vertex(grid, x0 + x + s_corner[e][2u * k],
                                  z0 + z + s_corner[e][2u * k + 1u], uv_scale);
          VkrVertex3d low = top;
          low.position.y -= skirt;
          vertices[first + k] = top;
          vertices[first + 2u + k] = low;
          lo = vec3_new(Min(lo.x, top.position.x), Min(lo.y, low.position.y),
                        Min(lo.z, top.position.z));
          hi = vec3_new(Max(hi.x, top.position.x), Max(hi.y, top.position.y),
                        Max(hi.z, top.position.z));
        }
        vertex_count += 4u;
        indices[index_count++] = first;
        indices[index_count++] = first + 1u;
        indices[index_count++] = first + 2u;
        indices[index_count++] = first + 2u;
        indices[index_count++] = first + 1u;
        indices[index_count++] = first + 3u;
      }
    }
  }
  VkrGeometryConfig config = {
      .vertex_size = sizeof(VkrVertex3d),
      .vertex_count = vertex_count,
      .vertices = vertices,
      .index_size = sizeof(uint32_t),
      .index_count = index_count,
      .indices = indices,
      .center = vec3_scale(vec3_add(lo, hi), 0.5f),
      .min_extents = lo,
      .max_extents = hi,
  };
  terrain_geometry_name(record, "seam", tx, tz, config.name,
                        sizeof(config.name));
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
  /* The vertex colors hold layer weights, which a plain material would draw
     as a tint. Until the layers stream in, the terrain is an opaque PBR
     terrain material whose every layer is white and rough, so it draws
     white. */
  VkrMaterial *placeholder =
      vkr_material_system_get_by_handle(&assets->material_system, handle);
  if (placeholder) {
    placeholder->material_type = VKR_MATERIAL_TYPE_PBR;
    placeholder->alpha_mode = VKR_MATERIAL_ALPHA_OPAQUE;
    placeholder->alpha_mode_explicit = true_v;
    placeholder->pbr.metallic = 0.0f;
    placeholder->pbr.roughness = 1.0f;
    placeholder->terrain = true_v;
    for (uint32_t i = 0; i < ArrayCount(placeholder->layers); ++i) {
      placeholder->layers[i] = (VkrMaterialLayer){
          .base_color = placeholder->pbr.base_color,
          .metallic = 0.0f,
          .roughness = 1.0f,
          .normal_scale = 1.0f,
          .occlusion_strength = 1.0f,
      };
    }
    for (uint32_t slot = VKR_TEXTURE_SLOT_LAYER1_BASE_COLOR;
         slot < VKR_TEXTURE_SLOT_COUNT; ++slot) {
      placeholder->textures[slot] = vkr_material_system_get_default_texture(
          &assets->material_system, (VkrTextureSlot)slot);
    }
    if (!vkr_material_system_publish(&assets->material_system, handle,
                                     &error)) {
      log_warn("Scene: the terrain placeholder material did not publish (%d)",
               (int)error);
    }
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

/* The fine tiles of overview tile (ox, oz) that draw, one bit each; false
   when none do. */
static bool8_t terrain_overview_holes(const TerrainRecord *record, uint32_t ox,
                                      uint32_t oz, uint64_t *holes) {
  const uint32_t tiles = terrain_tiles(record);
  bool8_t any = false_v;
  MemZero(holes, TERRAIN_HOLE_WORDS * sizeof(uint64_t));
  for (uint32_t z = 0; z < TERRAIN_FINE_PER_OVERVIEW; ++z) {
    for (uint32_t x = 0; x < TERRAIN_FINE_PER_OVERVIEW; ++x) {
      const uint32_t index = (oz * TERRAIN_FINE_PER_OVERVIEW + z) * tiles +
                             ox * TERRAIN_FINE_PER_OVERVIEW + x;
      if (record->tiles[index].id) {
        terrain_bit_set(holes, z * TERRAIN_FINE_PER_OVERVIEW + x, true_v);
        any = true_v;
      }
    }
  }
  return any;
}

/* Bytes of one batch: its works, then each work's buffers. */
static uint64_t terrain_batch_bytes(void) {
  const uint64_t snapshot =
      (uint64_t)TERRAIN_SNAPSHOT_SIDE * TERRAIN_SNAPSHOT_SIDE;
  const uint64_t tile =
      snapshot * (sizeof(float32_t) + sizeof(uint32_t)) +
      TERRAIN_TILE_VERTICES *
          (sizeof(VkrVertex3d) + sizeof(VkrPackedStaticVertex)) +
      (uint64_t)vkr_scene_terrain_tile_index_count() * sizeof(uint32_t);
  return TERRAIN_BATCH_MAX * (sizeof(TerrainTileWork) + tile);
}

/* Makes the batch's storage on first use. */
static bool8_t terrain_batch_reserve(VkrScene *scene, TerrainRecord *record) {
  TerrainBatch *batch = &record->batch;
  if (batch->works) {
    return true_v;
  }
  const uint64_t bytes = terrain_batch_bytes();
  uint8_t *storage = vkr_allocator_alloc(scene->alloc, bytes, TERRAIN_TAG);
  if (!storage) {
    return false_v;
  }
  MemZero(storage, sizeof(TerrainTileWork) * TERRAIN_BATCH_MAX);
  batch->works = (TerrainTileWork *)storage;
  batch->bytes = bytes;
  uint8_t *at = storage + sizeof(TerrainTileWork) * TERRAIN_BATCH_MAX;
  const uint64_t snapshot =
      (uint64_t)TERRAIN_SNAPSHOT_SIDE * TERRAIN_SNAPSHOT_SIDE;
  for (uint32_t i = 0; i < TERRAIN_BATCH_MAX; ++i) {
    TerrainTileWork *work = &batch->works[i];
    work->heights = (float32_t *)at;
    at += snapshot * sizeof(float32_t);
    work->weights = (uint32_t *)at;
    at += snapshot * sizeof(uint32_t);
    work->vertices = (VkrVertex3d *)at;
    at += TERRAIN_TILE_VERTICES * sizeof(VkrVertex3d);
    work->packed = (VkrPackedStaticVertex *)at;
    at += TERRAIN_TILE_VERTICES * sizeof(VkrPackedStaticVertex);
    work->indices = (uint32_t *)at;
    at += (uint64_t)vkr_scene_terrain_tile_index_count() * sizeof(uint32_t);
  }
  return true_v;
}

/* Adds tile (tx, tz) of the fine grid, or the overview grid when
   `overview`, to the batch being gathered, copying the samples it reads.
   Overview tiles leave out `holes` when given. NULL when the batch is full
   or its storage could not be made. */
static TerrainTileWork *terrain_batch_add(VkrScene *scene,
                                          TerrainRecord *record, uint32_t tx,
                                          uint32_t tz, bool8_t overview,
                                          const uint64_t *holes) {
  TerrainBatch *batch = &record->batch;
  if (batch->count == TERRAIN_BATCH_MAX ||
      !terrain_batch_reserve(scene, record)) {
    return NULL;
  }
  TerrainTileWork *work = &batch->works[batch->count++];
  const TerrainGrid live = terrain_grid(&record->field, overview);
  /* The tile's samples and the neighbour each normal reads past its edge. */
  const uint32_t x0 = tx * VKR_HEIGHTFIELD_TILE_CELLS;
  const uint32_t z0 = tz * VKR_HEIGHTFIELD_TILE_CELLS;
  const uint32_t sx0 = x0 ? x0 - 1u : 0u;
  const uint32_t sz0 = z0 ? z0 - 1u : 0u;
  const uint32_t sx1 = Min(live.last, x0 + VKR_HEIGHTFIELD_TILE_CELLS + 1u);
  const uint32_t sz1 = Min(live.last, z0 + VKR_HEIGHTFIELD_TILE_CELLS + 1u);
  for (uint32_t z = sz0; z <= sz1; ++z) {
    for (uint32_t x = sx0; x <= sx1; ++x) {
      const uint32_t at = (z - sz0) * TERRAIN_SNAPSHOT_SIDE + (x - sx0);
      work->heights[at] = terrain_grid_height(&live, x, z);
      work->weights[at] = terrain_grid_weights(&live, x, z);
    }
  }
  work->grid = live;
  work->grid.field = NULL;
  work->grid.heights = work->heights;
  work->grid.weights = work->weights;
  work->grid.x0 = sx0;
  work->grid.z0 = sz0;
  work->tx = tx;
  work->tz = tz;
  work->holed = holes != NULL;
  if (holes) {
    MemCopy(work->holes, holes, sizeof(work->holes));
  }
  work->uv_scale = 1.0f / Max(0.01f, record->texture_size);
  work->skirt = terrain_skirt(record);
  terrain_geometry_name(record, overview ? "overview" : "tile", tx, tz,
                        work->name, sizeof(work->name));
  work->ok = false_v;
  return work;
}

/* Starts preparing the gathered tiles on the job workers. */
static void terrain_batch_begin(VkrScene *scene, TerrainRecord *record,
                                TerrainBatchKind kind) {
  TerrainBatch *batch = &record->batch;
  batch->kind = kind;
  batch->age = 0u;
  batch->active = true_v;
  batch->loop = vkr_job_for_begin(scene->assets->job_system, batch->count,
                                  terrain_tile_prepare, batch->works);
}

/* Publishes the batch once its tiles are prepared, or once it has waited
   TERRAIN_BATCH_WAIT_MAX updates, finishing them here; each replaces the
   geometry its tile had. Loaded tiles mark the mesh changed. */
static void terrain_batch_poll(VkrScene *scene, TerrainRecord *record) {
  TerrainBatch *batch = &record->batch;
  if (!batch->active || (!vkr_job_for_done(batch->loop) &&
                         ++batch->age < TERRAIN_BATCH_WAIT_MAX)) {
    return;
  }
  vkr_job_for_end(batch->loop);
  batch->loop = NULL;
  batch->active = false_v;
  struct VkrRenderAssets *assets = scene->assets;
  const uint32_t tiles = terrain_tiles(record);
  const uint32_t overview_tiles = terrain_overview_tiles(record);
  const bool8_t overview = batch->kind == TERRAIN_BATCH_OVERVIEW;
  bool8_t ok = true_v;
  for (uint32_t i = 0; i < batch->count; ++i) {
    const TerrainTileWork *work = &batch->works[i];
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrGeometryHandle handle =
        work->ok ? vkr_geometry_system_create(&assets->geometry_system,
                                              &work->config, true_v, &error)
                 : (VkrGeometryHandle){0};
    if (!handle.id) {
      ok = false_v;
      continue;
    }
    if (!overview) {
      const uint32_t index = work->tz * tiles + work->tx;
      terrain_geometry_release(scene, &record->tiles[index]);
      record->tiles[index] = handle;
      terrain_change_tile(record, work->tx, work->tz);
      if (batch->kind == TERRAIN_BATCH_LOADS) {
        record->overview_dirty |= 1ull << terrain_overview_of(record, index);
        record->mesh_dirty = true_v;
      }
      continue;
    }
    const uint32_t index = work->tz * overview_tiles + work->tx;
    terrain_geometry_release(scene, &record->overview[index]);
    terrain_geometry_release(scene, &record->seams[index]);
    record->overview[index] = handle;
    if (work->holed) {
      const TerrainGrid grid = terrain_grid(&record->field, true_v);
      record->seams[index] = terrain_seam_geometry(
          scene, record, &grid, work->tx, work->tz, work->holes);
    }
  }
  batch->count = 0u;
  if (!ok) {
    snprintf(record->status, sizeof(record->status), "%s",
             batch->kind == TERRAIN_BATCH_LOADS
                 ? "A terrain tile could not be streamed in"
                 : "The terrain mesh failed");
  }
}

/* Drops the batch unpublished, once no job worker reads it, and its
   storage. */
static void terrain_batch_release(VkrScene *scene, TerrainRecord *record) {
  TerrainBatch *batch = &record->batch;
  vkr_job_for_end(batch->loop);
  if (batch->works) {
    vkr_allocator_free(scene->alloc, batch->works, batch->bytes, TERRAIN_TAG);
  }
  *batch = (TerrainBatch){0};
}

/* Starts rebuilding the changed tiles: the layer material first, when its
   layers changed. */
static bool8_t terrain_begin_build(VkrScene *scene, TerrainRecord *record) {
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
  record->building = true_v;
  return true_v;
}

/* Starts a batch rebuilding changed fine tiles, or once none is left,
   overview tiles; their dirty bits clear now, so edits meanwhile mark them
   again. Done once none is left. False when a batch could not start. */
static bool8_t terrain_build_step(VkrScene *scene, TerrainRecord *record,
                                  bool8_t *out_done) {
  const uint32_t tiles = terrain_tiles(record);
  *out_done = false_v;
  for (uint32_t index = 0;
       record->batch.count < TERRAIN_BATCH_MAX && index < tiles * tiles;
       ++index) {
    const bool8_t dirty = terrain_bit(record->dirty_tiles, index);
    const bool8_t drawn = record->tiles[index].id != 0u;
    /* A streamed terrain rebuilds only tiles that draw; streaming adds the
       rest. */
    if (!(record->streamed ? dirty && drawn : dirty || !drawn)) {
      continue;
    }
    if (!terrain_batch_add(scene, record, index % tiles, index / tiles, false_v,
                           NULL)) {
      return false_v;
    }
    terrain_bit_set(record->dirty_tiles, index, false_v);
  }
  if (record->batch.count) {
    terrain_batch_begin(scene, record, TERRAIN_BATCH_TILES);
    return true_v;
  }
  const uint32_t overview_tiles = terrain_overview_tiles(record);
  for (uint32_t index = 0; record->batch.count < TERRAIN_BATCH_MAX &&
                           index < overview_tiles * overview_tiles;
       ++index) {
    if (!(record->overview_dirty & (1ull << index))) {
      continue;
    }
    const uint32_t ox = index % overview_tiles;
    const uint32_t oz = index / overview_tiles;
    uint64_t holes[TERRAIN_HOLE_WORDS];
    const bool8_t holed = terrain_overview_holes(record, ox, oz, holes);
    bool8_t covered = true_v;
    for (uint32_t i = 0; i < TERRAIN_HOLE_WORDS; ++i) {
      covered = covered && holes[i] == UINT64_MAX;
    }
    record->overview_dirty &= ~(1ull << index);
    /* A tile fine tiles cover entirely draws nothing. */
    if (covered) {
      terrain_geometry_release(scene, &record->overview[index]);
      terrain_geometry_release(scene, &record->seams[index]);
      continue;
    }
    if (!terrain_batch_add(scene, record, ox, oz, true_v,
                           holed ? holes : NULL)) {
      return false_v;
    }
  }
  if (record->batch.count) {
    terrain_batch_begin(scene, record, TERRAIN_BATCH_OVERVIEW);
    return true_v;
  }
  /* Tiles a streamed terrain does not draw keep no dirty bit: streaming
     builds them afresh. */
  MemZero(record->dirty_tiles, sizeof(record->dirty_tiles));
  *out_done = true_v;
  return true_v;
}

static bool8_t terrain_geometry_settled(VkrGeometrySystem *system,
                                        VkrGeometryHandle handle) {
  const VkrGeometry *geometry =
      handle.id ? vkr_geometry_system_get_by_handle(system, handle) : NULL;
  return !geometry || vkr_publication_state_settled(&geometry->publication);
}

/* Whether every geometry and the material the mesh would show finished
   uploading. A mesh attached sooner waits unpublished: the whole terrain
   leaves the frame, and every local shadow redraws as the publication
   lands (level toolkit audit A1). */
static bool8_t terrain_mesh_settled(VkrScene *scene,
                                    const TerrainRecord *record) {
  struct VkrRenderAssets *assets = scene->assets;
  VkrGeometrySystem *system = &assets->geometry_system;
  const VkrPublicationState *material =
      record->material.id ? vkr_material_system_publication(
                                &assets->material_system, record->material)
                          : NULL;
  if (material && !vkr_publication_state_settled(material)) {
    return false_v;
  }
  const uint32_t tiles = terrain_tiles(record);
  for (uint32_t i = 0; i < tiles * tiles; ++i) {
    if (!terrain_geometry_settled(system, record->tiles[i])) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < TERRAIN_OVERVIEW_MAX; ++i) {
    if (!terrain_geometry_settled(system, record->overview[i]) ||
        !terrain_geometry_settled(system, record->seams[i])) {
      return false_v;
    }
  }
  return true_v;
}

/* Re-attaches the mesh with every tile that draws. */
static bool8_t terrain_attach_mesh(VkrScene *scene, TerrainRecord *record) {
  struct VkrRenderAssets *assets = scene->assets;
  const uint32_t tiles = terrain_tiles(record);
  const uint32_t overview_tiles = terrain_overview_tiles(record);
  bool8_t ok = true_v;
  uint32_t count = 0u;
  for (uint32_t i = 0; i < tiles * tiles; ++i) {
    count += record->tiles[i].id != 0u;
  }
  for (uint32_t i = 0; i < overview_tiles * overview_tiles; ++i) {
    count += (record->overview[i].id != 0u) + (record->seams[i].id != 0u);
  }
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  VkrSubMeshDesc *submeshes =
      ok && count ? vkr_allocator_alloc(&assets->scratch_allocator,
                                        count * sizeof(*submeshes), TERRAIN_TAG)
                  : NULL;
  ok = ok && submeshes;
  uint32_t filled = 0u;
  for (uint32_t i = 0; ok && i < TERRAIN_TILES_MAX + 2u * TERRAIN_OVERVIEW_MAX;
       ++i) {
    const VkrGeometryHandle geometry =
        i < TERRAIN_TILES_MAX ? record->tiles[i]
        : i < TERRAIN_TILES_MAX + TERRAIN_OVERVIEW_MAX
            ? record->overview[i - TERRAIN_TILES_MAX]
            : record->seams[i - TERRAIN_TILES_MAX - TERRAIN_OVERVIEW_MAX];
    if (!geometry.id) {
      continue;
    }
    /* The mesh acquires its own references; the record keeps its. */
    submeshes[filled++] = (VkrSubMeshDesc){
        .geometry = geometry,
        .material = record->material,
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .owns_geometry = true_v,
        .owns_material = record->material.id != 0u,
    };
  }
  /* The mesh changes in place, naming the ground that changed in world
     space, so retained shadows elsewhere stay valid. */
  const SceneTransform *transform = vkr_entity_get_component(
      scene->world, record->entity, scene->comp_transform);
  const Vec3 position =
      transform ? mat4_position(transform->world) : vec3_zero();
  Vec3 change_min[TERRAIN_CHANGE_MAX];
  Vec3 change_max[TERRAIN_CHANGE_MAX];
  const uint32_t change_count = record->change_all ? 0u : record->change_count;
  for (uint32_t i = 0; i < change_count; ++i) {
    change_min[i] = vec3_add(record->change_min[i], position);
    change_max[i] = vec3_add(record->change_max[i], position);
  }
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (ok && !vkr_scene_replace_generated_mesh(scene, record->entity, submeshes,
                                              filled, change_min, change_max,
                                              change_count, &scene_error)) {
    vkr_scene_detach_generated_mesh(scene, record->entity);
    ok = vkr_scene_attach_generated_mesh(scene, record->entity, submeshes,
                                         filled, &scene_error);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  record->attach_pending = false_v;
  record->change_count = 0u;
  record->change_all = false_v;
  return ok;
}

/* A height field collider of `side` padded samples a side whose first
   sample sits at local corner (x, z) metres. */
static VkrPhysicsColliderDesc terrain_collider(const TerrainRecord *record,
                                               const float32_t *heights,
                                               uint32_t side, Vec3 position,
                                               float32_t x, float32_t z) {
  const float32_t half = vkr_heightfield_half_size(&record->field);
  return (VkrPhysicsColliderDesc){
      .entity_id = record->entity.u64,
      .shape = VKR_PHYSICS_HEIGHT_FIELD,
      .position = {position.x - half + x, position.y, position.z - half + z},
      .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
      .scale = {1.0f, 1.0f, 1.0f},
      .geometry = {.positions = heights,
                   .vertex_count = side * side,
                   .height_samples = side,
                   .height_spacing = record->field.spacing},
      .enabled = true_v,
  };
}

/* The terrain's one static body, from `count` colliders. */
static bool8_t terrain_body(VkrScene *scene, TerrainRecord *record,
                            const VkrPhysicsColliderDesc *colliders,
                            uint32_t count) {
  const char *error = NULL;
  const bool8_t ok = vkr_scene_physics_generated_set(
      scene, terrain_collision_key(record->entity), record->entity, colliders,
      count, false_v, &error);
  if (!ok && error) {
    log_warn("Scene: terrain collision failed: %s", error);
  }
  return ok;
}

/* Gives the terrain the body `heights` describe (`side` samples a side whose
   first sits at local corner metres), at once when `now` is set, else once
   its shape has built on a physics worker; the body set meanwhile stays.
   Takes `heights`. False when physics refused it at once. */
static bool8_t terrain_body_set(VkrScene *scene, TerrainRecord *record,
                                float32_t *heights, uint32_t side,
                                Vec3 position, Vec2 corner, bool8_t now) {
  terrain_body_cancel(scene, record);
  const uint64_t bytes = sizeof(float32_t) * (uint64_t)side * side;
  if (!now) {
    const VkrPhysicsColliderDesc collider =
        terrain_collider(record, heights, side, position, corner.x, corner.y);
    record->body_build = vkr_physics_shape_build_begin(&collider);
  }
  if (record->body_build) {
    record->body_heights = heights;
    record->body_side = side;
    record->body_corner = corner;
    return true_v;
  }
  const VkrPhysicsColliderDesc collider =
      terrain_collider(record, heights, side, position, corner.x, corner.y);
  const bool8_t ok = terrain_body(scene, record, &collider, 1u);
  vkr_allocator_free(scene->alloc, heights, bytes, TERRAIN_TAG);
  return ok;
}

/* The finished body build, if any, becomes the terrain's body. */
static void terrain_body_poll(VkrScene *scene, TerrainRecord *record) {
  if (!vkr_physics_shape_build_done(record->body_build)) {
    return;
  }
  VkrPhysicsColliderDesc collider = terrain_collider(
      record, record->body_heights, record->body_side, record->built_position,
      record->body_corner.x, record->body_corner.y);
  collider.prebuilt = record->body_build;
  const bool8_t ok = terrain_body(scene, record, &collider, 1u);
  terrain_body_cancel(scene, record);
  if (record->streamed) {
    record->body_built = ok;
    record->collision_rejected = !ok;
  } else {
    record->collision_built = ok;
  }
}

/* One height field body over the whole terrain, holes padding the grid to a
   multiple of four samples; at once when the terrain has none. */
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
  const bool8_t now = !record->collision_built;
  const bool8_t ok = terrain_body_set(scene, record, heights, padded, position,
                                      vec2_new(0.0f, 0.0f), now);
  if (!record->body_build) {
    record->collision_built = ok;
  }
  record->built_position = position;
  record->collision_dirty = false_v;
}

/* The body of a streamed terrain: one height field over fine tiles
   `tiles` (inclusive), holes padding it to a multiple of four samples. A
   body needed `now` is built at once, else on a physics worker while the
   current one stays. */
static void terrain_build_window_collision(VkrScene *scene,
                                           TerrainRecord *record,
                                           VkrHeightfieldRect tiles,
                                           Vec3 position, bool8_t now) {
  VkrHeightfield *field = &record->field;
  const VkrHeightfieldRect rect = {
      .x0 = tiles.x0 * VKR_HEIGHTFIELD_TILE_CELLS,
      .z0 = tiles.z0 * VKR_HEIGHTFIELD_TILE_CELLS,
      .x1 = (tiles.x1 + 1u) * VKR_HEIGHTFIELD_TILE_CELLS,
      .z1 = (tiles.z1 + 1u) * VKR_HEIGHTFIELD_TILE_CELLS};
  const uint32_t side = Max(rect.x1 - rect.x0, rect.z1 - rect.z0) + 1u;
  const uint32_t padded = (side + 3u) & ~3u;
  const uint64_t bytes = sizeof(float32_t) * (uint64_t)padded * padded;
  terrain_body_cancel(scene, record);
  record->body_tiles = tiles;
  record->body_edited = false_v;
  record->built_position = position;
  float32_t *heights = vkr_allocator_alloc(scene->alloc, bytes, TERRAIN_TAG);
  if (!heights || !vkr_heightfield_load_rect(field, rect)) {
    if (heights) {
      vkr_allocator_free(scene->alloc, heights, bytes, TERRAIN_TAG);
    }
    record->body_built = false_v;
    return;
  }
  /* Holes pad the grid past the window's last row and column. */
  for (uint64_t i = 0; i < (uint64_t)padded * padded; ++i) {
    heights[i] = VKR_PHYSICS_HEIGHT_HOLE;
  }
  vkr_heightfield_read_metres(field, rect, heights, padded);
  const Vec2 corner = vec2_new((float32_t)rect.x0 * field->spacing,
                               (float32_t)rect.z0 * field->spacing);
  const bool8_t ok =
      terrain_body_set(scene, record, heights, padded, position, corner, now);
  if (!record->body_build) {
    record->body_built = ok;
    record->collision_rejected = !ok;
  }
}

/* The fine tiles within VKR_SCENE_TERRAIN_BODY_TILES of the tile under each
   source, as one rectangle; false when no source is over the terrain. */
static bool8_t terrain_body_window(const TerrainRecord *record,
                                   const Vec2 *sources, uint32_t count,
                                   VkrHeightfieldRect *out) {
  const int32_t tiles = (int32_t)terrain_tiles(record);
  const float32_t size =
      (float32_t)VKR_HEIGHTFIELD_TILE_CELLS * record->field.spacing;
  const int32_t reach = (int32_t)VKR_SCENE_TERRAIN_BODY_TILES;
  bool8_t any = false_v;
  VkrHeightfieldRect first = {0};
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t tx = (int32_t)floorf(sources[i].x / size);
    const int32_t tz = (int32_t)floorf(sources[i].y / size);
    if (tx < -reach || tz < -reach || tx >= tiles + reach ||
        tz >= tiles + reach) {
      continue;
    }
    const VkrHeightfieldRect rect = {.x0 = (uint32_t)Max(tx - reach, 0),
                                     .z0 = (uint32_t)Max(tz - reach, 0),
                                     .x1 = (uint32_t)Min(tx + reach, tiles - 1),
                                     .z1 =
                                         (uint32_t)Min(tz + reach, tiles - 1)};
    if (!any) {
      first = rect;
      *out = rect;
      any = true_v;
      continue;
    }
    out->x0 = Min(out->x0, rect.x0);
    out->z0 = Min(out->z0, rect.z0);
    out->x1 = Max(out->x1, rect.x1);
    out->z1 = Max(out->z1, rect.z1);
  }
  if (any && (out->x1 - out->x0 >= TERRAIN_BODY_TILES_MAX ||
              out->z1 - out->z0 >= TERRAIN_BODY_TILES_MAX)) {
    *out = first;
  }
  return any;
}

/* Whether every source over the terrain stands on the body's tiles, or with
   `inner` at least a tile inside its edge where the terrain continues. */
static bool8_t terrain_body_covers(const TerrainRecord *record,
                                   const Vec2 *sources, uint32_t count,
                                   float32_t size, bool8_t inner) {
  const int32_t tiles = (int32_t)terrain_tiles(record);
  const VkrHeightfieldRect body = record->body_tiles;
  const int32_t margin = inner ? 1 : 0;
  const int32_t x0 = (int32_t)body.x0 + (body.x0 > 0u ? margin : 0);
  const int32_t z0 = (int32_t)body.z0 + (body.z0 > 0u ? margin : 0);
  const int32_t x1 =
      (int32_t)body.x1 - ((int32_t)body.x1 < tiles - 1 ? margin : 0);
  const int32_t z1 =
      (int32_t)body.z1 - ((int32_t)body.z1 < tiles - 1 ? margin : 0);
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t tx = (int32_t)floorf(sources[i].x / size);
    const int32_t tz = (int32_t)floorf(sources[i].y / size);
    const bool8_t over = tx >= 0 && tz >= 0 && tx < tiles && tz < tiles;
    if (over && (tx < x0 || tx > x1 || tz < z0 || tz > z1)) {
      return false_v;
    }
  }
  return true_v;
}

/* Metres from the nearest source to the square of `size` metres at corner
   (x, z); infinite without sources. */
static float32_t terrain_source_distance(const Vec2 *sources, uint32_t count,
                                         float32_t x, float32_t z,
                                         float32_t size) {
  float32_t best = INFINITY;
  for (uint32_t i = 0; i < count; ++i) {
    const float32_t dx =
        Max(0.0f, Max(x - sources[i].x, sources[i].x - (x + size)));
    const float32_t dz =
        Max(0.0f, Max(z - sources[i].y, sources[i].y - (z + size)));
    best = Min(best, sqrtf(dx * dx + dz * dz));
  }
  return best;
}

typedef struct TerrainCandidate {
  float32_t distance;
  uint32_t index;
} TerrainCandidate;

/* The missing fine tiles within `radius` of a source, at most `capacity`
   of them and the nearest first. Each tile of the sources' window counts
   once, however many sources reach it. */
static uint32_t terrain_candidates(const TerrainRecord *record,
                                   const Vec2 *sources, uint32_t count,
                                   float32_t radius, TerrainCandidate *out,
                                   uint32_t capacity) {
  const int32_t tiles = (int32_t)terrain_tiles(record);
  const float32_t size =
      (float32_t)VKR_HEIGHTFIELD_TILE_CELLS * record->field.spacing;
  int32_t x0 = tiles;
  int32_t z0 = tiles;
  int32_t x1 = -1;
  int32_t z1 = -1;
  for (uint32_t s = 0; s < count; ++s) {
    x0 = Min(x0, (int32_t)floorf((sources[s].x - radius) / size));
    z0 = Min(z0, (int32_t)floorf((sources[s].y - radius) / size));
    x1 = Max(x1, (int32_t)floorf((sources[s].x + radius) / size));
    z1 = Max(z1, (int32_t)floorf((sources[s].y + radius) / size));
  }
  uint32_t found = 0u;
  for (int32_t tz = Max(z0, 0); tz <= Min(z1, tiles - 1); ++tz) {
    for (int32_t tx = Max(x0, 0); tx <= Min(x1, tiles - 1); ++tx) {
      const uint32_t index = (uint32_t)tz * (uint32_t)tiles + (uint32_t)tx;
      if (record->tiles[index].id) {
        continue;
      }
      const float32_t distance = terrain_source_distance(
          sources, count, (float32_t)tx * size, (float32_t)tz * size, size);
      if (distance > radius ||
          (found == capacity && distance >= out[found - 1u].distance)) {
        continue;
      }
      /* Insertion into the sorted list, dropping its farthest when full. */
      uint32_t at = found < capacity ? found++ : found - 1u;
      for (; at > 0u && out[at - 1u].distance > distance; --at) {
        out[at] = out[at - 1u];
      }
      out[at] = (TerrainCandidate){distance, index};
    }
  }
  return found;
}

/* Keeps a streamed terrain's fine tiles, samples and bodies to the window
   around the scene's streaming sources, loading the nearest missing tiles
   within the frame budget. True when the tiles that draw changed. */
static bool8_t terrain_stream(VkrScene *scene, TerrainRecord *record,
                              Vec3 position, float64_t start) {
  VkrHeightfield *field = &record->field;
  const uint32_t tiles = terrain_tiles(record);
  const float32_t size = (float32_t)VKR_HEIGHTFIELD_TILE_CELLS * field->spacing;
  const float32_t half = vkr_heightfield_half_size(field);
  const float32_t radius = record->stream_radius;
  /* Tiles stay a tile past the radius they load in, so a source moving
     along a tile edge does not churn them. */
  const float32_t keep = radius + size;
  Vec2 sources[VKR_SCENE_STREAM_SOURCES_MAX];
  const uint32_t count = scene->stream_source_count;
  for (uint32_t i = 0; i < count; ++i) {
    sources[i] = vec2_new(scene->stream_sources[i].x - position.x + half,
                          scene->stream_sources[i].z - position.z + half);
  }
  if (record->settle < UINT8_MAX) {
    record->settle++;
  }
  bool8_t changed = false_v;
  /* The tiles that draw hold still while a rebuilt mesh is built and waits
     to show them. */
  const bool8_t frozen = record->building || record->attach_pending;

  /* Detail that left the window returns to the overview. */
  for (uint32_t index = 0; !frozen && index < tiles * tiles; ++index) {
    if (record->tiles[index].id &&
        terrain_source_distance(
            sources, count, (float32_t)(index % tiles) * size,
            (float32_t)(index / tiles) * size, size) > keep) {
      terrain_geometry_release(scene, &record->tiles[index]);
      record->overview_dirty |= 1ull << terrain_overview_of(record, index);
      terrain_change_tile(record, index % tiles, index / tiles);
      changed = true_v;
    }
  }
  /* Samples no kept tile reads leave memory, unless they wait for a
     save. */
  const uint32_t storage = field->tiles_per_side;
  for (uint32_t index = 0; index < storage * storage; ++index) {
    if (field->tiles[index] && !field->tile_dirty[index] &&
        terrain_source_distance(
            sources, count, (float32_t)(index % storage) * size,
            (float32_t)(index / storage) * size, size) > keep + size) {
      (void)vkr_heightfield_tile_release(field, index % storage,
                                         index / storage);
    }
  }

  /* The nearest missing tiles load first, within the frame budget. */
  TerrainCandidate candidates[TERRAIN_STREAM_CANDIDATES];
  const float64_t budget = VKR_SCENE_TERRAIN_STREAM_BUDGET_MS / 1000.0;
  const uint32_t found =
      frozen ? 0u
             : terrain_candidates(record, sources, count, radius, candidates,
                                  ArrayCount(candidates));
  /* The nearest load as one batch, their samples read first, unless a
     batch is still building. */
  for (uint32_t i = 0; i < found && !record->batch.active; ++i) {
    const uint32_t index = candidates[i].index;
    if (record->tiles[index].id) {
      continue;
    }
    if (vkr_platform_get_absolute_time() - start > budget) {
      break;
    }
    /* The tile's samples and the neighbours its normals read. */
    const uint32_t x0 = index % tiles * VKR_HEIGHTFIELD_TILE_CELLS;
    const uint32_t z0 = index / tiles * VKR_HEIGHTFIELD_TILE_CELLS;
    const VkrHeightfieldRect reach = {
        .x0 = x0 ? x0 - 1u : 0u,
        .z0 = z0 ? z0 - 1u : 0u,
        .x1 = Min(x0 + VKR_HEIGHTFIELD_TILE_CELLS + 1u, field->cells),
        .z1 = Min(z0 + VKR_HEIGHTFIELD_TILE_CELLS + 1u, field->cells)};
    if (!vkr_heightfield_load_rect(field, reach) ||
        !terrain_batch_add(scene, record, index % tiles, index / tiles, false_v,
                           NULL)) {
      snprintf(record->status, sizeof(record->status),
               "A terrain tile could not be streamed in");
      break;
    }
    if (record->batch.count == TERRAIN_BATCH_MAX) {
      break;
    }
  }
  if (!record->batch.active && record->batch.count) {
    terrain_batch_begin(scene, record, TERRAIN_BATCH_LOADS);
  }

  /* The body covers the ground around the sources. It moves once a
     source nears its edge, and takes edits to it once edits rest. Unless a
     source has left it, a rebuild waits for a frame with budget left and
     builds on a physics worker. A pending build counts as the body. */
  VkrHeightfieldRect window = {0};
  const bool8_t wanted = terrain_body_window(record, sources, count, &window);
  const bool8_t moved =
      MemCompare(&position, &record->built_position, sizeof(Vec3)) != 0;
  const bool8_t settled = record->settle >= VKR_SCENE_TERRAIN_COLLISION_SETTLE;
  const bool8_t shifted =
      MemCompare(&window, &record->body_tiles, sizeof(window)) != 0;
  /* A source off the body's tiles needs it now; one near its edge, or
     edits once they rest, can wait for budget. */
  const bool8_t urgent =
      (!record->body_built && !record->body_build) || moved ||
      (shifted && !terrain_body_covers(record, sources, count, size, false_v));
  const bool8_t due =
      urgent || (record->body_edited && settled) ||
      (shifted && !terrain_body_covers(record, sources, count, size, true_v));
  if (!wanted && (record->body_built || record->body_build)) {
    terrain_body_cancel(scene, record);
    vkr_scene_physics_generated_remove(scene,
                                       terrain_collision_key(record->entity));
    record->body_built = false_v;
  } else if (wanted && !record->collision_rejected && due &&
             (urgent || vkr_platform_get_absolute_time() - start <= budget)) {
    terrain_build_window_collision(scene, record, window, position, urgent);
  }
  record->collision_dirty = record->body_built && record->body_edited;
  return changed;
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
    const Vec3 position = mat4_position(transform->world);
    terrain_body_poll(scene, record);
    /* Tiles the job workers finished replace those they rebuilt. */
    terrain_batch_poll(scene, record);
    /* Streaming and the body share one frame budget. */
    const float64_t start = vkr_platform_get_absolute_time();
    if (record->streamed && terrain_stream(scene, record, position, start)) {
      record->mesh_dirty = true_v;
    }
    /* Changed tiles build a batch at a time and show together once
       uploaded; changes meanwhile wait for the next build. */
    if (record->mesh_dirty && !record->building && !record->attach_pending &&
        !record->batch.active) {
      record->mesh_dirty = false_v;
      if (!terrain_begin_build(scene, record)) {
        snprintf(record->status, sizeof(record->status),
                 "The terrain mesh failed");
      }
    }
    if (record->building && !record->batch.active) {
      bool8_t done = false_v;
      if (!terrain_build_step(scene, record, &done)) {
        snprintf(record->status, sizeof(record->status),
                 "The terrain mesh failed");
        done = true_v;
      }
      record->building = !done;
      record->attach_pending = done;
    }
    if (record->attach_pending && terrain_mesh_settled(scene, record) &&
        !terrain_attach_mesh(scene, record)) {
      snprintf(record->status, sizeof(record->status),
               "The terrain mesh failed");
    }
    /* A resident terrain builds again only after edits; its batch storage
       goes until then. */
    if (!record->streamed && !record->building && !record->batch.active &&
        record->batch.works) {
      terrain_batch_release(scene, record);
    }
    if (record->streamed) {
      continue;
    }
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
