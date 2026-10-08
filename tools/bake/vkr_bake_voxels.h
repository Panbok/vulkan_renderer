#pragma once

#include "vkr_bake_bvh.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_BAKE_VOXEL_MAX_PROBES 256u
#define VKR_BAKE_VOXEL_MAX_OCCUPANCY_COUNT 8000000u
#define VKR_BAKE_VOXEL_DEFAULT_SIZE_FRACTION 0.25f
/* A fitted grid is never finer than this spacing in meters: diffuse
   irradiance changes little within a meter, so finer probes in a small scene
   only add bake time. */
#define VKR_BAKE_VOXEL_FIT_MIN_SPACING 1.0f
/* A fitted grid has at least four probes per axis. Probes sit half a spacing
   inside the bounds, and the full-cell proof reaches one voxel past a cell
   into the dilated boundary, so where geometry lies on a bounds face (a
   room's floor at the bottom of geometry-derived bounds) the outer cell on
   that side never proves clear. Four probes leave an inner cell layer. */
#define VKR_BAKE_VOXEL_FIT_MIN_DIMENSION 4u

/** `probe_dimensions` has at most `VKR_BAKE_VOXEL_MAX_PROBES` total nodes. */
typedef struct VkrBakeVoxelGridDesc {
  VkrBakeAabb bounds;
  uint32_t probe_dimensions[3];
  float32_t voxel_size;
} VkrBakeVoxelGridDesc;

typedef struct VkrBakeVoxelProbe {
  Vec3 position;
  /** Zero is exterior or insufficiently clear; interior regions begin at one.
   */
  uint32_t region_id;
} VkrBakeVoxelProbe;

/**
 * Arrays are owned by the caller-provided bake Arena. `cell_region_ids` has
 * `(probe_dimensions[0]-1) * (probe_dimensions[1]-1) *
 * (probe_dimensions[2]-1)` entries; zero marks an invalid interpolation cell.
 * A nonzero cell has a matching interior region throughout every overlapping
 * one-cell-dilated occupancy voxel.
 */
typedef struct VkrBakeVoxelResult {
  Vec3 origin;
  Vec3 spacing;
  uint32_t probe_dimensions[3];
  VkrBakeVoxelProbe *probes;
  uint32_t probe_count;
  uint32_t *cell_region_ids;
  uint32_t cell_count;
} VkrBakeVoxelResult;

/**
 * Fits probe dimensions to `bounds` when the recipe names no grid: the
 * finest near-cubic spacing, at least VKR_BAKE_VOXEL_FIT_MIN_SPACING, whose
 * grid of at least VKR_BAKE_VOXEL_FIT_MIN_DIMENSION probes per axis holds at
 * most VKR_BAKE_VOXEL_MAX_PROBES probes. False when `bounds` lacks a finite
 * positive extent on every axis.
 */
bool8_t vkr_bake_voxels_fit_grid(VkrBakeAabb bounds,
                                 uint32_t out_dimensions[3]);

/**
 * Conservative room classification. A true entry must cover opaque materials,
 * MASK materials without a sampled opacity proof, and active refractive or
 * transmissive materials. Only non-transmissive BLEND materials may be false.
 * The caller owns the resulting Arena storage through subsequent bake
 * transport reads. Each flood region is accepted once only when its
 * representative has a nearest blocking boundary on all six axis rays and
 * every boundary is front-facing toward room air; missing, grazing, mixed, or
 * outward-solid boundaries fail closed to region zero.
 */
bool8_t vkr_bake_voxels_build(const VkrBakeBvh *bvh,
                              const bool8_t *material_blocks_rooms,
                              uint32_t material_count,
                              VkrBakeVoxelGridDesc desc, Arena *arena,
                              VkrBakeVoxelResult *out_result);

#ifdef __cplusplus
}
#endif
