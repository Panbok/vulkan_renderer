#pragma once

#include "defines.h"
#include "math/vec.h"

/* Blockout shapes (ADR-084): editable stairs and corridors that their
 * settings describe, laid out as convex pieces, each the hull of its points.
 * The scene builds a shape's mesh and collision from its pieces
 * (vkr_scene_brush.h), the editor draws them as previews and Bake turns them
 * into brushes. The module owns no state. */

/* An editable blockout shape's settings, the `blockout` component. */
#define SCENE_BLOCKOUT_POINT_MAX 16u
#define SCENE_BLOCKOUT_OPENING_MAX 8u
#define SCENE_BLOCKOUT_MATERIAL_CAPACITY 160u

typedef enum SceneBlockoutShape {
  SCENE_BLOCKOUT_STAIRS = 0,
  SCENE_BLOCKOUT_CORRIDOR,
  SCENE_BLOCKOUT_SHAPE_COUNT,
} SceneBlockoutShape;

typedef enum SceneStairsKind {
  SCENE_STAIRS_STRAIGHT = 0,
  /* Two flights joined by a square landing, the second turned 90 degrees. */
  SCENE_STAIRS_L,
  /* Two flights joined by a landing, the second running back beside the
     first. */
  SCENE_STAIRS_U,
  /* Steps along an arc around a center beside the stairs. */
  SCENE_STAIRS_CURVED,
  /* Steps around a pole, as many turns as `turn` says. */
  SCENE_STAIRS_SPIRAL,
  SCENE_STAIRS_KIND_COUNT,
} SceneStairsKind;

typedef struct SceneBlockout {
  SceneBlockoutShape shape;
  SceneStairsKind stairs;
  /* Stairs: the rise; corridors: the inside height. */
  float32_t height;
  /* Stairs: the tread width; corridors: the inside width. */
  float32_t width;
  /* Straight stairs: the run; L and U stairs: the first flight with its
     landing. */
  float32_t length;
  float32_t step_height;
  /* Curved and spiral stairs: degrees swept. */
  float32_t turn;
  /* Curved and spiral stairs: the inner radius; corridors: the radius of
     the arcs at their corners, zero for sharp corners. */
  float32_t radius;
  /* Corridors: wall, floor and ceiling thickness; stairs: each step's slab
     thickness, zero for steps solid down to the floor. */
  float32_t thickness;
  /* Turning, curved and spiral stairs turn left instead of right. */
  bool8_t left;
  bool8_t ceiling;
  /* Corridors: the floor path in the group's space, and each point's corner
     radius, below zero for `radius`. */
  uint32_t point_count;
  Vec3 points[SCENE_BLOCKOUT_POINT_MAX];
  float32_t corners[SCENE_BLOCKOUT_POINT_MAX];
  /* Corridors: openings cut through their walls. Wall `walls[i]` is
     stretch `walls[i] / 2` of the built path, on side `walls[i] % 2`;
     `openings[i]` spans meters along the stretch (x to y) and above its
     floor (z to w). */
  uint32_t opening_count;
  uint32_t walls[SCENE_BLOCKOUT_OPENING_MAX];
  Vec4 openings[SCENE_BLOCKOUT_OPENING_MAX];
  char material[SCENE_BLOCKOUT_MATERIAL_CAPACITY];
  char floor_material[SCENE_BLOCKOUT_MATERIAL_CAPACITY];
} SceneBlockout;

/* Corners one piece's hull has at most. */
#define VKR_BLOCKOUT_PIECE_POINT_MAX 24u
/* Steps one stairs shape has at most, and stretches one corridor's path has
   with its rounded corners. */
#define VKR_BLOCKOUT_STEP_MAX 4096u
#define VKR_BLOCKOUT_CORRIDOR_SEGMENT_MAX 256u
/* The tallest shape, in meters, and the most degrees a spiral turns: its
   step limit at 22.5 degrees a step. */
#define VKR_BLOCKOUT_HEIGHT_MAX 1024.0f
#define VKR_BLOCKOUT_TURN_MAX 92160.0f

typedef enum VkrBlockoutPieceKind {
  VKR_BLOCKOUT_PIECE_STEP = 0,
  VKR_BLOCKOUT_PIECE_LANDING,
  VKR_BLOCKOUT_PIECE_POLE,
  VKR_BLOCKOUT_PIECE_FLOOR,
  VKR_BLOCKOUT_PIECE_WALL,
  VKR_BLOCKOUT_PIECE_CEILING,
} VkrBlockoutPieceKind;

typedef struct VkrBlockoutPiece {
  VkrBlockoutPieceKind kind;
  uint32_t point_count;
  Vec3 points[VKR_BLOCKOUT_PIECE_POINT_MAX];
} VkrBlockoutPiece;

/* Operation names of SceneStairsKind: straight, l, u, curved, spiral. */
extern const char *const vkr_blockout_stairs_kinds[];

/* The pieces vkr_blockout_layout may write for `shape`, which a caller's
   buffer holds. */
uint32_t vkr_blockout_piece_capacity(const SceneBlockout *shape);

/* The pieces of `shape` in its own space. Stairs start at the origin, the
   bottom front center (a spiral's pole), and head along +Z; each step is
   solid down to the floor, or a slab `thickness` thick. A corridor runs
   along its points, its corners rounded by their radii. Returns how many it
   wrote, or 0 with the reason in `error`. */
uint32_t vkr_blockout_layout(const SceneBlockout *shape, VkrBlockoutPiece *out,
                             uint32_t capacity, char *error,
                             uint64_t error_size);

/* The wall of corridor `shape` nearest `local` (in its space): its index
   (stretch * 2 + side, as SceneBlockout.walls), the distance along its
   stretch and the floor's height there; false when none is near. */
bool8_t vkr_blockout_corridor_wall_at(const SceneBlockout *shape, Vec3 local,
                                      uint32_t *out_wall, float32_t *out_along,
                                      float32_t *out_floor);
