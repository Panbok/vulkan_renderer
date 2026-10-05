#include "assets/vkr_light_layers.h"

#include "core/vkr_byte_io.h"

#include <math.h>

/* Unit-length tolerance for a sun key's direction. */
#define VKR_LIGHT_LAYER_UNIT_TOLERANCE 1.0e-3f

bool8_t vkr_light_group_name_valid(const char *name, uint64_t length) {
  if (!name || length >= VKR_LIGHT_GROUP_NAME_BYTES) {
    return false_v;
  }
  for (uint64_t i = 0u; i < length; ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false_v;
    }
  }
  return true_v;
}

/* The name's length when it is terminated within the field, or the field
   size when it is not. */
static uint64_t vkr_light_layer_name_length(const char *name) {
  uint64_t length = 0u;
  while (length < VKR_LIGHT_GROUP_NAME_BYTES && name[length] != '\0') {
    ++length;
  }
  return length;
}

static bool8_t vkr_light_layer_valid(const VkrLightLayer *layer) {
  const Vec3 d = layer->sun_direction;
  if (!isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z)) {
    return false_v;
  }
  const uint64_t name_length = vkr_light_layer_name_length(layer->name);
  if (!vkr_light_group_name_valid(layer->name, name_length)) {
    return false_v;
  }
  if (layer->kind == VKR_LIGHT_LAYER_SUN_KEY) {
    const float32_t length = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return name_length == 0u &&
           fabsf(length - 1.0f) <= VKR_LIGHT_LAYER_UNIT_TOLERANCE;
  }
  if (layer->kind == VKR_LIGHT_LAYER_LAMP_GROUP) {
    return name_length != 0u && d.x == 0.0f && d.y == 0.0f && d.z == 0.0f;
  }
  return false_v;
}

bool8_t vkr_light_layers_valid(const VkrLightLayer *layers, uint32_t count) {
  if (count != 0u && !layers) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrLightLayer *layer = &layers[i];
    if (!vkr_light_layer_valid(layer)) {
      return false_v;
    }
    const uint64_t name_length = vkr_light_layer_name_length(layer->name);
    uint32_t lamp_groups = 0u;
    for (uint32_t j = 0u; j < i; ++j) {
      const VkrLightLayer *other = &layers[j];
      if (other->kind != layer->kind) {
        continue;
      }
      /* Sun keys have no name; lamp groups are found by theirs. */
      const bool8_t same_name =
          vkr_light_layer_name_length(other->name) == name_length &&
          MemCompare(other->name, layer->name, name_length) == 0;
      if (other->index == layer->index ||
          (layer->kind == VKR_LIGHT_LAYER_LAMP_GROUP && same_name)) {
        return false_v;
      }
      lamp_groups += layer->kind == VKR_LIGHT_LAYER_LAMP_GROUP;
    }
    if (layer->kind == VKR_LIGHT_LAYER_LAMP_GROUP &&
        lamp_groups >= VKR_LIGHT_LAYER_MAX_LAMP_GROUPS) {
      return false_v;
    }
  }
  return true_v;
}

void vkr_light_layers_sun_weights(const VkrLightLayer *layers, uint32_t count,
                                  Vec3 sun_toward, float32_t *out_weights) {
  uint32_t keys = 0u;
  uint32_t nearest = UINT32_MAX;
  float32_t nearest_cosine = -2.0f;
  const float32_t length = vec3_length(sun_toward);
  const Vec3 sun =
      length > 0.0f ? vec3_scale(sun_toward, 1.0f / length) : vec3_zero();
  for (uint32_t i = 0u; i < count; ++i) {
    if (layers[i].kind != VKR_LIGHT_LAYER_SUN_KEY) {
      continue;
    }
    out_weights[i] = 0.0f;
    ++keys;
    const float32_t cosine = vec3_dot(sun, layers[i].sun_direction);
    if (length > 0.0f ? cosine > nearest_cosine : layers[i].index == 0u) {
      nearest_cosine = cosine;
      nearest = i;
    }
  }
  if (nearest == UINT32_MAX) {
    return;
  }
  if (keys == 1u || !(length > 0.0f)) {
    out_weights[nearest] = 1.0f;
    return;
  }
  /* The neighbour on the circle, by key number, that is nearer the sun. */
  const uint32_t before = (layers[nearest].index + keys - 1u) % keys;
  const uint32_t after = (layers[nearest].index + 1u) % keys;
  uint32_t neighbour = UINT32_MAX;
  float32_t neighbour_cosine = -2.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    if (layers[i].kind == VKR_LIGHT_LAYER_SUN_KEY &&
        (layers[i].index == before || layers[i].index == after)) {
      const float32_t cosine = vec3_dot(sun, layers[i].sun_direction);
      if (cosine > neighbour_cosine) {
        neighbour_cosine = cosine;
        neighbour = i;
      }
    }
  }
  if (neighbour == UINT32_MAX || neighbour == nearest) {
    out_weights[nearest] = 1.0f;
    return;
  }
  const float32_t to_nearest = acosf(fminf(fmaxf(nearest_cosine, -1.0f), 1.0f));
  const float32_t to_neighbour =
      acosf(fminf(fmaxf(neighbour_cosine, -1.0f), 1.0f));
  const float32_t span = to_nearest + to_neighbour;
  const float32_t t = span > 0.0f ? to_nearest / span : 0.0f;
  out_weights[nearest] = 1.0f - t;
  out_weights[neighbour] = t;
}

void vkr_light_layer_write(uint8_t *dst, const VkrLightLayer *layer) {
  vkr_store_le_u32(dst + 0u, layer->kind);
  vkr_store_le_u32(dst + 4u, layer->index);
  vkr_store_le_f32(dst + 8u, layer->sun_direction.x);
  vkr_store_le_f32(dst + 12u, layer->sun_direction.y);
  vkr_store_le_f32(dst + 16u, layer->sun_direction.z);
  /* Bytes 20 to 31 are reserved and zero; the name fills bytes 32 to 63,
     zero-padded. */
  MemCopy(dst + 32u, layer->name, vkr_light_layer_name_length(layer->name));
}

bool8_t vkr_light_layer_read(const uint8_t *src, VkrLightLayer *out_layer) {
  *out_layer = (VkrLightLayer){
      .kind = vkr_load_le_u32(src + 0u),
      .index = vkr_load_le_u32(src + 4u),
      .sun_direction =
          vec3_new(vkr_load_le_f32(src + 8u), vkr_load_le_f32(src + 12u),
                   vkr_load_le_f32(src + 16u)),
  };
  for (uint32_t offset = 20u; offset < 32u; ++offset) {
    if (src[offset] != 0u) {
      return false_v;
    }
  }
  /* A name ends at its first zero, and its padding is zero. */
  MemCopy(out_layer->name, src + 32u, VKR_LIGHT_GROUP_NAME_BYTES);
  const uint64_t name_length = vkr_light_layer_name_length(out_layer->name);
  if (name_length == VKR_LIGHT_GROUP_NAME_BYTES) {
    return false_v;
  }
  for (uint64_t i = name_length; i < VKR_LIGHT_GROUP_NAME_BYTES; ++i) {
    if (out_layer->name[i] != '\0') {
      return false_v;
    }
  }
  return true_v;
}
