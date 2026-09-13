#include "assets/vkr_animation_encode.h"

#include <float.h>

_Static_assert(sizeof(float32_t) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "VKA requires IEEE binary32");

typedef struct s_VkrAnimationWriter {
  uint8_t *bytes;
  uint64_t position;
  uint64_t decoded_size;
  bool8_t valid;
} s_VkrAnimationWriter;

/* Match the runtime bank's bounded aligned storage as well as wire size. */
static void measure_storage(s_VkrAnimationWriter *writer, uint64_t count,
                            uint64_t element_size) {
  uint64_t start = AlignPow2(writer->decoded_size, 16u);
  if (!writer->valid || count > VKR_ANIMATION_COOKED_MAX_BYTES / element_size ||
      start > VKR_ANIMATION_COOKED_MAX_BYTES - count * element_size) {
    writer->valid = false_v;
    return;
  }
  writer->decoded_size = start + count * element_size;
}

static void write_bytes(s_VkrAnimationWriter *writer, const void *bytes,
                        uint64_t count) {
  if (!writer->valid ||
      count > VKR_ANIMATION_COOKED_MAX_BYTES - writer->position) {
    writer->valid = false_v;
    return;
  }
  if (writer->bytes && count) {
    MemCopy(writer->bytes + writer->position, bytes, count);
  }
  writer->position += count;
}

static void write_u32(s_VkrAnimationWriter *writer, uint32_t value) {
  uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                      (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  write_bytes(writer, bytes, 4);
}

static void write_u64(s_VkrAnimationWriter *writer, uint64_t value) {
  write_u32(writer, (uint32_t)value);
  write_u32(writer, (uint32_t)(value >> 32));
}

static void write_float(s_VkrAnimationWriter *writer, float32_t value) {
  uint32_t bits;
  MemCopy(&bits, &value, sizeof(bits));
  write_u32(writer, bits);
}

static void write_name(s_VkrAnimationWriter *writer, String8 name) {
  if (name.length > UINT32_MAX) {
    writer->valid = false_v;
    return;
  }
  measure_storage(writer, name.length, 1);
  write_u32(writer, (uint32_t)name.length);
  write_bytes(writer, name.str, name.length);
}

static void write_asset(s_VkrAnimationWriter *writer,
                        const VkrAnimationAsset *asset, uint64_t size) {
  measure_storage(writer, asset->node_count, sizeof(*asset->nodes));
  measure_storage(writer, asset->node_count, sizeof(*asset->node_order));
  measure_storage(writer, asset->skin_count, sizeof(*asset->skins));
  measure_storage(writer, asset->clip_count, sizeof(*asset->clips));
  write_bytes(writer, "VKA1", 4);
  write_u32(writer, VKR_ANIMATION_COOKED_VERSION);
  write_u64(writer, size);
  write_u64(writer, asset->source_fingerprint);
  write_u64(writer, 0);
  write_u32(writer, asset->node_count);
  write_u32(writer, asset->skin_count);
  write_u32(writer, asset->clip_count);
  write_u32(writer, 0);
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    const VkrAnimationNode *node = &asset->nodes[i];
    write_name(writer, node->name);
    write_u32(writer, node->parent);
    write_u32(writer, node->matrix_authored);
    for (uint32_t j = 0; j < 16; ++j) {
      write_float(writer, node->local.elements[j]);
    }
    for (uint32_t j = 0; j < 3; ++j) {
      write_float(writer, node->rest.translation.elements[j]);
    }
    for (uint32_t j = 0; j < 4; ++j) {
      write_float(writer, node->rest.rotation.elements[j]);
    }
    for (uint32_t j = 0; j < 3; ++j) {
      write_float(writer, node->rest.scale.elements[j]);
    }
  }
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    write_u32(writer, asset->node_order[i]);
  }
  for (uint32_t i = 0; i < asset->skin_count; ++i) {
    const VkrAnimationSkin *skin = &asset->skins[i];
    write_name(writer, skin->name);
    write_u32(writer, skin->skeleton_node);
    write_u32(writer, skin->joint_count);
    measure_storage(writer, skin->joint_count, sizeof(*skin->joints));
    measure_storage(writer, skin->joint_count, sizeof(*skin->inverse_bind));
    for (uint32_t j = 0; j < skin->joint_count; ++j) {
      write_u32(writer, skin->joints[j]);
      for (uint32_t k = 0; k < 16; ++k) {
        write_float(writer, skin->inverse_bind[j].elements[k]);
      }
    }
  }
  for (uint32_t i = 0; i < asset->clip_count; ++i) {
    const VkrAnimationClip *clip = &asset->clips[i];
    write_name(writer, clip->name);
    write_float(writer, clip->duration);
    write_u32(writer, clip->channel_count);
    measure_storage(writer, clip->channel_count, sizeof(*clip->channels));
    for (uint32_t j = 0; j < clip->channel_count; ++j) {
      const VkrAnimationChannel *channel = &clip->channels[j];
      write_u32(writer, channel->node);
      write_u32(writer, channel->path);
      write_u32(writer, channel->interpolation);
      write_u32(writer, channel->key_count);
      uint64_t count =
          (uint64_t)channel->key_count *
          (channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE ? 3u : 1u);
      measure_storage(writer, channel->key_count, sizeof(*channel->times));
      measure_storage(writer, count, sizeof(*channel->values));
      for (uint32_t k = 0; k < channel->key_count; ++k) {
        write_float(writer, channel->times[k]);
      }
      for (uint64_t k = 0; k < count; ++k) {
        for (uint32_t component = 0; component < 4; ++component) {
          write_float(writer, channel->values[k].elements[component]);
        }
      }
    }
  }
}

bool8_t vkr_animation_cooked_encode(VkrAllocator *scratch_allocator,
                                    const VkrAnimationAsset *asset,
                                    uint8_t **bytes, uint64_t *size,
                                    const char **error) {
  if (error) {
    *error = "invalid animation encode arguments";
  }
  if (bytes) {
    *bytes = NULL;
  }
  if (size) {
    *size = 0;
  }
  if (!bytes || !size || !scratch_allocator ||
      scratch_allocator->type != VKR_ALLOCATOR_TYPE_ARENA ||
      !vkr_animation_validate(asset, scratch_allocator, error)) {
    return false_v;
  }
  s_VkrAnimationWriter measure = {.valid = true_v};
  write_asset(&measure, asset, 0);
  if (!measure.valid) {
    if (error) {
      *error = "animation artifact or decoded storage exceeds 1 GiB";
    }
    return false_v;
  }
  uint8_t *output = vkr_allocator_alloc(scratch_allocator, measure.position,
                                        VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!output) {
    if (error) {
      *error = "animation encode allocation failed";
    }
    return false_v;
  }
  s_VkrAnimationWriter writer = {.bytes = output, .valid = true_v};
  write_asset(&writer, asset, measure.position);
  uint64_t checksum = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < writer.position; ++i) {
    checksum ^= output[i];
    checksum *= UINT64_C(1099511628211);
  }
  s_VkrAnimationWriter checksum_writer = {
      .bytes = output, .position = 24, .valid = true_v};
  write_u64(&checksum_writer, checksum);
  *bytes = output;
  *size = writer.position;
  if (error) {
    *error = NULL;
  }
  return true_v;
}
