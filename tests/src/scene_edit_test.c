#include "scene_edit_test.h"

#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_types.h"
#include <assert.h>
#include <math.h>
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

static uint32_t edit_test_alive_named(const VkrScene *scene, const char *name,
                                      VkrEntityId *out) {
  uint32_t count = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const String8 entity_name = vkr_scene_get_name(scene, entity);
    if (vkr_scene_entity_alive(scene, entity) &&
        entity_name.length == strlen(name) &&
        MemCompare(entity_name.str, name, entity_name.length) == 0) {
      *out = entity;
      count++;
    }
  }
  return count;
}

/* A document entity with source identity `index` and document id `id`. */
static VkrEntityId edit_test_document_entity(VkrScene *scene, uint32_t index,
                                             const char *name) {
  VkrEntityId entity = edit_test_entity(scene, UINT32_MAX, name);
  SceneSourceIdentity identity = {.scene_entity_index = index,
                                  .gltf_node_index = UINT32_MAX,
                                  .source_fingerprint = 0x1122334455667788ull};
  assert(vkr_scene_set_source_identity(scene, entity, &identity));
  return entity;
}

static void edit_test_document_ids_set(VkrScene *scene, const char *first,
                                       const char *second) {
  VkrEntityRef *ids = vkr_scene_document_ids_reserve(scene, 2u);
  assert(ids);
  assert(vkr_entity_ref_parse(first, strlen(first), &ids[0]));
  assert(vkr_entity_ref_parse(second, strlen(second), &ids[1]));
}

/* Document-stable ids (ADR-076). The oracle is which entity an overlay edit
   lands on after the document swaps its two entities: binding by saved index
   would rename the wrong one; an id the document lost fails the load. */
static void edit_test_document_ids(VkrAllocator *allocator, String8 path) {
  static const char first[] = "1b4e28ba-2fa1-11d2-883f-0016d3cca427";
  static const char second[] = "6FA459EA-EE8A-3CA4-894E-DB77E160355E";
  static const char other[] = "00000000-0000-4000-8000-000000000001";
  VkrScene saved;
  assert(vkr_scene_init(&saved, allocator, 21, 8, NULL));
  (void)edit_test_document_entity(&saved, 0u, "first");
  VkrEntityId renamed = edit_test_document_entity(&saved, 1u, "second");
  edit_test_document_ids_set(&saved, first, second);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, allocator, 1);
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(&saved, renamed, &values));
  snprintf(values.name, sizeof(values.name), "edited");
  assert(vkr_scene_edit_apply(&state, &saved, renamed, &values));
  assert(vkr_scene_edit_save(&state, &saved, path));
  vkr_scene_shutdown(&saved, NULL);

  VkrScene swapped;
  assert(vkr_scene_init(&swapped, allocator, 21, 8, NULL));
  VkrEntityId moved = edit_test_document_entity(&swapped, 0u, "second");
  VkrEntityId kept = edit_test_document_entity(&swapped, 1u, "first");
  edit_test_document_ids_set(&swapped, second, first);
  vkr_scene_edit_reset(&state, allocator, 1);
  assert(vkr_scene_edit_load(&state, &swapped, path));
  /* A document entity's reference id is its document's id. */
  VkrEntityRef ref;
  VkrEntityRef expected;
  assert(vkr_entity_ref_parse(second, strlen(second), &expected));
  assert(vkr_scene_entity_ref(&swapped, moved, &ref) &&
         MemCompare(&ref, &expected, sizeof(ref)) == 0);
  assert(vkr_scene_find_entity_ref(&swapped, &expected).u64 == moved.u64);
  const String8 moved_name = vkr_scene_get_name(&swapped, moved);
  const String8 kept_name = vkr_scene_get_name(&swapped, kept);
  assert(moved_name.length == 6 &&
         MemCompare(moved_name.str, "edited", 6) == 0);
  assert(kept_name.length == 5 && MemCompare(kept_name.str, "first", 5) == 0);
  vkr_scene_shutdown(&swapped, NULL);

  VkrScene lost;
  assert(vkr_scene_init(&lost, allocator, 21, 8, NULL));
  (void)edit_test_document_entity(&lost, 0u, "first");
  (void)edit_test_document_entity(&lost, 1u, "other");
  edit_test_document_ids_set(&lost, first, other);
  vkr_scene_edit_reset(&state, allocator, 1);
  assert(!vkr_scene_edit_load(&state, &lost, path));
  vkr_scene_shutdown(&lost, NULL);
  vkr_scene_edit_reset(&state, allocator, 0);
}

/* The scene's texture limit is an undoable scene setting that survives a
   sidecar save and load, and the peek reads it before the scene loads. A file
   without the key, the earlier form, keeps full resolution; a limit that is
   not a supported power of two rejects the edit and the file. */
static void edit_test_texture_limit(VkrAllocator *allocator, String8 path,
                                    const char *cpath) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 23, 8, NULL));
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, allocator, 1);
  VkrSceneSettings settings = scene.settings;
  settings.texture_max_extent = 3000u;
  assert(!vkr_scene_edit_apply_scene_settings(&state, &scene, &settings));
  assert(scene.settings.texture_max_extent == 0u);
  settings.texture_max_extent = 2048u;
  assert(vkr_scene_edit_apply_scene_settings(&state, &scene, &settings));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(scene.settings.texture_max_extent == 0u);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(scene.settings.texture_max_extent == 2048u);

  assert(vkr_scene_edit_save(&state, &scene, path));
  char saved[4096];
  FILE *file = file_fopen(cpath, "rb");
  assert(file);
  const size_t saved_size = fread(saved, 1, sizeof(saved) - 1u, file);
  fclose(file);
  saved[saved_size] = 0;
  VkrSceneSettings peeked = {0};
  assert(vkr_scene_edit_peek_settings(allocator, path, &peeked));
  assert(peeked.texture_max_extent == 2048u && peeked.inherit_world);
  scene.settings.texture_max_extent = 0u;
  vkr_scene_edit_reset(&state, allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, path));
  assert(scene.settings.texture_max_extent == 2048u);

  edit_test_replace(cpath, saved, saved_size, "2048", "3000");
  assert(!vkr_scene_edit_load(&state, &scene, path));
  assert(vkr_scene_edit_peek_settings(allocator, path, &peeked));
  assert(peeked.texture_max_extent == 0u);

  scene.settings.texture_max_extent = 0u;
  vkr_scene_edit_reset(&state, allocator, 1);
  assert(vkr_scene_edit_save(&state, &scene, path));
  file = file_fopen(cpath, "rb");
  assert(file);
  const size_t earlier_size = fread(saved, 1, sizeof(saved) - 1u, file);
  fclose(file);
  saved[earlier_size] = 0;
  assert(!strstr(saved, "texture_max_extent"));
  scene.settings.texture_max_extent = 1024u;
  assert(vkr_scene_edit_load(&state, &scene, path));
  assert(scene.settings.texture_max_extent == 0u);

  vkr_scene_edit_reset(&state, allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
}

