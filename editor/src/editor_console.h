#pragma once

#include "core/logger.h"
#include "renderer/systems/vkr_ui_system.h"

/** Console storage lives from editor initialization through UI shutdown. */
typedef struct VkrEditorConsole {
  VkrAllocator *allocator;
  VkrLogRecord *records;
  uint32_t filtered[VKR_LOG_HISTORY_CAPACITY];
  uint64_t row_sequences[48];
  uint32_t count;
  uint32_t write_index;
  uint32_t filtered_count;
  uint32_t first_row;
  uint64_t after_sequence;
  uint64_t selected_sequence;
  uint64_t selection_anchor;
  uint64_t missed_count;
  uint64_t evicted_count;
  uint64_t pending_count;
  uint64_t detail_sequence;
  uint8_t search[256];
  uint32_t search_length;
  uint8_t detail[VKR_LOG_MESSAGE_CAPACITY + VKR_LOG_SOURCE_CAPACITY + 96u];
  uint32_t detail_length;
  bool8_t levels[6];
  VkrUiId filters_button_id;
  uint32_t filter_focus_index;
  bool8_t filters_open;
  bool8_t filter_focus_pending;
  bool8_t follow_tail;
  bool8_t filter_dirty;
  bool8_t initialized;
} VkrEditorConsole;

bool8_t vkr_editor_console_init(VkrEditorConsole *console, VkrAllocator *owner);
void vkr_editor_console_shutdown(VkrEditorConsole *console);
/** Called inside the dock's Console content panel. Rect is its pixel bounds. */
void vkr_editor_console_build(VkrEditorConsole *console, VkrUiSystem *ui,
                              VkrUiRect content_rect_px, VkrFontHandle heading);
