#include "type_desc_test.h"

#include "core/vkr_type_desc.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_types.h"
#include "vkr_graphics_settings.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Independent oracles: literal JSON documents and hand-computed values. The
 * descriptor is the code under test, so no test derives its expectation from
 * the descriptor table itself. */

typedef struct TestValue {
  bool8_t flag;
  uint32_t count;
  int32_t offset;
  float32_t scale;
  float32_t angle;
  Vec3 color;
  Vec3 direction;
  Vec2 size;
  uint32_t mode;
  char name[8];
  VkrEntityRef target;
  float32_t runtime;
} TestValue;

static const char *const s_modes[] = {"first", "second", NULL};

static const VkrPropertyDesc s_test_properties[] = {
    {.name = "flag",
     .label = "Flag",
     .offset = offsetof(TestValue, flag),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "count",
     .label = "Count",
     .offset = offsetof(TestValue, count),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 10.0f},
    {.name = "offset",
     .label = "Offset",
     .offset = offsetof(TestValue, offset),
     .kind = VKR_PROPERTY_I32},
    {.name = "scale",
     .label = "Scale",
     .offset = offsetof(TestValue, scale),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 2.0f},
    {.name = "angle",
     .label = "Angle",
     .offset = offsetof(TestValue, angle),
     .kind = VKR_PROPERTY_ANGLE,
     .min = 0.0f,
     .max = 90.0f},
    {.name = "color",
     .label = "Color",
     .offset = offsetof(TestValue, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = FLT_MAX},
    {.name = "direction",
     .label = "Direction",
     .offset = offsetof(TestValue, direction),
     .kind = VKR_PROPERTY_DIRECTION},
    {.name = "size",
     .label = "Size",
     .offset = offsetof(TestValue, size),
     .kind = VKR_PROPERTY_VEC2},
    {.name = "mode",
     .label = "Mode",
     .names = s_modes,
     .offset = offsetof(TestValue, mode),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "name",
     .label = "Name",
     .offset = offsetof(TestValue, name),
     .capacity = sizeof(((TestValue *)0)->name),
     .kind = VKR_PROPERTY_STRING},
    {.name = "target",
     .label = "Target",
     .offset = offsetof(TestValue, target),
     .kind = VKR_PROPERTY_ENTITY},
    {.name = "runtime",
     .label = "Runtime",
     .offset = offsetof(TestValue, runtime),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_TRANSIENT},
};

static bool8_t test_validate(const void *value, char *error,
                             uint32_t capacity) {
  const TestValue *test = value;
  if (test->flag && test->count == 0u) {
    snprintf(error, capacity, "flag needs a count");
    return false_v;
  }
  return true_v;
}

static const VkrTypeDesc s_test_type = {
    .name = "test_value",
    .label = "Test value",
    .properties = s_test_properties,
    .property_count = ArrayCount(s_test_properties),
    .size = sizeof(TestValue),
    .version = 3u,
    .validate = test_validate,
};

typedef struct TypeSink {
  uint8_t data[4096];
  uint64_t length;
} TypeSink;

static bool8_t type_sink_write(void *context, const uint8_t *data,
                               uint64_t length) {
  TypeSink *sink = context;
  if (sink->length + length > sizeof(sink->data)) {
    return false_v;
  }
  MemCopy(sink->data + sink->length, data, length);
  sink->length += length;
  return true_v;
}

static TestValue test_value_sample(void) {
  TestValue value = {0};
  value.flag = true_v;
  value.count = 7u;
  value.offset = -12;
  value.scale = 1.25f;
  value.angle = 0.5f;
  value.color = vec3_new(0.25f, 0.5f, 1.0f);
  value.direction = vec3_new(0.0f, -1.0f, 0.0f);
  value.size = vec2_new(3.0f, 4.5f);
  value.mode = 1u;
  MemCopy(value.name, "abc", 4);
  for (uint32_t i = 0; i < 16u; ++i) {
    value.target.bytes[i] = (uint8_t)i;
  }
  value.runtime = 99.0f;
  return value;
}

static bool8_t test_read(const char *json, TestValue *value, char *error) {
  return vkr_type_read_json_document(
      string8_create_from_cstr((const uint8_t *)json, strlen(json)),
      &s_test_type, value, NULL, error, 128u);
}

static void test_type_round_trip(VkrAllocator *allocator) {
  printf("  Running test_type_round_trip...\n");
  const TestValue written = test_value_sample();
  TypeSink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, type_sink_write, &sink);
  assert(vkr_type_write_json(&writer, &s_test_type, &written));
  assert(vkr_json_writer_complete(&writer));
  const String8 json = string8_create(sink.data, sink.length);
  /* The version leads, enums are names and transient state is omitted. */
  assert(sink.length > 12u && MemCompare(sink.data, "{\"version\":3", 12) == 0);
  assert(strstr((const char *)sink.data, "\"mode\":\"second\""));
  /* An entity reference is its id in canonical text order. */
  assert(strstr((const char *)sink.data,
                "\"target\":\"00010203-0405-0607-0809-0a0b0c0d0e0f\""));
  assert(!strstr((const char *)sink.data, "runtime"));

  TestValue read = {0};
  read.runtime = 5.0f;
  char error[128] = {0};
  assert(vkr_type_read_json_document(json, &s_test_type, &read, allocator,
                                     error, sizeof(error)));
  assert(read.flag == true_v && read.count == 7u && read.offset == -12);
  assert(read.scale == 1.25f && read.angle == 0.5f);
  assert(read.color.x == 0.25f && read.color.y == 0.5f && read.color.z == 1.0f);
  assert(read.color.w == 0.0f);
  assert(read.direction.y == -1.0f && read.size.y == 4.5f);
  assert(read.mode == 1u && strcmp(read.name, "abc") == 0);
  assert(read.target.bytes[0] == 0u && read.target.bytes[10] == 10u &&
         read.target.bytes[15] == 15u);
  /* Transient runtime state belongs to the destination, not the document. */
  assert(read.runtime == 5.0f);
  printf("  test_type_round_trip PASSED\n");
}