/* Structural journal (ADR-076). Independent oracles: component bytes, entity
   liveness, parent links and world positions, checked after undo and redo and
   after an overlay save and reload into a freshly built document scene. A
   missing id remap shows as a redo that edits a destroyed entity. */
/* Names `entity` through the journal; one ENTITY entry. */
static void edit_test_rename(VkrSceneEditState *state, VkrScene *scene,
                             VkrEntityId entity, const char *name) {
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(scene, entity, &values));
  values.fields = VKR_SCENE_EDIT_NAME;
  snprintf(values.name, sizeof(values.name), "%s", name);
  assert(vkr_scene_edit_apply(state, scene, entity, &values));
}

static VkrEntityId edit_test_create(VkrSceneEditState *state, VkrScene *scene,
                                    VkrEntityId parent, const char *name) {
  VkrSceneEditValues values = {.fields = VKR_SCENE_EDIT_NAME |
                                         VKR_SCENE_EDIT_TRANSFORM};
  snprintf(values.name, sizeof(values.name), "%s", name);
  values.rotation = vkr_quat_identity();
  values.scale = vec3_one();
  const VkrEntityId entity =
      vkr_scene_edit_create(state, scene, parent, &values);
  assert(entity.u64);
  return entity;
}

static bool8_t edit_test_named(const VkrScene *scene, VkrEntityId entity,
                               const char *name) {
  const String8 current = vkr_scene_get_name(scene, entity);
  return current.length == strlen(name) &&
         MemCompare(current.str, name, current.length) == 0;
}

/* Journal groups: one undo step, rollback, out-of-order revert and its
   refusal, the group limit and whole-group eviction. */
static void edit_test_groups(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(64), GB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 0, 8, &error));
  VkrEntityId other = edit_test_entity(&scene, 0, "other");
  vkr_scene_update(&scene, 0.0);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, &allocator, 1);

  /* A group of a create, a child create and a rename undoes and redoes as
     one step. */
  const uint64_t group = vkr_scene_edit_group_begin(&state);
  assert(group && !vkr_scene_edit_group_begin(&state));
  VkrEntityId room =
      edit_test_create(&state, &scene, VKR_ENTITY_ID_INVALID, "room");
  VkrEntityId wall = edit_test_create(&state, &scene, room, "wall");
  edit_test_rename(&state, &scene, wall, "north wall");
  assert(!vkr_scene_edit_undo(&state, &scene, false_v));
  vkr_scene_edit_group_end(&state);
  assert(state.undo_count == 3u && vkr_scene_edit_group_present(&state, group));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(state.undo_cursor == 0u);
  assert(!vkr_scene_entity_alive(&scene, room) &&
         !vkr_scene_entity_alive(&scene, wall));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(state.undo_cursor == 3u);
  VkrEntityId found = VKR_ENTITY_ID_INVALID;
  assert(edit_test_alive_named(&scene, "room", &room) == 1u);
  assert(edit_test_alive_named(&scene, "north wall", &wall) == 1u);
  assert(vkr_scene_get_transform(&scene, wall)->parent.u64 == room.u64);

  /* Rollback undoes and drops the open group's entries only. */
  assert(vkr_scene_edit_group_begin(&state));
  VkrEntityId extra =
      edit_test_create(&state, &scene, VKR_ENTITY_ID_INVALID, "extra");
  edit_test_rename(&state, &scene, other, "renamed other");
  assert(vkr_scene_edit_group_rollback(&state, &scene));
  assert(!state.group_open && state.undo_count == 3u &&
         state.undo_cursor == 3u);
  assert(!vkr_scene_entity_alive(&scene, extra));
  assert(edit_test_named(&scene, other, "other"));

  /* A later unrelated edit does not stop an out-of-order revert; the group
     leaves the journal and the later edit still undoes. */
  edit_test_rename(&state, &scene, other, "later");
  assert(vkr_scene_edit_group_revert(&state, &scene, group, NULL));
  assert(!vkr_scene_edit_group_present(&state, group));
  assert(state.undo_count == 1u && state.undo_cursor == 1u);
  assert(edit_test_alive_named(&scene, "room", &found) == 0u);
  assert(edit_test_alive_named(&scene, "north wall", &found) == 0u);
  assert(edit_test_named(&scene, other, "later"));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(edit_test_named(&scene, other, "other"));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));

  /* A later child of a grouped entity refuses the revert and names it. */
  const uint64_t parent_group = vkr_scene_edit_group_begin(&state);
  VkrEntityId hall =
      edit_test_create(&state, &scene, VKR_ENTITY_ID_INVALID, "hall");
  vkr_scene_edit_group_end(&state);
  VkrEntityId lamp = edit_test_create(&state, &scene, hall, "lamp");
  VkrEntityId conflict = VKR_ENTITY_ID_INVALID;
  assert(!vkr_scene_edit_group_revert(&state, &scene, parent_group, &conflict));
  assert(conflict.u64 == lamp.u64 || conflict.u64 == hall.u64);
  assert(vkr_scene_entity_alive(&scene, hall));
  /* Once the child's creation is undone, the group is the newest applied
     step and reverts; the redo entry above it is dropped. */
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_edit_group_revert(&state, &scene, parent_group, NULL));
  assert(!vkr_scene_entity_alive(&scene, hall));
  assert(state.undo_cursor == state.undo_count);

  /* A brush leaves with its faces in one step, and undo returns them under
     the restored brush. */
  VkrSceneEditValues brush_values = {.fields = VKR_SCENE_EDIT_NAME |
                                               VKR_SCENE_EDIT_TRANSFORM |
                                               VKR_SCENE_EDIT_COMPONENT};
  snprintf(brush_values.name, sizeof(brush_values.name), "brush");
  brush_values.rotation = vkr_quat_identity();
  brush_values.scale = vec3_one();
  brush_values.component_type = &vkr_scene_brush_type;
  vkr_type_defaults(&vkr_scene_brush_type, brush_values.component);
  VkrEntityId brush = vkr_scene_edit_create(
      &state, &scene, VKR_ENTITY_ID_INVALID, &brush_values);
  assert(brush.u64);
  for (uint32_t i = 0; i < 2u; ++i) {
    VkrSceneEditValues face = brush_values;
    snprintf(face.name, sizeof(face.name), "face %u", i);
    face.component_type = &vkr_scene_brush_face_type;
    vkr_type_defaults(&vkr_scene_brush_face_type, face.component);
    assert(vkr_scene_edit_create(&state, &scene, brush, &face).u64);
  }
  const char *refusal = NULL;
  assert(vkr_scene_edit_can_delete(&scene, brush, &refusal));
  const uint32_t before_delete = state.undo_count;
  assert(vkr_scene_edit_delete(&state, &scene, brush));
  assert(!vkr_scene_entity_alive(&scene, brush));
  assert(edit_test_alive_named(&scene, "face 0", &found) == 0u);
  assert(state.undo_count == before_delete + 3u);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(edit_test_alive_named(&scene, "brush", &brush) == 1u);
  assert(edit_test_alive_named(&scene, "face 1", &found) == 1u);
  assert(vkr_scene_get_transform(&scene, found)->parent.u64 == brush.u64);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(!vkr_scene_entity_alive(&scene, brush));

  /* A group holds at most VKR_SCENE_EDIT_GROUP_MAX entries. */
  vkr_scene_edit_reset(&state, &allocator, 1);
  assert(vkr_scene_edit_group_begin(&state));
  for (uint32_t i = 0; i < VKR_SCENE_EDIT_GROUP_MAX; ++i) {
    edit_test_rename(&state, &scene, other, (i & 1u) ? "odd" : "even");
  }
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(&scene, other, &values));
  values.fields = VKR_SCENE_EDIT_NAME;
  snprintf(values.name, sizeof(values.name), "over");
  assert(!vkr_scene_edit_apply(&state, &scene, other, &values));
  assert(vkr_scene_edit_group_rollback(&state, &scene));
  assert(state.undo_count == 0u && edit_test_named(&scene, other, "later"));

  /* Eviction at capacity drops the oldest group whole. */
  const uint64_t oldest = vkr_scene_edit_group_begin(&state);
  edit_test_rename(&state, &scene, other, "a");
  edit_test_rename(&state, &scene, other, "b");
  vkr_scene_edit_group_end(&state);
  while (state.undo_count < VKR_SCENE_EDIT_UNDO_CAPACITY) {
    edit_test_rename(&state, &scene, other,
                     (state.undo_count & 1u) ? "c" : "d");
  }
  assert(vkr_scene_edit_group_present(&state, oldest));
  edit_test_rename(&state, &scene, other, "e");
  assert(!vkr_scene_edit_group_present(&state, oldest));
  assert(state.undo_count == VKR_SCENE_EDIT_UNDO_CAPACITY - 1u);
  (void)found;

  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
}

