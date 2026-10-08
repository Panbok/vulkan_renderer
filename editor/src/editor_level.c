#include "editor_level.h"

#include "editor_agent.h"
#include "editor_brush_grid.h"
#include "editor_internal.h"

#include "level/vkr_brush.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_io_router.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Floors one grid cell holds at most. */
#define LEVEL_LAYER_MAX 4u
/* Offsets of each cell's rays from its center, in meters: small, and off
   the 1/16 m grid. */
#define LEVEL_SAMPLE_SKEW_X 0.0131f
#define LEVEL_SAMPLE_SKEW_Z 0.0073f
/* A gap the walk steps over is at most this share of the capsule's width. */
#define LEVEL_GAP_SHARE 0.75f
/* How far a column's next ray starts below one that started inside a solid,
   so the floor under a slab thinner than the capsule is still found. */
#define LEVEL_INSIDE_STEP 0.25f
/* Issues of one kind closer than this merge into one. */
#define LEVEL_MERGE_DISTANCE 1.0f

typedef enum LevelState {
  LEVEL_NONE = 0,
  LEVEL_WALKABLE,
  LEVEL_STEEP,
  LEVEL_LOW,
  LEVEL_NARROW,
  /* Too close to a wall for the capsule's center; normal beside walls. */
  LEVEL_WALL,
  /* Only a crouched capsule fits; passable crouched. */
  LEVEL_CROUCH,
  /* Floor narrower than the capsule's radius, as a rail, fence or wall top
     the capsule only balances on; not passable and not an issue. */
  LEVEL_THIN,
} LevelState;

typedef struct LevelNode {
  Vec3 position;
  VkrEntityId entity;
  /* What blocks a low or narrow node: the ceiling or a wall. */
  VkrEntityId blocker;
  float32_t normal_y;
  /* Headroom up to a ceiling for a low node; gap width for a narrow one. */
  float32_t headroom;
  int32_t parent;
  uint8_t state;
  bool8_t visited;
  /* The walk reached it up or down a ladder from its parent. */
  bool8_t ladder;
} LevelNode;

typedef struct LevelGrid {
  VkrScene *scene;
  const VkrEditorLevelCapsule *capsule;
  Vec3 min;
  Vec3 max;
  float32_t cell;
  uint32_t nx;
  uint32_t nz;
  LevelNode *nodes;
  uint8_t *layers;
  /* Node index pairs a ladder joins, both ways (level_ladders). */
  int32_t *links;
  uint32_t link_count;
  /* The walk keeps to floor a standing capsule fits. */
  bool8_t standing_only;
} LevelGrid;

VkrEditorLevelCapsule vkr_editor_level_capsule_default(void) {
  const VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  return (VkrEditorLevelCapsule){
      .radius = desc.radius,
      .height = 2.0f * (desc.half_height + desc.radius),
      .crouch_height =
          2.0f *
          (desc.half_height * VKR_PHYSICS_CROUCH_HEIGHT_SHARE + desc.radius),
      .step_up = desc.step_up,
      .max_slope_radians = desc.max_slope_radians,
  };
}

const char *vkr_editor_level_issue_name(VkrEditorLevelIssueKind kind) {
  static const char *const names[VKR_EDITOR_LEVEL_ISSUE_COUNT] = {
      "step_too_high",     "too_steep",   "low_ceiling", "too_narrow",
      "void_edge",         "unreachable", "overlap",     "invalid_brush",
      "broken_connection", "crouch_only", "z_fight",     "mover_timing"};
  return kind < VKR_EDITOR_LEVEL_ISSUE_COUNT ? names[kind] : "unknown";
}

// =============================================================================
// Sampling
// =============================================================================

static bool8_t level_ray(VkrScene *scene, Vec3 origin, Vec3 displacement,
                         VkrPhysicsRayHit *hit) {
  const VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX};
  return vkr_scene_physics_raycast_query(scene, origin, displacement, &filter,
                                         hit);
}

/* Whether a sphere touches solid collision; sensors and trigger brushes do
   not count. */
static bool8_t level_blocked(VkrScene *scene, Vec3 center, float32_t radius) {
  VkrPhysicsOverlapHit hits[16];
  uint32_t count = 0u;
  if (!vkr_scene_physics_overlap_sphere(scene, center, radius, UINT16_MAX, hits,
                                        ArrayCount(hits), &count)) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkrEntityId entity = {.u64 = hits[i].collider_entity_id
                                           ? hits[i].collider_entity_id
                                           : hits[i].entity_id};
    const SceneBrushSettings *brush =
        vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type);
    if (brush && brush->role == SCENE_BRUSH_ROLE_TRIGGER) {
      continue;
    }
    VkrScenePhysicsSnapshot body;
    if (!brush && vkr_scene_physics_read(scene, entity, &body) &&
        body.present && body.body.sensor) {
      continue;
    }
    return true_v;
  }
  return false_v;
}

static LevelNode *level_node(LevelGrid *grid, uint32_t x, uint32_t z,
                             uint32_t layer) {
  return &grid->nodes[((size_t)z * grid->nx + x) * LEVEL_LAYER_MAX + layer];
}

/* Whether floor within a step of `point` lies half the capsule's radius to
   one side of it along x and along z: a rail or wall top narrower than the
   radius only balances the capsule. A probe starting inside a solid counts,
   as beside a wall. */
static bool8_t level_supported(LevelGrid *grid, Vec3 point) {
  const float32_t reach = grid->capsule->radius * 0.5f;
  const float32_t step = grid->capsule->step_up + 0.05f;
  const Vec3 down = vec3_new(0.0f, -2.0f * step, 0.0f);
  const Vec3 sides[2][2] = {
      {{reach, step, 0.0f, 0.0f}, {-reach, step, 0.0f, 0.0f}},
      {{0.0f, step, reach, 0.0f}, {0.0f, step, -reach, 0.0f}}};
  for (uint32_t axis = 0; axis < 2u; ++axis) {
    VkrPhysicsRayHit hit = {0};
    if (!level_ray(grid->scene, vec3_add(point, sides[axis][0]), down, &hit) &&
        !level_ray(grid->scene, vec3_add(point, sides[axis][1]), down, &hit)) {
      return false_v;
    }
  }
  return true_v;
}

/* Every floor under one cell's center, top to bottom, classified for the
   capsule. */
static void level_sample_cell(LevelGrid *grid, uint32_t x, uint32_t z) {
  const VkrEditorLevelCapsule *capsule = grid->capsule;
  /* A ray exactly along an edge two collision pieces share can slip between
     their triangles, as down a stair riser of a blockout shape, and report
     the floor under the solid. Geometry snaps to 1/16 m, so each column is
     cast a little off its cell's center, off that grid. */
  const float32_t px =
      grid->min.x + ((float32_t)x + 0.5f) * grid->cell + LEVEL_SAMPLE_SKEW_X;
  const float32_t pz =
      grid->min.z + ((float32_t)z + 0.5f) * grid->cell + LEVEL_SAMPLE_SKEW_Z;
  const float32_t depth = grid->max.y - grid->min.y + 1.0f;
  uint8_t *count = &grid->layers[(size_t)z * grid->nx + x];
  for (float32_t start = grid->max.y + 0.5f;
       start > grid->min.y && *count < LEVEL_LAYER_MAX;
       start -= capsule->height) {
    VkrPhysicsRayHit hit = {0};
    if (!level_ray(grid->scene, vec3_new(px, start, pz),
                   vec3_new(0.0f, -depth, 0.0f), &hit)) {
      continue;
    }
    if (hit.fraction <= 0.0f) {
      /* The ray started inside a solid: the next starts a little lower. */
      start += capsule->height - LEVEL_INSIDE_STEP;
      continue;
    }
    const Vec3 point =
        vec3_new(hit.position[0], hit.position[1], hit.position[2]);
    bool8_t known = false_v;
    for (uint32_t i = 0; i < *count && !known; ++i) {
      known = fabsf(level_node(grid, x, z, i)->position.y - point.y) < 0.05f;
    }
    if (known || point.y < grid->min.y) {
      continue;
    }
    LevelNode *node = level_node(grid, x, z, (*count)++);
    *node = (LevelNode){.position = point,
                        .entity = {.u64 = hit.collider_entity_id
                                              ? hit.collider_entity_id
                                              : hit.entity_id},
                        .normal_y = hit.normal[1],
                        .parent = -1};
    if (hit.normal[1] < cosf(capsule->max_slope_radians)) {
      node->state = LEVEL_STEEP;
      continue;
    }
    if (!level_supported(grid, point)) {
      node->state = LEVEL_THIN;
      continue;
    }
    /* The capsule fits when spheres just above the step height, at its
       middle and at its top touch nothing: furniture such as a chair seat
       lies between what a step climbs and the capsule's middle. */
    const float32_t r = capsule->radius * 0.9f;
    const Vec3 low =
        vec3_add(point, vec3_new(0.0f, capsule->step_up + r + 0.02f, 0.0f));
    const bool8_t low_clear = !level_blocked(grid->scene, low, r);
    if (low_clear &&
        !level_blocked(
            grid->scene,
            vec3_add(point, vec3_new(0.0f, capsule->height * 0.5f, 0.0f)), r) &&
        !level_blocked(
            grid->scene,
            vec3_add(point,
                     vec3_new(0.0f, capsule->height - capsule->radius, 0.0f)),
            r)) {
      node->state = LEVEL_WALKABLE;
      continue;
    }
    /* Something too close: a ceiling straight above, walls on opposite
       sides closer than the capsule is wide, or only a nearby wall. What
       blocks only the standing capsule's upper part lets a crouched one
       pass, under a ceiling or beside the edge of one. */
    const float32_t crouch = capsule->crouch_height;
    const bool8_t crouch_fits =
        crouch > 2.0f * capsule->radius && crouch < capsule->height &&
        low_clear &&
        !level_blocked(grid->scene,
                       vec3_add(point, vec3_new(0.0f, crouch * 0.5f, 0.0f)),
                       r) &&
        !level_blocked(
            grid->scene,
            vec3_add(point, vec3_new(0.0f, crouch - capsule->radius, 0.0f)), r);
    VkrPhysicsRayHit up = {0};
    const bool8_t ceiling =
        level_ray(grid->scene, vec3_add(point, vec3_new(0.0f, 0.05f, 0.0f)),
                  vec3_new(0.0f, capsule->height, 0.0f), &up) &&
        up.fraction > 0.0f;
    if (crouch_fits) {
      node->state = LEVEL_CROUCH;
      node->headroom =
          ceiling ? up.fraction * capsule->height + 0.05f : capsule->height;
      node->blocker = (VkrEntityId){
          .u64 = up.collider_entity_id ? up.collider_entity_id : up.entity_id};
      continue;
    }
    if (ceiling) {
      node->state = LEVEL_LOW;
      node->headroom = up.fraction * capsule->height + 0.05f;
      node->blocker = (VkrEntityId){
          .u64 = up.collider_entity_id ? up.collider_entity_id : up.entity_id};
      continue;
    }
    const float32_t reach = 2.0f * capsule->radius;
    const Vec3 waist =
        vec3_add(point, vec3_new(0.0f, capsule->height * 0.5f, 0.0f));
    const Vec3 directions[4] = {{reach, 0.0f, 0.0f, 0.0f},
                                {-reach, 0.0f, 0.0f, 0.0f},
                                {0.0f, 0.0f, reach, 0.0f},
                                {0.0f, 0.0f, -reach, 0.0f}};
    float32_t free[4];
    for (uint32_t d = 0; d < 4u; ++d) {
      VkrPhysicsRayHit side = {0};
      free[d] = reach;
      if (level_ray(grid->scene, waist, directions[d], &side)) {
        free[d] = side.fraction * reach;
        node->blocker = (VkrEntityId){.u64 = side.collider_entity_id
                                                 ? side.collider_entity_id
                                                 : side.entity_id};
      }
    }
    const float32_t gap = Min(free[0] + free[1], free[2] + free[3]);
    node->state = gap < reach ? LEVEL_NARROW : LEVEL_WALL;
    node->headroom = gap;
  }
}

/* The cell edge of a grid over [min, max]: at least `cell` and the capsule
   radius, and large enough that the grid stays within
   VKR_EDITOR_LEVEL_CELL_MAX cells. Writes the cells along x and z. */
static float32_t level_grid_size(Vec3 min, Vec3 max,
                                 const VkrEditorLevelCapsule *capsule,
                                 float32_t cell, uint32_t *out_nx,
                                 uint32_t *out_nz) {
  const float32_t width = Max(max.x - min.x, 0.01f);
  const float32_t depth = Max(max.z - min.z, 0.01f);
  const float32_t edge =
      Max(Max(capsule->radius, cell),
          sqrtf(width * depth / (float32_t)VKR_EDITOR_LEVEL_CELL_MAX));
  *out_nx = (uint32_t)Max(1.0f, ceilf(width / edge));
  *out_nz = (uint32_t)Max(1.0f, ceilf(depth / edge));
  return edge;
}

/* A grid over [min, max] with cells of at least `cell` (zero for none) and
   the capsule radius, its cells not sampled yet. */
static bool8_t level_grid_alloc(LevelGrid *grid, Vec3 min, Vec3 max,
                                const VkrEditorLevelCapsule *capsule,
                                float32_t cell) {
  *grid = (LevelGrid){.capsule = capsule, .min = min, .max = max};
  grid->cell = level_grid_size(min, max, capsule, cell, &grid->nx, &grid->nz);
  const size_t cells = (size_t)grid->nx * grid->nz;
  grid->nodes = calloc(cells * LEVEL_LAYER_MAX, sizeof(*grid->nodes));
  grid->layers = calloc(cells, sizeof(*grid->layers));
  if (!grid->nodes || !grid->layers) {
    free(grid->nodes);
    free(grid->layers);
    return false_v;
  }
  return true_v;
}

static void level_grid_free(LevelGrid *grid) {
  free(grid->nodes);
  free(grid->layers);
}

/* A check's grid and how far its sampling came. The job owns the grid's
   arrays and its own copy of the capsule. */
struct VkrEditorLevelJob {
  LevelGrid grid;
  VkrEditorLevelCapsule capsule;
  uint32_t next_cell;
};

VkrEditorLevelJob *vkr_editor_level_job_begin(
    Vec3 min, Vec3 max, const VkrEditorLevelCapsule *capsule, float32_t cell) {
  VkrEditorLevelJob *job = calloc(1u, sizeof(*job));
  if (!job) {
    return NULL;
  }
  job->capsule = *capsule;
  if (!level_grid_alloc(&job->grid, min, max, &job->capsule, cell)) {
    free(job);
    return NULL;
  }
  return job;
}