static void test_type_rejections(VkrAllocator *allocator) {
  printf("  Running test_type_rejections...\n");
  const TestValue original = test_value_sample();
  TestValue value = original;
  char error[128];
  static const char *const rejected[] = {
      "{\"count\":1}",                           /* missing version */
      "{\"version\":2,\"count\":1}",             /* wrong version */
      "{\"version\":3,\"unknown\":1}",           /* unknown property */
      "{\"version\":3,\"count\":1,\"count\":2}", /* duplicate */
      "{\"version\":3,\"count\":11}",            /* above bound */
      "{\"version\":3,\"count\":1.5}",           /* fractional integer */
      "{\"version\":3,\"count\":-1}",            /* negative unsigned */
      "{\"version\":3,\"scale\":-0.1}",          /* below bound */
      "{\"version\":3,\"angle\":1.6}",           /* 91.7 degrees */
      "{\"version\":3,\"color\":[1,2]}",         /* short vector */
      "{\"version\":3,\"color\":[1,-2,3]}",      /* negative component */
      "{\"version\":3,\"mode\":\"third\"}",      /* unknown enum name */
      "{\"version\":3,\"mode\":1}",              /* enum as number */
      "{\"version\":3,\"flag\":1}",              /* bool as number */
      "{\"version\":3,\"runtime\":1}",           /* transient is not data */
      "{\"version\":3,\"target\":\"door\"}",     /* a name is not an id */
      "{\"version\":3,\"target\":7}",            /* id as number */
      "{\"version\":3,\"count\":0}",             /* validate hook */
      "{\"version\":3,\"count\":1,}",            /* trailing comma */
      "{\"version\":3} x",                       /* trailing text */
  };
  for (uint32_t i = 0; i < ArrayCount(rejected); ++i) {
    error[0] = 0;
    assert(!test_read(rejected[i], &value, error));
    assert(error[0]);
    assert(MemCompare(&value, &original, sizeof(value)) == 0);
  }
  /* The validate hook's message reaches the caller. */
  assert(!test_read("{\"version\":3,\"count\":0}", &value, error));
  assert(strcmp(error, "flag needs a count") == 0);
  /* Strings need a scratch allocator and must fit their capacity. */
  assert(!test_read("{\"version\":3,\"name\":\"x\"}", &value, error));
  assert(!vkr_type_read_json_document(
      string8_lit("{\"version\":3,\"name\":\"12345678\"}"), &s_test_type,
      &value, allocator, error, sizeof(error)));
  assert(MemCompare(&value, &original, sizeof(value)) == 0);
  /* Escapes decode; missing members keep their values. */
  assert(vkr_type_read_json_document(
      string8_lit("{ \"version\" : 3 , \"name\" : \"a\\\"b\" }"), &s_test_type,
      &value, allocator, error, sizeof(error)));
  assert(strcmp(value.name, "a\"b") == 0 && value.count == 7u);
  /* An empty id is no entity. */
  assert(test_read("{\"version\":3,\"target\":\"\"}", &value, error));
  assert(vkr_entity_ref_empty(&value.target));
  assert(test_read("{\"version\":3}", &value, error));
  printf("  test_type_rejections PASSED\n");
}