/* Writes an overlay of `count` created roots named "n<index>". */
/* vkr_scene_brush_faces answers from the scene's child index when it is
   valid. The index lists children in the order they joined and can keep one
   whose parent was written directly, as a restored object's is; the answer
   must still match the full scan: the same faces, by entity index. */
static void edit_test_brush_faces(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(16), MB(256), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 0, 8, &error));
  const VkrEntityId brush = edit_test_entity(&scene, 0, "brush");
  const VkrEntityId other = edit_test_entity(&scene, 1, "other");
  SceneBrushFace face_values;
  vkr_type_defaults(&vkr_scene_brush_face_type, &face_values);
  VkrEntityId faces[4];
  for (uint32_t i = 0; i < ArrayCount(faces); ++i) {
    faces[i] = edit_test_entity(&scene, 2u + i, "face");
    assert(vkr_scene_set_typed(&scene, faces[i], &vkr_scene_brush_face_type,
                               &face_values));
    vkr_scene_set_parent(&scene, faces[i], brush);
  }
  vkr_scene_update(&scene, 0.0);
  assert(scene.child_index_valid);

  /* faces[0] rejoins last; faces[2]'s parent changes behind the index. */
  vkr_scene_set_parent(&scene, faces[0], other);
  vkr_scene_set_parent(&scene, faces[0], brush);
  vkr_scene_get_transform(&scene, faces[2])->parent = other;
  assert(scene.child_index_valid);
  VkrEntityId indexed[4] = {0};
  const uint32_t indexed_count =
      vkr_scene_brush_faces(&scene, brush, indexed, ArrayCount(indexed));
  VkrEntityId first_two[2] = {0};
  assert(vkr_scene_brush_faces(&scene, brush, first_two, 2u) == 3u);

  scene.child_index_valid = false_v;
  VkrEntityId scanned[4] = {0};
  const uint32_t scanned_count =
      vkr_scene_brush_faces(&scene, brush, scanned, ArrayCount(scanned));
  scene.child_index_valid = true_v;
  assert(scanned_count == 3u && indexed_count == scanned_count);
  for (uint32_t i = 0; i < scanned_count; ++i) {
    assert(indexed[i].u64 == scanned[i].u64);
  }
  assert(scanned[0].u64 == faces[0].u64 && scanned[1].u64 == faces[1].u64 &&
         scanned[2].u64 == faces[3].u64);
  assert(first_two[0].u64 == faces[0].u64 && first_two[1].u64 == faces[1].u64);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
}

/* The editor's Hide and Isolate: a hidden parent hides what is under it,
   isolate draws only the isolated objects and what is under them, and the
   runtime's per-frame apply of an unchanged set, in any order, resyncs
   nothing. */
