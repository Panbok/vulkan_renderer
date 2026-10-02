#include "editor_project_store_test.h"
#include "../../editor/src/editor_project_store.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_types.h"
#include "vkr_sample_runtime.h"

#include <assert.h>
#if defined(PLATFORM_WINDOWS)
#include <direct.h>
#else
#include <unistd.h>
#endif

static VkrEditorProject s_project;
static VkrEditorProject s_loaded;

static FilePath project_test_path(const char *text) {
  return (FilePath){.path = {.str = (uint8_t *)text, .length = strlen(text)},
                    .type = FILE_PATH_TYPE_ABSOLUTE};
}

static void project_test_remove_directory(const char *path) {
#if defined(PLATFORM_WINDOWS)
  assert(_rmdir(path) == 0);
#else
  assert(rmdir(path) == 0);
#endif
}

static void project_test_write(const char *path, const char *bytes) {
  FilePath file = project_test_path(path);
  FileHandle handle = {0};
  uint64_t written;
  assert(file_open(&file, (FileMode){.set = FILE_MODE_WRITE | FILE_MODE_BINARY},
                   &handle) == FILE_ERROR_NONE);
  assert(file_write(&handle, strlen(bytes), (const uint8_t *)bytes, &written) ==
         FILE_ERROR_NONE);
  assert(written == strlen(bytes));
  file_close(&handle);
}

static bool8_t project_test_visit(const char *id, void *context) {
  assert(strcmp(id, s_project.id) == 0);
  ++*(uint32_t *)context;
  return true_v;
}

static bool8_t project_test_count(const char *id, void *context) {
  (void)id;
  ++*(uint32_t *)context;
  return true_v;
}

static void project_test_overlay(VkrAllocator *allocator,
                                 const char *project_manifest) {
  char root[1024];
  assert(vkr_string_copy_bounded(root, sizeof(root), project_manifest));
  char *separator = strrchr(root, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(root, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
#endif
  assert(separator);
  *separator = '\0';
  char scene_root[1024];
  snprintf(scene_root, sizeof(scene_root),
           "%s/scenes/00000000-0000-4000-8000-000000000003", root);
  String8 folder = {.str = (uint8_t *)scene_root, .length = strlen(scene_root)};
  assert(file_ensure_directory(allocator, &folder));
  char manifest[1024];
  snprintf(manifest, sizeof(manifest), "%s/scene.json", scene_root);
  project_test_write(manifest, "{\"version\":3,\"id\":\"00000000-0000-4000-"
                               "8000-000000000003\",\"entities\":[]}");
  VkrScene scene;
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, allocator, 19, 8, &scene_error));
  VkrEntityId entity = vkr_scene_create_entity(&scene, &scene_error);
  assert(entity.u64);
  assert(vkr_scene_set_name(&scene, entity, string8_lit("Authored light")));
  assert(vkr_scene_set_transform(&scene, entity, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  SceneSourceIdentity identity = {.scene_entity_index = 0,
                                  .gltf_node_index = UINT32_MAX,
                                  .source_fingerprint =
                                      UINT64_C(0x1122334455667788)};
  assert(vkr_scene_set_source_identity(&scene, entity, &identity));
  VkrSceneEditState edits = {0};
  vkr_scene_edit_reset(&edits, allocator, 1);
  VkrSceneEditValues values;
  assert(vkr_scene_edit_read(&scene, entity, &values));
  values.position.x = 7;
  assert(vkr_scene_edit_apply(&edits, &scene, entity, &values));
  VkrEditorProjectError error;
  String8 document;
  uint64_t fingerprint;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &document,
                                           &fingerprint, &error));
  assert(vkr_editor_project_save_scene_overlay(manifest, &fingerprint, &edits,
                                               &scene, allocator, &error));
  assert(edits.revision == edits.saved_revision);
  String8 published;
  uint64_t observed;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &published,
                                           &observed, &error));
  assert(fingerprint == observed);
  char relative[128];
  assert(vkr_editor_project_json_string(published, "edit_overlay", relative,
                                        sizeof(relative), &error));
  char overlay[1024];
  assert(vkr_editor_project_resolve(scene_root, relative, overlay, &error));
  vkr_scene_set_position(&scene, entity, vec3_zero());
  vkr_scene_edit_reset(&edits, allocator, 1);
  assert(vkr_scene_edit_load(
      &edits, &scene,
      (String8){.str = (uint8_t *)overlay, .length = strlen(overlay)}));
  assert(vkr_scene_get_transform(&scene, entity)->position.x == 7);
  // A version 4 manifest keeps its inventory revision across overlay saves.
  project_test_write(manifest,
                     "{\"version\":4,\"id\":\"00000000-0000-4000-8000-"
                     "000000000003\",\"entities\":[],\"inventory\":"
                     "\"inventory/records.json\"}");
  uint64_t inventory_fingerprint = 0;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &document,
                                           &inventory_fingerprint, &error));
  assert(vkr_scene_edit_read(&scene, entity, &values));
  values.position.x = 8;
  assert(vkr_scene_edit_apply(&edits, &scene, entity, &values));
  assert(vkr_editor_project_save_scene_overlay(
      manifest, &inventory_fingerprint, &edits, &scene, allocator, &error));
  assert(vkr_editor_project_json_read_file(manifest, allocator, &published,
                                           &observed, &error));
  char inventory[64];
  assert(vkr_editor_project_json_string(published, "inventory", inventory,
                                        sizeof(inventory), &error));
  assert(strcmp(inventory, "inventory/records.json") == 0);
  char inventory_overlay_relative[128];
  char inventory_overlay[1024];
  assert(vkr_editor_project_json_string(
      published, "edit_overlay", inventory_overlay_relative,
      sizeof(inventory_overlay_relative), &error));
  assert(vkr_editor_project_resolve(scene_root, inventory_overlay_relative,
                                    inventory_overlay, &error));
  FilePath inventory_overlay_file = project_test_path(inventory_overlay);
  assert(file_remove(&inventory_overlay_file) == FILE_ERROR_NONE);
  // Project jobs publish version 5, which saves the same way.
  project_test_write(manifest,
                     "{\"version\":5,\"id\":\"00000000-0000-4000-8000-"
                     "000000000003\",\"entities\":[],\"inventory\":"
                     "\"inventory/records.json\"}");
  uint64_t current_fingerprint = 0;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &document,
                                           &current_fingerprint, &error));
  assert(vkr_scene_edit_read(&scene, entity, &values));
  values.position.x = 10;
  assert(vkr_scene_edit_apply(&edits, &scene, entity, &values));
  assert(vkr_editor_project_save_scene_overlay(
      manifest, &current_fingerprint, &edits, &scene, allocator, &error));
  assert(edits.revision == edits.saved_revision);
  assert(vkr_editor_project_json_read_file(manifest, allocator, &published,
                                           &observed, &error));
  char current_overlay_relative[128];
  char current_overlay[1024];
  assert(vkr_editor_project_json_string(
      published, "edit_overlay", current_overlay_relative,
      sizeof(current_overlay_relative), &error));
  assert(vkr_editor_project_resolve(scene_root, current_overlay_relative,
                                    current_overlay, &error));
  FilePath current_overlay_file = project_test_path(current_overlay);
  assert(file_remove(&current_overlay_file) == FILE_ERROR_NONE);
  fingerprint = observed;
  assert(vkr_scene_edit_read(&scene, entity, &values));
  values.position.x = 9;
  assert(vkr_scene_edit_apply(&edits, &scene, entity, &values));
  uint64_t saved = edits.saved_revision;
  project_test_write(manifest, "{\"version\":3,\"external\":true}");
  assert(!vkr_editor_project_save_scene_overlay(manifest, &fingerprint, &edits,
                                                &scene, allocator, &error));
  assert(edits.saved_revision == saved && edits.revision != saved);
  // A current manifest without version 3 and an identity is refused before
  // anything is staged, even when its fingerprint matches.
  project_test_write(manifest, "{\"id\":\"00000000-0000-4000-8000-"
                               "000000000003\",\"entities\":[]}");
  String8 versionless;
  uint64_t versionless_fingerprint = 0;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &versionless,
                                           &versionless_fingerprint, &error));
  assert(!vkr_editor_project_save_scene_overlay(
      manifest, &versionless_fingerprint, &edits, &scene, allocator, &error));
  assert(strstr(error.message, "scene version 3"));
  assert(edits.saved_revision == saved);
  String8 unchanged;
  uint64_t unchanged_fingerprint = 0;
  assert(vkr_editor_project_json_read_file(manifest, allocator, &unchanged,
                                           &unchanged_fingerprint, &error));
  assert(unchanged_fingerprint == versionless_fingerprint);
  FilePath previous_overlay = project_test_path(overlay);
  assert(file_exists(&previous_overlay));
  assert(file_remove(&previous_overlay) == FILE_ERROR_NONE);
  FilePath manifest_file = project_test_path(manifest);
  assert(file_remove(&manifest_file) == FILE_ERROR_NONE);
  char path[1024];
  snprintf(path, sizeof(path), "%s/edits", scene_root);
  project_test_remove_directory(path);