static void test_type_property_access(void) {
  printf("  Running test_type_property_access...\n");
  TestValue value = test_value_sample();
  const uint32_t count =
      vkr_type_find_property(&s_test_type, string8_lit("count"));
  const uint32_t mode =
      vkr_type_find_property(&s_test_type, string8_lit("mode"));
  const uint32_t angle =
      vkr_type_find_property(&s_test_type, string8_lit("angle"));
  assert(count == 1u && mode == 8u && angle == 4u);
  assert(vkr_type_find_property(&s_test_type, string8_lit("nope")) ==
         UINT32_MAX);
  assert(vkr_property_set_number(&s_test_properties[count], &value, 3.6));
  assert(value.count == 4u);
  assert(!vkr_property_set_number(&s_test_properties[count], &value, -1.0));
  assert(!vkr_property_set_number(&s_test_properties[mode], &value, 2.0));
  assert(!vkr_property_set_number(&s_test_properties[count], &value, NAN));
  assert(vkr_property_enum_count(&s_test_properties[mode]) == 2u);
  assert(fabs(vkr_property_display_scale(&s_test_properties[angle]) -
              57.29577951308232) < 1e-9);

  /* Yaw zero faces -Z, positive yaw turns toward +X, elevation toward +Y. */
  float32_t yaw = 0.0f;
  float32_t elevation = 0.0f;
  vkr_property_direction_angles(vec3_new(0.0f, 0.0f, -1.0f), &yaw, &elevation);
  assert(fabsf(yaw) < 1e-4f && fabsf(elevation) < 1e-4f);
  vkr_property_direction_angles(vec3_new(1.0f, 0.0f, 0.0f), &yaw, &elevation);
  assert(fabsf(yaw - 90.0f) < 1e-4f);
  const Vec3 up = vkr_property_direction_from_angles(0.0f, 90.0f);
  assert(fabsf(up.y - 1.0f) < 1e-6f && up.w == 0.0f);

  const float32_t floats[4] = {9.0f, 8.0f, 7.0f, 6.0f};
  const uint32_t color =
      vkr_type_find_property(&s_test_type, string8_lit("color"));
  assert(vkr_property_set_floats(&s_test_properties[color], &value, floats));
  assert(value.color.z == 7.0f && value.color.w == 0.0f);
  printf("  test_type_property_access PASSED\n");
}

