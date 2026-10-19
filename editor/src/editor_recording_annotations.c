#include "editor_recording.h"

#include "core/vkr_json.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

/* Annotations in the editor (ADR-122): the cache of one recording's
   annotations.json, the playback bar's note field, segment marking, the
   Segments list, a segment's panel with its title and note, focus on one
   segment, the review toggle, the notes and segments on the bar's track,
   and the agent prompt a recording's card copies. */

/* Notes and segments the agent prompt lists; the rest are counted. */
#define EDITOR_ANNOTATIONS_PROMPT_ITEMS 10u
#define EDITOR_ANNOTATIONS_PROMPT_BYTES 16384u
/* A note's square on the track and a segment's band height, ring
   included. */
#define EDITOR_ANNOTATIONS_NOTE_PT 10.0f
#define EDITOR_ANNOTATIONS_SEGMENT_PT 14.0f
/* The segment panel and the Segments list: wider than the note field, the
   panel with a title row, a note of about five lines and a footer. */
#define EDITOR_ANNOTATIONS_WIDE_PT 640.0f
#define EDITOR_ANNOTATIONS_SEGMENT_PANEL_HEIGHT_PT 196.0f
#define EDITOR_ANNOTATIONS_LIST_ROWS 6u
/* A list entry: its title line over its note line, and the gap between
   grid rows. */
#define EDITOR_ANNOTATIONS_LIST_TITLE_PT 20.0f
#define EDITOR_ANNOTATIONS_LIST_NOTE_PT 16.0f
#define EDITOR_ANNOTATIONS_LIST_GAP_PT 4.0f
#define EDITOR_ANNOTATIONS_LIST_HEADER_PT 26.0f
/* The segment panel's padding and gap between its rows. */
#define EDITOR_ANNOTATIONS_PANEL_PAD_PT 8.0f
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
  /* Indices move when notes or segments come and go. */
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
  /* The card of the same bundle shows the new counts at once. */
  for (uint32_t i = 0u; i < recording->entry_count; ++i) {
    VkrEditorRecordingEntry *entry = &recording->entries[i];
    char path[VKR_RECORDING_PATH_MAX];
    string_format(path, sizeof(path), "%s/%s", recording->directory,
                  entry->bundle);
    if (string_equals(path, notes->bundle)) {
      entry->notes = notes->annotations.note_count;
      entry->segments = notes->annotations.segment_count;
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
  entry->segments = read->segment_count;
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

/* Loads segment `index`'s title and note into the panel's fields. */
vkr_internal void
editor_annotations_segment_load(VkrEditorRecordingNotes *notes,
                                uint32_t index) {
  const VkrRecordingSegment *segment = &notes->annotations.segments[index];
  notes->title_length =
      Min(string_length(segment->name), sizeof(notes->title) - 1u);
  MemCopy(notes->title, segment->name, notes->title_length);
  notes->note_length =
      Min(string_length(segment->note), sizeof(notes->note) - 1u);
  MemCopy(notes->note, segment->note, notes->note_length);
}

/* Whether the panel's fields differ from the selected segment's title and
   note. */
vkr_internal bool8_t
editor_annotations_segment_dirty(const VkrEditorRecordingNotes *notes) {
  if (notes->selected != VKR_EDITOR_RECORDING_SELECTION_SEGMENT ||
      notes->selected_index >= notes->annotations.segment_count) {
    return false_v;
  }
  const VkrRecordingSegment *segment =
      &notes->annotations.segments[notes->selected_index];
  return notes->title_length != string_length(segment->name) ||
         MemCompare(notes->title, segment->name, notes->title_length) != 0 ||
         notes->note_length != string_length(segment->note) ||
         MemCompare(notes->note, segment->note, notes->note_length) != 0;
}

/* Writes the panel's title and note to the selected segment when they
   changed; the segment stays selected. False when the title is empty,
   which keeps the fields as typed. */
vkr_internal bool8_t editor_annotations_segment_commit(VkrEditorUi *editor) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  if (!editor_annotations_segment_dirty(notes)) {
    return true_v;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t index = notes->selected_index;
  const String8 title = {.str = (uint8_t *)notes->title,
                         .length = notes->title_length};
  const String8 note = {.str = (uint8_t *)notes->note,
                        .length = notes->note_length};
  if (!vkr_recording_annotations_edit_segment(&notes->annotations, index,
                                              &title, &note)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "A segment needs a title.");
    return false_v;
  }
  if (!vkr_editor_recording_annotations_save(editor)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "The recording's annotations could not be written.");
    return false_v;
  }
  /* An edit moves no index, so the segment stays open. */
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_SEGMENT;
  notes->selected_index = index;
  vkr_editor_toast(editor, VKR_UI_ICON_SAVE, theme->text, "Saved the segment.");
  return true_v;
}

