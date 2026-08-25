#include "scene_loader_tests.h"

#include "containers/str.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/systems/vkr_scene_system.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct SceneLoaderTestContext {
  Arena *arena;
  VkrAllocator allocator;
  RendererFrontend renderer;
  VkrScene scene;
} SceneLoaderTestContext;

static bool8_t scene_loader_test_context_init(SceneLoaderTestContext *ctx) {
  if (!ctx) {
    return false_v;
  }

  MemZero(ctx, sizeof(*ctx));
  ctx->arena = arena_create(MB(4), MB(4));
  if (!ctx->arena) {
    return false_v;
  }

  ctx->allocator = (VkrAllocator){.ctx = ctx->arena};
  if (!vkr_allocator_arena(&ctx->allocator)) {
    arena_destroy(ctx->arena);
    ctx->arena = NULL;
    return false_v;
  }

  MemZero(&ctx->renderer, sizeof(ctx->renderer));
  ctx->renderer.arena = ctx->arena;
  ctx->renderer.allocator = ctx->allocator;
  ctx->renderer.scratch_arena = ctx->arena;
  ctx->renderer.scratch_allocator = ctx->allocator;

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!vkr_scene_init(&ctx->scene, &ctx->allocator, 1u, 32u, &scene_error)) {
    arena_destroy(ctx->arena);
    ctx->arena = NULL;
    return false_v;
  }

  return true_v;
}

static void scene_loader_test_context_shutdown(SceneLoaderTestContext *ctx) {
  if (!ctx) {
    return;
  }

  vkr_scene_shutdown(&ctx->scene, NULL);
  if (ctx->arena) {
    arena_destroy(ctx->arena);
    ctx->arena = NULL;
  }
}

