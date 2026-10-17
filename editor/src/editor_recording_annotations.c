#include "editor_recording.h"

#include "core/vkr_json.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

/* Annotations in the editor (docs/proposals/recordings.md, "Bundle"): the
   cache of one recording's annotations.json, the playback bar's note field,
   range marking and review toggle, the notes and ranges on its track, and
   the agent prompt a recording's card copies. */

/* Notes and ranges the agent prompt lists; the rest are counted. */
#define EDITOR_ANNOTATIONS_PROMPT_ITEMS 10u
#define EDITOR_ANNOTATIONS_PROMPT_BYTES 4096u
/* A note's square on the track and a range's band height, ring included. */
#define EDITOR_ANNOTATIONS_NOTE_PT 10.0f
#define EDITOR_ANNOTATIONS_RANGE_PT 14.0f
/* The note field over the playback bar. */
#define EDITOR_ANNOTATIONS_COMPOSER_PT 440.0f
#define EDITOR_ANNOTATIONS_COMPOSER_HEIGHT_PT 44.0f
/* The selected note's panel: a caption and up to three lines of text. */
#define EDITOR_ANNOTATIONS_NOTE_PANEL_HEIGHT_PT 86.0f

/* ------------------------------------------------------------------------ */
/* Cache                                                                     */

VkrRecordingAnnotations *
vkr_editor_recording_annotations(VkrEditorUi *editor, const char *bundle_path,
                                 char *error, uint64_t error_capacity) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  if (string_equals(notes->bundle, bundle_path)) {
    return &notes->annotations;
  }
  notes->bundle[0] = '\0';
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  if (!vkr_recording_annotations_read(bundle_path, &notes->annotations)) {
    string_format(error, error_capacity,
                  "The recording's %s is damaged; fix or delete it first.",
                  VKR_RECORDING_ANNOTATIONS_FILE);
    return NULL;
  }
  string_format(notes->bundle, sizeof(notes->bundle), "%s", bundle_path);
  return &notes->annotations;
}

bool8_t vkr_editor_recording_annotations_save(VkrEditorUi *editor) {
  VkrEditorRecording *recording = &editor->recording;
  VkrEditorRecordingNotes *notes = &recording->notes;
  if (!notes->bundle[0] ||
      !vkr_recording_annotations_write(notes->bundle, &notes->annotations)) {
    return false_v;
  }
  /* Indices move when notes or ranges come and go. */
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  /* The card of the same bundle shows the new counts at once. */
  for (uint32_t i = 0u; i < recording->entry_count; ++i) {
    VkrEditorRecordingEntry *entry = &recording->entries[i];
    char path[VKR_RECORDING_PATH_MAX];
    string_format(path, sizeof(path), "%s/%s", recording->directory,
                  entry->bundle);
    if (string_equals(path, notes->bundle)) {
      entry->notes = notes->annotations.note_count;
      entry->ranges = notes->annotations.range_count;
      entry->review = notes->annotations.review;
    }
  }
  return true_v;
}

void vkr_editor_recording_entry_annotations(VkrEditorUi *editor,
                                            const char *bundle_path,
                                            VkrEditorRecordingEntry *entry) {
  VkrRecordingAnnotations *read = &editor->recording.list_annotations;
  if (!vkr_recording_annotations_read(bundle_path, read)) {
    return;
  }
  entry->notes = read->note_count;
  entry->ranges = read->range_count;
  entry->review = read->review;
}

uint64_t vkr_editor_recording_frame_count(const VkrSampleUiFrame *frame,
                                          const char *bundle_path) {
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (playback && string_equals(playback->bundle_path, bundle_path)) {
    return playback->frame_count;
  }
  char path[VKR_RECORDING_PATH_MAX];
  if (string_format(path, sizeof(path), "%s/%s", bundle_path,
                    VKR_RECORDING_SUMMARY_FILE) >= (int32_t)sizeof(path)) {
    return 0u;
  }
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return 0u;
  }
  VkrJsonReader reader = vkr_json_reader_create(mapping.data, mapping.size);
  float64_t frames = 0.0;
  if (!vkr_json_find_root_field(&reader, "frames") ||
      !vkr_json_parse_double(&reader, &frames) || frames < 0.0) {
    frames = 0.0;
  }
  file_unmap(&mapping);
  return (uint64_t)frames;
}

/* ------------------------------------------------------------------------ */
/* Playback bar tools                                                        */