bool8_t vkr_editor_level_job_step(VkrEditorLevelJob *job, const VkrScene *scene,
                                  float64_t seconds) {
  LevelGrid *grid = &job->grid;
  /* Physics queries take a mutable scene but change none of its state. */
  grid->scene = (VkrScene *)scene;
  const uint32_t cells = grid->nx * grid->nz;
  const float64_t start = vkr_platform_get_absolute_time();
  while (job->next_cell < cells) {
    level_sample_cell(grid, job->next_cell % grid->nx,
                      job->next_cell / grid->nx);
    job->next_cell++;
    /* The clock is read every 32 cells, a fraction of a millisecond. */
    if ((job->next_cell & 31u) == 0u &&
        vkr_platform_get_absolute_time() - start >= seconds) {
      break;
    }
  }
  return job->next_cell >= cells;
}

float32_t vkr_editor_level_job_progress(const VkrEditorLevelJob *job) {
  const uint32_t cells = job->grid.nx * job->grid.nz;
  return cells ? (float32_t)job->next_cell / (float32_t)cells : 1.0f;
}

void vkr_editor_level_job_end(VkrEditorLevelJob *job) {
  if (!job) {
    return;
  }
  level_grid_free(&job->grid);
  free(job);
}

/* Whether a capsule stands or crouches on `node`. */
static bool8_t level_passable(const LevelNode *node) {
  return node->state == LEVEL_WALKABLE || node->state == LEVEL_CROUCH;
}

/* Whether a capsule walks from node `a` to node `b` of the next cell. */
static bool8_t level_step(LevelGrid *grid, const LevelNode *a,
                          const LevelNode *b) {
  if (!level_passable(a) || !level_passable(b) ||
      (grid->standing_only &&
       (a->state != LEVEL_WALKABLE || b->state != LEVEL_WALKABLE))) {
    return false_v;
  }
  const float32_t rise = b->position.y - a->position.y;
  if (rise > grid->capsule->step_up || rise < -VKR_EDITOR_LEVEL_DROP_MAX) {
    return false_v;
  }
  /* Nothing may stand between them above the step height. */
  const float32_t knee =
      Max(a->position.y, b->position.y) + grid->capsule->step_up + 0.05f;
  const Vec3 from = vec3_new(a->position.x, knee, a->position.z);
  const Vec3 to = vec3_new(b->position.x, knee, b->position.z);
  VkrPhysicsRayHit hit = {0};
  if (level_ray(grid->scene, from, vec3_sub(to, from), &hit)) {
    return false_v;
  }
  /* A drop falls clear to the lower floor: not through the slab under the
     higher one, as from a roof down to a catwalk under it. */
  const float32_t drop =
      Max(a->position.y, b->position.y) - Min(a->position.y, b->position.y);
  if (drop > grid->capsule->step_up) {
    const LevelNode *lower = a->position.y < b->position.y ? a : b;
    const LevelNode *higher = lower == a ? b : a;
    /* The capsule walks off the higher floor at its own height, so its top
       must fit over the lower one: not into a ceiling's edge, as from a
       stair's steps out through the hole it climbs into. */
    const float32_t r = grid->capsule->radius * 0.9f;
    const Vec3 head = vec3_new(lower->position.x,
                               higher->position.y + grid->capsule->height -
                                   grid->capsule->radius,
                               lower->position.z);
    if (level_blocked(grid->scene, head, r)) {
      return false_v;
    }
    const Vec3 top = vec3_new(lower->position.x, knee, lower->position.z);
    return !level_ray(grid->scene, top,
                      vec3_new(0.0f, lower->position.y + 0.1f - knee, 0.0f),
                      &hit);
  }
  return true_v;
}

static const int32_t s_level_dx[4] = {1, -1, 0, 0};
static const int32_t s_level_dz[4] = {0, 0, 1, -1};

/* Breadth-first walk from node `start`; marks visited and parents. Returns
   how many nodes it reached. */
static uint32_t level_walk(LevelGrid *grid, int32_t start, int32_t *queue) {
  const size_t total = (size_t)grid->nx * grid->nz * LEVEL_LAYER_MAX;
  /* The capsule's round bottom rests on both edges of a gap narrower than
     itself, so the walk also steps over one cell when that cell is that
     narrow; a wider cell is a real gap. */
  const uint32_t reach =
      grid->cell <= LEVEL_GAP_SHARE * 2.0f * grid->capsule->radius ? 2u : 1u;
  uint32_t head = 0u;
  uint32_t tail = 0u;
  grid->nodes[start].visited = true_v;
  queue[tail++] = start;
  while (head < tail) {
    const int32_t index = queue[head++];
    const uint32_t cell = (uint32_t)index / LEVEL_LAYER_MAX;
    const uint32_t x = cell % grid->nx;
    const uint32_t z = cell / grid->nx;
    for (uint32_t d = 0; d < 4u; ++d) {
      for (uint32_t k = 1u; k <= reach; ++k) {
        const int32_t nx = (int32_t)x + s_level_dx[d] * (int32_t)k;
        const int32_t nz = (int32_t)z + s_level_dz[d] * (int32_t)k;
        if (nx < 0 || nz < 0 || nx >= (int32_t)grid->nx ||
            nz >= (int32_t)grid->nz) {
          break;
        }
        const uint8_t layers = grid->layers[(size_t)nz * grid->nx + nx];
        for (uint32_t l = 0; l < layers; ++l) {
          LevelNode *next = level_node(grid, (uint32_t)nx, (uint32_t)nz, l);
          const int32_t next_index = (int32_t)(next - grid->nodes);
          if (!next->visited && (size_t)next_index < total &&
              level_step(grid, &grid->nodes[index], next)) {
            next->visited = true_v;
            next->parent = index;
            queue[tail++] = next_index;
          }
        }
      }
    }
    /* A ladder joins its foot and its top either way. */
    for (uint32_t i = 0; i < grid->link_count; ++i) {
      const int32_t *link = &grid->links[2u * i];
      const int32_t other = link[0] == index   ? link[1]
                            : link[1] == index ? link[0]
                                               : -1;
      if (other >= 0 && !grid->nodes[other].visited) {
        grid->nodes[other].visited = true_v;
        grid->nodes[other].parent = index;
        grid->nodes[other].ladder = true_v;
        queue[tail++] = other;
      }
    }
  }
  return tail;
}

/* The passable node within `margin` of [lo, hi] in x and z, with y in
   [y_min, y_max], nearest its center in x and z, the highest first when
   `highest`; -1 when none. */
static int32_t level_ladder_end(const LevelGrid *grid, Vec3 lo, Vec3 hi,
                                float32_t margin, float32_t y_min,
                                float32_t y_max, bool8_t highest) {
  const int32_t x0 =
      (int32_t)floorf((lo.x - margin - grid->min.x) / grid->cell);
  const int32_t x1 =
      (int32_t)floorf((hi.x + margin - grid->min.x) / grid->cell);
  const int32_t z0 =
      (int32_t)floorf((lo.z - margin - grid->min.z) / grid->cell);
  const int32_t z1 =
      (int32_t)floorf((hi.z + margin - grid->min.z) / grid->cell);
  const Vec2 center = {(lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f};
  int32_t best = -1;
  float32_t best_y = 0.0f;
  float32_t best_distance = INFINITY;
  for (int32_t z = Max(z0, 0); z <= Min(z1, (int32_t)grid->nz - 1); ++z) {
    for (int32_t x = Max(x0, 0); x <= Min(x1, (int32_t)grid->nx - 1); ++x) {
      const uint8_t layers = grid->layers[(size_t)z * grid->nx + x];
      for (uint32_t l = 0; l < layers; ++l) {
        const LevelNode *node =
            &grid->nodes[((size_t)z * grid->nx + x) * LEVEL_LAYER_MAX + l];
        if (!level_passable(node) || node->position.y < y_min ||
            node->position.y > y_max) {
          continue;
        }
        const float32_t distance =
            hypotf(node->position.x - center.x, node->position.z - center.y);
        const bool8_t higher = highest && node->position.y > best_y + 0.05f;
        const bool8_t level = !highest || node->position.y > best_y - 0.05f;
        if (best < 0 || higher || (level && distance < best_distance)) {
          best = (int32_t)(node - grid->nodes);
          best_y = node->position.y;
          best_distance = distance;
        }
      }
    }
  }
  return best;
}

/* Joins the floor at each ladder's foot to the floor its top reaches: a
   trigger brush with the FPS module's `fps_ladder`, reaching above the
   floor it leads to. The caller frees `grid->links`. */
static void level_ladders(LevelGrid *grid) {
  grid->links = NULL;
  grid->link_count = 0u;
  static const String8 name = {(uint8_t *)"fps_ladder", 10u};
  const VkrTypeDesc *type = vkr_scene_world_type_named(name);
  VkrEntityId ladders[64];
  const uint32_t found = type ? vkr_scene_find_typed(grid->scene, type, ladders,
                                                     ArrayCount(ladders))
                              : 0u;
  const uint32_t count = Min(found, (uint32_t)ArrayCount(ladders));
  VkrBrushGeometry *scratch = count ? malloc(sizeof(*scratch)) : NULL;
  grid->links = scratch ? malloc(2u * count * sizeof(*grid->links)) : NULL;
  if (!grid->links) {
    free(scratch);
    return;
  }
  const float32_t margin = grid->capsule->radius + 1.0f;
  for (uint32_t i = 0; i < count; ++i) {
    Vec3 lo = vec3_zero();
    Vec3 hi = vec3_zero();
    if (!vkr_editor_entity_world_box(grid->scene, ladders[i], scratch, &lo,
                                     &hi)) {
      continue;
    }
    const int32_t foot = level_ladder_end(
        grid, lo, hi, margin, lo.y - grid->capsule->step_up - 0.1f,
        lo.y + grid->capsule->step_up + 0.1f, false_v);
    const int32_t top = level_ladder_end(grid, lo, hi, margin,
                                         lo.y + grid->capsule->step_up + 0.5f,
                                         hi.y + 0.1f, true_v);
    if (foot >= 0 && top >= 0) {
      grid->links[2u * grid->link_count] = foot;
      grid->links[2u * grid->link_count + 1u] = top;
      grid->link_count++;
    }
  }
  free(scratch);
}

/* The walkable node nearest `point`, or -1. */
static int32_t level_nearest(const LevelGrid *grid, Vec3 point) {
  int32_t best = -1;
  float32_t best_distance = 2.0f;
  const int32_t cx = (int32_t)floorf((point.x - grid->min.x) / grid->cell);
  const int32_t cz = (int32_t)floorf((point.z - grid->min.z) / grid->cell);
  for (int32_t z = cz - 3; z <= cz + 3; ++z) {
    for (int32_t x = cx - 3; x <= cx + 3; ++x) {
      if (x < 0 || z < 0 || x >= (int32_t)grid->nx || z >= (int32_t)grid->nz) {
        continue;
      }
      const uint8_t layers = grid->layers[(size_t)z * grid->nx + x];
      for (uint32_t l = 0; l < layers; ++l) {
        const LevelNode *node =
            &grid->nodes[((size_t)z * grid->nx + x) * LEVEL_LAYER_MAX + l];
        const float32_t distance = vec3_length(vec3_sub(node->position, point));
        if (level_passable(node) && distance < best_distance) {
          best = (int32_t)(node - grid->nodes);
          best_distance = distance;
        }
      }
    }
  }
  return best;
}

// =============================================================================
// Lint
// =============================================================================

typedef struct LevelIssues {
  VkrEditorLevelIssue *items;
  uint32_t count;
  uint32_t capacity;
  uint32_t found;
} LevelIssues;

/* Whether an issue of `kind` at `position` merges into one already kept. */
static bool8_t level_issue_merges(const LevelIssues *issues,
                                  VkrEditorLevelIssueKind kind, Vec3 position,
                                  VkrEntityId entity) {
  for (uint32_t i = 0; i < issues->count; ++i) {
    /* One object's defect along its edge reads as one issue every 8 m. */
    const bool8_t same =
        entity.u64 && issues->items[i].entity.u64 == entity.u64;
    const float32_t merge = same ? 8.0f * LEVEL_MERGE_DISTANCE
                            : kind == VKR_EDITOR_LEVEL_VOID_EDGE
                                ? 2.0f * LEVEL_MERGE_DISTANCE
                                : LEVEL_MERGE_DISTANCE;
    if (issues->items[i].kind == kind &&
        vec3_length(vec3_sub(issues->items[i].position, position)) < merge) {
      return true_v;
    }
  }
  return false_v;
}

static void level_issue(LevelIssues *issues, VkrEditorLevelIssueKind kind,
                        Vec3 position, VkrEntityId entity, float32_t value) {
  if (level_issue_merges(issues, kind, position, entity)) {
    return;
  }
  issues->found++;
  if (issues->count < issues->capacity) {
    issues->items[issues->count++] = (VkrEditorLevelIssue){
        .kind = kind, .position = position, .entity = entity, .value = value};
  }
}

/* An issue of its own, never merged with a neighbour. */
static void level_issue_pair(LevelIssues *issues, VkrEditorLevelIssueKind kind,
                             Vec3 position, VkrEntityId entity,
                             VkrEntityId other, float32_t value) {
  issues->found++;
  if (issues->count < issues->capacity) {
    issues->items[issues->count++] = (VkrEditorLevelIssue){.kind = kind,
                                                           .position = position,
                                                           .entity = entity,
                                                           .other = other,
                                                           .value = value};
  }
}

/* Whether `point` lies in the region the grid covers. */
static bool8_t level_in_region(const LevelGrid *grid, Vec3 point) {
  return point.x >= grid->min.x && point.y >= grid->min.y &&
         point.z >= grid->min.z && point.x <= grid->max.x &&
         point.y <= grid->max.y && point.z <= grid->max.z;
}

/* Whether walkable floor lies within two cells of node (x, z, layer), at
   about its height. */
static bool8_t level_near_walkable(LevelGrid *grid, uint32_t x, uint32_t z,
                                   const LevelNode *node) {
  for (int32_t dz = -2; dz <= 2; ++dz) {
    for (int32_t dx = -2; dx <= 2; ++dx) {
      const int32_t nx = (int32_t)x + dx;
      const int32_t nz = (int32_t)z + dz;
      if (nx < 0 || nz < 0 || nx >= (int32_t)grid->nx ||
          nz >= (int32_t)grid->nz) {
        continue;
      }
      const uint8_t layers = grid->layers[(size_t)nz * grid->nx + nx];
      for (uint32_t l = 0; l < layers; ++l) {
        const LevelNode *other =
            level_node(grid, (uint32_t)nx, (uint32_t)nz, l);
        if (other->state == LEVEL_WALKABLE &&
            fabsf(other->position.y - node->position.y) <=
                grid->capsule->step_up) {
          return true_v;
        }
      }
    }
  }
  return false_v;
}

/* Floor problems next to walkable floor. */
static void level_lint_floor(LevelGrid *grid, LevelIssues *issues) {
  /* Low ceilings and narrow gaps where the capsule could otherwise walk. */
  for (uint32_t z = 0; z < grid->nz; ++z) {
    for (uint32_t x = 0; x < grid->nx; ++x) {
      const uint8_t layers = grid->layers[(size_t)z * grid->nx + x];
      for (uint32_t l = 0; l < layers; ++l) {
        const LevelNode *node = level_node(grid, x, z, l);
        if ((node->state == LEVEL_LOW || node->state == LEVEL_NARROW ||
             node->state == LEVEL_CROUCH) &&
            level_near_walkable(grid, x, z, node)) {
          level_issue(
              issues,
              node->state == LEVEL_LOW      ? VKR_EDITOR_LEVEL_LOW_CEILING
              : node->state == LEVEL_CROUCH ? VKR_EDITOR_LEVEL_CROUCH_ONLY
                                            : VKR_EDITOR_LEVEL_TOO_NARROW,
              node->position, node->blocker.u64 ? node->blocker : node->entity,
              node->headroom);
        }
      }
    }
  }
  for (uint32_t z = 0; z < grid->nz; ++z) {
    for (uint32_t x = 0; x < grid->nx; ++x) {
      const uint8_t layers = grid->layers[(size_t)z * grid->nx + x];
      for (uint32_t l = 0; l < layers; ++l) {
        const LevelNode *a = level_node(grid, x, z, l);
        if (a->state != LEVEL_WALKABLE) {
          continue;
        }
        for (uint32_t d = 0; d < 4u; ++d) {
          const int32_t nx = (int32_t)x + s_level_dx[d];
          const int32_t nz = (int32_t)z + s_level_dz[d];
          if (nx < 0 || nz < 0 || nx >= (int32_t)grid->nx ||
              nz >= (int32_t)grid->nz) {
            continue;
          }
          const uint8_t next_layers = grid->layers[(size_t)nz * grid->nx + nx];
          bool8_t reachable = false_v;
          for (uint32_t m = 0; m < next_layers; ++m) {
            const LevelNode *b =
                level_node(grid, (uint32_t)nx, (uint32_t)nz, m);
            const float32_t rise = b->position.y - a->position.y;
            reachable |= fabsf(rise) < VKR_EDITOR_LEVEL_DROP_MAX;
            const Vec3 middle =
                vec3_scale(vec3_add(a->position, b->position), 0.5f);
            if (b->state == LEVEL_WALKABLE && rise > grid->capsule->step_up &&
                rise <= 1.25f) {
              level_issue(issues, VKR_EDITOR_LEVEL_STEP_TOO_HIGH, middle,
                          b->entity, rise);
            } else if (b->state == LEVEL_STEEP && fabsf(rise) < 1.0f) {
              level_issue(issues, VKR_EDITOR_LEVEL_TOO_STEEP, b->position,
                          b->entity,
                          acosf(vkr_clamp_f32(b->normal_y, -1.0f, 1.0f)) *
                              57.29577951f);
            }
          }
          if (!reachable) {
            level_issue(issues, VKR_EDITOR_LEVEL_VOID_EDGE, a->position,
                        a->entity, 0.0f);
          }
        }
      }
    }
  }
}

/* A brush's planes in world space; false for a scaled or unread brush. */
static bool8_t level_brush_planes(const VkrScene *scene, VkrEntityId brush,
                                  VkrBrushPlane *out, uint32_t *count) {
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t total =
      vkr_scene_brush_faces(scene, brush, faces, VKR_BRUSH_FACE_MAX);
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, brush, scene->comp_transform);
  if (!transform || total > VKR_BRUSH_FACE_MAX) {
    return false_v;
  }
  const Mat4 world = transform->world;
  const Mat4 normal_matrix = mat4_transpose(mat4_inverse_affine(world));
  for (uint32_t i = 0; i < total; ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    const float32_t length = vec3_length(face->normal);
    const Vec3 n = vec3_scale(face->normal, 1.0f / length);
    const Vec4 wn = mat4_mul_vec4(normal_matrix, vec3_to_vec4(n, 0.0f));
    const Vec3 world_normal = vec3_normalize(vec3_new(wn.x, wn.y, wn.z));
    const Vec3 point =
        mat4_mul_vec3(world, vec3_scale(n, face->distance / length));
    out[i] = (VkrBrushPlane){.normal = world_normal,
                             .distance = vec3_dot(world_normal, point)};
  }
  *count = total;
  return true_v;
}

