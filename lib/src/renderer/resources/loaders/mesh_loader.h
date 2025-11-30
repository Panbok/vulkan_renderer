#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "core/vkr_job_system.h"
#include "math/vkr_transform.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"

typedef struct VkrMeshLoaderSubset {
  VkrGeometryConfig geometry_config;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  VkrMaterialHandle material_handle;
} VkrMeshLoaderSubset;
Array(VkrMeshLoaderSubset);

typedef struct VkrMeshLoaderResult {
  Arena *arena;
  String8 source_path;
  VkrTransform root_transform;
  Array_VkrMeshLoaderSubset subsets;
} VkrMeshLoaderResult;

typedef struct VkrMeshLoaderContext {
  Arena *arena;
  Arena *scratch_arena;
  VkrAllocator allocator;
  VkrGeometrySystem *geometry_system;
  VkrMaterialSystem *material_system;
  VkrMeshManager *mesh_manager;
  VkrJobSystem *job_system; // For async mesh loading
} VkrMeshLoaderContext;

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context);

// =============================================================================
// Batch Mesh Loading API
// =============================================================================

/**
 * @brief Result of loading a single mesh in a batch operation.
 */
typedef struct VkrMeshBatchResult {
  VkrMeshLoaderResult *result;
  VkrRendererError error;
  bool8_t success;
} VkrMeshBatchResult;

/**
 * @brief Batch load multiple meshes with parallel file I/O and material
 * loading.
 *
 * This function:
 * 1. Submits all mesh file read jobs in parallel
 * 2. Parses all mesh cache/OBJ files
 * 3. Collects all material paths from all meshes
 * 4. Batch loads all materials (which batch loads all textures)
 * 5. Assigns material handles to mesh subsets
 *
 * @param context The mesh loader context
 * @param mesh_paths Array of mesh file paths to load
 * @param count Number of meshes to load
 * @param temp_arena Temporary arena for intermediate allocations
 * @param out_results Array to receive batch results (must have 'count'
 * elements)
 * @return Number of meshes successfully loaded
 */
uint32_t vkr_mesh_loader_load_batch(VkrMeshLoaderContext *context,
                                    const String8 *mesh_paths, uint32_t count,
                                    Arena *temp_arena,
                                    VkrMeshBatchResult *out_results);

/**
 * @brief Free results from a batch mesh load operation.
 *
 * @param results Array of batch results
 * @param count Number of results
 */
void vkr_mesh_loader_free_batch_results(VkrMeshBatchResult *results,
                                        uint32_t count);
