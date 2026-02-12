#pragma once

#include "containers/str.h"
#include "core/vkr_job_system.h"
#include "defines.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Resource System - Loader registry and generic load/unload dispatch
// =============================================================================

typedef struct VkrMeshLoaderResult VkrMeshLoaderResult;

typedef enum VkrResourceType {
  VKR_RESOURCE_TYPE_UNKNOWN = 0,
  VKR_RESOURCE_TYPE_TEXTURE,
  VKR_RESOURCE_TYPE_MATERIAL,
  VKR_RESOURCE_TYPE_GEOMETRY,
  VKR_RESOURCE_TYPE_MESH,
  VKR_RESOURCE_TYPE_SCENE,
  VKR_RESOURCE_TYPE_BITMAP_FONT,
  VKR_RESOURCE_TYPE_SYSTEM_FONT,
  VKR_RESOURCE_TYPE_MTSDF_FONT,
  VKR_RESOURCE_TYPE_CUSTOM,
} VkrResourceType;

typedef struct VkrResourceHandleInfo {
  uint32_t
      loader_id; // id of the loader that created this handle, or VKR_INVALID_ID
  VkrResourceType type;
  VkrResourceLoadState load_state;
  VkrRendererError last_error;
  uint64_t request_id; // 0 when request tracking is not used
  union {
    VkrTextureHandle texture;
    VkrMaterialHandle material;
    VkrGeometryHandle geometry;
    VkrMeshLoaderResult *mesh;
    VkrSceneHandle scene;
    VkrFont font;
    void *custom; // VKR_RESOURCE_TYPE_CUSTOM
  } as;
} VkrResourceHandleInfo;

typedef struct VkrResourceAsyncBudget {
  uint32_t max_finalize_requests;
  uint32_t max_gpu_upload_ops;
  uint64_t max_gpu_upload_bytes;
} VkrResourceAsyncBudget;

/**
 * @brief Estimated GPU work consumed by one async finalize step.
 *
 * This is used by the resource-system pump to keep per-frame upload work
 * bounded. `gpu_upload_ops` is a coarse operation count and
 * `gpu_upload_bytes` is the estimated staging/upload byte volume.
 */
typedef struct VkrResourceAsyncFinalizeCost {
  uint32_t gpu_upload_ops;
  uint64_t gpu_upload_bytes;
} VkrResourceAsyncFinalizeCost;

typedef struct VkrResourceLoader VkrResourceLoader;
typedef struct VkrResourceSystem
    VkrResourceSystem; // forward decl for loader callbacks

struct VkrResourceLoader {
  uint32_t id;          // assigned on registration
  VkrResourceType type; // resource type
  String8 custom_type;  // optional custom subtype tag

  VkrRendererFrontendHandle renderer;
  void *resource_system; // opaque pointer to loader-specific resource system
                         // implementation

  /**
   * @brief Callback to check if the loader can load the resource
   * @param self The loader
   * @param name The name of the resource
   * @return True if the loader can load the resource, false otherwise
   */
  bool8_t (*can_load)(VkrResourceLoader *self, String8 name);

  /**
   * @brief Callback to load the resource
   * @param self The loader
   * @param name The name of the resource
   * @param temp_alloc The temporary allocator
   * @param out_handle The output handle
   * @param out_error The output error
   * @return True if the resource was loaded, false otherwise
   */
  bool8_t (*load)(VkrResourceLoader *self, String8 name,
                  VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handle,
                  VkrRendererError *out_error);

  /**
   * @brief Optional async worker-stage callback that performs CPU-only prep.
   *
   * This must not call Vulkan or mutate renderer/resource-system state.
   * Ownership of `*out_payload` transfers to the resource system, which will
   * later pass it to `finalize_async` and then `release_async_payload`.

   * @param self The loader
   * @param name The name of the resource
   * @param temp_alloc The temporary allocator
   * @param out_payload The output payload
   * @param out_error The output error
   * @return True if the resource was prepared successfully, false otherwise
   */
  bool8_t (*prepare_async)(VkrResourceLoader *self, String8 name,
                           VkrAllocator *temp_alloc, void **out_payload,
                           VkrRendererError *out_error);

