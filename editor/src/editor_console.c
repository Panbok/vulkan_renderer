#include "editor_console.h"
#include "editor_internal.h"

#include <math.h>
#include <stdio.h>

#define VKR_CONSOLE_POLL_CAPACITY 256u
#define VKR_CONSOLE_VISIBLE_ROWS 48u
#define VKR_CONSOLE_PREVIEW_BYTES 160u
#define VKR_CONSOLE_ROW_HEIGHT 20.0f
#define VKR_CONSOLE_MESSAGE_LEFT_PT 96.0f

static const char *s_level_names[6] = {"Fatal", "Error", "Warn",
                                       "Info",  "Debug", "Trace"};
static const VkrUiIcon s_level_icons[6] = {
    VKR_UI_ICON_LOG_FATAL, VKR_UI_ICON_LOG_ERROR, VKR_UI_ICON_LOG_WARNING,
    VKR_UI_ICON_LOG_INFO,  VKR_UI_ICON_LOG_DEBUG, VKR_UI_ICON_LOG_TRACE};

/* Color only marks problems; ordinary output stays neutral. */
static Vec4 console_level_color(uint32_t level) {
  const VkrUiTheme *theme = vkr_ui_theme();
  switch (level) {
  case LOG_LEVEL_FATAL:
  case LOG_LEVEL_ERROR:
    return theme->error;
  case LOG_LEVEL_WARN:
    return theme->warning;
  case LOG_LEVEL_INFO:
    return theme->text;
  default:
    return theme->text_secondary;
  }
}

static Vec4 console_level_icon_color(uint32_t level) {
  const VkrUiTheme *theme = vkr_ui_theme();
  return level == LOG_LEVEL_INFO ? theme->info : console_level_color(level);
}

/* Severity chips each toggle one or two retained levels. */
typedef struct ConsoleSeverityChip {
  const char *label;
  VkrUiIcon icon;
  uint32_t first_level;
  uint32_t last_level;
} ConsoleSeverityChip;

static const ConsoleSeverityChip s_chips[4] = {
    {"Errors", VKR_UI_ICON_LOG_ERROR, LOG_LEVEL_FATAL, LOG_LEVEL_ERROR},
    {"Warnings", VKR_UI_ICON_LOG_WARNING, LOG_LEVEL_WARN, LOG_LEVEL_WARN},
    {"Info", VKR_UI_ICON_LOG_INFO, LOG_LEVEL_INFO, LOG_LEVEL_INFO},
    {"Verbose", VKR_UI_ICON_LOG_DEBUG, LOG_LEVEL_DEBUG, LOG_LEVEL_TRACE},
};

bool8_t vkr_editor_console_init(VkrEditorConsole *console,
                                VkrAllocator *owner) {
  if (!console || !owner)
    return false_v;
  *console = (VkrEditorConsole){
      .allocator = owner, .follow_tail = true_v, .filter_dirty = true_v};
  console->records = vkr_allocator_alloc(
      owner, (uint64_t)VKR_LOG_HISTORY_CAPACITY * sizeof(*console->records),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!console->records)
    return false_v;
  for (uint32_t i = 0u; i < ArrayCount(console->levels); ++i)
    console->levels[i] = true_v;
  console->initialized = true_v;
  return true_v;
}

