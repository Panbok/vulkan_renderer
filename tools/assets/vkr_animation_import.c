#include "assets/vkr_animation_import.h"
#include "assets/vkr_cgltf.h"

#include <math.h>
#include <string.h>

static bool8_t animation_import_error(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static void *animation_import_array(VkrAllocator *allocator, uint64_t count,
                                    uint64_t size) {
  if (!count || size > UINT64_MAX / count) {
    return NULL;
  }
  void *result = vkr_allocator_alloc(allocator, count * size,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (result) {
    MemZero(result, count * size);
  }
  return result;
}

static bool8_t animation_import_name(VkrAllocator *allocator, const char *name,
                                     String8 *out) {
  *out = (String8){0};
  if (!name || !name[0]) {
    return true_v;
  }
  const uint64_t length = strlen(name);
  if (length > 65535u) {
    return false_v;
  }
  String8 view = string8_create((uint8_t *)name, length);
  *out = string8_duplicate(allocator, &view);
  return out->str != NULL;
}

static bool8_t animation_import_view_ready(const cgltf_buffer_view *view) {
  return !view || !view->has_meshopt_compression || view->data != NULL;
}

static float32_t *animation_import_floats(VkrAllocator *scratch,
                                          const cgltf_accessor *accessor,
                                          cgltf_type type,
                                          uint64_t expected_count) {
  if (!accessor || accessor->component_type != cgltf_component_type_r_32f ||
      accessor->type != type || accessor->count != expected_count ||
      accessor->normalized ||
      !animation_import_view_ready(accessor->buffer_view) ||
      (accessor->is_sparse &&
       (!animation_import_view_ready(accessor->sparse.indices_buffer_view) ||
        !animation_import_view_ready(accessor->sparse.values_buffer_view)))) {
    return NULL;
  }
  const uint64_t count = expected_count * cgltf_num_components(type);
  if (count > UINT32_MAX || count > SIZE_MAX / sizeof(float32_t)) {
    return NULL;
  }
  float32_t *values = animation_import_array(scratch, count, sizeof(*values));
  if (!values ||
      cgltf_accessor_unpack_floats(accessor, values, count) != count) {
    return NULL;
  }
  for (uint64_t i = 0; i < count; ++i) {
    if (!isfinite(values[i])) {
      return NULL;
    }
  }
  return values;
}

static bool8_t animation_import_nodes(VkrAllocator *result,
                                      VkrAllocator *scratch,
                                      const cgltf_data *data,
                                      VkrAnimationAsset *asset) {
  asset->nodes =
      animation_import_array(result, asset->node_count, sizeof(*asset->nodes));
  asset->node_order = animation_import_array(result, asset->node_count,
                                             sizeof(*asset->node_order));
  uint32_t *stack =
      animation_import_array(scratch, asset->node_count, sizeof(*stack));
  uint8_t *state = animation_import_array(scratch, asset->node_count, 1u);
  if (!asset->nodes || !asset->node_order || !stack || !state) {
    return false_v;
  }
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    const cgltf_node *source = &data->nodes[i];
    VkrAnimationNode *node = &asset->nodes[i];
    if (!animation_import_name(result, source->name, &node->name)) {
      return false_v;
    }
    node->parent = source->parent ? (uint32_t)(source->parent - data->nodes)
                                  : VKR_ANIMATION_NO_NODE;
    node->matrix_authored = source->has_matrix != 0;
    cgltf_node_transform_local(source, node->local.elements);
    node->rest = (VkrAnimationTrs){
        .translation =
            source->has_translation
                ? vec3_new(source->translation[0], source->translation[1],
                           source->translation[2])
                : vec3_zero(),
        .rotation = source->has_rotation
                        ? vec4_new(source->rotation[0], source->rotation[1],
                                   source->rotation[2], source->rotation[3])
                        : vec4_new(0.0f, 0.0f, 0.0f, 1.0f),
        .scale =
            source->has_scale
                ? vec3_new(source->scale[0], source->scale[1], source->scale[2])
                : vec3_new(1.0f, 1.0f, 1.0f),
    };
  }
  uint32_t ordered = 0u;
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    uint32_t node = i;
    uint32_t depth = 0u;
    while (node != VKR_ANIMATION_NO_NODE && node < asset->node_count &&
           state[node] == 0u) {
      stack[depth++] = node;
      state[node] = 1u;
      node = asset->nodes[node].parent;
    }
    if (node != VKR_ANIMATION_NO_NODE &&
        (node >= asset->node_count || state[node] == 1u)) {
      return false_v;
    }
    while (depth) {
      node = stack[--depth];
      state[node] = 2u;
      asset->node_order[ordered++] = node;
    }
  }
  return ordered == asset->node_count;
}