void vkr_editor_recording_segment_select(VkrEditorUi *editor, uint32_t index) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  if (!editor_annotations_segment_commit(editor) ||
      index >= notes->annotations.segment_count) {
    return;
  }
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_SEGMENT;
  notes->selected_index = index;
  notes->composing = false_v;
  notes->listing = false_v;
  notes->edit_focus = false_v;
  editor_annotations_segment_load(notes, index);
}

bool8_t vkr_editor_recording_segment_add(VkrEditorUi *editor,
                                         const VkrRecordingPlayback *playback,
                                         uint64_t first, uint64_t last) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!editor_annotations_segment_commit(editor)) {
    return false_v;
  }
  VkrRecordingAnnotations *annotations =
      editor_annotations_open(editor, playback);
  if (!annotations) {
    return false_v;
  }
  char title[VKR_RECORDING_SEGMENT_NAME_MAX];
  string_format(title, sizeof(title), "Segment %u",
                annotations->segment_count + 1u);
  const uint32_t index = vkr_recording_annotations_add_segment(
      annotations, first, last, "user",
      string8_create_from_cstr((const uint8_t *)title, string_length(title)),
      (String8){0});
  if (index == UINT32_MAX) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "This recording has as many segments as it can hold.");
    return false_v;
  }
  if (!vkr_editor_recording_annotations_save(editor)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "The recording's annotations could not be written.");
    return false_v;
  }
  /* The new segment opens with its title taking the keyboard. */
  vkr_editor_recording_segment_select(editor, index);
  notes->edit_focus = true_v;
  char text[128];
  string_format(text, sizeof(text),
                "Marked frames %llu to %llu; name the segment and say why it "
                "matters.",
                (unsigned long long)Min(first, last),
                (unsigned long long)Max(first, last));
  vkr_editor_toast(editor, VKR_UI_ICON_RULER, theme->text, text);
  return true_v;
}

void vkr_editor_recording_segment_mark(VkrEditorUi *editor,
                                       const VkrRecordingPlayback *playback) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const uint64_t current = editor_annotations_playhead(playback);
  if (!notes->marking) {
    notes->marking = true_v;
    notes->segment_first = current;
    return;
  }
  notes->marking = false_v;
  (void)vkr_editor_recording_segment_add(editor, playback, notes->segment_first,
                                         current);
}

void vkr_editor_recording_segments_toggle(VkrEditorUi *editor) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  if (!editor_annotations_segment_commit(editor)) {
    return;
  }
  notes->listing = !notes->listing;
  notes->composing = false_v;
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
}

/* Views segment `index` alone: the timeline opens on its frames, playback
   loops them from its first frame, and the bar's track shows it in the
   accent colour. */
vkr_internal void editor_annotations_focus(VkrEditorUi *editor,
                                           const VkrRecordingPlayback *playback,
                                           uint32_t index) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const VkrRecordingSegment *segment = &notes->annotations.segments[index];
  const uint64_t last_frame = playback->frame_count - 1u;
  notes->focusing = true_v;
  notes->focus_first = Min(segment->first, last_frame);
  notes->focus_last = Min(segment->last, last_frame);
  vkr_editor_recording_timeline_show(editor, playback, notes->focus_first,
                                     notes->focus_last);
  vkr_editor_recording_settings(editor, playback->speed, true_v,
                                playback->times[notes->focus_first],
                                playback->times[notes->focus_last]);
  VkrSampleRecordingRequest seek = {.command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
                                    .frame = notes->focus_first};
  (void)vkr_editor_recording_send(editor, seek, false_v);
  VkrSampleRecordingRequest play = {.command = VKR_SAMPLE_RECORDING_PLAY};
  (void)vkr_editor_recording_send(editor, play, false_v);
}

/* Ends the focus: the whole recording shows and playback stops looping. */
vkr_internal void
editor_annotations_unfocus(VkrEditorUi *editor,
                           const VkrRecordingPlayback *playback) {
  editor->recording.notes.focusing = false_v;
  vkr_editor_recording_settings(editor, playback->speed, false_v, 0.0, 0.0);
  vkr_editor_recording_timeline_show(editor, playback, UINT64_MAX, 0u);
}

