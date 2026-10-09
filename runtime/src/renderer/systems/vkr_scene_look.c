#include "renderer/systems/vkr_scene_look.h"

#include "renderer/systems/vkr_scene_types.h"

/* A box axis shorter than this has no inside to blend from. */
#define LOOK_VOLUME_MIN_EXTENT 1e-6f

float32_t vkr_look_volume_weight(const VkrLookVolume *volume, Vec3 point) {
  if (!volume->value.enabled) {
    return 0.0f;
  }
  const Mat4 *box = &volume->box;
  if (vec3_length(box->cols[0]) < LOOK_VOLUME_MIN_EXTENT ||
      vec3_length(box->cols[1]) < LOOK_VOLUME_MIN_EXTENT ||
      vec3_length(box->cols[2]) < LOOK_VOLUME_MIN_EXTENT) {
    return 0.0f;
  }
  /* The box's nearest point to `point`, found in the unit box and measured
     back in world metres, so a scaled or rotated box fades evenly. */
  const Vec3 local = mat4_mul_vec3(mat4_inverse_affine(*box), point);
  const Vec3 nearest_local =
      vec3_new(Clamp(local.x, -0.5f, 0.5f), Clamp(local.y, -0.5f, 0.5f),
               Clamp(local.z, -0.5f, 0.5f));
  if (nearest_local.x == local.x && nearest_local.y == local.y &&
      nearest_local.z == local.z) {
    return 1.0f;
  }
  const float32_t distance =
      vec3_distance(mat4_mul_vec3(*box, nearest_local), point);
  const float32_t blend = volume->value.blend_distance;
  if (!(blend > 0.0f) || distance >= blend) {
    return 0.0f;
  }
  return 1.0f - distance / blend;
}

static float32_t look_lerp(float32_t from, float32_t to, float32_t weight) {
  return from + (to - from) * weight;
}

void vkr_look_blend(VkrLook *look, VkrLookVolume *volumes, uint32_t count,
                    Vec3 point) {
  /* Ascending priority; insertion sort keeps equal priorities in order. */
  for (uint32_t i = 1u; i < count; ++i) {
    const VkrLookVolume key = volumes[i];
    uint32_t j = i;
    while (j > 0u && volumes[j - 1u].value.priority > key.value.priority) {
      volumes[j] = volumes[j - 1u];
      --j;
    }
    volumes[j] = key;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const float32_t weight = vkr_look_volume_weight(&volumes[i], point);
    if (!(weight > 0.0f)) {
      continue;
    }
    const SceneLookVolume *v = &volumes[i].value;
    if (v->override_exposure) {
      look->exposure_compensation_ev = look_lerp(
          look->exposure_compensation_ev, v->exposure_compensation_ev, weight);
    }
    if (v->override_metering) {
      look->metering = true_v;
      look->metering_min_ev =
          look_lerp(look->metering_min_ev, v->metering_min_ev, weight);
      look->metering_max_ev =
          look_lerp(look->metering_max_ev, v->metering_max_ev, weight);
    }
    if (v->override_white_balance) {
      look->white_balance_temperature =
          look_lerp(look->white_balance_temperature,
                    v->white_balance_temperature, weight);
      look->white_balance_tint =
          look_lerp(look->white_balance_tint, v->white_balance_tint, weight);
    }
    if (v->override_contrast) {
      look->contrast = look_lerp(look->contrast, v->contrast, weight);
    }
    if (v->override_saturation) {
      look->saturation = look_lerp(look->saturation, v->saturation, weight);
    }
    if (v->override_bloom) {
      look->bloom_intensity =
          look_lerp(look->bloom_intensity, v->bloom_intensity, weight);
    }
    if (v->override_fog_color) {
      look->fog_color = vec3_lerp(look->fog_color, v->fog_color, weight);
    }
    if (v->override_fog_density) {
      look->fog_density = look_lerp(look->fog_density, v->fog_density, weight);
    }
    if (v->override_sky_light) {
      look->sky_light_intensity =
          look_lerp(look->sky_light_intensity, v->sky_light_intensity, weight);
    }
  }
}

/* Appends the look volumes of `owner` named by `indices`. */
static uint32_t look_volumes_append(const VkrScene *owner,
                                    const uint32_t *indices,
                                    uint32_t index_count, VkrLookVolume *out,
                                    uint32_t count, uint32_t capacity) {
  const VkrComponentTypeId id =
      owner ? vkr_scene_type_id(owner, &vkr_scene_look_volume_type)
            : VKR_COMPONENT_TYPE_INVALID;
  for (uint32_t i = 0u;
       id != VKR_COMPONENT_TYPE_INVALID && i < index_count && count < capacity;
       ++i) {
    const VkrWorld *world = owner->world;
    if (indices[i] >= world->dir.capacity ||
        !world->dir.records[indices[i]].chunk) {
      continue;
    }
    /* Resolution named the entity; it may have changed since. */
    const VkrEntityId entity = vkr_entity_id_from_index(world, indices[i]);
    const SceneLookVolume *value = vkr_entity_get_component(world, entity, id);
    const SceneTransform *transform =
        vkr_entity_get_component(world, entity, owner->comp_transform);
    if (!value || !transform || !value->enabled ||
        !vkr_scene_entity_visible(owner, entity)) {
      continue;
    }
    out[count++] = (VkrLookVolume){.value = *value, .box = transform->world};
  }
  return count;
}

uint32_t vkr_scene_look_volumes(const VkrScene *scene, VkrLookVolume *out,
                                uint32_t capacity) {
  if (!scene || !scene->world) {
    return 0u;
  }
  const VkrSceneWorldState *state = &scene->world_state;
  uint32_t count =
      look_volumes_append(scene, state->look_volume_indices,
                          state->look_volume_own_count, out, 0u, capacity);
  return look_volumes_append(
      scene->world_fallback,
      state->look_volume_indices + state->look_volume_own_count,
      state->look_volume_count - state->look_volume_own_count, out, count,
      capacity);
}