vkr_internal uint64_t
editor_annotations_playhead(const VkrRecordingPlayback *playback) {
  return vkr_recording_playback_frame_at(playback, playback->time);
}

/* The open playback's annotations, or NULL after a toast saying why. */
vkr_internal VkrRecordingAnnotations *
editor_annotations_open(VkrEditorUi *editor,
                        const VkrRecordingPlayback *playback) {
  char error[192];
  VkrRecordingAnnotations *annotations = vkr_editor_recording_annotations(
      editor, playback->bundle_path, error, sizeof(error));
  if (!annotations) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     error);
  }
  return annotations;
}

vkr_internal void editor_annotations_saved(VkrEditorUi *editor, VkrUiIcon icon,
                                           const char *text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (vkr_editor_recording_annotations_save(editor)) {
    vkr_editor_toast(editor, icon, theme->text, text);
  } else {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "The recording's annotations could not be written.");
  }
}

void vkr_editor_recording_note_toggle(VkrEditorUi *editor) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  notes->composing = !notes->composing;
  notes->focus = notes->composing;
  notes->text_length = 0u;
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
}

void vkr_editor_recording_range_mark(VkrEditorUi *editor,
                                     const VkrRecordingPlayback *playback) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const uint64_t current = editor_annotations_playhead(playback);
  if (!notes->marking) {
    notes->marking = true_v;
    notes->range_first = current;
    return;
  }
  notes->marking = false_v;
  VkrRecordingAnnotations *annotations =
      editor_annotations_open(editor, playback);
  if (!annotations) {
    return;
  }
  char name[VKR_RECORDING_RANGE_NAME_MAX];
  string_format(name, sizeof(name), "Range %u", annotations->range_count + 1u);
  if (!vkr_recording_annotations_add_range(
          annotations, notes->range_first, current, "user",
          string8_create_from_cstr((const uint8_t *)name,
                                   string_length(name)))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "This recording has as many ranges as it can hold.");
    return;
  }
  char text[96];
  string_format(text, sizeof(text), "Added %s: frames %llu to %llu.", name,
                (unsigned long long)Min(notes->range_first, current),
                (unsigned long long)Max(notes->range_first, current));
  editor_annotations_saved(editor, VKR_UI_ICON_RULER, text);
}

void vkr_editor_recording_review_toggle(VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback,
                                        bool8_t flag) {
  VkrRecordingAnnotations *annotations =
      editor_annotations_open(editor, playback);
  if (!annotations) {
    return;
  }
  /* A click marks it reviewed or takes that back; a flag click flags it. */
  const VkrRecordingReview review =
      flag ? VKR_RECORDING_REVIEW_FLAGGED
      : annotations->review == VKR_RECORDING_REVIEW_REVIEWED
          ? VKR_RECORDING_REVIEW_UNREVIEWED
          : VKR_RECORDING_REVIEW_REVIEWED;
  vkr_recording_annotations_set_review(annotations, review, "user");
  editor_annotations_saved(
      editor,
      review == VKR_RECORDING_REVIEW_FLAGGED ? VKR_UI_ICON_WARNING_FILL
                                             : VKR_UI_ICON_CHECK_CIRCLE,
      review == VKR_RECORDING_REVIEW_FLAGGED ? "Flagged the recording."
      : review == VKR_RECORDING_REVIEW_REVIEWED
          ? "Marked the recording reviewed."
          : "Marked the recording unreviewed.");
}

/* ------------------------------------------------------------------------ */
/* Track                                                                     */

/* Selects a note or range, or deselects it when it was selected; the note
   field closes. A range's name fills the rename field. */
vkr_internal void editor_annotations_select(VkrEditorRecordingNotes *notes,
                                            VkrEditorRecordingSelection kind,
                                            uint32_t index, const char *name) {
  if (notes->selected == kind && notes->selected_index == index) {
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
    return;
  }
  notes->selected = kind;
  notes->selected_index = index;
  notes->composing = false_v;
  notes->name_length = 0u;
  if (name) {
    notes->name_length = Min(string_length(name), sizeof(notes->name) - 1u);
    MemCopy(notes->name, name, notes->name_length);
  }
}