  /**
   * @brief Optional async render-thread callback that finalizes a prepared
   * payload.
   *
   * This runs from `vkr_resource_system_pump`, where GPU objects may be
   * created/updated. The payload remains owned by the resource system.
   *
   * @param self The loader
   * @param name The name of the resource
   * @param payload The payload to finalize
   * @param out_handle The output handle
   * @param out_error The output error
   * @return True if the resource was finalized successfully, false otherwise
   */
  bool8_t (*finalize_async)(VkrResourceLoader *self, String8 name,
                            void *payload, VkrResourceHandleInfo *out_handle,
                            VkrRendererError *out_error);

  /**
   * @brief Optional estimate callback for async finalize GPU cost.
   *
   * When provided, the pump uses this estimate to enforce
   * `VkrResourceAsyncBudget.max_gpu_upload_ops` and
   * `VkrResourceAsyncBudget.max_gpu_upload_bytes`. Returning false falls back
   * to a conservative default cost.
   *
   * @param self The loader
   * @param name The name of the resource
   * @param payload The payload to estimate the cost of
   * @param out_cost The output cost
   * @return True if the cost was estimated successfully, false otherwise
   */
  bool8_t (*estimate_async_finalize_cost)(
      VkrResourceLoader *self, String8 name, void *payload,
      VkrResourceAsyncFinalizeCost *out_cost);

  /**
   * @brief Optional payload release callback for async prepare/finalize.
   *
   * Called exactly once for any payload produced by `prepare_async`, including
   * cancellation/failure paths where `finalize_async` is never reached.
   *
   * @param self The loader
   * @param payload The payload to release
   * @return True if the payload was released successfully, false otherwise
   */
  void (*release_async_payload)(VkrResourceLoader *self, void *payload);

  /**
   * @brief Callback to unload the resource
   * @param self The loader
   * @param handle The handle of the resource
   * @param name The name of the resource
   */
  void (*unload)(VkrResourceLoader *self, const VkrResourceHandleInfo *handle,
                 String8 name);

  /**
   * @brief Callback to batch load multiple resources in parallel
   * @param self The loader
   * @param paths Array of resource paths to load
   * @param count Number of resources to load
   * @param temp_alloc Temporary allocator for intermediate allocations
   * @param out_handles Output array of handle info (size = count)
   * @param out_errors Output array of errors (size = count)
   * @return Number of successfully loaded resources
   */
  uint32_t (*batch_load)(VkrResourceLoader *self, const String8 *paths,
                         uint32_t count, VkrAllocator *temp_alloc,
                         VkrResourceHandleInfo *out_handles,
                         VkrRendererError *out_errors);
};

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the resource system
 * @param allocator The allocator to use
 * @param renderer The renderer to use
 * @param job_system The job system to use for parallel loading (can be NULL)
 * @return True if the resource system was initialized, false otherwise
 */
bool8_t vkr_resource_system_init(VkrAllocator *allocator,
                                 VkrRendererFrontendHandle renderer,
                                 VkrJobSystem *job_system);

/**
 * @brief Registers a resource loader
 * @param resource_system Loader-specific resource system implementation (can be
 * NULL)
 * @param loader The loader to register
 * @return True if the loader was registered, false otherwise
 */
bool8_t vkr_resource_system_register_loader(void *resource_system,
                                            VkrResourceLoader loader);

// =============================================================================
// Generic API
// =============================================================================