#if !defined(PLATFORM_WINDOWS)
  snprintf(path, sizeof(path),
           "%s/scene-00000000-0000-4000-8000-000000000003.lock", scene_root);
  FilePath lock = project_test_path(path);
  assert(file_remove(&lock) == FILE_ERROR_NONE);
#endif
  project_test_remove_directory(scene_root);
  vkr_scene_edit_reset(&edits, allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
}

typedef struct ProjectSettingsBuffer {
  uint8_t data[4096];
  uint64_t length;
} ProjectSettingsBuffer;

static bool8_t project_settings_sink(void *context, const uint8_t *data,
                                     uint64_t length) {
  ProjectSettingsBuffer *buffer = context;
  if (length > sizeof(buffer->data) - buffer->length) {
    return false_v;
  }
  MemCopy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  return true_v;
}

static void project_test_runtime_settings(void) {
  // Independent fixture catches missing fields and camera settings silently
  // dropped on save. Large source fingerprints must survive beyond f64
  // precision.
  String8 fixture =
      string8_lit("{\"version\":2,\"filter_mode\":0,\"gizmo_mode\":1,"
                  "\"gizmo_space\":0,\"gizmo_size\":150,\"camera_speed\":3.25,"
                  "\"camera_sensitivity\":0.5,\"move_multiplier\":4,"
                  "\"rotation_multiplier\":2,\"ibl_debug_mode\":2,"
                  "\"ibl_debug_scalar\":0.75,\"pass_gpu_timings\":true}");
  VkrSampleRuntimePreferences value;
  assert(vkr_sample_runtime_preferences_read_json(fixture, &value));
  assert(value.camera_speed == 3.25f && value.pass_gpu_timings);
  ProjectSettingsBuffer buffer = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(vkr_sample_runtime_preferences_write_json(&value, &writer));
  assert(vkr_json_writer_complete(&writer));
  VkrSampleRuntimePreferences restored;
  assert(vkr_sample_runtime_preferences_read_json(
      (String8){.str = buffer.data, .length = buffer.length}, &restored));
  assert(restored.camera_speed == 3.25f && restored.camera_sensitivity == 0.5f);
  assert(restored.gizmo_mode == 1 && restored.ibl_debug_mode == 2);
  assert(restored.move_multiplier == 4 && restored.rotation_multiplier == 2);
  assert(restored.gizmo_size == 150 && restored.ibl_debug_scalar == 0.75f);
  assert(!vkr_sample_runtime_preferences_read_json(
      string8_lit("{\"version\":99}"), &restored));
  /* A version 1 gizmo size counted window pixels; it resets to the point
     default instead of doubling on a Retina display. */
  VkrSampleRuntimePreferences migrated;
  assert(vkr_sample_runtime_preferences_read_json(
      string8_lit("{\"version\":1,\"filter_mode\":0,\"gizmo_mode\":1,"
                  "\"gizmo_space\":0,\"gizmo_size\":150,"
                  "\"camera_speed\":3.25,\"camera_sensitivity\":0.5,"
                  "\"move_multiplier\":4,\"rotation_multiplier\":2,"
                  "\"ibl_debug_mode\":2,\"ibl_debug_scalar\":0.75,"
                  "\"pass_gpu_timings\":true}"),
      &migrated));
  assert(migrated.gizmo_size == VKR_GIZMO_CONFIG_DEFAULT.screen_size &&
         migrated.camera_speed == 3.25f);
  assert(restored.camera_speed == 3.25f);
  VkrSampleSceneRecall recall = {
      .camera_valid = true_v,
      .position = {1, 2, 3},
      .yaw = -90,
      .pitch = 12,
      .field_of_view = 65,
      .near_plane = 0.25f,
      .far_plane = 500,
      .selection_valid = true_v,
      .selection = {.scene_entity = 7,
                    .gltf_node = UINT32_MAX,
                    .source_fingerprint = UINT64_C(0xfedcba9876543210)}};
  buffer.length = 0;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(vkr_sample_scene_recall_write_json(&recall, &writer));
  assert(vkr_json_writer_complete(&writer));
  VkrSampleSceneRecall loaded;
  assert(vkr_sample_scene_recall_read_json(
      (String8){.str = buffer.data, .length = buffer.length}, &loaded));
  assert(loaded.selection.source_fingerprint == UINT64_C(0xfedcba9876543210));
  assert(loaded.selection.gltf_node == UINT32_MAX &&
         loaded.selection.scene_entity == 7);
  assert(loaded.camera_valid && loaded.selection_valid &&
         loaded.position.z == 3);
  assert(loaded.pitch == 12 && loaded.field_of_view == 65 &&
         loaded.near_plane == 0.25f);
  // A recall written without a grid height, as older files are, reads zero;
  // a lifted grid round-trips.
  assert(loaded.grid_height == 0.0f);
  recall.grid_height = 0.25f;
  buffer.length = 0;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(vkr_sample_scene_recall_write_json(&recall, &writer));
  assert(vkr_json_writer_complete(&writer));
  assert(vkr_sample_scene_recall_read_json(
      (String8){.str = buffer.data, .length = buffer.length}, &loaded));
  assert(loaded.grid_height == 0.25f);
  recall.near_plane = 600;
  buffer.length = 0;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(!vkr_sample_scene_recall_write_json(&recall, &writer));
}