/* Whether the focus is on segment `index`. */
vkr_internal bool8_t editor_annotations_focused(
    const VkrEditorRecordingNotes *notes, const VkrRecordingPlayback *playback,
    uint32_t index) {
  if (!notes->focusing || index >= notes->annotations.segment_count) {
    return false_v;
  }
  const VkrRecordingSegment *segment = &notes->annotations.segments[index];
  const uint64_t last_frame = playback->frame_count - 1u;
  return Min(segment->first, last_frame) == notes->focus_first &&
         Min(segment->last, last_frame) == notes->focus_last;
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

/* Selects a note, or deselects it when it was selected; the note field
   closes. */
vkr_internal void editor_annotations_select_note(VkrEditorUi *editor,
                                                 uint32_t index) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  if (!editor_annotations_segment_commit(editor)) {
    return;
  }
  if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE &&
      notes->selected_index == index) {
    notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
    return;
  }
  notes->selected = VKR_EDITOR_RECORDING_SELECTION_NOTE;
  notes->selected_index = index;
  notes->composing = false_v;
  notes->listing = false_v;
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
  const float32_t height = EDITOR_ANNOTATIONS_SEGMENT_PT;
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
        editor_annotations_x(playback, slider_px, scale, notes->segment_first);
    const float32_t b =
        editor_annotations_x(playback, slider_px, scale, current);
    VkrUiWidgetConfig pending = editor_annotations_band(
        slider_px, scale, Min(a, b), Max(a, b), theme->warning);
    vkr_ui_label(ui, string8_lit("editor.recording.segment.pending"),
                 (String8){0}, &pending);
  }
  if (!annotations) {
    return;
  }
  for (uint32_t i = 0u; i < annotations->segment_count; ++i) {
    const VkrRecordingSegment *segment = &annotations->segments[i];
    /* The focused segment in the accent colour, the others in the text's. */
    const bool8_t focused = editor_annotations_focused(notes, playback, i);
    const Vec4 color = focused ? theme->accent : theme->text;
    VkrUiWidgetConfig band = editor_annotations_band(
        slider_px, scale,
        editor_annotations_x(playback, slider_px, scale, segment->first),
        editor_annotations_x(playback, slider_px, scale, segment->last), color);
    band.style.hover_background_color = vkr_ui_color_alpha(color, 0.4f);
    band.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s: frames %llu to %llu%s%s%s%s", segment->name,
        (unsigned long long)segment->first, (unsigned long long)segment->last,
        segment->author[0] ? ", by " : "", segment->author,
        segment->note[0] ? "\n" : "", segment->note);
    (void)vkr_ui_push_id_u64(ui, i);
    if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_SEGMENT &&
        notes->selected_index == i) {
      band.style.background_color = vkr_ui_color_alpha(color, 0.4f);
    }
    if (vkr_ui_button(ui, string8_lit("editor.recording.segment"), (String8){0},
                      &band)) {
      VkrSampleRecordingRequest seek = {
          .command = VKR_SAMPLE_RECORDING_SEEK_FRAME, .frame = segment->first};
      (void)vkr_editor_recording_send(editor, seek, false_v);
      if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_SEGMENT &&
          notes->selected_index == i) {
        if (editor_annotations_segment_commit(editor)) {
          notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
        }
      } else {
        vkr_editor_recording_segment_select(editor, i);
      }
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
      editor_annotations_select_note(editor, i);
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

/* Whether the selection still names a note or segment of the cache. */
vkr_internal bool8_t
editor_annotations_selection_valid(const VkrEditorRecordingNotes *notes) {
  return (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE &&
          notes->selected_index < notes->annotations.note_count) ||
         (notes->selected == VKR_EDITOR_RECORDING_SELECTION_SEGMENT &&
          notes->selected_index < notes->annotations.segment_count);
}

/* Rows the Segments list shows on its page; at least one for its hint. */
vkr_internal uint32_t
editor_annotations_list_rows(const VkrEditorRecordingNotes *notes) {
  const uint32_t count = notes->annotations.segment_count;
  const uint32_t first = notes->list_page * EDITOR_ANNOTATIONS_LIST_ROWS;
  return count > first ? Min(count - first, EDITOR_ANNOTATIONS_LIST_ROWS) : 1u;
}

bool8_t vkr_editor_recording_composer_rect(const VkrEditorUi *editor,
                                           Vec3 bar_pt, Vec4 *out_pt) {
  const VkrEditorRecordingNotes *notes = &editor->recording.notes;
  float32_t height = 0.0f;
  float32_t width = EDITOR_ANNOTATIONS_COMPOSER_PT;
  if (notes->composing) {
    height = EDITOR_ANNOTATIONS_COMPOSER_HEIGHT_PT;
  } else if (notes->listing) {
    /* The header and two grid rows per entry inside the panel's padding,
       a gap before each grid row after the header. */
    const uint32_t rows = editor_annotations_list_rows(notes);
    height = 2.0f * EDITOR_ANNOTATIONS_PANEL_PAD_PT +
             EDITOR_ANNOTATIONS_LIST_HEADER_PT +
             (float32_t)rows * (EDITOR_ANNOTATIONS_LIST_TITLE_PT +
                                EDITOR_ANNOTATIONS_LIST_NOTE_PT +
                                2.0f * EDITOR_ANNOTATIONS_LIST_GAP_PT);
    width = EDITOR_ANNOTATIONS_WIDE_PT;
  } else if (editor_annotations_selection_valid(notes)) {
    if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE) {
      height = EDITOR_ANNOTATIONS_NOTE_PANEL_HEIGHT_PT;
    } else {
      height = EDITOR_ANNOTATIONS_SEGMENT_PANEL_HEIGHT_PT;
      width = EDITOR_ANNOTATIONS_WIDE_PT;
    }
  }
  if (height <= 0.0f) {
    return false_v;
  }
  /* Above the bar, from its left edge, so it reads as the bar's own. */
  *out_pt =
      (Vec4){bar_pt.x, bar_pt.y - height - 6.0f, Min(width, bar_pt.z), height};
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

