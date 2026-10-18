#include "editor_recording.h"

#include "containers/vkr_hashtable.h"
#include "containers/vkr_sort.h"
#include "core/vkr_json_writer.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_json.h"

/* Recording requests (docs/proposals/recordings.md, "Recording requests"):
   an agent writes a request into the recording directory's requests/; the
   user starts or declines it in the Recordings window; while it records,
   its question and steps stay over the Scene; when the recording stops, the
   request names it. Each request is one small JSON file, rewritten whole on
   every change, so the agent and the editor read the same state. */

/* Seconds between reads of the requests directory. */
#define EDITOR_REQUEST_READ_PERIOD 2.0
/* Seconds a started request waits for its recording before it returns to
   pending. */
#define EDITOR_REQUEST_START_LIMIT 5.0
#define EDITOR_REQUEST_FILE_BYTES (64u * 1024u)

static const char *const EDITOR_REQUEST_STATUS[] = {
    "pending", "accepted", "declined", "recorded", "cancelled", "expired",
};

_Static_assert(ArrayCount(EDITOR_REQUEST_STATUS) ==
                   VKR_EDITOR_RECORDING_REQUEST_STATUS_COUNT,
               "a name for every request status");

const char *vkr_editor_recording_request_status_name(
    VkrEditorRecordingRequestStatus status) {
  return (uint32_t)status < ArrayCount(EDITOR_REQUEST_STATUS)
             ? EDITOR_REQUEST_STATUS[status]
             : "unknown";
}

/* ------------------------------------------------------------------------ */
/* Files                                                                     */

/* <recording directory>/requests, created with its parents. */
vkr_internal bool8_t editor_request_directory(VkrEditorUi *editor, char *out,
                                              uint64_t capacity,
                                              bool8_t create) {
  char recordings[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_directory(editor, recordings, sizeof(recordings)) ||
      string_format(out, capacity, "%s/requests", recordings) >=
          (int32_t)capacity) {
    return false_v;
  }
  if (!create) {
    return true_v;
  }
  const FilePath directory = file_path_view(out);
  return file_create_directories(&directory);
}

/* Ids are letters, digits, '-' and '_', so an agent's id stays a file
   name inside requests/. */
