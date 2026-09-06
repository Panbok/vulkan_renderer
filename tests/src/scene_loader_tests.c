#include "scene_loader_tests.h"

#include "containers/str.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_system.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct SceneLoaderTestContext {
  Arena *arena;
  VkrAllocator allocator;
  VkrRenderAssets assets;
  VkrScene scene;
} SceneLoaderTestContext;

vkr_internal bool8_t
scene_loader_test_context_init(SceneLoaderTestContext *ctx) {
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

  MemZero(&ctx->assets, sizeof(ctx->assets));
  ctx->assets.arena = ctx->arena;
  ctx->assets.allocator = ctx->allocator;
  ctx->assets.scratch_arena = ctx->arena;
  ctx->assets.scratch_allocator = ctx->allocator;

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!vkr_scene_init(&ctx->scene, &ctx->allocator, 1u, 32u, &scene_error)) {
    arena_destroy(ctx->arena);
    ctx->arena = NULL;
    return false_v;
  }

  return true_v;
}

vkr_internal void
scene_loader_test_context_shutdown(SceneLoaderTestContext *ctx) {
  if (!ctx) {
    return;
  }

  vkr_scene_shutdown(&ctx->scene, NULL);
  if (ctx->arena) {
    arena_destroy(ctx->arena);
    ctx->arena = NULL;
  }
}

vkr_internal void test_scene_loader_missing_environment_succeeds(void) {
  printf("  Running test_scene_loader_missing_environment_succeeds...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit("{\"version\":2,\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_missing_environment_succeeds PASSED\n");
}

vkr_internal void
test_scene_loader_invalid_environment_preserves_scene_load(void) {
  printf("  Running "
         "test_scene_loader_invalid_environment_preserves_scene_load...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit(
      "{\"version\":2,\"environment\":{\"enabled\":true,\"intensity\":2.5},"
      "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(fabsf(ctx.scene.environment.intensity - 2.5f) < 0.001f);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_invalid_environment_preserves_scene_load "
         "PASSED\n");
}

vkr_internal void test_scene_loader_disabled_environment_parses_controls(void) {
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
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
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

vkr_internal void test_scene_loader_env_cubemap_load_failure_falls_back(void) {
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
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_cubemap.id == 0u);
  assert(ctx.scene.environment.prefilter_cubemap.id == 0u);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_env_cubemap_load_failure_falls_back PASSED\n");
}

vkr_internal void
test_scene_loader_environment_sources_are_mutually_exclusive(void) {
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
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json, &ctx.allocator,
                                  &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_environment_sources_are_mutually_exclusive "
         "PASSED\n");
}