/* A text button of the panels, sized as the bar's text buttons. */
vkr_internal VkrUiWidgetConfig
editor_annotations_text_button(VkrEditorUi *editor, uint32_t column,
                               uint32_t row, VkrUiIcon icon, String8 tooltip) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = editor_annotations_cell(column);
  button.placement.row = row;
  vkr_editor_action_style(&button, editor->heading_font);
  button.icon = icon;
  vkr_editor_button_size(&button);
  button.style.min_size_pt.y = button.style.max_size_pt.y = 26.0f;
  button.tooltip = tooltip;
  return button;
}

/* Writes a perf re-run of frames `first` to `last` and copies its command. */
vkr_internal void editor_annotations_rerun(VkrEditorUi *editor,
                                           const VkrRecordingPlayback *playback,
                                           uint64_t first, uint64_t last) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorRecordingRerun written;
  char error[192];
  if (!vkr_editor_recording_rerun(editor, playback->bundle_path, first, last,
                                  &written, error, sizeof(error))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning, error);
    return;
  }
  (void)vkr_platform_clipboard_write_text(
      (const uint8_t *)written.command,
      (uint32_t)string_length(written.command));
  char text[VKR_RECORDING_PATH_MAX + 64u];
  string_format(text, sizeof(text),
                "Wrote %s; its harness command is on the clipboard.",
                written.case_path);
  vkr_editor_toast(editor, VKR_UI_ICON_SPEED, theme->text, text);
}

/* The segment panel's first row: the title field, the previous and next
   segment, Focus, Re-run, Delete and Close. */
typedef enum EditorSegmentAction {
  EDITOR_SEGMENT_NONE = 0,
  EDITOR_SEGMENT_PREVIOUS,
  EDITOR_SEGMENT_NEXT,
  EDITOR_SEGMENT_FOCUS,
  EDITOR_SEGMENT_RERUN,
  EDITOR_SEGMENT_DELETE,
  EDITOR_SEGMENT_SAVE,
  EDITOR_SEGMENT_CLOSE,
} EditorSegmentAction;

vkr_internal EditorSegmentAction editor_annotations_segment_header(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame, bool8_t focused) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  EditorSegmentAction action = EDITOR_SEGMENT_NONE;
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = editor_annotations_cell(0u);
  vkr_editor_field_style(&field);
  field.style.min_size_pt.y = field.style.max_size_pt.y = 26.0f;
  field.tooltip = string8_lit("The segment's title; Enter saves");
  const VkrUiId title_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("title"));
  /* A new segment's title takes the keyboard until the field owns it. */
  if (notes->edit_focus) {
    ui->focused_id = title_id;
    ui->focused_is_text = true_v;
    (void)vkr_ui_keyboard_layer_set(ui, ui->input_layer);
  }
  VkrUiTextEditBuffer title = {.data = (uint8_t *)notes->title,
                               .length = notes->title_length,
                               .capacity = sizeof(notes->title) - 1u};
  (void)vkr_ui_text_field(ui, string8_lit("title"), &title, &field);
  notes->title_length = title.length;
  if (notes->edit_focus && ui->focused_id == title_id &&
      ui->keyboard_input_layer == ui->input_layer &&
      vkr_ui_widget_rect(ui, title_id, &(VkrUiRect){0})) {
    notes->edit_focus = false_v;
  }
  if (ui->focused_id == title_id &&
      input_key_just_pressed(ui->input, KEY_ENTER)) {
    action = EDITOR_SEGMENT_SAVE;
  }
  const uint32_t count = notes->annotations.segment_count;
  VkrUiWidgetConfig previous = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_CHEVRON_LEFT, string8_lit("Previous segment"));
  previous.disabled = notes->selected_index == 0u;
  if (vkr_ui_button(ui, string8_lit("previous"), (String8){0}, &previous)) {
    action = EDITOR_SEGMENT_PREVIOUS;
  }
  VkrUiWidgetConfig next = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_CHEVRON_RIGHT, string8_lit("Next segment"));
  next.disabled = notes->selected_index + 1u >= count;
  if (vkr_ui_button(ui, string8_lit("next"), (String8){0}, &next)) {
    action = EDITOR_SEGMENT_NEXT;
  }
  VkrUiWidgetConfig focus = editor_annotations_text_button(
      editor, 3u, 0u, VKR_UI_ICON_CROSSHAIR,
      focused ? string8_lit("Show the whole recording again")
              : string8_lit("View this segment alone: zoom the timeline to "
                            "it and loop its frames"));
  vkr_editor_toggle_style(&focus, focused);
  if (vkr_ui_button(ui, string8_lit("focus"),
                    focused ? string8_lit("Exit focus") : string8_lit("Focus"),
                    &focus)) {
    action = EDITOR_SEGMENT_FOCUS;
  }
  VkrUiWidgetConfig rerun = editor_annotations_text_button(
      editor, 4u, 0u, VKR_UI_ICON_SPEED,
      string8_lit("Benchmark this segment: write a harness case that "
                  "replays it and copy its command"));
  if (vkr_ui_button(ui, string8_lit("rerun"), string8_lit("Re-run"), &rerun)) {
    action = EDITOR_SEGMENT_RERUN;
  }
  VkrUiWidgetConfig remove = editor_annotations_delete_button(
      editor, 5u, 0u, string8_lit("Delete this segment"));
  if (vkr_ui_button(ui, string8_lit("delete"), string8_lit("Delete"),
                    &remove)) {
    action = EDITOR_SEGMENT_DELETE;
  }
  VkrUiWidgetConfig close = vkr_editor_icon_button_config(
      6u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close and save (Escape)"));
  if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close) ||
      input_key_just_pressed(ui->input, KEY_ESCAPE)) {
    action = EDITOR_SEGMENT_CLOSE;
  }
  return action;
}

