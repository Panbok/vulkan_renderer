#pragma once

#include "containers/array.h"
#include "defines.h"

typedef enum VkrEntityAccessState {
  VKR_ENTITY_ACCESS_STATE_FROZEN = 0,
  VKR_ENTITY_ACCESS_STATE_COLD = 1,
  VKR_ENTITY_ACCESS_STATE_HOT = 2,
} VkrEntityAccessState;

typedef enum VkrEntityType {
  VKR_ENTITY_TYPE_UNKNOWN = 0,
  VKR_ENTITY_TYPE_STATIC = 1,
  VKR_ENTITY_TYPE_DYNAMIC = 2,
} VkrEntityType;

typedef enum VkrEntityAttribute {
  VKR_ENTITY_ATTRIBUTE_NONE = 0 << 0,
  VKR_ENTITY_ATTRIBUTE_VISIBLE = 1 << 0,
  VKR_ENTITY_ATTRIBUTE_STATIC = 1 << 1,
} VkrEntityAttribute;

typedef enum VkrEntityProperty {
  VKR_ENTITY_PROPERTY_NONE = 0 << 0,
  VKR_ENTITY_PROPERTY_VISIBLE = 1 << 0,
  VKR_ENTITY_PROPERTY_STATIC = 1 << 1,
} VkrEntityProperty;

typedef struct VkrEntity {
  uint64_t id;
  VkrEntityAccessState access_state;
} VkrEntity;
Array(VkrEntity);