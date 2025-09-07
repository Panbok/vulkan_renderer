#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/renderer.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_texture_system.h"

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
  VkrResourceType type;
  union {
    VkrTextureHandle texture;
    VkrMaterialHandle material;
    VkrGeometryHandle geometry;
  } as;
} VkrResourceHandleInfo;

typedef struct VkrResourceLoader VkrResourceLoader;
typedef struct VkrResourceSystem
    VkrResourceSystem; // forward decl for loader callbacks

struct VkrResourceLoader {
  uint32_t id;             // assigned on registration
  VkrResourceType type;    // resource type
  const char *custom_type; // optional custom subtype tag
  const char *type_path;   // optional logical type path (unused for now)

  VkrResourceSystem *system;
  RendererFrontendHandle renderer;
  void *resource_system;

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

typedef struct VkrResourceSystem {
  Arena *arena; // persistent storage for keys/entries
  RendererFrontendHandle renderer;

  // Registered loaders
  VkrResourceLoader *loaders;
  uint32_t loader_count;
  uint32_t loader_capacity;
} VkrResourceSystem;

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the resource system
 * @param arena The arena to use
 * @param renderer The renderer to use
 * @param out_system The output system
 * @return True if the resource system was initialized, false otherwise
 */
bool8_t vkr_resource_system_init(Arena *arena, RendererFrontendHandle renderer,
                                 VkrResourceSystem *out_system);

/**
 * @brief Shuts down the resource system
 * @param system The resource system to shutdown
 */
void vkr_resource_system_shutdown(VkrResourceSystem *system);

/**
 * @brief Registers a resource loader
 * @param system The resource system to register the loader in
 * @param resource_system The resource system to use
 * @param loader The loader to register
 * @return True if the loader was registered, false otherwise
 */
bool8_t vkr_resource_system_register_loader(VkrResourceSystem *system,
                                            void *resource_system,
                                            VkrResourceLoader loader);

// =============================================================================
// Generic API
// =============================================================================

/**
 * @brief Loads a resource using a loader for the given type
 * @param system The resource system to load the resource from
 * @param type The type of the resource to load
 * @param name The name of the resource to load
 * @param temp_arena The temporary arena to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load(VkrResourceSystem *system,
                                 VkrResourceType type, String8 name,
                                 Arena *temp_arena,
                                 VkrResourceHandleInfo *out_info,
                                 RendererError *out_error);

/**
 * @brief Loads a resource using a custom type tag
 * @param system The resource system to load the resource from
 * @param custom_type The custom type tag to use
 * @param name The name of the resource to load
 * @param temp_arena The temporary arena to use
 * @param out_info The output info
 * @param out_error The output error
 * @return True if the resource was loaded, false otherwise
 */
bool8_t vkr_resource_system_load_custom(VkrResourceSystem *system,
                                        const char *custom_type, String8 name,
                                        Arena *temp_arena,
                                        VkrResourceHandleInfo *out_info,
                                        RendererError *out_error);

/**
 * @brief Unloads a resource using the appropriate loader
 * @param system The resource system to unload the resource from
 * @param type The type of the resource to unload
 * @param info The info of the resource to unload
 * @param name The name of the resource to unload
 */
void vkr_resource_system_unload(VkrResourceSystem *system, VkrResourceType type,
                                const VkrResourceHandleInfo *info,
                                String8 name);