/* The segment panel's note field and footer: its frames and length, who
   marked it, the note's size, and Save. */
vkr_internal EditorSegmentAction editor_annotations_segment_note(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, const VkrRecordingSegment *segment,
    float32_t note_height) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  EditorSegmentAction action = EDITOR_SEGMENT_NONE;
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = editor_annotations_cell(0u);
  field.placement.row = 1u;
  field.placement.column_span = 7u;
  field.placement.align = VKR_UI_ALIGN_STRETCH;
  vkr_editor_field_style(&field);
  field.style.padding_pt = (VkrUiEdges){5, 6, 5, 6};
  field.style.font_size_pt = theme->font_body;
  field.multiline = true_v;
  /* The note fills its row; a text field keeps its own height otherwise. */
  field.style.min_size_pt.y = field.style.max_size_pt.y = note_height;
  field.tooltip = string8_lit("Why this segment matters: what happens here and "
                              "what to look at");
  const VkrUiId note_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("note"));
  VkrUiTextEditBuffer note = {.data = (uint8_t *)notes->note,
                              .length = notes->note_length,
                              .capacity = sizeof(notes->note) - 1u};
  (void)vkr_ui_text_field(ui, string8_lit("note"), &note, &field);
  notes->note_length = note.length;
#if defined(PLATFORM_APPLE)
  const Keys primary = KEY_LWIN;
#else
  const Keys primary = KEY_LCONTROL;
#endif
  if (ui->focused_id == note_id && input_is_key_down(ui->input, primary) &&
      input_key_just_pressed(ui->input, KEY_ENTER)) {
    action = EDITOR_SEGMENT_SAVE;
  }
  const bool8_t dirty = editor_annotations_segment_dirty(notes);
  VkrUiWidgetConfig footer =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  footer.placement = editor_annotations_cell(0u);
  footer.placement.row = 2u;
  footer.placement.column_span = 5u;
  footer.placement.justify = VKR_UI_ALIGN_START;
  footer.icon = VKR_UI_ICON_RULER;
  footer.icon_size_pt = 12.0f;
  const uint64_t last_frame = playback->frame_count - 1u;
  const float64_t seconds = playback->times[Min(segment->last, last_frame)] -
                            playback->times[Min(segment->first, last_frame)];
  vkr_ui_label(ui, string8_lit("footer"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "Frames %llu\xe2\x80\x93%llu  \xc2\xb7  %.2f s  \xc2\xb7  "
                   "%s%s%s%u of 1024 bytes%s",
                   (unsigned long long)segment->first,
                   (unsigned long long)segment->last, seconds,
                   segment->author[0] ? "by " : "", segment->author,
                   segment->author[0] ? "  \xc2\xb7  " : "",
                   (uint32_t)notes->note_length,
#if defined(PLATFORM_APPLE)
                   dirty ? "  \xc2\xb7  unsaved: Cmd+Enter saves" : ""
#else
                   dirty ? "  \xc2\xb7  unsaved: Ctrl+Enter saves" : ""
#endif
                   ),
               &footer);
  VkrUiWidgetConfig save = vkr_ui_widget_config_default();
  save.placement = editor_annotations_cell(5u);
  save.placement.row = 2u;
  save.placement.column_span = 2u;
  /* Prominent only while there is something to save. */
  if (dirty) {
    vkr_editor_primary_style(&save, editor->heading_font);
  } else {
    vkr_editor_action_style(&save, editor->heading_font);
  }
  save.icon = VKR_UI_ICON_SAVE;
  vkr_editor_button_size(&save);
  save.style.min_size_pt.y = save.style.max_size_pt.y = 22.0f;
  save.disabled = !dirty;
  save.tooltip = string8_lit("Save the title and note");
  if (vkr_ui_button(ui, string8_lit("save"), string8_lit("Save"), &save)) {
    action = EDITOR_SEGMENT_SAVE;
  }
  return action;
}

/* The selected segment: its title and note to edit, its frames, the
   neighbouring segments, Focus, Re-run, Delete, Save and Close. */
