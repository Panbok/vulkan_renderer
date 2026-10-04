#pragma once

#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a material resource loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single materials and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @return The configured resource loader
 */
VkrResourceLoader vkr_material_loader_create(void);

/**
 * @brief Replaces the live material that a `.mt` definition names.
 *
 * Parses `definition` as the contents of a file at `path`, against which
 * relative texture references resolve (the file need not exist), and hands
 * it to vkr_material_system_replace: the material's factors, state and
 * textures change in one publication once its textures have loaded. The
 * material keeps its handle, name and holders.
 *
 * @return false with VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED when no material
 * of that name is live, or another error for an invalid definition or full
 * replacement queues.
 */
bool8_t vkr_material_loader_replace_live(VkrMaterialSystem *system,
                                         String8 path, String8 definition,
                                         VkrAllocator *temp_alloc,
                                         VkrRendererError *out_error);

/**
 * @brief Replaces live material `material` with a terrain material blending
 * the `.mt` files of four layers.
 *
 * Layer 0 supplies the material's factors and every texture; layers 1 to 3
 * supply their base color, metallic, roughness, normal scale and occlusion
 * strength, and their base color, normal and ORM maps. An empty path is a
 * plain white layer. The result is opaque PBR; transmission, thickness,
 * diffuse transmission and subsurface are off. Textures stream like any
 * replacement's (vkr_material_system_replace).
 *
 * @return false with VKR_RENDERER_ERROR_FILE_NOT_FOUND when a layer file
 * cannot be read, or another error for a stale handle, an invalid file or
 * full replacement queues.
 */
bool8_t vkr_material_loader_replace_terrain(
    VkrMaterialSystem *system, VkrMaterialHandle material,
    const String8 layer_paths[VKR_MATERIAL_TERRAIN_LAYERS],
    VkrAllocator *temp_alloc, VkrRendererError *out_error);