vkr_internal bool8_t editor_request_id_valid(const char *id) {
  if (!id || !id[0]) {
    return false_v;
  }
  for (const char *c = id; *c; ++c) {
    const bool8_t letter = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z');
    const bool8_t digit = *c >= '0' && *c <= '9';
    if (!letter && !digit && *c != '-' && *c != '_') {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t editor_request_path(VkrEditorUi *editor, const char *id,
                                         char *out, uint64_t capacity) {
  if (!editor_request_id_valid(id)) {
    return false_v;
  }
  char directory[VKR_RECORDING_PATH_MAX];
  return editor_request_directory(editor, directory, sizeof(directory),
                                  false_v) &&
         string_format(out, capacity, "%s/%s.json", directory, id) <
             (int32_t)capacity;
}

vkr_internal String8 editor_request_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

vkr_internal void editor_request_text(VkrJsonWriter *json, const char *name,
                                      const char *value) {
  vkr_json_writer_name(json, editor_request_str(name));
  vkr_json_writer_string(json, editor_request_str(value));
}

vkr_internal void editor_request_number(VkrJsonWriter *json, const char *name,
                                        float64_t value) {
  vkr_json_writer_name(json, editor_request_str(name));
  vkr_json_writer_f64(json, value);
}

/* Writes the whole request; the file writer replaces the old one at once. */
vkr_internal bool8_t editor_request_write(
    VkrEditorUi *editor, const VkrEditorRecordingRequest *request) {
  char directory[VKR_RECORDING_PATH_MAX];
  char path[VKR_RECORDING_PATH_MAX];
  if (!editor_request_directory(editor, directory, sizeof(directory), true_v) ||
      !editor_request_path(editor, request->id, path, sizeof(path))) {
    return false_v;
  }
  VkrJsonFileWriter file;
  if (!vkr_json_file_writer_begin(&file, editor_request_str(path))) {
    return false_v;
  }
  VkrJsonWriter *json = &file.writer;
  vkr_json_writer_begin_object(json);
  editor_request_text(json, "kind", "vkr.recording.request");
  editor_request_number(json, "version", 1.0);
  editor_request_text(json, "id", request->id);
  editor_request_number(json, "created", (float64_t)request->created);
  editor_request_number(json, "expires", (float64_t)request->expires);
  editor_request_text(json, "owner", request->owner);
  editor_request_text(
      json, "status",
      vkr_editor_recording_request_status_name(request->status));
  editor_request_text(json, "scene", request->scene);
  if (request->has_pose) {
    vkr_json_writer_name(json, string8_lit("start_pose"));
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, string8_lit("position"));
    vkr_json_writer_begin_array(json);
    vkr_json_writer_f64(json, request->position.x);
    vkr_json_writer_f64(json, request->position.y);
    vkr_json_writer_f64(json, request->position.z);
    vkr_json_writer_end_array(json);
    editor_request_number(json, "yaw", request->yaw);
    editor_request_number(json, "pitch", request->pitch);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_name(json, string8_lit("steps"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < request->step_count; ++i) {
    vkr_json_writer_string(json, editor_request_str(request->steps[i]));
  }
  vkr_json_writer_end_array(json);
  editor_request_text(json, "tier",
                      request->tier == VKR_RECORDING_TIER_NONE ? "none"
                      : request->tier == VKR_RECORDING_TIER_0  ? "0"
                                                               : "1");
  editor_request_text(json, "measurements",
                      request->all_measurements ? "all" : "standard");
  editor_request_number(json, "stop_seconds", request->stop_seconds);
  editor_request_text(json, "question", request->question);
  editor_request_text(json, "recording", request->recording);
  editor_request_text(json, "decline_reason", request->decline_reason);
  vkr_json_writer_end_object(json);
  return vkr_json_file_writer_commit(&file);
}

vkr_internal void editor_request_copy(const VkrBakeryJson *object,
                                      const char *key, char *out,
                                      uint64_t capacity) {
  String8 value = {0};
  out[0] = '\0';
  if (vkr_bakery_json_get_string(object, key, &value)) {
    string_format(out, capacity, "%.*s", (int32_t)value.length,
                  (const char *)value.str);
  }
}

vkr_internal float64_t editor_request_get_number(const VkrBakeryJson *object,
                                                 const char *key) {
  float64_t value = 0.0;
  return vkr_bakery_json_get_number(object, key, &value) ? value : 0.0;
}

vkr_internal float32_t editor_request_element(const VkrBakeryJson *array,
                                              uint32_t index) {
  const VkrBakeryJson *value = vkr_bakery_json_at(array, index);
  if (value && value->type == VKR_BAKERY_JSON_INT) {
    return (float32_t)value->integer;
  }
  return value && value->type == VKR_BAKERY_JSON_FLOAT
             ? (float32_t)value->number
             : 0.0f;
}

/* Fills a request from its parsed file. */
vkr_internal bool8_t editor_request_parse(const VkrBakeryJson *root,
                                          VkrEditorRecordingRequest *out) {
  MemZero(out, sizeof(*out));
  if (!root || root->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(root, "kind"),
                                 "vkr.recording.request")) {
    return false_v;
  }
  editor_request_copy(root, "id", out->id, sizeof(out->id));
  out->created = (int64_t)editor_request_get_number(root, "created");
  out->expires = (int64_t)editor_request_get_number(root, "expires");
  editor_request_copy(root, "owner", out->owner, sizeof(out->owner));
  char status[16];
  editor_request_copy(root, "status", status, sizeof(status));
  out->status = VKR_EDITOR_RECORDING_REQUEST_STATUS_COUNT;
  for (uint32_t i = 0u; i < ArrayCount(EDITOR_REQUEST_STATUS); ++i) {
    if (string_equals(status, EDITOR_REQUEST_STATUS[i])) {
      out->status = (VkrEditorRecordingRequestStatus)i;
    }
  }
  editor_request_copy(root, "scene", out->scene, sizeof(out->scene));
  const VkrBakeryJson *pose = vkr_bakery_json_get(root, "start_pose");
  const VkrBakeryJson *position =
      pose ? vkr_bakery_json_get(pose, "position") : NULL;
  if (position && position->type == VKR_BAKERY_JSON_ARRAY &&
      position->count == 3u) {
    out->has_pose = true_v;
    out->position = vec3_new(editor_request_element(position, 0u),
                             editor_request_element(position, 1u),
                             editor_request_element(position, 2u));
    out->yaw = (float32_t)editor_request_get_number(pose, "yaw");
    out->pitch = (float32_t)editor_request_get_number(pose, "pitch");
  }
  const VkrBakeryJson *steps = vkr_bakery_json_get(root, "steps");
  for (uint32_t i = 0u;
       steps && steps->type == VKR_BAKERY_JSON_ARRAY && i < steps->count &&
       out->step_count < VKR_EDITOR_RECORDING_REQUEST_STEP_MAX;
       ++i) {
    const VkrBakeryJson *step = vkr_bakery_json_at(steps, i);
    if (step && step->type == VKR_BAKERY_JSON_STRING) {
      string_format(out->steps[out->step_count++], sizeof(out->steps[0]),
                    "%.*s", (int32_t)step->string.length,
                    (const char *)step->string.str);
    }
  }
  char tier[8];
  editor_request_copy(root, "tier", tier, sizeof(tier));
  out->tier = string_equals(tier, "none") ? VKR_RECORDING_TIER_NONE
              : string_equals(tier, "0")  ? VKR_RECORDING_TIER_0
                                          : VKR_RECORDING_TIER_1;
  char measurements[16];
  editor_request_copy(root, "measurements", measurements, sizeof(measurements));
  out->all_measurements = string_equals(measurements, "all");
  out->stop_seconds = (uint32_t)editor_request_get_number(root, "stop_seconds");
  editor_request_copy(root, "question", out->question, sizeof(out->question));
  editor_request_copy(root, "recording", out->recording,
                      sizeof(out->recording));
  editor_request_copy(root, "decline_reason", out->decline_reason,
                      sizeof(out->decline_reason));
  return out->id[0] != '\0' &&
         out->status != VKR_EDITOR_RECORDING_REQUEST_STATUS_COUNT;
}

/* Reads one request file into `out`. */
vkr_internal bool8_t editor_request_load(const char *path,
                                         VkrEditorRecordingRequest *out) {
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return false_v;
  }
  bool8_t ok = false_v;
  Arena *arena = mapping.size <= EDITOR_REQUEST_FILE_BYTES
                     ? arena_create(KB(256), KB(64))
                     : NULL;
  if (arena) {
    VkrBakeryJsonError error = {0};
    const VkrBakeryJson *root = vkr_bakery_json_parse(
        arena, (const uint8_t *)mapping.data, mapping.size, 8u, &error);
    ok = editor_request_parse(root, out);
    arena_destroy(arena);
  }
  file_unmap(&mapping);
  return ok;
}

bool8_t vkr_editor_recording_request_read(VkrEditorUi *editor, const char *id,
                                          VkrEditorRecordingRequest *out) {
  char path[VKR_RECORDING_PATH_MAX];
  return id && id[0] && editor_request_path(editor, id, path, sizeof(path)) &&
         editor_request_load(path, out);
}

typedef struct EditorRequestListing {
  VkrEditorUi *editor;
  const char *directory;
  VkrEditorRecordingRequests *requests;
} EditorRequestListing;

vkr_internal bool8_t editor_request_list_visit(void *context, const char *name,
                                               bool8_t is_directory) {
  EditorRequestListing *listing = context;
  VkrEditorRecordingRequests *requests = listing->requests;
  const uint64_t length = string_length(name);
  if (is_directory || length < 6u ||
      !string_equals(name + length - 5u, ".json") ||
      requests->count == VKR_EDITOR_RECORDING_REQUEST_MAX) {
    return true_v;
  }
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s", listing->directory, name);
  VkrEditorRecordingRequest request;
  if (editor_request_load(path, &request)) {
    requests->items[requests->count++] = request;
  }
  return true_v;
}

vkr_internal int32_t editor_request_compare(const void *lhs, const void *rhs) {
  const VkrEditorRecordingRequest *a = lhs;
  const VkrEditorRecordingRequest *b = rhs;
  return a->created > b->created ? -1 : (a->created < b->created ? 1 : 0);
}

void vkr_editor_recording_requests_list(VkrEditorUi *editor) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  requests->count = 0u;
  char directory[VKR_RECORDING_PATH_MAX];
  if (!editor_request_directory(editor, directory, sizeof(directory),
                                false_v)) {
    return;
  }
  EditorRequestListing listing = {
      .editor = editor, .directory = directory, .requests = requests};
  const FilePath path = file_path_view(directory);
  (void)file_list_directory(&path, editor_request_list_visit, &listing);
  vkr_sort(requests->items, requests->count, sizeof(VkrEditorRecordingRequest),
           editor_request_compare);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */

bool8_t vkr_editor_recording_request_create(
    VkrEditorUi *editor, const VkrEditorRecordingRequest *request,
    uint32_t expire_seconds, char *out_id, uint64_t id_capacity, char *error,
    uint64_t error_capacity) {
  if (!request->question[0]) {
    string_format(error, error_capacity,
                  "A request needs the question its recording answers.");
    return false_v;
  }
  VkrEditorRecordingRequest created = *request;
  created.created = vkr_platform_get_unix_time();
  created.expires = created.created + (int64_t)Max(expire_seconds, 60u);
  created.status = VKR_EDITOR_RECORDING_REQUEST_PENDING;
  created.recording[0] = '\0';
  created.decline_reason[0] = '\0';
  /* Seconds since 1970 and the clock's microseconds keep ids apart; a
     taken id moves to the next salt. */
  uint32_t salt =
      (uint32_t)((uint64_t)(vkr_platform_get_absolute_time() * 1000000.0) &
                 0xffffu);
  VkrEditorRecordingRequest taken;
  for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
    string_format(created.id, sizeof(created.id), "req-%lld-%04x",
                  (long long)created.created, (salt + attempt) & 0xffffu);
    if (!vkr_editor_recording_request_read(editor, created.id, &taken)) {
      break;
    }
  }
  if (!editor_request_write(editor, &created)) {
    string_format(error, error_capacity,
                  "The request could not be written to the requests "
                  "directory.");
    return false_v;
  }
  string_format(out_id, id_capacity, "%s", created.id);
  vkr_editor_recording_requests_list(editor);
  return true_v;
}

/* The request `id` when it is in one of `allowed` states, else false with a
   reason. */
vkr_internal bool8_t editor_request_take(VkrEditorUi *editor, const char *id,
                                         uint32_t allowed,
                                         VkrEditorRecordingRequest *out,
                                         char *error, uint64_t error_capacity) {
  if (!vkr_editor_recording_request_read(editor, id, out)) {
    string_format(error, error_capacity, "No request named '%s'.",
                  id ? id : "");
    return false_v;
  }
  if (!((allowed >> (uint32_t)out->status) & 1u)) {
    string_format(error, error_capacity, "The request is %s.",
                  vkr_editor_recording_request_status_name(out->status));
    return false_v;
  }
  return true_v;
}

/* Whether the open scene is the one a request names: either path may be
   relative, so one ending in the other matches. */
vkr_internal bool8_t editor_request_scene_open(
    const VkrEditorRecordingRequest *request, String8 scene_path) {
  if (!request->scene[0]) {
    return true_v;
  }
  const uint64_t wanted = string_length(request->scene);
  const uint64_t open = scene_path.length;
  if (open == 0u) {
    return false_v;
  }
  const uint64_t shorter = Min(wanted, open);
  return MemCompare(request->scene + wanted - shorter,
                    scene_path.str + open - shorter, shorter) == 0;
}

bool8_t vkr_editor_recording_request_start(VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           const char *id, char *error,
                                           uint64_t error_capacity) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrEditorRecordingRequest request;
  if (!editor_request_take(editor, id,
                           1u << VKR_EDITOR_RECORDING_REQUEST_PENDING, &request,
                           error, error_capacity)) {
    return false_v;
  }
  if (requests->active) {
    string_format(error, error_capacity,
                  "Another request is recording; stop it first.");
    return false_v;
  }
  if (frame->recording &&
      (frame->recording->recording || frame->recording->playback)) {
    string_format(error, error_capacity,
                  frame->recording->recording
                      ? "Stop the recording in progress first."
                      : "Close the playback first.");
    return false_v;
  }
  if (!editor_request_scene_open(&request, frame->scene_path)) {
    string_format(error, error_capacity, "Open %s to record this request.",
                  request.scene);
    return false_v;
  }
  if (request.has_pose && frame->editor_state_request) {
    VkrSampleEditorStateRequest *state = frame->editor_state_request;
    state->move_camera = true_v;
    state->camera_position = request.position;
    state->camera_yaw = request.yaw;
    state->camera_pitch = request.pitch;
  }
  if (!vkr_editor_recording_start(
          editor, request.id, request.owner[0] ? request.owner : "agent",
          request.tier, request.all_measurements, request.all_measurements,
          true_v, error, error_capacity)) {
    return false_v;
  }
  request.status = VKR_EDITOR_RECORDING_REQUEST_ACCEPTED;
  if (!editor_request_write(editor, &request)) {
    string_format(error, error_capacity,
                  "The request's file could not be updated.");
    return false_v;
  }
  requests->active = true_v;
  string_format(requests->active_id, sizeof(requests->active_id), "%s",
                request.id);
  requests->step = 0u;
  requests->started_at = vkr_platform_get_absolute_time();
  requests->stop_sent = false_v;
  requests->saw_recording = false_v;
  vkr_editor_recording_requests_list(editor);
  return true_v;
}

