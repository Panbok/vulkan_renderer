#include "assets/vkr_recording_annotations.h"

#include "containers/str.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"

/* Larger files are not annotations this module wrote. */
#define VKR_RECORDING_ANNOTATIONS_FILE_MAX (256ull * 1024ull)

static const char *const VKR_RECORDING_REVIEW_NAMES[] = {
    "unreviewed",
    "reviewed",
    "flagged",
};

_Static_assert(ArrayCount(VKR_RECORDING_REVIEW_NAMES) ==
                   VKR_RECORDING_REVIEW_COUNT,
               "a name for every review state");

const char *vkr_recording_review_name(VkrRecordingReview review) {
  return (uint32_t)review < VKR_RECORDING_REVIEW_COUNT
             ? VKR_RECORDING_REVIEW_NAMES[review]
             : "unreviewed";
}

bool8_t vkr_recording_review_parse(String8 name, VkrRecordingReview *out) {
  for (uint32_t i = 0u; i < VKR_RECORDING_REVIEW_COUNT; ++i) {
    if (vkr_string8_equals_cstr(&name, VKR_RECORDING_REVIEW_NAMES[i])) {
      *out = (VkrRecordingReview)i;
      return true_v;
    }
  }
  return false_v;
}

/* Copies `text` into `out`, cut on a UTF-8 boundary to fit. */
vkr_internal void vkr_recording_annotations_copy(char *out, uint64_t capacity,
                                                 String8 text) {
  uint64_t length = Min(text.length, capacity - 1u);
  /* A cut inside a sequence backs up to the sequence's first byte. */
  if (length < text.length) {
    while (length > 0u && (text.str[length] & 0xc0u) == 0x80u) {
      --length;
    }
  }
  MemCopy(out, text.str, length);
  out[length] = '\0';
}

vkr_internal String8 vkr_recording_annotations_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */

typedef struct VkrRecordingAnnotationsRead {
  VkrAllocator *scratch;
  VkrRecordingAnnotations *out;
} VkrRecordingAnnotationsRead;

/* A decoded string field of an object into a fixed buffer; empty when it is
   absent. */
vkr_internal void
vkr_recording_annotations_text(VkrRecordingAnnotationsRead *read,
                               String8 object, const char *key, char *out,
                               uint64_t capacity) {
  VkrJsonReader reader = vkr_json_reader_from_string(object);
  String8 value = {0};
  out[0] = '\0';
  if (vkr_json_find_root_field(&reader, key) &&
      vkr_json_parse_string_decoded(&reader, read->scratch, &value)) {
    vkr_recording_annotations_copy(out, capacity, value);
  }
}

vkr_internal bool8_t vkr_recording_annotations_number(String8 object,
                                                      const char *key,
                                                      float64_t *out) {
  VkrJsonReader reader = vkr_json_reader_from_string(object);
  return vkr_json_find_root_field(&reader, key) &&
         vkr_json_parse_double(&reader, out) && *out >= 0.0;
}

/* The elements of the root array `key`, each as its whole object text. */
typedef bool8_t (*VkrRecordingAnnotationsVisit)(
    VkrRecordingAnnotationsRead *read, String8 object);