// Independent fixture: labels keep bundle and UUID ids, strip JSON delimiters
// that would corrupt the next save, skip unusable ids, and a version 1 file
// gains the folders its labels name. A version 2 save reloads unchanged, and
// folder moves carry their items without escaping the capacity.
static void project_test_asset_labels(void) {
  String8 fixture = string8_lit(
      "{\"version\":1,\"labels\":["
      "{\"id\":\"Inter-Regular-cooked\",\"folder\":\" /UI//Fonts/ \","
      "\"tags\":\"editor, text\"},"
      "{\"id\":\"00000000-0000-4000-8000-000000000002\","
      "\"folder\":\"Props\\\"\\\\\",\"tags\":\"\"},"
      "{\"id\":\"\",\"folder\":\"Lost\"},"
      "{\"id\":\"0123456789012345678901234567890123456789\"}]}");
  VkrEditorAssetLabel labels[4] = {0};
  VkrEditorFolder folders[8] = {0};
  VkrEditorContentLabels doc = {.labels = labels,
                                .label_capacity = ArrayCount(labels),
                                .folders = folders,
                                .folder_capacity = ArrayCount(folders)};
  VkrEditorProjectError error = {0};
  assert(vkr_editor_labels_parse(fixture, &doc, &error));
  assert(doc.label_count == 2u);
  assert(strcmp(labels[0].id, "Inter-Regular-cooked") == 0);
  assert(strcmp(labels[0].folder, "UI/Fonts") == 0);
  assert(strcmp(labels[0].tags, "editor, text") == 0);
  assert(strcmp(labels[1].folder, "Props") == 0 && !labels[1].tags[0]);
  assert(doc.folder_count == 3u && strcmp(folders[0].path, "UI") == 0 &&
         strcmp(folders[1].path, "UI/Fonts") == 0);
  assert(vkr_editor_folders_add(&doc, "Props/Bistro/Chairs"));
  assert(doc.folder_count == 5u);

  ProjectSettingsBuffer buffer = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(vkr_editor_labels_write(&writer, &doc));
  assert(vkr_json_writer_complete(&writer));
  VkrEditorAssetLabel restored_labels[4] = {0};
  VkrEditorFolder restored_folders[8] = {0};
  VkrEditorContentLabels restored = {
      .labels = restored_labels,
      .label_capacity = ArrayCount(restored_labels),
      .folders = restored_folders,
      .folder_capacity = ArrayCount(restored_folders)};
  assert(vkr_editor_labels_parse(
      (String8){.str = buffer.data, .length = buffer.length}, &restored,
      &error));
  assert(restored.label_count == doc.label_count &&
         restored.folder_count == doc.folder_count);
  assert(MemCompare(restored_labels, labels,
                    sizeof(labels[0]) * doc.label_count) == 0);
  assert(MemCompare(restored_folders, folders,
                    sizeof(folders[0]) * doc.folder_count) == 0);

  /* Moving a folder carries its subfolders and items; moving it into itself,
     onto an existing folder or removing a non-empty one changes nothing. */
  assert(!vkr_editor_folders_rename(&doc, "Props", "Props/Inner"));
  assert(!vkr_editor_folders_rename(&doc, "Props", "UI"));
  assert(!vkr_editor_folders_remove(&doc, "Props"));
  assert(vkr_editor_folders_rename(&doc, "Props", "World/Props"));
  assert(strcmp(labels[1].folder, "World/Props") == 0);
  bool8_t moved = false_v;
  for (uint32_t i = 0; i < doc.folder_count; ++i) {
    assert(!vkr_editor_folder_within(folders[i].path, "Props"));
    moved |= strcmp(folders[i].path, "World/Props/Bistro/Chairs") == 0;
  }
  assert(moved);
  assert(vkr_editor_folders_remove(&doc, "World/Props/Bistro/Chairs"));
  assert(strcmp(vkr_editor_folder_name("World/Props"), "Props") == 0);

  restored.label_count = 1u;
  assert(!vkr_editor_labels_parse(string8_lit("{\"version\":3,\"labels\":[]}"),
                                  &restored, &error));
  assert(restored.label_count == 0u && restored.folder_count == 0u);
}