bool8_t vkr_editor_recording_request_decline(VkrEditorUi *editor,
                                             const char *id, const char *reason,
                                             char *error,
                                             uint64_t error_capacity) {
  VkrEditorRecordingRequest request;
  if (!editor_request_take(editor, id,
                           1u << VKR_EDITOR_RECORDING_REQUEST_PENDING, &request,
                           error, error_capacity)) {
    return false_v;
  }
  request.status = VKR_EDITOR_RECORDING_REQUEST_DECLINED;
  string_format(request.decline_reason, sizeof(request.decline_reason), "%s",
                reason ? reason : "");
  if (!editor_request_write(editor, &request)) {
    string_format(error, error_capacity,
                  "The request's file could not be updated.");
    return false_v;
  }
  vkr_editor_recording_requests_list(editor);
  return true_v;
}

bool8_t vkr_editor_recording_request_cancel(VkrEditorUi *editor, const char *id,
                                            char *error,
                                            uint64_t error_capacity) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrEditorRecordingRequest request;
  if (!editor_request_take(editor, id,
                           (1u << VKR_EDITOR_RECORDING_REQUEST_PENDING) |
                               (1u << VKR_EDITOR_RECORDING_REQUEST_ACCEPTED),
                           &request, error, error_capacity)) {
    return false_v;
  }
  /* A request being recorded keeps its recording; the user stops it. */
  if (requests->active && string_equals(requests->active_id, request.id)) {
    requests->active = false_v;
  }
  request.status = VKR_EDITOR_RECORDING_REQUEST_CANCELLED;
  if (!editor_request_write(editor, &request)) {
    string_format(error, error_capacity,
                  "The request's file could not be updated.");
    return false_v;
  }
  vkr_editor_recording_requests_list(editor);
  return true_v;
}