static bool8_t animation_import_skins(VkrAllocator *result,
                                      VkrAllocator *scratch,
                                      const cgltf_data *data,
                                      VkrAnimationAsset *asset) {
  if (!asset->skin_count) {
    return true_v;
  }
  asset->skins =
      animation_import_array(result, asset->skin_count, sizeof(*asset->skins));
  if (!asset->skins) {
    return false_v;
  }
  for (uint32_t i = 0; i < asset->skin_count; ++i) {
    const cgltf_skin *source = &data->skins[i];
    VkrAnimationSkin *skin = &asset->skins[i];
    if (!source->joints_count || source->joints_count > asset->node_count ||
        !animation_import_name(result, source->name, &skin->name)) {
      return false_v;
    }
    skin->joint_count = (uint32_t)source->joints_count;
    skin->skeleton_node = source->skeleton
                              ? (uint32_t)(source->skeleton - data->nodes)
                              : VKR_ANIMATION_NO_NODE;
    skin->joints = animation_import_array(result, skin->joint_count,
                                          sizeof(*skin->joints));
    skin->inverse_bind = animation_import_array(result, skin->joint_count,
                                                sizeof(*skin->inverse_bind));
    if (!skin->joints || !skin->inverse_bind) {
      return false_v;
    }
    float32_t *inverse = NULL;
    if (source->inverse_bind_matrices) {
      if (source->inverse_bind_matrices->count < skin->joint_count ||
          source->inverse_bind_matrices->count > VKR_ANIMATION_MAX_NODES) {
        return false_v;
      }
      inverse = animation_import_floats(scratch, source->inverse_bind_matrices,
                                        cgltf_type_mat4,
                                        source->inverse_bind_matrices->count);
      if (!inverse) {
        return false_v;
      }
    }
    for (uint32_t j = 0; j < skin->joint_count; ++j) {
      skin->joints[j] = (uint32_t)(source->joints[j] - data->nodes);
      skin->inverse_bind[j] = mat4_identity();
      if (inverse) {
        MemCopy(skin->inverse_bind[j].elements, inverse + j * 16u,
                sizeof(skin->inverse_bind[j].elements));
      }
    }
  }
  return true_v;
}

static bool8_t animation_import_channel(VkrAllocator *result,
                                        VkrAllocator *scratch,
                                        const cgltf_data *data,
                                        const cgltf_animation_channel *source,
                                        VkrAnimationChannel *channel) {
  if (!source->target_node || !source->sampler || !source->sampler->input ||
      !source->sampler->output) {
    return false_v;
  }
  switch (source->target_path) {
  case cgltf_animation_path_type_translation:
    channel->path = VKR_ANIMATION_TRANSLATION;
    break;
  case cgltf_animation_path_type_rotation:
    channel->path = VKR_ANIMATION_ROTATION;
    break;
  case cgltf_animation_path_type_scale:
    channel->path = VKR_ANIMATION_SCALE;
    break;
  default:
    return false_v;
  }
  switch (source->sampler->interpolation) {
  case cgltf_interpolation_type_step:
    channel->interpolation = VKR_ANIMATION_STEP;
    break;
  case cgltf_interpolation_type_linear:
    channel->interpolation = VKR_ANIMATION_LINEAR;
    break;
  case cgltf_interpolation_type_cubic_spline:
    channel->interpolation = VKR_ANIMATION_CUBIC_SPLINE;
    break;
  default:
    return false_v;
  }
  if (!source->sampler->input->count ||
      source->sampler->input->count > VKR_ANIMATION_MAX_KEYS) {
    return false_v;
  }
  channel->node = (uint32_t)(source->target_node - data->nodes);
  channel->key_count = (uint32_t)source->sampler->input->count;
  const uint32_t factor =
      channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE ? 3u : 1u;
  const uint32_t components = channel->path == VKR_ANIMATION_ROTATION ? 4u : 3u;
  const uint64_t value_count = (uint64_t)channel->key_count * factor;
  float32_t *times = animation_import_floats(
      scratch, source->sampler->input, cgltf_type_scalar, channel->key_count);
  float32_t *values = animation_import_floats(
      scratch, source->sampler->output,
      components == 4u ? cgltf_type_vec4 : cgltf_type_vec3, value_count);
  channel->times = animation_import_array(result, channel->key_count,
                                          sizeof(*channel->times));
  channel->values =
      animation_import_array(result, value_count, sizeof(*channel->values));
  if (!times || !values || !channel->times || !channel->values) {
    return false_v;
  }
  MemCopy(channel->times, times, channel->key_count * sizeof(*times));
  for (uint64_t i = 0; i < value_count; ++i) {
    channel->values[i] =
        vec4_new(values[i * components], values[i * components + 1u],
                 values[i * components + 2u],
                 components == 4u ? values[i * components + 3u] : 0.0f);
  }
  return true_v;
}