/* Presets keep typed values: a written document reads back byte-equal, and
   records of unknown types or with values their type rejects are skipped. */
/* game.json (docs/proposals/project-packaging.md): a missing file yields the
   default, a save replaces what this editor owns and keeps the members
   another tool wrote, and validation refuses an excluded startup scene. */
static void project_test_game(const VkrEditorProject *project,
                              VkrAllocator *allocator) {
  static VkrEditorGame game;
  VkrEditorProjectError error = {0};
  bool8_t exists = true_v;
  assert(vkr_editor_game_load(project, allocator, &game, &exists, &error));
  assert(!exists && !strcmp(game.name, project->name) &&
         !strcmp(game.executable, project->name) && game.profile_count == 1u &&
         !strcmp(game.profiles[0].config, "development"));
  char path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  const char *slash = strrchr(project->manifest_path, '/');
  snprintf(path, sizeof(path), "%.*s/game.json",
           (int)(slash - project->manifest_path), project->manifest_path);
  project_test_write(
      path, "{\"version\":1,\"tool\":{\"kept\":true},\"game\":{\"name\":"
            "\"Arena\",\"future\":\"kept\",\"executable\":\"Arena\","
            "\"scenes\":[],\"window\":{\"mode\":\"windowed\",\"width\":"
            "1280,\"height\":720},\"graphics\":{\"version\":1,\"vsync\":"
            "false}},\"profiles\":[{\"name\":\"Ship\",\"platform\":"
            "\"host\",\"config\":\"shipping\",\"output\":\"/tmp/Arena\","
            "\"include\":[\"docs\"],\"bake_lighting\":true}]}");
  assert(vkr_editor_game_load(project, allocator, &game, &exists, &error));
  assert(exists && !strcmp(game.name, "Arena") && game.window_width == 1280u &&
         game.profile_count == 1u && game.profiles[0].bake_lighting &&
         game.profiles[0].include_count == 1u &&
         !strcmp(game.profiles[0].config, "shipping"));
  assert(vkr_editor_game_validate(&game, project, &error));
  assert(!strcmp(game.window_mode, "windowed"));
  snprintf(game.window_mode, sizeof(game.window_mode), "exclusive");
  assert(!vkr_editor_game_validate(&game, project, &error));
  snprintf(game.window_mode, sizeof(game.window_mode), "borderless");
  snprintf(game.version, sizeof(game.version), "2.0.0");
  assert(vkr_editor_game_save(project, &game, allocator, &error));
  String8 saved = {0};
  uint64_t fingerprint = 0u;
  String8 member = {0};
  String8 settings = {0};
  assert(vkr_editor_project_json_read_file(path, allocator, &saved,
                                           &fingerprint, &error));
  assert(vkr_editor_project_json_member(saved, "tool", &member, &error));
  assert(vkr_editor_project_json_member(saved, "game", &settings, &error));
  assert(vkr_editor_project_json_member(settings, "future", &member, &error));
  char text[32];
  assert(vkr_editor_project_json_string(settings, "version", text, sizeof(text),
                                        &error) &&
         !strcmp(text, "2.0.0"));
  assert(vkr_editor_project_json_member(settings, "graphics", &member, &error));
  assert(vkr_editor_game_load(project, allocator, &game, &exists, &error));
  assert(strstr(game.graphics, "\"vsync\":false") != NULL);
  assert(!strcmp(game.window_mode, "borderless"));
  snprintf(game.startup_scene, sizeof(game.startup_scene), "%s",
           "00000000-0000-4000-8000-000000000000");
  game.scene_count = 0u;
  assert(vkr_editor_game_validate(&game, project, &error));
  snprintf(game.scenes[game.scene_count++], sizeof(game.scenes[0]), "%s",
           "00000000-0000-4000-8000-000000000001");
  assert(!vkr_editor_game_validate(&game, project, &error));
  FilePath file = project_test_path(path);
  assert(file_remove(&file) == FILE_ERROR_NONE);
}

