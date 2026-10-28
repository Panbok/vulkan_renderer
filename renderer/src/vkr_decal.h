#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "vkr_render_resources.h"

/* Projected box decals (ADR-092): the frame's decals. Both pipelines bin
 * them on the GPU and lay each decal's base colour over the opaque surfaces
 * inside its box before lighting. */

/** Decals one frame carries: the runtime keeps those nearest the camera, at
 * most VKR_TILED_FRAME_DECALS for the tiled pipeline, the most one of its
 * tiles lists. The desktop pipeline's GPU clusters read the whole table. */
#define VKR_MAX_FRAME_DECALS 512u
#define VKR_TILED_FRAME_DECALS 64u

_Static_assert(VKR_TILED_FRAME_DECALS <= VKR_MAX_FRAME_DECALS,
               "The tiled pipeline's decals lie within the frame table");

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
  /** World-space bounds of the box, which the GPU binning culls by. */
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

/** Places `decal`'s box where the affine `box_to_world` maps [-0.5, 0.5] on
 * each axis: sets `world_to_box` and the bounds. False, leaving `decal`
 * unchanged, when the box has no volume or is not finite. */
bool8_t vkr_decal_set_box(VkrDecal *decal, Mat4 box_to_world);

/** Whether a frame may carry `decal`: a finite, invertible box with ordered
 * fades and an opacity in [0, 1]. */
bool8_t vkr_decal_valid(const VkrDecal *decal);
