#include "core/vkr_type_desc.h"

#include "math/vkr_quat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TYPE_DEGREES 57.29577951308232
#define TYPE_RADIANS 0.017453292519943295

static bool8_t type_fail(char *error, uint32_t capacity, const char *format,
                         ...) {
  if (error && capacity) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
  }
  return false_v;
}

static bool8_t type_name_equals(const char *name, String8 text) {
  const uint64_t length = strlen(name);
  return length == text.length && MemCompare(name, text.str, length) == 0;
}

static bool8_t type_bounded(const VkrPropertyDesc *property) {
  return property->min < property->max;
}

// =============================================================================
// Entity references
// =============================================================================

bool8_t vkr_entity_ref_parse(const char *text, uint64_t length,
                             VkrEntityRef *out) {
  if (!text || !out || length != 36u) {
    return false_v;
  }
  uint32_t byte = 0u;
  for (uint32_t i = 0; i < 36u; i += 2u) {
    if (i == 8u || i == 13u || i == 18u || i == 23u) {
      if (text[i] != '-') {
        return false_v;
      }
      ++i;
    }
    int32_t digits[2];
    for (uint32_t d = 0; d < 2u; ++d) {
      const char c = text[i + d];
      digits[d] = c >= '0' && c <= '9'   ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
      if (digits[d] < 0) {
        return false_v;
      }
    }
    out->bytes[byte++] = (uint8_t)(digits[0] * 16 + digits[1]);
  }
  return byte == 16u;
}

void vkr_entity_ref_format(const VkrEntityRef *id, char out[37]) {
  static const char hex[] = "0123456789abcdef";
  uint32_t at = 0u;
  for (uint32_t i = 0; i < 16u; ++i) {
    if (i == 4u || i == 6u || i == 8u || i == 10u) {
      out[at++] = '-';
    }
    out[at++] = hex[id->bytes[i] >> 4u];
    out[at++] = hex[id->bytes[i] & 15u];
  }
  out[at] = '\0';
}

uint32_t vkr_io_port_count(const VkrIoPort *ports) {
  uint32_t count = 0u;
  while (ports && ports[count].name) {
    ++count;
  }
  return count;
}

