#include "assets/vkr_recording_annotations.h"

#include "containers/str.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"

/* Larger files are not annotations this module wrote: 64 notes and 64
   segment notes, every byte escaped, stay under it. */
#define VKR_RECORDING_ANNOTATIONS_FILE_MAX (1024ull * 1024ull)

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

/* A segment, or a version 1 range, which has no note or creation time. */
vkr_internal bool8_t vkr_recording_annotations_read_segment(
    VkrRecordingAnnotationsRead *read, String8 object) {
  VkrRecordingAnnotations *out = read->out;
  float64_t first = 0.0;
  float64_t last = 0.0;
  float64_t created = 0.0;
  if (out->segment_count == VKR_RECORDING_SEGMENT_MAX) {
    return true_v;
  }
  if (!vkr_recording_annotations_number(object, "first", &first) ||
      !vkr_recording_annotations_number(object, "last", &last) ||
      last < first) {
    return false_v;
  }
  VkrRecordingSegment *segment = &out->segments[out->segment_count];
  *segment =
      (VkrRecordingSegment){.first = (uint64_t)first, .last = (uint64_t)last};
  if (vkr_recording_annotations_number(object, "created", &created)) {
    segment->created = (int64_t)created;
  }
  vkr_recording_annotations_text(read, object, "author", segment->author,
                                 sizeof(segment->author));
  vkr_recording_annotations_text(read, object, "name", segment->name,
                                 sizeof(segment->name));
  vkr_recording_annotations_text(read, object, "note", segment->note,
                                 sizeof(segment->note));
  if (segment->name[0]) {
    ++out->segment_count;
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
    vkr_recording_annotations_text(read, review, "note", out->review_note,
                                   sizeof(out->review_note));
    if (vkr_recording_annotations_number(review, "at", &at)) {
      out->review_at = (int64_t)at;
    }
  }
  /* Version 1 named its segments "ranges". */
  return vkr_recording_annotations_each(read, file, "notes",
                                        vkr_recording_annotations_read_note) &&
         vkr_recording_annotations_each(
             read, file, "segments", vkr_recording_annotations_read_segment) &&
         vkr_recording_annotations_each(read, file, "ranges",
                                        vkr_recording_annotations_read_segment);
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
  arena = arena_create(MB(1), KB(64));
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
  vkr_recording_annotations_u64(json, "version", 2u);
  vkr_json_writer_name(json, string8_lit("review"));
  vkr_json_writer_begin_object(json);
  vkr_recording_annotations_field(json, "state",
                                  vkr_recording_review_name(in->review));
  vkr_recording_annotations_field(json, "by", in->review_by);
  vkr_json_writer_name(json, string8_lit("at"));
  vkr_json_writer_i64(json, in->review_at);
  if (in->review_note[0]) {
    vkr_recording_annotations_field(json, "note", in->review_note);
  }
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

  vkr_json_writer_name(json, string8_lit("segments"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < in->segment_count; ++i) {
    const VkrRecordingSegment *segment = &in->segments[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_annotations_u64(json, "first", segment->first);
    vkr_recording_annotations_u64(json, "last", segment->last);
    vkr_json_writer_name(json, string8_lit("created"));
    vkr_json_writer_i64(json, segment->created);
    vkr_recording_annotations_field(json, "author", segment->author);
    vkr_recording_annotations_field(json, "name", segment->name);
    vkr_recording_annotations_field(json, "note", segment->note);
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

uint32_t vkr_recording_annotations_add_segment(VkrRecordingAnnotations *a,
                                               uint64_t first, uint64_t last,
                                               const char *author, String8 name,
                                               String8 note) {
  vkr_assert(a != NULL);
  if (name.length == 0u || a->segment_count == VKR_RECORDING_SEGMENT_MAX) {
    return UINT32_MAX;
  }
  const uint64_t low = Min(first, last);
  const uint64_t high = Max(first, last);
  /* After the segments that start earlier or at the same frame, so equal
     starts keep the order they were marked in. */
  uint32_t at = a->segment_count;
  while (at > 0u && a->segments[at - 1u].first > low) {
    --at;
  }
  MemCopy(&a->segments[at + 1u], &a->segments[at],
          sizeof(a->segments[0]) * (a->segment_count - at));
  VkrRecordingSegment *segment = &a->segments[at];
  *segment = (VkrRecordingSegment){
      .first = low, .last = high, .created = vkr_platform_get_unix_time()};
  vkr_recording_annotations_copy(
      segment->author, sizeof(segment->author),
      vkr_recording_annotations_str(author ? author : ""));
  vkr_recording_annotations_copy(segment->name, sizeof(segment->name), name);
  vkr_recording_annotations_copy(segment->note, sizeof(segment->note), note);
  ++a->segment_count;
  return at;
}

bool8_t vkr_recording_annotations_edit_segment(VkrRecordingAnnotations *a,
                                               uint32_t index,
                                               const String8 *name,
                                               const String8 *note) {
  vkr_assert(a != NULL);
  if (index >= a->segment_count || (name && name->length == 0u)) {
    return false_v;
  }
  VkrRecordingSegment *segment = &a->segments[index];
  if (name) {
    vkr_recording_annotations_copy(segment->name, sizeof(segment->name), *name);
  }
  if (note) {
    vkr_recording_annotations_copy(segment->note, sizeof(segment->note), *note);
  }
  return true_v;
}

uint32_t
vkr_recording_annotations_find_segment(const VkrRecordingAnnotations *a,
                                       String8 name) {
  vkr_assert(a != NULL);
  for (uint32_t i = 0u; i < a->segment_count; ++i) {
    if (vkr_string8_equals_cstr(&name, a->segments[i].name)) {
      return i;
    }
  }
  return UINT32_MAX;
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

void vkr_recording_annotations_remove_segment(VkrRecordingAnnotations *a,
                                              uint32_t index) {
  vkr_assert(a != NULL);
  if (index >= a->segment_count) {
    return;
  }
  MemCopy(&a->segments[index], &a->segments[index + 1u],
          sizeof(a->segments[0]) * (a->segment_count - index - 1u));
  --a->segment_count;
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
  if (a->review == VKR_RECORDING_REVIEW_UNREVIEWED) {
    a->review_note[0] = '\0';
  }
}

void vkr_recording_annotations_set_review_note(VkrRecordingAnnotations *a,
                                               String8 note) {
  vkr_assert(a != NULL);
  vkr_recording_annotations_copy(a->review_note, sizeof(a->review_note), note);
}
