#pragma once

#include "assets/vkr_recording_analysis.h"
#include "memory/vkr_dmemory.h"
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

/* Plot columns of the performance view: longer recordings keep the slowest
   frame of each run of frames. */
#define VKR_EDITOR_PERF_COLUMNS 2048u
/* CPU stages the view stacks: update, render prepare, submit and wait. */
#define VKR_EDITOR_PERF_STAGES 4u
/* Pass series the view stacks: the passes with the most total time, then
   the rest as one. */
#define VKR_EDITOR_PERF_PASS_SERIES 8u

/* The performance view of one recording (docs/proposals/recordings.md,
   "Performance view"): its loaded data, analysis and plot columns. */
typedef struct VkrEditorRecordingPerf {
  bool8_t loaded;
  char path[VKR_RECORDING_PATH_MAX];
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  char error[160];
  VkrDMemory memory;
  VkrAllocator allocator;
  VkrRecordingData data;
  VkrRecordingAnalysis analysis;
  uint32_t column_count;
  /* The frame each column shows. */
  uint64_t column_frame[VKR_EDITOR_PERF_COLUMNS];
  float32_t frame_ms[VKR_EDITOR_PERF_COLUMNS];
  uint8_t frame_color[VKR_EDITOR_PERF_COLUMNS];
  float32_t stage_ms[VKR_EDITOR_PERF_STAGES][VKR_EDITOR_PERF_COLUMNS];
  float32_t ram_gb[VKR_EDITOR_PERF_COLUMNS];
  float32_t vram_gb[VKR_EDITOR_PERF_COLUMNS];
  float32_t frame_ms_top;
  float32_t stage_ms_top;
  float32_t ram_range[2];
  float32_t vram_range[2];
  bool8_t has_stages;
  bool8_t has_ram;
  bool8_t has_vram;
  /* Tier 1. The pass chart has its own columns: each shows the frame of its
     run of frames with GPU times (CPU times when the recording has none)
     whose passes took longest. Series are the passes with the most total
     time, then the rest; with a focused pass, that pass alone. */
  bool8_t has_passes;
  bool8_t pass_gpu;
  uint32_t pass_focus;
  uint32_t pass_column_count;
  uint64_t pass_column_frame[VKR_EDITOR_PERF_COLUMNS];
  float32_t pass_ms[VKR_EDITOR_PERF_PASS_SERIES][VKR_EDITOR_PERF_COLUMNS];
  uint32_t pass_series[VKR_EDITOR_PERF_PASS_SERIES];
  uint32_t pass_series_count;
  float32_t pass_ms_top;
  uint32_t pass_hovered_column;
  /* The frame the view selected last, or UINT64_MAX; and the column the
     pointer was over in the last build, for the tooltip. */
  uint64_t selected;
  uint32_t hovered_column;
  float32_t spike_scroll;
} VkrEditorRecordingPerf;

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
  VkrEditorRecordingPerf perf;
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

/** Starts a recording named `name` (a time stamp when empty) by `author`
 * that measures `tier`. Returns its token, or zero with the reason in
 * `error`. */
uint64_t vkr_editor_recording_start(struct VkrEditorUi *editor,
                                    const char *name, const char *author,
                                    VkrRecordingTier tier, bool8_t toast,
                                    char *error, uint64_t error_capacity);

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

/** Loads a recording (a bundle path or name) into the Performance window and
 * shows it. False with the reason in `error`. */
bool8_t vkr_editor_recording_analyze(struct VkrEditorUi *editor,
                                     const char *name, char *error,
                                     uint64_t error_capacity);

/** Frees the Performance window's data. */
void vkr_editor_recording_perf_release(VkrEditorRecordingPerf *perf);

/** The Performance window's body. */
void vkr_editor_recording_perf_build(struct VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds);

/** Releases the Library's thumbnail textures and the Performance window's
 * data; call before the UI system shuts down. */
void vkr_editor_recording_shutdown(struct VkrEditorUi *editor);

/** The REC indicator and, in Playback, the playback bar over the Scene. */
void vkr_editor_recording_overlays_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
