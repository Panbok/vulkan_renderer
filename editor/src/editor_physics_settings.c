#include "editor_physics_settings.h"

#include "editor_internal.h"
#include "renderer/systems/vkr_scene_collision_layers.h"
#include <stdio.h>
#include <string.h>

#define SETTINGS_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

struct VkrEditorPhysicsSettings {
  VkrAllocator *allocator;
  VkrSceneCollisionLayers draft;
  uint64_t generation;
  uint64_t revision;
  float32_t scroll;
  uint32_t tab;
  uint32_t preset;
  bool8_t changed;
  char error[192];
  /* Matrix cell under the pointer last frame; drives the crosshair. */
  uint32_t hover_row;
  uint32_t hover_column;
};

static VkrUiWidgetConfig settings_widget(float32_t x, float32_t y,
                                         float32_t width, float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_START,
                                 .margin_pt = {y, 0, 0, x}};
  c.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  c.style.max_size_pt = c.style.min_size_pt;
  c.style.font_size_pt = vkr_ui_theme()->font_body;
  c.style.text_color = vkr_ui_theme()->text;
  c.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
  return c;
}

static String8 settings_text(const char *text) {
  return (String8){.str = (uint8_t *)text, .length = strlen(text)};
}

VkrEditorPhysicsSettings *
vkr_editor_physics_settings_create(VkrAllocator *allocator) {
  VkrEditorPhysicsSettings *settings =
      vkr_allocator_alloc(allocator, sizeof(*settings), SETTINGS_TAG);
  if (settings) {
    *settings = (VkrEditorPhysicsSettings){.allocator = allocator,
                                           .generation = UINT64_MAX,
                                           .hover_row = UINT32_MAX,
                                           .hover_column = UINT32_MAX};
  }
  return settings;
}

void vkr_editor_physics_settings_destroy(VkrEditorPhysicsSettings *settings) {
  if (settings) {
    vkr_allocator_free(settings->allocator, settings, sizeof(*settings),
                       SETTINGS_TAG);
  }
}

static void settings_read(VkrEditorPhysicsSettings *settings,
                          const VkrSampleUiFrame *frame) {
  vkr_scene_collision_layers_read(frame->scene, &settings->draft);
  settings->generation = frame->scene_generation;
  settings->revision = frame->scene->collision_layers_revision;
  settings->changed = false_v;
  settings->error[0] = 0;
  settings->preset = Min(
      settings->preset,
      settings->draft.preset_count ? settings->draft.preset_count - 1u : 0u);
}

