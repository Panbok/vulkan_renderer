#include "editor_recording.h"

#include "assets/vkr_recording_analysis.h"
#include "core/vkr_content_codec.h"
#include "editor_internal.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory_allocator.h"
#include "vkr_bakery_json.h"

/* Perf re-runs (docs/proposals/recordings.md, "Perf re-run"): a harness case
   whose camera replays a recording's frame range (ADR-051) under a timing
   case's settings. The editor writes the case and copies the recording where
   the case can name it; the harness runs it, never the editor, which would
   share the GPU with the benchmark. Paths are relative to the repository the
   editor runs from. */

/* The settings a re-run inherits: the windowed timing case the Mac's
   authoritative runs use, and the profile its command names. */
#define EDITOR_RERUN_TEMPLATE                                                  \
  "tools/cases/performance/bistro_shadow_orbit.case.json"
#define EDITOR_RERUN_PROFILE "tools/profiles/performance-windowed-gpu.json"
#define EDITOR_RERUN_ARTIFACTS "build/_artifacts/rerun"
/* Ignored by git: a re-run is a local input, like the recording and the
   scene assets it names, so the tree stays clean for authoritative
   profiles. The directory is the case's suite. */
#define EDITOR_RERUN_CASES "tools/cases/rerun"
#define EDITOR_RERUN_SUITE "rerun"
#define EDITOR_RERUN_FILE_MAX (16ull * 1024ull * 1024ull)

/* ------------------------------------------------------------------------ */
/* Files                                                                     */

/* A copy of `source` at `destination`, replacing it: a clone where the
   volume shares blocks, else a hard link, else the bytes. */
vkr_internal bool8_t editor_rerun_copy(const char *source,
                                       const char *destination) {
  const FilePath from = file_path_view(source);
  const FilePath to = file_path_view(destination);
  if (file_exists(&to)) {
    (void)file_remove(&to);
  }
  if (file_clone(&from, &to) == FILE_ERROR_NONE ||
      file_link(&from, &to) == FILE_ERROR_NONE) {
    return true_v;
  }
  FileMapping mapping = {0};
  if (file_map_readonly(&from, &mapping) != FILE_ERROR_NONE) {
    return false_v;
  }
  FileHandle handle = {0};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  bool8_t ok = file_open(&to, mode, &handle) == FILE_ERROR_NONE;
  if (ok) {
    uint64_t written = 0u;
    ok = file_write(&handle, mapping.size, (const uint8_t *)mapping.data,
                    &written) == FILE_ERROR_NONE &&
         written == mapping.size;
    file_close(&handle);
  }
  file_unmap(&mapping);
  return ok;
}

/* `text` as a case id or file name part: lowercase letters, digits and
   '_'; anything else becomes '_'. */
vkr_internal void editor_rerun_slug(const char *text, char *out,
                                    uint64_t capacity) {
  uint64_t length = 0u;
  for (const char *c = text; *c && length + 1u < capacity; ++c) {
    const char lower = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
    const bool8_t keep =
        (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
    out[length++] = keep ? lower : '_';
  }
  out[length] = '\0';
}

/* The parsed JSON file at `path`, or NULL. */
vkr_internal VkrBakeryJson *editor_rerun_read_json(Arena *arena,
                                                   const char *path) {
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return NULL;
  }
  VkrBakeryJson *json =
      mapping.size <= EDITOR_RERUN_FILE_MAX
          ? vkr_bakery_json_parse(arena, (const uint8_t *)mapping.data,
                                  mapping.size, 32u, NULL)
          : NULL;
  file_unmap(&mapping);
  return json && json->type == VKR_BAKERY_JSON_OBJECT ? json : NULL;
}

/* ------------------------------------------------------------------------ */
/* Case                                                                      */

typedef struct EditorRerunSource {
  char bundle[VKR_RECORDING_PATH_MAX];
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  char scene[VKR_RECORDING_PATH_MAX];
  uint64_t frame_count;
  float64_t first_time;
  float64_t last_time;
} EditorRerunSource;

/* The recording's scene, frame count and the range's times; the scene must
   be one of the repository's, which the harness can load. */