static void test_scene_loader_missing_environment_succeeds(void) {
  printf("  Running test_scene_loader_missing_environment_succeeds...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit("{\"version\":2,\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.irradiance_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_missing_environment_succeeds PASSED\n");
}

static void test_scene_loader_invalid_environment_preserves_scene_load(void) {
  printf("  Running "
         "test_scene_loader_invalid_environment_preserves_scene_load...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit(
      "{\"version\":2,\"environment\":{\"enabled\":true,\"intensity\":2.5},"
      "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(fabsf(ctx.scene.environment.intensity - 2.5f) < 0.001f);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.irradiance_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_invalid_environment_preserves_scene_load "
         "PASSED\n");
}

static void test_scene_loader_disabled_environment_parses_controls(void) {
  printf(
      "  Running test_scene_loader_disabled_environment_parses_controls...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit(
      "{\"version\":2,\"environment\":{\"enabled\":false,\"intensity\":1.5,"
      "\"diffuse_intensity\":0.75,\"specular_intensity\":0.25},"
      "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(fabsf(ctx.scene.environment.intensity - 1.5f) < 0.001f);
  assert(fabsf(ctx.scene.environment.diffuse_intensity - 0.75f) < 0.001f);
  assert(fabsf(ctx.scene.environment.specular_intensity - 0.25f) < 0.001f);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);
  assert(ctx.scene.environment.source_cubemap.id == 0u);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_disabled_environment_parses_controls PASSED\n");
}

static void test_scene_loader_env_cubemap_load_failure_falls_back(void) {
  printf(
      "  Running test_scene_loader_env_cubemap_load_failure_falls_back...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit(
      "{\"version\":2,\"environment\":{\"enabled\":true,"
      "\"cubemap\":{\"base_path\":\"assets/textures/does_not_exist\","
      "\"extension\":\"jpg\"}},\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.irradiance_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_env_cubemap_load_failure_falls_back PASSED\n");
}

static void test_scene_loader_environment_sources_are_mutually_exclusive(void) {
  printf("  Running "
         "test_scene_loader_environment_sources_are_mutually_exclusive...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  String8 json =
      string8_lit("{\"version\":2,\"environment\":{\"enabled\":true,"
                  "\"equirect\":\"assets/textures/environment.hdr\","
                  "\"cubemap\":{\"base_path\":\"assets/textures/skybox\","
                  "\"extension\":\"jpg\"}},\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                  &ctx.allocator, &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_environment_sources_are_mutually_exclusive "
         "PASSED\n");
}

static void test_scene_loader_equirect_load_failure_falls_back(void) {
  printf("  Running test_scene_loader_equirect_load_failure_falls_back...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  String8 json =
      string8_lit("{\"version\":2,\"environment\":{\"enabled\":true,"
                  "\"equirect\":\"assets/textures/does_not_exist.hdr\"},"
                  "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                  &ctx.allocator, &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_equirect_load_failure_falls_back PASSED\n");
}

static void test_scene_loader_disabled_environment_ignores_sources(void) {
  printf("  Running "
         "test_scene_loader_disabled_environment_ignores_sources...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  String8 json =
      string8_lit("{\"version\":2,\"environment\":{\"enabled\":false,"
                  "\"equirect\":\"assets/textures/environment.hdr\","
                  "\"cubemap\":{\"base_path\":\"assets/textures/skybox\","
                  "\"extension\":\"jpg\"}},\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                  &ctx.allocator, &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_disabled_environment_ignores_sources PASSED\n");
}

static void test_scene_loader_missing_reflection_probes_succeeds(void) {
  printf("  Running test_scene_loader_missing_reflection_probes_succeeds...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit("{\"version\":2,"
                             "\"environment\":{\"enabled\":false},"
                             "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.reflection_probe_count == 0u);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_missing_reflection_probes_succeeds PASSED\n");
}

static void test_scene_loader_reflection_probes_parse_valid_block(void) {
  printf(
      "  Running test_scene_loader_reflection_probes_parse_valid_block...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit(
      "{\"version\":2,"
      "\"reflection_probes\":["
      "{\"enabled\":false,\"center\":[1,2,3],\"extents\":[4,5,6],"
      "\"blend_distance\":2.5,\"intensity\":1.5,"
      "\"diffuse_intensity\":0.5,\"specular_intensity\":0.25},"
      "{\"enabled\":false,\"center\":[-1,0,1],\"extents\":[2,2,2]}],"
      "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.reflection_probe_count == 2u);

  const VkrSceneReflectionProbe *probe0 = &ctx.scene.reflection_probes[0];
  assert(probe0->enabled == false_v);
  assert(fabsf(probe0->center.x - 1.0f) < 0.001f);
  assert(fabsf(probe0->center.y - 2.0f) < 0.001f);
  assert(fabsf(probe0->center.z - 3.0f) < 0.001f);
  assert(fabsf(probe0->extents.x - 4.0f) < 0.001f);
  assert(fabsf(probe0->extents.y - 5.0f) < 0.001f);
  assert(fabsf(probe0->extents.z - 6.0f) < 0.001f);
  assert(fabsf(probe0->blend_distance - 2.5f) < 0.001f);
  assert(fabsf(probe0->intensity - 1.5f) < 0.001f);
  assert(fabsf(probe0->diffuse_intensity - 0.5f) < 0.001f);
  assert(fabsf(probe0->specular_intensity - 0.25f) < 0.001f);
  assert(probe0->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE);

  const VkrSceneReflectionProbe *probe1 = &ctx.scene.reflection_probes[1];
  assert(probe1->enabled == false_v);
  assert(fabsf(probe1->blend_distance - 1.0f) < 0.001f);
  assert(fabsf(probe1->intensity - 1.0f) < 0.001f);
  assert(fabsf(probe1->diffuse_intensity - 1.0f) < 0.001f);
  assert(fabsf(probe1->specular_intensity - 1.0f) < 0.001f);
  assert(probe1->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_reflection_probes_parse_valid_block PASSED\n");
}

static void test_scene_loader_reflection_probe_invalid_entries_skipped(void) {
  printf("  Running "
         "test_scene_loader_reflection_probe_invalid_entries_skipped...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json =
      string8_lit("{\"version\":2,"
                  "\"reflection_probes\":["
                  "{\"enabled\":true,\"center\":[0,0,0]},"
                  "{\"enabled\":true,\"center\":[0,0,0],\"extents\":[1,-1,1]},"
                  "{\"enabled\":false,\"center\":[0,0,0],\"extents\":[1,1,1]}],"
                  "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.reflection_probe_count == 1u);
  assert(ctx.scene.reflection_probes[0].enabled == false_v);
  assert(ctx.scene.reflection_probes[0].bake_state ==
         VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf(
      "  test_scene_loader_reflection_probe_invalid_entries_skipped PASSED\n");
}

static void
test_scene_loader_reflection_probe_missing_cubemap_disables_probe(void) {
  printf(
      "  Running "
      "test_scene_loader_reflection_probe_missing_cubemap_disables_probe...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json =
      string8_lit("{\"version\":2,"
                  "\"reflection_probes\":["
                  "{\"enabled\":true,\"center\":[0,0,0],\"extents\":[2,2,2]}],"
                  "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.renderer, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.reflection_probe_count == 1u);
  assert(ctx.scene.reflection_probes[0].enabled == false_v);
  assert(ctx.scene.reflection_probes[0].source_cubemap.id == 0u);
  assert(ctx.scene.reflection_probes[0].bake_state ==
         VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_reflection_probe_missing_cubemap_disables_probe "
         "PASSED\n");
}

static void test_scene_loader_imports_gltf_punctual_lights(void) {
  printf("  Running test_scene_loader_imports_gltf_punctual_lights...\n");
  char path[1024];
  snprintf(path, sizeof(path),
           "%stests/fixtures/rendering/punctual_lights.gltf",
           PROJECT_SOURCE_DIR);
  VkrSceneGltfPunctualLightImport lights[4] = {0};
  uint32_t count = 0u;
  const Mat4 scene_world = mat4_translate(vec3_new(10.0f, 0.0f, 0.0f));
  assert(vkr_scene_loader_read_gltf_punctual_lights(
      string8_create_from_cstr((const uint8_t *)path, string_length(path)),
      scene_world, 7u, lights, ArrayCount(lights), &count));
  assert(count == 3u);

  assert(lights[0].type == VKR_SCENE_GLTF_LIGHT_POINT);
  assert(strcmp(lights[0].name, "gltf.7.PointNode") == 0);
  assert(fabsf(lights[0].position.x - 11.0f) < 0.0001f);
  assert(fabsf(lights[0].position.y - 2.0f) < 0.0001f);
  assert(fabsf(lights[0].position.z - 3.0f) < 0.0001f);
  assert(fabsf(lights[0].color.x - 0.5f) < 0.0001f);
  assert(fabsf(lights[0].intensity - 12.0f) < 0.0001f);
  assert(fabsf(lights[0].range - 8.0f) < 0.0001f);

  assert(lights[1].type == VKR_SCENE_GLTF_LIGHT_SPOT);
  assert(fabsf(lights[1].inner_cone_angle - 0.2f) < 0.0001f);
  assert(fabsf(lights[1].outer_cone_angle - 0.6f) < 0.0001f);
  assert(fabsf(lights[1].direction.x) < 0.0001f);
  assert(fabsf(lights[1].direction.y) < 0.0001f);
  assert(fabsf(lights[1].direction.z + 1.0f) < 0.0001f);

  assert(lights[2].type == VKR_SCENE_GLTF_LIGHT_DIRECTIONAL);
  assert(fabsf(lights[2].intensity - 3.0f) < 0.0001f);
  printf("  test_scene_loader_imports_gltf_punctual_lights PASSED\n");
}

static void test_scene_loader_async_light_source_contract(void) {
  printf("  Running test_scene_loader_async_light_source_contract...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  assert(vkr_dmemory_create(KB(64), MB(2), &ctx.renderer.scene_async_memory));
  ctx.renderer.scene_async_allocator =
      (VkrAllocator){.ctx = &ctx.renderer.scene_async_memory};
  vkr_dmemory_allocator_create(&ctx.renderer.scene_async_allocator);
  assert(vkr_mutex_create(&ctx.allocator, &ctx.renderer.scene_async_mutex));

  VkrResourceLoader loader = vkr_scene_loader_create();
  loader.resource_system = &ctx.renderer;
  void *payload = NULL;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;

  assert(loader.prepare_async(
      &loader, string8_lit("tests/fixtures/rendering/empty.scene.json"),
      &ctx.allocator, &payload, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(payload != NULL);
  loader.release_async_payload(&loader, payload);

  payload = NULL;
  error = VKR_RENDERER_ERROR_NONE;
  assert(!loader.prepare_async(
      &loader,
      string8_lit(
          "tests/fixtures/rendering/missing_gltf_light_source.scene.json"),
      &ctx.allocator, &payload, &error));
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);
  assert(payload == NULL);

  vkr_mutex_destroy(&ctx.allocator, &ctx.renderer.scene_async_mutex);
  vkr_dmemory_allocator_destroy(&ctx.renderer.scene_async_allocator);
  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_async_light_source_contract PASSED\n");
}

bool32_t run_scene_loader_tests(void) {
  printf("--- Starting Scene Loader Tests ---\n");

  test_scene_loader_missing_environment_succeeds();
  test_scene_loader_invalid_environment_preserves_scene_load();
  test_scene_loader_disabled_environment_parses_controls();
  test_scene_loader_env_cubemap_load_failure_falls_back();
  test_scene_loader_environment_sources_are_mutually_exclusive();
  test_scene_loader_equirect_load_failure_falls_back();
  test_scene_loader_disabled_environment_ignores_sources();
  test_scene_loader_missing_reflection_probes_succeeds();
  test_scene_loader_reflection_probes_parse_valid_block();
  test_scene_loader_reflection_probe_invalid_entries_skipped();
  test_scene_loader_reflection_probe_missing_cubemap_disables_probe();
  test_scene_loader_imports_gltf_punctual_lights();
  test_scene_loader_async_light_source_contract();

  printf("--- Scene Loader Tests Completed ---\n");
  return true;
}
