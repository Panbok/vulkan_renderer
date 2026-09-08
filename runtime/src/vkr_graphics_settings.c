#include "vkr_graphics_settings.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum GraphicsFieldType {
  GRAPHICS_BOOL,
  GRAPHICS_FLOAT,
  GRAPHICS_UINT
} GraphicsFieldType;
typedef struct GraphicsField {
  const char *name;
  uint32_t offset;
  GraphicsFieldType type;
  float32_t minimum, maximum;
} GraphicsField;
#define BOOL_FIELD(n)                                                          \
  {#n, offsetof(VkrGraphicsSettings, n), GRAPHICS_BOOL, 0, 1}
#define FLOAT_FIELD(n, lo, hi)                                                 \
  {#n, offsetof(VkrGraphicsSettings, n), GRAPHICS_FLOAT, lo, hi}
#define UINT_FIELD(n, hi)                                                      \
  {#n, offsetof(VkrGraphicsSettings, n), GRAPHICS_UINT, 0, hi}
static const GraphicsField s_fields[] = {
    BOOL_FIELD(vsync),
    BOOL_FIELD(hdr),
    BOOL_FIELD(temporal_upscaling),
    BOOL_FIELD(dynamic_resolution),
    BOOL_FIELD(anti_aliasing),
    FLOAT_FIELD(render_scale, 1.0f / 3.0f, 1.0f),
    UINT_FIELD(frame_limit, 240),
    UINT_FIELD(shadow_quality, 2),
    BOOL_FIELD(soft_shadows),
    BOOL_FIELD(local_shadows),
    BOOL_FIELD(ambient_occlusion),
    BOOL_FIELD(screen_space_reflections),
    BOOL_FIELD(screen_space_gi),
    BOOL_FIELD(reflection_probes),
    BOOL_FIELD(subsurface_scattering),
    BOOL_FIELD(fog),
    BOOL_FIELD(volumetric_fog),
    BOOL_FIELD(bloom),
    BOOL_FIELD(depth_of_field),
    BOOL_FIELD(motion_blur),
    FLOAT_FIELD(brightness, -3, 3),
    FLOAT_FIELD(contrast, .5f, 1.5f),
    FLOAT_FIELD(saturation, 0, 2),
    FLOAT_FIELD(temperature, -1, 1),
    FLOAT_FIELD(tint, -1, 1),
    FLOAT_FIELD(sharpness, 0, 1),
    FLOAT_FIELD(bloom_intensity, 0, 1),
    FLOAT_FIELD(motion_blur_amount, 0, 1),
};
#undef BOOL_FIELD
#undef FLOAT_FIELD
#undef UINT_FIELD
_Static_assert(ArrayCount(s_fields) < 63,
               "Settings duplicate-field mask capacity");

VkrGraphicsSettings
vkr_graphics_settings_defaults(VkrRendererBackendType backend) {
  return (VkrGraphicsSettings){
      .vsync = true_v,
      .hdr = false_v,
      .temporal_upscaling = true_v,
      .dynamic_resolution = backend == VKR_RENDERER_BACKEND_TYPE_METAL,
      .anti_aliasing = true_v,
      .render_scale =
          backend == VKR_RENDERER_BACKEND_TYPE_METAL ? .8f : 2.0f / 3.0f,
      .shadow_quality = 2,
      .soft_shadows = true_v,
      .local_shadows = true_v,
      .ambient_occlusion = true_v,
      .screen_space_reflections = true_v,
      .reflection_probes = true_v,
      .subsurface_scattering = true_v,
      .fog = true_v,
      .volumetric_fog = true_v,
      .bloom = true_v,
      .contrast = 1,
      .saturation = 1,
      .sharpness = .25f,
      .bloom_intensity = .05f,
      .motion_blur_amount = .5f,
  };
}

bool8_t vkr_graphics_settings_valid(const VkrGraphicsSettings *settings) {
  if (!settings)
    return false_v;
  for (uint32_t i = 0; i < ArrayCount(s_fields); ++i) {
    const GraphicsField *field = &s_fields[i];
    const uint8_t *address = (const uint8_t *)settings + field->offset;
    const float64_t value =
        field->type == GRAPHICS_BOOL   ? *(const bool8_t *)address
        : field->type == GRAPHICS_UINT ? *(const uint32_t *)address
                                       : *(const float32_t *)address;
    if (!isfinite(value) || value < field->minimum || value > field->maximum)
      return false_v;
  }
  return (!settings->temporal_upscaling || settings->anti_aliasing) &&
         (!settings->dynamic_resolution || settings->temporal_upscaling);
}

bool8_t vkr_graphics_settings_restart_required(const VkrGraphicsSettings *a,
                                               const VkrGraphicsSettings *b) {
  return a->vsync != b->vsync || a->hdr != b->hdr ||
         a->temporal_upscaling != b->temporal_upscaling ||
         a->dynamic_resolution != b->dynamic_resolution ||
         a->render_scale != b->render_scale;
}

static bool8_t graphics_take(VkrJsonReader *reader, uint8_t token) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != token)
    return false_v;
  ++reader->pos;
  return true_v;
}

bool8_t vkr_graphics_settings_load(const char *path,
                                   VkrGraphicsSettings *settings) {
  if (!path || !path[0])
    return true_v;
  FILE *file = fopen(path, "rb");
  if (!file)
    return errno == ENOENT;
  uint8_t bytes[4096];
  const size_t length = fread(bytes, 1, sizeof(bytes), file);
  const bool8_t read_ok = !ferror(file) && length < sizeof(bytes);
  fclose(file);
  if (!read_ok)
    return false_v;
  VkrJsonReader reader = vkr_json_reader_create(bytes, length);
  VkrGraphicsSettings candidate = *settings;
  uint64_t seen = 0;
  bool8_t version_seen = false_v;
  if (!graphics_take(&reader, '{'))
    return false_v;
  do {
    String8 key = {0};
    if (!vkr_json_parse_string(&reader, &key) || !graphics_take(&reader, ':'))
      return false_v;
    if (key.length == 7 && MemCompare(key.str, "version", 7) == 0) {
      int32_t version;
      if (version_seen || !vkr_json_parse_int(&reader, &version) ||
          version != 1)
        return false_v;
      version_seen = true_v;
    } else {
      uint32_t index = 0;
      while (index < ArrayCount(s_fields) &&
             (strlen(s_fields[index].name) != key.length ||
              MemCompare(s_fields[index].name, key.str, key.length) != 0))
        ++index;
      if (index == ArrayCount(s_fields) || (seen & (UINT64_C(1) << index)))
        return false_v;
      seen |= UINT64_C(1) << index;
      const GraphicsField *field = &s_fields[index];
      uint8_t *address = (uint8_t *)&candidate + field->offset;
      bool8_t ok;
      if (field->type == GRAPHICS_BOOL)
        ok = vkr_json_parse_bool(&reader, (bool8_t *)address);
      else if (field->type == GRAPHICS_FLOAT)
        ok = vkr_json_parse_float(&reader, (float32_t *)address);
      else {
        int32_t value = 0;
        ok = vkr_json_parse_int(&reader, &value) && value >= 0;
        *(uint32_t *)address = (uint32_t)value;
      }
      if (!ok)
        return false_v;
    }
    if (graphics_take(&reader, '}'))
      break;
    if (!graphics_take(&reader, ','))
      return false_v;
  } while (reader.pos < reader.length);
  vkr_json_skip_whitespace(&reader);
  if (!version_seen || reader.pos != reader.length || !length ||
      bytes[length - 1] == ',' || !vkr_graphics_settings_valid(&candidate))
    return false_v;
  /* Require the closing token, including files ending immediately after a
   * value. */
  uint64_t end = length;
  while (end && (bytes[end - 1] == ' ' || bytes[end - 1] == '\n' ||
                 bytes[end - 1] == '\r' || bytes[end - 1] == '\t'))
    --end;
  if (!end || bytes[end - 1] != '}')
    return false_v;
  *settings = candidate;
  return true_v;
}

bool8_t vkr_graphics_settings_save(const char *path,
                                   const VkrGraphicsSettings *settings) {
  if (!vkr_graphics_settings_valid(settings))
    return false_v;
  if (!path || !path[0])
    return true_v;
  char temporary[1100];
  const int count = snprintf(temporary, sizeof(temporary), "%s.%u.tmp", path,
                             vkr_platform_get_process_id());
  if (count < 0 || count >= (int)sizeof(temporary))
    return false_v;
  FILE *file = fopen(temporary, "wb");
  if (!file)
    return false_v;
  bool8_t ok = fprintf(file, "{\n  \"version\": 1") > 0;
  for (uint32_t i = 0; i < ArrayCount(s_fields); ++i) {
    const GraphicsField *field = &s_fields[i];
    const uint8_t *address = (const uint8_t *)settings + field->offset;
    int result;
    if (field->type == GRAPHICS_BOOL)
      result = fprintf(file, ",\n  \"%s\": %s", field->name,
                       *(const bool8_t *)address ? "true" : "false");
    else if (field->type == GRAPHICS_UINT)
      result = fprintf(file, ",\n  \"%s\": %u", field->name,
                       *(const uint32_t *)address);
    else
      result = fprintf(file, ",\n  \"%s\": %.9g", field->name,
                       (double)*(const float32_t *)address);
    ok &= result > 0;
  }
  ok &= fprintf(file, "\n}\n") > 0;
  ok &= fclose(file) == 0;
  if (ok) {
    const FilePath source = {
        .path = string8_create((uint8_t *)temporary, strlen(temporary)),
        .type = FILE_PATH_TYPE_ABSOLUTE};
    const FilePath destination = {
        .path = string8_create((uint8_t *)path, strlen(path)),
        .type = FILE_PATH_TYPE_ABSOLUTE};
    ok = file_rename(&source, &destination, true_v) == FILE_ERROR_NONE;
  }
  if (!ok)
    remove(temporary);
  return ok;
}