void vkr_editor_physics_settings_build(VkrEditorPhysicsSettings *p,
                                       const VkrSampleUiFrame *f,
                                       VkrUiRect rect, VkrFontHandle heading) {
  if (!p || !f->scene || !f->scene_edit) {
    return;
  }
  VkrUiSystem *ui = f->ui;
  const float32_t width = rect.width / ui->content_scale;
  const float32_t height = rect.height / ui->content_scale;
  if (width < 200 || height < 80) {
    return;
  }
  if (p->generation != f->scene_generation ||
      p->revision != f->scene->collision_layers_revision) {
    settings_read(p, f);
  }
  const bool8_t paused = vkr_scene_physics_is_paused(f->scene);
  float32_t content_height = p->tab == 0 ? 90 + 16 * 28
                             : p->tab == 1
                                 ? 90 + 18 * 24
                                 : 90 + p->draft.preset_count * 26 + 20 * 28;
  if (!ui->mouse_captured && ui->mouse_input_layer == ui->input_layer &&
      ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
      ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height) {
    p->scroll -= ui->mouse_wheel * 40;
  }
  p->scroll = vkr_clamp_f32(p->scroll, 0, Max(0.0f, content_height - height));
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  const VkrUiTrack track = {.value = content_height, .unit = VKR_UI_TRACK_PX};
  scroll.rows = &track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("physics.settings.scroll"),
                                &scroll)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset_set(ui, p->scroll);
  const VkrUiTheme *theme = vkr_ui_theme();
  float32_t y = 10;
  /* Segmented tabs on the left, Revert/Apply on the right. */
  const char *tabs[] = {"Layers", "Collision matrix", "Presets"};
  const VkrUiIcon tab_icons[] = {VKR_UI_ICON_LAYERS, VKR_UI_ICON_GRID,
                                 VKR_UI_ICON_LIST};
  const float32_t tab_w = Min(150.0f, (width - 200) / 3);
  VkrUiWidgetConfig group = settings_widget(10, y - 2, tab_w * 3 + 4, 30);
  group.style.background_color = theme->field;
  group.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  group.style.border_color = theme->border;
  group.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
  vkr_ui_label(ui, string8_lit("tabs.group"), (String8){0}, &group);
  for (uint32_t i = 0; i < ArrayCount(tabs); ++i) {
    const bool8_t selected = p->tab == i;
    VkrUiWidgetConfig c = settings_widget(12 + i * tab_w, y, tab_w, 26);
    vkr_editor_ghost_style(&c);
    c.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
    c.style.background_color = selected ? theme->raised_hover : (Vec4){0};
    c.style.text_color = selected ? theme->text : theme->text_secondary;
    c.text.font = selected ? heading : VKR_FONT_HANDLE_INVALID;
    c.icon = tab_icons[i];
    c.icon_size_pt = 14.0f;
    c.icon_color = selected ? theme->accent_hover : theme->text_secondary;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("tab"), settings_text(tabs[i]), &c)) {
      p->tab = i;
      p->scroll = 0;
    }
    (void)vkr_ui_pop_id(ui);
  }
  VkrUiWidgetConfig revert = settings_widget(width - 186, y, 84, 26);
  vkr_editor_action_style(&revert, heading);
  revert.disabled = !p->changed;
  revert.icon = VKR_UI_ICON_RESET;
  revert.icon_size_pt = 13.0f;
  if (vkr_ui_button(ui, string8_lit("revert"), string8_lit("Revert"), &revert))
    settings_read(p, f);
  VkrUiWidgetConfig apply = settings_widget(width - 96, y, 86, 26);
  vkr_editor_primary_style(&apply, heading);
  apply.disabled = !p->changed || !paused;
  apply.icon = VKR_UI_ICON_CHECK;
  apply.icon_size_pt = 13.0f;
  apply.tooltip =
      string8_lit("Apply as one undo step (physics must be paused)");
  if (vkr_ui_button(ui, string8_lit("apply"), string8_lit("Apply"), &apply)) {
    const char *error = NULL;
    if (vkr_scene_collision_layers_validate(&p->draft, &error)) {
      *f->scene_edit =
          (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS,
                                .collision_layers = &p->draft};
    } else {
      snprintf(p->error, sizeof(p->error), "%s",
               error ? error : "Invalid settings.");
    }
  }
  y += 36;
  VkrUiWidgetConfig c = settings_widget(10, y, width - 20, 22);
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = p->error[0] ? theme->error
                       : !paused   ? theme->warning
                                   : theme->text_secondary;
  c.icon =
      p->error[0] || !paused ? VKR_UI_ICON_WARNING_FILL : VKR_UI_ICON_INFO_FILL;
  c.icon_size_pt = 12.0f;
  const char *status =
      p->error[0] ? p->error
      : !paused   ? "Pause physics to apply settings."
      : p->changed
          ? "Unapplied changes. Apply records one undo step; Save publishes."
          : f->edits->status;
  vkr_ui_label(ui, string8_lit("status"), settings_text(status), &c);
  y += 30;
  if (p->tab == 0) {
    for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
      (void)vkr_ui_push_id_u64(ui, i);
      c = settings_widget(10, y, 30, 24);
      c.style.text_color = theme->text_secondary;
      vkr_ui_label(ui, string8_lit("index"),
                   string8_create_formatted(ui->frame_allocator, "%u", i + 1u),
                   &c);
      c = settings_widget(42, y, width - 52, 24);
      vkr_editor_field_style(&c);
      VkrUiTextEditBuffer name = {(uint8_t *)p->draft.names[i],
                                  (uint32_t)strlen(p->draft.names[i]),
                                  sizeof(p->draft.names[i])};
      p->changed |= vkr_ui_text_field(ui, string8_lit("name"), &name, &c);
      (void)vkr_ui_pop_id(ui);
      y += 28;
    }
  } else if (p->tab == 1) {
    /* Symmetric pairs: draw the upper triangle only. Headers highlight the
     * row and column of last frame's hovered cell. */
    const float32_t left = Min(150.0f, width * 0.3f);
    const float32_t cell =
        Min(26.0f, (width - left - 14) / VKR_COLLISION_LAYER_COUNT);
    const uint32_t hover_row = p->hover_row, hover_column = p->hover_column;
    p->hover_row = p->hover_column = UINT32_MAX;
    for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
      c = settings_widget(left + i * cell, y, cell, 22);
      c.style.padding_pt = (VkrUiEdges){2, 0, 2, 0};
      c.style.font_size_pt = theme->font_caption;
      c.style.text_color =
          i == hover_column ? theme->accent_hover : theme->text_secondary;
      c.tooltip = settings_text(p->draft.names[i]);
      (void)vkr_ui_push_id_u64(ui, i);
      vkr_ui_label(ui, string8_lit("column"),
                   string8_create_formatted(ui->frame_allocator, "%u", i + 1u),
                   &c);
      (void)vkr_ui_pop_id(ui);
    }
    y += 24;
    for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
      (void)vkr_ui_push_id_u64(ui, i);
      c = settings_widget(10, y, left - 14, 22);
      c.tooltip = settings_text(p->draft.names[i]);
      c.style.text_color =
          i == hover_row ? theme->accent_hover : theme->text_secondary;
      if (i == hover_row) {
        c.style.background_color = vkr_ui_color_alpha(theme->accent, 0.14f);
        c.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
      }
      vkr_ui_label(ui, string8_lit("row"),
                   string8_create_formatted(ui->frame_allocator, "%u  %s",
                                            i + 1u, p->draft.names[i]),
                   &c);
      for (uint32_t j = i; j < VKR_COLLISION_LAYER_COUNT; ++j) {
        (void)vkr_ui_push_id_u64(ui, j);
        bool8_t enabled = (p->draft.matrix[i] & (1u << j)) != 0;
        c = settings_widget(left + j * cell + (cell - 18) * 0.5f, y + 2, 18,
                            18);
        c.style.padding_pt = (VkrUiEdges){0};
        c.tooltip =
            string8_create_formatted(ui->frame_allocator, "%s \xe2\x80\x93 %s",
                                     p->draft.names[i], p->draft.names[j]);
        const VkrUiId cell_id =
            vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("pair"));
        if (vkr_ui_checkbox(ui, string8_lit("pair"), string8_lit(""), &enabled,
                            &c)) {
          if (enabled) {
            p->draft.matrix[i] |= (uint16_t)(1u << j);
            p->draft.matrix[j] |= (uint16_t)(1u << i);
          } else {
            p->draft.matrix[i] &= (uint16_t)~(1u << j);
            p->draft.matrix[j] &= (uint16_t)~(1u << i);
          }
          p->changed = true_v;
        }
        if (ui->hot_id == cell_id) {
          p->hover_row = i;
          p->hover_column = j;
        }
        (void)vkr_ui_pop_id(ui);
      }
      (void)vkr_ui_pop_id(ui);
      y += 24;
    }
  } else {
    for (uint32_t i = 0; i < p->draft.preset_count; ++i) {
      c = settings_widget(4, y, width - 8, 24);
      c.disabled = p->preset == i;
      (void)vkr_ui_push_id_u64(ui, i);
      if (vkr_ui_button(ui, string8_lit("preset.select"),
                        settings_text(p->draft.presets[i].name), &c)) {
        p->preset = i;
      }
      (void)vkr_ui_pop_id(ui);
      y += 26;
    }
    c = settings_widget(4, y, (width - 8) / 2 - 2, 24);
    c.disabled = p->draft.preset_count == VKR_COLLISION_PRESET_CAPACITY;
    if (vkr_ui_button(ui, string8_lit("preset.add"), string8_lit("Add preset"),
                      &c)) {
      p->preset = p->draft.preset_count++;
      VkrSceneCollisionPreset *preset = &p->draft.presets[p->preset];
      *preset = (VkrSceneCollisionPreset){.membership = 1, .mask = UINT16_MAX};
      snprintf(preset->name, sizeof(preset->name), "Preset %u", p->preset + 1u);
      p->changed = true_v;
    }
    c = settings_widget(width / 2, y, width / 2 - 4, 24);
    c.disabled = p->draft.preset_count == 0;
    if (vkr_ui_button(ui, string8_lit("preset.remove"),
                      string8_lit("Remove preset"), &c)) {
      MemCopy(&p->draft.presets[p->preset], &p->draft.presets[p->preset + 1u],
              (p->draft.preset_count - p->preset - 1u) *
                  sizeof(p->draft.presets[0]));
      p->draft.preset_count--;
      MemZero(&p->draft.presets[p->draft.preset_count],
              sizeof(p->draft.presets[0]));
      p->preset = Min(p->preset,
                      p->draft.preset_count ? p->draft.preset_count - 1u : 0u);
      p->changed = true_v;
    }
    y += 28;
    if (p->draft.preset_count) {
      VkrSceneCollisionPreset *preset = &p->draft.presets[p->preset];
      (void)vkr_ui_push_id_u64(ui, p->preset);
      c = settings_widget(4, y, width - 8, 24);
      vkr_editor_field_style(&c);
      VkrUiTextEditBuffer name = {(uint8_t *)preset->name,
                                  (uint32_t)strlen(preset->name),
                                  sizeof(preset->name)};
      p->changed |=
          vkr_ui_text_field(ui, string8_lit("preset.name"), &name, &c);
      y += 28;
      c = settings_widget(4, y, width - 8, 24);
      p->changed |=
          vkr_ui_checkbox(ui, string8_lit("preset.sensor"),
                          string8_lit("Sensor role (Static / Kinematic only)"),
                          &preset->sensor, &c);
      y += 28;
      for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
        (void)vkr_ui_push_id_u64(ui, i);
        for (uint32_t mask = 0; mask < 2; ++mask) {
          (void)vkr_ui_push_id_u64(ui, mask);
          uint16_t *bits = mask ? &preset->mask : &preset->membership;
          bool8_t checked = (*bits & (1u << i)) != 0;
          c = settings_widget(4 + mask * (width - 8) / 2, y,
                              (width - 8) / 2 - 2, 24);
          if (vkr_ui_checkbox(ui, string8_lit("bit"),
                              string8_create_formatted(
                                  ui->frame_allocator, "%s: %s",
                                  mask ? "Hits" : "Is", p->draft.names[i]),
                              &checked, &c)) {
            *bits = checked ? *bits | (uint16_t)(1u << i)
                            : *bits & (uint16_t)~(1u << i);
            p->changed = true_v;
          }
          (void)vkr_ui_pop_id(ui);
        }
        (void)vkr_ui_pop_id(ui);
        y += 26;
      }
      (void)vkr_ui_pop_id(ui);
    }
  }
  vkr_ui_scroll_area_end(ui);
}