/* Connections whose source lies in the region and that will not route. */
static void level_lint_connections(LevelGrid *grid, LevelIssues *issues) {
  const VkrScene *scene = grid->scene;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_scene_entity_alive(scene, entity) &&
                vkr_scene_get_typed(scene, entity,
                                    &vkr_scene_io_connection_type)
            ? vkr_entity_get_component(scene->world, entity,
                                       scene->comp_transform)
            : NULL;
    const SceneTransform *source =
        transform ? vkr_entity_get_component(scene->world, transform->parent,
                                             scene->comp_transform)
                  : NULL;
    char problem[160];
    if (!source) {
      continue;
    }
    const Vec3 at = mat4_position(source->world);
    if (!level_in_region(grid, at) ||
        !vkr_io_connection_problem(scene, entity, problem, sizeof(problem))) {
      continue;
    }
    /* Each connection is its own issue, never merged with a neighbour. */
    level_issue_pair(issues, VKR_EDITOR_LEVEL_BROKEN_CONNECTION, at,
                     transform->parent, entity, 0.0f);
  }
}

/* Seconds a mover takes from one end to the other: its length over its
   speed, plus the time its acceleration spends speeding up and slowing down
   (vkr_io_router's io_mover_advance). A spinning mover never arrives. */
static float32_t level_mover_seconds(const VkrScene *scene, VkrEntityId entity,
                                     const SceneMover *mover) {
  float32_t length = fabsf(mover->angle);
  if (mover->angle == 0.0f) {
    Vec3 travel = vec3_zero();
    if (!vkr_io_mover_travel(scene, entity, &travel)) {
      return 0.0f;
    }
    length = vec3_length(travel);
  }
  if (!(mover->speed > 0.0f) || !(length > 0.0f)) {
    return 0.0f;
  }
  if (!(mover->acceleration > 0.0f)) {
    return length / mover->speed;
  }
  /* Speeding up and slowing down take speed^2 / acceleration of the way. */
  const float32_t ramps = mover->speed * mover->speed / mover->acceleration;
  if (length >= ramps) {
    return length / mover->speed + mover->speed / mover->acceleration;
  }
  return 2.0f * sqrtf(length / mover->acceleration);
}

/* A port name without its component, as `mover.open` -> `open`. */
static const char *level_port_name(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

/* Every vehicle keeps a fixed stay and departure: a looping mover rests at
   each end, and a mover its arrival opens (a door on it or at the stop)
   opens, waits and closes within that stay. A door that stays open, or one
   whose delay, opening, wait and closing outlast the stay, is still open as
   the vehicle sets off. */
static void level_lint_movers(LevelGrid *grid, LevelIssues *issues) {
  const VkrScene *scene = grid->scene;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity)) {
      continue;
    }
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, entity, scene->comp_transform);
    const SceneMover *mover =
        vkr_scene_get_typed(scene, entity, &vkr_scene_mover_type);
    if (transform && mover && mover->loop && !mover->spin &&
        !(mover->wait > 0.0f)) {
      const Vec3 at = mat4_position(transform->world);
      if (level_in_region(grid, at)) {
        level_issue_pair(issues, VKR_EDITOR_LEVEL_MOVER_TIMING, at, entity,
                         VKR_ENTITY_ID_INVALID, 0.0f);
      }
      continue;
    }
    const SceneIoConnection *connection =
        transform
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_io_connection_type)
            : NULL;
    const SceneMover *vehicle =
        connection ? vkr_scene_get_typed(scene, transform->parent,
                                         &vkr_scene_mover_type)
                   : NULL;
    if (!vehicle || !vehicle->loop || vehicle->spin ||
        !(vehicle->wait > 0.0f)) {
      continue;
    }
    const char *output = level_port_name(connection->output);
    const char *input = level_port_name(connection->input);
    if ((strcmp(output, "on_opened") && strcmp(output, "on_closed")) ||
        (strcmp(input, "open") && strcmp(input, "toggle"))) {
      continue;
    }
    const VkrEntityId door =
        vkr_scene_find_entity_ref(scene, &connection->target);
    const SceneMover *settings =
        door.u64 ? vkr_scene_get_typed(scene, door, &vkr_scene_mover_type)
                 : NULL;
    const SceneTransform *placed =
        settings ? vkr_entity_get_component(scene->world, door,
                                            scene->comp_transform)
                 : NULL;
    if (!placed || settings->spin) {
      continue;
    }
    const Vec3 at = mat4_position(placed->world);
    if (!level_in_region(grid, at)) {
      continue;
    }
    /* A door left open overruns the whole departure. */
    const float32_t open_for =
        settings->wait < 0.0f
            ? INFINITY
            : Max(0.0f, connection->delay) +
                  2.0f * level_mover_seconds(scene, door, settings) +
                  settings->wait;
    if (open_for > vehicle->wait + 1.0e-3f) {
      level_issue_pair(
          issues, VKR_EDITOR_LEVEL_MOVER_TIMING, at, door, transform->parent,
          isfinite(open_for) ? open_for - vehicle->wait : vehicle->wait);
    }
  }
}

/* Solid brushes that share volume, and brushes that did not build. */
static void level_lint_brushes(LevelGrid *grid, LevelIssues *issues) {
  const VkrScene *scene = grid->scene;
  enum { LEVEL_BRUSH_MAX = 512 };
  VkrEntityId *brushes = malloc(LEVEL_BRUSH_MAX * sizeof(*brushes));
  Vec3 *bounds = malloc(2u * LEVEL_BRUSH_MAX * sizeof(*bounds));
  VkrBrushGeometry *geometry = malloc(sizeof(*geometry));
  VkrBrushPiece *piece = malloc(sizeof(*piece));
  uint32_t count = 0u;
  for (uint32_t i = 0; brushes && bounds && geometry && piece &&
                       i < scene->world->dir.living && count < LEVEL_BRUSH_MAX;
       ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneBrushSettings *brush =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type)
            : NULL;
    const SceneTransform *transform =
        brush ? vkr_entity_get_component(scene->world, entity,
                                         scene->comp_transform)
              : NULL;
    if (!transform) {
      continue;
    }
    const Vec3 at = mat4_position(transform->world);
    if (at.x < grid->min.x || at.x > grid->max.x || at.z < grid->min.z ||
        at.z > grid->max.z || at.y < grid->min.y - 4.0f ||
        at.y > grid->max.y + 4.0f) {
      continue;
    }
    const char *status = vkr_scene_brush_status(scene, entity);
    if (status) {
      level_issue(issues, VKR_EDITOR_LEVEL_INVALID_BRUSH, at, entity, 0.0f);
      continue;
    }
    Vec3 lo = {0};
    Vec3 hi = {0};
    if ((brush->role == SCENE_BRUSH_ROLE_SOLID ||
         brush->role == SCENE_BRUSH_ROLE_CLIP) &&
        vkr_scene_entity_local_bounds(scene, entity, &lo, &hi)) {
      /* World bounds of the local box's corners. */
      Vec3 wlo = vec3_new(INFINITY, INFINITY, INFINITY);
      Vec3 whi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
      for (uint32_t c = 0; c < 8u; ++c) {
        const Vec3 p =
            mat4_mul_vec3(transform->world, vec3_new((c & 1u) ? hi.x : lo.x,
                                                     (c & 2u) ? hi.y : lo.y,
                                                     (c & 4u) ? hi.z : lo.z));
        wlo = vec3_new(Min(wlo.x, p.x), Min(wlo.y, p.y), Min(wlo.z, p.z));
        whi = vec3_new(Max(whi.x, p.x), Max(whi.y, p.y), Max(whi.z, p.z));
      }
      brushes[count] = entity;
      bounds[2u * count] = wlo;
      bounds[2u * count + 1u] = whi;
      count++;
    }
  }
  for (uint32_t a = 0; a < count; ++a) {
    for (uint32_t b = a + 1u; b < count; ++b) {
      const Vec3 lo = vec3_new(Max(bounds[2u * a].x, bounds[2u * b].x),
                               Max(bounds[2u * a].y, bounds[2u * b].y),
                               Max(bounds[2u * a].z, bounds[2u * b].z));
      const Vec3 hi =
          vec3_new(Min(bounds[2u * a + 1u].x, bounds[2u * b + 1u].x),
                   Min(bounds[2u * a + 1u].y, bounds[2u * b + 1u].y),
                   Min(bounds[2u * a + 1u].z, bounds[2u * b + 1u].z));
      if (hi.x - lo.x < 1.0e-3f || hi.y - lo.y < 1.0e-3f ||
          hi.z - lo.z < 1.0e-3f) {
        continue;
      }
      /* The exact intersection of the two convex solids. */
      uint32_t first = 0u;
      uint32_t second = 0u;
      *piece = (VkrBrushPiece){0};
      if (!level_brush_planes(scene, brushes[a], piece->planes, &first) ||
          first + VKR_BRUSH_FACE_MIN > VKR_BRUSH_FACE_MAX ||
          !level_brush_planes(scene, brushes[b], piece->planes + first,
                              &second) ||
          first + second > VKR_BRUSH_FACE_MAX) {
        continue;
      }
      piece->count = first + second;
      if (vkr_brush_prune(piece, geometry) && geometry->volume > 1.0e-3f) {
        const size_t index = issues->count;
        level_issue(issues, VKR_EDITOR_LEVEL_OVERLAP,
                    vec3_scale(vec3_add(lo, hi), 0.5f), brushes[a],
                    geometry->volume);
        if (issues->count > index) {
          issues->items[index].other = brushes[b];
        }
      }
    }
  }
  free(brushes);
  free(bounds);
  free(geometry);
  free(piece);
}

/* The smallest shared area that counts as z-fighting, one square
   centimeter, and how far in front of it another solid hides it. */
#define LEVEL_FIGHT_AREA_MIN 1.0e-4f
#define LEVEL_FIGHT_FRONT 0.01f
/* Shared areas one check reads at most; the issues merge far below it. */
#define LEVEL_FIGHT_MAX 4096u

/* A solid whose drawn faces the z-fighting check gathered: a brush, or a
   piece of a blockout shape (its entity), with its world planes and box. */
typedef struct LevelFightSolid {
  VkrEntityId entity;
  uint32_t first_plane;
  uint32_t plane_count;
  Vec3 lo;
  Vec3 hi;
} LevelFightSolid;

/* The drawn faces about a region in world space. The check owns every
   array and frees them when it returns. */