static void project_test_presets(void) {
  VkrEditorPreset presets[4];
  MemZero(presets, sizeof(presets));
  VkrEditorPresets doc = {.presets = presets, .capacity = ArrayCount(presets)};
  presets[0].type = vkr_editor_preset_type(string8_lit("post_process"));
  presets[1].type = vkr_editor_preset_type(string8_lit("point_light"));
  assert(presets[0].type == &vkr_scene_post_process_type);
  assert(presets[1].type == &vkr_scene_point_light_type);
  assert(!vkr_editor_preset_type(string8_lit("transform")));
  vkr_type_defaults(presets[0].type, presets[0].value);
  ((ScenePostProcess *)presets[0].value)->exposure_compensation_ev = 1.5f;
  vkr_type_defaults(presets[1].type, presets[1].value);
  ((ScenePointLight *)presets[1].value)->intensity = 12.0f;
  snprintf(presets[0].id, sizeof(presets[0].id), "p-grade");
  snprintf(presets[0].name, sizeof(presets[0].name), "Warm grade");
  snprintf(presets[1].id, sizeof(presets[1].id), "p-lamp");
  snprintf(presets[1].name, sizeof(presets[1].name), "Street lamp");
  doc.count = 2u;

  static ProjectSettingsBuffer buffer;
  buffer.length = 0u;
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_settings_sink, &buffer);
  assert(vkr_editor_presets_write(&writer, &doc));
  assert(vkr_json_writer_complete(&writer));
  VkrEditorPreset restored_presets[4];
  MemZero(restored_presets, sizeof(restored_presets));
  VkrEditorPresets restored = {.presets = restored_presets,
                               .capacity = ArrayCount(restored_presets)};
  VkrEditorProjectError error = {0};
  assert(vkr_editor_presets_parse(
      (String8){.str = buffer.data, .length = buffer.length}, &restored, NULL,
      &error));
  assert(restored.count == 2u);
  for (uint32_t i = 0; i < 2u; ++i) {
    assert(restored_presets[i].type == presets[i].type);
    assert(strcmp(restored_presets[i].name, presets[i].name) == 0);
    assert(MemCompare(restored_presets[i].value, presets[i].value,
                      presets[i].type->size) == 0);
  }

  assert(vkr_editor_presets_parse(
      string8_lit("{\"version\":1,\"presets\":["
                  "{\"id\":\"a\",\"name\":\"Gone\",\"type\":\"teapot\","
                  "\"values\":{}},"
                  "{\"id\":\"b\",\"name\":\"Bad\",\"type\":\"point_light\","
                  "\"values\":{\"intensity\":-5}},"
                  "{\"id\":\"c\",\"name\":\"Dim\",\"type\":\"point_light\","
                  "\"values\":{\"intensity\":2}}]}"),
      &restored, NULL, &error));
  assert(restored.count == 1u && strcmp(restored_presets[0].id, "c") == 0);
  assert(((ScenePointLight *)restored_presets[0].value)->intensity == 2.0f);
  assert(!vkr_editor_presets_parse(
      string8_lit("{\"version\":2,\"presets\":[]}"), &restored, NULL, &error));
  assert(restored.count == 0u);
}