/* The track's x, in points, of frame `frame`. */
vkr_internal float32_t
editor_annotations_x(const VkrRecordingPlayback *playback, VkrUiRect slider_px,
                     float32_t scale, uint64_t frame) {
  const uint64_t last = playback->frame_count - 1u;
  const float64_t time = playback->times[Min(frame, last)];
  const float32_t fraction =
      (float32_t)Clamp(time / Max(playback->duration, 0.001), 0.0, 1.0);
  return (slider_px.x + slider_px.width * fraction) / scale;
}

/* A band along the track from `x0` to `x1`, in points. */
vkr_internal VkrUiWidgetConfig editor_annotations_band(VkrUiRect slider_px,
                                                       float32_t scale,
                                                       float32_t x0,
                                                       float32_t x1,
                                                       Vec4 color) {
  const float32_t height = EDITOR_ANNOTATIONS_RANGE_PT;
  VkrUiWidgetConfig band = vkr_ui_widget_config_default();
  band.placement = VKR_UI_PLACEMENT_DEFAULT;
  band.placement.column = band.placement.row = 0u;
  band.placement.justify = band.placement.align = VKR_UI_ALIGN_START;
  band.placement.margin_pt = (VkrUiEdges){
      (slider_px.y + slider_px.height * 0.5f) / scale - height * 0.5f, 0.0f,
      0.0f, x0};
  band.style.min_size_pt = band.style.max_size_pt =
      (Vec2){Max(x1 - x0, height), height};
  /* A tinted box around the track segment; the track shows through, and
     the edge keeps it apart from the played part's colour. */
  band.style.background_color = vkr_ui_color_alpha(color, 0.22f);
  band.style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  band.style.border_color = vkr_ui_color_alpha(color, 0.9f);
  band.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  return band;
}

void vkr_editor_recording_annotations_track(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, VkrUiRect slider_px) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  if (playback->frame_count == 0u || slider_px.width <= 0.0f) {
    return;
  }
  char error[192];
  const VkrRecordingAnnotations *annotations = vkr_editor_recording_annotations(
      editor, playback->bundle_path, error, sizeof(error));
  if (notes->marking) {
    const uint64_t current = editor_annotations_playhead(playback);
    const float32_t a =
        editor_annotations_x(playback, slider_px, scale, notes->range_first);
    const float32_t b =
        editor_annotations_x(playback, slider_px, scale, current);
    VkrUiWidgetConfig pending = editor_annotations_band(
        slider_px, scale, Min(a, b), Max(a, b), theme->warning);
    vkr_ui_label(ui, string8_lit("editor.recording.range.pending"),
                 (String8){0}, &pending);
  }
  if (!annotations) {
    return;
  }
  for (uint32_t i = 0u; i < annotations->range_count; ++i) {
    const VkrRecordingRange *range = &annotations->ranges[i];
    VkrUiWidgetConfig band = editor_annotations_band(
        slider_px, scale,
        editor_annotations_x(playback, slider_px, scale, range->first),
        editor_annotations_x(playback, slider_px, scale, range->last),
        theme->text);
    band.style.hover_background_color = vkr_ui_color_alpha(theme->text, 0.4f);
    band.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s: frames %llu to %llu%s%s", range->name,
        (unsigned long long)range->first, (unsigned long long)range->last,
        range->author[0] ? ", by " : "", range->author);
    (void)vkr_ui_push_id_u64(ui, i);
    if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_RANGE &&
        notes->selected_index == i) {
      band.style.background_color = vkr_ui_color_alpha(theme->text, 0.4f);
    }
    if (vkr_ui_button(ui, string8_lit("editor.recording.range"), (String8){0},
                      &band)) {
      VkrSampleRecordingRequest seek = {
          .command = VKR_SAMPLE_RECORDING_SEEK_FRAME, .frame = range->first};
      (void)vkr_editor_recording_send(editor, seek, false_v);
      editor_annotations_select(notes, VKR_EDITOR_RECORDING_SELECTION_RANGE, i,
                                range->name);
    }
    (void)vkr_ui_pop_id(ui);
  }
  const float32_t size = EDITOR_ANNOTATIONS_NOTE_PT;
  for (uint32_t i = 0u; i < annotations->note_count; ++i) {
    const VkrRecordingNote *note = &annotations->notes[i];
    VkrUiWidgetConfig square = vkr_ui_widget_config_default();
    square.placement = VKR_UI_PLACEMENT_DEFAULT;
    square.placement.column = square.placement.row = 0u;
    square.placement.justify = square.placement.align = VKR_UI_ALIGN_START;
    /* Above the track, so a note and a marker on one frame both show. */
    square.placement.margin_pt = (VkrUiEdges){
        slider_px.y / scale + 1.0f, 0.0f, 0.0f,
        editor_annotations_x(playback, slider_px, scale, note->frame) -
            size * 0.5f};
    square.style.min_size_pt = square.style.max_size_pt = (Vec2){size, size};
    square.style.background_color = theme->text;
    square.style.hover_background_color = theme->accent_hover;
    square.style.border_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
    square.style.border_color = theme->popup;
    square.style.corner_radius_pt = (Vec4){2.0f, 2.0f, 2.0f, 2.0f};
    square.tooltip = string8_create_formatted(
        ui->frame_allocator, "Note at frame %llu%s%s:\n%s",
        (unsigned long long)note->frame, note->author[0] ? " by " : "",
        note->author, note->text);
    (void)vkr_ui_push_id_u64(ui, i);
    if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE &&
        notes->selected_index == i) {
      square.style.background_color = theme->accent_hover;
    }
    if (vkr_ui_button(ui, string8_lit("editor.recording.note"), (String8){0},
                      &square)) {
      VkrSampleRecordingRequest seek = {
          .command = VKR_SAMPLE_RECORDING_SEEK_FRAME, .frame = note->frame};
      (void)vkr_editor_recording_send(editor, seek, false_v);
      editor_annotations_select(notes, VKR_EDITOR_RECORDING_SELECTION_NOTE, i,
                                NULL);
    }
    (void)vkr_ui_pop_id(ui);
  }
}