vkr_internal void editor_annotations_segment_panel(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, Vec4 rect) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const uint32_t index = notes->selected_index;
  const VkrRecordingSegment segment = notes->annotations.segments[index];
  const bool8_t focused = editor_annotations_focused(notes, playback, index);
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack icon_track = {.value = 24.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                icon_track,
                                icon_track,
                                auto_track,
                                auto_track,
                                auto_track,
                                icon_track};
  const VkrUiTrack rows[] = {{.value = 26.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                             {.value = 22.0f, .unit = VKR_UI_TRACK_PX}};
  if (!editor_annotations_panel_begin(
          ui, string8_lit("editor.recording.segment"), rect, columns,
          ArrayCount(columns), rows, ArrayCount(rows))) {
    return;
  }
  EditorSegmentAction action =
      editor_annotations_segment_header(editor, frame, focused);
  /* The note row: the panel less its padding, the title row, the footer
     and the gaps between the three rows. */
  const float32_t note_height = rect.w -
                                2.0f * EDITOR_ANNOTATIONS_PANEL_PAD_PT - 26.0f -
                                22.0f - 2.0f * vkr_ui_theme()->space_md;
  const EditorSegmentAction footer = editor_annotations_segment_note(
      editor, frame, playback, &segment, note_height);
  if (action == EDITOR_SEGMENT_NONE) {
    action = footer;
  }
  (void)vkr_ui_panel_end(ui);

  switch (action) {
  case EDITOR_SEGMENT_SAVE:
    (void)editor_annotations_segment_commit(editor);
    break;
  case EDITOR_SEGMENT_PREVIOUS:
  case EDITOR_SEGMENT_NEXT: {
    const uint32_t other =
        action == EDITOR_SEGMENT_PREVIOUS ? index - 1u : index + 1u;
    vkr_editor_recording_segment_select(editor, other);
    if (notes->selected_index == other) {
      VkrSampleRecordingRequest seek = {
          .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
          .frame = notes->annotations.segments[other].first};
      (void)vkr_editor_recording_send(editor, seek, false_v);
      /* The focus follows to the neighbour. */
      if (focused) {
        editor_annotations_focus(editor, playback, other);
      }
    }
    break;
  }
  case EDITOR_SEGMENT_FOCUS:
    if (focused) {
      editor_annotations_unfocus(editor, playback);
    } else {
      editor_annotations_focus(editor, playback, index);
    }
    break;
  case EDITOR_SEGMENT_RERUN:
    editor_annotations_rerun(editor, playback, segment.first, segment.last);
    break;
  case EDITOR_SEGMENT_DELETE: {
    VkrRecordingAnnotations *annotations =
        editor_annotations_open(editor, playback);
    if (annotations) {
      if (focused) {
        editor_annotations_unfocus(editor, playback);
      }
      vkr_recording_annotations_remove_segment(annotations, index);
      editor_annotations_saved(editor, VKR_UI_ICON_TRASH,
                               "Deleted the segment.");
    }
    break;
  }
  case EDITOR_SEGMENT_CLOSE:
    if (editor_annotations_segment_commit(editor)) {
      notes->selected = VKR_EDITOR_RECORDING_SELECTION_NONE;
    }
    break;
  case EDITOR_SEGMENT_NONE:
    break;
  }
}

/* One entry of the Segments list on grid rows `row` and `row` + 1: its
   title and span over the note's first line, and Focus beside both.
   Returns 1 when the entry was clicked and 2 when its Focus was. */
