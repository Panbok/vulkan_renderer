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
