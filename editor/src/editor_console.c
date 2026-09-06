#include "editor_console.h"
#include "editor_internal.h"

#include <math.h>
#include <stdio.h>

#define VKR_CONSOLE_POLL_CAPACITY 256u
#define VKR_CONSOLE_VISIBLE_ROWS 48u
#define VKR_CONSOLE_PREVIEW_BYTES 160u
#define VKR_CONSOLE_ROW_HEIGHT 18.0f

static const char *s_level_names[6] = {"Fatal", "Error", "Warn",
                                       "Info",  "Debug", "Trace"};
static const Vec4 s_level_colors[6] = {
    {1.0f, 0.24f, 0.30f, 1.0f}, {1.0f, 0.43f, 0.40f, 1.0f},
    {1.0f, 0.72f, 0.18f, 1.0f}, {0.36f, 0.78f, 1.0f, 1.0f},
    {0.69f, 0.63f, 1.0f, 1.0f}, {0.48f, 0.86f, 0.72f, 1.0f}};
static const VkrUiIcon s_level_icons[6] = {
    VKR_UI_ICON_LOG_FATAL, VKR_UI_ICON_LOG_ERROR, VKR_UI_ICON_LOG_WARNING,
    VKR_UI_ICON_LOG_INFO,  VKR_UI_ICON_LOG_DEBUG, VKR_UI_ICON_LOG_TRACE};

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
  const uint32_t oldest =
      (console->write_index + VKR_LOG_HISTORY_CAPACITY - console->count) %
      VKR_LOG_HISTORY_CAPACITY;
  for (uint32_t i = 0u; i < console->count; ++i) {
    const uint32_t index = (oldest + i) % VKR_LOG_HISTORY_CAPACITY;
    const VkrLogRecord *record = &console->records[index];
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
  VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
  widget.placement.column = column;
  widget.placement.row = row;
  widget.style.font_size_pt = 11.0f;
  widget.style.padding_pt = (VkrUiEdges){2, 5, 2, 5};
  widget.style.text_color = (Vec4){0.84f, 0.87f, 0.90f, 1};
  widget.style.background_color = (Vec4){0.11f, 0.13f, 0.16f, 1};
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

static void console_copy(VkrEditorConsole *console, VkrUiSystem *ui) {
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

static bool8_t console_in_rect(const VkrUiSystem *ui, VkrUiRect rect) {
  return ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
         ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height;
}

static void console_filters_popup(VkrEditorConsole *console, VkrUiSystem *ui,
                                  float32_t width, float32_t height,
                                  float32_t left, float32_t top,
                                  bool8_t just_opened) {
  const float32_t row_height = 24.0f;
  const VkrUiTrack column = {.value = 1, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack rows[6];
  for (uint32_t i = 0u; i < ArrayCount(rows); ++i)
    rows[i] = (VkrUiTrack){.value = row_height, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 4u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {.top = top, .left = left},
  };
  popup.columns = &column;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = ArrayCount(rows);
  popup.style.background_color = (Vec4){0.09f, 0.11f, 0.14f, 1};
  popup.style.border_color = (Vec4){0.32f, 0.41f, 0.52f, 1};
  popup.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  popup.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
  popup.style.min_size_pt = (Vec2){width, height};
  popup.style.max_size_pt = popup.style.min_size_pt;
  popup.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("filters.popup"), &popup))
    return;
  if (!just_opened && console->filter_focus_pending) {
    (void)vkr_ui_scroll_area_offset_set(
        ui, Max(0.0f, (console->filter_focus_index + 1u) * row_height -
                          (height - 8.0f)));
  }
  const uint32_t focus_index = console->filter_focus_index;
  for (uint32_t i = 0u; i < ArrayCount(console->levels); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("level"));
    if (!just_opened && console->filter_focus_pending && i == focus_index)
      ui->focused_id = id;
    VkrUiWidgetConfig item = console_widget(0u, i);
    item.style.text_color = s_level_colors[i];
    item.style.background_color = (Vec4){0};
    item.tooltip = string8_lit(
        "Show or hide this severity. Debug and Trace require Verbose capture.");
    if (vkr_ui_checkbox(
            ui, string8_lit("level"),
            string8_create_from_cstr((const uint8_t *)s_level_names[i],
                                     string_length(s_level_names[i])),
            &console->levels[i], &item))
      console->filter_dirty = true_v;
    if (ui->focused_id == id && !console->filter_focus_pending)
      console->filter_focus_index = i;
    (void)vkr_ui_pop_id(ui);
  }
  if (!just_opened)
    console->filter_focus_pending = false_v;
  (void)vkr_ui_scroll_area_end(ui);
}

