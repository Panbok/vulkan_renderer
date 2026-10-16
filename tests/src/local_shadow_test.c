#include "defines.h"

#include "math/mat.h"
#include "renderer/systems/vkr_local_shadow_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "vkr_frame_input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static VkrPointLight local_shadow_test_light(VkrPointLightKind kind,
                                             bool8_t casts_shadow,
                                             float32_t range) {
  static uint32_t render_id = 0u;
  return (VkrPointLight){
      .render_id = ++render_id,
      .position = vec3_zero(),
      .color = vec3_one(),
      .intensity = 1.0f,
      .constant = 1.0f,
      .linear = 0.0f,
      .quadratic = 0.0f,
      .range = range,
      .direction = vec3_new(0.0f, 0.0f, -1.0f),
      .inner_cone_angle = 0.2f,
      .outer_cone_angle = 0.6f,
      .kind = kind,
      .casts_shadow = casts_shadow,
  };
}

static void assert_near(float32_t actual, float32_t expected) {
  assert(fabsf(actual - expected) < 0.0001f);
}

/* Cache state lives in static storage: the cache and its scratch are a few
 * hundred kilobytes. */
static VkrLocalShadowCache s_test_cache;
static VkrLocalShadowCache s_test_cache_scratch;
static VkrLocalShadowPendingHistory s_test_pending;

/* Resolves the cache for one frame with the camera at the origin. A zero
 * token means no pool exists yet, so every layer clears. */
static void local_shadow_resolve(const VkrPointLight *lights,
                                 uint32_t light_count, uint32_t face_budget,
                                 uint32_t map_size,
                                 VkrRetainedLocalShadowToken token,
                                 VkrLocalShadowPassPayload *out) {
  const VkrLocalShadowCamera camera = {
      .view = mat4_identity(),
      .position = vec3_new(0.0f, 0.0f, 0.0f),
      .delta_seconds = 1.0f / 60.0f,
      .frame_index = 1u,
  };
  const VkrLocalShadowCacheInput input = {
      .lights = lights,
      .light_count = light_count,
      .camera = &camera,
      .token = token,
      .static_generation = 1u,
      .publication_generation = 1u,
      .face_budget = face_budget,
      .map_size = map_size,
      .fade_distance = VKR_LOCAL_SHADOW_FADE_DISTANCE_DEFAULT,
  };
  vkr_local_shadow_cache_resolve(&s_test_cache, &s_test_cache_scratch, &input,
                                 &s_test_pending, out);
}

/* A fresh cache, resolved once without a pool. */
static void local_shadow_prepare(const VkrPointLight *lights,
                                 uint32_t light_count, uint32_t face_budget,
                                 uint32_t map_size,
                                 VkrLocalShadowPassPayload *out) {
  MemZero(&s_test_cache, sizeof(s_test_cache));
  local_shadow_resolve(lights, light_count, face_budget, map_size,
                       (VkrRetainedLocalShadowToken){0}, out);
}

/* The camera every input below points at: at the origin, no projection, so
 * every face is visible, one frame at 60 Hz. */
static VkrLocalShadowCamera s_test_camera;

/* One frame's input with the camera at the origin, for tests that set more
 * fields than local_shadow_resolve takes. */
static VkrLocalShadowCacheInput
local_shadow_input(const VkrPointLight *lights, uint32_t light_count,
                   uint32_t face_budget, VkrRetainedLocalShadowToken token) {
  s_test_camera = (VkrLocalShadowCamera){
      .view = mat4_identity(),
      .position = vec3_zero(),
      .delta_seconds = 1.0f / 60.0f,
      .frame_index = 1u,
  };
  return (VkrLocalShadowCacheInput){
      .lights = lights,
      .light_count = light_count,
      .camera = &s_test_camera,
      .token = token,
      .static_generation = 1u,
      .dynamic_generation = 1u,
      .publication_generation = 1u,
      .face_budget = face_budget,
      .map_size = 1024u,
      .fade_distance = VKR_LOCAL_SHADOW_FADE_DISTANCE_DEFAULT,
  };
}

static void local_shadow_resolve_input(const VkrLocalShadowCacheInput *input,
                                       VkrLocalShadowPassPayload *out) {
  vkr_local_shadow_cache_resolve(&s_test_cache, &s_test_cache_scratch, input,
                                 &s_test_pending, out);
}

