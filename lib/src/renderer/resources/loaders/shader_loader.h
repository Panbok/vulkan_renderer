/*
 * @file shader_loader.h
 * @brief Shader loader header file
 * @details Loads shader files and parses them into shader configurations
 */
#pragma once

#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Resource loader factory
// =============================================================================

VkrResourceLoader vkr_shader_loader_create();