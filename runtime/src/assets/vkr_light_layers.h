#pragma once

#include <stdint.h>

#include "defines.h"
#include "math/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Baked light layers (ADR-088): the meaning of one layer of a lightmap set
 * or a diffuse volume, and the light group names that tie lamp layers to
 * scene lights. A sun key holds sky light and sun bounce for one sun
 * direction; a lamp group holds the light of the static lights of one light
 * group. Assets store a layer as one 64-byte little-endian record.
 */

#define VKR_LIGHT_LAYER_RECORD_BYTES 64u
/* Lamp-group layers of one asset; a scene with more static groups fails to
   bake. */
#define VKR_LIGHT_LAYER_MAX_LAMP_GROUPS 4u

/*
 * Light group names: at most VKR_LIGHT_GROUP_NAME_BYTES - 1 letters, digits,
 * '_' or '-'. A scene light's empty name is the group VKR_LIGHT_GROUP_DEFAULT.
 */
#define VKR_LIGHT_GROUP_NAME_BYTES 32u
#define VKR_LIGHT_GROUP_DEFAULT "default"

typedef enum VkrLightLayerKind {
  /* Sky light and sun bounce for one sun direction, without the sun's direct
     term, which the runtime adds. */
  VKR_LIGHT_LAYER_SUN_KEY = 0,
  /* A light group's static lights and, for the default group, emission. */
  VKR_LIGHT_LAYER_LAMP_GROUP = 1,
} VkrLightLayerKind;

typedef struct VkrLightLayer {
  uint32_t kind;
  /* Sun-key or lamp-group number; a kind's numbers are unique. */
  uint32_t index;
  /* Unit direction toward the sun for a sun key; zero for a lamp group. */
  Vec3 sun_direction;
  /* A lamp group's light group name, unique among the asset's lamp groups;
     empty for a sun key. Null-terminated. */
  char name[VKR_LIGHT_GROUP_NAME_BYTES];
} VkrLightLayer;

/* Whether `name` is a light group name, empty included. */
bool8_t vkr_light_group_name_valid(const char *name, uint64_t length);

/*
 * Whether `layers` form one asset's layer table: each layer is a sun key with
 * a unit direction and no name or a lamp group with a zero direction and a
 * valid name; numbers are unique per kind, lamp group names are unique, and
 * at most VKR_LIGHT_LAYER_MAX_LAMP_GROUPS layers are lamp groups.
 */
bool8_t vkr_light_layers_valid(const VkrLightLayer *layers, uint32_t count);

/*
 * Weights of the sun-key layers for a sun toward `sun_toward` (any length):
 * the two keys next to each other on the daily circle that are nearest the
 * sun share weight one by their angles to it; a lone key weighs one, and so
 * does key 0 without a sun (zero vector). Lamp-group weights are left for
 * the caller, which scales them by their light groups.
 */
void vkr_light_layers_sun_weights(const VkrLightLayer *layers, uint32_t count,
                                  Vec3 sun_toward, float32_t *out_weights);

/* Writes `layer` as one record into `dst`, which holds
   VKR_LIGHT_LAYER_RECORD_BYTES zeroed bytes. */
void vkr_light_layer_write(uint8_t *dst, const VkrLightLayer *layer);

/* Reads one record; false when its reserved bytes or name padding are not
   zero or its name is unterminated. The table still needs
   vkr_light_layers_valid. */
bool8_t vkr_light_layer_read(const uint8_t *src, VkrLightLayer *out_layer);

#ifdef __cplusplus
}
#endif