typedef struct LevelFightSet {
  VkrBrushFaceRef *faces;
  Vec3 *vertices;
  LevelFightSolid *solids;
  VkrBrushPlane *planes;
  uint32_t face_count;
  uint32_t face_capacity;
  uint32_t vertex_count;
  uint32_t vertex_capacity;
  uint32_t solid_count;
  uint32_t solid_capacity;
  uint32_t plane_count;
  uint32_t plane_capacity;
  bool8_t failed;
} LevelFightSet;

/* Grows `*items` to hold at least `needed` items of `size` bytes; growth
   moves them. */
static bool8_t level_reserve(void **items, uint32_t *capacity, uint64_t needed,
                             uint64_t size) {
  if (needed <= *capacity) {
    return true_v;
  }
  if (needed > UINT32_MAX / 2u) {
    return false_v;
  }
  const uint32_t grown = Max(Max(*capacity * 2u, (uint32_t)needed), 256u);
  void *moved = realloc(*items, (size_t)grown * size);
  if (!moved) {
    return false_v;
  }
  *items = moved;
  *capacity = grown;
  return true_v;
}

/* Adds the faces of `geometry`, built in world space from `planes`, as one
   solid of `entity`. */
static void level_fight_add(LevelFightSet *set, VkrEntityId entity,
                            const VkrBrushGeometry *geometry,
                            const VkrBrushPlane *planes, uint32_t plane_count) {
  if (set->failed ||
      !level_reserve((void **)&set->faces, &set->face_capacity,
                     (uint64_t)set->face_count + geometry->face_count,
                     sizeof(*set->faces)) ||
      !level_reserve((void **)&set->vertices, &set->vertex_capacity,
                     (uint64_t)set->vertex_count + geometry->vertex_count,
                     sizeof(*set->vertices)) ||
      !level_reserve((void **)&set->solids, &set->solid_capacity,
                     (uint64_t)set->solid_count + 1u, sizeof(*set->solids)) ||
      !level_reserve((void **)&set->planes, &set->plane_capacity,
                     (uint64_t)set->plane_count + plane_count,
                     sizeof(*set->planes))) {
    set->failed = true_v;
    return;
  }
  const uint32_t owner = set->solid_count++;
  set->solids[owner] = (LevelFightSolid){.entity = entity,
                                         .first_plane = set->plane_count,
                                         .plane_count = plane_count,
                                         .lo = geometry->min,
                                         .hi = geometry->max};
  MemCopy(set->planes + set->plane_count, planes,
          plane_count * sizeof(*planes));
  set->plane_count += plane_count;
  for (uint32_t f = 0; f < geometry->face_count; ++f) {
    const VkrBrushPolygon polygon = geometry->polygons[f];
    set->faces[set->face_count++] =
        (VkrBrushFaceRef){.normal = geometry->normals[f],
                          .first = set->vertex_count,
                          .count = polygon.count,
                          .owner = owner};
    MemCopy(set->vertices + set->vertex_count,
            geometry->vertices + polygon.first,
            polygon.count * sizeof(*set->vertices));
    set->vertex_count += polygon.count;
  }
}

/* Whether `point` lies inside a gathered solid, deeper than the coplanar
   distance. */
static bool8_t level_fight_hidden(const LevelFightSet *set, Vec3 point) {
  for (uint32_t s = 0; s < set->solid_count; ++s) {
    const LevelFightSolid *solid = &set->solids[s];
    if (point.x < solid->lo.x || point.y < solid->lo.y ||
        point.z < solid->lo.z || point.x > solid->hi.x ||
        point.y > solid->hi.y || point.z > solid->hi.z) {
      continue;
    }
    bool8_t inside = true_v;
    for (uint32_t p = 0; p < solid->plane_count && inside; ++p) {
      const VkrBrushPlane plane = set->planes[solid->first_plane + p];
      inside = vec3_dot(plane.normal, point) - plane.distance <
               -VKR_BRUSH_COPLANAR_DISTANCE;
    }
    if (inside) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t level_box_apart(Vec3 a_lo, Vec3 a_hi, Vec3 b_lo, Vec3 b_hi) {
  return a_hi.x < b_lo.x || a_hi.y < b_lo.y || a_hi.z < b_lo.z ||
         b_hi.x < a_lo.x || b_hi.y < a_lo.y || b_hi.z < a_lo.z;
}

/* Gathers the drawn faces of the visible solid and visual brushes and
   blockout shape pieces whose boxes touch lo-hi. Clip and trigger brushes
   draw only while editing. */
static void level_fight_gather(const VkrScene *scene, Vec3 lo, Vec3 hi,
                               VkrBrushGeometry *geometry, LevelFightSet *set) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  for (uint32_t i = 0; i < scene->world->dir.living && !set->failed; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity) ||
        !vkr_scene_entity_visible(scene, entity)) {
      continue;
    }
    const SceneBrushSettings *brush =
        vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type);
    if (brush) {
      uint32_t count = 0u;
      if ((brush->role == SCENE_BRUSH_ROLE_SOLID ||
           brush->role == SCENE_BRUSH_ROLE_VISUAL) &&
          level_brush_planes(scene, entity, planes, &count) &&
          vkr_brush_build(planes, count, geometry, NULL) == VKR_BRUSH_OK &&
          !level_box_apart(geometry->min, geometry->max, lo, hi)) {
        level_fight_add(set, entity, geometry, planes, count);
      }
      continue;
    }
    VkrBlockoutPiece *pieces = NULL;
    const uint32_t piece_count =
        vkr_editor_shape_pieces(scene, entity, &pieces);
    for (uint32_t k = 0; k < piece_count; ++k) {
      const uint32_t count = vkr_brush_hull(
          pieces[k].points, pieces[k].point_count, planes, VKR_BRUSH_FACE_MAX);
      if (count &&
          vkr_brush_build(planes, count, geometry, NULL) == VKR_BRUSH_OK &&
          !level_box_apart(geometry->min, geometry->max, lo, hi)) {
        level_fight_add(set, entity, geometry, planes, count);
      }
    }
    free(pieces);
  }
}

/* Drawn faces of two solids that share a plane and face the same way over
   more than a square centimeter: z-fighting, which flickers in any view.
   A shared area counts in the region its middle lies in, and not when the
   point just in front of it lies inside another solid, as a face buried in
   a wall. */
static void level_lint_fights(LevelGrid *grid, LevelIssues *issues) {
  const Vec3 lo = vec3_new(grid->min.x, grid->min.y - 4.0f, grid->min.z);
  const Vec3 hi = vec3_new(grid->max.x, grid->max.y + 4.0f, grid->max.z);
  LevelFightSet set = {0};
  VkrBrushGeometry *geometry = malloc(sizeof(*geometry));
  VkrBrushFaceSlot *slots = NULL;
  VkrBrushFaceOverlap *fights = malloc(LEVEL_FIGHT_MAX * sizeof(*fights));
  if (!geometry || !fights) {
    goto cleanup;
  }
  level_fight_gather(grid->scene, lo, hi, geometry, &set);
  slots =
      malloc((size_t)set.face_count * VKR_BRUSH_FACE_SLOT_MAX * sizeof(*slots));
  if (set.failed || !slots) {
    goto cleanup;
  }

  const uint32_t found = vkr_brush_coplanar_overlaps(
      set.faces, set.face_count, set.vertices, slots, LEVEL_FIGHT_AREA_MIN,
      fights, LEVEL_FIGHT_MAX);
  for (uint32_t i = 0; i < Min(found, LEVEL_FIGHT_MAX); ++i) {
    const VkrBrushFaceOverlap *fight = &fights[i];
    const Vec3 at = fight->center;
    const VkrEntityId entity = set.solids[fight->owner_a].entity;
    if (at.x < grid->min.x || at.x >= grid->max.x || at.z < grid->min.z ||
        at.z >= grid->max.z || at.y < lo.y || at.y > hi.y ||
        level_issue_merges(issues, VKR_EDITOR_LEVEL_Z_FIGHT, at, entity) ||
        level_fight_hidden(
            &set, vec3_add(at, vec3_scale(fight->normal, LEVEL_FIGHT_FRONT)))) {
      continue;
    }
    const uint32_t index = issues->count;
    level_issue(issues, VKR_EDITOR_LEVEL_Z_FIGHT, at, entity, fight->area);
    if (issues->count > index) {
      issues->items[index].other = set.solids[fight->owner_b].entity;
    }
  }

cleanup:
  free(set.faces);
  free(set.vertices);
  free(set.solids);
  free(set.planes);
  free(geometry);
  free(slots);
  free(fights);
}

uint32_t vkr_editor_level_job_lint(VkrEditorLevelJob *job,
                                   const VkrScene *scene, const Vec3 *start,
                                   VkrEditorLevelIssue *out, uint32_t capacity,
                                   VkrEditorLevelStats *stats) {
  LevelIssues issues = {.items = out, .capacity = capacity};
  /* Physics queries take a mutable scene but change none of its state. */
  LevelGrid grid = job->grid;
  grid.scene = (VkrScene *)scene;
  level_ladders(&grid);
  const size_t total = (size_t)grid.nx * grid.nz * LEVEL_LAYER_MAX;
  uint32_t walkable = 0u;
  uint32_t samples = 0u;
  for (size_t i = 0; i < total; ++i) {
    samples += grid.nodes[i].state != LEVEL_NONE;
    walkable += grid.nodes[i].state == LEVEL_WALKABLE;
  }
  level_lint_floor(&grid, &issues);
  level_lint_brushes(&grid, &issues);
  level_lint_fights(&grid, &issues);
  level_lint_connections(&grid, &issues);
  level_lint_movers(&grid, &issues);
  uint32_t reachable = 0u;
  int32_t *queue = malloc(total * sizeof(*queue));
  const int32_t origin = start && queue ? level_nearest(&grid, *start) : -1;
  if (origin >= 0) {
    reachable = level_walk(&grid, origin, queue);
    /* Each walkable area the start cannot reach, once, with its size. */
    for (size_t i = 0; i < total; ++i) {
      if (!level_passable(&grid.nodes[i]) || grid.nodes[i].visited) {
        continue;
      }
      const uint32_t area = level_walk(&grid, (int32_t)i, queue);
      if (area >= 4u) {
        level_issue(&issues, VKR_EDITOR_LEVEL_UNREACHABLE,
                    grid.nodes[i].position, grid.nodes[i].entity,
                    (float32_t)area * grid.cell * grid.cell);
      }
    }
  }
  free(queue);
  free(grid.links);
  if (stats) {
    *stats = (VkrEditorLevelStats){.cell = grid.cell,
                                   .samples = samples,
                                   .walkable = walkable,
                                   .reachable = reachable};
  }
  return issues.found;
}

void vkr_editor_level_reachable_region(Vec3 from, Vec3 to, Vec3 *out_min,
                                       Vec3 *out_max) {
  const Vec3 pad = vec3_new(16.0f, 4.0f, 16.0f);
  *out_min = vec3_sub(
      vec3_new(Min(from.x, to.x), Min(from.y, to.y), Min(from.z, to.z)), pad);
  *out_max = vec3_add(
      vec3_new(Max(from.x, to.x), Max(from.y, to.y), Max(from.z, to.z)), pad);
}

bool8_t vkr_editor_level_job_reachable(VkrEditorLevelJob *job,
                                       const VkrScene *scene, Vec3 from,
                                       Vec3 to, Vec3 *path,
                                       uint32_t path_capacity,
                                       uint32_t *path_count, float32_t *length,
                                       VkrEditorLevelRoute *route) {
  *path_count = 0u;
  *length = 0.0f;
  *route = (VkrEditorLevelRoute){0};
  LevelGrid grid = job->grid;
  grid.scene = (VkrScene *)scene;
  level_ladders(&grid);
  const size_t total = (size_t)grid.nx * grid.nz * LEVEL_LAYER_MAX;
  int32_t *queue = malloc(total * sizeof(*queue));
  const int32_t start = level_nearest(&grid, from);
  const int32_t goal = level_nearest(&grid, to);
  route->from_found = start >= 0;
  route->to_found = goal >= 0;
  bool8_t reached = false_v;
  int32_t end = -1;
  if (queue && start >= 0) {
    /* A standing route first, so `crouch` means one is needed. */
    grid.standing_only = true_v;
    uint32_t visited = level_walk(&grid, start, queue);
    reached = goal >= 0 && grid.nodes[goal].visited;
    if (!reached) {
      for (size_t i = 0; i < total; ++i) {
        grid.nodes[i].visited = false_v;
        grid.nodes[i].parent = -1;
        grid.nodes[i].ladder = false_v;
      }
      grid.standing_only = false_v;
      visited = level_walk(&grid, start, queue);
      reached = goal >= 0 && grid.nodes[goal].visited;
    }
    end = reached ? goal : start;
    /* A failed route ends at the reached floor nearest `to`, to show where
       the way stops. */
    float32_t best = INFINITY;
    for (uint32_t i = 0; !reached && i < visited; ++i) {
      const float32_t distance =
          vec3_length(vec3_sub(grid.nodes[queue[i]].position, to));
      if (distance < best) {
        best = distance;
        end = queue[i];
      }
    }
    route->closest = grid.nodes[end].position;
    route->gap = reached ? 0.0f : best;
  }
  if (end >= 0) {
    /* Walk back from the end to total the route, then again keeping an even
       share of its steps that fits `path`, with the start, the end and both
       ends of each change in height beyond a step, as a ladder or a drop,
       and reverse into start-to-end order. Keeping the first steps found
       would drop the start of a long route. */
    const float32_t step_up = grid.capsule->step_up;
    uint32_t steps = 0u;
    uint32_t changes = 0u;
    for (int32_t at = end; at >= 0; at = grid.nodes[at].parent) {
      const int32_t parent = grid.nodes[at].parent;
      route->crouch |= grid.nodes[at].state == LEVEL_CROUCH;
      route->ladders += grid.nodes[at].ladder ? 1u : 0u;
      if (parent >= 0) {
        *length += vec3_length(
            vec3_sub(grid.nodes[at].position, grid.nodes[parent].position));
        changes += fabsf(grid.nodes[at].position.y -
                         grid.nodes[parent].position.y) > step_up
                       ? 2u
                       : 0u;
      }
      steps++;
    }
    const uint32_t share =
        path_capacity > changes + 2u ? path_capacity - changes - 2u : 1u;
    const uint32_t stride = Max(1u, (steps + share - 1u) / share);
    uint32_t count = 0u;
    uint32_t from_start = steps - 1u;
    /* The walk runs from the end: whether the step from a node to the one
       after it on the route changes height beyond a step. */
    bool8_t change_after = false_v;
    for (int32_t at = end; at >= 0 && count < path_capacity;
         at = grid.nodes[at].parent, from_start--) {
      const int32_t parent = grid.nodes[at].parent;
      const bool8_t change_before =
          parent >= 0 && fabsf(grid.nodes[at].position.y -
                               grid.nodes[parent].position.y) > step_up;
      if (from_start % stride == 0u || from_start == steps - 1u ||
          change_before || change_after) {
        path[count++] = grid.nodes[at].position;
      }
      change_after = change_before;
    }
    for (uint32_t i = 0; i < count / 2u; ++i) {
      const Vec3 swap = path[i];
      path[i] = path[count - 1u - i];
      path[count - 1u - i] = swap;
    }
    *path_count = count;
  }
  free(queue);
  free(grid.links);
  return reached;
}