static void test_graphics_preferences_type(void) {
  printf("  Running test_graphics_preferences_type...\n");
  VkrGraphicsSettings settings =
      vkr_graphics_settings_defaults(VKR_RENDERER_BACKEND_TYPE_VULKAN);
  assert(vkr_graphics_settings_valid(&settings));
  /* Documents written by earlier editors stay readable: numeric shadow
   * quality and a partial member set. */
  assert(vkr_graphics_settings_read_json(
      string8_lit("{\"version\":1,\"shadow_quality\":1,\"frame_limit\":60}"),
      &settings));
  assert(settings.shadow_quality == 1u && settings.frame_limit == 60u);
  /* Files written before the high-DPI setting keep physical pixels. */
  assert(settings.high_dpi);
  assert(!vkr_graphics_settings_read_json(
      string8_lit("{\"version\":1,\"shadow_quality\":4}"), &settings));
  assert(!vkr_graphics_settings_read_json(
      string8_lit("{\"version\":1,\"temporal_upscaling\":true,"
                  "\"anti_aliasing\":false}"),
      &settings));
  assert(settings.shadow_quality == 1u);

  /* Normalization repairs the dependency an interactive toggle breaks. */
  settings.temporal_upscaling = true_v;
  settings.anti_aliasing = false_v;
  assert(!vkr_graphics_settings_valid(&settings));
  vkr_graphics_settings_type.normalize(&settings);
  assert(settings.anti_aliasing && vkr_graphics_settings_valid(&settings));

  /* Which backend offers dynamic resolution, and which effects the tiled
   * pipeline draws, follow the running renderer's state (ADR-087). */
  const uint32_t dynamic = vkr_type_find_property(
      &vkr_graphics_settings_type, string8_lit("dynamic_resolution"));
  const uint32_t occlusion = vkr_type_find_property(
      &vkr_graphics_settings_type, string8_lit("ambient_occlusion"));
  assert(dynamic != UINT32_MAX && occlusion != UINT32_MAX);
  const VkrGraphicsSettingsState vulkan_state = {
      .temporal_upscaling_available = true_v,
      .graphics_pipeline = VKR_GRAPHICS_PIPELINE_DESKTOP,
  };
  const VkrGraphicsSettingsState metal_state = {
      .dynamic_resolution_available = true_v,
      .graphics_pipeline = VKR_GRAPHICS_PIPELINE_TILED,
  };
  assert(vkr_type_property_state(&vkr_graphics_settings_type, &settings,
                                 dynamic, &vulkan_state)
             .flags &
         VKR_PROPERTY_STATE_DISABLED);
  assert(!(vkr_type_property_state(&vkr_graphics_settings_type, &settings,
                                   dynamic, &metal_state)
               .flags &
           VKR_PROPERTY_STATE_DISABLED));
  assert(!(vkr_type_property_state(&vkr_graphics_settings_type, &settings,
                                   occlusion, &vulkan_state)
               .flags &
           VKR_PROPERTY_STATE_DISABLED));
  assert(vkr_type_property_state(&vkr_graphics_settings_type, &settings,
                                 occlusion, &metal_state)
             .flags &
         VKR_PROPERTY_STATE_DISABLED);
  /* Files written while the pipeline class was a preference still load. */
  assert(vkr_graphics_settings_read_json(
      string8_lit("{\"version\":1,\"tiled_pipeline\":true}"), &settings));

  /* A project opened in a running editor keeps the machine's restart-time
   * settings and its own others, and leaves no restart pending. */
  VkrGraphicsSettings running =
      vkr_graphics_settings_defaults(VKR_RENDERER_BACKEND_TYPE_METAL);
  running.dynamic_resolution = false_v;
  running.vsync = false_v;
  running.hdr = true_v;
  running.render_scale = 0.75f;
  VkrGraphicsSettings project =
      vkr_graphics_settings_defaults(VKR_RENDERER_BACKEND_TYPE_METAL);
  project.dynamic_resolution = true_v;
  project.anti_aliasing = true_v;
  project.vsync = true_v;
  project.hdr = false_v;
  project.render_scale = 1.0f;
  project.shadow_quality = 0u;
  assert(vkr_graphics_settings_valid(&project));
  assert(vkr_graphics_settings_restart_required(&project, &running));
  vkr_graphics_settings_keep_restart(&project, &running);
  assert(!vkr_graphics_settings_restart_required(&project, &running));
  assert(vkr_graphics_settings_valid(&project));
  assert(project.shadow_quality == 0u);
  printf("  test_graphics_preferences_type PASSED\n");
}

