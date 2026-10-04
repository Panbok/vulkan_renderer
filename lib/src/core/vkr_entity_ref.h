#pragma once

/* A foundation value type, so the script SDK can hold references too. Its
   text form lives with the type descriptors (vkr_type_desc.h). */

#include "defines.h"

/**
 * Document-stable entity id (ADR-076): an entity's UUID, as 16 bytes in text
 * order. A scene document names each entity by one, the editor gives every
 * entity it creates one, and an ENTITY property or script field stores one
 * to reference an entity of its own container. All zero is no entity.
 */
typedef struct VkrEntityRef {
  uint8_t bytes[16];
} VkrEntityRef;

/** Whether `id` names no entity. */
static inline bool8_t vkr_entity_ref_empty(const VkrEntityRef *id) {
  for (uint32_t i = 0; i < 16u; ++i) {
    if (id->bytes[i]) {
      return false_v;
    }
  }
  return true_v;
}
