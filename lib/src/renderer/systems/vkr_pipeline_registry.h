#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Configuration and Constants
// =============================================================================

#define VKR_MAX_PIPELINE_COUNT 1024
#define VKR_MAX_PIPELINES_PER_DOMAIN 64
#define VKR_PIPELINE_REGISTRY_DEFAULT_ARENA_RSV MB(16)
#define VKR_PIPELINE_REGISTRY_DEFAULT_ARENA_CMT MB(8)

typedef struct VkrPipelineRegistryConfig {
  uint32_t max_pipeline_count;
  uint32_t max_pipelines_per_domain;
} VkrPipelineRegistryConfig;

#define VKR_PIPELINE_REGISTRY_CONFIG_DEFAULT                                   \
  (VkrPipelineRegistryConfig) {                                                \
    .max_pipeline_count = VKR_MAX_PIPELINE_COUNT,                              \
    .max_pipelines_per_domain = VKR_MAX_PIPELINES_PER_DOMAIN,                  \
  }

// =============================================================================
// Pipeline Registry Data Structures
// =============================================================================

typedef struct VkrPipelineEntry {
  uint32_t id;              // Index into pipelines array + 1
  uint32_t ref_count;       // Reference counting for lifetime management
  bool8_t auto_release;     // Auto-release when ref_count hits 0
  const char *name;         // Pipeline name (hash key)
  VkrPipelineDomain domain; // Pipeline domain classification
} VkrPipelineEntry;
VkrHashTable(VkrPipelineEntry);

// Forward declarations for array types
Array(VkrPipelineHandle);

// Pipeline state tracking for optimization
typedef struct VkrPipelineState {
  VkrPipelineHandle current_pipeline; // Currently bound pipeline
  VkrPipelineDomain current_domain;   // Currently active domain
  bool8_t global_state_dirty;         // Global uniforms need update
  bool8_t pipeline_bound;             // Pipeline is currently bound
  uint32_t
      frame_pipeline_changes; // Pipeline changes this frame (for profiling)
  uint32_t frame_redundant_binds_avoided; // Redundant binds avoided this frame
} VkrPipelineState;

typedef struct VkrPipelineRegistry {
  // Core configuration and memory management
  Arena *pipeline_arena;       // Persistent allocations
  Arena *temp_arena;           // Temporary allocations during operations
  VkrAllocator allocator;      // persistent allocator wrapping pipeline_arena
  VkrAllocator temp_allocator; // scratch allocator wrapping temp_arena
  VkrPipelineRegistryConfig config;

  // Renderer integration
  VkrRendererFrontendHandle renderer;

  // Pipeline storage and management
  uint32_t generation_counter; // Global generation counter
  Array_VkrPipeline pipelines; // Contiguous array of pipelines
  VkrHashTable_VkrPipelineEntry pipelines_by_name; // Name-based lookup

  // ID reuse tracking (freelist)
  uint32_t next_free_index;
  uint32_t free_count;
  Array_uint32_t free_ids;

  // Domain-based organization
  Array_VkrPipelineHandle pipelines_by_domain[VKR_PIPELINE_DOMAIN_COUNT];

  // Pipeline state tracking and optimization
  VkrPipelineState state;

  // Statistics and profiling
  struct {
    uint32_t total_pipelines_created;
    uint32_t total_pipeline_binds;
    uint32_t redundant_binds_avoided;
    uint32_t total_meshes_batched;
    // telemetry
    uint64_t total_global_applies;
    uint64_t total_instance_applies;
    uint64_t total_instance_acquired;
    uint64_t total_instance_released;
    uint64_t total_descriptor_writes_avoided;
  } stats;
} VkrPipelineRegistry;

// =============================================================================
// Lifecycle Management
// =============================================================================

