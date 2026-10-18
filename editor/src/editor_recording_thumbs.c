#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ops.h"
#include "editor_projects.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "stb_image.h"

/* Recording thumbnails (docs/proposals/recordings.md, "Library" and
   "Player"): while the Scene records, a capture of its image without the
   editor UI twice a second. The main thread samples each capture down to a
   thumbnail as it arrives, a few texels per pixel, and a thread of its own
   encodes and writes it, so a recording's frames carry neither the full
   capture's conversion nor the file. The timeline loads them back for the
   frame under the pointer. */

/* Builds after a recording starts before its first capture, so the Scene
   shows its first frames; builds to wait for a capture; seconds between
   captures. A Scene that fails this many captures in a row, as one without
   an editor image does, stops asking until the next recording. */
#define EDITOR_THUMBS_DELAY 10u
#define EDITOR_THUMBS_TIMEOUT 120u
#define EDITOR_THUMBS_INTERVAL 0.5
#define EDITOR_THUMBS_FAILURES 3u
/* Builds the Scene image keeps its size before the warm-up capture. */
#define EDITOR_THUMBS_WARM_STABLE 30u
#define EDITOR_THUMBS_TOKEN (1ull << 63u)
#define EDITOR_THUMBS_QUALITY 82
#define EDITOR_THUMBS_INDEX_MIN 256u

vkr_internal void editor_thumbs_forget(VkrEditorRecordingThumbs *thumbs);

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */

/* The write thread: thumbs/<frame>.jpg, then thumb.png for the cover. */
vkr_internal void *editor_thumbs_write(void *argument) {
  VkrEditorRecordingThumbJob *job = argument;
  char path[VKR_RECORDING_PATH_MAX + 32u];
  string_format(path, sizeof(path), "%s/%s", job->bundle,
                VKR_EDITOR_RECORDING_THUMBS_DIRECTORY);
  const FilePath directory = file_path_view(path);
  bool8_t written = file_create_directories(&directory);
  if (written) {
    string_format(path, sizeof(path), "%s/%s/%08llu.jpg", job->bundle,
                  VKR_EDITOR_RECORDING_THUMBS_DIRECTORY,
                  (unsigned long long)job->frame);
    written = vkr_editor_jpg_write(path, job->pixels, job->width, job->height,
                                   EDITOR_THUMBS_QUALITY);
  }
  if (written && job->cover) {
    string_format(path, sizeof(path), "%s/%s", job->bundle,
                  VKR_EDITOR_RECORDING_THUMB_FILE);
    written = vkr_editor_png_write(path, job->pixels, job->width, job->height);
  }
  job->written = written;
  vkr_atomic_bool_store(&job->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

/* Joins a write thread that finished, or with `wait`, one still writing. */
vkr_internal void editor_thumbs_finish(VkrEditorRecordingThumbs *thumbs,
                                       bool8_t wait) {
  VkrEditorRecordingThumbJob *job = &thumbs->job;
  if (!job->running ||
      (!wait && !vkr_atomic_bool_load(&job->done, VKR_MEMORY_ORDER_ACQUIRE))) {
    return;
  }
  (void)vkr_thread_join(job->thread);
  (void)vkr_thread_destroy(thumbs->allocator, &job->thread);
  job->running = false_v;
  if (job->cover && job->written &&
      string_equals(job->bundle, thumbs->bundle)) {
    thumbs->cover_done = true_v;
  }
}

/* The capture's size fitted into the thumbnail box, its aspect kept. */
vkr_internal void editor_thumbs_fit(const VkrCaptureItemResult *item,
                                    uint32_t *out_width, uint32_t *out_height) {
  uint64_t width = Min(item->width, VKR_EDITOR_RECORDING_THUMB_WIDTH);
  uint64_t height = (width * item->height + item->width / 2u) / item->width;
  if (height > VKR_EDITOR_RECORDING_THUMB_HEIGHT) {
    height = VKR_EDITOR_RECORDING_THUMB_HEIGHT;
    width = (height * item->width + item->height / 2u) / item->height;
  }
  *out_width = (uint32_t)Max(width, 1u);
  *out_height = (uint32_t)Max(height, 1u);
}

/* Samples the capture into the job and starts its write thread. */
vkr_internal void editor_thumbs_start(VkrEditorRecordingThumbs *thumbs,
                                      VkrUiSystem *ui,
                                      const VkrCaptureItemResult *item) {
  VkrEditorRecordingThumbJob *job = &thumbs->job;
  if (job->running || item->width == 0u || item->height == 0u) {
    return;
  }
  if (!job->pixels) {
    job->pixels = vkr_heap_alloc((uint64_t)VKR_EDITOR_RECORDING_THUMB_WIDTH *
                                 VKR_EDITOR_RECORDING_THUMB_HEIGHT * 4u);
    if (!job->pixels) {
      return;
    }
  }
  editor_thumbs_fit(item, &job->width, &job->height);
  if (!vkr_editor_capture_sample(item, job->width, job->height, job->pixels)) {
    return;
  }
  job->frame = thumbs->frame;
  job->cover = !thumbs->cover_done;
  job->written = false_v;
  string_format(job->bundle, sizeof(job->bundle), "%s", thumbs->bundle);
  vkr_atomic_bool_store(&job->done, false_v, VKR_MEMORY_ORDER_RELAXED);
  thumbs->allocator = &ui->retained_allocator;
  job->running = vkr_thread_create(thumbs->allocator, &job->thread,
                                   editor_thumbs_write, job);
}

/* Whether the capture slot is free for a capture asked in this build. A
   project scene opening blurs the Scene, and the launcher replaces it, so
   neither renders a Scene image: the renderer refused such captures and
   cancelled their frames, and three refusals at startup kept the warm-up
   capture from ever running. */
vkr_internal bool8_t editor_thumbs_slot_free(const VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame) {
  return frame->capture_request && !frame->capture_request->request &&
         !frame->capture_busy && frame->mapping_valid &&
         !frame->scene_rendering_stopped &&
         !vkr_editor_projects_loading(editor->projects) &&
         !vkr_editor_projects_modal(editor->projects);
}

/* The renderer grows its readback memory the first time a capture needs
   more, and waits for the GPU to do it: a frame of about 6 ms on Bistro.
   While nothing records, one capture of a Scene image that has kept its
   size for a while grows it, so a recording's first thumbnail does not
   carry that frame. */
vkr_internal void editor_thumbs_warm(const VkrEditorUi *editor,
                                     VkrEditorRecordingThumbs *thumbs,
                                     const VkrSampleUiFrame *frame) {
  if (thumbs->warm_token) {
    const VkrSampleCaptureReady *ready = frame->capture_ready;
    if (ready && ready->token == thumbs->warm_token) {
      if (!ready->failed && ready->item) {
        thumbs->warm_bytes = Max(thumbs->warm_bytes, ready->item->data_size);
      } else {
        ++thumbs->warm_failures;
      }
      thumbs->warm_token = 0u;
    } else if (++thumbs->warm_wait > EDITOR_THUMBS_TIMEOUT) {
      ++thumbs->warm_failures;
      thumbs->warm_token = 0u;
    }
    return;
  }
  if (!frame->mapping_valid) {
    return;
  }
  /* RGBA8 rows of the Scene image, padded as the renderer pads them. */
  const uint64_t width = (uint64_t)frame->mapping.image_rect_px.z;
  const uint64_t height = (uint64_t)frame->mapping.image_rect_px.w;
  const uint64_t bytes = ((width * 4u + 255u) & ~255ull) * height;
  if (bytes != thumbs->warm_size) {
    thumbs->warm_size = bytes;
    thumbs->warm_stable = 0u;
    return;
  }
  if (bytes == 0u || bytes <= thumbs->warm_bytes ||
      thumbs->warm_failures >= EDITOR_THUMBS_FAILURES ||
      ++thumbs->warm_stable < EDITOR_THUMBS_WARM_STABLE ||
      !editor_thumbs_slot_free(editor, frame)) {
    return;
  }
  thumbs->warm_token = EDITOR_THUMBS_TOKEN | ++thumbs->serial;
  thumbs->warm_wait = 0u;
  *frame->capture_request = (VkrSampleCaptureRequest){
      .request = true_v, .token = thumbs->warm_token, .scene_only = true_v};
}

void vkr_editor_recording_thumbs_update(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame) {
  VkrEditorRecordingThumbs *thumbs = &editor->recording.thumbs;
  ++thumbs->builds;
  editor_thumbs_finish(thumbs, false_v);
  const VkrSampleRecordingStatus *status = frame->recording;
  if (!status || !status->recording) {
    thumbs->token = 0u;
    editor_thumbs_warm(editor, thumbs, frame);
    return;
  }
  if (!string_equals(thumbs->bundle, status->recording_path)) {
    string_format(thumbs->bundle, sizeof(thumbs->bundle), "%s",
                  status->recording_path);
    /* The bundle may replace one the timeline listed. */
    editor_thumbs_forget(thumbs);
    thumbs->token = 0u;
    thumbs->wait = 0u;
    thumbs->next_time = 0.0;
    thumbs->failures = 0u;
    thumbs->warm_token = 0u;
    thumbs->cover_done = false_v;
  }
  ++thumbs->wait;
  if (thumbs->token) {
    const VkrSampleCaptureReady *ready = frame->capture_ready;
    const bool8_t answered = ready && ready->token == thumbs->token;
    const bool8_t captured = answered && !ready->failed && ready->item;
    if (captured) {
      editor_thumbs_start(thumbs, frame->ui, ready->item);
    }
    if (answered || thumbs->wait > EDITOR_THUMBS_TIMEOUT) {
      thumbs->failures = captured ? 0u : thumbs->failures + 1u;
      thumbs->token = 0u;
      thumbs->wait = 0u;
    }
    return;
  }
  /* An agent capture asked for in this build, or one rendering, goes
     first; so does the last thumbnail's write. */
  const float64_t now = vkr_platform_get_absolute_time();
  if (thumbs->failures >= EDITOR_THUMBS_FAILURES ||
      thumbs->wait < EDITOR_THUMBS_DELAY || now < thumbs->next_time ||
      thumbs->job.running || !editor_thumbs_slot_free(editor, frame)) {
    return;
  }
  thumbs->token = EDITOR_THUMBS_TOKEN | ++thumbs->serial;
  thumbs->wait = 0u;
  thumbs->frame = status->frames;
  thumbs->next_time = now + EDITOR_THUMBS_INTERVAL;
  *frame->capture_request = (VkrSampleCaptureRequest){
      .request = true_v, .token = thumbs->token, .scene_only = true_v};
}

/* ------------------------------------------------------------------------ */
/* Previews                                                                  */

vkr_internal String8 editor_thumbs_path(const VkrEditorRecordingThumbs *thumbs,
                                        uint64_t frame, bool8_t query,
                                        char *out, uint64_t capacity) {
  const int32_t length = string_format(
      out, capacity, "%s/%s/%08llu.jpg%s", thumbs->index_bundle,
      VKR_EDITOR_RECORDING_THUMBS_DIRECTORY, (unsigned long long)frame,
      query ? "?cs=srgb&source=only" : "");
  return string8_create_from_cstr((const uint8_t *)out,
                                  length > 0 ? (uint64_t)length : 0u);
}

vkr_internal void editor_thumbs_unload(VkrEditorRecordingThumbs *thumbs,
                                       VkrEditorRecordingPreview *preview) {
  if (preview->request.request_id) {
    char path[VKR_RECORDING_PATH_MAX + 64u];
    vkr_resource_system_unload(
        &preview->request,
        editor_thumbs_path(thumbs, preview->frame, true_v, path, sizeof(path)));
  }
  *preview = (VkrEditorRecordingPreview){0};
}

/* Collects the frames of thumbs/<frame>.jpg files. */
vkr_internal bool8_t editor_thumbs_visit(void *context, const char *name,
                                         bool8_t is_directory) {
  VkrEditorRecordingThumbs *thumbs = context;
  if (is_directory) {
    return true_v;
  }
  uint64_t frame = 0u;
  uint32_t digits = 0u;
  const char *c = name;
  for (; *c >= '0' && *c <= '9' && digits < 12u; ++c, ++digits) {
    frame = frame * 10u + (uint64_t)(*c - '0');
  }
  if (digits == 0u || !string_equals(c, ".jpg")) {
    return true_v;
  }
  if (thumbs->frame_count == thumbs->frame_capacity) {
    const uint32_t capacity =
        Max(EDITOR_THUMBS_INDEX_MIN, thumbs->frame_capacity * 2u);
    uint64_t *frames =
        vkr_heap_realloc(thumbs->frames, (uint64_t)capacity * sizeof(*frames));
    if (!frames) {
      return false_v;
    }
    thumbs->frames = frames;
    thumbs->frame_capacity = capacity;
  }
  thumbs->frames[thumbs->frame_count++] = frame;
  return true_v;
}

vkr_internal int32_t editor_thumbs_compare(const void *lhs, const void *rhs) {
  const uint64_t a = *(const uint64_t *)lhs;
  const uint64_t b = *(const uint64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

vkr_internal void
editor_thumbs_release_previews(VkrEditorRecordingThumbs *thumbs) {
  for (uint32_t i = 0u; i < VKR_EDITOR_RECORDING_PREVIEWS; ++i) {
    editor_thumbs_unload(thumbs, &thumbs->previews[i]);
  }
}

/* Drops the listed bundle, so the next preview lists it again. */
vkr_internal void editor_thumbs_forget(VkrEditorRecordingThumbs *thumbs) {
  editor_thumbs_release_previews(thumbs);
  thumbs->index_bundle[0] = '\0';
  thumbs->frame_count = 0u;
}

/* Lists the bundle's thumbnails once, when it is first previewed. */
vkr_internal void editor_thumbs_index(VkrEditorRecordingThumbs *thumbs,
                                      const char *bundle_path) {
  if (string_equals(thumbs->index_bundle, bundle_path)) {
    return;
  }
  editor_thumbs_release_previews(thumbs);
  string_format(thumbs->index_bundle, sizeof(thumbs->index_bundle), "%s",
                bundle_path);
  thumbs->frame_count = 0u;
  char path[VKR_RECORDING_PATH_MAX + 32u];
  string_format(path, sizeof(path), "%s/%s", bundle_path,
                VKR_EDITOR_RECORDING_THUMBS_DIRECTORY);
  const FilePath directory = file_path_view(path);
  (void)file_list_directory(&directory, editor_thumbs_visit, thumbs);
  vkr_sort(thumbs->frames, thumbs->frame_count, sizeof(*thumbs->frames),
           editor_thumbs_compare);
}

/* Starts loading the thumbnail of `frame` into `preview`, its size read
   from the file's header. */
vkr_internal void editor_thumbs_load(VkrEditorRecordingThumbs *thumbs,
                                     VkrEditorRecordingPreview *preview,
                                     uint64_t frame, VkrUiSystem *ui) {
  editor_thumbs_unload(thumbs, preview);
  preview->frame = frame;
  preview->failed = true_v;
  char path[VKR_RECORDING_PATH_MAX + 64u];
  (void)editor_thumbs_path(thumbs, frame, false_v, path, sizeof(path));
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return;
  }
  int width = 0;
  int height = 0;
  int components = 0;
  const bool8_t sized =
      mapping.size <= (uint64_t)INT32_MAX &&
      stbi_info_from_memory((const stbi_uc *)mapping.data, (int)mapping.size,
                            &width, &height, &components) != 0 &&
      width > 0 && height > 0;
  file_unmap(&mapping);
  if (!sized) {
    return;
  }
  preview->width = (uint32_t)width;
  preview->height = (uint32_t)height;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  preview->failed = !vkr_resource_system_load(
      VKR_RESOURCE_TYPE_TEXTURE,
      editor_thumbs_path(thumbs, frame, true_v, path, sizeof(path)),
      ui->frame_allocator, &preview->request, &error);
}

/* The preview's texture once it resolves; a failed load stays failed. */
vkr_internal bool8_t editor_thumbs_ready(VkrEditorRecordingPreview *preview) {
  if (preview->texture.id) {
    return true_v;
  }
  if (preview->failed || !preview->request.request_id) {
    return false_v;
  }
  VkrResourceHandleInfo resolved = {0};
  if (vkr_resource_system_try_get_resolved(&preview->request, &resolved)) {
    preview->texture = resolved.as.texture;
    return true_v;
  }
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrResourceLoadState state =
      vkr_resource_system_get_state(&preview->request, &error);
  preview->failed = state == VKR_RESOURCE_LOAD_STATE_FAILED ||
                    state == VKR_RESOURCE_LOAD_STATE_CANCELED;
  return false_v;
}

/* The index of the last listed thumbnail at or before `frame_index`, else
   0 for the first; the list must not be empty. */
vkr_internal uint32_t editor_thumbs_find(const VkrEditorRecordingThumbs *thumbs,
                                         uint64_t frame_index) {
  uint32_t low = 0u;
  uint32_t high = thumbs->frame_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (thumbs->frames[middle] <= frame_index) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low > 0u ? low - 1u : 0u;
}

uint32_t vkr_editor_recording_thumb_frames(VkrEditorUi *editor,
                                           const char *bundle_path,
                                           const uint64_t **out_frames) {
  VkrEditorRecordingThumbs *thumbs = &editor->recording.thumbs;
  editor_thumbs_index(thumbs, bundle_path);
  *out_frames = thumbs->frames;
  return thumbs->frame_count;
}

bool8_t vkr_editor_recording_thumb_at(VkrEditorUi *editor,
                                      const char *bundle_path,
                                      uint64_t frame_index, char *out,
                                      uint64_t capacity) {
  VkrEditorRecordingThumbs *thumbs = &editor->recording.thumbs;
  editor_thumbs_index(thumbs, bundle_path);
  if (thumbs->frame_count == 0u) {
    return false_v;
  }
  const uint64_t frame =
      thumbs->frames[editor_thumbs_find(thumbs, frame_index)];
  (void)editor_thumbs_path(thumbs, frame, false_v, out, capacity);
  return true_v;
}

bool8_t vkr_editor_recording_thumb_preview(VkrEditorUi *editor, VkrUiSystem *ui,
                                           const char *bundle_path,
                                           uint64_t frame_index,
                                           VkrTextureHandle *out_texture,
                                           Vec2 *out_size, bool8_t *out_any) {
  VkrEditorRecordingThumbs *thumbs = &editor->recording.thumbs;
  editor_thumbs_index(thumbs, bundle_path);
  *out_any = thumbs->frame_count > 0u;
  if (thumbs->frame_count == 0u) {
    return false_v;
  }
  const uint64_t frame =
      thumbs->frames[editor_thumbs_find(thumbs, frame_index)];
  VkrEditorRecordingPreview *wanted = NULL;
  VkrEditorRecordingPreview *oldest = &thumbs->previews[0];
  for (uint32_t i = 0u; i < VKR_EDITOR_RECORDING_PREVIEWS; ++i) {
    VkrEditorRecordingPreview *preview = &thumbs->previews[i];
    const bool8_t used = preview->request.request_id || preview->failed;
    if (used && preview->frame == frame) {
      wanted = preview;
      break;
    }
    if (!used || (oldest->used && preview->used < oldest->used)) {
      oldest = preview;
    }
  }
  if (!wanted) {
    wanted = oldest;
    editor_thumbs_load(thumbs, wanted, frame, ui);
  }
  wanted->used = thumbs->builds;
  /* While it loads, the loaded thumbnail nearest it stands in. */
  const bool8_t ready = editor_thumbs_ready(wanted);
  VkrEditorRecordingPreview *shown = ready ? wanted : NULL;
  uint64_t nearest = UINT64_MAX;
  for (uint32_t i = 0u; i < VKR_EDITOR_RECORDING_PREVIEWS && !ready; ++i) {
    VkrEditorRecordingPreview *preview = &thumbs->previews[i];
    const uint64_t distance = preview->frame > frame ? preview->frame - frame
                                                     : frame - preview->frame;
    if (preview != wanted && distance < nearest &&
        editor_thumbs_ready(preview)) {
      nearest = distance;
      shown = preview;
    }
  }
  if (!shown) {
    return false_v;
  }
  *out_texture = shown->texture;
  *out_size = (Vec2){(float32_t)shown->width, (float32_t)shown->height};
  return true_v;
}

void vkr_editor_recording_thumbs_shutdown(VkrEditorUi *editor) {
  VkrEditorRecordingThumbs *thumbs = &editor->recording.thumbs;
  editor_thumbs_finish(thumbs, true_v);
  editor_thumbs_release_previews(thumbs);
  vkr_heap_free(thumbs->job.pixels);
  vkr_heap_free(thumbs->frames);
  thumbs->job.pixels = NULL;
  thumbs->frames = NULL;
  thumbs->frame_count = 0u;
  thumbs->frame_capacity = 0u;
  thumbs->index_bundle[0] = '\0';
}
