#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "vkr_render_resources.h"

/* Projected box decals (ADR-092): the frame's decals and their world-space
 * lookup. Only the tiled pipeline draws them; it lays each decal's base colour
 * over the opaque surfaces inside its box before lighting. */

/** Decals one frame carries: the runtime keeps those nearest the camera. */
#define VKR_MAX_FRAME_DECALS 64u
#define VKR_DECAL_GRID_MASK_WORDS 2u
#define VKR_DECAL_GRID_MAX_CELLS 256u
#define VKR_DECAL_GRID_MIN_CELL_SIZE 2.0f

_Static_assert(VKR_MAX_FRAME_DECALS == VKR_DECAL_GRID_MASK_WORDS * 32u,
               "The decal-grid mask must represent the complete frame table");

/**
 * One decal lowered for a frame. `world_to_box` maps a world position into
 * the decal's box, whose inside is [-0.5, 0.5] on each axis. The decal
 * projects along the box's -Y onto surfaces that face its +Y, as the editor
 * places objects upright on the surface under the cursor. Seen from its +Y
 * with its -Z up, the texture reads upright and unmirrored: u runs along +X
 * and v, whose zero is an image's bottom row, along -Z. Its material's base
 * colour texture times the material's
 * tint is laid over the surface's base colour, weighted by the texture's
 * alpha times `opacity`, the angle fade and the depth fade.
 */
typedef struct VkrDecal {
  Mat4 world_to_box;
  /** World-space bounds of the box, which place it in the grid. */
  Vec3 bounds_min;
  Vec3 bounds_max;
  VkrMaterialHandle material;
  /** In [0, 1]. */
  float32_t opacity;
  /** Cosines of the angle between a surface's normal and the box's +Y at
   * which the decal starts to fade and at which it is gone; start > end. */
  float32_t fade_cos_start;
  float32_t fade_cos_end;
  /** Share of the box's half depth, in [0, 1], over which the decal fades out
   * toward its near and far faces; zero cuts it off at them. */
  float32_t depth_fade;
} VkrDecal;

/** Raw 64-bit decal membership, one bit per frame table row. */
typedef struct VkrDecalMask {
  uint32_t words[VKR_DECAL_GRID_MASK_WORDS];
} VkrDecalMask;

/** Camera-independent, conservative world-space lookup: a cell holds every
 * decal whose bounds meet it. Positions outside the grid meet no decal. */
typedef struct VkrDecalGrid {
  Vec3 origin;
  float32_t cell_size;
  uint32_t dimensions[3];
  uint32_t cell_count;
  VkrDecalMask masks[VKR_DECAL_GRID_MAX_CELLS];
  uint32_t reference_count;
  uint32_t max_decals_per_cell;
} VkrDecalGrid;

/** Places `decal`'s box where the affine `box_to_world` maps [-0.5, 0.5] on
 * each axis: sets `world_to_box` and the bounds. False, leaving `decal`
 * unchanged, when the box has no volume or is not finite. */
bool8_t vkr_decal_set_box(VkrDecal *decal, Mat4 box_to_world);

/** Whether a frame may carry `decal`: a finite, invertible box with ordered
 * fades and an opacity in [0, 1]. */
bool8_t vkr_decal_valid(const VkrDecal *decal);

/** Rebuilds `grid` over the bounds of the first `count` decals; `count` is at
 * most VKR_MAX_FRAME_DECALS. */
void vkr_decal_grid_build(const VkrDecal *decals, uint32_t count,
                          VkrDecalGrid *grid);

/** The decals whose cell holds `world_position`; none outside the grid. */
VkrDecalMask vkr_decal_grid_mask_at(const VkrDecalGrid *grid,
                                    Vec3 world_position);