/* ------------------------------------------------------------------------ */
/* Note field                                                                */

vkr_internal VkrUiPlacement editor_annotations_cell(uint32_t column) {
  return (VkrUiPlacement){.column = column,
                          .row = 0u,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* Adds the field's text as a note at the playhead and closes the field. */
vkr_internal void
editor_annotations_add_note(VkrEditorUi *editor,
                            const VkrRecordingPlayback *playback) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const uint64_t current = editor_annotations_playhead(playback);
  VkrRecordingAnnotations *annotations =
      editor_annotations_open(editor, playback);
  if (!annotations) {
    return;
  }
  const String8 text = {.str = (uint8_t *)notes->text,
                        .length = notes->text_length};
  if (!vkr_recording_annotations_add_note(annotations, current, "user", text)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "This recording has as many notes as it can hold.");
    return;
  }
  notes->composing = false_v;
  notes->text_length = 0u;
  char message[64];
  string_format(message, sizeof(message), "Added a note at frame %llu.",
                (unsigned long long)current);
  editor_annotations_saved(editor, VKR_UI_ICON_PENCIL_LINE, message);
}

/* Whether the selection still names a note or range of the cache. */
vkr_internal bool8_t
editor_annotations_selection_valid(const VkrEditorRecordingNotes *notes) {
  return (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE &&
          notes->selected_index < notes->annotations.note_count) ||
         (notes->selected == VKR_EDITOR_RECORDING_SELECTION_RANGE &&
          notes->selected_index < notes->annotations.range_count);
}

bool8_t vkr_editor_recording_composer_rect(const VkrEditorUi *editor,
                                           Vec3 bar_pt, Vec4 *out_pt) {
  const VkrEditorRecordingNotes *notes = &editor->recording.notes;
  float32_t height = 0.0f;
  if (notes->composing) {
    height = EDITOR_ANNOTATIONS_COMPOSER_HEIGHT_PT;
  } else if (editor_annotations_selection_valid(notes)) {
    height = notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE
                 ? EDITOR_ANNOTATIONS_NOTE_PANEL_HEIGHT_PT
                 : EDITOR_ANNOTATIONS_COMPOSER_HEIGHT_PT;
  }
  if (height <= 0.0f) {
    return false_v;
  }
  /* Above the bar, from its left edge, so it reads as the bar's own. */
  *out_pt = (Vec4){bar_pt.x, bar_pt.y - height - 6.0f,
                   Min(EDITOR_ANNOTATIONS_COMPOSER_PT, bar_pt.z), height};
  return true_v;
}

/* The glass panel over the bar at `rect`, in points. */
vkr_internal bool8_t editor_annotations_panel_begin(
    VkrUiSystem *ui, String8 id, Vec4 rect, const VkrUiTrack *columns,
    uint32_t column_count, const VkrUiTrack *rows, uint32_t row_count) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0.0f, 0.0f, rect.x};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  panel.style.gap_pt = vkr_ui_theme()->space_md;
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){rect.z, rect.w};
  panel.columns = columns;
  panel.column_count = column_count;
  panel.rows = rows;
  panel.row_count = row_count;
  return vkr_ui_panel_begin(ui, id, &panel);
}

