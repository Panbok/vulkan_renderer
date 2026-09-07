#include "assets/vkr_scene_light_bake.h"

#include <cgltf.h>
#include <math.h>

#include "containers/str.h"

#define VKR_SCENE_BAKE_PATH_MAX 1024u

static bool8_t vkr_scene_bake_name_equals(String8 value, const char *name) {
  const uint64_t length = name ? string_length(name) : 0u;
  return value.str && value.length == length &&
         MemCompare(value.str, name, length) == 0;
}

bool8_t vkr_scene_bake_apply_light_ranges(
    String8 source_path, VkrMeshSource *source,
    const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrRendererError *out_error) {
  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (!source_path.str || source_path.length == 0u ||
      source_path.length >= VKR_SCENE_BAKE_PATH_MAX || !source || !out_error ||
      (range_override_count && !range_overrides)) {
    return false_v;
  }
  char path[VKR_SCENE_BAKE_PATH_MAX] = {0};
  MemCopy(path, source_path.str, source_path.length);
  cgltf_options options = {0};
  cgltf_data *data = NULL;
  if (cgltf_parse_file(&options, path, &data) != cgltf_result_success ||
      !data) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }
  for (uint32_t override_index = 0u; override_index < range_override_count;
       ++override_index) {
    const VkrSceneLightRangeOverride *override =
        &range_overrides[override_index];
    uint32_t light_index = UINT32_MAX;
    uint32_t definition_count = 0u;
    uint32_t node_count = 0u;
    if (!override->light_name.str || override->light_name.length == 0u ||
        !isfinite(override->range) || override->range <= 0.0f) {
      cgltf_free(data);
      return false_v;
    }
    for (cgltf_size index = 0u; index < data->lights_count; ++index) {
      const cgltf_light *light = &data->lights[index];
      if (!vkr_scene_bake_name_equals(override->light_name, light->name)) {
        continue;
      }
      if (light->type == cgltf_light_type_directional ||
          ++definition_count > 1u) {
        cgltf_free(data);
        return false_v;
      }
      light_index = (uint32_t)index;
    }
    if (definition_count != 1u) {
      cgltf_free(data);
      return false_v;
    }
    for (uint64_t node_index = 0u; node_index < source->nodes.length;
         ++node_index) {
      VkrMeshSourceNode *node = &source->nodes.data[node_index];
      if (node->light != light_index) {
        continue;
      }
      if (node->punctual.kind != 2u && node->punctual.kind != 3u) {
        cgltf_free(data);
        return false_v;
      }
      node->punctual.range = override->range;
      ++node_count;
    }
    if (node_count == 0u) {
      cgltf_free(data);
      return false_v;
    }
  }
  cgltf_free(data);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}