vkr_internal bool8_t editor_rerun_source(Arena *arena, const char *bundle,
                                         uint64_t first, uint64_t *last,
                                         EditorRerunSource *out, char *error,
                                         uint64_t error_capacity) {
  char summary_path[VKR_RECORDING_PATH_MAX];
  string_format(summary_path, sizeof(summary_path), "%s/%s", bundle,
                VKR_RECORDING_SUMMARY_FILE);
  const VkrBakeryJson *summary = editor_rerun_read_json(arena, summary_path);
  String8 scene = {0};
  if (!summary || !vkr_bakery_json_get_string(summary, "scene", &scene) ||
      scene.length == 0u) {
    string_format(error, error_capacity,
                  "The recording has no summary.json naming its scene.");
    return false_v;
  }
  string_format(out->scene, sizeof(out->scene), "%.*s", (int32_t)scene.length,
                (const char *)scene.str);
  const FilePath scene_file = file_path_view(out->scene);
  if (out->scene[0] == '/' || string_contains(out->scene, "..") ||
      !file_exists(&scene_file)) {
    string_format(error, error_capacity,
                  "The harness loads the repository's scenes; '%s' is not one.",
                  out->scene);
    return false_v;
  }
  /* The range's times come from the recorded frames. */
  VkrDMemory memory;
  if (!vkr_dmemory_create(MB(4), GB(1), &memory)) {
    string_format(error, error_capacity, "Out of memory.");
    return false_v;
  }
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrRecordingData data;
  bool8_t ok = vkr_recording_data_load(&data, &allocator, bundle,
                                       vkr_content_codec_zstd_decoder()) ==
               VKR_RECORDING_DATA_OK;
  if (ok) {
    out->frame_count = data.frame_count;
    *last = Min(*last, data.frame_count - 1u);
    ok = first <= *last;
    if (ok) {
      out->first_time = data.times[first];
      out->last_time = data.times[*last];
    } else {
      string_format(error, error_capacity,
                    "The range starts after the recording's %llu frames.",
                    (unsigned long long)data.frame_count);
    }
    vkr_recording_data_free(&data);
  } else {
    string_format(error, error_capacity, "'%s' is not a readable recording.",
                  bundle);
  }
  vkr_dmemory_allocator_destroy(&allocator);
  return ok;
}

/* The template with the re-run's identity, scene, measured frames and
   recording camera, without its captures and assertions, which describe
   another workload. */
vkr_internal bool8_t editor_rerun_case(Arena *arena,
                                       const EditorRerunSource *source,
                                       const char *slug, const char *recording,
                                       uint64_t first, uint64_t last,
                                       VkrEditorRecordingRerun *out,
                                       char *error, uint64_t error_capacity) {
  VkrBakeryJson *json = editor_rerun_read_json(arena, EDITOR_RERUN_TEMPLATE);
  float64_t fixed_delta = 0.0;
  const VkrBakeryJson *frames =
      json ? vkr_bakery_json_get(json, "frames") : NULL;
  int64_t warmup = 0;
  if (!json || !vkr_bakery_json_get_number(json, "fixed_delta", &fixed_delta) ||
      fixed_delta <= 0.0 || !frames ||
      !vkr_bakery_json_get_int(frames, "warmup", &warmup)) {
    string_format(error, error_capacity, "The template case %s cannot be read.",
                  EDITOR_RERUN_TEMPLATE);
    return false_v;
  }
  /* Every recorded second at the case's fixed delta, its last frame too. */
  out->seconds = source->last_time - source->first_time;
  out->measure_frames = (uint64_t)vkr_ceil_f64(out->seconds / fixed_delta) + 1u;
  char id[160];
  string_format(id, sizeof(id), EDITOR_RERUN_SUITE ".%s", slug);
  vkr_bakery_json_set(arena, json, "id", vkr_bakery_json_cstr(arena, id));
  vkr_bakery_json_set(arena, json, "suite",
                      vkr_bakery_json_cstr(arena, EDITOR_RERUN_SUITE));
  char description[256];
  string_format(description, sizeof(description),
                "Perf re-run of recording %s, frames %llu to %llu, with %s's "
                "settings.",
                source->name, (unsigned long long)first,
                (unsigned long long)last, EDITOR_RERUN_TEMPLATE);
  vkr_bakery_json_set(arena, json, "description",
                      vkr_bakery_json_cstr(arena, description));
  vkr_bakery_json_set(arena, json, "scene",
                      vkr_bakery_json_cstr(arena, source->scene));
  VkrBakeryJson *measured = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, measured, "warmup",
                      vkr_bakery_json_int(arena, warmup));
  vkr_bakery_json_set(arena, measured, "measure",
                      vkr_bakery_json_int(arena, (int64_t)out->measure_frames));
  vkr_bakery_json_set(arena, json, "frames", measured);
  VkrBakeryJson *camera = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, camera, "mode",
                      vkr_bakery_json_cstr(arena, "recording"));
  vkr_bakery_json_set(arena, camera, "recording",
                      vkr_bakery_json_cstr(arena, recording));
  vkr_bakery_json_set(arena, camera, "first_frame",
                      vkr_bakery_json_int(arena, (int64_t)first));
  vkr_bakery_json_set(arena, camera, "last_frame",
                      vkr_bakery_json_int(arena, (int64_t)last));
  vkr_bakery_json_set(arena, json, "camera", camera);
  (void)vkr_bakery_json_remove(json, "captures");
  (void)vkr_bakery_json_remove(json, "assertions");

  String8 text = {0};
  if (!vkr_bakery_json_write(arena, json, VKR_BAKERY_JSON_PRETTY, &text)) {
    string_format(error, error_capacity, "The case could not be written.");
    return false_v;
  }
  string_format(out->case_path, sizeof(out->case_path), "%s/rerun_%s.case.json",
                EDITOR_RERUN_CASES, slug);
  FileHandle handle = {0};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  const FilePath path = file_path_view(out->case_path);
  uint64_t written = 0u;
  bool8_t ok = file_open(&path, mode, &handle) == FILE_ERROR_NONE;
  if (ok) {
    ok = file_write(&handle, text.length, text.str, &written) ==
             FILE_ERROR_NONE &&
         written == text.length;
    file_close(&handle);
  }
  if (!ok) {
    string_format(error, error_capacity, "%s could not be written.",
                  out->case_path);
  }
  return ok;
}