/* The active request in the list, or NULL. */
vkr_internal VkrEditorRecordingRequest *
editor_request_active(VkrEditorUi *editor) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  if (!requests->active) {
    return NULL;
  }
  for (uint32_t i = 0u; i < requests->count; ++i) {
    if (string_equals(requests->items[i].id, requests->active_id)) {
      return &requests->items[i];
    }
  }
  return NULL;
}

bool8_t vkr_editor_recording_request_next_step(VkrEditorUi *editor) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  const VkrEditorRecordingRequest *request = editor_request_active(editor);
  if (!request || requests->step >= request->step_count) {
    return false_v;
  }
  VkrSampleRecordingRequest mark = {
      .command = VKR_SAMPLE_RECORDING_MARK,
      .marker_kind = VKR_RECORDING_MARKER_STEP,
  };
  string_format(mark.label, sizeof(mark.label), "step %u: %s",
                requests->step + 1u, request->steps[requests->step]);
  (void)vkr_editor_recording_send(editor, mark, false_v);
  ++requests->step;
  return true_v;
}

/* A pending request appears once in a toast. */
vkr_internal void
editor_request_announce(VkrEditorUi *editor,
                        const VkrEditorRecordingRequest *request) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  const uint64_t key =
      vkr_hash_table_key_cstr(request->id).hash ^ (uint64_t)request->created;
  for (uint32_t i = 0u; i < requests->announced_count; ++i) {
    if (requests->announced[i] == key) {
      return;
    }
  }
  if (requests->announced_count == VKR_EDITOR_RECORDING_REQUEST_MAX) {
    MemCopy(requests->announced, requests->announced + 1u,
            sizeof(requests->announced[0]) *
                (VKR_EDITOR_RECORDING_REQUEST_MAX - 1u));
    --requests->announced_count;
  }
  requests->announced[requests->announced_count++] = key;
  char text[192];
  string_format(text, sizeof(text),
                "%s asks for a recording: %.120s Open Recordings to start "
                "it.",
                request->owner[0] ? request->owner : "An agent",
                request->question);
  vkr_editor_toast(editor, VKR_UI_ICON_RECORD, vkr_ui_theme()->accent, text);
}

