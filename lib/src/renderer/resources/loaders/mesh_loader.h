#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "core/vkr_job_system.h"
#include "math/vkr_transform.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Mesh Loader Types
// =============================================================================

typedef struct VkrMeshLoaderSubset {
  VkrGeometryConfig geometry_config;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  VkrMaterialHandle material_handle;
} VkrMeshLoaderSubset;
Array(VkrMeshLoaderSubset);

/**
 * @brief CPU-side mesh buffer payload owned by the loader arena.
 *
 * The data pointers remain valid until the loader result is unloaded.
 */
typedef struct VkrMeshLoaderBuffer {
  uint32_t vertex_size;
  uint32_t vertex_count;
  void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  void *indices;
} VkrMeshLoaderBuffer;

/**
 * @brief Range metadata for a submesh inside a merged mesh buffer.
 *
 * Bounds are stored as center + min/max extents in mesh-local space.
 */
typedef struct VkrMeshLoaderSubmeshRange {
  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  VkrMaterialHandle material_handle;
} VkrMeshLoaderSubmeshRange;
Array(VkrMeshLoaderSubmeshRange);

typedef struct VkrMeshLoaderResult {
  Arena *arena;           /**< Buffer-backed arena for mesh data */
  void *pool_chunk;       /**< Chunk pointer for returning to pool (NULL if not
                             pooled) */
  VkrAllocator allocator; /**< Arena allocator wrapper (used for accounting) */
  String8 source_path;
  VkrTransform root_transform;
  bool8_t
      has_mesh_buffer; /**< True when mesh_buffer/submeshes are populated. */
  VkrMeshLoaderBuffer mesh_buffer; /**< Merged vertex/index payload. */
  Array_VkrMeshLoaderSubmeshRange submeshes; /**< Per-submesh ranges. */
  Array_VkrMeshLoaderSubset subsets;
} VkrMeshLoaderResult;

typedef struct VkrMeshLoaderContext {
  VkrAllocator allocator;
  VkrDMemory async_memory;
  VkrAllocator async_allocator;
  VkrMutex async_mutex;
  VkrGeometrySystem *geometry_system;
  VkrMaterialSystem *material_system;
  VkrMeshManager *mesh_manager;
  VkrJobSystem *job_system; /**< For async mesh loading */
  VkrArenaPool *arena_pool; /**< Pool for mesh loading arenas (optional) */
} VkrMeshLoaderContext;

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a mesh resource loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single meshes and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @param context The mesh loader context (stored as resource_system pointer)
 * @return The configured resource loader
 */
VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context);