/* A Delete button in the error colour, sized as the bar's text buttons. */
vkr_internal VkrUiWidgetConfig editor_annotations_delete_button(
    VkrEditorUi *editor, uint32_t column, uint32_t row, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig remove = vkr_ui_widget_config_default();
  remove.placement = editor_annotations_cell(column);
  remove.placement.row = row;
  vkr_editor_ghost_style(&remove);
  remove.text.font = editor->heading_font;
  remove.icon = VKR_UI_ICON_TRASH;
  vkr_editor_button_size(&remove);
  remove.style.min_size_pt.y = remove.style.max_size_pt.y = 26.0f;
  remove.style.text_color = theme->error;
  remove.icon_color = theme->error;
  remove.tooltip = tooltip;
  return remove;
}

/* The note field: the playhead's frame, the text, Add note and Close. */
vkr_internal void
editor_annotations_composer(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            const VkrRecordingPlayback *playback, Vec4 rect) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.unit = VKR_UI_TRACK_AUTO},
                                {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO},
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  if (!editor_annotations_panel_begin(
          ui, string8_lit("editor.recording.composer"), rect, columns,
          ArrayCount(columns), &row, 1u)) {
    return;
  }
  const uint64_t current = editor_annotations_playhead(playback);
  VkrUiWidgetConfig where =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  where.placement = editor_annotations_cell(0u);
  where.icon = VKR_UI_ICON_PENCIL_LINE;
  where.icon_size_pt = 12.0f;
  vkr_ui_label(ui, string8_lit("where"),
               string8_create_formatted(ui->frame_allocator, "Frame %llu",
                                        (unsigned long long)current),
               &where);

  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = editor_annotations_cell(1u);
  vkr_editor_field_style(&field);
  field.style.min_size_pt.y = field.style.max_size_pt.y = 26.0f;
  field.tooltip = string8_lit("The note; Enter adds it, Escape closes");
  /* The field takes the keyboard when it opens; the request holds until
     the field owns it, as the Script editor's name field does. */
  const VkrUiId field_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("text"));
  if (notes->focus) {
    ui->focused_id = field_id;
    ui->focused_is_text = true_v;
    (void)vkr_ui_keyboard_layer_set(ui, ui->input_layer);
  }
  VkrUiTextEditBuffer buffer = {.data = (uint8_t *)notes->text,
                                .length = notes->text_length,
                                .capacity = sizeof(notes->text) - 1u};
  (void)vkr_ui_text_field(ui, string8_lit("text"), &buffer, &field);
  notes->text_length = buffer.length;
  const bool8_t focused = ui->focused_id == field_id;
  if (notes->focus && ui->focused_id == field_id &&
      ui->keyboard_input_layer == ui->input_layer &&
      vkr_ui_widget_rect(ui, field_id, &(VkrUiRect){0})) {
    notes->focus = false_v;
  }
  const bool8_t enter = focused && input_key_just_pressed(ui->input, KEY_ENTER);
  const bool8_t escape =
      focused && input_key_just_pressed(ui->input, KEY_ESCAPE);

  VkrUiWidgetConfig add = vkr_ui_widget_config_default();
  add.placement = editor_annotations_cell(2u);
  vkr_editor_primary_style(&add, editor->heading_font);
  add.icon = VKR_UI_ICON_ADD;
  vkr_editor_button_size(&add);
  add.style.min_size_pt.y = add.style.max_size_pt.y = 26.0f;
  add.disabled = notes->text_length == 0u;
  add.tooltip = string8_lit("Add the note at the playhead (Enter)");
  const bool8_t clicked =
      vkr_ui_button(ui, string8_lit("add"), string8_lit("Add note"), &add);
  VkrUiWidgetConfig close = vkr_editor_icon_button_config(
      3u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close (Escape)"));
  if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close) || escape) {
    notes->composing = false_v;
  } else if ((clicked || enter) && notes->text_length > 0u) {
    editor_annotations_add_note(editor, playback);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The selected note: who wrote it at which frame, its text, Delete and
   Close. */
vkr_internal void
editor_annotations_note_panel(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              const VkrRecordingPlayback *playback, Vec4 rect) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingNote *note =
      &notes->annotations.notes[notes->selected_index];
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO},
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack rows[] = {{.value = 26.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  if (!editor_annotations_panel_begin(ui, string8_lit("editor.recording.note"),
                                      rect, columns, ArrayCount(columns), rows,
                                      ArrayCount(rows))) {
    return;
  }
  VkrUiWidgetConfig caption =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  caption.placement = editor_annotations_cell(0u);
  caption.placement.justify = VKR_UI_ALIGN_START;
  caption.icon = VKR_UI_ICON_PENCIL_LINE;
  caption.icon_size_pt = 12.0f;
  vkr_ui_label(ui, string8_lit("caption"),
               string8_create_formatted(
                   ui->frame_allocator, "Note at frame %llu%s%s",
                   (unsigned long long)note->frame,
                   note->author[0] ? "  \xc2\xb7  " : "", note->author),
               &caption);
  VkrUiWidgetConfig remove = editor_annotations_delete_button(
      editor, 1u, 0u, string8_lit("Delete this note"));
  const bool8_t deleting =
      vkr_ui_button(ui, string8_lit("delete"), string8_lit("Delete"), &remove);
  VkrUiWidgetConfig close = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close (Escape)"));
  const bool8_t closing =
      vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close) ||
      (!ui->focused_is_text && input_key_just_pressed(ui->input, KEY_ESCAPE));
  VkrUiWidgetConfig text =
      vkr_editor_text_config(theme->font_body, theme->text);
  text.placement = editor_annotations_cell(0u);
  text.placement.row = 1u;
  text.placement.column_span = 3u;
  text.placement.justify = VKR_UI_ALIGN_START;
  text.placement.align = VKR_UI_ALIGN_START;
  text.text.layout.word_wrap = true_v;
  text.text.layout.max_width = rect.z - 20.0f;
  vkr_ui_label(ui, string8_lit("text"),
               string8_create_from_cstr((const uint8_t *)note->text,
                                        string_length(note->text)),
               &text);
  (void)vkr_ui_panel_end(ui);
  if (deleting) {
    VkrRecordingAnnotations *annotations =
        editor_annotations_open(editor, playback);
    if (annotations) {
      vkr_recording_annotations_remove_note(annotations, notes->selected_index);
      editor_annotations_saved(editor, VKR_UI_ICON_TRASH, "Deleted the note.");
    }
  } else if (closing) {
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  }
}