void vkr_editor_recording_requests_update(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  const float64_t now = vkr_platform_get_absolute_time();
  const bool8_t recording =
      frame->recording != NULL && frame->recording->recording;

  /* The active request: its stop rule, and its end when the recording
     stops. */
  VkrEditorRecordingRequest *active = editor_request_active(editor);
  if (requests->active && recording) {
    requests->saw_recording = true_v;
    if (active) {
      string_format(active->recording, sizeof(active->recording), "%s",
                    frame->recording->recording_path);
    }
    if (active && active->stop_seconds > 0u && !requests->stop_sent &&
        now - requests->started_at >= (float64_t)active->stop_seconds) {
      requests->stop_sent = true_v;
      (void)vkr_editor_recording_send(
          editor,
          (VkrSampleRecordingRequest){.command = VKR_SAMPLE_RECORDING_STOP},
          true_v);
    }
  } else if (requests->active && !requests->saw_recording &&
             now - requests->started_at >= EDITOR_REQUEST_START_LIMIT) {
    /* The recording never started; the request waits for another try. */
    VkrEditorRecordingRequest unstarted;
    if (vkr_editor_recording_request_read(editor, requests->active_id,
                                          &unstarted) &&
        unstarted.status == VKR_EDITOR_RECORDING_REQUEST_ACCEPTED) {
      unstarted.status = VKR_EDITOR_RECORDING_REQUEST_PENDING;
      (void)editor_request_write(editor, &unstarted);
    }
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "The request's recording did not start.");
    requests->active = false_v;
    active = NULL;
    vkr_editor_recording_requests_list(editor);
  } else if (requests->active && requests->saw_recording) {
    VkrEditorRecordingRequest finished;
    if (vkr_editor_recording_request_read(editor, requests->active_id,
                                          &finished) &&
        finished.status == VKR_EDITOR_RECORDING_REQUEST_ACCEPTED) {
      finished.status = VKR_EDITOR_RECORDING_REQUEST_RECORDED;
      if (active) {
        string_format(finished.recording, sizeof(finished.recording), "%s",
                      active->recording);
      }
      (void)editor_request_write(editor, &finished);
      vkr_editor_toast(editor, VKR_UI_ICON_CHECK, vkr_ui_theme()->success,
                       "Recorded the request; its agent can read it now.");
    }
    requests->active = false_v;
    active = NULL;
    vkr_editor_recording_requests_list(editor);
  }

  if (now - requests->read_at < EDITOR_REQUEST_READ_PERIOD) {
    return;
  }
  requests->read_at = now;
  /* Recordings keep their active request's last copy across the read. */
  char active_recording[VKR_RECORDING_PATH_MAX] = {0};
  if (active) {
    string_format(active_recording, sizeof(active_recording), "%s",
                  active->recording);
  }
  vkr_editor_recording_requests_list(editor);
  active = editor_request_active(editor);
  if (active && active_recording[0]) {
    string_format(active->recording, sizeof(active->recording), "%s",
                  active_recording);
  }
  const int64_t unix_now = vkr_platform_get_unix_time();
  for (uint32_t i = 0u; i < requests->count; ++i) {
    VkrEditorRecordingRequest *request = &requests->items[i];
    if (request->status != VKR_EDITOR_RECORDING_REQUEST_PENDING) {
      continue;
    }
    if (request->expires > 0 && unix_now >= request->expires) {
      request->status = VKR_EDITOR_RECORDING_REQUEST_EXPIRED;
      (void)editor_request_write(editor, request);
      continue;
    }
    editor_request_announce(editor, request);
  }
}

/* ------------------------------------------------------------------------ */
/* Recordings window                                                         */

vkr_internal VkrUiPlacement editor_request_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* Shown requests: pending ones and the one recording. */
vkr_internal bool8_t
editor_request_shown(const VkrEditorRecordingRequest *request) {
  return request->status == VKR_EDITOR_RECORDING_REQUEST_PENDING ||
         request->status == VKR_EDITOR_RECORDING_REQUEST_ACCEPTED;
}

uint32_t vkr_editor_recording_requests_shown(struct VkrEditorUi *editor) {
  const VkrEditorRecordingRequests *requests = &editor->recording.requests;
  uint32_t shown = 0u;
  for (uint32_t i = 0u; i < requests->count; ++i) {
    shown += editor_request_shown(&requests->items[i]) ? 1u : 0u;
  }
  return shown;
}

/* The facts line: who asks, the steps, the scene, the stop rule and the
   time left; a part with nothing to say is left out. */