bool32_t run_editor_project_store_tests(void) {
  project_test_runtime_settings();
  project_test_asset_labels();
  project_test_presets();
  printf("Running editor project store tests...\n");
  VkrEditorProjectError error = {0};
  assert(vkr_editor_project_name_valid("Освітлення 日光", &error));
  assert(!vkr_editor_project_name_valid("   ", &error));
  assert(!vkr_editor_project_name_valid("bad\nname", &error));
  assert(!vkr_editor_project_name_valid("\xc0\xaf", &error));

  // Independent input fixtures detect malformed syntax, duplicate decoded keys,
  // unknown versions and membership traversal rather than mirroring
  // serialization.
  const char *valid =
      "{\"version\":1,\"id\":\"00000000-0000-4000-8000-000000000001\","
      "\"name\":\"A \\u65e5 \\ud83d\\ude00\",\"default_font\":{},"
      "\"editor_settings\":{\"version\":1,\"future\":{\"value\":9}},"
      "\"scene_editor_state\":{},\"assets\":[],\"scenes\":[]}";
  String8 bytes = {.str = (uint8_t *)valid, .length = strlen(valid)};
  assert(vkr_editor_project_parse(bytes, &s_project, &error));
  assert(strcmp(s_project.name, "A 日 😀") == 0);
  const char *bad[] = {"{\"version\":1,\"version\":1}",
                       "{\"version\":1,\"\\u0076ersion\":1}",
                       "{\"version\":2}",
                       "{\"version\":1,}",
                       "{\"version\":01}",
                       "{\"version\":1,\"a\":NaN}",
                       "{\"version\":1,\"a\":1e999}",
                       "{\"a\":\"\\ud800\"}",
                       "{\"a\":\"\x80\"}",
                       "{\"version\":1} junk",
                       "{\"a\": [1,]}"};
  for (uint32_t i = 0; i < ArrayCount(bad); ++i) {
    assert(!vkr_editor_project_parse(
        (String8){.str = (uint8_t *)bad[i], .length = strlen(bad[i])},
        &s_loaded, &error));
  }
  // These variants retain every required manifest field. Rejecting them cannot
  // accidentally pass solely because a required field is absent.
  const char *invalid_extra[] = {
      "\"version\":1",       "\"\\u0076ersion\":1",
      "\"bad\":NaN",         "\"bad\":1e999",
      "\"bad\":01",          "\"bad\":[1,]",
      "\"bad\":\"\\ud800\"", "\"bad\":{\"x\":1,\"x\":2}"};
  for (uint32_t i = 0; i < ArrayCount(invalid_extra); ++i) {
    char malformed[2048];
    snprintf(malformed, sizeof(malformed), "{%s,%s", invalid_extra[i],
             valid + 1);
    assert(!vkr_editor_project_parse(
        (String8){.str = (uint8_t *)malformed, .length = strlen(malformed)},
        &s_loaded, &error));
  }
  String8 member;
  assert(vkr_editor_project_json_member(s_project.editor_settings, "future",
                                        &member, &error));
  assert(member.length == strlen("{\"value\":9}"));
  assert(MemCompare(member.str, "{\"value\":9}", member.length) == 0);

  char uuid[37];
  assert(vkr_editor_project_id_generate(uuid, &error));
  char directory[1024];
  snprintf(directory, sizeof(directory), "%stests/tmp/project-%s",
           PROJECT_SOURCE_DIR, uuid);
  Arena *arena = arena_create(MB(8), MB(1));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  // A valid inventory can exceed the old fixed 16K-token ceiling while staying
  // far below the supported one-MiB document limit.
  uint8_t *inventory =
      vkr_allocator_alloc(&allocator, KB(512), VKR_ALLOCATOR_MEMORY_TAG_STRING);
  assert(inventory);
  uint32_t inventory_length =
      (uint32_t)snprintf((char *)inventory, KB(512), "{\"assets\":[");
  for (uint32_t i = 0; i < 20000; ++i) {
    inventory_length += (uint32_t)snprintf((char *)inventory + inventory_length,
                                           KB(512) - inventory_length,
                                           "%s{\"id\":%u}", i ? "," : "", i);
  }
  inventory_length +=
      (uint32_t)snprintf((char *)inventory + inventory_length,
                         KB(512) - inventory_length, "],\"complete\":true}");
  String8 inventory_marker;
  VkrEditorProjectError inventory_error = {0};
  assert(vkr_editor_project_json_member(
      (String8){.str = inventory, .length = inventory_length}, "complete",
      &inventory_marker, &inventory_error));
  assert(inventory_marker.length == 4 &&
         MemCompare(inventory_marker.str, "true", 4) == 0);
  vkr_allocator_free(&allocator, inventory, KB(512),
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  String8 merged;
  VkrEditorProjectError merge_error = {0};
  assert(vkr_editor_project_json_merge_objects(
      &allocator,
      string8_lit("{\"graphics\":{\"known\":1,\"future\":9007199254740993},"
                  "\"future_root\":[1,2]}"),
      string8_lit("{\"graphics\":{\"known\":2},\"new\":true}"), &merged,
      &merge_error));
  String8 merged_graphics;
  String8 merged_future;
  assert(vkr_editor_project_json_member(merged, "graphics", &merged_graphics,
                                        &merge_error));
  assert(vkr_editor_project_json_member(merged_graphics, "future",
                                        &merged_future, &merge_error));
  assert(merged_future.length == 16 &&
         MemCompare(merged_future.str, "9007199254740993", 16) == 0);
  assert(vkr_editor_project_json_member(merged_graphics, "known",
                                        &merged_future, &merge_error));
  assert(merged_future.length == 1 && merged_future.str[0] == '2');
  assert(vkr_editor_project_json_member(merged, "future_root", &merged_future,
                                        &merge_error));
  assert(merged_future.length == 5 &&
         MemCompare(merged_future.str, "[1,2]", 5) == 0);

  String8 directory_view = {.str = (uint8_t *)directory,
                            .length = strlen(directory)};
  assert(file_ensure_directory(&allocator, &directory_view));
  char locator[1024];
  char located[1024];
  snprintf(locator, sizeof(locator), "%s/local/editor.json", directory);
  assert(vkr_editor_workspace_locator_load(locator, located, &error));
  assert(!located[0]);
  assert(vkr_editor_workspace_locator_save(locator, directory, &error));
  assert(vkr_editor_workspace_locator_load(locator, located, &error));
  FilePath selected_directory = project_test_path(directory);
  char canonical_directory[1024];
  assert(file_path_resolve(&selected_directory, canonical_directory,
                           sizeof(canonical_directory)) == FILE_ERROR_NONE);
  assert(file_path_equals(located, canonical_directory));
  project_test_write(locator, "{\"version\":2,\"directory\":\"/\"}");
  assert(!vkr_editor_workspace_locator_load(locator, located, &error));
  FilePath locator_file = project_test_path(locator);
  assert(file_remove(&locator_file) == FILE_ERROR_NONE);
  char local_directory[1024];
  snprintf(local_directory, sizeof(local_directory), "%s/local", directory);
  project_test_remove_directory(local_directory);
  VkrEditorWorkspace workspace;
  assert(vkr_editor_workspace_open(directory, false_v, &workspace, &error));
  assert(!workspace.initialized);
  FilePath not_created = project_test_path(workspace.root);
  assert(!file_exists(&not_created));
  assert(vkr_editor_project_create(&workspace, "Portable project", &s_project,
                                   &error));
  VkrEditorWorkspaceLease lease = {0};
  VkrEditorWorkspaceLease competing_lease = {0};
  assert(vkr_editor_workspace_lease_acquire(&workspace, &lease, &error));
  assert(!vkr_editor_workspace_lease_acquire(&workspace, &competing_lease,
                                             &error));
  vkr_editor_workspace_lease_release(&lease);
  assert(
      vkr_editor_workspace_lease_acquire(&workspace, &competing_lease, &error));
  vkr_editor_workspace_lease_release(&competing_lease);
  // Creation publishes the manifest, so a failed first project job cannot
  // leave an unlisted directory behind.
  uint32_t visits = 0;
  assert(vkr_editor_workspace_visit(&workspace, project_test_visit, &visits,
                                    &error));
  assert(visits == 1);
  s_project.editor_settings =
      string8_lit("{\"version\":1,\"graphics\":{\"exposure\":1.75},\"future\":{"
                  "\"text\":\"日\",\"n\":9007199254740993}}");
  assert(vkr_editor_project_save(&s_project, &error));
  project_test_game(&s_project, &allocator);
  visits = 0;
  assert(vkr_editor_workspace_visit(&workspace, project_test_visit, &visits,
                                    &error));
  assert(visits == 1);
  assert(vkr_editor_project_load(&workspace, s_project.id, &allocator,
                                 &s_loaded, &error));
  assert(s_loaded.editor_settings.length == s_project.editor_settings.length);
  assert(MemCompare(s_loaded.editor_settings.str, s_project.editor_settings.str,
                    s_project.editor_settings.length) == 0);
  // Request-generation uses the loaded native manifest path as the scene owner.
  char selected_scene[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  s_loaded.scene_count = 1;
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].path, "project.json");
  assert(vkr_editor_project_scene_path(&s_loaded, 0, true_v, selected_scene,
                                       &error));
  assert(strcmp(selected_scene, s_loaded.manifest_path) == 0);
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].path, "missing-scene.json");
  assert(!vkr_editor_project_scene_path(&s_loaded, 0, true_v, selected_scene,
                                        &error));
  assert(vkr_editor_project_scene_path(&s_loaded, 0, false_v, selected_scene,
                                       &error));
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].path, "../escape.json");
  assert(!vkr_editor_project_scene_path(&s_loaded, 0, false_v, selected_scene,
                                        &error));
  s_loaded.scene_count = 0;
  char lock_directory[1024];
  snprintf(lock_directory, sizeof(lock_directory), "%s/projects/%s",
           workspace.root, s_project.id);
  char lock_name[64];
  snprintf(lock_name, sizeof(lock_name), "project-%s", s_project.id);
  VkrPlatformProcessLock held_lock = {0};
  assert(
      vkr_platform_process_lock_acquire(lock_name, lock_directory, &held_lock));
  assert(!vkr_editor_project_save(&s_loaded, &error));
  assert(strstr(error.message, "another editor"));
  vkr_platform_process_lock_release(&held_lock);
  VKR_STRING_COPY_LITERAL(s_loaded.name, "Renamed 日");
  assert(vkr_editor_project_save(&s_loaded, &error));
  // A second loaded instance must not overwrite the rename with an old
  // snapshot.
  assert(!vkr_editor_project_save(&s_project, &error));
  assert(strstr(error.message, "changed outside"));
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].id,
                          "00000000-0000-4000-8000-000000000002");
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].name, "Scene");
  VKR_STRING_COPY_LITERAL(s_loaded.scenes[0].path, "../escape/scene.json");
  s_loaded.scene_count = 1;
  assert(!vkr_editor_project_save(&s_loaded, &error));
  s_loaded.scene_count = 0;
  assert(vkr_editor_project_save(&s_loaded, &error));
  project_test_overlay(&allocator, s_loaded.manifest_path);
  // Deletion must preserve order and unrelated recall, survive reopening, and
  // restore the entire selection on a stale manifest conflict.
  for (uint32_t i = 0; i < 3; ++i) {
    snprintf(s_loaded.scenes[i].id, sizeof(s_loaded.scenes[i].id),
             "00000000-0000-4000-8000-%012u", i + 10);
    snprintf(s_loaded.scenes[i].name, sizeof(s_loaded.scenes[i].name),
             "Deletion fixture %u", i);
    snprintf(s_loaded.scenes[i].path, sizeof(s_loaded.scenes[i].path),
             "scenes/%s/scene.json", s_loaded.scenes[i].id);
  }
  s_loaded.scene_count = 3;
  s_loaded.scene_editor_state =
      string8_lit("{\"00000000-0000-4000-8000-000000000010\":{\"marker\":10},"
                  "\"00000000-0000-4000-8000-000000000011\":{\"marker\":11},"
                  "\"00000000-0000-4000-8000-000000000012\":{\"marker\":12}}");
  assert(vkr_editor_project_save(&s_loaded, &error));
  s_project = s_loaded;
  assert(vkr_editor_project_remove_scene(&s_loaded, 1, &allocator, &error));
  assert(s_loaded.scene_count == 2);
  assert(
      !strcmp(s_loaded.scenes[1].id, "00000000-0000-4000-8000-000000000012"));
  assert(!vkr_editor_project_remove_scene(&s_project, 0, &allocator, &error));
  assert(strstr(error.message, "changed outside"));
  assert(s_project.scene_count == 3);
  assert(
      !strcmp(s_project.scenes[1].id, "00000000-0000-4000-8000-000000000011"));
  assert(strstr((char *)s_project.scene_editor_state.str, "marker\":11"));
  assert(vkr_editor_project_load(&workspace, s_loaded.id, &allocator,
                                 &s_project, &error));
  assert(s_project.scene_count == 2);
  // A failed lookup clears its output, including for malformed documents.
  String8 malformed_member = string8_lit("stale");
  assert(!vkr_editor_project_json_member(string8_lit("not json"), "version",
                                         &malformed_member, &error));
  assert(!malformed_member.str && !malformed_member.length);
  String8 deleted_recall;
  assert(!vkr_editor_project_json_member(s_project.scene_editor_state,
                                         "00000000-0000-4000-8000-000000000011",
                                         &deleted_recall, &error));
  assert(!deleted_recall.length);
  assert(vkr_editor_project_remove_scene(&s_loaded, 1, &allocator, &error));
  assert(vkr_editor_project_remove_scene(&s_loaded, 0, &allocator, &error));
  assert(s_loaded.scene_count == 0);
  const String8 empty_recall = string8_lit("{}");
  assert(string8_equals(&s_loaded.scene_editor_state, &empty_recall));
  assert(!vkr_editor_project_remove_scene(&s_loaded, 0, &allocator, &error));
  assert(vkr_editor_project_load(&workspace, s_loaded.id, &allocator,
                                 &s_project, &error));
  assert(s_project.scene_count == 0);
  String8 replaced;
  assert(vkr_editor_project_json_replace_member(
      &allocator, string8_lit("{\"untouched\":9007199254740993,\"value\":1}"),
      "value", string8_lit("{\"nested\":true}"), &replaced, &error));
  assert(strstr((const char *)replaced.str, "9007199254740993"));
  String8 nested;
  assert(vkr_editor_project_json_member(replaced, "value", &nested, &error));
  assert(nested.length == strlen("{\"nested\":true}"));
  String8 extended;
  assert(vkr_editor_project_json_replace_member(
      &allocator, replaced, "new_key", string8_lit("[]"), &extended, &error));
  assert(vkr_editor_project_json_member(extended, "new_key", &nested, &error));
  assert(nested.length == 2);
  char resolved[1024];
  assert(!vkr_editor_project_resolve(workspace.root, "../outside", resolved,
                                     &error));
  assert(!vkr_editor_project_resolve(workspace.root, "/etc/passwd", resolved,
                                     &error));
  assert(!vkr_editor_project_resolve(
      workspace.root, "projects/../workspace.json", resolved, &error));