static void test_light_types(void) {
  printf("  Running test_light_types...\n");
  ScenePointLight spot = {.color = vec3_new(1, 1, 1),
                          .intensity = 2.0f,
                          .constant = 1.0f,
                          .range = 5.0f,
                          .direction_local = vec3_new(0, 0, -1),
                          .inner_cone_angle = 0.2f,
                          .outer_cone_angle = 0.6f,
                          .kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT,
                          .enabled = true_v,
                          .casts_shadow = true_v};
  char error[128];
  assert(vkr_type_validate(&vkr_scene_point_light_type, &spot, error,
                           sizeof(error)));
  ScenePointLight invalid = spot;
  invalid.range = 0.0f;
  assert(!vkr_type_validate(&vkr_scene_point_light_type, &invalid, error,
                            sizeof(error)));
  invalid = spot;
  invalid.inner_cone_angle = 0.7f;
  assert(!vkr_type_validate(&vkr_scene_point_light_type, &invalid, error,
                            sizeof(error)));
  invalid = spot;
  invalid.color.y = -0.5f;
  assert(!vkr_type_validate(&vkr_scene_point_light_type, &invalid, error,
                            sizeof(error)));
  /* A point light may keep a zero direction; a spot may not. */
  ScenePointLight point = spot;
  point.kind = VKR_POINT_LIGHT_KIND_GLTF_POINT;
  point.direction_local = vec3_new(0, 0, 0);
  assert(vkr_type_validate(&vkr_scene_point_light_type, &point, error,
                           sizeof(error)));
  spot.direction_local = vec3_new(0, 0, 0);
  assert(!vkr_type_validate(&vkr_scene_point_light_type, &spot, error,
                            sizeof(error)));
  const uint32_t inner = vkr_type_find_property(&vkr_scene_point_light_type,
                                                string8_lit("inner_cone"));
  assert(
      vkr_type_property_state(&vkr_scene_point_light_type, &point, inner, NULL)
          .flags &
      VKR_PROPERTY_STATE_HIDDEN);

  SceneDirectionalLight sun = {.color = vec3_new(1, 1, 1),
                               .intensity = 3.0f,
                               .direction_local = vec3_new(0, -1, 0),
                               .temperature_kelvin = 5800.0f,
                               .enabled = true_v};
  assert(vkr_type_validate(&vkr_scene_directional_light_type, &sun, error,
                           sizeof(error)));
  sun.temperature_kelvin = 500.0f;
  assert(!vkr_type_validate(&vkr_scene_directional_light_type, &sun, error,
                            sizeof(error)));
  SceneRectangleLight rect = {.color = vec3_new(1, 1, 1),
                              .radiance = 1.0f,
                              .size = vec2_new(1.0f, 0.0f),
                              .enabled = true_v};
  assert(!vkr_type_validate(&vkr_scene_rectangle_light_type, &rect, error,
                            sizeof(error)));
  printf("  test_light_types PASSED\n");
}

/* Two layouts of one script-style type: fields moved, one dropped, one
 * added, one's kind and another's bounds changed. */
typedef struct MigrateBefore {
  float32_t speed;
  int32_t count;
  Vec3 tint;
  char label[8];
  float32_t gone;
} MigrateBefore;

typedef struct MigrateAfter {
  Vec3 tint;
  float32_t count;
  float32_t speed;
  char label[4];
  float32_t added;
} MigrateAfter;

static const VkrPropertyDesc s_before_properties[] = {
    {.name = "speed",
     .offset = offsetof(MigrateBefore, speed),
     .kind = VKR_PROPERTY_F32},
    {.name = "count",
     .offset = offsetof(MigrateBefore, count),
     .kind = VKR_PROPERTY_I32},
    {.name = "tint",
     .offset = offsetof(MigrateBefore, tint),
     .kind = VKR_PROPERTY_COLOR},
    {.name = "label",
     .offset = offsetof(MigrateBefore, label),
     .kind = VKR_PROPERTY_STRING,
     .capacity = 8u},
    {.name = "gone",
     .offset = offsetof(MigrateBefore, gone),
     .kind = VKR_PROPERTY_F32},
};