vkr_internal String8 editor_request_facts(
    VkrUiSystem *ui, const VkrEditorRecordingRequest *request) {
  const char *scene = request->scene[0] ? request->scene : "any scene";
  for (const char *c = request->scene; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      scene = c + 1;
    }
  }
  /* The file name without its .scene.json. */
  uint64_t scene_length = string_length(scene);
  const uint64_t suffix = string_length(".scene.json");
  if (scene_length > suffix &&
      string_equals(scene + scene_length - suffix, ".scene.json")) {
    scene_length -= suffix;
  }
  char steps[24] = "";
  if (request->step_count > 0u) {
    string_format(steps, sizeof(steps), "  \xc2\xb7  %u step%s",
                  request->step_count, request->step_count == 1u ? "" : "s");
  }
  char stop[32] = "";
  if (request->stop_seconds > 0u) {
    string_format(stop, sizeof(stop), "  \xc2\xb7  stops after %u s",
                  request->stop_seconds);
  }
  const int64_t left = request->expires - vkr_platform_get_unix_time();
  char expiry[32] = "";
  if (request->status == VKR_EDITOR_RECORDING_REQUEST_PENDING && left > 0) {
    string_format(expiry, sizeof(expiry), "  \xc2\xb7  %lld min left",
                  (long long)((left + 59) / 60));
  }
  return string8_create_formatted(
      ui->frame_allocator, "%s%s  \xc2\xb7  %.*s%s%s",
      request->owner[0] ? request->owner : "agent", steps,
      (int32_t)scene_length, scene, stop, expiry);
}

/* The facts line's tooltip spells out what the short line abbreviates. */
vkr_internal String8 editor_request_facts_tooltip(
    VkrUiSystem *ui, const VkrEditorRecordingRequest *request) {
  char stop[48] = "You stop the recording.";
  if (request->stop_seconds > 0u) {
    string_format(stop, sizeof(stop), "It stops itself after %u s.",
                  request->stop_seconds);
  }
  return string8_create_formatted(
      ui->frame_allocator, "From %s, request %s\nScene: %s\n%s",
      request->owner[0] ? request->owner : "an agent", request->id,
      request->scene[0] ? request->scene : "any", stop);
}

/* A text button sized as every Recordings action is. */
vkr_internal VkrUiWidgetConfig editor_request_button(VkrEditorUi *editor,
                                                     uint32_t column,
                                                     bool8_t primary,
                                                     VkrUiIcon icon) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = editor_request_cell(column, 0u);
  if (primary) {
    vkr_editor_primary_style(&button, editor->heading_font);
  } else {
    vkr_editor_action_style(&button, editor->heading_font);
  }
  button.icon = icon;
  vkr_editor_button_size(&button);
  return button;
}

/* The Decline row: a reason field with Decline and Back. */
vkr_internal void
editor_request_decline_row(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                           const VkrEditorRecordingRequest *request) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO},
                                {.unit = VKR_UI_TRACK_AUTO}};
  const VkrUiTrack row = {.unit = VKR_UI_TRACK_AUTO};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = editor_request_cell(0u, 2u);
  panel.placement.column_span = 2u;
  panel.columns = columns;
  panel.column_count = ArrayCount(columns);
  panel.rows = &row;
  panel.row_count = 1u;
  panel.style.gap_pt = theme->space_md;
  panel.style.padding_pt.top = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("decline"), &panel)) {
    return;
  }
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = editor_request_cell(0u, 0u);
  vkr_editor_field_style(&field);
  /* As tall as the buttons beside it. */
  vkr_editor_button_size(&field);
  field.style.padding_pt.left = field.style.padding_pt.right = 8.0f;
  field.tooltip = string8_lit("Why not? The agent reads this; it may be "
                              "empty");
  VkrUiTextEditBuffer buffer = {.data = (uint8_t *)requests->decline_text,
                                .length = requests->decline_length,
                                .capacity = sizeof(requests->decline_text)};
  (void)vkr_ui_text_field(ui, string8_lit("reason"), &buffer, &field);
  requests->decline_length = buffer.length;
  VkrUiWidgetConfig send =
      editor_request_button(editor, 1u, false_v, VKR_UI_ICON_CLOSE);
  send.style.text_color = theme->error;
  send.icon_color = theme->error;
  send.tooltip = string8_lit("Decline the request with this reason");
  if (vkr_ui_button(ui, string8_lit("send"), string8_lit("Decline"), &send)) {
    char error[160];
    requests->decline_text[requests->decline_length] = '\0';
    if (!vkr_editor_recording_request_decline(editor, request->id,
                                              requests->decline_text, error,
                                              sizeof(error))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning, error);
    }
    requests->decline_id[0] = '\0';
    requests->decline_length = 0u;
  }
  VkrUiWidgetConfig back =
      editor_request_button(editor, 2u, false_v, VKR_UI_ICON_NONE);
  vkr_editor_ghost_style(&back);
  back.tooltip = string8_lit("Keep the request");
  if (vkr_ui_button(ui, string8_lit("back"), string8_lit("Back"), &back)) {
    requests->decline_id[0] = '\0';
    requests->decline_length = 0u;
  }
  (void)vkr_ui_panel_end(ui);
}