void vkr_editor_console_shutdown(VkrEditorConsole *console) {
  if (!console || !console->initialized)
    return;
  vkr_allocator_free(console->allocator, console->records,
                     (uint64_t)VKR_LOG_HISTORY_CAPACITY *
                         sizeof(*console->records),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  *console = (VkrEditorConsole){0};
}

static bool8_t console_contains(const uint8_t *text, uint32_t length,
                                const uint8_t *query, uint32_t query_length) {
  if (query_length == 0u)
    return true_v;
  if (length < query_length)
    return false_v;
  for (uint32_t i = 0u; i <= length - query_length; ++i) {
    uint32_t j = 0u;
    for (; j < query_length; ++j) {
      uint8_t a = text[i + j], b = query[j];
      if (a >= 'A' && a <= 'Z')
        a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z')
        b += 'a' - 'A';
      if (a != b)
        break;
    }
    if (j == query_length)
      return true_v;
  }
  return false_v;
}

static void console_poll(VkrEditorConsole *console) {
  const uint32_t capacity =
      Min(VKR_CONSOLE_POLL_CAPACITY,
          VKR_LOG_HISTORY_CAPACITY - console->write_index);
  const VkrLogHistorySnapshot snapshot =
      log_history_snapshot(console->after_sequence,
                           console->records + console->write_index, capacity);
  if (snapshot.count) {
    const uint64_t first = console->records[console->write_index].sequence;
    if (first > console->after_sequence + 1u)
      console->missed_count += first - console->after_sequence - 1u;
    console->after_sequence =
        console->records[console->write_index + snapshot.count - 1u].sequence;
    if (console->count + snapshot.count > VKR_LOG_HISTORY_CAPACITY)
      console->evicted_count +=
          console->count + snapshot.count - VKR_LOG_HISTORY_CAPACITY;
    console->count =
        Min(console->count + snapshot.count, VKR_LOG_HISTORY_CAPACITY);
    console->write_index =
        (console->write_index + snapshot.count) % VKR_LOG_HISTORY_CAPACITY;
    console->filter_dirty = true_v;
  }
  console->pending_count =
      snapshot.newest_sequence > console->after_sequence
          ? snapshot.newest_sequence - console->after_sequence
          : 0u;
}

static void console_filter(VkrEditorConsole *console) {
  if (!console->filter_dirty)
    return;
  console->filtered_count = 0u;
  MemZero(console->level_counts, sizeof(console->level_counts));
  const uint32_t oldest =
      (console->write_index + VKR_LOG_HISTORY_CAPACITY - console->count) %
      VKR_LOG_HISTORY_CAPACITY;
  for (uint32_t i = 0u; i < console->count; ++i) {
    const uint32_t index = (oldest + i) % VKR_LOG_HISTORY_CAPACITY;
    const VkrLogRecord *record = &console->records[index];
    console->level_counts[record->level]++;
    if (!console->levels[record->level])
      continue;
    if (!console_contains(record->message, record->message_length,
                          console->search, console->search_length) &&
        !console_contains(record->source, record->source_length,
                          console->search, console->search_length))
      continue;
    console->filtered[console->filtered_count++] = index;
  }
  console->filter_dirty = false_v;
}

static VkrUiWidgetConfig console_widget(uint32_t column, uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
  widget.placement.column = column;
  widget.placement.row = row;
  widget.style.font_size_pt = theme->font_body;
  widget.style.padding_pt = (VkrUiEdges){2, 6, 2, 6};
  widget.style.text_color = theme->text;
  widget.style.background_color = (Vec4){0};
  return widget;
}

static bool8_t console_shift(VkrUiSystem *ui) {
  return input_is_key_down(ui->input, KEY_SHIFT) ||
         input_is_key_down(ui->input, KEY_LSHIFT) ||
         input_is_key_down(ui->input, KEY_RSHIFT);
}

static bool8_t console_selected(const VkrEditorConsole *console,
                                uint64_t sequence) {
  return console->selected_sequence &&
         sequence >=
             Min(console->selected_sequence, console->selection_anchor) &&
         sequence <= Max(console->selected_sequence, console->selection_anchor);
}

static uint32_t console_format_record(const VkrLogRecord *record,
                                      uint8_t *buffer, uint32_t capacity) {
  const uint32_t ms = record->utc_time_ms;
  const int length = snprintf(
      (char *)buffer, capacity, "%02u:%02u:%02u.%03u UTC [%s] %.*s:%u\n%.*s%s",
      ms / 3600000u, ms / 60000u % 60u, ms / 1000u % 60u, ms % 1000u,
      s_level_names[record->level], record->source_length, record->source,
      record->line, record->message_length, record->message,
      record->truncated ? "\n[record truncated]" : "");
  return length > 0 ? Min((uint32_t)length, capacity - 1u) : 0u;
}

void vkr_editor_console_copy_selection(VkrEditorConsole *console,
                                       VkrUiSystem *ui) {
  console_filter(console);
  uint32_t selected = 0u;
  for (uint32_t i = 0u; i < console->filtered_count; ++i)
    selected += console_selected(
        console, console->records[console->filtered[i]].sequence);
  if (!selected)
    return;
  const uint32_t per_record =
      VKR_LOG_MESSAGE_CAPACITY + VKR_LOG_SOURCE_CAPACITY + 96u;
  const uint32_t capacity = selected * per_record + 1u;
  uint8_t *copy = vkr_allocator_alloc(ui->frame_allocator, capacity,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!copy)
    return;
  uint32_t length = 0u;
  for (uint32_t i = 0u; i < console->filtered_count; ++i) {
    const VkrLogRecord *record = &console->records[console->filtered[i]];
    if (!console_selected(console, record->sequence))
      continue;
    length += console_format_record(record, copy + length, capacity - length);
    copy[length++] = '\n';
  }
  (void)vkr_platform_clipboard_write_text(copy, length);
}

void vkr_editor_console_clear(VkrEditorConsole *console) {
  const VkrLogHistorySnapshot latest = log_history_snapshot(0u, NULL, 0u);
  console->after_sequence = latest.newest_sequence;
  console->count = 0u;
  console->write_index = 0u;
  console->first_row = 0u;
  console->selected_sequence = 0u;
  console->selection_anchor = 0u;
  console->detail_sequence = 0u;
  console->detail_length = 0u;
  console->missed_count = 0u;
  console->evicted_count = 0u;
  console->filter_dirty = true_v;
}

/* Search, severity chips with counts, and view actions. Narrow docks wrap
 * the chips and actions onto a second row. */
static void console_build_toolbar(VkrEditorConsole *console, VkrUiSystem *ui,
                                  bool8_t narrow, bool8_t tiny) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack wide_columns[] = {
      {.value = 1, .unit = VKR_UI_TRACK_FR},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.value = 10, .unit = VKR_UI_TRACK_PX},
      {.value = 26, .unit = VKR_UI_TRACK_PX},
      {.value = 26, .unit = VKR_UI_TRACK_PX},
      {.value = 26, .unit = VKR_UI_TRACK_PX},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const VkrUiTrack toolbar_rows[2] = {{.value = 26, .unit = VKR_UI_TRACK_PX},
                                      {.value = 26, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig toolbar = vkr_ui_panel_config_default();
  toolbar.placement.column = 0u;
  toolbar.placement.row = 0u;
  toolbar.columns = wide_columns;
  toolbar.column_count = ArrayCount(wide_columns);
  toolbar.rows = toolbar_rows;
  toolbar.row_count = narrow ? 2u : 1u;
  toolbar.style.gap_pt = theme->space_sm;
  toolbar.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("tools"), &toolbar))
    return;
  VkrUiTextEditBuffer edit = {console->search, console->search_length,
                              sizeof(console->search)};
  VkrUiPlacement search = VKR_UI_PLACEMENT_DEFAULT;
  search.column = 0u;
  search.row = 0u;
  search.column_span = narrow ? ArrayCount(wide_columns) : 1u;
  if (vkr_editor_search_field(
          ui, string8_lit("search"), &edit, search,
          string8_lit("Search messages and sources"),
          string8_lit("Search message and source (ASCII case-insensitive)"),
          VKR_FONT_HANDLE_INVALID)) {
    console->search_length = edit.length;
    console->filter_dirty = true_v;
    console->first_row = 0u;
    console->follow_tail = false_v;
  }
  const uint32_t row = narrow ? 1u : 0u;
  for (uint32_t i = 0u; i < ArrayCount(s_chips); ++i) {
    const ConsoleSeverityChip *chip = &s_chips[i];
    uint32_t count = 0u;
    for (uint32_t level = chip->first_level; level <= chip->last_level; ++level)
      count += console->level_counts[level];
    const bool8_t shown = console->levels[chip->first_level];
    VkrUiWidgetConfig button = console_widget(1u + i, row);
    button.placement.justify = VKR_UI_ALIGN_START;
    button.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&button);
    button.style.min_size_pt.y = 22.0f;
    button.style.padding_pt = (VkrUiEdges){2, 7, 2, 6};
    button.icon = chip->icon;
    button.icon_size_pt = 13.0f;
    button.icon_color = console_level_icon_color(chip->first_level);
    button.style.text_color = shown ? theme->text : theme->text_disabled;
    if (shown) {
      button.style.background_color = theme->raised;
      button.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      button.style.border_color = theme->border;
    } else {
      button.icon_color.w = 0.4f;
    }
    button.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s %s. Debug and Trace require Verbose capture.",
        shown ? "Hide" : "Show", chip->label);
    const String8 text =
        tiny ? string8_create_formatted(ui->frame_allocator, "%u", count)
             : string8_create_formatted(ui->frame_allocator, "%s  %u",
                                        chip->label, count);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("chip"), text, &button)) {
      for (uint32_t level = chip->first_level; level <= chip->last_level;
           ++level)
        console->levels[level] = !shown;
      console->filter_dirty = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }
  VkrUiWidgetConfig copy = vkr_editor_icon_button_config(
      6u, row, VKR_UI_ICON_COPY,
      string8_lit("Copy selected matching logs with timestamp and source"));
  if (vkr_ui_button(ui, string8_lit("copy"), (String8){0}, &copy))
    vkr_editor_console_copy_selection(console, ui);
  VkrUiWidgetConfig clear = vkr_editor_icon_button_config(
      7u, row, VKR_UI_ICON_BROOM,
      string8_lit("Clear this Console view; new logs continue to arrive"));
  if (vkr_ui_button(ui, string8_lit("clear"), (String8){0}, &clear))
    vkr_editor_console_clear(console);
  VkrUiWidgetConfig tail = vkr_editor_icon_button_config(
      8u, row, VKR_UI_ICON_FOLLOW_TAIL,
      string8_lit("Follow newest logs; scrolling or selecting pauses"));
  vkr_editor_toggle_style(&tail, console->follow_tail);
  if (vkr_ui_button(ui, string8_lit("tail"), (String8){0}, &tail))
    console->follow_tail = !console->follow_tail;
  VkrUiWidgetConfig capture = console_widget(9u, row);
  capture.placement.align = VKR_UI_ALIGN_CENTER;
  capture.style.text_color = theme->text_secondary;
  capture.tooltip =
      string8_lit("Capture new Debug and Trace records (may be expensive). "
                  "Severity chips only hide captured records.");
  bool8_t verbose = log_max_level_get() >= LOG_LEVEL_DEBUG;
  if (vkr_ui_checkbox(ui, string8_lit("capture.verbose"),
                      tiny ? (String8){0} : string8_lit("Verbose capture"),
                      &verbose, &capture))
    log_max_level_set(verbose ? LOG_LEVEL_TRACE : LOG_LEVEL_INFO);
  (void)vkr_ui_panel_end(ui);
}