/**
 * @brief Initialize the pipeline registry system
 * @param registry Pipeline registry to initialize
 * @param renderer Frontend renderer handle
 * @param config Configuration settings
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_init(VkrPipelineRegistry *registry,
                                   VkrRendererFrontendHandle renderer,
                                   const VkrPipelineRegistryConfig *config);

/**
 * @brief Shutdown the pipeline registry and release all resources
 * @param registry Pipeline registry to shutdown
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_shutdown(VkrPipelineRegistry *registry);

// =============================================================================
// Pipeline Creation and Management
// =============================================================================

/**
 * @brief Create a graphics pipeline from explicit description
 * @param registry Pipeline registry
 * @param desc Graphics pipeline description
 * @param name Pipeline name for lookup (optional, can be NULL)
 * @param out_handle Output pipeline handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_create_graphics_pipeline(
    VkrPipelineRegistry *registry, const VkrGraphicsPipelineDescription *desc,
    String8 name, VkrPipelineHandle *out_handle, VkrRendererError *out_error);

/**
 * @brief Create a pipeline from a shader config
 * @param registry Pipeline registry
 * @param config Shader config
 * @param domain Pipeline domain
 * @param name Pipeline name for lookup
 * @param out_handle Output pipeline handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_create_from_shader_config(
    VkrPipelineRegistry *registry, const VkrShaderConfig *config,
    VkrPipelineDomain domain, String8 name, VkrPipelineHandle *out_handle,
    VkrRendererError *out_error);

/**
 * @brief Acquire a pipeline by name (loads if not present and auto-creation
 * enabled)
 * @param registry Pipeline registry
 * @param name Pipeline name
 * @param auto_release Auto-release when ref count hits 0
 * @param out_handle Output pipeline handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_acquire_by_name(VkrPipelineRegistry *registry,
                                              String8 name,
                                              bool8_t auto_release,
                                              VkrPipelineHandle *out_handle,
                                              VkrRendererError *out_error);
bool8_t vkr_pipeline_registry_find_by_name(VkrPipelineRegistry *registry,
                                           String8 name,
                                           VkrPipelineHandle *out_handle);

/**
 * @brief Destroy a pipeline and release associated resources
 * @param registry Pipeline registry
 * @param handle Pipeline handle to destroy
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_destroy_pipeline(VkrPipelineRegistry *registry,
                                               VkrPipelineHandle handle);

// =============================================================================
// Reference Counting and Lifetime Management
// =============================================================================

/**
 * @brief Acquire a reference to a pipeline (increment ref count)
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_acquire(VkrPipelineRegistry *registry,
                                      VkrPipelineHandle handle);

/**
 * @brief Release a reference to a pipeline (decrement ref count)
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_release(VkrPipelineRegistry *registry,
                                      VkrPipelineHandle handle);

/**
 * @brief Get pipeline pointer from handle (with validation)
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @param out_pipeline Output pipeline pointer
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_get_pipeline(VkrPipelineRegistry *registry,
                                           VkrPipelineHandle handle,
                                           VkrPipeline **out_pipeline);

// =============================================================================
// Pipeline State Management and Optimization
// =============================================================================

/**
 * @brief Get the currently bound pipeline
 * @param registry Pipeline registry
 * @return Current pipeline handle, or invalid handle if none bound
 */
VkrPipelineHandle
vkr_pipeline_registry_get_current_pipeline(VkrPipelineRegistry *registry);

/**
 * @brief Check if a specific pipeline is currently bound
 * @param registry Pipeline registry
 * @param handle Pipeline handle to check
 * @return true if pipeline is bound, false otherwise
 */
bool8_t vkr_pipeline_registry_is_pipeline_bound(VkrPipelineRegistry *registry,
                                                VkrPipelineHandle handle);

/**
 * @brief Bind a pipeline (with state caching optimization)
 * @param registry Pipeline registry
 * @param handle Pipeline handle to bind
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_bind_pipeline(VkrPipelineRegistry *registry,
                                            VkrPipelineHandle handle,
                                            VkrRendererError *out_error);

/**
 * @brief Update global state for the current pipeline
 * @param registry Pipeline registry
 * @param global_uniform Global uniform data
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_update_global_state(VkrPipelineRegistry *registry,
                                                  const void *global_uniform,
                                                  VkrRendererError *out_error);

/**
 * @brief Mark global state as dirty (needs update)
 * @param registry Pipeline registry
 */
void vkr_pipeline_registry_mark_global_state_dirty(
    VkrPipelineRegistry *registry);