/* Start and Decline; or, while the request records, its next step. */
vkr_internal void
editor_request_actions(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                       const VkrEditorRecordingRequest *request,
                       bool8_t recording) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.unit = VKR_UI_TRACK_AUTO},
                                {.unit = VKR_UI_TRACK_AUTO}};
  const VkrUiTrack row = {.unit = VKR_UI_TRACK_AUTO};
  VkrUiPanelConfig buttons = vkr_ui_panel_config_default();
  buttons.placement = editor_request_cell(1u, 0u);
  buttons.placement.row_span = 2u;
  buttons.placement.justify = VKR_UI_ALIGN_END;
  buttons.columns = columns;
  buttons.column_count = recording ? 1u : 2u;
  buttons.rows = &row;
  buttons.row_count = 1u;
  buttons.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(ui, string8_lit("buttons"), &buttons)) {
    return;
  }
  if (recording) {
    VkrUiWidgetConfig next =
        editor_request_button(editor, 0u, true_v, VKR_UI_ICON_CHECK);
    next.disabled = requests->step >= request->step_count;
    char key[32] = "";
    (void)vkr_editor_command_shortcut(CMD_REQUEST_NEXT_STEP, key, sizeof(key));
    next.tooltip = string8_create_formatted(ui->frame_allocator,
                                            "Mark the current step done (%s)",
                                            key[0] ? key : "no shortcut");
    const String8 label = request->step_count == 0u ? string8_lit("Recording")
                          : requests->step < request->step_count
                              ? string8_create_formatted(
                                    ui->frame_allocator, "Finish step %u of %u",
                                    requests->step + 1u, request->step_count)
                              : string8_lit("All steps done");
    if (vkr_ui_button(ui, string8_lit("next"), label, &next)) {
      (void)vkr_editor_recording_request_next_step(editor);
    }
  } else {
    VkrUiWidgetConfig start =
        editor_request_button(editor, 0u, true_v, VKR_UI_ICON_RECORD);
    start.disabled = requests->active;
    start.tooltip = requests->active
                        ? string8_lit("Another request is recording")
                        : string8_lit("Place the camera and start recording");
    if (vkr_ui_button(ui, string8_lit("start"), string8_lit("Start"), &start)) {
      char error[192];
      if (!vkr_editor_recording_request_start(editor, frame, request->id, error,
                                              sizeof(error))) {
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                         error);
      }
    }
    VkrUiWidgetConfig decline =
        editor_request_button(editor, 1u, false_v, VKR_UI_ICON_NONE);
    decline.tooltip = string8_lit("Tell the agent you will not record it");
    if (vkr_ui_button(ui, string8_lit("decline"), string8_lit("Decline"),
                      &decline)) {
      string_format(requests->decline_id, sizeof(requests->decline_id), "%s",
                    request->id);
      requests->decline_length = 0u;
    }
  }
  (void)vkr_ui_panel_end(ui);
}

/* Points of a card that are not its question: padding, borders, the
   buttons and the gap before them. */
#define EDITOR_REQUEST_CARD_CHROME_PT 236.0f

/* One request: the question over its facts, with the actions at the right
   and, while declining, the reason row below. */
vkr_internal void editor_request_card(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const VkrEditorRecordingRequest *request,
                                      uint32_t row, float32_t width_pt) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t recording =
      request->status == VKR_EDITOR_RECORDING_REQUEST_ACCEPTED;
  const bool8_t declining =
      !recording && string_equals(requests->decline_id, request->id);
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO}};
  const VkrUiTrack rows[] = {{.unit = VKR_UI_TRACK_AUTO},
                             {.unit = VKR_UI_TRACK_AUTO},
                             {.unit = VKR_UI_TRACK_AUTO}};
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement = editor_request_cell(0u, row);
  card.columns = columns;
  card.column_count = ArrayCount(columns);
  card.rows = rows;
  card.row_count = declining ? 3u : 2u;
  card.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 14.0f};
  card.style.gap_pt = theme->space_sm;
  card.style.background_color = theme->raised;
  /* The accent edge says who acts next: the user, or the recording. */
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 3};
  card.style.border_color = recording ? theme->error : theme->accent;
  card.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  (void)vkr_ui_push_id_u64(ui, vkr_hash_table_key_cstr(request->id).hash);
  if (!vkr_ui_panel_begin(ui, string8_lit("request"), &card)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }
  VkrUiWidgetConfig question =
      vkr_editor_text_config(theme->font_body, theme->text);
  question.placement = editor_request_cell(0u, 0u);
  question.placement.justify = VKR_UI_ALIGN_START;
  question.placement.align = VKR_UI_ALIGN_END;
  question.text.font = editor->heading_font;
  question.text.layout.word_wrap = true_v;
  question.text.layout.max_width =
      Max(120.0f, width_pt - EDITOR_REQUEST_CARD_CHROME_PT);
  vkr_ui_label(ui, string8_lit("question"),
               editor_request_str(request->question), &question);
  VkrUiWidgetConfig facts =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  facts.placement = editor_request_cell(0u, 1u);
  facts.placement.justify = VKR_UI_ALIGN_START;
  facts.placement.align = VKR_UI_ALIGN_START;
  facts.tooltip = editor_request_facts_tooltip(ui, request);
  vkr_ui_label(ui, string8_lit("facts"), editor_request_facts(ui, request),
               &facts);
  editor_request_actions(editor, frame, request, recording);
  if (declining) {
    editor_request_decline_row(editor, frame, request);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

bool8_t vkr_editor_recording_requests_build(VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            uint32_t row, float32_t width_pt) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t shown = vkr_editor_recording_requests_shown(editor);
  if (shown == 0u) {
    return false_v;
  }
  VkrUiTrack rows[VKR_EDITOR_RECORDING_REQUEST_MAX + 1u];
  for (uint32_t i = 0u; i <= shown; ++i) {
    rows[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};
  }
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig section = vkr_ui_panel_config_default();
  section.placement = editor_request_cell(0u, row);
  section.placement.align = VKR_UI_ALIGN_START;
  section.columns = &column;
  section.column_count = 1u;
  section.rows = rows;
  section.row_count = shown + 1u;
  section.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(ui, string8_lit("requests"), &section)) {
    return true_v;
  }
  VkrUiWidgetConfig title =
      vkr_editor_section_label_config(editor->heading_font);
  title.placement = editor_request_cell(0u, 0u);
  title.placement.justify = VKR_UI_ALIGN_START;
  title.tooltip = string8_lit("Agents ask for these recordings; Start one to "
                              "record it, or Decline it with a reason");
  vkr_ui_label(ui, string8_lit("title"),
               string8_create_formatted(ui->frame_allocator,
                                        "Requests from agents  \xc2\xb7  %u",
                                        shown),
               &title);
  uint32_t card_row = 1u;
  for (uint32_t i = 0u; i < requests->count; ++i) {
    if (editor_request_shown(&requests->items[i])) {
      editor_request_card(editor, frame, &requests->items[i], card_row++,
                          width_pt);
    }
  }
  (void)vkr_ui_panel_end(ui);
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Scene overlay                                                             */