static void console_scroll_records(VkrEditorConsole *console, VkrUiSystem *ui,
                                   VkrUiRect content_rect_px,
                                   float32_t tools_height,
                                   float32_t list_height,
                                   uint32_t visible_rows) {
  const uint32_t max_first = console->filtered_count > visible_rows
                                 ? console->filtered_count - visible_rows
                                 : 0u;
  const VkrUiRect list_rect = {
      content_rect_px.x, content_rect_px.y + tools_height * ui->content_scale,
      content_rect_px.width, list_height * ui->content_scale};
  const bool8_t over_list = !ui->mouse_captured &&
                            ui->input_layer == ui->mouse_input_layer &&
                            ui->mouse_x >= list_rect.x &&
                            ui->mouse_x < list_rect.x + list_rect.width &&
                            ui->mouse_y >= list_rect.y &&
                            ui->mouse_y < list_rect.y + list_rect.height;
  if (over_list && ui->mouse_wheel) {
    const int64_t next =
        (int64_t)console->first_row - (int64_t)ui->mouse_wheel * 3;
    console->first_row =
        (uint32_t)Max((int64_t)0, Min((int64_t)max_first, next));
    console->follow_tail = false_v;
    ui->capture.mouse = true_v;
  }
  if (console->follow_tail)
    console->first_row = max_first;
  else
    console->first_row = Min(console->first_row, max_first);
}

