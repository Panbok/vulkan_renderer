#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/renderer.h"
#include "renderer/resources/vkr_resources.h"

// =============================================================================
// Resource System - Loader registry and generic load/unload dispatch
// =============================================================================

typedef enum VkrResourceType {
  VKR_RESOURCE_TYPE_UNKNOWN = 0,
  VKR_RESOURCE_TYPE_TEXTURE,
  VKR_RESOURCE_TYPE_MATERIAL,
  VKR_RESOURCE_TYPE_GEOMETRY,
  VKR_RESOURCE_TYPE_CUSTOM,
} VkrResourceType;

typedef struct VkrResourceHandleInfo {
  uint32_t
      loader_id; // id of the loader that created this handle, or VKR_INVALID_ID
  VkrResourceType type;
  union {
    VkrTextureHandle texture;
    VkrMaterialHandle material;
    VkrGeometryHandle geometry;
    void *custom; // VKR_RESOURCE_TYPE_CUSTOM
  } as;
} VkrResourceHandleInfo;

typedef struct VkrResourceLoader VkrResourceLoader;
typedef struct VkrResourceSystem
    VkrResourceSystem; // forward decl for loader callbacks

struct VkrResourceLoader {
  uint32_t id;          // assigned on registration
  VkrResourceType type; // resource type
  String8 custom_type;  // optional custom subtype tag

  RendererFrontendHandle renderer;
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
   * @param temp_arena The temporary arena
   * @param out_handle The output handle
   * @param out_error The output error
   * @return True if the resource was loaded, false otherwise
   */
  bool8_t (*load)(VkrResourceLoader *self, String8 name, Arena *temp_arena,
                  VkrResourceHandleInfo *out_handle, RendererError *out_error);

  /**
   * @brief Callback to unload the resource
   * @param self The loader
   * @param handle The handle of the resource
   * @param name The name of the resource
   */
  void (*unload)(VkrResourceLoader *self, const VkrResourceHandleInfo *handle,
                 String8 name);
};

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the resource system
 * @param arena The arena to use
 * @param renderer The renderer to use
 * @return True if the resource system was initialized, false otherwise
 */
bool8_t vkr_resource_system_init(Arena *arena, RendererFrontendHandle renderer);

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
 * @param temp_arena The temporary arena to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load(VkrResourceType type, String8 name,
                                 Arena *temp_arena,
                                 VkrResourceHandleInfo *out_info,
                                 RendererError *out_error);

/**
 * @brief Loads a resource using a custom type tag
 * @param custom_type The custom type tag to use
 * @param name The name of the resource to load
 * @param temp_arena The temporary arena to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load_custom(String8 custom_type, String8 name,
                                        Arena *temp_arena,
                                        VkrResourceHandleInfo *out_info,
                                        RendererError *out_error);

/**
 * @brief Unloads a resource using the appropriate loader
 * @param info The info of the resource to unload
 * @param name The name of the resource to unload
 */
void vkr_resource_system_unload(const VkrResourceHandleInfo *info,
                                String8 name);

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
