/*
 * @file shader_loader.h
 * @brief Shader loader header file
 * @details Loads shader files and parses them into shader configurations
 */
#pragma once

#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Parse Result and Error Types
// =============================================================================

typedef enum VkrShaderConfigErrorType {
  VKR_SHADER_CONFIG_ERROR_NONE = 0,
  VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND,
  VKR_SHADER_CONFIG_ERROR_FILE_READ_FAILED,
  VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
  VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD,
  VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
  VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
  VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
  VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
  VKR_SHADER_CONFIG_ERROR_DUPLICATE_KEY,
  VKR_SHADER_CONFIG_ERROR_UNKNOWN
} VkrShaderConfigErrorType;

typedef struct VkrShaderConfigParseResult {
  bool8_t is_valid;
  VkrShaderConfigErrorType error_type;
  String8 error_message;  // Allocator-allocated detailed error message
  uint64_t line_number;   // 0 if not line-specific
  uint64_t column_number; // 0 if not column-specific
} VkrShaderConfigParseResult;

// =============================================================================
// Manifest parsing
// =============================================================================

/**
 * @brief Parses a `.shadercfg` manifest and computes its uniform layout.
 *
 * Pure file-to-config: no resource system, renderer, or device involved. The
 * offsets it assigns are what the frontend's uniform staging writes to, so they
 * are the thing worth checking against SPIR-V reflection.
 *
 * @param path Manifest path
 * @param allocator Owns the produced config's allocations
 * @param scratch_alloc Scratch used during parsing
 * @param out_config Populated on success
 */
VkrShaderConfigParseResult vkr_shader_loader_parse(String8 path,
                                                   VkrAllocator *allocator,
                                                   VkrAllocator *scratch_alloc,
                                                   VkrShaderConfig *out_config);

// =============================================================================
// Resource loader factory
// =============================================================================

VkrResourceLoader vkr_shader_loader_create();