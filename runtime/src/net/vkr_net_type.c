#include "vkr_net_type.h"

#include "core/vkr_byte_io.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t net_type_float_count(const VkrPropertyDesc *property) {
  switch (property->kind) {
  case VKR_PROPERTY_F32:
  case VKR_PROPERTY_ANGLE:
    return 1u;
  case VKR_PROPERTY_VEC2:
    return 2u;
  case VKR_PROPERTY_VEC3:
  case VKR_PROPERTY_COLOR:
  case VKR_PROPERTY_DIRECTION:
    return 3u;
  case VKR_PROPERTY_VEC4:
  case VKR_PROPERTY_QUAT:
    return 4u;
  default:
    return 0u;
  }
}

static bool8_t net_type_skips(const VkrPropertyDesc *property) {
  return (property->flags & VKR_PROPERTY_FLAG_TRANSIENT) != 0u;
}

bool8_t vkr_net_type_write(VkrBitWriter *writer, const VkrTypeDesc *type,
                           const void *value) {
  const uint8_t *base = value;
  for (uint32_t i = 0u; i < type->property_count; ++i) {
    const VkrPropertyDesc *property = &type->properties[i];
    if (net_type_skips(property)) {
      continue;
    }
    const uint8_t *field = base + property->offset;
    const uint32_t floats = net_type_float_count(property);
    if (floats > 0u) {
      for (uint32_t k = 0u; k < floats; ++k) {
        float32_t component = 0.0f;
        memcpy(&component, field + k * sizeof(float32_t), sizeof(component));
        if (!isfinite(component)) {
          return false_v;
        }
        vkr_bit_write(writer, vkr_f32_bits(component), 32u);
      }
      continue;
    }
    switch (property->kind) {
    case VKR_PROPERTY_BOOL:
      vkr_bit_write(writer, *field ? 1u : 0u, 1u);
      break;
    case VKR_PROPERTY_I32: {
      int32_t number = 0;
      memcpy(&number, field, sizeof(number));
      const uint32_t zigzag =
          ((uint32_t)number << 1) ^ (uint32_t)(number >> 31);
      vkr_bit_write_varuint(writer, zigzag);
      break;
    }
    case VKR_PROPERTY_U32:
    case VKR_PROPERTY_ENUM: {
      uint32_t number = 0u;
      memcpy(&number, field, sizeof(number));
      vkr_bit_write_varuint(writer, number);
      break;
    }
    case VKR_PROPERTY_STRING: {
      const uint8_t *end = memchr(field, 0, property->capacity);
      if (!end) {
        return false_v;
      }
      const uint32_t length = (uint32_t)(end - field);
      vkr_bit_write_varuint(writer, length);
      vkr_bit_write_bytes(writer, field, length);
      break;
    }
    case VKR_PROPERTY_ENTITY:
      vkr_bit_write(writer, vkr_load_le_u64(field), 64u);
      vkr_bit_write(writer, vkr_load_le_u64(field + 8), 64u);
      break;
    default:
      return false_v;
    }
  }
  return !writer->overflow;
}

bool8_t vkr_net_type_read(VkrBitReader *reader, const VkrTypeDesc *type,
                          void *out, char *error, uint32_t capacity) {
  uint8_t *base = out;
  vkr_type_defaults(type, out);
  for (uint32_t i = 0u; i < type->property_count; ++i) {
    const VkrPropertyDesc *property = &type->properties[i];
    if (net_type_skips(property)) {
      continue;
    }
    uint8_t *field = base + property->offset;
    const uint32_t floats = net_type_float_count(property);
    if (floats > 0u) {
      for (uint32_t k = 0u; k < floats; ++k) {
        const float32_t component =
            vkr_f32_from_bits((uint32_t)vkr_bit_read(reader, 32u));
        if (!isfinite(component)) {
          snprintf(error, capacity, "%s.%s is not finite", type->name,
                   property->name);
          return false_v;
        }
        memcpy(field + k * sizeof(float32_t), &component, sizeof(component));
      }
      continue;
    }
    switch (property->kind) {
    case VKR_PROPERTY_BOOL:
      *field = (uint8_t)vkr_bit_read(reader, 1u);
      break;
    case VKR_PROPERTY_I32: {
      const uint64_t zigzag = vkr_bit_read_varuint(reader);
      if (zigzag > UINT32_MAX) {
        snprintf(error, capacity, "%s.%s is out of range", type->name,
                 property->name);
        return false_v;
      }
      const uint32_t bits = (uint32_t)zigzag;
      const int32_t number = (int32_t)(bits >> 1) ^ -(int32_t)(bits & 1u);
      memcpy(field, &number, sizeof(number));
      break;
    }
    case VKR_PROPERTY_U32:
    case VKR_PROPERTY_ENUM: {
      const uint64_t number = vkr_bit_read_varuint(reader);
      if (number > UINT32_MAX) {
        snprintf(error, capacity, "%s.%s is out of range", type->name,
                 property->name);
        return false_v;
      }
      const uint32_t stored = (uint32_t)number;
      memcpy(field, &stored, sizeof(stored));
      break;
    }
    case VKR_PROPERTY_STRING: {
      const uint64_t length = vkr_bit_read_varuint(reader);
      if (length >= property->capacity) {
        snprintf(error, capacity, "%s.%s is too long", type->name,
                 property->name);
        return false_v;
      }
      const uint8_t *bytes = vkr_bit_read_bytes(reader, (uint32_t)length);
      if (!bytes || memchr(bytes, 0, (size_t)length)) {
        snprintf(error, capacity, "%s.%s is malformed", type->name,
                 property->name);
        return false_v;
      }
      memcpy(field, bytes, (size_t)length);
      field[length] = 0u;
      break;
    }
    case VKR_PROPERTY_ENTITY:
      vkr_store_le_u64(field, vkr_bit_read(reader, 64u));
      vkr_store_le_u64(field + 8, vkr_bit_read(reader, 64u));
      break;
    default:
      snprintf(error, capacity, "%s.%s has no wire form", type->name,
               property->name);
      return false_v;
    }
    if (reader->overflow) {
      break;
    }
  }
  if (reader->overflow) {
    snprintf(error, capacity, "%s ends early", type->name);
    return false_v;
  }
  return vkr_type_validate(type, out, error, capacity);
}