static const VkrPropertyDesc s_after_properties[] = {
    {.name = "tint",
     .offset = offsetof(MigrateAfter, tint),
     .kind = VKR_PROPERTY_VEC3},
    {.name = "count",
     .offset = offsetof(MigrateAfter, count),
     .kind = VKR_PROPERTY_F32},
    {.name = "speed",
     .offset = offsetof(MigrateAfter, speed),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 5.0f},
    {.name = "label",
     .offset = offsetof(MigrateAfter, label),
     .kind = VKR_PROPERTY_STRING,
     .capacity = 4u},
    {.name = "added",
     .offset = offsetof(MigrateAfter, added),
     .kind = VKR_PROPERTY_F32},
};

static void migrate_after_defaults(void *value) {
  ((MigrateAfter *)value)->added = 2.5f;
}

static const VkrTypeDesc s_migrate_before = {
    .name = "migrate_probe",
    .properties = s_before_properties,
    .property_count = ArrayCount(s_before_properties),
    .size = sizeof(MigrateBefore),
    .align = AlignOf(MigrateBefore),
};

static const VkrTypeDesc s_migrate_after = {
    .name = "migrate_probe",
    .flags = VKR_TYPE_FLAG_TOLERANT,
    .properties = s_after_properties,
    .property_count = ArrayCount(s_after_properties),
    .size = sizeof(MigrateAfter),
    .align = AlignOf(MigrateAfter),
    .defaults = migrate_after_defaults,
};

/* Values follow their fields by name into a changed layout, and a tolerant
 * reader skips members an older layout wrote. */
static void test_type_migrate(VkrAllocator *allocator) {
  const MigrateBefore before = {.speed = 9.0f,
                                .count = -3,
                                .tint = {.x = 0.25f, .y = 0.5f, .z = 1.0f},
                                .label = "abcdef",
                                .gone = 7.0f};
  MigrateAfter after;
  vkr_type_migrate(&s_migrate_before, &before, &s_migrate_after, &after);
  assert(after.tint.x == 0.25f && after.tint.y == 0.5f && after.tint.z == 1.0f);
  assert(after.count == -3.0f);
  assert(after.speed == 5.0f);
  assert(strcmp(after.label, "abc") == 0);
  assert(after.added == 2.5f);

  // A member the new layout dropped and one whose kind changed: the tolerant
  // type skips the first and keeps the default for the second.
  const char json[] =
      "{\"gone\":7,\"count\":[1,2],\"speed\":1.5,\"label\":\"hi\"}";
  MigrateAfter read;
  char error[128] = {0};
  vkr_type_defaults(&s_migrate_after, &read);
  read.count = 4.0f;
  assert(vkr_type_read_json_document(
      string8_create_from_cstr((const uint8_t *)json, strlen(json)),
      &s_migrate_after, &read, allocator, error, sizeof(error)));
  assert(read.speed == 1.5f && read.count == 4.0f && read.added == 2.5f);
  assert(strcmp(read.label, "hi") == 0);
  // Without the flag the same document fails on the dropped member.
  VkrTypeDesc strict = s_migrate_after;
  strict.flags = VKR_TYPE_FLAG_NONE;
  assert(!vkr_type_read_json_document(
      string8_create_from_cstr((const uint8_t *)json, strlen(json)), &strict,
      &read, allocator, error, sizeof(error)));
}

static bool8_t test_tags_parse(const char *text, SceneTags *out) {
  char error[128] = {0};
  const bool8_t ok = vkr_scene_tags_parse(
      string8_create_from_cstr((const uint8_t *)text, strlen(text)), out, error,
      sizeof(error));
  /* A refusal always says why. */
  assert(ok || error[0]);
  return ok;
}

/* Tags (ADR-084): typed text becomes one canonical form, over-limit or
   malformed text is refused instead of cut, and documents hold only the
   canonical form, so a filter can compare tags byte for byte. */