/* The selected range: its name to edit, its frames, Delete and Close. */
vkr_internal void editor_annotations_range_panel(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, Vec4 rect) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t index = notes->selected_index;
  const VkrRecordingRange *range = &notes->annotations.ranges[index];
  const VkrUiTrack columns[] = {{.unit = VKR_UI_TRACK_AUTO},
                                {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO},
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  if (!editor_annotations_panel_begin(ui, string8_lit("editor.recording.range"),
                                      rect, columns, ArrayCount(columns), &row,
                                      1u)) {
    return;
  }
  VkrUiWidgetConfig frames =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  frames.placement = editor_annotations_cell(0u);
  frames.icon = VKR_UI_ICON_RULER;
  frames.icon_size_pt = 12.0f;
  vkr_ui_label(ui, string8_lit("frames"),
               string8_create_formatted(ui->frame_allocator,
                                        "%llu\xe2\x80\x93%llu",
                                        (unsigned long long)range->first,
                                        (unsigned long long)range->last),
               &frames);
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = editor_annotations_cell(1u);
  vkr_editor_field_style(&field);
  field.style.min_size_pt.y = field.style.max_size_pt.y = 26.0f;
  field.tooltip = string8_lit("The range's name; Enter renames it");
  const VkrUiId field_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("name"));
  VkrUiTextEditBuffer buffer = {.data = (uint8_t *)notes->name,
                                .length = notes->name_length,
                                .capacity = sizeof(notes->name) - 1u};
  (void)vkr_ui_text_field(ui, string8_lit("name"), &buffer, &field);
  notes->name_length = buffer.length;
  const bool8_t focused = ui->focused_id == field_id;
  const bool8_t enter = focused && input_key_just_pressed(ui->input, KEY_ENTER);
  const bool8_t escape = input_key_just_pressed(ui->input, KEY_ESCAPE) &&
                         (focused || !ui->focused_is_text);
  VkrUiWidgetConfig remove = editor_annotations_delete_button(
      editor, 2u, 0u, string8_lit("Delete this range"));
  const bool8_t deleting =
      vkr_ui_button(ui, string8_lit("delete"), string8_lit("Delete"), &remove);
  VkrUiWidgetConfig close = vkr_editor_icon_button_config(
      3u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close (Escape)"));
  const bool8_t closing =
      vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close) || escape;
  (void)vkr_ui_panel_end(ui);

  if (deleting || enter) {
    VkrRecordingAnnotations *annotations =
        editor_annotations_open(editor, playback);
    if (!annotations) {
      return;
    }
    if (deleting) {
      vkr_recording_annotations_remove_range(annotations, index);
      editor_annotations_saved(editor, VKR_UI_ICON_TRASH, "Deleted the range.");
      return;
    }
    const String8 name = {.str = (uint8_t *)notes->name,
                          .length = notes->name_length};
    if (!vkr_recording_annotations_rename_range(annotations, index, name)) {
      return;
    }
    editor_annotations_saved(editor, VKR_UI_ICON_RULER, "Renamed the range.");
    /* A rename moves no index; the range stays selected. */
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_RANGE;
    notes->selected_index = index;
  } else if (closing) {
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  }
}