float32_t vkr_editor_level_map_size(Vec3 min, Vec3 max,
                                    const VkrEditorLevelCapsule *capsule,
                                    float32_t cell, uint32_t *out_columns,
                                    uint32_t *out_rows) {
  return level_grid_size(min, max, capsule, cell, out_columns, out_rows);
}

bool8_t vkr_editor_level_job_map(VkrEditorLevelJob *job, const VkrScene *scene,
                                 const Vec3 *start, char *text,
                                 float32_t *heights, uint32_t capacity,
                                 VkrEditorLevelStats *stats,
                                 bool8_t *out_start_found) {
  LevelGrid grid = job->grid;
  grid.scene = (VkrScene *)scene;
  const size_t cells = (size_t)grid.nx * grid.nz;
  if (cells > capacity) {
    return false_v;
  }
  level_ladders(&grid);
  const size_t total = cells * LEVEL_LAYER_MAX;
  int32_t *queue = start ? malloc(total * sizeof(*queue)) : NULL;
  const int32_t origin = queue ? level_nearest(&grid, *start) : -1;
  uint32_t reachable = 0u;
  if (origin >= 0) {
    reachable = level_walk(&grid, origin, queue);
  }
  free(queue);
  free(grid.links);
  uint32_t samples = 0u;
  uint32_t walkable = 0u;
  for (size_t c = 0; c < cells; ++c) {
    /* The highest walkable floor of the cell, else its highest crouch
       floor, else its highest floor; floors are found from the top down. */
    const LevelNode *shown = NULL;
    for (uint32_t l = 0; l < grid.layers[c]; ++l) {
      const LevelNode *node = &grid.nodes[c * LEVEL_LAYER_MAX + l];
      samples++;
      walkable += node->state == LEVEL_WALKABLE;
      if (!shown ||
          (node->state == LEVEL_WALKABLE && shown->state != LEVEL_WALKABLE) ||
          (node->state == LEVEL_CROUCH && !level_passable(shown))) {
        shown = node;
      }
    }
    char mark = '-';
    if (shown && shown->state == LEVEL_WALKABLE) {
      mark = origin >= 0 && !shown->visited ? ',' : '.';
    } else if (shown && shown->state == LEVEL_CROUCH) {
      mark = origin >= 0 && !shown->visited ? ';' : 'c';
    } else if (shown && shown->state == LEVEL_STEEP) {
      mark = '/';
    } else if (shown && shown->state == LEVEL_LOW) {
      mark = '_';
    } else if (shown && shown->state == LEVEL_NARROW) {
      mark = 'n';
    } else if (shown) {
      mark = '#';
    }
    text[c] = mark;
    if (heights) {
      heights[c] = shown ? shown->position.y : NAN;
    }
  }
  if (origin >= 0) {
    text[(size_t)origin / LEVEL_LAYER_MAX] = 'S';
  }
  if (stats) {
    *stats = (VkrEditorLevelStats){.cell = grid.cell,
                                   .samples = samples,
                                   .walkable = walkable,
                                   .reachable = reachable};
  }
  if (out_start_found) {
    *out_start_found = origin >= 0;
  }
  return true_v;
}

bool8_t vkr_editor_brush_ray(const VkrScene *scene, VkrEntityId brush,
                             Vec3 origin, Vec3 direction,
                             float32_t max_distance, float32_t *out_distance,
                             VkrEntityId *out_face) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  uint32_t count = 0u;
  if (!level_brush_planes(scene, brush, planes, &count) ||
      vkr_scene_brush_faces(scene, brush, faces, VKR_BRUSH_FACE_MAX) != count) {
    return false_v;
  }
  /* Slab test: enter at the last plane facing the ray, leave at the first
     facing away. */
  float32_t enter = -INFINITY;
  float32_t leave = max_distance;
  uint32_t entered = UINT32_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    const float32_t facing = vec3_dot(planes[i].normal, direction);
    const float32_t gap =
        planes[i].distance - vec3_dot(planes[i].normal, origin);
    if (fabsf(facing) < 1.0e-6f) {
      if (gap < 0.0f) {
        return false_v;
      }
      continue;
    }
    const float32_t t = gap / facing;
    if (facing < 0.0f) {
      if (t > enter) {
        enter = t;
        entered = i;
      }
    } else {
      leave = Min(leave, t);
    }
  }
  /* A ray starting inside the brush enters no face. */
  if (entered == UINT32_MAX || enter > leave || enter <= 0.0f) {
    return false_v;
  }
  *out_distance = enter;
  *out_face = faces[entered];
  return true_v;
}

bool8_t vkr_editor_brush_pick(const VkrSampleUiFrame *frame, Vec3 origin,
                              Vec3 direction, float32_t max_distance,
                              VkrEntityId *out_face) {
  const VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {frame->scene,
                                                         frame->world};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = frame->additive[i];
  }
  float32_t nearest = max_distance;
  bool8_t found = false_v;
  for (uint32_t s = 0; s < ArrayCount(scenes); ++s) {
    const VkrScene *scene = scenes[s];
    if (!scene || (s == 1u && scene == frame->scene)) {
      continue;
    }
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      /* What the editor hides cannot be picked. */
      if (!vkr_scene_entity_alive(scene, entity) ||
          !vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type) ||
          !vkr_scene_editor_shown(scene, entity)) {
        continue;
      }
      float32_t distance = 0.0f;
      VkrEntityId face = {0};
      if (vkr_editor_brush_ray(scene, entity, origin, direction, nearest,
                               &distance, &face)) {
        nearest = distance;
        *out_face = face;
        found = true_v;
      }
    }
  }
  return found;
}

uint32_t vkr_editor_brush_world_planes(const VkrScene *scene, VkrEntityId brush,
                                       VkrBrushPlane *out, uint32_t capacity) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  uint32_t count = 0u;
  if (!vkr_scene_entity_alive(scene, brush) ||
      !level_brush_planes(scene, brush, planes, &count) || count > capacity) {
    return 0u;
  }
  MemCopy(out, planes, count * sizeof(*out));
  return count;
}

uint32_t vkr_editor_brush_build(const VkrScene *scene, VkrEntityId brush,
                                VkrBrushGeometry *scratch, VkrEntityId *faces) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  uint32_t count = 0u;
  uint32_t bad_face = 0u;
  if (!vkr_scene_entity_alive(scene, brush) ||
      !level_brush_planes(scene, brush, planes, &count) ||
      vkr_scene_brush_faces(scene, brush, faces, VKR_BRUSH_FACE_MAX) != count ||
      vkr_brush_build(planes, count, scratch, &bad_face) != VKR_BRUSH_OK) {
    return 0u;
  }
  return count;
}

// =============================================================================
// Brush magnet
// =============================================================================

/* The world boxes of the brushes a moved or drawn brush snaps to, and the
   moved brush's box relative to where its move started. */
typedef struct VkrEditorMagnet {
  bool8_t moving;
  Vec3 moving_lo;
  Vec3 moving_hi;
  float32_t moving_floor;
  uint32_t count;
  uint32_t capacity;
  /* Minimum and maximum corner of each brush, and its floor: a slab's top,
     else its bottom. */
  Vec3 *boxes;
  float32_t *floors;
  VkrBrushGeometry geometry;
} VkrEditorMagnet;

static VkrEditorMagnet *magnet_state(VkrEditorUi *editor) {
  if (!editor->magnet) {
    editor->magnet = calloc(1u, sizeof(*editor->magnet));
  }
  return editor->magnet;
}

/* Whether `entity` is `ancestor` or lies below it. */
static bool8_t level_within(const VkrScene *scene, VkrEntityId entity,
                            VkrEntityId ancestor) {
  for (uint32_t depth = 0; entity.u64 && depth < 64u; ++depth) {
    if (entity.u64 == ancestor.u64) {
      return true_v;
    }
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, entity, scene->comp_transform);
    entity = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  }
  return false_v;
}

bool8_t vkr_editor_entity_free(const VkrScene *scene, VkrEntityId entity) {
  const SceneFreePlacement *free =
      scene && vkr_scene_entity_alive(scene, entity)
          ? vkr_scene_get_typed(scene, entity, &vkr_scene_free_placement_type)
          : NULL;
  return free && free->enabled;
}

uint32_t vkr_editor_shape_pieces(const VkrScene *scene, VkrEntityId entity,
                                 VkrBlockoutPiece **out) {
  *out = NULL;
  const SceneBlockout *shape =
      scene && vkr_scene_entity_alive(scene, entity)
          ? vkr_scene_get_typed(scene, entity, &vkr_scene_blockout_type)
          : NULL;
  const SceneTransform *transform =
      shape ? vkr_entity_get_component(scene->world, entity,
                                       scene->comp_transform)
            : NULL;
  if (!transform) {
    return 0u;
  }
  const uint32_t capacity = vkr_blockout_piece_capacity(shape);
  VkrBlockoutPiece *pieces = malloc((size_t)capacity * sizeof(*pieces));
  char error[8];
  const uint32_t count = pieces ? vkr_blockout_layout(shape, pieces, capacity,
                                                      error, sizeof(error))
                                : 0u;
  if (!count) {
    free(pieces);
    return 0u;
  }
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t p = 0; p < pieces[i].point_count; ++p) {
      pieces[i].points[p] =
          mat4_mul_vec3(transform->world, pieces[i].points[p]);
    }
  }
  *out = pieces;
  return count;
}

/* A solid of a scene, by its world box: a brush that builds, or a piece of
   a blockout shape. */
typedef struct LevelSolid {
  /* The brush, or the shape the piece belongs to. */
  VkrEntityId entity;
  Vec3 lo;
  Vec3 hi;
  bool8_t trigger;
} LevelSolid;

typedef void (*LevelSolidVisit)(void *context, const LevelSolid *solid);

/* Visits every solid of `scene`; `scratch` holds the last brush built. */
static void level_solids(const VkrScene *scene, VkrBrushGeometry *scratch,
                         LevelSolidVisit visit, void *context) {
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  for (uint32_t i = 0; scene && i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity)) {
      continue;
    }
    const SceneBrushSettings *brush =
        vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type);
    if (brush && vkr_editor_brush_build(scene, entity, scratch, faces)) {
      const LevelSolid solid = {
          .entity = entity,
          .lo = scratch->min,
          .hi = scratch->max,
          .trigger = brush->role == SCENE_BRUSH_ROLE_TRIGGER,
      };
      visit(context, &solid);
      continue;
    }
    VkrBlockoutPiece *pieces = NULL;
    const uint32_t count = vkr_editor_shape_pieces(scene, entity, &pieces);
    for (uint32_t k = 0; k < count; ++k) {
      LevelSolid solid = {
          .entity = entity,
          .lo = vec3_new(INFINITY, INFINITY, INFINITY),
          .hi = vec3_new(-INFINITY, -INFINITY, -INFINITY),
      };
      for (uint32_t p = 0; p < pieces[k].point_count; ++p) {
        const Vec3 at = pieces[k].points[p];
        solid.lo = vec3_new(Min(solid.lo.x, at.x), Min(solid.lo.y, at.y),
                            Min(solid.lo.z, at.z));
        solid.hi = vec3_new(Max(solid.hi.x, at.x), Max(solid.hi.y, at.y),
                            Max(solid.hi.z, at.z));
      }
      visit(context, &solid);
    }
    free(pieces);
  }
}

/* The box around the solids within `root`, and whether any is. */
typedef struct LevelBox {
  const VkrScene *scene;
  VkrEntityId root;
  Vec3 lo;
  Vec3 hi;
  bool8_t found;
} LevelBox;

static void level_box_visit(void *context, const LevelSolid *solid) {
  LevelBox *box = context;
  if (!level_within(box->scene, solid->entity, box->root)) {
    return;
  }
  box->lo = vec3_new(Min(box->lo.x, solid->lo.x), Min(box->lo.y, solid->lo.y),
                     Min(box->lo.z, solid->lo.z));
  box->hi = vec3_new(Max(box->hi.x, solid->hi.x), Max(box->hi.y, solid->hi.y),
                     Max(box->hi.z, solid->hi.z));
  box->found = true_v;
}

bool8_t vkr_editor_entity_world_box(const VkrScene *scene, VkrEntityId entity,
                                    VkrBrushGeometry *scratch, Vec3 *out_lo,
                                    Vec3 *out_hi) {
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  LevelBox box = {
      .scene = scene,
      .root = entity,
      .lo = vec3_new(INFINITY, INFINITY, INFINITY),
      .hi = vec3_new(-INFINITY, -INFINITY, -INFINITY),
  };
  level_solids(scene, scratch, level_box_visit, &box);
  Vec3 lo = box.lo;
  Vec3 hi = box.hi;
  bool8_t found = box.found;
  /* Without brushes, the box around its meshes and shapes. */
  Vec3 local_lo = vec3_zero();
  Vec3 local_hi = vec3_zero();
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (!found && transform &&
      vkr_scene_entity_local_bounds(scene, entity, &local_lo, &local_hi)) {
    for (uint32_t c = 0; c < 8u; ++c) {
      const Vec3 corner = mat4_mul_vec3(
          transform->world, vec3_new((c & 1u) ? local_hi.x : local_lo.x,
                                     (c & 2u) ? local_hi.y : local_lo.y,
                                     (c & 4u) ? local_hi.z : local_lo.z));
      lo = vec3_new(Min(lo.x, corner.x), Min(lo.y, corner.y),
                    Min(lo.z, corner.z));
      hi = vec3_new(Max(hi.x, corner.x), Max(hi.y, corner.y),
                    Max(hi.z, corner.z));
    }
    found = true_v;
  }
  *out_lo = lo;
  *out_hi = hi;
  return found;
}

bool8_t vkr_editor_box_slab(Vec3 lo, Vec3 hi) {
  return hi.y - lo.y <= Max(0.5f, 0.25f * Min(hi.x - lo.x, hi.z - lo.z));
}

/* The slabs at the bottom of the box `lo` around `root`'s solids: the area
   they cover and the highest top. */
typedef struct LevelFloor {
  const VkrScene *scene;
  VkrEntityId root;
  Vec3 lo;
  float32_t covered;
  float32_t top;
} LevelFloor;

