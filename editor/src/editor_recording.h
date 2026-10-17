#pragma once

#include "assets/vkr_recording_analysis.h"
#include "assets/vkr_recording_annotations.h"
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
  /* The recording holds actions and a player track to drive with. */
  bool8_t drivable;
  /* From annotations.json. */
  uint32_t notes;
  uint32_t ranges;
  VkrRecordingReview review;
  /* thumb.png as a UI texture, loaded when the card first shows; released
     when the list is read again. */
  VkrResourceHandleInfo thumb_request;
  VkrTextureHandle thumb_texture;
  uint32_t thumb_width;
  uint32_t thumb_height;
  bool8_t thumb_failed;
} VkrEditorRecordingEntry;

/* Points of the open playback's route in the Scene. */
#define VKR_EDITOR_RECORDING_PATH_POINTS 4096u

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

/* Recording requests (docs/proposals/recordings.md, "Recording requests"):
   an agent asks the user to record something, through a file in the
   recording directory's requests/ the editor watches. */
#define VKR_EDITOR_RECORDING_REQUEST_MAX 16u
#define VKR_EDITOR_RECORDING_REQUEST_STEP_MAX 8u
#define VKR_EDITOR_RECORDING_REQUEST_TEXT_MAX 256u
#define VKR_EDITOR_RECORDING_REQUEST_ID_MAX 48u

typedef enum VkrEditorRecordingRequestStatus {
  VKR_EDITOR_RECORDING_REQUEST_PENDING = 0,
  /* The user started recording it. */
  VKR_EDITOR_RECORDING_REQUEST_ACCEPTED,
  VKR_EDITOR_RECORDING_REQUEST_DECLINED,
  /* Its recording stopped; `recording` names the bundle. */
  VKR_EDITOR_RECORDING_REQUEST_RECORDED,
  VKR_EDITOR_RECORDING_REQUEST_CANCELLED,
  VKR_EDITOR_RECORDING_REQUEST_EXPIRED,
  VKR_EDITOR_RECORDING_REQUEST_STATUS_COUNT,
} VkrEditorRecordingRequestStatus;

typedef struct VkrEditorRecordingRequest {
  char id[VKR_EDITOR_RECORDING_REQUEST_ID_MAX];
  int64_t created;
  int64_t expires;
  /* The requesting agent. */
  char owner[32];
  VkrEditorRecordingRequestStatus status;
  /* The scene to record in; empty for any. */
  char scene[VKR_RECORDING_PATH_MAX];
  /* Where the camera starts, in scene space. */
  bool8_t has_pose;
  Vec3 position;
  float32_t yaw;
  float32_t pitch;
  char steps[VKR_EDITOR_RECORDING_REQUEST_STEP_MAX]
            [VKR_EDITOR_RECORDING_REQUEST_TEXT_MAX];
  uint32_t step_count;
  VkrRecordingTier tier;
  /* Stop after this many seconds of recording; zero for the user's stop. */
  uint32_t stop_seconds;
  char question[2u * VKR_EDITOR_RECORDING_REQUEST_TEXT_MAX];
  char recording[VKR_RECORDING_PATH_MAX];
  char decline_reason[VKR_EDITOR_RECORDING_REQUEST_TEXT_MAX];
} VkrEditorRecordingRequest;

typedef struct VkrEditorRecordingRequests {
  VkrEditorRecordingRequest items[VKR_EDITOR_RECORDING_REQUEST_MAX];
  uint32_t count;
  /* When the files were last read. */
  float64_t read_at;
  /* Pending requests already announced, by id hash. */
  uint64_t announced[VKR_EDITOR_RECORDING_REQUEST_MAX];
  uint32_t announced_count;
  /* The request being recorded, its id, the step it is on and when its
     recording began; active is false otherwise. */
  bool8_t active;
  char active_id[VKR_EDITOR_RECORDING_REQUEST_ID_MAX];
  uint32_t step;
  float64_t started_at;
  bool8_t stop_sent;
  bool8_t saw_recording;
  /* The card whose Decline asks for a reason, and the reason typed. */
  char decline_id[VKR_EDITOR_RECORDING_REQUEST_ID_MAX];
  char decline_text[VKR_EDITOR_RECORDING_REQUEST_TEXT_MAX];
  uint32_t decline_length;
} VkrEditorRecordingRequests;