static void test_scene_tags(VkrAllocator *allocator) {
  printf("  Running test_scene_tags...\n");
  SceneTags tags;
  assert(test_tags_parse("Labs, #CHAIR  #labs\tchair_2 a-b", &tags));
  assert(strcmp(tags.text, "#labs #chair #chair_2 #a-b") == 0);
  assert(vkr_scene_tags_has(&tags, string8_lit("#chair")));
  assert(!vkr_scene_tags_has(&tags, string8_lit("#chai")));
  assert(!vkr_scene_tags_has(&tags, string8_lit("chair")));
  assert(test_tags_parse("  ", &tags) && tags.text[0] == '\0');

  /* A refusal leaves the previous value. */
  assert(test_tags_parse("#kept", &tags));
  static const char *const refused[] = {
      "#",                                  /* no name */
      "#lab#s",                             /* '#' inside */
      "#caf\xc3\xa9",                       /* outside a-z */
      "#a.b",                               /* punctuation */
      "#abcdefghijklmnopqrstuvwxyz0123456", /* 33 characters */
  };
  for (uint32_t i = 0; i < ArrayCount(refused); ++i) {
    assert(!test_tags_parse(refused[i], &tags));
    assert(strcmp(tags.text, "#kept") == 0);
  }
  assert(test_tags_parse("#abcdefghijklmnopqrstuvwxyz012345", &tags));

  /* Sixteen tags fit, repeats do not count, a seventeenth is refused. */
  char many[256] = {0};
  for (uint32_t i = 0; i < 16u; ++i) {
    snprintf(many + strlen(many), sizeof(many) - strlen(many), "#t%u ", i);
  }
  snprintf(many + strlen(many), sizeof(many) - strlen(many), "#t3 T7");
  assert(test_tags_parse(many, &tags));
  assert(strncmp(tags.text, "#t0 #t1 ", 8u) == 0);
  snprintf(many + strlen(many), sizeof(many) - strlen(many), " #t16");
  assert(!test_tags_parse(many, &tags));

  /* Sixteen tags of the longest length fit the stored capacity. */
  char longest[SCENE_TAGS_CAPACITY * 2u] = {0};
  for (uint32_t i = 0; i < 16u; ++i) {
    snprintf(longest + strlen(longest), sizeof(longest) - strlen(longest),
             "#%02uabcdefghijklmnopqrstuvwxyz0123 ", i);
  }
  assert(test_tags_parse(longest, &tags));
  assert(strlen(tags.text) == 16u * 34u - 1u);

  /* Documents hold canonical text only. */
  char error[128];
  SceneTags read = {0};
  assert(vkr_type_read_json_document(string8_lit("{\"tags\":\"#labs #chair\"}"),
                                     &vkr_scene_tags_type, &read, allocator,
                                     error, sizeof(error)));
  assert(strcmp(read.text, "#labs #chair") == 0);
  static const char *const noncanonical[] = {
      "{\"tags\":\"#Labs\"}",         /* uppercase */
      "{\"tags\":\"labs\"}",          /* no '#' */
      "{\"tags\":\"#labs  #chair\"}", /* two spaces */
      "{\"tags\":\"#labs #labs\"}",   /* repeat */
      "{\"tags\":\" #labs\"}",        /* leading space */
      "{\"tags\":\"#labs,#chair\"}",  /* comma */
  };
  for (uint32_t i = 0; i < ArrayCount(noncanonical); ++i) {
    error[0] = 0;
    assert(!vkr_type_read_json_document(
        string8_create_from_cstr((const uint8_t *)noncanonical[i],
                                 strlen(noncanonical[i])),
        &vkr_scene_tags_type, &read, allocator, error, sizeof(error)));
    assert(error[0] && strcmp(read.text, "#labs #chair") == 0);
  }
  printf("  test_scene_tags PASSED\n");
}

/* Brush faces, blockout shapes and terrains written before surface tags
   name retired dev palette materials; reading them gives the surface and
   mark that replaced each one, and new documents round-trip both. */