/**
 * @brief Update instance state for a specific pipeline
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @param data Per-object shader state (model, local_state handle)
 * @param material Optional material state (uniforms + textures)
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_update_instance_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    const VkrShaderStateObject *data, const VkrRendererMaterialState *material,
    VkrRendererError *out_error);

// =============================================================================
// Local State Management
// =============================================================================

/**
 * @brief Acquire local state for a pipeline
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @param out_local_state Output local state handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_acquire_instance_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererInstanceStateHandle *out_local_state,
    VkrRendererError *out_error);

/**
 * @brief Release local state for a pipeline
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @param local_state Local state handle to release
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_release_instance_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererInstanceStateHandle local_state, VkrRendererError *out_error);

// =============================================================================
// High-Level Rendering Interface
// =============================================================================

/**
 * @brief Render a single renderable with automatic pipeline selection
 * @param registry Pipeline registry
 * @param mesh Mesh to render
 * @param global_uniform Global uniform data (view/projection matrices)
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_render_renderable(VkrPipelineRegistry *registry,
                                                const VkrMesh *mesh,
                                                const void *global_uniform,
                                                VkrRendererError *out_error);

// =============================================================================
// Domain and Query Interface
// =============================================================================

/**
 * @brief Get all pipelines for a specific domain
 * @param registry Pipeline registry
 * @param domain Pipeline domain
 * @param out_handles Output array of pipeline handles (must be pre-allocated)
 * @param max_handles Maximum number of handles to return
 * @param out_count Actual number of handles returned
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_get_pipelines_by_domain(
    VkrPipelineRegistry *registry, VkrPipelineDomain domain,
    VkrPipelineHandle *out_handles, uint32_t max_handles, uint32_t *out_count);

/**
 * @brief Get the best pipeline for a material and geometry combination
 * @param registry Pipeline registry
 * @param material_pipeline_id Material's preferred pipeline ID
 * @param out_handle Output pipeline handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_get_pipeline_for_material(
    VkrPipelineRegistry *registry, const char *shader_name,
    uint32_t material_pipeline_id, VkrPipelineHandle *out_handle,
    VkrRendererError *out_error);

/**
 * @brief Register an additional name (alias) for an existing pipeline handle
 *        to enable lookups by alternate keys (e.g., shader name).
 * @param registry Pipeline registry
 * @param handle Pipeline handle
 * @param alias Alias name
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_pipeline_registry_alias_pipeline_name(VkrPipelineRegistry *registry,
                                                  VkrPipelineHandle handle,
                                                  String8 alias,
                                                  VkrRendererError *out_error);

// =============================================================================
// Statistics and Debugging
// =============================================================================

/**
 * @brief Reset frame statistics
 * @param registry Pipeline registry
 */
void vkr_pipeline_registry_reset_frame_stats(VkrPipelineRegistry *registry);

/**
 * @brief Get current frame statistics
 * @param registry Pipeline registry
 * @param out_pipeline_changes Number of pipeline changes this frame
 * @param out_redundant_binds_avoided Number of redundant binds avoided this
 * frame
 */
void vkr_pipeline_registry_get_frame_stats(
    VkrPipelineRegistry *registry, uint32_t *out_pipeline_changes,
    uint32_t *out_redundant_binds_avoided);

/**
 * @brief Get overall registry statistics
 * @param registry Pipeline registry
 * @param out_total_pipelines Total number of pipelines created
 * @param out_total_binds Total number of pipeline binds
 * @param out_total_redundant_avoided Total redundant binds avoided
 * @param out_total_batched Total renderables batched
 */
void vkr_pipeline_registry_get_stats(VkrPipelineRegistry *registry,
                                     uint32_t *out_total_pipelines,
                                     uint32_t *out_total_binds,
                                     uint32_t *out_total_redundant_avoided,
                                     uint32_t *out_total_batched);

/**
 * @brief Collect backend telemetry metrics (e.g., descriptor writes avoided)
 * @param registry Pipeline registry
 */
void vkr_pipeline_registry_collect_backend_telemetry(
    VkrPipelineRegistry *registry);