#if !defined(PLATFORM_WINDOWS)
  char link[1024];
  snprintf(link, sizeof(link), "%s/escape", workspace.root);
  assert(symlink(directory, link) == 0);
  assert(
      !vkr_editor_project_resolve(workspace.root, "escape", resolved, &error));
  assert(unlink(link) == 0);
  // Move the selected workspace and reopen: all stored references stay
  // relative.
  char relocated[1024];
  snprintf(relocated, sizeof(relocated), "%stests/tmp/moved-%s",
           PROJECT_SOURCE_DIR, uuid);
  assert(rename(directory, relocated) == 0);
  assert(vkr_string_copy_bounded(directory, sizeof(directory), relocated));
  assert(vkr_editor_workspace_open(relocated, false_v, &workspace, &error));
  assert(vkr_editor_project_load(&workspace, s_project.id, &allocator,
                                 &s_loaded, &error));
  assert(strcmp(s_loaded.name, "Renamed 日") == 0);
#endif
  // External unsupported document is preserved by the stale-write refusal.
  project_test_write(s_loaded.manifest_path, "{\"version\":999}");
  assert(!vkr_editor_project_save(&s_loaded, &error));
  assert(!vkr_editor_project_load(&workspace, s_project.id, &allocator,
                                  &s_project, &error));
  // Unpublishing removes discovery before any file erasure, even for a
  // manifest this store cannot parse; repeating it reports the missing file.
  uint32_t listed = 0;
  assert(vkr_editor_workspace_visit(&workspace, project_test_count, &listed,
                                    &error));
  assert(listed == 1);
  assert(vkr_editor_project_unpublish(&workspace, s_loaded.id, &error));
  listed = 0;
  assert(vkr_editor_workspace_visit(&workspace, project_test_count, &listed,
                                    &error));
  assert(listed == 0);
  assert(!vkr_editor_project_unpublish(&workspace, s_loaded.id, &error));
  FilePath final_manifest = project_test_path(s_loaded.manifest_path);
  assert(!file_exists(&final_manifest));
  char project_root[1024];
  assert(vkr_string_copy_bounded(project_root, sizeof(project_root),
                                 s_loaded.manifest_path));
  char *slash = strrchr(project_root, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(project_root, '\\');
  if (backslash && (!slash || backslash > slash)) {
    slash = backslash;
  }
#endif
  assert(slash);
  *slash = '\0';
  char cleanup[1024];
  snprintf(cleanup, sizeof(cleanup), "%s/scenes", project_root);
  project_test_remove_directory(cleanup);
#if !defined(PLATFORM_WINDOWS)
  snprintf(cleanup, sizeof(cleanup), "%s/project-%s.lock", project_root,
           s_loaded.id);
  FilePath lock_path = project_test_path(cleanup);
  assert(file_remove(&lock_path) == FILE_ERROR_NONE);
#endif
  project_test_remove_directory(project_root);
  snprintf(cleanup, sizeof(cleanup), "%s/projects", workspace.root);
  project_test_remove_directory(cleanup);
  snprintf(cleanup, sizeof(cleanup), "%s/workspace.json", workspace.root);
  FilePath workspace_manifest = project_test_path(cleanup);
  assert(file_remove(&workspace_manifest) == FILE_ERROR_NONE);
#if !defined(PLATFORM_WINDOWS)
  snprintf(cleanup, sizeof(cleanup), "%s/workspace-%s.lock", workspace.root,
           workspace.id);
  FilePath lease_path = project_test_path(cleanup);
  assert(file_remove(&lease_path) == FILE_ERROR_NONE);
#endif
  project_test_remove_directory(workspace.root);
  project_test_remove_directory(directory);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  printf("Editor project store tests PASSED\n");
  return true_v;
}
