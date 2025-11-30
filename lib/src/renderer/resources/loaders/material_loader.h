#pragma once

#include "containers/str.h"
#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Resource loader factory
// =============================================================================

VkrResourceLoader vkr_material_loader_create(void);

// =============================================================================
// Batch Material Loading API
// =============================================================================

/**
 * @brief Maximum path length for texture paths in parsed material data.
 * Using fixed buffers instead of String8 to avoid arena sharing issues
 * when parsing in parallel.
 */
#define VKR_MATERIAL_PATH_MAX 512

/**
 * @brief Parsed material data before textures are loaded.
 * Used for batch loading to separate parsing from GPU upload.
 * Uses fixed-size buffers to be thread-safe during parallel parsing.
 */
typedef struct VkrParsedMaterialData {
  char name[128];
  char shader_name[128];
  uint32_t pipeline_id;
  VkrPhongProperties phong;

  // Texture paths as fixed buffers (thread-safe for parallel parsing)
  char diffuse_path[VKR_MATERIAL_PATH_MAX];
  char specular_path[VKR_MATERIAL_PATH_MAX];
  char normal_path[VKR_MATERIAL_PATH_MAX];

  bool8_t parse_success;
  VkrRendererError parse_error;
} VkrParsedMaterialData;

/**
 * @brief Context for batch material loading operations.
 */
typedef struct VkrMaterialBatchContext {
  struct VkrMaterialSystem *material_system;
  VkrJobSystem *job_system;
  Arena *arena;
  Arena *temp_arena;
} VkrMaterialBatchContext;

/**
 * @brief Batch load multiple materials with parallel file parsing and texture
 * loading.
 *
 * This function:
 * 1. Parses all material files in parallel using the job system
 * 2. Collects all texture paths from all materials
 * 3. Batch loads all textures in parallel
 * 4. Creates material entries and assigns texture handles
 *
 * @param context The batch loading context
 * @param material_paths Array of material file paths to load
 * @param count Number of materials to load
 * @param out_handles Array to receive material handles (must have 'count'
 * elements)
 * @param out_errors Array to receive per-material errors (must have 'count'
 * elements)
 * @return Number of materials successfully loaded
 */
uint32_t vkr_material_loader_load_batch(VkrMaterialBatchContext *context,
                                        const String8 *material_paths,
                                        uint32_t count,
                                        VkrMaterialHandle *out_handles,
                                        VkrRendererError *out_errors);

/**
 * @brief Parse a material file without loading textures.
 * Used internally for parallel parsing.
 *
 * @param arena Arena for allocating parsed data
 * @param path Path to the material file
 * @param out_data Parsed material data output
 * @return True if parsing succeeded
 */
bool8_t vkr_material_loader_parse_file(Arena *arena, String8 path,
                                       VkrParsedMaterialData *out_data);