static void test_surface_documents(VkrAllocator *allocator) {
  char error[160];
  SceneBrushFace face;
  vkr_type_defaults(&vkr_scene_brush_face_type, &face);
  assert(vkr_type_read_json_document(
      string8_lit("{\"normal\":[0,1,0],\"distance\":0.5,\"material\":"
                  "\"assets/materials/dev/dev_orange.mt\"}"),
      &vkr_scene_brush_face_type, &face, allocator, error, sizeof(error)));
  assert(face.surface == VKR_SURFACE_NONE);
  assert(face.mark == VKR_SURFACE_MARK_ORANGE);
  assert(face.material[0] == '\0');
  assert(vkr_type_read_json_document(
      string8_lit("{\"normal\":[0,1,0],\"distance\":0.5,\"material\":"
                  "\"assets/materials/dev/dev_light.mt\"}"),
      &vkr_scene_brush_face_type, &face, allocator, error, sizeof(error)));
  assert(face.surface == VKR_SURFACE_EMISSIVE);
  assert(face.mark == VKR_SURFACE_MARK_NONE);
  assert(face.material[0] == '\0');

  /* Another material stays art-owned beside the face's surface. */
  assert(vkr_type_read_json_document(
      string8_lit("{\"normal\":[0,1,0],\"distance\":0.5,\"surface\":"
                  "\"brick\",\"mark\":\"hazard\",\"material\":"
                  "\"assets/materials/bistro/wall.mt\"}"),
      &vkr_scene_brush_face_type, &face, allocator, error, sizeof(error)));
  assert(face.surface == VKR_SURFACE_BRICK);
  assert(face.mark == VKR_SURFACE_MARK_HAZARD);
  assert(strcmp(face.material, "assets/materials/bistro/wall.mt") == 0);
  assert(!vkr_type_read_json_document(
      string8_lit("{\"normal\":[0,1,0],\"distance\":0.5,\"surface\":"
                  "\"marble\"}"),
      &vkr_scene_brush_face_type, &face, allocator, error, sizeof(error)));

  /* Shapes drop the material keys they had; surfaces replace them. */
  SceneBlockout shape;
  vkr_type_defaults(&vkr_scene_blockout_type, &shape);
  assert(vkr_type_read_json_document(
      string8_lit("{\"shape\":\"Corridor\",\"material\":"
                  "\"assets/materials/dev/dev_wall.mt\",\"floor_material\":"
                  "\"assets/materials/dev/dev_floor.mt\",\"surface\":"
                  "\"concrete\",\"floor_surface\":\"tile\",\"mark\":"
                  "\"blue\"}"),
      &vkr_scene_blockout_type, &shape, allocator, error, sizeof(error)));
  assert(shape.surface == VKR_SURFACE_CONCRETE);
  assert(shape.floor_surface == VKR_SURFACE_TILE);
  assert(shape.mark == VKR_SURFACE_MARK_BLUE);

  /* Terrain layers take the floor greybox look of their replacement. */
  SceneTerrain terrain;
  vkr_type_defaults(&vkr_scene_terrain_type, &terrain);
  assert(vkr_type_read_json_document(
      string8_lit("{\"layer0\":\"assets/materials/dev/dev_grid.mt\","
                  "\"layer1\":\"assets/materials/dev/dev_orange.mt\","
                  "\"layer2\":\"assets/materials/rock.mt\"}"),
      &vkr_scene_terrain_type, &terrain, allocator, error, sizeof(error)));
  assert(strcmp(terrain.layer0, "assets/materials/greybox/none_floor.mt") == 0);
  assert(strcmp(terrain.layer1,
                "assets/materials/greybox/mark_orange_floor.mt") == 0);
  assert(strcmp(terrain.layer2, "assets/materials/rock.mt") == 0);
  printf("  test_surface_documents PASSED\n");
}

bool32_t run_type_desc_tests(void) {
  printf("--- Starting Type Descriptor Tests ---\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  test_type_round_trip(&allocator);
  test_type_rejections(&allocator);
  test_type_property_access();
  test_graphics_preferences_type();
  test_light_types();
  test_type_migrate(&allocator);
  test_scene_tags(&allocator);
  test_surface_documents(&allocator);
  vkr_dmemory_destroy(&memory);
  printf("--- Type Descriptor Tests completed. ---\n");
  return true_v;
}