bool8_t vkr_editor_recording_rerun(VkrEditorUi *editor, const char *name,
                                   uint64_t first, uint64_t last,
                                   VkrEditorRecordingRerun *out, char *error,
                                   uint64_t error_capacity) {
  MemZero(out, sizeof(*out));
  /* The case and the harness are the repository's; the editor must run from
     its root. */
  const FilePath template_file = file_path_view(EDITOR_RERUN_TEMPLATE);
  const FilePath cases = file_path_view(EDITOR_RERUN_CASES);
  if (!file_exists(&template_file) || !file_create_directories(&cases)) {
    string_format(error, error_capacity,
                  "Perf re-runs need the editor started from the VKR "
                  "repository, which holds %s.",
                  EDITOR_RERUN_TEMPLATE);
    return false_v;
  }
  EditorRerunSource source = {0};
  if (!vkr_editor_recording_resolve(editor, name, source.bundle,
                                    sizeof(source.bundle))) {
    string_format(error, error_capacity, "No recording named '%s'.",
                  name ? name : "");
    return false_v;
  }
  /* The bundle's name, without its directory. */
  const char *base = source.bundle;
  for (const char *c = source.bundle; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      base = c + 1;
    }
  }
  string_format(source.name, sizeof(source.name), "%s", base);
  const uint64_t name_length = string_length(source.name);
  const uint64_t extension = sizeof(VKR_RECORDING_BUNDLE_EXTENSION) - 1u;
  const bool8_t bundle_name =
      name_length > extension &&
      string_equals(source.name + name_length - extension,
                    VKR_RECORDING_BUNDLE_EXTENSION);
  Arena *arena = arena_create(MB(16), KB(64));
  if (!arena) {
    string_format(error, error_capacity, "Out of memory.");
    return false_v;
  }
  bool8_t ok = editor_rerun_source(arena, source.bundle, first, &last, &source,
                                   error, error_capacity);
  char slug[96] = {0};
  char directory[VKR_RECORDING_PATH_MAX];
  if (ok) {
    /* The case's id and file take the name without its extension. */
    char stem[VKR_RECORDING_LABEL_MAX + 1u];
    string_format(
        stem, sizeof(stem), "%.*s",
        (int32_t)(bundle_name ? name_length - extension : name_length),
        source.name);
    editor_rerun_slug(stem, slug, sizeof(slug));
    string_format(directory, sizeof(directory), "%s/%s", EDITOR_RERUN_ARTIFACTS,
                  source.name);
    string_format(out->recording_path, sizeof(out->recording_path), "%s",
                  directory);
    const FilePath target = file_path_view(directory);
    ok = file_create_directories(&target);
    const char *const files[] = {VKR_RECORDING_STREAM_FILE,
                                 VKR_RECORDING_SUMMARY_FILE};
    for (uint32_t i = 0u; ok && i < ArrayCount(files); ++i) {
      char from[VKR_RECORDING_PATH_MAX];
      char to[VKR_RECORDING_PATH_MAX];
      string_format(from, sizeof(from), "%s/%s", source.bundle, files[i]);
      string_format(to, sizeof(to), "%s/%s", directory, files[i]);
      ok = editor_rerun_copy(from, to);
    }
    if (!ok) {
      string_format(error, error_capacity,
                    "The recording could not be copied to %s.", directory);
    }
  }
  if (ok) {
    ok = editor_rerun_case(arena, &source, slug, out->recording_path, first,
                           last, out, error, error_capacity);
  }
  arena_destroy(arena);
  if (!ok) {
    return false_v;
  }
  out->first_frame = first;
  out->last_frame = last;
  string_format(out->command, sizeof(out->command),
                "env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u "
                "VK_INSTANCE_LAYERS ./build_release/tools/vkr_harness profile "
                "--case %s --profile %s",
                out->case_path, EDITOR_RERUN_PROFILE);
  return true_v;
}