/* The panel's width; its height follows the question and the steps. */
#define EDITOR_REQUEST_OVERLAY_PT 320.0f

void vkr_editor_recording_request_overlay_build(VkrEditorUi *editor,
                                                const VkrSampleUiFrame *frame) {
  VkrEditorRecordingRequests *requests = &editor->recording.requests;
  const VkrEditorRecordingRequest *request = editor_request_active(editor);
  if (!request || !frame->mapping_valid || !frame->recording ||
      !frame->recording->recording) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const float32_t width = EDITOR_REQUEST_OVERLAY_PT;
  const uint32_t steps = request->step_count;
  VkrUiTrack rows[VKR_EDITOR_RECORDING_REQUEST_STEP_MAX + 3u];
  for (uint32_t i = 0u; i < steps + 3u; ++i) {
    rows[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};
  }
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  /* Below the Scene toolbar, at the Scene's right edge. */
  panel.placement.margin_pt =
      (VkrUiEdges){viewport.y / scale + 48.0f, 0, 0,
                   (viewport.x + viewport.z) / scale - width - 12.0f};
  panel.style = vkr_editor_glass_style();
  panel.style.min_size_pt = (Vec2){width, 0.0f};
  panel.style.max_size_pt = (Vec2){width, 0.0f};
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = steps + 3u;
  panel.style.padding_pt = (VkrUiEdges){10.0f, 14.0f, 10.0f, 14.0f};
  panel.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.request.overlay"), &panel)) {
    return;
  }
  VkrUiWidgetConfig caption =
      vkr_editor_section_label_config(editor->heading_font);
  caption.placement = editor_request_cell(0u, 0u);
  caption.placement.justify = VKR_UI_ALIGN_START;
  caption.icon = VKR_UI_ICON_RECORD;
  caption.icon_size_pt = 10.0f;
  caption.icon_color = theme->error;
  vkr_ui_label(
      ui, string8_lit("caption"),
      string8_create_formatted(ui->frame_allocator, "Request from %s",
                               request->owner[0] ? request->owner : "an agent"),
      &caption);
  VkrUiWidgetConfig question =
      vkr_editor_text_config(theme->font_body, theme->text);
  question.placement = editor_request_cell(0u, 1u);
  question.placement.justify = VKR_UI_ALIGN_START;
  question.text.font = editor->heading_font;
  question.text.layout.word_wrap = true_v;
  question.text.layout.max_width = width - 28.0f;
  question.style.padding_pt.bottom = theme->space_sm;
  vkr_ui_label(ui, string8_lit("question"),
               editor_request_str(request->question), &question);
  for (uint32_t i = 0u; i < steps; ++i) {
    const bool8_t done = i < requests->step;
    const bool8_t current = i == requests->step;
    VkrUiWidgetConfig step = vkr_editor_text_config(
        theme->font_body, done      ? theme->text_disabled
                          : current ? theme->text
                                    : theme->text_secondary);
    step.placement = editor_request_cell(0u, 2u + i);
    step.placement.justify = VKR_UI_ALIGN_START;
    step.text.layout.word_wrap = true_v;
    step.text.layout.max_width = width - 50.0f;
    step.icon = done      ? VKR_UI_ICON_CHECK
                : current ? VKR_UI_ICON_CHEVRON_RIGHT
                          : VKR_UI_ICON_DOT;
    step.icon_size_pt = 12.0f;
    step.icon_color = done      ? theme->success
                      : current ? theme->accent
                                : theme->text_disabled;
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("step"), editor_request_str(request->steps[i]),
                 &step);
    (void)vkr_ui_pop_id(ui);
  }
  char key[32] = "";
  (void)vkr_editor_command_shortcut(CMD_REQUEST_NEXT_STEP, key, sizeof(key));
  char stop[48] = "";
  if (request->stop_seconds > 0u) {
    const float64_t left =
        (float64_t)request->stop_seconds -
        (vkr_platform_get_absolute_time() - requests->started_at);
    string_format(stop, sizeof(stop), "%sstops in %.0f s",
                  steps > 0u ? "  \xc2\xb7  " : "", Max(left, 0.0));
  }
  VkrUiWidgetConfig hint =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  hint.placement = editor_request_cell(0u, 2u + steps);
  hint.placement.justify = VKR_UI_ALIGN_START;
  hint.style.padding_pt.top = theme->space_sm;
  vkr_ui_label(
      ui, string8_lit("hint"),
      string8_create_formatted(ui->frame_allocator, "%s%s%s",
                               steps > 0u ? (key[0] ? key : "Next step") : "",
                               steps > 0u ? " finishes a step" : "", stop),
      &hint);
  (void)vkr_ui_panel_end(ui);
}
