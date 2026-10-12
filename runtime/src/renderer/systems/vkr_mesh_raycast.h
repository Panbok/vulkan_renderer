#pragma once

#include "math/vec.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_mesh_manager.h"

/* CPU ray tests against the triangles a mesh instance draws. Imported meshes
 * such as Bistro's carry no physics collision and their vertices live only on
 * the GPU after upload, so editor surface tools and scatter placement read
 * LOD 0 positions back from the asset's cooked mesh file. The cache keeps
 * each asset's triangles and a bounding volume hierarchy over them, keyed by
 * the asset handle and its mesh path, and evicts the least recently used
 * assets past its byte budget.
 *
 * Main thread only: the cache and the mesh manager it reads are not
 * synchronized. */

typedef struct VkrMeshRaycastHit {
  Vec3 position;      /* world space */
  Vec3 normal;        /* world space, unit, facing against the ray */
  float32_t fraction; /* along `delta`, in [0, 1] */
} VkrMeshRaycastHit;

typedef struct s_VkrMeshRaycastCache VkrMeshRaycastCache;

/* Created once per VkrMeshManager owner. `allocator` must free individual
 * blocks (DMemory or the system heap) and outlive the cache; `budget_bytes`
 * bounds the CPU triangle memory. Returns NULL when allocation fails. */
VkrMeshRaycastCache *vkr_mesh_raycast_cache_create(VkrAllocator *allocator,
                                                   uint64_t budget_bytes);
void vkr_mesh_raycast_cache_destroy(VkrMeshRaycastCache *cache);

/* Releases the held decoded cooked file once no asset has extracted from it
 * across trims spanning more than 10 seconds of `now_seconds`. Call once per
 * frame with a monotonic time. */
void vkr_mesh_raycast_cache_trim(VkrMeshRaycastCache *cache,
                                 float64_t now_seconds);

/* Cooked files the cache has decoded since creation, for tests. */
uint32_t vkr_mesh_raycast_cache_decode_count(const VkrMeshRaycastCache *cache);

/* Nearest hit of the segment origin..origin+delta on the LOD 0 triangles of
 * `instance` (its asset's geometry moved by instance->model), both faces. The
 * first call for an asset reads its triangles from the asset's cooked mesh
 * file synchronously and builds a BVH; later calls reuse them and allocate
 * nothing. The cache holds the most recently decoded file outside the budget
 * (about 88 MB for Bistro's 31 MB file, near 120 MB at the decode's peak),
 * so further assets of that file extract their source mesh without decoding
 * it again; another file's decode, trim or destroy releases it. False when
 * the segment misses, the instance or asset is stale or not loaded, the
 * asset has no cooked mesh file, or the file cannot be decoded (logged once
 * per asset). A skinned instance is tested in its bind pose. */
bool8_t vkr_mesh_raycast_instance(VkrMeshRaycastCache *cache,
                                  VkrMeshManager *meshes,
                                  VkrMeshInstanceHandle instance, Vec3 origin,
                                  Vec3 delta, VkrMeshRaycastHit *out_hit);

/* For tests and generated callers: sets the triangles of the live asset
 * `asset` in `meshes` from local-space `positions` and `indices` (three per
 * triangle, each below `position_count`) instead of decoding its file. They
 * replace any cached triangles of that asset and count toward the budget.
 * False for a stale asset, invalid indices or failed allocation. */
bool8_t vkr_mesh_raycast_cache_insert_triangles(
    VkrMeshRaycastCache *cache, VkrMeshManager *meshes,
    VkrMeshAssetHandle asset, const VkrPackedVec3 *positions,
    uint32_t position_count, const uint32_t *indices, uint32_t index_count);
