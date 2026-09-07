#pragma once

#include "containers/str.h"
#include "assets/vkr_mesh_source.h"
#include "vkr_renderer.h"

typedef struct VkrSceneLightRangeOverride {
  String8 light_name;
  float32_t range;
} VkrSceneLightRangeOverride;

/** Applies definition-name range overrides to the corresponding cooked mesh
 * source nodes. The source node `light` index preserves glTF definition
 * identity in the VKB artifact. */
bool8_t vkr_scene_bake_apply_light_ranges(
    String8 source_path, VkrMeshSource *source,
    const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrRendererError *out_error);
