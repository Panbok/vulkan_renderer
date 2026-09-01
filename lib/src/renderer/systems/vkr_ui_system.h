/**
 * @file vkr_ui_system.h
 * @brief Immediate-mode UI backed by retained widget and text state.
 */
#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_draw.h"
#include "core/ui/vkr_ui_grid.h"
#include "core/ui/vkr_ui_id.h"
#include "core/ui/vkr_ui_style.h"
#include "core/ui/vkr_ui_tile.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/ui/vkr_ui_text.h"

struct s_RendererFrontend;
typedef struct s_RendererFrontend RendererFrontend;
typedef struct VkrUiRetainedState VkrUiRetainedState;
typedef struct VkrUiFrameNode VkrUiFrameNode;
typedef struct VkrPreparedUiDrawList VkrPreparedUiDrawList;

#define VKR_UI_FRAME_NODE_CAPACITY 1024u
#define VKR_UI_RETAINED_BUCKET_CAPACITY 2048u
#define VKR_UI_RETAINED_GRACE_FRAMES 120u
#define VKR_UI_CONTAINER_STACK_CAPACITY 32u

typedef struct VkrUiInputCapture {
  bool8_t mouse;
  bool8_t keyboard;
  bool8_t text;
  VkrUiId hot_id;
  VkrUiId active_id;
} VkrUiInputCapture;

/** Grid placement authored in logical points. */
typedef struct VkrUiPlacement {
  uint32_t column;
  uint32_t row;
  uint32_t column_span;
  uint32_t row_span;
  VkrUiAlign justify;
  VkrUiAlign align;
  VkrUiEdges margin_pt;
} VkrUiPlacement;

#define VKR_UI_PLACEMENT_DEFAULT                                               \
  (VkrUiPlacement) {                                                           \
    .column = VKR_UI_GRID_AUTO, .row = VKR_UI_GRID_AUTO, .column_span = 1u,    \
    .row_span = 1u, .justify = VKR_UI_ALIGN_STRETCH,                           \
    .align = VKR_UI_ALIGN_STRETCH                                              \
  }

/** Track declarations and box style for a panel or scroll container. */
typedef struct VkrUiPanelConfig {
  VkrUiPlacement placement;
  const VkrUiTrack *columns;
  uint32_t column_count;
  const VkrUiTrack *rows;
  uint32_t row_count;
  VkrUiStyle style;
  bool8_t clip_children;
} VkrUiPanelConfig;

/** Shared placement, style, and text settings for leaf widgets. */
typedef struct VkrUiWidgetConfig {
  VkrUiPlacement placement;
  VkrUiStyle style;
  VkrUiTextConfig text;
} VkrUiWidgetConfig;

typedef struct VkrUiTextEditBuffer {
  uint8_t *data;
  uint32_t length;
  /** Includes space for the trailing NUL maintained by the text field. */
  uint32_t capacity;
} VkrUiTextEditBuffer;

VkrUiPanelConfig vkr_ui_panel_config_default(void);
VkrUiWidgetConfig vkr_ui_widget_config_default(void);

/** Persistent UI system plus one frame's scratch-backed immediate context. */
typedef struct VkrUiSystem {
  VkrDMemory retained_memory;
  VkrAllocator retained_allocator;
  VkrUiRetainedState **retained_buckets;
  uint32_t retained_bucket_capacity;
  uint32_t retained_count;

  VkrAllocator *frame_allocator;
  RendererFrontend *renderer;
  InputState *input;
  VkrUiFrameNode *frame_nodes;
  uint32_t frame_node_count;
  uint32_t frame_node_capacity;
  uint32_t container_stack[VKR_UI_CONTAINER_STACK_CAPACITY];
  uint32_t container_count;
  VkrUiIdStack id_stack;
  VkrUiDrawCommand *frame_commands;
  uint32_t frame_command_count;
  uint32_t frame_command_capacity;
  VkrUiTileCache tile_cache;
  VkrUiTileFrame tile_frame;
  float32_t dirty_tile_ratio;
  uint32_t dirty_tile_count;
  uint32_t tile_count;

  uint64_t frame_index;
  uint32_t target_width;
  uint32_t target_height;
  float32_t content_scale;
  float64_t delta_time;
  int32_t mouse_x;
  int32_t mouse_y;
  int8_t mouse_wheel;
  bool8_t mouse_captured;
  bool8_t mouse_pressed;
  bool8_t mouse_released;
  bool8_t focus_claimed;
  bool8_t focused_is_text;
  VkrUiId active_id;
  VkrUiId focused_id;
  VkrUiId hot_id;
  VkrUiInputCapture capture;
  Keys repeat_key;
  float64_t repeat_elapsed;
  float64_t repeat_next;

  uint32_t offscreen_width;
  uint32_t offscreen_height;
  bool8_t offscreen_enabled;
  float32_t offscreen_content_scale;
  uint32_t offscreen_content_scale_revision;
  uint32_t content_scale_revision;
  bool8_t frame_open;
  bool8_t draw_capacity_warning_emitted;
  bool8_t tile_build_warning_emitted;
  bool8_t initialized;
} VkrUiSystem;

