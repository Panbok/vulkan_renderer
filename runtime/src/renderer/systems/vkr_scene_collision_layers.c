#include "vkr_scene_collision_layers.h"

#include "vkr_scene_physics.h"
#include <stdio.h>
#include <string.h>

#define LAYERS_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

struct s_VkrSceneCollisionLayersPrepared {
  VkrScene *scene;
  VkrSceneCollisionLayers *replacement;
};

static bool8_t layers_fail(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

VkrSceneCollisionLayers vkr_scene_collision_layers_default(void) {
  VkrSceneCollisionLayers settings = {0};
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    snprintf(settings.names[i], sizeof(settings.names[i]), "Layer %u", i + 1u);
    settings.matrix[i] = UINT16_MAX;
  }
  settings.preset_count = 3;
  settings.presets[0] = (VkrSceneCollisionPreset){
      .name = "Default", .membership = 1, .mask = UINT16_MAX};
  settings.presets[1] = (VkrSceneCollisionPreset){
      .name = "Sensor", .membership = 2, .mask = UINT16_MAX, .sensor = true_v};
  settings.presets[2] = (VkrSceneCollisionPreset){
      .name = "No collision", .membership = 1, .mask = 0};
  return settings;
}

void vkr_scene_collision_layers_read(const VkrScene *scene,
                                     VkrSceneCollisionLayers *settings) {
  *settings = scene && scene->collision_layers
                  ? *scene->collision_layers
                  : vkr_scene_collision_layers_default();
}

static bool8_t layers_name_valid(const char *name) {
  if (!name[0] || !memchr(name, 0, VKR_COLLISION_LAYER_NAME_CAPACITY)) {
    return false_v;
  }
  for (uint32_t i = 0; name[i]; ++i) {
    if ((uint8_t)name[i] < 32u || (uint8_t)name[i] == 127u) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t
vkr_scene_collision_layers_validate(const VkrSceneCollisionLayers *settings,
                                    const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!settings || settings->preset_count > VKR_COLLISION_PRESET_CAPACITY) {
    return layers_fail(error, "Invalid collision preset count.");
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if (!layers_name_valid(settings->names[i])) {
      return layers_fail(
          error,
          "Layer names must be nonempty single-line strings under 48 bytes.");
    }
    for (uint32_t j = 0; j < VKR_COLLISION_LAYER_COUNT; ++j) {
      if (j < i && !strcmp(settings->names[i], settings->names[j])) {
        return layers_fail(error, "Layer names must be unique.");
      }
      if (((settings->matrix[i] >> j) & 1u) !=
          ((settings->matrix[j] >> i) & 1u)) {
        return layers_fail(error, "The collision matrix must be symmetric.");
      }
    }
  }
  for (uint32_t i = 0; i < settings->preset_count; ++i) {
    const VkrSceneCollisionPreset *preset = &settings->presets[i];
    if (!layers_name_valid(preset->name) || preset->sensor > 1u) {
      return layers_fail(error,
                         "Invalid collision preset name or sensor flag.");
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (!strcmp(preset->name, settings->presets[j].name)) {
        return layers_fail(error, "Collision preset names must be unique.");
      }
    }
  }
  return true_v;
}

uint16_t vkr_scene_collision_layers_effective_mask(const VkrScene *scene,
                                                   uint16_t membership,
                                                   uint16_t requested_mask) {
  const VkrSceneCollisionLayers *settings =
      scene->collision_layers_pending ? scene->collision_layers_pending
                                      : scene->collision_layers;
  if (!settings) {
    return requested_mask;
  }
  uint16_t allowed = 0;
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if (membership & (1u << i)) {
      allowed |= settings->matrix[i];
    }
  }
  return requested_mask & allowed;
}