/* The annotations of one recording, cached while its playback is open or an
   operation edits them, and the playback bar's note and range tools. */
typedef struct VkrEditorRecordingNotes {
  /* The bundle the cache holds; empty when none. */
  char bundle[VKR_RECORDING_PATH_MAX];
  VkrRecordingAnnotations annotations;
  /* The note field above the playback bar, and whether it takes the
     keyboard on its next build. */
  bool8_t composing;
  bool8_t focus;
  char text[VKR_RECORDING_NOTE_TEXT_MAX];
  uint64_t text_length;
  /* A range being marked: its first frame, until a second press ends it. */
  bool8_t marking;
  uint64_t range_first;
} VkrEditorRecordingNotes;

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
  /* A drive ran last frame; its end shows its measurement. */
  bool8_t was_driving;
  /* The open playback's edits were reported as not replayable. */
  bool8_t edits_warned;
  /* The open playback's route: the player's track when it has one, else
     the camera's, one point per run of frames with the origin offset added,
     the frame it shows and the run's worst frame time (0 within budget, 1
     up to twice it, 2 beyond, UINT8_MAX for a break). Built per bundle;
     drawn by path_id. */
  char path_bundle[VKR_RECORDING_PATH_MAX];
  uint32_t path_count;
  bool8_t path_player;
  float64_t path_points[VKR_EDITOR_RECORDING_PATH_POINTS][3];
  uint64_t path_frames[VKR_EDITOR_RECORDING_PATH_POINTS];
  uint8_t path_classes[VKR_EDITOR_RECORDING_PATH_POINTS];
  VkrUiId path_id;
  VkrEditorRecordingRequests requests;
  VkrEditorRecordingNotes notes;
  /* Reused while the list reads each bundle's annotations. */
  VkrRecordingAnnotations list_annotations;
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

/** Gives the Scene overlays that take clicks (the playback bar, its note
 * field and the REC chip) the pointer over their rectangles. Call with the
 * floating windows' layers, before the Scene reads its input. */
void vkr_editor_recording_register_input(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);

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

/* ---- Recording requests (editor_recording_requests.c) ---- */

/** Writes a new pending request, giving it an id, its creation time and an
 * expiry `expire_seconds` from now. */
bool8_t vkr_editor_recording_request_create(
    struct VkrEditorUi *editor, const VkrEditorRecordingRequest *request,
    uint32_t expire_seconds, char *out_id, uint64_t id_capacity, char *error,
    uint64_t error_capacity);
/** The request named `id`, read from its file; false when there is none. */
bool8_t vkr_editor_recording_request_read(struct VkrEditorUi *editor,
                                          const char *id,
                                          VkrEditorRecordingRequest *out);
/** Reads every request, newest first, into the editor's list. */
void vkr_editor_recording_requests_list(struct VkrEditorUi *editor);
/** The user starts a pending request: the camera goes to its start pose
 * and a recording starts under its id. */
bool8_t vkr_editor_recording_request_start(struct VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           const char *id, char *error,
                                           uint64_t error_capacity);
/** The user declines a pending request with a reason, which may be empty. */
bool8_t vkr_editor_recording_request_decline(struct VkrEditorUi *editor,
                                             const char *id, const char *reason,
                                             char *error,
                                             uint64_t error_capacity);
/** The agent withdraws a request that is not recorded yet. */
bool8_t vkr_editor_recording_request_cancel(struct VkrEditorUi *editor,
                                            const char *id, char *error,
                                            uint64_t error_capacity);
/** Marks the active request's current step done with a step marker. */
bool8_t vkr_editor_recording_request_next_step(struct VkrEditorUi *editor);
/** Per frame: reads new requests and announces them, expires old ones,
 * applies the active request's stop rule and records its end. */