/* Views of light `index` whose square lies in atlas layer `layer`. */
static uint32_t
local_shadow_views_in_layer(const VkrLocalShadowPassPayload *out,
                            uint32_t index, uint32_t face_count,
                            uint32_t layer) {
  const uint32_t first = out->light_first_view[index];
  assert(first != 0u);
  uint32_t count = 0u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    if (out->views[first - 1u + face].atlas_rect.w == (float32_t)layer)
      count++;
  }
  return count;
}

static Vec3 local_shadow_project(const VkrLocalShadowView *view, Vec3 point) {
  const Vec4 clip = mat4_mul_vec4(view->light_view_projection,
                                  vec4_new(point.x, point.y, point.z, 1.0f));
  assert(fabsf(clip.w) > 0.000001f);
  return vec3_new(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f,
                  clip.z / clip.w);
}

static bool32_t test_local_shadow_cardinal_centers(void) {
  printf("  Running test_local_shadow_cardinal_centers...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(&light, 1u, 6u, 1024u, &payload);
  assert(payload.view_count == 6u && payload.light_first_view[0] == 1u);

  const Vec3 directions[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
  };
  const float32_t near_clip = payload.views[0].light_position_near.w;
  for (uint32_t face = 0u; face < 6u; ++face) {
    const VkrLocalShadowView *view = &payload.views[face];
    const Vec3 near_point = vec3_scale(directions[face], near_clip);
    const Vec3 far_point = vec3_scale(directions[face], light.range);
    const Vec3 near_uvz = local_shadow_project(view, near_point);
    const Vec3 far_uvz = local_shadow_project(view, far_point);
    assert_near(near_uvz.x, 0.5f);
    assert_near(near_uvz.y, 0.5f);
    assert_near(near_uvz.z, 0.0f);
    assert_near(far_uvz.x, 0.5f);
    assert_near(far_uvz.y, 0.5f);
    assert_near(far_uvz.z, 1.0f);
  }
  printf("  test_local_shadow_cardinal_centers PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_edge_uv_orientation(void) {
  printf("  Running test_local_shadow_edge_uv_orientation...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(&light, 1u, 6u, 1024u, &payload);
  /* Literal rays in front of each face with known right/up components. */
  const float32_t radius = 1.0f;
  const float32_t tangent = 0.5f;
  const Vec3 edge_points[6] = {
      {radius, tangent, 0},  {-radius, -tangent, 0}, {tangent, radius, 0},
      {tangent, -radius, 0}, {tangent, 0, radius},   {tangent, 0, -radius},
  };
  const float32_t expected_u[6] = {0.5f, 0.5f, 0.75f, 0.25f, 0.25f, 0.75f};
  const float32_t expected_v[6] = {0.25f, 0.75f, 0.5f, 0.5f, 0.5f, 0.5f};
  for (uint32_t face = 0u; face < 6u; ++face) {
    const Vec3 uvz =
        local_shadow_project(&payload.views[face], edge_points[face]);
    assert_near(uvz.x, expected_u[face]);
    assert_near(uvz.y, expected_v[face]);
  }
  printf("  test_local_shadow_edge_uv_orientation PASSED\n");
  return true_v;
}

/* The face budget bounds the faces drawn per frame, in complete lights; a
 * light whose faces are not all drawn yet stays unshadowed and fills on a
 * later frame while committed faces are reused. */
static bool32_t test_local_shadow_cache_fills_within_budget(void) {
  printf("  Running test_local_shadow_cache_fills_within_budget...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_SPOT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(lights, 3u, 7u, 1024u, &payload);
  assert(payload.render_count == 7u && payload.view_count == 7u);
  assert(payload.atlas_clear_mask == 1u);
  assert(payload.light_first_view[0] != 0u &&
         payload.light_first_view[1] != 0u);
  assert(payload.light_first_view[2] == 0u);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 1u);

  const VkrRetainedLocalShadowToken token = {
      .resource_generation = 1u,
      .atlas_layer_count = payload.atlas_layer_count,
      .valid_layer_mask = 1u,
  };
  local_shadow_resolve(lights, 3u, 7u, 1024u, token, &payload);
  assert(payload.atlas_clear_mask == 0u);
  assert(payload.render_count == 6u && payload.view_count == 13u);
  for (uint32_t i = 0u; i < 3u; ++i)
    assert(payload.light_first_view[i] != 0u);
  for (uint32_t slot = 0u; slot < payload.render_count; ++slot)
    assert(payload.render_views[slot] - (payload.light_first_view[2] - 1u) <
           6u);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 2u);

  local_shadow_resolve(lights, 3u, 7u, 1024u, token, &payload);
  assert(payload.render_count == 0u && payload.view_count == 13u);
  printf("  test_local_shadow_cache_fills_within_budget PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_rejects_disabled_and_invalid(void) {
  printf("  Running test_local_shadow_rejects_disabled_and_invalid...\n");
  VkrPointLight lights[4] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, false_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 0.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, NAN),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  lights[3].position.x = NAN;
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(lights, 4u, 6u, 1024u, &payload);
  assert(payload.view_count == 0u);
  assert(payload.light_first_view[0] == 0u &&
         payload.light_first_view[3] == 0u);
  printf("  test_local_shadow_rejects_disabled_and_invalid PASSED\n");
  return true_v;
}

/* A face's side follows its light's range, never the camera, and the map size
 * caps it. */
static bool32_t test_local_shadow_map_size_and_zero_config(void) {
  printf("  Running test_local_shadow_map_size_and_zero_config...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  const uint32_t sizes[] = {128u, 256u, 1024u};
  const uint32_t expected[] = {128u, 256u, 256u};
  for (uint32_t i = 0u; i < ArrayCount(sizes); ++i) {
    VkrLocalShadowPassPayload payload = {0};
    local_shadow_prepare(&light, 1u, 6u, sizes[i], &payload);
    assert(payload.view_count == 6u);
    assert_near(payload.views[0].projection_params.y,
                1.0f / (float32_t)expected[i]);
    assert_near(payload.views[0].atlas_rect.z,
                (float32_t)expected[i] /
                    (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE);
  }
  assert(vkr_local_shadow_face_size_for_range(7.5f, 1024u) == 512u);
  assert(vkr_local_shadow_face_size_for_range(15.0f, 1024u) == 1024u);
  assert(vkr_local_shadow_face_size_for_range(0.5f, 1024u) == 128u);
  /* Faces are atlas squares, so sizes must be powers of two in range. */
  const uint32_t invalid_sizes[] = {1u, 64u, 300u, 2048u};
  for (uint32_t i = 0u; i < ArrayCount(invalid_sizes); ++i) {
    VkrLocalShadowPassPayload payload = {0};
    local_shadow_prepare(&light, 1u, 6u, invalid_sizes[i], &payload);
    assert(payload.view_count == 0u);
  }
  VkrLocalShadowPassPayload zero_budget = {0};
  local_shadow_prepare(&light, 1u, 0u, 1024u, &zero_budget);
  assert(zero_budget.view_count == 0u);
  VkrLocalShadowPassPayload zero_map = {0};
  local_shadow_prepare(&light, 1u, 6u, 0u, &zero_map);
  assert(zero_map.view_count == 0u);
  printf("  test_local_shadow_map_size_and_zero_config PASSED\n");
  return true_v;
}

/* Three 16 m point lights take 18 squares of 1024, more than the 16 one atlas
 * layer holds, so the atlas takes a second layer; every face must land in a
 * disjoint, aligned square of its layer. */
static bool32_t test_local_shadow_atlas_faces_are_disjoint(void) {
  printf("  Running test_local_shadow_atlas_faces_are_disjoint...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 16.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 16.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 16.0f),
  };
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(lights, 3u, 18u, 1024u, &payload);
  assert(payload.view_count == 18u && payload.atlas_layer_count == 2u);

  const float32_t atlas = (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
  uint64_t texels = 0u;
  for (uint32_t i = 0u; i < payload.view_count; ++i) {
    const Vec4 a = payload.views[i].atlas_rect;
    const float32_t size = a.z * atlas;
    assert(size == 1024.0f);
    assert(fmodf(a.x * atlas, size) == 0.0f &&
           fmodf(a.y * atlas, size) == 0.0f);
    assert(a.x >= 0.0f && a.y >= 0.0f && a.x + a.z <= 1.0f &&
           a.y + a.z <= 1.0f && (a.w == 0.0f || a.w == 1.0f));
    assert_near(payload.views[i].projection_params.y, 1.0f / size);
    texels += (uint64_t)size * (uint64_t)size;
    for (uint32_t j = 0u; j < i; ++j) {
      const Vec4 b = payload.views[j].atlas_rect;
      const bool8_t apart = a.w != b.w || a.x + a.z <= b.x ||
                            b.x + b.z <= a.x || a.y + a.z <= b.y ||
                            b.y + b.z <= a.y;
      assert(apart);
    }
  }
  assert(texels == 18u * 1024u * 1024u);
  printf("  test_local_shadow_atlas_faces_are_disjoint PASSED\n");
  return true_v;
}

/* Another atlas layer count is another atlas image that clears every layer,
 * and the face budget then redraws a few lights a frame while the others
 * hide. The dynamic casters coming and going add and drop the band's layer,
 * so the atlas keeps the live image's layers instead: when the band leaves
 * and returns, nothing clears, nothing redraws and every light keeps its
 * shadow. */
static bool32_t test_local_shadow_layer_count_survives_band_flip(void) {
  printf("  Running test_local_shadow_layer_count_survives_band_flip...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  /* One static layer and the band. */
  const VkrRetainedLocalShadowToken token = {
      .resource_generation = 1u,
      .atlas_layer_count = 2u,
      .valid_layer_mask = UINT32_C(0x3),
  };
  VkrLocalShadowCacheInput input = local_shadow_input(lights, 3u, 6u, token);
  input.dynamic_casters = true_v;
  VkrLocalShadowPassPayload payload = {0};
  MemZero(&s_test_cache, sizeof(s_test_cache));
  /* Six faces a frame fill the three lights in three frames. */
  for (uint32_t frame = 0u; frame < 3u; ++frame) {
    local_shadow_resolve_input(&input, &payload);
    assert(payload.atlas_layer_count == 2u && payload.render_count == 6u);
    vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 1u + frame);
  }

  for (uint32_t frame = 0u; frame < 4u; ++frame) {
    input.dynamic_casters = (frame & 1u) != 0u;
    local_shadow_resolve_input(&input, &payload);
    assert(payload.atlas_layer_count == 2u);
    assert(payload.dynamic_layer_count == (input.dynamic_casters ? 1u : 0u));
    assert(payload.atlas_clear_mask == 0u && payload.render_count == 0u);
    for (uint32_t i = 0u; i < 3u; ++i)
      assert(payload.light_first_view[i] != 0u);
    vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 4u + frame);
  }
  printf("  test_local_shadow_layer_count_survives_band_flip PASSED\n");
  return true_v;
}

/* A spot that turns has invalid content; while a more important light takes
 * the only face of the budget, it shows the content its square holds with
 * the committed projection and fades out slowly, instead of hiding at once
 * and fading in from nothing once it draws. */
static bool32_t test_local_shadow_turned_spot_keeps_committed_content(void) {
  printf(
      "  Running test_local_shadow_turned_spot_keeps_committed_content...\n");
  VkrPointLight lights[2] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_SPOT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_SPOT, true_v, 4.0f),
  };
  const VkrRetainedLocalShadowToken token = {
      .resource_generation = 1u,
      .atlas_layer_count = 1u,
      .valid_layer_mask = UINT32_C(1),
  };
  VkrLocalShadowPassPayload payload = {0};
  local_shadow_prepare(lights, 1u, 1u, 1024u, &payload);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 1u);
  local_shadow_resolve(lights, 1u, 1u, 1024u, token, &payload);
  assert(payload.render_count == 0u);
  const VkrLocalShadowView committed = payload.views[0];
  assert(committed.shadow_params.x == 1.0f);

  /* The spot turns as a brighter newcomer arrives. */
  lights[0].direction = vec3_new(1.0f, 0.0f, -1.0f);
  lights[1].intensity = 10.0f;
  local_shadow_resolve(lights, 2u, 1u, 1024u, token, &payload);
  assert(payload.render_count == 1u);
  assert(payload.render_views[0] == payload.light_first_view[1] - 1u);
  assert(payload.light_first_view[0] != 0u);
  const VkrLocalShadowView *shown =
      &payload.views[payload.light_first_view[0] - 1u];
  assert(MemCompare(&shown->light_view_projection,
                    &committed.light_view_projection,
                    sizeof(committed.light_view_projection)) == 0);
  assert(MemCompare(&shown->atlas_rect, &committed.atlas_rect,
                    sizeof(committed.atlas_rect)) == 0);
  assert(shown->light_position_near.x == lights[0].position.x &&
         shown->light_position_near.y == lights[0].position.y &&
         shown->light_position_near.z == lights[0].position.z);
  assert_near(shown->shadow_params.x, 1.0f - 1.0f / 60.0f);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 2u);

  /* With the slot free it draws its new projection and fades back in. */
  local_shadow_resolve(lights, 2u, 1u, 1024u, token, &payload);
  assert(payload.render_count == 1u);
  assert(payload.render_views[0] == payload.light_first_view[0] - 1u);
  shown = &payload.views[payload.light_first_view[0] - 1u];
  assert(MemCompare(&shown->light_view_projection,
                    &committed.light_view_projection,
                    sizeof(committed.light_view_projection)) != 0);
  assert(shown->shadow_params.x == 1.0f);
  printf("  test_local_shadow_turned_spot_keeps_committed_content PASSED\n");
  return true_v;
}

