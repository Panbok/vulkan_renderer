#include "scene_animation_tests.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "test_stats.h"

#include "assets/vkr_animation_encode.h"
#include "assets/vkr_animation_import.h"
#include "assets/vkr_mesh_cook_source.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/animation_loader.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_animation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static const char *s_mesh_path = "build/vkr_scene_animation.vkb";
static const char *s_bank_path = "build/vkr_scene_animation.vka";
static const char *s_wrong_path = "build/vkr_scene_animation_wrong.vka";

static String8 scene_animation_test_path(const char *path) {
  return string8_create_from_cstr((const uint8_t *)path, string_length(path));
}

static void scene_animation_test_write(const char *path, const void *bytes,
                                       uint64_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(bytes, 1, size, file) == size);
  assert(fclose(file) == 0);
}

static void scene_animation_test_requests(VkrAllocator *scratch,
                                          const char *bank,
                                          VkrResourceHandleInfo *mesh_request,
                                          VkrResourceHandleInfo *bank_request) {
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_MESH,
                                       scene_animation_test_path(s_mesh_path),
                                       scratch, mesh_request, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_ANIMATION,
                                       scene_animation_test_path(bank), scratch,
                                       bank_request, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
}

static VkrEntityId
scene_animation_test_wrapper(VkrScene *scene,
                             VkrResourceHandleInfo *mesh_request,
                             VkrEntityId *node, uint32_t index, float32_t x) {
  VkrResourceHandleInfo resolved = {0};
  assert(vkr_resource_system_try_get_resolved(mesh_request, &resolved));
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  VkrEntityId wrapper = vkr_scene_create_entity(scene, &error);
  assert(wrapper.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(scene, wrapper, vec3_new(x, 0, 0),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_instantiate_source_nodes(scene, &resolved.as.mesh->source,
                                            wrapper, index, node, &error));
  vkr_scene_update(scene, 0);
  return wrapper;
}

static void scene_animation_test_json(VkrAllocator *allocator,
                                      VkrAllocator *scratch,
                                      VkrArenaPool *pool) {
  VkrRenderAssets assets = {.allocator = *allocator,
                            .scratch_allocator = *scratch};
  VkrScene scene = {0};
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  VkrSceneLoadResult result = {0};
  const char *file_path = "build/vkr_scene_animation.scene.json";
  const char file_json[] =
      "{\"version\":2,\"entities\":["
      "{\"name\":\"Default\",\"mesh\":{\"path\":\"./vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./vkr_scene_animation.vka\"}},"
      "{\"name\":\"Reverse\",\"mesh\":{\"path\":\"./vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./vkr_scene_animation.vka\",\"clip\":0,"
      "\"rate\":-2,\"loop\":false,\"playing\":false}}]}";
  scene_animation_test_write(file_path, file_json, sizeof(file_json) - 1);
  assert(vkr_scene_init(&scene, allocator, 2, 32, &error));
  bool8_t loaded = vkr_scene_load_from_file(
      &scene, &assets, scene_animation_test_path(file_path), scratch, &result,
      &error);
  if (!loaded) {
    fprintf(stderr, "Animation scene file load failed: error=%u\n", error);
  }
  assert(loaded);
  assert(error == VKR_SCENE_ERROR_NONE && result.entity_count == 4);
  VkrAnimationPlayer *a = vkr_scene_animation_get_player(
      &scene, vkr_scene_find_entity_by_name(&scene, string8_lit("Default")));
  VkrAnimationPlayer *b = vkr_scene_animation_get_player(
      &scene, vkr_scene_find_entity_by_name(&scene, string8_lit("Reverse")));
  assert(a && b && a != b);
  assert(vkr_animation_player_playing(a));
  assert(!vkr_animation_player_playing(b));
  vkr_scene_update(&scene, 2.5);
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_time(b) == 0);
  assert(vkr_animation_player_seek(b, 1));
  vkr_animation_player_set_playing(b, true_v);
  vkr_scene_update(&scene, 0.25);
  assert(vkr_animation_player_time(a) == 0.75);
  assert(vkr_animation_player_time(b) == 0.5);
  vkr_scene_update(&scene, 1);
  assert(vkr_animation_player_time(b) == 0);
  vkr_scene_shutdown(&scene, NULL);
  assert(pool->pool.allocated == 0);
  assert(remove(file_path) == 0);

  const char *valid =
      "{\"version\":2,\"entities\":[{\"name\":\"Buffer\","
      "\"mesh\":{\"path\":\"build/vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"build/vkr_scene_animation.vka\","
      "\"controller\":{\"version\":1,\"cycle\":2,\"root\":0,\"initial\":0,"
      "\"parameters\":[],\"nodes\":[[0,0,0,0,0,0,[],[]]],"
      "\"states\":[],\"transitions\":[]}}}]}";
  assert(vkr_scene_init(&scene, allocator, 3, 32, &error));
  assert(vkr_scene_load_from_json(&scene, &assets,
                                  scene_animation_test_path(valid), scratch,
                                  &result, &error));
  a = vkr_scene_animation_get_player(
      &scene, vkr_scene_find_entity_by_name(&scene, string8_lit("Buffer")));
  assert(a && vkr_animation_player_clip(a) == 0);
  assert(vkr_scene_animation_get_graph(
      &scene, vkr_scene_find_entity_by_name(&scene, string8_lit("Buffer"))));
  vkr_scene_update(&scene, 0.5);
  assert(vkr_animation_player_global_pose(a)[0].elements[12] == 2);
  vkr_scene_shutdown(&scene, NULL);
  assert(pool->pool.allocated == 0);

  const char *invalid_animation[] = {
      "null",
      "[]",
      "{}",
      "{\"path\":\"\"}",
      "{\"path\":3}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"clip\":-1}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"clip\":0.5}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"clip\":4294967296}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"clip\":1}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"loop\":1}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"playing\":\"true\"}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"rate\":1e999}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"rate\":+1}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"rate\":.5}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"rate\":01}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"rate\":1e}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"controller\":null}",
      "{\"path\":\"build/vkr_scene_animation.vka\",\"controller\":{}}",
      "{\"path\":\"build/vkr_scene_animation_wrong.vka\"}",
  };
  for (uint32_t i = 0; i < ArrayCount(invalid_animation); ++i) {
    char json[1024];
    int count =
        snprintf(json, sizeof(json),
                 "{\"version\":2,\"entities\":[{\"mesh\":{\"path\":"
                 "\"build/vkr_scene_animation.vkb\"},\"animation\":%s}]}",
                 invalid_animation[i]);
    assert(count > 0 && (uint64_t)count < sizeof(json));
    assert(vkr_scene_init(&scene, allocator, 4, 32, &error));
    assert(!vkr_scene_load_from_json(&scene, &assets,
                                     scene_animation_test_path(json), scratch,
                                     &result, &error));
    assert(error != VKR_SCENE_ERROR_NONE);
    assert(!scene.animations);
    vkr_scene_shutdown(&scene, NULL);
    assert(pool->pool.allocated == 0);
  }
  assert(vkr_scene_init(&scene, allocator, 5, 32, &error));
  assert(!vkr_scene_load_from_json(
      &scene, &assets,
      string8_lit("{\"version\":2,\"entities\":[{\"animation\":{\"path\":"
                  "\"build/vkr_scene_animation.vka\"}}]}"),
      scratch, &result, &error));
  vkr_scene_shutdown(&scene, NULL);
  assert(pool->pool.allocated == 0);
}

