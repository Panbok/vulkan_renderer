#include "assets/vkr_animation_cooked.h"

#include <float.h>

_Static_assert(sizeof(float32_t) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "VKA requires IEEE binary32");

typedef struct s_VkrAnimationReader {
  const uint8_t *bytes;
  uint64_t size;
  uint64_t position;
  uint8_t *storage;
  uint64_t storage_size;
  uint64_t used;
  bool8_t valid;
} s_VkrAnimationReader;

static const uint8_t *read_bytes(s_VkrAnimationReader *reader, uint64_t count) {
  if (!reader->valid || count > reader->size - reader->position) {
    reader->valid = false_v;
    return NULL;
  }
  const uint8_t *bytes = reader->bytes + reader->position;
  reader->position += count;
  return bytes;
}

static uint32_t read_u32(s_VkrAnimationReader *reader) {
  const uint8_t *bytes = read_bytes(reader, 4);
  if (!bytes) {
    return 0;
  }
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(s_VkrAnimationReader *reader) {
  uint64_t low = read_u32(reader);
  uint64_t high = read_u32(reader);
  return low | (high << 32);
}

static float32_t read_float(s_VkrAnimationReader *reader) {
  uint32_t bits = read_u32(reader);
  float32_t value;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

/* All decoded arrays share one 16-byte-aligned arena allocation. A dry scan
 * measures this storage before any file-controlled allocation takes place. */
static void *reserve(s_VkrAnimationReader *reader, uint64_t count,
                     uint64_t element_size) {
  uint64_t start = AlignPow2(reader->used, 16u);
  if (!reader->valid || count > VKR_ANIMATION_COOKED_MAX_BYTES / element_size ||
      start > VKR_ANIMATION_COOKED_MAX_BYTES - count * element_size) {
    reader->valid = false_v;
    return NULL;
  }
  reader->used = start + count * element_size;
  if (!reader->storage || !count) {
    return NULL;
  }
  if (reader->used > reader->storage_size) {
    reader->valid = false_v;
    return NULL;
  }
  return reader->storage + start;
}

static String8 read_name(s_VkrAnimationReader *reader) {
  uint32_t length = read_u32(reader);
  const uint8_t *source = read_bytes(reader, length);
  uint8_t *destination = reserve(reader, length, 1);
  if (destination && source) {
    MemCopy(destination, source, length);
  }
  return (String8){.str = destination, .length = length};
}

static bool8_t parse_asset(s_VkrAnimationReader *reader,
                           VkrAnimationAsset *asset) {
  reader->position = 16;
  asset->source_fingerprint = read_u64(reader);
  read_u64(reader);
  asset->node_count = read_u32(reader);
  asset->skin_count = read_u32(reader);
  asset->clip_count = read_u32(reader);
  uint32_t reserved = read_u32(reader);
  if (reserved || !asset->node_count ||
      asset->node_count > VKR_ANIMATION_MAX_NODES ||
      asset->skin_count > VKR_ANIMATION_MAX_NODES ||
      asset->clip_count > VKR_ANIMATION_MAX_CLIPS ||
      (uint64_t)asset->node_count * 120u + (uint64_t)asset->skin_count * 12u +
              (uint64_t)asset->clip_count * 12u >
          reader->size - reader->position) {
    return false_v;
  }
  asset->nodes = reserve(reader, asset->node_count, sizeof(*asset->nodes));
  asset->node_order =
      reserve(reader, asset->node_count, sizeof(*asset->node_order));
  asset->skins = reserve(reader, asset->skin_count, sizeof(*asset->skins));
  asset->clips = reserve(reader, asset->clip_count, sizeof(*asset->clips));
  for (uint32_t i = 0; i < asset->node_count && reader->valid; ++i) {
    VkrAnimationNode node = {0};
    node.name = read_name(reader);
    node.parent = read_u32(reader);
    uint32_t matrix_authored = read_u32(reader);
    if (matrix_authored > 1) {
      return false_v;
    }
    node.matrix_authored = (bool8_t)matrix_authored;
    for (uint32_t j = 0; j < 16; ++j) {
      node.local.elements[j] = read_float(reader);
    }
    for (uint32_t j = 0; j < 3; ++j) {
      node.rest.translation.elements[j] = read_float(reader);
    }
    for (uint32_t j = 0; j < 4; ++j) {
      node.rest.rotation.elements[j] = read_float(reader);
    }
    for (uint32_t j = 0; j < 3; ++j) {
      node.rest.scale.elements[j] = read_float(reader);
    }
    if (asset->nodes) {
      asset->nodes[i] = node;
    }
  }
  for (uint32_t i = 0; i < asset->node_count && reader->valid; ++i) {
    uint32_t node = read_u32(reader);
    if (asset->node_order) {
      asset->node_order[i] = node;
    }
  }
  for (uint32_t i = 0; i < asset->skin_count && reader->valid; ++i) {
    VkrAnimationSkin skin = {0};
    skin.name = read_name(reader);
    skin.skeleton_node = read_u32(reader);
    skin.joint_count = read_u32(reader);
    if (!skin.joint_count || skin.joint_count > asset->node_count ||
        (uint64_t)skin.joint_count * 68u > reader->size - reader->position) {
      return false_v;
    }
    skin.joints = reserve(reader, skin.joint_count, sizeof(*skin.joints));
    skin.inverse_bind =
        reserve(reader, skin.joint_count, sizeof(*skin.inverse_bind));
    for (uint32_t j = 0; j < skin.joint_count && reader->valid; ++j) {
      uint32_t joint = read_u32(reader);
      Mat4 inverse_bind = {0};
      for (uint32_t k = 0; k < 16; ++k) {
        inverse_bind.elements[k] = read_float(reader);
      }
      if (skin.joints && skin.inverse_bind) {
        skin.joints[j] = joint;
        skin.inverse_bind[j] = inverse_bind;
      }
    }
    if (asset->skins) {
      asset->skins[i] = skin;
    }
  }
  uint64_t total_keys = 0;
  for (uint32_t i = 0; i < asset->clip_count && reader->valid; ++i) {
    VkrAnimationClip clip = {0};
    clip.name = read_name(reader);
    clip.duration = read_float(reader);
    clip.channel_count = read_u32(reader);
    if (clip.channel_count > asset->node_count * 3u ||
        (uint64_t)clip.channel_count * 16u > reader->size - reader->position) {
      return false_v;
    }
    clip.channels = reserve(reader, clip.channel_count, sizeof(*clip.channels));
    for (uint32_t j = 0; j < clip.channel_count && reader->valid; ++j) {
      VkrAnimationChannel channel = {0};
      channel.node = read_u32(reader);
      uint32_t path = read_u32(reader);
      uint32_t interpolation = read_u32(reader);
      channel.key_count = read_u32(reader);
      total_keys += channel.key_count;
      if (path > VKR_ANIMATION_SCALE ||
          interpolation > VKR_ANIMATION_CUBIC_SPLINE || !channel.key_count ||
          total_keys > VKR_ANIMATION_MAX_KEYS) {
        return false_v;
      }
      channel.path = (VkrAnimationPath)path;
      channel.interpolation = (VkrAnimationInterpolation)interpolation;
      uint64_t value_count =
          (uint64_t)channel.key_count *
          (interpolation == VKR_ANIMATION_CUBIC_SPLINE ? 3u : 1u);
      if ((uint64_t)channel.key_count * 4u + value_count * 16u >
          reader->size - reader->position) {
        return false_v;
      }
      channel.times =
          reserve(reader, channel.key_count, sizeof(*channel.times));
      channel.values = reserve(reader, value_count, sizeof(*channel.values));
      for (uint32_t k = 0; k < channel.key_count && reader->valid; ++k) {
        float32_t time = read_float(reader);
        if (channel.times) {
          channel.times[k] = time;
        }
      }
      for (uint64_t k = 0; k < value_count && reader->valid; ++k) {
        Vec4 value = {0};
        for (uint32_t component = 0; component < 4; ++component) {
          value.elements[component] = read_float(reader);
        }
        if (channel.values) {
          channel.values[k] = value;
        }
      }
      if (clip.channels) {
        clip.channels[j] = channel;
      }
    }
    if (asset->clips) {
      asset->clips[i] = clip;
    }
  }
  return reader->valid && reader->position == reader->size;
}

bool8_t vkr_animation_cooked_decode(VkrAllocator *result_allocator,
                                    VkrAllocator *scratch_allocator,
                                    const uint8_t *bytes, uint64_t size,
                                    VkrAnimationAsset *out_asset,
                                    const char **error) {
  if (out_asset) {
    MemZero(out_asset, sizeof(*out_asset));
  }
  if (error) {
    *error = "invalid animation artifact or allocator";
  }
  if (!out_asset || !bytes || size < 48 ||
      size > VKR_ANIMATION_COOKED_MAX_BYTES || !result_allocator ||
      result_allocator == scratch_allocator ||
      result_allocator->type != VKR_ALLOCATOR_TYPE_ARENA ||
      !scratch_allocator || result_allocator->ctx == scratch_allocator->ctx ||
      !scratch_allocator->supports_scopes || MemCompare(bytes, "VKA1", 4)) {
    return false_v;
  }
  s_VkrAnimationReader header = {
      .bytes = bytes, .size = size, .position = 4, .valid = true_v};
  uint32_t version = read_u32(&header);
  uint64_t recorded_size = read_u64(&header);
  read_u64(&header);
  uint64_t recorded_checksum = read_u64(&header);
  if (version != VKR_ANIMATION_COOKED_VERSION || recorded_size != size) {
    return false_v;
  }
  uint64_t checksum = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < size; ++i) {
    checksum ^= (i >= 24 && i < 32) ? 0 : bytes[i];
    checksum *= UINT64_C(1099511628211);
  }
  if (checksum != recorded_checksum) {
    return false_v;
  }
  s_VkrAnimationReader measure = {
      .bytes = bytes, .size = size, .valid = true_v};
  VkrAnimationAsset measured = {0};
  if (!parse_asset(&measure, &measured)) {
    return false_v;
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }
  bool8_t success = false_v;
  uint8_t *temporary = vkr_allocator_alloc_aligned(
      scratch_allocator, measure.used, 16, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!temporary) {
    goto cleanup;
  }
  s_VkrAnimationReader reader = {.bytes = bytes,
                                 .size = size,
                                 .storage = temporary,
                                 .storage_size = measure.used,
                                 .valid = true_v};
  VkrAnimationAsset asset = {0};
  if (!parse_asset(&reader, &asset) ||
      !vkr_animation_validate(&asset, scratch_allocator, error)) {
    goto cleanup;
  }
  uint8_t *result = vkr_allocator_alloc_aligned(
      result_allocator, measure.used, 16, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!result) {
    if (error) {
      *error = "animation decode allocation failed";
    }
    goto cleanup;
  }
  /* Identical immutable input has already passed both scans and validation. */
  reader = (s_VkrAnimationReader){.bytes = bytes,
                                  .size = size,
                                  .storage = result,
                                  .storage_size = measure.used,
                                  .valid = true_v};
  parse_asset(&reader, out_asset);
  success = true_v;
  if (error) {
    *error = NULL;
  }
cleanup:
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  return success;
}
