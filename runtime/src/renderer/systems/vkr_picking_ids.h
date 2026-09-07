/**
 * @file vkr_picking_ids.h
 * @brief Picking ID encoding helpers.
 *
 * Encodes a type tag into the high bits of object_id while keeping
 * the low bits as a stable per-kind value.
 */
#pragma once

#include "defines.h"

typedef enum VkrPickingIdKind {
  VKR_PICKING_ID_KIND_SCENE = 0,      /**< Scene render id (mesh entities). */
  VKR_PICKING_ID_KIND_UI_TEXT = 1,    /**< UI text slot id. */
  VKR_PICKING_ID_KIND_WORLD_TEXT = 2, /**< World text slot id. */
  VKR_PICKING_ID_KIND_LIGHT = 3,      /**< Reserved for light picking. */
  VKR_PICKING_ID_KIND_GIZMO = 4,      /**< Gizmo handle ids. */
} VkrPickingIdKind;

typedef struct VkrPickingDecodedId {
  VkrPickingIdKind kind;
  uint32_t value;
  bool8_t valid;
} VkrPickingDecodedId;

#define VKR_PICKING_ID_KIND_BITS 3u
#define VKR_PICKING_ID_KIND_SHIFT (32u - VKR_PICKING_ID_KIND_BITS)
#define VKR_PICKING_ID_KIND_MASK                                             \
  (((1u << VKR_PICKING_ID_KIND_BITS) - 1u) << VKR_PICKING_ID_KIND_SHIFT)
#define VKR_PICKING_ID_VALUE_MASK ((1u << VKR_PICKING_ID_KIND_SHIFT) - 1u)
// MAX_VALUE is one less than VALUE_MASK because encoding adds 1 to reserve 0 as
// invalid
#define VKR_PICKING_ID_MAX_VALUE (VKR_PICKING_ID_VALUE_MASK - 1u)

static inline uint32_t vkr_picking_encode_id(VkrPickingIdKind kind,
                                             uint32_t value) {
  if (value > VKR_PICKING_ID_MAX_VALUE) {
    return 0;
  }
  // Add 1 to value to reserve payload 0 as invalid
  return ((uint32_t)kind << VKR_PICKING_ID_KIND_SHIFT) | (value + 1u);
}

static inline VkrPickingDecodedId vkr_picking_decode_id(uint32_t object_id) {
  VkrPickingDecodedId decoded = {
      .kind = VKR_PICKING_ID_KIND_SCENE,
      .value = 0,
      .valid = false_v,
  };
  if (object_id == 0) {
    return decoded;
  }

  uint32_t payload = object_id & VKR_PICKING_ID_VALUE_MASK;
  if (payload == 0) {
    return decoded;
  }

  decoded.kind = (VkrPickingIdKind)((object_id & VKR_PICKING_ID_KIND_MASK) >>
                                    VKR_PICKING_ID_KIND_SHIFT);
  // Subtract 1 to recover original value (inverse of encoder's +1)
  decoded.value = payload - 1u;
  decoded.valid = true_v;
  return decoded;
}