static VkrResourceLoadState
scene_animation_test_pump(VkrResourceHandleInfo *request) {
  VkrResourceSubmissionState submission = {.submit_serial = 1,
                                           .completed_submit_serial = 100,
                                           .frame_active = true_v};
  VkrResourceLoadState state = VKR_RESOURCE_LOAD_STATE_INVALID;
  for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
    vkr_resource_system_pump(submission, NULL);
    state = vkr_resource_system_get_state(request, NULL);
    if (state == VKR_RESOURCE_LOAD_STATE_READY ||
        state == VKR_RESOURCE_LOAD_STATE_FAILED) {
      break;
    }
    vkr_platform_sleep(2);
  }
  if (state != VKR_RESOURCE_LOAD_STATE_READY &&
      state != VKR_RESOURCE_LOAD_STATE_FAILED) {
    fprintf(stderr, "Animation scene async request timed out: state=%u\n",
            state);
  }
  return state;
}

static void scene_animation_test_async(VkrAllocator *allocator,
                                       VkrAllocator *scratch,
                                       VkrArenaPool *pool) {
  const char *path = "build/vkr_scene_animation_async.scene.json";
  const char *wrong_path = "build/vkr_scene_animation_async_wrong.scene.json";
  const char json[] =
      "{\"version\":2,\"entities\":["
      "{\"name\":\"AsyncA\",\"mesh\":{\"path\":\"./vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./vkr_scene_animation.vka\"}},"
      "{\"name\":\"AsyncB\",\"mesh\":{\"path\":\"./vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./"
      "vkr_scene_animation.vka\",\"playing\":false}},"
      "{\"name\":\"AsyncGraph\",\"mesh\":{\"path\":\"./"
      "vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./vkr_scene_animation.vka\","
      "\"controller\":{\"version\":1,\"cycle\":4,\"root\":0,\"initial\":0,"
      "\"parameters\":[],\"nodes\":[[0,0,0,0,0,0,[],[]]],"
      "\"states\":[],\"transitions\":[]}}}]"
      "}";
  const char wrong_json[] =
      "{\"version\":2,\"entities\":["
      "{\"mesh\":{\"path\":\"./vkr_scene_animation.vkb\"},"
      "\"animation\":{\"path\":\"./vkr_scene_animation_wrong.vka\"}}]}";
  scene_animation_test_write(path, json, sizeof(json) - 1);
  scene_animation_test_write(wrong_path, wrong_json, sizeof(wrong_json) - 1);
  VkrJobSystem jobs = {0};
  VkrJobSystemConfig job_config = vkr_job_system_config_default();
  job_config.worker_count = 1;
  job_config.max_jobs = 64;
  job_config.queue_capacity = 64;
  assert(vkr_job_system_init(&job_config, &jobs));
  VkrRenderAssets assets = {.allocator = *allocator,
                            .scratch_allocator = *scratch};
  VkrMeshLoaderContext mesh_context = {.arena_pool = pool, .job_system = &jobs};
  assert(vkr_dmemory_create(KB(64), MB(4), &assets.scene_async_memory));
  assets.scene_async_allocator =
      (VkrAllocator){.ctx = &assets.scene_async_memory};
  vkr_dmemory_allocator_create(&assets.scene_async_allocator);
  assert(vkr_mutex_create(allocator, &assets.scene_async_mutex));
  assert(vkr_dmemory_create(KB(64), MB(4), &mesh_context.async_memory));
  mesh_context.async_allocator =
      (VkrAllocator){.ctx = &mesh_context.async_memory};
  vkr_dmemory_allocator_create(&mesh_context.async_allocator);
  assert(vkr_mutex_create(allocator, &mesh_context.async_mutex));
  assert(vkr_resource_system_init(allocator, &jobs, NULL));
  assert(vkr_resource_system_register_loader(
      &mesh_context, vkr_mesh_loader_create(&mesh_context)));
  assert(vkr_resource_system_register_loader(&assets,
                                             vkr_animation_loader_create()));
  assert(
      vkr_resource_system_register_loader(&assets, vkr_scene_loader_create()));
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrResourceHandleInfo request = {0};
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE,
                                  scene_animation_test_path(path), scratch,
                                  &request, &error));
  assert(request.request_id != 0);
  assert(scene_animation_test_pump(&request) == VKR_RESOURCE_LOAD_STATE_READY);
  VkrResourceHandleInfo resolved = {0};
  assert(vkr_resource_system_try_get_resolved(&request, &resolved));
  VkrScene *scene = vkr_scene_handle_get_scene(resolved.as.scene);
  assert(scene);
  VkrAnimationPlayer *a = vkr_scene_animation_get_player(
      scene, vkr_scene_find_entity_by_name(scene, string8_lit("AsyncA")));
  VkrAnimationPlayer *b = vkr_scene_animation_get_player(
      scene, vkr_scene_find_entity_by_name(scene, string8_lit("AsyncB")));
  assert(a && b && a != b);
  vkr_scene_update(scene, 0.5);
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_time(b) == 0);
  assert(vkr_animation_player_global_pose(a)[0].elements[12] == 2);
  VkrEntityId graph_entity =
      vkr_scene_find_entity_by_name(scene, string8_lit("AsyncGraph"));
  const VkrAnimationGraphInstance *controller =
      vkr_scene_animation_get_graph(scene, graph_entity);
  assert(controller && controller->time == 0.5);
  VkrAnimationPlayer *graph_player =
      vkr_scene_animation_get_player(scene, graph_entity);
  assert(graph_player &&
         vkr_animation_player_global_pose(graph_player)[0].elements[12] == 1);
  /* Retain actual shared dependency requests across scene unload. Two scene
   * bindings must release their references without retiring this caller's. */
  VkrResourceHandleInfo mesh = {0};
  VkrResourceHandleInfo bank = {0};
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH,
                                  scene_animation_test_path(s_mesh_path),
                                  scratch, &mesh, &error));
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION,
                                  scene_animation_test_path(s_bank_path),
                                  scratch, &bank, &error));
  assert(mesh.request_id && bank.request_id);
  assert(scene_animation_test_pump(&mesh) == VKR_RESOURCE_LOAD_STATE_READY);
  assert(scene_animation_test_pump(&bank) == VKR_RESOURCE_LOAD_STATE_READY);
  vkr_resource_system_unload(&request, scene_animation_test_path(path));
  assert(!vkr_resource_system_try_get_resolved(&request, &resolved));
  assert(vkr_resource_system_try_get_resolved(&bank, &resolved));
  assert(resolved.as.animation->asset.clip_count == 1);
  assert(vkr_resource_system_try_get_resolved(&mesh, &resolved));
  assert(pool->pool.allocated == 1);
  vkr_resource_system_unload(&bank, scene_animation_test_path(s_bank_path));
  vkr_resource_system_unload(&mesh, scene_animation_test_path(s_mesh_path));
  assert(!vkr_resource_system_try_get_resolved(&bank, &resolved));
  assert(!vkr_resource_system_try_get_resolved(&mesh, &resolved));
  assert(pool->pool.allocated == 0);

  VkrResourceHandleInfo failed = {0};
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE,
                                  scene_animation_test_path(wrong_path),
                                  scratch, &failed, &error));
  assert(scene_animation_test_pump(&failed) == VKR_RESOURCE_LOAD_STATE_FAILED);
  vkr_resource_system_unload(&failed, scene_animation_test_path(wrong_path));
  for (uint32_t i = 0; i < 4; ++i) {
    VkrResourceHandleInfo cancelled = {0};
    assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE,
                                    scene_animation_test_path(path), scratch,
                                    &cancelled, &error));
    vkr_resource_system_unload(&cancelled, scene_animation_test_path(path));
    VkrResourceSubmissionState submission = {.submit_serial = 1,
                                             .completed_submit_serial = 100,
                                             .frame_active = true_v};
    VkrResourceLoadState state = VKR_RESOURCE_LOAD_STATE_CANCELED;
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
      vkr_resource_system_pump(submission, NULL);
      state = vkr_resource_system_get_state(&cancelled, NULL);
      if (state == VKR_RESOURCE_LOAD_STATE_INVALID) {
        break;
      }
      assert(state == VKR_RESOURCE_LOAD_STATE_CANCELED);
      vkr_platform_sleep(2);
    }
    assert(state == VKR_RESOURCE_LOAD_STATE_INVALID);
  }
  /* Joining workers before registry shutdown ensures cancelled prepare results
   * are cleaned while both loader contexts and their allocators still live. */
  vkr_job_system_shutdown(&jobs);
  vkr_resource_system_shutdown();
  assert(pool->pool.allocated == 0);
  VKR_TEST_ASSERT_STATS(assets.scene_async_allocator.stats.total_allocated ==
                        0);
  VKR_TEST_ASSERT_STATS(mesh_context.async_allocator.stats.total_allocated ==
                        0);
  vkr_mutex_destroy(allocator, &mesh_context.async_mutex);
  vkr_mutex_destroy(allocator, &assets.scene_async_mutex);
  vkr_dmemory_allocator_destroy(&mesh_context.async_allocator);
  vkr_dmemory_allocator_destroy(&assets.scene_async_allocator);
  assert(remove(path) == 0);
  assert(remove(wrong_path) == 0);
}

