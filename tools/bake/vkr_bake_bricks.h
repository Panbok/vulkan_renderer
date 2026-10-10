#pragma once

#include "assets/vkr_diffuse_volume.h"
#include "vkr_bake_bvh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fraction of a probe's visibility rays that may hit back faces before the
   probe counts as inside geometry. */
#define VKR_BAKE_BRICK_INSIDE_BACKFACE_FRACTION 0.25f
/* Visibility rays per interior moment texel. */
#define VKR_BAKE_BRICK_RAYS_PER_TEXEL 16u
/* Relocation attempts before a probe still inside geometry is invalid. */
#define VKR_BAKE_BRICK_RELOCATION_STEPS 3u

/** How the bake treats one material in probe placement and visibility. */
typedef struct VkrBakeBrickMaterial {
  /** False for surfaces light passes, such as non-transmissive blending. */
  bool8_t blocks;
  /** A double-sided surface has no back face. */
  bool8_t double_sided;
} VkrBakeBrickMaterial;

/**
 * Sparse brick placement (ADR-054). The indirection
 * grid covers `bounds` with one entry per level-0 brick span, three probe
 * spacings; its dimensions round up to whole top-level blocks. A level-k
 * brick spans 3^k entries per axis. A block refines into its 27 children
 * where blocking geometry lies within the child's box grown by
 * `margin_spans` of the child's brick span; a block with at least one
 * child left unrefined stores one brick of its own level for those entries.
 */
typedef struct VkrBakeBrickDesc {
  VkrBakeAabb bounds;
  float32_t spacing;
  uint32_t level_count;
  float32_t margin_spans;
} VkrBakeBrickDesc;

/**
 * One distinct probe position of the layout. A probe shared by several
 * bricks of one level, such as one on a common face, is baked once. Its
 * lattice coordinate counts finest spacings from the layout origin.
 */
typedef struct VkrBakeBrickProbe {
  uint32_t lattice[3];
  uint32_t level;
} VkrBakeBrickProbe;

/**
 * Arrays live in the Arena given to vkr_bake_bricks_place. `brick_probes`
 * maps brick b's probe i (x + 4 * (y + 4 * z)) at `b * 64 + i` to its index
 * in `probes`.
 */
typedef struct VkrBakeBrickLayout {
  Vec3 origin;
  float32_t spacing;
  uint32_t level_count;
  uint32_t dimensions[3];
  uint32_t *entries;
  uint32_t entry_count;
  VkrDiffuseVolumeBrick *bricks;
  uint32_t brick_count;
  uint32_t *brick_probes;
  VkrBakeBrickProbe *probes;
  uint32_t probe_count;
} VkrBakeBrickLayout;

/** Visibility of one probe after relocation. */
typedef struct VkrBakeBrickVisibility {
  /** World offset from the lattice position, at most half the probe's
      spacing on each axis. */
  Vec3 offset;
  bool8_t valid;
  float32_t backface_fraction;
  /** Mean and mean square of first-hit distances per moment texel, as
      VKR_DIFFUSE_VOLUME_MOMENT_TEXELS RG pairs with the border filled. */
  float32_t moments[VKR_DIFFUSE_VOLUME_MOMENT_TEXELS * 2u];
} VkrBakeBrickVisibility;

/**
 * Places bricks over `desc.bounds`. False when the bounds are not finite
 * with positive extent, the spacing or level count is out of range, or the
 * grid or brick count exceeds the DVOL limits.
 */
bool8_t vkr_bake_bricks_place(const VkrBakeBvh *bvh,
                              const VkrBakeBrickMaterial *materials,
                              uint32_t material_count, VkrBakeBrickDesc desc,
                              Arena *arena, VkrBakeBrickLayout *out_layout);

/** World position of a layout probe before relocation. */
Vec3 vkr_bake_bricks_probe_position(const VkrBakeBrickLayout *layout,
                                    uint32_t probe);

/** Probe spacing of a layout probe's level. */
float32_t vkr_bake_bricks_probe_spacing(const VkrBakeBrickLayout *layout,
                                        uint32_t probe);

/**
 * Relocates every layout probe out of geometry and measures its distance
 * moments (`out_visibility[probe]`), on `thread_count` workers (zero for
 * every hardware thread). Each probe's rays depend only on the probe, so the
 * result is the same for any worker count.
 */
bool8_t vkr_bake_bricks_visibility(const VkrBakeBvh *bvh,
                                   const VkrBakeBrickMaterial *materials,
                                   uint32_t material_count,
                                   const VkrBakeBrickLayout *layout,
                                   uint32_t thread_count,
                                   VkrBakeBrickVisibility *out_visibility);

/**
 * Removes bricks whose probes are all invalid, renumbering the rest and
 * pointing their entries at VKR_DIFFUSE_VOLUME_ENTRY_EMPTY. Probe records
 * are kept; a dropped brick's probes may be shared with stored bricks.
 */
void vkr_bake_bricks_drop_invalid(VkrBakeBrickLayout *layout,
                                  const VkrBakeBrickVisibility *visibility);

#ifdef __cplusplus
}
#endif