void vkr_editor_console_build(VkrEditorConsole *console, VkrUiSystem *ui,
                              VkrUiRect content_rect_px,
                              VkrFontHandle heading) {
  if (!console || !console->initialized)
    return;
  console_poll(console);
  console_filter(console);
  const float32_t width = content_rect_px.width / ui->content_scale;
  const float32_t height = content_rect_px.height / ui->content_scale;
  if (width < 64.0f || height < 42.0f)
    return;
  const bool8_t narrow = width < 600.0f;
  const bool8_t tiny = width < 180.0f;
  const float32_t toolbar_height = narrow ? 48.0f : 24.0f;
  const float32_t footer_height = 16.0f;
  const float32_t tools_height = toolbar_height;
  const float32_t filter_left = narrow ? 0.0f : Max(0.0f, width - 356.0f);
  const float32_t filter_width =
      narrow ? Max(0.0f, width - 8.0f) * 1.8f / 6.6f : 104.0f;
  const float32_t filter_top = narrow ? 26.0f : 0.0f;
  const float32_t popup_width = Min(176.0f, width);
  const float32_t popup_height = Min(154.0f, Max(24.0f, height - tools_height));
  const float32_t popup_left = Min(filter_left, Max(0.0f, width - popup_width));
  const float32_t popup_top =
      Min(tools_height, Max(0.0f, height - popup_height));
  const VkrUiRect popup_rect = {
      content_rect_px.x + popup_left * ui->content_scale,
      content_rect_px.y + popup_top * ui->content_scale,
      popup_width * ui->content_scale, popup_height * ui->content_scale};
  const VkrUiRect filter_button_rect = {
      content_rect_px.x + filter_left * ui->content_scale,
      content_rect_px.y + filter_top * ui->content_scale,
      filter_width * ui->content_scale,
      (narrow ? 22.0f : 24.0f) * ui->content_scale};
  const bool8_t own_keyboard =
      !ui->mouse_captured && ui->input_layer == ui->keyboard_input_layer;
  if (console->filters_open) {
    const bool8_t dismiss_key =
        own_keyboard && (input_key_just_pressed(ui->input, KEY_ESCAPE) ||
                         input_key_just_pressed(ui->input, KEY_TAB));
    if (dismiss_key || ui->keyboard_input_layer != ui->input_layer ||
        (ui->mouse_pressed && !console_in_rect(ui, popup_rect) &&
         !console_in_rect(ui, filter_button_rect))) {
      console->filters_open = false_v;
      console->filter_focus_pending = false_v;
      if (dismiss_key)
        ui->focused_id = console->filters_button_id;
    } else if (own_keyboard && (input_key_just_pressed(ui->input, KEY_UP) ||
                                input_key_just_pressed(ui->input, KEY_DOWN))) {
      console->filter_focus_index =
          (console->filter_focus_index +
           (input_key_just_pressed(ui->input, KEY_UP) ? 5u : 1u)) %
          6u;
      console->filter_focus_pending = true_v;
      ui->capture.keyboard = true_v;
    }
  }
  const bool8_t mouse_captured = ui->mouse_captured;
  const bool8_t over_popup =
      console->filters_open && console_in_rect(ui, popup_rect);
  bool8_t filters_just_opened = false_v;

  const float32_t available = Max(0.0f, height - tools_height - footer_height);
  const float32_t detail_height =
      console->detail_sequence &&
              available >= 6 * VKR_CONSOLE_ROW_HEIGHT + 56.0f
          ? Min(112.0f, available - 6 * VKR_CONSOLE_ROW_HEIGHT)
          : 0.0f;
  const float32_t list_height = available - detail_height;
  const uint32_t visible_rows =
      Min(VKR_CONSOLE_VISIBLE_ROWS,
          (uint32_t)(list_height / VKR_CONSOLE_ROW_HEIGHT));
  const VkrUiTrack columns[] = {{.value = 1, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack rows[] = {{.value = toolbar_height, .unit = VKR_UI_TRACK_PX},
                             {.value = list_height, .unit = VKR_UI_TRACK_PX},
                             {.value = detail_height, .unit = VKR_UI_TRACK_PX},
                             {.value = footer_height, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.columns = columns;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("console"), &panel))
    return;

  /* The popup paints last. Its bounds must not activate covered log rows. */
  ui->mouse_captured = mouse_captured || over_popup;

  const VkrUiTrack toolbar_columns[] = {
      {.value = 1, .unit = VKR_UI_TRACK_FR},
      {.value = 104, .unit = VKR_UI_TRACK_PX},
      {.value = 44, .unit = VKR_UI_TRACK_PX},
      {.value = 44, .unit = VKR_UI_TRACK_PX},
      {.value = 44, .unit = VKR_UI_TRACK_PX},
      {.value = 112, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig toolbar = vkr_ui_panel_config_default();
  toolbar.placement.column = 0u;
  toolbar.placement.row = 0u;
  const VkrUiTrack narrow_columns[5] = {
      {.value = 1.8f, .unit = VKR_UI_TRACK_FR},
      {.value = 1, .unit = VKR_UI_TRACK_FR},
      {.value = 1, .unit = VKR_UI_TRACK_FR},
      {.value = 1, .unit = VKR_UI_TRACK_FR},
      {.value = 1.8f, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack toolbar_rows[2] = {{.value = 24, .unit = VKR_UI_TRACK_PX},
                                      {.value = 22, .unit = VKR_UI_TRACK_PX}};
  toolbar.columns = narrow ? narrow_columns : toolbar_columns;
  toolbar.column_count = narrow ? 5u : ArrayCount(toolbar_columns);
  toolbar.rows = toolbar_rows;
  toolbar.row_count = narrow ? 2u : 1u;
  toolbar.style.gap_pt = 2.0f;
  toolbar.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("tools"), &toolbar)) {
    VkrUiWidgetConfig search = console_widget(0u, 0u);
    vkr_editor_field_style(&search);
    search.placement.column_span = narrow ? 5u : 1u;
    search.tooltip =
        string8_lit("Search message and source (ASCII case-insensitive)");
    VkrUiTextEditBuffer edit = {console->search, console->search_length,
                                sizeof(console->search)};
    if (vkr_ui_text_field(ui, string8_lit("search"), &edit, &search)) {
      console->search_length = edit.length;
      console->filter_dirty = true_v;
      console->first_row = 0u;
      console->follow_tail = false_v;
    }
    const VkrUiId search_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("search"));
    if (console->search_length == 0u && ui->focused_id != search_id) {
      VkrUiWidgetConfig placeholder = search;
      placeholder.style.background_color = (Vec4){0};
      placeholder.style.border_color = (Vec4){0};
      placeholder.style.text_color = (Vec4){0.56f, 0.62f, 0.68f, 1};
      vkr_ui_label(ui, string8_lit("search.placeholder"),
                   string8_lit("Search logs"), &placeholder);
    }
    VkrUiWidgetConfig filter =
        console_widget(narrow ? 0u : 1u, narrow ? 1u : 0u);
    vkr_editor_action_style(&filter, heading);
    filter.tooltip = string8_lit(
        "Filter severities; arrows navigate, Space toggles, Escape closes");
    console->filters_button_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("filters"));
    uint32_t enabled_levels = 0u;
    for (uint32_t i = 0u; i < ArrayCount(console->levels); ++i)
      enabled_levels += console->levels[i];
    const String8 filter_label = string8_create_formatted(
        ui->frame_allocator, width < 260.0f ? "F %u/6 %s" : "Filters %u/6 %s",
        enabled_levels, console->filters_open ? "^" : "v");
    if (vkr_ui_button(ui, string8_lit("filters"), filter_label, &filter)) {
      console->filters_open = !console->filters_open;
      filters_just_opened = console->filters_open;
      console->filter_focus_pending = console->filters_open;
    }
    VkrUiWidgetConfig button =
        console_widget(narrow ? 1u : 2u, narrow ? 1u : 0u);
    vkr_editor_action_style(&button, heading);
    button.tooltip =
        string8_lit("Copy selected matching logs with timestamp and source");
    if (vkr_ui_button(ui, string8_lit("copy"),
                      tiny ? string8_lit("C") : string8_lit("Copy"), &button))
      console_copy(console, ui);
    button.placement.column = narrow ? 2u : 3u;
    button.tooltip =
        string8_lit("Clear this Console view; new logs continue to arrive");
    if (vkr_ui_button(ui, string8_lit("clear"),
                      tiny ? string8_lit("X") : string8_lit("Clear"),
                      &button)) {
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
    button.placement.column = narrow ? 3u : 4u;
    button.tooltip = string8_lit(
        "Follow newest logs; scrolling or selecting pauses following");
    button.style.background_color = console->follow_tail
                                        ? (Vec4){0.18f, 0.31f, 0.23f, 1}
                                        : button.style.background_color;
    if (vkr_ui_button(ui, string8_lit("tail"),
                      tiny ? string8_lit("T") : string8_lit("Tail"), &button))
      console->follow_tail = !console->follow_tail;
    VkrUiWidgetConfig capture =
        console_widget(narrow ? 4u : 5u, narrow ? 1u : 0u);
    capture.tooltip =
        string8_lit("Capture new Debug and Trace records (may be expensive). "
                    "Severity filters only hide captured records.");
    bool8_t verbose = log_max_level_get() >= LOG_LEVEL_DEBUG;
    if (vkr_ui_checkbox(ui, string8_lit("capture.verbose"),
                        tiny     ? string8_lit("V")
                        : narrow ? string8_lit("Verbose")
                                 : string8_lit("Verbose capture"),
                        &verbose, &capture))
      log_max_level_set(verbose ? LOG_LEVEL_TRACE : LOG_LEVEL_INFO);
    (void)vkr_ui_panel_end(ui);
  }

  console_filter(console);
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
      const String8 text = string8_create_formatted(
          ui->frame_allocator, "%02u:%02u:%02u.%03u  %-5s  %.*s%s",
          record->utc_time_ms / 3600000u, record->utc_time_ms / 60000u % 60u,
          record->utc_time_ms / 1000u % 60u, record->utc_time_ms % 1000u,
          s_level_names[record->level], preview_length, record->message,
          preview_length < record->message_length || record->truncated ? " ..."
                                                                       : "");
      VkrUiWidgetConfig item = console_widget(0u, row - console->first_row);
      item.style.text_color = s_level_colors[record->level];
      item.style.background_color = console_selected(console, record->sequence)
                                        ? (Vec4){0.20f, 0.30f, 0.40f, 1}
                                        : (Vec4){0.055f, 0.064f, 0.078f, 1};
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
      const uint32_t slot = row - console->first_row;
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
      list_focused |= ui->focused_id == row_id;
      item.style.background_color = (Vec4){0};
      item.tooltip = (String8){0};
      item.icon = s_level_icons[record->level];
      item.icon_size_pt = 12.0f;
      vkr_ui_label(ui, string8_lit("message"), text, &item);
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
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
      console_copy(console, ui);
  }
  if (detail_height > 0.0f) {
    VkrUiWidgetConfig detail = console_widget(0u, 2u);
    detail.read_only = true_v;
    detail.text.layout.word_wrap = true_v;
    detail.text.layout.max_width = Max(1.0f, width - 12.0f);
    detail.tooltip =
        string8_lit("Log detail: drag to select; Cmd/Ctrl+C copies text.");
    VkrUiTextEditBuffer buffer = {console->detail, console->detail_length,
                                  sizeof(console->detail)};
    (void)vkr_ui_text_field(ui, string8_lit("detail"), &buffer, &detail);
  }
  VkrUiWidgetConfig status = console_widget(0u, 3u);
  status.style.background_color = (Vec4){0};
  status.placement.justify = VKR_UI_ALIGN_START;
  const String8 status_text = string8_create_formatted(
      ui->frame_allocator,
      "%u / %u logs | %llu evicted | %llu missed | %llu pending%s",
      console->filtered_count, console->count,
      (unsigned long long)console->evicted_count,
      (unsigned long long)console->missed_count,
      (unsigned long long)console->pending_count,
      console->follow_tail ? " | following" : "");
  vkr_ui_label(ui, string8_lit("summary"), status_text, &status);
  ui->mouse_captured = mouse_captured;
  if (console->filters_open) {
    if (over_popup)
      ui->capture.mouse = true_v;
    console_filters_popup(console, ui, popup_width, popup_height, popup_left,
                          popup_top, filters_just_opened);
  }

  (void)vkr_ui_panel_end(ui);
}