bool8_t vkr_ui_system_init(RendererFrontend *rf, VkrUiSystem *system);
void vkr_ui_system_shutdown(RendererFrontend *rf, VkrUiSystem *system);
void vkr_ui_system_resize(RendererFrontend *rf, VkrUiSystem *system,
                          uint32_t width, uint32_t height);
void vkr_ui_system_set_offscreen_size(RendererFrontend *rf, VkrUiSystem *system,
                                      bool8_t enabled, uint32_t width,
                                      uint32_t height);
void vkr_ui_system_set_offscreen_content_scale(RendererFrontend *rf,
                                               VkrUiSystem *system,
                                               float32_t content_scale);

/** Begin one immediate UI frame. The root itself is a grid container. */
bool8_t vkr_ui_begin(RendererFrontend *rf, VkrUiSystem *system,
                     InputState *input, bool8_t mouse_captured,
                     float64_t delta_time, const VkrUiPanelConfig *root_config);

/** Resolve layout, retain state, and produce the frame's input capture. */
VkrUiInputCapture vkr_ui_end(VkrUiSystem *system);

bool8_t vkr_ui_push_id_label(VkrUiSystem *system, String8 label);
bool8_t vkr_ui_push_id_u64(VkrUiSystem *system, uint64_t key);
bool8_t vkr_ui_push_id_pointer(VkrUiSystem *system, const void *pointer);
bool8_t vkr_ui_pop_id(VkrUiSystem *system);

bool8_t vkr_ui_panel_begin(VkrUiSystem *system, String8 id_label,
                           const VkrUiPanelConfig *config);
bool8_t vkr_ui_panel_end(VkrUiSystem *system);
void vkr_ui_label(VkrUiSystem *system, String8 id_label, String8 content,
                  const VkrUiWidgetConfig *config);
bool8_t vkr_ui_button(VkrUiSystem *system, String8 id_label, String8 content,
                      const VkrUiWidgetConfig *config);
bool8_t vkr_ui_checkbox(VkrUiSystem *system, String8 id_label, String8 content,
                        bool8_t *value, const VkrUiWidgetConfig *config);
bool8_t vkr_ui_slider_f32(VkrUiSystem *system, String8 id_label,
                          float32_t *value, float32_t minimum,
                          float32_t maximum, const VkrUiWidgetConfig *config);
bool8_t vkr_ui_scroll_area_begin(VkrUiSystem *system, String8 id_label,
                                 const VkrUiPanelConfig *config);
bool8_t vkr_ui_scroll_area_end(VkrUiSystem *system);
bool8_t vkr_ui_text_field(VkrUiSystem *system, String8 id_label,
                          VkrUiTextEditBuffer *buffer,
                          const VkrUiWidgetConfig *config);

VkrUiInputCapture vkr_ui_system_capture(const VkrUiSystem *system);

/** Most recent CPU damage result; 1 means every tile needs redraw. */
float32_t vkr_ui_system_dirty_tile_ratio(const VkrUiSystem *system);

/** Build the packet-facing indexed stream from the resolved frame commands. */
bool8_t vkr_ui_system_prepare_draw_list(VkrUiSystem *system,
                                        VkrAllocator *frame_allocator,
                                        uint32_t target_width,
                                        uint32_t target_height,
                                        VkrPreparedUiDrawList *out_draw_list);
