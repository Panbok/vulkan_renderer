#pragma once

#include "assets/vkr_recording_analysis.h"
#include "assets/vkr_recording_annotations.h"
#include "assets/vkr_recording_compare.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "editor_table.h"
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
/* thumbs/<frame>.jpg in a bundle: the Scene twice a second while it
   records, the frame zero-padded to eight digits. */
#define VKR_EDITOR_RECORDING_THUMBS_DIRECTORY "thumbs"
/* Thumbnails fit this box; Content's preview loader takes 256 a side. */
#define VKR_EDITOR_RECORDING_THUMB_WIDTH 256u
#define VKR_EDITOR_RECORDING_THUMB_HEIGHT 240u
/* Thumbnails the timeline keeps loaded for its hover preview. */
#define VKR_EDITOR_RECORDING_PREVIEWS 4u

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
  /* The recording holds actions and a player track to drive with, and
     the tick hashes of a session that started while it recorded, to
     re-simulate. */
  bool8_t drivable;
  bool8_t resimulable;
  /* Recorded with every measurement. */
  bool8_t all_measurements;
  /* From annotations.json. */
  uint32_t notes;
  uint32_t segments;
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

/* Options a segmented control holds at most. */
#define VKR_EDITOR_PERF_SEGMENTS_MAX 8u

/* A 2D view the pointer pans and zooms, such as the render graph's flow or
   the level map: a world point shows at (world - center) * zoom points from
   the middle of the canvas. */
typedef struct VkrEditorPerfCanvas {
  Vec2 center;
  /* Points per world unit; zero until fitted. */
  float32_t zoom;
  float32_t fit_zoom;
  bool8_t fitted;
  /* The canvas's size in points, from its last layout. */
  Vec2 size_pt;
  bool8_t dragging;
  Vec2 drag_px;
} VkrEditorPerfCanvas;

/* What the pointer did on a canvas this build: over it, clicked without a
   drag, and where it is in world units. */
typedef struct VkrEditorPerfCanvasInput {
  bool8_t hovered;
  bool8_t clicked;
  Vec2 pointer;
} VkrEditorPerfCanvasInput;

/* Passes the Render graph tab follows, and the updates its trends keep. */
#define VKR_EDITOR_PERF_GRAPH_PASSES 256u
#define VKR_EDITOR_PERF_GRAPH_HISTORY 60u

/* The value each pass shows in the Render graph tab: its mean or slowest
   time over the update window, or its time in the shown frame. */
typedef enum VkrEditorPerfGraphValue {
  VKR_EDITOR_PERF_GRAPH_MEAN = 0,
  VKR_EDITOR_PERF_GRAPH_MAX,
  VKR_EDITOR_PERF_GRAPH_FRAME,
  VKR_EDITOR_PERF_GRAPH_VALUE_COUNT,
} VkrEditorPerfGraphValue;

/* The Render graph tab's numbers for one recording, indexed like the shown
   graph's passes. They change only when the update window steps, so they
   hold still while a playback plays. */
