#pragma once

#include <spirv_reflect.h>

#include "vulkan_types.h"

/**
 * Parsed reflection module bound to a specific entry point.
 *
 * The reflected data (including `entry_point` and any names referenced by
 * returned pointers) is owned by `module` and remains valid only until
 * `vulkan_spirv_reflection_module_destroy()` is called.
 */
typedef struct VulkanSpirvReflectionModule {
  SpvReflectShaderModule module;
  const SpvReflectEntryPoint *entry_point;
  VkShaderStageFlagBits stage;
  String8 entry_point_name;
  bool8_t is_initialized;
} VulkanSpirvReflectionModule;

/**
 * Returns the effective entry point used by reflection.
 *
 * Empty entry points are canonicalized to `"main"` to keep stage modules and
 * reflection cache keys deterministic.
 */
String8 vulkan_spirv_reflection_resolve_entry_point(String8 entry_point);

/**
 * Parses a SPIR-V module and resolves a required entry point.
 *
 * `expected_stage` can be `0` to skip stage validation. On failure, this
 * function populates `out_error` (if provided) and leaves `out_module`
 * uninitialized.
 */
bool8_t vulkan_spirv_reflection_module_create(
    const uint8_t *spirv_bytes, uint64_t spirv_size,
    VkShaderStageFlagBits expected_stage, String8 entry_point,
    VulkanSpirvReflectionModule *out_module,
    VkrReflectionErrorContext *out_error);

/**
 * Destroys reflection module state acquired by
 * `vulkan_spirv_reflection_module_create()`.
 */
void vulkan_spirv_reflection_module_destroy(
    VulkanSpirvReflectionModule *module);

/**
 * Builds canonical reflection output from a multi-stage shader program.
 *
 * Reflection output is allocated via `create_info->allocator`. Scratch work
 * buffers are sourced from `create_info->temp_allocator` and released at
 * function exit. `temp_allocator` must be non-null, distinct from
 * `create_info->allocator`, and support scoped allocations.
 */
bool8_t vulkan_spirv_shader_reflection_create(
    const VkrSpirvReflectionCreateInfo *create_info,
    VkrShaderReflection *out_reflection, VkrReflectionErrorContext *out_error);

/**
 * Destroys memory owned by `VkrShaderReflection`.
 *
 * This function is safe to call on zero-initialized reflections and partially
 * initialized reflections after a failed create.
 */
void vulkan_spirv_shader_reflection_destroy(VkrAllocator *allocator,
                                            VkrShaderReflection *reflection);

/**
 * Returns a static C string for renderer reflection error codes.
 */
const char *vulkan_reflection_error_string(VkrReflectionError error);

/**
 * Resets reflection error context to the default "no error" state.
 */
void vulkan_reflection_error_context_reset(VkrReflectionErrorContext *context);