uint32_t vkr_io_port_find(const VkrIoPort *ports, String8 name) {
  for (uint32_t i = 0; ports && ports[i].name; ++i) {
    if (type_name_equals(ports[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

// =============================================================================
// Properties
// =============================================================================

uint32_t vkr_property_components(const VkrPropertyDesc *property) {
  switch (property->kind) {
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
    return 1u;
  }
}

float64_t vkr_property_display_scale(const VkrPropertyDesc *property) {
  if (property->kind == VKR_PROPERTY_ANGLE) {
    return TYPE_DEGREES;
  }
  return property->display_scale != 0.0f ? property->display_scale : 1.0;
}

uint32_t vkr_property_enum_count(const VkrPropertyDesc *property) {
  uint32_t count = 0u;
  if ((property->kind == VKR_PROPERTY_ENUM ||
       property->kind == VKR_PROPERTY_U32) &&
      property->names) {
    while (property->names[count]) {
      ++count;
    }
  }
  return count;
}

uint32_t vkr_type_find_property(const VkrTypeDesc *type, String8 name) {
  for (uint32_t i = 0u; i < type->property_count; ++i) {
    if (type_name_equals(type->properties[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

static const uint8_t *property_address(const VkrPropertyDesc *property,
                                       const void *value) {
  return (const uint8_t *)value + property->offset;
}

bool8_t vkr_property_get_number(const VkrPropertyDesc *property,
                                const void *value, float64_t *out) {
  const uint8_t *address = property_address(property, value);
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    *out = *(const bool8_t *)address ? 1.0 : 0.0;
    return true_v;
  case VKR_PROPERTY_I32:
    *out = *(const int32_t *)address;
    return true_v;
  case VKR_PROPERTY_U32:
  case VKR_PROPERTY_ENUM:
    *out = *(const uint32_t *)address;
    return true_v;
  case VKR_PROPERTY_F32:
  case VKR_PROPERTY_ANGLE:
    *out = *(const float32_t *)address;
    return true_v;
  default:
    return false_v;
  }
}

bool8_t vkr_property_set_number(const VkrPropertyDesc *property, void *value,
                                float64_t number) {
  if (!isfinite(number)) {
    return false_v;
  }
  uint8_t *address = (uint8_t *)value + property->offset;
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    *(bool8_t *)address = number != 0.0 ? true_v : false_v;
    return true_v;
  case VKR_PROPERTY_I32: {
    const float64_t rounded = round(number);
    if (rounded < (float64_t)INT32_MIN || rounded > (float64_t)INT32_MAX) {
      return false_v;
    }
    *(int32_t *)address = (int32_t)rounded;
    return true_v;
  }
  case VKR_PROPERTY_U32:
  case VKR_PROPERTY_ENUM: {
    const float64_t rounded = round(number);
    if (rounded < 0.0 || rounded > (float64_t)UINT32_MAX) {
      return false_v;
    }
    if (property->kind == VKR_PROPERTY_ENUM &&
        rounded >= (float64_t)vkr_property_enum_count(property)) {
      return false_v;
    }
    *(uint32_t *)address = (uint32_t)rounded;
    return true_v;
  }
  case VKR_PROPERTY_F32:
  case VKR_PROPERTY_ANGLE: {
    const float32_t narrowed = (float32_t)number;
    if (!isfinite(narrowed)) {
      return false_v;
    }
    *(float32_t *)address = narrowed;
    return true_v;
  }
  default:
    return false_v;
  }
}

bool8_t vkr_property_get_floats(const VkrPropertyDesc *property,
                                const void *value, float32_t out[4]) {
  if (property->kind != VKR_PROPERTY_F32 &&
      property->kind != VKR_PROPERTY_ANGLE &&
      vkr_property_components(property) < 2u) {
    return false_v;
  }
  MemCopy(out, property_address(property, value),
          sizeof(float32_t) * vkr_property_components(property));
  return true_v;
}

bool8_t vkr_property_set_floats(const VkrPropertyDesc *property, void *value,
                                const float32_t in[4]) {
  if (property->kind != VKR_PROPERTY_F32 &&
      property->kind != VKR_PROPERTY_ANGLE &&
      vkr_property_components(property) < 2u) {
    return false_v;
  }
  const uint32_t count = vkr_property_components(property);
  for (uint32_t i = 0u; i < count; ++i) {
    if (!isfinite(in[i])) {
      return false_v;
    }
  }
  /* Vec3 storage is four lanes; only the first three are written, so W keeps
   * its zero from the enclosing value. */
  MemCopy((uint8_t *)value + property->offset, in, sizeof(float32_t) * count);
  return true_v;
}

void vkr_property_direction_angles(Vec3 direction, float32_t *yaw_degrees,
                                   float32_t *elevation_degrees) {
  const float32_t horizontal = hypotf(direction.x, direction.z);
  *yaw_degrees = (float32_t)(atan2f(direction.x, -direction.z) * TYPE_DEGREES);
  *elevation_degrees =
      (float32_t)(atan2f(direction.y, horizontal) * TYPE_DEGREES);
}

Vec3 vkr_property_direction_from_angles(float32_t yaw_degrees,
                                        float32_t elevation_degrees) {
  const float32_t yaw = (float32_t)(yaw_degrees * TYPE_RADIANS);
  const float32_t elevation = (float32_t)(elevation_degrees * TYPE_RADIANS);
  return (Vec3){sinf(yaw) * cosf(elevation), sinf(elevation),
                -cosf(yaw) * cosf(elevation), 0.0f};
}

void vkr_property_quat_euler(Vec4 rotation, float32_t out_degrees[3]) {
  float32_t roll = 0.0f;
  float32_t pitch = 0.0f;
  float32_t yaw = 0.0f;
  vkr_quat_to_euler(rotation, &roll, &pitch, &yaw);
  out_degrees[0] = (float32_t)(roll * TYPE_DEGREES);
  out_degrees[1] = (float32_t)(pitch * TYPE_DEGREES);
  out_degrees[2] = (float32_t)(yaw * TYPE_DEGREES);
}

Vec4 vkr_property_quat_from_euler(const float32_t degrees[3]) {
  return vkr_quat_from_euler((float32_t)(degrees[0] * TYPE_RADIANS),
                             (float32_t)(degrees[1] * TYPE_RADIANS),
                             (float32_t)(degrees[2] * TYPE_RADIANS));
}

// =============================================================================
// Validation
// =============================================================================

static bool8_t property_validate(const VkrPropertyDesc *property,
                                 const void *value, char *error,
                                 uint32_t capacity) {
  const uint8_t *address = property_address(property, value);
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    if (*(const bool8_t *)address > 1u) {
      return type_fail(error, capacity, "%s must be true or false",
                       property->label);
    }
    return true_v;
  case VKR_PROPERTY_ENUM: {
    const uint32_t count = vkr_property_enum_count(property);
    if (*(const uint32_t *)address >= count) {
      return type_fail(error, capacity, "%s has an unknown value",
                       property->label);
    }
    return true_v;
  }
  case VKR_PROPERTY_STRING:
    if (!property->capacity ||
        !memchr(address, 0, (size_t)property->capacity)) {
      return type_fail(error, capacity, "%s is too long", property->label);
    }
    return true_v;
  case VKR_PROPERTY_ENTITY:
    /* Any id is well formed; whether it names an entity is the owner's
       question at publication. */
    return true_v;
  case VKR_PROPERTY_I32:
  case VKR_PROPERTY_U32: {
    float64_t number = 0.0;
    (void)vkr_property_get_number(property, value, &number);
    if (type_bounded(property) && !(property->zero_label && number == 0.0) &&
        (number < property->min || number > property->max)) {
      return type_fail(error, capacity, "%s must be in [%g, %g]",
                       property->label, property->min, property->max);
    }
    return true_v;
  }
  default:
    break;
  }

  float32_t components[4] = {0};
  const uint32_t count = vkr_property_components(property);
  MemCopy(components, address, sizeof(float32_t) * count);
  float64_t length_squared = 0.0;
  for (uint32_t i = 0u; i < count; ++i) {
    if (!isfinite(components[i])) {
      return type_fail(error, capacity, "%s must be finite", property->label);
    }
    length_squared += (float64_t)components[i] * components[i];
    float64_t bounded = components[i];
    if (property->kind == VKR_PROPERTY_ANGLE) {
      bounded *= TYPE_DEGREES;
    }
    /* Angles compare in the degrees their bounds are written in; a small
     * tolerance absorbs the radian round trip of an exact bound. */
    const float64_t tolerance =
        property->kind == VKR_PROPERTY_ANGLE ? 1e-4 : 0.0;
    const bool8_t zero_off = property->zero_label && bounded == 0.0;
    if (type_bounded(property) && !zero_off &&
        (bounded < property->min - tolerance ||
         bounded > property->max + tolerance)) {
      return type_fail(error, capacity, "%s must be in [%g, %g]%s%s",
                       property->label, property->min, property->max,
                       property->unit ? " " : "",
                       property->unit ? property->unit : "");
    }
  }
  if (property->kind == VKR_PROPERTY_QUAT && !(length_squared >= 1e-12)) {
    return type_fail(error, capacity, "%s must not be zero", property->label);
  }
  return true_v;
}

bool8_t vkr_type_validate(const VkrTypeDesc *type, const void *value,
                          char *error, uint32_t capacity) {
  if (!type || !value) {
    return type_fail(error, capacity, "Missing value");
  }
  for (uint32_t i = 0u; i < type->property_count; ++i) {
    if (!property_validate(&type->properties[i], value, error, capacity)) {
      return false_v;
    }
  }
  if (type->validate) {
    if (error && capacity) {
      error[0] = 0;
    }
    if (!type->validate(value, error, capacity)) {
      if (error && capacity && !error[0]) {
        snprintf(error, capacity, "Invalid %s", type->label);
      }
      return false_v;
    }
  }
  return true_v;
}

void vkr_type_defaults(const VkrTypeDesc *type, void *value) {
  MemZero(value, type->size);
  if (type->defaults) {
    type->defaults(value);
  }
}

static bool8_t type_retired(const VkrTypeDesc *type, String8 name) {
  for (uint32_t i = 0u; type->retired && type->retired[i]; ++i) {
    if (type_name_equals(type->retired[i], name)) {
      return true_v;
    }
  }
  return false_v;
}

/* Skip one JSON value of a retired member: a scalar, string or a flat
 * array of scalars, which is every shape older documents wrote. */
static bool8_t json_skip_value(VkrJsonReader *reader);

VkrPropertyState vkr_type_property_state(const VkrTypeDesc *type,
                                         const void *value, uint32_t property,
                                         const void *context) {
  if (!type->state) {
    return (VkrPropertyState){0};
  }
  return type->state(value, property, context);
}

static uint32_t property_storage_size(const VkrPropertyDesc *property) {
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    return sizeof(bool8_t);
  case VKR_PROPERTY_STRING:
    return property->capacity;
  case VKR_PROPERTY_ENTITY:
    return sizeof(VkrEntityRef);
  case VKR_PROPERTY_I32:
  case VKR_PROPERTY_U32:
  case VKR_PROPERTY_ENUM:
    return sizeof(uint32_t);
  default:
    return sizeof(float32_t) * vkr_property_components(property);
  }
}

bool8_t vkr_property_equal(const VkrPropertyDesc *property, const void *a,
                           const void *b) {
  if (property->kind == VKR_PROPERTY_STRING) {
    return strncmp((const char *)property_address(property, a),
                   (const char *)property_address(property, b),
                   property->capacity) == 0;
  }
  return MemCompare(property_address(property, a),
                    property_address(property, b),
                    property_storage_size(property)) == 0;
}

static bool8_t property_scalar(VkrPropertyKind kind) {
  return kind == VKR_PROPERTY_BOOL || kind == VKR_PROPERTY_I32 ||
         kind == VKR_PROPERTY_U32 || kind == VKR_PROPERTY_F32 ||
         kind == VKR_PROPERTY_ANGLE || kind == VKR_PROPERTY_ENUM;
}

static bool8_t property_vector(VkrPropertyKind kind) {
  return kind == VKR_PROPERTY_VEC2 || kind == VKR_PROPERTY_VEC3 ||
         kind == VKR_PROPERTY_VEC4 || kind == VKR_PROPERTY_QUAT ||
         kind == VKR_PROPERTY_COLOR || kind == VKR_PROPERTY_DIRECTION;
}

/* ANGLE bounds are degrees over radians; they stay unapplied here. */
static float64_t property_clamp(const VkrPropertyDesc *property,
                                float64_t value) {
  if (property->min < property->max && property->kind != VKR_PROPERTY_ANGLE) {
    value = Max(value, (float64_t)property->min);
    value = Min(value, (float64_t)property->max);
  }
  return value;
}

/* Converts one property's value; false leaves `to_value` as it was. */
static bool8_t property_migrate(const VkrPropertyDesc *from,
                                const void *from_value,
                                const VkrPropertyDesc *to, void *to_value) {
  uint8_t *target = (uint8_t *)to_value + to->offset;
  const uint8_t *source = property_address(from, from_value);
  if (from->kind == VKR_PROPERTY_STRING || to->kind == VKR_PROPERTY_STRING) {
    if (from->kind != to->kind || !to->capacity) {
      return false_v;
    }
    uint32_t length = 0u;
    while (length < from->capacity && source[length]) {
      ++length;
    }
    const uint32_t kept = Min(length, to->capacity - 1u);
    MemCopy(target, source, kept);
    MemZero(target + kept, to->capacity - kept);
    return true_v;
  }
  if (from->kind == VKR_PROPERTY_ENTITY || to->kind == VKR_PROPERTY_ENTITY) {
    if (from->kind != to->kind) {
      return false_v;
    }
    MemCopy(target, source, sizeof(VkrEntityRef));
    return true_v;
  }
  if (property_scalar(from->kind) && property_scalar(to->kind)) {
    float64_t number = 0.0;
    return vkr_property_get_number(from, from_value, &number) &&
           vkr_property_set_number(to, to_value, property_clamp(to, number));
  }
  if (property_vector(from->kind) && property_vector(to->kind)) {
    float32_t values[4] = {0};
    float32_t current[4] = {0};
    if (!vkr_property_get_floats(from, from_value, values) ||
        !vkr_property_get_floats(to, to_value, current)) {
      return false_v;
    }
    const uint32_t shared =
        Min(vkr_property_components(from), vkr_property_components(to));
    for (uint32_t i = 0; i < shared; ++i) {
      current[i] = (float32_t)property_clamp(to, values[i]);
    }
    return vkr_property_set_floats(to, to_value, current);
  }
  return false_v;
}

void vkr_type_migrate(const VkrTypeDesc *from, const void *from_value,
                      const VkrTypeDesc *to, void *to_value) {
  vkr_type_defaults(to, to_value);
  for (uint32_t i = 0; i < to->property_count; ++i) {
    const VkrPropertyDesc *property = &to->properties[i];
    const uint32_t index = vkr_type_find_property(
        from, string8_create_from_cstr((const uint8_t *)property->name,
                                       strlen(property->name)));
    if (index == UINT32_MAX) {
      continue;
    }
    /* A failed conversion must not leave half a value behind. */
    _Alignas(16) uint8_t kept[VKR_TYPE_VALUE_MAX];
    const uint32_t size = property_storage_size(property);
    MemCopy(kept, (uint8_t *)to_value + property->offset, size);
    if (!property_migrate(&from->properties[index], from_value, property,
                          to_value)) {
      MemCopy((uint8_t *)to_value + property->offset, kept, size);
    }
  }
}

// =============================================================================
// JSON
// =============================================================================

bool8_t vkr_type_write_json(VkrJsonWriter *writer, const VkrTypeDesc *type,
                            const void *value) {
  if (!writer || !vkr_type_validate(type, value, NULL, 0u) ||
      !vkr_json_writer_begin_object(writer)) {
    return false_v;
  }
  if (type->version && (!vkr_json_writer_name(writer, string8_lit("version")) ||
                        !vkr_json_writer_u64(writer, type->version))) {
    return false_v;
  }
  for (uint32_t i = 0u; i < type->property_count; ++i) {
    const VkrPropertyDesc *property = &type->properties[i];
    if (property->flags & VKR_PROPERTY_FLAG_TRANSIENT) {
      continue;
    }
    const uint8_t *address = property_address(property, value);
    if (!vkr_json_writer_name(
            writer, string8_create_from_cstr((const uint8_t *)property->name,
                                             strlen(property->name)))) {
      return false_v;
    }
    bool8_t written = false_v;
    switch (property->kind) {
    case VKR_PROPERTY_BOOL:
      written = vkr_json_writer_bool(writer, *(const bool8_t *)address);
      break;
    case VKR_PROPERTY_I32:
      written = vkr_json_writer_i64(writer, *(const int32_t *)address);
      break;
    case VKR_PROPERTY_U32:
      written = vkr_json_writer_u64(writer, *(const uint32_t *)address);
      break;
    case VKR_PROPERTY_ENUM: {
      const char *name = property->names[*(const uint32_t *)address];
      written = vkr_json_writer_string(
          writer,
          string8_create_from_cstr((const uint8_t *)name, strlen(name)));
      break;
    }
    case VKR_PROPERTY_F32:
    case VKR_PROPERTY_ANGLE:
      written = vkr_json_writer_f64(writer, *(const float32_t *)address);
      break;
    case VKR_PROPERTY_STRING:
      written = vkr_json_writer_string(
          writer,
          string8_create_from_cstr(address, strlen((const char *)address)));
      break;
    case VKR_PROPERTY_ENTITY: {
      char text[37] = {0};
      if (!vkr_entity_ref_empty((const VkrEntityRef *)address)) {
        vkr_entity_ref_format((const VkrEntityRef *)address, text);
      }
      written = vkr_json_writer_string(
          writer,
          string8_create_from_cstr((const uint8_t *)text, strlen(text)));
      break;
    }
    default: {
      const uint32_t count = vkr_property_components(property);
      written = vkr_json_writer_begin_array(writer);
      for (uint32_t c = 0u; written && c < count; ++c) {
        written = vkr_json_writer_f64(writer, ((const float32_t *)address)[c]);
      }
      written = written && vkr_json_writer_end_array(writer);
      break;
    }
    }
    if (!written) {
      return false_v;
    }
  }
  return vkr_json_writer_end_object(writer);
}

static bool8_t json_take(VkrJsonReader *reader, uint8_t token) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != token) {
    return false_v;
  }
  ++reader->pos;
  return true_v;
}

static bool8_t json_peek(VkrJsonReader *reader, uint8_t token) {
  vkr_json_skip_whitespace(reader);
  return reader->pos < reader->length && reader->data[reader->pos] == token;
}

static bool8_t json_skip_value(VkrJsonReader *reader) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length) {
    return false_v;
  }
  const uint8_t c = reader->data[reader->pos];
  if (c == '"') {
    String8 text = {0};
    return vkr_json_parse_string(reader, &text);
  }
  if (c == 't' || c == 'f') {
    bool8_t flag = false_v;
    return vkr_json_parse_bool(reader, &flag);
  }
  if (c == '[') {
    ++reader->pos;
    if (json_take(reader, ']')) {
      return true_v;
    }
    for (;;) {
      float64_t number = 0.0;
      if (!vkr_json_parse_double(reader, &number)) {
        return false_v;
      }
      if (json_take(reader, ']')) {
        return true_v;
      }
      if (!json_take(reader, ',')) {
        return false_v;
      }
    }
  }
  float64_t number = 0.0;
  return vkr_json_parse_double(reader, &number);
}

static bool8_t json_read_property(VkrJsonReader *reader,
                                  const VkrPropertyDesc *property,
                                  uint8_t *candidate, VkrAllocator *scratch,
                                  char *error, uint32_t capacity) {
  uint8_t *address = candidate + property->offset;
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    if (!vkr_json_parse_bool(reader, (bool8_t *)address)) {
      return type_fail(error, capacity, "%s expects true or false",
                       property->name);
    }
    return true_v;
  case VKR_PROPERTY_I32:
  case VKR_PROPERTY_U32: {
    float64_t number = 0.0;
    if (!vkr_json_parse_double(reader, &number) || number != floor(number) ||
        !vkr_property_set_number(property, candidate, number)) {
      return type_fail(error, capacity, "%s expects an integer",
                       property->name);
    }
    return true_v;
  }
  case VKR_PROPERTY_ENUM: {
    String8 text = {0};
    if (!vkr_json_parse_string(reader, &text)) {
      return type_fail(error, capacity, "%s expects a name", property->name);
    }
    const uint32_t count = vkr_property_enum_count(property);
    for (uint32_t i = 0u; i < count; ++i) {
      if (type_name_equals(property->names[i], text)) {
        *(uint32_t *)address = i;
        return true_v;
      }
    }
    return type_fail(error, capacity, "%s has no value '%.*s'", property->name,
                     (int)Min(text.length, 64u), text.str);
  }
  case VKR_PROPERTY_F32:
  case VKR_PROPERTY_ANGLE: {
    float64_t number = 0.0;
    if (!vkr_json_parse_double(reader, &number) ||
        !vkr_property_set_number(property, candidate, number)) {
      return type_fail(error, capacity, "%s expects a finite number",
                       property->name);
    }
    return true_v;
  }
  case VKR_PROPERTY_ENTITY: {
    String8 text = {0};
    VkrEntityRef id = {0};
    if (!vkr_json_parse_string(reader, &text) ||
        (text.length &&
         !vkr_entity_ref_parse((const char *)text.str, text.length, &id))) {
      return type_fail(error, capacity, "%s expects an entity id or \"\"",
                       property->name);
    }
    MemCopy(address, &id, sizeof(id));
    return true_v;
  }
  case VKR_PROPERTY_STRING: {
    String8 text = {0};
    if (!scratch) {
      return type_fail(error, capacity, "%s needs a scratch allocator",
                       property->name);
    }
    if (!vkr_json_parse_string_decoded(reader, scratch, &text)) {
      return type_fail(error, capacity, "%s expects a string", property->name);
    }
    const bool8_t fits = text.length < property->capacity;
    if (fits) {
      MemCopy(address, text.str, text.length);
      MemZero(address + text.length, property->capacity - text.length);
    }
    vkr_allocator_free(scratch, text.str, text.length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!fits) {
      return type_fail(error, capacity, "%s is longer than %u bytes",
                       property->name, property->capacity - 1u);
    }
    return true_v;
  }
  default:
    break;
  }

  const uint32_t count = vkr_property_components(property);
  float32_t components[4] = {0};
  if (!json_take(reader, '[')) {
    return type_fail(error, capacity, "%s expects an array of %u numbers",
                     property->name, count);
  }
  for (uint32_t i = 0u; i < count; ++i) {
    float64_t number = 0.0;
    if ((i && !json_take(reader, ',')) ||
        !vkr_json_parse_double(reader, &number) ||
        !isfinite((float32_t)number)) {
      return type_fail(error, capacity, "%s expects an array of %u numbers",
                       property->name, count);
    }
    components[i] = (float32_t)number;
  }
  if (!json_take(reader, ']')) {
    return type_fail(error, capacity, "%s expects an array of %u numbers",
                     property->name, count);
  }
  MemCopy(address, components, sizeof(float32_t) * count);
  return true_v;
}

bool8_t vkr_type_read_json(VkrJsonReader *reader, const VkrTypeDesc *type,
                           void *value, VkrAllocator *scratch, char *error,
                           uint32_t capacity) {
  if (!reader || !type || !value || type->size > VKR_TYPE_VALUE_MAX ||
      type->property_count > VKR_TYPE_PROPERTY_MAX) {
    return type_fail(error, capacity, "Unsupported type");
  }
  const uint64_t start = reader->pos;
  uint8_t candidate[VKR_TYPE_VALUE_MAX];
  MemCopy(candidate, value, type->size);
  uint64_t seen = 0u;
  bool8_t version_seen = false_v;

  if (!json_take(reader, '{')) {
    reader->pos = start;
    return type_fail(error, capacity, "%s expects an object", type->name);
  }
  if (!json_take(reader, '}')) {
    for (;;) {
      String8 key = {0};
      if (!vkr_json_parse_string(reader, &key) || !json_take(reader, ':')) {
        reader->pos = start;
        return type_fail(error, capacity, "%s has a malformed member",
                         type->name);
      }
      if (type->version && type_name_equals("version", key)) {
        float64_t version = 0.0;
        if (version_seen || !vkr_json_parse_double(reader, &version) ||
            version != (float64_t)type->version) {
          reader->pos = start;
          return type_fail(error, capacity, "%s requires version %u",
                           type->name, type->version);
        }
        version_seen = true_v;
      } else if (type_retired(type, key)) {
        if (!json_skip_value(reader)) {
          reader->pos = start;
          return type_fail(error, capacity, "%s has a malformed member",
                           type->name);
        }
      } else {
        const bool8_t tolerant = (type->flags & VKR_TYPE_FLAG_TOLERANT) != 0u;
        const uint32_t index = vkr_type_find_property(type, key);
        if (tolerant && index == UINT32_MAX) {
          /* A member an older layout had. */
          if (!json_skip_value(reader)) {
            reader->pos = start;
            return type_fail(error, capacity, "%s has a malformed member",
                             type->name);
          }
        } else {
          if (index == UINT32_MAX ||
              (type->properties[index].flags & VKR_PROPERTY_FLAG_TRANSIENT)) {
            reader->pos = start;
            return type_fail(error, capacity, "%s has no property '%.*s'",
                             type->name, (int)Min(key.length, 64u), key.str);
          }
          if (seen & (UINT64_C(1) << index)) {
            reader->pos = start;
            return type_fail(error, capacity, "%s repeats '%s'", type->name,
                             type->properties[index].name);
          }
          seen |= UINT64_C(1) << index;
          const uint64_t member = reader->pos;
          if (!json_read_property(reader, &type->properties[index], candidate,
                                  scratch, error, capacity)) {
            /* A value of a property whose kind changed keeps the default. */
            reader->pos = member;
            if (!tolerant || !json_skip_value(reader)) {
              reader->pos = start;
              return false_v;
            }
            if (error && capacity) {
              error[0] = '\0';
            }
          }
        }
      }
      if (json_take(reader, '}')) {
        break;
      }
      if (!json_take(reader, ',') || json_peek(reader, '}')) {
        reader->pos = start;
        return type_fail(error, capacity, "%s has a malformed member list",
                         type->name);
      }
    }
  }
  if (type->version && !version_seen) {
    reader->pos = start;
    return type_fail(error, capacity, "%s requires version %u", type->name,
                     type->version);
  }
  if (!vkr_type_validate(type, candidate, error, capacity)) {
    reader->pos = start;
    return false_v;
  }
  MemCopy(value, candidate, type->size);
  return true_v;
}

bool8_t vkr_type_read_json_document(String8 json, const VkrTypeDesc *type,
                                    void *value, VkrAllocator *scratch,
                                    char *error, uint32_t capacity) {
  if (!json.str || !json.length) {
    return type_fail(error, capacity, "Empty document");
  }
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  uint8_t candidate[VKR_TYPE_VALUE_MAX];
  if (!type || type->size > VKR_TYPE_VALUE_MAX) {
    return type_fail(error, capacity, "Unsupported type");
  }
  MemCopy(candidate, value, type->size);
  if (!vkr_type_read_json(&reader, type, candidate, scratch, error, capacity)) {
    return false_v;
  }
  vkr_json_skip_whitespace(&reader);
  if (reader.pos != reader.length) {
    return type_fail(error, capacity, "Unexpected text after %s", type->name);
  }
  MemCopy(value, candidate, type->size);
  return true_v;
}