typedef struct VkrEditorPerfGraph {
  bool8_t valid;
  /* What the values were computed for: the graph, the window step, the
     update period in ms (zero every frame) and the value. */
  uint32_t graph;
  uint64_t step;
  uint32_t rate_ms;
  uint32_t value;
  /* The window's frames, and those of them with pass times. */
  uint64_t first;
  uint64_t last;
  uint32_t timed_frames;
  /* GPU times, else CPU times when the recording has none. */
  bool8_t gpu;
  float32_t ms[VKR_EDITOR_PERF_GRAPH_PASSES];
  float32_t cpu_ms[VKR_EDITOR_PERF_GRAPH_PASSES];
  bool8_t has[VKR_EDITOR_PERF_GRAPH_PASSES];
  float32_t total_ms;
  /* When each value last moved by more than VKR_EDITOR_PERF_GRAPH_FLASH,
     in platform seconds, for its cell's fading highlight. */
  float64_t changed_at[VKR_EDITOR_PERF_GRAPH_PASSES];
  /* Each pass's value at the window and the 59 windows before it, oldest
     first. */
  float32_t history[VKR_EDITOR_PERF_GRAPH_PASSES]
                   [VKR_EDITOR_PERF_GRAPH_HISTORY];
  /* Each pass's median time over the shown frames. */
  float32_t median[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint32_t median_graph;
  uint64_t median_first;
  uint64_t median_last;
  /* The flow's layout of graph `layout_graph`: each pass's dependency depth
     and its place in its row. */
  uint32_t layout_graph;
  uint16_t depth[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint16_t slot[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint16_t row_width[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint32_t depth_count;
  uint32_t widest;
} VkrEditorPerfGraph;

/* Series the Memory tab stacks: the largest allocators or GPU owners, then
   the rest as one. */
#define VKR_EDITOR_PERF_MEMORY_SERIES 7u
/* GPU allocation owners the Memory tab reads at most. */
#define VKR_EDITOR_PERF_GPU_OWNERS 16u

/* The Memory tab's charts over the frame chart's columns and its
   statistics over the shown frames, rebuilt when the columns are. */
typedef struct VkrEditorPerfMemory {
  uint32_t columns_version;
  bool8_t valid;
  /* RAM by allocator: the stacked allocator ids, the last series the
     rest; and the process's resident size when no allocator is named. */
  uint32_t ram_series[VKR_EDITOR_PERF_MEMORY_SERIES];
  uint32_t ram_series_count;
  bool8_t ram_by_allocator;
  float32_t ram_mb[VKR_EDITOR_PERF_MEMORY_SERIES][VKR_EDITOR_PERF_COLUMNS];
  float32_t ram_top;
  /* VRAM by owner: the stacked owners (indices into owner_series), the
     last series the rest. */
  uint32_t owner_count;
  uint32_t owner_series[VKR_EDITOR_PERF_GPU_OWNERS];
  uint32_t owner_allocations[VKR_EDITOR_PERF_GPU_OWNERS];
  uint32_t owner_created[VKR_EDITOR_PERF_GPU_OWNERS];
  uint32_t vram_series[VKR_EDITOR_PERF_MEMORY_SERIES];
  uint32_t vram_series_count;
  float32_t vram_mb[VKR_EDITOR_PERF_MEMORY_SERIES][VKR_EDITOR_PERF_COLUMNS];
  float32_t vram_top;
  /* Allocations up and frees down per column, from the named allocators. */
  float32_t allocs[VKR_EDITOR_PERF_COLUMNS];
  float32_t frees[VKR_EDITOR_PERF_COLUMNS];
  float32_t rate_top;
  /* Over the shown frames. */
  float64_t allocs_per_frame;
  float64_t frees_per_frame;
  float64_t scopes_per_frame;
  uint32_t deepest_scope;
  int64_t live_growth;
} VkrEditorPerfMemory;

/* CPU stages of the frame's recording the Backend tab stacks. */
#define VKR_EDITOR_PERF_RECORD_STAGES 6u

/* The Backend tab's chart over the frame chart's columns, rebuilt when the
   columns are. */
typedef struct VkrEditorPerfBackend {
  uint32_t columns_version;
  bool8_t valid;
  uint32_t stage_series[VKR_EDITOR_PERF_RECORD_STAGES];
  float32_t stage_ms[VKR_EDITOR_PERF_RECORD_STAGES][VKR_EDITOR_PERF_COLUMNS];
  float32_t stage_top;
  bool8_t has_stages;
} VkrEditorPerfBackend;

/* Census states the Draws tab stacks over time: drawn, drawn late,
   occluded, outside the frustum and past the draw distance. */
#define VKR_EDITOR_PERF_DRAW_SERIES 5u

/* The Draws tab's per-object view of one frame and its chart over the frame
   chart's columns. The object arrays come from the Performance window's
   memory and follow the box set in effect. */
typedef struct VkrEditorPerfDraws {
  uint32_t columns_version;
  bool8_t valid;
  float32_t counts[VKR_EDITOR_PERF_DRAW_SERIES][VKR_EDITOR_PERF_COLUMNS];
  float32_t top;
  /* The box set the arrays below follow, its objects' extent on the ground
     (x, z) and their order by footprint, largest first. */
  uint32_t bounds;
  uint32_t object_count;
  uint32_t *order;
  Vec2 extent_min;
  Vec2 extent_max;
  /* Each candidate of id table `ids`'s object index, or UINT32_MAX. */
  uint32_t ids;
  uint32_t candidate_count;
  uint32_t *candidate_object;
  /* Each object's VKR_RECORDING_DRAW_STATE_* in frame `state_frame`, its
     most visible candidate's, or UINT8_MAX without one. */
  uint64_t state_frame;
  uint8_t *object_state;
  /* That frame's candidates by state. */
  uint32_t state_counts[VKR_RECORDING_DRAW_STATE_DISTANCE + 1u];
} VkrEditorPerfDraws;

/* The Performance window's tabs. Overview holds the frame charts, the
   inspector, the spikes and the passes; the others each read one part of
   the recording around the frame the window shows. */
typedef enum VkrEditorPerfTab {
  VKR_EDITOR_PERF_TAB_OVERVIEW = 0,
  VKR_EDITOR_PERF_TAB_BACKEND,
  VKR_EDITOR_PERF_TAB_MEMORY,
  VKR_EDITOR_PERF_TAB_DRAWS,
  VKR_EDITOR_PERF_TAB_GRAPH,
  VKR_EDITOR_PERF_TAB_COUNT,
} VkrEditorPerfTab;

/* The performance view of one recording (docs/proposals/recordings.md,
   "Performance view"): its loaded data, analysis and plot columns. */
/* The Performance window's layout and tables, kept when another recording
   loads. */
typedef struct VkrEditorRecordingPerfView {
  /* The shown tab, a VkrEditorPerfTab. */
  uint32_t tab;
  /* Charts above the lists, spikes beside passes, and the charts beside the
     frame inspector. */
  VkrEditorSplit charts;
  VkrEditorSplit lists;
  VkrEditorSplit inspector;
  /* CPU stages the stage chart leaves out, by its legend. */
  bool8_t stage_hidden[VKR_EDITOR_PERF_STAGES];
  VkrEditorTable spikes;
  VkrEditorTable passes;
  VkrEditorTable passes_compare;
  /* The right list shows every metrics series instead of the passes. */
  bool8_t show_series;
  VkrEditorTable series;
  /* Series whose name holds this text, ASCII case-insensitive. */
  uint8_t series_search[64];
  uint32_t series_search_length;
  /* Render graph tab: the flow instead of the list, the update period as
     an index into its choices plus one (zero for the default), the value
     shown (VkrEditorPerfGraphValue), the pass table, the flow's canvas, and
     the selected pass by name. */
  bool8_t graph_flow;
  uint32_t graph_rate;
  uint32_t graph_value;
  VkrEditorTable graph_table;
  VkrEditorPerfCanvas graph_canvas;
  char graph_selected[VKR_RECORDING_PASS_NAME_MAX + 1u];
  /* Memory tab: its two tables. */
  VkrEditorTable memory_where;
  VkrEditorTable memory_arenas;
  /* Backend tab: its three tables. */
  VkrEditorTable backend_passes;
  VkrEditorTable backend_counters;
  VkrEditorTable backend_functions;
  /* Draws tab: the map, its layers (VKR_EDITOR_PERF_DRAW_LAYER_* bits plus
     one, zero for the default), whether it follows the camera, the section
     table's kind (0 counters, 1 cells) and the table. */
  VkrEditorPerfCanvas draws_canvas;
  uint32_t draws_layers;
  bool8_t draws_follow;
  uint32_t draws_sections;
  VkrEditorTable draws_table;
} VkrEditorRecordingPerfView;

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
  /* Grows whenever the columns are rebuilt, so tabs know to rebuild theirs. */
  uint32_t columns_version;
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
  /* Shown frames that carry the pass chart's times. */
  uint64_t pass_view_frames;
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
  /* The frame under the pointer on any chart in the last build, which every
     chart marks and the inspector shows while nothing is selected; or
     UINT64_MAX. */
  uint64_t hover_frame;
  uint64_t hover_next;
  /* The frames a drag across a chart covers while it lasts, which every
     chart marks; its release zooms to them. */
  bool8_t brushing;
  uint64_t brush_first;
  uint64_t brush_last;
  /* The frames the charts show, [view_first, view_last]; the whole
     recording until a drag across a chart zooms in. */
  uint64_t view_first;
  uint64_t view_last;
  /* Frame-time statistics over the shown frames, for the cards while
     zoomed. */
  VkrRecordingStats view_time;
  float64_t view_low_1pct;
  uint64_t view_over_budget;
  /* A metrics series the Series table charted, or UINT32_MAX, and its
     columns and range. */
  uint32_t chart_series;
  float32_t series_values[VKR_EDITOR_PERF_COLUMNS];
  float32_t series_range[2];
  VkrEditorRecordingPerfView view;
  /* Compare: recording B against this one, A, read into the same memory,
     and B's frame time at each column's frame for the frame chart's line,
     holding the last one where B has none. */
  bool8_t comparing;
  char compare_path[VKR_RECORDING_PATH_MAX];
  char compare_name[VKR_RECORDING_LABEL_MAX + 1u];
  VkrRecordingData compare_data;
  VkrRecordingAnalysis compare_analysis;
  VkrRecordingCompare compare;
  float32_t compare_ms[VKR_EDITOR_PERF_COLUMNS];
  /* The Render graph tab's numbers. */
  VkrEditorPerfGraph graph;
  /* The Memory tab's charts and statistics. */
  VkrEditorPerfMemory memory_tab;
  /* The Backend tab's chart. */
  VkrEditorPerfBackend backend;
  /* The Draws tab's objects and chart. */
  VkrEditorPerfDraws draws;
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
  /* Record every measurement ("measurements": "all"). */
  bool8_t all_measurements;
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

/* What the panel over the playback bar shows besides the note field. */
typedef enum VkrEditorRecordingSelection {
  VKR_EDITOR_RECORDING_SELECTION_NONE = 0,
  VKR_EDITOR_RECORDING_SELECTION_NOTE,
  VKR_EDITOR_RECORDING_SELECTION_SEGMENT,
} VkrEditorRecordingSelection;

/* The annotations of one recording, cached while its playback is open or an
   operation edits them, and the playback bar's note and segment tools. */
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
  /* A segment being marked: its first frame, until a second press ends it. */
  bool8_t marking;
  uint64_t segment_first;
  /* The note or segment selected on the track or in the Segments list, by
     index into the cached annotations; any change to the annotations clears
     it, as indices move. A selected segment's title and note are edited
     here and written on Save, Enter, Close or another selection; `edit_focus`
     gives the title the keyboard on the next build. */
  VkrEditorRecordingSelection selected;
  uint32_t selected_index;
  char title[VKR_RECORDING_SEGMENT_NAME_MAX];
  uint64_t title_length;
  char note[VKR_RECORDING_SEGMENT_NOTE_MAX];
  uint64_t note_length;
  bool8_t edit_focus;
  /* The Segments list over the bar, and its page of rows. */
  bool8_t listing;
  uint32_t list_page;
  /* Viewing one segment alone: the timeline shows its frames and the
     playback loops them. Kept by frames, which survive index changes. */
  bool8_t focusing;
  uint64_t focus_first;
  uint64_t focus_last;
} VkrEditorRecordingNotes;

/* Columns the timeline's frame-time lane draws at most; each shows the
   slowest frame of its run of frames. */
#define VKR_EDITOR_TIMELINE_COLUMNS 512u
/* The System lane's rows (ADR-125): thermal level (0 to 3), memory pressure
   (0 to 2), low-power mode (0 or 1) and errors (bit 0 GPU, bit 1 log); a
   column without a sample of a row holds VKR_EDITOR_TIMELINE_SYSTEM_NONE. */
#define VKR_EDITOR_TIMELINE_SYSTEM_ROWS 4u
#define VKR_EDITOR_TIMELINE_SYSTEM_NONE 0xFFu

/* The timeline over the playback bar: the frames it shows, the frames
   brushed on it, and the frame-time columns built for that view. */
typedef struct VkrEditorRecordingTimeline {
  bool8_t open;
  /* The bundle the view belongs to; another one resets it. */
  char bundle[VKR_RECORDING_PATH_MAX];
  uint64_t view_first;
  uint64_t view_last;
  /* A brushed selection, inclusive; `selected` is false without one. */
  bool8_t selected;
  uint64_t select_first;
  uint64_t select_last;
  /* Columns of [columns_first, columns_last]: each column's slowest frame,
     its time and its budget class (0 within, 1 up to twice, 2 beyond). */
  uint64_t columns_first;
  uint64_t columns_last;
  uint32_t column_count;
  uint64_t column_frame[VKR_EDITOR_TIMELINE_COLUMNS];
  float32_t column_ms[VKR_EDITOR_TIMELINE_COLUMNS];
  uint8_t column_class[VKR_EDITOR_TIMELINE_COLUMNS];
  float32_t top_ms;
  /* With a memory lane: each column's highest resident and GPU driver
     memory in GB, carried over columns without a sample, and each line's
     lowest and highest column; -1 for a line without samples. */
  bool8_t memory_lane;
  float32_t column_ram[VKR_EDITOR_TIMELINE_COLUMNS];
  float32_t column_vram[VKR_EDITOR_TIMELINE_COLUMNS];
  float32_t memory_min_gb[2];
  float32_t memory_max_gb[2];
  VkrUiId memory_id;
  uint32_t memory_hovered;
  /* With a System lane: the platform signals of each column's frames, read
     from the playback's platform fields. Each row keeps the column's worst
     value, 1 in system_band where it is drawn, and the view's worst in
     system_worst; system_footprint is each column's highest footprint in
     bytes, negative without a sample. system_source is the playback's
     field array, compared only, so another recording rebuilds the columns;
     system_errors says the recording counts GPU or log errors. */
  bool8_t system_lane;
  bool8_t system_errors;
  const void *system_source;
  uint8_t system_level[VKR_EDITOR_TIMELINE_SYSTEM_ROWS]
                      [VKR_EDITOR_TIMELINE_COLUMNS];
  float32_t system_band[VKR_EDITOR_TIMELINE_SYSTEM_ROWS]
                       [VKR_EDITOR_TIMELINE_COLUMNS];
  uint8_t system_worst[VKR_EDITOR_TIMELINE_SYSTEM_ROWS];
  float32_t system_footprint[VKR_EDITOR_TIMELINE_COLUMNS];
  uint32_t system_hovered;
  /* The frame-time lane's id, for its last drawn rectangle, and the column
     the pointer was over in the last build, for the tooltip. */
  VkrUiId plot_id;
  uint32_t hovered_column;
  /* The recording has thumbnails, so the hover preview, not the tooltip,
     describes the hovered frame. */
  bool8_t previewed;
} VkrEditorRecordingTimeline;

/* The Thread timeline window (editor_recording_flame.c): the span of the
   open playback's clock it shows, in ns, the playhead frame it last
   followed and a seek of its own it waits for, its vertical scroll, the
   selected zone or GPU span by index, and the pointer's drag. Another
   bundle resets it. */
typedef struct VkrEditorRecordingFlame {
  char bundle[VKR_RECORDING_PATH_MAX];
  int64_t view_begin_ns;
  int64_t view_end_ns;
  uint64_t followed_frame;
  uint64_t awaited_frame;
  uint32_t awaited_builds;
  float32_t scroll_pt;
  bool8_t selected;
  bool8_t selected_gpu;
  uint32_t selected_index;
  /* The pointer drags the view; drag_px is where it was last build. */
  bool8_t dragging;
  Vec2 drag_px;
} VkrEditorRecordingFlame;

/* One thumbnail on its way to disk: pixels sampled from the capture on the
   main thread, then encoded and written by a thread of its own. */
typedef struct VkrEditorRecordingThumbJob {
  VkrThread thread;
  VkrAtomicBool done;
  bool8_t running;
  /* Set by the thread: the JPEG, and thumb.png when `cover`, were written. */
  bool8_t written;
  bool8_t cover;
  /* VKR_EDITOR_RECORDING_THUMB_WIDTH x _HEIGHT RGBA8, allocated with the
     first thumbnail and kept. */
  uint8_t *pixels;
  uint32_t width;
  uint32_t height;
  uint64_t frame;
  char bundle[VKR_RECORDING_PATH_MAX];
} VkrEditorRecordingThumbJob;

/* A thumbnail loaded for the timeline's hover preview. */
typedef struct VkrEditorRecordingPreview {
  uint64_t frame;
  VkrResourceHandleInfo request;
  VkrTextureHandle texture;
  uint32_t width;
  uint32_t height;
  bool8_t failed;
  /* The build that last asked for it; the oldest is replaced. */
  uint64_t used;
} VkrEditorRecordingPreview;

typedef struct VkrEditorRecordingThumbs {
  /* Capturing: the bundle recording, the capture's token (its high bit keeps
     it apart from agent captures), builds waited, the recorder frame it
     shows, captures failed in a row, and when the next one is due.
     `cover_done`: thumb.png exists. */
  char bundle[VKR_RECORDING_PATH_MAX];
  uint64_t token;
  uint64_t serial;
  uint32_t wait;
  uint32_t failures;
  uint64_t frame;
  float64_t next_time;
  bool8_t cover_done;
  /* The warm-up capture while nothing records: its token, builds waited,
     failures, the readback bytes a capture has needed so far, and the
     Scene image's bytes with the builds it has kept them. */
  uint64_t warm_token;
  uint32_t warm_wait;
  uint32_t warm_failures;
  uint64_t warm_bytes;
  uint64_t warm_size;
  uint32_t warm_stable;
  /* The allocator the write threads come from. */
  VkrAllocator *allocator;
  VkrEditorRecordingThumbJob job;
  /* Previewing: the thumbnail frames of the bundle playing, ascending, read
     once per bundle, and the loaded previews. */
  char index_bundle[VKR_RECORDING_PATH_MAX];
  uint64_t *frames;
  uint32_t frame_count;
  uint32_t frame_capacity;
  VkrEditorRecordingPreview previews[VKR_EDITOR_RECORDING_PREVIEWS];
  uint64_t builds;
} VkrEditorRecordingThumbs;

typedef struct VkrEditorRecording {
  /* Requests waiting for a frame; the runtime takes one per frame. */
  VkrSampleRecordingRequest queue[VKR_EDITOR_RECORDING_QUEUE];
  uint32_t queue_count;
  /* Builds the first queued Watch or Drive has waited for the project
     scene it was recorded in to open; zero before it asked for one. */
  uint32_t scene_wait;
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
  /* Record takes every measurement (the Recordings window's checkbox,
     kept with the project's editor settings). */
  bool8_t all_measurements;
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
  /* Route points collision hides from the camera, swept a bounded number
     a build from the cursor; `path_swept` counts the points swept since
     the view last changed, `path_view` that view. */
  bool8_t path_hidden[VKR_EDITOR_RECORDING_PATH_POINTS];
  uint32_t path_cursor;
  uint32_t path_swept;
  Mat4 path_view;
  VkrUiId path_id;
  VkrEditorRecordingRequests requests;
  VkrEditorRecordingNotes notes;
  VkrEditorRecordingTimeline timeline;
  VkrEditorRecordingFlame flame;
  /* Reused while the list reads each bundle's annotations. */
  VkrRecordingAnnotations list_annotations;
  /* One past the entry whose Delete was pressed once, or zero; a second
     press deletes it. */
  uint32_t delete_armed;
  float32_t scroll;
  VkrEditorRecordingThumbs thumbs;
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

/** The colour of a recording marker of `kind`, on the bar and timeline. */
Vec4 vkr_editor_recording_marker_color(uint32_t kind);

/** Sets the open playback's speed and loop; `loop_start` and `loop_end` are
 * the recorded times it loops between, an empty span the whole recording.
 * Every settings change passes the span it keeps. */
void vkr_editor_recording_settings(struct VkrEditorUi *editor, float32_t speed,
                                   bool8_t loop, float64_t loop_start,
                                   float64_t loop_end);

/** Starts a recording named `name` (a time stamp when empty) by `author`
 * that measures `tier`, or with `all_measurements` every measurement the
 * engine has; `zones` adds every thread's CPU zones for the timeline.
 * Returns its token, or zero with the reason in `error`. */
uint64_t vkr_editor_recording_start(struct VkrEditorUi *editor,
                                    const char *name, const char *author,
                                    VkrRecordingTier tier,
                                    bool8_t all_measurements, bool8_t zones,
                                    bool8_t toast, char *error,
                                    uint64_t error_capacity);

/* Seconds the Recordings window's Keep last control keeps. */
#define VKR_EDITOR_RECORDING_ROLLING_SECONDS 30.0

/** Starts keeping the last `seconds` in memory (a rolling recording at Tier
 * 1 with CPU zones); nothing is written until
 * vkr_editor_recording_save_window. Returns its token, or zero with the
 * reason in `error`. */
uint64_t vkr_editor_recording_start_rolling(struct VkrEditorUi *editor,
                                            float64_t seconds, bool8_t toast,
                                            char *error,
                                            uint64_t error_capacity);

/** Saves the rolling recording's held seconds as a recording named `name`
 * (a time stamp when empty) in the recording directory; the rolling
 * recording keeps running. Returns its token, or zero with the reason. */
uint64_t vkr_editor_recording_save_window(struct VkrEditorUi *editor,
                                          const char *name, bool8_t toast,
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

/** Compares recording `name` (B, a bundle path or name) with the analyzed
 * recording (A) in the Performance window, aligned by `align` (`marker`
 * names the label for VKR_RECORDING_ALIGN_MARKER; NULL takes the first
 * both carry). False with the reason in `error`, leaving any comparison
 * before it. */
bool8_t vkr_editor_recording_compare(struct VkrEditorUi *editor,
                                     const char *name, VkrRecordingAlign align,
                                     const char *marker, char *error,
                                     uint64_t error_capacity);
/** Ends the comparison; A stays analyzed. */
void vkr_editor_recording_compare_stop(VkrEditorRecordingPerf *perf);

/** Frees the Performance window's data. */
void vkr_editor_recording_perf_release(VkrEditorRecordingPerf *perf);

/** The Performance window's body. */
void vkr_editor_recording_perf_build(struct VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds);

/** The frame the Performance window's tabs describe: the open playback's
 * playhead while it plays, else the selected frame, else the playhead, else
 * the frame under the pointer on a chart; UINT64_MAX when there is none. */
uint64_t vkr_editor_perf_focus(const VkrEditorRecordingPerf *perf,
                               const VkrSampleUiFrame *frame);

/** A tab's message in the middle of `placement`, such as what a recording
 * lacks for it and how to record it. */
void vkr_editor_perf_empty(struct VkrEditorUi *editor, VkrUiSystem *ui,
                           VkrUiPlacement placement, VkrUiIcon icon,
                           const char *title, const char *text);

/* Layout helpers of the Performance window's tab files. */
VkrUiPlacement vkr_editor_perf_cell(uint32_t column, uint32_t row);
VkrUiPanelConfig vkr_editor_perf_grid(VkrUiPlacement placement,
                                      const VkrUiTrack *columns,
                                      uint32_t column_count,
                                      const VkrUiTrack *rows,
                                      uint32_t row_count);
/** Caption-size text at `placement`, left and vertically centred. */
void vkr_editor_perf_caption(VkrUiSystem *ui, const char *id, String8 text,
                             VkrUiPlacement placement, Vec4 color);

/** Selects `target` for every tab, and moves an open playback of this
 * recording to it. */
void vkr_editor_perf_select(struct VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame, uint64_t target);

/** A row of `count` options of which `selected` is on; returns the option
 * clicked this build, else `selected`. `tips` may be NULL. */
uint32_t vkr_editor_perf_segmented(VkrUiSystem *ui, const char *id,
                                   VkrUiPlacement placement,
                                   const char *const *labels,
                                   const char *const *tips, uint32_t count,
                                   uint32_t selected);

/** A stat card in column `column` of a row of cards: a caption, a value in
 * `color`, and an optional last line. */
void vkr_editor_perf_card(struct VkrEditorUi *editor, VkrUiSystem *ui,
                          uint32_t column, const char *caption, String8 value,
                          Vec4 color, String8 line, Vec4 line_color);

/** A chart of the shown frames over the frame chart's columns
 * (VkrEditorRecordingPerf.column_frame); with `passes`, over the pass
 * chart's. It marks the selected frame and the one under the pointer on any
 * chart, a click selects a column's frame and a drag zooms every chart.
 * An empty `tooltip` describes the hovered column's frame time. */
void vkr_editor_perf_plot(struct VkrEditorUi *editor,
                          const VkrSampleUiFrame *frame, const char *id,
                          VkrUiPlacement placement, VkrUiPlotConfig *plot,
                          bool8_t passes, String8 tooltip);

/* Height of a chart title with keys: the title over a line of keys. */
#define VKR_EDITOR_PERF_CHART_TITLE_PT 34.0f

/** A chart's title in caption text over a line with a dot and a name per
 * key; VKR_EDITOR_PERF_CHART_TITLE_PT tall with keys, 16 without. */
void vkr_editor_perf_chart_title(VkrUiSystem *ui, const char *id,
                                 VkrUiPlacement placement, String8 title,
                                 const char *const *keys, const Vec4 *colors,
                                 uint32_t key_count);

/** The Backend tab's body (editor_recording_perf_backend.c). */
void vkr_editor_recording_perf_backend_build(struct VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame,
                                             VkrUiPlacement placement);

/** The backend that made the analyzed recording, from its pipeline class:
 * "Metal" for tiled, "Vulkan" for desktop, else "Backend". */
const char *vkr_editor_perf_backend_name(const VkrEditorRecordingPerf *perf);

/** The Draws tab's body (editor_recording_perf_draws.c). */
void vkr_editor_recording_perf_draws_build(struct VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiPlacement placement);

/** Releases the Draws tab's object arrays. */
void vkr_editor_recording_perf_draws_release(VkrEditorRecordingPerf *perf);

/** The Memory tab's body (editor_recording_perf_memory.c). */
void vkr_editor_recording_perf_memory_build(struct VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            VkrUiPlacement placement);

/** The Render graph tab's body (editor_recording_perf_graph.c). */
void vkr_editor_recording_perf_graph_build(struct VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiPlacement placement);

/** World point `world` in the canvas's points, from its top-left. */
Vec2 vkr_editor_perf_canvas_point(const VkrEditorPerfCanvas *canvas,
                                  Vec2 world);

/** Centres [min, max] in the canvas with `margin_pt` around it; zoom then
 * ranges about this fit. */
void vkr_editor_perf_canvas_fit(VkrEditorPerfCanvas *canvas, Vec2 min, Vec2 max,
                                float32_t margin_pt);

/** The pointer on a canvas at `placement`: build it first in its cell, then
 * what it shows over it. The wheel zooms about the pointer and a drag pans;
 * `tooltip` follows the pointer. */
VkrEditorPerfCanvasInput
vkr_editor_perf_canvas_input(VkrUiSystem *ui, const char *id,
                             VkrEditorPerfCanvas *canvas,
                             VkrUiPlacement placement, String8 tooltip);

/* ---- Thread timeline (editor_recording_flame.c) ---- */

/** The Thread timeline window's body: each thread's CPU zones and the GPU's
 * frames and passes around the open playback's playhead (ADR-126). */
void vkr_editor_recording_flame_build(struct VkrEditorUi *editor,
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

/* ---- Timeline (editor_recording_timeline.c) ---- */

/** The timeline's height in points when open over the open playback, else
 * zero. */
float32_t vkr_editor_recording_timeline_height(struct VkrEditorUi *editor);
/** The timeline above the playback bar, whose top-left corner and width are
 * `bar_pt`: the frame-time lane, markers, notes and segments, a ruler, zoom,
 * and actions on a brushed selection. */
void vkr_editor_recording_timeline_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         Vec3 bar_pt);
/** Opens the timeline on frames `first` to `last` with a margin of a tenth
 * of their span each side, or on the whole recording when `first` is
 * UINT64_MAX. */
void vkr_editor_recording_timeline_show(struct VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback,
                                        uint64_t first, uint64_t last);

/* ---- Thumbnails (editor_recording_thumbs.c) ---- */

/** While the Scene records, captures it twice a second without the editor
 * UI and writes each capture under the bundle's thumbs/, the first also as
 * thumb.png. Call once per build. */
void vkr_editor_recording_thumbs_update(struct VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame);
/** The thumbnail nearest at or before `frame_index` of the bundle at
 * `bundle_path`, once loaded: its texture and size. False while it loads or
 * when the bundle has none; `out_any` says whether it has any. */
bool8_t vkr_editor_recording_thumb_preview(struct VkrEditorUi *editor,
                                           VkrUiSystem *ui,
                                           const char *bundle_path,
                                           uint64_t frame_index,
                                           VkrTextureHandle *out_texture,
                                           Vec2 *out_size, bool8_t *out_any);
/** The frames of the bundle's thumbnails, ascending; borrowed until
 * another bundle's are listed. */
uint32_t vkr_editor_recording_thumb_frames(struct VkrEditorUi *editor,
                                           const char *bundle_path,
                                           const uint64_t **out_frames);
/** The file of the bundle's thumbnail at or before `frame_index`, else its
 * first; false when it has none. */
bool8_t vkr_editor_recording_thumb_at(struct VkrEditorUi *editor,
                                      const char *bundle_path,
                                      uint64_t frame_index, char *out,
                                      uint64_t capacity);
/** Waits for a thumbnail being written and releases the previews. */
void vkr_editor_recording_thumbs_shutdown(struct VkrEditorUi *editor);

/* ---- Perf re-run (editor_recording_rerun.c) ---- */

/* A written re-run case: where it is, the recording copy it names, the
   frames it replays, how many it measures and the harness command. */
typedef struct VkrEditorRecordingRerun {
  char case_path[VKR_RECORDING_PATH_MAX];
  char recording_path[VKR_RECORDING_PATH_MAX];
  char command[1536];
  uint64_t first_frame;
  uint64_t last_frame;
  uint64_t measure_frames;
  float64_t seconds;
} VkrEditorRecordingRerun;

/** Writes a harness case that replays frames `first` to `last` (UINT64_MAX:
 * the end) of recording `name` with the windowed timing case's settings, and
 * copies the recording where the case names it; the harness runs it. False
 * with the reason in `error`, such as a scene outside the repository. */
bool8_t vkr_editor_recording_rerun(struct VkrEditorUi *editor, const char *name,
                                   uint64_t first, uint64_t last,
                                   VkrEditorRecordingRerun *out, char *error,
                                   uint64_t error_capacity);

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
/** The playback bar's tools: open or close the note field, mark a
 * segment's first or last frame at the playhead, open or close the Segments
 * list, and set the review state, flagging it when `flag`. */
void vkr_editor_recording_note_toggle(struct VkrEditorUi *editor);
void vkr_editor_recording_segment_mark(struct VkrEditorUi *editor,
                                       const VkrRecordingPlayback *playback);
void vkr_editor_recording_segments_toggle(struct VkrEditorUi *editor);
/** Selects segment `index` of the cached annotations and opens its panel,
 * writing the open segment's unsaved title and note first. */
void vkr_editor_recording_segment_select(struct VkrEditorUi *editor,
                                         uint32_t index);
/** Adds frames `first` to `last` as a new segment and opens it for its
 * title and note; false after a toast saying why not. */
bool8_t vkr_editor_recording_segment_add(struct VkrEditorUi *editor,
                                         const VkrRecordingPlayback *playback,
                                         uint64_t first, uint64_t last);
void vkr_editor_recording_review_toggle(struct VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback,
                                        bool8_t flag);
/** The open playback's segments and notes on the playback bar's track, whose
 * pixel rectangle is `slider_px`. */
void vkr_editor_recording_annotations_track(
    struct VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, VkrUiRect slider_px);
/** The rectangle in points of the panel over the playback bar (the note
 * field, the Segments list, or the selected note or segment) while one is
 * open, from the bar's top-left corner and width. */
bool8_t vkr_editor_recording_composer_rect(const struct VkrEditorUi *editor,
                                           Vec3 bar_pt, Vec4 *out_pt);
/** The note field over the playback bar, whose top-left corner and width
 * are `bar_pt`; it adds the note at the playhead on Enter or Add. */
void vkr_editor_recording_composer_build(struct VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         Vec3 bar_pt);
/** Puts a prompt on the clipboard that points an agent at the recording:
 * its facts, markers, notes, segments with their notes, review, the request
 * it answers and the playhead when it is open. */
void vkr_editor_recording_copy_prompt(struct VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const VkrEditorRecordingEntry *entry);
/** Fills the entry's note and segment counts and review state. */
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