/* A light that moves, such as a lamp riding a mover, has no static square to
 * keep: its faces draw every caster in one pass, with no copy and no
 * dynamic square. */
static bool32_t test_local_shadow_moving_light_draws_one_pass(void) {
  printf("  Running test_local_shadow_moving_light_draws_one_pass...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  const VkrRetainedLocalShadowToken token = {
      .resource_generation = 1u,
      .atlas_layer_count = 2u,
      .valid_layer_mask = UINT32_C(0x3),
  };
  const uint8_t reached[1] = {0x1u};
  VkrLocalShadowCacheInput input = local_shadow_input(&light, 1u, 6u, token);
  input.dynamic_casters = true_v;
  input.dynamic_faces = reached;
  VkrLocalShadowPassPayload payload = {0};
  MemZero(&s_test_cache, sizeof(s_test_cache));
  local_shadow_resolve_input(&input, &payload);
  assert(payload.render_count == 7u && payload.dynamic_render_count == 1u);
  assert(payload.static_render_mask == 0x3Fu);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 1u);

  light.position.x += 0.25f;
  local_shadow_resolve_input(&input, &payload);
  assert(payload.render_count == 6u && payload.dynamic_render_count == 0u);
  assert(payload.companion_view_count == 0u);
  assert(payload.static_render_mask == 0u);
  printf("  test_local_shadow_moving_light_draws_one_pass PASSED\n");
  return true_v;
}

