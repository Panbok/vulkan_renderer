#pragma once

#include "assets/vkr_mesh_encode.h"
#include "assets/vkr_scene_light_bake.h"

bool8_t vkr_mesh_cook_source(String8 source_path, String8 output_path,
                             VkrAllocator *source_allocator,
                             VkrAllocator *scratch_allocator,
                             VkrMeshCookStats *out_stats,
                             VkrRendererError *out_error);

/* Resolves source glTF punctual-light definition ranges into the cooked node
 * metadata before the artifact is encoded. */
bool8_t vkr_mesh_cook_source_with_light_ranges(
    String8 source_path, String8 output_path,
    const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrAllocator *source_allocator,
    VkrAllocator *scratch_allocator, VkrMeshCookStats *out_stats,
    VkrRendererError *out_error);

/* Managed outputs live below an absolute bundle root. output_path must be a
 * direct child of that root. import_id contains only ASCII letters, digits,
 * '-' and '_'. All published dependency references are file-relative. The
 * caller owns the unpublished bundle and removes it on failure/cancellation. */
bool8_t vkr_mesh_cook_source_managed(
    String8 source_path, String8 output_path, String8 bundle_root,
    String8 import_id, const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrAllocator *source_allocator,
    VkrAllocator *scratch_allocator, VkrMeshCookStats *out_stats,
    VkrRendererError *out_error);