static void edit_test_editor_hidden(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(16), MB(256), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 0, 8, &error));
  const VkrEntityId room = edit_test_entity(&scene, 0, "room");
  const VkrEntityId wall = edit_test_entity(&scene, 1, "wall");
  const VkrEntityId crate = edit_test_entity(&scene, 2, "crate");
  vkr_scene_set_parent(&scene, wall, room);
  vkr_scene_update(&scene, 0.0);
  scene.render_full_sync_needed = false_v;

  const VkrEntityId hide_room[1] = {room};
  assert(vkr_scene_set_editor_hidden(&scene, hide_room, 1u, false_v));
  assert(scene.render_full_sync_needed);
  assert(!vkr_scene_editor_shown(&scene, room) &&
         !vkr_scene_editor_shown(&scene, wall) &&
         vkr_scene_editor_shown(&scene, crate));

  /* Ids rise with creation, so `crate, room` is out of order. */
  const VkrEntityId in_order[2] = {room, crate};
  assert(vkr_scene_set_editor_hidden(&scene, in_order, 2u, true_v));
  scene.render_full_sync_needed = false_v;
  const VkrEntityId reordered[2] = {crate, room};
  assert(reordered[0].u64 > reordered[1].u64);
  assert(vkr_scene_set_editor_hidden(&scene, reordered, 2u, true_v));
  assert(!scene.render_full_sync_needed);
  assert(vkr_scene_editor_shown(&scene, wall) &&
         vkr_scene_editor_shown(&scene, crate));
  const VkrEntityId wall_only[1] = {wall};
  assert(vkr_scene_set_editor_hidden(&scene, wall_only, 1u, true_v));
  assert(vkr_scene_editor_shown(&scene, wall) &&
         !vkr_scene_editor_shown(&scene, room) &&
         !vkr_scene_editor_shown(&scene, crate));

  assert(vkr_scene_set_editor_hidden(&scene, NULL, 0u, true_v));
  assert(vkr_scene_editor_shown(&scene, room) && !scene.editor_isolate);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
}

static void edit_test_created_file(const char *path, uint32_t count) {
  FILE *file = file_fopen(path, "wb");
  assert(file);
  fputs("{\"version\":5,\"overrides\":[],\"created\":[", file);
  for (uint32_t i = 0; i < count; ++i) {
    fprintf(file,
            "%s{\"id\":%u,\"parent\":null,\"fields\":3,\"name\":\"n%u\","
            "\"position\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]}",
            i ? "," : "", i + 1u, i);
  }
  /* Version 3 and later overlays carry the scene's collision layers; these
     are the defaults a save writes. */
  fputs("],\"collision_settings\":{\"version\":1,\"names\":[", file);
  for (uint32_t i = 0; i < 16u; ++i) {
    fprintf(file, "%s\"Layer %u\"", i ? "," : "", i + 1u);
  }
  fputs("],\"matrix\":[", file);
  for (uint32_t i = 0; i < 16u; ++i) {
    fprintf(file, "%s65535", i ? "," : "");
  }
  fputs("],\"presets\":[{\"name\":\"Default\",\"membership\":1,"
        "\"mask\":65535,\"sensor\":false},{\"name\":\"Sensor\","
        "\"membership\":2,\"mask\":65535,\"sensor\":true},{\"name\":"
        "\"No collision\",\"membership\":1,\"mask\":0,\"sensor\":false}]}}",
        file);
  fclose(file);
}

/* A level-sized build (ADR-084). Independent oracles: entity liveness by name,
   the journal's own entries summed against its byte count, and the status a
   refused load or save names. A journal whose allocator fills used to refuse
   every further edit, and an overlay past 1,024 created objects saved but
   never loaded. */