bool32_t run_scene_animation_tests(void) {
  printf("Running scene animation lifecycle tests...\n");
  /* This metadata-only synthetic fixture isolates CPU binding ownership. It
   * never creates a renderer, GPU object, or rendered scene. */
  const char *source_path = "build/vkr_scene_animation.gltf";
  const char *buffer_path = "build/vkr_scene_animation.bin";
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"vkr_scene_animation.bin\",\"byteLength\":32}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
      "\"type\":\"SCALAR\",\"min\":[0],\"max\":[2]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}]"
      ","
      "\"nodes\":[{\"name\":\"Joint\"}],\"skins\":[{\"joints\":[0],"
      "\"skeleton\":0}],"
      "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
      "\"animations\":[{\"name\":\"Move\",\"samplers\":[{\"input\":0,"
      "\"output\":1,"
      "\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,"
      "\"target\":{\"node\":0,\"path\":\"translation\"}}]}]}";
  /* Explicit little-endian float bits avoid using a matching encoder as the
   * source-data oracle: times [0,2], translations [(0,0,0),(8,0,0)]. */
  const uint8_t buffer[32] = {0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0};
  scene_animation_test_write(source_path, json, sizeof(json) - 1);
  scene_animation_test_write(buffer_path, buffer, sizeof(buffer));
  Arena *arena = arena_create(MB(32), MB(2));
  Arena *scratch_arena = arena_create(MB(32), MB(2));
  assert(arena && scratch_arena);
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch));
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrMeshCookStats stats = {0};
  assert(vkr_mesh_cook_source(scene_animation_test_path(source_path),
                              scene_animation_test_path(s_mesh_path),
                              &allocator, &scratch, &stats, &renderer_error));
  assert(stats.range_count == 0);
  const char *error = NULL;
  VkrAnimationAsset bank = {0};
  assert(vkr_animation_import_gltf(&allocator, &scratch,
                                   scene_animation_test_path(source_path),
                                   &bank, &error));
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  assert(vkr_animation_cooked_encode(&scratch, &bank, &bytes, &size, &error));
  scene_animation_test_write(s_bank_path, bytes, size);
  bank.source_fingerprint ^= 1u;
  assert(vkr_animation_cooked_encode(&scratch, &bank, &bytes, &size, &error));
  scene_animation_test_write(s_wrong_path, bytes, size);

  VkrArenaPool pool = {0};
  assert(vkr_arena_pool_create(MB(1), 4, &allocator, &pool));
  VkrMeshLoaderContext mesh_context = {.arena_pool = &pool};
  assert(vkr_resource_system_init(&allocator, NULL, NULL));
  assert(vkr_resource_system_register_loader(
      &mesh_context, vkr_mesh_loader_create(&mesh_context)));
  assert(vkr_resource_system_register_loader(&allocator,
                                             vkr_animation_loader_create()));
  VkrScene scene = {0};
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 1, 32, &scene_error));
  VkrEntityId wrappers[2];
  VkrEntityId nodes[2];
  VkrSceneAnimationConfig config = VKR_SCENE_ANIMATION_CONFIG_DEFAULT;
  for (uint32_t i = 0; i < 2; ++i) {
    VkrResourceHandleInfo mesh_request = {0};
    VkrResourceHandleInfo bank_request = {0};
    scene_animation_test_requests(&scratch, s_bank_path, &mesh_request,
                                  &bank_request);
    wrappers[i] = scene_animation_test_wrapper(&scene, &mesh_request, &nodes[i],
                                               i, 10.0f * (i + 1));
    assert(vkr_scene_animation_attach(&scene, wrappers[i], &mesh_request,
                                      &bank_request, &nodes[i], 1, &config,
                                      &scratch, &error));
    assert(!error);
    assert(mesh_request.loader_id == VKR_INVALID_ID);
    assert(bank_request.loader_id == VKR_INVALID_ID);
  }
  VkrAnimationPlayer *a = vkr_scene_animation_get_player(&scene, wrappers[0]);
  VkrAnimationPlayer *b = vkr_scene_animation_get_player(&scene, wrappers[1]);
  assert(a && b && a != b);
  Mat4 local = vkr_scene_get_transform(&scene, nodes[0])->local;
  Mat4 world = vkr_scene_get_transform(&scene, nodes[0])->world;
  vkr_animation_player_set_playing(b, false_v);
  vkr_scene_update(&scene, 0.5);
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_time(b) == 0);
  assert(fabsf(vkr_animation_player_global_pose(a)[0].elements[12] - 2) <
         1e-6f);
  assert(MemCompare(&local, &vkr_scene_get_transform(&scene, nodes[0])->local,
                    sizeof(local)) == 0);
  assert(MemCompare(&world, &vkr_scene_get_transform(&scene, nodes[0])->world,
                    sizeof(world)) == 0);
  vkr_scene_update(&scene, -1);
  vkr_scene_update(&scene, NAN);
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_seek(b, 1));
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_global_pose(b)[0].elements[12] == 4);

  /* The scene must advance the installed controller, not overwrite its pose
   * with the underlying clip clock. Cycle 4s maps 1s to source time .5s. */
  VkrAnimationGraph graph = {
      .node_count = 1,
      .cycle_seconds = 4.0,
      .nodes = {{.kind = VKR_ANIMATION_GRAPH_CLIP, .clip = 0}}};
  assert(vkr_scene_animation_apply_graph(&scene, wrappers[0], &graph, &error));
  assert(vkr_scene_animation_get_graph(&scene, wrappers[0]));
  vkr_scene_update(&scene, 1.0);
  assert(fabsf(vkr_animation_player_global_pose(a)[0].elements[12] - 2) <
         1e-6f);
  uint64_t generation = vkr_animation_player_generation(a);
  graph.nodes[0].clip = 9;
  assert(!vkr_scene_animation_apply_graph(&scene, wrappers[0], &graph, &error));
  assert(vkr_animation_player_generation(a) == generation);
  assert(vkr_scene_animation_get_graph(&scene, wrappers[0])->time == 1.0);
  assert(vkr_scene_animation_seek(&scene, wrappers[0], 2.0));
  assert(fabsf(vkr_animation_player_global_pose(a)[0].elements[12] - 4) <
         1e-6f);
  assert(vkr_scene_animation_apply_graph(&scene, wrappers[0], NULL, &error));
  assert(!vkr_scene_animation_get_graph(&scene, wrappers[0]));

  /* A solved bone drives the existing inverse-bind skin palette without
   * overwriting imported node TRS or the sampled local animation pose. */
  vkr_scene_physics_set_paused(&scene, true_v);
  VkrScenePhysicsChange ragdoll[1];
  uint32_t ragdoll_count = 0;
  assert(vkr_scene_physics_ragdoll_plan(&scene, wrappers[0],
                                        VKR_SCENE_RAGDOLL_CREATE, ragdoll, 1,
                                        &ragdoll_count, &error));
  assert(ragdoll_count == 1 && ragdoll[0].entity.u64 == nodes[0].u64);
  assert(vkr_scene_physics_apply(&scene, ragdoll[0].entity,
                                 &ragdoll[0].snapshot, &error));
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_animation_player_time(a) == 0.0);
  for (uint32_t tick = 0; tick < 30; ++tick) {
    assert(vkr_scene_physics_step(&scene, &error));
  }
  assert(fabs(vkr_animation_player_time(a) - 0.5) < 1e-9);
  VkrPhysicsPose solved;
  assert(vkr_scene_physics_get_pose(&scene, nodes[0], &solved));
  const Mat4 *palette = vkr_animation_player_skin_palette(a, 0);
  assert(palette && solved.position[1] < -1.0f);
  assert(fabsf(palette[0].elements[13] - solved.position[1]) < 0.001f);
  assert(MemCompare(&local, &vkr_scene_get_transform(&scene, nodes[0])->local,
                    sizeof(local)) == 0);
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_animation_player_time(a) == 0.0);
  /* A failure after seek-to-zero must retain the prior published slot and
   * fade/controller state. Simulate an invalid external ECS ancestor write;
   * native reset validation rejects it after all players have sought. */
  assert(vkr_animation_player_crossfade(a, 0, true_v, 1.0));
  assert(vkr_animation_player_advance(a, 0.1));
  graph.nodes[0].clip = 0;
  assert(vkr_scene_animation_apply_graph(&scene, wrappers[1], &graph, &error));
  assert(vkr_scene_animation_seek(&scene, wrappers[1], 0.7));
  const float64_t saved_time = vkr_animation_player_time(a);
  const float64_t saved_fade = vkr_animation_player_crossfade_progress(a);
  const uint64_t saved_generation = vkr_animation_player_generation(a);
  const Mat4 saved_palette = vkr_animation_player_skin_palette(a, 0)[0];
  const Mat4 saved_global = vkr_animation_player_global_pose(a)[0];
  const float64_t saved_graph_time =
      vkr_scene_animation_get_graph(&scene, wrappers[1])->time;
  SceneTransform *reset_wrapper = vkr_scene_get_transform(&scene, wrappers[0]);
  const SceneTransform saved_wrapper = *reset_wrapper;
  reset_wrapper->scale.x = -1.0f;
  assert(vkr_scene_physics_set_body_disabled(&scene, nodes[0], true_v, &error));
  assert(vkr_scene_physics_set_disabled(&scene, true_v, &error));
  assert(!vkr_scene_physics_reset(&scene, &error));
  assert(vkr_scene_physics_is_disabled(&scene));
  assert(vkr_scene_physics_body_is_disabled(&scene, nodes[0]));
  *reset_wrapper = saved_wrapper;
  assert(vkr_animation_player_time(a) == saved_time);
  assert(vkr_animation_player_crossfade_active(a));
  assert(vkr_animation_player_crossfade_progress(a) == saved_fade);
  assert(vkr_animation_player_generation(a) == saved_generation);
  assert(MemCompare(&saved_palette, vkr_animation_player_skin_palette(a, 0),
                    sizeof(saved_palette)) == 0);
  assert(MemCompare(&saved_global, vkr_animation_player_global_pose(a),
                    sizeof(saved_global)) == 0);
  assert(vkr_scene_animation_get_graph(&scene, wrappers[1])->time ==
         saved_graph_time);
  assert(vkr_scene_physics_set_disabled(&scene, false_v, &error));
  assert(vkr_scene_animation_apply_graph(&scene, wrappers[1], NULL, &error));
  assert(vkr_scene_physics_apply(&scene, nodes[0], NULL, &error));

  VkrResourceHandleInfo wrong_mesh = {0};
  VkrResourceHandleInfo wrong_bank = {0};
  scene_animation_test_requests(&scratch, s_wrong_path, &wrong_mesh,
                                &wrong_bank);
  VkrEntityId wrong_node;
  VkrEntityId wrong_wrapper =
      scene_animation_test_wrapper(&scene, &wrong_mesh, &wrong_node, 2, 30);
  VkrResourceHandleInfo wrong_mesh_before = wrong_mesh;
  VkrResourceHandleInfo wrong_bank_before = wrong_bank;
  assert(!vkr_scene_animation_attach(&scene, wrong_wrapper, &wrong_mesh,
                                     &wrong_bank, &wrong_node, 1, &config,
                                     &scratch, &error));
  assert(error);
  assert(MemCompare(&wrong_mesh_before, &wrong_mesh, sizeof(wrong_mesh)) == 0);
  assert(MemCompare(&wrong_bank_before, &wrong_bank, sizeof(wrong_bank)) == 0);
  VkrResourceHandleInfo resolved = {0};
  assert(vkr_resource_system_try_get_resolved(&wrong_mesh, &resolved));
  assert(vkr_resource_system_try_get_resolved(&wrong_bank, &resolved));
  vkr_resource_system_unload(&wrong_bank,
                             scene_animation_test_path(s_wrong_path));
  vkr_resource_system_unload(&wrong_mesh,
                             scene_animation_test_path(s_mesh_path));

  uint64_t live_before_detach =
      vkr_allocator_get_global_statistics().total_allocated;
  vkr_scene_set_parent(&scene, nodes[0], wrappers[1]);
  vkr_scene_update(&scene, 0);
  assert(!vkr_scene_animation_get_player(&scene, wrappers[0]));
  assert(pool.pool.allocated == 1);
  VKR_TEST_ASSERT_STATS(vkr_allocator_get_global_statistics().total_allocated <
                        live_before_detach);
  assert(vkr_scene_animation_get_player(&scene, wrappers[1]) == b);
  assert(vkr_scene_physics_ragdoll_plan(&scene, wrappers[1],
                                        VKR_SCENE_RAGDOLL_CREATE, ragdoll, 1,
                                        &ragdoll_count, &error));
  assert(vkr_scene_physics_apply(&scene, ragdoll[0].entity,
                                 &ragdoll[0].snapshot, &error));
  vkr_scene_destroy_entity(&scene, wrappers[1]);
  assert(vkr_scene_physics_body_count(&scene) == 0);
  assert(!vkr_scene_animation_get_player(&scene, wrappers[1]));
  assert(pool.pool.allocated == 0);

  VkrResourceHandleInfo last_mesh = {0};
  VkrResourceHandleInfo last_bank = {0};
  scene_animation_test_requests(&scratch, s_bank_path, &last_mesh, &last_bank);
  VkrEntityId last_node;
  VkrEntityId last_wrapper =
      scene_animation_test_wrapper(&scene, &last_mesh, &last_node, 3, 40);
  assert(vkr_scene_animation_attach(&scene, last_wrapper, &last_mesh,
                                    &last_bank, &last_node, 1, &config,
                                    &scratch, &error));
  vkr_scene_shutdown(&scene, NULL);
  assert(pool.pool.allocated == 0);
  scene_animation_test_json(&allocator, &scratch, &pool);
  vkr_resource_system_shutdown();
  scene_animation_test_async(&allocator, &scratch, &pool);
  vkr_arena_pool_destroy(&allocator, &pool);
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(scratch_arena);
  arena_destroy(arena);
  assert(remove(source_path) == 0);
  assert(remove(buffer_path) == 0);
  assert(remove(s_mesh_path) == 0);
  assert(remove(s_bank_path) == 0);
  assert(remove(s_wrong_path) == 0);
  printf("Scene animation lifecycle tests PASSED\n");
  return true_v;
}
