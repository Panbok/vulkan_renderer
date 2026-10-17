#pragma once

#include "renderer/systems/vkr_resource_system.h"
#include "vkr_sample_runtime.h"

/*
 * Recordings in the editor (docs/proposals/recordings.md): the requests the
 * editor, its commands and the agent operations send to the runtime, one
 * per frame, and the Scene's REC indicator and playback bar.
 */

#define VKR_EDITOR_RECORDING_QUEUE 8u
#define VKR_EDITOR_RECORDING_LIST_MAX 128u
/* thumb.png in a bundle: the Scene a moment after the recording starts. */
#define VKR_EDITOR_RECORDING_THUMB_FILE "thumb.png"
#define VKR_EDITOR_RECORDING_THUMB_WIDTH 240u

/* One bundle in the recording directory, from its summary.json. */
typedef struct VkrEditorRecordingEntry {
  char bundle[VKR_RECORDING_LABEL_MAX + 16u];
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  char scene[160];
  char author[32];
  char status[24];
  uint64_t frames;
  float64_t duration;
  uint32_t markers;
  uint64_t lost;
  int64_t started_unix;
  bool8_t has_summary;
  /* thumb.png as a UI texture, loaded when the card first shows; released
     when the list is read again. */
  VkrResourceHandleInfo thumb_request;
  VkrTextureHandle thumb_texture;
  uint32_t thumb_width;
  uint32_t thumb_height;
  bool8_t thumb_failed;
} VkrEditorRecordingEntry;

typedef struct VkrEditorRecording {
  /* Requests waiting for a frame; the runtime takes one per frame. */
  VkrSampleRecordingRequest queue[VKR_EDITOR_RECORDING_QUEUE];
  uint32_t queue_count;
  uint64_t next_token;
  /* Tokens the editor's own controls sent; their outcomes show as toasts. */
  uint64_t toast_tokens[VKR_EDITOR_RECORDING_QUEUE];
  uint32_t toast_count;
  uint64_t outcome_seen;
  /* The playback bar's scrub position while the pointer drags it. */
  bool8_t scrubbing;
  /* The Recordings window's list, read when it opens, after a recording
     stops and on Refresh; newest first. */
  char directory[VKR_RECORDING_PATH_MAX];
  VkrEditorRecordingEntry entries[VKR_EDITOR_RECORDING_LIST_MAX];
  uint32_t entry_count;
  bool8_t listed;
  bool8_t was_recording;
  /* One past the entry whose Delete was pressed once, or zero; a second
     press deletes it. */
  uint32_t delete_armed;
  float32_t scroll;
  /* The thumbnail capture of the recording in progress: the bundle it
     belongs to, the capture's token (its high bit keeps it apart from agent
     captures), frames waited and attempts made. */
  char thumb_bundle[VKR_RECORDING_PATH_MAX];
  uint64_t thumb_token;
  uint64_t thumb_serial;
  uint32_t thumb_wait;
  uint32_t thumb_attempts;
  bool8_t thumb_done;
} VkrEditorRecording;

struct VkrEditorUi;

/** Sends the next queued request and reports the outcome of the editor's own
 * requests; call once per build before the operations and controls. */
void vkr_editor_recording_update(struct VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame);

/** Queues `request` under a new token, returned; zero when the queue is full.
 * `toast` shows the outcome to the designer. */
uint64_t vkr_editor_recording_send(struct VkrEditorUi *editor,
                                   VkrSampleRecordingRequest request,
                                   bool8_t toast);

/** The directory new recordings go to: the workspace's local/recordings, or
 * the agent's private directory outside a project. */
bool8_t vkr_editor_recording_directory(struct VkrEditorUi *editor, char *out,
                                       uint64_t capacity);

/** Starts a recording named `name` (a time stamp when empty) by `author`.
 * Returns its token, or zero with the reason in `error`. */
uint64_t vkr_editor_recording_start(struct VkrEditorUi *editor,
                                    const char *name, const char *author,
                                    bool8_t toast, char *error,
                                    uint64_t error_capacity);

/** Resolves a recording argument: a path, or a bundle name in the recording
 * directory with or without its extension. */
bool8_t vkr_editor_recording_resolve(struct VkrEditorUi *editor,
                                     const char *name, char *out,
                                     uint64_t capacity);

/** The Recordings window's body: the recording directory's bundles with
 * Watch, Copy agent prompt and Delete. */
void vkr_editor_recording_window_build(struct VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect bounds);

/** Releases the Library's thumbnail textures; call before the UI system
 * shuts down. */
void vkr_editor_recording_shutdown(struct VkrEditorUi *editor);

/** The REC indicator and, in Playback, the playback bar over the Scene. */
void vkr_editor_recording_overlays_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