vkr_internal bool8_t vkr_recording_annotations_each(
    VkrRecordingAnnotationsRead *read, String8 file, const char *key,
    VkrRecordingAnnotationsVisit visit) {
  VkrJsonReader reader = vkr_json_reader_from_string(file);
  if (!vkr_json_find_root_field(&reader, key)) {
    return true_v;
  }
  if (reader.pos >= reader.length || reader.data[reader.pos] != '[') {
    return false_v;
  }
  ++reader.pos;
  while (vkr_json_next_array_element(&reader)) {
    String8 object = {0};
    if (!vkr_json_capture_composite(&reader, &object) || !visit(read, object)) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_recording_annotations_read_note(
    VkrRecordingAnnotationsRead *read, String8 object) {
  VkrRecordingAnnotations *out = read->out;
  float64_t frame = 0.0;
  float64_t created = 0.0;
  /* Notes past the limit are left out; a note without a frame is damage. */
  if (out->note_count == VKR_RECORDING_NOTE_MAX) {
    return true_v;
  }
  if (!vkr_recording_annotations_number(object, "frame", &frame)) {
    return false_v;
  }
  VkrRecordingNote *note = &out->notes[out->note_count];
  *note = (VkrRecordingNote){.frame = (uint64_t)frame};
  if (vkr_recording_annotations_number(object, "created", &created)) {
    note->created = (int64_t)created;
  }
  vkr_recording_annotations_text(read, object, "author", note->author,
                                 sizeof(note->author));
  vkr_recording_annotations_text(read, object, "text", note->text,
                                 sizeof(note->text));
  if (note->text[0]) {
    ++out->note_count;
  }
  return true_v;
}

vkr_internal bool8_t vkr_recording_annotations_read_range(
    VkrRecordingAnnotationsRead *read, String8 object) {
  VkrRecordingAnnotations *out = read->out;
  float64_t first = 0.0;
  float64_t last = 0.0;
  if (out->range_count == VKR_RECORDING_RANGE_MAX) {
    return true_v;
  }
  if (!vkr_recording_annotations_number(object, "first", &first) ||
      !vkr_recording_annotations_number(object, "last", &last) ||
      last < first) {
    return false_v;
  }
  VkrRecordingRange *range = &out->ranges[out->range_count];
  *range =
      (VkrRecordingRange){.first = (uint64_t)first, .last = (uint64_t)last};
  vkr_recording_annotations_text(read, object, "author", range->author,
                                 sizeof(range->author));
  vkr_recording_annotations_text(read, object, "name", range->name,
                                 sizeof(range->name));
  if (range->name[0]) {
    ++out->range_count;
  }
  return true_v;
}

vkr_internal bool8_t vkr_recording_annotations_parse(
    VkrRecordingAnnotationsRead *read, String8 file) {
  VkrRecordingAnnotations *out = read->out;
  String8 kind = {0};
  VkrJsonReader reader = vkr_json_reader_from_string(file);
  if (!vkr_json_find_root_field(&reader, "kind") ||
      !vkr_json_parse_string(&reader, &kind) ||
      !vkr_string8_equals_cstr(&kind, "vkr.recording.annotations")) {
    return false_v;
  }
  /* The review object's fields. */
  reader = vkr_json_reader_from_string(file);
  String8 review = {0};
  if (vkr_json_find_root_field(&reader, "review") &&
      vkr_json_capture_composite(&reader, &review)) {
    char state[16];
    float64_t at = 0.0;
    vkr_recording_annotations_text(read, review, "state", state, sizeof(state));
    if (!vkr_recording_review_parse(vkr_recording_annotations_str(state),
                                    &out->review)) {
      out->review = VKR_RECORDING_REVIEW_UNREVIEWED;
    }
    vkr_recording_annotations_text(read, review, "by", out->review_by,
                                   sizeof(out->review_by));
    if (vkr_recording_annotations_number(review, "at", &at)) {
      out->review_at = (int64_t)at;
    }
  }
  return vkr_recording_annotations_each(read, file, "notes",
                                        vkr_recording_annotations_read_note) &&
         vkr_recording_annotations_each(read, file, "ranges",
                                        vkr_recording_annotations_read_range);
}

vkr_internal bool8_t vkr_recording_annotations_path(const char *bundle_path,
                                                    char *out,
                                                    uint64_t capacity) {
  return string_format(out, capacity, "%s/%s", bundle_path,
                       VKR_RECORDING_ANNOTATIONS_FILE) < (int32_t)capacity;
}

bool8_t vkr_recording_annotations_read(const char *bundle_path,
                                       VkrRecordingAnnotations *out) {
  vkr_assert(bundle_path != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  char path[VKR_RECORDING_ANNOTATIONS_PATH_MAX];
  if (!vkr_recording_annotations_path(bundle_path, path, sizeof(path))) {
    return false_v;
  }
  const FilePath file = file_path_view(path);
  if (!file_exists(&file)) {
    return true_v;
  }
  FileMapping mapping = {0};
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return false_v;
  }
  bool8_t ok = false_v;
  Arena *arena = NULL;
  VkrAllocator scratch = {0};
  VkrRecordingAnnotationsRead read = {.scratch = &scratch, .out = out};
  if (mapping.size > VKR_RECORDING_ANNOTATIONS_FILE_MAX) {
    goto cleanup;
  }
  /* Decoded strings live until the read returns. */
  arena = arena_create(KB(256), KB(64));
  scratch.ctx = arena;
  if (!arena || !vkr_allocator_arena(&scratch)) {
    goto cleanup;
  }
  ok = vkr_recording_annotations_parse(
      &read, (String8){.str = (uint8_t *)mapping.data, .length = mapping.size});

cleanup:
  if (arena) {
    arena_destroy(arena);
  }
  file_unmap(&mapping);
  if (!ok) {
    MemZero(out, sizeof(*out));
  }
  return ok;
}

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */

vkr_internal void vkr_recording_annotations_field(VkrJsonWriter *json,
                                                  const char *name,
                                                  const char *value) {
  vkr_json_writer_name(json, vkr_recording_annotations_str(name));
  vkr_json_writer_string(json, vkr_recording_annotations_str(value));
}

vkr_internal void vkr_recording_annotations_u64(VkrJsonWriter *json,
                                                const char *name,
                                                uint64_t value) {
  vkr_json_writer_name(json, vkr_recording_annotations_str(name));
  vkr_json_writer_u64(json, value);
}

bool8_t vkr_recording_annotations_write(const char *bundle_path,
                                        const VkrRecordingAnnotations *in) {
  vkr_assert(bundle_path != NULL && in != NULL);
  char path[VKR_RECORDING_ANNOTATIONS_PATH_MAX];
  if (!vkr_recording_annotations_path(bundle_path, path, sizeof(path))) {
    return false_v;
  }
  VkrJsonFileWriter file;
  if (!vkr_json_file_writer_begin(&file, vkr_recording_annotations_str(path))) {
    return false_v;
  }
  VkrJsonWriter *json = &file.writer;
  vkr_json_writer_begin_object(json);
  vkr_recording_annotations_field(json, "kind", "vkr.recording.annotations");
  vkr_recording_annotations_u64(json, "version", 1u);
  vkr_json_writer_name(json, string8_lit("review"));
  vkr_json_writer_begin_object(json);
  vkr_recording_annotations_field(json, "state",
                                  vkr_recording_review_name(in->review));
  vkr_recording_annotations_field(json, "by", in->review_by);
  vkr_json_writer_name(json, string8_lit("at"));
  vkr_json_writer_i64(json, in->review_at);
  vkr_json_writer_end_object(json);

  vkr_json_writer_name(json, string8_lit("notes"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < in->note_count; ++i) {
    const VkrRecordingNote *note = &in->notes[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_annotations_u64(json, "frame", note->frame);
    vkr_json_writer_name(json, string8_lit("created"));
    vkr_json_writer_i64(json, note->created);
    vkr_recording_annotations_field(json, "author", note->author);
    vkr_recording_annotations_field(json, "text", note->text);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);

  vkr_json_writer_name(json, string8_lit("ranges"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < in->range_count; ++i) {
    const VkrRecordingRange *range = &in->ranges[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_annotations_u64(json, "first", range->first);
    vkr_recording_annotations_u64(json, "last", range->last);
    vkr_recording_annotations_field(json, "author", range->author);
    vkr_recording_annotations_field(json, "name", range->name);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
  return vkr_json_file_writer_commit(&file);
}

/* ------------------------------------------------------------------------ */
/* Changes                                                                   */

bool8_t vkr_recording_annotations_add_note(VkrRecordingAnnotations *a,
                                           uint64_t frame, const char *author,
                                           String8 text) {
  vkr_assert(a != NULL);
  if (text.length == 0u || a->note_count == VKR_RECORDING_NOTE_MAX) {
    return false_v;
  }
  /* After the notes of earlier or equal frames, so equal frames keep the
     order they were written in. */
  uint32_t at = a->note_count;
  while (at > 0u && a->notes[at - 1u].frame > frame) {
    --at;
  }
  MemCopy(&a->notes[at + 1u], &a->notes[at],
          sizeof(a->notes[0]) * (a->note_count - at));
  VkrRecordingNote *note = &a->notes[at];
  *note = (VkrRecordingNote){.frame = frame,
                             .created = vkr_platform_get_unix_time()};
  vkr_recording_annotations_copy(
      note->author, sizeof(note->author),
      vkr_recording_annotations_str(author ? author : ""));
  vkr_recording_annotations_copy(note->text, sizeof(note->text), text);
  ++a->note_count;
  return true_v;
}

bool8_t vkr_recording_annotations_add_range(VkrRecordingAnnotations *a,
                                            uint64_t first, uint64_t last,
                                            const char *author, String8 name) {
  vkr_assert(a != NULL);
  if (name.length == 0u || a->range_count == VKR_RECORDING_RANGE_MAX) {
    return false_v;
  }
  const uint64_t low = Min(first, last);
  const uint64_t high = Max(first, last);
  uint32_t at = a->range_count;
  while (at > 0u && a->ranges[at - 1u].first > low) {
    --at;
  }
  MemCopy(&a->ranges[at + 1u], &a->ranges[at],
          sizeof(a->ranges[0]) * (a->range_count - at));
  VkrRecordingRange *range = &a->ranges[at];
  *range = (VkrRecordingRange){.first = low, .last = high};
  vkr_recording_annotations_copy(
      range->author, sizeof(range->author),
      vkr_recording_annotations_str(author ? author : ""));
  vkr_recording_annotations_copy(range->name, sizeof(range->name), name);
  ++a->range_count;
  return true_v;
}

void vkr_recording_annotations_remove_note(VkrRecordingAnnotations *a,
                                           uint32_t index) {
  vkr_assert(a != NULL);
  if (index >= a->note_count) {
    return;
  }
  MemCopy(&a->notes[index], &a->notes[index + 1u],
          sizeof(a->notes[0]) * (a->note_count - index - 1u));
  --a->note_count;
}

void vkr_recording_annotations_remove_range(VkrRecordingAnnotations *a,
                                            uint32_t index) {
  vkr_assert(a != NULL);
  if (index >= a->range_count) {
    return;
  }
  MemCopy(&a->ranges[index], &a->ranges[index + 1u],
          sizeof(a->ranges[0]) * (a->range_count - index - 1u));
  --a->range_count;
}

void vkr_recording_annotations_set_review(VkrRecordingAnnotations *a,
                                          VkrRecordingReview review,
                                          const char *author) {
  vkr_assert(a != NULL);
  a->review = review < VKR_RECORDING_REVIEW_COUNT
                  ? review
                  : VKR_RECORDING_REVIEW_UNREVIEWED;
  a->review_at = vkr_platform_get_unix_time();
  vkr_recording_annotations_copy(
      a->review_by, sizeof(a->review_by),
      vkr_recording_annotations_str(author ? author : ""));
}