/* Returns true when a visible record row holds keyboard focus. */
static bool8_t console_build_records(VkrEditorConsole *console, VkrUiSystem *ui,
                                     uint32_t visible_rows, float32_t available,
                                     VkrFontHandle mono) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack list_row = {.value = VKR_CONSOLE_ROW_HEIGHT,
                               .unit = VKR_UI_TRACK_PX};
  VkrUiTrack list_rows[VKR_CONSOLE_VISIBLE_ROWS];
  for (uint32_t i = 0u; i < visible_rows; ++i)
    list_rows[i] = list_row;
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement.column = 0u;
  list.placement.row = 1u;
  list.rows = list_rows;
  list.row_count = visible_rows;
  list.clip_children = true_v;
  list.style.background_color = theme->field;
  list.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  list.style.border_color = theme->separator;
  list.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  bool8_t list_focused = false_v;
  if (vkr_ui_panel_begin(ui, string8_lit("records"), &list)) {
    const uint32_t end =
        Min(console->first_row + visible_rows, console->filtered_count);
    for (uint32_t row = console->first_row; row < end; ++row) {
      const VkrLogRecord *record = &console->records[console->filtered[row]];
      uint32_t preview_length =
          Min((uint32_t)record->message_length, VKR_CONSOLE_PREVIEW_BYTES);
      while (preview_length < record->message_length && preview_length &&
             (record->message[preview_length] & 0xc0u) == 0x80u)
        --preview_length;
      for (uint32_t i = 0u; i < preview_length; ++i)
        if (record->message[i] == '\n' || record->message[i] == '\r') {
          preview_length = i;
          break;
        }
      const String8 time = string8_create_formatted(
          ui->frame_allocator, "%02u:%02u:%02u.%03u",
          record->utc_time_ms / 3600000u, record->utc_time_ms / 60000u % 60u,
          record->utc_time_ms / 1000u % 60u, record->utc_time_ms % 1000u);
      const String8 message = string8_create_formatted(
          ui->frame_allocator, "%.*s%s", preview_length, record->message,
          preview_length < record->message_length || record->truncated
              ? " \xe2\x80\xa6"
              : "");
      const bool8_t selected = console_selected(console, record->sequence);
      const uint32_t visible_row = row - console->first_row;
      VkrUiWidgetConfig item = console_widget(0u, visible_row);
      item.fill = true_v;
      item.style.corner_radius_pt =
          (Vec4){theme->radius_small, theme->radius_small, theme->radius_small,
                 theme->radius_small};
      if (selected)
        item.style.background_color = theme->selection;
      else if (record->level <= LOG_LEVEL_ERROR)
        item.style.background_color = vkr_ui_color_alpha(theme->error, 0.08f);
      else if (record->level == LOG_LEVEL_WARN)
        item.style.background_color = vkr_ui_color_alpha(theme->warning, 0.06f);
      item.style.hover_background_color =
          selected ? theme->selection : theme->row_hover;
      item.tooltip =
          available < 6 * VKR_CONSOLE_ROW_HEIGHT + 56.0f
              ? string8_lit(
                    "Select log; Shift-click selects range. Copy copies full "
                    "records. Enlarge Console for selectable detail.")
              : string8_lit(
                    "Select log; Shift-click selects range. Copy copies full "
                    "records. Drag in the detail field to select text.");
      // Fixed view slots bound retained text/widget capacity. Selection belongs
      // to sequence IDs; rebinding a slot cancels its old keyboard/mouse
      // action.
      const uint32_t slot = visible_row;
      (void)vkr_ui_push_id_u64(ui, slot);
      const VkrUiId row_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("record"));
      if (console->row_sequences[slot] != record->sequence) {
        if (ui->focused_id == row_id)
          ui->focused_id = VKR_UI_ID_NONE;
        if (ui->active_id == row_id)
          ui->active_id = VKR_UI_ID_NONE;
        console->row_sequences[slot] = record->sequence;
      }
      if (vkr_ui_button(ui, string8_lit("record"), (String8){0}, &item)) {
        if (!console_shift(ui) || !console->selection_anchor)
          console->selection_anchor = record->sequence;
        console->selected_sequence = record->sequence;
        console->follow_tail = false_v;
        if (console->detail_sequence != record->sequence) {
          console->detail_length = console_format_record(
              record, console->detail, sizeof(console->detail));
          console->detail_sequence = record->sequence;
        }
      }
      /* Right click keeps a multi-record selection that already includes
       * the row; otherwise it selects the row, then asks for the menu. */
      if (ui->hot_id == row_id && !ui->mouse_captured &&
          input_button_just_pressed(ui->input, BUTTON_RIGHT)) {
        if (!console_selected(console, record->sequence)) {
          console->selection_anchor = record->sequence;
          console->selected_sequence = record->sequence;
        }
        console->follow_tail = false_v;
        console->context_requested = true_v;
        console->context_position_pt =
            (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                   (float32_t)ui->mouse_y / ui->content_scale};
      }
      list_focused |= ui->focused_id == row_id;
      VkrUiWidgetConfig stamp = console_widget(0u, visible_row);
      stamp.placement.justify = VKR_UI_ALIGN_START;
      stamp.placement.align = VKR_UI_ALIGN_CENTER;
      stamp.style.text_color = theme->text_disabled;
      stamp.style.font_size_pt = theme->font_body;
      stamp.text.font = mono;
      vkr_ui_label(ui, string8_lit("time"), time, &stamp);
      VkrUiWidgetConfig text = stamp;
      text.placement.margin_pt.left = VKR_CONSOLE_MESSAGE_LEFT_PT;
      text.style.text_color = console_level_color(record->level);
      text.icon = s_level_icons[record->level];
      text.icon_size_pt = 13.0f;
      text.icon_color = console_level_icon_color(record->level);
      vkr_ui_label(ui, string8_lit("message"), message, &text);
      (void)vkr_ui_pop_id(ui);
    }
    if (!console->filtered_count) {
      VkrUiWidgetConfig empty = console_widget(0u, 0u);
      empty.placement.row_span = Max(1u, Min(visible_rows, 3u));
      empty.placement.justify = VKR_UI_ALIGN_CENTER;
      empty.placement.align = VKR_UI_ALIGN_CENTER;
      empty.style.text_color = theme->text_disabled;
      vkr_ui_label(ui, string8_lit("empty"),
                   console->count ? string8_lit("No messages match the filters")
                                  : string8_lit("No messages yet"),
                   &empty);
    }
    (void)vkr_ui_panel_end(ui);
  }
  return list_focused;
}

