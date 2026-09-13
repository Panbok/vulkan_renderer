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
  FILE *file = file_fopen(path, "wb");
  assert(file);
  assert(fwrite(bytes, 1, size, file) == size);
  assert(fclose(file) == 0);
}

static void edit_test_replace(const char *path, const char *bytes,
                              size_t length, const char *needle,
                              const char *replacement) {
  const char *match = strstr(bytes, needle);
  assert(match);
  FILE *file = file_fopen(path, "wb");
  assert(file);
  const size_t prefix = (size_t)(match - bytes);
  const size_t needle_length = strlen(needle);
  assert(fwrite(bytes, 1, prefix, file) == prefix);
  assert(fwrite(replacement, 1, strlen(replacement), file) ==
         strlen(replacement));
  const size_t suffix = length - prefix - needle_length;
  assert(fwrite(match + needle_length, 1, suffix, file) == suffix);
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
           PROJECT_SOURCE_DIR "tests/tmp/scene_edit_тест_%u.json",
           vkr_platform_get_process_id());
  String8 file_path = string8_create((uint8_t *)path, strlen(path));
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  char saved[4096];
  FILE *file = file_fopen(path, "rb");
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
    file = file_fopen(path, "rb");
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
  /* Structural undo must restore authored IDs even though recreated ECS
     children receive new generation handles. A malformed later record must
     discard the staged replacement without deleting the old compound. */
  vkr_scene_physics_set_paused(&scene, true_v);
  VkrSceneEditValues physics = {.fields = VKR_SCENE_EDIT_PHYSICS};
  physics.physics = vkr_scene_physics_default();
  physics.physics.present = true_v;
  physics.physics.motion = VKR_PHYSICS_DYNAMIC;
  physics.physics.collider_count = 2;
  physics.physics.colliders[0] =
      (VkrSceneColliderConfig){.authored_id = UINT64_C(0xfedcba9876543210),
                               .shape = VKR_PHYSICS_BOX,
                               .scale = {1, 1, 1},
                               .rotation = vkr_quat_identity(),
                               .half_extent = {1, 2, 3},
                               .radius = 0.5f,
                               .half_height = 0.5f,
                               .enabled = true_v};
  physics.physics.colliders[1] = physics.physics.colliders[0];
  physics.physics.colliders[1].authored_id = 42;
  physics.physics.colliders[1].shape = VKR_PHYSICS_SPHERE;
  physics.physics.colliders[1].position.x = 4;
  assert(vkr_scene_edit_apply(&state, &scene, parent, &physics));
  VkrEntityId collider = vkr_scene_physics_collider_entity(&scene, parent, 42);
  assert(collider.u64 &&
         vkr_scene_physics_owner(&scene, collider).u64 == parent.u64);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_edit_read(&scene, parent, &read) && !read.physics.present);
  assert(!vkr_scene_entity_alive(&scene, collider));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_scene_edit_read(&scene, parent, &read));
  assert(read.physics.collider_count == 2 &&
         read.physics.colliders[1].authored_id == 42);
  assert(read.physics.colliders[0].authored_id == UINT64_C(0xfedcba9876543210));
  physics.physics.collider_count = 1;
  assert(vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(!vkr_scene_physics_collider_entity(&scene, parent, 42).u64);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64);
  state.sidecar_conflict = false_v;
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  char physics_saved[8192];
  file = file_fopen(path, "rb");
  assert(file);
  size_t physics_saved_size =
      fread(physics_saved, 1, sizeof(physics_saved) - 1u, file);
  assert(physics_saved_size &&
         physics_saved_size < sizeof(physics_saved) - 1u && feof(file));
  fclose(file);
  physics_saved[physics_saved_size] = 0;
  char *saved_id = strstr(physics_saved, "000000000000002a");
  assert(saved_id);
  MemCopy(saved_id, "fedcba9876543210", 16);
  edit_test_write(path, physics_saved, physics_saved_size);
  collider = vkr_scene_physics_collider_entity(&scene, parent, 42);
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  MemCopy(saved_id, "000000000000002a", 16);
  edit_test_write(path, physics_saved, physics_saved_size);
  VkrScenePhysicsSnapshot absent = {0};
  assert(vkr_scene_physics_apply(&scene, parent, &absent, NULL));
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_edit_read(&scene, parent, &read));
  assert(read.physics.collider_count == 2 &&
         read.physics.motion == VKR_PHYSICS_DYNAMIC);
  assert(read.physics.colliders[0].half_extent.y == 2);
  assert(read.physics.colliders[0].authored_id == UINT64_C(0xfedcba9876543210));
  assert(read.physics.colliders[1].position.x == 4);
  const char *bad_physics =
      "{\"version\":2,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":0,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":64,\"physics\":{"
      "\"version\":1,\"present\":false,\"motion\":0,\"layer\":1,\"mask\":65535,"
      "\"parameters\":[1,0.5,0,1,"
      "0,0],"
      "\"enabled\":true,\"sleep\":true,\"continuous\":false,\"sensor\":false,"
      "\"colliders\":[]}},"
      "{\"scene_entity\":7,\"gltf_node\":1,\"source_fingerprint\":"
      "\"0000000000000000\","
      "\"fields\":2,\"name\":\"bad\"}]}";
  edit_test_write(path, bad_physics, strlen(bad_physics));
  collider = vkr_scene_physics_collider_entity(&scene, parent, 42);
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  assert(vkr_scene_edit_read(&scene, parent, &read) &&
         read.physics.collider_count == 2);
  physics.physics = read.physics;
  physics.physics.colliders[1].authored_id =
      physics.physics.colliders[0].authored_id;
  assert(!vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  physics.physics = read.physics;
  physics.fields = VKR_SCENE_EDIT_PHYSICS | VKR_SCENE_EDIT_TRANSFORM;
  physics.position = vec3_new(10000001.0f, 0, 0);
  physics.rotation = vkr_quat_identity();
  physics.scale = vec3_one();
  assert(!vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(vkr_scene_get_transform(&scene, parent)->position.x == 10);
  const char *invalid_physics_pose =
      "{\"version\":2,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":0,"
      "\"source_fingerprint\":\"1122334455667788\",\"fields\":65,"
      "\"position\":[10000001,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],"
      "\"physics\":{\"version\":1,\"present\":true,\"motion\":2,\"layer\":1,"
      "\"mask\":65535,\"parameters\":[1,0.5,0,1,0,0],\"enabled\":true,"
      "\"sleep\":true,\"continuous\":false,\"sensor\":false,\"colliders\":[]}}]"
      "}";
  edit_test_write(path, invalid_physics_pose, strlen(invalid_physics_pose));
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_get_transform(&scene, parent)->position.x == 10);
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  /* Named settings affect all bodies atomically and occupy one journal entry
     independently of owner snapshots. Symmetry is checked at the input
     boundary. */
  VkrSceneCollisionLayers layers = vkr_scene_collision_layers_default();
  MemCopy(layers.names[0], "World", sizeof("World"));
  MemCopy(layers.names[1], "Actors", sizeof("Actors"));
  layers.matrix[0] &= (uint16_t)~2u;
  layers.matrix[1] &= (uint16_t)~1u;
  uint32_t undo_before = state.undo_cursor;
  assert(vkr_scene_edit_apply_collision_layers(&state, &scene, &layers));
  assert(state.undo_cursor == undo_before + 1u);
  assert(vkr_scene_collision_layers_effective_mask(&scene, 1, UINT16_MAX) ==
         65533);
  assert(vkr_scene_collision_layers_effective_mask(&scene, 2, UINT16_MAX) ==
         65534);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  VkrSceneCollisionLayers settings_read;
  vkr_scene_collision_layers_read(&scene, &settings_read);
  assert(!strcmp(settings_read.names[0], "Layer 1"));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  vkr_scene_collision_layers_read(&scene, &settings_read);
  assert(!strcmp(settings_read.names[0], "World"));
  VkrSceneCollisionLayers asymmetric = layers;
  asymmetric.matrix[0] |= 2u;
  assert(!vkr_scene_edit_apply_collision_layers(&state, &scene, &asymmetric));
  assert(vkr_scene_collision_layers_effective_mask(&scene, 1, UINT16_MAX) ==
         65533);

  /* A joint may refer to a body introduced later in the same batch. Undo and
     redo must restore both owners together, including the previously absent
     body. */
  const VkrEntityId target = edit_test_entity(&scene, 2, "joint_target");
  const SceneSourceIdentity target_source =
      *(const SceneSourceIdentity *)vkr_entity_get_component(
          scene.world, target, scene.comp_source_identity);
  VkrScenePhysicsChange changes[2] = {{.entity = parent}, {.entity = target}};
  assert(vkr_scene_physics_read(&scene, parent, &changes[0].snapshot));
  changes[0].snapshot.joint_count = 1;
  changes[0].snapshot.joints[0] =
      (VkrSceneJointConfig){.authored_id = 11,
                            .target_source = target_source,
                            .type = VKR_PHYSICS_JOINT_FIXED,
                            .axis_a = {1, 0, 0},
                            .axis_b = {1, 0, 0},
                            .normal_a = {0, 1, 0},
                            .normal_b = {0, 1, 0},
                            .enabled = true_v};
  changes[1].snapshot = vkr_scene_physics_default();
  undo_before = state.undo_cursor;
  assert(vkr_scene_edit_apply_physics_batch(&state, &scene, changes, 2));
  assert(state.undo_cursor == undo_before + 1u);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  VkrScenePhysicsSnapshot target_read;
  assert(vkr_scene_physics_read(&scene, target, &target_read) &&
         !target_read.present);
  assert(vkr_scene_physics_read(&scene, parent, &target_read) &&
         target_read.joint_count == 0);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_scene_physics_read(&scene, target, &target_read) &&
         target_read.present);
  state.sidecar_conflict = false_v;
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  char advanced_saved[16384];
  file = file_fopen(path, "rb");
  assert(file);
  const size_t advanced_size =
      fread(advanced_saved, 1, sizeof(advanced_saved) - 1u, file);
  assert(advanced_size && advanced_size < sizeof(advanced_saved) - 1u &&
         feof(file));
  fclose(file);
  advanced_saved[advanced_size] = 0;
  assert(strstr(advanced_saved, "\"version\":3"));
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  vkr_scene_collision_layers_read(&scene, &settings_read);
  assert(!strcmp(settings_read.names[1], "Actors"));
  assert(vkr_scene_physics_read(&scene, parent, &target_read) &&
         target_read.joint_count == 1);
  assert(target_read.joints[0].target_source.gltf_node_index == 2);
  assert(target_read.colliders[0].scale.x == 1);
  edit_test_replace(path, advanced_saved, advanced_size, "\"matrix\":[65533",
                    "\"matrix\":[65535");
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_collision_layers_effective_mask(&scene, 1, UINT16_MAX) ==
         65533);
  assert(vkr_scene_physics_read(&scene, parent, &target_read) &&
         target_read.joint_count == 1);
  /* Missing cooked data is an acquisition failure before any old shape is
     released. No library success mock can detect this lost-reference defect. */
  assert(vkr_scene_edit_read(&scene, parent, &physics));
  physics.fields = VKR_SCENE_EDIT_PHYSICS;
  physics.physics.colliders[0].shape = VKR_PHYSICS_CONVEX_HULL;
  MemCopy(physics.physics.colliders[0].asset_path,
          "missing_collision_fixture.vkc",
          sizeof("missing_collision_fixture.vkc"));
  collider = vkr_scene_physics_collider_entity(&scene, parent, 42);
  assert(!vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  /* Unresolved endpoints remain authored suspended links, so removing a
     referenced owner does not silently erase a joint. Undo restores its
     previously resolved source identity. */
  physics.physics = target_read;
  physics.physics.joints[0].target_source.source_fingerprint = UINT64_MAX;
  assert(vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(vkr_scene_physics_read(&scene, parent, &target_read));
  assert(target_read.joints[0].target_source.source_fingerprint == UINT64_MAX);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_physics_read(&scene, parent, &target_read));
  assert(target_read.joints[0].target_source.source_fingerprint ==
         target_source.source_fingerprint);
  /* A self-joint fails only after resolving the staged graph. The old native
     compound must survive this later failure unchanged. */
  physics.physics = target_read;
  physics.physics.joints[0].target_source =
      *(const SceneSourceIdentity *)vkr_entity_get_component(
          scene.world, parent, scene.comp_source_identity);
  collider = vkr_scene_physics_collider_entity(&scene, parent, 42);
  assert(!vkr_scene_edit_apply(&state, &scene, parent, &physics));
  assert(vkr_scene_physics_collider_entity(&scene, parent, 42).u64 ==
         collider.u64);
  FilePath saved_path = {.path = file_path, .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&saved_path) == FILE_ERROR_NONE);
  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("--- Scene Edit Tests Completed ---\n");
  return true_v;
}