static void level_floor_visit(void *context, const LevelSolid *solid) {
  LevelFloor *floor = context;
  if (level_within(floor->scene, solid->entity, floor->root) &&
      solid->lo.y - floor->lo.y < 0.02f &&
      vkr_editor_box_slab(solid->lo, solid->hi)) {
    floor->covered += (solid->hi.x - solid->lo.x) * (solid->hi.z - solid->lo.z);
    floor->top = Max(floor->top, solid->hi.y);
  }
}

float32_t vkr_editor_entity_floor(const VkrScene *scene, VkrEntityId entity,
                                  VkrBrushGeometry *scratch, Vec3 lo, Vec3 hi) {
  const SceneBlockout *shape =
      scene && vkr_scene_entity_alive(scene, entity)
          ? vkr_scene_get_typed(scene, entity, &vkr_scene_blockout_type)
          : NULL;
  if (!scene || (shape && shape->shape == SCENE_BLOCKOUT_STAIRS)) {
    return lo.y;
  }
  LevelFloor floor = {.scene = scene, .root = entity, .lo = lo, .top = lo.y};
  level_solids(scene, scratch, level_floor_visit, &floor);
  const float32_t footprint = Max((hi.x - lo.x) * (hi.z - lo.z), 1.0e-4f);
  return floor.covered >= footprint * 0.25f ? floor.top : lo.y;
}

/* The magnet gathering the solids of a scene outside `exclude`. */
typedef struct MagnetGather {
  VkrEditorMagnet *magnet;
  const VkrScene *scene;
  VkrEntityId exclude;
} MagnetGather;

static void magnet_gather_visit(void *context, const LevelSolid *solid) {
  MagnetGather *gather = context;
  VkrEditorMagnet *magnet = gather->magnet;
  /* Triggers are not solids to line up with, nor what the editor hides. */
  if (solid->trigger || !vkr_scene_editor_shown(gather->scene, solid->entity) ||
      (gather->exclude.u64 &&
       level_within(gather->scene, solid->entity, gather->exclude))) {
    return;
  }
  if (magnet->count == magnet->capacity) {
    const uint32_t capacity = Max(64u, magnet->capacity * 2u);
    Vec3 *grown =
        realloc(magnet->boxes, 2u * (size_t)capacity * sizeof(*grown));
    if (!grown) {
      return;
    }
    magnet->boxes = grown;
    float32_t *floors =
        realloc(magnet->floors, (size_t)capacity * sizeof(*floors));
    if (!floors) {
      return;
    }
    magnet->floors = floors;
    magnet->capacity = capacity;
  }
  magnet->boxes[2u * magnet->count] = solid->lo;
  magnet->boxes[2u * magnet->count + 1u] = solid->hi;
  magnet->floors[magnet->count] =
      vkr_editor_box_slab(solid->lo, solid->hi) ? solid->hi.y : solid->lo.y;
  magnet->count++;
}

/* Adds the boxes of `scene`'s solids outside `exclude`: brushes and the
   pieces of blockout shapes. */
static void magnet_gather(VkrEditorMagnet *magnet, const VkrScene *scene,
                          VkrEntityId exclude) {
  MagnetGather gather = {.magnet = magnet, .scene = scene, .exclude = exclude};
  level_solids(scene, &magnet->geometry, magnet_gather_visit, &gather);
}

/* How far apart boxes `lo`-`hi` and `blo`-`bhi` lie on `axis`; zero when
   they overlap or touch. */
static float32_t magnet_gap(Vec3 lo, Vec3 hi, Vec3 blo, Vec3 bhi,
                            uint32_t axis) {
  return Max(0.0f, Max(blo.elements[axis] - hi.elements[axis],
                       lo.elements[axis] - bhi.elements[axis]));
}

/* The reach of the magnet at `point`: 2 % of its distance from the eye,
   about 15 to 20 pixels on screen. */
static float32_t magnet_reach(Vec3 point, Vec3 eye) {
  return vkr_clamp_f32(0.02f * vec3_length(vec3_sub(point, eye)), 0.05f, 2.0f);
}

Vec3 vkr_editor_magnet_move(VkrEditorUi *editor, const VkrScene *scene,
                            VkrEntityId entity, Vec3 from, Vec3 to, Vec3 eye,
                            bool8_t start) {
  const float32_t step = editor->placement.move_grid && editor->move_step > 0.0f
                             ? editor->move_step
                             : 0.0f;
  if ((!editor->placement.magnet && step <= 0.0f) || !scene ||
      !vkr_scene_entity_alive(scene, entity) ||
      vkr_editor_entity_free(scene, entity)) {
    return to;
  }
  VkrEditorMagnet *magnet = magnet_state(editor);
  if (!magnet) {
    return to;
  }
  if (start) {
    /* The entity has not moved yet: its box is where the move starts. */
    Vec3 lo = vec3_zero();
    Vec3 hi = vec3_zero();
    magnet->count = 0u;
    magnet->moving =
        vkr_editor_entity_world_box(scene, entity, &magnet->geometry, &lo, &hi);
    magnet->moving_lo = magnet->moving ? vec3_sub(lo, from) : vec3_zero();
    magnet->moving_hi = magnet->moving ? vec3_sub(hi, from) : vec3_zero();
    magnet->moving_floor =
        magnet->moving ? vkr_editor_entity_floor(scene, entity,
                                                 &magnet->geometry, lo, hi) -
                             from.y
                       : 0.0f;
    if (magnet->moving && editor->placement.magnet) {
      magnet_gather(magnet, scene, entity);
    }
  }

  /* The grid takes the box's low corner, or the origin of an object without
     a box, before the magnet pulls it against a neighbour. */
  if (step > 0.0f) {
    for (uint32_t axis = 0; axis < 3u; ++axis) {
      const float32_t corner =
          magnet->moving_lo.elements[axis] + to.elements[axis];
      to.elements[axis] += roundf(corner / step) * step - corner;
    }
  }
  if (!magnet->moving || !editor->placement.magnet) {
    return to;
  }

  /* Per axis, the smallest shift within reach that makes a side flush
     against a neighbour it overlaps on the other axes, or level with a
     side of a neighbour it touches. */
  const Vec3 lo = vec3_add(magnet->moving_lo, to);
  const Vec3 hi = vec3_add(magnet->moving_hi, to);
  const float32_t reach = magnet_reach(vec3_scale(vec3_add(lo, hi), 0.5f), eye);
  float32_t best[3] = {reach, reach, reach};
  Vec3 shift = vec3_zero();
  for (uint32_t b = 0; b < magnet->count; ++b) {
    const Vec3 blo = magnet->boxes[2u * b];
    const Vec3 bhi = magnet->boxes[2u * b + 1u];
    float32_t gaps[3];
    bool8_t touching = true_v;
    for (uint32_t axis = 0; axis < 3u; ++axis) {
      gaps[axis] = magnet_gap(lo, hi, blo, bhi, axis);
      touching = touching && gaps[axis] <= reach;
    }
    if (!touching) {
      continue;
    }
    /* Stacked boxes overlap across X and Z: one rests on the other and
       neither sinks to the other's bottom. Boxes side by side level their
       floors instead of their bottoms or tops. */
    const bool8_t stacked = gaps[0] <= 1.0e-4f && gaps[2] <= 1.0e-4f;
    const float32_t floor_shift =
        magnet->floors[b] - (magnet->moving_floor + to.y);
    for (uint32_t axis = 0; axis < 3u; ++axis) {
      const bool8_t flush = gaps[(axis + 1u) % 3u] <= 1.0e-4f &&
                            gaps[(axis + 2u) % 3u] <= 1.0e-4f;
      float32_t candidates[4] = {
          blo.elements[axis] - hi.elements[axis],
          bhi.elements[axis] - lo.elements[axis],
          blo.elements[axis] - lo.elements[axis],
          bhi.elements[axis] - hi.elements[axis],
      };
      uint32_t first = flush ? 0u : 2u;
      uint32_t end = 4u;
      if (axis == 1u && stacked) {
        end = 2u;
      } else if (axis == 1u) {
        candidates[2] = floor_shift;
        first = 2u;
        end = 3u;
      }
      for (uint32_t c = first; c < end; ++c) {
        if (fabsf(candidates[c]) < best[axis]) {
          best[axis] = fabsf(candidates[c]);
          shift.elements[axis] = candidates[c];
        }
      }
    }
  }
  return vec3_add(to, shift);
}

void vkr_editor_magnet_begin(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  VkrEditorMagnet *magnet =
      editor->placement.magnet ? magnet_state(editor) : NULL;
  if (!magnet) {
    return;
  }
  magnet->count = 0u;
  magnet->moving = false_v;
  magnet_gather(magnet, frame->scene, VKR_ENTITY_ID_INVALID);
  magnet_gather(magnet, frame->world, VKR_ENTITY_ID_INVALID);
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    magnet_gather(magnet, frame->additive[i], VKR_ENTITY_ID_INVALID);
  }
}

float32_t vkr_editor_magnet_value(const VkrEditorUi *editor, uint32_t axis,
                                  float32_t value, Vec3 lo, Vec3 hi, Vec3 eye) {
  const VkrEditorMagnet *magnet = editor->magnet;
  if (!editor->placement.magnet || !magnet) {
    return value;
  }
  Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  center.elements[axis] = value;
  const float32_t reach = magnet_reach(center, eye);
  float32_t best = reach;
  float32_t snapped = value;
  for (uint32_t b = 0; b < magnet->count; ++b) {
    const Vec3 blo = magnet->boxes[2u * b];
    const Vec3 bhi = magnet->boxes[2u * b + 1u];
    if (magnet_gap(lo, hi, blo, bhi, (axis + 1u) % 3u) > reach ||
        magnet_gap(lo, hi, blo, bhi, (axis + 2u) % 3u) > reach) {
      continue;
    }
    const float32_t sides[2] = {blo.elements[axis], bhi.elements[axis]};
    for (uint32_t i = 0; i < 2u; ++i) {
      if (fabsf(sides[i] - value) < best) {
        best = fabsf(sides[i] - value);
        snapped = sides[i];
      }
    }
  }
  return snapped;
}

void vkr_editor_magnet_destroy(VkrEditorUi *editor) {
  if (editor->magnet) {
    free(editor->magnet->boxes);
    free(editor->magnet->floors);
  }
  free(editor->magnet);
  editor->magnet = NULL;
}

uint32_t vkr_editor_brush_face_outline(const VkrScene *scene, VkrEntityId face,
                                       VkrBrushGeometry *scratch, Vec3 *out,
                                       uint32_t capacity) {
  const SceneTransform *transform =
      vkr_scene_entity_alive(scene, face)
          ? vkr_entity_get_component(scene->world, face, scene->comp_transform)
          : NULL;
  if (!transform ||
      !vkr_scene_get_typed(scene, face, &vkr_scene_brush_face_type)) {
    return 0u;
  }
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      vkr_editor_brush_build(scene, transform->parent, scratch, faces);
  for (uint32_t i = 0; i < count; ++i) {
    if (faces[i].u64 != face.u64) {
      continue;
    }
    const VkrBrushPolygon polygon = scratch->polygons[i];
    const uint32_t written = Min(polygon.count, capacity);
    MemCopy(out, scratch->vertices + polygon.first, written * sizeof(*out));
    return written;
  }
  return 0u;
}

// =============================================================================
// Level checks window
// =============================================================================

static const char *level_issue_label(VkrEditorLevelIssueKind kind) {
  static const char *const labels[VKR_EDITOR_LEVEL_ISSUE_COUNT] = {
      "Step too high",   "Too steep",           "Low ceiling",
      "Gap too narrow",  "Edge into void",      "Unreachable area",
      "Brushes overlap", "Brush did not build", "Connection does not route",
      "Crouch only",     "Faces z-fight",       "Mover overruns its stay"};
  return kind < VKR_EDITOR_LEVEL_ISSUE_COUNT ? labels[kind] : "Issue";
}

VkrEditorLevelReport *vkr_editor_level_report(VkrEditorUi *editor) {
  if (!editor->level_report) {
    editor->level_report = calloc(1u, sizeof(*editor->level_report));
  }
  return editor->level_report;
}

static void level_run(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorLevelReport *report = vkr_editor_level_report(editor);
  if (!report) {
    return;
  }
  const VkrScene *scene = frame->scene ? frame->scene : frame->world;
  const Vec4 image = frame->mapping.image_rect_px;
  VkrEditorDropPose pose;
  if (!scene || !vkr_editor_viewport_place(
                    editor, frame,
                    (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f},
                    0.0f, &pose)) {
    report->checked = false_v;
    return;
  }
  const Vec3 half = vec3_new(20.0f, 10.0f, 20.0f);
  report->min = vec3_sub(pose.position, vec3_new(half.x, 2.0f, half.z));
  report->max = vec3_add(pose.position, vec3_new(half.x, half.y, half.z));
  const VkrEditorLevelCapsule capsule = vkr_editor_level_capsule_default();
  /* The walk starts at the scene's first enabled Player Start. */
  Vec3 start = {0};
  bool8_t has_start = false_v;
  for (uint32_t i = 0; i < scene->world->dir.living && !has_start; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const ScenePlayerStart *player =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_player_start_type)
            : NULL;
    const SceneTransform *transform =
        player && player->enabled
            ? vkr_entity_get_component(scene->world, entity,
                                       scene->comp_transform)
            : NULL;
    if (transform) {
      start = mat4_position(transform->world);
      has_start = true_v;
    }
  }
  /* The check samples a slice each frame (level_window_step), so a dense region
     never stalls the Scene. */
  vkr_editor_level_job_end(report->job);
  report->job =
      vkr_editor_level_job_begin(report->min, report->max, &capsule, 0.0f);
  report->center = pose.position;
  report->start = start;
  report->has_start = has_start;
  report->generation = frame->scene_generation;
}

/* Time a Level checks frame spends sampling. */
#define LEVEL_WINDOW_STEP_SECONDS 0.004

/* Samples the running check and, once every cell is sampled, turns it into
   the report, nearest issue first. A new scene drops the check. */
static void level_window_step(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  VkrEditorLevelReport *report = editor->level_report;
  if (!report || !report->job) {
    return;
  }
  const VkrScene *scene = frame->scene ? frame->scene : frame->world;
  if (!scene || frame->scene_generation != report->generation) {
    vkr_editor_level_job_end(report->job);
    report->job = NULL;
    return;
  }
  if (!vkr_editor_level_job_step(report->job, scene,
                                 LEVEL_WINDOW_STEP_SECONDS)) {
    return;
  }
  report->found = vkr_editor_level_job_lint(
      report->job, scene, report->has_start ? &report->start : NULL,
      report->issues, VKR_EDITOR_LEVEL_SHOWN_MAX, &report->stats);
  vkr_editor_level_job_end(report->job);
  report->job = NULL;
  report->count = Min(report->found, VKR_EDITOR_LEVEL_SHOWN_MAX);
  report->checked = true_v;
  /* Nearest to the view first, by insertion. */
  for (uint32_t i = 1; i < report->count; ++i) {
    const VkrEditorLevelIssue issue = report->issues[i];
    const float32_t distance =
        vec3_length(vec3_sub(issue.position, report->center));
    uint32_t j = i;
    while (j > 0u && vec3_length(vec3_sub(report->issues[j - 1u].position,
                                          report->center)) > distance) {
      report->issues[j] = report->issues[j - 1u];
      --j;
    }
    report->issues[j] = issue;
  }
}