void vkr_editor_recording_requests_update(struct VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
/** The Recordings window's Requests section in `row` of its body, which is
 * `width_pt` wide; returns whether it showed anything. */
bool8_t vkr_editor_recording_requests_build(struct VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            uint32_t row, float32_t width_pt);
/** The newest recording that can be analyzed, reading the directory if the
 * Recordings window has not; NULL when there is none. The pointer lives
 * until the list is read again. */
const VkrEditorRecordingEntry *
vkr_editor_recording_newest(struct VkrEditorUi *editor);

/* ---- Annotations (editor_recording_annotations.c) ---- */

/** The annotations of the bundle at `bundle_path`, read into the cache when
 * it holds another bundle; NULL with the reason in `error` when the file is
 * damaged. The pointer lives until another bundle is read. */
VkrRecordingAnnotations *
vkr_editor_recording_annotations(struct VkrEditorUi *editor,
                                 const char *bundle_path, char *error,
                                 uint64_t error_capacity);
/** Writes the cached annotations and updates the bundle's card. */
bool8_t vkr_editor_recording_annotations_save(struct VkrEditorUi *editor);
/** Frames of the recording at `bundle_path`: the open playback's, else its
 * summary.json's; zero when unknown. */
uint64_t vkr_editor_recording_frame_count(const VkrSampleUiFrame *frame,
                                          const char *bundle_path);
/** The playback bar's tools: open or close the note field, mark a range's
 * first or last frame at the playhead, and set the review state, flagging it
 * when `flag`. */
void vkr_editor_recording_note_toggle(struct VkrEditorUi *editor);
void vkr_editor_recording_range_mark(struct VkrEditorUi *editor,
                                     const VkrRecordingPlayback *playback);
void vkr_editor_recording_review_toggle(struct VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback,
                                        bool8_t flag);
/** The open playback's ranges and notes on the playback bar's track, whose
 * pixel rectangle is `slider_px`. */
void vkr_editor_recording_annotations_track(
    struct VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, VkrUiRect slider_px);
/** The note field's rectangle in points while it is open, from the playback
 * bar's top-left corner and width. */
bool8_t vkr_editor_recording_composer_rect(const struct VkrEditorUi *editor,
                                           Vec3 bar_pt, Vec4 *out_pt);
/** The note field over the playback bar, whose top-left corner and width
 * are `bar_pt`; it adds the note at the playhead on Enter or Add. */
void vkr_editor_recording_composer_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         Vec3 bar_pt);
/** Puts a prompt on the clipboard that points an agent at the recording:
 * its facts, markers, notes, ranges, review, the request it answers and
 * the playhead when it is open. */
void vkr_editor_recording_copy_prompt(struct VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const VkrEditorRecordingEntry *entry);
/** Fills the entry's note and range counts and review state. */
void vkr_editor_recording_entry_annotations(struct VkrEditorUi *editor,
                                            const char *bundle_path,
                                            VkrEditorRecordingEntry *entry);
/** Requests the Requests section shows: pending ones and the one
 * recording. */
uint32_t vkr_editor_recording_requests_shown(struct VkrEditorUi *editor);
/** The active request's question and steps over the Scene. */
void vkr_editor_recording_request_overlay_build(struct VkrEditorUi *editor,
                                                const VkrSampleUiFrame *frame);
/** "pending", "accepted", "declined", "recorded", "cancelled", "expired". */
const char *vkr_editor_recording_request_status_name(
    VkrEditorRecordingRequestStatus status);

/** Places the open playback's route with the frame's final camera. */
void vkr_editor_recording_project(struct VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame);

/** Releases the Library's thumbnail textures and the Performance window's
 * data; call before the UI system shuts down. */
void vkr_editor_recording_shutdown(struct VkrEditorUi *editor);

/** The REC indicator and, in Playback, the playback bar over the Scene. */
void vkr_editor_recording_overlays_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