void vkr_editor_recording_composer_build(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         Vec3 bar_pt) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  /* The panel describes the open playback's cache only. */
  if (!string_equals(notes->bundle, playback->bundle_path)) {
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  }
  Vec4 rect;
  if (!vkr_editor_recording_composer_rect(editor, bar_pt, &rect)) {
    return;
  }
  if (notes->composing) {
    editor_annotations_composer(editor, frame, playback, rect);
  } else if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE) {
    editor_annotations_note_panel(editor, frame, playback, rect);
  } else {
    editor_annotations_range_panel(editor, frame, playback, rect);
  }
}

/* ------------------------------------------------------------------------ */
/* Agent prompt                                                              */

typedef struct EditorPrompt {
  char text[EDITOR_ANNOTATIONS_PROMPT_BYTES];
  uint64_t length;
} EditorPrompt;

/* Appends formatted text; a full prompt keeps what fit. */
vkr_internal void editor_prompt_add(EditorPrompt *prompt, const char *format,
                                    ...) {
  if (prompt->length + 1u >= sizeof(prompt->text)) {
    return;
  }
  va_list args;
  va_start(args, format);
  const int32_t wrote =
      string_format_v(prompt->text + prompt->length,
                      sizeof(prompt->text) - prompt->length, format, args);
  va_end(args);
  if (wrote > 0) {
    prompt->length =
        Min(prompt->length + (uint64_t)wrote, sizeof(prompt->text) - 1u);
  }
}

/* The summary's markers, as JSON-escaped labels: frame, kind and label. */
vkr_internal void editor_prompt_markers(EditorPrompt *prompt,
                                        const char *bundle_path) {
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s", bundle_path,
                VKR_RECORDING_SUMMARY_FILE);
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return;
  }
  VkrJsonReader reader = vkr_json_reader_create(mapping.data, mapping.size);
  uint32_t count = 0u;
  if (vkr_json_find_root_field(&reader, "markers") &&
      reader.pos < reader.length && reader.data[reader.pos] == '[') {
    ++reader.pos;
    String8 object = {0};
    while (vkr_json_next_array_element(&reader) &&
           vkr_json_capture_composite(&reader, &object)) {
      if (count++ >= EDITOR_ANNOTATIONS_PROMPT_ITEMS) {
        continue;
      }
      VkrJsonReader fields = vkr_json_reader_from_string(object);
      float64_t marker_frame = 0.0;
      String8 kind = {0};
      String8 label = {0};
      (void)(vkr_json_find_root_field(&fields, "frame") &&
             vkr_json_parse_double(&fields, &marker_frame));
      fields.pos = 0u;
      (void)(vkr_json_find_root_field(&fields, "kind") &&
             vkr_json_parse_string(&fields, &kind));
      fields.pos = 0u;
      (void)(vkr_json_find_root_field(&fields, "label") &&
             vkr_json_parse_string(&fields, &label));
      editor_prompt_add(prompt, "%s- frame %.0f, %.*s: \"%.*s\"\n",
                        count == 1u ? "Markers:\n" : "", marker_frame,
                        (int32_t)kind.length, (const char *)kind.str,
                        (int32_t)label.length, (const char *)label.str);
    }
  }
  if (count > EDITOR_ANNOTATIONS_PROMPT_ITEMS) {
    editor_prompt_add(prompt, "- and %u more in summary.json\n",
                      count - EDITOR_ANNOTATIONS_PROMPT_ITEMS);
  }
  file_unmap(&mapping);
}

