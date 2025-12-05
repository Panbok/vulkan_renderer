#pragma once

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