vkr_internal uint32_t editor_annotations_list_row(
    VkrEditorUi *editor, VkrUiSystem *ui, const VkrRecordingPlayback *playback,
    uint32_t index, uint32_t row, Vec4 rect) {
  const VkrEditorRecordingNotes *notes = &editor->recording.notes;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingSegment *segment = &notes->annotations.segments[index];
  const bool8_t focused = editor_annotations_focused(notes, playback, index);
  const uint64_t last_frame = playback->frame_count - 1u;
  const float64_t start = playback->times[Min(segment->first, last_frame)];
  const float64_t end = playback->times[Min(segment->last, last_frame)];
  /* The note's first line, cut to what fits one row. */
  uint64_t line = 0u;
  while (segment->note[line] && segment->note[line] != '\n' && line < 120u) {
    ++line;
  }
  const String8 tooltip =
      segment->note[0]
          ? string8_create_from_cstr((const uint8_t *)segment->note,
                                     string_length(segment->note))
          : string8_lit("No note yet: open it to say why it matters");
  uint32_t result = 0u;
  (void)vkr_ui_push_id_u64(ui, index);
  VkrUiWidgetConfig title = vkr_ui_widget_config_default();
  title.placement = editor_annotations_cell(0u);
  title.placement.row = row;
  title.placement.column_span = 2u;
  vkr_editor_ghost_style(&title);
  title.text.font = editor->heading_font;
  title.style.padding_pt = (VkrUiEdges){1.0f, 6.0f, 1.0f, 6.0f};
  title.style.min_size_pt.y = title.style.max_size_pt.y =
      EDITOR_ANNOTATIONS_LIST_TITLE_PT;
  title.style.text_color = focused ? theme->accent : theme->text;
  title.text.layout.max_width = rect.z - 150.0f;
  title.tooltip = tooltip;
  if (vkr_ui_button(
          ui, string8_lit("title"),
          string8_create_formatted(ui->frame_allocator,
                                   "%s   %.2f\xe2\x80\x93%.2f s  (%.2f s)",
                                   segment->name, start, end, end - start),
          &title)) {
    result = 1u;
  }
  VkrUiWidgetConfig note = title;
  note.placement.row = row + 1u;
  note.text.font = VKR_FONT_HANDLE_INVALID;
  note.style.font_size_pt = theme->font_caption;
  note.style.min_size_pt.y = note.style.max_size_pt.y =
      EDITOR_ANNOTATIONS_LIST_NOTE_PT;
  note.style.text_color = theme->text_secondary;
  if (vkr_ui_button(ui, string8_lit("note"),
                    string8_create_formatted(
                        ui->frame_allocator, "%.*s", (int32_t)line,
                        segment->note[0] ? segment->note : "No note yet"),
                    &note)) {
    result = 1u;
  }
  VkrUiWidgetConfig focus = editor_annotations_text_button(
      editor, 2u, row, VKR_UI_ICON_CROSSHAIR,
      focused ? string8_lit("Show the whole recording again")
              : string8_lit("View this segment alone"));
  focus.placement.row_span = 2u;
  vkr_editor_toggle_style(&focus, focused);
  if (vkr_ui_button(ui, string8_lit("focus"),
                    focused ? string8_lit("Exit") : string8_lit("Focus"),
                    &focus)) {
    result = 2u;
  }
  (void)vkr_ui_pop_id(ui);
  return result;
}

/* The Segments list over the bar: every segment by first frame, a page of
   rows at a time; a row opens its segment, Focus views it alone. */