/* Past the dynamic face cap, a face that showed a dynamic square keeps it
 * until a competitor's light is VKR_LOCAL_SHADOW_DYNAMIC_INCUMBENT_BONUS
 * more important: a light that becomes a little brighter does not take two
 * faces from another light, whose moving casters' shadows would vanish. */
static bool32_t test_local_shadow_dynamic_cap_keeps_incumbents(void) {
  printf("  Running test_local_shadow_dynamic_cap_keeps_incumbents...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  lights[2].intensity = 0.5f;
  const VkrRetainedLocalShadowToken token = {
      .resource_generation = 1u,
      .atlas_layer_count = 2u,
      .valid_layer_mask = UINT32_C(0x3),
  };
  /* Every face of every light wants a dynamic square: 18 past the cap. */
  const uint8_t reached[3] = {0x3Fu, 0x3Fu, 0x3Fu};
  VkrLocalShadowCacheInput input = local_shadow_input(lights, 3u, 18u, token);
  input.dynamic_casters = true_v;
  input.dynamic_faces = reached;
  VkrLocalShadowPassPayload payload = {0};
  MemZero(&s_test_cache, sizeof(s_test_cache));
  local_shadow_resolve_input(&input, &payload);
  assert(payload.dynamic_faces_wanted == 18u);
  assert(payload.dynamic_render_count ==
         VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX);
  const uint32_t expected[3] = {6u, 6u, 4u};
  for (uint32_t i = 0u; i < 3u; ++i)
    assert(local_shadow_views_in_layer(&payload, i, 6u, 1u) == expected[i]);
  vkr_local_shadow_cache_commit(&s_test_cache, &s_test_pending, 1u);

  /* The third light becomes 1.3 times as important as the others: its two
     faces without a square still wait, and the idle squares are reused. */
  lights[2].intensity = 1.3f;
  local_shadow_resolve_input(&input, &payload);
  assert(payload.render_count == 0u);
  assert(payload.dynamic_reused_count ==
         VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX);
  for (uint32_t i = 0u; i < 3u; ++i)
    assert(local_shadow_views_in_layer(&payload, i, 6u, 1u) == expected[i]);
  printf("  test_local_shadow_dynamic_cap_keeps_incumbents PASSED\n");
  return true_v;
}

bool32_t run_local_shadow_tests(void) {
  printf("--- Running Local Shadow tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_local_shadow_cardinal_centers();
  passed &= test_local_shadow_edge_uv_orientation();
  passed &= test_local_shadow_cache_fills_within_budget();
  passed &= test_local_shadow_rejects_disabled_and_invalid();
  passed &= test_local_shadow_map_size_and_zero_config();
  passed &= test_local_shadow_atlas_faces_are_disjoint();
  passed &= test_local_shadow_layer_count_survives_band_flip();
  passed &= test_local_shadow_turned_spot_keeps_committed_content();
  passed &= test_local_shadow_moving_light_draws_one_pass();
  passed &= test_local_shadow_dynamic_cap_keeps_incumbents();
  return passed;
}