static void edit_test_large(void) {
  VkrDMemory scene_memory;
  assert(vkr_dmemory_create(MB(16), MB(512), &scene_memory));
  VkrAllocator scene_allocator = {.ctx = &scene_memory};
  vkr_dmemory_allocator_create(&scene_allocator);
  /* 1,500 creations journal about 17 MiB, eight times this pool. */
  VkrDMemory journal_memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &journal_memory));
  VkrAllocator journal_allocator = {.ctx = &journal_memory};
  vkr_dmemory_allocator_create(&journal_allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &scene_allocator, 0, 8, &error));
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, &journal_allocator, 1);
  const uint32_t built = 1500u;
  char name[32];
  for (uint32_t i = 0; i < built; ++i) {
    snprintf(name, sizeof(name), "n%u", i);
    (void)edit_test_create(&state, &scene, VKR_ENTITY_ID_INVALID, name);
  }
  uint64_t kept = 0u;
  for (uint32_t i = 0; i < state.undo_count; ++i) {
    kept += state.undo[i].payload_size;
  }
  assert(state.undo_count < built && state.payload_bytes == kept);
  assert(state.created_count == built);
  /* The newest step still undoes and redoes. */
  VkrEntityId found = VKR_ENTITY_ID_INVALID;
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(edit_test_alive_named(&scene, "n1499", &found) == 0u);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(edit_test_alive_named(&scene, "n1499", &found) == 1u);

  /* Every object saves and loads back. */
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/scene_large_%u.json",
           vkr_platform_get_process_id());
  const String8 file_path = string8_create((uint8_t *)path, strlen(path));
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  vkr_scene_edit_reset(&state, &scene_allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  assert(vkr_scene_init(&scene, &scene_allocator, 0, 8, &error));
  vkr_scene_edit_reset(&state, &scene_allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(state.created_count == built);
  assert(edit_test_alive_named(&scene, "n0", &found) == 1u &&
         edit_test_alive_named(&scene, "n1499", &found) == 1u);
  vkr_scene_edit_reset(&state, &scene_allocator, 0);
  vkr_scene_shutdown(&scene, NULL);

  /* A file at the limit loads; one more object refuses the save, and a file
     past the limit names the limit instead of a malformed schema. */
  edit_test_created_file(path, VKR_SCENE_EDIT_CREATED_MAX);
  assert(vkr_scene_init(&scene, &scene_allocator, 0, 8, &error));
  vkr_scene_edit_reset(&state, &scene_allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(state.created_count == VKR_SCENE_EDIT_CREATED_MAX);
  (void)edit_test_create(&state, &scene, VKR_ENTITY_ID_INVALID, "extra");
  assert(!vkr_scene_edit_save(&state, &scene, file_path));
  assert(strstr(state.status, "Save blocked"));
  vkr_scene_edit_reset(&state, &scene_allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  edit_test_created_file(path, VKR_SCENE_EDIT_CREATED_MAX + 1u);
  assert(vkr_scene_init(&scene, &scene_allocator, 0, 8, &error));
  vkr_scene_edit_reset(&state, &scene_allocator, 1);
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(strstr(state.status, "creates more than") &&
         state.created_count == 0u);

  FilePath saved_path = {.path = file_path, .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&saved_path) == FILE_ERROR_NONE);
  vkr_scene_edit_reset(&state, &scene_allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&journal_allocator);
  vkr_dmemory_allocator_destroy(&scene_allocator);
}

static void edit_test_structure(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 0, 8, &error));
  VkrEntityId parent = edit_test_entity(&scene, 0, "parent");
  VkrEntityId child = edit_test_entity(&scene, 1, "child");
  VkrEntityId lamp = edit_test_entity(&scene, 2, "lamp");
  vkr_scene_set_position(&scene, parent, vec3_new(10, 0, 0));
  vkr_scene_set_position(&scene, child, vec3_new(2, 3, 4));
  vkr_scene_set_parent(&scene, child, parent);
  assert(vkr_scene_set_point_light(
      &scene, lamp,
      &(ScenePointLight){.color = vec3_one(),
                         .intensity = 4.0f,
                         .constant = 1.0f,
                         .range = 6.0f,
                         .inner_cone_angle = 0.35f,
                         .outer_cone_angle = 0.6f,
                         .source_radius = 0.05f,
                         .enabled = true_v,
                         .mobility = VKR_LIGHT_MOBILITY_DYNAMIC,
                         .light_group = "street"}));
  vkr_scene_update(&scene, 0.0);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, &allocator, 1);

  /* An emitter radius above the limit cannot be published. */
  VkrSceneEditValues oversized;
  assert(vkr_scene_edit_read(&scene, lamp, &oversized));
  oversized.fields = VKR_SCENE_EDIT_POINT_LIGHT;
  oversized.point_light.source_radius =
      2.0f * VKR_POINT_LIGHT_SOURCE_RADIUS_MAX;
  assert(!vkr_scene_edit_apply(&state, &scene, lamp, &oversized));
  assert(vkr_scene_get_point_light(&scene, lamp)->source_radius == 0.05f);

  /* Add and remove a component; undo restores the removed bytes exactly. */
  VkrFogSettings fog;
  vkr_type_defaults(&vkr_scene_fog_type, &fog);
  fog.density = 0.25f;
  assert(vkr_scene_edit_add_component(&state, &scene, child,
                                      &vkr_scene_fog_type, &fog));
  assert(!vkr_scene_edit_add_component(&state, &scene, child,
                                       &vkr_scene_fog_type, NULL));
  assert(!vkr_scene_edit_add_component(&state, &scene, child,
                                       &vkr_scene_environment_type, NULL));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(!vkr_scene_get_typed(&scene, child, &vkr_scene_fog_type));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  const VkrFogSettings *stored =
      vkr_scene_get_typed(&scene, child, &vkr_scene_fog_type);
  assert(stored && stored->density == 0.25f);
  assert(vkr_scene_edit_remove_component(&state, &scene, child,
                                         &vkr_scene_fog_type));
  assert(!vkr_scene_get_typed(&scene, child, &vkr_scene_fog_type));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  stored = vkr_scene_get_typed(&scene, child, &vkr_scene_fog_type);
  assert(stored && stored->density == 0.25f);

  /* Create under a parent, edit, undo both, redo both: the recreated entity
     receives the later edit. */
  VkrSceneEditValues values = {.fields = VKR_SCENE_EDIT_NAME |
                                         VKR_SCENE_EDIT_TRANSFORM |
                                         VKR_SCENE_EDIT_POINT_LIGHT};
  snprintf(values.name, sizeof(values.name), "created lamp");
  values.position = vec3_new(0, 1, 0);
  values.rotation = vkr_quat_identity();
  values.scale = vec3_one();
  values.point_light = *vkr_scene_get_point_light(&scene, lamp);
  VkrEntityId created = vkr_scene_edit_create(&state, &scene, parent, &values);
  if (!created.u64)
    printf("create failed: %s\n", state.status);
  assert(created.u64 && state.created_count == 1u);
  assert(vkr_scene_get_transform(&scene, created)->parent.u64 == parent.u64);
  /* A created entity gets its own id; a document without ids gives none. */
  VkrEntityRef created_ref;
  VkrEntityRef ref;
  assert(vkr_scene_entity_ref(&scene, created, &created_ref) &&
         !vkr_entity_ref_empty(&created_ref));
  assert(!vkr_scene_entity_ref(&scene, lamp, &ref));
  VkrSceneEditValues rename;
  assert(vkr_scene_edit_read(&scene, created, &rename));
  rename.fields = VKR_SCENE_EDIT_NAME;
  snprintf(rename.name, sizeof(rename.name), "renamed lamp");
  assert(vkr_scene_edit_apply(&state, &scene, created, &rename));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(!vkr_scene_entity_alive(&scene, created));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  VkrEntityId found = VKR_ENTITY_ID_INVALID;
  assert(edit_test_alive_named(&scene, "renamed lamp", &found) == 1u);
  assert(found.u64 != created.u64 && state.created[0].entity.u64 == found.u64);
  assert(vkr_scene_get_transform(&scene, found)->parent.u64 == parent.u64);
  assert(vkr_scene_get_point_light(&scene, found)->intensity == 4.0f);
  /* Redo recreates it under the same id, so references to it still hold. */
  assert(vkr_scene_find_entity_ref(&scene, &created_ref).u64 == found.u64);
  created = found;

  /* Delete refuses a parent, then deletes and restores a document light. */
  const char *reason = NULL;
  assert(!vkr_scene_edit_can_delete(&scene, parent, &reason) && reason);
  assert(!vkr_scene_edit_delete(&state, &scene, parent));
  assert(vkr_scene_edit_delete(&state, &scene, lamp));
  assert(!vkr_scene_entity_alive(&scene, lamp) && state.deleted_count == 1u);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(edit_test_alive_named(&scene, "lamp", &found) == 1u);
  assert(state.deleted_count == 0u);
  assert(vkr_scene_get_point_light(&scene, found)->range == 6.0f);
  const SceneSourceIdentity *identity =
      vkr_entity_get_component(scene.world, found, scene.comp_source_identity);
  assert(identity && identity->gltf_node_index == 2u);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(!vkr_scene_entity_alive(&scene, found) && state.deleted_count == 1u);

  /* Reparent to the root keeps the world position; undo restores the link
     and the local position. Cycles are refused. */
  assert(!vkr_scene_edit_reparent(&state, &scene, parent, child));
  assert(vkr_scene_edit_reparent(&state, &scene, child, VKR_ENTITY_ID_INVALID));
  SceneTransform *transform = vkr_scene_get_transform(&scene, child);
  assert(!transform->parent.u64 &&
         fabsf(transform->position.x - 12.0f) < 1e-4f);
  vkr_scene_update(&scene, 0.0);
  assert(fabsf(transform->world.elements[12] - 12.0f) < 1e-4f);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  transform = vkr_scene_get_transform(&scene, child);
  assert(transform->parent.u64 == parent.u64 && transform->position.x == 2.0f);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));

  /* A created object's physics body is saved with it. */
  VkrSceneEditValues body = {.fields = VKR_SCENE_EDIT_PHYSICS};
  body.physics = vkr_scene_physics_default();
  body.physics.present = true_v;
  body.physics.collider_count = 1;
  body.physics.colliders[0] =
      (VkrSceneColliderConfig){.authored_id = 7,
                               .shape = VKR_PHYSICS_BOX,
                               .scale = {1, 1, 1},
                               .rotation = vkr_quat_identity(),
                               .half_extent = {0.5f, 0.25f, 0.5f},
                               .radius = 0.5f,
                               .half_height = 0.5f,
                               .enabled = true_v};
  assert(vkr_scene_edit_apply(&state, &scene, created, &body));

  /* Save, rebuild the document scene and reload: the created entity with its
     body, the added component, the deletion and the new root all return. */
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/scene_structure_%u.json",
           vkr_platform_get_process_id());
  const String8 file_path = string8_create((uint8_t *)path, strlen(path));
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);

  assert(vkr_scene_init(&scene, &allocator, 0, 8, &error));
  parent = edit_test_entity(&scene, 0, "parent");
  child = edit_test_entity(&scene, 1, "child");
  lamp = edit_test_entity(&scene, 2, "lamp");
  vkr_scene_set_parent(&scene, child, parent);
  vkr_scene_edit_reset(&state, &allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  assert(!vkr_scene_entity_alive(&scene, lamp) && state.deleted_count == 1u);
  stored = vkr_scene_get_typed(&scene, child, &vkr_scene_fog_type);
  assert(stored && stored->density == 0.25f);
  transform = vkr_scene_get_transform(&scene, child);
  assert(!transform->parent.u64 &&
         fabsf(transform->position.x - 12.0f) < 1e-4f);
  assert(edit_test_alive_named(&scene, "renamed lamp", &found) == 1u);
  assert(vkr_scene_get_transform(&scene, found)->parent.u64 == parent.u64);
  assert(vkr_scene_get_point_light(&scene, found)->intensity == 4.0f);
  assert(vkr_scene_get_point_light(&scene, found)->source_radius == 0.05f);
  assert(vkr_scene_get_point_light(&scene, found)->mobility ==
         VKR_LIGHT_MOBILITY_DYNAMIC);
  assert(strcmp(vkr_scene_get_point_light(&scene, found)->light_group,
                "street") == 0);
  assert(state.created_count == 1u && state.created[0].entity.u64 == found.u64);
  assert(vkr_scene_entity_ref(&scene, found, &ref) &&
         MemCompare(&ref, &created_ref, sizeof(ref)) == 0);
  VkrScenePhysicsSnapshot body_read;
  assert(vkr_scene_physics_read(&scene, found, &body_read) &&
         body_read.present && body_read.collider_count == 1 &&
         body_read.colliders[0].half_extent.y == 0.25f);

  /* Version 3 files cannot carry version 4 structure. */
  const char old_delete[] =
      "{\"version\":3,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":1,"
      "\"source_fingerprint\":\"1122334455667788\",\"deleted\":true}],"
      "\"collision_settings\":{}}";
  edit_test_write(path, old_delete, sizeof(old_delete) - 1);
  assert(!vkr_scene_edit_load(&state, &scene, file_path));
  assert(vkr_scene_entity_alive(&scene, child));

  edit_test_document_ids(&allocator, file_path);
  edit_test_texture_limit(&allocator, file_path, path);

  FilePath saved_path = {.path = file_path, .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&saved_path) == FILE_ERROR_NONE);
  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
}