vkr_internal void
editor_annotations_list_panel(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              const VkrRecordingPlayback *playback, Vec4 rect) {
  VkrEditorRecordingNotes *notes = &editor->recording.notes;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t count = notes->annotations.segment_count;
  const uint32_t pages = Max((count + EDITOR_ANNOTATIONS_LIST_ROWS - 1u) /
                                 EDITOR_ANNOTATIONS_LIST_ROWS,
                             1u);
  notes->list_page = Min(notes->list_page, pages - 1u);
  const uint32_t first = notes->list_page * EDITOR_ANNOTATIONS_LIST_ROWS;
  const uint32_t shown = editor_annotations_list_rows(notes);
  VkrUiTrack rows[1u + 2u * EDITOR_ANNOTATIONS_LIST_ROWS];
  rows[0] = (VkrUiTrack){.value = EDITOR_ANNOTATIONS_LIST_HEADER_PT,
                         .unit = VKR_UI_TRACK_PX};
  for (uint32_t i = 0u; i < shown; ++i) {
    rows[1u + 2u * i] = (VkrUiTrack){.value = EDITOR_ANNOTATIONS_LIST_TITLE_PT,
                                     .unit = VKR_UI_TRACK_PX};
    rows[2u + 2u * i] = (VkrUiTrack){.value = EDITOR_ANNOTATIONS_LIST_NOTE_PT,
                                     .unit = VKR_UI_TRACK_PX};
  }
  const VkrUiTrack icon_track = {.value = 24.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                icon_track,
                                {.unit = VKR_UI_TRACK_AUTO},
                                icon_track,
                                icon_track};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0.0f, 0.0f, rect.x};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){EDITOR_ANNOTATIONS_PANEL_PAD_PT, 10.0f,
                                        EDITOR_ANNOTATIONS_PANEL_PAD_PT, 10.0f};
  panel.style.gap_pt = EDITOR_ANNOTATIONS_LIST_GAP_PT;
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){rect.z, rect.w};
  panel.columns = columns;
  panel.column_count = ArrayCount(columns);
  panel.rows = rows;
  panel.row_count = 1u + 2u * shown;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.recording.segments"),
                          &panel)) {
    return;
  }
  VkrUiWidgetConfig header =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  header.placement = editor_annotations_cell(0u);
  header.placement.column_span = 2u;
  header.placement.justify = VKR_UI_ALIGN_START;
  header.icon = VKR_UI_ICON_LIST;
  header.icon_size_pt = 12.0f;
  vkr_ui_label(ui, string8_lit("header"),
               count == 0u ? string8_lit("Segments")
                           : string8_create_formatted(ui->frame_allocator,
                                                      "Segments  %u", count),
               &header);
  VkrUiWidgetConfig page =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  page.placement = editor_annotations_cell(2u);
  vkr_ui_label(ui, string8_lit("page"),
               pages > 1u
                   ? string8_create_formatted(ui->frame_allocator, "%u / %u",
                                              notes->list_page + 1u, pages)
                   : (String8){0},
               &page);
  /* Pages turn only when the segments fill more than one. */
  if (pages > 1u) {
    VkrUiWidgetConfig previous = vkr_editor_icon_button_config(
        1u, 0u, VKR_UI_ICON_CHEVRON_LEFT, string8_lit("Previous page"));
    previous.disabled = notes->list_page == 0u;
    if (vkr_ui_button(ui, string8_lit("previous"), (String8){0}, &previous)) {
      --notes->list_page;
    }
    VkrUiWidgetConfig next = vkr_editor_icon_button_config(
        3u, 0u, VKR_UI_ICON_CHEVRON_RIGHT, string8_lit("Next page"));
    next.disabled = notes->list_page + 1u >= pages;
    if (vkr_ui_button(ui, string8_lit("next"), (String8){0}, &next)) {
      ++notes->list_page;
    }
  }
  VkrUiWidgetConfig close = vkr_editor_icon_button_config(
      4u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close (Escape)"));
  const bool8_t closing =
      vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close) ||
      (!ui->focused_is_text && input_key_just_pressed(ui->input, KEY_ESCAPE));
  uint32_t opened = UINT32_MAX;
  uint32_t focus = UINT32_MAX;
  if (count == 0u) {
    VkrUiWidgetConfig hint =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    hint.placement = editor_annotations_cell(0u);
    hint.placement.row = 1u;
    hint.placement.row_span = 2u;
    hint.placement.column_span = 5u;
    hint.placement.justify = VKR_UI_ALIGN_START;
    vkr_ui_label(ui, string8_lit("hint"),
                 string8_lit("No segments yet. Mark one with the ruler: click "
                             "at its first frame, then at its last, or select "
                             "frames on the timeline."),
                 &hint);
  }
  for (uint32_t i = 0u; count > 0u && i < shown; ++i) {
    const uint32_t clicked = editor_annotations_list_row(
        editor, ui, playback, first + i, 1u + 2u * i, rect);
    if (clicked == 1u) {
      opened = first + i;
    } else if (clicked == 2u) {
      focus = first + i;
    }
  }
  (void)vkr_ui_panel_end(ui);
  if (focus != UINT32_MAX) {
    if (editor_annotations_focused(notes, playback, focus)) {
      editor_annotations_unfocus(editor, playback);
    } else {
      editor_annotations_focus(editor, playback, focus);
    }
  } else if (opened != UINT32_MAX) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
        .frame = notes->annotations.segments[opened].first};
    (void)vkr_editor_recording_send(editor, seek, false_v);
    vkr_editor_recording_segment_select(editor, opened);
  } else if (closing) {
    notes->listing = false_v;
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
    notes->listing = false_v;
    notes->focusing = false_v;
  }
  Vec4 rect;
  if (!vkr_editor_recording_composer_rect(editor, bar_pt, &rect)) {
    return;
  }
  if (notes->composing) {
    editor_annotations_composer(editor, frame, playback, rect);
  } else if (notes->listing) {
    editor_annotations_list_panel(editor, frame, playback, rect);
  } else if (notes->selected == VKR_EDITOR_RECORDING_SELECTION_NOTE) {
    editor_annotations_note_panel(editor, frame, playback, rect);
  } else {
    editor_annotations_segment_panel(editor, frame, playback, rect);
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
  /* Segments first: the parts someone marked as worth looking at. */
  for (uint32_t i = 0u;
       i < Min(a->segment_count, EDITOR_ANNOTATIONS_PROMPT_ITEMS); ++i) {
    const VkrRecordingSegment *segment = &a->segments[i];
    editor_prompt_add(prompt, "%s- \"%s\": frames %llu to %llu%s%s%s\n",
                      i == 0u ? "Segments to look at:\n" : "", segment->name,
                      (unsigned long long)segment->first,
                      (unsigned long long)segment->last,
                      segment->note[0] ? ". Why: \"" : "", segment->note,
                      segment->note[0] ? "\"" : "");
  }
  if (a->segment_count > EDITOR_ANNOTATIONS_PROMPT_ITEMS) {
    editor_prompt_add(prompt, "- and %u more segments\n",
                      a->segment_count - EDITOR_ANNOTATIONS_PROMPT_ITEMS);
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
      "view.capture shows the frame; rec.segments lists the segments with "
      "their frame-time statistics, rec.frames and rec.rerun take a "
      "'segment', and rec.segment and rec.note add yours. Offline, vkr_rec "
      "segments, summary, spikes, frames and passes read the bundle, and %s/ "
      "holds a JPEG of the Scene twice a second named by its frame.\n",
      VKR_RECORDING_SUMMARY_FILE, entry->bundle,
      VKR_EDITOR_RECORDING_THUMBS_DIRECTORY);
  (void)vkr_platform_clipboard_write_text((const uint8_t *)prompt.text,
                                          (uint32_t)prompt.length);
}