vkr_internal void editor_prompt_annotations(EditorPrompt *prompt,
                                            const VkrRecordingAnnotations *a) {
  if (a->review != VKR_RECORDING_REVIEW_UNREVIEWED) {
    editor_prompt_add(prompt, "Review: %s%s%s.\n",
                      vkr_recording_review_name(a->review),
                      a->review_by[0] ? " by " : "", a->review_by);
  }
  for (uint32_t i = 0u; i < Min(a->note_count, EDITOR_ANNOTATIONS_PROMPT_ITEMS);
       ++i) {
    const VkrRecordingNote *note = &a->notes[i];
    editor_prompt_add(prompt, "%s- frame %llu, %s: \"%s\"\n",
                      i == 0u ? "Notes:\n" : "",
                      (unsigned long long)note->frame,
                      note->author[0] ? note->author : "someone", note->text);
  }
  if (a->note_count > EDITOR_ANNOTATIONS_PROMPT_ITEMS) {
    editor_prompt_add(prompt, "- and %u more notes\n",
                      a->note_count - EDITOR_ANNOTATIONS_PROMPT_ITEMS);
  }
  for (uint32_t i = 0u;
       i < Min(a->range_count, EDITOR_ANNOTATIONS_PROMPT_ITEMS); ++i) {
    const VkrRecordingRange *range = &a->ranges[i];
    editor_prompt_add(prompt, "%s- \"%s\": frames %llu to %llu\n",
                      i == 0u ? "Ranges:\n" : "", range->name,
                      (unsigned long long)range->first,
                      (unsigned long long)range->last);
  }
  if (a->range_count > EDITOR_ANNOTATIONS_PROMPT_ITEMS) {
    editor_prompt_add(prompt, "- and %u more ranges\n",
                      a->range_count - EDITOR_ANNOTATIONS_PROMPT_ITEMS);
  }
}

void vkr_editor_recording_copy_prompt(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const VkrEditorRecordingEntry *entry) {
  VkrEditorRecording *recording = &editor->recording;
  EditorPrompt prompt = {0};
  char bundle[VKR_RECORDING_PATH_MAX];
  string_format(bundle, sizeof(bundle), "%s/%s", recording->directory,
                entry->bundle);
  editor_prompt_add(&prompt,
                    "Look at the VKR recording \"%s\" at %s.\n"
                    "It is %.1f s and %llu frames of %s, recorded by %s.\n",
                    entry->name, bundle, entry->duration,
                    (unsigned long long)entry->frames,
                    entry->scene[0] ? entry->scene : "an unknown scene",
                    entry->author[0] ? entry->author : "someone");
  /* A request's recording carries its id as its name. */
  VkrEditorRecordingRequest request;
  if (vkr_editor_recording_request_read(editor, entry->name, &request)) {
    editor_prompt_add(&prompt, "It answers %s's request: \"%s\"\n",
                      request.owner[0] ? request.owner : "an agent",
                      request.question);
  }
  editor_prompt_markers(&prompt, bundle);
  char error[192];
  const VkrRecordingAnnotations *annotations =
      vkr_editor_recording_annotations(editor, bundle, error, sizeof(error));
  if (annotations) {
    editor_prompt_annotations(&prompt, annotations);
  }
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (playback && string_equals(playback->bundle_path, bundle)) {
    editor_prompt_add(&prompt, "The playhead is at frame %llu (%.2f s).\n",
                      (unsigned long long)editor_annotations_playhead(playback),
                      playback->time);
  }
  editor_prompt_add(
      &prompt,
      "Read %s in the bundle first. In the editor, rec.open {\"name\": "
      "\"%s\"} opens it, rec.seek takes a frame, a time or a marker, and "
      "view.capture shows the frame; rec.annotations reads the notes and "
      "ranges, and rec.note and rec.range add yours. Offline, vkr_rec "
      "summary, spikes, frames and passes read the bundle.\n",
      VKR_RECORDING_SUMMARY_FILE, entry->bundle);
  (void)vkr_platform_clipboard_write_text((const uint8_t *)prompt.text,
                                          (uint32_t)prompt.length);
}