static bool8_t animation_import_clips(VkrAllocator *result,
                                      VkrAllocator *scratch,
                                      const cgltf_data *data,
                                      VkrAnimationAsset *asset) {
  if (!asset->clip_count) {
    return true_v;
  }
  asset->clips =
      animation_import_array(result, asset->clip_count, sizeof(*asset->clips));
  if (!asset->clips) {
    return false_v;
  }
  uint64_t total_keys = 0u;
  for (uint32_t i = 0; i < asset->clip_count; ++i) {
    const cgltf_animation *source = &data->animations[i];
    VkrAnimationClip *clip = &asset->clips[i];
    if (!source->channels_count ||
        source->channels_count > asset->node_count * 3u ||
        !animation_import_name(result, source->name, &clip->name)) {
      return false_v;
    }
    clip->channel_count = (uint32_t)source->channels_count;
    clip->channels = animation_import_array(result, clip->channel_count,
                                            sizeof(*clip->channels));
    if (!clip->channels) {
      return false_v;
    }
    for (uint32_t c = 0; c < clip->channel_count; ++c) {
      const cgltf_animation_channel *input = &source->channels[c];
      if (!input->sampler || !input->sampler->input) {
        return false_v;
      }
      total_keys += input->sampler->input->count;
      if (total_keys > VKR_ANIMATION_MAX_KEYS) {
        return false_v;
      }
      VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
      if (!vkr_allocator_scope_is_valid(&scope)) {
        return false_v;
      }
      bool8_t ok = animation_import_channel(result, scratch, data, input,
                                            &clip->channels[c]);
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!ok) {
        return false_v;
      }
      const VkrAnimationChannel *channel = &clip->channels[c];
      clip->duration =
          Max(clip->duration, channel->times[channel->key_count - 1u]);
    }
  }
  return true_v;
}

bool8_t vkr_animation_import_gltf(VkrAllocator *result_allocator,
                                  VkrAllocator *scratch_allocator,
                                  String8 source_path,
                                  VkrAnimationAsset *out_asset,
                                  const char **error) {
  if (out_asset) {
    *out_asset = (VkrAnimationAsset){0};
  }
  if (error) {
    *error = NULL;
  }
  if (!result_allocator || !scratch_allocator || !out_asset ||
      result_allocator->type != VKR_ALLOCATOR_TYPE_ARENA ||
      result_allocator->ctx == scratch_allocator->ctx ||
      !vkr_allocator_supports_scopes(scratch_allocator) || !source_path.str ||
      !source_path.length || source_path.length > 32768u ||
      memchr(source_path.str, 0, source_path.length)) {
    return animation_import_error(error, "Invalid animation import arguments");
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return animation_import_error(error, "Animation scratch scope unavailable");
  }
  String8 path = string8_duplicate(scratch_allocator, &source_path);
  cgltf_options options = {.file = vkr_cgltf_file_options()};
  cgltf_data *data = NULL;
  VkrAnimationAsset asset = {0};
  bool8_t success = false_v;
  const char *message = "Unable to parse animation glTF";
  if (!path.str || cgltf_parse_file(&options, string8_cstr(&path), &data) !=
                       cgltf_result_success) {
    goto cleanup;
  }
  message = "Unable to load or validate animation glTF buffers";
  if (cgltf_load_buffers(&options, data, string8_cstr(&path)) !=
          cgltf_result_success ||
      cgltf_validate(data) != cgltf_result_success) {
    goto cleanup;
  }
  message = "Animation bank import supports core glTF only; required "
            "extensions are unsupported";
  if (data->extensions_required_count) {
    goto cleanup;
  }
  message = "Animation node/skin/clip capacity exceeded or source has no nodes";
  if (!data->nodes_count || data->nodes_count > VKR_ANIMATION_MAX_NODES ||
      data->skins_count > VKR_ANIMATION_MAX_NODES ||
      data->animations_count > VKR_ANIMATION_MAX_CLIPS) {
    goto cleanup;
  }
  asset.node_count = (uint32_t)data->nodes_count;
  asset.skin_count = (uint32_t)data->skins_count;
  asset.clip_count = (uint32_t)data->animations_count;
  asset.source_fingerprint = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < data->json_size; ++i) {
    asset.source_fingerprint =
        (asset.source_fingerprint ^ (uint8_t)data->json[i]) *
        UINT64_C(1099511628211);
  }
  for (cgltf_size b = 0; b < data->buffers_count; ++b) {
    const uint8_t *bytes = data->buffers[b].data;
    for (uint64_t i = 0; i < data->buffers[b].size; ++i) {
      asset.source_fingerprint =
          (asset.source_fingerprint ^ bytes[i]) * UINT64_C(1099511628211);
    }
  }
  message = "Invalid animation hierarchy or allocation failure";
  if (!animation_import_nodes(result_allocator, scratch_allocator, data,
                              &asset)) {
    goto cleanup;
  }
  message = "Invalid skin mapping/inverse binds, unsupported compressed "
            "accessor, or allocation failure";
  if (!animation_import_skins(result_allocator, scratch_allocator, data,
                              &asset)) {
    goto cleanup;
  }
  message = "Invalid animation channel, unsupported morph/compressed track, or "
            "allocation failure";
  if (!animation_import_clips(result_allocator, scratch_allocator, data,
                              &asset)) {
    goto cleanup;
  }
  if (!vkr_animation_validate(&asset, scratch_allocator, &message)) {
    goto cleanup;
  }
  *out_asset = asset;
  success = true_v;
cleanup:
  if (data) {
    cgltf_free(data);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!success) {
    animation_import_error(error, message);
  }
  return success;
}