vkr_internal void test_scene_loader_equirect_load_failure_falls_back(void) {
  printf("  Running test_scene_loader_equirect_load_failure_falls_back...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  String8 json =
      string8_lit("{\"version\":2,\"environment\":{\"enabled\":true,"
                  "\"equirect\":\"assets/textures/does_not_exist.hdr\"},"
                  "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json, &ctx.allocator,
                                  &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_equirect_load_failure_falls_back PASSED\n");
}

vkr_internal void test_scene_loader_disabled_environment_ignores_sources(void) {
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
  assert(vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json, &ctx.allocator,
                                  &result, &error));
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(ctx.scene.environment.enabled == false_v);
  assert(ctx.scene.environment.source_kind == VKR_SCENE_ENV_SOURCE_NONE);
  assert(ctx.scene.environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_NONE);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_disabled_environment_ignores_sources PASSED\n");
}

vkr_internal void test_scene_loader_missing_reflection_probes_succeeds(void) {
  printf("  Running test_scene_loader_missing_reflection_probes_succeeds...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);

  String8 json = string8_lit("{\"version\":2,"
                             "\"environment\":{\"enabled\":false},"
                             "\"entities\":[]}");
  VkrSceneLoadResult result = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
                                        &ctx.allocator, &result, &error);
  assert(ok == true_v);
  assert(error == VKR_SCENE_ERROR_NONE);
  assert(result.entity_count == 0u);
  assert(ctx.scene.reflection_probe_count == 0u);

  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_missing_reflection_probes_succeeds PASSED\n");
}

vkr_internal void test_scene_loader_reflection_probes_parse_valid_block(void) {
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
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
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

vkr_internal void
test_scene_loader_reflection_probe_invalid_entries_skipped(void) {
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
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
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

vkr_internal void
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
  bool8_t ok = vkr_scene_load_from_json(&ctx.scene, &ctx.assets, json,
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

vkr_internal void test_scene_loader_imports_gltf_punctual_lights(void) {
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

vkr_internal void test_scene_loader_async_light_source_contract(void) {
  printf("  Running test_scene_loader_async_light_source_contract...\n");

  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx) == true_v);
  assert(vkr_dmemory_create(KB(64), MB(2), &ctx.assets.scene_async_memory));
  ctx.assets.scene_async_allocator =
      (VkrAllocator){.ctx = &ctx.assets.scene_async_memory};
  vkr_dmemory_allocator_create(&ctx.assets.scene_async_allocator);
  assert(vkr_mutex_create(&ctx.allocator, &ctx.assets.scene_async_mutex));
  assert(vkr_resource_system_init(&ctx.allocator, NULL, NULL));

  VkrResourceLoader loader = vkr_scene_loader_create();
  loader.resource_system = &ctx.assets;
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

  const char *valid_path =
      "tests/fixtures/rendering/gltf_light_range_override_valid.scene.json";
  payload = NULL;
  error = VKR_RENDERER_ERROR_NONE;
  assert(
      loader.prepare_async(&loader,
                           string8_create_from_cstr((const uint8_t *)valid_path,
                                                    string_length(valid_path)),
                           &ctx.allocator, &payload, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(payload != NULL);
  loader.release_async_payload(&loader, payload);

  const char *invalid_paths[] = {
      "tests/fixtures/rendering/"
      "gltf_light_range_override_ambiguous.scene.json",
      "tests/fixtures/rendering/gltf_light_range_override_duplicate.scene.json",
      "tests/fixtures/rendering/gltf_light_range_override_unknown.scene.json",
      "tests/fixtures/rendering/"
      "gltf_light_range_override_directional.scene.json",
      "tests/fixtures/rendering/"
      "gltf_light_range_override_nonpositive.scene.json",
      "tests/fixtures/rendering/gltf_light_range_override_nonfinite.scene.json",
      "tests/fixtures/rendering/gltf_light_range_override_malformed.scene.json",
      "tests/fixtures/rendering/"
      "gltf_light_range_override_missing_source.scene.json",
  };
  for (uint32_t i = 0u; i < ArrayCount(invalid_paths); ++i) {
    payload = NULL;
    error = VKR_RENDERER_ERROR_NONE;
    const String8 path = string8_create_from_cstr(
        (const uint8_t *)invalid_paths[i], string_length(invalid_paths[i]));
    assert(
        !loader.prepare_async(&loader, path, &ctx.allocator, &payload, &error));
    assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);
    assert(payload == NULL);
  }

  vkr_resource_system_shutdown();
  vkr_mutex_destroy(&ctx.allocator, &ctx.assets.scene_async_mutex);
  vkr_dmemory_allocator_destroy(&ctx.assets.scene_async_allocator);
  scene_loader_test_context_shutdown(&ctx);
  printf("  test_scene_loader_async_light_source_contract PASSED\n");
}

static void test_scene_source_nodes_preserve_hierarchy_and_exact_matrix(void) {
  SceneLoaderTestContext ctx;
  assert(scene_loader_test_context_init(&ctx));
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  VkrEntityId wrapper = vkr_scene_create_entity(&ctx.scene, &error);
  assert(vkr_scene_set_transform(&ctx.scene, wrapper, vec3_new(100, 0, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrMeshSourceNode nodes[3] = {
      {.name = string8_lit("group"),
       .local = mat4_identity(),
       .parent = UINT32_MAX,
       .mesh = UINT32_MAX,
       .mesh_variant = UINT32_MAX,
       .camera = UINT32_MAX,
       .skin = UINT32_MAX,
       .light = UINT32_MAX,
       .in_scene = true_v},
      {.name = string8_lit("shared A"),
       .local = mat4_identity(),
       .parent = 0,
       .mesh = 0,
       .mesh_variant = 0,
       .camera = UINT32_MAX,
       .skin = UINT32_MAX,
       .light = UINT32_MAX,
       .in_scene = true_v},
      {.name = string8_lit("shared B"),
       .local = mat4_identity(),
       .parent = 0,
       .mesh = 0,
       .mesh_variant = 0,
       .camera = UINT32_MAX,
       .skin = UINT32_MAX,
       .light = UINT32_MAX,
       .in_scene = true_v},
  };
  nodes[0].local.elements[12] = 10.0f;
  nodes[1].local.elements[12] = 2.0f;
  nodes[2].local.elements[12] = -2.0f;
  const VkrMeshSource source = {.nodes = {.data = nodes, .length = 3},
                                .fingerprint = 777u};
  VkrEntityId ids[3];
  assert(vkr_scene_instantiate_source_nodes(&ctx.scene, &source, wrapper, 4,
                                            ids, &error));
  vkr_scene_update(&ctx.scene, 0.0);
  assert(vkr_scene_get_transform(&ctx.scene, ids[1])->world.elements[12] ==
         112.0f);
  assert(vkr_scene_get_transform(&ctx.scene, ids[2])->world.elements[12] ==
         108.0f);
  const SceneSourceIdentity *identity = vkr_entity_get_component(
      ctx.scene.world, ids[2], ctx.scene.comp_source_identity);
  assert(identity && identity->scene_entity_index == 4 &&
         identity->gltf_node_index == 2);
  assert(identity->gltf_mesh_index == 0 &&
         identity->source_fingerprint == 777u);
  vkr_scene_set_position(&ctx.scene, ids[1], vec3_new(5, 0, 0));
  vkr_scene_update(&ctx.scene, 0.0);
  assert(vkr_scene_get_transform(&ctx.scene, ids[1])->world.elements[12] ==
         115.0f);
  assert(vkr_scene_get_transform(&ctx.scene, ids[2])->world.elements[12] ==
         108.0f);
  Mat4 shear = mat4_identity();
  shear.elements[4] = 0.25f;
  assert(vkr_scene_set_local_matrix(&ctx.scene, ids[1], shear));
  vkr_scene_update(&ctx.scene, 0.0);
  SceneTransform *transform = vkr_scene_get_transform(&ctx.scene, ids[1]);
  assert(transform->matrix_authored && !transform->trs_editable);
  assert(transform->parent.u64 == ids[0].u64);
  assert(MemCompare(&transform->local, &shear, sizeof(shear)) == 0);
  vkr_scene_set_scale(&ctx.scene, ids[1], vec3_new(2, 2, 2));
  vkr_scene_update(&ctx.scene, 0.0);
  assert(MemCompare(&transform->local, &shear, sizeof(shear)) == 0);
  uint64_t revision = ctx.scene.structure_revision;
  assert(vkr_scene_set_name(&ctx.scene, ids[2], string8_lit("renamed")));
  assert(ctx.scene.structure_revision > revision);

  // Original glTF array order need not put a parent before its children.
  VkrMeshSourceNode reordered[3] = {nodes[1], nodes[2], nodes[0]};
  reordered[0].parent = reordered[1].parent = 2u;
  VkrMeshSource late_parent_source = source;
  late_parent_source.nodes.data = reordered;
  VkrEntityId reordered_ids[3];
  assert(vkr_scene_instantiate_source_nodes(
      &ctx.scene, &late_parent_source, wrapper, 4u, reordered_ids, &error));
  vkr_scene_update(&ctx.scene, 0.0);
  assert(vkr_scene_get_transform(&ctx.scene, reordered_ids[0])->parent.u64 ==
         reordered_ids[2].u64);
  assert(vkr_scene_get_transform(&ctx.scene, reordered_ids[0])
             ->world.elements[12] == 112.0f);
  assert(vkr_scene_get_transform(&ctx.scene, reordered_ids[1])
             ->world.elements[12] == 108.0f);
  assert(ctx.scene.topo_count == 7u);

  const SceneSourceIdentity prior_wrapper_identity =
      *(const SceneSourceIdentity *)vkr_entity_get_component(
          ctx.scene.world, wrapper, ctx.scene.comp_source_identity);
  reordered[1].local.elements[0] = NAN;
  VkrEntityId failed_ids[3];
  error = VKR_SCENE_ERROR_NONE;
  assert(!vkr_scene_instantiate_source_nodes(&ctx.scene, &late_parent_source,
                                             wrapper, 4u, failed_ids, &error));
  for (uint32_t i = 0; i < 3u; ++i)
    assert(failed_ids[i].u64 == VKR_ENTITY_ID_INVALID.u64);
  const SceneSourceIdentity *after_failure = vkr_entity_get_component(
      ctx.scene.world, wrapper, ctx.scene.comp_source_identity);
  assert(after_failure->source_fingerprint ==
         prior_wrapper_identity.source_fingerprint);
  vkr_scene_update(&ctx.scene, 0.0);
  assert(ctx.scene.topo_count == 7u);
  scene_loader_test_context_shutdown(&ctx);
}

static void test_scene_source_fingerprint_detects_entity_reordering(void) {
  SceneLoaderTestContext a, b;
  assert(scene_loader_test_context_init(&a));
  assert(scene_loader_test_context_init(&b));
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_load_from_json(
      &a.scene, &a.assets,
      string8_lit(
          "{\"version\":2,\"entities\":[{\"name\":\"A\"},{\"name\":\"B\"}]}"),
      &a.allocator, NULL, &error));
  assert(vkr_scene_load_from_json(
      &b.scene, &b.assets,
      string8_lit(
          "{\"version\":2,\"entities\":[{\"name\":\"B\"},{\"name\":\"A\"}]}"),
      &b.allocator, NULL, &error));
  vkr_scene_update(&a.scene, 0.0);
  vkr_scene_update(&b.scene, 0.0);
  assert(a.scene.topo_count == 2u && b.scene.topo_count == 2u);
  const SceneSourceIdentity *first = vkr_entity_get_component(
      a.scene.world, a.scene.topo_order[0], a.scene.comp_source_identity);
  const SceneSourceIdentity *second = vkr_entity_get_component(
      b.scene.world, b.scene.topo_order[0], b.scene.comp_source_identity);
  assert(first && second && first->source_fingerprint &&
         second->source_fingerprint);
  assert(first->source_fingerprint != second->source_fingerprint);
  scene_loader_test_context_shutdown(&b);
  scene_loader_test_context_shutdown(&a);
}

bool32_t run_scene_loader_tests(void) {
  printf("--- Starting Scene Loader Tests ---\n");

  test_scene_source_nodes_preserve_hierarchy_and_exact_matrix();
  test_scene_source_fingerprint_detects_entity_reordering();
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