bool8_t vkr_scene_collision_layers_prepare(
    VkrScene *scene, const VkrSceneCollisionLayers *settings,
    VkrSceneCollisionLayersPrepared **prepared, const char **error) {
  *prepared = NULL;
  if (!scene || !vkr_scene_physics_is_paused(scene) ||
      scene->collision_layers_pending) {
    return layers_fail(
        error, "Pause physics and finish the previous settings transaction.");
  }
  if (!vkr_scene_collision_layers_validate(settings, error)) {
    return false_v;
  }
  VkrSceneCollisionLayersPrepared *pending =
      vkr_allocator_alloc(scene->alloc, sizeof(*pending), LAYERS_TAG);
  if (!pending) {
    return layers_fail(error, "Could not stage collision settings.");
  }
  *pending = (VkrSceneCollisionLayersPrepared){.scene = scene};
  pending->replacement = vkr_allocator_alloc(
      scene->alloc, sizeof(*pending->replacement), LAYERS_TAG);
  if (!pending->replacement) {
    vkr_allocator_free(scene->alloc, pending, sizeof(*pending), LAYERS_TAG);
    return layers_fail(error, "Could not stage collision settings.");
  }
  *pending->replacement = *settings;
  scene->collision_layers_pending = pending->replacement;
  *prepared = pending;
  return true_v;
}

void vkr_scene_collision_layers_commit(
    VkrSceneCollisionLayersPrepared *prepared) {
  if (!prepared) {
    return;
  }
  VkrScene *scene = prepared->scene;
  VkrSceneCollisionLayers *old = scene->collision_layers;
  scene->collision_layers = prepared->replacement;
  scene->collision_layers_pending = NULL;
  scene->collision_layers_revision++;
  if (old) {
    vkr_allocator_free(scene->alloc, old, sizeof(*old), LAYERS_TAG);
  }
  vkr_allocator_free(scene->alloc, prepared, sizeof(*prepared), LAYERS_TAG);
}

void vkr_scene_collision_layers_discard(
    VkrSceneCollisionLayersPrepared *prepared) {
  if (!prepared) {
    return;
  }
  VkrScene *scene = prepared->scene;
  scene->collision_layers_pending = NULL;
  vkr_allocator_free(scene->alloc, prepared->replacement,
                     sizeof(*prepared->replacement), LAYERS_TAG);
  vkr_allocator_free(scene->alloc, prepared, sizeof(*prepared), LAYERS_TAG);
}

bool8_t
vkr_scene_collision_layers_apply(VkrScene *scene,
                                 const VkrSceneCollisionLayers *settings,
                                 const char **error) {
  VkrSceneCollisionLayersPrepared *pending = NULL;
  if (!vkr_scene_collision_layers_prepare(scene, settings, &pending, error)) {
    return false_v;
  }
  VkrScenePhysicsPrepared *bodies[VKR_SCENE_PHYSICS_MAX_BODIES] = {0};
  uint32_t prepared_count = 0;
  const uint32_t count = vkr_scene_physics_body_count(scene);
  VkrSceneCollisionLayers before;
  vkr_scene_collision_layers_read(scene, &before);
  if (MemCompare(before.matrix, settings->matrix, sizeof(before.matrix))) {
    for (uint32_t i = 0; i < count; ++i) {
      const VkrEntityId entity = vkr_scene_physics_body_at(scene, i);
      VkrScenePhysicsSnapshot snapshot;
      if (!vkr_scene_physics_read(scene, entity, &snapshot) ||
          !vkr_scene_physics_prepare(scene, entity, &snapshot,
                                     &bodies[prepared_count], error)) {
        goto failed;
      }
      prepared_count++;
    }
    if (!vkr_scene_physics_prepare_complete(scene, error)) {
      goto failed;
    }
  }
  for (uint32_t i = 0; i < prepared_count; ++i) {
    vkr_scene_physics_commit(bodies[i]);
  }
  vkr_scene_collision_layers_commit(pending);
  return true_v;
failed:
  for (uint32_t i = 0; i < prepared_count; ++i) {
    vkr_scene_physics_discard(bodies[i]);
  }
  vkr_scene_collision_layers_discard(pending);
  return false_v;
}

void vkr_scene_collision_layers_shutdown(VkrScene *scene) {
  if (scene->collision_layers) {
    vkr_allocator_free(scene->alloc, scene->collision_layers,
                       sizeof(*scene->collision_layers), LAYERS_TAG);
    scene->collision_layers = NULL;
  }
}
