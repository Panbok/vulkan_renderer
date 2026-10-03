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
  settings.temporal_upscaling = false_v;
  settings.dynamic_resolution = true_v;
  vkr_graphics_settings_type.normalize(&settings);
  assert(!settings.dynamic_resolution);

  const uint32_t dynamic = vkr_type_find_property(
      &vkr_graphics_settings_type, string8_lit("dynamic_resolution"));
  assert(dynamic != UINT32_MAX);
  const VkrPropertyState state = vkr_type_property_state(
      &vkr_graphics_settings_type, &settings, dynamic, NULL);
  assert(state.flags & VKR_PROPERTY_STATE_DISABLED);
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
  vkr_dmemory_destroy(&memory);
  printf("--- Type Descriptor Tests completed. ---\n");
  return true_v;
}
