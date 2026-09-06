#include "scene_edit_test.h"

#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_edit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void edit_test_write(const char *path, const char *bytes, size_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(bytes, 1, size, file) == size);
  assert(fclose(file) == 0);
}

static VkrEntityId edit_test_entity(VkrScene *scene, uint32_t node,
                                    const char *name) {
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  VkrEntityId entity = vkr_scene_create_entity(scene, &error);
  assert(entity.u64 && error == VKR_SCENE_ERROR_NONE);
  assert(vkr_scene_set_name(scene, entity,
                            string8_create((uint8_t *)name, strlen(name))));
  assert(vkr_scene_set_transform(scene, entity, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_set_visibility(scene, entity, true_v, true_v);
  SceneSourceIdentity identity = {.scene_entity_index = 7,
                                  .gltf_node_index = node,
                                  .source_fingerprint = 0x1122334455667788ull};
  assert(vkr_scene_set_source_identity(scene, entity, &identity));
  return entity;
}

/* A build cannot detect partial file acceptance or mutations before a later
   identity conflict. These literal files have independent scene-value oracles;
   the real scene allocator and serializer also exercise name ownership. */
bool32_t run_scene_edit_tests(void) {
  printf("--- Starting Scene Edit Tests ---\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 19, 8, &error));
  VkrEntityId parent = edit_test_entity(&scene, 0, "parent");
  VkrEntityId child = edit_test_entity(&scene, 1, "child");
  vkr_scene_set_position(&scene, parent, vec3_new(10, 0, 0));
  vkr_scene_set_parent(&scene, child, parent);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, &allocator, 1);
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(&scene, child, &values));
  const char unicode_name[] = "Камера 🚀\n\"quoted\"";
  MemCopy(values.name, unicode_name, sizeof(unicode_name));
  values.position = vec3_new(2, 3, 4);
  values.scale = vec3_new(2, 1, 1);
  values.visibility.visible = false_v;
  assert(vkr_scene_edit_apply(&state, &scene, child, &values));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  VkrSceneEditValues read;
  assert(vkr_scene_edit_read(&scene, child, &read));
  assert(strcmp(read.name, "child") == 0 && read.position.x == 0 &&
         read.visibility.visible);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_scene_edit_read(&scene, child, &read));
  assert(strcmp(read.name, unicode_name) == 0 && read.position.x == 2 &&
         !read.visibility.visible);
  vkr_scene_update(&scene, 0.0);
  assert(vkr_scene_get_transform(&scene, child)->parent.u64 == parent.u64);
  assert(vkr_scene_get_transform(&scene, child)->world.elements[12] == 12);

  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/scene_edit_%u.json",
           vkr_platform_get_process_id());
  String8 file_path = string8_create((uint8_t *)path, strlen(path));
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  char saved[4096];
  FILE *file = fopen(path, "rb");
  assert(file);
  size_t saved_size = fread(saved, 1, sizeof(saved), file);
  assert(saved_size > 0 && saved_size < sizeof(saved) && feof(file));
  fclose(file);

  assert(vkr_scene_set_name(&scene, child, string8_lit("temporary")));
  vkr_scene_set_position(&scene, child, vec3_zero());
  vkr_scene_set_visibility(&scene, child, true_v, true_v);
  vkr_scene_edit_reset(&state, &allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(state.touched_count == 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(state.touched_count == 1);
  assert(vkr_scene_edit_read(&scene, child, &read));
  assert(strcmp(read.name, unicode_name) == 0 && read.position.x == 2 &&
         read.position.y == 3 && read.position.z == 4 && read.scale.x == 2 &&
         !read.visibility.visible);
  assert(vkr_scene_get_transform(&scene, child)->parent.u64 == parent.u64);

  const char *invalid[] = {
      "{\"version\":1,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":1,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":2,\"name\":"
      "\"first\"},"
      "{\"scene_entity\":7,\"gltf_node\":1,\"source_fingerprint\":"
      "\"1122334455667788\","
      "\"fields\":2,\"name\":\"duplicate\"}]}",

      "{\"version\":1,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":1,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":2,\"name\":"
      "\"mutated\"}",
      "{\"version\":1,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":0,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":2,\"name\":"
      "\"mutated\"},"
      "{\"scene_entity\":7,\"gltf_node\":1,\"source_fingerprint\":"
      "\"0000000000000000\","
      "\"fields\":2,\"name\":\"stale\"}]}",
      "{\"nested\":{\"version\":1},\"overrides\":[]}",
      "{\"version\":1,\"version\":1,\"overrides\":[]}",
      "{\"version\":1,\"overrides\":[]} garbage",
      "{\"version\":1,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":1,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":2,\"name\":"
      "\"\\ud800\"}]}",
  };
  uint32_t touched = state.touched_count;
  uint64_t revision = state.revision;
  for (uint32_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
    edit_test_write(path, invalid[i], strlen(invalid[i]));
    assert(!vkr_scene_edit_load(&state, &scene, file_path));
    assert(state.sidecar_conflict && state.touched_count == touched &&
           state.revision == revision);
    assert(vkr_scene_edit_read(&scene, child, &read));
    assert(strcmp(read.name, unicode_name) == 0 && read.position.x == 2 &&
           !read.visibility.visible);
    String8 name = vkr_scene_get_name(&scene, parent);
    assert(name.length == 6 && MemCompare(name.str, "parent", 6) == 0);
    assert(!vkr_scene_edit_save(&state, &scene, file_path));
    file = fopen(path, "rb");
    assert(file);
    char unchanged[1024];
    size_t size = fread(unchanged, 1, sizeof(unchanged), file);
    fclose(file);
    assert(size == strlen(invalid[i]) &&
           MemCompare(unchanged, invalid[i], size) == 0);
  }
  edit_test_write(path, saved, saved_size);
  assert(vkr_scene_edit_load(&state, &scene, file_path) &&
         !state.sidecar_conflict);

  const char escaped[] =
      "{\"overrides\":[{\"scene_entity\":7,\"gltf_node\":1,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":2,\"name\":"
      "\"\\u041a\\ud83d\\ude80\"}],\"version\":1}";
  edit_test_write(path, escaped, sizeof(escaped) - 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_edit_read(&scene, child, &read) &&
         strcmp(read.name, "К🚀") == 0);

  VkrSceneEditValues before = read, after = read;
  before.fields = after.fields = VKR_SCENE_EDIT_TRANSFORM;
  after.position = vec3_new(9, 8, 7);
  vkr_scene_set_position(&scene, child, after.position);
  assert(
      vkr_scene_edit_record_external(&state, &scene, child, &before, &after));
  assert(vkr_scene_get_transform(&scene, child)->position.x == 9);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_get_transform(&scene, child)->position.x == 2);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_scene_get_transform(&scene, child)->position.x == 9);
  assert(remove(path) == 0);
  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("--- Scene Edit Tests Completed ---\n");
  return true_v;
}