/**
 * @brief Loads a resource using a loader for the given type
 * @param type The type of the resource to load
 * @param name The name of the resource to load
 * @param temp_alloc The temporary allocator to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load(VkrResourceType type, String8 name,
                                 VkrAllocator *temp_alloc,
                                 VkrResourceHandleInfo *out_info,
                                 VkrRendererError *out_error);

/**
 * @brief Loads a resource synchronously using the matching loader.
 *
 * This bypasses async request scheduling and returns only when loading reaches
 * a terminal state.
 *
 * @param type The type of the resource to load
 * @param name The name of the resource to load
 * @param temp_alloc The temporary allocator to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load_sync(VkrResourceType type, String8 name,
                                      VkrAllocator *temp_alloc,
                                      VkrResourceHandleInfo *out_info,
                                      VkrRendererError *out_error);

/**
 * @brief Loads a resource using a custom type tag
 * @param custom_type The custom type tag to use
 * @param name The name of the resource to load
 * @param temp_alloc The temporary allocator to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load_custom(String8 custom_type, String8 name,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_info,
                                        VkrRendererError *out_error);

/**
 * @brief Unloads a resource using the appropriate loader
 * @param info The info of the resource to unload
 * @param name The name of the resource to unload
 */
void vkr_resource_system_unload(const VkrResourceHandleInfo *info,
                                String8 name);

/**
 * @brief Batch loads multiple resources of the same type in parallel
 * @param type The type of resources to load
 * @param paths Array of resource paths to load
 * @param count Number of resources to load
 * @param temp_alloc Temporary allocator for intermediate allocations
 * @param out_handles Output array of handle info (size = count)
 * @param out_errors Output array of errors (size = count)
 * @return Number of successfully loaded resources
 *
 * @note If the loader has a batch_load callback, it will be used for parallel
 *       loading. Otherwise, falls back to sequential load calls.
 */
uint32_t vkr_resource_system_load_batch(VkrResourceType type,
                                        const String8 *paths, uint32_t count,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_handles,
                                        VkrRendererError *out_errors);

/**
 * @brief Synchronously batch-load resources of the same type.
 * @param type The type of the resource to load
 * @param paths Array of resource paths to load
 * @param count Number of resources to load
 * @param temp_alloc Temporary allocator for intermediate allocations
 * @param out_handles Output array of handle info (size = count)
 * @param out_errors Output array of errors (size = count)
 * @return Number of successfully loaded resources
 */
uint32_t vkr_resource_system_load_batch_sync(VkrResourceType type,
                                             const String8 *paths,
                                             uint32_t count,
                                             VkrAllocator *temp_alloc,
                                             VkrResourceHandleInfo *out_handles,
                                             VkrRendererError *out_errors);

/**
 * @brief Query current request state for an async/sync handle.
 * @param info The info of the resource to query
 * @param out_error The output error
 * @return The state of the resource
 */
VkrResourceLoadState
vkr_resource_system_get_state(const VkrResourceHandleInfo *info,
                              VkrRendererError *out_error);

/**
 * @brief Copies the resolved READY handle payload for a tracked request.
 *
 * Returns true only when the target request is in READY state.
 * @param tracked_info The info of the resource to query
 * @param out_info The output info
 * @return True if the resource was resolved, false otherwise
 */
bool8_t
vkr_resource_system_try_get_resolved(const VkrResourceHandleInfo *tracked_info,
                                     VkrResourceHandleInfo *out_info);

/**
 * @brief Returns true when resource reached READY state.
 * @param info The info of the resource to query
 * @return True if the resource is ready, false otherwise
 */
bool8_t vkr_resource_system_is_ready(const VkrResourceHandleInfo *info);

/**
 * @brief Progress async resource finalization on the render thread.
 *
 * The pointer can be NULL to use default budget limits.
 * @param budget The budget to use
 */
void vkr_resource_system_pump(const VkrResourceAsyncBudget *budget);

/**
 * @brief Mark an async request as canceled.
 * @param info The info of the resource to cancel
 */
void vkr_resource_system_cancel(const VkrResourceHandleInfo *info);

// =============================================================================
// Getters
// =============================================================================

/**
 * @brief Gets the loader id for a resource
 * @param type The type of the resource
 * @param name The name of the resource
 * @return The loader id
 */
uint32_t vkr_resource_system_get_loader_id(VkrResourceType type, String8 name);

/**
 * @brief Gets the job system used by the resource system
 * @return The job system, or NULL if not set
 */
VkrJobSystem *vkr_resource_system_get_job_system();
