#pragma once

#include "containers/str.h"
#include "defines.h"

/*
 * A recording's annotations (ADR-122): notes on frames, segments (named
 * frame spans with a note on why they matter) and a review state, written by
 * the user and by agents after the recording ends.
 *
 *   <name>.vkrrec/annotations.json
 *
 * The file is small and rewritten whole on every change, so the stream and
 * summary.json never change after the recording stops. A bundle without the
 * file has no annotations.
 */

#define VKR_RECORDING_ANNOTATIONS_FILE "annotations.json"
/* A bundle path with the file name appended fits this many bytes. */
#define VKR_RECORDING_ANNOTATIONS_PATH_MAX 1024u
#define VKR_RECORDING_NOTE_MAX 64u
#define VKR_RECORDING_SEGMENT_MAX 64u
#define VKR_RECORDING_NOTE_TEXT_MAX 256u
#define VKR_RECORDING_SEGMENT_NAME_MAX 64u
#define VKR_RECORDING_SEGMENT_NOTE_MAX 1024u
#define VKR_RECORDING_AUTHOR_MAX 32u

typedef enum VkrRecordingReview {
  VKR_RECORDING_REVIEW_UNREVIEWED = 0,
  VKR_RECORDING_REVIEW_REVIEWED,
  /* Someone found something that needs work. */
  VKR_RECORDING_REVIEW_FLAGGED,
  VKR_RECORDING_REVIEW_COUNT,
} VkrRecordingReview;

typedef struct VkrRecordingNote {
  uint64_t frame;
  /* Seconds since 1970. */
  int64_t created;
  char author[VKR_RECORDING_AUTHOR_MAX];
  char text[VKR_RECORDING_NOTE_TEXT_MAX];
} VkrRecordingNote;

/* Frames `first` to `last`, inclusive, marked for a reason: a short name
   and a note that says what happens there and why it matters, so a person
   or an agent pointed at it knows what to look for. */
typedef struct VkrRecordingSegment {
  uint64_t first;
  uint64_t last;
  /* Seconds since 1970; zero for a segment from before version 2. */
  int64_t created;
  char author[VKR_RECORDING_AUTHOR_MAX];
  char name[VKR_RECORDING_SEGMENT_NAME_MAX];
  char note[VKR_RECORDING_SEGMENT_NOTE_MAX];
} VkrRecordingSegment;

typedef struct VkrRecordingAnnotations {
  VkrRecordingReview review;
  char review_by[VKR_RECORDING_AUTHOR_MAX];
  int64_t review_at;
  /* What the reviewer concluded; empty without a note. Clearing the review
     clears it. */
  char review_note[VKR_RECORDING_SEGMENT_NOTE_MAX];
  /* By frame. */
  VkrRecordingNote notes[VKR_RECORDING_NOTE_MAX];
  uint32_t note_count;
  /* By first frame. */
  VkrRecordingSegment segments[VKR_RECORDING_SEGMENT_MAX];
  uint32_t segment_count;
} VkrRecordingAnnotations;

const char *vkr_recording_review_name(VkrRecordingReview review);

/** Reads "unreviewed", "reviewed" or "flagged"; false for anything else. */
bool8_t vkr_recording_review_parse(String8 name, VkrRecordingReview *out);

/**
 * @brief Reads a bundle's annotations. A bundle without the file reads as
 * empty and unreviewed. A version 1 file's "ranges" read as segments
 * without notes.
 * @return false when the file exists but is not readable annotations.
 */
bool8_t vkr_recording_annotations_read(const char *bundle_path,
                                       VkrRecordingAnnotations *out);

/** Replaces the bundle's annotations file (version 2). */
bool8_t vkr_recording_annotations_write(const char *bundle_path,
                                        const VkrRecordingAnnotations *in);

/**
 * @brief Adds a note in frame order; `text` is cut at
 * VKR_RECORDING_NOTE_TEXT_MAX - 1 bytes on a UTF-8 boundary.
 * @return false when the text is empty or the notes are full.
 */
bool8_t vkr_recording_annotations_add_note(VkrRecordingAnnotations *a,
                                           uint64_t frame, const char *author,
                                           String8 text);

/**
 * @brief Adds a segment in order of its first frame; the frames may come in
 * either order. `name` and `note` are cut on a UTF-8 boundary to fit.
 * @return its index, or UINT32_MAX when the name is empty or the segments
 * are full.
 */
uint32_t vkr_recording_annotations_add_segment(VkrRecordingAnnotations *a,
                                               uint64_t first, uint64_t last,
                                               const char *author, String8 name,
                                               String8 note);

/**
 * @brief Changes segment `index`'s name and note; a NULL one stays.
 * @return false for an empty name or a missing segment, which change
 * nothing.
 */
bool8_t vkr_recording_annotations_edit_segment(VkrRecordingAnnotations *a,
                                               uint32_t index,
                                               const String8 *name,
                                               const String8 *note);

/** The first segment named `name`, or UINT32_MAX. */
uint32_t
vkr_recording_annotations_find_segment(const VkrRecordingAnnotations *a,
                                       String8 name);

void vkr_recording_annotations_remove_note(VkrRecordingAnnotations *a,
                                           uint32_t index);

void vkr_recording_annotations_remove_segment(VkrRecordingAnnotations *a,
                                              uint32_t index);

void vkr_recording_annotations_set_review(VkrRecordingAnnotations *a,
                                          VkrRecordingReview review,
                                          const char *author);

/** Sets the review's note, cut on a UTF-8 boundary to fit; empty removes
 * it. */
void vkr_recording_annotations_set_review_note(VkrRecordingAnnotations *a,
                                               String8 note);
