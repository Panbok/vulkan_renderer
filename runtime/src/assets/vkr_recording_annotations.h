#pragma once

#include "containers/str.h"
#include "defines.h"

/*
 * A recording's annotations (docs/proposals/recordings.md, "Bundle"): notes
 * on frames, named frame ranges and a review state, written by the user and
 * by agents after the recording ends.
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
#define VKR_RECORDING_RANGE_MAX 32u
#define VKR_RECORDING_NOTE_TEXT_MAX 256u
#define VKR_RECORDING_RANGE_NAME_MAX 64u
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

/* Frames `first` to `last`, inclusive. */
typedef struct VkrRecordingRange {
  uint64_t first;
  uint64_t last;
  char author[VKR_RECORDING_AUTHOR_MAX];
  char name[VKR_RECORDING_RANGE_NAME_MAX];
} VkrRecordingRange;

typedef struct VkrRecordingAnnotations {
  VkrRecordingReview review;
  char review_by[VKR_RECORDING_AUTHOR_MAX];
  int64_t review_at;
  /* By frame. */
  VkrRecordingNote notes[VKR_RECORDING_NOTE_MAX];
  uint32_t note_count;
  /* By first frame. */
  VkrRecordingRange ranges[VKR_RECORDING_RANGE_MAX];
  uint32_t range_count;
} VkrRecordingAnnotations;

const char *vkr_recording_review_name(VkrRecordingReview review);

/** Reads "unreviewed", "reviewed" or "flagged"; false for anything else. */
bool8_t vkr_recording_review_parse(String8 name, VkrRecordingReview *out);

/**
 * @brief Reads a bundle's annotations. A bundle without the file reads as
 * empty and unreviewed.
 * @return false when the file exists but is not readable annotations.
 */
bool8_t vkr_recording_annotations_read(const char *bundle_path,
                                       VkrRecordingAnnotations *out);

/** Replaces the bundle's annotations file. */
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
 * @brief Adds a range in order of its first frame; the frames may come in
 * either order.
 * @return false when the name is empty or the ranges are full.
 */
bool8_t vkr_recording_annotations_add_range(VkrRecordingAnnotations *a,
                                            uint64_t first, uint64_t last,
                                            const char *author, String8 name);

void vkr_recording_annotations_remove_note(VkrRecordingAnnotations *a,
                                           uint32_t index);

void vkr_recording_annotations_remove_range(VkrRecordingAnnotations *a,
                                            uint32_t index);

void vkr_recording_annotations_set_review(VkrRecordingAnnotations *a,
                                          VkrRecordingReview review,
                                          const char *author);