static bool8_t level_button(VkrEditorUi *editor, VkrUiSystem *ui, String8 id,
                            String8 text, uint32_t column, bool8_t primary,
                            bool8_t disabled) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  if (primary) {
    vkr_editor_primary_style(&button, editor->heading_font);
  } else {
    vkr_editor_action_style(&button, editor->heading_font);
  }
  button.placement = (VkrUiPlacement){.column = column,
                                      .row = 0u,
                                      .column_span = 1u,
                                      .row_span = 1u,
                                      .justify = VKR_UI_ALIGN_STRETCH,
                                      .align = VKR_UI_ALIGN_CENTER,
                                      .margin_pt = {0.0f, 3.0f, 0.0f, 3.0f}};
  button.style.padding_pt = (VkrUiEdges){3.0f, 10.0f, 3.0f, 10.0f};
  button.style.min_size_pt.y = vkr_ui_theme()->control_height;
  button.disabled = disabled;
  return vkr_ui_button(ui, id, text, &button);
}

void vkr_editor_level_window_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorLevelReport *report = vkr_editor_level_report(editor);
  if (!report) {
    return;
  }
  level_window_step(editor, frame);
  /* The rows that fit below the header; the summary counts the rest. */
  const float32_t body_pt = bounds.height / ui->content_scale - 52.0f;
  const uint32_t shown =
      Min(report->count, (uint32_t)Max(1.0f, floorf(body_pt / 30.0f)));
  VkrUiTrack rows[VKR_EDITOR_LEVEL_SHOWN_MAX + 2u];
  rows[0] = (VkrUiTrack){.value = 36.0f, .unit = VKR_UI_TRACK_PX};
  for (uint32_t i = 0; i < shown; ++i) {
    rows[i + 1u] = (VkrUiTrack){.value = 30.0f, .unit = VKR_UI_TRACK_PX};
  }
  rows[shown + 1u] = (VkrUiTrack){.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement = (VkrUiPlacement){.column = 0u,
                                    .row = 0u,
                                    .column_span = 1u,
                                    .row_span = 1u,
                                    .justify = VKR_UI_ALIGN_STRETCH,
                                    .align = VKR_UI_ALIGN_STRETCH};
  list.columns = &column;
  list.column_count = 1u;
  list.rows = rows;
  list.row_count = shown + 2u;
  list.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  list.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("level.list"), &list)) {
    return;
  }
  const VkrUiTrack header_columns[] = {
      column, {.value = 120.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = list.placement;
  header.columns = header_columns;
  header.column_count = ArrayCount(header_columns);
  header.rows = &column;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  if (vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    char text[200];
    if (report->job) {
      snprintf(text, sizeof(text), "Checking 40 m around the view... %.0f%%",
               100.0f * vkr_editor_level_job_progress(report->job));
    } else if (report->checked) {
      snprintf(text, sizeof(text),
               "%u issue%s in 40 m around the view, nearest first",
               report->found, report->found == 1u ? "" : "s");
    } else {
      snprintf(text, sizeof(text),
               "Checks the floor around the view against the player capsule");
    }
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    label.placement = (VkrUiPlacement){.column = 0u,
                                       .row = 0u,
                                       .column_span = 1u,
                                       .row_span = 1u,
                                       .justify = VKR_UI_ALIGN_START,
                                       .align = VKR_UI_ALIGN_CENTER};
    vkr_ui_label(ui, string8_lit("summary"),
                 string8_create_from_cstr((const uint8_t *)text, strlen(text)),
                 &label);
    if (level_button(editor, ui, string8_lit("check"),
                     report->job ? string8_lit("Checking")
                                 : string8_lit("Check"),
                     1u, true_v, report->job != NULL)) {
      level_run(editor, frame);
    }
    (void)vkr_ui_panel_end(ui);
  }
  const VkrUiTrack row_columns[] = {column,
                                    {.value = 72.0f, .unit = VKR_UI_TRACK_PX}};
  for (uint32_t i = 0; i < shown; ++i) {
    const VkrEditorLevelIssue *issue = &report->issues[i];
    (void)vkr_ui_push_id_u64(ui, i + 1u);
    VkrUiPanelConfig row = vkr_ui_panel_config_default();
    row.placement = list.placement;
    row.placement.row = i + 1u;
    row.columns = row_columns;
    row.column_count = ArrayCount(row_columns);
    row.rows = &column;
    row.row_count = 1u;
    row.style.padding_pt = (VkrUiEdges){2.0f, 6.0f, 2.0f, 8.0f};
    if (vkr_ui_panel_begin(ui, string8_lit("row"), &row)) {
      const VkrScene *scene = vkr_editor_entity_scene(frame, issue->entity);
      const String8 name = scene && vkr_scene_entity_alive(scene, issue->entity)
                               ? vkr_scene_get_name(scene, issue->entity)
                               : (String8){0};
      /* Steps, headroom and gaps are meters; slopes are degrees; z-fighting
         is the shared area, between two named solids. */
      char value[32] = {0};
      String8 other = {0};
      if (issue->kind == VKR_EDITOR_LEVEL_TOO_STEEP) {
        snprintf(value, sizeof(value), "%.0f deg", issue->value);
      } else if (issue->kind <= VKR_EDITOR_LEVEL_TOO_NARROW) {
        snprintf(value, sizeof(value), "%.2f m", issue->value);
      } else if (issue->kind == VKR_EDITOR_LEVEL_Z_FIGHT ||
                 issue->kind == VKR_EDITOR_LEVEL_MOVER_TIMING) {
        snprintf(value, sizeof(value),
                 issue->kind == VKR_EDITOR_LEVEL_Z_FIGHT ? "%.3g m2" : "%.2f s",
                 issue->value);
        if (issue->other.u64 != issue->entity.u64 && scene &&
            vkr_scene_entity_alive(scene, issue->other)) {
          other = vkr_scene_get_name(scene, issue->other);
        }
      }
      char text[200];
      snprintf(text, sizeof(text), "%s  %s  %.*s%s%.*s",
               level_issue_label(issue->kind), value,
               (int)Min(name.length, 60u), (const char *)name.str,
               other.length ? " / " : "", (int)Min(other.length, 60u),
               other.length ? (const char *)other.str : "");
      VkrUiWidgetConfig label =
          vkr_editor_text_config(theme->font_body, theme->text);
      label.placement = (VkrUiPlacement){.column = 0u,
                                         .row = 0u,
                                         .column_span = 1u,
                                         .row_span = 1u,
                                         .justify = VKR_UI_ALIGN_START,
                                         .align = VKR_UI_ALIGN_CENTER};
      vkr_ui_label(
          ui, string8_lit("label"),
          string8_create_from_cstr((const uint8_t *)text, strlen(text)),
          &label);
      if (level_button(editor, ui, string8_lit("focus"), string8_lit("Focus"),
                       1u, false_v, false_v) &&
          frame->view_request) {
        frame->view_request->frame_box = true_v;
        frame->view_request->frame_min =
            vec3_sub(issue->position, vec3_new(1.5f, 1.5f, 1.5f));
        frame->view_request->frame_max =
            vec3_add(issue->position, vec3_new(1.5f, 1.5f, 1.5f));
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

// =============================================================================
// Level Design palette
// =============================================================================

static void palette_entity(VkrEntityId entity, char *out, uint64_t size) {
  snprintf(out, size, "%u:%u:%u", (unsigned)entity.parts.world,
           (unsigned)entity.parts.index, (unsigned)entity.parts.generation);
}

/* Runs one brush operation on the selection through the agent queue, as
   one undo step, without review: the designer asked for it. */
static void palette_op(VkrEditorUi *editor, const char *op, const char *key,
                       VkrEntityId entity, const char *extra) {
  char id[48];
  palette_entity(entity, id, sizeof(id));
  char line[512];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"palette\",\"op\":\"%s\",\"args\":{\"%s\":\"%s\""
           "%s,\"review\":false}}",
           op, key, id, extra ? extra : "");
  (void)vkr_editor_agent_submit(editor->agent, line);
}

/* The swatch of material file `path`, the Grid one for an empty path, or
   VKR_EDITOR_BRUSH_MATERIAL_COUNT for a material no swatch paints. */
static uint32_t palette_material_index(const char *path) {
  if (!path[0]) {
    return 0u;
  }
  for (uint32_t i = 0; i < VKR_EDITOR_BRUSH_MATERIAL_COUNT; ++i) {
    if (strcmp(path, vkr_editor_brush_materials[i].path) == 0) {
      return i;
    }
  }
  return VKR_EDITOR_BRUSH_MATERIAL_COUNT;
}

/* The swatch every face of `brush` shows, or VKR_EDITOR_BRUSH_MATERIAL_COUNT
   when they differ. */
static uint32_t palette_brush_material(const VkrScene *scene,
                                       VkrEntityId brush) {
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      Min(vkr_scene_brush_faces(scene, brush, faces, ArrayCount(faces)),
          (uint32_t)ArrayCount(faces));
  uint32_t shared = VKR_EDITOR_BRUSH_MATERIAL_COUNT;
  for (uint32_t i = 0; i < count; ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    const uint32_t index = face ? palette_material_index(face->material)
                                : VKR_EDITOR_BRUSH_MATERIAL_COUNT;
    if (i > 0u && index != shared) {
      return VKR_EDITOR_BRUSH_MATERIAL_COUNT;
    }
    shared = index;
  }
  return shared;
}

void vkr_editor_level_palette_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  VkrEditorPalette palette =
      vkr_editor_palette_begin(editor, frame, bounds, 4.0f);
  /* The selection as the operations read it: a face, or a brush. */
  const VkrScene *scene =
      vkr_editor_entity_scene(frame, frame->selected_entity);
  const bool8_t alive =
      scene && vkr_scene_entity_alive(scene, frame->selected_entity);
  const SceneBrushFace *face =
      alive ? vkr_scene_get_typed(scene, frame->selected_entity,
                                  &vkr_scene_brush_face_type)
            : NULL;
  VkrEntityId brush = VKR_ENTITY_ID_INVALID;
  if (face) {
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, frame->selected_entity, scene->comp_transform);
    brush = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  } else if (alive && vkr_scene_get_typed(scene, frame->selected_entity,
                                          &vkr_scene_brush_type)) {
    brush = frame->selected_entity;
  }
  const bool8_t ops = editor->agent != NULL;

  vkr_editor_palette_heading(&palette, string8_lit("palette.draw"),
                             string8_lit("DRAW AND CREATE"));
  vkr_editor_palette_command(&palette, string8_lit("palette.draw_box"),
                             "Draw box", VKR_UI_ICON_PENCIL_LINE,
                             CMD_BRUSH_DRAW, editor->brush_draw);
  vkr_editor_palette_create(&palette, string8_lit("palette.box"), "Box",
                            VKR_UI_ICON_SHAPES, "brush_box");
  vkr_editor_palette_create(&palette, string8_lit("palette.wedge"), "Wedge",
                            VKR_UI_ICON_ANGLE, "brush_wedge");
  vkr_editor_palette_create(&palette, string8_lit("palette.cylinder"),
                            "Cylinder", VKR_UI_ICON_CIRCLE, "brush_cylinder");
  vkr_editor_palette_create(&palette, string8_lit("palette.room"), "Room",
                            VKR_UI_ICON_BOUNDING_BOX, "blockout_room");
  vkr_editor_palette_create(&palette, string8_lit("palette.trigger"), "Trigger",
                            VKR_UI_ICON_LIGHTNING, "trigger_volume");
  const VkrEditorSceneTool running = vkr_editor_scene_tool(editor);
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.stairs"), "Stairs",
          VKR_UI_ICON_CHART_BAR,
          string8_lit("Click where stairs start, then where their run ends, "
                      "then raise them to their height and click"),
          running == VKR_EDITOR_SCENE_TOOL_STAIRS, !ops)) {
    vkr_editor_scene_tool_set(editor, running == VKR_EDITOR_SCENE_TOOL_STAIRS
                                          ? VKR_EDITOR_SCENE_TOOL_NONE
                                          : VKR_EDITOR_SCENE_TOOL_STAIRS);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.corridor"), "Corridor",
          VKR_UI_ICON_ARROW_RIGHT,
          string8_lit("Click the corridor's points; click the last one again "
                      "or press Enter to build it"),
          running == VKR_EDITOR_SCENE_TOOL_CORRIDOR, !ops)) {
    vkr_editor_scene_tool_set(editor, running == VKR_EDITOR_SCENE_TOOL_CORRIDOR
                                          ? VKR_EDITOR_SCENE_TOOL_NONE
                                          : VKR_EDITOR_SCENE_TOOL_CORRIDOR);
  }

  /* The kind of the next stairs and the shape of the next corridor. */
  vkr_editor_palette_heading(&palette, string8_lit("palette.paths"),
                             string8_lit("STAIRS AND CORRIDORS"));
  static const char *const stairs_labels[SCENE_STAIRS_KIND_COUNT] = {
      "Straight", "L turn", "U turn", "Curved", "Spiral"};
  static const VkrUiIcon stairs_icons[SCENE_STAIRS_KIND_COUNT] = {
      VKR_UI_ICON_CHART_BAR, VKR_UI_ICON_ARROW_RIGHT, VKR_UI_ICON_ARROW_UP,
      VKR_UI_ICON_WAVES, VKR_UI_ICON_CIRCLE};
  for (uint32_t i = 0; i < SCENE_STAIRS_KIND_COUNT; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_editor_palette_button(
            &palette, string8_lit("palette.stairs_kind"), stairs_labels[i],
            stairs_icons[i],
            string8_lit("Straight; two flights turning at a landing; two "
                        "flights running back; steps along an arc; or steps "
                        "around a pole"),
            editor->stairs_kind == i, false_v)) {
      editor->stairs_kind = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.stairs_left"), "Turn left",
          VKR_UI_ICON_ARROW_LEFT,
          string8_lit("Turning and spiral stairs turn left instead of right"),
          editor->stairs_left, false_v)) {
    editor->stairs_left = !editor->stairs_left;
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.corridor_curved"), "Curved",
          VKR_UI_ICON_WAVES,
          string8_lit("New corridors round their corners with 2 m arcs; "
                      "Details sets the radius afterwards"),
          editor->corridor_curved, false_v)) {
    editor->corridor_curved = !editor->corridor_curved;
  }

  /* The role of the next box, wedge, cylinder or stairs. */
  vkr_editor_palette_heading(&palette, string8_lit("palette.role"),
                             string8_lit("NEW BRUSH ROLE"));
  static const char *const role_labels[VKR_EDITOR_BRUSH_ROLE_COUNT] = {
      "Solid", "Visual", "Clip", "Trigger"};
  static const VkrUiIcon role_icons[VKR_EDITOR_BRUSH_ROLE_COUNT] = {
      VKR_UI_ICON_SHAPES, VKR_UI_ICON_EYE, VKR_UI_ICON_COLLIDER,
      VKR_UI_ICON_LIGHTNING};
  for (uint32_t i = 0; i < VKR_EDITOR_BRUSH_ROLE_COUNT; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_editor_palette_button(
            &palette, string8_lit("palette.role_button"), role_labels[i],
            role_icons[i],
            string8_lit("Solid collides and draws; visual only draws; clip "
                        "only collides; trigger reports what enters"),
            editor->brush_role == i, false_v)) {
      editor->brush_role = i;
    }
    (void)vkr_ui_pop_id(ui);
  }

  vkr_editor_palette_heading(&palette, string8_lit("palette.edit"),
                             string8_lit("EDIT"));
  vkr_editor_palette_command(
      &palette, string8_lit("palette.select"), "Select", VKR_UI_ICON_SELECT,
      CMD_TOOL_SELECT, frame->view_state.gizmo_tool == VKR_GIZMO_MODE_NONE);
  vkr_editor_palette_command(
      &palette, string8_lit("palette.move"), "Move", VKR_UI_ICON_MOVE,
      CMD_TOOL_MOVE, frame->view_state.gizmo_tool == VKR_GIZMO_MODE_TRANSLATE);
  vkr_editor_palette_command(&palette, string8_lit("palette.clip"), "Clip",
                             VKR_UI_ICON_RULER, CMD_BRUSH_CLIP,
                             editor->clip_tool);
  const float32_t grid = frame->view_state.grid_spacing > 0.0f
                             ? frame->view_state.grid_spacing
                             : 1.0f;
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.extrude"), "Extrude",
          VKR_UI_ICON_ARROW_UP,
          string8_lit("Grow a new brush out of the selected face "
                      "by one grid step (Alt+click selects a "
                      "face)"),
          false_v, !ops || !face)) {
    char extra[48];
    snprintf(extra, sizeof(extra), ",\"distance\":%g,\"select\":true", grid);
    palette_op(editor, "brush.extrude", "face", frame->selected_entity, extra);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.hollow"), "Hollow", VKR_UI_ICON_SQUARE,
          string8_lit("Turn the selected brush into walls around "
                      "its inside"),
          false_v, !ops || !brush.u64)) {
    palette_op(editor, "brush.hollow", "brush", brush, NULL);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.carve"), "Carve",
          VKR_UI_ICON_SELECTION,
          string8_lit("Subtract the selected brush from every "
                      "brush it touches, then delete it"),
          false_v, !ops || !brush.u64)) {
    palette_op(editor, "brush.carve", "cutter", brush, NULL);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.doorway"), "Doorway",
          VKR_UI_ICON_FRAME,
          string8_lit("Cut the patch selected on the brush's grid (drag "
                      "across cells) through it, as a door or window; "
                      "without a patch, a 1 x 2.125 m doorway through the "
                      "middle of a box wall"),
          false_v, !ops || !brush.u64) &&
      !vkr_editor_brush_grid_patch_cut(editor)) {
    palette_op(editor, "blockout.doorway", "wall", brush, NULL);
  }
  /* Merge joins the selected brushes (Ctrl+click adds them). */
  VkrEntityId selection[VKR_EDITOR_SELECTION_MAX];
  const uint32_t selected = vkr_editor_selection_list(editor, frame, selection,
                                                      ArrayCount(selection));
  char brushes[VKR_EDITOR_SELECTION_MAX * 32u] = "";
  char objects[VKR_EDITOR_SELECTION_MAX * 32u] = "";
  uint32_t brush_count = 0u;
  uint32_t free_count = 0u;
  uint64_t used = 0u;
  uint64_t objects_used = 0u;
  for (uint32_t i = 0u; i < selected; ++i) {
    const VkrScene *owner = vkr_editor_entity_scene(frame, selection[i]);
    char id[32];
    snprintf(id, sizeof(id), "\"%u:%u:%u\"", (unsigned)selection[i].parts.world,
             (unsigned)selection[i].parts.index,
             (unsigned)selection[i].parts.generation);
    objects_used += (uint64_t)snprintf(objects + objects_used,
                                       sizeof(objects) - objects_used, "%s%s",
                                       i ? "," : "", id);
    free_count += vkr_editor_entity_free(owner, selection[i]) ? 1u : 0u;
    if (owner &&
        vkr_scene_get_typed(owner, selection[i], &vkr_scene_brush_type)) {
      used += (uint64_t)snprintf(brushes + used, sizeof(brushes) - used, "%s%s",
                                 brush_count ? "," : "", id);
      brush_count++;
    }
  }
  /* Mover groups the selection under a new entity that opens and closes
     it in play (ADR-084). */
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.mover"), "Mover", VKR_UI_ICON_ARROW_UP,
          string8_lit("Group the selected objects under a mover: in play its "
                      "open and close inputs slide them along its direction, "
                      "colliding as they go; Details sets how"),
          false_v, !ops || !selected)) {
    char line[VKR_EDITOR_SELECTION_MAX * 32u + 160u];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"palette\",\"op\":\"mover.create\",\"args\":"
             "{\"objects\":[%s],\"review\":false,\"select\":true}}",
             objects);
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.merge"), "Merge", VKR_UI_ICON_LAYERS,
          string8_lit("Join 2 to 8 selected touching brushes into one when "
                      "their union is convex; Ctrl+click selects more"),
          false_v, !ops || brush_count < 2u || brush_count > 8u)) {
    char line[VKR_EDITOR_SELECTION_MAX * 32u + 160u];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"palette\",\"op\":\"brush.merge\",\"args\":"
             "{\"brushes\":[%s],\"review\":false,\"select\":true}}",
             brushes);
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.snap_together"), "Snap",
          VKR_UI_ICON_SNAP,
          string8_lit("Move the other selected objects flush against and "
                      "lined up with the first one, free ones excepted; "
                      "Ctrl+click selects more"),
          false_v, !ops || selected < 2u)) {
    char line[VKR_EDITOR_SELECTION_MAX * 32u + 160u];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"palette\",\"op\":\"brush.snap\",\"args\":"
             "{\"brushes\":[%s],\"review\":false}}",
             objects);
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  /* Free marks the selection to stay out of every snap, or clears it. */
  const bool8_t all_free = selected && free_count == selected;
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.free"), "Free", VKR_UI_ICON_HAND,
          string8_lit("The selected objects never snap: the magnet, Snap and "
                      "parenting in the Outliner leave them where they are"),
          all_free, !ops || !selected)) {
    char line[VKR_EDITOR_SELECTION_MAX * 120u + 160u];
    int written = snprintf(line, sizeof(line),
                           "{\"v\":1,\"id\":\"palette\",\"op\":\"batch\","
                           "\"args\":{\"review\":false,\"ops\":[");
    uint32_t added = 0u;
    for (uint32_t i = 0u;
         i < selected && written > 0 && (size_t)written < sizeof(line); ++i) {
      const VkrScene *owner = vkr_editor_entity_scene(frame, selection[i]);
      const bool8_t has =
          owner && vkr_scene_get_typed(owner, selection[i],
                                       &vkr_scene_free_placement_type) != NULL;
      /* Clearing removes the marker; marking adds it where it is missing
         and enables it where it is off. */
      const char *op = all_free ? "component.remove"
                       : has    ? "component.set"
                                : "component.add";
      if (all_free && !has) {
        continue;
      }
      written +=
          snprintf(line + written, sizeof(line) - (size_t)written,
                   "%s{\"op\":\"%s\",\"args\":{\"entity\":\"%u:%u:%u\","
                   "\"type\":\"free_placement\"%s}}",
                   added ? "," : "", op, (unsigned)selection[i].parts.world,
                   (unsigned)selection[i].parts.index,
                   (unsigned)selection[i].parts.generation,
                   has && !all_free ? ",\"values\":{\"enabled\":true}" : "");
      added++;
    }
    if (added && written > 0 && (size_t)written < sizeof(line)) {
      snprintf(line + written, sizeof(line) - (size_t)written, "]}}");
      (void)vkr_editor_agent_submit(editor->agent, line);
    }
  }
  const bool8_t shape_selected =
      alive && vkr_scene_get_typed(scene, frame->selected_entity,
                                   &vkr_scene_blockout_type) != NULL;
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.bake"), "Bake", VKR_UI_ICON_LAYERS,
          string8_lit("Turn the selected stairs or corridor into plain "
                      "brushes to edit one by one; its handles and settings "
                      "go"),
          false_v, !ops || !shape_selected)) {
    char line[256];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"palette\",\"op\":\"blockout.bake\","
             "\"args\":{\"entity\":\"%u:%u:%u\",\"review\":false}}",
             (unsigned)frame->selected_entity.parts.world,
             (unsigned)frame->selected_entity.parts.index,
             (unsigned)frame->selected_entity.parts.generation);
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  vkr_editor_palette_command(&palette, string8_lit("palette.duplicate"),
                             "Duplicate", VKR_UI_ICON_DUPLICATE, CMD_DUPLICATE,
                             false_v);
  vkr_editor_palette_command(&palette, string8_lit("palette.delete"), "Delete",
                             VKR_UI_ICON_TRASH, CMD_DELETE, false_v);

  /* A swatch paints the selected face alone, else every face of the
     selected brush, and new brushes take it; without a selection it only
     picks the material of new brushes. It shows the selection's material,
     or the new brushes' one. */
  vkr_editor_palette_heading(&palette, string8_lit("palette.materials"),
                             string8_lit("MATERIAL"));
  const uint32_t shown = face        ? palette_material_index(face->material)
                         : brush.u64 ? palette_brush_material(scene, brush)
                                     : editor->brush_material;
  for (uint32_t i = 0; i < VKR_EDITOR_BRUSH_MATERIAL_COUNT; ++i) {
    const VkrEditorBrushMaterial *swatch = &vkr_editor_brush_materials[i];
    (void)vkr_ui_push_id_u64(ui, i);
    const bool8_t clicked = vkr_editor_palette_button(
        &palette, string8_lit("palette.swatch"), swatch->label,
        VKR_UI_ICON_MATERIAL,
        face        ? string8_lit("Paint the selected face; new brushes "
                                         "take this material too")
        : brush.u64 ? string8_lit("Paint every face of the selected brush; "
                                  "new brushes take this material too")
                    : string8_lit("New brushes take this material; select a "
                                  "brush or face to paint it"),
        shown == i, brush.u64 && !face && !ops);
    (void)vkr_ui_pop_id(ui);
    if (!clicked) {
      continue;
    }
    editor->brush_material = i;
    if (!brush.u64) {
      continue;
    }
    if (face) {
      VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_APPLY,
                                     .entity = frame->selected_entity};
      request.values.fields = VKR_SCENE_EDIT_COMPONENT;
      request.values.component_type = &vkr_scene_brush_face_type;
      SceneBrushFace *value = (SceneBrushFace *)request.values.component;
      *value = *face;
      snprintf(value->material, sizeof(value->material), "%s", swatch->path);
      *frame->scene_edit = request;
    } else {
      char extra[96];
      snprintf(extra, sizeof(extra), ",\"material\":\"%s\"", swatch->path);
      palette_op(editor, "brush.set_material", "brush", brush, extra);
    }
  }

  vkr_editor_palette_heading(&palette, string8_lit("palette.snap"),
                             string8_lit("SNAPPING"));
  static const char *const snap_labels[VKR_EDITOR_SNAP_COUNT] = {
      "Free", "Surface", "Grid"};
  static const VkrUiIcon snap_icons[VKR_EDITOR_SNAP_COUNT] = {
      VKR_UI_ICON_HAND, VKR_UI_ICON_SNAP, VKR_UI_ICON_GRID};
  for (uint32_t i = 0; i < VKR_EDITOR_SNAP_COUNT; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_editor_palette_button(
            &palette, string8_lit("palette.snap_target"), snap_labels[i],
            snap_icons[i], string8_lit("Where drawn and created objects land"),
            editor->placement.target == i, false_v)) {
      editor->placement.target = (VkrEditorSnapTarget)i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.magnet"), "Magnet", VKR_UI_ICON_SNAP,
          string8_lit("Moved and drawn brushes snap flush against and level "
                      "with nearby brushes"),
          editor->placement.magnet, false_v)) {
    editor->placement.magnet = !editor->placement.magnet;
  }
  char step[48];
  snprintf(step, sizeof(step), "GRID STEP %g M", grid);
  vkr_editor_palette_heading(
      &palette, string8_lit("palette.grid_step"),
      string8_create_from_cstr((const uint8_t *)step, strlen(step)));
  if (vkr_editor_palette_button(
          &palette, string8_lit("palette.grid"), "Show grid", VKR_UI_ICON_EYE,
          string8_lit("Show or hide the grid"), frame->view_state.grid_enabled,
          !frame->view_request)) {
    VkrSampleViewState next = frame->view_state;
    next.grid_enabled = !next.grid_enabled;
    *frame->view_request =
        (VkrSampleViewRequest){.value = next, .apply = true_v};
  }
  /* Grid steps halve and double between 1/16 m and 64 m. */
  const bool8_t finer = vkr_editor_palette_button(
      &palette, string8_lit("palette.finer"), "Finer", VKR_UI_ICON_ZOOM_IN,
      string8_lit("Halve the grid step"), false_v,
      !frame->view_request || grid <= 0.0625f);
  const bool8_t coarser = vkr_editor_palette_button(
      &palette, string8_lit("palette.coarser"), "Coarser", VKR_UI_ICON_ZOOM_OUT,
      string8_lit("Double the grid step"), false_v,
      !frame->view_request || grid >= 64.0f);
  if (finer || coarser) {
    VkrSampleViewState next = frame->view_state;
    next.grid_spacing =
        vkr_clamp_f32(finer ? grid * 0.5f : grid * 2.0f, 0.0625f, 64.0f);
    *frame->view_request =
        (VkrSampleViewRequest){.value = next, .apply = true_v};
  }

  vkr_editor_palette_heading(&palette, string8_lit("palette.check"),
                             string8_lit("CHECK"));
  vkr_editor_palette_command(&palette, string8_lit("palette.checks"),
                             "Level checks", VKR_UI_ICON_PERSON_WALK,
                             CMD_LEVEL_CHECKS, false_v);
  vkr_editor_palette_end(&palette);
}