void vkr_editor_console_build(VkrEditorConsole *console, VkrUiSystem *ui,
                              VkrUiRect content_rect_px, VkrFontHandle heading,
                              VkrFontHandle mono) {
  (void)heading;
  if (!console || !console->initialized)
    return;
  console_poll(console);
  console_filter(console);
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t inset = theme->space_md;
  const float32_t width =
      content_rect_px.width / ui->content_scale - inset * 2.0f;
  const float32_t height =
      content_rect_px.height / ui->content_scale - inset * 1.5f;
  if (width < 64.0f || height < 42.0f)
    return;
  const bool8_t narrow = width < 640.0f;
  const bool8_t tiny = width < 260.0f;
  const float32_t toolbar_height = narrow ? 56.0f : 26.0f;
  const float32_t footer_height = 18.0f;
  const float32_t tools_height = toolbar_height + theme->space_sm;
  const float32_t available = Max(0.0f, height - tools_height - footer_height);
  const float32_t detail_height =
      console->detail_sequence &&
              available >= 6 * VKR_CONSOLE_ROW_HEIGHT + 56.0f
          ? Min(112.0f, available - 6 * VKR_CONSOLE_ROW_HEIGHT)
          : 0.0f;
  const float32_t list_height = available - detail_height;
  const uint32_t visible_rows =
      Min(VKR_CONSOLE_VISIBLE_ROWS,
          (uint32_t)(Max(0.0f, list_height - 2.0f) / VKR_CONSOLE_ROW_HEIGHT));
  const VkrUiTrack columns[] = {{.value = 1, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack rows[] = {{.value = tools_height, .unit = VKR_UI_TRACK_PX},
                             {.value = list_height, .unit = VKR_UI_TRACK_PX},
                             {.value = detail_height, .unit = VKR_UI_TRACK_PX},
                             {.value = footer_height, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.columns = columns;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style.padding_pt = (VkrUiEdges){inset, inset, inset * 0.5f, inset};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("console"), &panel))
    return;

  console_build_toolbar(console, ui, narrow, tiny);

  console_filter(console);
  const VkrUiRect list_origin = {content_rect_px.x + inset * ui->content_scale,
                                 content_rect_px.y + inset * ui->content_scale,
                                 content_rect_px.width, content_rect_px.height};
  console_scroll_records(console, ui, list_origin, tools_height, list_height,
                         visible_rows);

  const bool8_t list_focused =
      console_build_records(console, ui, visible_rows, available, mono);
  if (list_focused && !ui->mouse_captured &&
      ui->input_layer == ui->keyboard_input_layer) {
    if (input_key_shortcut_modifier(ui->input, KEY_A) &&
        input_key_just_pressed(ui->input, KEY_A) && console->filtered_count) {
      console->selection_anchor =
          console->records[console->filtered[0]].sequence;
      console->selected_sequence =
          console->records[console->filtered[console->filtered_count - 1u]]
              .sequence;
    }
    if (input_key_shortcut_modifier(ui->input, KEY_C) &&
        input_key_just_pressed(ui->input, KEY_C))
      vkr_editor_console_copy_selection(console, ui);
  }
  if (detail_height > 0.0f) {
    VkrUiWidgetConfig detail = console_widget(0u, 2u);
    detail.placement.margin_pt.top = theme->space_sm;
    detail.style.min_size_pt = (Vec2){width, detail_height - theme->space_sm};
    detail.read_only = true_v;
    detail.text.font = mono;
    detail.text.layout.word_wrap = true_v;
    detail.text.layout.max_width = Max(1.0f, width - 12.0f);
    detail.tooltip =
        string8_lit("Log detail: drag to select; Cmd/Ctrl+C copies text.");
    vkr_editor_field_style(&detail);
    VkrUiTextEditBuffer buffer = {console->detail, console->detail_length,
                                  sizeof(console->detail)};
    (void)vkr_ui_text_field(ui, string8_lit("detail"), &buffer, &detail);
  }
  VkrUiWidgetConfig status = console_widget(0u, 3u);
  status.placement.justify = VKR_UI_ALIGN_START;
  status.placement.align = VKR_UI_ALIGN_END;
  status.style.padding_pt = (VkrUiEdges){0, 2, 0, 2};
  status.style.font_size_pt = theme->font_caption;
  status.style.text_color = theme->text_secondary;
  const bool8_t losses = console->evicted_count || console->missed_count;
  const String8 status_text =
      losses ? string8_create_formatted(
                   ui->frame_allocator,
                   "%u of %u messages  \xc2\xb7  %llu evicted  \xc2\xb7  "
                   "%llu missed  \xc2\xb7  %llu pending%s",
                   console->filtered_count, console->count,
                   (unsigned long long)console->evicted_count,
                   (unsigned long long)console->missed_count,
                   (unsigned long long)console->pending_count,
                   console->follow_tail ? "  \xc2\xb7  following" : "")
             : string8_create_formatted(
                   ui->frame_allocator, "%u of %u messages%s",
                   console->filtered_count, console->count,
                   console->follow_tail ? "  \xc2\xb7  following" : "");
  vkr_ui_label(ui, string8_lit("summary"), status_text, &status);

  (void)vkr_ui_panel_end(ui);
}