/* A build cannot detect partial file acceptance or mutations before a later
   identity conflict. These literal files have independent scene-value oracles;
   the real scene allocator and serializer also exercise name ownership. */
/* The text of file `path`, or an empty string when it is missing. */
static size_t edit_test_read(const char *path, char *out, size_t capacity) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    out[0] = '\0';
    return 0u;
  }
  const size_t size = fread(out, 1, capacity - 1u, file);
  fclose(file);
  out[size] = '\0';
  return size;
}

static VkrEntityId edit_test_create_at(VkrSceneEditState *state,
                                       VkrScene *scene, VkrEntityId parent,
                                       const char *name, Vec3 position) {
  const VkrEntityId entity = edit_test_create(state, scene, parent, name);
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(scene, entity, &values));
  values.fields = VKR_SCENE_EDIT_TRANSFORM;
  values.position = position;
  assert(vkr_scene_edit_apply(state, scene, entity, &values));
  return entity;
}

/* World partition cells (ADR-086). Oracles: which document holds each
   object after a save, byte-identical documents for cells no edit reached,
   the journal and unsaved-edit rules that refuse an unload, and the objects
   and parent links a cell's load brings back. */
static void edit_test_partition(VkrAllocator *allocator) {
  char root[1024];
  char sidecar[1024];
  char near_path[1100];
  char far_path[1100];
  char index[1100];
  snprintf(root, sizeof(root), PROJECT_SOURCE_DIR "tests/tmp/cells_%u",
           vkr_platform_get_process_id());
  snprintf(sidecar, sizeof(sidecar),
           PROJECT_SOURCE_DIR "tests/tmp/cells_%u.editor.json",
           vkr_platform_get_process_id());
  snprintf(near_path, sizeof(near_path), "%s/0_0.json", root);
  snprintf(far_path, sizeof(far_path), "%s/2_-1.json", root);
  snprintf(index, sizeof(index), "%s/index.json", root);
  const String8 sidecar_path =
      string8_create_from_cstr((const uint8_t *)sidecar, strlen(sidecar));
  const String8 root_path =
      string8_create_from_cstr((const uint8_t *)root, strlen(root));

  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 23, 8, NULL));
  SceneWorldPartition partition;
  vkr_type_defaults(&vkr_scene_world_partition_type, &partition);
  assert(vkr_scene_create_typed_entity(&scene, string8_lit("Partition"),
                                       &vkr_scene_world_partition_type,
                                       &partition)
             .u64);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, allocator, 1);
  vkr_scene_edit_set_cells_root(&state, root_path);
  assert(vkr_scene_edit_cells_open(&state, &scene));
  const VkrEntityId crate = edit_test_create_at(
      &state, &scene, (VkrEntityId){0}, "crate", vec3_new(10, 0, 10));
  const VkrEntityId tower = edit_test_create_at(
      &state, &scene, (VkrEntityId){0}, "tower", vec3_new(300, 0, -20));
  (void)edit_test_create_at(&state, &scene, tower, "flag", vec3_new(0, 5, 0));
  const VkrEntityId keep = edit_test_create_at(&state, &scene, (VkrEntityId){0},
                                               "keep", vec3_new(900, 0, 900));
  SceneAlwaysLoaded always = {.enabled = true_v};
  assert(vkr_scene_edit_add_component(&state, &scene, keep,
                                      &vkr_scene_always_loaded_type, &always));
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));

  static char text[16384];
  assert(edit_test_read(sidecar, text, sizeof(text)));
  assert(strstr(text, "\"keep\"") && !strstr(text, "\"crate\"") &&
         !strstr(text, "\"tower\""));
  assert(edit_test_read(near_path, text, sizeof(text)));
  assert(strstr(text, "\"crate\"") && !strstr(text, "\"tower\""));
  assert(edit_test_read(far_path, text, sizeof(text)));
  assert(strstr(text, "\"tower\"") && strstr(text, "\"flag\""));
  static char far_before[16384];
  const size_t far_size =
      edit_test_read(far_path, far_before, sizeof(far_before));
  assert(edit_test_read(index, text, sizeof(text)));
  assert(strstr(text, "[0,0]") && strstr(text, "[2,-1]"));

  /* Moving the crate rewrites its cell alone. */
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(&scene, crate, &values));
  values.fields = VKR_SCENE_EDIT_TRANSFORM;
  values.position = vec3_new(20, 0, 10);
  assert(vkr_scene_edit_apply(&state, &scene, crate, &values));
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(far_path, text, sizeof(text)) == far_size &&
         MemCompare(text, far_before, far_size) == 0);
  assert(edit_test_read(near_path, text, sizeof(text)) && strstr(text, "20"));
  /* The journal names the tower, so its cell stays. */
  assert(!vkr_scene_edit_cell_unloadable(&state, &scene,
                                         (VkrScenePartitionCell){2, -1}));
  vkr_scene_edit_reset(&state, allocator, 1);
  vkr_scene_shutdown(&scene, NULL);

  /* A fresh scene loads the persistent layer, then cells on demand. */
  assert(vkr_scene_init(&scene, allocator, 24, 8, NULL));
  assert(vkr_scene_create_typed_entity(&scene, string8_lit("Partition"),
                                       &vkr_scene_world_partition_type,
                                       &partition)
             .u64);
  vkr_scene_edit_reset(&state, allocator, 1);
  vkr_scene_edit_set_cells_root(&state, root_path);
  assert(vkr_scene_edit_load(&state, &scene, sidecar_path));
  assert(vkr_scene_edit_cells_open(&state, &scene));
  VkrEntityId found;
  assert(edit_test_alive_named(&scene, "keep", &found) == 1u);
  assert(edit_test_alive_named(&scene, "tower", &found) == 0u);
  const VkrScenePartitionCell far_cell = {2, -1};
  const VkrScenePartitionCellRecord *record =
      vkr_scene_partition_cell(&scene, far_cell, false_v);
  assert(record && record->flags == VKR_SCENE_PARTITION_CELL_ON_DISK);
  assert(vkr_scene_edit_cell_load(&state, &scene, far_cell));
  VkrEntityId tower_loaded;
  VkrEntityId flag_loaded;
  assert(edit_test_alive_named(&scene, "tower", &tower_loaded) == 1u);
  assert(edit_test_alive_named(&scene, "flag", &flag_loaded) == 1u);
  assert(vkr_scene_get_transform(&scene, flag_loaded)->parent.u64 ==
         tower_loaded.u64);
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_edit_cell_unloadable(&state, &scene, far_cell));
  /* An unsaved edit elsewhere leaves a cell that holds what its document
     does free to go; one whose objects moved outside the journal, as a
     simulation moves them, stays. */
  VkrEntityId keep_loaded;
  assert(edit_test_alive_named(&scene, "keep", &keep_loaded) == 1u);
  edit_test_rename(&state, &scene, keep_loaded, "castle");
  assert(vkr_scene_edit_cell_unloadable(&state, &scene, far_cell));
  edit_test_rename(&state, &scene, keep_loaded, "fort");
  assert(vkr_scene_set_transform(&scene, tower_loaded, vec3_new(310, 0, -20),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update_transforms(&scene);
  assert(!vkr_scene_edit_cell_unloadable(&state, &scene, far_cell));
  assert(vkr_scene_set_transform(&scene, tower_loaded, vec3_new(300, 0, -20),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(far_path, text, sizeof(text)) == far_size &&
         MemCompare(text, far_before, far_size) == 0);
  assert(vkr_scene_edit_cell_unloadable(&state, &scene, far_cell));
  vkr_scene_edit_cell_unload(&state, &scene, far_cell);
  assert(edit_test_alive_named(&scene, "tower", &found) == 0u);
  assert(edit_test_alive_named(&scene, "flag", &found) == 0u);
  assert(!(vkr_scene_partition_cell(&scene, far_cell, false_v)->flags &
           VKR_SCENE_PARTITION_CELL_LOADED));
  /* Saving with the cell unloaded keeps its document. */
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(far_path, text, sizeof(text)) == far_size &&
         MemCompare(text, far_before, far_size) == 0);

  /* A cell loading while the origin is rebased (ADR-086) places its roots
     that far from their documents; children keep their parent offsets, and
     membership stays in document space. */
  SceneWorldPartition settings;
  assert(vkr_scene_partition_settings(&scene, &settings));
  scene.origin_offset = vec3_new(4096.0f, 0.0f, 0.0f);
  assert(vkr_scene_edit_cell_load(&state, &scene, far_cell));
  assert(edit_test_alive_named(&scene, "tower", &tower_loaded) == 1u);
  assert(edit_test_alive_named(&scene, "flag", &flag_loaded) == 1u);
  vkr_scene_update_transforms(&scene);
  const Vec3 tower_at =
      mat4_position(vkr_scene_get_transform(&scene, tower_loaded)->world);
  const Vec3 flag_at =
      mat4_position(vkr_scene_get_transform(&scene, flag_loaded)->world);
  assert(tower_at.x == 300.0f - 4096.0f && tower_at.z == -20.0f);
  assert(flag_at.x == tower_at.x && flag_at.y == tower_at.y + 5.0f);
  VkrScenePartitionCell at;
  assert(
      vkr_scene_partition_entity_cell(&scene, &settings, tower_loaded, &at) &&
      at.x == far_cell.x && at.z == far_cell.z);
  vkr_scene_edit_cell_unload(&state, &scene, far_cell);
  scene.origin_offset = vec3_zero();

  /* An unreadable document stays listed and untouched: streaming does not
     load it, and a save refuses an object that would land in it. */
  edit_test_write(far_path, "{", 1u);
  assert(!vkr_scene_edit_cell_load(&state, &scene, far_cell));
  assert(vkr_scene_partition_cell(&scene, far_cell, false_v)->flags &
         VKR_SCENE_PARTITION_CELL_UNREADABLE);
  const Vec3 source = vec3_new(300.0f, 0.0f, -20.0f);
  vkr_scene_set_stream_sources(&scene, &source, 1u);
  VkrScenePartitionPlan plan;
  vkr_scene_partition_plan(&scene, &settings, &plan);
  for (uint32_t i = 0; i < plan.load_count; ++i) {
    assert(plan.load[i].x != far_cell.x || plan.load[i].z != far_cell.z);
  }
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(index, text, sizeof(text)) && strstr(text, "[2,-1]"));
  assert(edit_test_read(far_path, text, sizeof(text)) == 1u && text[0] == '{');
  (void)edit_test_create_at(&state, &scene, (VkrEntityId){0}, "barrel",
                            vec3_new(320, 0, -30));
  vkr_scene_update_transforms(&scene);
  assert(!vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(far_path, text, sizeof(text)) == 1u && text[0] == '{');

  /* A listed document that is missing loads as an empty cell. */
  FilePath far_file = {.path = string8_create_from_cstr(
                           (const uint8_t *)far_path, strlen(far_path)),
                       .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&far_file) == FILE_ERROR_NONE);
  assert(vkr_scene_edit_save(&state, &scene, sidecar_path));
  assert(edit_test_read(far_path, text, sizeof(text)) &&
         strstr(text, "\"barrel\"") && !strstr(text, "\"tower\""));
  vkr_scene_edit_reset(&state, allocator, 1);
  vkr_scene_shutdown(&scene, NULL);

  const char *paths[] = {near_path, far_path, index, sidecar};
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    FilePath file = {.path = string8_create_from_cstr((const uint8_t *)paths[i],
                                                      strlen(paths[i])),
                     .type = FILE_PATH_TYPE_ABSOLUTE};
    (void)file_remove(&file);
  }
  /* POSIX removes the emptied directory; elsewhere it stays behind. */
  (void)remove(root);
  printf("  edit_test_partition PASSED\n");
}

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

  /* A changed source still binds an override whose node keeps the name it
     saved, as another machine's download of the same source gives. */
  const char rebound[] =
      "{\"version\":1,\"overrides\":[{\"scene_entity\":7,\"gltf_node\":0,"
      "\"source_fingerprint\":\"0000000000000000\",\"fields\":2,\"name\":"
      "\"parent\"}]}";
  edit_test_write(path, rebound, sizeof(rebound) - 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path) &&
         !state.sidecar_conflict);
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
  physics.physics.body.motion = VKR_PHYSICS_DYNAMIC;
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
         read.physics.body.motion == VKR_PHYSICS_DYNAMIC);
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
  assert(strstr(advanced_saved, "{\"version\":5,"));
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
  /* Directional records carry the colour temperature and atmosphere-sun flag
     at their own journal keys; a crossed key index drops or rejects them. */
  assert(vkr_scene_set_directional_light(
      &scene, child,
      &(SceneDirectionalLight){.color = vec3_one(),
                               .intensity = 1.0f,
                               .direction_local = vec3_new(0, -1, 0),
                               .enabled = true_v,
                               .atmosphere_sun = true_v}));
  vkr_scene_edit_reset(&state, &allocator, 1);
  VkrSceneEditValues sun;
  assert(vkr_scene_edit_read(&scene, child, &sun));
  sun.fields = VKR_SCENE_EDIT_DIRECTIONAL_LIGHT;
  sun.directional_light.temperature_kelvin = 3200.0f;
  sun.directional_light.atmosphere_sun = false_v;
  assert(vkr_scene_edit_apply(&state, &scene, child, &sun));
  assert(vkr_scene_edit_save(&state, &scene, file_path));
  SceneDirectionalLight *light = vkr_scene_get_directional_light(&scene, child);
  light->temperature_kelvin = 0.0f;
  light->atmosphere_sun = true_v;
  vkr_scene_edit_reset(&state, &allocator, 1);
  assert(vkr_scene_edit_load(&state, &scene, file_path));
  light = vkr_scene_get_directional_light(&scene, child);
  assert(light->temperature_kelvin == 3200.0f && !light->atmosphere_sun);

  FilePath saved_path = {.path = file_path, .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&saved_path) == FILE_ERROR_NONE);
  vkr_scene_edit_reset(&state, &allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  edit_test_structure();
  edit_test_groups();
  edit_test_brush_faces();
  edit_test_editor_hidden();
  edit_test_large();
  VkrDMemory partition_memory;
  assert(vkr_dmemory_create(MB(16), MB(64), &partition_memory));
  VkrAllocator partition_allocator = {.ctx = &partition_memory};
  vkr_dmemory_allocator_create(&partition_allocator);
  edit_test_partition(&partition_allocator);
  vkr_dmemory_allocator_destroy(&partition_allocator);
  vkr_dmemory_destroy(&partition_memory);
  printf("--- Scene Edit Tests Completed ---\n");
  return true_v;
}
